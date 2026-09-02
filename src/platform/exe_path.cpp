#include "platform/exe_path.h"

#ifdef _WIN32
#include <windows.h>

std::string exeDir() {
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string() : s.substr(0, p);
}

#else
#include <unistd.h>
#include <limits.h>

std::string exeDir() {
    char buf[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string s(buf, n > 0 ? n : 0);
    size_t p = s.find_last_of('/');
    return (p == std::string::npos) ? std::string() : s.substr(0, p);
}

#endif
