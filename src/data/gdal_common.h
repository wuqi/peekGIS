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
#include <filesystem>
#include <system_error>

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
        CPLSetConfigOption("GDAL_DATA", gdalData.c_str());
        CPLSetConfigOption("PROJ_LIB", projData.c_str());
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
inline bool gdalKeeperWait(std::shared_ptr<KeptDs>& k, std::unique_lock<std::mutex>& ul) {
    k->cv.wait(ul, [&] { return k->openDone; });
    return k->ds != nullptr;
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
