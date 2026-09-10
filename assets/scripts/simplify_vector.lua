-- ===== 简化(矢量) =====
toolbox.def.label = "简化(矢量)"
toolbox.def.description = [[按容差简化矢量几何(gdal:vector:simplify)。]]
toolbox.def.tool = "gdal:vector:simplify"

toolbox.input("input", { title="输入矢量", required=true })
toolbox.param("tolerance", "0.0001", { title="简化容差 (必填)", required=true })
toolbox.param("overwrite", "true", { title="已存在时覆盖" })
toolbox.output("output", { title="输出矢量 (.gpkg)", load=true })