#pragma once

// 测试沙箱 CLI 参数解析（2026-11-01 沙箱动态库化拆出）。
// --sandbox 子命令 → (cmd + 参数 JSON) 的纯字符串解析，无 Win32 状态、无 Qt 对象，
// 供三方共用（exe 的 RunSandboxMode 子进程模式、GhostSandbox.dll、单元测试）——
// 解析必须在「不加载任何沙箱动态库」的环境可用（CLI 子进程只转发不实现），
// 故从 CSandboxCore 拆出独立编译单元。

#include <QString>
#include <QJsonObject>

namespace sbx {

// 键名(ctrl/enter/f1/a...)→虚拟键码；未知返回 0。SendKeys（Core）与 ParseCli 共用。
quint32 VkOfName(const QString& a_sName);

// a_aArgs：完整命令行（含 argv[0] 与 "--sandbox"）。成功拆出 cmd + 参数 JSON。
// 支持：windows/status/record_on/record_off/restore-all（无参）；
//   capture [hwnd] [outfile]；click hwnd x y [dbl|right]；drag hwnd x1 y1 x2 y2 [ms]；
//   type hwnd text...；keys hwnd combo；command hwnd id；button ctrl；
//   tree hwnd；add hwnd；restore hwnd；close hwnd；launch exe [args...]。
bool ParseCli(const QStringList& a_aArgs, QString* a_psCmd, QJsonObject* a_pOut);

} // namespace sbx
