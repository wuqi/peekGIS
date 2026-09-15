#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "glad/glad.h"
#include "map/map_scene.h"

class MapScene;

class GLBackend {
public:
    GLuint fbo = 0, tex = 0, program = 0;
    int texW = 0, texH = 0;
    GLuint rProgram = 0;   // 栅格采样 program(纹理四边形)
    int rLocCenter = -1, rLocInv = -1, rLocImage = -1;
    GLuint bakeProgram = 0;   // 烘焙贴图 program(R8 覆盖度 × 层颜色)
    int bLocCenter = -1, bLocInv = -1, bLocImage = -1, bLocColor = -1;

    GLuint gridVao = 0, gridVbo = 0;   // 无数据时的测试网格
    long long gridCount = 0;

    // 矢量分块: 显示 CRS 网格切分后的一个空间块。几何在缓冲内按 v/p/f 拼接连续布局,
    // glDrawArrays 用 (first, count) 分段绘制; 每块独立 VBO, 由视口 LRU 管理。
    struct VectorBucket {
        std::vector<float> v, p, f;           // CPU 副本(显示CRS)
        GLuint vao = 0, vbo = 0;
        long long count = 0, pcount = 0, fcount = 0;  // v/p/f 各自顶点数(f 在 p 之后)
        double minx = 0, miny = 0, maxx = 0, maxy = 0; // 该块几何范围(显示CRS)
        long long lastUse = 0;                // 帧号, LRU 淘汰依据
        bool resident = false;                // 是否已上传 GPU
    };

    struct LayerGeom {
        // staging(未最终化): 加载流式期间沿用旧单缓冲, 行为与分块前一致
        GLuint vao = 0; GLuint vbo = 0; long long count = 0, capV = 0;     // 线/面边界
        GLuint pvao = 0; GLuint pvbo = 0; long long pcount = 0, capP = 0; // 点
        GLuint fvao = 0; GLuint fvbo = 0; long long fcount = 0, capF = 0; // 面填充三角形
        std::vector<float> cpuV;  // display-CRS CPU 副本(容量扩容时重传用)
        std::vector<float> cpuP;
        std::vector<float> cpuF;
        // committed(最终化): 分块后每块独立 VBO, 视口 LRU 上传/淘汰
        bool committed = false;
        bool blockBuckets = false;   // 块桶层: 缓存命中逐块直接成桶(块=桶), 无空间网格/裁剪
        std::vector<VectorBucket> buckets;
        std::vector<int> cellToBucket;        // 格下标 -> buckets 索引(-1=空格)
        std::vector<uint8_t> wantedScratch;   // syncVectorView 每帧复用, 避免反复堆分配
        size_t gridN = 0;                     // 网格边数(格数 = gridN^2)
        double minx = 0, miny = 0, maxx = 0, maxy = 0; // 层 bbox(显示CRS)
        double cellW = 1, cellH = 1;
        size_t bucketCapacity = 256;          // 最多驻留块数(超出则 LRU 淘汰)
        // 后台重建(显示 CRS 切换)期间的身份标记
        uint64_t seq = 0;         // 图层代际(新建图层递增, 防索引复用误配对)
        uint64_t reTok = 0;       // 最近一次重建的 token(重启重建时旧结果作废)
        bool rebuilding = false;  // 后台重建进行中(该层暂不渲染)
        int reTarget = 0;         // 重建目标显示 CRS(用于重复请求去重)
        int bucketsEpsg = 0;      // 已提交桶顶点实际所在 CRS(0=未知/源原生)。CRS 切换判据
        bool blockRebuilding = false;  // 块桶层 CRS 重建中: 从缓存重读重投影像新桶(首块到达即结束)
    };

    // 纯 CPU 分块结果(无 GL 对象), 供后台重建线程产出、主线程换桶
    struct BucketCpu {
        std::vector<VectorBucket> buckets;
        std::vector<int> cellToBucket;
        size_t gridN = 0;
        double minx = 0, miny = 0, maxx = 0, maxy = 0, cellW = 1, cellH = 1;
        long long totalVerts = 0;
    };
    std::vector<LayerGeom> geoms;       // 每个图层独立 VBO
    long long frameNo = 0;              // 帧计数(块 lastUse)

    // 一个细节块纹理(由 LRU 管理)。span=每块覆盖的源像素(≈256屏幕像素), 纹理比 span 小(重采样后)。
    struct RasterTile {
        GLuint tex = 0;        // RGBA8 纹理
        GLuint vao = 0, vbo = 0;  // 四边形顶点(位置+uv)
        int span = 0, tx = 0, ty = 0;   // 块网格步长(源像素) + 网格下标
        int sw = 0, sh = 0;            // 块实际覆盖的源像素窗口(边缘块可能 < span)
        int w = 0, h = 0;             // 纹理像素尺寸(重采样后的输出)
    };

    // 栅格图层: 底图(不黑) + 细节块 LRU(清晰)。采样用 sampler2D 绑定活动纹理单元(TEXTURE0)。
    struct RasterLayer {
        // 底图
        GLuint baseTex = 0;              // 全局降采样纹理
        int baseW = 0, baseH = 0;
        bool hasBase = false;
        // 细节块 LRU 缓存
        std::vector<std::vector<int>> tileQueue;  // LRU 访问顺序(最近在尾)
        std::map<long long, RasterTile> tiles;   // key = tileKey(span,tx,ty), span 区分 LOD
        size_t tileLimit = 256;                  // 最多保留块数
        // 共享四边形 VBO 模板(每块复用, 位置运行时改)
        GLuint tplVao = 0, tplVbo = 0;
        std::vector<float> tplVerts;             // 6 顶点×(x,y,u,v) 空槽, 每帧按块世界矩形填充
    };
    std::vector<RasterLayer> rasters;
    // 已知会用到的 GL uniforms
    int locCenter = -1, locInv = -1, locAlpha = -1, locColor = -1;

    void init();
    void resize(int w, int h);                 // 重建 FBO 纹理以匹配地图视口
    void createTestGrid();                    // 无数据时显示的参考网格
    int addLayerPlaceholder();                // 先建空图层, 后续 appendLayer 增量填数据
    void appendLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts = {},
                     const std::vector<float>& fill = {}); // 增量追加
    // 整层几何直接移入 CPU staging(免 GPU staging 与重复拷贝)。用于缓存命中整层单块。
    void setStagingCpu(int idx, std::vector<float>&& v, std::vector<float>&& p, std::vector<float>&& f);
    // 把 CPU staging 几何移出到 out(并释放其 GPU staging), 供后台重建零拷贝快照。
    bool stealStagingCpu(int idx, VectorData& out);
    int addLayer(const std::vector<float>& v, const std::vector<float>& pts = {},
                 const std::vector<float>& fill = {});   // 一次性建+填
    void updateLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts = {},
                     const std::vector<float>& fill = {});  // 整体替换(重投影切换)
    void removeLayer(int idx);   // 按索引移除图层(同步删除其 VBO)
    void clearLayers();
    // 把 staging 数据按显示CRS网格切分成块(每块独立 VBO), 之后由视口 LRU 管理
    void finalizeLayer(int idx, int bucketsEpsg = 0);   // epsg: 传入流式提交时桶所在显示 CRS
    // 每帧: 按视口上传缺失块 + LRU 淘汰超出 bucketCapacity 的旧块
    void syncVectorView(const MapScene& scene);

    // 块桶层(缓存命中逐块): 块=桶, 不做空间网格切分/视口裁剪(上层一次性画整层)。
    // crsEpsg=桶顶点实际所在 CRS(0=源原生, 未重投影)。该层 L.data 无整层几何,
    // 不支持 applyDisplayCrs 的后台重建(由上层拦截/隐藏)。上传后桶 CPU 副本即释放。
    void beginBucketLayer(int idx, int crsEpsg);
    void addBucket(int idx, std::vector<float>& v, std::vector<float>& p, std::vector<float>& f);
    bool isBlockBucketLayer(int idx) const;
    bool isBlockRebuilding(int idx) const;   // 块桶层是否处于 CRS 重建态(待缓存重读)
    void startBlockRebuild(int idx, int targetEpsg);   // 块桶层 CRS 重建: 清旧桶进入重建态

    // 纯 CPU 分块(无 GL): 可选做 源CRS->targetEpsg 重投影(相等时视为已投影直接使用)。
    // 供 finalizeLayer 与后台重建线程共用, 也是可单测的几何核心。
    static bool buildBucketCpu(const std::vector<float>& V, const std::vector<float>& P,
                               const std::vector<float>& F, int srcEpsg, int targetEpsg,
                               BucketCpu& out);
    // 显示 CRS 切换: 后台线程重投影+分块(快照源数据), 完成后主线程 pollRebuilds 换桶。
    // 期间该层隐藏(旧 CRS 几何不再渲染), 层内已提交块被释放。
    void startLayerRebuild(int gi, std::shared_ptr<const VectorData> snap, int targetEpsg);
    void pollRebuilds();   // 主线程每帧调用: 收取完成的重建结果并上传/换桶
    void debugFrameStats();   // main.cpp 每帧调用: 记录帧间 dt (PEEK_DEBUG_FRAME=1)

    // 栅格接口(底图 + 细节块 LRU)
    int addRasterLayer(const RasterData& rd);                       // 建底图纹理 + 模板四边形
    void setRasterBase(int idx, const std::vector<unsigned char>& rgba, int w, int h);  // 上传底图
    // 上传一块细节(RGBA8): 块覆盖源像素窗口 [x0,y0]-[x0+sw,y0+sh], 纹理重采样为 w×h
    void setRasterTile(int idx, int span, int tx, int ty, int sw, int sh,
                       const std::vector<unsigned char>& rgba, int w, int h);
    void removeRasterLayer(int idx);
    void clearRasterLayers();
    // 查询某栅格图层的某块是否已缓存(驱动据此决定发起异步读)
    bool hasRasterTile(int idx, int span, int tx, int ty) const;
    // 清空一个图层所有细节块(缩放/重载时)
    void clearRasterTiles(int idx);

    void render(const MapScene& scene);        // 渲染到 FBO(面填充 alpha 取各图层 color[3])
    uint32_t texture() const { return tex; }

    // ---- 烘焙 LOD(瓦片金字塔): 每级 2^L x 2^L 片, 每片 kTileRes² R8 覆盖度 ----
    // 按需烘焙: 视口要哪片烘哪片(OGR 查该片范围几何); 显存 LRU + 磁盘缓存 => 可无限扩展。
    struct BakeTile { GLuint fbo = 0, tex = 0, rbo = 0; long long lastUse = 0; };
    struct BakeLayer {
        std::map<uint64_t, BakeTile> tiles;   // 显存片: key=(level,tx,ty)
        int maxLevel = 6;                     // 最高级别
        bool hasBounds = false;
        double minx = 0, miny = 0, maxx = 0, maxy = 0;   // 层 bbox(显示CRS)
        float color[4] = {0.3f, 0.8f, 0.9f, 0.35f};
        GLuint vao = 0, vbo = 0;       // 烘焙块上传(2 float/顶点)
        GLuint qvao = 0, qvbo = 0;     // 贴图四边形(4 float: x,y,u,v)
        bool baking = false;
        int curLevel = 0, curTx = 0, curTy = 0;
        double curCx = 0, curCy = 0, curInvX = 1, curInvY = 1;   // 当前片 view(世界->NDC, 各向异性)
    };
    static const int kTileRes = 256;
    std::vector<BakeLayer> bakes;
    uint64_t tileKey(int level, int tx, int ty) const;
    void setBakeBounds(int idx, double minx, double miny, double maxx, double maxy, int maxLevel);
    bool hasBakeBounds(int idx) const;
    int  bakeLevelFor(int idx, const MapScene& scene) const;   // 该用的级别; -1=over-zoom
    bool bakeTileRange(int idx, int level, const MapScene& scene,
                       int& tx0, int& ty0, int& tx1, int& ty1) const;
    bool hasBakeTile(int idx, int level, int tx, int ty) const;
    void bakeTileBegin(int idx, int level, int tx, int ty);
    void bakeTileAppend(int idx, std::vector<float>& v, std::vector<float>& p, std::vector<float>& f);
    // 按环模板(奇偶)填充: rings 为若干环(世界坐标 xy 交替)。GPU 填充, 不需耳切。
    void bakeTileAppendRings(int idx, const std::vector<float>& lines,
                             const std::vector<float>& points,
                             const std::vector<std::vector<float>>& rings);
    void bakeTileEnd(int idx);
    void uploadBakeTile(int idx, int level, int tx, int ty, const unsigned char* px, int res);
    bool dumpBakeTile(int idx, int level, int tx, int ty, std::vector<unsigned char>& out);   // 读回片纹理(R8)
    void evictBakeTiles(int idx, size_t maxTiles);
    void removeBake(int idx);

    // ---- over-zoom: 放大超过烘焙精度时, 从原始数据按视口 bbox 查询到的矢量几何 ----
    struct OverZoom {
        GLuint vao = 0, vbo = 0;
        long long count = 0, pcount = 0, fcount = 0;
        bool has = false;
        float color[4] = {0.3f, 0.8f, 0.9f, 0.35f};
    };
    std::vector<OverZoom> overzooms;
    void setOverZoom(int idx, const std::vector<float>& v, const std::vector<float>& p,
                     const std::vector<float>& f, const float color[4]);
    void clearOverZoom(int idx);

private:
    void ensureFbo(int w, int h);
    uint32_t buildProgram();
    GLuint buildRasterProgram();       // 栅格采样顶点/片段 program(带纹理采样)
    GLuint buildBakeProgram();         // 烘焙贴图 program(R8 覆盖度 × 颜色)
    void appendGeom(LayerGeom& g, const std::vector<float>& v, const std::vector<float>& pts,
                    const std::vector<float>& fill);
    size_t uploadBucket(VectorBucket& b, long long frame);   // 返回本块上传的字节数(供逐帧预算)
    void evictBucket(VectorBucket& b);
    void mergeChunk(LayerGeom& g, const std::vector<float>& v, const std::vector<float>& pts,
                    const std::vector<float>& fill);
    void releaseStaging(LayerGeom& g);

    std::mutex rm_;                    // 保护 rebuilt_
    struct Rebuilt { int gi; uint64_t seq; uint64_t tok; int targetEpsg; std::unique_ptr<BucketCpu> res; };
    std::vector<Rebuilt> rebuilt_;     // 后台线程产出, 主线程 pollRebuilds 收取
    uint64_t layerSeq_ = 1;            // 新图层代际(递增)
    uint64_t rebuildTok_ = 1;          // 重建任务 token(递增)
    double lastRenderMs_ = 0;            // 上一帧 render() CPU 耗时(ms), debugFrameStats 读取用
};
