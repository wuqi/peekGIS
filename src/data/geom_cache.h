#pragma once
#include <string>
#include <vector>
#include <functional>
#include "data/vector_reader.h"

struct AppConfig;

namespace peekg::data {
// 缓存管理: 每个图层是一个独立缓存单元(记录/删除/驱逐均按图层)
struct CacheEntry {
    std::string sourceId;
    int layerIdx = 0;
    std::string layerName;      // 图层名
    std::string sourcePath;     // 原始数据路径
    int64_t bytes = 0;
    int64_t lastAccess = 0;     // 毫秒时间戳
};

// v3 缓存布局(每图层一个缓存文件 + meta 头, float 载荷用 zstd 压缩):
//   cache/<sourceId>/meta.bin   头部: magic/版本/源文件签名/源路径/各图层元信息(不含几何)
//   cache/<sourceId>/l0.bin    图层0: 顶点+点 载荷的 zstd 压缩块
//   cache/<sourceId>/l1.bin  ...
// 读缓存只解压所需图层; 索引(.index)按"每个图层一个记录"维护 LRU 与删除。
//
// 矢量几何缓存工具类: 读取 GDAL 出缓存 / 命中解码 / 流式写缓存 / LRU 索引管理。
// 全部为静态方法(不持有状态), 配置通过 AppConfig 参数传入。
class GeomCache {
public:
    // 带 .geomcache 缓存的矢量加载(兼容旧调用): 命中且签名有效则跳过 GDAL; 未命中则读取并写缓存。
    static bool loadVectorCachedAll(const std::string& path, std::vector<VectorData>& out,
                                    const AppConfig& cfg);

    // 读取全部图层 / 仅指定图层(用于 filtered 加载, 只解压被选图层)
    static bool readCacheAll(const std::string& path, std::vector<VectorData>& out,
                             const AppConfig& cfg);
    static bool readCacheLayers(const std::string& path, const std::vector<int>& indices,
                                std::vector<VectorData>& out, const AppConfig& cfg);
    // 逐块读取单个图层且不累积整层几何(与 readCacheAll 的"整层解压到内存"相反):
    // 每解出一块立即回调 onChunk(移动语义, 参数引用), 内存峰值 ≈ 一块大小。
    // outMeta 带出该层整层元信息(名称/CRS/范围/要素数)。返回 false 表示缓存失效(MISS, 调用方转 GDAL)。
    static bool readCacheLayerChunks(
        const std::string& path, int layerIdx, const AppConfig& cfg, VectorData& outMeta,
        const std::function<void(std::vector<float>&, std::vector<float>&, std::vector<float>&)>& onChunk);
    // 写入全部图层缓存(每图层一个文件)
    static void writeCacheAll(const std::string& path, const std::vector<VectorData>& vds,
                              const AppConfig& cfg);

    // 缓存管理: 每个图层是一个独立缓存单元(记录/删除/驱逐均按图层)
    static std::vector<CacheEntry> listCacheEntries(const AppConfig& cfg);
    static bool deleteCacheEntry(const std::string& sourceId, int layerIdx, const AppConfig& cfg);
    static void clearAllCache(const AppConfig& cfg);
};

// ---- 流式写缓存: 边读边把块追加进缓存文件, 整体几何不常驻内存 ----
// class CacheWriter 的具体实现见 geom_cache.cpp。
// 读取路径把几何累积进内存, 收尾(finalize)时按显示==源坐标分区成网格单元格逐格压缩落盘。
// 单元格与渲染分块(client bucket)同一套网格参数(见 render/bucketize.h), 命中时每格即一个桶,
// 解压一格立即可渲染(首帧 ≈ 一格解压时间)。析构时写 meta/索引并执行 LRU 预算。
// 半成品(写入中途出错)析构会自动清理, 不产生脏缓存。线程安全由调用方保证(每图层一个 writer)。
class CacheWriter {
public:
    CacheWriter() = default;
    CacheWriter(const CacheWriter&) = delete;
    CacheWriter& operator=(const CacheWriter&) = delete;
    CacheWriter(CacheWriter&&) noexcept = default;
    CacheWriter& operator=(CacheWriter&&) noexcept = default;

    // 打开一个缓存源(写 meta.bin + 每图层一个空文件)。层数 = layerMeta.size()。
    // 构造失败(空元数据/签名失败/文件打开失败)时 ok() 为 false, 析构不发生任何收尾。
    CacheWriter(const std::string& path, const std::vector<VectorData>& layerMeta,
                const AppConfig& cfg);
    ~CacheWriter();                                  // 收尾: 回填块数、写索引、LRU 预算

    bool ok() const { return openOk_; }

    // 追加一个块到指定图层文件(chunk = 一个 pushChunk 的几何)。zstd 压缩后落盘, 不驻留内存。
    void append(int layerIdx, const std::vector<float>& verts,
                const std::vector<float>& pts, const std::vector<float>& tris);

private:
    struct ChunkWriter;
    void finalize();

    std::string dir_, id_, sourcePath_;
    int64_t mtime_ = 0, size_ = 0;
    bool openOk_ = false;      // 构造成功打开(拥有缓存目录), 之后才可能收尾/清理
    bool writeOk_ = true;      // 写入过程是否仍健康(append 出错置 false → 析构清理半成品)
    bool done_ = false;        // 已收尾(防重复)
    int maxMb_ = 0;            // LRU 预算(MB), 来自 cfg.cache_max_mb
    std::vector<ChunkWriter> layers_;
};

}  // namespace peekg::data