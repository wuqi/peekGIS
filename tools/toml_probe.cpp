#include <toml.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string path = (argc > 1) ? argv[1] : "config.toml";
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
        std::cout << "raw read " << bytes.size() << " bytes, first 64 hex: ";
        for (size_t i = 0; i < bytes.size() && i < 64; i++)
            printf("%02X ", (unsigned char)bytes[i]);
        std::cout << "\n";
    }
    std::ifstream f(path);  // 与 app 相同的文本模式打开
    if (!f) { std::cout << "open fail\n"; return 1; }
    try {
        auto data = toml::parse(f);
        std::cout << "parse OK, contains cache=" << data.contains("cache")
                  << " display=" << data.contains("display") << "\n";
    } catch (const std::exception& e) {
        std::cout << "parse FAIL: " << e.what() << "\n";
    }
    return 0;
}