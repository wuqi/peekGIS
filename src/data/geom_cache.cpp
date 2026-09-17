#include "data/geom_cache.h"
#include "data/gdal_common.h"
#include "data/reproject.h"
#include "data/sha1.h"
#include "platform/exe_path.h"
#include "platform/path_util.h"
#include "config/app_config.h"

#include <zstd.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>

namespace fs = std::filesystem;

namespace peekg::data {

// v3 缂撳瓨: cache/<sourceId>/meta.bin  (澶撮儴+鍥惧眰鍏冧俊鎭? 鍚簮璺緞)
//          cache/<sourceId>/l0.bin    (鍥惧眰杞借嵎, zstd 鍘嬬缉鐨?verts||pts)
// index .index: one record per layer, maintains LRU and deletion; management window based on disk scan (self-healing)
static const char* kMagic = "PGC3";
static const uint32_t kVersion = 4;
// v4: layer files carry magic + version, added face-fill triangles
static const char* kLayerMagic = "PGCL";
static const uint32_t kLayerVersion = 1; // v1 鏁村眰鍗曞潡(鏃?; v2 鍒嗗潡娴佸紡(瑙佷笅)
static const uint32_t kLayerVersionChunked = 2;
static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool fileSig(const std::string& path, int64_t& mtime, int64_t& size) {
    std::error_code ec;
    fs::path p = toFsPath(path);
    if (!fs::exists(p, ec)) return false;
// folder/gdb source data: sum sizes and newest mtime of all files
if (fs::is_directory(p, ec)) {
        int64_t mt = 0, sz = 0;
        std::error_code itEc;
        for (auto it = fs::recursive_directory_iterator(p, itEc);
             it != fs::recursive_directory_iterator(); ++it) {
            std::error_code e2;
            if (!fs::is_regular_file(it->status())) continue;
            int64_t fsize = (int64_t)fs::file_size(it->path(), e2);
            if (e2) continue;
            auto ft = fs::last_write_time(it->path(), e2);
            if (e2) continue;
            int64_t fm = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(ft)
                    .time_since_epoch()).count();
            sz += fsize;
            if (fm > mt) mt = fm;
        }
        mtime = mt; size = sz;
        return true;
    }
    size = (int64_t)fs::file_size(p, ec);
    auto ft = fs::last_write_time(p, ec);
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    mtime = std::chrono::duration_cast<std::chrono::seconds>(
        sys.time_since_epoch()).count();
    return !ec;
}

static std::string cacheDir(const AppConfig& cfg) {
    std::string d = cfg.cache_dir;
    if (d.empty()) d = "cache";
    if (!toFsPath(d).is_absolute()) {
        std::string e = exeDir();
        if (!e.empty()) d = e + "/" + d;
    }
    return d;
}

static std::string sourceIdOf(const std::string& path, int64_t mtime, int64_t size) {
    return sha1_hex(path + "|" + std::to_string(mtime) + "|" + std::to_string(size));
}

static void putStr(std::ofstream& o, const std::string& s) {
    uint32_t n = (uint32_t)s.size();
    o.write((const char*)&n, 4);
    o.write(s.data(), (std::streamsize)n);
}
static bool getStr(std::ifstream& i, std::string& s) {
    uint32_t n = 0;
    i.read((char*)&n, 4);
    if (i.gcount() != 4) return false;
    s.resize(n);
    if (n && (std::streamsize)n != i.read(&s[0], n).gcount()) return false;
    return true;
}

// layer header (meta) -- does not carry geometry payload
struct MetaLayer {
    int32_t epsg = 0;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    long long featureCount = 0;
    std::string name;
    std::string sourceCrs;
};
struct MetaHead {
    int64_t mtime = 0, size = 0;
    std::string sourcePath;
    std::vector<MetaLayer> layers;
};

// 从几何(vertices/points/triangles)重算包围盒; 任一非空则合并各数组范围。返回是否取到值
static bool bboxFromGeom(const VectorData& vd, double& mnx, double& mny,
                         double& mxx, double& mxy) {
    bool any = false;
    auto merge = [&](const std::vector<float>& arr) {
        if (arr.empty()) return;
        double a, b, c, d;
        computeExtent(arr, a, b, c, d);
        if (!any) { mnx = a; mny = b; mxx = c; mxy = d; }
        else { mnx = std::min(mnx, a); mny = std::min(mny, b); mxx = std::max(mxx, c); mxy = std::max(mxy, d); }
        any = true;
    };
    merge(vd.vertices);
    merge(vd.points);
    merge(vd.triangles);
    return any;
}

// 若范围无效(哨兵/NaN)则从几何现场重算覆盖。返回是否有几何可作依据
static bool repairExtentsIfInvalid(VectorData& vd) {
    if (vd.minx <= vd.maxx && std::isfinite(vd.minx) && std::isfinite(vd.maxx) &&
        std::isfinite(vd.miny) && std::isfinite(vd.maxy))
        return false;   // 范围已有效
    if (!bboxFromGeom(vd, vd.minx, vd.miny, vd.maxx, vd.maxy)) {
        vd.minx = vd.miny = vd.maxx = vd.maxy = 0;
        return false;
    }
    return true;
}

static void writeMeta(std::ofstream& o, const std::string& srcPath, int64_t mt, int64_t sz,
                      const std::vector<VectorData>& vds) {
    o.write(kMagic, 4);
    uint32_t ver = kVersion; o.write((const char*)&ver, 4);
    o.write((const char*)&mt, 8); o.write((const char*)&sz, 8);
    putStr(o, srcPath);
    uint32_t n = (uint32_t)vds.size(); o.write((const char*)&n, 4);
    for (uint32_t k = 0; k < n; k++) {
        const VectorData& vd = vds[k];
        double mnx = vd.minx, mny = vd.miny, mxx = vd.maxx, mxy = vd.maxy;
        // 旧数据源可能未算范围(哨兵/NaN): 写缓存前从几何现场重算, 避免缓存读回后
        // 图层范围变成 (0,0,0,0) 使"适配视图"缩到原点 → 图层不可见。
        if (vd.minx > vd.maxx || !std::isfinite(vd.minx) || !std::isfinite(vd.maxx)) {
            double a = 0, b = 0, c = 0, d = 0;
            if (bboxFromGeom(vd, a, b, c, d)) { mnx = a; mny = b; mxx = c; mxy = d; }
        }
        int32_t epsg = vd.srcEpsg; o.write((const char*)&epsg, 4);
        o.write((const char*)&mnx, 8); o.write((const char*)&mny, 8);
        o.write((const char*)&mxx, 8); o.write((const char*)&mxy, 8);
        int64_t fc = vd.featureCount; o.write((const char*)&fc, 8);
        putStr(o, vd.name);
        putStr(o, vd.sourceCrs);
    }
}
// 按 MetaHead 重写 meta.bin(缓存命中修复 epsg 后, 重写单层信息)。
// 与 writeMeta 保持同一二进制布局; vds 由 MetaHead 内存结构等价输出。
static bool writeMetaHead(const std::string& metaPath, const MetaHead& h) {
    std::ofstream o(toFsPath(metaPath), std::ios::binary);
    if (!o) return false;
    o.write(kMagic, 4);
    uint32_t ver = kVersion; o.write((const char*)&ver, 4);
    o.write((const char*)&h.mtime, 8); o.write((const char*)&h.size, 8);
    putStr(o, h.sourcePath);
    uint32_t n = (uint32_t)h.layers.size(); o.write((const char*)&n, 4);
    for (const MetaLayer& ml : h.layers) {
        int32_t epsg = ml.epsg; o.write((const char*)&epsg, 4);
        o.write((const char*)&ml.minx, 8); o.write((const char*)&ml.miny, 8);
        o.write((const char*)&ml.maxx, 8); o.write((const char*)&ml.maxy, 8);
        int64_t fc = ml.featureCount; o.write((const char*)&fc, 8);
        putStr(o, ml.name);
        putStr(o, ml.sourceCrs);
    }
    return true;
}
static bool readMeta(const std::string& metaPath, MetaHead& h) {
    std::ifstream i(toFsPath(metaPath), std::ios::binary);
    if (!i) return false;
    char magic[4] = {0};
    i.read(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return false;
    uint32_t ver = 0; i.read((char*)&ver, 4);
    if (ver != kVersion) return false;
    i.read((char*)&h.mtime, 8); i.read((char*)&h.size, 8);
    if (!getStr(i, h.sourcePath)) return false;
    uint32_t n = 0; i.read((char*)&n, 4);
    if (i.gcount() != 4) return false;
    h.layers.resize(n);
    for (uint32_t k = 0; k < n; k++) {
        MetaLayer& ml = h.layers[k];
        int32_t epsg = 0; i.read((char*)&epsg, 4);
        ml.epsg = epsg;
        i.read((char*)&ml.minx, 8); i.read((char*)&ml.miny, 8);
        i.read((char*)&ml.maxx, 8); i.read((char*)&ml.maxy, 8);
        int64_t fc = 0; i.read((char*)&fc, 8);
        ml.featureCount = fc;
        if (!getStr(i, ml.name)) return false;
        if (!getStr(i, ml.sourceCrs)) return false;
    }
    return true;
}

// ---- zstd 鎵撳寘/瑙ｅ寘: verts||pts||triangles 鍚堝苟鍘嬬缉 ----
static bool zstdPack(const std::vector<float>& verts, const std::vector<float>& pts,
                     const std::vector<float>& tris, std::vector<char>& blob,
                     size_t& uvert, size_t& upts, size_t& utri) {
    uvert = (size_t)verts.size() * sizeof(float);
    upts = (size_t)pts.size() * sizeof(float);
    utri = (size_t)tris.size() * sizeof(float);
    size_t total = uvert + upts + utri;
    if (total == 0) { blob.clear(); return true; }
    std::vector<char> raw(total);
    if (uvert) std::memcpy(raw.data(), verts.data(), uvert);
    if (upts) std::memcpy(raw.data() + uvert, pts.data(), upts);
    if (utri) std::memcpy(raw.data() + uvert + upts, tris.data(), utri);
    size_t bound = ZSTD_compressBound(total);
    blob.resize(bound);
    size_t cz = ZSTD_compress(blob.data(), bound, raw.data(), total, 3);
    if (ZSTD_isError(cz)) { blob.clear(); return false; }
    blob.resize(cz);
    return true;
}
static bool zstdUnpack(const void* blob, size_t cz, size_t uvert, size_t upts, size_t utri,
                       std::vector<float>& verts, std::vector<float>& pts,
                       std::vector<float>& tris) {
    verts.resize(uvert / sizeof(float));
    pts.resize(upts / sizeof(float));
    tris.resize(utri / sizeof(float));
    size_t total = uvert + upts + utri;
    if (total == 0) return true;
    std::vector<char> raw(total);
    size_t got = ZSTD_decompress(raw.data(), total, blob, cz);
    if (ZSTD_isError(got) || got != total) return false;
    if (uvert) std::memcpy(verts.data(), raw.data(), uvert);
    if (upts) std::memcpy(pts.data(), raw.data() + uvert, upts);
    if (utri) std::memcpy(tris.data(), raw.data() + uvert + upts, utri);
    return true;
}

// 鍥惧眰鏂囦欢: "PGCL" | ver(u32) | uvert | upts | utri | cz | zstd(verts||pts||tris)
// 鏃ф牸寮?鏃犻瓟鏁?鐗堟湰, uvert|upts|cz)浼氳鐗堝紡鏍￠獙鎷掓敹 -> MISS 鑷剤閲嶅啓, 涓嶄細鍐嶆妸閲庡昂瀵稿杺缁?vector
struct LayerHead {
    char magic[4];
    uint32_t ver;
    size_t uvert, upts, utri, cz;
};
static constexpr size_t kSanityCap = size_t(1) << 33;   // 鍗曟暟缁勫瓧鑺備笂闄?8GB, 鎷掔粷閲庡€?
static bool layerHeadRead(std::ifstream& i, LayerHead& h) {
    i.read(h.magic, 4); i.read((char*)&h.ver, 4);
    i.read((char*)&h.uvert, 8); i.read((char*)&h.upts, 8);
    i.read((char*)&h.utri, 8); i.read((char*)&h.cz, 8);
    if (i.gcount() != 8) return false;
    if (std::memcmp(h.magic, kLayerMagic, 4) != 0) return false;
    if (h.ver != kLayerVersion) return false;
    auto okU = [](size_t x) { return x % sizeof(float) == 0 && x <= kSanityCap; };
    return okU(h.uvert) && okU(h.upts) && okU(h.utri) && h.cz <= kSanityCap;
}
static bool writeLayerFile(const fs::path& layerPath, const VectorData& vd) {
    std::vector<char> blob;
    size_t uvert = 0, upts = 0, utri = 0;
    if (!zstdPack(vd.vertices, vd.points, vd.triangles, blob, uvert, upts, utri)) return false;
    std::ofstream o(layerPath, std::ios::binary);
    if (!o) return false;
    o.write(kLayerMagic, 4);
    uint32_t ver = kLayerVersion; o.write((const char*)&ver, 4);
    o.write((const char*)&uvert, 8);
    o.write((const char*)&upts, 8);
    o.write((const char*)&utri, 8);
    size_t cz = blob.size();
    o.write((const char*)&cz, 8);
    if (cz) o.write(blob.data(), (std::streamsize)cz);
    return (bool)o;
}
static bool readLayerFile(const fs::path& layerPath, const MetaLayer& ml, VectorData& out,
                          bool* repaired = nullptr) {
    std::ifstream i(layerPath, std::ios::binary);
    if (!i) return false;
    char magic[4];
    i.read(magic, 4);
    if (i.gcount() != 4 || std::memcmp(magic, kLayerMagic, 4) != 0) return false;
    uint32_t ver = 0; i.read((char*)&ver, 4);
    if (i.gcount() != 4) return false;
    if (ver != kLayerVersionChunked) {
        // v1 鏁村眰鍗曞潡: 鍥為€€鍒版棫璇诲彇閫昏緫
        std::error_code ec;
        auto fsz = fs::file_size(layerPath, ec);
        if (ec) return false;
        // 閲嶅紑鎸夋棫鏍煎紡瑙ｆ瀽
        i.close();
        std::ifstream i2(layerPath, std::ios::binary);
        if (!i2) return false;
        char mg2[4]; i2.read(mg2, 4);
        uint32_t v2 = 0; i2.read((char*)&v2, 4);
        size_t uvert = 0, upts = 0, utri = 0, cz = 0;
        i2.read((char*)&uvert, 8); i2.read((char*)&upts, 8);
        i2.read((char*)&utri, 8); i2.read((char*)&cz, 8);
        if (i2.gcount() != 8) return false;
        auto okU = [](size_t x) { return x % sizeof(float) == 0 && x <= kSanityCap; };
        if (!(okU(uvert) && okU(upts) && okU(utri) && cz <= kSanityCap)) return false;
        if (fsz != (std::streamoff)(40 + (long long)cz)) return false;
        std::vector<char> blob(cz);
        if (cz && (std::streamsize)cz != i2.read(blob.data(), (std::streamsize)cz).gcount()) return false;
        auto tZ0 = std::chrono::steady_clock::now();
        if (!zstdUnpack(blob.empty() ? nullptr : blob.data(), cz, uvert, upts, utri,
                        out.vertices, out.points, out.triangles))
            return false;
        auto tZ1 = std::chrono::steady_clock::now();
        if (getenv("PEEK_TIMING"))
            spdlog::info("[timing] decompress cz={}MB -> uv={}MB up={}MB ut={}MB in {:.0f}ms",
                         cz / (1024.0 * 1024.0), uvert / (1024.0 * 1024.0),
                         upts / (1024.0 * 1024.0), utri / (1024.0 * 1024.0),
                         std::chrono::duration<double, std::milli>(tZ1 - tZ0).count());
    } else {
        // v2 鍒嗗潡: 閫愬潡瑙ｅ帇杩藉姞
        uint64_t numChunks = 0; i.read((char*)&numChunks, 8);
        if (i.gcount() != 8) return false;
        for (uint64_t c = 0; c < numChunks; c++) {
            size_t uvert = 0, upts = 0, utri = 0, cz = 0;
            i.read((char*)&uvert, 8); i.read((char*)&upts, 8);
            i.read((char*)&utri, 8); i.read((char*)&cz, 8);
            if (i.gcount() != 8) return false;
            auto okU = [](size_t x) { return x % sizeof(float) == 0 && x <= kSanityCap; };
            if (!(okU(uvert) && okU(upts) && okU(utri) && cz <= kSanityCap)) return false;
            std::vector<char> blob(cz);
            if (cz && (std::streamsize)cz != i.read(blob.data(), (std::streamsize)cz).gcount()) return false;
            std::vector<float> v, p, t;
            if (!zstdUnpack(blob.empty() ? nullptr : blob.data(), cz, uvert, upts, utri, v, p, t))
                return false;
            out.vertices.insert(out.vertices.end(), v.begin(), v.end());
            out.points.insert(out.points.end(), p.begin(), p.end());
            out.triangles.insert(out.triangles.end(), t.begin(), t.end());
        }
    }
    out.name = ml.name;
    out.sourceCrs = ml.sourceCrs;
    out.srcEpsg = ml.epsg;
    out.featureCount = ml.featureCount;
    out.minx = ml.minx; out.miny = ml.miny; out.maxx = ml.maxx; out.maxy = ml.maxy;
    // 旧缓存可能只写了哨兵范围(1e300/-1e300)或 NaN, 会令上层"适配视图"缩到原点导致
    // 图层不可见。读回时若范围无效且确有几何, 现场从几何重算(一次性, 并在服务端重写 meta)。
    if (repaired) *repaired = repairExtentsIfInvalid(out);
    return true;
}


// ---- 绱㈠紩: sourceId -> {婧愯矾寰? 鍥惧眰璁板綍鍒楄〃}; 浠呯敤浜?LRU 棰勭畻涓?lastAccess ----
struct LayerRec { int layerIdx = 0; int64_t bytes = 0; int64_t lastAccess = 0; };
struct SourceRec { std::string sourcePath; std::vector<LayerRec> layers; };
static std::unordered_map<std::string, SourceRec> g_index;
static bool g_indexLoaded = false;
static std::mutex g_cacheMtx;

static std::string indexLineKey(const std::string& sourceId, int layerIdx) {
    return sourceId + "|" + std::to_string(layerIdx);
}

static void loadIndex(const std::string& dir) {
    g_index.clear();
    std::ifstream in(toFsPath(dir + "/.index"));
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // 璁板綍鍚?`|`, 浠庡彸渚у垏: 鏈鍥惧眰鍚? 涔嬪墠鍚勬浠?'|' 鍒嗛殧(璺緞/鍥惧眰鍚嶉€氬父涓嶅惈 '|')
        std::istringstream ss(line);
        std::string id, lstr, bstr, astr;
        std::getline(ss, id, '|');
        std::getline(ss, lstr, '|');
        std::getline(ss, bstr, '|');
        std::getline(ss, astr, '|');
        int layerIdx = atoi(lstr.c_str());
        int64_t bytes = atoll(bstr.c_str());
        int64_t la = atoll(astr.c_str());
        if (id.empty()) continue;
        // 鍓╀綑閮ㄥ垎鍗虫簮璺緞
        std::string rest;
        std::getline(ss, rest);
        SourceRec& rec = g_index[id];
        rec.sourcePath = rest;
        LayerRec lr; lr.layerIdx = layerIdx; lr.bytes = bytes; lr.lastAccess = la;
        for (auto& orr : rec.layers) if (orr.layerIdx == layerIdx) { orr = lr; lr.layerIdx = -1; break; }
        if (lr.layerIdx >= 0) rec.layers.push_back(lr);
    }
}

static std::string toStdString(const fs::path& p) {
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

// 浠ョ鐩樻壂鎻忎负鍑嗛噸寤虹储寮?鑷剤: 娈嬬暀/缂哄け璁板綍浼氳淇), 骞跺彔鍔?.index 涓緝鏂扮殑 lastAccess
static void scanDisk(const std::string& dir) {
    g_index.clear();
    std::error_code ec;
    if (!fs::exists(toFsPath(dir), ec)) return;
    for (auto& entry : fs::directory_iterator(toFsPath(dir), ec)) {
        if (!entry.is_directory()) continue;
        fs::path folder = entry.path();
        fs::path metaPath = folder / "meta.bin";
        if (!fs::exists(metaPath, ec)) continue;
        MetaHead h;
        if (!readMeta(toStdString(metaPath), h)) continue;
        std::string id = toStdString(folder.filename());
        SourceRec& rec = g_index[id];
        rec.sourcePath = h.sourcePath;
        for (size_t k = 0; k < h.layers.size(); k++) {
            fs::path lp = folder / ("l" + std::to_string(k) + ".bin");
            std::error_code e2;
            if (!fs::exists(lp, e2)) continue;
            int64_t sz = (int64_t)fs::file_size(lp, e2);
            std::error_code e3;
            auto ft = fs::last_write_time(lp, e3);
            int64_t fm = 0;
            if (!e3) fm = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count();
            LayerRec lr; lr.layerIdx = (int)k; lr.bytes = sz; lr.lastAccess = fm;
            rec.layers.push_back(lr);
        }
    }
}

static void saveIndex(const std::string& dir) {
    std::error_code e;
    fs::create_directories(toFsPath(dir), e);
    std::ofstream out(toFsPath(dir + "/.index"));
    if (!out) return;
    for (auto& kv : g_index)
        for (auto& lr : kv.second.layers)
            out << indexLineKey(kv.first, lr.layerIdx) << "|"
                << lr.bytes << "|" << lr.lastAccess << "|"
                << kv.second.sourcePath << "\n";
}

static void ensureIndex(const std::string& dir) {
    if (g_indexLoaded) return;
// load old index first (may have fresher lastAccess), then rebuild & merge from disk scan
loadIndex(dir);
    std::unordered_map<std::string, SourceRec> old = std::move(g_index);
    scanDisk(dir);
    for (auto& kv : old) {
        auto it = g_index.find(kv.first);
        if (it == g_index.end()) continue;
        for (auto& olr : kv.second.layers) {
            for (auto& nlr : it->second.layers) {
                if (nlr.layerIdx == olr.layerIdx) {
                    if (olr.lastAccess > nlr.lastAccess) nlr.lastAccess = olr.lastAccess;
                    break;
                }
            }
        }
    }
    g_indexLoaded = true;
}

// LRU 棰勭畻(鎸夊浘灞傚崟浣嶉┍閫?; skip 鎸囧悜姝ｅ湪鍐欏叆鐨勬簮, 涓嶅緱椹遍€愬叾浠讳綍鍥惧眰
static void enforceBudget(const std::string& dir, int maxMb, const std::string& skipSourceId) {
    if (maxMb <= 0) return;
    int64_t maxBytes = (int64_t)maxMb * 1024 * 1024;
    int64_t total = 0;
    for (auto& kv : g_index)
        for (auto& lr : kv.second.layers) total += lr.bytes;
    while (total > maxBytes) {
        std::string bestId; int bestIdx = -1; int64_t bestLa = INT64_MAX;
        bool any = false;
        for (auto& kv : g_index) {
            if (kv.first == skipSourceId) continue;
            for (auto& lr : kv.second.layers) {
                any = true;
                if (lr.lastAccess < bestLa) { bestLa = lr.lastAccess; bestId = kv.first; bestIdx = lr.layerIdx; }
            }
        }
        if (!any || bestIdx < 0) break;
        std::error_code ec;
        fs::remove(toFsPath(dir + "/" + bestId + "/l" + std::to_string(bestIdx) + ".bin"), ec);
        auto& rec = g_index[bestId];
        int64_t removed = 0;
        for (auto it = rec.layers.begin(); it != rec.layers.end(); ++it) {
            if (it->layerIdx == bestIdx) { removed = it->bytes; rec.layers.erase(it); break; }
        }
        if (rec.layers.empty()) {
            fs::remove_all(toFsPath(dir + "/" + bestId), ec);
            g_index.erase(bestId);
        }
        total -= removed;
    }
}

static void touchLayers(const std::string& id, const std::vector<int>& idxs, const std::string& dir) {
    auto it = g_index.find(id);
    if (it == g_index.end()) return;
    int64_t now = nowMs();
    for (auto& lr : it->second.layers)
        for (int li : idxs)
            if (lr.layerIdx == li) { lr.lastAccess = now; break; }
    saveIndex(dir);
}

// ---- v2 鍒嗗潡缂撳瓨: 娴佸紡鍐?姣忓潡鐙珛鍘嬬缉, 鏁村眰涓嶉┗鐣欏唴瀛? ----
constexpr size_t kChunkDword = 1 << 20;   // 鍗曞潡椤剁偣/鐐?涓夎鍚勬柟鍚?float 鏁颁笂闄?绾?4MB 姣忔暟缁?
struct CacheWriter::ChunkWriter {
    fs::path file;
    std::ofstream os;
    uint64_t chunks = 0;   // 已写块数, 收尾时回填到文件头
    bool ok = true;
};

CacheWriter::CacheWriter(const std::string& path, const std::vector<VectorData>& layerMeta,
                         const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (layerMeta.empty()) return;
    std::string dir = cacheDir(cfg);
    std::error_code ec;
    fs::create_directories(toFsPath(dir), ec);
    std::string id;
    if (!fileSig(path, mtime_, size_)) return;
    id = sourceIdOf(path, mtime_, size_);
    fs::path folder = toFsPath(dir + "/" + id);
    fs::create_directories(folder, ec);

    std::ofstream mo(folder / "meta.bin", std::ios::binary);
    if (!mo) { fs::remove_all(folder, ec); return; }
    writeMeta(mo, path, mtime_, size_, layerMeta);
    mo.close();

    dir_ = std::move(dir);
    id_ = std::move(id);
    sourcePath_ = path;
    maxMb_ = cfg.cache_max_mb <= 0 ? 0 : (int)cfg.cache_max_mb;
    layers_.resize(layerMeta.size());
    for (size_t k = 0; k < layerMeta.size(); k++) {
        ChunkWriter& cw = layers_[k];
        cw.file = folder / ("l" + std::to_string(k) + ".bin");
        cw.os.open(cw.file, std::ios::binary);
        if (!cw.os) { fs::remove_all(folder, ec); return; }
        // v2 分块头: magic(4) | ver(4) | numChunks(8), 块数收尾时回填
        cw.os.write(kLayerMagic, 4);
        uint32_t ver = kLayerVersionChunked; cw.os.write((const char*)&ver, 4);
        uint64_t nc = 0; cw.os.write((const char*)&nc, 8);
    }
    openOk_ = true;
}

void CacheWriter::append(int layerIdx, const std::vector<float>& verts,
                         const std::vector<float>& pts, const std::vector<float>& tris) {
    if (!openOk_ || !writeOk_) return;
    if (layerIdx < 0 || layerIdx >= (int)layers_.size()) return;
    ChunkWriter& cw = layers_[layerIdx];
    if (!cw.os) return;
    std::vector<char> blob;
    size_t uvert = 0, upts = 0, utri = 0;
    if (!zstdPack(verts, pts, tris, blob, uvert, upts, utri)) { writeOk_ = false; return; }
    cw.os.write((const char*)&uvert, 8);
    cw.os.write((const char*)&upts, 8);
    cw.os.write((const char*)&utri, 8);
    size_t cz = blob.size();
    cw.os.write((const char*)&cz, 8);
    if (cz) cw.os.write(blob.data(), (std::streamsize)cz);
    if (!cw.os) writeOk_ = false;
    cw.chunks++;
}

void CacheWriter::finalize() {
    if (!openOk_ || done_) return;
    done_ = true;
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (writeOk_) {
        for (size_t k = 0; k < layers_.size(); k++) {
            ChunkWriter& cw = layers_[k];
            if (!cw.os) continue;
            // 回填块数(v2 头 = magic 4 + ver 4 + numChunks 8)
            cw.os.seekp(4 + 4, std::ios::beg);
            cw.os.write((const char*)&cw.chunks, 8);
            cw.os.close();
            if (!cw.os) writeOk_ = false;
        }
    }
    if (writeOk_) {
        // 重建索引记录并执行 LRU 预算
        ensureIndex(dir_);
        SourceRec& rec = g_index[id_];
        rec.sourcePath = sourcePath_;
        rec.layers.clear();
        for (size_t k = 0; k < layers_.size(); k++) {
            std::error_code ec;
            LayerRec lr;
            lr.layerIdx = (int)k;
            lr.bytes = (int64_t)fs::file_size(layers_[k].file, ec);
            lr.lastAccess = nowMs();
            rec.layers.push_back(lr);
        }
        saveIndex(dir_);
        enforceBudget(dir_, maxMb_, id_);
        saveIndex(dir_);
    } else {
        // 失败: 丢弃半成品输出
        for (auto& cw : layers_) { if (cw.os) cw.os.close(); }
        std::error_code ec;
        fs::remove_all(toFsPath(dir_ + "/" + id_), ec);
    }
}

CacheWriter::~CacheWriter() { finalize(); }

void GeomCache::writeCacheAll(const std::string& path, const std::vector<VectorData>& vds, const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (vds.empty()) return;
    std::string dir = cacheDir(cfg);
    std::error_code ec;
    fs::create_directories(toFsPath(dir), ec);

    int64_t mt = 0, sz = 0;
    if (!fileSig(path, mt, sz)) return;
    std::string id = sourceIdOf(path, mt, sz);
    fs::path folder = toFsPath(dir + "/" + id);
    fs::create_directories(folder, ec);

    // meta 澶撮儴
    {
        std::ofstream o(folder / "meta.bin", std::ios::binary);
        if (!o) return;
        writeMeta(o, path, mt, sz, vds);
    }
// one cache file per layer
for (size_t k = 0; k < vds.size(); k++) {
        fs::path lp = folder / ("l" + std::to_string(k) + ".bin");
        if (!writeLayerFile(lp, vds[k])) return;
    }

    ensureIndex(dir);
    SourceRec& rec = g_index[id];
    rec.sourcePath = path;
    rec.layers.clear();
    for (size_t k = 0; k < vds.size(); k++) {
        LayerRec lr;
        lr.layerIdx = (int)k;
        lr.bytes = (int64_t)fs::file_size(folder / ("l" + std::to_string(k) + ".bin"), ec);
        lr.lastAccess = nowMs();
        rec.layers.push_back(lr);
    }
    saveIndex(dir);
    enforceBudget(dir, cfg.cache_max_mb, id);
    saveIndex(dir);
}

static bool readLayerFile(const fs::path& lp, const MetaLayer& ml, VectorData& vd);

// 旧缓存修复: 缓存命中后发现 LayerMeta.epsg==0(crs=unknown), 说明该缓存是较早版本写的
// (当时 gdalSrsEpsg 对 ArcGIS 私有 WKT 识别不全)。就地用 GDAL 重查该层 SRS 并重写
// meta.bin, 一次修复永久生效; 拉不到 SRS(源打不开)时保持 epsg=0, 与旧行为一致。
static void repairCachedEpsgOnce(const std::string& path, int li, MetaLayer& ml,
                                 const std::string& dir, const std::string& id, MetaHead& h) {
    if (ml.epsg != 0 || ml.sourceCrs != "unknown") return;   // 已有值, 无需查
    // 已在本会话判定过(打标记), 避免同一进程反复打开同一文件
    static std::mutex pmu;
    static std::unordered_set<std::string> fixed;
    std::string key = id + "|" + std::to_string(li);
    {
        std::lock_guard<std::mutex> g(pmu);
        if (fixed.count(key)) return;
    }

    OGRSpatialReferenceH srs = nullptr;
    {   // 只取该层 SRS, 不迭代要素; 静音打开, 失败保持 epsg=0 交给上层
        CPLPushErrorHandler(CPLQuietErrorHandler);
        GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                     nullptr, nullptr, nullptr);
        CPLPopErrorHandler();
        if (ds) {
            int nl = GDALDatasetGetLayerCount(ds);
            if (li >= 0 && li < nl) {
                OGRSpatialReferenceH s = OGR_L_GetSpatialRef(GDALDatasetGetLayer(ds, li));
                if (s) srs = OSRClone(s);
            } else if (nl >= 1) {
                OGRSpatialReferenceH s = OGR_L_GetSpatialRef(GDALDatasetGetLayer(ds, 0));
                if (s) srs = OSRClone(s);
            }
            GDALClose(ds);
        }
    }

    if (srs) {
        int epsg = gdalSrsEpsg(srs);
        OSRDestroySpatialReference(srs);
        if (epsg != 0) {
            ml.epsg = epsg;
            ml.sourceCrs = "EPSG:" + std::to_string(epsg);
            writeMetaHead(dir + "/" + id + "/meta.bin", h);
            spdlog::info("[cache] repair layer[{}] srcEpsg=0 -> {} ({})", li, epsg, path);
        }
    } else {
        // 源文件打不开(占位/暂缺): 无法判定, 保持 epsg=0 交由上层按未知处理
        spdlog::debug("[cache] repair layer[{}]: cannot open source, keep epsg=0 ({})", li, path);
    }
    std::lock_guard<std::mutex> g(pmu);
    fixed.insert(key);
}

static bool readCacheLayersLocked(const std::string& path, const std::vector<int>& indices,
                                  std::vector<VectorData>& out, const AppConfig& cfg) {
    int64_t mt = 0, sz = 0;
    if (!fileSig(path, mt, sz)) return false;
    std::string dir = cacheDir(cfg);
    ensureIndex(dir);
    std::string id = sourceIdOf(path, mt, sz);
    MetaHead h;
    if (!readMeta(dir + "/" + id + "/meta.bin", h)) return false;
    if (h.mtime != mt || h.size != sz) return false;

    // 缓存命中但个别层 epsg=0 = 旧缓存, 现场查 SRS 修复(9/4 前缓存常见)
    bool needRepair = false;
    for (int li : indices)
        if (li >= 0 && li < (int)h.layers.size() && h.layers[li].epsg == 0) { needRepair = true; break; }
    if (needRepair) {
        for (int li : indices) {
            if (li < 0 || li >= (int)h.layers.size()) return false;
            repairCachedEpsgOnce(path, li, h.layers[li], dir, id, h);
        }
    }

    out.clear();
    out.reserve(indices.size());
    fs::path folder = toFsPath(dir + "/" + id);
    bool metaRepair = false;
    for (int li : indices) {
        if (li < 0 || li >= (int)h.layers.size()) return false;
        VectorData vd;
        bool repaired = false;
        if (!readLayerFile(folder / ("l" + std::to_string(li) + ".bin"), h.layers[li], vd, &repaired))
            return false;
        if (repaired) {
            // 范围从几何重算成功: 把修复值写回 meta 头, 下面落盘, 以后打开不再扫几何
            h.layers[li].minx = vd.minx; h.layers[li].miny = vd.miny;
            h.layers[li].maxx = vd.maxx; h.layers[li].maxy = vd.maxy;
            metaRepair = true;
        }
        out.push_back(std::move(vd));
    }
    if (metaRepair && !writeMetaHead(dir + "/" + id + "/meta.bin", h)) {
        // 重写失败不致命: 本次读取已带正确范围, 仅下次继续重算一次
    }
    touchLayers(id, indices, dir);
    return true;
}

bool GeomCache::readCacheLayers(const std::string& path, const std::vector<int>& indices,
                     std::vector<VectorData>& out, const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    return readCacheLayersLocked(path, indices, out, cfg);
}

bool GeomCache::readCacheAll(const std::string& path, std::vector<VectorData>& out, const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    int64_t mt = 0, sz = 0;
    if (!fileSig(path, mt, sz)) return false;
    std::string dir = cacheDir(cfg);
    ensureIndex(dir);
    std::string id = sourceIdOf(path, mt, sz);
    MetaHead h;
    if (!readMeta(dir + "/" + id + "/meta.bin", h)) return false;
    if (h.mtime != mt || h.size != sz) return false;

    std::vector<int> all;
    all.resize(h.layers.size());
    for (size_t k = 0; k < h.layers.size(); k++) all[k] = (int)k;
    return readCacheLayersLocked(path, all, out, cfg);
}

// 逐块读取单个图层: meta 校验与 readCacheLayersLocked 一致, 但几何不累积整层,
// 每解出一块立即回调 onChunk(移动语义)。块=写缓存时的块(读库顺序), 不做空间分区。
bool GeomCache::readCacheLayerChunks(
    const std::string& path, int layerIdx, const AppConfig& cfg, VectorData& outMeta,
    const std::function<void(std::vector<float>&, std::vector<float>&, std::vector<float>&)>& onChunk) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    int64_t mt = 0, sz = 0;
    if (!fileSig(path, mt, sz)) return false;
    std::string dir = cacheDir(cfg);
    ensureIndex(dir);
    std::string id = sourceIdOf(path, mt, sz);
    MetaHead h;
    if (!readMeta(dir + "/" + id + "/meta.bin", h)) return false;
    if (h.mtime != mt || h.size != sz) return false;
    if (layerIdx < 0 || layerIdx >= (int)h.layers.size()) return false;

    MetaLayer& ml = h.layers[layerIdx];
    if (ml.epsg == 0)
        repairCachedEpsgOnce(path, layerIdx, ml, dir, id, h);   // 旧缓存 epsg 修复(同全量路径)

    outMeta = VectorData{};
    outMeta.name = ml.name;
    outMeta.sourceCrs = ml.sourceCrs;
    outMeta.srcEpsg = ml.epsg;
    outMeta.featureCount = ml.featureCount;
    outMeta.minx = ml.minx; outMeta.miny = ml.miny;
    outMeta.maxx = ml.maxx; outMeta.maxy = ml.maxy;
    const bool metaOk = !(ml.minx > ml.maxx || !std::isfinite(ml.minx) || !std::isfinite(ml.maxx));

    const fs::path lp = toFsPath(dir + "/" + id + "/l" + std::to_string(layerIdx) + ".bin");
    std::ifstream i(lp, std::ios::binary);
    if (!i) return false;
    char magic[4];
    i.read(magic, 4);
    if (i.gcount() != 4 || std::memcmp(magic, kLayerMagic, 4) != 0) return false;
    uint32_t ver = 0; i.read((char*)&ver, 4);
    if (i.gcount() != 4) return false;
    auto okU = [](size_t x) { return x % sizeof(float) == 0 && x <= kSanityCap; };

    // 旧缓存 meta 范围可能是哨兵/NaN: 逐块累计几何范围, 收尾修复并重写 meta(后续免扫几何)
    double accMinx = 1e300, accMiny = 1e300, accMaxx = -1e300, accMaxy = -1e300;
    auto accumulate = [&](const std::vector<float>& arr) {
        if (arr.empty()) return;
        double a, b, c, d;
        computeExtent(arr, a, b, c, d);
        accMinx = std::min(accMinx, a); accMiny = std::min(accMiny, b);
        accMaxx = std::max(accMaxx, c); accMaxy = std::max(accMaxy, d);
    };

    if (ver == kLayerVersionChunked) {
        // v2 分块: 逐块解压 -> 立即回调(不整层累积)
        uint64_t numChunks = 0; i.read((char*)&numChunks, 8);
        if (i.gcount() != 8) return false;
        for (uint64_t c = 0; c < numChunks; c++) {
            size_t uvert = 0, upts = 0, utri = 0, cz = 0;
            i.read((char*)&uvert, 8); i.read((char*)&upts, 8);
            i.read((char*)&utri, 8); i.read((char*)&cz, 8);
            if (i.gcount() != 8) return false;
            if (!(okU(uvert) && okU(upts) && okU(utri) && cz <= kSanityCap)) return false;
            std::vector<char> blob(cz);
            if (cz && (std::streamsize)cz != i.read(blob.data(), (std::streamsize)cz).gcount()) return false;
            std::vector<float> v, p, t;
            if (!zstdUnpack(blob.empty() ? nullptr : blob.data(), cz, uvert, upts, utri, v, p, t))
                return false;
            if (!metaOk) { accumulate(v); accumulate(p); accumulate(t); }
            if (!v.empty() || !p.empty() || !t.empty())
                onChunk(v, p, t);
        }
    } else {
        // v1 整层单块: 与 v2 相同的单次回调(块=整层)
        std::error_code ec;
        auto fsz = fs::file_size(lp, ec);
        if (ec) return false;
        i.close();
        std::ifstream i2(lp, std::ios::binary);
        if (!i2) return false;
        char mg2[4]; i2.read(mg2, 4);
        uint32_t vty = 0; i2.read((char*)&vty, 4);
        size_t uvert = 0, upts = 0, utri = 0, cz = 0;
        i2.read((char*)&uvert, 8); i2.read((char*)&upts, 8);
        i2.read((char*)&utri, 8); i2.read((char*)&cz, 8);
        if (i2.gcount() != 8) return false;
        if (!(okU(uvert) && okU(upts) && okU(utri) && cz <= kSanityCap)) return false;
        if (fsz != (std::streamoff)(40 + (long long)cz)) return false;
        std::vector<char> blob(cz);
        if (cz && (std::streamsize)cz != i2.read(blob.data(), (std::streamsize)cz).gcount()) return false;
        std::vector<float> v, p, t;
        if (!zstdUnpack(blob.empty() ? nullptr : blob.data(), cz, uvert, upts, utri, v, p, t))
            return false;
        if (!metaOk) { accumulate(v); accumulate(p); accumulate(t); }
        if (!v.empty() || !p.empty() || !t.empty())
            onChunk(v, p, t);
    }

    if (!metaOk) {
        if (accMinx <= accMaxx) {
            outMeta.minx = accMinx; outMeta.miny = accMiny;
            outMeta.maxx = accMaxx; outMeta.maxy = accMaxy;
            ml.minx = accMinx; ml.miny = accMiny; ml.maxx = accMaxx; ml.maxy = accMaxy;
            if (!writeMetaHead(dir + "/" + id + "/meta.bin", h)) {
                // 重写失败不致命: 本次读取已带正确范围, 仅下次继续重算一次
            }
        } else {
            outMeta.minx = outMeta.miny = outMeta.maxx = outMeta.maxy = 0;
        }
    }
    touchLayers(id, std::vector<int>{layerIdx}, dir);
    return true;
}

bool GeomCache::loadVectorCachedAll(const std::string& path, std::vector<VectorData>& out, const AppConfig& cfg) {
    ensureGdal();
    if (readCacheAll(path, out, cfg)) {
        spdlog::info("[cache] HIT  {}  ({} layers)", path, (int)out.size());
        return true;
    }
    if (loadVectorFileAll(path, out)) {
        writeCacheAll(path, out, cfg);
        spdlog::info("[cache] MISS -> written  {}", path);
        return true;
    }
    return false;
}

// ---- 缂撳瓨绠＄悊(浠ョ鐩樻壂鎻忎负鍑? ----

std::vector<CacheEntry> GeomCache::listCacheEntries(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    std::string dir = cacheDir(cfg);
    ensureIndex(dir);
    scanDisk(dir);
    saveIndex(dir);
    std::vector<CacheEntry> out;
    std::error_code ec;
    if (!fs::exists(toFsPath(dir), ec)) return out;
    for (auto& entry : fs::directory_iterator(toFsPath(dir), ec)) {
        if (!entry.is_directory()) continue;
        fs::path folder = entry.path();
        fs::path metaPath = folder / "meta.bin";
        if (!fs::exists(metaPath, ec)) continue;
        MetaHead h;
        if (!readMeta(toStdString(metaPath), h)) continue;
        std::string id = toStdString(folder.filename());
        auto it = g_index.find(id);
        for (size_t k = 0; k < h.layers.size(); k++) {
            fs::path lp = folder / ("l" + std::to_string(k) + ".bin");
            std::error_code e2;
            if (!fs::exists(lp, e2)) continue;
            CacheEntry e;
            e.sourceId = id;
            e.layerIdx = (int)k;
            e.layerName = k < h.layers.size() ? h.layers[k].name : ("layers " + std::to_string(k));
            e.sourcePath = h.sourcePath;
            e.bytes = (int64_t)fs::file_size(lp, e2);
            e.lastAccess = 0;
            if (it != g_index.end()) {
                for (auto& lr : it->second.layers)
                    if (lr.layerIdx == (int)k) { e.lastAccess = lr.lastAccess; break; }
            }
            if (!e.lastAccess) {
                std::error_code e3;
                auto ft = fs::last_write_time(lp, e3);
                if (!e3) e.lastAccess = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count();
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool GeomCache::deleteCacheEntry(const std::string& sourceId, int layerIdx, const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    std::string dir = cacheDir(cfg);
    ensureIndex(dir);
    std::error_code ec;
    fs::path folder = toFsPath(dir + "/" + sourceId);
    bool removed = fs::remove(folder / ("l" + std::to_string(layerIdx) + ".bin"), ec);
    // 鍚屾鍐呭瓨绱㈠紩
    auto it = g_index.find(sourceId);
    if (it != g_index.end()) {
        for (auto lr = it->second.layers.begin(); lr != it->second.layers.end(); ++lr)
            if (lr->layerIdx == layerIdx) { it->second.layers.erase(lr); break; }
        if (it->second.layers.empty()) g_index.erase(it);
    }
    // 鍓╀綑鍥惧眰鏁颁负 0 鏃跺垹鎺夌洰褰曚笌 meta
    bool hasAny = false;
    if (fs::exists(folder, ec)) {
        for (auto& ent : fs::directory_iterator(folder, ec))
            if (ent.path().filename() != "meta.bin") { hasAny = true; break; }
        if (!hasAny) fs::remove_all(folder, ec);
    }
    saveIndex(dir);
    return removed;
}

void GeomCache::clearAllCache(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    std::string dir = cacheDir(cfg);
    std::error_code ec;
    if (!fs::exists(toFsPath(dir), ec)) return;
    for (auto& entry : fs::directory_iterator(toFsPath(dir), ec)) {
        std::error_code e2;
        if (entry.is_directory()) {
            // v3 婧愮洰褰? 鍚?meta.bin
            if (fs::exists(entry.path() / "meta.bin", e2)) fs::remove_all(entry.path(), e2);
        } else if (entry.path().extension() == ".geomcache") {
            // 鍏煎 v2 閬楃暀
            fs::remove(entry.path(), e2);
        } else if (entry.path().filename() == ".index") {
            fs::remove(entry.path(), e2);
        }
    }
    g_index.clear();
    g_indexLoaded = true;
    saveIndex(dir);
}

}  // namespace peekg::data


