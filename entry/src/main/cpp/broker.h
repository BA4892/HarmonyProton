#ifndef WINE_BROKER_H
#define WINE_BROKER_H

#include <string>
#include <vector>

// 全局: Broker 构造 NCP entryParams 时加 homeDir 前缀 (由 LaunchPadMode 设置)
extern std::string gBrokerHomeDir;

void SetBrokerSessionEnv(std::vector<std::string> env);
void ClearBrokerSessionEnv();

// 启动 Process Broker Unix socket server（在后台线程运行）
// 返回 0 表示成功，非 0 表示失败
int StartBrokerServer();

#endif // WINE_BROKER_H
