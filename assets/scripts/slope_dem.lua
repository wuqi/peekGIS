-- ===== 坡度(DEM) =====
toolbox.def.label = "坡度 (DEM)"
toolbox.def.description = [[从高程栅格生成坡度(gdal:raster:slope)。]]
toolbox.def.tool = "gdal:raster:slope"

toolbox.input("input", { title="输入 DEM 栅格", required=true })
toolbox.param("unit", "degrees", { title="单位 degrees/percent" })
toolbox.param("band", "1", { title="高程波段" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出坡度栅格 (.tif)", load=true })