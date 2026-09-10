-- ===== 缓冲区(矢量) =====
toolbox.def.label = "缓冲区(矢量)"
toolbox.def.description = [[对矢量要素创建缓冲区(gdal:vector:buffer)。距离单位为要素坐标系单位。]]
toolbox.def.tool = "gdal:vector:buffer"

toolbox.input("input", { title="输入矢量", required=true })
toolbox.param("distance", "100", { title="缓冲距离 (必填)", required=true })
toolbox.param("endcap-style", "round", { title="端帽样式 round/flat/square" })
toolbox.param("join-style", "round", { title="拐角样式 round/miter/bevel" })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出矢量 (.gpkg)", load=true })