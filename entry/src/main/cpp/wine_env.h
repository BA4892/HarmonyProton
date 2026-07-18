#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <cstdlib>
#include <string>
#include <vector>

// -- Box64 性能调优 (static inline, 供 napi_init / wine_child 共用) --
static inline void SetBox64PerfEnv() {
    setenv("BOX64_LOG", "0", 1);
    setenv("BOX64_NOBANNER", "1", 1);
    setenv("BOX64_SHOWSEGV", "1", 1);
    setenv("BOX64_DYNAREC_SAFEFLAGS", "0", 1);
    setenv("BOX64_DYNAREC_BIGBLOCK", "3", 1);
    setenv("BOX64_DYNAREC_CALLRET", "2", 1);
    setenv("BOX64_DYNAREC_FORWARD", "1024", 1);
    setenv("BOX64_DYNAREC_WEAKBARRIER", "2", 1);
    setenv("BOX64_AVX", "0", 1);
    // 关闭 box64 的 PE Volatile Metadata 解析 (默认开启), 原因:
    // box64 的 my_mmap64 (wrappedlibc.c) 对 Wine 每个首次 mmap 的文件调用
    // ParseVolatileMetadata (src/tools/pe_tools.c), 该函数只校验 MZ 魔数、
    // 不校验 e_lfanew 边界。DOS/16位 MZ 可执行文件 (非 PE, 如仙剑 DOS 版的
    // PAL!.EXE / DJGPP 工具 exe) 的 0x3C 处是 DOS stub 文本 (" by "/"mail"),
    // 被当作 e_lfanew 偏移 → base+~545MB 越界解引用 → 宿主侧 SIGSEGV →
    // 被 Wine SEH 当客体异常恢复, 撕裂 my_mmap64 宿主调用栈 → explorer
    // 浏览含 DOS exe 的目录 (图标提取逐个 mmap) 连炸后挂死 (2026-07 实测)。
    // 副作用≈0: 本项目 STRONGMEM=0, 该元数据唯一生效消费点是给新 MSVC
    // (2019 16.10+) 编译的 PE 标注点额外加 DMB_ISHST 屏障 (正确性增强,
    // 非性能优化); 老游戏无此元数据, lock/原子指令走独立路径不受影响。
    // 若日后 fork 内给 pe_tools.c 补上边界检查, 可移除此行重新启用。
    setenv("BOX64_DYNAREC_VOLATILE_METADATA", "0", 1);
}

inline void AppendBox64PerfStrings(std::vector<std::string>& env) {
    env.push_back("BOX64_LOG=0");
    env.push_back("BOX64_NOBANNER=1");
    env.push_back("BOX64_SHOWSEGV=1");
    env.push_back("BOX64_DYNAREC_SAFEFLAGS=0");
    env.push_back("BOX64_DYNAREC_BIGBLOCK=3");
    env.push_back("BOX64_DYNAREC_CALLRET=2");
    env.push_back("BOX64_DYNAREC_FORWARD=1024");
    env.push_back("BOX64_DYNAREC_WEAKBARRIER=2");
    env.push_back("BOX64_AVX=0");
    // 关闭 Volatile Metadata 解析, 详细原因见上方 SetBox64PerfEnv() 注释
    // (box64 pe_tools.c 对 DOS MZ exe 无边界检查 → explorer 浏览目录挂死)
    env.push_back("BOX64_DYNAREC_VOLATILE_METADATA=0");
}

// -- Wine 环境变量构建 --
std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir);

// -- Audio bootstrap --
int CreateAudioBootstrapFd(const std::string& runtimeDir);

// -- entryParams 序列化 --
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env);

// -- Graphics 辅助 --
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
