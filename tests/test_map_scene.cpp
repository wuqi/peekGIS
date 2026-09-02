#include "doctest.h"
#include "map/map_scene.h"

static VectorData makeLayer(double minx, double miny, double maxx, double maxy) {
    VectorData vd;
    vd.minx = minx; vd.miny = miny; vd.maxx = maxx; vd.maxy = maxy;
    vd.vertices = { (float)minx, (float)miny, (float)maxx, (float)maxy };
    vd.srcEpsg = 4326;
    vd.name = "L";
    return vd;
}

TEST_CASE("fitToView: 中心与缩放比例") {
    MapScene s;
    s.addLayer(makeLayer(0, 0, 100, 50));
    s.fitToView(200, 100);
    CHECK(s.view.centerX == doctest::Approx(50.0));
    CHECK(s.view.centerY == doctest::Approx(25.0));
    // scale = max(100*1.1/200, 50*1.1/100) = max(0.55, 0.55) = 0.55
    CHECK(s.view.scale == doctest::Approx(0.55).epsilon(1e-9));
}

TEST_CASE("expandExtent: 合并范围") {
    MapScene s;
    s.expandExtent(0, 0, 10, 10);
    CHECK(s.bboxMinX == 0); CHECK(s.bboxMaxX == 10);
    s.expandExtent(5, 5, 20, 20);
    CHECK(s.bboxMinX == 0); CHECK(s.bboxMinY == 0);
    CHECK(s.bboxMaxX == 20); CHECK(s.bboxMaxY == 20);
}

TEST_CASE("screenToWorld 与 fitToView 互逆(中心)") {
    MapScene s;
    s.addLayer(makeLayer(0, 0, 100, 50));
    s.fitToView(200, 100);
    double wx, wy;
    s.screenToWorld(100, 50, wx, wy);  // 视口中心
    CHECK(wx == doctest::Approx(s.view.centerX).epsilon(1e-9));
    CHECK(wy == doctest::Approx(s.view.centerY).epsilon(1e-9));
}

TEST_CASE("pan 改变中心") {
    MapScene s;
    s.addLayer(makeLayer(0, 0, 100, 50));
    s.fitToView(200, 100);
    double cx0 = s.view.centerX;
    s.pan(10, 0);  // 向右平移 10 像素 -> 世界中心左移 10*scale
    CHECK(s.view.centerX == doctest::Approx(cx0 - 10 * 0.55).epsilon(1e-9));
}
