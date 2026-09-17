#pragma once
#include <string>
#include <vector>

// 运行 gdal.exe 子命令(统一 gdal 3.12 CLI)并捕获 stdout/stderr/退出码。
// args 不含 exe 本身; 内部处理 UTF-8 路径编码与命令行引号转义。
class GdalCli {
public:
    // 返回 false 表示进程无法启动(路径/权限错误); 正常结束则返回 true 且 exitCode 反映结果。
    static bool run(const std::string& exePath,
                    const std::vector<std::string>& args,
                    std::string* stdoutOut,
                    std::string* stderrOut,
                    int* exitCode);

private:
    // Windows CreateProcess 命令行转义: 含空格/引号才加双引号,
    // 内部引号与反斜杠按 MSVCRT 规则转义(\ -> \\, " -> \" 且转义前导反斜杠)。
    static std::string quoteArg(const std::string& a);
};