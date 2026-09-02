#include "dpi.h"
#include "imgui.h"
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

float getDpiScale(GLFWwindow*) {
#ifdef _WIN32
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    if (dpi > 0) return (float)dpi / 96.0f;
#endif
    return 1.0f;
}

void loadAppFont(ImGuiIO& io, float scale, const std::string& fontPath) {
    // 中文需要显式指定 CJK 字形范围, 否则默认 Latin 范围会渲染成 "?????"
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    auto tryLoad = [&](const std::string& p) -> bool {
        std::error_code ec;
        if (p.empty() || !std::filesystem::exists(p, ec)) return false;
        ImFontConfig cfg;
        cfg.OversampleH = 2; cfg.OversampleV = 1;
        ImFont* f = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f * scale, &cfg, ranges);
        if (f) { io.FontDefault = f; return true; }
        return false;
    };
    // 1) 配置指定的字体(如 fonts/LXGW.ttf)
    if (tryLoad(fontPath)) return;
    // 2) 回退: 系统自带 CJK 字体(需与构建目标平台匹配)
#ifdef _WIN32
    if (tryLoad("C:/Windows/Fonts/msyh.ttc")) return;   // 微软雅黑
    if (tryLoad("C:/Windows/Fonts/msyh.ttf")) return;
    if (tryLoad("C:/Windows/Fonts/simhei.ttf")) return; // 黑体
    if (tryLoad("C:/Windows/Fonts/simsun.ttc")) return; // 宋体
#else
    if (tryLoad("/usr/share/fonts/truetype/wenquanyi/wqy-zenhei.ttc")) return;
    if (tryLoad("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")) return;
    if (tryLoad("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttf")) return;
    if (tryLoad("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")) return;
#endif
    // 3) 仍失败: 保持 ImGui 内置默认字体(此时中文会显示为缺字)
}
