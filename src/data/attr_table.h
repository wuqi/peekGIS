#pragma once
#include <string>
#include <vector>
#include "data/encoding.h"

// 底部属性表: 按页从源文件读取属性 + 几何(支持跨 GDAL 格式分页), 编码可实时切换。
// 设计见 docs/属性表设计.md。

namespace peekg::data {
struct AttrCell {
    std::vector<unsigned char> rawName;  // 字段名原始字节(未转码)
    std::string name;                    // 字段名(UTF-8, 按所选编码转码)
    std::vector<unsigned char> raw;      // 字符串字段原始字节(未转码); 非字符串为空
    std::string text;                    // 显示文本(UTF-8)
    bool isString = false;               // 是否字符串字段(需按编码转码)
    bool isNull = false;
};

struct AttrRow {
    long long fid = 0;
    std::vector<AttrCell> cells;
    std::vector<float> outline;   // 描边线段(源 CRS)
    std::vector<float> points;    // 点(源 CRS)
    std::vector<float> fillTris;  // 面填充三角形(源 CRS)
    bool hasGeom = false;
};

struct AttrPageData {
    int page = 0;
    std::vector<AttrRow> rows;
    long long firstFid = -1;      // 页首 FID(调试/定位用)
};

struct AttrFieldDef {
    std::vector<unsigned char> rawName;
    int width = 0;
    int precision = 0;
    bool isString = false;
};

struct AttrLayerInfo {
    bool ok = false;
    std::string layerName;
    std::string driverName;       // "ESRI Shapefile" 等(判可否切编码)
    bool canEncode = true;        // DBF 可切编码; 其他(pgkg 等)UTF-8 直通
    long long total = 0;
    std::vector<AttrFieldDef> fields;
};

// 打开图层: 读取字段定义 + 驱动名 + 图层名(快)。不读具体要素、不取总数。
// 成功返回 true, 并把 info.total 置为 -1(总数未取, 需另调 attrFeatureCount)。
bool attrOpenLayer(const std::string& path, int layerIdx, AttrLayerInfo& info);

// 只取图层要素总数(可能较慢, 供打开后异步补全)。失败/未知返回 -1。
long long attrFeatureCount(const std::string& path, int layerIdx);

// 拉取第 page 页(0 基), rowsPerPage 行。优先 FID 直达, 无随机读能力回退顺序跳过。
// info 提供字段定义; 字符串值按 enc 解码。成功返回 true。
bool attrFetchPage(const std::string& path, int layerIdx, int page, int rowsPerPage,
                   TextEncoding enc, const AttrLayerInfo& info, AttrPageData& out);

// 仅重转已缓存页(不重读文件): 把每格字符串字段/字段名按新编码重转。
void attrReencodePage(AttrPageData& p, TextEncoding enc);

// 按 FID 回源查询的结果分类
enum class AttrQueryResult {
    Found,      // 命中
    NotFound,   // 该 FID 无此要素(FID 空洞/越界, OGR 返回 null)
    Error,      // 图层打不开/字段定义无效/私有连接失败
};

// 按 FID 回源取**单个**要素的属性 + 几何。OGR_L_GetFeature 直接按 FID 定位, 不做全表扫描,
// 通常毫秒级(与 featureWkt 同一路径)。info 提供字段定义(可复用属性表已打开的 info, 不必重开图层)。
// 几何与属性表分页读一样是源 CRS, 由调用方按需重投影。
// 注意: 走共享 keeper 句柄(gdalKeeperEnsure), 与属性表分页读同一把 per-path 锁。
// 将来若要加"按属性条件查询", 必须改走私有连接(像 attrFeatureCount 那样),
// 否则 SetAttributeFilter 会污染属性表正在用的图层句柄。
AttrQueryResult attrFetchByFid(const std::string& path, int layerIdx, long long fid,
                               TextEncoding enc, const AttrLayerInfo& info, AttrRow& out);

}  // namespace peekg::data
