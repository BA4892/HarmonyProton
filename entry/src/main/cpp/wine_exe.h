#pragma once

#include <napi/native_api.h>

#include <string>
#include <vector>

// 与 Index.ets runWineProgram 参数一一对应。自动拉起路径
// (LaunchPadMode 非桌面分支的 explorer) 也复用同一结构, 保证自动启动
// 与手动启动走完全相同的 broker 启动路径。
struct ProgramOptions {
    std::string windowsExePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    std::string prefixMode = "reuse";
    std::string d3dBackend = "dxvk_legacy";
    std::string presentBackend = "virgl_compositor";
    bool automationMode = false;
};

// 经 broker 通道启动一个 Wine 程序 (手动 runWineProgram 与自动拉起共用)。
// 返回子进程 pid, <= 0 表示启动失败。
int SpawnWineProgram(const ProgramOptions& options);

napi_value RunWineExe(napi_env env, napi_callback_info info);
napi_value RunWineProgram(napi_env env, napi_callback_info info);
napi_value RunGuestProgram(napi_env env, napi_callback_info info);
napi_value RunHostProgram(napi_env env, napi_callback_info info);
napi_value RunHostReplay(napi_env env, napi_callback_info info);
napi_value IsHostReplayRunning(napi_env env, napi_callback_info info);
napi_value QueryWineProcess(napi_env env, napi_callback_info info);
napi_value TerminateWineProcess(napi_env env, napi_callback_info info);
