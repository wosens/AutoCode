#pragma once
// 2026.08.22 崩溃转储（用户需求：崩溃或自动退出时在 <exe 目录>\dump\ 下存 dump 文件）：
//   - SEH 未处理异常（空指针/越界/除零等）→ SetUnhandledExceptionFilter → MiniDumpWriteDump
//   - CRT 级致命路径（abort / purecall / invalid parameter / new 失败 / std::terminate）
//     统一导流到未处理异常过滤器，保证同一份 dump 口径。
//   dump 落 <exe 目录>\dump\AutoCode_YYYYMMDD_HHMMSS_<pid>.dmp，只保 30 份最新（超出自动清）。
namespace CCrashDump {
// 安装全部钩子（main() 最先调用一次即可，幂等）。
void Install();
}
