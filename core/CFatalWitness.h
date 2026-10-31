#pragma once

// 2026.10.28 qFatal 见证（用户跨机器反馈：AutoCode 当日多次零痕迹消失，任务正跑）。
//   Qt 6.5+ 的 qFatal/qAbort 在 Windows 直接 __fastfail（int 29h）：绕过 SEH 未处理
//   异常过滤器（CCrashDump 收不到）、绕过 CRT abort/purecall/terminate 钩子、多数情
//   况绕过 WER——进程瞬间消失零痕迹。qInstallMessageHandler 注册的 handler 位于
//   qFatal 调用链最前端（先于 qAbort），在此拦 QtFatalMsg 写"遗言"文件后放行默认
//   处理，把零痕迹死亡变成有痕迹死亡（QtFatalMsg 文本含 qFatal 原因，如
//   "QThread: Destroyed while thread ... is still running"）。
//   非 Qt 的 __fastfail（/GS cookie、UCRT abort 新路径）仍零痕迹——由 WER LocalDumps
//   注册表兜底（对 0xC0000409 多数能收到 dump），见 scripts/install_env.bat。
namespace CFatalWitness {
void Install();   // main() 里 CCrashDump::Install() 之后调用一次（先于一切 QApplication/模式分支）
}
