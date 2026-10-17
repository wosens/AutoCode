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
    // 2026.10.11 进程外写 dump（目标机实害：AutoCode_20261011_161024 dump 損毁——堆
    //   损坏型崩溃里 MiniDumpWriteDump 自身会在破损堆上二次崩溃（本次实测：异常线程栈
    //   全是 dbgcore/ntdll，17 线程栈零 AutoCode 帧，现场全丢）。改为：崩溃进程只负责拉
    //   起健康的助手进程（自身 exe --crash-dump-helper <pid> <tid> <code> <addr>），
    //   由它 OpenProcess 后写 dump——崩溃进程只做栈上操作（CreateProcessA/
    //   WaitForSingleObject，零堆使用），助手进程堆完整，dump 可用性大幅提升。
    //   异常码/地址经命令行传递（记录在 dump 目录 sidecar；异常流不在进程外伪造，
    //   线程栈即现场）。超时兑底 40s 后放行（防死等）。旧进程内路径仅当拉起失败
    //   （极端：exe 被副[A/V]占用/删改）时作兑底。
    char szExe[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, szExe, MAX_PATH) != 0) {
        char szDir[MAX_PATH] = {0};
        std::strncpy(szDir, szExe, MAX_PATH - 1);
        char* pSlash = std::strrchr(szDir, '\\');
        if (pSlash != nullptr) {
            *pSlash = '\0';
            CreateDirectoryA(szDir, nullptr);
            char szDumpDir[MAX_PATH] = {0};
            std::snprintf(szDumpDir, MAX_PATH, "%s\\dump", szDir);
            CreateDirectoryA(szDumpDir, nullptr);
        }
        const DWORD dwPid = GetCurrentProcessId();
        const DWORD dwTid = GetCurrentThreadId();
        const unsigned long long uCode = a_pInfo != nullptr && a_pInfo->ExceptionRecord != nullptr
            ? static_cast<unsigned long long>(a_pInfo->ExceptionRecord->ExceptionCode)
            : 0ull;
        const unsigned long long uAddr = a_pInfo != nullptr && a_pInfo->ExceptionRecord != nullptr
            ? reinterpret_cast<unsigned long long>(a_pInfo->ExceptionRecord->ExceptionAddress)
            : 0ull;
        char szCmd[MAX_PATH * 2] = {0};
        std::snprintf(szCmd, sizeof(szCmd),
                      "\"%s\" --crash-dump-helper %lu %lu %llu %llu",
                      szExe, static_cast<unsigned long>(dwPid),
                      static_cast<unsigned long>(dwTid), uCode, uAddr);
        STARTUPINFOA si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(nullptr, szCmd, nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 40000);
            CloseHandle(pi.hProcess);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    // 兑底：旧进程内路径（拉起失败时）。
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
            char* pSlash2 = std::strrchr(szDir, '\\');
            if (pSlash2 != nullptr) {
                *pSlash2 = '\0';
                PruneOldDumps(szDir);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 交给系统默认处理（WER/退出）
}

// 2026.10.11 进程外 dump 助手主体（由 RunDumpHelper 分流；独立于崩溃进程，堆完整）。
namespace {
int WriteDumpForPid(DWORD a_dwPid, DWORD a_dwTid,
                    unsigned long long a_uCode, unsigned long long a_uAddr) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, a_dwPid);
    if (hProc == nullptr) {
        hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, a_dwPid);
    }
    if (hProc == nullptr) {
        return 2;
    }
    char szDmp[MAX_PATH] = {0};
    if (!MakeDumpPath(szDmp)) {
        CloseHandle(hProc);
        return 3;
    }
    HANDLE hFile = CreateFileA(szDmp, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CloseHandle(hProc);
        return 4;
    }
    const MINIDUMP_TYPE eType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal
        | MiniDumpWithIndirectlyReferencedMemory
        | MiniDumpWithDataSegs
        | MiniDumpWithThreadInfo);
    const BOOL bOk = MiniDumpWriteDump(hProc, a_dwPid, hFile, eType,
                                       nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    CloseHandle(hProc);
    if (!bOk) {
        return 5;
    }
    // 异常码/地址 sidecar 记录（进程外无异常流，栈外补充原始异常信息）。
    char szSide[MAX_PATH] = {0};
    std::snprintf(szSide, MAX_PATH, "%s.txt", szDmp);
    HANDLE hSide = CreateFileA(szSide, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSide != INVALID_HANDLE_VALUE) {
        char szBuf[256];
        const int iN = std::snprintf(szBuf, sizeof(szBuf),
            "pid=%lu tid=%lu code=0x%llX addr=0x%llX\n",
            static_cast<unsigned long>(a_dwPid),
            static_cast<unsigned long>(a_dwTid), a_uCode, a_uAddr);
        DWORD dwWr = 0;
        WriteFile(hSide, szBuf, iN > 0 ? static_cast<DWORD>(iN) : 0, &dwWr, nullptr);
        CloseHandle(hSide);
    }
    char szDir[MAX_PATH] = {0};
    std::strncpy(szDir, szDmp, MAX_PATH - 1);
    char* pSlash = std::strrchr(szDir, '\\');
    if (pSlash != nullptr) {
        *pSlash = '\0';
        PruneOldDumps(szDir);
    }
    return 0;
}
} // namespace

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

int RunDumpHelper(int a_iArgc, char* a_pArgv[]) {
    // --crash-dump-helper <pid> <tid> <code> <addr>：main 最先分流（早于 QApplication，
    //   自身不装过滤器——助手若意外崩溃直接系统默认处理，避免助手拉助手的递归）。
    if (a_iArgc < 6) {
        return 1;
    }
    const DWORD dwPid = static_cast<DWORD>(std::strtoul(a_pArgv[2], nullptr, 0));
    const DWORD dwTid = static_cast<DWORD>(std::strtoul(a_pArgv[3], nullptr, 0));
    const unsigned long long uCode = std::strtoull(a_pArgv[4], nullptr, 0);
    const unsigned long long uAddr = std::strtoull(a_pArgv[5], nullptr, 0);
    if (dwPid == 0) {
        return 1;
    }
    return WriteDumpForPid(dwPid, dwTid, uCode, uAddr);
}

} // namespace CCrashDump
