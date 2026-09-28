#pragma once
// 测试用的仓库内临时目录。
//
// 为什么不直接用 __FILE__ 拼 "build/tmp": __FILE__ 在部分构建下是相对路径,
// absolute() 会按当前工作目录解析, 于是从 bin\ 启动(launcher 的 .vbs 会把 CWD
// 设成 exe 目录)就变成 bin\build\tmp, 把测试产物写进发布目录。
// 这里改成"先按 __FILE__ 定位仓库根, 定位不到再从 exe 目录向上找 xmake.lua"。
//
// AGENTS: 禁止把测试输出写到 C 盘(含 C:\tmp), 一律放仓库内 build\tmp。
#include <filesystem>
#include <string>

namespace peekg::test {

inline std::filesystem::path repoRoot() {
    std::error_code ec;
    // 1) __FILE__ 所在目录的上级就是仓库根(tests/ 的父目录)
    auto p = std::filesystem::absolute(std::filesystem::path(__FILE__), ec);
    if (!ec) {
        auto root = p.parent_path().parent_path();
        if (std::filesystem::exists(root / "xmake.lua")) return root;
    }
    // 2) 回退: 从当前工作目录向上找 xmake.lua
    auto cwd = std::filesystem::current_path(ec);
    for (auto d = cwd; !d.empty(); d = d.parent_path()) {
        if (std::filesystem::exists(d / "xmake.lua")) return d;
    }
    return cwd;
}

// 仓库内 build/tmp 下的一个路径, 父目录已创建
inline std::string tmpPath(const char* name) {
    auto d = repoRoot() / "build" / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return (d / name).string();
}

// 仓库内 build/tmp 下的一个目录(用于缓存类测试), 已创建
inline std::string tmpDir(const char* name) {
    auto d = repoRoot() / "build" / "tmp" / name;
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d.string();
}

}  // namespace peekg::test
