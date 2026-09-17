# 测试数据目录 (tests/testdata)

单元测试所需的**可选**数据文件。缺省时相关测试会自动跳过(打印 MESSAGE), 不影响其余测试。

## 目录结构

```
tests/testdata/
├── gdal_data/          ← raster 测试数据 (GDAL autotest gcore 子集)
│   ├── byte.tif        20×20 uint8, 带 geotransform (读窗口/灰度/伪彩色用例)
│   ├── rgbsmall.tif    3 波段小图 (RGB 组装用例)
│   └── float64.tif     double 波段 (精度用例)
├── s55/                ← SRS 诊断栅格 (diag_srs 工具)
│   └── S55_12.img      IMAGINE 影像, UTM Zone 55S (即"下S55_12.img")
├── poly.shp            ← 矢量几何用例 (面要素, 含属性表)
│   ├── poly.shx
│   ├── poly.dbf
│   └── poly.prj
└── aanp.shp            ← SRS 诊断矢量 (diag_srs 工具, 公开版 25 万新疆)
    ├── aanp.shx
    ├── aanp.dbf
    └── aanp.prj        ← ArcGIS 私有命名 (测试 gdalSrsEpsg 启发式兜底)
```

> shapefile 是分体文件, poly / aanp 需要 `.shp/.shx/.dbf/.prj` 全套放 `tests/testdata/` 下。
> (后继若把 `.shp` 等加回跟踪, 记得去掉 `.gitignore` 里的对应规则。)

## 怎么获取

### 方式一: 环境变量(推荐, 不拷贝文件)

测试启动时依次查找以下来源, 命中即用:

| 环境变量 | 用途 |
|---|---|
| `PEEKGIS_TEST_GDAL_DATA` | raster 测试: 指向含 `byte.tif` 的目录 |
| `PEEKGIS_TEST_POLY` | 矢量测试: 指向 `poly.shp` 完整路径 |
| `PEEKGIS_TEST_S55_IMG` | diag_srs: 指向 `S55_12.img` 完整路径 |
| `PEEKGIS_TEST_AANP` | diag_srs: 指向 `aanp.shp` 完整路径 |

例:
```powershell
$env:PEEKGIS_TEST_GDAL_DATA = "D:\somewhere\gcore\data"
$env:PEEKGIS_TEST_POLY      = "D:\somewhere\poly.shp"
$env:PEEKGIS_TEST_S55_IMG   = "D:\somewhere\下S55_12.img"
$env:PEEKGIS_TEST_AANP      = "D:\somewhere\aanp.shp"
build\windows\x64\release\tests.exe
build\windows\x64\release\diag_srs.exe
```

### 方式二: 放本目录(可移植)

把文件复制到上面列出的位置, 不设环境变量也会被相对路径找到。

### 数据来源

推荐从 GDAL 官方 autotest 目录取(随 vcpkg 的 GDAL 一起, 或克隆 GDAL 仓库):

- raster: `autotest/gcore/data/byte.tif`, `rgbsmall.tif`, `float64.tif`
- 矢量:  `autotest/ogr/data/poly.shp` (+ shx/dbf/prj)

diag_srs 数据没有公开镜像, 从已有机器/硬盘拷贝即可:
- 栅格: `G:/s55/下S55_12.img` (IMAGINE, UTM Zone 55S)
- 矢量: `G:/cp/公开版25万-新疆shp/aanp.shp` (+ shx/dbf/prj)

## 查找顺序

`tests/test_raster.cpp` 的 `locateGdalData()`:
1. 环境变量 `PEEKGIS_TEST_GDAL_DATA`
2. 相对目录 `tests/testdata/gdal_data` / `testdata/gdal_data` / `share/testdata/gdal_data`
3. 绝对兜底(本机 `D:/Github/HeadFirstGDAL/...`)

`tests/test_geom_util.cpp` 的 `locatePolyShp()`:
1. 环境变量 `PEEKGIS_TEST_POLY`
2. 相对目录 `tests/testdata/poly.shp` / `testdata/poly.shp`
3. 已知候选绝对路径

`tools/diag_srs.cpp` 的 `locateFile()` (栅格和矢量同一套逻辑):
1. 环境变量 `PEEKGIS_TEST_S55_IMG` / `PEEKGIS_TEST_AANP`
2. 相对目录 `tests/testdata/s55/S55_12.img` / `tests/testdata/aanp.shp`
3. 绝对兜底(如 G 盘原路径)

未找到时工具打印 `跳过: 未找到 ...` 并正常返回, 不视为失败。
