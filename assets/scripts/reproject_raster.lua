-- ===== 重投影(栅格) =====
toolbox.def.label = "重投影(栅格)"
toolbox.def.description = [[把栅格数据集重投影到目标坐标系(gdal:raster:reproject)。]]
toolbox.def.tool = "gdal:raster:reproject"

toolbox.input("input", { title="输入栅格", required=true })
toolbox.param("dst-crs", "EPSG:4326", { title="目标坐标系 (如 EPSG:3857)" })
toolbox.param("resampling", "nearest", { title="重采样方法" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出栅格 (.tif)", load=true })