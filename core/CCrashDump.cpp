#include "CCrashDump.h"

// Windows 专用实现：DbgHelp MiniDumpWriteDump + CRT 致命路径导流。
#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>   // std::set_terminate（<new> 未用且非权威头）

#pragma comment(lib, "dbghelp.lib")

namespace {

// 组 dump 文件名并确保目录存在：<exe 目录>\dump\AutoCode_YYYYMMDD_HHMMSS_<pid>.dmp
bool MakeDumpPath(char a_szOut[MAX_PATH]) {
    char szExe[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, szExe, MAX_PATH) == 0) {
        return false;
    }
    char* pSlash = std::strrchr(szExe, '\\');
    if (pSlash == nullptr) {
        return false;
    }
    *pSlash = '\0';
    char szDir[MAX_PATH] = {0};
    std::snprintf(szDir, MAX_PATH, "%s\\dump", szExe);
    CreateDirectoryA(szDir, nullptr);   // 已存在则失败，无碍

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(a_szOut, MAX_PATH,
                  "%s\\AutoCode_%04d%02d%02d_%02d%02d%02d_%lu.dmp",
                  szDir, st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    return true;
}

// dump 只保最新 30 份：按名字（含时间戳，字典序=时间序）降序清多余的。
void PruneOldDumps(const char* a_pszDir) {
    WIN32_FIND_DATAA fdFind;
    char szPattern[MAX_PATH];
    std::snprintf(szPattern, MAX_PATH, "%s\\AutoCode_*.dmp", a_pszDir);
    HANDLE hFind = FindFirstFileA(szPattern, &fdFind);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }
    // 收集上限须大于保留数（kDumpKeep），否则下方 i>=kDumpKeep 的删除段不可达（历史缺陷：
    //   names[30]+n<30 封顶致清理永不执行，dump 目录无限增长）。
    constexpr int kDumpKeep = 30;      // 保留份数
    constexpr int kScanMax  = 128;     // 单轮收集上限（防超长目录一次占爆栈）
    static_assert(kScanMax > kDumpKeep, "collect cap must exceed keep count");
    char aszNames[kScanMax][64];   // 排序前先收集，再统一删（不能边枚举边删）
    int iCount = 0;
    do {
        if (iCount < kScanMax) {
            std::strncpy(aszNames[iCount], fdFind.cFileName, 63);
            aszNames[iCount][63] = '\0';
            ++iCount;
        }
    } while (FindNextFileA(hFind, &fdFind));
    FindClose(hFind);

    // 简单降序（名字含时间戳，字典序即时间序）。
    for (int i = 0; i < iCount; ++i) {
        for (int j = i + 1; j < iCount; ++j) {
            if (std::strcmp(aszNames[j], aszNames[i]) > 0) {
                char szTmp[64];
                std::strcpy(szTmp, aszNames[i]);
                std::strcpy(aszNames[i], aszNames[j]);
                std::strcpy(aszNames[j], szTmp);
            }
        }
    }
    for (int i = kDumpKeep; i < iCount; ++i) {   // 越界的（更旧）删除
        char szFull[MAX_PATH];
        std::snprintf(szFull, MAX_PATH, "%s\\%s", a_pszDir, aszNames[i]);
        DeleteFileA(szFull);
    }
}

LONG WINAPI UnhandledExFilter(EXCEPTION_POINTERS* a_pInfo) {
    char szDmp[MAX_PATH] = {0};
    if (MakeDumpPath(szDmp)) {
        HANDLE hFile = CreateFileA(szDmp, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = a_pInfo;
            mei.ClientPointers    = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                              static_cast<MINIDUMP_TYPE>(
                                  MiniDumpNormal
                                  | MiniDumpWithIndirectlyReferencedMemory
                                  | MiniDumpWithDataSegs),
                              &mei, nullptr, nullptr);
            CloseHandle(hFile);

            char szDir[MAX_PATH] = {0};
            std::snprintf(szDir, MAX_PATH, "%s", szDmp);
            char* pSlash = std::strrchr(szDir, '\\');
            if (pSlash != nullptr) {
                *pSlash = '\0';
                PruneOldDumps(szDir);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 交给系统默认处理（WER/退出）
}

// CRT 致命路径统一导流到 dump：主动抛一个可识别异常码（0xE0000001 | 子码）。
void __cdecl CrtFatalExit(unsigned int a_uCode) {
    RaiseException(0xE0000001u | (a_uCode & 0xFFFFu), EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

} // namespace

namespace CCrashDump {

void Install() {
    SetUnhandledExceptionFilter(UnhandledExFilter);
    _set_abort_behavior(0, _CALL_REPORTFAULT);          // abort 不弹 WER，走我们过滤器
    signal(SIGABRT, [](int) { CrtFatalExit(1); });      // abort/SIGABRT
    _set_purecall_handler([] { CrtFatalExit(2); });     // 纯虚函数调用
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
            CrtFatalExit(3);                             // CRT 参数非法
        });
    std::set_terminate([] { CrtFatalExit(4); });         // std::terminate（未捕获异常等）
}

} // namespace CCrashDump
