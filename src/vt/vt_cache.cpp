#include "vt/vt_cache.h"

#include <zstd.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

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
    f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_.is_open()) return false;
    f_.seekg(0);
    f_.read((char*)&h_, sizeof(VtFileHeader));
    if (!f_.good() && !f_.eof()) { close(); return false; }
    if (std::memcmp(h_.magic, VT_MAGIC, 8) != 0 || h_.version != VT_VERSION) { close(); return false; }
    if (h_.headerSize != sizeof(VtFileHeader)) { close(); return false; }
    path_ = path;
    dirtyHeader_ = false;
    tilesWritten_ = 0;
    return true;
}

bool VtCache::reloadHeader() {
    if (!f_.is_open()) return false;
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
    if (!f_.is_open() || !levelValid(level, tx, ty)) return false;
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.seekg((std::streamoff)slotPos(level, tx, ty));
    f_.read((char*)&s, sizeof(s));
    return s.valid != 0 && s.size > 0;
}

bool VtCache::writeTile(int level, int tx, int ty, const VtTile& t) {
    if (!f_.is_open() || !levelValid(level, tx, ty)) return false;
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

    if (getenv("PEEK_VT_TRACE"))
        fprintf(stderr, "[vt-trace] write L%d (%d,%d) bytes=%zu %s dataBytes=%llu\n",
                level, tx, ty, cs, re ? "覆盖" : "追加",
                (unsigned long long)(h_.dataEnd - h_.dataStart));
    {
        const char* mx = getenv("PEEK_VT_MAXBYTES");
        long long lim = mx ? atoll(mx) : 0;
        if (lim > 0 && (long long)(h_.dataEnd - h_.dataStart) > lim) {
            spdlog::error("[vt] 数据段 {} 字节 > 上限 {} -> 立即退出(保留文件供分析)",
                          h_.dataEnd - h_.dataStart, lim);
            f_.flush();
            std::exit(3);
        }
    }
    ++tilesWritten_;
    dirtyHeader_ = true;
    f_.flush();   // 立即落盘: 构建中读端(另一文件句柄)才能看到该片
    return true;
}

bool VtCache::readTile(int level, int tx, int ty, VtTile& t) const {
    if (!f_.is_open() || !levelValid(level, tx, ty)) return false;
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.seekg((std::streamoff)slotPos(level, tx, ty));
    f_.read((char*)&s, sizeof(s));
    if (s.valid == 0 || s.size == 0) return false;

    std::vector<char> comp(s.size);
    f_.seekg((std::streamoff)s.offset);
    f_.read(comp.data(), (std::streamsize)s.size);
    if (!f_.good() && (size_t)f_.gcount() != s.size) return false;

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

void VtCache::finalize() {
    if (!f_.is_open()) return;
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
                f_.seekp((std::streamoff)dst);
                f_.write(buf.data(), (std::streamsize)e.size);
                VtSlot s{};
                s.offset = dst; s.size = e.size; s.valid = 1;
                f_.seekp((std::streamoff)e.slotOff);
                f_.write((const char*)&s, sizeof(s));
            }
            dst += e.size;
        }
        h_.dataEnd = dst;
        f_.seekp(0);
        f_.write((const char*)&h_, sizeof(VtFileHeader));
        f_.flush();
    }
    // 截断到压实后大小(去掉垃圾尾巴)。fstream 不能截断 -> 关掉重开再 resize。
    std::string p = path_;
    f_.close();
    std::error_code ec;
    std::filesystem::resize_file(u8ToPath(p), h_.dataEnd, ec);
    f_.open(p, std::ios::in | std::ios::out | std::ios::binary);
    path_ = p;
    dirtyHeader_ = false;
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
        e.bytes = (uint64_t)it->file_size(e2);
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
