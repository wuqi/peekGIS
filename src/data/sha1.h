#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

// 紧凑 SHA-1 实现(用于缓存 sourceId), 跨平台, 无外部依赖
class SHA1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t len = 0;
    unsigned char buf[64] = {0};
    int bufn = 0;

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void process(const unsigned char* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;             k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

public:
    void update(const void* data, size_t n) {
        const unsigned char* p = (const unsigned char*)data;
        len += n;
        while (n > 0) {
            size_t take = 64 - bufn;
            if (take > n) take = n;
            for (size_t i = 0; i < take; i++) buf[bufn + i] = p[i];
            bufn += (int)take; p += take; n -= take;
            if (bufn == 64) { process(buf); bufn = 0; }
        }
    }

    void finalize() {
        uint64_t bitlen = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        if (bufn > 56) { while (bufn != 0) update(&zero, 1); }
        while (bufn != 56) update(&zero, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; i++) lb[i] = (unsigned char)((bitlen >> (56 - 8 * i)) & 0xFF);
        update(lb, 8);
    }

    std::string hex() {
        static const char* hx = "0123456789abcdef";
        std::string s;
        s.resize(40);
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 8; j++)
                s[i*8 + j] = hx[(h[i] >> (28 - j*4)) & 0xF];
        return s;
    }
};

inline std::string sha1_hex(const std::string& s) {
    SHA1 c;
    c.update(s.data(), s.size());
    c.finalize();
    return c.hex();
}
