#pragma once
// 矢量图层空间索引: 能力位检测 + 后台补建。依据 docs/空间索引.md:
//   - shapefile:  ExecuteSQL("CREATE SPATIAL INDEX ON <图层名>")   -> 同名 .qix
//   - gpkg:       ExecuteSQL("SELECT CreateSpatialIndex('<表>','<几何列>')")
//   - PostgreSQL: ExecuteSQL("CREATE INDEX ... USING GIST (<几何列>)")  (GiST, GDAL 只是 SQL 通道)
//   - FlatGeobuf: 不支持原地建索引(需 ogr2ogr 重写), 直接返回 Failed
// vt 直读用 OLCFastSpatialFilter 判定能否走空间过滤; 无索引的大表会被直接判死
// (见 vt_build.cpp RawRegionStream::open), 所以这里给用户一个手动补救入口。
#include "data/gdal_common.h"
#include <ogr_api.h>

#include <string>

namespace peekg::data {

// 该矢量源是否已有可用的空间索引(只读打开, 查能力位)。
// path 可以是文件路径, 也可以是 PG 连接串(postgresql:// 或 PG:)。
inline bool vectorHasSpatialIndex(const std::string& path, int layerIdx = 0) {
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return false;
    bool ok = false;
    if (layerIdx >= 0 && layerIdx < GDALDatasetGetLayerCount(ds)) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
        if (lyr) ok = OGR_L_TestCapability(lyr, OLCFastSpatialFilter) != 0;
    }
    GDALClose(ds);
    return ok;
}

enum class QixResult { Ok, NoGeometry, Failed };

inline std::string sqlQuote(const std::string& s) {   // SQL 字符串字面量 '...'
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "''"; else o += c; }
    o += "'";
    return o;
}

inline std::string sqlIdent(const std::string& s) {   // SQL 标识符 "..."(PG/GPKG 用)
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    o += "\"";
    return o;
}

// 几何列名: OGR_L_GetGeometryColumn(gpkg/PG 驱动都会填; shapefile 返回 null 但不需要)。
// gpkg 拿不到时回查 gpkg_geometry_columns 元数据表(几何列名是用户自定的, 不能硬编码)。
inline std::string layerGeometryColumn(GDALDatasetH ds, OGRLayerH lyr,
                                       const std::string& layer, const std::string& drv) {
    if (lyr) {
        if (const char* g = OGR_L_GetGeometryColumn(lyr)) {
            if (*g) return g;
        }
    }
    if (drv == "GPKG" && ds) {
        std::string q = "SELECT column_name FROM gpkg_geometry_columns WHERE table_name = "
                        + sqlQuote(layer);
        OGRLayerH res = GDALDatasetExecuteSQL(ds, q.c_str(), nullptr, nullptr);
        std::string out;
        if (res) {
            OGRFeatureH f = OGR_L_GetNextFeature(res);
            if (f) {
                const char* v = OGR_F_GetFieldAsString(f, 0);
                if (v && *v) out = v;
                OGR_F_Destroy(f);
            }
            GDALDatasetReleaseResultSet(ds, res);
        }
        return out;
    }
    return {};
}

// 按驱动选语句补建空间索引(见 docs/空间索引.md)。必须在后台线程调用(大表数秒~数十秒)。
inline QixResult buildVectorSpatialIndex(const std::string& path, int layerIdx = 0) {
    ensureGdal();
    // 建索引需要写权限: 文件被本程序 keeper 持有只读句柄不影响(驱动允许再开写句柄),
    // PG 则要求连接串里有写权限的账号。
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                nullptr, nullptr, nullptr);
    if (!ds) {
        spdlog::warn("[qix] 无法以写模式打开(文件被占用/无写权限?): {}", path);
        return QixResult::Failed;
    }
    if (layerIdx < 0 || layerIdx >= GDALDatasetGetLayerCount(ds)) {
        GDALClose(ds);
        return QixResult::Failed;
    }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    const char* lname = lyr ? OGR_L_GetName(lyr) : nullptr;
    if (!lyr || !lname || !*lname) {
        GDALClose(ds);
        return QixResult::NoGeometry;
    }
    const std::string layer = lname;
    GDALDriverH dr = GDALGetDatasetDriver(ds);
    const char* drvName = dr ? GDALGetDriverShortName(dr) : nullptr;
    const std::string drv = drvName ? drvName : "";
    const std::string gcol = layerGeometryColumn(ds, lyr, layer, drv);

    std::string sql;
    if (drv == "GPKG") {
        if (gcol.empty()) {
            spdlog::warn("[qix] gpkg 找不到几何列: {}", layer);
            GDALClose(ds);
            return QixResult::NoGeometry;
        }
        sql = "SELECT CreateSpatialIndex(" + sqlQuote(layer) + ", " + sqlQuote(gcol) + ")";
    } else if (drv == "PostgreSQL" || drv == "PGSQL") {
        if (gcol.empty()) {
            spdlog::warn("[qix] PG 找不到几何列: {}", layer);
            GDALClose(ds);
            return QixResult::NoGeometry;
        }
        // 幂等: 已存在同名索引时跳过(不是错误)
        sql = "CREATE INDEX IF NOT EXISTS " + sqlIdent(layer + "_geom_idx") +
              " ON " + sqlIdent(layer) + " USING GIST (" + sqlIdent(gcol) + ")";
    } else if (drv == "ESRI Shapefile") {
        sql = "CREATE SPATIAL INDEX ON " + layer;   // 几何列由驱动自取, 产出同名 .qix
    } else {
        // FlatGeobuf 等: 无原地索引方案(docs/空间索引.md: fgb 需 ogr2ogr 重写)
        spdlog::warn("[qix] 驱动 {} 不支持原地建空间索引: {}", drv, path);
        GDALClose(ds);
        return QixResult::Failed;
    }

    spdlog::info("[qix] 建索引: 驱动={} 表={} 几何列={} SQL={}", drv, layer, gcol, sql);
    CPLErrorReset();
    OGRLayerH res = GDALDatasetExecuteSQL(ds, sql.c_str(), nullptr, nullptr);
    bool sqlErr = (CPLGetLastErrorType() == CE_Failure);
    std::string err = CPLGetLastErrorMsg();
    // 该类语句不返回结果集, 返回 null 属正常, 不能据此判失败
    if (res) GDALDatasetReleaseResultSet(ds, res);
    GDALClose(ds);
    if (sqlErr) spdlog::warn("[qix] SQL 报错: {}", err);

    // 判据: 关掉重开, 能力位是否变为可用
    if (vectorHasSpatialIndex(path, layerIdx)) return QixResult::Ok;
    return QixResult::Failed;
}

}  // namespace peekg::data
