#pragma once
// v2 矢量瓦片存储: 单文件 + 每层固定槽表 + 单瓦片 zstd。
// 布局: [VtFileHeader][L0 槽表][L1 槽表]...[数据段(追加的 zstd 块)]
// 随机读 O(1); 写入追加 + 回写 16B 槽; 崩溃最多丢该瓦片(缓存可重建)。
#include "vt/vt_types.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace peekg::vt {

// 源身份 hash(路径 + mtime + size)。文件不可 stat(如 /vsizip)时退化为路径 hash。
uint64_t sourceHash(const std::string& path);

// 缓存文件路径: <cacheDir>/vtk/<hash8>_e<srcEpsg>_d<dstEpsg>.vtk
std::string vtCachePath(const std::string& cacheDir, const std::string& srcPath,
                        int srcEpsg, int dstEpsg);

// v2 缓存条目(供缓存管理窗口列出)
struct VtCacheEntry {
    std::string path;
    uint64_t bytes = 0;
    int srcEpsg = 0, dstEpsg = 0;
    int maxLevel = 0;
    int64_t buildTime = 0;
};
std::vector<VtCacheEntry> listVtCaches(const std::string& cacheDir);
bool deleteVtCache(const std::string& path);
void clearVtCaches(const std::string& cacheDir);

class VtCache {
public:
    VtCache() = default;
    ~VtCache();
    VtCache(const VtCache&) = delete;
    VtCache& operator=(const VtCache&) = delete;

    // 新建(截断): 写文件头 + 预置槽表区(清零)
    bool create(const std::string& path, const VtFileHeader& h);
    // 打开已有文件
    bool open(const std::string& path);
    // 重新从磁盘读文件头(构建中 fullyBuiltLevels/dataEnd 会变; 读端用它刷新)
    bool reloadHeader();
    void close();

    bool isOpen() const { return f_.is_open(); }
    const VtFileHeader& header() const { return h_; }

    bool writeTile(int level, int tx, int ty, const VtTile& t);
    bool readTile(int level, int tx, int ty, VtTile& t) const;
    bool hasTile(int level, int tx, int ty) const;

    void setFullyBuilt(int level);
    void finalize();   // 回写文件头(dataEnd / fullyBuiltLevels)

    uint64_t tilesWritten() const { return tilesWritten_; }
    uint64_t dataBytes() const { return h_.dataEnd > h_.dataStart ? h_.dataEnd - h_.dataStart : 0; }

private:
    uint64_t slotIndex(int level, int tx, int ty) const;
    uint64_t slotPos(int level, int tx, int ty) const;
    bool levelValid(int level, int tx, int ty) const;

    mutable std::fstream f_;
    VtFileHeader h_{};
    std::string path_;
    bool dirtyHeader_ = false;
    uint64_t tilesWritten_ = 0;
};

}  // namespace peekg::vt
