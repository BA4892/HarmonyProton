#include "wine_exe.h"

#include "broker.h"
#include "graphics_broker.h"
#include "wayland_server.h"
#include "wine_constants.h"
#include "wine_env.h"
#include "wine_process.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

extern napi_threadsafe_function gStateTsfn;

namespace {

struct ProgramOptions {
    std::string windowsExePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    std::string prefixMode = "reuse";
    std::string d3dBackend = "wined3d";
    std::string presentBackend = "virgl_compositor";
    bool automationMode = false;
};

struct GuestProgramOptions {
    std::string executablePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    bool automationMode = true;
};

static bool HasUnsafeProtocolChar(const std::string& value)
{
    return value.find('|') != std::string::npos || value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
}

static std::string ReadString(napi_env env, napi_value value)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
    std::vector<char> buffer(length + 1);
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length) != napi_ok) return {};
    return std::string(buffer.data(), length);
}

static bool GetNamed(napi_env env, napi_value object, const char* name, napi_value* out)
{
    bool has = false;
    if (napi_has_named_property(env, object, name, &has) != napi_ok || !has) return false;
    return napi_get_named_property(env, object, name, out) == napi_ok;
}

static std::string GetString(napi_env env, napi_value object, const char* name,
                             const std::string& fallback = {})
{
    napi_value value;
    napi_valuetype type;
    if (!GetNamed(env, object, name, &value) || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_string)
        return fallback;
    std::string result = ReadString(env, value);
    return result.empty() ? fallback : result;
}

static bool GetBool(napi_env env, napi_value object, const char* name, bool fallback)
{
    napi_value value;
    napi_valuetype type;
    bool result = fallback;
    if (GetNamed(env, object, name, &value) && napi_typeof(env, value, &type) == napi_ok &&
        type == napi_boolean)
        napi_get_value_bool(env, value, &result);
    return result;
}

static void ReadStringArray(napi_env env, napi_value object, const char* name,
                            std::vector<std::string>* out)
{
    napi_value array;
    bool isArray = false;
    uint32_t length = 0;
    if (!GetNamed(env, object, name, &array) || napi_is_array(env, array, &isArray) != napi_ok || !isArray ||
        napi_get_array_length(env, array, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value item;
        napi_valuetype type;
        if (napi_get_element(env, array, i, &item) == napi_ok &&
            napi_typeof(env, item, &type) == napi_ok && type == napi_string)
            out->push_back(ReadString(env, item));
    }
}

static bool IsValidEnvKey(const std::string& key)
{
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
    return std::all_of(key.begin() + 1, key.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    });
}

static void ReadEnvironment(napi_env env, napi_value object, std::vector<std::string>* out)
{
    napi_value record;
    napi_valuetype type;
    if (!GetNamed(env, object, "environment", &record) ||
        napi_typeof(env, record, &type) != napi_ok || type != napi_object)
        return;

    napi_value keys;
    uint32_t length = 0;
    if (napi_get_property_names(env, record, &keys) != napi_ok ||
        napi_get_array_length(env, keys, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value keyValue, value;
        napi_valuetype valueType;
        if (napi_get_element(env, keys, i, &keyValue) != napi_ok) continue;
        std::string key = ReadString(env, keyValue);
        if (!IsValidEnvKey(key) || napi_get_property(env, record, keyValue, &value) != napi_ok ||
            napi_typeof(env, value, &valueType) != napi_ok || valueType != napi_string)
            continue;
        std::string line = key + "=" + ReadString(env, value);
        if (!HasUnsafeProtocolChar(line)) out->push_back(std::move(line));
    }
}

static std::string EnvKey(const std::string& line)
{
    size_t separator = line.find('=');
    return separator == std::string::npos ? line : line.substr(0, separator);
}

static void UpsertEnv(std::vector<std::string>* env, std::string line)
{
    const std::string key = EnvKey(line);
    env->erase(std::remove_if(env->begin(), env->end(), [&](const std::string& existing) {
        return EnvKey(existing) == key;
    }), env->end());
    env->push_back(std::move(line));
}

static std::string PrefixForMode(const std::string& mode)
{
    return mode == "clean" ? WINE_SMOKE_PREFIX : WINE_PREFIX;
}

static std::string NativePathToWindows(const std::string& path, const std::string& prefix)
{
    const std::string driveRoot = prefix + "/drive_c/";
    if (path.rfind(driveRoot, 0) != 0) return path;
    std::string result = "C:\\" + path.substr(driveRoot.size());
    std::replace(result.begin(), result.end(), '/', '\\');
    return result;
}

static pid_t SpawnViaBroker(const std::string& entryParams)
{
    const char* brokerPath = getenv("PROCESSBROKER");
    if (!brokerPath || !brokerPath[0]) brokerPath = WINE_BROKER_SOCKET;
    int brokerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (brokerFd < 0) return -1;

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (strlen(brokerPath) >= sizeof(address.sun_path))
    {
        close(brokerFd);
        return -1;
    }
    strcpy(address.sun_path, brokerPath);
    if (connect(brokerFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Program] broker connect failed: %{public}s", strerror(errno));
        close(brokerFd);
        return -1;
    }

    static constexpr char header[] = "SPAWN\n";
    iovec iov[3] = {
        {const_cast<char*>(header), sizeof(header) - 1},
        {const_cast<char*>(entryParams.data()), entryParams.size()},
        {const_cast<char*>("\n"), 1},
    };
    msghdr message = {};
    message.msg_iov = iov;
    message.msg_iovlen = 3;
    if (sendmsg(brokerFd, &message, MSG_NOSIGNAL) < 0)
    {
        close(brokerFd);
        return -1;
    }

    int32_t response[2] = {-1, -1};
    ssize_t received = recv(brokerFd, response, sizeof(response), MSG_WAITALL);
    close(brokerFd);
    if (received != sizeof(response) || response[1] != 0 || response[0] <= 0) return -1;
    return response[0];
}

static napi_value MakeProcessObject(napi_env env, const WineProcessEntry* entry, bool found)
{
    napi_value object;
    napi_create_object(env, &object);

    auto setBool = [&](const char* name, bool value) {
        napi_value item; napi_get_boolean(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setInt = [&](const char* name, int32_t value) {
        napi_value item; napi_create_int32(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setDouble = [&](const char* name, double value) {
        napi_value item; napi_create_double(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setString = [&](const char* name, const std::string& value) {
        napi_value item; napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, object, name, item);
    };

    setBool("found", found);
    if (!found || !entry)
    {
        setInt("pid", -1);
        setString("status", "unknown");
        setString("exitCodeSource", "unknown");
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
        return object;
    }

    setInt("pid", entry->pid);
    setString("status", entry->running ? "running" : "exited");
    setDouble("startTimestamp", static_cast<double>(entry->startTimestampMs));
    setDouble("endTimestamp", static_cast<double>(entry->endTimestampMs));
    setString("exitCodeSource", entry->exitCodeSource);
    if (entry->exitCode >= 0) setInt("exitCode", entry->exitCode);
    else
    {
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
    }
    return object;
}

static pid_t SpawnWineProgram(const ProgramOptions& options)
{
    if (options.windowsExePath.empty() || HasUnsafeProtocolChar(options.windowsExePath)) return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string prefixDir = PrefixForMode(options.prefixMode);
    const std::string homeDir = options.automationMode ? WINE_AUTOMATION_HOME
        : (gBrokerHomeDir.empty() ? "/storage/Users/currentUser/Download" : gBrokerHomeDir);
    const std::string sockDir = prefixDir;
    const std::string sockName = "wine-wayland";
    const std::string libPath = binDir + ":" + binDir + "/x86_64-unix";
    const std::string exePath = NativePathToWindows(options.windowsExePath, prefixDir);

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    winehua::GraphicsBroker::GetInstance().SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    if (!winehua::GraphicsBroker::GetInstance().EnsureStarted(prefixDir)) return -1;
    winehua::GraphicsBroker::GetInstance().SetVulkanPresentMode(
        options.presentBackend == "venus_broker_present" ||
        options.presentBackend == "venus_direct_present");

    std::vector<std::string> envStrs = BuildWineEnv(
        sockDir, sockName, libPath, binDir, -1, homeDir, prefixDir);
    for (const std::string& line : options.environment) UpsertEnv(&envStrs, line);
    UpsertEnv(&envStrs, "WINEHUA_D3D_BACKEND=" + options.d3dBackend);
    UpsertEnv(&envStrs, "WINEHUA_PRESENT_BACKEND=" + options.presentBackend);
    UpsertEnv(&envStrs, std::string("WINEHUA_AUTOMATION=") + (options.automationMode ? "1" : "0"));
    /* DXVK is a managed WineHua runtime overlay, never a game-provided DLL. */
    AppendD3dBackendEnv(envStrs, options.d3dBackend, binDir);
    if (options.d3dBackend.rfind("dxvk_", 0) == 0)
        OH_LOG_INFO(LOG_APP, "[WineProgram] managed D3D backend=%{public}s",
                    options.d3dBackend.c_str());
    UpsertEnv(&envStrs, "WINEHUA_WINE_UNIX_ARCH=x86_64");
    UpsertEnv(&envStrs, "WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    if (!options.workingDirectory.empty())
        UpsertEnv(&envStrs, "WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);

#ifdef __aarch64__
    std::string entryParams = binDir + "|" + exePath;
#else
    std::string entryParams = binDir + "|wine|" + exePath;
#endif
    for (const std::string& arg : options.argv) entryParams += "|" + arg;
    AppendMissingEntryParamsEnvOverrides(entryParams, envStrs);

    const pid_t pid = SpawnViaBroker(entryParams);
    if (pid <= 0) return -1;
    AddProcess(pid, options.windowsExePath, -1);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] pid=%{public}d exe=%{public}s prefix=%{public}s d3d=%{public}s present=%{public}s automation=%{public}s",
                pid, exePath.c_str(), prefixDir.c_str(), options.d3dBackend.c_str(),
                options.presentBackend.c_str(), options.automationMode ? "true" : "false");
    if (gStateTsfn)
    {
        char state[64];
        snprintf(state, sizeof(state), "%d:wine-running", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(state), napi_tsfn_blocking);
    }
    return pid;
}

static pid_t SpawnGuestProgram(const GuestProgramOptions& options)
{
    const std::string guestRoot = std::string(WINE_RUNTIME_BIN) + "/guest_vulkan";
    if (options.executablePath.rfind(guestRoot + "/", 0) != 0 ||
        HasUnsafeProtocolChar(options.executablePath))
        return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string guestLib = guestRoot + "/lib";
    const std::string gfxLib = binDir + "/guest_gfx/lib";
    const std::string unixLib = binDir + "/x86_64-unix";
    const std::string libraryPath = guestLib + ":" + gfxLib + ":" + binDir + ":" + unixLib;
    const std::string icd = guestRoot + "/share/vulkan/icd.d/venus_icd.x86_64.json";

    std::vector<std::string> envStrs = BuildWineEnv(
        WINE_PREFIX, "wine-wayland", libraryPath, binDir, -1,
        WINE_AUTOMATION_HOME, WINE_PREFIX);
#ifdef __aarch64__
    // The NCP and box64.so are native AArch64.  Guest x86_64 directories must
    // only enter Box64's emulated lookup path; putting them in LD_LIBRARY_PATH
    // can make the native dynamic linker inspect wrong-architecture objects.
    envStrs.erase(std::remove_if(envStrs.begin(), envStrs.end(), [](const std::string& line) {
        return EnvKey(line) == "LD_LIBRARY_PATH";
    }), envStrs.end());
#else
    UpsertEnv(&envStrs, "LD_LIBRARY_PATH=" + libraryPath);
#endif
    UpsertEnv(&envStrs, "BOX64_LD_LIBRARY_PATH=" + libraryPath);
    UpsertEnv(&envStrs,
        "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
        "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
        "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
        "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
        "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
        "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8");
    // Library loading has its own smoke assertions.  Function-call tracing is
    // prohibitively noisy when a disconnected vtest socket is polled and can
    // otherwise grow the shared stderr log by gigabytes before the watchdog.
    UpsertEnv(&envStrs, "BOX64_LOG=1");
    UpsertEnv(&envStrs, "BOX64_NOBANNER=1");
    UpsertEnv(&envStrs, "VK_DRIVER_FILES=" + icd);
    UpsertEnv(&envStrs, "VK_ICD_FILENAMES=" + icd);
    UpsertEnv(&envStrs, "VN_DEBUG=vtest");
    // OHOS Host Vulkan memory uses an explicit SHM shadow when the driver
    // cannot export dma-buf/opaque-fd memory.  GPU fence feedback writes only
    // the Host mapping, so query the real Host fence and synchronize the
    // shadow after it signals instead of polling a stale Guest feedback slot.
    UpsertEnv(&envStrs, "VN_PERF=no_fence_feedback");
    UpsertEnv(&envStrs, "WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    UpsertEnv(&envStrs, std::string("WINEHUA_AUTOMATION=") +
              (options.automationMode ? "1" : "0"));
    if (!options.workingDirectory.empty())
        UpsertEnv(&envStrs, "WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);
    for (const std::string& line : options.environment) UpsertEnv(&envStrs, line);

    std::string entryParams = binDir + "|__winehua_guest_elf__|" + options.executablePath;
    for (const std::string& arg : options.argv) entryParams += "|" + arg;
    AppendMissingEntryParamsEnvOverrides(entryParams, envStrs);

    const pid_t pid = SpawnViaBroker(entryParams);
    if (pid <= 0) return -1;
    AddProcess(pid, options.executablePath, -1);
    OH_LOG_INFO(LOG_APP, "[GuestProgram] pid=%{public}d elf=%{public}s icd=%{public}s",
                pid, options.executablePath.c_str(), icd.c_str());
    return pid;
}

} // namespace

napi_value RunWineProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    ProgramOptions options;
    options.windowsExePath = GetString(env, args[0], "windowsExePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.prefixMode = GetString(env, args[0], "prefixMode", "reuse");
    options.d3dBackend = GetString(env, args[0], "d3dBackend", "wined3d");
    options.presentBackend = GetString(env, args[0], "presentBackend", "virgl_compositor");
    options.automationMode = GetBool(env, args[0], "automationMode", false);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnWineProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunGuestProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    GuestProgramOptions options;
    options.executablePath = GetString(env, args[0], "executablePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.automationMode = GetBool(env, args[0], "automationMode", true);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnGuestProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value QueryWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value TerminateWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    bool ok = pid > 0 && kill(pid, SIGKILL) == 0;
    if (ok) RemoveProcess(pid, -1, "unknown");
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

napi_value RunWineExe(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return MakeProcessObject(env, nullptr, false);

    ProgramOptions options;
    options.windowsExePath = NativePathToWindows(ReadString(env, args[3]), WINE_PREFIX);
    options.workingDirectory.clear();
    options.prefixMode = "reuse";
    options.automationMode = false;
    const pid_t pid = SpawnWineProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}
