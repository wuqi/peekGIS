#include "vt/vt_source.h"
#include "data/gdal_common.h"

#include <ogr_api.h>

namespace peekg::vt {

using peekg::data::ensureGdal;
using peekg::data::gdalOpenVector;
using peekg::data::gdalSrsEpsg;

bool readVtLayerInfo(const std::string& path, int layerIdx, LayerInfo& out) {
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return false;
    bool ok = false;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx >= 0 && layerIdx < nl) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
        const char* nm = OGR_L_GetName(lyr);
        out.name = nm ? nm : "";
        out.featureCount = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
        out.geomType = (int)OGR_L_GetGeomType(lyr);
        out.srcEpsg = gdalSrsEpsg(OGR_L_GetSpatialRef(lyr));
        OGREnvelope env;
        if (OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE) {
            out.hasExtent = true;
            out.minx = env.MinX; out.miny = env.MinY;
            out.maxx = env.MaxX; out.maxy = env.MaxY;
        }
        ok = true;
    }
    GDALClose(ds);
    return ok;
}

namespace {

int countPoints(OGRGeometryH g) {
    if (!g) return 0;
    int n = OGR_G_GetGeometryCount(g);
    if (n > 0) {
        int s = 0;
        for (int i = 0; i < n; ++i) s += countPoints(OGR_G_GetGeometryRef(g, i));
        return s;
    }
    return OGR_G_GetPointCount(g);
}

void collectRing(OGRGeometryH ring, OGRCoordinateTransformationH ct, std::vector<double>& out) {
    int n = OGR_G_GetPointCount(ring);
    out.reserve(out.size() + (size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        double x = OGR_G_GetX(ring, i);
        double y = OGR_G_GetY(ring, i);
        if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
        out.push_back(x);
        out.push_back(y);
    }
}

void emitGeom(OGRGeometryH g, OGRCoordinateTransformationH ct, uint32_t& polyCounter,
              long long featureIdx,
              const std::function<void(const SourceRing&)>& sink) {
    if (!g) return;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPolygon: {
            uint32_t pg = ++polyCounter;
            int nr = OGR_G_GetGeometryCount(g);
            for (int r = 0; r < nr; ++r) {
                SourceRing sr;
                sr.type = RING_FACE;
                sr.hole = (r == 0) ? 0 : 1;
                sr.polyGroup = pg;
                sr.featureIdx = featureIdx;
                collectRing(OGR_G_GetGeometryRef(g, r), ct, sr.xy);
                if (sr.xy.size() >= 6) sink(sr);   // >=3 点
            }
            break;
        }
        case wkbLineString:
        case wkbLinearRing: {
            SourceRing sr;
            sr.type = RING_LINE;
            sr.featureIdx = featureIdx;
            collectRing(g, ct, sr.xy);
            if (sr.xy.size() >= 4) sink(sr);       // >=2 点
            break;
        }
        case wkbPoint: {
            double x = OGR_G_GetX(g, 0), y = OGR_G_GetY(g, 0);
            if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
            SourceRing sr;
            sr.type = RING_POINT;
            sr.featureIdx = featureIdx;
            sr.xy = {x, y};
            sink(sr);
            break;
        }
        case wkbMultiPoint:
        case wkbMultiPolygon:
        case wkbMultiLineString:
        case wkbGeometryCollection: {
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; ++i)
                emitGeom(OGR_G_GetGeometryRef(g, i), ct, polyCounter, featureIdx, sink);
            break;
        }
        default:
            break;
    }
}

}  // namespace

long long streamVtRings(const std::string& path, int layerIdx, int dstEpsg,
                        const std::function<void(const SourceRing&)>& sink) {
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return -1;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return -1; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);

    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = gdalSrsEpsg(srcSrs);
    OGRCoordinateTransformationH ct = nullptr;
    if (dstEpsg > 0 && srcEpsg > 0 && dstEpsg != srcEpsg && srcSrs) {
        OGRSpatialReferenceH dstSrs = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(dstSrs, dstEpsg) == OGRERR_NONE)
            ct = OCTNewCoordinateTransformation(srcSrs, dstSrs);
        OSRDestroySpatialReference(dstSrs);
    }

    uint32_t polyCounter = 0;
    long long nfeat = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) emitGeom(g, ct, polyCounter, nfeat, sink);
        OGR_F_Destroy(f);
        ++nfeat;
    }

    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    return nfeat;
}

long long estimateSourceVerts(const std::string& path, int layerIdx, long long sampleK) {
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return -1;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return -1; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (F <= 0) { GDALClose(ds); return 0; }
    long long sum = 0, n = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while (n < sampleK && (f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) { sum += countPoints(g); ++n; }
        OGR_F_Destroy(f);
    }
    GDALClose(ds);
    if (n == 0) return 0;
    double mean = (double)sum / (double)n;
    return (long long)(mean * (double)F);
}

}  // namespace peekg::vt
