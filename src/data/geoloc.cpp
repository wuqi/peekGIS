#include "data/geoloc.h"

#include <netcdf.h>

#include <gdal.h>
#include <gdal_alg.h>
#include <gdalwarper.h>
#include <cpl_conv.h>

#include "data/gdal_common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace peekg::data {
namespace {

std::string lowerExt(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = p.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

bool isNetcdfishPath(const std::string& p) {
    std::string e = lowerExt(p);
    return e == ".nc" || e == ".nc4" || e == ".cdf" || e == ".hdf" || e == ".h5" ||
           e == ".he5" || e == ".hdf5";
}

std::vector<std::string> splitTokens(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (std::isspace((unsigned char)c) || c == ',') {
            if (!cur.empty()) { r.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

// 2D 坐标变量的判定: units 是否 lon/lat 或变量名是否常见经度/纬度名
bool isLonVar(const std::string& name, const std::string& units) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (n == "lon" || n == "longitude" || n == "x") return true;
    return units.find("east") != std::string::npos;
}
bool isLatVar(const std::string& name, const std::string& units) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (n == "lat" || n == "latitude" || n == "y") return true;
    return units.find("north") != std::string::npos;
}

std::string varUnits(int ncid, int vid) {
    nc_type tmpt = NC_NAT;
    size_t lt = 0;
    if (nc_inq_att(ncid, vid, "units", &tmpt, &lt) != NC_NOERR || lt == 0) return "";
    std::string s(lt, '\0');
    if (nc_get_att_text(ncid, vid, "units", &s[0]) != NC_NOERR) return "";
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// 已打开文件里, 指定变量的 coordinates 属性是否指向 >=2 个 2D 变量(即 geolocation 数据层)
bool isGeolocVar(int ncid, int vi) {
    nc_type tmpt = NC_NAT;
    size_t lt = 0;
    if (nc_inq_att(ncid, vi, "coordinates", &tmpt, &lt) != NC_NOERR) return false;
    if (lt == 0) return false;
    std::string coords(lt, '\0');
    if (nc_get_att_text(ncid, vi, "coordinates", &coords[0]) != NC_NOERR) return false;
    int n2d = 0;
    for (auto& t : splitTokens(coords)) {
        if (t.empty()) continue;
        int cid;
        if (nc_inq_varid(ncid, t.c_str(), &cid) != NC_NOERR) continue;
        int nd = 0;
        if (nc_inq_varndims(ncid, cid, &nd) != NC_NOERR) continue;
        if (nd == 2) n2d++;
    }
    return n2d >= 2;
}

// 首维(通常 time)大小; 2D 变量返回 1
int varFirstDimSize(int ncid, int vi) {
    int nd = 0;
    if (nc_inq_varndims(ncid, vi, &nd) != NC_NOERR) return 1;
    if (nd < 3) return 1;
    int dims[NC_MAX_DIMS];
    if (nc_inq_vardimid(ncid, vi, dims) != NC_NOERR) return 1;
    size_t s = 0;
    if (nc_inq_dimlen(ncid, dims[0], &s) != NC_NOERR) return 1;
    return (int)s;
}

// 数据变量的 coordinates 里挑 lon/lat 两个 2D 变量(必须成对)
struct GridPair {
    int lonVar = -1, latVar = -1;
    std::string lonName, latName;
};
bool gridPairOf(int ncid, int dv, GridPair& g) {
    nc_type tmpt = NC_NAT;
    size_t len = 0;
    if (nc_inq_att(ncid, dv, "coordinates", &tmpt, &len) != NC_NOERR || len == 0) return false;
    std::string coords(len, '\0');
    if (nc_get_att_text(ncid, dv, "coordinates", &coords[0]) != NC_NOERR) return false;
    for (auto& t : splitTokens(coords)) {
        if (t.empty()) continue;
        int cid;
        if (nc_inq_varid(ncid, t.c_str(), &cid) != NC_NOERR) continue;
        int nd = 0;
        if (nc_inq_varndims(ncid, cid, &nd) != NC_NOERR || nd != 2) continue;
        std::string u = varUnits(ncid, cid);
        if (g.lonVar == -1 && isLonVar(t, u)) { g.lonVar = cid; g.lonName = t; }
        if (g.latVar == -1 && isLatVar(t, u)) { g.latVar = cid; g.latName = t; }
    }
    return g.lonVar != -1 && g.latVar != -1;
}

void copyVarAttrs(int in, int iv, int on, int ov, const char* skipName = nullptr) {
    int na = 0;
    if (nc_inq_varnatts(in, iv, &na) != NC_NOERR) return;
    for (int a = 0; a < na; a++) {
        char name[NC_MAX_NAME + 1] = {0};
        if (nc_inq_attname(in, iv, a, name) != NC_NOERR) continue;
        if (skipName && std::strcmp(name, skipName) == 0) continue;
        nc_copy_att(in, iv, name, on, ov);
    }
}

// 读二维整片(2D 变量)或"前导维取第 first 片"(多维变量), 写入 out 的 2D 变量。
bool copyVar2D(int in, int iv, int on, int ov, size_t first, size_t ny, size_t nx) {
    int nd = 0;
    if (nc_inq_varndims(in, iv, &nd) != NC_NOERR || nd < 2) return false;
    nc_type vt = NC_FLOAT;
    if (nc_inq_vartype(in, iv, &vt) != NC_NOERR) return false;
    std::vector<size_t> start(nd, 0), count(nd, 0);
    size_t total = ny * nx;
    for (int i = 0; i < nd; i++) {
        start[i] = (i == 0 && nd >= 3) ? first : 0;
        count[i] = (i == nd - 2) ? ny : (i == nd - 1 ? nx : 1);
    }
    size_t os[2] = {0, 0}, oc[2] = {ny, nx};
    if (vt == NC_FLOAT) {
        std::vector<float> b(total);
        if (nc_get_vara_float(in, iv, start.data(), count.data(), b.data()) != NC_NOERR) return false;
        if (nc_put_vara_float(on, ov, os, oc, b.data()) != NC_NOERR) return false;
    } else if (vt == NC_DOUBLE) {
        std::vector<double> b(total);
        if (nc_get_vara_double(in, iv, start.data(), count.data(), b.data()) != NC_NOERR) return false;
        if (nc_put_vara_double(on, ov, os, oc, b.data()) != NC_NOERR) return false;
    } else {
        // 其它类型统一浮点中转(netCDF 会按目标类型转换)
        std::vector<double> b(total);
        if (nc_get_vara_double(in, iv, start.data(), count.data(), b.data()) != NC_NOERR) return false;
        if (nc_put_vara_double(on, ov, os, oc, b.data()) != NC_NOERR) return false;
    }
    return true;
}

// 修出临时 netCDF: 只含 [数据变量(首维切片到 timeIdx) + lon/lat 2D], 剥
// time_bnds/nbnds/1D 轴/grid_mapping 等会把 GDAL netCDF 驱动绕晕的部分。
bool makeTempNetcdf(const std::string& path, const std::string& varName, size_t timeIdx,
                    const std::string& outPath) {
    int ncid = -1;
    if (nc_open(path.c_str(), NC_NOWRITE, &ncid) != NC_NOERR) return false;
    int dv = -1;
    if (nc_inq_varid(ncid, varName.c_str(), &dv) != NC_NOERR) { nc_close(ncid); return false; }
    if (!isGeolocVar(ncid, dv)) { nc_close(ncid); return false; }
    GridPair g;
    if (!gridPairOf(ncid, dv, g)) { nc_close(ncid); return false; }

    // 坐标变量用各自自身的维度; 数据变量前导维取 timeIdx, 后面的空间维沿用坐标变量维度
    int cdim0 = -1, cdim1 = -1;
    {
        int dims[2];
        if (nc_inq_vardimid(ncid, g.lonVar, dims) != NC_NOERR) { nc_close(ncid); return false; }
        // lon/lat 一般 (y,x); 若反序拿到的维度解析仍成立(仅需两个空间维大小一致)
        cdim0 = dims[0]; cdim1 = dims[1];
    }
    char dn0[NC_MAX_NAME + 1] = {0}, dn1[NC_MAX_NAME + 1] = {0};
    size_t dy = 0, dx = 0;
    if (nc_inq_dim(ncid, cdim0, dn0, &dy) != NC_NOERR) { nc_close(ncid); return false; }
    if (nc_inq_dim(ncid, cdim1, dn1, &dx) != NC_NOERR) { nc_close(ncid); return false; }

    int oncid = -1;
    if (nc_create(outPath.c_str(), NC_CLOBBER, &oncid) != NC_NOERR) { nc_close(ncid); return false; }
    std::vector<int> odims(2);
    if (nc_def_dim(oncid, dn0, dy, &odims[0]) != NC_NOERR) { nc_close(oncid); nc_close(ncid); return false; }
    if (nc_def_dim(oncid, dn1, dx, &odims[1]) != NC_NOERR) { nc_close(oncid); nc_close(ncid); return false; }

    // 2D 坐标变量原样拷贝
    for (int vid : {g.lonVar, g.latVar}) {
        if (vid < 0) continue;
        char vn[NC_MAX_NAME + 1] = {0};
        nc_type vt = NC_FLOAT;
        if (nc_inq_varname(ncid, vid, vn) != NC_NOERR) continue;
        nc_inq_vartype(ncid, vid, &vt);
        int ov = -1;
        if (nc_def_var(oncid, vn, vt, 2, odims.data(), &ov) != NC_NOERR) continue;
        copyVarAttrs(ncid, vid, oncid, ov);
    }

    // 数据变量: 2D 化(前导维取第 timeIdx 片), 保留 _FillValue 等属性(供 GDAL 掩膜)
    nc_type vt = NC_FLOAT;
    if (nc_inq_vartype(ncid, dv, &vt) != NC_NOERR) { nc_close(oncid); nc_close(ncid); return false; }
    int odv = -1;
    if (nc_def_var(oncid, varName.c_str(), vt, 2, odims.data(), &odv) != NC_NOERR) {
        nc_close(oncid); nc_close(ncid); return false;
    }
    copyVarAttrs(ncid, dv, oncid, odv, "grid_mapping");
    // 必须先退出 define mode 才能写入数据
    if (nc_enddef(oncid) != NC_NOERR) {
        nc_close(oncid); nc_close(ncid);
        fs::remove(outPath); return false;
    }

    // 全部定义结束后再写数据(define mode 里 netCDF 拒绝 put)
    for (int vid : {g.lonVar, g.latVar}) {
        if (vid < 0) continue;
        char vn[NC_MAX_NAME + 1] = {0};
        nc_inq_varname(ncid, vid, vn);
        int ov = -1;
        if (nc_inq_varid(oncid, vn, &ov) != NC_NOERR) continue;
        copyVar2D(ncid, vid, oncid, ov, 0, dy, dx);
    }
    if (!copyVar2D(ncid, dv, oncid, odv, timeIdx, dy, dx)) {
        nc_close(oncid); nc_close(ncid);
        fs::remove(outPath);
        return false;
    }
    nc_close(oncid);
    nc_close(ncid);
    return true;
}

// warped 输出里 NoData(geolocation 网格覆盖外)糊成数据最小值, 免得渲染成大片纯白
void stretchNodata(const std::string& tif) {
    GDALDatasetH t = GDALOpenEx(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr, nullptr, nullptr);
    if (!t) return;
    GDALRasterBandH b = GDALGetRasterBand(t, 1);
    int has = 0;
    double nd = GDALGetRasterNoDataValue(b, &has);
    int tw = GDALGetRasterXSize(t), th = GDALGetRasterYSize(t);
    if (!has || tw <= 0 || th <= 0) { GDALClose(t); return; }
    std::vector<float> buf((size_t)tw * th);
    if (GDALRasterIO(b, GF_Read, 0, 0, tw, th, (void*)buf.data(), tw, th, GDT_Float32, 0, 0) != CE_None) {
        GDALClose(t);
        return;
    }
    double mn = std::numeric_limits<double>::max();
    for (float v : buf)
        if (!std::isnan(v) && (double)v != nd && v < mn) mn = v;
    if (mn == std::numeric_limits<double>::max()) mn = 0;
    for (auto& v : buf)
        if ((double)v == nd) v = (float)mn;
    GDALRasterIO(b, GF_Write, 0, 0, tw, th, (void*)buf.data(), tw, th, GDT_Float32, 0, 0);
    GDALSetRasterNoDataValue(b, nd);   // 保留属性: 统计/渲染可忽略填满的边界
    GDALClose(t);
}

}  // namespace

bool geolocationVars(const std::string& path, std::vector<GeolocVar>& out) {
    out.clear();
    if (!isNetcdfishPath(path) || !fs::exists(path)) return false;
    int ncid = -1;
    if (nc_open(path.c_str(), NC_NOWRITE, &ncid) != NC_NOERR) return false;
    int nvars = 0;
    nc_inq(ncid, nullptr, &nvars, nullptr, nullptr);
    for (int vi = 0; vi < nvars; vi++) {
        char vn[NC_MAX_NAME + 1] = {0};
        if (nc_inq_varname(ncid, vi, vn) != NC_NOERR) continue;
        if (!isGeolocVar(ncid, vi)) continue;
        GeolocVar gv;
        gv.var = vn;
        gv.timeCount = varFirstDimSize(ncid, vi);
        out.push_back(std::move(gv));
    }
    nc_close(ncid);
    return !out.empty();
}

bool netcdfHasGeolocation(const std::string& path) {
    std::vector<GeolocVar> v;
    return geolocationVars(path, v);
}

std::string makeGeolocSpec(const std::string& path, const std::string& var, int timeIdx) {
    return "GEOLOC:" + path + "|" + var + "|" + std::to_string(timeIdx);
}

bool parseGeolocSpec(const std::string& spec, std::string& path, std::string& var, int& timeIdx) {
    if (spec.rfind("GEOLOC:", 0) != 0) return false;
    size_t p1 = spec.find('|');
    if (p1 == std::string::npos) return false;
    size_t p2 = spec.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;
    path = spec.substr(7, p1 - 7);
    var = spec.substr(p1 + 1, p2 - p1 - 1);
    timeIdx = std::atoi(spec.c_str() + p2 + 1);
    if (timeIdx < 0) timeIdx = 0;
    return true;
}

std::string geolocCachePath(const std::string& path, const std::string& var, int timeIdx) {
    const char* tmp = std::getenv("TEMP");
    std::string dir = tmp && *tmp ? tmp : ".";
    dir += "/peekgis-geoloc";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string key = path + "|" + var + "|" + std::to_string(timeIdx);
    std::size_t h = std::hash<std::string>{}(key);
    char hex[32];
    snprintf(hex, sizeof(hex), "%016zx", h);
    // 附上可读名, 便于排查
    std::string base;
    size_t s = var.find_last_of('.'), p = path.find_last_of("/\\");
    base = var.substr(s == std::string::npos ? 0 : s + 1);
    if (base.empty()) base = (p == std::string::npos) ? var : path.substr(p + 1);
    return dir + "/" + hex + "_" + base + ".tif";
}

bool buildGeolocationWarp(const std::string& path, const std::string& var, int timeIdx,
                          const std::string& outTif) {
    if (!fs::exists(path)) return false;

    size_t tIdx = (size_t)(timeIdx < 0 ? 0 : timeIdx);
    std::string tmpNc = outTif + ".repair.nc";
    if (!makeTempNetcdf(path, var, tIdx, tmpNc)) return false;

    ensureGdal();
    GDALDatasetH src = GDALOpenEx(tmpNc.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                  nullptr, nullptr, nullptr);
    if (!src) { fs::remove(tmpNc); return false; }
    char** gl = GDALGetMetadata(src, "GEOLOCATION");
    if (!gl || !*gl) { GDALClose(src); fs::remove(tmpNc); return false; }

    // GeoLoc transformer: 直接吃 GEOLOCATION 元数据, 与 gdalwarp 对 geolocation 源的处理一致
    void* tr = GDALCreateGeoLocTransformer(src, gl, FALSE);
    if (!tr) { GDALClose(src); fs::remove(tmpNc); return false; }

    // 21x21 网格采样源角点 -> 目标(lon/lat)坐标, 定目标范围。批量一次性调用即可。
    int sw = GDALGetRasterXSize(src), sh = GDALGetRasterYSize(src);
    const int S = 21;
    const int N = S * S;
    std::vector<double> px(N), py(N), pz(N, 0.0);
    std::vector<int> ok(N, 0);
    int k = 0;
    for (int i = 0; i < S; i++) {
        for (int j = 0; j < S; j++) {
            px[k] = sw > 1 ? (double)j * (sw - 1) / (S - 1) : 0;
            py[k] = sh > 1 ? (double)i * (sh - 1) / (S - 1) : 0;
            k++;
        }
    }
    // bDstToSrc=FALSE: 源像素/行 -> 目标(数组自带 lon/lat)坐标
    if (!GDALGeoLocTransform(tr, FALSE, N, px.data(), py.data(), pz.data(), ok.data()))
        ok.assign(N, 0);
    double bminx = std::numeric_limits<double>::max(), bminy = bminx;
    double bmaxx = -bminx, bmaxy = -bminx;
    for (int i = 0; i < N; i++) {
        if (!ok[i]) continue;
        double X = px[i], Y = py[i];
        if (X < bminx) bminx = X;
        if (Y < bminy) bminy = Y;
        if (X > bmaxx) bmaxx = X;
        if (Y > bmaxy) bmaxy = Y;
    }
    if (bminx == std::numeric_limits<double>::max()) {
        GDALDestroyGeoLocTransformer(tr); GDALClose(src); fs::remove(tmpNc);
        return false;
    }
    // 目标像元大小 ≈ 源像元对应角度跨度, 保持网格数量级
    double xres = (bmaxx - bminx) / std::max(1, sw - 1);
    double yres = (bmaxy - bminy) / std::max(1, sh - 1);
    if (!(xres > 0)) xres = (bmaxx - bminx) / 100;
    if (!(yres > 0)) yres = (bmaxy - bminy) / 100;
    if (!(xres > 0) || !(yres > 0)) {
        GDALDestroyGeoLocTransformer(tr); GDALClose(src); fs::remove(tmpNc);
        return false;
    }
    int w = (int)std::lround((bmaxx - bminx) / xres) + 1;
    int h = (int)std::lround((bmaxy - bminy) / yres) + 1;
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w > 16384) w = 16384;
    if (h > 16384) h = 16384;

    double gt[6] = { bminx, xres, 0, bmaxy, 0, -yres };
    // 此数据集是 curl 网格(lon/lat 在行列方向均非单调), GDALGeoLocTransform 的反向(按
    // (lon,lat) 搜最近源像素)基本全部失败 -> 会在 ~99% 输出像素填 NoData。正算(源
    // 像素/行 -> lon/lat)是可靠的, 因此这里用"前向散射"代替反算聚集:
    // 每个源像素摆到其真实地理坐标的最近输出单元, 免去反算搜索。
    const double ndv = 9.9692099683868690e+36;
    std::vector<float> dst((size_t)w * h, (float)ndv);
    std::vector<float> ddist((size_t)w * h, std::numeric_limits<float>::max());
    // 重叠取舍: 距输出单元中心最近者胜(近似 nearest)
    GDALRasterBandH sband = GDALGetRasterBand(src, 1);
    std::vector<float> row(sw);
    for (int line = 0; line < sh; line++) {
        if (GDALRasterIO(sband, GF_Read, 0, line, sw, 1, (void*)row.data(),
                         sw, 1, GDT_Float32, 0, 0) != CE_None)
            continue;
        for (int k = 0; k < sw; k++) { px[k] = (double)k; py[k] = (double)line; }
        (void)GDALGeoLocTransform(tr, FALSE, sw, px.data(), py.data(), pz.data(), ok.data());
        for (int k = 0; k < sw; k++) {
            if (!ok[k] || !std::isfinite(px[k]) || !std::isfinite(py[k])) continue;
            int c = (int)std::lround((px[k] - bminx) / xres);
            int r = (int)std::lround((bmaxy - py[k]) / yres);
            if (c < 0 || c >= w || r < 0 || r >= h) continue;
            double cx = bminx + ((double)c + 0.5) * xres;
            double cy = bmaxy - ((double)r + 0.5) * yres;
            float dd = (float)((px[k] - cx) * (px[k] - cx) + (py[k] - cy) * (py[k] - cy));
            size_t o = (size_t)r * w + c;
            if (dd < ddist[o]) { ddist[o] = dd; dst[o] = row[k]; }
        }
    }
    // 目标 SRS 直接从 GEOLOCATION 元数据取(自带完整 WKT, 不依赖 OSR 上下文)
    std::string dstWkt;
    for (char** p = gl; *p; p++) {
        if (strncmp(*p, "SRS=", 4) == 0) { dstWkt = *p + 4; break; }
    }
    if (dstWkt.empty())
        dstWkt = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";
    // MEM -> GTiff, 保持 LZW 压缩
    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    GDALDatasetH mds = memDrv ? GDALCreate(memDrv, "", w, h, 1, GDT_Float32, nullptr) : nullptr;
    const char* opts[] = { "COMPRESS=LZW", "BIGTIFF=IF_SAFER", nullptr };
    GDALDatasetH out = nullptr;
    if (mds) {
        GDALSetGeoTransform(mds, gt);
        GDALSetProjection(mds, dstWkt.c_str());
        GDALRasterBandH mb = GDALGetRasterBand(mds, 1);
        GDALSetRasterNoDataValue(mb, ndv);
        if (GDALRasterIO(mb, GF_Write, 0, 0, w, h, (void*)dst.data(), w, h, GDT_Float32, 0, 0) == CE_None)
            out = GDALCreateCopy(GDALGetDriverByName("GTiff"), outTif.c_str(), mds,
                                 FALSE, (char**)opts, nullptr, nullptr);
        GDALClose(mds);
    }
    GDALDestroyGeoLocTransformer(tr);
    GDALClose(src);
    if (!std::getenv("PEEK_KEEP_REPAIR")) fs::remove(tmpNc);

    if (!out) { fs::remove(outTif); return false; }
    GDALClose(out);
    stretchNodata(outTif);
    return fs::exists(outTif);
}

}  // namespace peekg::data