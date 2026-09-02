#pragma once
#include <cstdint>
#include <vector>
#include "glad/glad.h"

class MapScene;

class GLBackend {
public:
    GLuint fbo = 0, tex = 0, program = 0;
    int texW = 0, texH = 0;

    GLuint gridVao = 0, gridVbo = 0;   // 无数据时的测试网格
    long long gridCount = 0;

    struct LayerGeom {
        GLuint vao = 0; GLuint vbo = 0; long long count = 0, capV = 0;     // 线/面边界
        GLuint pvao = 0; GLuint pvbo = 0; long long pcount = 0, capP = 0; // 点
        GLuint fvao = 0; GLuint fvbo = 0; long long fcount = 0, capF = 0; // 面填充三角形
        std::vector<float> cpuV;  // display-CRS CPU 副本(容量扩容时重传用)
        std::vector<float> cpuP;
        std::vector<float> cpuF;
    };
    std::vector<LayerGeom> geoms;       // 每个图层独立 VBO

    void init();
    void resize(int w, int h);                 // 重建 FBO 纹理以匹配地图视口
    void createTestGrid();                    // 无数据时显示的参考网格
    int addLayerPlaceholder();                // 先建空图层, 后续 appendLayer 增量填数据
    void appendLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts = {},
                     const std::vector<float>& fill = {}); // 增量追加
    int addLayer(const std::vector<float>& v, const std::vector<float>& pts = {},
                 const std::vector<float>& fill = {});   // 一次性建+填
    void updateLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts = {},
                     const std::vector<float>& fill = {});  // 整体替换(重投影切换)
    void removeLayer(int idx);   // 按索引移除图层(同步删除其 VBO)
    void clearLayers();
    void render(const MapScene& scene);        // 渲染到 FBO(面填充 alpha 取各图层 color[3])
    uint32_t texture() const { return tex; }

private:
    void ensureFbo(int w, int h);
    uint32_t buildProgram();
    void appendGeom(LayerGeom& g, const std::vector<float>& v, const std::vector<float>& pts,
                    const std::vector<float>& fill);
};
