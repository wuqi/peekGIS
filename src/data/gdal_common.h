#pragma once
#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <ogr_srs_api.h>
#include "platform/exe_path.h"
#include "util/logger.h"

#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <memory>
#include <thread>
#include <cstdlib>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <system_error>

// ---------------------------------------------------------------------------
// EPSG 推导: 尽力把 GDAL 的 OGRSpatialReference 归一成整数 EPSG。
// 很多国产 shp 的 .prj 是 ArcGIS 私有命名(如 CGCS2000 写成 DATUM["D_2000"]/
// SPHEROID["S_2000"]), GDAL 拿不到权威码 -> 有投影几何却 srcEpsg=0, 无法重投影。
// 这里依次尝试: ① 权威码; ② ESRI morph 后重查权威码; ③ WKT 名称启发式兜底。
// 返回 0 表示无法确定。
// ---------------------------------------------------------------------------
namespace peekg::data {
inline int gdalSrsEpsg(OGRSpatialReferenceH srs) {
    if (!srs) return 0;
    int epsg = 0;
    const char* auth = OSRGetAuthorityName(srs, nullptr);
    const char* code = OSRGetAuthorityCode(srs, nullptr);
    if (auth && code && *code) epsg = std::atoi(code);
    if (epsg == 0) {
        OGRSpatialReferenceH clone = OSRClone(srs);
        if (clone) {
            OSRMorphFromESRI(clone);
            // AutoIdentifyEPSG: 对老式 ESRI/自定义 WKT(如 TWD97_TM2_zone_121)补识别权威码
            if (OSRAutoIdentifyEPSG(clone) == OGRERR_NONE) {
                const char* a2 = OSRGetAuthorityName(clone, nullptr);
                const char* c2 = OSRGetAuthorityCode(clone, nullptr);
                if (a2 && c2 && *c2 && strcmp(a2, "EPSG") == 0) epsg = std::atoi(c2);
            }
            OSRDestroySpatialReference(clone);
        }
    }
    if (epsg == 0) {
        char* wkt = nullptr;
        if (OSRExportToWkt(srs, &wkt) == OGRERR_NONE && wkt) {
            std::string t = wkt;
            CPLFree(wkt);
            // 只对地理坐标系(GEOGCS, 无 PROJCS)做名称兜底; 投影系带分带号, 无法可靠猜
            if (t.find("PROJCS") == std::string::npos) {
                auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
                if (has("CGCS") || has("D_2000") || has("S_2000") ||
                    has("China Geodetic Coordinate System 2000") || has("2000"))
                    epsg = 4490;                       // CGCS2000 地理
                else if (has("Xian_1980") || has("D_Xian_1980") || has("Xian 1980"))
                    epsg = 4610;                       // 西安1980 地理
                else if (has("Beijing_1954") || has("D_Beijing_1954") || has("Beijing 1954"))
                    epsg = 4214;                       // 北京1954 地理
                else if (has("WGS_1984") || has("WGS 84"))
                    epsg = 4326;                       // WGS84 地理
            }
        }
    }
    return epsg;
}

// EPSG 码是否为地理坐标系(经纬度,度)。自动选显示基准 CRS 时优先用地理系,
// 以避免把异半球/异分带的数据重投影到错误位置(如栅格 UTM 55S 南半球 + 矢量
// CGCS2000 北半球: 若基准取投影系, 矢量会被扔到千万米之外而不可见)。
inline bool epsgIsGeographic(int epsg) {
    if (epsg == 0) return false;
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    if (!srs) return false;
    bool geo = (OSRImportFromEPSG(srs, epsg) == OGRERR_NONE) && (OSRIsGeographic(srs) == 1);
    OSRDestroySpatialReference(srs);
    return geo;
}

// 捕获 GDAL/OGR 最近一次错误信息, 便于在打开失败时展示给用户
inline std::mutex& gdalErrMtx() { static std::mutex m; return m; }
inline std::string& gdalErrBuf() { static std::string s; return s; }
inline void CPL_STDCALL peekGdalErrHandler(CPLErr, int, const char* msg) {
    if (msg) {
        // 过滤 PROJ 的 "another PROJ installation" 噪音: 那是机器上残留的旧版(如 PostgreSQL)
        // proj.db 被 PROJ 默认扫描路径发现所致, 程序已用自带库, 报错毫无用处。
        if (std::string(msg).find("another PROJ installation") != std::string::npos) return;
        fprintf(stderr, "[gdal] %s\n", msg);  // 同时打到 stderr, 方便排查
        std::lock_guard<std::mutex> g(gdalErrMtx());
        gdalErrBuf() = msg;
    }
}

// 统一初始化: 设置 GDAL_DATA/PROJ_LIB(指向 exe 旁 share), 并注册驱动(只做一次)
inline void ensureGdal() {
    static bool inited = false;
    if (inited) return;
    // 尽早挂错误处理: 必须在 GDALAllRegister 之前, 否则注册驱动时 PROJ 扫描
    // 机器上其它旧 proj.db(如 PostgreSQL 附属)会由 GDAL 默认处理器往 stderr 刷噪音。
    CPLSetErrorHandler(peekGdalErrHandler);
    std::string d = exeDir();
    if (!d.empty()) {
        std::string gdalData = d + "/share/gdal";
        std::string projData = d + "/share/proj";
        // GDAL/PROJ 的数据路径必须真正写进进程环境变量(GDAL_DATA / PROJ_LIB)才生效:
        // 单用 CPLSetConfigOption 只影响 GDAL 配置项, PROJ 走 getenv("PROJ_LIB") 读到的是
        // 系统/用户已设置的值(如 PostgreSQL 附属的旧 proj.db), 从而投影解析走错数据库。
        _putenv_s("GDAL_DATA", gdalData.c_str());
        _putenv_s("PROJ_LIB", projData.c_str());
        _putenv_s("PROJ_DATA", projData.c_str());   // PROJ 9.x 用 PROJ_DATA(不再读 PROJ_LIB)
        CPLSetConfigOption("GDAL_DATA", gdalData.c_str());
        CPLSetConfigOption("PROJ_LIB", projData.c_str());
        CPLSetConfigOption("PROJ_DATA", projData.c_str());
        // 再强制 PROJ 搜索路径指向随程序携带的 proj.db, 双保险。
        std::error_code projEc;
        if (std::filesystem::exists(projData + "/proj.db", projEc)) {
            const char* projPaths[2] = { projData.c_str(), nullptr };   // NULL 结尾列表
            OSRSetPROJSearchPaths(projPaths);
        }
        spdlog::info("GDAL_DATA={} exists={}", gdalData, std::filesystem::exists(gdalData) ? "Y" : "N");
        spdlog::info("PROJ_LIB ={} exists={}", projData, std::filesystem::exists(projData) ? "Y" : "N");
    } else {
        spdlog::warn("exeDir() 为空, 未设置 GDAL_DATA/PROJ_LIB");
    }
    GDALAllRegister();
    OGRRegisterAll();  // 确保矢量驱动(某些 GDAL 构建需要显式注册)
    // 注意: 绝不在全局设置 SHAPE_RESTORE_SHX=YES —— 那会让 GDAL 每次打开都重写用户
    // 的 .shx。保持 GDAL 默认行为(仅当 .shx 缺失时自动重建; 已存在的绝不动)。
    // 让 shapefile 的 .dbf 字符串字段返回【原始字节】(不按 .cpg/LDID 自动转码), 交由
    // 属性面板按用户所选编码(GBK/Big5/CP1252/UTF-8/UTF-16)固定解释。见 docs/属性表设计.md §2.4。
    // 仅影响 DBF 字符串字段的编解码, 不影响几何读取。
    CPLSetConfigOption("SHAPE_ENCODING", "");
    inited = true;
}

inline void gdalErrClear() { std::lock_guard<std::mutex> g(gdalErrMtx()); gdalErrBuf().clear(); }
inline std::string gdalErrLast() { std::lock_guard<std::mutex> g(gdalErrMtx()); return gdalErrBuf(); }

// ---------------------------------------------------------------------------
// 打开归口: 纯只读打开, 绝不重写用户文件。保持 GDAL 默认(index 缺失才重建),
// 已存在的 .shx 原样使用(毫秒级打开); 不设置任何 SHAPE_RESTORE_SHX 选项。
// 并发读线程也走这里——永远不会触发索引重建, 不会出现多线程抢写同一 .shx。
// ---------------------------------------------------------------------------
inline GDALDatasetH gdalOpenVector(const std::string& path) {
    ensureGdal();
    return GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                      nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// 打开 dataset 复用(identify 等高频查询的关键优化: 避免每次双击都重开文件,
// 大 shapefile 即使 .shx 正常, 也不需要多次重建内存要素表)。
// 每个路径一个独立条目(per-path mutex/cv): 某个文件的打开/查询不阻塞其他文件。
// ---------------------------------------------------------------------------
struct KeptDs {
    std::mutex mu;                 // 保护 ds + 串行化同一文件的矢量读取
    std::condition_variable cv;
    GDALDatasetH ds = nullptr;
    bool opening = false;          // 后台打开进行中(首次时置位, 查询线程等 cv)
    bool openDone = false;
};

inline std::mutex& g_kdMapMtx() {
    static std::mutex m;
    return m;
}
inline std::map<std::string, std::shared_ptr<KeptDs>>& g_kdMap() {
    static std::map<std::string, std::shared_ptr<KeptDs>> m;
    return m;
}

// 取(或发起后台打开)指定路径的条目。不阻塞调用方。
inline std::shared_ptr<KeptDs> gdalKeeperEnsure(const std::string& path) {
    ensureGdal();
    std::lock_guard<std::mutex> g(g_kdMapMtx());
    auto& slot = g_kdMap()[path];
    if (!slot) {
        slot = std::make_shared<KeptDs>();
        slot->opening = true;
        std::string p = path;
        std::weak_ptr<KeptDs> w = slot;
        std::thread([p, w]() {
            auto k = w.lock();
            if (!k) return;
            gdalErrClear();
            GDALDatasetH d = gdalOpenVector(p);
            {
                std::lock_guard<std::mutex> l(k->mu);
                k->ds = d;
                k->opening = false;
                k->openDone = true;
            }
            k->cv.notify_all();
        }).detach();
    }
    return slot;
}

// 等待条目打开完成并返回 dataset(失败返回 nullptr)。锁由调用方持有该条目进行查询。
// 后台打开线程若迟迟未完成(首开后慢/失败), 这里最多等待短暂时间即返回 false,
// 由上层作"暂未就绪"处理(可稍后重试), 避免把界面/任务永久卡死在等待上。
inline bool gdalKeeperWait(std::shared_ptr<KeptDs>& k, std::unique_lock<std::mutex>& ul) {
    // 后台打开通常 <50ms, 给足余量; 拿不到就按失败返回, 由上层补发重试。
    const int ms = 3000;
    bool opened = k->cv.wait_for(ul, std::chrono::milliseconds(ms),
                                 [&] { return k->openDone; });
    return opened && (k->ds != nullptr);
}

inline void gdalKeeperClear() {
    std::lock_guard<std::mutex> g(g_kdMapMtx());
    for (auto& e : g_kdMap()) {
        std::lock_guard<std::mutex> l(e.second->mu);
        if (e.second->ds) GDALClose(e.second->ds);
        e.second->ds = nullptr;
    }
    g_kdMap().clear();
}

}  // namespace peekg::data
