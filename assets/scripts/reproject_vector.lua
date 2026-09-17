-- ===== 重投影(矢量) =====
toolbox.def.label = "重投影(矢量)"
toolbox.def.description = [[把矢量数据集重投影到目标坐标系(gdal:vector:reproject)。]]
toolbox.def.tool = "gdal:vector:reproject"

toolbox.input("input", { title="输入矢量", required=true })
toolbox.param("dst-crs", "EPSG:4326", { title="目标坐标系 (必填, 如 EPSG:3857)", required=true })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出矢量 (.gpkg)", load=true })