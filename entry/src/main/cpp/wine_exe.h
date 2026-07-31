#pragma once

#include <napi/native_api.h>

napi_value RunWineExe(napi_env env, napi_callback_info info);
napi_value RunWineProgram(napi_env env, napi_callback_info info);
napi_value RunGuestProgram(napi_env env, napi_callback_info info);
napi_value RunHostProgram(napi_env env, napi_callback_info info);
napi_value RunHostReplay(napi_env env, napi_callback_info info);
napi_value IsHostReplayRunning(napi_env env, napi_callback_info info);
napi_value QueryWineProcess(napi_env env, napi_callback_info info);
napi_value TerminateWineProcess(napi_env env, napi_callback_info info);
