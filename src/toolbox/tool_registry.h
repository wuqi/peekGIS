#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

// 工具参数定义(来自 gdal --json-usage 的参数节点, 精简到表单/命令行所需的字段)。
struct ArgDef {
    std::string name;
    std::string type;              // boolean/string/integer/real/.../dataset/dataset_list
    bool required = false;
    std::string description;
    std::string category;          // Basic / Advanced
    std::string defaultValue;      // 默认值文本(布尔为 true/false)
    bool hasDefault = false;
    std::vector<std::string> choices;
    long long minCount = 0, maxCount = 0;   // 0 表示未限定
    double minValue = 0;
    bool hasMinValue = false;
    bool minValueIncluded = false;
    std::string metavar;
    std::string datasetType;       // dataset_type[0] (raster/vector/mdim/...)
    bool isInput = false;          // input_flags 含 "dataset"
    bool isOutput = false;         // output_flags 含 "dataset"
    bool packedValuesAllowed = true;  // 列表可逗号打包(--name=a,b)
    bool repeatedArgAllowed = false;  // 允许重复 --name= 传多值
};

// 工具节点: 目录节点无参数, 叶子节点(无子算法)即可执行工具。
struct ToolDef {
    std::string name;              // 本层短名 (如 "reproject")
    std::string fullPath;          // 冒号拼接路径 (如 "gdal:raster:reproject")
    std::string description;
    std::string url;
    std::vector<ArgDef> args;      // 叶子请假参数(含输入/输出数据集)
    std::vector<int> children;     // 子节点索引 (小根树, 子索引 > 父索引)
    int parent = -1;
    bool isLeaf = false;
};

// 工具目录: 从 gdal.exe --json-usage 一次性导入全量工具树。
// 导入结果按原始 JSON 文本缓存到 exe 旁 cache/toolbox_index.json, 启动零扫描
// (除非 gdal.exe mtime 更新)。
class ToolRegistry {
public:
    // gdalExe: gdal.exe 路径; 空则自动推导(exe 旁 / PATH)。
    // 返回 true 表示工具树可用。失败时错误写入 lastError()。
    bool ensureLoaded(const std::string& gdalExe = {});

    bool ready() const { return !m_tools.empty(); }
    const std::vector<ToolDef>& tools() const { return m_tools; }
    const ToolDef* find(const std::string& fullPath) const;
    std::vector<const ToolDef*> leaves() const;
    const std::string& lastError() const { return m_error; }
    const std::string& cachePath() const { return m_cachePath; }
    std::string gdalExe() const { return m_gdalExe; }

    // 把表单值组装成 gdal 子命令参数(--name=value / 布尔 --flag / 数据集按顺序位置参数)。
    // opts: 参数名 -> 值(空=不传)。数据集参数值=文件路径(逗号分隔多值)。
    // 返回 false 表示必填数据集缺值。
    bool buildArgs(const ToolDef& tool, const std::map<std::string, std::string>& opts,
                   std::vector<std::string>* args, std::string* err) const;

    // buildArgs + 前置命令段(full_path 冒号拆段, 去掉根 "gdal"):
    // 结果可直接喂 GdalCli::run(Linux: 整体 argv, Windows: 内部再整串拼接)。
    bool runArgs(const ToolDef& tool, const std::map<std::string, std::string>& opts,
                 std::vector<std::string>* argv, std::string* err) const;

private:
    bool importFromGdal(const std::string& gdalExe);
    bool loadCache(const std::string& gdalExe);
    bool parseJson(const std::string& json, std::string* err);

    std::vector<ToolDef> m_tools;
    mutable std::vector<const ToolDef*> m_leafCache;
    std::string m_error;
    std::string m_gdalExe;
    std::string m_cachePath;
};