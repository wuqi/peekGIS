# AGENTS.md — 环境全局限制 (必须遵守)

> 本文件约束本仓库所有自动化会话的工具与环境使用。违反会导致数据/构建损坏。

## Python: 只准用 conda

- Python 一律走 **`d:\miniconda3\python.exe`** (conda base), 已装 netCDF4 1.6.5 / numpy 1.26.4 / h5py 等。
- 激活方式: `%windir%\System32\cmd.exe /K d:\miniconda3\Scripts\activate.bat d:\miniconda3`;
  非交互调用直接写全路径 `& "d:\miniconda3\python.exe" ...`。
- **禁止** `python` / `py` 裸命令 (解析到 `C:\Users\wuqi\AppData\Local\Microsoft\WindowsApps\python.exe`,
  是 Store 占位符, 执行即退出码 9009, 不产生任何输出)。
- **禁止**猜测其它 Python 路径 (如 `C:\Python310\python.exe` 不存在/无解释器, 只有 Lib/Scripts)。
- Windows Python stdout 默认 GBK, 中文输出前先 `sys.stdout.reconfigure(encoding='utf-8')`。

## GDAL / 构建

- `gdalinfo` 等命令行工具不在 PATH, 不要直接调用。
- GDAL 库与头文件来自 vcpkg: `d:\dev\vcpkg\installed\x64-windows` (xmake 已默认 `--vcpkg_dir` 指向它)。
- 构建一律: 在仓库根 `G:\Develope\peekGIS` 下执行 `xmake build <target>`; 需要 gdalinfo 等价功能时
  用仓库自建工具 (如 `diag_srs.exe`, 二进制输出在 `build\windows\x64\release\bin\`) 或写临时 C++/Python 脚本。
- 运行期资源(GDAL/PROJ 数据 `share`、字体 `assets/fonts`、图标 `assets/`)都随 exe 布到
  `build\windows\x64\release\bin\`, 均按 exe 目录定位, 与启动工作目录无关。启动入口集中在仓库根 `launcher/` 目录
  (Windows: `peekgis.vbs` 无黑窗 / `peekgis.cmd` 控制台会闪一下 / 或直接 exe; Linux: `peekgis.sh`、`peekGIS.desktop` 模板)。
  构建产物里另有按出厂布局生成的启动脚本在对应 release 根。

## 常用本地数据 (只读)

| 用途 | 路径 |
|---|---|
| netCDF 测试 | `F:\ads\acpcp.1982.nc` (37.6MB, geolocation 阵列用例) |
| raster SRS 用例 | `G:\s55\下S55_12.img` (IMAGINE, UTM 55S, EPSG:32755) |
| vector SRS 用例 | `G:\cp\公开版25万-新疆shp\aanp.shp` (CGCS2000, EPSG:4490) |
| 其它 netCDF | `F:\ads\aa.nc`, `F:\ads\output_within_bounds2.nc` |

## 其它

- 整盘递归搜索 (`Get-ChildItem G:\ -Recurse`) 极易超时/卡死, 禁止对大目录无深度限制盲搜; 先问用户或限定 `-Depth`。
- 改路径等敏感内容前, 先确认文件存在 (Test-Path); 不要乱填不存在路径。
- **geoloc warp 关键结论(勿回退)**: `GDALGeoLocTransform` 的正算(源像素/行->lon/lat)对 curl 网格(行列方向均非单调)可靠; 但反算(gather, 按目标 lon/lat 搜最近源像素, `GDALCreateWarpedVRT`)在此类网格上 ~99% 输出像素返回失败/`inf` -> 全部 NoData。因此 `buildGeolocationWarp` 用**前向散射**: 逐行读源波段, 每像素正算到其真实 lon/lat, 摆到最近输出单元(距单元中心平方距离最近者胜, 近似 nearest); 结果写 MEM 再 `GDALCreateCopy` 到 GTiff(LZW)。验证: 源全局最大值格点(17.46 @ -111.265,11.187)落进缓存 16.98, 0.0081 基线各采样点精确命中。诊断: `diag_geoloc --transformprobe <repair.nc>` 会打印正/反算; 保留 repair 文件用环境变量 `PEEK_KEEP_REPAIR=1`。
- `stretchNodata` 会把 warp 后边界 NoData 糊成数据最小值 -> 缓存 TIFF 里可能没有剩余 nodata 值(全数组为有限值), 比对时勿用 `np.isnan` 当"空洞"判据。