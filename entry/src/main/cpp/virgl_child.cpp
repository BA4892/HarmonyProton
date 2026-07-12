#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "virgl-child"

namespace {

using WinehuaVtestMain = int (*)(int argc, char** argv);

bool IsAllowedHostEnv(const std::string& key)
{
    return key == "LD_LIBRARY_PATH" ||
           key == "VTEST_USE_GLES" ||
           key == "VTEST_USE_EGL_SURFACELESS" ||
           key == "VTEST_SYNC_GL_FINISH" ||
           key == "VIRGL_DISABLE_NATIVE_FENCE_FD" ||
           key == "WINEHUA_VIRGL_SYNC_MODE" ||
           key == "WINEHUA_VIRGL_LOG_PATH" ||
           key == "EGL_PLATFORM";
}

void ClearGuestGraphicsEnv()
{
    const char* keys[] = {
        "GALLIUM_DRIVER",
        "MESA_LOADER_DRIVER_OVERRIDE",
        "LIBGL_ALWAYS_SOFTWARE",
        "LIBGL_DRIVERS_PATH",
        "EGL_DRIVERS_PATH",
        "__EGL_VENDOR_LIBRARY_DIRS",
        "BOX64_LD_LIBRARY_PATH",
        "BOX64_EMULATED_LIBS",
        "VTEST_SOCKET_NAME",
    };

    for (const char* key : keys) unsetenv(key);
}

void ApplyHostEnv(const char* token)
{
    static const char prefix[] = "__env=";
    const char* assignment;
    const char* equals;
    std::string key;

    if (!token || strncmp(token, prefix, sizeof(prefix) - 1)) return;
    assignment = token + sizeof(prefix) - 1;
    equals = strchr(assignment, '=');
    if (!equals || equals == assignment) return;

    key.assign(assignment, static_cast<size_t>(equals - assignment));
    if (!IsAllowedHostEnv(key))
    {
        OH_LOG_WARN(LOG_APP, "[virgl-child] rejected env key=%{public}s", key.c_str());
        return;
    }
    setenv(key.c_str(), equals + 1, 1);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char* entryParams = args.entryParams ? args.entryParams : "";
    char* buffer = strdup(entryParams);
    char* save = nullptr;
    char* helperPath;
    char* socketPath;
    void* handle;
    WinehuaVtestMain vtestMain;

    OH_LOG_INFO(LOG_APP, "[virgl-child] Main enter pid=%{public}d params=%{public}s",
                getpid(), entryParams);
    if (!buffer)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] entryParams allocation failed");
        return;
    }

    helperPath = strtok_r(buffer, "|", &save);
    socketPath = strtok_r(nullptr, "|", &save);
    if (!helperPath || !socketPath)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] invalid entryParams");
        free(buffer);
        return;
    }

    ClearGuestGraphicsEnv();
    for (char* token = strtok_r(nullptr, "|", &save); token; token = strtok_r(nullptr, "|", &save))
        ApplyHostEnv(token);

    OH_LOG_INFO(LOG_APP,
                "[virgl-child] helper=%{public}s socket=%{public}s hostLib=%{public}s egl=%{public}s gles=%{public}s sync=%{public}s",
                helperPath, socketPath,
                getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(unset)",
                getenv("EGL_PLATFORM") ? getenv("EGL_PLATFORM") : "(unset)",
                getenv("VTEST_USE_GLES") ? getenv("VTEST_USE_GLES") : "(unset)",
                getenv("WINEHUA_VIRGL_SYNC_MODE") ? getenv("WINEHUA_VIRGL_SYNC_MODE") : "egl-thread");

    handle = dlopen(helperPath, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] dlopen helper failed: %{public}s", dlerror());
        free(buffer);
        return;
    }

    vtestMain = reinterpret_cast<WinehuaVtestMain>(dlsym(handle, "winehua_vtest_main"));
    if (!vtestMain)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] winehua_vtest_main missing: %{public}s", dlerror());
        dlclose(handle);
        free(buffer);
        return;
    }

    char arg0[] = "virgl_test_server";
    char arg1[] = "--no-fork";
    char arg2[] = "--multi-clients";
    char arg3[] = "--use-egl-surfaceless";
    char arg4[] = "--use-gles";
    char arg5[] = "--socket-path";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, socketPath, nullptr};
    int rc = vtestMain(7, argv);

    OH_LOG_WARN(LOG_APP, "[virgl-child] vtest exited rc=%{public}d", rc);
    dlclose(handle);
    free(buffer);
}
