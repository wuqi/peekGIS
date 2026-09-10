-- ===== 山体阴影(DEM) =====
toolbox.def.label = "山体阴影 (DEM)"
toolbox.def.description = [[从高程栅格生成山体阴影(gdal:raster:hillshade)。]]
toolbox.def.tool = "gdal:raster:hillshade"

toolbox.input("input", { title="输入 DEM 栅格", required=true })
toolbox.param("azimuth", "315", { title="太阳方位角 (0-360)" })
toolbox.param("altitude", "45", { title="太阳高度角 (0-90)" })
toolbox.param("zfactor", "1", { title="垂直夸张系数" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出山影栅格 (.tif)", load=true })