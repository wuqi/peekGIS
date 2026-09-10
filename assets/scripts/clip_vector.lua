-- ===== 裁剪(矢量) =====
toolbox.def.label = "裁剪(矢量)"
toolbox.def.description = [[按边界框裁剪矢量数据(gdal:vector:clip)。]]
toolbox.def.tool = "gdal:vector:clip"

toolbox.input("input", { title="输入矢量", required=true })
toolbox.param("bbox", "", { title="裁剪范围 minx,miny,maxx,maxy (留空=整体复制)" })
toolbox.param("bbox-crs", "EPSG:4326", { title="bbox 坐标系" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出矢量 (.gpkg)", load=true })