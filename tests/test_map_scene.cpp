#include "doctest.h"
#include "map/map_scene.h"
#include "data/gdal_common.h"
#include <cmath>

using namespace peekg::data;

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

TEST_CASE("zoomToLayer: 同 CRS 直接用源范围") {
    MapScene s;
    s.addLayer(makeLayer(100, 50, 200, 100));   // srcEpsg=4326
    s.displayEpsg = 4326;
    s.view.vpW = 200; s.view.vpH = 100;
    s.zoomToLayer(0);
    CHECK(s.view.centerX == doctest::Approx(150.0).epsilon(1e-9));
    CHECK(s.view.centerY == doctest::Approx(75.0).epsilon(1e-9));
    CHECK(s.view.scale == doctest::Approx(0.55).epsilon(1e-9));  // max(100*1.1/200, 50*1.1/100)
}

TEST_CASE("zoomToLayer: 动态投影(src!=display)转显示CRS范围") {
    ensureGdal();
    MapScene s;
    // 4326 小范围包围盒, 显示 CRS = Web Mercator
    s.addLayer(makeLayer(116.0, 39.0, 117.0, 41.0));
    s.displayEpsg = 3857;
    s.view.vpW = 400; s.view.vpH = 300;
    s.zoomToLayer(0);
    const double R = 6378137.0;
    auto mx = [&](double lon) { return R * (lon * M_PI / 180.0); };
    auto my = [&](double lat) { return R * std::log(std::tan(M_PI / 4.0 + (lat * M_PI / 180.0) / 2.0)); };
    CHECK(s.view.centerX == doctest::Approx(mx(116.5)).epsilon(5.0));
    CHECK(s.view.centerY == doctest::Approx(my(40.0)).epsilon(5.0));
    // 视图中心必须落在显示 CRS 的量级(而不是仍停留在源经纬度数值导致"飞不见")
    CHECK(s.view.centerX > 1e6);
    CHECK(s.view.scale > 0.0);
    // scale = max(wWorld, hWorld)*1.1  (pad=1.1, 视口 400x300)
    double wWorld = mx(117.0) - mx(116.0);
    double hWorld = my(41.0) - my(39.0);
    CHECK(s.view.scale == doctest::Approx(std::max(wWorld, hWorld) * 1.1).epsilon(5.0));
}

TEST_CASE("zoomToLayer: 栅格优先显示CRS范围, 无显式范围回退源范围") {
    MapScene s;
    RasterData rd;
    rd.name = "R";
    rd.minx = 0; rd.miny = 0; rd.maxx = 10; rd.maxy = 10;
    rd.hasDispExtent = true;
    rd.dispMinx = -180; rd.dispMiny = -90; rd.dispMaxx = 180; rd.dispMaxy = 90;
    s.addRasterLayer(rd);
    s.view.vpW = 400; s.view.vpH = 300;
    s.zoomToLayer(0);
    CHECK(s.view.centerX == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(s.view.centerY == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(s.view.scale == doctest::Approx(360.0 * 1.1 / 400.0).epsilon(1e-9));

    MapScene s2;
    RasterData rd2;
    rd2.name = "R2";
    rd2.minx = 0; rd2.miny = 0; rd2.maxx = 20; rd2.maxy = 10;
    rd2.hasDispExtent = false;
    s2.addRasterLayer(rd2);
    s2.view.vpW = 200; s2.view.vpH = 200;
    s2.zoomToLayer(0);
    CHECK(s2.view.centerX == doctest::Approx(10.0).epsilon(1e-9));
    CHECK(s2.view.centerY == doctest::Approx(5.0).epsilon(1e-9));
}
