// phone_virgl_dispatch.h — 手机适配层：virgl IPC dispatch（child 侧）
#pragma once
#include <IPCKit/ipc_kit.h>
#include <cstdint>

// 业务回调：等价于 IPC stub 的 OnRequest 签名
// graphics_broker 对侧为 phone_virgl_relay.cpp
using PhoneVirglHandler = int (*)(uint32_t code, const OHIPCParcel* data,
                                   OHIPCParcel* reply, void* userData);

// 在独立线程启动 dispatch loop，从 fd 读请求 → 调用 handler → 回送 reply
void PhoneVirgl_DispatchStart(int fd, PhoneVirglHandler handler);
