#pragma once
#include <GLFW/glfw3.h>
#include <string>

// 返回窗口内容缩放(高 DPI 适配)
float getDpiScale(GLFWwindow* w);

// 加载字体：优先使用 fontPath(霞鹜新晰黑)，不存在则回退内置默认字体
void loadAppFont(class ImGuiIO& io, float scale, const std::string& fontPath);
