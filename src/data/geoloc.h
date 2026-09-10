#pragma once
#include <string>
#include <vector>

// ===== geolocation (RFC4 / CF "2D 坐标变量") 自动栅格化 =====
// netCDF/HDF5 的 geolocation 产品: 数据变量用 2 维 lat/lon 阵列定位像素, 无正则仿射变换。
// 流程: 用户在 subdataset 对话框选「数据变量 + 时次」(netcdf-c 只列可 geolocation 的变量,
// 自动忽略 lat/lon/1D/无坐标变量) -> 修出临时 netCDF(该变量那一个时次切片 + lat/lon 2D)
// -> GDAL GeoLocTransformer(GDALCreateGeoLocTransformer + GDALGeoLocTransform, 不依赖驱动
// 自动填 GEOLOCATION 域) warp 成 EPSG:4326 常规 GeoTIFF 缓存, 上层当普通栅格读。
// 重采样使用 GDAL warp 默认 NEAREST(用户要求锯齿, 不要平滑)。

// 一个可 geolocation 的数据变量。timeCount = 首维大小(通常为时次数; 2D 变量为 1)。
namespace peekg::data {
struct GeolocVar {
    std::string var;
    int timeCount = 1;
};

// 扫描 netCDF: 返回所有「coordinates 属性指向 >=2 个 2D 变量」的数据变量(即 geolocation 数据层)。
// path 为普通文件路径。找到 0 个返回 false。
bool geolocationVars(const std::string& path, std::vector<GeolocVar>& out);

// 快速判断: 该文件是否含 geolocation 数据(等价 geolocationVars 非空)。
bool netcdfHasGeolocation(const std::string& path);

// 打开规格编码: "GEOLOC:<path>|<var>|<timeIdx>"。Windows 文件名不允许 '|', 可安全拆分。
// 应用里用这个 spec 入栅格队列, readRasterMetadata 识别后转成缓存 TIFF。
std::string makeGeolocSpec(const std::string& path, const std::string& var, int timeIdx);
bool parseGeolocSpec(const std::string& spec, std::string& path, std::string& var, int& timeIdx);

// 缓存 TIFF 路径: %TEMP%/peekgis-geoloc/<hash>.tif, 按 (path, var, timeIdx) 稳定。
std::string geolocCachePath(const std::string& path, const std::string& var, int timeIdx);

// 把 geolocation 数据栅格化为 EPSG:4326 GeoTIFF(修复 + 切片 + GeoLoc warp)。
// 成功返回 true, 写出 outTif。
bool buildGeolocationWarp(const std::string& path, const std::string& var, int timeIdx,
                          const std::string& outTif);

}  // namespace peekg::data