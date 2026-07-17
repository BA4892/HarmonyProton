#pragma once

#include <string>
#include <vector>
#include <napi/native_api.h>

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized();
