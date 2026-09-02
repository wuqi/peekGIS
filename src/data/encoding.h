#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cpl_string.h>   // CPLRecode (跨平台回退, 基于 iconv)
#endif

// 属性编码: 用户为 DBF/.dbf 字符串属性选择"固定解释编码"。
// 读取阶段保存原始字节, 显示/切换时按所选编码转成 UTF-8(见 docs/属性表设计.md §1.1/§2.5)。
enum class TextEncoding : int {
    Utf8 = 0,
    Gbk = 1,
    Big5 = 2,
    Cp1252 = 3,
    Utf16LE = 4,
};

// 面板下拉显示名(顺序须与 TextEncoding 一致)
inline const char* const kEncodingNames[] = {
    "UTF-8", "GBK", "Big5", "CP1252", "UTF-16(LE)"
};
inline constexpr int kEncodingCount = 5;

// UTF-16LE 小端字节 -> UTF-8(手工, 避免 C 字符串被内部 NUL 截断)
inline std::string utf16leToUtf8(const std::vector<unsigned char>& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint32_t cp = (uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < raw.size()) {
            uint32_t lo = (uint32_t)raw[i + 2] | ((uint32_t)raw[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// 单/多字节编码(codepage)原始字节 -> UTF-8
inline bool codepageBytesToUtf8(const std::vector<unsigned char>& raw, int codepage, std::string& out) {
    if (raw.empty()) { out.clear(); return true; }
    if (codepage == 65001) {  // UTF-8 直通
        out.assign((const char*)raw.data(), raw.size());
        return true;
    }
#ifdef _WIN32
    // 以 \0 结尾的临时 C 串(MultiByte 系列需要)
    std::string s((const char*)raw.data(), raw.size());
    int wn = MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), &w[0], wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return false;
    out.resize(un);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &out[0], un, nullptr, nullptr);
    return true;
#else
    // 跨平台回退: 用 GDAL 的 CPLRecode(基于 iconv)
    std::string s((const char*)raw.data(), raw.size());
    const char* src = codepage == 936 ? "GBK" : codepage == 950 ? "BIG5" : "CP1252";
    char* r = CPLRecode(s.c_str(), src, "UTF-8");
    if (!r) return false;
    out = r;
    CPLFree(r);
    return true;
#endif
}

// 通用入口: 按所选编码把原始字节转成 UTF-8。非法/失败返回空串(界面显示为乱码占位由调用方处理)
inline std::string decodeRawToUtf8(const std::vector<unsigned char>& raw, TextEncoding enc) {
    if (raw.empty()) return {};
    if (enc == TextEncoding::Utf16LE) {
#ifdef _WIN32
        std::wstring wide;
        wide.reserve(raw.size() / 2);
        for (size_t i = 0; i + 1 < raw.size(); i += 2) {
            wchar_t wc = (wchar_t)(raw[i] | (raw[i + 1] << 8));
            wide.push_back(wc);
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &out[0], n, nullptr, nullptr);
        return out;
#else
        return utf16leToUtf8(raw);
#endif
    }
    static const int cpFor[] = { 65001, 936, 950, 1252 };  // Utf8, Gbk, Big5, Cp1252
    std::string out;
    return codepageBytesToUtf8(raw, cpFor[(int)enc], out) ? out : std::string();
}
