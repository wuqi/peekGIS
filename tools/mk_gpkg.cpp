#include <gdal.h>
#include <ogr_api.h>
#include <cstdio>

static void addGeom(GDALDatasetH ds, const char* name, OGRwkbGeometryType t, const char* wkt) {
    OGRLayerH l = GDALDatasetCreateLayer(ds, name, nullptr, t, nullptr);
    OGRFieldDefnH f = OGR_Fld_Create("id", OFTInteger);
    OGR_L_CreateField(l, f, 1);
    OGR_Fld_Destroy(f);
    OGRFeatureH feat = OGR_F_Create(OGR_L_GetLayerDefn(l));
    OGRGeometryH g = nullptr;
    char* w = (char*)wkt;
    OGR_G_CreateFromWkt(&w, nullptr, &g);
    OGR_F_SetGeometryDirectly(feat, g);
    OGR_L_CreateFeature(l, feat);
    OGR_F_Destroy(feat);
}

int main(int argc, char** argv) {
    GDALAllRegister();
    const char* path = (argc > 1) ? argv[1] : "tools/test.gpkg";
    GDALDriverH drv = GDALGetDriverByName("GPKG");
    GDALDatasetH ds = GDALCreate(drv, path, 0, 0, 0, GDT_Unknown, nullptr);
    addGeom(ds, "layer_a", wkbPolygon, "POLYGON((0 0,1 0,1 1,0 1,0 0))");
    addGeom(ds, "layer_b", wkbLineString, "LINESTRING(2 2,3 3,4 2)");
    addGeom(ds, "layer_c", wkbPolygon, "POLYGON((10 10,11 10,11 11,10 11,10 10))");
    addGeom(ds, "layer_d", wkbLineString, "LINESTRING(20 20,21 21)");
    GDALClose(ds);
    printf("created %s (4 layers)\n", path);
    return 0;
}
