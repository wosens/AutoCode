#pragma once

// ============================================================================
// 测试沙箱动态库 C ABI（2026-11-01 沙箱动态库化）
//
// 架构：AutoCode.exe（宿主）在启动时按 config.ini [Sandbox] Type 加载
//   <exe目录>\TestSandbox\<Type>.dll（QLibrary 运行期解析，缺失优雅降级——
//   沙箱整体不可用，主程序其余功能不受影响）。每个沙箱 = 一个 dll +
//   同名私有目录（TestSandbox\<Type>\，存放该沙箱的依赖与自有配置）。
//
// 约定（接入方必读）：
//   ① 本头文件是宿主与沙箱的唯一边界。所有导出函数为 extern "C"（跨编译器
//      ABI 稳定）；实现方用 .def 或 __declspec(dllexport) 导出全部符号。
//   ② 字符串一律 UTF-16（wchar_t*）。凡「沙箱分配、宿主使用」的返回串，
//      宿主用毕必须调 sbx_free() 归还——两侧 CRT 可能不同，禁止跨界 free。
//   ③ sbx_execute 与旧 CSandboxManager::Execute 同语义：可能长阻塞（UIA 遍历/
//      PrintWindow 截图/同步点击），宿主必须在工作线程调用（现有实现：线程池
//      + MTA COM），绝不在 UI 线程直调。
//   ④ 线程模型：宿主可多线程并发调用 sbx_execute；沙箱实现内部自行串行化
//      （参考实现 CSandboxManager::m_mtxState）。生命周期函数（start/shutdown/
//      reload_config）只在宿主主线程调用，与 execute 之间由宿主保证时序
//      （shutdown 前 drain 在途 execute，宿主负责）。
//   ⑤ 首个被调用的函数必须是 sbx_abi_version()：宿主比对 SBX_ABI_VERSION，
//      不匹配立即卸载，绝不调用其余符号（前向兼容的唯一闸门）。
//   ⑥ 沙箱内部如需 UI（管理对话框），由 sbx_create_dialog 创建并返回原生
//      QDialog 指针（void* 传递）；对话框自身（含刷新定时/信号连接）留在
//      沙箱内部，宿主只负责 show/raise/生命周期兜底（WA_DeleteOnClose）。
//   ⑦ JSON 配置（sbx_start/sbx_reload_config 的入参）为宿主 [Sandbox] 节
//      通用键：{"Enable":bool,"AutoGhost":bool,"Record":bool,"Fps":int,
//      "RetainDays":int}。沙箱私有配置应放 TestSandbox\<Type>\ 内自读，
//      不得要求宿主转发。
// ============================================================================

#include <cstdint>

// ABI 版本：只在「不兼容的函数签名/语义变更」时 +1 并同步文档；
// 增量新增函数（尾插）不破坏兼容，宿主按符号是否存在渐进探测。
#define SBX_ABI_VERSION 1

extern "C" {

// —— 元信息（加载后最先探测；任何失败宿主立即卸载）——
__declspec(dllexport) uint32_t    sbx_abi_version();
// 沙箱标识（与 dll 文件名一致，如 "GhostSandbox"；宿主按它路由 [Sandbox] Type）。
__declspec(dllexport) const wchar_t* sbx_id();
// 显示名（设置页下拉用，如「内置幽灵沙箱（Windows GUI 窗口级）」）。
__declspec(dllexport) const wchar_t* sbx_display_name();
// 一句话能力描述（设置页 tooltip）。
__declspec(dllexport) const wchar_t* sbx_description();

// —— 生命周期 ——
// 初始化并启动（返回 false=启动失败，宿主置沙箱不可用）。jsonCfg 见约定⑦。
__declspec(dllexport) bool sbx_start(const wchar_t* json_cfg);
// 配置热应用（设置页保存后）。
__declspec(dllexport) bool sbx_reload_config(const wchar_t* json_cfg);
// 打开/切换工程时刷新工程根（续收纳/兑底判定用；空串=无工程）。
__declspec(dllexport) void sbx_set_project_root(const wchar_t* root);
// 扫描带「曾收纳」标记且属于当前工程的窗口，续收纳（切工程/重启后调用）。
__declspec(dllexport) void sbx_readopt_project_windows();
// 停止并还原全部（退出前必调；幂等）。
__declspec(dllexport) void sbx_shutdown();

// —— 命令通道（MCP sandbox_* 工具与 --sandbox CLI 的统一落点）——
// cmd 小写命令名；json_args 参数对象；*ok 出参。返回多行文本（宿主 sbx_free）。
__declspec(dllexport) wchar_t* sbx_execute(const wchar_t* cmd, const wchar_t* json_args,
                                           bool* ok);
// 归还沙箱分配的字符串（execute/rows_json/record_dir/shot_dir 的返回值）。
__declspec(dllexport) void sbx_free(wchar_t* p);

// —— 状态查询（管理界面/诊断用；均返回静态或须 sbx_free 的串）——
// 收纳窗口表 JSON 数组：[{"hwnd":dec,"pid":dec,"exe":"","title":"",
//   "ghostAtMs":dec,"recPath":"","recording":bool},...]
__declspec(dllexport) wchar_t* sbx_rows_json();
__declspec(dllexport) const wchar_t* sbx_record_dir();   // 录像目录（静态串）
__declspec(dllexport) const wchar_t* sbx_shot_dir();     // 截图目录（静态串）
__declspec(dllexport) bool sbx_enabled();
__declspec(dllexport) bool sbx_recording_on();
__declspec(dllexport) int32_t sbx_fps();
__declspec(dllexport) int32_t sbx_ghosted_count();
// 诊断：进程是否在出生登记表内（Agent 后代判定，日志排查用）。
__declspec(dllexport) bool sbx_is_agent_born_pid(uint32_t pid);

// —— 豁免/手工登记（宿主 UI 侧调用）——
// 用户主动启动的进程登记豁免（资源管理器打开文件等；hProcess 句柄移交沙箱持有）。
__declspec(dllexport) void sbx_register_user_root(uint32_t pid, void* h_process);
// 手工根登记（测试/特殊场景：该 pid 及其后代视同 Agent 树）。
__declspec(dllexport) void sbx_add_manual_root_pid(uint32_t pid);

// —— 管理对话框 ——
// 创建本沙箱的管理对话框（parent 为宿主主窗口 QWidget*；返回 QDialog*，
//   宿主 setAttribute(WA_DeleteOnClose) 后 show；空返回=该沙箱无管理界面）。
__declspec(dllexport) void* sbx_create_dialog(void* parent_qwidget);

} // extern "C"
