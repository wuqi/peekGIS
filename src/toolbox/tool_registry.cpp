#include "toolbox/tool_registry.h"

#include "platform/exe_path.h"
#include "platform/path_util.h"
#include "toolbox/gdal_proc.h"
#include "util/logger.h"

#include <cpl_json.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// 默认 gdal 可执行文件路径: exe 旁同名(随产品部署), 否则回退 PATH 上的 gdal。
std::string defaultGdalExe() {
#ifdef _WIN32
    std::string p = exeDir() + "/gdal.exe";
#else
    std::string p = exeDir() + "/gdal";
#endif
    if (fs::exists(toFsPath(p))) return p;
    return "gdal";
}

void splitComma(const std::string& s, std::vector<std::string>* out) {
    size_t start = 0;
    while (start <= s.size()) {
        size_t c = s.find(',', start);
        if (c == std::string::npos) c = s.size();
        std::string tok = s.substr(start, c - start);
        if (!tok.empty()) out->push_back(tok);
        if (c == s.size()) break;
        start = c + 1;
    }
}

bool arrContains(const CPLJSONArray& a, const std::string& want) {
    for (const auto& o : a) {
        if (o.GetType() == CPLJSONObject::Type::String && o.ToString("") == want) return true;
    }
    return false;
}

// 解析一个参数定义节点。
ArgDef parseArgDef(const CPLJSONObject& o) {
    ArgDef a;
    a.name = o["name"].ToString("");
    a.type = o["type"].ToString("");
    a.required = o["required"].ToBool(false);
    a.description = o["description"].ToString("");
    a.category = o["category"].ToString("");
    a.metavar = o["metavar"].ToString("");
    a.packedValuesAllowed = o["packed_values_allowed"].ToBool(false);
    a.repeatedArgAllowed = o["repeated_arg_allowed"].ToBool(false);

    CPLJSONObject jdef = o["default"];
    if (jdef.GetType() != CPLJSONObject::Type::Null && jdef.IsValid()) {
        a.hasDefault = true;
        switch (jdef.GetType()) {
            case CPLJSONObject::Type::Boolean: a.defaultValue = jdef.ToBool(false) ? "true" : "false"; break;
            case CPLJSONObject::Type::Integer: a.defaultValue = std::to_string(jdef.ToInteger(0)); break;
            case CPLJSONObject::Type::Long:    a.defaultValue = std::to_string(jdef.ToLong(0)); break;
            case CPLJSONObject::Type::Double: {
                double d = jdef.ToDouble(0);
                if (d == (long long)d) a.defaultValue = std::to_string((long long)d);
                else { char buf[32]; std::snprintf(buf, sizeof(buf), "%.10g", d); a.defaultValue = buf; }
                break;
            }
            default: a.defaultValue = jdef.ToString("");
        }
    }
    a.minCount = o["min_count"].ToLong(0);
    a.maxCount = o["max_count"].ToLong(0);
    CPLJSONObject jmin = o["min_value"];
    if (jmin.IsValid() && jmin.GetType() != CPLJSONObject::Type::Null) {
        a.hasMinValue = true;
        a.minValue = jmin.ToDouble(0);
        a.minValueIncluded = o["min_value_is_included"].ToBool(false);
    }
    CPLJSONArray jchoices = o["choices"].ToArray();
    if (jchoices.Size() > 0) {
        for (const auto& c : jchoices) {
            // value 可为字符串, 也可为 {value,description} 对象
            if (c.GetType() == CPLJSONObject::Type::String) a.choices.push_back(c.ToString(""));
            else {
                std::string v = c["value"].ToString("");
                if (!v.empty()) a.choices.push_back(v);
            }
        }
    }
    CPLJSONArray jdt = o["dataset_type"].ToArray();
    if (jdt.Size() > 0) a.datasetType = jdt[0].ToString("");
    a.isInput = arrContains(o["input_flags"].ToArray(), "dataset");
    a.isOutput = arrContains(o["output_flags"].ToArray(), "dataset");
    return a;
}

std::string boolText(const std::string& v) {
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "on") return "true";
    return "false";
}

}  // namespace

bool ToolRegistry::ensureLoaded(const std::string& gdalExe) {
    if (ready()) return true;
    m_gdalExe = gdalExe.empty() ? defaultGdalExe() : gdalExe;
    m_cachePath = exeDir() + "/cache/toolbox_index.json";
    if (loadCache(m_gdalExe)) return true;

    std::string json, stderrOut;
    int code = 0;
    std::string err;
    if (!GdalCli::run(m_gdalExe, { "--json-usage" }, &json, &stderrOut, &code)) {
        m_error = "无法启动 gdal: " + m_gdalExe + (stderrOut.empty() ? "" : " (" + stderrOut + ")");
        spdlog::error("toolbox: {}", m_error);
        return false;
    }
    if (code != 0 || json.empty()) {
        m_error = "gdal --json-usage 退出码 " + std::to_string(code) + (stderrOut.empty() ? "" : ": " + stderrOut);
        spdlog::error("toolbox: {}", m_error);
        return false;
    }
    // 丢弃 stdout 里 --json-usage 之前的任何非 JSON 尾行后的片段: 直接按首字符判断
    if (json[0] != '{') {
        size_t pos = json.find('{');
        if (pos == std::string::npos) { m_error = "gdal --json-usage 输出无 JSON"; return false; }
        json = json.substr(pos);
    }
    if (!parseJson(json, &err)) {
        m_error = "工具树解析失败: " + err;
        spdlog::error("toolbox: {}", m_error);
        return false;
    }
    // 写缓存(原样 JSON 文本, 下次启动免 spawn)
    try {
        fs::create_directories(toFsPath(exeDir() + "/cache"));
        std::ofstream ofs(toFsPath(exeDir() + "/cache/toolbox_index.json"),
                          std::ios::binary | std::ios::trunc);
        ofs.write(json.data(), (std::streamsize)json.size());
    } catch (...) {
        // 缓存失败不影响工具树可用
    }
    return true;
}

bool ToolRegistry::loadCache(const std::string& gdalExe) {
    if (!fs::exists(toFsPath(m_cachePath))) return false;
    // gdal 比缓存新时视为失效
    try {
        auto cacheT = fs::last_write_time(toFsPath(m_cachePath));
        auto gdalT = fs::last_write_time(toFsPath(gdalExe));
        if (gdalT > cacheT) return false;
    } catch (...) {
        return false;
    }
    std::string json;
    try {
        std::ifstream ifs(toFsPath(m_cachePath), std::ios::binary);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        json = ss.str();
    } catch (...) {
        return false;
    }
    if (json.empty()) return false;
    std::string err;
    if (!parseJson(json, &err)) {
        spdlog::warn("toolbox: 缓存解析失败({}), 重新导入", err);
        return false;
    }
    return true;
}

bool ToolRegistry::parseJson(const std::string& json, std::string* err) {
    CPLJSONDocument doc;
    if (!doc.LoadMemory(json)) { if (err) *err = "invalid json"; return false; }
    CPLJSONObject root = doc.GetRoot();
    if (root.GetType() != CPLJSONObject::Type::Object) { if (err) *err = "root not object"; return false; }

    m_tools.clear();
    m_leafCache.clear();

    // 就地构造出整棵子树(vector 保证右顺序: 先父后子, 便于保留父引用)。
    std::vector<ToolDef> sub;
    std::function<std::size_t(const CPLJSONObject&, const std::string&, const std::string&)> rec =
        [&](const CPLJSONObject& o, const std::string& name, const std::string& fullPath) -> std::size_t {
        std::size_t myIdx = sub.size();
        sub.push_back(ToolDef{});
        sub[myIdx].name = name;
        sub[myIdx].fullPath = fullPath;
        sub[myIdx].description = o["description"].ToString("");
        sub[myIdx].url = o["url"].ToString("");
        sub[myIdx].parent = -1;

        // 参数: 输入 + 输入输出 + 输出(保持顺序, 位置参数顺序由它决定)
        for (const char* key : { "input_arguments", "input_output_arguments", "output_arguments" }) {
            CPLJSONArray arr = o[key].ToArray();
            if (arr.Size() > 0) {
                for (const auto& a : arr) sub[myIdx].args.push_back(parseArgDef(a));
            }
        }

        bool hasChildren = false;
        CPLJSONArray subs = o["sub_algorithms"].ToArray();
        if (subs.Size() > 0) {
            for (const auto& c : subs) {
                if (c.GetType() != CPLJSONObject::Type::Object) continue;
                std::string cname = c["name"].ToString("");
                if (cname.empty()) continue;
                hasChildren = true;
                // full_path 是绝对路径数组(含祖先)
                CPLJSONArray fp = c["full_path"].ToArray();
                std::string cpath = fullPath;
                if (cpath.empty()) cpath = "gdal";
                if (fp.Size() > 0) {
                    cpath.clear();
                    for (int i = 0; i < fp.Size(); i++) {
                        if (i > 0) cpath += ":";
                        cpath += fp[i].ToString("");
                    }
                } else {
                    cpath += ":" + cname;
                }
                std::size_t childIdx = rec(c, cname, cpath);
                sub[childIdx].parent = (int)myIdx;
                sub[myIdx].children.push_back((int)childIdx);
            }
        }
        sub[myIdx].isLeaf = !hasChildren;
        return myIdx;
    };
    rec(root, "gdal", "gdal");

    if (sub.size() <= 1) { if (err) *err = "empty tree"; return false; }
    m_tools = std::move(sub);
    return true;
}

const ToolDef* ToolRegistry::find(const std::string& fullPath) const {
    for (const auto& t : m_tools) {
        if (t.fullPath == fullPath) return &t;
    }
    return nullptr;
}

std::vector<const ToolDef*> ToolRegistry::leaves() const {
    if (!m_leafCache.empty()) return m_leafCache;
    for (const auto& t : m_tools) {
        if (t.isLeaf) m_leafCache.push_back(&t);
    }
    return m_leafCache;
}

bool ToolRegistry::buildArgs(const ToolDef& tool,
                             const std::map<std::string, std::string>& opts,
                             std::vector<std::string>* args,
                             std::string* err) const {
    if (err) err->clear();
    args->clear();
    for (const ArgDef& a : tool.args) {
        auto it = opts.find(a.name);
        const bool provided = (it != opts.end()) && !it->second.empty();
        if (a.isInput || a.isOutput) {
            if (provided) {
                std::vector<std::string> parts;
                splitComma(it->second, &parts);
                for (const std::string& p : parts) args->push_back(p);
            } else if (a.required) {
                if (err) *err = "缺少必填数据集: " + a.name;
                args->clear();
                return false;
            }
            continue;
        }
        if (!provided) continue;
        const std::string& v = it->second;
        if (a.type == "boolean") {
            if (boolText(v) == "true") args->push_back("--" + a.name);
            continue;
        }
        bool isList = a.type.size() > 5 && a.type.compare(a.type.size() - 5, 5, "_list") == 0;
        if (isList && !a.packedValuesAllowed) {
            std::vector<std::string> parts;
            splitComma(v, &parts);
            for (const std::string& p : parts) args->push_back("--" + a.name + "=" + p);
        } else {
            args->push_back("--" + a.name + "=" + v);
        }
    }
    return true;
}

bool ToolRegistry::runArgs(const ToolDef& tool,
                           const std::map<std::string, std::string>& opts,
                           std::vector<std::string>* argv,
                           std::string* err) const {
    if (!buildArgs(tool, opts, argv, err)) return false;
    // full_path 冒号拆段; 首段恒为 "gdal"(即程序名自身), 丢弃, 余下为
    // 命令段序列 (gdal raster reproject <args...>)。
    std::vector<std::string> segs;
    size_t start = 0;
    while (true) {
        size_t c = tool.fullPath.find(':', start);
        if (c == std::string::npos) { segs.push_back(tool.fullPath.substr(start)); break; }
        segs.push_back(tool.fullPath.substr(start, c - start));
        start = c + 1;
    }
    if (!segs.empty()) segs.erase(segs.begin());   // 去掉 "gdal"
    argv->insert(argv->begin(), segs.begin(), segs.end());
    return true;
}