// 测试沙箱宿主门面实现（说明见同名 .h；2026-11-01 动态库化，2026-11-02 双槽路由）。
#include "CSandboxHost.h"
#include "SandboxAbi.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDialog>
#include <QList>
#include <windows.h>

// —— 进程单例 ——
static CSandboxHost* g_pHost = nullptr;
CSandboxHost* CSandboxHost::Instance() { return g_pHost; }
void CSandboxHost::SetInstance(CSandboxHost* a_p) { g_pHost = a_p; }

// exe 同级 TestSandbox 目录定位（便携原则：GetModuleFileName 取路径）。
// 2026-11-01 修：GetModuleFileNameW 返回反斜杠路径，原 lastIndexOf('/') 恒 -1
//   → SandboxDir 返回空 → 枚举/加载全废（用户实测设置页无法选择沙箱）。
static QString SandboxDir() {
    WCHAR wsz[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, wsz, MAX_PATH);
    const QString sExe = QString::fromWCharArray(wsz);
    const int iSlash = sExe.lastIndexOf(QLatin1Char('/'));
    const int iBack  = sExe.lastIndexOf(QLatin1Char('\\'));
    const int i = qMax(iSlash, iBack);
    if (i < 0) {
        return QString();
    }
    return sExe.left(i) + QStringLiteral("/TestSandbox");
}

QList<CSandboxHost::SProvider> CSandboxHost::EnumSandboxes(QString* psErr) {
    QList<SProvider> aOut;
    const QString sDir = SandboxDir();
    if (!QDir(sDir).exists()) {
        if (psErr != nullptr) {
            *psErr = QStringLiteral("TestSandbox 目录不存在");
        }
        return aOut;
    }
    const QStringList aDlls = QDir(sDir).entryList(
        { QStringLiteral("*.dll") }, QDir::Files, QDir::Name);
    for (const QString& sDll : aDlls) {
        QLibrary lib(sDir + QLatin1Char('/') + sDll);
        if (!lib.load()) {
            if (psErr != nullptr) {
                *psErr = QStringLiteral("%1 加载失败：%2").arg(sDll, lib.errorString());
            }
            continue;
        }
        auto fnAbi = reinterpret_cast<uint32_t (*)()>(lib.resolve("sbx_abi_version"));
        auto fnId  = reinterpret_cast<const wchar_t* (*)()>(lib.resolve("sbx_id"));
        auto fnName = reinterpret_cast<const wchar_t* (*)()>(lib.resolve("sbx_display_name"));
        if (fnAbi == nullptr || fnId == nullptr || fnName == nullptr) {
            if (psErr != nullptr) {
                *psErr = QStringLiteral("%1 缺少必需导出（非测试沙箱库？）").arg(sDll);
            }
            lib.unload();
            continue;
        }
        if (fnAbi() != SBX_ABI_VERSION) {
            if (psErr != nullptr) {
                *psErr = QStringLiteral("%1 ABI 版本不匹配（%2 ≠ 宿主 %3）")
                             .arg(sDll).arg(fnAbi()).arg(SBX_ABI_VERSION);
            }
            lib.unload();
            continue;
        }
        SProvider st;
        st.sId = QString::fromWCharArray(fnId());
        st.sDisplayName = QString::fromWCharArray(fnName());
        aOut.append(st);
        lib.unload();   // 枚举即卸载；正式加载由 Load() 负责
    }
    return aOut;
}

CSandboxHost::CSandboxHost(QObject* a_pParent) : QObject(a_pParent) {
    g_pHost = this;
}

CSandboxHost::~CSandboxHost() {
    if (g_pHost == this) {
        g_pHost = nullptr;
    }
    // 2026-11-02 补关停协议：与 LoadSingle 卸旧库路径一致——析构直接 unload 会
    //   跳过沙箱内部清理（收纳窗口还原、登记句柄、线程收尾）。
    m_stWin.Unload();
    m_stVm.Unload();
}

// 2026-11-02 卸载/加载失败路径统一清零：全部 fn* 一并置空（释放即置空纪律），
//   旧实现只清 fnAbi，其余函数指针悬垂指向已卸载 DLL——当前靠 Ready() 单点
//   守护未解引用，新增调用点极易踩中野指针。
void CSandboxHost::SSlot::ResetFnPtrs() {
    fnAbi = nullptr;
    fnId = nullptr;
    fnDisplayName = nullptr;
    fnDescription = nullptr;
    fnStart = nullptr;
    fnReload = nullptr;
    fnSetRoot = nullptr;
    fnReadopt = nullptr;
    fnShutdown = nullptr;
    fnExecute = nullptr;
    fnFree = nullptr;
    fnRegUserRoot = nullptr;
    fnCreateDialog = nullptr;
}

void CSandboxHost::SSlot::Unload() {
    if (pLib != nullptr) {
        if (fnShutdown != nullptr) {
            fnShutdown();
        }
        pLib->unload();
        delete pLib;
        pLib = nullptr;
        ResetFnPtrs();
    }
}

template <typename T>
T CSandboxHost::Resolve(SSlot& a_st, const char* a_sName) {
    return reinterpret_cast<T>(a_st.pLib->resolve(a_sName));
}

bool CSandboxHost::Load(const SSbxCfg& a_stCfg, QString* a_psErr) {
    m_stCfg = a_stCfg;
    QStringList aErrs;
    // —— 窗口槽：GUI 测试主力；配置类型加载失败且非 GhostSandbox 时回落
    //    GhostSandbox 保住窗口收纳链路（2026-11-01 实害教训的槽位化版本）。 ——
    m_stWin.bEnable = a_stCfg.bWindowEnable;
    m_stWin.sType   = a_stCfg.sWindowType;
    if (m_stWin.bEnable) {
        const QString sWant = m_stWin.sType.isEmpty()
                                  ? QStringLiteral("GhostSandbox") : m_stWin.sType;
        const QString sErr = LoadSingle(m_stWin, sWant, a_stCfg);
        if (!sErr.isEmpty() && sWant != QLatin1String("GhostSandbox")) {
            const QString sErrFallback =
                LoadSingle(m_stWin, QStringLiteral("GhostSandbox"), a_stCfg);
            if (sErrFallback.isEmpty()) {
                aErrs.append(QStringLiteral("窗口沙箱 %1 不可用，已回落 GhostSandbox（GUI 测试照常）")
                                 .arg(sWant));
            } else {
                aErrs.append(QStringLiteral("窗口沙箱 %1：%2；回落也失败：%3")
                                 .arg(sWant, sErr, sErrFallback));
            }
        } else if (!sErr.isEmpty()) {
            aErrs.append(QStringLiteral("窗口沙箱 %1：%2").arg(sWant, sErr));
        }
    }
    // —— VM 槽：Linux microVM；失败只降级本槽（vm_* 命令不可用），不影响窗口槽。 ——
    m_stVm.bEnable = a_stCfg.bVmEnable;
    m_stVm.sType   = a_stCfg.sVmType;
    if (m_stVm.bEnable) {
        const QString sWant = m_stVm.sType.isEmpty()
                                  ? QStringLiteral("MsbSandbox") : m_stVm.sType;
        const QString sErr = LoadSingle(m_stVm, sWant, a_stCfg);
        if (!sErr.isEmpty()) {
            aErrs.append(QStringLiteral("VM 沙箱 %1 不可用（vm_* 命令将拒绝）：%2")
                             .arg(sWant, sErr));
        }
    }
    m_sErr = aErrs.join(QStringLiteral("；"));
    if (a_psErr != nullptr) {
        *a_psErr = m_sErr;
    }
    return m_stWin.Ready() || m_stVm.Ready();
}

QString CSandboxHost::LoadSingle(SSlot& a_st, const QString& a_sId, const SSbxCfg& a_stCfg) {
    // 重载场景：先卸旧库（正常启动只调一次）。
    a_st.Unload();
    const QString sPath = SandboxDir() + QLatin1Char('/') + a_sId + QStringLiteral(".dll");
    auto* pLib = new QLibrary(sPath);
    if (!pLib->load()) {
        const QString sErr = QStringLiteral("加载 %1 失败：%2")
                                 .arg(sPath, pLib->errorString());
        delete pLib;
        return sErr;
    }
    auto fnAbi = reinterpret_cast<uint32_t (*)()>(pLib->resolve("sbx_abi_version"));
    if (fnAbi == nullptr) {
        pLib->unload();
        delete pLib;
        return QStringLiteral("%1 无 sbx_abi_version 导出").arg(a_sId);
    }
    if (fnAbi() != SBX_ABI_VERSION) {
        pLib->unload();
        delete pLib;
        return QStringLiteral("%1 ABI 版本 %2 ≠ 宿主 %3")
                   .arg(a_sId).arg(fnAbi()).arg(SBX_ABI_VERSION);
    }
    a_st.pLib = pLib;
    a_st.fnAbi          = fnAbi;
    a_st.fnId           = Resolve<const wchar_t* (*)()>(a_st, "sbx_id");
    a_st.fnDisplayName  = Resolve<const wchar_t* (*)()>(a_st, "sbx_display_name");
    a_st.fnDescription  = Resolve<const wchar_t* (*)()>(a_st, "sbx_description");
    a_st.fnStart        = Resolve<bool (*)(const wchar_t*)>(a_st, "sbx_start");
    a_st.fnReload       = Resolve<bool (*)(const wchar_t*)>(a_st, "sbx_reload_config");
    a_st.fnSetRoot      = Resolve<void (*)(const wchar_t*)>(a_st, "sbx_set_project_root");
    a_st.fnReadopt      = Resolve<void (*)()>(a_st, "sbx_readopt_project_windows");
    a_st.fnShutdown     = Resolve<void (*)()>(a_st, "sbx_shutdown");
    a_st.fnExecute      = Resolve<wchar_t* (*)(const wchar_t*, const wchar_t*, bool*)>(a_st, "sbx_execute");
    a_st.fnFree         = Resolve<void (*)(wchar_t*)>(a_st, "sbx_free");
    a_st.fnRegUserRoot  = Resolve<void (*)(uint32_t, void*)>(a_st, "sbx_register_user_root");
    a_st.fnCreateDialog = Resolve<void* (*)(void*)>(a_st, "sbx_create_dialog");
    if (a_st.fnStart == nullptr || a_st.fnExecute == nullptr || a_st.fnFree == nullptr
            || a_st.fnShutdown == nullptr) {
        a_st.pLib = nullptr;
        a_st.ResetFnPtrs();
        pLib->unload();
        delete pLib;
        return QStringLiteral("%1 缺少核心导出（start/execute/free/shutdown）").arg(a_sId);
    }
    a_st.sId = a_sId;
    if (!a_st.fnStart(reinterpret_cast<const wchar_t*>(CfgJson().utf16()))) {
        a_st.pLib = nullptr;
        a_st.ResetFnPtrs();
        pLib->unload();
        delete pLib;
        return QStringLiteral("%1 sbx_start 失败").arg(a_sId);
    }
    a_st.sErr.clear();
    return QString();
}

QString CSandboxHost::CfgJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("Enable"), m_stCfg.bEnable);
    o.insert(QStringLiteral("AutoGhost"), m_stCfg.bAutoGhost);
    o.insert(QStringLiteral("Record"), m_stCfg.bRecord);
    o.insert(QStringLiteral("Fps"), m_stCfg.iFps);
    o.insert(QStringLiteral("RetainDays"), m_stCfg.iRetainDays);
    o.insert(QStringLiteral("BlockSynth"), m_stCfg.bBlockSynth);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString CSandboxHost::TakeW(const SSlot& a_st, wchar_t* a_p) const {
    QString s = (a_p != nullptr) ? QString::fromWCharArray(a_p) : QString();
    if (a_st.fnFree != nullptr && a_p != nullptr) {
        a_st.fnFree(a_p);
    }
    return s;
}

CSandboxHost::SSlot* CSandboxHost::RouteSlot(const QString& a_sCmd) {
    // 2026-11-02 自动路由：vm_* 前缀 = Linux microVM 命令（vm_exec/vm_stop/
    //   vm_write/vm_read）→ VM 槽；其余（windows/launch/capture/click/restore_all
    //   等窗口类）→ 窗口槽。命令名路由稳定可靠，无需"猜测测试需求"。
    return a_sCmd.startsWith(QStringLiteral("vm_")) ? &m_stVm : &m_stWin;
}

QString CSandboxHost::SlotUnavailableText(const SSlot& a_st, const QString& a_sCmd) const {
    QString sWhy = a_st.sErr.isEmpty() ? QStringLiteral("未启用") : a_st.sErr;
    if (a_st.Ready()) {
        sWhy = QStringLiteral("已禁用（设置→测试沙箱）");
    }
    return QStringLiteral("[沙箱不可用：%1 需要 %2 沙箱——当前状态：%3。"
                          "可在 设置→测试沙箱 勾选启用；窗口类测试请用窗口沙箱，"
                          "Linux 命令/不可信代码请用 VM 沙箱]")
        .arg(a_sCmd, a_st.sType.isEmpty() ? QStringLiteral("(未配置)") : a_st.sType, sWhy);
}

bool CSandboxHost::Execute(const QString& a_sCmd, const QJsonObject& a_oArgs, QString& a_sOut) {
    SSlot* pSlot = RouteSlot(a_sCmd);
    if (!pSlot->Ready() || pSlot->fnExecute == nullptr) {
        a_sOut = SlotUnavailableText(*pSlot, a_sCmd);
        return false;
    }
    const std::wstring wCmd = a_sCmd.toStdWString();
    const QString sArgs = QString::fromUtf8(QJsonDocument(a_oArgs).toJson(QJsonDocument::Compact));
    const std::wstring wArgs = sArgs.toStdWString();
    bool bOk = false;
    wchar_t* p = pSlot->fnExecute(wCmd.c_str(), wArgs.c_str(), &bOk);
    a_sOut = TakeW(*pSlot, p);
    return bOk;
}

void CSandboxHost::RegisterUserRoot(DWORD a_dwPid, HANDLE a_hProc) {
    // 豁免登记只对窗口槽有意义（Ghost 的 AutoGhost 进程树登记）。
    CSandboxHost* pH = Instance();
    if (pH != nullptr && pH->m_stWin.Ready() && pH->m_stWin.fnRegUserRoot != nullptr) {
        pH->m_stWin.fnRegUserRoot(static_cast<uint32_t>(a_dwPid), a_hProc);
        return;   // 句柄所有权移交 dll
    }
    // 无窗口沙箱：关句柄防泄漏（原实现句柄由登记表持有，退化路径必须自理）。
    if (a_hProc != nullptr) {
        CloseHandle(a_hProc);
    }
}

QDialog* CSandboxHost::CreateDialog(QWidget* a_pParent) {
    // 管理对话框 = 窗口槽能力（Ghost 的收纳窗口管理 UI）。
    if (!m_stWin.Ready() || m_stWin.fnCreateDialog == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<QDialog*>(m_stWin.fnCreateDialog(a_pParent));
}

bool CSandboxHost::IsLoaded() const {
    return m_stWin.Ready() || m_stVm.Ready();
}

QString CSandboxHost::LoadedId() const {
    QStringList aIds;
    if (m_stWin.Ready()) {
        aIds.append(m_stWin.sId);
    }
    if (m_stVm.Ready()) {
        aIds.append(m_stVm.sId);
    }
    return aIds.join(QStringLiteral("+"));
}

void CSandboxHost::SetConfig(const SSbxCfg& a_stCfg) {
    m_stCfg = a_stCfg;
}

void CSandboxHost::Start() {
    // 常规路径 Load() 已带 start；此函数仅为兼容原调用形状保留（重启动场景）。
    SSlot* aSlots[] = { &m_stWin, &m_stVm };
    for (SSlot* p : aSlots) {
        if (p->Ready() && p->fnStart != nullptr) {
            p->fnStart(reinterpret_cast<const wchar_t*>(CfgJson().utf16()));
        }
    }
}

void CSandboxHost::Shutdown() {
    SSlot* aSlots[] = { &m_stWin, &m_stVm };
    for (SSlot* p : aSlots) {
        if (p->Ready() && p->fnShutdown != nullptr) {
            p->fnShutdown();
        }
    }
}

void CSandboxHost::ReloadConfig() {
    SSlot* aSlots[] = { &m_stWin, &m_stVm };
    for (SSlot* p : aSlots) {
        if (p->Ready() && p->fnReload != nullptr) {
            p->fnReload(reinterpret_cast<const wchar_t*>(CfgJson().utf16()));
        }
    }
}

void CSandboxHost::SetProjectRoot(const QString& a_sRoot) {
    const std::wstring w = a_sRoot.toStdWString();
    SSlot* aSlots[] = { &m_stWin, &m_stVm };
    for (SSlot* p : aSlots) {
        if (p->Ready() && p->fnSetRoot != nullptr) {
            p->fnSetRoot(w.c_str());
        }
    }
}

void CSandboxHost::ReadoptProjectWindows() {
    // 跨实例续收纳只与窗口槽相关（microVM 无窗口语义）。
    if (m_stWin.Ready() && m_stWin.fnReadopt != nullptr) {
        m_stWin.fnReadopt();
    }
}
