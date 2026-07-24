// phone_virgl_relay.h — 手机适配层：virgl IPC 中继（parent 侧）
#pragma once
#include <IPCKit/ipc_kit.h>
#include <cstdint>

// 返回给调用方：调用方只判断 == OH_IPC_SUCCESS
constexpr int kPhoneVirglRelayError = -1001;

// Attach 在手机 fork 路径下被拒绝：上层走 shm 回退
constexpr int32_t kPhoneVirglAttachDenied = -100;

// 通过手机适配层配置 socket 发送 virgl IPC 请求，返回 OH_IPC_SUCCESS 或错误
int PhoneVirgl_RelayRequest(uint32_t code, const OHIPCParcel* data, OHIPCParcel* reply);
