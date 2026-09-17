#include "doctest.h"
#include "render/bucketize.h"

TEST_CASE("partitionLayer: 空层返回空分区") {
    vbucket::CpuPartition p;
    CHECK(vbucket::partitionLayer({}, {}, {}, 0, 0, p));
    CHECK(p.gridN == 1);
    CHECK(p.bucketCell.empty());
    CHECK(p.cellToBucket.empty());
    CHECK(p.totalVerts == 0);
}

TEST_CASE("partitionLayer: 小图层单块, 顶点数保持") {
    // 2 条线段(4 顶点) + 2 点 + 1 三角形
    std::vector<float> V = {0, 0, 10, 0,  10, 10, 20, 20};
    std::vector<float> P = {5, 5,  15, 15};
    std::vector<float> F = {1, 1,  3, 1,  2, 3};
    vbucket::CpuPartition p;
    REQUIRE(vbucket::partitionLayer(V, P, F, 0, 0, p));
    CHECK(p.gridN == 1);
    CHECK(p.bucketCell.size() == 1);
    CHECK(p.totalVerts == 4 + 2 + 3);
    // 各几何分量按原始数据完整归入单块
    const auto& bv = p.cellV[p.bucketCell[0]];
    const auto& bp = p.cellP[p.bucketCell[0]];
    const auto& bf = p.cellF[p.bucketCell[0]];
    CHECK(bv.size() == V.size());
    CHECK(bp.size() == P.size());
    CHECK(bf.size() == F.size());
    CHECK(p.cellToBucket[0] == 0);
    // 层次范围 = 所有几何合并范围
    CHECK(p.minx == doctest::Approx(0.0));
    CHECK(p.miny == doctest::Approx(0.0));
    CHECK(p.maxx == doctest::Approx(20.0));
    CHECK(p.maxy == doctest::Approx(20.0));
}

TEST_CASE("partitionLayer: 分散点序列分到对应格, 地理对应正确") {
    // 30 万点(> 4*262144 阈值)在 4 个象限 -> 网格 2x2 自然形成, 各象限归对应格
    std::vector<float> P;
    P.reserve(4 * 300000);
    for (int i = 0; i < 75000; i++) { P.push_back(1); P.push_back(1); }
    for (int i = 0; i < 75000; i++) { P.push_back(9); P.push_back(1); }
    for (int i = 0; i < 75000; i++) { P.push_back(1); P.push_back(9); }
    for (int i = 0; i < 75000; i++) { P.push_back(9); P.push_back(9); }
    std::vector<float> V, F;
    vbucket::CpuPartition p;
    REQUIRE(vbucket::partitionLayer(V, P, F, 0, 0, p));
    REQUIRE(p.gridN == 2);
    REQUIRE(p.bucketCell.size() == 4);
    REQUIRE(p.totalVerts == 300000);
    // 层次 bbox = 所有点范围
    CHECK(p.minx == doctest::Approx(1.0));
    CHECK(p.miny == doctest::Approx(1.0));
    CHECK(p.maxx == doctest::Approx(9.0));
    CHECK(p.maxy == doctest::Approx(9.0));
    // 格下标 cy*nc+cx; x/y=9 被 clamp 到 cx/cy=1
    auto cellOf = [&](double x, double y) {
        return vbucket::vecCellIndex(x, y, p.minx, p.miny, p.cellW, p.cellH, p.gridN);
    };
    size_t c00 = cellOf(1, 1), c10 = cellOf(9, 1), c01 = cellOf(1, 9), c11 = cellOf(9, 9);
    REQUIRE(c00 != c10); REQUIRE(c00 != c01); REQUIRE(c00 != c11);
    REQUIRE(c01 != c11); REQUIRE(c10 != c11);
    // 每格仅含自己象限的点: 对应 cellP 数组元素全等于该象限坐标
    for (const auto& cell : {std::make_pair(c00, 1.0f), std::make_pair(c10, 9.0f),
                             std::make_pair(c01, 1.0f), std::make_pair(c11, 9.0f)}) {
        size_t ci = cell.first; float qx = cell.second;
        const auto& b = p.cellP[ci];
        CHECK(b.size() == 150000);   // 75000 点
        for (size_t i = 0; i < b.size(); i += 2) {
            CAPTURE(i); CAPTURE(b[i]); CAPTURE(b[i + 1]);
            CHECK(b[i] == doctest::Approx(qx));
            if (ci == c00 || ci == c10)
                CHECK(b[i + 1] == doctest::Approx(1.0f));
            else
                CHECK(b[i + 1] == doctest::Approx(9.0f));
        }
    }
    // cellToBucket 与 bucketCell 互相一致
    CHECK(p.cellToBucket[c00] == 0);
    CHECK(p.cellToBucket[c10] == 1);
    CHECK(p.cellToBucket[c01] == 2);
    CHECK(p.cellToBucket[c11] == 3);
    CHECK(p.bucketCell[0] == c00);
    CHECK(p.bucketCell[1] == c10);
    CHECK(p.bucketCell[2] == c01);
    CHECK(p.bucketCell[3] == c11);
}

TEST_CASE("partitionLayer: srcEpsg==target 零重投影路径") {
    std::vector<float> P = {116.0f, 39.0f};
    vbucket::CpuPartition p;
    REQUIRE(vbucket::partitionLayer({}, P, {}, 4326, 4326, p));
    CHECK(p.totalVerts == 1);
    auto& b = p.cellP[p.bucketCell[0]];
    CHECK(b.size() == 2);
    CHECK(b[0] == doctest::Approx(116.0f));
    CHECK(b[1] == doctest::Approx(39.0f));
}

TEST_CASE("partitionLayer: 跨CRS重投影(4326->3857)") {
    // 北京附近 (116.3E, 39.9N) -> 3857 米制(正值, 百万米量级)
    std::vector<float> P = {116.3f, 39.9f};
    vbucket::CpuPartition p;
    REQUIRE(vbucket::partitionLayer({}, P, {}, 4326, 3857, p));
    REQUIRE(p.bucketCell.size() == 1);
    auto& b = p.cellP[p.bucketCell[0]];
    REQUIRE(b.size() == 2);
    CHECK(b[0] > 1e7);    // x 约 12.9M 米
    CHECK(b[1] > 4e6);    // y 约 4.8M 米
    CHECK(p.totalVerts == 1);
}

TEST_CASE("partitionLayer: 专用化网格尺寸随顶点规模增长") {
    // 64K 顶点 -> 预计 n = ceil(sqrt(65536/262144)) = 1; 百万顶点 -> 2
    std::vector<float> P;
    vbucket::CpuPartition p;
    for (long long i = 0; i < 65536; i++) { P.push_back(0); P.push_back(0); }
    REQUIRE(vbucket::partitionLayer({}, P, {}, 0, 0, p));
    CHECK(p.gridN >= 1);
    CHECK(p.totalVerts == 65536);

    std::vector<float> Q;
    for (long long i = 0; i < 1000000; i++) { Q.push_back(0); Q.push_back(0); }
    vbucket::CpuPartition q;
    REQUIRE(vbucket::partitionLayer({}, Q, {}, 0, 0, q));
    CHECK(q.gridN == 2);
    CHECK(q.totalVerts == 1000000);
}