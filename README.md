# peekGIS

一个基于 C++20 / OpenGL / Dear ImGui 的轻量矢量 GIS 查看器。通过 GDAL/OGR 读取多种矢量格式（shp、gpkg 等），支持动态坐标投影、渐进异步加载、要素属性查询与属性表浏览。

## 功能

- **矢量读取**：支持 GDAL 支持的各种矢量格式（ESRI Shapefile、GeoPackage、FileGDB 等）。
- **动态投影**：加载到"显示 CRS"，无需预转换；启动时可选择默认坐标系。
- **渐进异步加载**：大文件后台分块加载并渐进绘制，不卡界面。
- **图层管理**：左侧图层面板，支持右键菜单（卸载图层、缩放至图层、打开属性表）。
- **要素属性查询**：双击地图查询要素，右侧面板显示属性，支持固定编码切换（UTF-8 / GBK / Big5 / CP1252 / UTF-16LE），切换不重读文件。
- **属性表**：底部停靠面板分页浏览属性；可勾选显示列；双击某行可平移居中并高亮该要素；编码可实时切换。
- **缓存管理**：几何缓存（`config.toml` 可配置大小/路径），带管理窗口。
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

## 目录结构

```
src/
  app/        UI（面板、停靠布局、编码选择、属性表界面）
  data/       数据层（GDAL 读取、属性表分页读取、缓存、重投影、编码解码）
  map/        场景与视图（图层、平移缩放、屏幕坐标换算）
  render/     渲染后端（GL）
  platform/   平台工具
docs/         设计文档（需求分析、总体设计、详细设计、属性表设计）
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