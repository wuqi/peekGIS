// 压缩包里的矢量数据源解析。
//
// GDAL 只把"xxx.shp.zip / xxx.gpkg.zip / xxx.gdb.zip"这类**复合扩展名**登记成压缩包,
// 裸 "xxx.zip" 直接喂给 GDAL 会报 "not recognized"。而"把整个 shapefile 打包成 data.zip"
// 是最常见的交付形态, 所以这里补一层: 直接打开失败且路径像 zip 时, 退回 /vsizip/ 挂载,
// 在里面按优先级找一个能打开的矢量数据集。
//
// 返回的路径(形如 /vsizip/I:/data.zip/data.shp)本身就能喂给 GDAL, 因此可以直接当
// sourcePath 用(属性表/识别/要素查询/v2 建缓存都靠它)。注意它是**虚拟路径**:
// 属性表里的"打开文件"之类按磁盘路径处理的逻辑对它不适用(目前没有这类调用)。
//
// 单独成文件而不是放进 vector_reader.cpp: vt_build / vt_estimate 这两个命令行工具只链
// gdal_common 一侧, 不链整个 vector_reader(那会拖进 raster/geoloc/attr 等一串依赖)。
#include "data/gdal_common.h"

#include <cctype>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <string>

namespace peekg::data {

namespace {
std::string lowerExtOf(const std::string& s) {
    std::string e = s;
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}
bool hasZipExt(const std::string& s) {
    const std::string l = lowerExtOf(s);
    return l.size() >= 4 && l.compare(l.size() - 4, 4, ".zip") == 0;
}
}  // namespace

bool resolveVectorSourcePath(const std::string& path, std::string& out) {
    // 能直接开(含 /vsizip/ 这类前缀)就原样返回: 这是最常见的情况, 不做任何额外工作
    {
        GDALDatasetH ds = gdalOpenVector(path);
        if (ds) {
            GDALClose(ds);
            out = path;
            return true;
        }
    }
    if (!hasZipExt(path)) return false;
    if (path.rfind("/vsizip/", 0) == 0) return false;   // 已是 vsizip 的失败了就别再套一层

    ensureGdal();
    VSIStatBufL st;
    if (VSIStatExL(path.c_str(), &st, VSI_STAT_EXISTS_FLAG) != 0) return false;
    // /vsizip/ 后面必须是正斜杠路径: 把 Windows 反斜杠统一成 "/", 免得拼出混合写法
    std::string zipPath = path;
    for (char& c : zipPath) if (c == '\\') c = '/';
    const std::string zipDir = "/vsizip/" + zipPath + "/";

    char** kids = VSIReadDirRecursive(zipDir.c_str());
    if (!kids) return false;
    int bestScore = -1;
    std::string best;
    for (char** k = kids; *k; ++k) {
        const std::string full = zipDir + *k;
        const std::string lk = lowerExtOf(full);
        int score = -1;
        if (lk.size() >= 4) {
            const std::string e = lk.substr(lk.size() - 4);
            if (e == ".shp") {
                score = 100;
                // 有 .shx 才是完整 shapefile(按 FID 随机读全靠它)
                const std::string shx = full.substr(0, full.size() - 4) + ".shx";
                VSIStatBufL st2;
                if (VSIStatExL(shx.c_str(), &st2, VSI_STAT_EXISTS_FLAG) == 0) score += 20;
            } else if (e == ".gpkg" || e == ".gdb" || e == ".sqlite") {
                score = 90;
            }
        }
        if (score > bestScore) { bestScore = score; best = full; }
    }
    CSLDestroy(kids);
    if (best.empty() || bestScore < 0) return false;

    // 最终确认: 这个候选真的能开吗(避免选到 .dbf/.xml 之类打不开的成员)
    GDALDatasetH ds = gdalOpenVector(best);
    if (!ds) return false;
    GDALClose(ds);
    out = best;
    return true;
}

}  // namespace peekg::data