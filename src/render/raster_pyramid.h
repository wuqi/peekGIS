#pragma once
// 栅格瓦片金字塔(与矢量同构): 扫源一遍把要素直接光栅化进最深层, 再逐层 2x2 降采样。
// 每片存 R8 覆盖度; 渲染时由 shader 乘图层颜色(可换色)。
// 输出 <cacheDir>/bake/<源哈希>_<ext>_epsg<dst>/L<l>_<tx>_<ty>.bin (zstd R8)。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace peekg::render {

// maxLevel: 最深层(与矢量 estimateMaxLevel 同法算出)。tileRes: 每片像素边长。
// onProgress: 0..100。失败返回 false。
bool buildRasterPyramid(const std::string& srcPath, int layerIdx, int dstEpsg,
                        int maxLevel, int tileRes, float fillAlpha, const std::string& cacheDir,
                        const std::function<void(int)>& onProgress,
                        const std::function<void(int, int, int)>& onTile = nullptr);

// 一层一个 .pak 的随机读写(构建与 app 显示共用同一句柄, 内部加锁, 线程安全)。
// bakeTileLoad 只读, 文件不存在返回 false; bakeTileSave 追加写(n = 片字节数, 须为平方数)。
bool bakeTileLoad(const std::string& cacheDir, const std::string& srcPath, int dstEpsg,
                  int level, int tx, int ty, std::vector<uint8_t>& out);
bool bakeTileSave(const std::string& cacheDir, const std::string& srcPath, int dstEpsg,
                  int level, int tx, int ty, const uint8_t* px, size_t n);

}  // namespace peekg::render
