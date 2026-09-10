#include "toolbox/gdal_proc.h"

#include "platform/path_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#else
#include <array>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

std::string GdalCli::quoteArg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    size_t bs = 0;
    for (char c : a) {
        if (c == '\\') { bs++; continue; }
        if (c == '"') {
            r.append(bs * 2 + 1, '\\');
            r += '"';
            bs = 0;
        } else {
            r.append(bs, '\\');
            bs = 0;
            r += c;
        }
    }
    r.append(bs * 2, '\\');
    r += '"';
    return r;
}

#ifdef _WIN32
bool GdalCli::run(const std::string& exePath,
                const std::vector<std::string>& args,
                std::string* stdoutOut,
                std::string* stderrOut,
                int* exitCode) {
    if (stdoutOut) stdoutOut->clear();
    if (stderrOut) stderrOut->clear();
    if (exitCode) *exitCode = -1;

    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    HANDLE hErrRead = nullptr, hErrWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0) || !CreatePipe(&hErrRead, &hErrWrite, &sa, 0)) {
        if (hOutRead) CloseHandle(hOutRead);
        if (hOutWrite) CloseHandle(hOutWrite);
        return false;
    }
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring app = toFsPath(exePath).wstring();
    std::wstring cl;
    for (const auto& a : args) {
        if (!cl.empty()) cl += L" ";
        cl += toFsPath(quoteArg(a)).wstring();
    }
    cl = L"\"" + app + L"\"" + (cl.empty() ? L"" : L" " + cl);

    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutWrite;
    si.hStdError = hErrWrite;

    bool ok = CreateProcessW(app.c_str(), &cl[0], nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOutWrite);
    CloseHandle(hErrWrite);
    if (!ok) {
        CloseHandle(hOutRead);
        CloseHandle(hErrRead);
        return false;
    }

    std::string outBuf, errBuf;
    std::thread tOut([&] {
        char buf[65536];
        DWORD got = 0;
        while (ReadFile(hOutRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
            outBuf.append(buf, got);
        }
    });
    std::thread tErr([&] {
        char buf[65536];
        DWORD got = 0;
        while (ReadFile(hErrRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
            errBuf.append(buf, got);
        }
    });

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    tOut.join();
    tErr.join();
    CloseHandle(hOutRead);
    CloseHandle(hErrRead);

    if (stdoutOut) *stdoutOut = std::move(outBuf);
    if (stderrOut) *stderrOut = std::move(errBuf);
    if (exitCode) *exitCode = (int)code;
    return true;
}

#else
bool GdalCli::run(const std::string& exePath,
                const std::vector<std::string>& args,
                std::string* stdoutOut,
                std::string* stderrOut,
                int* exitCode) {
    if (stdoutOut) stdoutOut->clear();
    if (stderrOut) stderrOut->clear();
    if (exitCode) *exitCode = -1;

    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) return false;
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid == 0) {
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        std::vector<std::string> av{ exePath };
        av.insert(av.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& s : av) argv.push_back(&s[0]);
        argv.push_back(nullptr);
        execv(exePath.c_str(), argv.data());
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);

    std::string outBuf, errBuf;
    for (;;) {
        char buf[65536];
        ssize_t n = read(outPipe[0], buf, sizeof(buf));
        if (n > 0) outBuf.append(buf, n);
        n = read(errPipe[0], buf, sizeof(buf));
        if (n > 0) errBuf.append(buf, n);
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) outBuf.append(buf, n);
            while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) errBuf.append(buf, n);
            close(outPipe[0]); close(errPipe[0]);
            if (stdoutOut) *stdoutOut = std::move(outBuf);
            if (stderrOut) *stderrOut = std::move(errBuf);
            if (exitCode) *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            return true;
        }
        usleep(2000);
    }
}
#endif