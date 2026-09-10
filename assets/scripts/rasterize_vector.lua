-- ===== 栅格化(矢量→栅格) =====
toolbox.def.label = "栅格化(矢量→栅格)"
toolbox.def.description = [[把矢量要素烧录进栅格(gdal:vector:rasterize)。]]
toolbox.def.tool = "gdal:vector:rasterize"

toolbox.input("input", { title="输入矢量", required=true })
toolbox.param("attribute-name", "", { title="按属性值烧录的字段 (留空=固定值)" })
toolbox.param("burn", "1", { title="固定烧录值" })
toolbox.param("resolution", "100,100", { title="目标分辨率 xres,yres" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出栅格 (.tif)", load=true })