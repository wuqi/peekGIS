#include "doctest.h"
#include "data/sha1.h"

TEST_CASE("sha1 已知向量") {
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex("The quick brown fox jumps over the lazy dog") ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("sha1 对同一输入稳定") {
    CHECK(sha1_hex("peekGIS") == sha1_hex(std::string("peekGIS")));
}
