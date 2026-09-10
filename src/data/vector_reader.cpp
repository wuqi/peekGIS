#include "data/vector_reader.h"
#include "data/gdal_common.h"
#include "data/geom_util.h"
#include "data/reproject.h"
#include "platform/path_util.h"
#include "util/logger.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <limits>

namespace peekg::data {
namespace {

// 抽取单个图层(索引 li)的几何与元信息到 o
void extractOne(GDALDatasetH ds, int li, VectorData& o) {
    OGRLayerH lyr = GDALDatasetGetLayer(ds, li);
    o.name = OGR_L_GetName(lyr);
    OGREnvelope env;
    if (OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE) {
        o.minx = env.MinX; o.miny = env.MinY;
        o.maxx = env.MaxX; o.maxy = env.MaxY;
    }
    o.featureCount = OGR_L_GetFeatureCount(lyr, TRUE);
    OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
    if (srs) {
        o.srcEpsg = gdalSrsEpsg(srs);
        if (o.srcEpsg) o.sourceCrs = "EPSG:" + std::to_string(o.srcEpsg);
        else o.sourceCrs = "unknown";
    } else {
        o.sourceCrs = "unknown";
    }
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        addGeometry(g, o.vertices, &o.points);
        OGR_F_Destroy(f);
    }
}

void appendFieldAttr(OGRFeatureH f, int k, std::vector<IdentifyAttr>& attrs) {
    OGRFieldDefnH fd = OGR_F_GetFieldDefnRef(f, k);
    IdentifyAttr a;
    if (fd) {
        const char* nm = OGR_Fld_GetNameRef(fd);
        if (nm) {
            const char* e = nm;
            while (*e) e++;
            a.rawName.assign((const unsigned char*)nm, (const unsigned char*)e);
        }
    }
    char buf[64];
    if (OGR_F_IsFieldSetAndNotNull(f, k)) {
        OGRFieldType t = fd ? OGR_Fld_GetType(fd) : OFTString;
        switch (t) {
            case OFTInteger:
                snprintf(buf, sizeof(buf), "%d", OGR_F_GetFieldAsInteger(f, k)); a.value = buf; break;
            case OFTInteger64:
                snprintf(buf, sizeof(buf), "%lld", (long long)OGR_F_GetFieldAsInteger64(f, k)); a.value = buf; break;
            case OFTReal: case OFTRealList:
                snprintf(buf, sizeof(buf), "%.10g", OGR_F_GetFieldAsDouble(f, k)); a.value = buf; break;
            case OFTDate: case OFTDateTime: case OFTTime: {
                int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, tz = 0;
                if (OGR_F_GetFieldAsDateTime(f, k, &y, &mo, &d, &h, &mi, &s, &tz))
                    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
                else a.value = "NULL";
                break;
            }
            default: {
                // 字符串字段: 保留原始字节(SHAPE_ENCODING="" 已禁用 GDAL 自动转码),
                // 由 IdentifyHit::applyEncoding 按用户所选编码转成 UTF-8。
                a.isString = true;
                const char* s = OGR_F_GetFieldAsString(f, k);
                if (s) {
                    const char* e = s;
                    while (*e) e++;
                    a.raw.assign((const unsigned char*)s, (const unsigned char*)e);
                }
            }
        }
    } else {
        a.value = "NULL";
    }
    attrs.push_back(std::move(a));
}

}  // namespace

void IdentifyHit::applyEncoding(TextEncoding enc) {
    encoding = enc;
    for (auto& a : attrs) {
        // rawName 为空(程序内部生成的字段, 如栅格识别的"像方 列/行"等, name 里已是 UTF-8)
        // 时保持原字段名不动; 否则视为文件原始字节按所选编码重转。
        if (!a.rawName.empty()) a.name = decodeRawToUtf8(a.rawName, enc);
        if (a.isString) a.value = decodeRawToUtf8(a.raw, enc);
    }
}

bool loadVectorFile(const std::string& path, VectorData& out) {
    ensureGdal();

    GDALDatasetH ds = GDALOpenEx(path.c_str(),
                                 GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) {
        fprintf(stderr, "[gdal] open failed: %s\n", path.c_str());
        return false;
    }

    int nLayer = GDALDatasetGetLayerCount(ds);
    if (nLayer == 0) {
        fprintf(stderr, "[gdal] no layers: %s\n", path.c_str());
        GDALClose(ds);
        return false;
    }

    VectorData vd;
    extractOne(ds, 0, vd);
    vd.name = baseName(path);  // 单图层时以文件名命名
    out = std::move(vd);
    GDALClose(ds);
    fprintf(stdout, "[gdal] loaded %s  crs=%s  verts=%lld  feats=%lld  layers=%d\n",
            out.name.c_str(), out.sourceCrs.c_str(),
            (long long)out.vertices.size(), out.featureCount, nLayer);
    return true;
}

bool loadVectorFileAll(const std::string& path, std::vector<VectorData>& out) {
    ensureGdal();
    out.clear();

    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) {
        fprintf(stderr, "[gdal] open failed: %s\n", path.c_str());
        return false;
    }
    int nLayer = GDALDatasetGetLayerCount(ds);
    if (nLayer == 0) {
        fprintf(stderr, "[gdal] no layers: %s\n", path.c_str());
        GDALClose(ds);
        return false;
    }
    for (int li = 0; li < nLayer; li++) {
        VectorData vd;
        extractOne(ds, li, vd);
        out.push_back(std::move(vd));
    }
    GDALClose(ds);
    fprintf(stdout, "[gdal] loaded %d layers from %s\n", nLayer, baseName(path).c_str());
    return true;
}

bool readLayerMetadata(const std::string& path, std::vector<LayerMeta>& out) {
    ensureGdal();
    out.clear();
    spdlog::info("[meta] reading metadata for: {}", path);
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) {
        fprintf(stderr, "[gdal] open failed: %s\n", path.c_str());
        return false;
    }
    int nLayer = GDALDatasetGetLayerCount(ds);
    spdlog::info("[meta] nLayer={}", nLayer);
    for (int li = 0; li < nLayer; li++) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, li);
        LayerMeta m;
        m.name = OGR_L_GetName(lyr);
        m.featureCount = OGR_L_GetFeatureCount(lyr, FALSE);
        m.selected = true;
        spdlog::info("[meta]   layer[{}] name={} features={}", li, m.name, m.featureCount);
        out.push_back(std::move(m));
    }
    GDALClose(ds);
    return true;
}

bool identifyFeatures(const std::string& path, int layerIdx,
                      double dx, double dy, double dtol,
                      int displayEpsg, int srcEpsg, IdentifyHit& out) {
    ensureGdal();
    // 显示坐标 -> 源坐标(必要时两点重投影换算容差)
    double sx = dx, sy = dy, tol = dtol;
    if (displayEpsg != 0 && srcEpsg != 0 && displayEpsg != srcEpsg) {
        std::vector<float> p0 = {(float)dx, (float)dy};
        std::vector<float> p1 = {(float)(dx + dtol), (float)dy};
        std::vector<float> ra, rb;
        if (reprojectVertices(p0, displayEpsg, srcEpsg, ra) &&
            reprojectVertices(p1, displayEpsg, srcEpsg, rb) && ra.size() >= 2) {
            sx = ra[0]; sy = ra[1];
            tol = std::fabs(rb[0] - ra[0]);
            if (!(tol > 0)) tol = dtol;
        }
    }

    // 每个文件独立锁: 取(或后台打开)该路径的只读 dataset, 等待打开完成后再查询。
    // 只锁本文件, 互不妨碍其他图层的查询。
    auto kDS = gdalKeeperEnsure(path);
    if (!kDS) return false;
    std::unique_lock<std::mutex> ul(kDS->mu);
    if (!gdalKeeperWait(kDS, ul)) return false;
    GDALDatasetH ds = kDS->ds;
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    bool hit = false;
    if (lyr) {
        OGR_L_ResetReading(lyr);
        OGR_L_SetSpatialFilterRect(lyr, sx - tol, sy - tol, sx + tol, sy + tol);
        OGRGeometryH pt = OGR_G_CreateGeometry(wkbPoint);
        OGR_G_SetPoint_2D(pt, 0, sx, sy);
        OGRFeatureH bestF = nullptr;
        double bestD = 1e300;
        long long n = 0;
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            OGRGeometryH g = OGR_F_GetGeometryRef(f);
            bool keep = false;
            if (g) {
                double d = OGR_G_Distance(g, pt);
                if (d <= tol && d < bestD) { bestD = d; keep = true; }
            }
            if (keep) {
                if (bestF) OGR_F_Destroy(bestF);
                bestF = f;
            } else {
                OGR_F_Destroy(f);
            }
            if (++n > 1000000) break;   // 保险: 超大量文件别卡死界面
        }
        OGR_G_DestroyGeometry(pt);
        if (bestF) {
            out.layerName = OGR_L_GetName(lyr);
            OGRGeometryH bg = OGR_F_GetGeometryRef(bestF);
            out.geomType = bg ? OGR_G_GetGeometryName(bg) : "";
            out.srcEpsg = srcEpsg;
            out.outline.clear();
            out.fillTris.clear();
            out.points.clear();
            if (bg) addFilledGeometry(bg, out.outline, out.fillTris, &out.points);
            out.attrs.clear();
            int nf = OGR_F_GetFieldCount(bestF);
            for (int k = 0; k < nf; k++) appendFieldAttr(bestF, k, out.attrs);
            out.applyEncoding(TextEncoding::Utf8);   // 默认按 UTF-8 显示; 界面可切换编码
            OGR_F_Destroy(bestF);
            hit = true;
        }
        OGR_L_SetSpatialFilter(lyr, nullptr);
    }
    // 不关闭 ds: 归 keeper 复用(进程内只开一次)
    return hit;
}

bool loadWktToVectorData(const std::string& name, const std::string& wkt,
                         const std::string& srcCrs, int srcEpsg, VectorData& out) {
    OGRGeometryH g = nullptr;
    std::string buf = wkt;   // OGR 会就地改写缓冲区, 必须用可变副本
    char* p = const_cast<char*>(buf.data());
    if (strncmp(buf.c_str(), "NULL", 4) == 0) return false;
    if (OGR_G_CreateFromWkt(&p, nullptr, &g) != OGRERR_NONE || !g)
        return false;

    out = VectorData{};
    out.name = name;
    out.sourceCrs = srcCrs;
    out.srcEpsg = srcEpsg;
    out.featureCount = 1;
    addFilledGeometry(g, out.vertices, out.triangles, &out.points);
    OGR_G_DestroyGeometry(g);

    if (out.vertices.empty() && out.triangles.empty() && out.points.empty())
        return false;

    double a = 0, b = 0, c = 0, d = 0;
    bool any = false;
    if (!out.vertices.empty()) { computeExtent(out.vertices, a, b, c, d); any = true; }
    if (!out.points.empty()) {
        double x0, y0, x1, y1;
        computeExtent(out.points, x0, y0, x1, y1);
        if (!any) { a = x0; b = y0; c = x1; d = y1; }
        else { a = std::min(a, x0); b = std::min(b, y0); c = std::max(c, x1); d = std::max(d, y1); }
        any = true;
    }
    if (!out.triangles.empty()) {
        double x0, y0, x1, y1;
        computeExtent(out.triangles, x0, y0, x1, y1);
        if (!any) { a = x0; b = y0; c = x1; d = y1; }
        else { a = std::min(a, x0); b = std::min(b, y0); c = std::max(c, x1); d = std::max(d, y1); }
        any = true;
    }
    if (!any) return false;
    out.minx = a; out.miny = b; out.maxx = c; out.maxy = d;
    return true;
}

}  // namespace peekg::data