#include "data/reproject.h"
#include "data/gdal_common.h"
#include <ogr_spatialref.h>
#include <algorithm>
#include <cmath>

namespace peekg::data {
bool reprojectVertices(const std::vector<float>& src, int srcEpsg, int dstEpsg,
                       std::vector<float>& dst) {
    dst.clear();
    if (src.empty()) return true;
    if (dstEpsg == 0 || srcEpsg == dstEpsg) { dst = src; return true; }

    ensureGdal();

    OGRSpatialReference oSrc, oDst;
    if (oSrc.importFromEPSG(srcEpsg) != OGRERR_NONE ||
        oDst.importFromEPSG(dstEpsg) != OGRERR_NONE) {
        return false;
    }
    // GDAL3/PROJ6 默认按 SRS 轴序(4326 为 lat,lon); 统一改为传统 GIS 序(lon,lat / x,y)
    oSrc.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    oDst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&oSrc, &oDst);
    if (!ct) return false;

    int n = (int)(src.size() / 2);
    std::vector<double> xs(n), ys(n);
    for (int i = 0; i < n; i++) { xs[i] = src[2 * i]; ys[i] = src[2 * i + 1]; }

    int ok = ct->Transform(n, xs.data(), ys.data(), nullptr);
    if (ok) {
        dst.resize(src.size());
        for (int i = 0; i < n; i++) {
            dst[2 * i] = (float)xs[i];
            dst[2 * i + 1] = (float)ys[i];
        }
    }

    OGRCoordinateTransformation::DestroyCT(ct);
    return ok != 0;
}

bool reprojectPoint(double x, double y, int srcEpsg, int dstEpsg, double& ox, double& oy) {
    if (srcEpsg == 0 || dstEpsg == 0 || srcEpsg == dstEpsg) { ox = x; oy = y; return true; }
    ensureGdal();
    OGRSpatialReference oSrc, oDst;
    if (oSrc.importFromEPSG(srcEpsg) != OGRERR_NONE ||
        oDst.importFromEPSG(dstEpsg) != OGRERR_NONE) return false;
    oSrc.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    oDst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&oSrc, &oDst);
    if (!ct) return false;
    ox = x; oy = y;
    int ok = ct->Transform(1, &ox, &oy, nullptr);
    OGRCoordinateTransformation::DestroyCT(ct);
    return ok != 0;
}

void computeExtent(const std::vector<float>& v,
                   double& minx, double& miny, double& maxx, double& maxy) {
    if (v.empty()) { minx = miny = maxx = maxy = 0; return; }
    minx = miny = 1e300;
    maxx = maxy = -1e300;
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        minx = std::min(minx, (double)v[i]);
        maxx = std::max(maxx, (double)v[i]);
        miny = std::min(miny, (double)v[i + 1]);
        maxy = std::max(maxy, (double)v[i + 1]);
    }
}

}  // namespace peekg::data
