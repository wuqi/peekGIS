# peekGIS

一个基于 C++20 / OpenGL / Dear ImGui 的轻量矢量 GIS 查看器。通过 GDAL/OGR 读取多种矢量格式（shp、gpkg 等），支持动态坐标投影、渐进异步加载、要素属性查询与属性表浏览。

## 功能

- **矢量读取**：支持 GDAL 支持的各种矢量格式（ESRI Shapefile、GeoPackage、FileGDB 等）。
- **动态投影**：加载到"显示 CRS"，无需预转换；启动时可选择默认坐标系。
- **渐进异步加载**：大文件后台分块加载并渐进绘制，不卡界面。
- **图层管理**：左侧图层面板，支持右键菜单（卸载图层、缩放至图层、打开属性表）。
- **要素属性查询**：双击地图查询要素，右侧面板显示属性，支持固定编码切换（UTF-8 / GBK / Big5 / CP1252 / UTF-16LE），切换不重读文件。
- **属性表**：底部停靠面板分页浏览属性；可勾选显示列；双击某行可平移居中并高亮该要素；编码可实时切换。
- **缓存管理**：几何缓存（v1.0，`config.toml` 可配置大小/路径）与矢量瓦片缓存（v2）统一在「缓存管理」窗口列出、删除。
- **超大矢量瓦片缓存（v2）**：千万级要素预切成瓦片金字塔（单文件 + 每层槽表 + 单瓦片 zstd），按视口只取可见瓦片流式渲染，跨 CRS 后台重投影；`vt_build` 建缓存，打开源文件自动命中。
- **WKT 渲染**：可直接粘贴 WKT 生成图层。

## 构建

依赖（均 vendored 或 vcpkg 提供）：

- xmake
- C++20 编译器（MSVC，Windows）
- vcpkg 安装的 GDAL + zstd（见 `xmake.lua` 中 `vcpkg` 路径）
- vendored：Dear ImGui（docking）、glad、GLFW、glm、spdlog、toml11、earcut

```bash
xmake            # 默认构建
xmake build tests
xmake run tests  # 单元测试(doctest)
```

构建后 exe 旁边会拷贝 GDAL/PROJ 数据与运行期 DLL 以及字体（见 `xmake.lua` 的 `after_build`）。

### 辅助工具

```bash
xmake build vt_build     # 建 v2 矢量瓦片缓存 (超大数据)
xmake build vt_estimate  # 只读估算：顶点数/金字塔层数/体积/v1.0-v2 路由建议
xmake build vt_tests     # v2 管线单测(不依赖 lua/GL)
xmake build diag_srs diag_geoloc   # 坐标/地理定位诊断
```

工具二进制输出到 `build/windows/x64/release/bin/`。

### 配置 vcpkg 路径

GDAL/Zstd 由 vcpkg 提供。`xmake.lua` 顶部的 `vcpkg_dir` 变量持有该路径，默认为 `d:/dev/vcpkg/installed/x64-windows`。本机 vcpkg 位置不同时可覆盖：

```bash
xmake f --vcpkg_dir=D:/vcpkg/installed/x64-windows   # 重新配置
xmake                                                        # 再构建
```

也可以直接改 `xmake.lua` 顶部 `option("vcpkg_dir")` 的 `set_default(...)`。

## 运行

```bash
xmake run
```

首次运行会加载 `fonts/LXGW.ttf` 显示中文界面。配置文件 `config.toml` 可调整显示 CRS、缓存目录与大小、字体等。

## 使用提示

- 菜单加载 shp / gpkg 等文件后自动缩放到图层范围。
- 双击地图 → 右侧属性面板；下拉选择编码可修正乱码（如 GBK 的 shp）。
- 图层面板右键 → "打开属性表"。
- 属性表顶栏 "选择列" 可折叠显示字段，"编码" 可切换字符串解释编码（不重读文件）。

## 超大型矢量数据（v2 矢量瓦片缓存）

对千万级要素 / 上亿顶点的矢量（如省域地类图斑 GDB），直接加载会内存爆、帧率低。
v2 方案把数据预切成 512 网格的瓦片金字塔（自研单文件：文件头 + 每层固定槽表 + 单瓦片 zstd），
渲染时按视口只取当前层可见瓦片，后台读取/（显示 CRS 不同时）重投影后上传，帧率稳定。

### 建缓存（`vt_build`）

```bash
xmake build vt_build
# --auto: 写到 app 会自动发现的路径 <cache_dir>/vtk/<hash8>_e<源EPSG>_d<显示EPSG>.vtk
build/windows/x64/release/bin/vt_build.exe "D:/data/guangdong.gdb.zip" --auto
# 也可显式给输出路径；常用参数：
#   --epsg D      显示 CRS（默认=源 EPSG）
#   --levels L    强制最深层（默认按目标每瓦片顶点自动估算）
#   --target V    目标每瓦片顶点数（默认 2048）
#   --cap C       最深层上限（默认 12）
#   --sfactor F   各层抽稀容差 = 该层格距 * F（默认 1.0；`--no-simplify` 关闭）
```

建完后在 peekGIS 里**直接打开源文件**（如 `.gdb` / `.shp`）即自动命中并走瓦片渲染；也可直接打开 `.vtk`。

### 估算（`vt_estimate`，只读）

建缓存前先评估规模，避免盲目构建：

```bash
xmake build vt_estimate
build/windows/x64/release/bin/vt_estimate.exe "D:/data/xxx.gdb" --samples 40000
# 输出：要素数、原始总顶点估计 N、各候选层降采样点数 P(L)、选出的最细层 L*、
#       金字塔体积估计、v1.0 每帧三角数与路由建议(v1.0 / v2)
```

### v2 单测

`vt_tests` 只编译 `src/vt/*` + `vt_render`，不依赖 lua/GL 上下文，便于在依赖不全的环境验证：

```bash
xmake build vt_tests && xmake run vt_tests
```

> 说明：v2 瓦片烘焙在缓存文件记录的 CRS 里。显示 CRS 与之一致时直接绘制；
> 不一致时后台整块重投影到显示 CRS（每片一次，非每帧），一份缓存即可适配多显示 CRS。

### 自动分流（v1.0 / v2）

打开矢量源文件时：

1. 若已有匹配的 v2 缓存 → 直接走瓦片渲染；
2. 否则快速估算顶点数（顺序取前 5 万个要素的平均点数 × 要素数）：
   - 低于阈值 → 走 **v1.0**（原始几何缓存，自动读写）；
   - 超过阈值且开启自动构建 → **后台生成 v2 缓存**（状态栏显示进度），完成后自动加载为瓦片图层。

阈值与开关在 `config.toml`：

```toml
[vt]
auto_build = true
threshold_verts = 10000000   # 顶点数阈值, 超过则走 v2
```

> 估算是"取前 N 个要素"的快速近似，空间自相关时可能低估；大文件若未自动触发，
> 可用 `vt_build --auto` 手动建缓存（或调低 `threshold_verts`）。

## 目录结构

```
src/
  app/        UI（面板、停靠布局、编码选择、属性表界面）
  data/       数据层（GDAL 读取、属性表分页读取、缓存、重投影、编码解码）
  map/        场景与视图（图层、平移缩放、屏幕坐标换算）
  render/     渲染后端（GL）+ v2 瓦片渲染器（vt_render）
  vt/         v2 矢量瓦片：类型/存储/要素环流/构建管道/瓦片几何
  platform/   平台工具
docs/         设计文档（需求分析、总体设计、详细设计、属性表设计、矢量缓存方案）
tools/        辅助工具（vt_build / vt_estimate / diag_srs / diag_geoloc 等）
tests/        单元测试 (doctest)
assets/       应用图标等资源
fonts/        界面字体
thirdparty/   第三方库
```

## 致谢

本项目使用了以下开源库与资源：

| 库/资源 | 用途 | 许可证 |
| --- | --- | --- |
| [GDAL/OGR](https://gdal.org) | 矢量数据读取（shp/gpkg 等） | MIT |
| [proj](https://proj.org) | 坐标系投影/重投影 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | 界面（含 docking/backends） | MIT |
| [glad](https://github.com/Dav1dde/glad) | OpenGL 加载器 | MIT |
| [GLFW](https://www.glfw.org) | 窗口与 OpenGL 上下文 | Zlib |
| [glm](https://github.com/g-truc/glm) | 数学库 | MIT |
| [spdlog](https://github.com/gabime/spdlog) | 日志 | MIT |
| [FMT](https://github.com/fmtlib/fmt) | 字符串格式化（随 spdlog 使用） | MIT |
| [toml11](https://github.com/ToruNiina/toml11) | 配置读取 | MIT |
| [earcut](https://github.com/mapbox/earcut.hpp) | 多边形三角化 | ISC |
| [doctest](https://github.com/doctest/doctest) | 单元测试框架 | MIT |
| [LXGW Neo XiHei](https://github.com/lxgw/LxgwNeoXiHei)（霞鹜新晰黑） | 界面中文字体 | SIL OFL 1.1 |

各库版权归其原作者所有，许可证文本见各库自身仓库。

## 测试

`tests` 用 doctest 编写。另有两个文件实测探针（通过环境变量启用）：

```bash
$env:PEEKGIS_TEST_IDENTIFY='D:\data\some.shp'; xmake run tests   # identify 分层耗时
$env:PEEKGIS_TEST_ATTR='D:\data\some.shp';     xmake run tests   # 属性表分页读取
```