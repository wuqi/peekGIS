#pragma once
#include <string>
#include <vector>
#include "data/gdal_datasource.h"

struct AppConfig;

// v3 缓存布局(每图层一个缓存文件 + meta 头, float 载荷用 zstd 压缩):
//   cache/<sourceId>/meta.bin   头部: magic/版本/源文件签名/源路径/各图层元信息(不含几何)
//   cache/<sourceId>/l0.bin    图层0: 顶点+点 载荷的 zstd 压缩块
//   cache/<sourceId>/l1.bin  ...
// 读缓存只解压所需图层; 索引(.index)按"每个图层一个记录"维护 LRU 与删除。
//
// 带 .geomcache 缓存的矢量加载(兼容旧调用): 命中且签名有效则跳过 GDAL; 未命中则读取并写缓存。
bool loadVectorCachedAll(const std::string& path, std::vector<VectorData>& out, const AppConfig& cfg);

// 读取全部图层 / 仅指定图层(用于 filtered 加载, 只解压被选图层)
bool readCacheAll(const std::string& path, std::vector<VectorData>& out, const AppConfig& cfg);
bool readCacheLayers(const std::string& path, const std::vector<int>& indices,
                     std::vector<VectorData>& out, const AppConfig& cfg);
// 写入全部图层缓存(每图层一个文件)
void writeCacheAll(const std::string& path, const std::vector<VectorData>& vds, const AppConfig& cfg);

// ---- 流式写缓存(分块, 避免整层几何常驻内存) ----
// 读取路径边读边把块追加进缓存文件, 不累积整层内存。收尾时写 meta/索引并执行 LRU 预算。
// CacheWriter 在 cpp 内实现(不透明句柄)。线程安全由调用方保证(每图层一个 writer)。
struct CacheWriter;
// 打开一个缓存源(写 meta.bin + 每图层一个空文件)。层数 = layerCount。
CacheWriter* cacheWriterOpen(const std::string& path, const std::vector<VectorData>& layerMeta,
                             const AppConfig& cfg);
// 追加一个块到指定图层文件(chunk = 一次 pushChunk 的几何)。zstd 压缩后落地, 不驻留内存。
void cacheWriterAppend(CacheWriter* w, int layerIdx,
                       const std::vector<float>& verts, const std::vector<float>& pts,
                       const std::vector<float>& tris);
// 收尾: 回填块数、写索引、LRU 预算、关闭。必须在写进程结束时调用一次。
void cacheWriterClose(CacheWriter* w);

// 缓存管理: 每个图层是一个独立缓存单元(记录/删除/驱逐均按图层)
struct CacheEntry {
    std::string sourceId;
    int layerIdx = 0;
    std::string layerName;      // 图层名
    std::string sourcePath;     // 原始数据路径
    int64_t bytes = 0;
    int64_t lastAccess = 0;     // 毫秒时间戳
};
std::vector<CacheEntry> listCacheEntries(const AppConfig& cfg);
bool deleteCacheEntry(const std::string& sourceId, int layerIdx, const AppConfig& cfg);
void clearAllCache(const AppConfig& cfg);