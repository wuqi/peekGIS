-- ===== 栅格转矢量(polygonize) =====
toolbox.def.label = "栅格转矢量 (Polygonize)"
toolbox.def.description = [[把栅格波段连片值转为面要素(gdal:raster:polygonize)。]]
toolbox.def.tool = "gdal:raster:polygonize"

toolbox.input("input", { title="输入栅格", required=true })
toolbox.param("band", "1", { title="转化波段" })
toolbox.param("attribute-name", "DN", { title="值字段名" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出矢量 (.gpkg)", load=true })