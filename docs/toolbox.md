# 工具箱(Toolbox)

peekGIS 的「工具箱」面板提供两组工具:

1. **工具目录** — 由 `gdal --json-usage` 在启动时自动导入的全部 GDAL 内置 CLI 工具
   (reproject / convert / clip / buffer / slope …), 按目录树浏览, 直接填参运行。
2. **脚本工具** — 由 `bin/scripts/*.lua` 声明的常用操作快捷入口, 声明式 API,
   免去记忆 gdal 工具全名与参数名。

运行原理: 界面填好参数 → 组装 `runArgs`(fullPath 冒号拆段 + 命令行参数)
→ `gdal_proc` 以子进程方式启动 `gdal.exe`(仓库根 `launcher/` 或 release bin 随附),
退出码与 stdout/stderr 实时回填面板; 输出文件标记 `load=true` 时提供「加载输出到地图」。

## 脚本工具

每个 `bin/scripts/*.lua` 是一个脚本工具(文件名作为 `script:<name>` 标识)。

### 声明 API

| 函数 | 用法 |
|---|---|
| `toolbox.def.label = "中文名"` | 面板显示名(必写, 否则按文件名显示) |
| `toolbox.def.description = "说明文字"` | 悬停提示 / 表单上方说明 |
| `toolbox.def.tool = "gdal:raster:reproject"` | 对应的 GDAL 工具 fullPath(编进 `gdal --json-usage`) |
| `toolbox.input("input", {title=, required=true})` | 输入数据, 面板渲染为「浏览…+路径」 |
| `toolbox.param("dst-crs", "EPSG:4326", {title=})` | 普通参数, 面板预填默认值的文本, 空值不上传 |
| `toolbox.output("output", {title=, load=true})` | 输出数据; `load=true` 时成功后显示「加载输出到地图」 |

约定:

- `toolbox.def.tool` 必须引用 `gdal --json-usage` 里真实存在的 fullPath
  (可用函数名是工具目录树显示名, 见 `gdal --json-usage` 输出)。
- 可选的 `on_run(ctx)` 函数在参数传给 gdal 前执行一次, 可改写 `ctx`(参数名→字符串的映射),
  **必须返回被修改后的 `ctx` 表**(就地修改不回读)。
- 单个脚本加载失败只会记录一条日志并跳过, 不影响其它脚本。
- 缺省值按参数默认自动补全; 必填(`required=true`)的 input/output 未填时禁用「运行」。

### 预置脚本

| 脚本 | tool | 说明 |
|---|---|---|
| `reproject_raster.lua` | gdal:raster:reproject | 栅格重投影(dst-crs / resampling / overwrite) |
| `reproject_vector.lua` | gdal:vector:reproject | 矢量重投影(dst-crs 必填) |
| `clip_vector.lua` | gdal:vector:clip | 矢量裁剪(bbox 留空=整体复制) |
| `buffer_vector.lua` | gdal:vector:buffer | 矢量缓冲区(distance 必填) |
| `simplify_vector.lua` | gdal:vector:simplify | 矢量简化(tolerance 必填) |
| `rasterize_vector.lua` | gdal:vector:rasterize | 矢量栅格化(burn / resolution) |
| `slope_dem.lua` | gdal:raster:slope | 坡度(unit degrees/percent) |
| `hillshade_dem.lua` | gdal:raster:hillshade | 山体阴影(azimuth / altitude / zfactor) |
| `polygonize_raster.lua` | gdal:raster:polygonize | 栅格转矢量(attribute-name) |
| `convert_raster.lua` | gdal:raster:convert | 栅格格式转换(output-format) |

自定义脚本直接放入 `bin/scripts/`, 面板下次打开即可见; 仓库内源文件在 `assets/scripts/`(构建时拷入)。

## 运行期资源定位

- `gdal.exe`、`lua51.dll`、`share/`(GDAL/PROJ 数据)、`scripts/`、`assets/` 均随 exe 布到
  release bin, 一律按 exe 目录定位, 与启动工作目录无关。
- 工具箱面板默认在左侧图标条(toolbox 图标)打开; 图标与资源样式同 `assets/toolbox-solid.png`。