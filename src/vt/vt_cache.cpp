#include "vt/vt_cache.h"

#include <zstd.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace peekg::vt {

namespace {

#ifdef _WIN32
std::filesystem::path u8ToPath(const std::string& u8) {
    if (u8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return std::filesystem::path(w);
}
#else
std::filesystem::path u8ToPath(const std::string& u8) { return std::filesystem::path(u8); }
#endif

std::string pathToU8(const std::filesystem::path& p) {
#ifdef _WIN32
    std::wstring w = p.wstring();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

// 进程级按路径共享的读写锁: 同一 .vtk 的构建写端与渲染读端是不同 VtCache 实例,
// 需要跨实例的"读共享/写独占"来避免并发写/压实/截断与读撕裂。
std::shared_ptr<std::shared_mutex> fileMutexFor(const std::string& path) {
    static std::mutex regMtx;
    static std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> reg;
    std::lock_guard<std::mutex> lk(regMtx);
    auto& p = reg[path];
    if (!p) p = std::make_shared<std::shared_mutex>();
    return p;
}

}  // namespace

uint64_t sourceHash(const std::string& path) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    };
    mix(path.data(), path.size());
    std::error_code ec;
    auto p = u8ToPath(path);
    if (std::filesystem::exists(p, ec)) {
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) { uint64_t s = (uint64_t)sz; mix(&s, 8); }
        auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) { int64_t tt = (int64_t)t.time_since_epoch().count(); mix(&tt, 8); }
    }
    return h;
}

std::string vtCachePath(const std::string& cacheDir, const std::string& srcPath,
                        int srcEpsg, int dstEpsg) {
    uint64_t hash = sourceHash(srcPath);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08llx_e%d_d%d.vtk",
                  (unsigned long long)(hash & 0xffffffffULL), srcEpsg, dstEpsg);
    std::string dir = cacheDir.empty() ? "cache" : cacheDir;
    return dir + "/vtk/" + buf;
}

VtCache::~VtCache() { close(); }

uint64_t VtCache::slotIndex(int level, int tx, int ty) const {
    return ((uint64_t)ty << level) | (uint64_t)tx;
}

uint64_t VtCache::slotPos(int level, int tx, int ty) const {
    uint64_t base = h_.slotTableOffset;
    for (int k = 0; k < level; ++k) base += slotCount(k) * sizeof(VtSlot);
    return base + slotIndex(level, tx, ty) * sizeof(VtSlot);
}

bool VtCache::levelValid(int level, int tx, int ty) const {
    if (level < 0 || level > (int)h_.maxLevel) return false;
    int n = 1 << level;
    return tx >= 0 && tx < n && ty >= 0 && ty < n;
}

bool VtCache::create(const std::string& path, const VtFileHeader& h) {
    close();
    auto fm = fileMutexFor(path);
    std::unique_lock<std::shared_mutex> flk(*fm);
    f_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f_.is_open()) return false;
    h_ = h;
    std::memcpy(h_.magic, VT_MAGIC, 8);
    h_.version = VT_VERSION;
    h_.headerSize = sizeof(VtFileHeader);
    h_.slotTableOffset = sizeof(VtFileHeader);
    h_.dataStart = h_.slotTableOffset + slotTableBytes((int)h_.maxLevel);
    h_.dataEnd = h_.dataStart;
    h_.fullyBuiltLevels = 0;
    path_ = path;
    fileMtx_ = fm;
    dirtyHeader_ = true;
    tilesWritten_ = 0;

    f_.seekp(0);
    f_.write((const char*)&h_, sizeof(VtFileHeader));
    // 预置槽表区(清零): 用大块分次写, 避免一次性分配过大
    uint64_t total = slotTableBytes((int)h_.maxLevel);
    static const char zeros[65536] = {0};
    uint64_t left = total;
    while (left > 0) {
        size_t chunk = (size_t)std::min<uint64_t>(left, sizeof(zeros));
        f_.write(zeros, (std::streamsize)chunk);
        left -= chunk;
    }
    f_.flush();
    return f_.good();
}

bool VtCache::open(const std::string& path) {
    close();
    auto fm = fileMutexFor(path);
    std::unique_lock<std::shared_mutex> flk(*fm);
    f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_.is_open()) return false;
    f_.seekg(0);
    f_.read((char*)&h_, sizeof(VtFileHeader));
    // 注意: 此处已持有 *fm 独占锁, 失败时只能直接关 fstream, 不能再调 close()(会对同一锁二次加锁)
    if ((!f_.good() && !f_.eof()) || std::memcmp(h_.magic, VT_MAGIC, 8) != 0 ||
        h_.version != VT_VERSION || h_.headerSize != sizeof(VtFileHeader)) {
        f_.close();
        return false;
    }
    path_ = path;
    fileMtx_ = fm;
    dirtyHeader_ = false;
    tilesWritten_ = 0;
    return true;
}

bool VtCache::reloadHeader() {
    if (!f_.is_open() || !fileMtx_) return false;
    std::shared_lock<std::shared_mutex> flk(*fileMtx_);
    std::lock_guard<std::mutex> lk(ioMtx_);
    f_.clear();
    f_.seekg(0);
    VtFileHeader h{};
    f_.read((char*)&h, sizeof(h));
    if (!f_.good() && !f_.eof()) return false;
    if (std::memcmp(h.magic, VT_MAGIC, 8) != 0) return false;
    h_.fullyBuiltLevels = h.fullyBuiltLevels;
    h_.dataEnd = h.dataEnd;
    return true;
}

void VtCache::close() {
    if (f_.is_open()) {
        std::unique_lock<std::shared_mutex> flk;
        if (fileMtx_) flk = std::unique_lock<std::shared_mutex>(*fileMtx_);
        if (dirtyHeader_) {
            f_.seekp(0);
            f_.write((const char*)&h_, sizeof(VtFileHeader));
            f_.flush();
        }
        f_.close();
    }
    path_.clear();
}

bool VtCache::hasTile(int level, int tx, int ty) const {
    if (!f_.is_open() || !fileMtx_ || !levelValid(level, tx, ty)) return false;
    std::shared_lock<std::shared_mutex> flk(*fileMtx_);
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.clear();
    f_.seekg((std::streamoff)slotPos(level, tx, ty));
    f_.read((char*)&s, sizeof(s));
    return s.valid != 0 && s.size > 0;
}

bool VtCache::writeTile(int level, int tx, int ty, const VtTile& t) {
    if (!f_.is_open() || !fileMtx_ || !levelValid(level, tx, ty)) return false;
    std::unique_lock<std::shared_mutex> flk(*fileMtx_);
    std::lock_guard<std::mutex> lk(ioMtx_);
    // 读旧槽: 决定"原地覆盖"还是"追加"。
    // 同一片被反复 flush 时, 若新块 <= 旧块就原地覆盖 -> 不追加、不产生垃圾、文件不涨。
    VtSlot prev{};
    f_.clear();
    f_.seekg((std::streamoff)slotPos(level, tx, ty));
    f_.read((char*)&prev, sizeof(prev));
    bool re = prev.valid != 0 && prev.size > 0;

    std::vector<uint8_t> raw;
    serializeTile(t, raw);
    std::vector<char> comp(ZSTD_compressBound(raw.size()));
    size_t cs = ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(cs)) return false;

    uint64_t off;
    if (re && cs <= prev.size) {
        off = prev.offset;                 // 原地覆盖: dataEnd 不动
    } else {
        if (h_.dataEnd < h_.dataStart) h_.dataEnd = h_.dataStart;
        off = h_.dataEnd;                  // 新片, 或变大(旧份留垃圾, 由 finalize 压实)
        h_.dataEnd = off + cs;
    }
    f_.seekp((std::streamoff)off);
    f_.write(comp.data(), (std::streamsize)cs);
    if (!f_.good()) return false;

    VtSlot s{};
    s.offset = off;
    s.size = (uint32_t)cs;
    s.valid = 1;
    f_.seekp((std::streamoff)slotPos(level, tx, ty));
    f_.write((const char*)&s, sizeof(s));
    if (!f_.good()) return false;

    static const bool trace = std::getenv("PEEK_VT_TRACE") != nullptr;
    static const long long maxBytes = [] {
        const char* mx = std::getenv("PEEK_VT_MAXBYTES");
        return mx ? std::atoll(mx) : 0LL;
    }();
    if (trace)
        fprintf(stderr, "[vt-trace] write L%d (%d,%d) bytes=%zu %s dataBytes=%llu\n",
                level, tx, ty, cs, re ? "覆盖" : "追加",
                (unsigned long long)(h_.dataEnd - h_.dataStart));
    if (maxBytes > 0 && (long long)(h_.dataEnd - h_.dataStart) > maxBytes) {
        spdlog::error("[vt] 数据段 {} 字节 > 上限 {} -> 立即退出(保留文件供分析)",
                      h_.dataEnd - h_.dataStart, maxBytes);
        f_.flush();
        std::exit(3);
    }
    ++tilesWritten_;
    dirtyHeader_ = true;
    f_.flush();   // 立即落盘: 构建中读端(另一文件句柄)才能看到该片
    return true;
}

bool VtCache::readTile(int level, int tx, int ty, VtTile& t) const {
    if (!f_.is_open() || !fileMtx_ || !levelValid(level, tx, ty)) return false;
    std::shared_lock<std::shared_mutex> flk(*fileMtx_);
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.clear();
    f_.seekg((std::streamoff)slotPos(level, tx, ty));
    f_.read((char*)&s, sizeof(s));
    if (s.valid == 0 || s.size == 0) return false;

    std::vector<char> comp(s.size);
    f_.clear();
    f_.seekg((std::streamoff)s.offset);
    f_.read(comp.data(), (std::streamsize)s.size);
    if ((size_t)f_.gcount() != s.size) return false;

    unsigned long long rawSize = ZSTD_getFrameContentSize(comp.data(), comp.size());
    if (rawSize == ZSTD_CONTENTSIZE_ERROR || rawSize == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    std::vector<uint8_t> raw((size_t)rawSize);
    size_t ds = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    if (ZSTD_isError(ds)) return false;
    return deserializeTile(raw.data(), ds, t);
}

void VtCache::setFullyBuilt(int level) {
    if (level < 0 || level > (int)h_.maxLevel) return;
    h_.fullyBuiltLevels |= (1u << level);
    dirtyHeader_ = true;
}

bool VtCache::finalize() {
    if (!f_.is_open() || !fileMtx_) return false;
    std::unique_lock<std::shared_mutex> flk(*fileMtx_);
    {
        std::lock_guard<std::mutex> lk(ioMtx_);
        // 压实: 按数据段偏移顺序把有效块向前滑动, 消除反复 flush 留下的垃圾。
        // 目的位置 <= 源位置, 顺序向前拷贝在同一文件内是安全的。
        struct Ent { uint64_t slotOff; uint64_t off; uint32_t size; };
        std::vector<Ent> ents;
        for (int L = 0; L <= (int)h_.maxLevel; ++L) {
            uint64_t sc = slotCount(L);
            uint64_t base = h_.slotTableOffset;
            for (int k = 0; k < L; ++k) base += slotCount(k) * sizeof(VtSlot);
            for (uint64_t i = 0; i < sc; ++i) {
                VtSlot s{};
                f_.clear();
                f_.seekg((std::streamoff)(base + i * sizeof(VtSlot)));
                f_.read((char*)&s, sizeof(s));
                if ((size_t)f_.gcount() != sizeof(s)) return false;   // 槽表读失败: 中止, 不写脏数据
                if (s.valid && s.size) ents.push_back({base + i * sizeof(VtSlot), s.offset, s.size});
            }
        }
        std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.off < b.off; });
        uint64_t dst = h_.dataStart;
        std::vector<char> buf;
        for (auto& e : ents) {
            if (e.off != dst) {
                buf.resize(e.size);
                f_.clear();
                f_.seekg((std::streamoff)e.off);
                f_.read(buf.data(), (std::streamsize)e.size);
                if ((size_t)f_.gcount() != e.size) return false;   // 块读失败: 中止
                f_.clear();
                f_.seekp((std::streamoff)dst);
                f_.write(buf.data(), (std::streamsize)e.size);
                if (!f_.good()) return false;
                VtSlot s{};
                s.offset = dst; s.size = e.size; s.valid = 1;
                f_.seekp((std::streamoff)e.slotOff);
                f_.write((const char*)&s, sizeof(s));
                if (!f_.good()) return false;
            }
            dst += e.size;
        }
        h_.dataEnd = dst;
        f_.seekp(0);
        f_.write((const char*)&h_, sizeof(VtFileHeader));
        f_.flush();
        if (!f_.good()) return false;
    }
    // 截断到压实后大小(去掉垃圾尾巴)。fstream 不能截断 -> 关掉重开再 resize。
    std::string p = path_;
    f_.close();
    std::error_code ec;
    std::filesystem::resize_file(u8ToPath(p), h_.dataEnd, ec);
    if (ec) spdlog::warn("[vt] resize_file 失败(保留未截断文件): {} ({})", p, ec.message());
    f_.open(p, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_.is_open()) {
        spdlog::error("[vt] finalize 后重开失败: {}", p);
        dirtyHeader_ = true;   // 头部已写盘, 但对象不可用; 交由上层处理
        return false;
    }
    path_ = p;
    dirtyHeader_ = false;
    return !ec;
}

std::vector<VtCacheEntry> listVtCaches(const std::string& cacheDir) {
    std::vector<VtCacheEntry> out;
    std::string dir = (cacheDir.empty() ? std::string("cache") : cacheDir) + "/vtk";
    std::error_code ec;
    std::filesystem::path dp = u8ToPath(dir);
    if (!std::filesystem::is_directory(dp, ec)) return out;
    for (std::filesystem::directory_iterator it(dp, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (ext != ".vtk") continue;
        VtCacheEntry e;
        e.path = pathToU8(it->path());
        std::error_code e3;
        auto sz = it->file_size(e3);
        if (!e3) e.bytes = (uint64_t)sz;
        std::ifstream f(u8ToPath(e.path), std::ios::binary);
        VtFileHeader h{};
        if (f) {
            f.read((char*)&h, sizeof(h));
            if (std::memcmp(h.magic, VT_MAGIC, 8) == 0) {
                e.srcName = std::string(h.srcName, strnlen(h.srcName, sizeof(h.srcName)));
                e.srcHash = h.srcHash;
                e.srcEpsg = h.srcEpsg;
                e.dstEpsg = h.dstEpsg;
                e.maxLevel = (int)h.maxLevel;
                e.buildTime = h.buildTime;
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool deleteVtCache(const std::string& path) {
    std::error_code ec;
    return std::filesystem::remove(u8ToPath(path), ec);
}

void clearVtCaches(const std::string& cacheDir) {
    for (auto& e : listVtCaches(cacheDir)) deleteVtCache(e.path);
}

}  // namespace peekg::vt
