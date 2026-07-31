#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

typedef struct WindowSearch {
    DWORD processId;
    const wchar_t* titlePrefix;
    HWND result;
} WindowSearch;

typedef struct ButtonSearch {
    const wchar_t* text;
    HWND result;
} ButtonSearch;

static void LogMessage(const wchar_t* message)
{
    FILE* file = fopen("C:\\windows\\temp\\winehua_win32_driver.log", "a");
    if (file) {
        fwprintf(file, L"%lu %ls\n", GetTickCount(), message);
        fclose(file);
    }
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

static BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter)
{
    WindowSearch* search = (WindowSearch*)parameter;
    DWORD processId = 0;
    wchar_t title[512];
    GetWindowThreadProcessId(window, &processId);
    if ((search->processId && processId != search->processId) || !IsWindowVisible(window)) return TRUE;
    if (!GetWindowTextW(window, title, (int)(sizeof(title) / sizeof(title[0])))) return TRUE;
    if (wcsncmp(title, search->titlePrefix, wcslen(search->titlePrefix)) != 0) return TRUE;
    search->result = window;
    return FALSE;
}

static BOOL CALLBACK FindButtonCallback(HWND window, LPARAM parameter)
{
    ButtonSearch* search = (ButtonSearch*)parameter;
    wchar_t className[64];
    wchar_t text[256];
    if (!GetClassNameW(window, className, (int)(sizeof(className) / sizeof(className[0]))))
        return TRUE;
    if (_wcsicmp(className, L"Button") != 0) return TRUE;
    if (!GetWindowTextW(window, text, (int)(sizeof(text) / sizeof(text[0])))) return TRUE;
    if (wcscmp(text, search->text) != 0) return TRUE;
    search->result = window;
    return FALSE;
}

static HWND DeepestChildAtPoint(HWND window, POINT screenPoint)
{
    HWND current = window;
    for (;;) {
        POINT clientPoint = screenPoint;
        HWND child;
        if (!ScreenToClient(current, &clientPoint)) break;
        child = ChildWindowFromPointEx(current, clientPoint,
            CWP_SKIPDISABLED | CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == current) break;
        current = child;
    }
    return current;
}

static BOOL SendRelativeClientClick(HWND window, int xPermille, int yPermille)
{
    RECT clientRect;
    POINT screenPoint;
    POINT targetPoint;
    HWND target;
    wchar_t className[128] = L"";
    wchar_t message[320];

    if (!GetClientRect(window, &clientRect)) return FALSE;
    screenPoint.x = clientRect.left +
        (clientRect.right - clientRect.left) * xPermille / 1000;
    screenPoint.y = clientRect.top +
        (clientRect.bottom - clientRect.top) * yPermille / 1000;
    if (!ClientToScreen(window, &screenPoint)) return FALSE;

    target = DeepestChildAtPoint(window, screenPoint);
    targetPoint = screenPoint;
    if (!ScreenToClient(target, &targetPoint)) return FALSE;
    GetClassNameW(target, className, (int)(sizeof(className) / sizeof(className[0])));

    swprintf(message, sizeof(message) / sizeof(message[0]),
        L"relative click target parent=%p target=%p class=%ls screen=%ld,%ld target=%ld,%ld",
        window, target, className, screenPoint.x, screenPoint.y,
        targetPoint.x, targetPoint.y);
    LogMessage(message);

    SetForegroundWindow(window);
    if (!PostMessageW(target, WM_MOUSEMOVE, 0,
            MAKELPARAM(targetPoint.x, targetPoint.y)) ||
        !PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(targetPoint.x, targetPoint.y)) ||
        !PostMessageW(target, WM_LBUTTONUP, 0,
            MAKELPARAM(targetPoint.x, targetPoint.y)))
        return FALSE;

    swprintf(message, sizeof(message) / sizeof(message[0]),
        L"relative click messages posted target=%p", target);
    LogMessage(message);
    return TRUE;
}

static wchar_t* ParentDirectory(const wchar_t* path)
{
    size_t length = wcslen(path);
    wchar_t* result = (wchar_t*)HeapAlloc(GetProcessHeap(), 0,
                                          (length + 1) * sizeof(wchar_t));
    wchar_t* slash;
    if (!result) return NULL;
    wcscpy(result, path);
    slash = wcsrchr(result, L'\\');
    if (!slash) slash = wcsrchr(result, L'/');
    if (slash) *slash = L'\0';
    else result[0] = L'\0';
    return result;
}

static int RunDriver(int argc, wchar_t** argv)
{
    const wchar_t* launchPath = NULL;
    const wchar_t* titlePrefix = NULL;
    const wchar_t* buttonText = NULL;
    BOOL attachOnly = FALSE;
    int clickXPermille = -1;
    int clickYPermille = -1;
    BOOL useClientClick;
    DWORD timeoutMs = 30000;
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {0};
    wchar_t* commandLine;
    wchar_t* workingDirectory;
    DWORD deadline;
    WindowSearch windowSearch;
    ButtonSearch buttonSearch;
    DWORD_PTR clickResult = 0;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--launch") && i + 1 < argc) launchPath = argv[++i];
        else if (!wcscmp(argv[i], L"--attach")) attachOnly = TRUE;
        else if (!wcscmp(argv[i], L"--title-prefix") && i + 1 < argc) titlePrefix = argv[++i];
        else if (!wcscmp(argv[i], L"--button-text") && i + 1 < argc) buttonText = argv[++i];
        else if (!wcscmp(argv[i], L"--client-x-permille") && i + 1 < argc)
            clickXPermille = wcstol(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--client-y-permille") && i + 1 < argc)
            clickYPermille = wcstol(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--timeout-ms") && i + 1 < argc)
            timeoutMs = wcstoul(argv[++i], NULL, 10);
    }
    useClientClick = clickXPermille >= 0 && clickYPermille >= 0;
    if ((!attachOnly && !launchPath) || !titlePrefix ||
        (!buttonText && !useClientClick) ||
        (clickXPermille >= 0) != (clickYPermille >= 0) ||
        clickXPermille > 1000 || clickYPermille > 1000 ||
        timeoutMs < 1000 || timeoutMs > 120000) {
        LogMessage(L"invalid arguments");
        return 2;
    }

    if (!attachOnly) {
        commandLine = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            (wcslen(launchPath) + 3) * sizeof(wchar_t));
        workingDirectory = ParentDirectory(launchPath);
        if (!commandLine || !workingDirectory) return 3;
        swprintf(commandLine, wcslen(launchPath) + 3, L"\"%ls\"", launchPath);
        if (!CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL,
                            workingDirectory[0] ? workingDirectory : NULL,
                            &startup, &process)) {
            LogMessage(L"CreateProcessW failed");
            HeapFree(GetProcessHeap(), 0, commandLine);
            HeapFree(GetProcessHeap(), 0, workingDirectory);
            return 4;
        }
        HeapFree(GetProcessHeap(), 0, commandLine);
        HeapFree(GetProcessHeap(), 0, workingDirectory);
        WaitForInputIdle(process.hProcess, 10000);
    }

    windowSearch.processId = attachOnly ? 0 : process.dwProcessId;
    windowSearch.titlePrefix = titlePrefix;
    windowSearch.result = NULL;
    buttonSearch.text = buttonText;
    buttonSearch.result = NULL;
    deadline = GetTickCount() + timeoutMs;
    while ((LONG)(deadline - GetTickCount()) > 0) {
        windowSearch.result = NULL;
        EnumWindows(FindWindowCallback, (LPARAM)&windowSearch);
        if (windowSearch.result) {
            buttonSearch.result = NULL;
            if (buttonText)
                EnumChildWindows(windowSearch.result, FindButtonCallback, (LPARAM)&buttonSearch);
            if (buttonSearch.result || useClientClick) break;
        }
        if (process.hProcess && WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) break;
        if (!process.hProcess) Sleep(100);
    }
    if (!windowSearch.result || (!buttonSearch.result && !useClientClick)) {
        LogMessage(L"target window or button not found");
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        return 5;
    }

    if (!buttonSearch.result) {
        if (!SendRelativeClientClick(
                windowSearch.result, clickXPermille, clickYPermille)) {
            LogMessage(L"relative client click failed or timed out");
            if (process.hThread) CloseHandle(process.hThread);
            if (process.hProcess) CloseHandle(process.hProcess);
            return 6;
        }
    } else {
        SetForegroundWindow(windowSearch.result);
        if (!SendMessageTimeoutW(buttonSearch.result, BM_CLICK, 0, 0,
                                 SMTO_ABORTIFHUNG, 5000, &clickResult)) {
            LogMessage(L"BM_CLICK failed or timed out");
            if (process.hThread) CloseHandle(process.hThread);
            if (process.hProcess) CloseHandle(process.hProcess);
            return 6;
        }
        LogMessage(L"BM_CLICK sent");
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int result;
    (void)instance;
    (void)previous;
    (void)commandLine;
    (void)showCommand;
    if (!argv) return 1;
    result = RunDriver(argc, argv);
    LocalFree(argv);
    return result;
}
