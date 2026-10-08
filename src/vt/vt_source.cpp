#include "vt/vt_source.h"
#include "data/gdal_common.h"

#include <chrono>
#include <mutex>
#include <ogr_api.h>
#include <unordered_map>

namespace peekg::vt {

using peekg::data::ensureGdal;
using peekg::data::gdalOpenVector;
using peekg::data::gdalSrsEpsg;

bool ProbeBudget::expired(const void* startClock) const {
    if (cancelled()) return true;
    if (budgetSec <= 0.0) return false;
    const auto* t0 = static_cast<const std::chrono::steady_clock::time_point*>(startClock);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - *t0).count() > budgetSec;
}

namespace {
// 会话级图层元数据缓存: 要素计数是这里唯一的昂贵项(大表 COUNT(*) 十秒级),
// 同一源只付一次。featureCount = -1 表示"未计数/未知"(尚未升级为完整条目)。
std::mutex gLiMtx;
std::unordered_map<std::string, LayerInfo> gLiCache;

std::string liKey(const std::string& path, int layerIdx) {
    return path + '#' + std::to_string(layerIdx);
}
}  // namespace

bool readVtLayerInfo(const std::string& path, int layerIdx, LayerInfo& out, bool withCount,
                     const ProbeBudget* budget) {
    ensureGdal();
    const std::string key = liKey(path, layerIdx);
    {
        std::lock_guard<std::mutex> lk(gLiMtx);
        auto it = gLiCache.find(key);
        if (it != gLiCache.end() && (it->second.featureCount >= 0 || !withCount)) {
            out = it->second;
            return true;
        }
    }
    if (budget && budget->cancelled()) return false;
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return false;
    LayerInfo li;
    bool ok = false;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx >= 0 && layerIdx < nl) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
        const char* nm = OGR_L_GetName(lyr);
        li.name = nm ? nm : "";
        // 要素数: 有预算且已超时就置 -1(=未知)而不是硬等。打开流程要的是"要不要走 v2",
        // 拿不到精确数时宁可退回 v1.0 也不要卡住界面。
        const auto t0 = std::chrono::steady_clock::now();
        if (!withCount) li.featureCount = -1;
        else if (budget && budget->expired(&t0)) li.featureCount = -1;
        else li.featureCount = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
        li.geomType = (int)OGR_L_GetGeomType(lyr);
        li.srcEpsg = gdalSrsEpsg(OGR_L_GetSpatialRef(lyr));
        OGREnvelope env;
        // 先用 bForce=FALSE: 驱动能从头部/索引直接给范围时是毫秒级。bForce=TRUE 会在拿不到
        // 现成范围时**扫全表**(大表/压缩包上就是几十秒到几分钟), 而且那是单次 GDAL 调用,
        // 外面的取消位根本插不进去 —— 打开阶段卡死的一大来源。
        if (OGR_L_GetExtent(lyr, &env, FALSE) != OGRERR_NONE ||
            env.MaxX < env.MinX || env.MaxY < env.MinY) {
            // 只有驱动确实没有现成范围时才付全表扫描的代价
            if (OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE &&
                env.MaxX >= env.MinX && env.MaxY >= env.MinY) {
                li.hasExtent = true;
                li.minx = env.MinX; li.miny = env.MinY;
                li.maxx = env.MaxX; li.maxy = env.MaxY;
            }
        } else {
            li.hasExtent = true;
            li.minx = env.MinX; li.miny = env.MinY;
            li.maxx = env.MaxX; li.maxy = env.MaxY;
        }
        ok = true;
    }
    GDALClose(ds);
    if (!ok) return false;
    {
        std::lock_guard<std::mutex> lk(gLiMtx);
        auto it = gLiCache.find(key);
        if (it == gLiCache.end()) {
            gLiCache.emplace(key, li);
            out = li;
        } else {
            if (it->second.featureCount < 0 && li.featureCount >= 0) it->second = li;  // 升级为完整条目
            else if (li.featureCount >= 0 && withCount) it->second = li;               // 刷新
            out = it->second;
        }
    }
    return true;
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
        // 曲线几何(GPKG 的 MULTICURVE/COMPOUNDCURVE 等; 直线内容也会按曲线类型存储):
        // 容器逐层下钻(子件多为 LINESTRING, 零额外开销); 叶子曲线/曲线面线性化后重新分发。
        // 不处理则全部落 default 丢弃 -> 0 环 -> 空缓存(实测 1640 万要素的 US roads)。
        case wkbCompoundCurve:
        case wkbMultiCurve:
        case wkbMultiSurface: {
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; ++i)
                emitGeom(OGR_G_GetGeometryRef(g, i), ct, polyCounter, featureIdx, sink);
            break;
        }
        case wkbCircularString:
        case wkbCurvePolygon:
        case wkbCurve:
        case wkbSurface: {
            OGRGeometryH lin = OGR_G_GetLinearGeometry(g, 0.0, nullptr);
            if (lin) {
                if (!OGR_G_HasCurveGeometry(lin, FALSE))   // 防递归: 线性化结果必须是线性类型
                    emitGeom(lin, ct, polyCounter, featureIdx, sink);
                OGR_G_DestroyGeometry(lin);
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace

long long streamVtRings(const std::string& path, int layerIdx, int dstEpsg,
                        const std::function<void(const SourceRing&)>& sink,
                        const ProbeBudget* budget) {
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
    const auto t0 = std::chrono::steady_clock::now();
    OGRFeatureH f;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        // 每 256 个要素查一次时钟/取消位: 既别让"整趟读"变成不可中断的长循环, 也别每个要素都取时钟
        if (budget && (nfeat & 255) == 0 && budget->expired(&t0)) { OGR_F_Destroy(f); break; }
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) emitGeom(g, ct, polyCounter, nfeat, sink);
        OGR_F_Destroy(f);
        ++nfeat;
    }

    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    return nfeat;
}

long long estimateSourceVerts(const std::string& path, int layerIdx, long long sampleK,
                              const ProbeBudget* budget, bool* outPartial) {
    if (outPartial) *outPartial = false;
    ensureGdal();
    // 预算已置位: 什么都不做, 返回 -1(=未知)。上层会退回 v1.0 路径, 而不是误判成"空数据"。
    if (budget && budget->cancelled()) return -1;
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) return -1;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return -1; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    const auto t0 = std::chrono::steady_clock::now();
    // 全表精确计数: 没有预算就照旧; 有预算且已经超时就跳过(后面只靠采样比例推算)
    long long F = 0;
    if (budget) {
        if (!budget->expired(&t0)) F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    } else {
        F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    }
    const bool gotCount = (F > 0);
    // 预算判定必须挂在**迭代计数**上, 不能挂在"有几何的要素数"上。
    // 之前写成 `n >= kMinSample && (n & 255) == 0` 而 n 只在拿到几何时才自增: 一旦要素几何
    // 读不出来(压缩包里部分记录损坏/驱动解析失败), n 永远是 0 -> 预算检查永远不执行 ->
    // 循环只能等 GetNextFeature 把整表读完。tl_2025_06_tabblock20.zip 上就是这样把界面
    // 挂在"分析中…"好几分钟: 预算形同虚设。
    constexpr long long kMinIter = 256;
    long long sum = 0, n = 0, iter = 0;
    bool bailed = false;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while (n < sampleK && (f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        if (budget && iter >= kMinIter && (iter & 255) == 0 && budget->expired(&t0)) {
            if (outPartial) *outPartial = true;
            bailed = true;
            OGR_F_Destroy(f);
            break;
        }
        ++iter;
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) { sum += countPoints(g); ++n; }
        OGR_F_Destroy(f);
    }
    GDALClose(ds);
    if (n == 0) return bailed ? -1 : 0;   // 一个带几何的都没读到: 未知(-1), 不能报 0(会被当成空数据)
    if (!gotCount) {
        // 没拿到精确要素数: 按"采样段本身说明数据不小"给个保守下界, 只用于路由决策
        if (outPartial) *outPartial = true;
        return (long long)((double)sum / (double)n * (double)sampleK / 10.0);
    }
    double mean = (double)sum / (double)n;
    return (long long)(mean * (double)F);
}

}  // namespace peekg::vt
