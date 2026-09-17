-- ===== 栅格转换(gdal raster convert) =====
toolbox.def.label = "栅格转换 (convert)"
toolbox.def.description = [[栅格格式转换/复制(gdal:raster:convert), 相当于 gdal_translate。]]
toolbox.def.tool = "gdal:raster:convert"

toolbox.input("input", { title="输入栅格", required=true })
toolbox.param("output-format", "GTiff", { title="输出格式驱动名 (如 GTiff)" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出栅格 (.tif)", load=true })