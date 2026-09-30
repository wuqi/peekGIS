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

// 缓存键归一化: 同一文件无论以哪种拼写打开(G:\ vs g:\, / vs \), 都得命中同一份缓存。
// 否则 sourceHash(直接哈希路径字节)会因盘符大小写不同而生成两份缓存。Windows 路径不区分大小写与分隔符。
std::string canonicalKey(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    std::filesystem::path p = u8ToPath(path);
    std::filesystem::path ap = std::filesystem::absolute(p, ec);
    if (!ec) p = ap;
    p = p.lexically_normal();
#ifdef _WIN32
    std::wstring w = p.wstring();
    // 分隔符统一成 '\' (lexically_normal 不保证转换): / 与 \ 都算同一路径
    for (auto& c : w) {
        if (c == L'/') c = L'\\';
        else if (c >= L'A' && c <= L'Z') c += (wchar_t)(L'a' - L'A');
    }
    return pathToU8(std::filesystem::path(w));
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
    std::string key = canonicalKey(path);
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    };
    mix(key.data(), key.size());
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

namespace {
// 构建期槽位内存键: 与 tileKey 同构(level<<48 | tx<<24 | ty)
inline uint64_t slotKey(int level, int tx, int ty) {
    return ((uint64_t)level << 48) | ((uint64_t)tx << 24) | (uint64_t)ty;
}
inline void decodeSlotKey(uint64_t k, int& level, int& tx, int& ty) {
    level = (int)(k >> 48);
    tx = (int)((k >> 24) & 0xffffff);
    ty = (int)(k & 0xffffff);
}
}  // namespace

void VtCache::putSlotMem(int level, int tx, int ty, uint64_t off, uint32_t size) {
    slotsMem_[slotKey(level, tx, ty)] = SlotRec{off, size};
}

bool VtCache::getSlotMem(int level, int tx, int ty, SlotRec& out) const {
    auto it = slotsMem_.find(slotKey(level, tx, ty));
    if (it == slotsMem_.end()) return false;
    out = it->second;
    return true;
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
    // 数据段紧接头之后; 稀疏槽区与行目录在 finalize() 时追加到数据段尾部
    h_.dataStart = sizeof(VtFileHeader);
    h_.dataEnd = h_.dataStart;
    h_.slotAreaOffset = 0;
    h_.rowDirOffset = 0;
    h_.rowDirCount = 0;
    h_.fullyBuiltLevels = 0;
    path_ = path;
    fileMtx_ = fm;
    dirtyHeader_ = true;
    tilesWritten_ = 0;
    slotsMem_.clear();
    rowDir_.clear();
    slotAreaStart_ = 0;

    f_.seekp(0);
    f_.write((const char*)&h_, sizeof(VtFileHeader));
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
    loadRowDir();
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

// 读入行目录(稀疏槽区起点 + 每 (level,ty) 的槽区间)
void VtCache::loadRowDir() {
    rowDir_.clear();
    slotAreaStart_ = (uint32_t)h_.slotAreaOffset;
    if (h_.rowDirOffset == 0 || h_.rowDirCount == 0) return;   // 未 finalize
    if (!f_.is_open() || !fileMtx_) return;
    std::lock_guard<std::mutex> lk(ioMtx_);
    f_.clear();
    f_.seekg((std::streamoff)h_.rowDirOffset);
    std::vector<VtRowDir> rows(h_.rowDirCount);
    f_.read((char*)rows.data(), (std::streamsize)(rows.size() * sizeof(VtRowDir)));
    if ((size_t)f_.gcount() != rows.size() * sizeof(VtRowDir)) {
        rowDir_.clear();
        return;
    }
    // 目录按 (level, ty) 升序写入; std::map 的键同序, 直接插入即可
    for (const auto& r : rows) rowDir_.emplace(std::make_pair((int)r.level, (int)r.ty), r);
}

// 经行目录定位一片的槽文件偏移(读不到目录/越界返回 false)
bool VtCache::findSlot(int level, int tx, int ty, uint64_t& slotFilePos) const {
    if (rowDir_.empty() || h_.slotAreaOffset == 0) return false;
    auto it = rowDir_.find(std::make_pair(level, ty));
    if (it == rowDir_.end()) return false;
    const VtRowDir& r = it->second;
    if (tx < (int)r.txLo) return false;
    uint32_t i = (uint32_t)tx - r.txLo;
    if (i >= (uint32_t)r.count) return false;
    slotFilePos = h_.slotAreaOffset +
                  ((uint64_t)r.slotStart + i) * sizeof(VtSlot);
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
    // 构建期(未 finalize): 行目录还不存在, 回退到内存槽表
    if (rowDir_.empty()) {
        SlotRec sr{};
        return getSlotMem(level, tx, ty, sr) && sr.size > 0;
    }
    uint64_t sp = 0;
    if (!findSlot(level, tx, ty, sp)) return false;
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.clear();
    f_.seekg((std::streamoff)sp);
    f_.read((char*)&s, sizeof(s));
    return s.valid != 0 && s.size > 0;
}

bool VtCache::writeTile(int level, int tx, int ty, const VtTile& t) {
    if (!f_.is_open() || !fileMtx_ || !levelValid(level, tx, ty)) return false;
    std::unique_lock<std::shared_mutex> flk(*fileMtx_);
    std::lock_guard<std::mutex> lk(ioMtx_);
    // 旧槽(仅构建期内存里有): 决定"原地覆盖"还是"追加"。
    // 同一片被反复 flush 时, 若新块 <= 旧块就原地覆盖 -> 不追加、不产生垃圾、文件不涨。
    SlotRec prev{};
    bool re = getSlotMem(level, tx, ty, prev) && prev.size > 0;

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

    // 槽位只记内存; finalize() 时才按 (level,ty) 分组落成稀疏槽表 + 行目录
    putSlotMem(level, tx, ty, off, (uint32_t)cs);

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
    // 构建期(未 finalize): 行目录还不存在, 回退到内存槽表
    if (rowDir_.empty()) {
        SlotRec sr{};
        if (!getSlotMem(level, tx, ty, sr) || sr.size == 0) return false;
        std::lock_guard<std::mutex> lk(ioMtx_);
        std::vector<char> comp(sr.size);
        f_.clear();
        f_.seekg((std::streamoff)sr.offset);
        f_.read(comp.data(), (std::streamsize)sr.size);
        if ((size_t)f_.gcount() != sr.size) return false;
        unsigned long long rsz = ZSTD_getFrameContentSize(comp.data(), comp.size());
        if (rsz == ZSTD_CONTENTSIZE_ERROR || rsz == ZSTD_CONTENTSIZE_UNKNOWN) return false;
        std::vector<uint8_t> raw((size_t)rsz);
        size_t ds = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
        if (ZSTD_isError(ds)) return false;
        return deserializeTile(raw.data(), ds, t);
    }
    uint64_t sp = 0;
    if (!findSlot(level, tx, ty, sp)) return false;
    std::lock_guard<std::mutex> lk(ioMtx_);
    VtSlot s{};
    f_.clear();
    f_.seekg((std::streamoff)sp);
    f_.read((char*)&s, sizeof(s));
    if ((size_t)f_.gcount() != sizeof(s)) return false;
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

// finalize 的核心: 把构建期内存里的槽位表落成「按行稀疏槽表 + 行目录」。
// 步骤:
//   1. 把 slotsMem_ 按 (level, ty, tx) 排序 —— 同一行的片聚在一起
//   2. 每行算 [txLo, txHi] 区间, 只为该区间分配 count 个连续槽
//   3. 先压实数据段(按 offset 升序前移, 消除反复 flush 的垃圾),
//      同时更新内存里各片的 offset
//   4. 把紧凑槽表写到 dataEnd 处, 行目录写到槽表之后, 回填头并截断
bool VtCache::finalize() {
    if (!f_.is_open() || !fileMtx_) return false;
    std::unique_lock<std::shared_mutex> flk(*fileMtx_);

    // ---- 1/2. 排序 + 分行 ----
    struct Item { int level, tx, ty; uint64_t off; uint32_t size; };
    std::vector<Item> items;
    items.reserve(slotsMem_.size());
    for (const auto& kv : slotsMem_) {
        int lv, tx, ty;
        decodeSlotKey(kv.first, lv, tx, ty);
        items.push_back({lv, tx, ty, kv.second.offset, kv.second.size});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.level != b.level) return a.level < b.level;
        if (a.ty != b.ty) return a.ty < b.ty;
        return a.tx < b.tx;
    });
    // 构造行目录 + 槽数组。槽下标 = 前面各行 count 之和(紧凑连续)。
    std::vector<VtRowDir> rows;
    std::vector<VtSlot> slots;
    {
        size_t i = 0;
        while (i < items.size()) {
            size_t j = i;
            const int lv = items[i].level, ty = items[i].ty;
            uint32_t txLo = (uint32_t)items[i].tx, txHi = txLo;
            while (j < items.size() && items[j].level == lv && items[j].ty == ty) {
                txLo = std::min(txLo, (uint32_t)items[j].tx);
                txHi = std::max(txHi, (uint32_t)items[j].tx);
                ++j;
            }
            const uint32_t count = txHi - txLo + 1;
            // 目录条目字段宽度有限(ty/count 为 16 位, level 为 8 位); 超限直接放弃该行,
            // 不静默写坏。maxLevel 上限 12~14 时不会触发(单行最多 16384 个 tx)。
            if (count > 0xffffu || ty > 0xffff || lv > 0xff) {
                spdlog::error("[vt] 行目录条目超宽(L{} ty{} count{}), 放弃该行", lv, ty, count);
                return false;
            }
            VtRowDir r{};
            r.slotStart = (uint32_t)slots.size();   // 本行第一个槽的全局下标
            r.txLo = txLo;
            r.count = (uint16_t)count;
            r.ty = (uint16_t)ty;
            r.level = (uint8_t)lv;
            rows.push_back(r);
            const size_t rowBase = slots.size();
            slots.resize(rowBase + count);           // 中间空洞保持 valid=0
            for (size_t k = i; k < j; ++k) {
                VtSlot s{};
                s.offset = items[k].off;
                s.size = items[k].size;
                s.valid = 1;
                slots[rowBase + ((uint32_t)items[k].tx - txLo)] = s;
            }
            i = j;
        }
    }

    // ---- 3. 压实数据段(按 offset 升序前移), 就地更新 slots 里的 offset ----
    {
        std::lock_guard<std::mutex> lk(ioMtx_);
        std::vector<uint32_t> order(slots.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = (uint32_t)i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return slots[a].offset < slots[b].offset;
        });
        uint64_t dst = h_.dataStart;
        std::vector<char> buf;
        for (uint32_t si : order) {
            VtSlot& s = slots[si];
            if (!s.valid) continue;                     // 空洞
            if (s.offset != dst) {
                buf.resize(s.size);
                f_.clear();
                f_.seekg((std::streamoff)s.offset);
                f_.read(buf.data(), (std::streamsize)s.size);
                if ((size_t)f_.gcount() != s.size) return false;   // 块读失败: 中止
                f_.clear();
                f_.seekp((std::streamoff)dst);
                f_.write(buf.data(), (std::streamsize)s.size);
                if (!f_.good()) return false;
                s.offset = dst;
            }
            dst += s.size;
        }
        h_.dataEnd = dst;
    }

    // ---- 4. 写槽区 + 行目录, 回填头, 截断 ----
    uint64_t slotAreaOff = h_.dataEnd;
    uint64_t rowDirOff = slotAreaOff + (uint64_t)slots.size() * sizeof(VtSlot);
    {
        std::lock_guard<std::mutex> lk(ioMtx_);
        // 槽区
        if (!slots.empty()) {
            f_.clear();
            f_.seekp((std::streamoff)slotAreaOff);
            f_.write((const char*)slots.data(),
                     (std::streamsize)(slots.size() * sizeof(VtSlot)));
        }
        // 行目录
        if (!rows.empty()) {
            f_.clear();
            f_.seekp((std::streamoff)rowDirOff);
            f_.write((const char*)rows.data(),
                     (std::streamsize)(rows.size() * sizeof(VtRowDir)));
        }
        h_.slotAreaOffset = slotAreaOff;
        h_.rowDirOffset = rowDirOff;
        h_.rowDirCount = (uint32_t)rows.size();
        f_.seekp(0);
        f_.write((const char*)&h_, sizeof(VtFileHeader));
        f_.flush();
        if (!f_.good()) return false;
    }
    uint64_t fileSize = rowDirOff + (uint64_t)rows.size() * sizeof(VtRowDir);

    // 截断到最终大小(去掉垃圾尾巴)。fstream 不能截断 -> 关掉重开再 resize。
    std::string p = path_;
    f_.close();
    std::error_code ec;
    std::filesystem::resize_file(u8ToPath(p), fileSize, ec);
    if (ec) spdlog::warn("[vt] resize_file 失败(保留未截断文件): {} ({})", p, ec.message());
    f_.open(p, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_.is_open()) {
        spdlog::error("[vt] finalize 后重开失败: {}", p);
        dirtyHeader_ = true;   // 头部已写盘, 但对象不可用; 交由上层处理
        return false;
    }
    path_ = p;
    dirtyHeader_ = false;
    slotsMem_.clear();
    // 重新读入行目录, 让本对象立即可读
    loadRowDir();
    spdlog::info("[vt] 稀疏槽表: {} 片 -> {} 行(槽区 {:.2f} MB, 目录 {:.2f} MB)",
                 items.size(), rows.size(),
                 slots.size() * sizeof(VtSlot) / 1048576.0,
                 rows.size() * sizeof(VtRowDir) / 1048576.0);
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
