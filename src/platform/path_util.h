#pragma once
#include <string>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// 文件名(去目录、去扩展名): "a/b/c.shp" -> "c"。视图/图层面板显示用。
inline std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    std::string f = (s == std::string::npos) ? p : p.substr(s + 1);
    size_t d = f.find_last_of('.');
    return (d == std::string::npos) ? f : f.substr(0, d);
}

// UTF-8 路径 -> 平台原生 fs::path (Windows 下转 wide, 否则直接用 UTF-8)
inline std::filesystem::path toFsPath(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(utf8);
#endif
}
