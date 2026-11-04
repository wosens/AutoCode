// GhostSandbox.dll C ABI 适配层（2026-11-01 沙箱动态库化）。
// 边界契约见 SandboxAbi.h。实现策略：dll 内持进程级 CSandboxManager 单例
// （无 Qt 父对象——生命周期由 sbx_start/sbx_shutdown 显式驱动，与宿主窗口
// 树解耦，避免宿主析构序问题）；JSON 配置串 ↔ SSbxCfg 值对象互转；字符串
// 返回统一 sbx_free 归还（dll 内 new[]，两侧 CRT 刻意同源也按契约走边界释放）。
//
// 本 dll 即「内置幽灵沙箱」本体（B）：收纳/幽灵化/录像/操控全部实现都在
// 本模块内（CSandboxCore/Manager/Recorder/Dialog），宿主只经 C ABI 调用。

#include "SandboxAbi.h"
#include "CSandboxManager.h"
#include "CSandboxDialog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>
#include <cstring>
#include <windows.h>

namespace {

CSandboxManager* g_pMgr = nullptr;   // sbx_start 建一次；shutdown 后留壳（幂等）

CSandboxManager* Mgr() { return g_pMgr; }

// wchar_t*（UTF-16）→ QString（参数串来自宿主 JSON 序列化）。
QString W2Q(const wchar_t* s) { return s ? QString::fromWCharArray(s) : QString(); }

// QString → 沙箱分配串（宿主 sbx_free 归还）。失败/空串返回分配好的空串
// （宿主无需判空，统一 sbx_free）。
wchar_t* Q2W(const QString& s) {
    const int n = s.size();
    wchar_t* p = new wchar_t[n + 1];
    if (n > 0) {
        memcpy(p, s.data(), static_cast<size_t>(n) * sizeof(wchar_t));
    }
    p[n] = L'\0';
    return p;
}

// [Sandbox] 通用配置 JSON → SSbxCfg（缺键回结构体默认）。
CSandboxManager::SSbxCfg CfgFromJson(const QString& sJson) {
    CSandboxManager::SSbxCfg st;
    const QJsonObject o = QJsonDocument::fromJson(sJson.toUtf8()).object();
    st.bEnable     = o.value(QStringLiteral("Enable")).toBool(st.bEnable);
    st.bAutoGhost  = o.value(QStringLiteral("AutoGhost")).toBool(st.bAutoGhost);
    st.bRecord     = o.value(QStringLiteral("Record")).toBool(st.bRecord);
    st.iFps        = o.value(QStringLiteral("Fps")).toInt(st.iFps);
    st.iRetainDays = o.value(QStringLiteral("RetainDays")).toInt(st.iRetainDays);
    st.bBlockSynth = o.value(QStringLiteral("BlockSynth")).toBool(st.bBlockSynth);
    return st;
}

} // namespace

// ============================================================================
// —— 元信息 ——
// ============================================================================
uint32_t sbx_abi_version() { return SBX_ABI_VERSION; }

const wchar_t* sbx_id() {
    return L"GhostSandbox";   // 与 dll 文件名一致（宿主按 [Sandbox] Type 加载）
}

const wchar_t* sbx_display_name() {
    return L"内置幽灵沙箱（Windows GUI 窗口级）";
}

const wchar_t* sbx_description() {
    return L"Agent 后代 GUI 窗口自动幽灵化收纳（不可见/不抢焦点/可截图/消息级输入），"
            L"自动 MP4 录像；sandbox_* 命令通道。窗口级隔离，不隔离文件系统。";
}

// ============================================================================
// —— 生命周期 ——
// ============================================================================
bool sbx_start(const wchar_t* json_cfg) {
    try {   // 2026-11-02 异常屏障（同 sbx_execute：new/Start 异常不得越过 C 边界）
        if (g_pMgr == nullptr) {
            g_pMgr = new CSandboxManager(nullptr);   // 无父：dll 内显式管生命周期
        }
        g_pMgr->SetConfig(CfgFromJson(W2Q(json_cfg)));
        g_pMgr->Start();
        return true;
    } catch (...) {
        return false;
    }
}

bool sbx_reload_config(const wchar_t* json_cfg) {
    if (Mgr() == nullptr) {
        return false;
    }
    Mgr()->SetConfig(CfgFromJson(W2Q(json_cfg)));
    Mgr()->ReloadConfig();
    return true;
}

void sbx_set_project_root(const wchar_t* root) {
    if (Mgr() != nullptr) {
        Mgr()->SetProjectRoot(W2Q(root));
    }
}

void sbx_readopt_project_windows() {
    if (Mgr() != nullptr) {
        Mgr()->ReadoptProjectWindows();
    }
}

void sbx_shutdown() {
    if (g_pMgr != nullptr) {
        g_pMgr->Shutdown();
        // 不 delete：进程退出路径上销毁 Qt 对象树（录像线程/钩子线程收尾序）
        // 风险大于收益；单例随 dll 卸载/进程终止自然消亡。
    }
}

// ============================================================================
// —— 命令通道 ——
// ============================================================================
wchar_t* sbx_execute(const wchar_t* cmd, const wchar_t* json_args, bool* ok) {
    // 2026-11-02 异常屏障：C++ 异常穿越 extern "C" 边界进宿主（QLibrary 函数指针
    //   调用）是 UB——bad_alloc/内部异常一律就地捕获，置 *ok=false 返回错误串。
    try {
        if (Mgr() == nullptr) {
            if (ok != nullptr) {
                *ok = false;
            }
            return Q2W(QStringLiteral("[沙箱未启动]"));
        }
        const QJsonObject oArgs = QJsonDocument::fromJson(W2Q(json_args).toUtf8()).object();
        QString sOut;
        const bool bOk = Mgr()->Execute(W2Q(cmd), oArgs, sOut);   // 阻塞（契约③）
        if (ok != nullptr) {
            *ok = bOk;
        }
        return Q2W(sOut);
    } catch (...) {
        if (ok != nullptr) {
            *ok = false;
        }
        return Q2W(QStringLiteral("[沙箱内部异常]"));
    }
}

void sbx_free(wchar_t* p) {
    delete[] p;
}

// ============================================================================
// —— 状态查询 ——
// ============================================================================
wchar_t* sbx_rows_json() {
    try {   // 2026-11-02 异常屏障（同 sbx_execute：异常不得越过 extern "C" 边界）
    if (Mgr() == nullptr) {
        return Q2W(QStringLiteral("[]"));
    }
    QJsonArray a;
    const auto aRows = Mgr()->Rows();
    for (const auto& r : aRows) {
        QJsonObject o;
        o.insert(QStringLiteral("hwnd"), QString::number(reinterpret_cast<quintptr>(r.h)));
        o.insert(QStringLiteral("pid"), static_cast<double>(r.dwPid));
        o.insert(QStringLiteral("exe"), r.sExe);
        o.insert(QStringLiteral("title"), r.sTitle);
        o.insert(QStringLiteral("ghostAtMs"), static_cast<double>(r.iGhostAtMs));
        o.insert(QStringLiteral("recPath"), r.sRecPath);
        o.insert(QStringLiteral("recording"), r.bRecording);
        a.append(o);
    }
    return Q2W(QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
    } catch (...) {
        return Q2W(QStringLiteral("[]"));
    }
}

// 2026-11-02 修 static 一次性缓存：首次调用发生在 sbx_start 之前（Mgr() 为 null）
//   时将永久返回空串、start 之后也不再刷新。改懒初始化可变静态，Mgr() 就绪后填充。
const wchar_t* sbx_record_dir() {
    static std::wstring s;
    if (s.empty() && Mgr() != nullptr) {
        s = Mgr()->RecordDir().toStdWString();
    }
    return s.c_str();
}

const wchar_t* sbx_shot_dir() {
    static std::wstring s;
    if (s.empty() && Mgr() != nullptr) {
        s = Mgr()->ShotDir().toStdWString();
    }
    return s.c_str();
}

bool sbx_enabled() { return Mgr() != nullptr && Mgr()->IsEnabled(); }
bool sbx_recording_on() { return Mgr() != nullptr && Mgr()->IsRecordingOn(); }
int32_t sbx_fps() { return Mgr() != nullptr ? Mgr()->Fps() : 0; }
int32_t sbx_ghosted_count() { return Mgr() != nullptr ? Mgr()->GhostedCount() : 0; }
bool sbx_is_agent_born_pid(uint32_t pid) {
    return Mgr() != nullptr && Mgr()->IsAgentBornPid(static_cast<DWORD>(pid));
}

// ============================================================================
// —— 豁免/手工登记 ——
// ============================================================================
void sbx_register_user_root(uint32_t pid, void* h_process) {
    CSandboxManager::RegisterUserRoot(static_cast<DWORD>(pid),
                                      static_cast<HANDLE>(h_process));   // 句柄移交
}

void sbx_add_manual_root_pid(uint32_t pid) {
    if (Mgr() != nullptr) {
        Mgr()->AddManualRootPid(static_cast<DWORD>(pid));
    }
}

// ============================================================================
// —— 管理对话框 ——
// ============================================================================
void* sbx_create_dialog(void* parent_qwidget) {
    if (Mgr() == nullptr) {
        return nullptr;
    }
    auto* pDlg = new CSandboxDialog(Mgr(), reinterpret_cast<QWidget*>(parent_qwidget));
    return pDlg;
}
