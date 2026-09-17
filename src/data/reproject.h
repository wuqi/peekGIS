#pragma once
#include <vector>

// 将源 CRS 的线段顶点(x,y 交替)重投影到目标 EPSG。失败返回 false(调用方回退源坐标)
namespace peekg::data {
bool reprojectVertices(const std::vector<float>& src, int srcEpsg, int dstEpsg,
                       std::vector<float>& dst);

// 重投影单个点
bool reprojectPoint(double x, double y, int srcEpsg, int dstEpsg, double& ox, double& oy);

// 计算顶点数组(x,y 交替)的包围盒
void computeExtent(const std::vector<float>& v,
                   double& minx, double& miny, double& maxx, double& maxy);

}  // namespace peekg::data
