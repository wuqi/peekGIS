#include "doctest.h"
#include "platform/path_util.h"
#include <filesystem>

TEST_CASE("toFsPath: ASCII 路径保持") {
    auto p = toFsPath("a/b/c.shp");
    CHECK(p == std::filesystem::path("a/b/c.shp"));
}

TEST_CASE("toFsPath: UTF-8 中文路径往返一致") {
    // 关键修复: 中文(等 UTF-8)路径经 wide 转换后内容不丢失, GDAL/缓存才能打开
    std::string utf8 = "中文目录/测试文件.shp";
    auto p = toFsPath(utf8);
#ifdef _WIN32
    std::u8string u8 = p.u8string();
    std::string got(u8.begin(), u8.end());
    CHECK(got == utf8);
#else
    CHECK(p.string() == utf8);
#endif
}
