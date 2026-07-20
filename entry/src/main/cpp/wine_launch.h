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
    std::string prefixDir;
    std::string d3dBackend = "wined3d";
    bool automationMode = false;
    std::vector<std::string> envStrs;
    std::vector<char*> envp;
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized(const std::string& prefixDir);
bool IsWinePrefixInitialized();
