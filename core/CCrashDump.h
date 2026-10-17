#pragma once

// 崩溃 dump：安装 SEH/CRT 致命路径过滤器（详见 CCrashDump.cpp）。
namespace CCrashDump {

void Install();

// 2026.10.11 进程外 dump 助手：主程序带 --crash-dump-helper <pid> <tid> <code> <addr>
//   启动时在 main 最先分流到这里（早于 QApplication/任何堆使用）。返回进程退出码。
int RunDumpHelper(int a_iArgc, char* a_pArgv[]);

} // namespace CCrashDump
