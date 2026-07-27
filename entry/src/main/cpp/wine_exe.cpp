#include <napi/native_api.h>
#include "wine_env.h"
#include "wine_process.h"
#include "broker.h"
#include "graphics_broker.h"
#include "wayland_server.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

extern napi_threadsafe_function gStateTsfn;

napi_value RunWineExe(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return nullptr;

    char binDir[512] = {}, sockPath[512] = {}, libPath[2048] = {}, wineExe[1024] = {}, homePath[1024] = {};
    napi_get_value_string_utf8(env, args[0], binDir, sizeof(binDir), nullptr);
    napi_get_value_string_utf8(env, args[1], sockPath, sizeof(sockPath), nullptr);
    napi_get_value_string_utf8(env, args[2], libPath, sizeof(libPath), nullptr);
    napi_get_value_string_utf8(env, args[3], wineExe, sizeof(wineExe), nullptr);
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], homePath, sizeof(homePath), nullptr);
    }

    std::string homeDir(homePath);
    if (homeDir.empty()) homeDir = gBrokerHomeDir;
    if (homeDir.empty()) homeDir = "/storage/Users/currentUser/Download";

    std::string exePath(wineExe);
    {
        std::string lower = exePath;
        for (auto& c : lower) c = tolower(c);
        if (lower.find("/drive_c/") != std::string::npos) {
            auto slash = exePath.find_last_of('/');
            if (slash != std::string::npos) exePath = exePath.substr(slash + 1);
        }
    }

    OH_LOG_INFO(LOG_APP, "[Wine] runWineExe bin=%{public}s exe=%{public}s (final=%{public}s) home=%{public}s",
                binDir, wineExe, exePath.c_str(), homeDir.c_str());

    std::string sockStr(sockPath);
    auto pos = sockStr.find_last_of('/');
    std::string sockDir = (pos == std::string::npos) ? "/tmp" : sockStr.substr(0, pos);
    std::string sockName = (pos == std::string::npos) ? sockStr : sockStr.substr(pos + 1);

    int audioBootstrapFd = -1;  // broker 会为每个子进程创建 audio fd, 此处无需传递

    std::vector<std::string> wineEnv = BuildWineEnv(sockDir, sockName, libPath, binDir, audioBootstrapFd, homeDir);

    // desktop 模式: 将进程接入 explorer 创建的 shell desktop,
    // 使其窗口出现在任务栏, 且能与其他 shell 进程互相访问
    if (WaylandServer::GetInstance()->IsDesktopMode())
        wineEnv.push_back("WINEHUA_DESKTOP=shell");

    {
#ifdef __aarch64__
        std::string entryParams = std::string(binDir) + "|" + exePath;
#else
        std::string entryParams = std::string(binDir) + "|wine|" + exePath;
#endif
        entryParams += SerializeEnvToEntryParams(wineEnv);
        OH_LOG_INFO(LOG_APP, "[Wine] runWineExe via broker: %{public}s", entryParams.c_str());

        int broker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (broker_fd < 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker socket failed: %{public}s", strerror(errno));
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, getenv("PROCESSBROKER"));
        if (connect(broker_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker connect failed: %{public}s", strerror(errno));
            close(broker_fd);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        char req_hdr[32];
        int hdr_len = snprintf(req_hdr, sizeof(req_hdr), "SPAWN\n");
        size_t ep_len = entryParams.size();
        struct iovec iov[3] = {
            {req_hdr, (size_t)hdr_len},
            {(void*)entryParams.c_str(), ep_len},
            {(void*)"\n", 1}
        };
        struct msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = 3;

        if (sendmsg(broker_fd, &msg, MSG_NOSIGNAL) < 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker sendmsg failed: %{public}s", strerror(errno));
            close(broker_fd);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        int32_t response[2] = {-1, -1};
        ssize_t n = recv(broker_fd, response, sizeof(response), MSG_WAITALL);
        close(broker_fd);
        if (n != sizeof(response) || response[1] != 0 || response[0] <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker spawn failed pid=%{public}d status=%{public}d", response[0], response[1]);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        pid_t pid = response[0];
        AddProcess(pid, wineExe, -1);
        OH_LOG_INFO(LOG_APP, "[Wine] wine pid=%{public}d exe=%{public}s (via broker)", pid, wineExe);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:wine-running", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
    return nullptr;
}
