#include "platform/file_dialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

std::string openFileDialog() {
    OPENFILENAMEW ofn;
    wchar_t buf[4096] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrFilter =
        L"Vector Files (*.shp;*.gpkg;*.gdb;*.geojson;*.kml;*.json)\0"
        L"*.shp;*.gpkg;*.gdb;*.geojson;*.kml;*.json\0"
        L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return "";
    // wide -> UTF-8
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], n, nullptr, nullptr);
    return out;
}

#else
#include <cstdio>
#include <cstring>

std::string openFileDialog() {
    // 优先 zenity, 回退 kdialog
    const char* cmd = "zenity --file-selection 2>/dev/null";
    FILE* f = popen(cmd, "r");
    if (!f) {
        f = popen("kdialog --getopenfilename . 2>/dev/null", "r");
        if (!f) return "";
    }
    char buf[4096] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        pclose(f);
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        return std::string(buf);
    }
    pclose(f);
    return "";
}

#endif
