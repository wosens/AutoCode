#pragma once

// 内置测试沙箱（2026-09-26）核心能力层：纯 Win32/COM 工具函数集合，无 Qt 对象状态。
// 提供四类能力，供 CSandboxManager（主实例）与单元测试共用：
//   ① 进程树归属：Agent runner 及其子孙进程（测试程序）的判定依据；
//   ② 幽灵化/还原：alpha=1 + 点击穿透 + 无任务栏/Alt-Tab（窗口保持重绘可截图可投递消息）；
//   ③ 离屏截图 + 消息级输入：PrintWindow(PW_RENDERFULLCONTENT) / PostMessage 系列；
//   ④ UIA 控件树转储：IUIAutomation 控件视图遍历，产出带可点击坐标的文本树。
// 设计要点：绝不移动真实光标、绝不 SendInput/mouse_event——用户全程无感。

#include <QString>
#include <QImage>
#include <QHash>
#include <QJsonObject>
#include <windows.h>

namespace sbx {

// —— 进程树归属 ——
// 全量 pid→ppid 快照（CreateToolhelp32Snapshot；含已死父进程的记录值）。
// 2026-10-14 增补 a_oCreated：pid→创建时刻（Unix ms；0=不可得）。归属链判定
//   要求每一跳「父不晚于子出生」——孤儿进程的真父退出后父 PID 被 Agent 树内
//   新进程复用（构建期短命进程高 churn 场景，实测 TuKan 被误收养），复用者
//   创建时刻必然晚于子 → 链路判伪，杜绝跨树误收纳。
bool BuildParentMap(QHash<DWORD, DWORD>& a_oMap, QHash<DWORD, qint64>& a_oCreated);
// a_dwPid 的父链（快照内）是否可达 a_dwRoot（含死父 PID 记录值上溯；防环上限 64 跳）。
// a_oCreated 每跳校验复用（见 BuildParentMap 注释）；传空表退化为旧行为。
bool IsDescendantOf(DWORD a_dwPid, DWORD a_dwRoot, const QHash<DWORD, DWORD>& a_oMap,
                    const QHash<DWORD, qint64>& a_oCreated = QHash<DWORD, qint64>());
// 2026-10-16 断链判定（IsProjectTestPid 条件③收紧配套）：pid 的父进程是否仍活着。
//   表键=活进程快照、值=快照记录的原始 ppid（父已死仍非 0），故判活必须看父 pid
//   是否仍是表键，不能只看 ppid 非 0。三态：
//   ① ppid 非 0 且父 pid 在表中 → 父活着（explorer/QuickTool/任何启动器）→ true；
//   ② ppid 非 0 但父 pid 不在表中 → 父已死（Agent 经 Start-Process/cmd start 等
//     中间人启动、中间 shell 秒退的断链场景）→ false；
//   ③ 本 pid 不在表中（快照早于进程创建的竞态）→ false（视为断链，交上层
//     <10 分钟年龄闸把关——配合 IsProjectTestPid 判否前的防陈旧重建，实测不可达）。
bool IsParentAlive(DWORD a_dwPid, const QHash<DWORD, DWORD>& a_oMap);
// 进程 exe 全路径（QueryFullProcessImageNameW；失败空串）。
QString ExePathOfPid(DWORD a_dwPid);

// —— 窗口信息 ——
QString WndTitle(HWND a_hWnd);
QString WndClass(HWND a_hWnd);

// —— 幽灵化 / 还原 ——
// 幽灵化：叠加 WS_EX_LAYERED|TRANSPARENT|NOACTIVATE|TOOLWINDOW + alpha=1，
// 任务栏摘除按钮；SetProp 标记（AutoCodeGhost/origEx），供崩溃后新实例还原。
bool GhostWindow(HWND a_hWnd);
// 还原：按标记恢复原 ex 样式（含原本非分层的窗口摘掉 WS_EX_LAYERED），任务栏补回。
bool RestoreWindow(HWND a_hWnd, bool a_bKeepAdoptMark = false);
bool IsGhosted(HWND a_hWnd);
// 2026-10-08 「曾收纳」标记（kPropAdopt）：AutoCode 退出还原时保留，用户主动还原
//   时清除；新实例扫描到它 + 窗口进程 exe 在工程目录内 → 续收纳重新隐形。
// 2026-10-09 值=打标/还原时刻：续收纳只在时效窗（10 分钟）内生效，超龄=Agent
//   早不再测、用户可能在用 → 清标记恢复常态，不再自动收走（修用户实测的
//   「工程内软件窗口反复自动消失/出现」）。
bool HasAdoptMark(HWND a_hWnd);
void ClearAdoptMark(HWND a_hWnd);
ULONG AdoptMarkTick(HWND a_hWnd);            // 打标时刻（GetTickCount 快照）
DWORD GhostOwnerPid(HWND a_hWnd);            // 幽灵中窗口的收纳者 PID（互斥用）
// prop 名导出（单元测试伪造跨实例场景直接 SetProp 用）。
extern const wchar_t* kPropGhost;
extern const wchar_t* kPropAdopt;
// 全屏扫描还原所有带本产品标记的窗口（启动兜底：上次崩溃残留的隐形窗口）。
int  RestoreAllMarked(bool a_bKeepAdoptMark = false);

// —— 收纳前置判定（不做进程树/宿主判定，那两步在 Manager 里做）——
// 可见顶层窗口、非控制台/提示类、尺寸达下限、未被幽灵化、DWM 未 cloak。
bool IsGhostableShape(HWND a_hWnd);
// 系统宿主进程（explorer/dllhost/svchost 等）——其窗口永不收纳。
bool IsSystemHostPid(DWORD a_dwPid);
// 2026-10-09 受保护应用路径：系统/公共应用目录（Windows、Program Files、
//   Program Files (x86)、LocalAppData\Microsoft\WindowsApps）下的 exe。这些目录
//   里的程序（浏览器、Office、记事本等）被进程树内启动几乎都是「Agent 打开给
//   用户看」（看报告/文档/网页）或系统组件，绝不是被测工程程序——自动收纳会让
//   用户正在查看的窗口凭空消失（用户实测），add 收纳也必须拒绝。
bool IsProtectedAppPath(const QString& a_sExe);

// —— 离屏截图 ——
// PrintWindow(PW_RENDERFULLCONTENT) 优先，失败回退 GetWindowDC+BitBlt(CAPTUREBLT)。
QImage CaptureWindow(HWND a_hWnd);

// —— 消息级输入（全部 PostMessage，不动真实键鼠、不激活窗口）——
bool ClickWindow(HWND a_hWnd, int a_iX, int a_iY, bool a_bDouble, bool a_bRight);
bool DragWindow(HWND a_hWnd, int a_iX1, int a_iY1, int a_iX2, int a_iY2, int a_iMs);
bool TypeText(HWND a_hWnd, const QString& a_sText);          // 逐 UTF-16 码元 WM_CHAR
bool SendKeys(HWND a_hWnd, const QString& a_sCombo);         // "ctrl+shift+s"
bool SendWmCommand(HWND a_hWnd, int a_iCmd);                 // 菜单命令 WM_COMMAND
bool ClickButton(HWND a_hCtrl);                              // 按钮 BM_CLICK

// —— UIA 控件树 ——
// 控件视图遍历转文本：缩进+[类型] 名称 (类名) [wxh @x,y] click=(cx,cy)（客户区坐标）。
// 上限：深度 24 / 节点 1600 / 文本 16KB（防巨型树撑爆上下文）。
QString DumpUiTree(HWND a_hWnd, int a_iMaxDepth = 24);

// —— 杂项 ——
bool ParseHwnd(const QString& a_sText, HWND* a_phOut);       // 十进制/0x 前缀十六进制
quint32 VkOfName(const QString& a_sName);                    // 键名(ctrl/enter/f1/a)→VK，未知 0

// —— CLI 参数解析（--sandbox 模式，main.cpp 与测试共用）——
// a_aArgs：完整命令行（含 argv[0] 与 "--sandbox"）。成功拆出 cmd + 参数 JSON。
// 支持：windows/status/record_on/record_off/restore-all（无参）；
//   capture [hwnd] [outfile]；click hwnd x y [dbl|right]；drag hwnd x1 y1 x2 y2 [ms]；
//   type hwnd text...；keys hwnd combo；command hwnd id；button ctrl；
//   tree hwnd；add hwnd；restore hwnd；close hwnd。
bool ParseCli(const QStringList& a_aArgs, QString* a_psCmd, QJsonObject* a_pOut);

} // namespace sbx
