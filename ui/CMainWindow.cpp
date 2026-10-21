#include "CMainWindow.h"

#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QWidgetAction>   // 2026.09.26 最近打开按钮面板内嵌子菜单
#include <QAction>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QAbstractButton>
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QUuid>
#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <QThreadPool>   // 2026-10-01 沙箱命令异步化：投线程池执行（MTA COM）
#include <objbase.h>     // CoInitializeEx（WIN32_LEAN_AND_MEAN 裁掉了 ole2）
#include <algorithm>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QProcess>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QCloseEvent>
#include <QEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QToolButton>
#include <QPainter>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QAbstractNativeEventFilter>
#include <functional>

// Win32 ShellExecuteW（"runas" 动词触发 UAC 提权，启动环境安装脚本）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM（WM_NCHITTEST 坐标拆分）
#include <shellapi.h>
#include <dwmapi.h>     // DwmExtendFrameIntoClientArea（frameless 窗口补 Win10/11 系统阴影）

#include "CTaskListPanel.h"
#include "CExecutionPanel.h"
#include "CTitleBar.h"
#include "CTipStyle.h"   // 2026.09.10 v2.2.156 全局 tooltip 11 号字体 + 长文本自动换行
#include "CRollbackDialog.h"   // FR-6 历史版本回退对话框
#include "CCodeReviewDialog.h"   // 2026-09-30 资源管理器右键代码审查（ReviewSdk）
#include "CProjectManager.h"
#include "CTaskOrchestrator.h"
#include "CClaudeRunner.h"
#include "CAgentRunnerFactory.h"   // 2026.08.18 后端单选分派（自带 CDeepSeekRunner.h）
#include "sandbox/CSandboxManager.h"   // 2026-09-26 内置测试沙箱
#include "sandbox/CSandboxDialog.h"
#include "CCodexRunner.h"         // 第四后端 CodexHarness 参数回填/探测自愈
#include "CPiAgentRunner.h"        // 2026.08.19 第三后端 PIAgent 参数回填/探测自愈
#include "CHermesRunner.h"        // 第五后端 Hermes 参数回填（任务 3213cdbf）
#include "CVersionDialog.h"       // 2026.09 后端版本管理（任务 23183ccb）
#include "CTaskInfo.h"
#include "core/StageDef.h"   // 2026.08.17 FindStage：中断恢复提示里显示阶段中文名
#include "TaskTypes.h"
#include <core/CVersion.h>
#include "CStageMachine.h"   // 2026.08.15 流程档位：GetFlowStages（新建任务全勾判定）
#include "CRecentProjectsDialog.h"
#include "CRecentChipsPanel.h"   // 2026.09.26 最近打开按钮面板（子菜单内嵌）
#include "CSettingsDialog.h"
#include "CAskUserDialog.h"
#include "CAboutDialog.h"
#include "CNewTaskDialog.h"
#include "COrchDesignDialog.h"   // 2026-10-14 任务编排设计（功能菜单）
#include "CTaskHistoryDB.h"

// [Scan] ExcludeDirs 逗号串 → 逐项去空白的清单。用户常在逗号后带空格（".autodev, build"），
// 不逐项 trim 会让 " build" 匹配失败、该目录排除悄悄失效（备份路径 BackupExcludeDirs
// 内部已 trim，扫描路径此前漏了）。全空 → 空清单 = SetExcludeDirs 恢复内置默认。
static QStringList ScanDirList(const QString& a_sDirs) {
    QStringList aItems;
    const QStringList aRaw = a_sDirs.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& sItem : aRaw) {
        const QString sTrim = sItem.trimmed();
        if (!sTrim.isEmpty()) {
            aItems.append(sTrim);
        }
    }
    return aItems;
}

// 2026.08.18 自定义大模型表注入：SDshModelEntry（config.ini [DshModels]）→
//   CDeepSeekRunner::SCustomModel（字段一一对应）。空表/无效下标原样传入 = 未启用，
//   runner 内部 CustomActiveValid 判定，走 [Agent] DshProvider/DshModel 固定路由。
//   生效时机 = 下一次 DSH 进程冷启动（长驻进程 env 在启动时注入，运行中的会话不受影响）。
static void ApplyDshCustomModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    QVector<CDeepSeekRunner::SCustomModel> aRunnerModels;
    aRunnerModels.reserve(a_aModels.size());
    for (const SDshModelEntry& stE : a_aModels) {
        CDeepSeekRunner::SCustomModel stM;
        stM.sDisp    = stE.sDisp;
        stM.sBaseURL = stE.sBaseURL;
        stM.sApiKey  = stE.sApiKey;
        stM.sApi     = stE.sApi;
        stM.sModelId = stE.sModelId;
        stM.iCtx     = stE.iCtx;
        stM.iMaxTok  = stE.iMaxTok;
        aRunnerModels.append(stM);
    }
    CDeepSeekRunner::SetCustomModels(aRunnerModels, a_iActive);
}

// 应用级原生消息过滤器：先于 Qt 内部消息处理看到所有 Win32 消息（QWidget::nativeEvent
// 之前），Qt 内部即便特殊处理/吞掉 WM_NCLBUTTONDOWN 也拦不到这里——图标弹系统菜单
// 的可靠触发通道。主 HWND 之外的消息一概放行。
class CTitleBarMsgFilter : public QAbstractNativeEventFilter {
public:
    // a_fnMenu(锚点)：弹系统菜单；a_fnCapRClick(光标)：标题栏空白右键弹同一菜单；
    // a_fnSysCmd：接管 SC_MAXIMIZE/SC_RESTORE/SC_MINIMIZE/SC_CLOSE（图标态同步）。
    CTitleBarMsgFilter(HWND a_hMain,
                       std::function<void(const QPoint&)> a_fnMenu,
                       std::function<void(const QPoint&)> a_fnCapRClick,
                       std::function<void(unsigned int)> a_fnSysCmd)
        : m_hMain(a_hMain), m_fnMenu(std::move(a_fnMenu)),
          m_fnCapRClick(std::move(a_fnCapRClick)), m_fnSysCmd(std::move(a_fnSysCmd)) {}

    bool nativeEventFilter(const QByteArray& a_sType, void* a_pMsg, qintptr* a_piRes) override {
        if (a_sType != "windows_generic_MSG" || a_pMsg == nullptr) {
            return false;
        }
        MSG* pMsg = static_cast<MSG*>(a_pMsg);
        if (pMsg->hwnd != m_hMain) {
            return false;   // 只处理主窗口（对话框等其它窗口放行）
        }
        const auto fnCursor = []() -> QPoint {
            POINT stPt;
            GetCursorPos(&stPt);
            return QPoint(stPt.x, stPt.y);
        };
        // 图标区（HTSYSMENU）左/右键 → 弹系统菜单（锚点=图标左下角由 CMainWindow 决定）
        if ((pMsg->message == WM_NCLBUTTONDOWN || pMsg->message == WM_NCRBUTTONDOWN) &&
            pMsg->wParam == HTSYSMENU && m_fnMenu != nullptr) {
            m_fnMenu(QPoint());   // 空点=让 OnTitleIconClicked 取图标锚点
            if (a_piRes != nullptr) {
                *a_piRes = 0;
            }
            return true;   // 已处理，阻止 DefWindowProc 再弹一份
        }
        // 标题栏空白（HTCAPTION）右键 → 在光标处弹同一菜单（Windows 原生行为）
        if (pMsg->message == WM_NCRBUTTONDOWN && pMsg->wParam == HTCAPTION &&
            m_fnCapRClick != nullptr) {
            m_fnCapRClick(fnCursor());
            if (a_piRes != nullptr) {
                *a_piRes = 0;
            }
            return true;
        }
        // 接管四项窗口命令（图标菜单/任务栏菜单/Alt+F4 统一入口）：交 CMainWindow 走
        // 自有路径（最大化/还原经 OnMaximizeToggle，图标态单一来源永不错位）。
        if (pMsg->message == WM_SYSCOMMAND && m_fnSysCmd != nullptr) {
            const unsigned int uCmd = static_cast<unsigned int>(pMsg->wParam & 0xFFF0);
            switch (uCmd) {
                case SC_MAXIMIZE:
                case SC_RESTORE:
                case SC_MINIMIZE:
                case SC_CLOSE:
                    m_fnSysCmd(uCmd);
                    if (a_piRes != nullptr) {
                        *a_piRes = 0;
                    }
                    return true;   // 已接管，阻断 DefWindowProc 重复动作
                default:
                    return false;  // SC_MOVE/SC_SIZE 等 → 系统默认（模态拖动/缩放循环）
            }
        }
        return false;
    }

private:
    HWND m_hMain = nullptr;
    std::function<void(const QPoint&)> m_fnMenu;
    std::function<void(const QPoint&)> m_fnCapRClick;
    std::function<void(unsigned int)> m_fnSysCmd;
};

void CMainWindow::OnConfigFileChanged() {
    // 2026.08.20 防抖定时器到点：自身保存窗口（2s）内的变更跳过；其余视为其他
    //   实例保存 → 静默热重载（不弹 DSH/Pi 警告）+ 状态栏提示。
    if (QDateTime::currentMSecsSinceEpoch() - m_iCfgSelfSaveMs < 2000) {
        return;
    }
    ApplyAgentConfigToRuntime(false);
    // 2026.09.07 v2.1.123 最近打开列表跨实例热同步：他实例打开/移除工程落盘 config.ini
    //   → 本实例菜单即时刷新（原先只在重启后生效）。
    m_pPm->LoadRecent();
    RebuildRecentMenu();
    SetStatus(QStringLiteral("配置已从其他 AutoCode 实例同步（config.ini 热重载）"));
}

// 2026.09.10 公共模型配置：四张模型表（当前设置参数源范围——任务级覆盖激活时为
//   任务档，否则全局；四表各自走 TableCfgPath 活回退）合并为一份共享表后统一应用
//   到四个 runner。合并规则见 CModelEntry.h MergeSharedModelTables：基底 = 最多
//   条目的表（传参优先级 Pi > DSH > Hermes > Codex），其余表新身份追加；共享当前项
//   = 基底表 Active 按身份映射。磁盘上的四张节在用户保存设置前不回写（设置页
//   UnifySharedModels 展示同一份、保存时同一份内容写回四节，收敛自然发生）。
void CMainWindow::ApplySharedModelsToRunners() {
    if (m_pPm == nullptr) {
        return;
    }
    int iDshAct = -1;
    int iPiAct = -1;
    int iCodexAct = -1;
    int iHermesAct = -1;
    const QList<SDshModelEntry> aDsh = m_pPm->LoadDshModels(&iDshAct);
    const QList<SDshModelEntry> aPi = m_pPm->LoadPiModels(&iPiAct);
    const QList<SDshModelEntry> aCodex = m_pPm->LoadCodexModels(&iCodexAct);
    const QList<SDshModelEntry> aHermes = m_pPm->LoadHermesModels(&iHermesAct);
    // 2026.09.23 v3.1.229 搬机自愈（用户报告：复制的 config.ini 缺 [CodexModels]/
    //   [DshModels] 节——源机两套 ini 分叉的现场态）：共享表非空且四表有缺节时，
    //   把合并后的共享表回写到缺的节（幂等；下次启动/旧工具读原节不再「模型未配置」）。
    //   仅在无任务覆盖时写（SettingsCfgPath=全局；覆盖态由保存链路自行同步，避免双源）。
    {
        const QList<SDshModelEntry>* apTables[] = { &aPi, &aDsh, &aHermes, &aCodex };
        const int aiActs[] = { iPiAct, iDshAct, iHermesAct, iCodexAct };
        const bool bAnyMissing = aPi.isEmpty() || aDsh.isEmpty()
                                 || aHermes.isEmpty() || aCodex.isEmpty();
        const bool bAnyHas = !aPi.isEmpty() || !aDsh.isEmpty()
                            || !aHermes.isEmpty() || !aCodex.isEmpty();
        if (bAnyMissing && bAnyHas && !m_pPm->TaskConfigOverlayActive()) {
            const int iHave = [&apTables, &aiActs]() {
                for (int i = 0; i < 4; ++i) {
                    if (aiActs[i] >= 0 && !apTables[i]->isEmpty()) {
                        return i;
                    }
                }
                return -1;
            }();
            if (iHave >= 0) {
                const SSharedModels stPre = MergeSharedModelTables(
                    { aPi, aDsh, aHermes, aCodex },
                    { iPiAct, iDshAct, iHermesAct, iCodexAct }, iHave);
                if (!stPre.aModels.isEmpty() && stPre.iActive >= 0) {
                    if (aDsh.isEmpty()) {
                        m_pPm->SaveDshModels(stPre.aModels, stPre.iActive);
                    }
                    if (aPi.isEmpty()) {
                        m_pPm->SavePiModels(stPre.aModels, stPre.iActive);
                    }
                    if (aHermes.isEmpty()) {
                        m_pPm->SaveHermesModels(stPre.aModels, stPre.iActive);
                    }
                    if (aCodex.isEmpty()) {
                        m_pPm->SaveCodexModels(stPre.aModels, stPre.iActive);
                    }
                }
            }
        }
    }
    const QList<QList<SDshModelEntry>> aTables = { aPi, aDsh, aHermes, aCodex };
    const QList<int> aActives = { iPiAct, iDshAct, iHermesAct, iCodexAct };
    // 共享当前项优先取「当前后端」自己表的激活条目（保任务级/后端级已选模型不被
    //   大表盖掉；磁盘四表收敛后各表 Active 本就相同，该优先级只在未收敛期起作用）。
    int iPrefTable = -1;
    if (CAgentRunnerFactory::IsPi()) {
        iPrefTable = 0;
    } else if (CAgentRunnerFactory::IsDsh()) {
        iPrefTable = 1;
    } else if (CAgentRunnerFactory::IsHermes()) {
        iPrefTable = 2;
    } else if (CAgentRunnerFactory::IsCodex()) {
        iPrefTable = 3;
    }
    const SSharedModels stM = MergeSharedModelTables(aTables, aActives, iPrefTable);
    // DSH：autocode-active env 槽位（下次 DSH 进程冷启动生效）。
    ApplyDshCustomModels(stM.aModels, stM.iActive);
    // Pi：Set 即合成写 ~/.pi/agent/models.json 的 autocode-<n> 槽位（原行为）。
    CPiAgentRunner::SetPiModels(stM.aModels, stM.iActive);
    // Codex：纯内存 + ~/.codex 自愈写（原 Codex 启动块行为；错误走既有报错路径）。
    CCodexRunner::SetCodexModels(stM.aModels, stM.iActive);
    CCodexRunner::WriteCodexUserConfig();
    // Hermes：激活条目覆盖模型/密钥/BaseUrl 固定路由 + 窗口/输出上限（钳位在 runner 内收口）。
    if (stM.iActive >= 0 && stM.iActive < stM.aModels.size()) {
        const SDshModelEntry& stE = stM.aModels.at(stM.iActive);
        CHermesRunner::SetHermesModel(stE.sModelId);
        CHermesRunner::SetHermesApiKey(stE.sApiKey);
        CHermesRunner::SetHermesBaseUrl(stE.sBaseURL);
        CHermesRunner::SetHermesApi(stE.sApi);   // 2026.09.21 协议感知：anthropic 端点 → config.yaml api_mode
        CHermesRunner::SetHermesRuntimeLimits(stE.iCtx, stE.iMaxTok);
    }
    // 2026-10-05 共享表同步推给面板：右键「选择模型」菜单列条目 + 上下文进度条模型名
    //   兜底/即时刷新（启动/热重载/设置保存/右键切换四路都会走到这里；面板可能尚未
    //   创建——早期单实例挂接路径会先跑一次，判空即可）。
    // 2026-10-08 Claude 后端：共享表快照 + 跟随开关（StartStage per-进程 env 注入；
    //   助手实例槽覆盖优先，跟随开关仅在无覆盖时生效）。
    CClaudeRunner::SetSharedModelTable(stM.aModels, stM.iActive);
    CClaudeRunner::SetFollowSharedModels(
        m_pPm != nullptr && m_pPm->LoadClaudeFollowShared());
    if (m_pExecPanel != nullptr) {
        m_pExecPanel->SetSharedModels(stM.aModels, stM.iActive);
    }
    // 2026-10-08 提示词长度闸门参数推送（启动/热重载/设置保存/右键切换四路都到这；
    //   面板闸门与编排器提炼轮即时生效）。
    if (m_pExecPanel != nullptr && m_pPm != nullptr) {
        m_pExecPanel->SetPromptLimits(m_pPm->LoadAdhocPromptCtxPctMax(),
                                      m_pPm->LoadAdhocPromptTokMax());
    }
}

// 2026-10-05 上下文进度条右键「选择模型」：共享当前项换 a_iIndex（下标口径 = 面板
//   SetSharedModels 收到的同一份合并表）→ 四张节同写 + ApplySharedModelsToRunners
//   重应用（DSH env 槽位/Pi models.json/Codex 内存+~/.codex/Hermes 固定路由，
//   下一轮即用新模型，无需重启 AutoCode/进设置页）。任务级覆盖激活时写入任务档
//   （Save*Models 走 TableCfgPath，口径同设置页保存）。
void CMainWindow::OnSwitchSharedModel(int a_iIndex) {
    if (m_pPm == nullptr) {
        return;
    }
    int iDshAct = -1;
    int iPiAct = -1;
    int iCodexAct = -1;
    int iHermesAct = -1;
    const QList<SDshModelEntry> aDsh = m_pPm->LoadDshModels(&iDshAct);
    const QList<SDshModelEntry> aPi = m_pPm->LoadPiModels(&iPiAct);
    const QList<SDshModelEntry> aCodex = m_pPm->LoadCodexModels(&iCodexAct);
    const QList<SDshModelEntry> aHermes = m_pPm->LoadHermesModels(&iHermesAct);
    int iPrefTable = -1;
    if (CAgentRunnerFactory::IsPi()) {
        iPrefTable = 0;
    } else if (CAgentRunnerFactory::IsDsh()) {
        iPrefTable = 1;
    } else if (CAgentRunnerFactory::IsHermes()) {
        iPrefTable = 2;
    } else if (CAgentRunnerFactory::IsCodex()) {
        iPrefTable = 3;
    }
    const SSharedModels stM = MergeSharedModelTables(
        { aPi, aDsh, aHermes, aCodex },
        { iPiAct, iDshAct, iHermesAct, iCodexAct }, iPrefTable);
    if (a_iIndex < 0 || a_iIndex >= stM.aModels.size()) {
        return;
    }
    const bool bOk = m_pPm->SaveDshModels(stM.aModels, a_iIndex)
                  && m_pPm->SavePiModels(stM.aModels, a_iIndex)
                  && m_pPm->SaveHermesModels(stM.aModels, a_iIndex)
                  && m_pPm->SaveCodexModels(stM.aModels, a_iIndex);
    if (!bOk) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("切换模型"),
            QStringLiteral("模型配置写入 config.ini 失败，切换可能未持久化（重启后失效）。"), this);
    }
    ApplySharedModelsToRunners();   // 重应用 runner + 回推面板（进度条模型名/菜单勾选即时刷新）
    SetStatus(QStringLiteral("已切换模型：%1（下一轮生效）")
                  .arg(stM.aModels.at(a_iIndex).sModelId));
}

// 2026.08.20 抽取自 ctor 启动回填块（多实例配置热重载复用）：config.ini → 运行期
//   静态配置（联网检索开关/三个 runner 静态项/后端单选/自定义模型表/扫描排除）。
//   a_bStartup=false 时跳过 DSH/Pi 不可用警告弹窗（外部实例触发的热重载静默）。
void CMainWindow::ApplyAgentConfigToRuntime(bool a_bStartup) {
    // 2026.08.15 阶段联网检索开关回填（[Agent] WebSearch；默认开，0=全阶段禁用）。
    m_pOrch->SetWebSearch(m_pPm->LoadWebSearch());
    // 2026.08.15 Agent 运行参数回填（[Agent] ClaudeCmd / AskTimeoutSec；进程级静态配置）。
    CClaudeRunner::SetClaudeCmd(m_pPm->LoadClaudeCmd());
    CClaudeRunner::SetAskTimeoutSec(m_pPm->LoadAskTimeoutSec());
    // 2026.08.17 停滞检测阈值回填（[Agent] StallMinutes，默认 10 分钟）。
    CClaudeRunner::SetStallMinutes(m_pPm->LoadStallMinutes());
    // 2026.08.18 Agent 后端与 DSH 参数回填（全局单选：工厂分派 + CDeepSeekRunner 进程级静态）。
    // 2026.08.19 第三后端 PIAgent（Backend=3）。
    {
        // 2026.09.19 后端切换清会话链：切换前后端不同且非启动路径（启动时会话尚未装载，
        //   档载入侧 backend 键守卫已覆盖）→ 清各助手 sid/撤销栈。覆盖三路：热重载
        //   （OnConfigFileChanged，另一实例改后端）/任务档重应用（ApplyTaskConfigOverlay）
        //   /启动以外的一切重应用。设置页保存路径在 OnSettingsAccepted 里另有同款挂钩。
        const int iBackendBefore = CAgentRunnerFactory::IsDsh() ? 2
                                  : CAgentRunnerFactory::IsPi() ? 3
                                  : CAgentRunnerFactory::IsCodex() ? 4
                                  : CAgentRunnerFactory::IsHermes() ? 5 : 1;
        const int iBackendStartup = m_pPm->LoadAgentBackend();
        CAgentRunnerFactory::SetBackend(iBackendStartup == 2 ? CAgentRunnerFactory::BackendDsh
                                        : iBackendStartup == 3 ? CAgentRunnerFactory::BackendPi
                                        : iBackendStartup == 4 ? CAgentRunnerFactory::BackendCodex
                                        : iBackendStartup == 5 ? CAgentRunnerFactory::BackendHermes
                                                               : CAgentRunnerFactory::BackendClaude);
        if (!a_bStartup && iBackendStartup != iBackendBefore) {
            ClearAssistantSessionsOnBackendSwitch();
        }
    }
    // 2026.08.18 自动配置：DSH 根目录——config 显式键优先；空或失效（键指向的目录
    //   无 bin.js/cordis.yml，如 config.ini 随发布包带到别的机器后的死路径）时探测
    //   exe 同级/上级的 DeepSeekHarness\deepseek-harness（install_env.bat 固定安装
    //   布局），探测命中即回写 config.ini（安装完成后零手填 + 搬机死键自愈）。
    QString sDshRootStartup = m_pPm->LoadDshRoot();
    if (!CDeepSeekRunner::LooksLikeDshRoot(sDshRootStartup)) {
        const QString sProbed = CDeepSeekRunner::ProbeSiblingRoot();
        if (CDeepSeekRunner::LooksLikeDshRoot(sProbed)) {
            sDshRootStartup = sProbed;
            m_pPm->SaveDshRoot(sDshRootStartup);
        }
    }
    CDeepSeekRunner::SetDshRootPath(sDshRootStartup);
    CDeepSeekRunner::SetDshNodePath(m_pPm->LoadDshNodePath());
    // 2026.09.27 额外可写目录（[Dsh] ExtraWriteDirs；授 ACE 放行 pwsh 沙箱）。
    CDeepSeekRunner::SetDshExtraWriteDirs(m_pPm->LoadDshExtraWriteDirs());
    CDeepSeekRunner::SetDshBinJsPath(m_pPm->LoadDshBinJsPath());
    CDeepSeekRunner::SetDshCordisPath(m_pPm->LoadDshCordisPath());
    CDeepSeekRunner::SetDshProvider(m_pPm->LoadDshProvider());
    // 2026-10-19 会话权限模式（[Agent] DshPermissionMode；缺省 danger-full-access）。
    CDeepSeekRunner::SetDshPermissionMode(m_pPm->LoadDshPermissionMode());
    CDeepSeekRunner::SetDshModel(m_pPm->LoadDshModel());
    // 2026-10-08 思考强度（initialize reasoningEffort；空=默认不注入）。
    CDeepSeekRunner::SetDshEffort(m_pPm->LoadDshEffort());
    CDeepSeekRunner::SetDshPriceInPerM(m_pPm->LoadDshPriceInPerM());
    CDeepSeekRunner::SetDshPriceCacheReadPerM(m_pPm->LoadDshPriceCacheReadPerM());
    CDeepSeekRunner::SetDshPriceCacheWritePerM(m_pPm->LoadDshPriceCacheWritePerM());
    CDeepSeekRunner::SetDshPriceOutPerM(m_pPm->LoadDshPriceOutPerM());
    // 2026.08.20 PIAgent 计费单价（$/M token）：pi 只报 token，按单价折算费用。
    CPiAgentRunner::SetPiPriceInPerM(m_pPm->LoadPiPriceInPerM());
    CPiAgentRunner::SetPiPriceCacheReadPerM(m_pPm->LoadPiPriceCacheReadPerM());
    CPiAgentRunner::SetPiPriceCacheWritePerM(m_pPm->LoadPiPriceCacheWritePerM());
    CPiAgentRunner::SetPiPriceOutPerM(m_pPm->LoadPiPriceOutPerM());
    // 2026.08.18 自定义大模型表：四后端共享同一份（见下方 ApplySharedModelsToRunners，
    //   在四后端根目录/参数全部就绪后统一应用）。
    // 2026.08.18 配置了 DSH 后端但本机不可用 → 启动即明确警告：不能等第一次阶段
    //   启动才报错，用户需要立刻知道哪里坏了、怎么处理（文案含一键安装指引）。
    if (a_bStartup && CAgentRunnerFactory::IsDsh()) {
        const QString sDshErr = CDeepSeekRunner::DshConfigError();
        if (!sDshErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（DeepSeekHarness）不可用"),
                sDshErr + QStringLiteral("\n\n当前后端为 DeepSeekHarness，问题解决前任务/助手/guide "
                                          "将无法启动；也可到设置中切回 Claude Code。"), this);
        }
    }
    // 2026.08.19 PIAgent 参数回填 + 根目录探测自愈（口径同 DSH：显式键优先，空/失效
    //   时探测 exe 同级 PIAgent（install_env.bat /pi 固定布局），命中即回写 config.ini）。
    QString sPiRootStartup = m_pPm->LoadPiRoot();
    if (!CPiAgentRunner::LooksLikePiRoot(sPiRootStartup)) {
        const QString sPiProbed = CPiAgentRunner::ProbeSiblingRoot();
        if (!sPiProbed.isEmpty()) {
            sPiRootStartup = sPiProbed;
            m_pPm->SavePiRoot(sPiProbed);
        }
    }
    CPiAgentRunner::SetPiRootPath(sPiRootStartup);
    CPiAgentRunner::SetPiNodePath(m_pPm->LoadPiNodePath());
    CPiAgentRunner::SetPiProvider(m_pPm->LoadPiProvider());
    CPiAgentRunner::SetPiThinking(m_pPm->LoadPiThinking());
    // 2026-10-08 思考强度/思考预算（claude MAX_THINKING_TOKENS / codex -c / hermes config.yaml；空=默认）。
    CClaudeRunner::SetMaxThinkingTokens(m_pPm->LoadClaudeMaxThinkTok());
    CCodexRunner::SetCodexEffort(m_pPm->LoadCodexEffort());
    CHermesRunner::SetHermesEffort(m_pPm->LoadHermesEffort());
    // 2026.08.20 PiAgent 自定义大模型表：四后端共享同一份（见下方 ApplySharedModelsToRunners）。
    // 配置了 Pi 后端但本机不可用 → 启动即明确警告（口径同 DSH，不等第一次阶段启动）。
    if (a_bStartup && CAgentRunnerFactory::IsPi()) {
        const QString sPiErr = CPiAgentRunner::PiConfigError();
        if (!sPiErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（PIAgent）不可用"),
                sPiErr + QStringLiteral("\n\n当前后端为 PIAgent，问题解决前任务/助手/guide "
                                        "将无法启动；也可到设置中切回 Claude Code。"), this);
        }
    }
    // 第四后端 CodexHarness：参数回填 + 根目录探测自愈（口径同 Pi：显式键优先，空/失效
    //   时探测 exe 同级 CodexHarness，命中即回写 config.ini）。
    QString sCodexRootStartup = m_pPm->LoadCodexRoot();
    if (!CCodexRunner::LooksLikeCodexRoot(sCodexRootStartup)) {
        const QString sCodexProbed = CCodexRunner::ProbeSiblingRoot();
        if (!sCodexProbed.isEmpty()) {
            sCodexRootStartup = sCodexProbed;
            m_pPm->SaveCodexRoot(sCodexProbed);
        }
    }
    CCodexRunner::SetCodexRoot(sCodexRootStartup);
    CCodexRunner::SetCodexNodePath(m_pPm->LoadCodexNodePath());
    CCodexRunner::SetCodexProvider(m_pPm->LoadCodexProvider());
    CCodexRunner::SetCodexWireApi(m_pPm->LoadCodexWireApi());
    // 2026.08.21 Codex 自定义大模型表：四后端共享同一份——模型静态变量由下方
    //   ApplySharedModelsToRunners 统一注入，可用性检查在其后（v3.1.230：检查原先
    //   在注入之前，CodexEffModel 恒空 → 后端=Codex 时无论配置多完整启动必误报
    //   「模型未配置」；检查块移至 ApplySharedModelsToRunners 之后，口径同 Hermes）。
    // 第五后端 Hermes（任务 3213cdbf）：参数回填（口径同 Pi/Codex 静态注入；
    //   ~/.hermes 运行时配置由 StartStage 幂等自愈写，此处不落盘）。
    CHermesRunner::SetHermesRoot(m_pPm->LoadHermesRoot());
    CHermesRunner::SetHermesNodePath(m_pPm->LoadHermesNodePath());
    CHermesRunner::SetHermesPriceInPerM(m_pPm->LoadHermesPriceInPerM());
    CHermesRunner::SetHermesPriceCacheReadPerM(m_pPm->LoadHermesPriceCacheReadPerM());
    CHermesRunner::SetHermesPriceCacheWritePerM(m_pPm->LoadHermesPriceCacheWritePerM());
    CHermesRunner::SetHermesPriceOutPerM(m_pPm->LoadHermesPriceOutPerM());
    // 2026.09.10 公共模型配置：四后端共享同一份模型表——四张表（当前参数源范围，
    //   任务级覆盖时为任务档）合并归一后统一应用到四个 runner，切换后端即用同一份
    //   列表/当前项（用户需求：模型配置都一样，切换后端不用重配）。含 Codex 侧
    //   ~/.codex 自愈写（原 Codex 块行为）。Pi 的 Set 会同步合成写 ~/.pi/agent/
    //   models.json（原行为）。
    ApplySharedModelsToRunners();
    // 配置了 Codex 后端但本机不可用 → 启动即明确警告（口径同 DSH/Pi）。
    // 2026.09.24 v3.1.230 移到 ApplySharedModelsToRunners 之后：CodexConfigError 的
    //   模型判据（激活条目/固定路由静态变量）唯一注入点就是 ApplySharedModelsToRunners，
    //   先查后注 → 恒报「CodexHarness 模型未配置」（实测配置完整仍弹；v3.1.229 修的
    //   磁盘缺节自愈解决不了这层时序）。真缺配置时检查照样触发，只消除误报。
    if (a_bStartup && CAgentRunnerFactory::IsCodex()) {
        const QString sCodexErr = CCodexRunner::CodexConfigError();
        if (!sCodexErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（CodexHarness）不可用"),
                sCodexErr + QStringLiteral("\n\n当前后端为 CodexHarness，问题解决前任务/助手/guide "
                                          "将无法启动；也可到设置中切回 Claude Code。"), this);
        }
    }
    // 配置了 Hermes 后端但本机不可用 → 启动即明确警告（口径同 DSH/Pi/Codex）。
    if (a_bStartup && CAgentRunnerFactory::IsHermes()) {
        const QString sHermesErr = CHermesRunner::HermesConfigError();
        if (!sHermesErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（Hermes）不可用"),
                sHermesErr + QStringLiteral("\n\n当前后端为 Hermes，问题解决前任务/助手/guide "
                                          "将无法启动；也可到设置中切回 Claude Code。"), this);
        }
    }
    // 2026.08.15 扫描排除目录回填（[Scan] ExcludeDirs；空=恢复内置默认）。
    // 2026.08.18 修：逐项 trim（逗号后空格不再让排除失效）+ 空也下发（清空即回默认）。
    CGuideFingerprintScanner::SetExcludeDirs(ScanDirList(m_pPm->LoadScanExcludeDirs()));
    // 2026.09.23 资源管理器显示隐藏清单（[Explorer] HiddenDirs；缺键=内置默认）。
    if (m_pTaskPanel != nullptr) {
        m_pTaskPanel->SetExplorerHiddenDirs(
            ScanDirList(m_pPm->LoadExplorerHiddenDirs()));
    }
}

CMainWindow::CMainWindow(QWidget* a_pParent)
    : QMainWindow(a_pParent) {
    // 窗口标题带三段式版本号（自绘标题栏上的版本显示见 CTitleBar）；工程打开后由
    //   UpdateWindowTitle 刷成「工程名-AutoCode 版本」（2026.09.07 需求：任务管理器
    //   可辨认多实例各开的工程）。
    setWindowTitle(QStringLiteral("AutoCode ") + AutoCodeVersionText());
    resize(1240, 780);

    // 2026.09.10 v2.2.156 全局 tooltip 样式（用户需求：设置页提示字体太小统一 11 号、
    //   长文本自动换行）：必须早于 CreateUi——先装 ToolTipChange 过滤器，随后各控件
    //   setToolTip 的纯文本才能被就地转富文本（自动换行）。
    CTipStyle::ApplyGlobal();

    // FR-3：去掉系统原生标题栏，统一用自绘 CTitleBar（避免系统标题栏与自绘标题栏重复）。
    // frameless 后系统标题栏/边框全部移除，边缘缩放交 nativeEvent 的 WM_NCHITTEST 自绘命中。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // 补回 Win10/11 系统阴影：DWM 在客户区四周保留 1px 玻璃边框，触发桌面合成器为
    // 无边框窗口绘制投影，视觉上仍贴近标准窗口（否则 frameless 窗口会贴在桌面上无层次感）。
    if (HWND hWnd = reinterpret_cast<HWND>(winId())) {
        MARGINS stMargin { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hWnd, &stMargin);
    }

    m_pPm   = new CProjectManager(this);
    m_pOrch = new CTaskOrchestrator(m_pPm, this);

    // 恢复上次窗口位置/大小/最大化态（exe 同级 config.ini 的 [Window] 段）；
    // 无记录、或位置已移出所有屏幕（换显示器 / 分辨率变化）则保留上面 resize() 的默认几何。
    QRect stGeo;
    bool bMax = false;
    if (m_pPm->LoadWindowGeometry(stGeo, bMax) && IsGeometryOnScreen(stGeo)) {
        setGeometry(stGeo);
        if (bMax) {
            setWindowState(windowState() | Qt::WindowMaximized);   // 此时未 show，show() 时生效
        }
    }

    // 2026.08.20 历史库按工程隔离（<工程>/.autodev/history.db）：启动先做一次性迁移
    //   （旧全局库各行按 project_dir 归位到对应工程，旧库收档 .migrated），随后不再
    //   打开全局库——OpenProjectDir 打开当前工程自己的库，历史页只显示本工程任务。
    CTaskHistoryDB::MigrateLegacyToProjects();
    m_pHistoryDb = std::make_unique<CTaskHistoryDB>();

    // 2026.09.07 v2.1.122 起 LoadRecent 固定在 CreateUi() 之前调（勿回退）：CreateUi()
    //   结束后的 RebuildRecentMenu 需要已载入的最近列表（旧时序在 CreateUi() 之后调，
    //   最近子菜单恒空）。2026.09.09 v2.1.137 启动自动恢复上次工程已按用户需求移除
    //   （启动停在未开工程态，经「最近打开」/「打开工程目录」手动进入）——本行仅为
    //   最近列表服务，保留。
    m_pPm->LoadRecent();

    CreateUi();
    CreateMenus();

    // 重建子菜单（m_pMenuRecent 已在 CreateMenus 中创建；列表已在上面载入）。
    RebuildRecentMenu();

    // 2026.08.15 启动配置回填（单一数据源 = exe 同级 config.ini；设置对话框可改并落盘）：
    //   助手 autocompact 已改比例制（[Adhoc] AutoCompactPct），编排器在每次 StartAdhocStage
    //   现读 m_pPm->AutoCompactKFor() 换算，启动期无需回填。
    //   （2026-10-15 上下文窗口上限回填已删：[Adhoc] ContextLimit 机制整线移除，
    //   窗口唯一来源 = 按模型自动探测。）
    // 评审多视角并行档位回填（[Review] Parallel；1=串行回退零回归，默认 3）。
    m_pOrch->SetReviewParallel(m_pPm->LoadReviewParallel());
    // 2026.08.20 抽取为 ApplyAgentConfigToRuntime（多实例热重载复用）。
    ApplyAgentConfigToRuntime(true);
    // 2026-10-19 DSH 自愈失败提权回调：EnsureProcess 静默自愈失败（过滤令牌下
    //   SetNamedSecurityInfoW Win32 5）且在 GUI 线程时经此同步询问用户——确认 →
    //   runas icacls 补账户级 F → 复查；true=已修复（EnsureProcess 信此值发成功
    //   日志，不二次复查）。复查未过同样登记抑制集，防每次 submit 重复弹窗。
    CDeepSeekRunner::SetAclElevateHandler([this](const QString& a_sDir, const QString& a_sCmd) {
        if (!RunAclElevateFlow(QStringLiteral("工程目录权限修复"), {a_sDir}, a_sCmd)) {
            return false;
        }
        // 2026-10-21 按命令类型复查：/setintegritylevel=沙箱遗留标签恢复（对
        //   RestoreNeutralSecurity）；其余仍查账户写权限 ACE（EnsureDirWriteAce）。
        const bool bRelabel = a_sCmd.contains(QStringLiteral("/setintegritylevel"));
        const bool bOk = bRelabel
            ? CDeepSeekRunner::RestoreNeutralSecurity(a_sDir) == 0
            : CDeepSeekRunner::EnsureDirWriteAce(a_sDir) == 0;
        if (!bOk) {
            m_aAclElevateSuppressed.insert(QDir::toNativeSeparators(a_sDir).toLower());
        }
        ExecMsgBox(bOk ? QMessageBox::Information : QMessageBox::Warning,
                   QStringLiteral("工程目录权限修复"),
                   bOk ? (bRelabel
                              ? QStringLiteral("提权修复成功：沙箱遗留低完整性标签已清除，"
                                              "普通程序（VS/MSBuild 等）可直接写入，任务继续。")
                              : QStringLiteral("提权修复成功：已补当前账户写权限 ACE，任务继续。"))
                       : QStringLiteral("提权修复后复查仍未通过。\n可手工执行：\n%1").arg(a_sCmd),
                   this);
        return bOk;
    });
    // 2026.08.20 多实例配置同步：盯 config.ini（exe 同级全局共享），其他实例保存后
    //   本实例热重载。WritePrivateProfileStringW 非原子、设置页一次写几十键，防抖 1s
    //   合并成一次；自身保存 2s 内的变更跳过（m_iCfgSelfSaveMs 打点）。
    const QString sCfgPathIni = m_pPm->ConfigFilePath();
    if (!sCfgPathIni.isEmpty()) {
        m_pCfgWatcher = new QFileSystemWatcher(
            QStringList{QFileInfo(sCfgPathIni).absoluteFilePath()}, this);
        m_pCfgReloadTm = new QTimer(this);
        m_pCfgReloadTm->setSingleShot(true);
        m_pCfgReloadTm->setInterval(1000);
        connect(m_pCfgWatcher, &QFileSystemWatcher::fileChanged, this,
                [this](const QString& sPath) {
                    if (m_pCfgWatcher != nullptr && !m_pCfgWatcher->files().contains(sPath)) {
                        m_pCfgWatcher->addPath(sPath);   // 写入可能触发 watch 移除，重挂
                    }
                    m_pCfgReloadTm->start();
                });
        connect(m_pCfgReloadTm, &QTimer::timeout, this, &CMainWindow::OnConfigFileChanged);
    }

    // 2026-10-02 助手运行态孤儿看门狗：10s 交叉核对面板 bRunning 与编排器会话活性，
    //   结束信号丢失（档切换竞态丢 finished 等）时自动恢复输入栏，不再永卡到重启。
    m_pAdhocHealthTm = new QTimer(this);
    m_pAdhocHealthTm->setInterval(10000);
    connect(m_pAdhocHealthTm, &QTimer::timeout, this, &CMainWindow::OnAdhocHealthCheck);
    m_pAdhocHealthTm->start();

    // 编排器 → 执行面板
    connect(m_pOrch, &CTaskOrchestrator::sigStageLog,   this, &CMainWindow::OnStageLog);
    connect(m_pOrch, &CTaskOrchestrator::sigStageEvent, this, &CMainWindow::OnStageEvent);
    connect(m_pOrch, &CTaskOrchestrator::sigTaskChanged,this, &CMainWindow::OnTaskChanged);
    connect(m_pOrch, &CTaskOrchestrator::sigTaskFinished,this, &CMainWindow::OnTaskFinished);
    // 2026.09.22 评审升级人工介入：评审多次驳回熔断暂停时弹窗问放行/再迭代/保持暂停。
    connect(m_pOrch, &CTaskOrchestrator::sigReviewEscalated, this, &CMainWindow::OnReviewEscalated);
    // 结构化对话流信号 → 直接接面板（渲染 Claude Code 风格对话流）
    connect(m_pOrch, &CTaskOrchestrator::sigAssistantText,
            m_pExecPanel, &CExecutionPanel::OnAssistantText);
    connect(m_pOrch, &CTaskOrchestrator::sigToolUse,
            m_pExecPanel, &CExecutionPanel::OnToolUse);
    connect(m_pOrch, &CTaskOrchestrator::sigToolResult,
            m_pExecPanel, &CExecutionPanel::OnToolResult);

    // 提示词 agent → 助手 Tab（独立对话流，不经任务）
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocAssistantText,
            m_pExecPanel, &CExecutionPanel::OnAdhocAssistantText);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocToolUse,
            m_pExecPanel, &CExecutionPanel::OnAdhocToolUse);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocToolResult,
            m_pExecPanel, &CExecutionPanel::OnAdhocToolResult);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocLog,
            m_pExecPanel, &CExecutionPanel::OnAdhocLog);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocEvent,
            m_pExecPanel, &CExecutionPanel::OnAdhocEvent);
    // 2026.09.18 暂存提示词生命周期：排队轮启动（面板此刻渲染正式用户块）/队列作废（清空暂存框）。
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocPendingStarted,
            m_pExecPanel, &CExecutionPanel::OnAdhocPendingStarted);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocPendingDropped,
            m_pExecPanel, &CExecutionPanel::OnAdhocPendingDropped);
    // 2026.09.20 暂存框编辑/删除未发送排队指令：面板 → 编排器 aPendingInjects 同步
    //   （双份队列，按原文本匹配；未命中=已被合并消费，面板侧已自行保持一致）。
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocQueuedRemove,
            m_pOrch, &CTaskOrchestrator::RemoveQueuedAdhoc);
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocQueuedEdit,
            m_pOrch, &CTaskOrchestrator::EditQueuedAdhoc);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocContext,
            m_pExecPanel, &CExecutionPanel::SetAdhocContext);
    // 2026.08.27 本轮实时计量（任务/助手通用）→ 费用栏右侧“本轮 ↑x ↓y”。
    //   2026.09.05 任务/助手分路：任务侧全局单值；助手侧带 id 路由到该助手页内标签。
    connect(m_pOrch, &CTaskOrchestrator::sigLiveUsage,
            m_pExecPanel, &CExecutionPanel::SetLiveUsage);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocLiveUsage,
            m_pExecPanel, &CExecutionPanel::OnAdhocLiveUsage);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocModel,
            m_pExecPanel, &CExecutionPanel::OnAdhocModel);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocUndo,
            m_pExecPanel, &CExecutionPanel::OnAdhocUndo);
    // 2026.08.22 多助手：一轮结束先回填运行助手会话（须在面板 OnAdhocFinished 清运行标记前执行，
    //   Qt 同线程信号按连接顺序派发，故本连接置于面板连接之前）。
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocFinished,
            this, &CMainWindow::OnAdhocRunFinished);
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocFinished,
            m_pExecPanel, &CExecutionPanel::OnAdhocFinished);
    // 2026.08.19 自动续发排队指令：编排器直接起新轮，面板无 UI 事件可感知——
    //   StartAdhocStage 起跑时广播运行态（手动发送路径面板已自置，此处幂等）。
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocRunning,
            m_pExecPanel, [this](const QString& a_sAsstId) {
        m_pExecPanel->SetAsstRunning(a_sAsstId, true);   // id 幂等置位（手动路径已自置）
        // 2026.09.23 起跑即同步粘性后端到面板镜像：助手输出块的说话人标签取镜像
        //   （v3.1.228 修正块标签用粘性后端），首提绑定/切后端后跑的会话镜像也能及时
        //   跟上，块标注不再靠兑底全局猜。
        if (m_pOrch != nullptr) {
            m_pExecPanel->UpdateAsstBackend(a_sAsstId, m_pOrch->AsstAdhocBackend(a_sAsstId));
        }
    });
    // 面板输入栏 → 编排器（经本窗口转发）
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocSubmit,
            this, &CMainWindow::OnAdhocSubmit);
    // 2026-09-22 助手右键「自动重命名」：面板收集提示词 → 编排器借会话交 Agent 总结。
    connect(m_pExecPanel, &CExecutionPanel::sigAsstAutoRename,
            this, &CMainWindow::OnAsstAutoRename);
    // 2026-10-08 提示词长度闸门：面板超限上抛 → 编排器起一次性提炼 Agent，
    //   提炼稿回投 SubmitAdhoc 正常提交（失败兑底原文，事件行留痕）。
    connect(m_pExecPanel, &CExecutionPanel::sigAsstRefinePrompt, this,
            [this](const QString& sId, const QString& sText, int iLimitTok, int iPerm) {
                m_pOrch->RefinePromptAsstNow(sId, sText, iLimitTok, iPerm);
            });
    // 2026.08.15 助手消耗归集：一次成功调用 → 当前档任务（费用/输入/输出/耗时）。
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocUsage,
            this, &CMainWindow::OnAdhocUsage);
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocUndo,
            this, &CMainWindow::OnAdhocUndo);
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocCompact,
            this, &CMainWindow::OnAdhocCompact);
    // #4 运行中点「停止」→ 编排器 StopAdhoc(id)（emit ⏹ 已停止 事件 + kill 该助手进程，
    //   不影响其他并发助手）
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocStop,
            this, [this](const QString& a_sAsstId) { m_pOrch->StopAdhoc(a_sAsstId); });
    // 2026-10-02 跨档幽灵接线：点击幽灵 Tab → 切回归属任务/工程档（同档幂等，无循环）；
    //   幽灵轮次结束 → 最终块落其档 + 面板除名 + 编排器会话释放。
    connect(m_pExecPanel, &CExecutionPanel::sigAsstForeignActivated,
            this, &CMainWindow::OnAsstForeignActivated);
    connect(m_pExecPanel, &CExecutionPanel::sigAsstForeignFinished,
            this, &CMainWindow::OnAsstForeignFinished);
    // （2026-10-15 sigAdhocContextLimitChanged→SaveAdhocContextLimit 接线已随
    //   「设置上下文大小」整线移除：窗口唯一来源 = 按模型自动探测。）
    // 2026-10-05 上下文条右键「选择模型」：面板菜单选择 → 四张节落盘 + 重应用 runner
    //   （下一轮即用新模型）+ 回推面板刷新进度条模型名/菜单勾选。
    connect(m_pExecPanel, &CExecutionPanel::sigSwitchSharedModel,
            this, &CMainWindow::OnSwitchSharedModel);
    // 2026-10-07 对话级模型覆盖（右键「选择模型」仅本助手）：面板 → 编排器会话快照
    //   （StartAdhocStage 每轮按身份解析注入该会话 runner；全局四张节/静态激活不动）。
    connect(m_pExecPanel, &CExecutionPanel::sigAsstModelOverride, m_pOrch,
            &CTaskOrchestrator::SetAsstModelOverride);

    // #1/#6 对话流持久化：助手流/sid 变化 → 写 assistant.json；任务流变化 → 写 chat.json。
    //   2026-10-04 lambda 包一层：SaveAssistantState 带 a_bForce 参数后，带默认参的
    //   成员函数指针不能直接连参更少的信号（Qt 指针连接不用默认参）。
    connect(m_pExecPanel, &CExecutionPanel::sigAdhocBlocksChanged,
            this, [this]() { SaveAssistantState(); });
    // 2026.08.22 多助手事件：切助手换载编排器会话；增删/改名/排序 → 落盘。
    connect(m_pExecPanel, &CExecutionPanel::sigAsstSwitched,
            this, &CMainWindow::OnAsstSwitched);
    connect(m_pExecPanel, &CExecutionPanel::sigAsstAdded,
            this, &CMainWindow::OnAsstAdded);
    connect(m_pExecPanel, &CExecutionPanel::sigAsstRemoved,
            this, &CMainWindow::OnAsstRemoved);
    connect(m_pExecPanel, &CExecutionPanel::sigAsstLayoutChanged,
            this, [this]() { SaveAssistantState(); });
    connect(m_pOrch, &CTaskOrchestrator::sigAdhocStateChanged,
            this, [this](const QString&) { SaveAssistantState(); });
    connect(m_pExecPanel, &CExecutionPanel::sigTaskChatChanged,
            this, [this](const QString& a_sTaskId) {
        if (m_pPm == nullptr || a_sTaskId.isEmpty()) {
            return;
        }
        const QByteArray aData =
            QJsonDocument(m_pExecPanel->TaskBlocksToJson(a_sTaskId)).toJson(QJsonDocument::Compact);
        m_pPm->WriteTaskChat(a_sTaskId, aData);
    });

    // 需求3：阶段表右键「再次执行」→ 编排器 RunSingleStage（仅该阶段，不推进后续）。
    connect(m_pExecPanel, &CExecutionPanel::sigRerunStage,
            this, [this](CTaskInfo* a_pTask, int a_eStage) {
        if (m_pOrch != nullptr && a_pTask != nullptr) {
            m_pOrch->RunSingleStage(a_pTask, static_cast<EStage>(a_eStage));
        }
    });

    // guide 生成回调
    connect(m_pPm, &CProjectManager::sigGuideLog,     this, &CMainWindow::OnGuideLog);
    connect(m_pPm, &CProjectManager::sigGuideFinished,this, &CMainWindow::OnGuideFinished);
    connect(m_pPm, &CProjectManager::sigGuideSyncFinished, this, &CMainWindow::OnGuideSyncFinished);

    // 启动 ask_user 提问服务：监听 AutoCodeMcpAsk-<PID>，供孙进程 MCP server 转发提问。
    StartAskServer();

    // 2026-09-26 内置测试沙箱：Agent 后代 GUI 窗口自动幽灵化收纳 + MP4 录像；
    //   同桥 ({type:sbx}) 承接 sandbox_* MCP 工具与 --sandbox CLI。退出时 closeEvent
    //   统一 Shutdown（全部还原 + 停录像）；启动时先还原上次崩溃残留的幽灵窗口
    //   （2026-10-08 起：工程内残留窗口改为续收纳保持隐形，详见 ReadoptProjectWindows）。
    m_pSbx = new CSandboxManager(this);
    {
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable     = m_pPm->LoadSandboxEnable();
        stCfg.bAutoGhost  = m_pPm->LoadSandboxAutoGhost();
        stCfg.bRecord     = m_pPm->LoadSandboxRecord();
        stCfg.iFps        = m_pPm->LoadSandboxFps();
        stCfg.iRetainDays = m_pPm->LoadSandboxRetainDays();
        m_pSbx->SetConfig(stCfg);
        m_pSbx->SetProjectRoot(m_pPm->ProjectDir());   // 启动自动恢复工程时供续收纳判定
        m_pSbx->Start();
    }

    // 图标/标题栏右键弹系统菜单：应用级原生消息过滤器（先于 Qt 内部处理，见类注释）。
    const HWND hWndMain = reinterpret_cast<HWND>(winId());
    m_pMsgFilter = new CTitleBarMsgFilter(
        hWndMain,
        [this](const QPoint& a_stAt) { OnTitleIconClicked(a_stAt); },
        [this](const QPoint& a_stAt) { OnTitleIconClicked(a_stAt); },
        [this](unsigned int a_uCmd) { OnSysCommand(a_uCmd); });
    QCoreApplication::instance()->installNativeEventFilter(m_pMsgFilter);

    // 任务栏点图标最小化硬伤：frameless 窗口是 WS_POPUP，天生无 WS_MINIMIZEBOX/
    // WS_MAXIMIZEBOX —— shell 判定窗口不可最小化，前台时点任务栏图标直接不发命令
    // （恢复不受影响，走的是激活路径）。补上样式即可；无标题栏时不画 min/max 按钮，
    // 无视觉副作用，样式只影响 shell 的行为判定。随后任务栏点图标 → shell 发
    // SC_MINIMIZE → 过滤器统一接管 → showMinimized()，与标题栏按钮同一出口。
    if (hWndMain != nullptr) {
        const LONG_PTR lStyle = GetWindowLongPtr(hWndMain, GWL_STYLE);
        SetWindowLongPtr(hWndMain, GWL_STYLE,
                         lStyle | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    }

    SetStatus(QStringLiteral("请打开或新建一个工程目录"));
}

// 2026.10.09 启动界面选定工程的打开入口（公有薄转调，见头文件注释）：多实例锁确认、
//   MRU 记录、标题/菜单刷新等全部复用 OpenProjectDir 既有路径，无第二实现。
// 2026.10.14 a_bAsWorkspace=true（启动页「跳过」）：打开 exe 同级 AutoCode 工作目录，
//   任务/助手全功能可用但不进最近列表（OpenProjectDir 的 RecordRecent 旁路）。
void CMainWindow::OpenStartupProject(const QString& a_sDir, bool a_bAsWorkspace) {
    if (a_sDir.isEmpty()) {
        return;
    }
    OpenProjectDir(a_sDir, /*a_bRecordRecent=*/!a_bAsWorkspace);
}

CMainWindow::~CMainWindow() {
    // 卸载并释放原生消息过滤器（先于 qApp 析构，防悬垂）
    if (m_pMsgFilter != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(m_pMsgFilter);
        delete m_pMsgFilter;
        m_pMsgFilter = nullptr;
    }
    // 2026-10-19 摘除 DSH 自愈失败提权回调（静态槽持 this，主窗先亡防悬垂）。
    CDeepSeekRunner::SetAclElevateHandler({});
    // 兜底落盘：sigChanged 已即时保存，此处防异常退出漏盘
    if (m_pPm != nullptr && !m_pPm->ProjectDir().isEmpty()) {
        for (CTaskInfo* pTask : m_aTasks) {
            if (pTask != nullptr) {
                m_pPm->InitTaskDir(pTask->Id());
                pTask->SaveToFile(m_pPm->TaskDir(pTask->Id()) + QStringLiteral("/task.json"));
            }
        }
        // 2026.09.07 v2.1.123 摘本实例持有的工程在席锁（崩溃/硬杀残留由后继打开者
        //   自动接管：2026-10-17 v4.4.387 起在席信号随进程消亡自动消失，文件锁
        //   残留由 PID 判活+创建时刻交叉验证收尾）。 
        m_pPm->DropProjectSeatEvent();   // 常规退出主动摘信号（进程退出句柄也会自动关，双保险）
        CProjectManager::ReleaseProjectLock(m_pPm->ProjectDir());
    }
    // 释放历史页 DB 副本（与活跃任务 m_aTasks 相互独立的对象集）
    for (CTaskInfo* pTask : m_aHistoryTasks) {
        delete pTask;
    }
    // m_pHistoryDb 为 unique_ptr，自动 Close 释放
}

void CMainWindow::CreateUi() {
    // FR-3：顶部自绘标题栏 + 主体 QSplitter，用一个 QVBoxLayout 包住作为 centralWidget。
    auto* pCentral = new QWidget(this);
    auto* pV = new QVBoxLayout(pCentral);
    // 四周留 1px：露出 paintEvent 自绘的窗口描边。标题栏是不透明背景（WA_StyledBackground +
    // #ffffff），margins=0 时会从 (0,0) 铺满、盖住顶边及左右上段的描边线（表现为整圈有线唯独标题栏缺）。
    pV->setContentsMargins(1, 1, 1, 1);
    pV->setSpacing(0);

    // 自绘标题栏（固定一行高）。三按钮信号连系统操作。
    m_pTitleBar = new CTitleBar(pCentral);
    connect(m_pTitleBar, &CTitleBar::sigMinimize, this, &QWidget::showMinimized);
    // 最大化/还原：按钮与标题栏双击（nativeEvent WM_NCLBUTTONDBLCLK）共用 OnMaximizeToggle，
    // 图标态与窗口动作永远同源（见该槽注释）。
    connect(m_pTitleBar, &CTitleBar::sigMaximizeToggle, this, &CMainWindow::OnMaximizeToggle);
    connect(m_pTitleBar, &CTitleBar::sigClose, this, &QWidget::close);
    // 左上角图标点击弹系统菜单：走 nativeEvent 的 HTSYSMENU 原生路径（见 OnTitleIconClicked）。
    pV->addWidget(m_pTitleBar);

    auto* pSplit = new QSplitter(Qt::Horizontal, pCentral);
    m_pMainSplit = pSplit;   // FR-4：记成成员，closeEvent 时 saveState 持久化比例
    m_pTaskPanel = new CTaskListPanel(pSplit);
    // 2026-10-17 自绘三期：对话日志渲染引擎构造时快照传入（[View] Engine 缺省
    //   Legacy 卡片流；ModelView=自绘链——面板/助手页建视图二选一，双引擎灰度）。
    m_pExecPanel = new CExecutionPanel(
        pSplit, CProjectManager::IsModelViewEngine(m_pPm->LoadViewEngine()));

    pSplit->addWidget(m_pTaskPanel);
    pSplit->addWidget(m_pExecPanel);
    pSplit->setStretchFactor(0, 1);
    pSplit->setStretchFactor(1, 3);
    pSplit->setSizes({340, 900});
    // 「当前任务」(左) 与「阶段」(右) 之间的分隔条收窄到 5px（默认 8px 显得过宽）。
    pSplit->setHandleWidth(5);
    pSplit->setChildrenCollapsible(false);   // 防误拖把面板折没
    // FR-4：恢复上次主分隔条比例（无记录/还原失败 → 维持上面 setSizes 的默认 340:900）。
    if (m_pPm != nullptr) {
        const QByteArray aState = m_pPm->LoadSplitterState();
        if (!aState.isEmpty()) {
            pSplit->restoreState(aState);
        }
        // 2026.08.18 恢复任务列表高度分隔条位置（左栏内部 垂直分隔条，[Layout] VSplit；
        //   无记录 → 保留面板 ctor setSizes 的默认比例）。
        m_pTaskPanel->RestoreVSplitState(m_pPm->LoadVSplitState());
        // 2026.08.29 主界面布局碎片补齐：历史任务列宽 / 左栏 Tab 页 / 阶段产物分割条
        //   （[Layout] TaskCols/TaskTab/ArtifactsSplit；坏档自弃回默认）。
        m_pTaskPanel->RestoreTaskCols(m_pPm->LoadLayoutValue(QStringLiteral("TaskCols")));
        m_pTaskPanel->RestoreTaskTab(m_pPm->LoadLayoutValue(QStringLiteral("TaskTab")).toInt());
        if (m_pExecPanel != nullptr) {
            const QString sArt = m_pPm->LoadLayoutValue(QStringLiteral("ArtifactsSplit"));
            if (!sArt.isEmpty()) {
                m_pExecPanel->RestoreArtifactsSplit(
                    QByteArray::fromBase64(sArt.toLatin1()));
            }
            // 2026-10-16 助手重命名弹窗几何（[Layout] RenameDlgGeo；坏档/空档自弃）。
            const QString sRename = m_pPm->LoadLayoutValue(QStringLiteral("RenameDlgGeo"));
            if (!sRename.isEmpty()) {
                m_pExecPanel->SetRenameDlgGeo(
                    QByteArray::fromBase64(sRename.toLatin1()));
            }
        }
    }
    pV->addWidget(pSplit, 1);

    setCentralWidget(pCentral);

    m_pLblStatus = new QLabel(this);
    statusBar()->addWidget(m_pLblStatus, 1);

    // 面板信号
    connect(m_pTaskPanel, &CTaskListPanel::sigNewTask,      this, &CMainWindow::OnNewTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigStartTask,    this, &CMainWindow::OnStartTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigPauseTask,    this, &CMainWindow::OnPauseTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigStopTask,     this, &CMainWindow::OnStopTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigRollback,     this, &CMainWindow::OnRollback);   // FR-6
    // 2026.08.23 助手栏 ↶ 右侧▼（sigRollbackTurn）入口 v1.0.224 已删，此 connect 同步移除；
    //   代码回退唯一入口 = 任务列表「回退」按钮（OnRollback）。OnRollbackTurnOpen 保留供编排层调用。
    connect(m_pTaskPanel, &CTaskListPanel::sigDeleteTask,   this, &CMainWindow::OnDeleteTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigRemoveTask,   this, &CMainWindow::OnRemoveTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigTaskSelected, this, &CMainWindow::OnTaskSelected);
    connect(m_pTaskPanel, &CTaskListPanel::sigReviveTask,  this, &CMainWindow::OnReviveTask);
    connect(m_pTaskPanel, &CTaskListPanel::sigViewTask,    this, &CMainWindow::OnViewTask);   // FR-5
    connect(m_pTaskPanel, &CTaskListPanel::sigRenameTask,  this, &CMainWindow::OnRenameTask); // 2026.08.20 重命名
    connect(m_pTaskPanel, &CTaskListPanel::sigAutoNameTask, this, &CMainWindow::OnAutoNameTask); // 2026.08.20 自动命名
    connect(m_pOrch, &CTaskOrchestrator::sigAutoNameDone,   this, &CMainWindow::OnAutoNameDone); // 2026.08.20 Agent 起名回调
    // 2026-09-22 助手自动重命名回调：编排器 → 面板回填 sName/sDesc（含失败事件行）。
    connect(m_pOrch, &CTaskOrchestrator::sigAsstAutoNameDone, m_pExecPanel,
            &CExecutionPanel::ApplyAsstAutoName);
    // FR-3：资源管理器「添加到聊天框」→ 注入助手输入框。
    connect(m_pTaskPanel, &CTaskListPanel::sigAddToChat,
            m_pExecPanel, &CExecutionPanel::AppendToAdhocInput);
    // 2026.08.18 资源管理器双击文本文件 → 执行面板「助手」右侧 Tab 内嵌显示。
    //   2026-09-30 改 lambda 转发：ShowFileTab 新增 a_iLine 缺省参数，PMF 直连会因
    //   形参多于信号被 static_assert 拦下（缺省实参不参与 PMF 连接匹配）。
    connect(m_pTaskPanel, &CTaskListPanel::sigFileOpen,
            m_pExecPanel, [this](const QString& a_sAbs, bool a_bSwitch) {
        m_pExecPanel->ShowFileTab(a_sAbs, a_bSwitch);
    });
    // 2026-09-30 资源管理器右键「代码审查」→ 非模态审查对话框（ReviewSdk+当前后端）。
    //   2026-10-08 改为每次右键新建独立窗口（并存互不影响）——原先先关旧窗会连带
    //   停掉正在跑的审查（关闭即析构 → Service::Stop）；对话框双击发现行 → 文件预览定位。
    connect(m_pTaskPanel, &CTaskListPanel::sigCodeReview,
            this, [this](const QString& a_sAbsPath) {
        auto* pDlg = new CCodeReviewDialog(a_sAbsPath, this);
        pDlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(pDlg, &CCodeReviewDialog::sigOpenFile,
                this, [this](const QString& a_sAbs, int a_iLine) {
            m_pExecPanel->ShowFileTab(a_sAbs, true, a_iLine);
            // 2026-10-08 双击发现行定位：审查窗是非模态顶层窗，可能挡住主窗口的
            //   预览页 → 定位后把主窗口带到前台（用户能立即看到定位结果）。
            show();
            raise();
            activateWindow();
        });
        // 2026-10-02「按建议修复」：修复提示词注入当前助手提问框（同「添加到聊天框」链路）。
        connect(pDlg, &CCodeReviewDialog::sigFixToChat,
                m_pExecPanel, &CExecutionPanel::AppendToAdhocInput);
        pDlg->show();
    });
    // FR-3：任务运行中介入输入栏 → 编排器 InjectTaskMessage。
    connect(m_pExecPanel, &CExecutionPanel::sigTaskInject,
            this, &CMainWindow::OnTaskInject);
    // （FR-9 阶段表高度对齐已随 2026.09.04 阶段图标条化移除：阶段条固定高 58px 横排，
    //   不再需要与左栏任务列表底部做 setSizes 同步。）
    // 2026.09.09 v2.1.137 移除「#6 启动自动恢复上次工程」块（用户需求：启动不自动
    //   加载最后的项目）——启动停在未开工程空态，工程经「最近打开」子菜单或
    //   「打开工程目录...」手动进入；OpenProjectDir 的 a_bAutoRestore 参数与其持锁
    //   静默跳过分支随之删除（该参数唯一调用方即本块）。LoadRecent 仍提前到
    //   CreateUi() 之前（最近子菜单依赖，勿回退）。
}

void CMainWindow::CreateMenus() {
    // FR-3：菜单下沉到左栏（CTaskListPanel）顶部，不再用窗口顶部 menuBar()。
    // Action 对象、层级、connect 全部保留，仅宿主迁移（AC-3.3/3.6 不丢项）。
    m_pMenuBar = new QMenuBar(m_pTaskPanel);
    m_pTaskPanel->MenuBarLayout()->addWidget(m_pMenuBar);

    auto* pMenu = m_pMenuBar->addMenu(QStringLiteral("工程(&P)"));

    // 2026.09.22 v3.0.205 移除「新建工程...」菜单项（用户需求：与打开工程重复）——
    //   两者实现除对话框标题外完全相同，且「打开工程目录...」的目录选择对话框
    //   本就支持选空目录从零建工程，功能完全被覆盖。
    auto* pActOpen = pMenu->addAction(QStringLiteral("打开工程目录..."));
    connect(pActOpen, &QAction::triggered, this, &CMainWindow::OnOpenProject);

    // 「最近打开」子菜单（位置：打开之后、Guide 之前）。2026.09.26 v3.1.239
    //   面板化：子菜单内嵌 CRecentChipsPanel（QWidgetAction）——自绘按钮 + 流式行
    //   布局 + 底部状态栏，取代旧 20 条 QAction。内嵌而非独立弹窗是关键决策：
    //   QMenu 打开期间独占鼠标事件路由，独立顶层窗口收不到点击；内嵌则子菜单
    //   全部语义白拿（悬停弹出/位置一致/移到其他菜单项自动隐藏/点按钮才收起）。
    m_pMenuRecent = pMenu->addMenu(QStringLiteral("最近打开"));
    m_pRecentPanel = new CRecentChipsPanel(
        [this](const QString& sPath) { OnRecentChipOpen(sPath); },
        [this]() { OnRecentChipMore(); },
        /*a_pParent=*/this,
        // 2026-10-09 拖拽尺寸持久化：调用时取当前 config.ini（exe 同级，PM 可能未建）
        [this]() -> QString {
            return (m_pPm != nullptr) ? m_pPm->ConfigFilePath() : QString();
        },
        // 2026-10-09 右键删除：PM 移除（含落盘）+ 面板就地刷新（exec 返回后重排）
        [this](const QString& sPath) {
            if (m_pPm != nullptr && m_pRecentPanel != nullptr) {
                m_pPm->RemoveRecent(sPath);
                m_pRecentPanel->Refresh(m_pPm->RecentProjects(),
                                        m_pPm->RecentOpenCounts());
            }
        });
    {
        auto* pWgtAct = new QWidgetAction(m_pMenuRecent);
        pWgtAct->setDefaultWidget(m_pRecentPanel);
        m_pMenuRecent->addAction(pWgtAct);
        // 每次弹出前取最新 MRU（打开/删除/热重载后均无需手工同步）
        connect(m_pMenuRecent, &QMenu::aboutToShow, this, [this]() {
            if (m_pPm != nullptr && m_pRecentPanel != nullptr) {
                m_pRecentPanel->Refresh(m_pPm->RecentProjects(),
                                        m_pPm->RecentOpenCounts());
            }
        });
    }

    auto* pActSyncGuide = pMenu->addAction(QStringLiteral("同步 Guide"));
    pActSyncGuide->setToolTip(QStringLiteral(
        "按 record.md 新增改动记录增量更新 .autodev/guide.md（任务完成/助手修改后自动触发，此项为手动）"));
    connect(pActSyncGuide, &QAction::triggered, this, &CMainWindow::OnSyncGuide);

    pMenu->addSeparator();
    pMenu->addAction(QStringLiteral("退出"), this, &QWidget::close);

    // 功能菜单（2026.08.15 设置在前、环境安装在后）
    auto* pMenuFunc = m_pMenuBar->addMenu(QStringLiteral("功能(&F)"));
    // 2026.08.15 设置：集中编辑 config.ini 可调参数（策略/Agent/评审/助手/目录排除）。
    auto* pActSettings = pMenuFunc->addAction(QStringLiteral("设置..."));
    pActSettings->setToolTip(QStringLiteral(
        "编辑运行策略、Agent 参数、评审与助手行为、目录排除等配置（保存到 exe 同级 config.ini）"));
    connect(pActSettings, &QAction::triggered, this, &CMainWindow::OnSettings);
    // 2026-10-14 任务编排设计：任务类型/流程档位/执行阶段动态编排（含阶段要求编辑）。
    auto* pActOrch = pMenuFunc->addAction(QStringLiteral("任务编排设计..."));
    pActOrch->setToolTip(QStringLiteral(
        "编排任务类型、流程档位与执行阶段（增删改/缺省选择/阶段要求提示词编辑），\n"
        "新建任务按编排后的最新版本显示与执行；新阶段提示词存 prompts 目录"));
    connect(pActOrch, &QAction::triggered, this, &CMainWindow::OnOrchDesign);
    // 2026.09 后端版本管理（任务 23183ccb）：四后端当前/最新版本与更新分派；
    //   设置页 Agent 组有同源入口。
    auto* pActVersion = pMenuFunc->addAction(QStringLiteral("后端版本管理..."));
    pActVersion->setToolTip(QStringLiteral(
        "查看四 Agent 后端当前版本与远端最新版，并提供各自更新入口"));
    connect(pActVersion, &QAction::triggered, this, &CMainWindow::OnVersionManage);
    auto* pActInstallEnv = pMenuFunc->addAction(QStringLiteral("环境安装..."));
    pActInstallEnv->setToolTip(QStringLiteral(
        "自动安装 Claude Code 运行所需环境：Git、Node.js、Claude Code CLI"));
    connect(pActInstallEnv, &QAction::triggered, this, &CMainWindow::OnInstallEnv);
    // 2026.08.26 恢复无任务也可用的回退入口：v1.0.224 删了助手侧入口后回退只剩任务列表
    //   按钮（需选中任务），未选任务时功能彻底不可达。菜单入口走 OnRollbackTurnOpen：
    //   选中任务 → 按该任务档列版本；无任务 → 工程全局档（owner=global）。
    auto* pActRollback = pMenuFunc->addAction(QStringLiteral("回退历史版本..."));
    pActRollback->setToolTip(QStringLiteral(
        "查看/回退代码备份历史（选中任务时按任务档过滤，无任务时为工程全局档）"));
    connect(pActRollback, &QAction::triggered, this, &CMainWindow::OnRollbackTurnOpen);
    // 2026-09-26 内置测试沙箱：Agent 启动的 GUI 窗口自动隐藏收纳（无扰测试 + MP4 录像）。
    auto* pActSandbox = pMenuFunc->addAction(QStringLiteral("测试沙箱..."));
    pActSandbox->setToolTip(QStringLiteral(
        "查看/管理内置测试沙箱：Agent 启动的 GUI 程序窗口自动隐藏收纳（不抢焦点、"
        "点击穿透、不打扰用户），Agent 经 sandbox_* 工具操控/截图，全程 MP4 录像"));
    connect(pActSandbox, &QAction::triggered, this, &CMainWindow::OnSandboxDialog);

    // 帮助菜单（FR-9）
    auto* pMenuHelp = m_pMenuBar->addMenu(QStringLiteral("帮助(&H)"));
    auto* pActHelp = pMenuHelp->addAction(QStringLiteral("帮助"));
    connect(pActHelp, &QAction::triggered, this, &CMainWindow::OnHelp);
    auto* pActAbout = pMenuHelp->addAction(QStringLiteral("关于"));
    connect(pActAbout, &QAction::triggered, this, &CMainWindow::OnAbout);
}

void CMainWindow::SetStatus(const QString& a_sText) {
    if (m_pLblStatus) {
        m_pLblStatus->setText(a_sText);
    }
}

// FR-3：拦截 Win32 WM_NCHITTEST——把标题栏空白区标记为可拖拽（HTCAPTION），
// 保留系统对边缘缩放（HTLEFT/HTTOP/...）的处理；按钮区 return false 让 Qt 派发 click。
// 处理函数只做坐标比较（经 CTitleBar::HitTest），无分配/无 widget 树遍历，满足高频消息性能约束。
bool CMainWindow::nativeEvent(const QByteArray& a_sType, void* a_pMsg, qintptr* a_piRes) {
#ifdef Q_OS_WIN
    if (a_sType == "windows_generic_MSG" && a_pMsg != nullptr && a_piRes != nullptr) {
        MSG* pMsg = static_cast<MSG*>(a_pMsg);
        // 图标区（HTSYSMENU）左键/右键按下 → 弹系统菜单（全原生路径，不经 Qt 鼠标事件）。
        if (m_pTitleBar != nullptr && pMsg->wParam == HTSYSMENU &&
            (pMsg->message == WM_NCLBUTTONDOWN || pMsg->message == WM_NCRBUTTONDOWN)) {
            OnTitleIconClicked();
            *a_piRes = 0;
            return true;   // 拦掉默认（DefWindowProc 对 frameless 无系统菜单默认行为）
        }
        // 图标区双击：Windows 原生默认 = 关闭窗口（SC_CLOSE），对本程序太危险，直接吞掉。
        if (pMsg->message == WM_NCLBUTTONDBLCLK && pMsg->wParam == HTSYSMENU) {
            *a_piRes = 0;
            return true;
        }
        if (pMsg->message == WM_NCLBUTTONDBLCLK && m_pTitleBar != nullptr) {
            // 标题栏空白双击 = 点最大化按钮：与按钮共用 OnMaximizeToggle（图标态同源翻转）；
            // 拦掉 DefWindowProc 默认（native 路径不同步按钮图标，会造成两边状态打架）。
            // 2026.10.09 DPI 修复：lParam 是物理像素，Qt 几何（mapFromGlobal）是 DIP，
            //   非 100% 缩放下须除以 devicePixelRatioF 再比较，否则命中错位（双击失效）。
            const double dDpr = devicePixelRatioF();
            const QPoint stDb(QPoint(qRound(GET_X_LPARAM(pMsg->lParam) / dDpr),
                                     qRound(GET_Y_LPARAM(pMsg->lParam) / dDpr)));
            if (m_pTitleBar->HitTest(stDb) == CTitleBar::HitCaption) {
                OnMaximizeToggle();
                *a_piRes = 0;
                return true;
            }
            return false;   // 边缘等区域双击交系统默认
        }
        if (pMsg->message == WM_NCHITTEST && m_pTitleBar != nullptr) {
            // 2026.10.09 DPI 修复：lParam 是物理像素，Qt 几何（HitTest/frameGeometry）
            //   是 DIP。非 100% 缩放下（125%/150%）两者差 N 倍，直接比较导致标题栏
            //   拖不动、关闭按钮点不到、边缘缩放带永久错位——统一除以 devicePixelRatioF
            //   转成 DIP 再参与全部 Qt 侧命中判定。
            const double dDpr = devicePixelRatioF();
            const QPoint stGlobal(QPoint(qRound(GET_X_LPARAM(pMsg->lParam) / dDpr),
                                         qRound(GET_Y_LPARAM(pMsg->lParam) / dDpr)));
            // 先判标题栏：按钮区交还 Qt，图标区返回 HTSYSMENU，空白区返回 Caption。
            switch (m_pTitleBar->HitTest(stGlobal)) {
                case CTitleBar::HitButton:
                    return false;   // 让 Qt 把 click 送到按钮（否则按钮点不动）
                case CTitleBar::HitIcon:
                    *a_piRes = HTSYSMENU;   // 系统菜单区：随后的 WM_NCLBUTTONDOWN 弹菜单
                    return true;
                case CTitleBar::HitCaption:
                    *a_piRes = HTCAPTION;
                    return true;    // 可拖拽 + 双击最大化
                case CTitleBar::HitNone:
                default:
                    break;          // 落到下面的边缘缩放判定
            }
            // 最大化时不启用边缘缩放（最大化态无边框，避免鼠标贴近屏幕边缘误触缩放光标）。
            // 用 Win32 IsZoomed 读窗口真实最大化态（与视觉一致）；缩放带 8px。
            const HWND hWndHit = pMsg->hwnd;
            if (hWndHit != nullptr && IsZoomed(hWndHit)) {
                return false;
            }
            // 边缘缩放带（窗口相对坐标，约 8px）：交还系统做 HTLEFT/HTTOP/... 判定。
            const QRect rWin = frameGeometry();
            const int iEdge = 8;
            const int iX = stGlobal.x() - rWin.left();
            const int iY = stGlobal.y() - rWin.top();
            if (iX < iEdge && iY < iEdge)                { *a_piRes = HTTOPLEFT;    return true; }
            if (iX > rWin.width() - iEdge && iY < iEdge) { *a_piRes = HTTOPRIGHT;   return true; }
            if (iX < iEdge && iY > rWin.height() - iEdge){ *a_piRes = HTBOTTOMLEFT; return true; }
            if (iX > rWin.width() - iEdge &&
                iY > rWin.height() - iEdge)              { *a_piRes = HTBOTTOMRIGHT;return true; }
            if (iX < iEdge)                              { *a_piRes = HTLEFT;   return true; }
            if (iX > rWin.width() - iEdge)               { *a_piRes = HTRIGHT;  return true; }
            if (iY < iEdge)                              { *a_piRes = HTTOP;    return true; }
            if (iY > rWin.height() - iEdge)              { *a_piRes = HTBOTTOM; return true; }
            return false;   // 其余交系统默认
        }
    }
#else
    Q_UNUSED(a_sType); Q_UNUSED(a_pMsg); Q_UNUSED(a_piRes);
#endif
    return QMainWindow::nativeEvent(a_sType, a_pMsg, a_piRes);
}

// 需求6：按「图标态」判最大化/还原翻转方向（图标与动作同源，永远自洽；图标状态只由
// 本槽掌管，无异步回滚）。标题栏双击（nativeEvent 的 WM_NCLBUTTONDBLCLK）与最大化
// 按钮共用此入口，保证双击后按钮图标同样翻转。
void CMainWindow::OnMaximizeToggle() {
    if (m_pTitleBar == nullptr) {
        return;
    }
    if (m_pTitleBar->IsMaxState()) {        // 当前图标=还原 □（已最大化）
        showNormal();
        m_pTitleBar->SetMaximized(false);   // 图标 → 最大化 ❐
    } else {                                // 当前图标=最大化 ❐（未最大化）
        showMaximized();
        m_pTitleBar->SetMaximized(true);    // 图标 → 还原 □
    }
}

// 标题栏图标点击 → 弹系统菜单样式弹出菜单（还原/移动/大小/最小化/最大化/关闭）。
// 触发路径全原生：WM_NCHITTEST 图标区返回 HTSYSMENU → 应用级消息过滤器截获
// WM_NCLBUTTONDOWN(wParam=HTSYSMENU) 调本函数（Qt 鼠标事件分发路径不可靠，弃用）。
// 硬伤修复（v1.0.12）：Qt frameless 窗口无 WS_SYSMENU 样式 → GetSystemMenu 恒返 NULL，
// 拿不到系统菜单副本；改为 CreatePopupMenu 自建菜单（同为原生弹出控件，外观一致），
// 菜单项 ID 直接用 SC_*，选中后 PostMessage(WM_SYSCOMMAND) —— 过滤器再统一接管。
void CMainWindow::OnTitleIconClicked(const QPoint& a_stAt) {
#ifdef Q_OS_WIN
    const HWND hWnd = reinterpret_cast<HWND>(winId());
    if (hWnd == nullptr) {
        return;
    }
    // 自建菜单（绕开 GetSystemMenu NULL 硬伤）。置灰规则与系统一致：
    // 最大化态 → 还原可用、移动/大小/最大化灰；正常态反之。
    HMENU hMenu = CreatePopupMenu();
    if (hMenu == nullptr) {
        return;
    }
    const BOOL bZoomed = IsZoomed(hWnd);
    const UINT uFlagRest = bZoomed ? MF_ENABLED : MF_GRAYED;    // 最大化态才可还原
    const UINT uFlagNorm = bZoomed ? MF_GRAYED : MF_ENABLED;    // 移动/大小/最大化仅正常态
    AppendMenu(hMenu, MF_STRING | uFlagRest, SC_RESTORE, L"还原(&R)");
    AppendMenu(hMenu, MF_STRING | uFlagNorm, SC_MOVE,    L"移动(&M)");
    AppendMenu(hMenu, MF_STRING | uFlagNorm, SC_SIZE,    L"大小(&S)");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, SC_MINIMIZE, L"最小化(&N)");
    AppendMenu(hMenu, MF_STRING | uFlagNorm, SC_MAXIMIZE, L"最大化(&X)");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, SC_CLOSE, L"关闭(&C)    Alt+F4");

    // 锚点：优先调用方给定（标题栏右键=光标处，物理坐标）；空点 → 图标左下角（原生样式）；
    // 取不到图标则退回鼠标位置。TrackPopupMenu 需要物理像素坐标，而 IconPopupPos
    // （mapToGlobal）返回 DIP——2026.10.09 DPI 修复：非 100% 缩放下乘 devicePixelRatioF
    // 换回物理，否则系统菜单弹出位置偏左上。
    QPoint stPos = a_stAt;
    if (stPos.isNull() && m_pTitleBar != nullptr) {
        const double dDpr = devicePixelRatioF();
        const QPoint stDip = m_pTitleBar->IconPopupPos();
        stPos = QPoint(qRound(stDip.x() * dDpr), qRound(stDip.y() * dDpr));
    }
    if (stPos.isNull()) {
        POINT stPt;
        GetCursorPos(&stPt);
        stPos = QPoint(stPt.x, stPt.y);
    }
    // SetForegroundWindow 是 TrackPopupMenu 点击菜单外区域自动关闭的前提。
    SetForegroundWindow(hWnd);
    const INT iCmd = TrackPopupMenu(hMenu,
                                    TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                                    stPos.x(), stPos.y(), 0, hWnd, nullptr);
    DestroyMenu(hMenu);                 // 弹出菜单用毕即毁（TPM_RETURNCMD 不自动销毁）
    PostMessage(hWnd, WM_NULL, 0, 0);   // 配合上面的前台切换，确保菜单随后能正常收尾
    if (iCmd != 0) {
        PostMessage(hWnd, WM_SYSCOMMAND, static_cast<WPARAM>(iCmd), 0);
    }
#endif
}

// 系统命令统一入口（图标菜单/任务栏菜单/Alt+F4 都汇到这里）：
// 最大化/还原经 OnMaximizeToggle —— 与按钮、标题栏双击同一翻转原语，图标态单一来源永不错位；
// 最小化/关闭直接走 Qt 路径。SC_MOVE/SC_SIZE 不到这里（过滤器已放行交系统模态循环）。
// SC_RESTORE 语义二分（修复：最小化后点任务栏无法恢复——此前只看图标态，最小化时图标态
// 恒为「非最大化」导致 no-op 且拦截了系统默认动作，窗口永远起不来）：
//   窗口最小化（IsIconic）→ SC_RESTORE 是「从最小化恢复」：ShowWindow(SW_RESTORE) 恢复到
//     最小化前的状态（含最大化），图标由 showEvent 按 IsZoomed 校准，不能走 OnMaximizeToggle
//     （那会强制 showNormal，把「最大化+最小化」错误还原成普通态）；
//   窗口最大化（未最小化）→ SC_RESTORE 是「还原到普通态」：走 OnMaximizeToggle。
void CMainWindow::OnSysCommand(unsigned int a_uCmd) {
    const HWND hWnd = reinterpret_cast<HWND>(winId());
    switch (a_uCmd) {
        case SC_MAXIMIZE:
            if (m_pTitleBar != nullptr && !m_pTitleBar->IsMaxState()) {
                OnMaximizeToggle();
            }
            return;
        case SC_RESTORE:
            if (hWnd != nullptr && IsIconic(hWnd)) {
                ShowWindow(hWnd, SW_RESTORE);   // 从最小化恢复（保留原最大化态），showEvent 校准图标
            } else if (m_pTitleBar != nullptr && m_pTitleBar->IsMaxState()) {
                OnMaximizeToggle();             // 最大化 → 普通态
            }
            return;
        case SC_MINIMIZE:
            showMinimized();
            return;
        case SC_CLOSE:
            close();
            return;
        default:
            return;
    }
}

// 需求6：最大化图标态只由按钮掌管；showEvent 仅负责首帧/从最小化恢复时按真实态(IsZoomed)初始化图标。
void CMainWindow::showEvent(QShowEvent* a_pEvent) {
    QMainWindow::showEvent(a_pEvent);
    if (m_pTitleBar != nullptr) {
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        if (hWnd != nullptr) {
            m_pTitleBar->SetMaximized(IsZoomed(hWnd) != FALSE);
        }
    }
}

// frameless 窗口无系统边框：自绘 1px 深色描边让窗口边界清晰。只画边框线，
// 不填充背景——客户区浅色底仍由 QMainWindow 默认 palette 提供，内容区不会被染深。
void CMainWindow::paintEvent(QPaintEvent* a_pEvent) {
    QMainWindow::paintEvent(a_pEvent);
    QPainter stPainter(this);
    stPainter.setPen(QPen(QColor(0x1f, 0x1f, 0x1f), 1));
    stPainter.drawRect(rect().adjusted(0, 0, -1, -1));   // 1px 内描边，adjusted 防 1px 画到像素外
}

// —— FR3 弹窗字体放大（正文 + 按钮） ——
void CMainWindow::ExecMsgBox(QMessageBox::Icon a_eIcon, const QString& a_sTitle,
                             const QString& a_sText, QWidget* a_pParent) {
    QMessageBox stBox(a_eIcon, a_sTitle, a_sText, QMessageBox::Ok, a_pParent);
    QFont fntBox = stBox.font();
    fntBox.setPointSize(s_kiMsgPt);
    stBox.setFont(fntBox);
    // R9：部分 Qt 风格下按钮不继承 box 字体，逐个显式设字号。
    for (QAbstractButton* pBtn : stBox.buttons()) {
        QFont fntBtn = pBtn->font();
        fntBtn.setPointSize(s_kiMsgPt);
        pBtn->setFont(fntBtn);
    }
    stBox.exec();
}

QMessageBox::StandardButton CMainWindow::ExecQuestion(const QString& a_sTitle,
                                                     const QString& a_sText,
                                                     QWidget* a_pParent) {
    QMessageBox stBox(QMessageBox::Question, a_sTitle, a_sText,
                      QMessageBox::Yes | QMessageBox::No, a_pParent);
    QFont fntBox = stBox.font();
    fntBox.setPointSize(s_kiMsgPt);
    stBox.setFont(fntBox);
    for (QAbstractButton* pBtn : stBox.buttons()) {
        QFont fntBtn = pBtn->font();
        fntBtn.setPointSize(s_kiMsgPt);
        pBtn->setFont(fntBtn);
    }
    return static_cast<QMessageBox::StandardButton>(stBox.exec());
}

bool CMainWindow::RunAclElevateFlow(const QString& a_sTitle, const QStringList& a_aDirs,
                                    const QString& a_sCmds) {
    // 2026-10-19 三挂点共用的提权修复公共流（见头文件声明注释）。抑制键=native
    //   小写目录路径（QDir::toNativeSeparators + toLower，盘符大小写归一）。
    QStringList aKeys;
    for (const QString& s : a_aDirs) {
        aKeys << QDir::toNativeSeparators(s).toLower();
    }
    for (const QString& sKey : aKeys) {
        if (m_aAclElevateSuppressed.contains(sKey)) {
            return false;   // 本会话已拒绝/失败过：不再骚扰
        }
    }
    const QMessageBox::StandardButton eAns = ExecQuestion(a_sTitle,
        QStringLiteral("以下目录缺少当前账户的写权限，且自动修复失败\n"
                       "（管理员账户经 UAC 过滤令牌运行时常见）：\n\n%1\n\n"
                       "将以管理员权限执行：\n%2\n\n"
                       "修复后所有后端（Harness）均可使用这些目录。\n现在提权修复吗？")
            .arg(a_aDirs.join(QLatin1Char('\n')), a_sCmds), this);
    if (eAns != QMessageBox::Yes) {
        for (const QString& sKey : aKeys) {
            m_aAclElevateSuppressed.insert(sKey);
        }
        return false;
    }
    // runas 提权执行 + 有界等待（用户停在 UAC 框时主窗短暂无响应属预期——安全
    //   桌面本就独占输入）；失败统一登记抑制并给出手工命令兜底。
    if (!CDeepSeekRunner::RunElevatedCmdBlocking(a_sCmds, 90000)) {
        for (const QString& sKey : aKeys) {
            m_aAclElevateSuppressed.insert(sKey);
        }
        ExecMsgBox(QMessageBox::Warning, a_sTitle,
                   QStringLiteral("提权修复未完成（UAC 被拒或命令失败）。\n可手工执行：\n%1")
                       .arg(a_sCmds), this);
        return false;
    }
    return true;   // 命令已成功执行；修复复查归调用方（口径随挂点不同）
}

void CMainWindow::UpdateWindowTitle() {
    // 2026.09.07 需求：标题＝「工程名-AutoCode 版本」（未开工程＝「AutoCode 版本」）。
    //   窗口标题（WM_GETTEXT）是任务管理器/Alt-Tab 悬停与展开列表的信息源——多实例
    //   （Execute 主/副本）各开不同工程时可直接辨认；自绘标题栏左上角同步刷工程名。
    //   工程名取工程目录末段（与最近打开列表/任务目录口径一致），空目录（未开工程）
    //   回退纯 AutoCode 标题。2026.10.14 「跳过」工作目录（exe 同级 \AutoCode）
    //   工程名恰好也叫 AutoCode，拼出「AutoCode-AutoCode」难看，同口径回退纯标题。
    const QString sName = QFileInfo(m_pPm->ProjectDir()).fileName();
    if (sName.isEmpty() || IsWorkspaceDir(m_pPm->ProjectDir())) {
        setWindowTitle(QStringLiteral("AutoCode ") + AutoCodeVersionText());
        if (m_pTitleBar != nullptr) {
            m_pTitleBar->SetTitleText(QStringLiteral("AutoCode"));
        }
        return;
    }
    setWindowTitle(QStringLiteral("%1-AutoCode %2").arg(sName, AutoCodeVersionText()));
    if (m_pTitleBar != nullptr) {
        m_pTitleBar->SetTitleText(QStringLiteral("%1-AutoCode").arg(sName));
    }
}

// 2026.10.14 是否为「跳过」模式的 AutoCode 工作目录（exe 同级 \AutoCode）：路径
//   双向 cleanPath 后按 Windows 习惯大小写不敏感比较（exe 目录可能与工程目录仅
//   盘符/段大小写不同）。
bool CMainWindow::IsWorkspaceDir(const QString& a_sDir) {
    if (a_sDir.isEmpty()) {
        return false;
    }
    const QString sWs = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                        + QStringLiteral("/AutoCode"));
    const QString sDir = QDir::cleanPath(a_sDir);
    return sDir.compare(sWs, Qt::CaseInsensitive) == 0;
}

void CMainWindow::ClearAssistantSessionsOnBackendSwitch() {
    // 2026.09.20 v2.4.193 语义变更（用户需求：助手不支持中途换 Harness → 发送提示词时
    //   自动忽略全局设置、继续用该助手当前的 Harness）：**不再清会话链**——粘性绑定由
    //   会话 sBackend 承载（EnsureAdhocRunner 按绑定后端建 runner，v2.4.183 的清除
    //   语义废止）。此处仅状态栏提示新规则（新建助手才会用新后端）。
    SetStatus(QStringLiteral("Agent 后端已切换：已有助手继续使用各自原后端；"
                             "新建助手将使用新后端"));
}

void CMainWindow::ApplyTaskConfigOverlay(CTaskInfo* a_pTask) {
    // 2026.09.09 v2.1.143 任务级配置覆盖（说明见 CProjectManager.h 同名节）：
    //   选中任务 → 参数源切到任务档（缺键合并回填自全局）；取消选中/删除/切工程 →
    //   回全局。切换后按生效配置重应用运行期（含助手上下文上限——启动路径只回填
    //   一次全局值，此处随选中刷新）。
    if (m_pPm == nullptr) {
        return;
    }
    m_pPm->SetTaskConfigOverlay(a_pTask != nullptr ? a_pTask->Id() : QString());
    ApplyAgentConfigToRuntime(false);   // 静默口径（后端不可用警告属启动/保存路径）
}

void CMainWindow::OpenProjectDir(const QString& a_sDir, bool a_bRecordRecent) {
    // 2026.09.07 v2.1.123 多实例工程锁（查锁必须最先做：取消时零副作用）：
    //   工程态（assistant.json/task.json/guide/record）是整文件后写覆盖先写，
    //   双实例同开一工程会互踩（仅 history.db 并发安全）。他实例活持有 →
    //   阻断式确认（默认 No；原启动自动恢复路径的静默跳过分支已随 v2.1.137
    //   移除自动恢复一并删除）。
    //   崩溃残留锁由 IsLockHolderAlive 按 PID 存活+进程名复核自动接管。
    QString sLockDesc;
    if (CProjectManager::IsProjectHeldByOther(a_sDir, &sLockDesc)) {
        const QMessageBox::StandardButton eLock = QMessageBox::question(
            this, QStringLiteral("工程已被其他实例打开"),
            QStringLiteral("该工程正被另一个 AutoCode 实例使用：\n%1\n\n"
                           "两个实例同时操作同一工程时，助手对话、任务状态、"
                           "guide/record 等会以后写覆盖先写互相冲掉"
                           "（仅历史库 history.db 并发安全）。\n"
                           "建议改开别的工程，或本实例只浏览不提交。\n\n"
                           "仍要在此实例打开吗？").arg(sLockDesc),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (eLock != QMessageBox::Yes) {
            return;
        }
    }
    const QString sPrevProject = m_pPm->ProjectDir();   // 切走后摘旧工程的锁
    // 2026-10-04 切工程隔离窗开启：自此至 LoadAssistantState 收尾，一切信号驱动的
    //   assistant.json 落盘与自动命名轮全部旁路（成员注释详见 CMainWindow.h）。
    //   实害：旧工程助手停链中会同步触发命名子轮（收尾自动命名/挂起命名执行），
    //   ClearTasks 兑底 StopAllAdhoc 的 ⏹/落盘链会把整版旧工程助手写进【新工程】档。
    m_bProjectSwitching = true;
    // #1/#6 切工程前：先停在跑的助手（⏹ 停止事件此刻仍写【旧】工程的档——之后 m_pPm 将指向
    //   新工程，若等 ClearTasks 再停，旧对话的尾块会被写进新工程的全局 assistant.json），
    //   再把当前工程的助手对话流落盘（Open 后 m_pPm 指向新工程就丢了上下文）。
    if (m_pOrch != nullptr && m_pOrch->IsAnyAdhocRunning()) {
        m_pOrch->StopAllAdhoc();   // 2026.09.03 多助手并发：整组停（各 ⏹ 痕迹落旧档）
    }
    SaveAssistantState(/*a_bForce=*/true);   // 隔离窗内唯一放行：旧工程终快照

    // FR-7 最低验收：切工程前停止在跑的任务 runner + 编译门，并置空编排器 m_pCurTask，
    // 使后续 ClearTasks 的 delete pTask 不触发 runner 异步 finished 回调的悬垂访问（崩溃根因）。
    if (m_pOrch != nullptr) {
        m_pOrch->StopAll();
        // 2026-10-04 会话先死、后切目标：m_pPm->Open 指向新工程之前整组释放助手会话
        //   （含停链中可能新起的命名子轮、3s 自动命名重试定时器的会话依赖）——此后
        //   不存在任何能发射助手落盘信号的源头，ClearTasks 内的 StopAllAdhoc/
        //   ResetAdhocSessions 成为真空操作，跨工程串档窗口彻底关闭。
        m_pOrch->ResetAdhocSessions();
    }

    if (!m_pPm->Open(a_sDir)) {
        m_bProjectSwitching = false;   // 失败路径关闭隔离窗（旧面板未动，恢复正常落盘）
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("错误"),
                   QStringLiteral("无法打开目录：%1").arg(a_sDir), this);
        return;
    }
    m_pPm->EnsureAutoDev();

    UpdateWindowTitle();   // 2026.09.07 标题刷成「工程名-AutoCode 版本」（任务管理器可辨认）

    // 2026-10-19 工程根+关联目录写权限 ACE 预检（全后端通用，不限 DSH）：静默自愈
    //   失败（过滤令牌下 SetNamedSecurityInfoW Win32 5）→ 一次提权确认（目录清单
    //   = 工程根 + 配置的额外可写目录中自愈失败者）→ runas icacls 账户级 F → 复查。
    //   账户级 F 是通用钥匙：DSH 沙箱 grantWrite/各后端 Harness 内部的 DACL 授权
    //   都跑在用户令牌下，账户拿回 WRITE_DAC/WRITE_OWNER 后全部恢复（S-1-4 能力
    //   SID 精确条目 icacls 表达不了，仍由各后端非提权自愈补齐）。插在 Open 成功
    //   后、座位锁换岗（首次向工程根写文件）前。健康目录静默自愈成功，零打扰；
    //   修复失败不 return——工程照常打开，与 EnsureProcess 自愈「不阻断」口径一致。
    {
        QStringList aNeed;
        if (CDeepSeekRunner::EnsureDirWriteAce(m_pPm->ProjectDir()) < 0) {
            aNeed << m_pPm->ProjectDir();
        }
        for (const QString& sDir : CDeepSeekRunner::DshExtraWriteDirs()) {
            if (CDeepSeekRunner::EnsureDirWriteAce(sDir) < 0 && !aNeed.contains(sDir)) {
                aNeed << sDir;   // 目录不在场时 EnsureDirWriteAce 返回 0，自动跳过
            }
        }
        QStringList aCmds;
        for (const QString& sDir : aNeed) {
            aCmds << CDeepSeekRunner::BuildIcaclsGrantCmd(sDir);
        }
        aCmds.removeAll(QString());
        if (!aCmds.isEmpty()
                && RunAclElevateFlow(QStringLiteral("目录权限修复"), aNeed,
                                     aCmds.join(QStringLiteral(" & ")))) {
            QStringList aStill;
            for (const QString& sDir : aNeed) {
                if (CDeepSeekRunner::EnsureDirWriteAce(sDir) != 0) {
                    aStill << sDir;
                }
            }
            for (const QString& sDir : aStill) {
                m_aAclElevateSuppressed.insert(QDir::toNativeSeparators(sDir).toLower());
            }
            ExecMsgBox(aStill.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
                       QStringLiteral("目录权限修复"),
                       aStill.isEmpty()
                           ? QStringLiteral("提权修复成功：已为 %1 个目录补当前账户写权限，"
                                            "所有后端（Harness）均可使用。")
                                 .arg(QString::number(aNeed.size()))
                           : QStringLiteral("以下目录提权修复后仍未通过：\n%1\n"
                                            "可手工执行：\n%2")
                                 .arg(aStill.join(QLatin1Char('\n')),
                                      aCmds.join(QStringLiteral(" & "))),
                       this);
        }
    }

    // 2026-10-21 沙箱关闭（danger-full-access 缺省）时，工程根/关联目录的沙箱遗留
    //   低完整性标签恢复：静默失败 → 复用提权流跑 icacls /setintegritylevel
    //   （替换式、子树继承自动跟随，一条清整树）。紧随上面的 ACE 预检，同一次
    //   打开至多一次 UAC；健康机器（无低标签）零提示。workspace-write 时代工程
    //   拷到新机器即命中，此处是发布场景的主入口（EnsureProcess 兜底运行中补）。
    if (CDeepSeekRunner::DshPermissionMode() == QStringLiteral("danger-full-access")) {
        QStringList aLblNeed;
        QStringList aLblCmds;
        QStringList aLblAll = { m_pPm->ProjectDir() };
        aLblAll += CDeepSeekRunner::DshExtraWriteDirs();
        for (const QString& sDir : aLblAll) {
            QString sCmd;
            if (CDeepSeekRunner::RestoreNeutralSecurity(sDir, &sCmd) < 0
                    && !sCmd.isEmpty()) {
                aLblNeed << sDir;
                aLblCmds << sCmd;
            }
        }
        if (!aLblNeed.isEmpty()
                && RunAclElevateFlow(QStringLiteral("目录权限修复"), aLblNeed,
                                     aLblCmds.join(QStringLiteral(" & ")))) {
            QStringList aStill;
            for (const QString& sDir : aLblNeed) {
                if (CDeepSeekRunner::RestoreNeutralSecurity(sDir) != 0) {
                    aStill << sDir;
                }
            }
            for (const QString& sDir : aStill) {
                m_aAclElevateSuppressed.insert(QDir::toNativeSeparators(sDir).toLower());
            }
            ExecMsgBox(aStill.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
                       QStringLiteral("目录权限修复"),
                       aStill.isEmpty()
                           ? QStringLiteral("提权修复成功：已清除 %1 个目录的沙箱遗留"
                                            "低完整性标签，普通程序（VS/MSBuild 等）"
                                            "可直接写入。")
                                 .arg(QString::number(aLblNeed.size()))
                           : QStringLiteral("以下目录标签恢复仍未完成：\n%1\n"
                                            "可手工执行：\n%2")
                                 .arg(aStill.join(QLatin1Char('\n')),
                                      aLblCmds.join(QStringLiteral(" & "))),
                       this);
        }
    }

    // 2026.09.07 v2.1.123 工程锁换岗：摘旧工程自己的锁 → 写新工程在席锁。
    //   2026-10-17 v4.4.387 在席信号同步换岗：先摘旧信号（句柄随旧工程失效）再
    //   持新信号；Hold 失败（强制打开下他实例仍持/信号层不可用）不阻断——降级
    //   文件锁口径继续，后续新实例探测时仍能识别。
    m_pPm->DropProjectSeatEvent();
    if (!sPrevProject.isEmpty()) {
        CProjectManager::ReleaseProjectLock(sPrevProject);
    }
    CProjectManager::WriteProjectLock(m_pPm->ProjectDir());
    m_pPm->HoldProjectSeatEvent(m_pPm->ProjectDir());

    // 2026.08.20 打开本工程的历史库（.autodev/history.db）：历史页只显示当前工程任务。
    if (m_pHistoryDb) {
        m_pHistoryDb->Close();
        m_pHistoryDb->Open(CTaskHistoryDB::ProjectDbPath(m_pPm->ProjectDir()));
    }

    // 加载该工程的全部历史任务（持久化恢复：终止后可从上次阶段继续）
    ClearTasks();
    QList<CTaskInfo*> aInterrupted;   // 2026.08.17 上次会话仍在运行的任务（异常退出残留）
    for (CTaskInfo* pTask : m_pPm->LoadAllTasks()) {
        pTask->SetProjectDir(m_pPm->ProjectDir());   // 以当前工程路径为准
        // 终态任务（Done/Failed）不进活跃列表——只入库归档，由历史表展示。
        // 否则它进 m_aTasks 后，ReloadHistory 按活跃集合把它从历史表过滤，
        // 造成「当前表不放终态、历史表又排除活跃」的两表间凭空消失
        // （含 tankwar 这类上个会话已完成、本次打开卡住的任务）。再次执行走历史表
        // 右键，OnReviveTask 经 DB 副本重建 task.json，不依赖此处是否载入活跃。
        if (pTask->Status() == ETaskStatus::Done ||
            pTask->Status() == ETaskStatus::Failed ||
            pTask->Archived()) {   // 2026.08.22 「移除」归档任务同理：只入历史库，不回当前表
            if (m_pHistoryDb) {
                m_pHistoryDb->Upsert(pTask);   // 确保终态记录在库（历史表由此展示）
            }
            delete pTask;   // 活跃列表不收，回收该副本（task.json 留底，不改不删）
            continue;
        }
        pTask->setParent(this);
        connect(pTask, &CTaskInfo::sigChanged, this, [this, pTask]() { SaveTask(pTask); });
        // Running 是易失运行态（进程已不存在），回退 Paused 以便「启动/继续」接续
        if (pTask->Status() == ETaskStatus::Running) {
            pTask->SetStatus(ETaskStatus::Paused);   // 触发 sigChanged → 即时落盘修正
            aInterrupted.append(pTask);              // 2026.08.17 记入中断清单供恢复提示
        }
        m_aTasks.append(pTask);
        // 镜像进本工程历史库（幂等：主键 id，UUID 不撞；=.autodev/tasks 进 DB）
        if (m_pHistoryDb) {
            m_pHistoryDb->Upsert(pTask);
        }
    }
    m_pTaskPanel->SetTasks(m_aTasks);
    // FR-3：刷新资源管理器文件树根为新工程目录。
    m_pTaskPanel->SetProjectRoot(m_pPm->ProjectDir());
    ReloadHistory();   // 刷新全局历史页（含本次导入 + 其它工程历史）

    // 2026.08.17 异常退出恢复提示：task.json 里残留 Running = 上次会话未正常收尾（崩溃/断电/
    //   强杀）。已全部转 Paused（上文），此处主动询问是否立即续跑第一个——编排器 LaunchStage
    //   对带 lastSessionId 的任务走 --resume 断点续跑（会话上下文保留，不整阶段重跑）。
    //   正常切工程路径会先 StopAll（任务落 Stopped），不会触发本提示。
    if (!aInterrupted.isEmpty()) {
        QStringList aNames;
        for (const CTaskInfo* pTask : aInterrupted) {
            const SStageDef* pDef = FindStage(pTask->CurrentStage());
            aNames << QStringLiteral("· %1（阶段：%2）")
                          .arg(pTask->Name(),
                               pDef ? pDef->sNameZh : QStringLiteral("?"));
        }
        const QMessageBox::StandardButton eAns = QMessageBox::question(
            this, QStringLiteral("恢复中断任务"),
            QStringLiteral("检测到 %1 个任务在上次会话结束时仍在运行"
                           "（AutoCode 可能异常退出）：\n\n%2\n\n"
                           "已全部转为「暂停」。是否立即续跑第一个任务？"
                           "（其余可在任务列表中手动启动）")
                .arg(aInterrupted.size())
                .arg(aNames.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (eAns == QMessageBox::Yes) {
            OnStartTask(aInterrupted.first());   // 内部自带选中+busy+guide 校验
        }
    }

    const bool bHasGuide = m_pPm->HasGuide();
    SetStatus(QStringLiteral("工程：%1  |  guide.md：%2 | 已载入 %3 个任务")
                  .arg(m_pPm->ProjectDir(),
                       bHasGuide ? QStringLiteral("已就绪")
                                 : QStringLiteral("缺失（启动任务时自动生成）"))
                       .arg(m_aTasks.size()));
    // guide.md 缺失不再在打开工程时扫描，改为启动任务时作为初始任务自动生成（见 OnStartTask）。

    // 成功打开才记录历史（失败路径已提前 return，FR-2.2）。
    //   2026.10.14 a_bRecordRecent=false（启动页「跳过」的 AutoCode 工作目录）不记：
    //   工作目录不算工程，不进最近打开列表。
    // 注：2026.09.22 起「新建工程」菜单已删（与打开工程重复），从零建工程走
    //   OnOpenProject 选空目录，同样经此入口计入最近 —— 待 Q-6 确认的历史项。
    if (a_bRecordRecent) {
        RecordRecent(a_sDir);
    }

    // 工程就绪：启用提示词 agent 输入栏（ClearTasks 已先禁用，此处恢复）
    if (m_pExecPanel != nullptr) {
        m_pExecPanel->SetAdhocEnabled(true);
    }

    // #1 恢复该工程的助手会话链与对话流（sid/撤销栈/助手块）。
    //   2026-10-04 a_bProjectSwitch：切工程不搞跨档幽灵——外工程助手的 ownerKey 在
    //   新工程无意义（空串撞全局档 key，会被当本地助手落盘串档），一律丢弃。
    LoadAssistantState(/*a_bProjectSwitch=*/true);
    m_bProjectSwitching = false;   // 隔离窗关闭：新工程档已恢复，恢复正常落盘

    // 统一备份（方案 §4.4）：打开工程时探测回退中断残留——还原现场（幂等安全），
    // 用户确认目标后可重新发起回退。孤儿 blob 已在 CProjectManager::Open 尾后台节流
    //   GC（2026-10-08：巨型工程同步全量解析卡 UI 改异步 24h 节流，详见函数注释）。
    if (m_pPm->HasRestoreTmp()) {
        QString sErr;
        if (m_pPm->RestoreFromTmp(sErr)) {
            ExecMsgBox(QMessageBox::Information, QStringLiteral("代码回退"),
                QStringLiteral("检测到上次回退被中断，已将工程还原到回退前状态。"
                               "如需继续，请重新发起回退。"), this);
        } else {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("代码回退"),
                QStringLiteral("检测到上次回退被中断的残留，还原现场失败：%1").arg(sErr), this);
        }
    }

    // 2026-10-08 沙箱跨实例续收纳：工程切换后刷新判定根，把「上个 AutoCode 实例
    //   收纳中、进程未退出、exe 属于本工程」的测试窗口重新入册保持隐形——否则
    //   它们既被退出还原成了可见窗口，新实例的进程树出身判定又永不再认（已退出
    //   实例的后代），用户桌面被测试窗口打扰（Compositer 工程实测）。
    if (m_pSbx != nullptr) {
        m_pSbx->SetProjectRoot(m_pPm->ProjectDir());
        m_pSbx->ReadoptProjectWindows();
    }
}

void CMainWindow::OnOpenProject() {
    // 2026.08.20 初始定位=上次选择的目录（config.ini [UI] LastProjectDir），无记录回退 exe 目录。
    const QString sDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("打开工程目录"), m_pPm->LastBrowseDir());
    if (!sDir.isEmpty()) {
        m_pPm->SetLastBrowseDir(sDir);
        m_iCfgSelfSaveMs = QDateTime::currentMSecsSinceEpoch();   // 2026.08.20 自身保存打点
        OpenProjectDir(sDir);
    }
}

void CMainWindow::OnGenerateGuide() {
    if (m_pPm->ProjectDir().isEmpty()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("请先打开工程目录"), this);
        return;
    }
    if (m_bGuideBuilding) {
        return;
    }
    m_bGuideBuilding = true;
    SetStatus(QStringLiteral("正在生成 guide.md ..."));
    m_pPm->BuildGuideAsync();
}

void CMainWindow::OnSyncGuide() {
    // 需求4：手动「同步 Guide」——按 record.md 水位线之后的新增记录增量更新 guide.md（无新改动则提示）。
    if (m_pPm->ProjectDir().isEmpty()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("请先打开工程目录"), this);
        return;
    }
    SetStatus(QStringLiteral("正在按 record.md 新增记录同步 guide.md ..."));
    m_pPm->SyncGuideAsync(QString());   // 手动触发：按 record 水位线增量（无新改动则静默）
}

void CMainWindow::OnGuideSyncFinished(bool a_bOk, const QString& a_sMsg) {
    // 需求4：同步结果仅刷新状态栏（Done 触发 / 手动触发统一，不打扰弹窗）。
    (void)a_bOk;
    SetStatus(a_sMsg);
}

void CMainWindow::OnGuideLog(const QString& a_sLine) {
    // guide 日志显示在状态栏（截断）
    SetStatus(QStringLiteral("[guide] %1").arg(a_sLine.left(120)));
}

void CMainWindow::OnGuideFinished(bool a_bOk, const QString& a_sMsg) {
    m_bGuideBuilding = false;
    SetStatus(a_sMsg);

    // 启动任务时 guide 缺失触发的初始任务：生成完成自动续跑该任务（不再弹完成框）。
    if (!m_pTaskPendingGuide.isNull()) {
        CTaskInfo* pPending = m_pTaskPendingGuide.data();
        m_pTaskPendingGuide = nullptr;
        // 生成期的 Running 是 guide 等待态，先复位（StartTask 对 Running 早退），
        // 最终状态由下方分支决定。
        pPending->SetStatus(ETaskStatus::Paused);
        if (!a_bOk) {
            m_pExecPanel->AppendEvent(pPending->Id(), QStringLiteral("init_guide"),
                // 2026.08.17 详细原因并入事件持久化（chat.json）：a_sMsg 形如
                //   "guide.md 生成失败：<真因>"，此前后半段只进状态栏转瞬即逝，
                //   异机部署失败无从排查（飞机大战 16:37 实测：7 秒即败、真因丢失）。
                QStringLiteral("初始任务：%1，任务已暂停，请重新启动重试")
                    .arg(a_sMsg.left(200)));
            OnTaskChanged(pPending);
            return;
        }
        // 生成期间用户点了停止/暂停：尊重请求不强行续跑（StartTask 会清标志，须先判）。
        if (pPending->StopRequested()) {
            pPending->SetStatus(ETaskStatus::Stopped);
            m_pExecPanel->AppendEvent(pPending->Id(), QStringLiteral("init_guide"),
                QStringLiteral("初始任务：guide.md 就绪，但任务已请求停止"));
            OnTaskChanged(pPending);
            return;
        }
        if (pPending->PauseRequested()) {
            m_pExecPanel->AppendEvent(pPending->Id(), QStringLiteral("init_guide"),
                QStringLiteral("初始任务：guide.md 就绪，任务已请求暂停，点击「继续」启动"));
            OnTaskChanged(pPending);
            return;
        }
        m_pExecPanel->AppendEvent(pPending->Id(), QStringLiteral("init_guide"),
            QStringLiteral("初始任务：guide.md 就绪，启动任务"));
        m_pOrch->StartTask(pPending);   // 内部 RunCurrentStage 会重新置 Running 并刷新 UI
        return;
    }

    // 非任务触发（菜单「生成 / 刷新 Guide」）：弹完成提示。
    ExecMsgBox(QMessageBox::Information, QStringLiteral("Guide"),
        a_bOk ? QStringLiteral("guide.md 生成完成") : a_sMsg, this);
}

void CMainWindow::OnInstallEnv() {
    // 安装脚本随构建输出到 exe 同目录（Execute/install_env.bat）。
    // 脚本内部用 winget 安装 Git / Node.js，再 npm 安装 Claude Code CLI。
    const QString sDir = QCoreApplication::applicationDirPath();
    const QString sScript = sDir + QStringLiteral("/install_env.bat");

    if (!QFile::exists(sScript)) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("环境安装"),
            QStringLiteral("未找到安装脚本：\n%1\n\n请先完成构建（脚本会随编译输出到 exe 目录）。").arg(sScript), this);
        return;
    }

    // 用 ShellExecuteW 的 "runas" 动词以管理员权限启动 cmd /k 跑脚本：
    // 立即弹 UAC 授权框（满足“权限不足需授权”），授权后开一个可见的、已提权的
    // cmd 窗口执行安装；脚本内 net session 因此通过、跳过自身提权，避免双重弹窗。
    // /k 保留窗口便于查看进度与结果，避免窗口闪退看不到错误。
    // 用户在 UAC 点「否」时返回 ERROR_CANCELLED(<=32)，归入失败分支提示手动运行。
    const QString sArgs = QStringLiteral("/k \"") + sScript + QStringLiteral("\"");
    const HINSTANCE hInst = ShellExecuteW(
        nullptr,
        L"runas",
        L"cmd.exe",
        reinterpret_cast<LPCWSTR>(sArgs.utf16()),
        reinterpret_cast<LPCWSTR>(sDir.utf16()),
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("环境安装"),
            QStringLiteral("未启动安装脚本（可能未在 UAC 授权管理员权限）。请右键脚本「以管理员身份运行」：\n%1").arg(sScript), this);
        return;
    }
    SetStatus(QStringLiteral("正在以管理员权限运行环境安装脚本，请在弹出的窗口中查看进度..."));
}

void CMainWindow::OnVersionManage() {
    // 2026.09 后端版本管理（任务 23183ccb）：功能菜单入口（设置页 Agent 组同源）。
    //   D6 判据：目标后端==当前全局后端 && (IsRunnerBusy()||IsAdhocRunning())
    //   → 对话框更新前阻断式确认（口径同设置页后端切换保护，仅收敛到单后端）。
    const int iCurBackend = CAgentRunnerFactory::IsDsh() ? 2
                          : CAgentRunnerFactory::IsPi() ? 3
                          : CAgentRunnerFactory::IsCodex() ? 4
                          : CAgentRunnerFactory::IsHermes() ? 5 : 1;
    CVersionDialog::FBusyCheck fnBusy = [this, iCurBackend](int a_iBackend) {
        return a_iBackend == iCurBackend && m_pOrch != nullptr
            && (m_pOrch->IsRunnerBusy() || m_pOrch->IsAnyAdhocRunning());
    };
    CVersionDialog stDlg(fnBusy, this);
    // 2026.10.21 右键「自动修复」跨窗分派：对话框组好自愈提示词，主窗口建助手提交。
    connect(&stDlg, &CVersionDialog::sigAutoFixRequested,
            this, &CMainWindow::OnBackendAutoFix);
    stDlg.exec();
}

void CMainWindow::OnBackendAutoFix(int a_iFaultBackend, int a_iFixBackend,
                                   const QString& a_sPrompt) {
    Q_UNUSED(a_iFaultBackend);   // 故障后端信息已在 a_sPrompt 内（情况说明段）
    // 后端自愈落地：复用/新建「后端自愈」助手 → 面板档 + 编排器会话双写粘性修复后端
    //   → OnAdhocSubmit 走既有助手链（流式日志/统计/中断全复用）。
    //   前置：工程已开（助手会话需要工作目录）；目标助手不在跑（复用名撞运行中时
    //   LoadAsstAdhoc 覆盖会话链会伤及在跑轮，直接拒绝并提示）。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr
            || a_iFixBackend < 1 || a_iFixBackend > 5) {
        return;
    }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("后端自愈"),
            QStringLiteral("请先打开一个工程再使用自动修复（自愈 Agent 需要工作目录）。"));
        return;
    }
    // 后端号 → 粘性名（与 CAgentRunnerFactory::NameOf 同词汇表，字符串比较防漂移）。
    const QString asName[6] = { QString(), QStringLiteral("claude"), QStringLiteral("dsh"),
                                QStringLiteral("pi"), QStringLiteral("codex"),
                                QStringLiteral("hermes") };
    const QString sFixBackend = asName[a_iFixBackend];
    const QString sId = m_pExecPanel->CreateHealAsst();
    if (sId.isEmpty()) {
        return;
    }
    if (m_pOrch->IsAsstAdhocRunning(sId)) {
        QMessageBox::warning(this, QStringLiteral("后端自愈"),
            QStringLiteral("「后端自愈」助手正在执行上一轮修复，请等它结束后再试。"));
        return;
    }
    // 绑定修复后端：面板档（持久化正本）+ 编排器会话（运行权威；LoadAsstAdhoc 覆盖
    //   会话链=本轮全新开始，旧修复历史不回灌，防上轮故障噪音带偏本轮）。
    m_pExecPanel->UpdateAsstBackend(sId, sFixBackend);
    m_pOrch->LoadAsstAdhoc(sId, QString(), QJsonArray(), QString(), sFixBackend);
    OnAdhocSubmit(sId, a_sPrompt, 0 /*PermAuto 自动审批*/);
}

void CMainWindow::OnSettings() {
    // 2026.08.15 设置对话框：构造时回填 config.ini 当前值；确定后统一落盘 + 应用运行期。
    //   策略/压缩比例/备份排除：消费方每次现读 ini，落盘即生效；
    //   联网开关/评审并行/claude 命令/提问超时/扫描排除：需同步到内存（编排器/CClaudeRunner/扫描器）。
    // 2026.09 D6 判据注入（任务 23183ccb）：设置页 Agent 组「后端版本管理…」按钮
    //   与功能菜单同源（同判据 lambda，口径同下方 OnVersionManage）。
    // 2026.09.09 v2.1.143 任务级配置覆盖：选中任务时开设置，对话框读到的已是任务档
    //   优先的合并值（CProjectManager 覆盖机制路由），确定后 SaveXxx 也落任务档。
    CSettingsDialog stDlg(m_pPm, this, [this](int a_iBackend) {
        const int iCurBackend = CAgentRunnerFactory::IsDsh() ? 2
                              : CAgentRunnerFactory::IsPi() ? 3
                              : CAgentRunnerFactory::IsCodex() ? 4
                              : CAgentRunnerFactory::IsHermes() ? 5 : 1;
        return a_iBackend == iCurBackend && m_pOrch != nullptr
            && (m_pOrch->IsRunnerBusy() || m_pOrch->IsAnyAdhocRunning());
    }, (m_pExecPanel != nullptr && m_pExecPanel->CurrentTask() != nullptr)
        ? m_pExecPanel->CurrentTask()->Name() : QString());
    if (stDlg.exec() != QDialog::Accepted || m_pPm == nullptr) {
        return;
    }

    // —— 落盘（全部键一次写齐，个别写失败不阻断其余）——
    m_pPm->SaveMaxIter(stDlg.MaxIter());
    m_pPm->SaveMaxResume(stDlg.MaxResume());
    m_pPm->SaveReviewResume(stDlg.ReviewResume());
    m_pPm->SaveReviewParallel(stDlg.ReviewParallel());
    m_pPm->SaveClaudeCmd(stDlg.ClaudeCmd());
    // 2026-10-08 claude 思考预算：落盘 + 立即应用 runner 静态（下一轮即生效）。
    m_pPm->SaveClaudeMaxThinkTok(stDlg.ClaudeThinkTok());
    CClaudeRunner::SetMaxThinkingTokens(stDlg.ClaudeThinkTok());
    m_pPm->SaveAskTimeoutSec(stDlg.AskTimeoutSec());
    // 2026.08.18 后端切换保护（2026.09.21 收窄，用户需求）：任务 runner 或 guide
    //   运行中仍拒切后端——Backend 键保持原值（其余键照常落）；任务多阶段续跑
    //   复用同一 sid 链，中途换后端会跨后端语义错乱。助手已豁免：v2.4.193 起助手
    //   粘性后端（会话绑定 sBackend，全局切换不影响任何已有助手——运行中也不
    //   受影响，仅新建助手用新后端），EnsureAdhocRunner 的占用不重建守卫兜底
    //   运行中实例绝不被换，助手运行中允许切换无逻辑问题。
    const int iNewBackend = stDlg.AgentBackend();
    // 2026.08.19 三后端化：切换判定改为「目标后端 ≠ 工厂当前后端」（旧二元比较
    //   (==2)!=IsDsh() 无法覆盖 pi↔dsh 间的切换）。
    // 2026.09.19 补 Hermes（5）：旧映射 ...IsCodex()?4:1 把 Hermes 当 Claude，
    //   claude↔hermes 互切不判变更（忙时不拦）且无法触发切换后清会话链。
    const int iCurBackend = CAgentRunnerFactory::IsDsh() ? 2
                           : CAgentRunnerFactory::IsPi() ? 3
                           : CAgentRunnerFactory::IsCodex() ? 4
                           : CAgentRunnerFactory::IsHermes() ? 5 : 1;
    const bool bBackendChanged = (iNewBackend != iCurBackend);
    const bool bBusy = (m_pOrch != nullptr && m_pOrch->IsRunnerBusy())
        || (m_pPm != nullptr && m_pPm->IsGuideBusy());
    bool bBackendSaved = true;
    if (bBackendChanged && bBusy) {
        bBackendSaved = false;
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("设置"),
                   QStringLiteral("有任务或 guide 正在运行，Agent 后端切换已保留原值"
                                  "（其余设置已保存）。请空闲后再切换。\n"
                                  "注：助手运行中不影响切换——已有助手继续用各自原后端，"
                                  "仅新建助手使用新后端。"), this);
    } else {
        m_pPm->SaveAgentBackend(iNewBackend);
        // 2026.09.03 v2.0.105 选 Hermes 后端保存时自动敷设 oneshot 事件补丁：
        //   软编码（install_env.bat /hermes-patch + venv python 幂等敷设，补丁
        //   正本=exe 同级 hermes_oneshot_patch.py）；失败不阻断也不弹窗——
        //   StartStage 兕底会再试并在失败时提示。
        if (iNewBackend == 5 && QFile::exists(QCoreApplication::applicationDirPath()
                                              + QStringLiteral("/install_env.bat"))) {
            const QString sHermesRoot = stDlg.HermesRoot();
            if (!sHermesRoot.isEmpty()
                && QFile::exists(sHermesRoot + QStringLiteral("/hermes-agent"))) {
                QProcess::execute(QStringLiteral("cmd"),
                                  { QStringLiteral("/c"),
                                    QCoreApplication::applicationDirPath()
                                        + QStringLiteral("/install_env.bat"),
                                    QStringLiteral("/hermes-patch"), sHermesRoot });
            }
        }
    }
    // 2026.08.18 轻量自动配置：根目录单字段落盘；root 生效时清掉旧 bin/cordis 显式键
    //   （解析优先级=显式键>root 推导，清键防双源歧义；root 留空则旧键原样保留作回退）。
    m_pPm->SaveDshRoot(stDlg.DshRoot());
    // 2026.09.27 额外可写目录落盘 + 授/撤 ACE（当前工程根派生 SID；新加的授、
    //   移除的撤；运行中进程下一轮 EnsureProcess 自愈补授）。
    {
        const QStringList aOld = CDeepSeekRunner::DshExtraWriteDirs();
        const QStringList aNew = stDlg.DshExtraWriteDirs();
        m_pPm->SaveDshExtraWriteDirs(aNew);
        CDeepSeekRunner::SetDshExtraWriteDirs(aNew);
        const QString sCurProj = (m_pPm != nullptr) ? m_pPm->ProjectDir() : QString();
        // 2026-10-19 授权失败清单出参 + 提权修复（全后端通用）：需提权目录 =
        //   ①DSH 工作区写 SID 精确授权（ApplyExtraWriteDirs）失败者——多为过滤
        //   令牌下拿不到 WRITE_DAC；②本轮新加目录账户级 ACE 静默自愈失败者
        //   （不限 DSH：账户拿回 WRITE_DAC/WRITE_OWNER 后所有后端 Harness 的
        //   沙箱授权都恢复）。runas icacls 只补账户级 F，精确条目（S-1-4 能力
        //   SID ACE + Low 标签，icacls 表达不了）由重跑既有幂等实现收尾。多目录
        //   以 " & " 串联进单次 UAC（icacls 参数各带引号，cmd 对引号内 & 不切分）；
        //   串联退出码只反映末条，判定以重跑复查为准。
        QStringList aFailed;
        CDeepSeekRunner::ApplyExtraWriteDirs(sCurProj, aNew, aOld, &aFailed);
        QStringList aNeed = aFailed;
        for (const QString& sDir : aNew) {
            if (!aOld.contains(sDir) && !aNeed.contains(sDir)
                    && CDeepSeekRunner::EnsureDirWriteAce(sDir) < 0) {
                aNeed << sDir;   // 新加目录账户级预敷失败（目录不在场返回 0，自动跳过）
            }
        }
        if (!aNeed.isEmpty()) {
            QStringList aCmds;
            for (const QString& sDir : aNeed) {
                aCmds << CDeepSeekRunner::BuildIcaclsGrantCmd(sDir);
            }
            aCmds.removeAll(QString());
            if (!aCmds.isEmpty()
                    && RunAclElevateFlow(QStringLiteral("目录授权"), aNeed,
                                         aCmds.join(QStringLiteral(" & ")))) {
                QStringList aStill;
                CDeepSeekRunner::ApplyExtraWriteDirs(sCurProj, aNew, QStringList(), &aStill);
                for (const QString& sDir : aNeed) {
                    if (CDeepSeekRunner::EnsureDirWriteAce(sDir) != 0
                            && !aStill.contains(sDir)) {
                        aStill << sDir;
                    }
                }
                for (const QString& sDir : aStill) {
                    m_aAclElevateSuppressed.insert(
                        QDir::toNativeSeparators(sDir).toLower());
                }
                ExecMsgBox(aStill.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
                           QStringLiteral("目录授权"),
                           aStill.isEmpty()
                               ? QStringLiteral("提权授权成功，%1 个目录已全部放行，"
                                                "所有后端（Harness）均可使用。")
                                     .arg(QString::number(aNeed.size()))
                               : QStringLiteral("以下目录提权修复后仍未通过（授权部分完成）：\n%1\n"
                                                "可手工执行：\n%2")
                                     .arg(aStill.join(QLatin1Char('\n')),
                                          aCmds.join(QStringLiteral(" & "))),
                           this);
            }
        }
    }
    m_pPm->SaveDshNodePath(stDlg.DshNodePath());
    if (!stDlg.DshRoot().isEmpty()) {
        m_pPm->SaveDshBinJsPath(QString());
        m_pPm->SaveDshCordisPath(QString());
    }
    m_pPm->SaveDshProvider(stDlg.DshProvider());
    // 2026-10-08 思考强度：落盘 + 应用（initialize reasoningEffort，下次冷启动即用）。
    m_pPm->SaveDshEffort(stDlg.DshEffort());
    CDeepSeekRunner::SetDshEffort(stDlg.DshEffort());
    m_pPm->SaveDshPriceInPerM(stDlg.DshPriceInPerM());
    m_pPm->SaveDshPriceCacheReadPerM(stDlg.DshPriceCacheReadPerM());
    m_pPm->SaveDshPriceCacheWritePerM(stDlg.DshPriceCacheWritePerM());
    m_pPm->SaveDshPriceOutPerM(stDlg.DshPriceOutPerM());
    // 2026.08.20 PIAgent 计费单价落盘（[Agent] PiPrice*PerM）。
    m_pPm->SavePiPriceInPerM(stDlg.PiPriceInPerM());
    m_pPm->SavePiPriceCacheReadPerM(stDlg.PiPriceCacheReadPerM());
    m_pPm->SavePiPriceCacheWritePerM(stDlg.PiPriceCacheWritePerM());
    m_pPm->SavePiPriceOutPerM(stDlg.PiPriceOutPerM());
    // 2026.08.18 自定义大模型表落盘（[DshModels] Count/Active/M<i>*）。
    m_pPm->SaveDshModels(stDlg.DshModels(), stDlg.DshActiveModel());
    m_pPm->SaveWebSearch(stDlg.WebSearch());
    m_pPm->SaveExpertAssistant(stDlg.ExpertAssistant());
    // 2026-10-08 claude 跟随共享表开关（大模型页）：落盘即重应用（含 runner 静态槽）。
    m_pPm->SaveClaudeFollowShared(stDlg.ClaudeFollowShared());
    m_pPm->SaveAutoCompactPct(stDlg.AutoCompactPct());
    // 2026.09.27 「上下文窗口上限」设置项已删：[Adhoc] ContextLimit 不再由设置页读写。
    //   2026-10-15 右键「设置上下文大小」也已删：上下文窗口唯一来源 = 按模型自动探测。
    m_pPm->SaveAdhocMaxTurns(stDlg.AdhocMaxTurns());   // 2026.09.23 助手轮次上限（下一条提示词即时生效）
    m_pPm->SaveAdhocMaxTurnsAuto(stDlg.AdhocMaxTurnsAuto());   // 2026.09.23 v3.0.219 工作量评估开关
    // 2026-10-08 提示词长度闸门：落盘后由下方 ApplySharedModelsToRunners 尾部统一推面板。
    m_pPm->SaveAdhocPromptCtxPctMax(stDlg.AdhocPromptCtxPctMax());
    m_pPm->SaveAdhocPromptTokMax(stDlg.AdhocPromptTokMax());
    m_pPm->SaveScanExcludeDirs(stDlg.ScanExcludeDirs());
    m_pPm->SaveBackupExcludeDirs(stDlg.BackupExcludeDirs());
    // 2026.09.23 资源管理器显示隐藏目录：落盘 + 立即应用到树（含已加载行重隐/重显）。
    m_pPm->SaveExplorerHiddenDirs(stDlg.ExplorerHiddenDirs());
    if (m_pTaskPanel != nullptr) {
        m_pTaskPanel->SetExplorerHiddenDirs(ScanDirList(stDlg.ExplorerHiddenDirs()));
    }
    // 2026-10-17 对话日志渲染引擎（[View] Engine）：落盘即可——建视图时读取（面板
    //   构造快照/助手页创建），已建页不重建（重开助手 Tab / 重启全部生效，灰度期
    //   双引擎并存零迁移）。状态栏提示口径与 HarnessAgent v1.9.44 一致。
    {
        const QString sOldEngine = m_pPm->LoadViewEngine();
        const QString sNewEngine = stDlg.ViewEngine();
        m_pPm->SaveViewEngine(sNewEngine);
        if (sOldEngine != sNewEngine) {
            const QString sName = (sNewEngine == QStringLiteral("ModelView"))
                ? QStringLiteral("新版（自绘）") : QStringLiteral("旧版（卡片流）");
            statusBar()->showMessage(
                QStringLiteral("日志渲染引擎已切换：%1（对新建立的会话页生效，重开助手 Tab / 重启后全部生效）")
                    .arg(sName), 8000);
        }
    }
    // 2026-09-26 内置测试沙箱：落盘 + 热应用到管理器（开关/帧率即时生效，
    //   录像开关对后续新收纳窗口生效；存量窗口录像保持原状）。
    m_pPm->SaveSandboxEnable(stDlg.SandboxEnable());
    m_pPm->SaveSandboxAutoGhost(stDlg.SandboxAutoGhost());
    m_pPm->SaveSandboxRecord(stDlg.SandboxRecord());
    m_pPm->SaveSandboxFps(stDlg.SandboxFps());
    m_pPm->SaveSandboxRetainDays(stDlg.SandboxRetainDays());
    if (m_pSbx != nullptr) {
        CSandboxManager::SSbxCfg stSbx;
        stSbx.bEnable     = stDlg.SandboxEnable();
        stSbx.bAutoGhost  = stDlg.SandboxAutoGhost();
        stSbx.bRecord     = stDlg.SandboxRecord();
        stSbx.iFps        = stDlg.SandboxFps();
        stSbx.iRetainDays = stDlg.SandboxRetainDays();
        m_pSbx->SetConfig(stSbx);
        m_pSbx->ReloadConfig();
    }
    m_iCfgSelfSaveMs = QDateTime::currentMSecsSinceEpoch();   // 2026.08.20 自身保存打点（跳过自触发重载）

    // —— 应用到运行期内存 ——
    if (m_pOrch != nullptr) {
        m_pOrch->SetReviewParallel(stDlg.ReviewParallel());
        m_pOrch->SetWebSearch(stDlg.WebSearch());
    }
    CClaudeRunner::SetClaudeCmd(stDlg.ClaudeCmd().isEmpty()
        ? QStringLiteral("claude") : stDlg.ClaudeCmd());
    CClaudeRunner::SetAskTimeoutSec(stDlg.AskTimeoutSec());
    // 2026.08.18 后端/DSH 参数应用运行期：工厂分派立即切换（漂移重建由各持有点的
    //   Ensure 逻辑负责——非运行中的旧 runner 在下一次使用前被替换为新后端实例）。
    if (bBackendSaved) {
        CAgentRunnerFactory::SetBackend(iNewBackend == 2 ? CAgentRunnerFactory::BackendDsh
                                        : iNewBackend == 3 ? CAgentRunnerFactory::BackendPi
                                        : iNewBackend == 4 ? CAgentRunnerFactory::BackendCodex
                                        : iNewBackend == 5 ? CAgentRunnerFactory::BackendHermes
                                                           : CAgentRunnerFactory::BackendClaude);
        // 2026.09.19 后端切换清会话链：档载入侧 backend 键守卫的运行期对应物——旧后端的
        //   sid/撤销栈交给新后端必错（PixelPorter 实害：pi 切 claude 后首轮报
        //   「No conversation found with session ID」）；对话历史/统计不受影响。
        if (bBackendChanged) {
            ClearAssistantSessionsOnBackendSwitch();
        }
    }
    // 2026.10.11 Claude 后端 + 非 anthropic 协议当前条目拦截提示（目标机实害：用户把
    //   当前模型切到「glm5.2」（10.1.0.201/api/v1 openai 协议）——claude 只说 anthropic
    //   协议，注入被跳过 → Not logged in。同名易混条目 glm5.2 vs glm5.2-公司）一保存
    //   就拦住，不再等到发提示词才报 Not logged in。
    if (bBackendSaved && iNewBackend == 1) {
        const QList<SDshModelEntry> aUnify = stDlg.DshModels();   // 四页统一后同一份
        const int iAct = stDlg.DshActiveModel();
        if (iAct >= 0 && iAct < aUnify.size()) {
            const QString sApi = aUnify.at(iAct).sApi.trimmed().toLower();
            if (!sApi.isEmpty() && sApi != QLatin1String("anthropic-messages")) {
                const QString sName = aUnify.at(iAct).sDisp.isEmpty()
                    ? aUnify.at(iAct).sModelId : aUnify.at(iAct).sDisp;
                ExecMsgBox(QMessageBox::Warning, QStringLiteral("当前模型 Claude 无法使用"),
                    QStringLiteral("后端已选 Claude Code，但当前模型条目「%1」的 API 类型"
                                   "为 %2——claude 只支持 anthropic-messages 协议端点，"
                                   "该条目会被跳过（助手将报 Not logged in）。\n\n"
                                   "请改选 anthropic-messages 类型的条目（如名称含"
                                   "「公司/claude」的接入点）。")
                        .arg(sName, sApi), this);
            }
        }
    }
    // 2026.08.18 根目录应用运行期：显式值优先；空或失效（死路径自愈）时探测 exe
    //   同级并自动回填落盘（设置页装完 DSH → 确定 → 探测命中即写 config.ini，
    //   与启动路径同口径）。
    QString sDshRootApply = stDlg.DshRoot();
    if (!CDeepSeekRunner::LooksLikeDshRoot(sDshRootApply)) {
        const QString sProbed = CDeepSeekRunner::ProbeSiblingRoot();
        if (CDeepSeekRunner::LooksLikeDshRoot(sProbed)) {
            sDshRootApply = sProbed;
            m_pPm->SaveDshRoot(sDshRootApply);
        }
    }
    CDeepSeekRunner::SetDshRootPath(sDshRootApply);
    CDeepSeekRunner::SetDshNodePath(stDlg.DshNodePath());
    // bin/cordis 静态跟随刚落盘的键值（root 已存 → 上方刚清空；root 留空 → 旧档回退值）。
    CDeepSeekRunner::SetDshBinJsPath(m_pPm->LoadDshBinJsPath());
    CDeepSeekRunner::SetDshCordisPath(m_pPm->LoadDshCordisPath());
    CDeepSeekRunner::SetDshProvider(stDlg.DshProvider());
    CDeepSeekRunner::SetDshPriceInPerM(stDlg.DshPriceInPerM());
    CDeepSeekRunner::SetDshPriceCacheReadPerM(stDlg.DshPriceCacheReadPerM());
    CDeepSeekRunner::SetDshPriceCacheWritePerM(stDlg.DshPriceCacheWritePerM());
    CDeepSeekRunner::SetDshPriceOutPerM(stDlg.DshPriceOutPerM());
    CPiAgentRunner::SetPiPriceInPerM(stDlg.PiPriceInPerM());
    CPiAgentRunner::SetPiPriceCacheReadPerM(stDlg.PiPriceCacheReadPerM());
    CPiAgentRunner::SetPiPriceCacheWritePerM(stDlg.PiPriceCacheWritePerM());
    CPiAgentRunner::SetPiPriceOutPerM(stDlg.PiPriceOutPerM());
    // 2026.08.18 自定义大模型表应用到 runner 静态表（下次 DSH 进程冷启动生效）。
    ApplyDshCustomModels(stDlg.DshModels(), stDlg.DshActiveModel());
    // 2026.08.19 PIAgent 参数落盘 + 应用运行期（口径同 DSH：root 探测自愈回填后 Set 静态）。
    m_pPm->SavePiNodePath(stDlg.PiNodePath());
    m_pPm->SavePiProvider(stDlg.PiProvider());
    m_pPm->SavePiThinking(stDlg.PiThinking());
    QString sPiRootApply = stDlg.PiRoot();
    if (!CPiAgentRunner::LooksLikePiRoot(sPiRootApply)) {
        const QString sPiProbed = CPiAgentRunner::ProbeSiblingRoot();
        if (!sPiProbed.isEmpty()) {
            sPiRootApply = sPiProbed;
            m_pPm->SavePiRoot(sPiProbed);
        }
    }
    m_pPm->SavePiRoot(sPiRootApply);
    CPiAgentRunner::SetPiRootPath(sPiRootApply);
    CPiAgentRunner::SetPiNodePath(stDlg.PiNodePath());
    CPiAgentRunner::SetPiProvider(stDlg.PiProvider());
    CPiAgentRunner::SetPiThinking(stDlg.PiThinking());
    // 2026.08.20 PiAgent 自定义大模型表：[PiModels] 落盘 + runner 应用（Set 即合成写
    //   ~/.pi/agent/models.json 的 autocode-<n> 槽位；pi 每次打开 /model 重读，
    //   下一次 StartStage 冷启动即用新配置）。
    m_pPm->SavePiModels(stDlg.PiModels(), stDlg.PiActiveModel());
    CPiAgentRunner::SetPiModels(stDlg.PiModels(), stDlg.PiActiveModel());
    // 2026.08.18 保存后仍不可用 → 立即警告兜底：设置页检测拦住了根目录缺失，但
    //   cordis 未部署等半安装态在这里补提醒（与启动警告同文案口径）。
    if (CAgentRunnerFactory::IsDsh()) {
        const QString sDshErr = CDeepSeekRunner::DshConfigError();
        if (!sDshErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（DeepSeekHarness）不可用"),
                sDshErr + QStringLiteral("\n\n后端已切到 DeepSeekHarness，问题解决前任务/助手/guide "
                                          "无法启动；也可重新打开设置切回 Claude Code。"), this);
        }
    }
    // 2026.08.19 同口径兜底：pi 后端保存后仍不可用立即警告（如 API key/provider 问题
    //   启动校验才暴露的项；文案与启动警告一致）。
    if (CAgentRunnerFactory::IsPi()) {
        const QString sPiErr = CPiAgentRunner::PiConfigError();
        if (!sPiErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（PIAgent）不可用"),
                sPiErr + QStringLiteral("\n\n后端已切到 PIAgent，问题解决前任务/助手/guide "
                                        "无法启动；也可重新打开设置切回 Claude Code。"), this);
        }
    }
    // CodexHarness 第四后端：参数落盘 + 应用运行期（口径同 Pi：root 探测自愈回填后
    //   Set 静态）；保存时同步写 ~/.codex（config.toml + auth.json）。
    m_pPm->SaveCodexNodePath(stDlg.CodexNodePath());
    m_pPm->SaveCodexProvider(stDlg.CodexProvider());
    m_pPm->SaveCodexWireApi(stDlg.CodexWireApi());
    // 2026-10-08 思考强度：落盘 + 应用（-c model_reasoning_effort，下一轮即用）。
    m_pPm->SaveCodexEffort(stDlg.CodexEffort());
    CCodexRunner::SetCodexEffort(stDlg.CodexEffort());
    QString sCodexRootApply = stDlg.CodexRoot();
    if (!CCodexRunner::LooksLikeCodexRoot(sCodexRootApply)) {
        const QString sCodexProbed = CCodexRunner::ProbeSiblingRoot();
        if (!sCodexProbed.isEmpty()) {
            sCodexRootApply = sCodexProbed;
            m_pPm->SaveCodexRoot(sCodexProbed);
        }
    }
    m_pPm->SaveCodexRoot(sCodexRootApply);
    CCodexRunner::SetCodexRoot(sCodexRootApply);
    CCodexRunner::SetCodexNodePath(stDlg.CodexNodePath());
    CCodexRunner::SetCodexProvider(stDlg.CodexProvider());
    CCodexRunner::SetCodexWireApi(stDlg.CodexWireApi());
    // 2026.08.21 Codex 自定义大模型表：落盘 [CodexModels] + 应用运行期（口径同 Pi：
    //   Set 即合成写 ~/.codex，激活条目覆盖固定路由）。
    m_pPm->SaveCodexModels(stDlg.CodexModels(), stDlg.CodexActiveModel());
    CCodexRunner::SetCodexModels(stDlg.CodexModels(), stDlg.CodexActiveModel());
    // ~/.codex 写入（表单齐备才写；错误只提示不阻断其余设置）。
    {
        QString sCodexWriteErr;
        if (!CCodexRunner::WriteCodexUserConfig(&sCodexWriteErr)
            && CAgentRunnerFactory::IsCodex()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("CodexHarness 配置写入"),
                sCodexWriteErr + QStringLiteral("\n\n~/.codex 写入失败，CodexHarness 后端"
                                                "可能无法启动；请检查磁盘权限后重试。"),
                this);
        }
    }
    // 保存后仍不可用 → 立即警告兜底（口径同 DSH/Pi）。
    if (CAgentRunnerFactory::IsCodex()) {
        const QString sCodexErr = CCodexRunner::CodexConfigError();
        if (!sCodexErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（CodexHarness）不可用"),
                sCodexErr + QStringLiteral("\n\n后端已切到 CodexHarness，问题解决前任务/助手/guide "
                                          "无法启动；也可重新打开设置切回 Claude Code。"), this);
        }
    }
    // 第五后端 Hermes（任务 3213cdbf）：表单落盘 [Agent] Hermes* / [HermesModels] +
    //   应用运行期静态参数（口径同 Pi/Codex；~/.hermes 由 StartStage 幂等写，此处
    //   只做探测告警）。
    m_pPm->SaveHermesNodePath(stDlg.HermesNodePath());
    m_pPm->SaveHermesRoot(stDlg.HermesRoot());
    // 2026-10-08 思考强度：落盘 + 应用（config.yaml agent.reasoning_effort，同一次写管）。
    m_pPm->SaveHermesEffort(stDlg.HermesEffort());
    CHermesRunner::SetHermesEffort(stDlg.HermesEffort());
    m_pPm->SaveHermesPriceInPerM(stDlg.HermesPriceInPerM());
    m_pPm->SaveHermesPriceCacheReadPerM(stDlg.HermesPriceCacheReadPerM());
    m_pPm->SaveHermesPriceCacheWritePerM(stDlg.HermesPriceCacheWritePerM());
    m_pPm->SaveHermesPriceOutPerM(stDlg.HermesPriceOutPerM());
    m_pPm->SaveHermesModels(stDlg.HermesModels(), stDlg.HermesActiveModel());
    CHermesRunner::SetHermesRoot(stDlg.HermesRoot());
    CHermesRunner::SetHermesNodePath(stDlg.HermesNodePath());
    CHermesRunner::SetHermesPriceInPerM(stDlg.HermesPriceInPerM());
    CHermesRunner::SetHermesPriceCacheReadPerM(stDlg.HermesPriceCacheReadPerM());
    CHermesRunner::SetHermesPriceCacheWritePerM(stDlg.HermesPriceCacheWritePerM());
    CHermesRunner::SetHermesPriceOutPerM(stDlg.HermesPriceOutPerM());
    CHermesRunner::SetHermesRuntimeLimits(
        CHermesRunner::ClampCtx(200000), CHermesRunner::ClampMaxTok(32768));   // 固定路由默认
    {
        const QList<SDshModelEntry> aHermesApply = stDlg.HermesModels();
        const int iHermesActiveApply = stDlg.HermesActiveModel();
        if (iHermesActiveApply >= 0 && iHermesActiveApply < aHermesApply.size()) {
            const SDshModelEntry& stE = aHermesApply.at(iHermesActiveApply);
            CHermesRunner::SetHermesModel(stE.sModelId);
            CHermesRunner::SetHermesApiKey(stE.sApiKey);
            CHermesRunner::SetHermesBaseUrl(stE.sBaseURL);
            CHermesRunner::SetHermesApi(stE.sApi);   // 2026.09.21 协议感知：anthropic 端点 → config.yaml api_mode
            CHermesRunner::SetHermesRuntimeLimits(
                CHermesRunner::ClampCtx(stE.iCtx), CHermesRunner::ClampMaxTok(stE.iMaxTok));
        }
    }
    // 保存后仍不可用 → 立即警告兜底（口径同 DSH/Pi/Codex）。
    if (CAgentRunnerFactory::IsHermes()) {
        const QString sHermesErr = CHermesRunner::HermesConfigError();
        if (!sHermesErr.isEmpty()) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("Agent 后端（Hermes）不可用"),
                sHermesErr + QStringLiteral("\n\n后端已切到 Hermes，问题解决前任务/助手/guide "
                                          "无法启动；也可重新打开设置切回 Claude Code。"), this);
        }
    }
    // 2026.08.18 修：逐项 trim + 空清单恢复内置默认（与启动路径同源，见 ScanDirList）。
    CGuideFingerprintScanner::SetExcludeDirs(ScanDirList(stDlg.ScanExcludeDirs()));

    // 2026.10.11 任务态保存可见性（用户实害：选中任务时改「当前模型」→只写任务档且
    //   全局 Active 保持原值；取消选择/重启后助手回退全局旧模型，用户以为没生效）：
    //   覆盖态保存时①状态栏明示保存位置；②刚存的激活项与全局现值不一致 → 弹窗告知
    //   本次仅任务生效 + 全局当前模型现值 + 全局生效操作路径。
    QString sStatusTail;
    if (m_pPm != nullptr && m_pPm->TaskConfigOverlayActive()) {
        sStatusTail = QStringLiteral("（已保存到当前任务的任务配置，仅该任务选中时生效）");
        struct SSavedActive {
            const wchar_t* pSect;
            int iSaved;
        };
        const SSavedActive aSaved[4] = {
            { L"PiModels",     stDlg.PiActiveModel() },
            { L"DshModels",    stDlg.DshActiveModel() },
            { L"HermesModels", stDlg.HermesActiveModel() },
            { L"CodexModels",  stDlg.CodexActiveModel() },
        };
        bool bActiveDiffers = false;
        for (const SSavedActive& st : aSaved) {
            if (st.iSaved >= 0 && st.iSaved != m_pPm->LoadModelsActiveGlobal(st.pSect)) {
                bActiveDiffers = true;
                break;
            }
        }
        if (bActiveDiffers) {
            ExecMsgBox(QMessageBox::Information, QStringLiteral("当前模型仅保存到任务配置"),
                QStringLiteral("当前选中任务时保存，本次「当前模型」只写入了"
                               "任务配置（任务选中期间生效）。\n\n"
                               "全局默认的当前模型保持不变：取消选择任务后"
                               "助手/重启后将使用全局当前模型。若要全局生效："
                               "取消选择任务 → 设置 → 大模型 → 设为当前 → 保存。"), this);
        }
    }
    // 2026.10.15 设置页改模型（名称/接入点/当前项/增删）后同步面板：尾部统一走
    //   ApplySharedModelsToRunners 重应用四 runner + SetSharedModels 回推面板——右键
    //   「选择模型」菜单条目、上下文进度条模型名/窗口即时新表。此前仅靠自身保存触发
    //   的 config 热重载链，而该链被 m_iCfgSelfSaveMs 2s 防抖跳过 → 面板停留旧表，
    //   改名后右键菜单仍列旧名（本次缺口）。
    ApplySharedModelsToRunners();
    SetStatus(QStringLiteral("设置已保存到 config.ini（策略/压缩比例/目录排除对下一阶段生效）%1")
                  .arg(sStatusTail));
}

void CMainWindow::OnOrchDesign() {
    // 2026-10-14 任务编排设计：模态窗口编辑任务类型/流程档位/执行阶段。
    //   对话框直接编辑 COrchConfig 单例，结构性变更即时落盘（orchestration.json）
    //   并注册动态阶段表（FindStage 立即可见）——关闭即全部生效，新建任务即用最新编排。
    COrchDesignDialog dlg(this);
    dlg.exec();
}

void CMainWindow::OnHelp() {
    // 2026.08.19 弃帮助内嵌窗口：点菜单直接用系统默认浏览器打开 exe 同目录
    //   help\使用说明.html（WebView2/QTextBrowser 双方案随 CHelpDialog 一并移除）。
    const QString sDir = QCoreApplication::applicationDirPath();
    const QString sHelp = sDir + QStringLiteral("/help/使用说明.html");

    if (!QFile::exists(sHelp)) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("帮助"),
                   QStringLiteral("未找到使用说明文件：\n%1").arg(sHelp), this);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(sHelp));
}

void CMainWindow::OnAbout() {
    // FR-9 关于：正式版式对话框（图标/应用名/版本/简介/作者/版权）。
    CAboutDialog dlg(this);
    dlg.exec();
}

void CMainWindow::RebuildRecentMenu() {
    if (m_pMenuRecent == nullptr) {
        return;
    }
    // 2026.09.26 v3.1.239 面板化：条目内容不再走 QAction（面板在菜单 aboutToShow
    //   时自取最新 MRU），此处只管空历史置灰（面板不展示空态）。
    m_pMenuRecent->menuAction()->setEnabled(
        m_pPm != nullptr && !m_pPm->RecentProjects().isEmpty());
}

void CMainWindow::RecordRecent(const QString& a_sDir) {
    if (a_sDir.isEmpty()) {
        return;
    }
    m_pPm->AddRecent(a_sDir);   // 内含去重置顶 + 落盘
    RebuildRecentMenu();
}

void CMainWindow::OnRecentChipOpen(const QString& a_sPath) {
    // 2026.09.26 面板按钮点击：先收菜单链（选定动作，整条菜单关闭），
    //   再复用统一打开入口（在席锁确认/计数累加/MRU 置顶全在其中）。
    CloseRecentMenus();
    if (!a_sPath.isEmpty()) {
        OpenProjectDir(a_sPath);
    }
}

void CMainWindow::OnRecentChipMore() {
    // 面板「更多...」：先收菜单链再开全量对话框（对话框是模态 exec，
    //   菜单不收会叠在对话框下面且吞交互）。
    CloseRecentMenus();
    OnOpenAllRecent();
}

void CMainWindow::CloseRecentMenus() {
    if (m_pMenuRecent != nullptr) {
        m_pMenuRecent->hide();
        // 父级工程菜单（addMenu 父子关系）一并收起，菜单栏回到空闲态。
        if (auto* pMenu = qobject_cast<QMenu*>(m_pMenuRecent->parentWidget())) {
            pMenu->hide();
        }
    }
}

void CMainWindow::OnOpenAllRecent() {
    CRecentProjectsDialog dlg(m_pPm->RecentProjects(), m_pPm->RecentOpenCounts(),
                              m_pPm->ConfigFilePath(), this);
    const int iRc = dlg.exec();
    // 2026.08.20 右键删除：无论 OK/取消，被删条目都从 MRU 移除并落盘、重建菜单。
    const QStringList aRemoved = dlg.RemovedPaths();
    for (const QString& sPath : aRemoved) {
        m_pPm->RemoveRecent(sPath);
    }
    if (!aRemoved.isEmpty()) {
        RebuildRecentMenu();
    }
    if (iRc == QDialog::Accepted && !dlg.SelectedPath().isEmpty()) {
        OpenProjectDir(dlg.SelectedPath());
    }
}

void CMainWindow::OnViewTask(CTaskInfo* a_pTask) {
    // FR-5：弹出只读对话框展示任务完整内容（View 态：字段只读、无 OK/启动勾选）。
    if (a_pTask == nullptr) {
        return;
    }
    CNewTaskDialog dlg(CNewTaskDialog::View, this);
    dlg.SetTask(a_pTask);
    dlg.exec();
}

void CMainWindow::OnRenameTask(CTaskInfo* a_pTask) {
    // 2026.08.20 右键「重命名」：输入框改任务名。改名落在活跃副本（sigChanged → SaveTask
    //   即时写 task.json + 历史库）；历史表选中的是 DB 副本时按 id 找活跃副本，找不到则
    //   直接改副本并手写 DB/task.json。清 autoNamed，避免后续阶段再被自动命名覆盖。
    if (a_pTask == nullptr) {
        return;
    }
    bool bOk = false;
    // 2026.08.20 对话框加宽：默认 QInputDialog 太窄，长任务名看不全——宽度翻倍（≥720）。
    //   QInputDialog 布局会按 sizeHint 收缩，setMinimumWidth 不生效；改为直接约束
    //   行编辑器宽度 + resize 双管齐下（宽度才能稳定落到 720）。
    QInputDialog stDlg(this);
    stDlg.setWindowTitle(QStringLiteral("重命名任务"));
    stDlg.setLabelText(QStringLiteral("任务名称："));
    stDlg.setTextValue(a_pTask->Name());
    if (QLineEdit* pEdit = stDlg.findChild<QLineEdit*>()) {
        pEdit->setMinimumWidth(640);   // 布局最小宽随行编辑器撑开，窗口 ≥720
    }
    stDlg.resize(720, stDlg.sizeHint().height());
    bOk = (stDlg.exec() == QDialog::Accepted);
    const QString sName = stDlg.textValue().trimmed();
    if (!bOk || sName.isEmpty() || sName == a_pTask->Name()) {
        return;   // 取消 / 空名 / 未变化
    }
    // 定位改名目标：优先活跃副本（sigChanged 已接 SaveTask）。
    CTaskInfo* pTarget = a_pTask;
    for (CTaskInfo* pTask : m_aTasks) {
        if (pTask->Id() == a_pTask->Id()) { pTarget = pTask; break; }
    }
    pTarget->SetAutoNamed(false);
    pTarget->SetName(sName);   // 活跃副本经 sigChanged 自动落盘
    // 历史副本（非活跃）：手动镜像 task.json + 历史库（无 sigChanged 接线）。
    if (pTarget == a_pTask && m_aTasks.indexOf(a_pTask) < 0) {
        const QString sJson = m_pPm->TaskDir(pTarget->Id()) + QStringLiteral("/task.json");
        if (QFile::exists(sJson)) {
            pTarget->SaveToFile(sJson);
        }
        if (m_pHistoryDb) {
            m_pHistoryDb->Upsert(pTarget);
        }
    }
    m_pTaskPanel->UpdateTaskRow(pTarget);
    ReloadHistory();   // 历史表经 DB 重拉刷新名称
}

void CMainWindow::OnAutoNameTask(CTaskInfo* a_pTask) {
    // 2026.08.20 (Agent 起名) 右键「自动命名」：任务内容 + guide.md 拼提示词经
    //   编排器 AutoNameNow 交 Agent 异步生成一句话名称，完成回调 OnAutoNameDone
    //   （DB 副本镜像 + 刷新两表）；助手忙时 AutoNameNow 内部同步回退本地启发式，
    //   此时名称已就地改好 → 直接镜像刷新。
    if (a_pTask == nullptr) {
        return;
    }
    CTaskInfo* pTarget = a_pTask;
    for (CTaskInfo* pTask : m_aTasks) {
        if (pTask->Id() == a_pTask->Id()) { pTarget = pTask; break; }
    }
    const QString sOldName = pTarget->Name();
    if (!m_pOrch->AutoNameNow(pTarget,
            m_pExecPanel != nullptr ? m_pExecPanel->ActiveAsstId() : QString())) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("自动命名"),
            QStringLiteral("未能生成有效名称（素材不足或助手异常），任务名保持不变。"), this);
        return;
    }
    if (pTarget->Name() == sOldName) {
        return;   // 异步路径：已提交 Agent，OnAutoNameDone 回调里镜像/刷新
    }
    MirrorTaskIfDetached(pTarget);   // 本地兜底同步完成：立即镜像 + 刷新
}

void CMainWindow::OnAutoNameDone(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sName) {
    // 2026.08.20 (Agent 起名回调)：sigAutoNameDone → 历史 DB 副本镜像 + 刷新两表。
    //   失败仅提示（名称已在编排器保持不变）。
    if (a_pTask == nullptr) {
        return;
    }
    if (!a_bOk) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("自动命名"),
            QStringLiteral("Agent 未能生成有效名称，任务名保持不变。"), this);
        return;
    }
    Q_UNUSED(a_sName);
    MirrorTaskIfDetached(a_pTask);
}

void CMainWindow::MirrorTaskIfDetached(CTaskInfo* a_pTarget) {
    // 历史副本（非活跃，无 sigChanged→SaveTask 接线）：手动镜像 task.json + 历史库；
    //   随后无论活跃与否都刷新两表。
    if (a_pTarget == nullptr) { return; }
    if (m_aTasks.indexOf(a_pTarget) < 0) {
        const QString sJson = m_pPm->TaskDir(a_pTarget->Id()) + QStringLiteral("/task.json");
        if (QFile::exists(sJson)) {
            a_pTarget->SaveToFile(sJson);
        }
        if (m_pHistoryDb) {
            m_pHistoryDb->Upsert(a_pTarget);
        }
    }
    m_pTaskPanel->UpdateTaskRow(a_pTarget);
    ReloadHistory();   // 历史表经 DB 重拉刷新名称
}

void CMainWindow::OnNewTask() {
    if (m_pPm->ProjectDir().isEmpty()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("请先打开工程目录"), this);
        return;
    }
    // FR-5：抽取的统一对话框（New 态：可编辑 + 立刻启动勾选）。
    CNewTaskDialog dlg(CNewTaskDialog::New, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;   // AC-4.③ 取消 → 不创建任何任务
    }
    // 需求描述非空校验已在对话框内 Validate() 完成（2026.08.26：不通过不关窗、输入不丢）。

    auto* pTask = new CTaskInfo(this);
    pTask->SetId(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    // 2026.08.20 (名称可留空)：未填名称 → 临时名占位（“任务-MMdd-HHmmss”）并标记
    // autoNamed；Intake 阶段产出 requirements.md 后由编排器推断正式名称回填（MaybeAutoNameTask）。
    if (dlg.Name().isEmpty()) {
        pTask->SetName(QStringLiteral("任务-%1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("MMdd-HHmmss"))));
        pTask->SetAutoNamed(true);
    } else {
        pTask->SetName(dlg.Name());
    }
    pTask->SetType(dlg.Type());
    // 2026.08.15 (流程档位) 档位 + 勾选阶段清单；2026-10-14 编排：勾选流与该基础类型
    //   内置规范流一致才存空表（旧任务兼容语义）；含自定义阶段/重排序时必须显式存清单。
    pTask->SetTier(dlg.Tier());
    pTask->SetFlowStages(dlg.IsCanonicalFlow() ? QList<EStage>() : dlg.CheckedStages());
    pTask->SetUserRequest(dlg.UserRequest());
    pTask->SetProjectDir(m_pPm->ProjectDir());
    pTask->SetCurrentStage(EStage::Intake);

    m_aTasks.append(pTask);
    connect(pTask, &CTaskInfo::sigChanged, this, [this, pTask]() { SaveTask(pTask); });
    SaveTask(pTask);   // 立即落盘 task.json
    m_pTaskPanel->SetTasks(m_aTasks);

    // 2026.08.20 未填名称 → 创建即起名（不等 Intake/guide.md）：任务内容（用户需求）
    //   + 当前 guide.md（有则拼入，无则纯按需求）交 Agent 生成名称，异步回填
    //   （sigChanged → SaveTask 自动落盘）。失败保持 autoNamed=true，Intake 后兜底重试。
    if (dlg.Name().isEmpty()) {
        m_pOrch->AutoNameNow(pTask,
            m_pExecPanel != nullptr ? m_pExecPanel->ActiveAsstId() : QString());
    }

    // FR-1 自动选中三连（复用 OnStartTask 中同款：选中并显示新任务，日志区就位）。
    // 助手档先切（须在 SelectTask 触发的 OnTaskSelected 之前：届时 key 已相等走早退，
    //   旧档已在本次切换中落盘；若此处裸赋值 m_pCur，OnTaskSelected 会因 key 相等早退而永不切档）。
    SwitchAssistantArchive(pTask);
    m_pTaskPanel->SelectTask(pTask);
    m_pExecPanel->SetCurrentTask(pTask);
    // 2026.08.26 新建任务后自动切到助手日志页：用户下一步就是与助手对话（2026.08.23
    //   的「保持原页」改掉——用户反馈新建后停在原页不符合常理）。
    m_pExecPanel->ShowActiveAssistantPage();

    // FR-4「立刻启动」：勾选则创建后立即运行；不勾则仅选中（状态 Pending，未运行）。
    if (dlg.StartImmediately()) {
        OnStartTask(pTask);   // AC-4.①
    } else {
        SetStatus(QStringLiteral("已创建任务：%1，点击「启动/继续」开始").arg(pTask->Name()));   // AC-4.②
    }
}

void CMainWindow::OnStartTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    if (m_pPm->ProjectDir().isEmpty()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("请先打开工程目录"), this);
        return;
    }
    // 堵抢占口子：已有任务运行中 → 拒绝启动（避免 m_pCurTask 被覆盖致回调串扰、原任务孤儿）。
    // 单任务串行语义；多任务真并发为后续 Phase。
    if (m_pOrch != nullptr && m_pOrch->IsRunnerBusy()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("已有任务运行中，请先暂停或停止当前任务，再启动新任务。"), this);
        return;
    }
    // guide.md 生成期即任务运行期（初始任务并入首阶段）：期间同样拒绝启动其他任务。
    if (m_bGuideBuilding) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("提示"),
                   QStringLiteral("guide.md 正在生成（作为任务首阶段），完成后将自动启动，请稍候。"), this);
        return;
    }
    // 先选中并显示该任务（日志/阶段表立即就位），再启动——确保随后的 stage_start
    // 等事件能上屏，避免「启动了却看不到」(#7)。
    // SwitchAssistantArchive 须在 SelectTask 之前（其触发的 OnTaskSelected 因 key 相等早退）。
    SwitchAssistantArchive(a_pTask);
    m_pTaskPanel->SelectTask(a_pTask);
    m_pExecPanel->SetCurrentTask(a_pTask);   // 2026.09.10 v2.2.153 载任务即自动切「日志」页
                                             //   （统一收在 SetCurrentTask；2026.08.24 的调用点
                                             //   散补 ShowTaskLogPage 作废）

    // 初始任务：guide.md 缺失则先自动扫描生成，完成后再续跑（不在打开工程时触发）。
    // 生成 guide.md 属于任务的一环（并入首阶段）：期间任务即置 Running——「继续」按钮置灰、
    // 费用栏前执行态指示器转圈、阶段表首阶段显示「▶ 进行中」。
    // guide.md 已存在时直接进编排器（StartTask 内另有指纹增量同步，无需本阶段）。
    if (!m_pPm->HasGuide()) {
        if (!m_pTaskPendingGuide.isNull() && m_pTaskPendingGuide.data() != a_pTask) {
            // 旧待启动任务被顶替：其 Running 是 guide 等待态，回退 Paused 防孤儿运行态。
            CTaskInfo* pOldPending = m_pTaskPendingGuide.data();
            pOldPending->SetStatus(ETaskStatus::Paused);
            OnTaskChanged(pOldPending);
        }
        m_pTaskPendingGuide = a_pTask;
        m_pExecPanel->AppendEvent(a_pTask->Id(), QStringLiteral("init_guide"),
            QStringLiteral("初始任务：工程缺少 guide.md，开始自动扫描生成..."));
        OnGenerateGuide();   // 异步扫描；完成后 OnGuideFinished 自动续跑
        a_pTask->SetStatus(ETaskStatus::Running);   // 生成期即任务运行期（并入首阶段）
        OnTaskChanged(a_pTask);   // 刷新按钮/指示器/阶段表；续跑由 OnGuideFinished 决定
        return;   // guide 未就绪前不进入编排器（其 StartTask 的 guard 也会把任务置 Paused）
    }

    m_pOrch->StartTask(a_pTask);
}

void CMainWindow::OnPauseTask(CTaskInfo* a_pTask) {
    // 2026.08.25 guide 生成等待期（初始任务缺 guide.md，无 runner）：暂停立即落 Paused
    //   （旧实现只置标志，转圈不停；生成完成后 OnGuideFinished 按 PauseRequested 停续跑）。
    if (!m_pTaskPendingGuide.isNull() && m_pTaskPendingGuide.data() == a_pTask) {
        m_pOrch->PauseTask(a_pTask);   // 置 PauseRequested + 事件入日志
        a_pTask->SetStatus(ETaskStatus::Paused);
        OnTaskChanged(a_pTask);       // 刷新按钮/执行态指示器（转圈停）
        return;
    }
    m_pOrch->PauseTask(a_pTask);
}

void CMainWindow::OnStopTask(CTaskInfo* a_pTask) {
    // 2026.08.25 guide 生成等待期（初始任务缺 guide.md，无 runner）：停止立即落 Stopped
    //   并解除待续跑（旧实现只置标志，转圈不停；生成完仍会按 StopRequested 停但期间一直转）。
    if (!m_pTaskPendingGuide.isNull() && m_pTaskPendingGuide.data() == a_pTask) {
        m_pTaskPendingGuide = nullptr;   // 解除生成完自动续跑（OnGuideFinished 早退）
        a_pTask->SetStopRequested(true);
        a_pTask->SetStatus(ETaskStatus::Stopped);
        m_pExecPanel->AppendEvent(a_pTask->Id(), QStringLiteral("init_guide"),
            QStringLiteral("初始任务：guide.md 仍在生成，任务已停止（生成完成后不会自动启动）"));
        OnTaskChanged(a_pTask);       // 刷新按钮/执行态指示器（转圈停）
        return;
    }
    m_pOrch->StopTask(a_pTask);
}

void CMainWindow::OnRollback(CTaskInfo* a_pTask) {
    // 统一回退界面（方案 §5）：任务侧入口——打开 master-detail 对话框，accept 后交引擎。
    // 2026.08.26 空任务（取消选中）不再直接 return：落工程全局档（owner=global，
    //   助手产生的 turn 快照所在），与「功能→回退历史版本」菜单同语义。
    if (m_pPm == nullptr) {
        return;
    }
    if (a_pTask == nullptr) {
        OnRollbackTurnOpen();
        return;
    }
    if (a_pTask->Status() == ETaskStatus::Running) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("回退"),
            QStringLiteral("任务运行中，请先停止再回退。"), this);
        return;
    }
    CRollbackDialog stDlg(a_pTask->Id(), m_pPm, this, a_pTask->Name());
    const int iDlgRet = stDlg.exec();
    // 2026.08.22 对话框 hide/析构已写 config.ini（布局）→ 打点跳过自身触发的配置热重载。
    m_iCfgSelfSaveMs = QDateTime::currentMSecsSinceEpoch();
    if (iDlgRet != QDialog::Accepted || stDlg.SelectedVersion() == nullptr) {
        return;
    }
    OnRollbackTo(a_pTask, *stDlg.SelectedVersion());
}

QString CMainWindow::CurrentAdhocOwner() const {
    return (m_pCur != nullptr) ? m_pCur->Id() : QStringLiteral("global");
}

void CMainWindow::OnRollbackTurnOpen() {
    // 助手侧入口：owner = 当前助手档（选中任务=任务 id / 工程全局档="global"）。
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        ExecMsgBox(QMessageBox::Information, QStringLiteral("代码回退"),
            QStringLiteral("请先打开一个工程再使用代码回退。"), this);
        return;
    }
    CRollbackDialog stDlg(CurrentAdhocOwner(), m_pPm, this,
                          m_pCur ? m_pCur->Name() : QString());
    const int iDlgRet = stDlg.exec();
    // 2026.08.22 同上：写盘打点，防双实例/自身配置热重载误触发。
    m_iCfgSelfSaveMs = QDateTime::currentMSecsSinceEpoch();
    if (iDlgRet != QDialog::Accepted || stDlg.SelectedVersion() == nullptr) {
        return;
    }
    OnRollbackTurnTo(*stDlg.SelectedVersion());
}

void CMainWindow::OnRollbackTo(CTaskInfo* a_pTask, const SBackupVersion& a_stVer) {
    if (a_pTask == nullptr) {
        return;
    }
    ExecuteRollback(a_pTask, a_stVer);
}

void CMainWindow::OnRollbackTurnTo(const SBackupVersion& a_stVer) {
    ExecuteRollback(nullptr, a_stVer);
}

void CMainWindow::ExecuteRollback(CTaskInfo* a_pTask, const SBackupVersion& a_stVer) {
    // §8 回退引擎：0 残留前置 → 1 运行守卫 → 2 破坏性预检 → 3 rollback-pre 快照
    //                → 4 RestoreTree → 5 收尾 + SetPendingNotice。
    if (m_pPm == nullptr) {
        return;
    }
    // 步骤 0：崩溃残留前置（上次回退被进程死亡打断）——还原现场后由用户重新发起。
    if (m_pPm->HasRestoreTmp()) {
        QString sErr;
        if (!m_pPm->RestoreFromTmp(sErr)) {
            ExecMsgBox(QMessageBox::Critical, QStringLiteral("回退"),
                QStringLiteral("检测到上次回退中断的残留，还原现场失败：%1").arg(sErr), this);
            return;
        }
    }
    // 步骤 1：运行互斥守卫（挡助手与任务双侧 + 阶段后台备份线程——备份期间
    //          IsRunnerBusy 为 false 但统一库/源码树正被并发写入）。
    if (m_pOrch != nullptr && (m_pOrch->IsRunnerBusy() || m_pOrch->IsAnyAdhocRunning())) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("回退"),
            QStringLiteral("有任务或助手正在运行，请先停止再回退。"), this);
        return;
    }
    if (m_pOrch != nullptr && m_pOrch->IsBackupRunning()) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("回退"),
            QStringLiteral("阶段前快照备份进行中，请稍后重试。"), this);
        return;
    }
    const QString sOwner = (a_pTask != nullptr) ? a_pTask->Id() : CurrentAdhocOwner();

    // 步骤 2：破坏性预检（目标 vs 该 owner 最新记录点：覆盖/删除计数 + 人工改动警示）。
    QString sDetail;
    const QList<SBackupVersion> aVersions = m_pPm->ListUnifiedBackups(sOwner);
    if (!aVersions.isEmpty() && !aVersions.first().bLegacy) {
        const SBackupVersion& stLatest = aVersions.first();
        if (!stLatest.bLegacy && !a_stVer.bLegacy && stLatest.iSeq != a_stVer.iSeq) {
            SDiffResult stDiff;
            if (m_pPm->DiffVersions(a_stVer.iSeq, stLatest.iSeq, stDiff)) {
                QStringList aWarn;
                for (const SDiffEntry& st : stDiff.aEntries) {
                    if (aWarn.size() >= 20) {
                        aWarn.append(QStringLiteral("… 等 %1 项")
                            .arg(stDiff.aEntries.size() - 20));
                        break;
                    }
                    aWarn.append(st.sPath);
                }
                if (!aWarn.isEmpty()) {
                    sDetail = QStringLiteral("\n\n将覆盖的文件（含最新记录点之后的人工/其他改动，共 %1 处）：\n· %2")
                        .arg(stDiff.aEntries.size()).arg(aWarn.join(QStringLiteral("\n· ")));
                }
            }
        }
    }
    const auto eRet = ExecQuestion(QStringLiteral("确认回退"),
        QStringLiteral("即将把工程源码恢复到记录点 #%1。\n"
                       "当前代码会先自动快照为新的记录点，随时可再回退撤销。%2\n确认继续？")
            .arg(a_stVer.iSeq).arg(sDetail), this);
    if (eRet != QMessageBox::Yes) {
        return;
    }

    // 步骤 3：rollback-pre 现场保护快照（失败即中止——宁可不回退，不可丢现场）。
    const int iPre = m_pPm->BackupTree(QStringLiteral("rollback-pre"), sOwner,
                                       QString(), QString(), a_stVer.iSeq);
    if (iPre <= 0) {
        ExecMsgBox(QMessageBox::Critical, QStringLiteral("回退中止"),
            QStringLiteral("回退前现场快照失败，已中止（工程未改动）。请检查磁盘空间/权限后重试。"), this);
        return;
    }
    // 步骤 4：原子恢复。
    QString sErr;
    if (!m_pPm->RestoreTree(a_stVer, sErr)) {
        ExecMsgBox(QMessageBox::Critical, QStringLiteral("回退失败"),
            QStringLiteral("恢复源码失败：%1（工程目录已回滚到恢复前状态）").arg(sErr), this);
        return;
    }
    // 步骤 5：收尾。
    m_pPm->SetPendingNotice(a_stVer.iSeq, sOwner);
    if (a_pTask != nullptr) {
        a_pTask->SetStatus(ETaskStatus::Stopped);
        SaveTask(a_pTask);
        m_pTaskPanel->RefreshTask(a_pTask);
        m_pExecPanel->RefreshTask(a_pTask);
        SetStatus(QStringLiteral("[%1] 已回退到 #%2（当前现场已存为 #%3）")
            .arg(a_pTask->Name()).arg(a_stVer.iSeq).arg(iPre));
    } else {
        m_pExecPanel->AppendEvent(sOwner, QStringLiteral("rollback"),
            QStringLiteral("已回退到 #%1 之前（现场已存为 #%2）").arg(a_stVer.iSeq).arg(iPre));
        SetStatus(QStringLiteral("已回退到 #%1（当前现场已存为 #%2）")
            .arg(a_stVer.iSeq).arg(iPre));
    }
}

void CMainWindow::OnDeleteTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    if (a_pTask->Status() == ETaskStatus::Running) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("提示"),
            QStringLiteral("任务运行中，请先停止再删除"), this);
        return;
    }
    const auto eRet = ExecQuestion(
        QStringLiteral("删除任务"),
        QStringLiteral("确认彻底删除任务「%1」？\n该任务将从当前列表与历史库中删除，工程侧任务目录（task.json、阶段产物、聊天/助手档、备份）整体移除，不可恢复")
            .arg(a_pTask->Name()), this);
    if (eRet != QMessageBox::Yes) {
        return;
    }

    const QString sId = a_pTask->Id();

    // 统一备份库（方案 §2.4）：删该 owner 全部版本 + 引用 GC（旧全量库随任务目录整体删除）。
    if (m_pPm != nullptr) {
        m_pPm->PurgeOwner(sId);
    }

    // 1. 删全局历史库（按 id）
    if (m_pHistoryDb) {
        m_pHistoryDb->Delete(sId);
    }

    // 2. 判断 a_pTask 属于哪个列表：活跃副本(m_aTasks) 还是历史副本(m_aHistoryTasks)。
    // 2026.08.19 task.json 两边都删（此前历史副本不动——当前工程的终态任务也在历史表，
    //    只删 DB 会因重开工程 LoadAllTasks 复活再镜像回 DB）；历史副本按任务自身
    //    ProjectDir 定位（其它工程的记录同样彻底删除）。
    const int iAct = m_aTasks.indexOf(a_pTask);
    const bool bActive = (iAct >= 0);
    if (bActive) {
        m_aTasks.removeAt(iAct);
    } else {
        const int iHis = m_aHistoryTasks.indexOf(a_pTask);
        if (iHis >= 0) {
            m_aHistoryTasks.removeAt(iHis);
        }
    }

    // 3. 清理选中态：助手档回退工程全局档（须在删目录之前——SaveAssistantState 会往
    //    该任务目录写档；先落最后一档并回退全局档，任务目录才可整体移除不留残件）。
    if (m_pCur == a_pTask) {
        if (m_pOrch != nullptr && m_pOrch->IsAnyAdhocRunning()) {
            m_pOrch->StopAllAdhoc();   // ⏹ 停止痕迹落该任务档
        }
        SaveAssistantState();       // 该任务助手档最后落盘（随后整目录删除，不残留）
        m_pCur = nullptr;
        m_pExecPanel->SetCurrentTask(nullptr);
        ApplyTaskConfigOverlay(nullptr);   // 2026.09.09 v2.1.143 任务删除/移除：参数源回全局
        LoadAssistantState();       // 回退工程全局档（无则清空）
    }

    // 4. 整目录删除（2026.08.20 按用户预期改彻底删除：此前仅删 task.json、.md/chat/
    //    assistant/backups 留盘，用户发现"删除后任务文件根本没删"。目录随任务一起移除，
    //    removeRecursively 已含 task.json；跨工程历史副本按任务自身 ProjectDir 定位）。
    if (!a_pTask->ProjectDir().isEmpty()) {
        const QString sTaskDir = bActive && m_pPm != nullptr && !m_pPm->ProjectDir().isEmpty()
            ? m_pPm->TaskDir(sId)
            : QDir::cleanPath(a_pTask->ProjectDir() + QStringLiteral("/.autodev/tasks/")
                + sId);
        QDir stTaskDir(sTaskDir);
        if (stTaskDir.exists() && !stTaskDir.removeRecursively()) {
            // 兜底：目录被占用（explorer 预览/杀软扫描）时退回至少删 task.json，防复活。
            QFile::remove(sTaskDir + QStringLiteral("/task.json"));
        }
    }

    // 5. 释放对象（a_pTask 已从所属列表移除，不会悬垂于列表）；而板若正指向该任务
    //   （挂起档切换场景 m_pCur≠面板任务）同步摘下，防 OnAdhocSubmit 等解引用已释放内存。
    if (m_pExecPanel != nullptr && m_pExecPanel->CurrentTask() == a_pTask) {
        m_pExecPanel->SetCurrentTask(nullptr);
    }
    delete a_pTask;

    // 6. 刷新两表（历史表从 DB 重拉，确保删行后一致）
    m_pTaskPanel->SetTasks(m_aTasks);
    ReloadHistory();
}

void CMainWindow::OnRemoveTask(CTaskInfo* a_pTask) {
    // #3 「移除」：把当前任务移到历史任务（保留记录，可再次执行）。
    // 与 OnDeleteTask 的区别——不删 DB / task.json，只置 Stopped 并存档，再从活跃列表摘除。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const auto eRet = ExecQuestion(
        QStringLiteral("移除任务"),
        QStringLiteral("是否将当前任务移除到历史任务中，如有需求，历史任务可以再次运行"), this);
    if (eRet != QMessageBox::Yes) {
        return;
    }

    // 1. 运行中先停止（StopTask 置停止标志，编排器下一轮退出执行循环）
    if (a_pTask->Status() == ETaskStatus::Running && m_pOrch != nullptr) {
        m_pOrch->StopTask(a_pTask);
    }

    // 2. 标记 Stopped 并落盘（task.json + DB 镜像），使其在历史表显示为「已停止」；
    //    同时置归档标记——否则重启后 OpenProjectDir 无法区分普通停止与归档，又载回当前表
    a_pTask->SetArchived(true);
    a_pTask->SetStatus(ETaskStatus::Stopped);
    SaveTask(a_pTask);

    // 3. 从活跃列表摘除；其数据已存 DB，历史副本由 ReloadHistory 自 DB 重建，故活跃副本可回收
    const int iAct = m_aTasks.indexOf(a_pTask);
    if (iAct >= 0) {
        m_aTasks.removeAt(iAct);
    }

    // 4. 清选中态（若移除的正是当前选中任务）：助手档回退工程全局档
    if (m_pCur == a_pTask) {
        if (m_pOrch != nullptr && m_pOrch->IsAnyAdhocRunning()) {
            m_pOrch->StopAllAdhoc();   // ⏹ 停止痕迹落该任务档
        }
        SaveAssistantState();       // 该任务助手档最后落盘
        m_pCur = nullptr;
        m_pExecPanel->SetCurrentTask(nullptr);
        ApplyTaskConfigOverlay(nullptr);   // 2026.09.09 v2.1.143 任务删除/移除：参数源回全局
        LoadAssistantState();       // 回退工程全局档（无则清空）
    }
    // 2026.09.28 面板可能仍指向被移除任务（挂起档切换 m_pCur≠面板任务）：摘下防悬垂。
    if (m_pExecPanel != nullptr && m_pExecPanel->CurrentTask() == a_pTask) {
        m_pExecPanel->SetCurrentTask(nullptr);
    }

    delete a_pTask;   // 活跃副本回收（DB 记录与 task.json 均保留，再次执行靠 OnReviveTask 重建）

    // 5. 刷新两表：当前表少一行；历史表经 ReloadHistory 过滤后多出该 Stopped 任务
    m_pTaskPanel->SetTasks(m_aTasks);
    ReloadHistory();
}

void CMainWindow::OnTaskSelected(CTaskInfo* a_pTask) {
    SwitchAssistantArchive(a_pTask);   // 助手档跟任务切换（停跑→存旧档→载新档；同档早退）
    // 2026.09.09 v2.1.143 任务级配置覆盖：点击任务即切设置参数源（任务档键优先，
    //   缺键已由激活时合并回填/回退全局），并按生效配置重应用运行期（后端/模型表/
    //   路径自愈/单价等全部随之切换；静默口径不弹后端警告——警告属启动/保存路径）。
    ApplyTaskConfigOverlay(a_pTask);
    // 2026.08.15 历史表选中 DB 副本（无阶段历史数组）→ 从该工程 task.json 懒加载完整执行态，
    //   使阶段表按执行时进度标注（✔/✖），而不是全部「待执行」。加载失败保持 DB 副本原样。
    if (a_pTask != nullptr && a_pTask->History().isEmpty()
        && !a_pTask->ProjectDir().isEmpty() && !a_pTask->Id().isEmpty()) {
        bool bActive = false;   // 活跃任务内存中已有完整历史，不读盘
        for (CTaskInfo* p : m_aTasks) {
            if (p != nullptr && p->Id() == a_pTask->Id()) { bActive = true; break; }
        }
        if (!bActive) {
            const QString sJson = QDir::cleanPath(a_pTask->ProjectDir()
                + QStringLiteral("/.autodev/tasks/") + a_pTask->Id()
                + QStringLiteral("/task.json"));
            if (QFile::exists(sJson) && a_pTask->LoadFromFile(sJson)) {
                // 历史副本不连 sigChanged，此处置状态无回写副作用。
                if (a_pTask->Status() == ETaskStatus::Running) {
                    a_pTask->SetStatus(ETaskStatus::Paused);   // 易失运行态回退（与打开工程时一致）
                }
                // 回填全局库 run_ms/费用等列（幂等）+ 原地刷新历史行耗时列，
                // 使历史表「耗时」列不必等下一次 ReloadHistory 才有值。
                if (m_pHistoryDb) {
                    m_pHistoryDb->Upsert(a_pTask);
                }
                if (m_pTaskPanel) {
                    m_pTaskPanel->UpdateTaskRow(a_pTask);
                }
            }
        }
    }
    // #6 懒加载任务对话流：首次选中且 chat.json 存在时，先注入 block 缓存，
    // 随后 SetCurrentTask 的 RebuildView 即可渲染恢复的历史日志（无需等新输出）。
    if (a_pTask != nullptr && m_pExecPanel != nullptr && m_pPm != nullptr) {
        const QString sId = a_pTask->Id();
        if (!m_pExecPanel->HasTaskBlocks(sId)) {
            const QByteArray aData = m_pPm->ReadTaskChat(sId);
            if (!aData.isEmpty()) {
                const QJsonDocument oDoc = QJsonDocument::fromJson(aData);
                if (oDoc.isArray()) {
                    m_pExecPanel->LoadTaskBlocksFromJson(sId, oDoc.array());
                }
            }
        }
    }
    m_pExecPanel->SetCurrentTask(a_pTask);
}

void CMainWindow::OnStageLog(CTaskInfo* a_pTask, const QString& a_sLine) {
    if (a_pTask == nullptr) {
        return;
    }
    m_pExecPanel->AppendLog(a_pTask->Id(), a_sLine);
}

void CMainWindow::OnStageEvent(CTaskInfo* a_pTask, const QString& a_sKind, const QString& a_sText) {
    if (a_pTask == nullptr) {
        return;
    }
    m_pExecPanel->AppendEvent(a_pTask->Id(), a_sKind, a_sText);
    SetStatus(QStringLiteral("[%1] %2").arg(a_pTask->Name(), a_sText.left(100)));
    // FR-5：阶段成功结束（产物已落盘）且为当前选中任务时，刷新阶段产物清单（AC-5.1），
    //        保留当前查看态（AC-5.2）。选 stage_done 时机避免运行中途频繁扫盘（NFR-3）。
    if (a_sKind == QStringLiteral("stage_done") && a_pTask == m_pCur) {
        m_pExecPanel->RefreshStageOutputs(a_pTask);
    }

    // 迭代熔断（escalation）：任务已暂停并升级到用户——弹模态对话框强制提醒，
    // 否则只在执行面板日志流里出现一条 ⚠，用户不盯着就完全错过。
    if (a_sKind == QStringLiteral("escalation")) {
        OnEscalation(a_pTask, a_sText);
    }
}

// 迭代熔断弹窗：显示阻塞详情（escalation.md）+ 三动作（打开任务目录 / 继续并可附指导意见 / 忽略）。
// 2026.08.15 (熔断人工决策) 「继续」升级为决策入口：可输入指导意见（可空），
//   随下一轮生产阶段 prompt 以「## 人工指导意见（最高优先级）」注入，一次性消费。
void CMainWindow::OnEscalation(CTaskInfo* a_pTask, const QString& a_sEventText) {
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const QString sReportPath =
        m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/escalation.md");
    QString sDetail;
    CFrwDevice fReport(sReportPath);
    if (fReport.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sDetail = QString::fromUtf8(fReport.readAll());
        fReport.close();
    } else {
        sDetail = a_sEventText;
    }
    if (sDetail.length() > 1200) {
        sDetail = sDetail.left(1200) + QStringLiteral("\n...");
    }

    QMessageBox stBox(QMessageBox::Warning,
        QStringLiteral("任务需要人工处理 — %1").arg(a_pTask->Name()),
        QStringLiteral("迭代熔断：评审已连续驳回 %1 次，开发未能收敛，任务已暂停。\n"
                       "报告：\n%2\n"
                       "提示：选「忽略」后再从任务列表点「继续」，只会再跑一轮即再次熔断；"
                       "建议用下方「继续」按钮处理。").arg(a_pTask->StageIter()).arg(sReportPath),
        QMessageBox::NoButton, this);
    stBox.setInformativeText(sDetail);
    stBox.setTextFormat(Qt::PlainText);
    QPushButton* pBtnOpen  = stBox.addButton(QStringLiteral("打开任务目录"),
                                             QMessageBox::AcceptRole);
    QPushButton* pBtnRetry = stBox.addButton(QStringLiteral("继续（可附指导意见）"),
                                             QMessageBox::AcceptRole);
    stBox.addButton(QStringLiteral("忽略"), QMessageBox::RejectRole);
    QFont fntBox = stBox.font();
    fntBox.setPointSize(s_kiMsgPt);
    stBox.setFont(fntBox);
    stBox.exec();

    if (stBox.clickedButton() == pBtnRetry) {
        // 人工决策：先收集指导意见（多行、可留空），再重置迭代计数 + 清暂停标志 + 续跑
        // （再给完整 N 次迭代机会；指导意见注入下一轮生产阶段 prompt 后自动清空）。
        bool bOkDlg = false;
        const QString sGuidance = QInputDialog::getMultiLineText(this,
            QStringLiteral("人工指导意见 — %1").arg(a_pTask->Name()),
            QStringLiteral(
                "可选。将作为「人工指导意见（最高优先级）」注入下一轮重做阶段的提示词。\n"
                "例如：指出评审意见中可放宽的条目、正确的实现方向、或让 agent 改用保守方案。\n"
                "留空则仅重置迭代计数并继续。"),
            QString(), &bOkDlg).trimmed();
        if (!bOkDlg) {
            return;   // 取消输入：保持 Paused，用户可稍后再决策
        }
        a_pTask->SetResumeGuidance(sGuidance);
        a_pTask->ResetStageIter();
        a_pTask->SetPauseRequested(false);
        OnStartTask(a_pTask);
    } else if (stBox.clickedButton() == pBtnOpen) {
        // 打开 .autodev/tasks/<id>/ 供用户查看 cr.md / escalation.md。
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_pPm->TaskDir(a_pTask->Id())));
    }
    // 「忽略」：任务保持 Paused，等用户自行处理。
}

void CMainWindow::OnTaskChanged(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    m_pTaskPanel->RefreshTask(a_pTask);
    if (a_pTask == m_pCur) {
        m_pExecPanel->RefreshTask(a_pTask);
    }
}

void CMainWindow::OnReviewEscalated(CTaskInfo* a_pTask) {
    // 2026.09.22 评审人工介入（用户需求：审核失败时允许人工确认后继续）：
    //   评审多次驳回熔断暂停时弹窗——人工放行（视为评审通过继续）/ 再迭代一轮 / 保持暂停。
    //   [Policy] ReviewAskOnEscalate=0 时不弹，退回旧版静默升级（无人值守跑批）。
    if (a_pTask == nullptr || m_pPm == nullptr || m_pOrch == nullptr) {
        return;
    }
    if (!m_pPm->LoadReviewAskOnEscalate()) {
        return;
    }
    const QString sReport = m_pPm->TaskDir(a_pTask->Id())
        + QStringLiteral("/escalation.md");
    QMessageBox box(this);
    QFont fBox = box.font();
    fBox.setPointSize(11);
    box.setFont(fBox);
    box.setWindowTitle(QStringLiteral("评审人工介入"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral(
        "任务「%1」的评审阶段已迭代多次仍被驳回，已按迭代上限暂停。\n\n"
        "可选择：\n"
        "· 人工放行，继续 —— 忽略本次驳回，视为评审通过继续推进后续流程；\n"
        "· 再迭代一轮 —— 重置迭代计数并重跑回炉阶段（评审意见仍会注入）；\n"
        "· 保持暂停 —— 稍后在任务列表点「继续」自行处理。\n\n"
        "评审报告：%2")
        .arg(a_pTask->Name(), sReport));
    QPushButton* pPass = box.addButton(QStringLiteral("人工放行，继续"), QMessageBox::AcceptRole);
    QPushButton* pRerun = box.addButton(QStringLiteral("再迭代一轮"), QMessageBox::ActionRole);
    QPushButton* pWait = box.addButton(QStringLiteral("保持暂停"), QMessageBox::RejectRole);
    box.setDefaultButton(pWait);   // Esc/回车缺省＝保守：不动流程
    box.exec();
    if (box.clickedButton() == pPass) {
        m_pOrch->ResolveReviewEscalation(a_pTask, 0);
    } else if (box.clickedButton() == pRerun) {
        m_pOrch->ResolveReviewEscalation(a_pTask, 1);
    } else {
        Q_UNUSED(pWait);
        m_pOrch->ResolveReviewEscalation(a_pTask, 2);
    }
}

void CMainWindow::OnTaskFinished(CTaskInfo* a_pTask, bool a_bOk) {
    if (a_pTask == nullptr) {
        return;
    }

    // 终态归档（Done/Failed）：把任务从活跃列表迁出到历史表。
    // 必要性——ReloadHistory 会过滤掉仍在 m_aTasks 的任务，若不迁出，则该任务
    //   当前表因「只放非终态」不显示、历史表因「活跃集合排除」也不显示，凭空消失；
    //   且活跃副本此后无用（DB + task.json 已写穿留底，再次执行靠 OnReviveTask 重建）。
    // 做法与 OnRemoveTask 一致：摘 m_aTasks → 清选中 → 回收活跃副本 → 刷两表。
    // 提示文案须在 delete 前取数据（Name/LastError 来自对象，回收后不可访问）。
    const ETaskStatus eSt = a_pTask->Status();
    if (eSt == ETaskStatus::Done || eSt == ETaskStatus::Failed) {
        const QString sName = a_pTask->Name();
        const QString sDoneId = a_pTask->Id();   // 需求4：delete 前捕获，用于同步 guide.md
        if (eSt == ETaskStatus::Done) {
            SetStatus(QStringLiteral("任务「%1」已完成 ✔").arg(sName));
        } else {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("任务失败"),
                QStringLiteral("任务「%1」失败：%2")
                    .arg(sName, a_pTask->LastError().left(300)), this);
        }

        const int iAct = m_aTasks.indexOf(a_pTask);
        if (iAct >= 0) {
            m_aTasks.removeAt(iAct);
        }
        if (m_pCur == a_pTask) {
            if (m_pOrch != nullptr && m_pOrch->IsAnyAdhocRunning()) {
                m_pOrch->StopAllAdhoc();   // ⏹ 停止痕迹落该任务档
            }
            SaveAssistantState();       // 该任务助手档最后落盘
            m_pCur = nullptr;
            m_pExecPanel->SetCurrentTask(nullptr);
            ApplyTaskConfigOverlay(nullptr);   // 2026.09.09 v2.1.143 任务终态归档：参数源回全局
            LoadAssistantState();       // 回退工程全局档（无则清空）
        }
        // 2026.09.28 面板可能仍指向终态归档任务（挂起档切换 m_pCur≠面板任务）：摘下防悬垂。
        if (m_pExecPanel != nullptr && m_pExecPanel->CurrentTask() == a_pTask) {
            m_pExecPanel->SetCurrentTask(nullptr);
        }
        delete a_pTask;   // 活跃副本回收（DB 记录与 task.json 均保留，再次执行靠 OnReviveTask 重建）
        m_pTaskPanel->SetTasks(m_aTasks);   // 当前表少一行
        ReloadHistory();                    // 历史表经 DB 重拉，多出该终态任务
        // 2026.09.10 v2.2.153 任务终态：切「历史任务」页并选中刚结束的任务——与「再次执行
        //   切当前任务」同口径，选中即自动显示其任务日志，用户视角从运行日志无跳变落到
        //   终态记录上（否则终态后选中清空，左栏停在当前表、日志区变空白）。
        CTaskInfo* pHis = nullptr;
        for (CTaskInfo* p : m_aHistoryTasks) {
            if (p != nullptr && p->Id() == sDoneId) { pHis = p; break; }
        }
        if (pHis != nullptr) {
            m_pTaskPanel->SelectHistoryTask(pHis);
        }
        // 需求4：任务完成（改过代码）→ 按 record.md 新增记录增量同步 guide.md
        //   （无清单则 SyncGuideAsync 自行跳过；Failed 不同步）。
        if (eSt == ETaskStatus::Done) {
            m_pPm->SyncGuideAsync(sDoneId);
        }
        return;
    }

    // 非终态（Paused/Stopped 等）：任务留在当前表，保持原刷新行为
    m_pTaskPanel->RefreshTask(a_pTask);
    if (a_pTask == m_pCur) {
        m_pExecPanel->RefreshTask(a_pTask);
    }
    if (a_bOk) {
        SetStatus(QStringLiteral("任务「%1」已完成 ✔").arg(a_pTask->Name()));
    } else if (eSt != ETaskStatus::Stopped && eSt != ETaskStatus::Paused) {
        ExecMsgBox(QMessageBox::Warning, QStringLiteral("任务失败"),
            QStringLiteral("任务「%1」失败：%2")
                .arg(a_pTask->Name(), a_pTask->LastError().left(300)), this);
    }
    ReloadHistory();
}

void CMainWindow::OnAdhocSubmit(const QString& a_sAsstId, const QString& a_sText, int a_iPerm) {
    // 2026.09.03 多助手并发：目标助手 id 由面板锥死（激活助手）；编排器会话 per-助手
    //   常驻，不再有换载步骤。跨轮上下文回灌记录按该助手流提取。
    //   2026-09-11 a_iPerm：权限三态（0 自动审批/1 手工审批/2 只读）原样透传导编排器。
    if (m_pOrch != nullptr && !a_sAsstId.isEmpty()) {
        QStringList aTranscript;
        if (m_pExecPanel != nullptr) {
            aTranscript = m_pExecPanel->BuildAsstTranscript(a_sAsstId, a_sText);
        }
        // 传当前选中任务 id：助手档/经验记忆跟着任务走（空=无任务，走工程全局助手档）。
        // 2026.09.09 取面板有效选中（CurrentTask）而非 m_pCur：取消选中时若助手在跑，
        //   SwitchAssistantArchive 会早退保留 m_pCur（旧任务仅作存档 key，UI 已显示未选中）——
        //   若用 m_pCur 取 id，未选任务的新提示词仍会注入旧任务的【任务经验记忆】等
        //   任务信息；面板 m_pCurTask 与用户所见严格同步，设它为准：没选任务就不带任务信息。
        CTaskInfo* pSel = (m_pExecPanel != nullptr) ? m_pExecPanel->CurrentTask() : m_pCur;
        m_pOrch->SubmitAdhoc(a_sAsstId, a_sText, a_iPerm,
                             pSel != nullptr ? pSel->Id() : QString(), &aTranscript);
    }
}

void CMainWindow::OnAsstAutoRename(QString a_sAsstId, QString a_sPrompts) {
    // 2026-09-22 助手自动重命名：面板已收集好提示词，转编排器借会话交 Agent 总结名称+说明；
    //   同步失败（工程未开/无会话/全部助手忙）也走面板 ApplyAsstAutoName 留失败事件行。
    //   2026.10.11 参数改值传（172129 崩溃实锤）：原 const& 直连链全程引用面板结构体
    //   成员，链上同步槽可重排/销毁助手数组 → 引用悬垂；值拷贝在链启动前完成，链上
    //   任意时点（含失败回退 ApplyAsstAutoName 再用）都安全。
    if (m_bProjectSwitching) {
        return;   // 2026-10-04 切工程隔离窗：停链收尾触发的自动命名不起新轮
                  //   （命名子轮会在旧工程面板尚在时启动，轮块/尾块落盘串新工程档）。
    }
    if (m_pOrch == nullptr || !m_pOrch->AutoNameAsstNow(a_sAsstId, a_sPrompts)) {
        if (m_pExecPanel != nullptr) {
            m_pExecPanel->ApplyAsstAutoName(a_sAsstId, false, QString(),
                QStringLiteral("当前无可用的空闲助手会话（或工程未打开），请稍后再试"));
        }
    }
}

void CMainWindow::OnAdhocUndo(const QString& a_sAsstId) {
    if (m_pOrch != nullptr) {
        m_pOrch->UndoAdhoc(a_sAsstId);
    }
}

void CMainWindow::OnAdhocCompact(const QString& a_sAsstId) {
    if (m_pOrch != nullptr) {
        m_pOrch->CompactAdhoc(a_sAsstId);
    }
}

void CMainWindow::OnAdhocUsage(const QString& a_sAsstId, qint64 a_iIn, qint64 a_iOut,
                               double a_dCost, qint64 a_iElapsedMs, int a_iReqs) {
    // 2026.09.05 任务/助手统计完全隔离（用户需求：每个助手自己统计自己的）：助手消耗
    //   一律归集到产出助手自身（随 assistant.json 持久化，切助手 Tab 各自查看），
    //   不再并入当前选中任务的统计——任务侧只计任务流水线阶段本身的消耗。
    //   消耗归属 = 产出助手 id（信号首参锥定；空则回退激活助手）。
    if (m_pExecPanel == nullptr) {
        return;
    }
    QString sId = a_sAsstId;
    if (sId.isEmpty()) {
        sId = m_pExecPanel->ActiveAsstId();
    }
    if (sId.isEmpty()) {
        return;
    }
    m_pExecPanel->AddAsstUsage(sId, a_iIn, a_iOut, a_dCost, a_iElapsedMs, a_iReqs);
    SaveAssistantState();   // 助手档落盘（选中任务时写任务私有档，否则工程全局档）
}

void CMainWindow::OnTaskInject(const QString& a_sTaskId, const QString& a_sText) {
    // FR-3：按 taskId 找任务对象 → 交编排器注入（确定性路由，NFR-7）。
    if (m_pOrch == nullptr || a_sTaskId.isEmpty()) {
        return;
    }
    CTaskInfo* pTask = nullptr;
    if (m_pCur != nullptr && m_pCur->Id() == a_sTaskId) {
        pTask = m_pCur;
    } else {
        for (CTaskInfo* p : m_aTasks) {
            if (p != nullptr && p->Id() == a_sTaskId) {
                pTask = p;
                break;
            }
        }
    }
    if (pTask == nullptr) {
        return;
    }
    m_pOrch->InjectTaskMessage(pTask, a_sText);
}

// 2026-09-26 内置测试沙箱管理对话框（非模态）：重复点击只抬升已有窗口。
void CMainWindow::OnSandboxDialog() {
    if (m_pSbxDlg == nullptr) {
        m_pSbxDlg = new CSandboxDialog(m_pSbx, this);
        m_pSbxDlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_pSbxDlg, &QObject::destroyed, this, [this]() { m_pSbxDlg = nullptr; });
    }
    m_pSbxDlg->show();
    m_pSbxDlg->raise();
    m_pSbxDlg->activateWindow();
}

void CMainWindow::StartAskServer() {
    // 管道名绑定主窗口进程 PID，多实例互不串扰；removeServer 防上次残留。
    const QString sName = QStringLiteral("AutoCodeMcpAsk-%1").arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(sName);
    m_pAskServer = new QLocalServer(this);
    connect(m_pAskServer, &QLocalServer::newConnection, this, &CMainWindow::OnAskConnection);
    if (!m_pAskServer->listen(sName)) {
        // 监听失败不阻断主程序，仅提问能力不可用（孙进程会回友好提示）。
        m_pAskServer->deleteLater();
        m_pAskServer = nullptr;
    }
}

void CMainWindow::OnAskConnection() {
    if (m_pAskServer == nullptr) {
        return;
    }
    QLocalSocket* pSock = m_pAskServer->nextPendingConnection();
    if (pSock == nullptr) {
        return;
    }
    pSock->setParent(this);
    // 断连清理（排队/在答两种状态都挂）：离队 + 延迟释放；若仍是当前连接则摘下。
    connect(pSock, &QLocalSocket::disconnected, this, [this, pSock]() {
        m_aAskQueue.removeAll(pSock);
        if (m_pAskClient == pSock) {
            m_pAskClient = nullptr;
        }
        pSock->deleteLater();
    });
    // 2026-09-12 串行改排队：多助手并发手工审批可同时到达（claude ask_user 与
    //   dsh/pi/codex/hermes 的 --ask 子进程同桥），先答先问不丢连接。
    // 2026.09.28 弹窗期间（m_pAskClient 已置空）新连接同样入队：旧实现会直通
    //   ActivateAskClient 弹出嵌套第二张模态框，外层连接超时断连即丢答案。
    if (m_pAskClient != nullptr || m_bAskDialogOpen) {
        m_aAskQueue.append(pSock);
        return;
    }
    ActivateAskClient(pSock);
}

void CMainWindow::ActivateAskClient(QLocalSocket* a_pSock) {
    m_pAskClient = a_pSock;
    m_bufAsk.clear();
    connect(m_pAskClient, &QLocalSocket::readyRead, this, &CMainWindow::OnAskReadyRead);
    // 数据可能已到，立即尝试读一次。
    if (m_pAskClient->bytesAvailable() > 0) {
        OnAskReadyRead();
    }
}

void CMainWindow::OnAskReadyRead() {
    if (m_pAskClient == nullptr) {
        return;
    }
    m_bufAsk += m_pAskClient->readAll();
    int iIdx = 0;
    while ((iIdx = m_bufAsk.indexOf('\n')) >= 0) {
        const QByteArray aLine = m_bufAsk.left(iIdx).trimmed();
        m_bufAsk = m_bufAsk.mid(iIdx + 1);
        if (aLine.isEmpty()) {
            continue;
        }
        const QJsonDocument stDoc = QJsonDocument::fromJson(aLine);
        if (!stDoc.isObject()) {
            continue;
        }
        const QJsonObject oAsk = stDoc.object();
        const QString sMsgType = oAsk.value(QStringLiteral("type")).toString();
        // 2026-09-26 内置沙箱命令通道（一连接一命令）：{type:sbx,cmd,args} →
        //   CSandboxManager::Execute → 回 {type:sbxres,ok,text} 后断连。不弹窗不
        //   摘连接，直接在当前连接上应答（与 ask 的模态弹窗不同，毫秒~秒级返回）。
        if (sMsgType == QStringLiteral("sbx")) {
            // 2026-10-01 异步化（修「UI+助手全卡死」）：旧实现在此同步调 Execute——
            //   UIA/截图遇 hung 窗口或输入法钩子死锁时 UI 线程永久阻塞（UIA 的 STA
            //   回调又需 UI 泵消息，循环等待；MeloTTS 实例实测卡死 2h+）。改为入队投
            //   线程池，连接移交队列，结果回包后断连。UI 线程即刻返回继续泵消息。
            const QString sCmd = oAsk.value(QStringLiteral("cmd")).toString();
            const QJsonObject oSbxArgs = oAsk.value(QStringLiteral("args")).toObject();
            QPointer<QLocalSocket> pSbxSock = m_pAskClient;
            m_pAskClient = nullptr;
            m_bufAsk.clear();
            if (!m_aAskQueue.isEmpty()) {
                ActivateAskClient(m_aAskQueue.takeFirst());
            }
            EnqueueSbx(pSbxSock, sCmd, oSbxArgs);
            break;   // 一连接一命令（连接由沙箱队列接管，结果回写后断连）
        }
        if (sMsgType != QStringLiteral("ask")) {
            continue;
        }
        // 解析 question + options
        const QString sQuestion = oAsk.value(QStringLiteral("question")).toString();
        QStringList aOptions;
        const QJsonArray aOpts = oAsk.value(QStringLiteral("options")).toArray();
        for (const QJsonValue& v : aOpts) {
            aOptions.append(v.toString());
        }
        // 弹窗前摘下当前连接（对话框模态期间对端可能超时断连→socket 被延迟释放，
        // 不可再握成员指针；QPointer 在删除后自动置空，防旧版潜在 UAF）。
        QPointer<QLocalSocket> pSock = m_pAskClient;
        m_pAskClient = nullptr;
        m_bufAsk.clear();
        m_bAskDialogOpen = true;   // 模态期门禁：期间到达的新连接在 OnAskConnection 排队
        // 弹窗（模态 exec：阻塞到用户作答，期间对话框可交互）
        CAskUserDialog dlg(sQuestion, aOptions, this);
        // 2026-10-01 对端超时断连（MCP 子进程 600s 上限）自动收框：旧实现对端放弃后
        //   对话框仍无限挂着——模态挡住主窗口，后续 ask 连接排队连环超时；用户
        //   迟到的作答也无处可送。QPointer 防已答后 reject 误用（已关对话框拒收无害）。
        {
            QPointer<CAskUserDialog> pDlg = &dlg;
            connect(pSock, &QLocalSocket::disconnected, this, [pDlg]() {
                if (pDlg != nullptr) {
                    pDlg->reject();
                }
            });
        }
        QJsonObject oReply;
        if (dlg.exec() == QDialog::Accepted && !dlg.AnswerText().isEmpty()) {
            oReply.insert(QStringLiteral("type"), QStringLiteral("answer"));
            oReply.insert(QStringLiteral("text"), dlg.AnswerText());
        } else {
            oReply.insert(QStringLiteral("type"), QStringLiteral("cancel"));
        }
        m_bAskDialogOpen = false;   // 弹窗已关：后续连接可直连激活
        if (pSock != nullptr
            && pSock->state() != QLocalSocket::UnconnectedState) {
            const QByteArray aBytes = QJsonDocument(oReply).toJson(QJsonDocument::Compact);
            pSock->write(aBytes + "\n");
            pSock->flush();
            pSock->disconnectFromServer();
        }
        // 队列里有排队提问 → 立即处理下一个
        if (!m_aAskQueue.isEmpty()) {
            ActivateAskClient(m_aAskQueue.takeFirst());
        }
        break;   // 一连接一问
    }
}

// ---------------------------------------------------------------------------
// 2026-10-01 沙箱命令异步化：入队 → 线程池（MTA COM）执行 → queued 回 UI 写包。
// 队首即在飞；看门狗 150s 兑底（对端 MCP/CLI 均有 180s 上限，保证总有回包）。
// ---------------------------------------------------------------------------
void CMainWindow::EnqueueSbx(QLocalSocket* a_pSock, const QString& a_sCmd,
                             const QJsonObject& a_oArgs) {
    SSbxReq stReq;
    stReq.iSeq = ++m_iSbxSeq;
    stReq.pSock = a_pSock;
    stReq.iT0Ms = QDateTime::currentMSecsSinceEpoch();
    stReq.sCmd = a_sCmd;
    stReq.oArgs = a_oArgs;
    if (m_pSbx == nullptr) {
        // 主实例未初始化：直接回包，不占队列。
        QJsonObject oReply;
        oReply.insert(QStringLiteral("type"), QStringLiteral("sbxres"));
        oReply.insert(QStringLiteral("ok"), false);
        oReply.insert(QStringLiteral("text"), QStringLiteral("[沙箱不可用：主实例未初始化]"));
        if (a_pSock != nullptr && a_pSock->state() != QLocalSocket::UnconnectedState) {
            a_pSock->write(QJsonDocument(oReply).toJson(QJsonDocument::Compact) + "\n");
            a_pSock->flush();
            a_pSock->disconnectFromServer();
        }
        return;
    }
    m_qSbx.append(stReq);
    if (m_pSbxWd == nullptr) {
        m_pSbxWd = new QTimer(this);
        m_pSbxWd->setInterval(5000);
        connect(m_pSbxWd, &QTimer::timeout, this, &CMainWindow::OnSbxWatchdog);
    }
    m_pSbxWd->start();
    DispatchSbx();
}

void CMainWindow::DispatchSbx() {
    if (m_pSbx == nullptr) {
        return;
    }
    // 2026-10-01 并行分发：每请求各投一池线程（叶子操作无锁化后，CSandboxManager
    //   的 m_mtxState 只在状态段串行，互不饿死）；看门狗兑底 150s 回包。
    for (int i = 0; i < m_qSbx.size(); ++i) {
        if (m_qSbx.at(i).iT0Ms > 0) {   // 正 T0 = 未投出
            const qint64 iSeq = m_qSbx[i].iSeq;
            const QString sCmd = m_qSbx[i].sCmd;
            const QJsonObject oArgs = m_qSbx[i].oArgs;
            m_qSbx[i].iT0Ms = -m_qSbx[i].iT0Ms;   // 置负标记已投池
            // 线程池 + MTA COM：UIA 客户端在 MTA 下无 STA 消息泵依赖，消除
            // 「UI 线程泵不动 → UIA 回调卡死」的循环等待路径。
            QThreadPool::globalInstance()->start([this, iSeq, sCmd, oArgs]() {
                const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                QString sText;
                const bool bOk = m_pSbx->Execute(sCmd, oArgs, sText);
                if (SUCCEEDED(hrCom)) {
                    CoUninitialize();
                }
                // 回 UI 线程写包（窗口已析构则安全丢弃）。
                QMetaObject::invokeMethod(this,
                    [this, iSeq, bOk, sText]() { OnSbxDone(iSeq, bOk, sText); },
                    Qt::QueuedConnection);
            });
        }
    }
}

void CMainWindow::OnSbxDone(qint64 a_iSeq, bool a_bOk, const QString& a_sText) {
    FinishSbx(a_iSeq, a_bOk, a_sText);
    DispatchSbx();
}

void CMainWindow::FinishSbx(qint64 a_iSeq, bool a_bOk, const QString& a_sText) {
    const int iIdx = static_cast<int>(
        std::find_if(m_qSbx.cbegin(), m_qSbx.cend(),
                     [a_iSeq](const SSbxReq& st) { return st.iSeq == a_iSeq; })
        - m_qSbx.cbegin());
    if (iIdx >= m_qSbx.size()) {
        return;   // 已被看门狗摘除（超时回包过）——丢弃迟到结果
    }
    const SSbxReq stReq = m_qSbx.takeAt(iIdx);
    QJsonObject oReply;
    oReply.insert(QStringLiteral("type"), QStringLiteral("sbxres"));
    oReply.insert(QStringLiteral("ok"), a_bOk);
    oReply.insert(QStringLiteral("text"), a_sText);
    if (stReq.pSock != nullptr && stReq.pSock->state() != QLocalSocket::UnconnectedState) {
        stReq.pSock->write(QJsonDocument(oReply).toJson(QJsonDocument::Compact) + "\n");
        stReq.pSock->flush();
        stReq.pSock->disconnectFromServer();
    }
}

void CMainWindow::OnSbxWatchdog() {
    if (m_qSbx.isEmpty()) {
        if (m_pSbxWd != nullptr) {
            m_pSbxWd->stop();
        }
        return;
    }
    const qint64 iNow = QDateTime::currentMSecsSinceEpoch();
    const qint64 iLimitMs = 150000;   // < MCP/CLI 对端 180s 等待上限
    for (int i = m_qSbx.size() - 1; i >= 0; --i) {
        const qint64 iT0 = qAbs(m_qSbx.at(i).iT0Ms);   // 负值=已投池在飞
        if (iNow - iT0 > iLimitMs) {
            const qint64 iSeq = m_qSbx.at(i).iSeq;
            qWarning("[SbxWd] 命令 %lld (%s) 超时 150s，回错误包（池线程若仍 hung 则泄漏）",
                     iSeq, qPrintable(m_qSbx.at(i).sCmd));
            FinishSbx(iSeq, false,
                      QStringLiteral("[沙箱命令超时：目标窗口无响应或 UIA 阻塞，建议检查被测程序后重试]"));
        }
    }
    if (m_qSbx.isEmpty() && m_pSbxWd != nullptr) {
        m_pSbxWd->stop();
    }
}

void CMainWindow::OnAdhocHealthCheck() {
    // 2026-10-02 孤儿 busy 看门狗：面板 bRunning=true 但编排器侧会话不存在/runner
    //   无活性（进程已终、无宽限/重试）＝结束信号在某一环节丢失（实测：档切换竞态
    //   丢 OnAdhocFinished；防御其他未知漏发路径）——连续两次不健康即自动恢复输入栏
    //   并记事件。正常链路全程同线程同步，不存在合法的「面板 running 而会话无活性」
    //   空档，二次确认仅兑底极端调度场景。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr) {
        return;
    }
    const QStringList aRunning = m_pExecPanel->RunningAsstIds();
    for (const QString& sId : aRunning) {
        if (m_pOrch->IsAsstAdhocRunning(sId)) {
            m_aAdhocSuspect.remove(sId);
            continue;
        }
        if (!m_aAdhocSuspect.contains(sId)) {
            m_aAdhocSuspect.insert(sId);   // 首次不健康：待下轮复确认
            continue;
        }
        m_aAdhocSuspect.remove(sId);
        m_pExecPanel->SetAsstRunning(sId, false);
        // 2026-10-09 配套解除命名挂起（bSendAfterAutoName）并退回暂存指令：否则首条
        //   「先改名后执行」链断裂后，后续发送永远只暂存不提交（另机实测死锁）。
        m_pExecPanel->AdhocOrphanHeal(sId);
        m_pExecPanel->OnAdhocEvent(sId, QStringLiteral("self_heal"),
            QStringLiteral("⚠ 检测到助手运行态孤儿（进程已结束但结束信号丢失，可能因后端启动失败/档切换竞态），"
                           "已自动恢复输入栏；若刚才的工作未完成请重发指令"));
    }
    // 疑点表里已不在运行列表的 id 清掉（助手被删/档切换重建）
    for (auto it = m_aAdhocSuspect.begin(); it != m_aAdhocSuspect.end();) {
        if (!aRunning.contains(*it)) {
            it = m_aAdhocSuspect.erase(it);
        } else {
            ++it;
        }
    }
}

void CMainWindow::OnReviveTask(CTaskInfo* a_pTask) {
    // 历史任务「再次执行」（#7）：Done 从头重跑，Failed/Stopped 从当前阶段续跑。
    if (a_pTask == nullptr || m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return;
    }
    const QString sId = a_pTask->Id();

    // 1) 优先复用当前工程的活跃副本
    CTaskInfo* pTask = nullptr;
    for (CTaskInfo* p : m_aTasks) {
        if (p != nullptr && p->Id() == sId) {
            pTask = p;
            break;
        }
    }
    // 2) 历史副本（CTaskInfo 继承 QObject，禁拷贝）→ 经 task.json 往返重建为活跃副本
    if (pTask == nullptr) {
        m_pPm->InitTaskDir(sId);
        const QString sJson = m_pPm->TaskDir(sId) + QStringLiteral("/task.json");
        if (!a_pTask->SaveToFile(sJson)) {
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("再次执行"),
                QStringLiteral("无法写入任务数据，再次执行失败"), this);
            return;
        }
        pTask = new CTaskInfo(this);
        if (!pTask->LoadFromFile(sJson)) {
            delete pTask;   // 加载失败：立即回收，避免悬垂
            ExecMsgBox(QMessageBox::Warning, QStringLiteral("再次执行"),
                QStringLiteral("任务数据损坏，再次执行失败"), this);
            return;
        }
        pTask->SetProjectDir(m_pPm->ProjectDir());
        connect(pTask, &CTaskInfo::sigChanged, this, [this, pTask]() { SaveTask(pTask); });
        m_aTasks.append(pTask);
    }

    // Done 任务：ResetForRevive 回到 Intake 从头跑（否则末阶段续跑会立即又 Done，无可见反应）；
    // Failed/Stopped 任务：仅清运行态标志，从当前阶段续跑。
    if (pTask->Status() == ETaskStatus::Done) {
        pTask->ResetForRevive();
    } else {
        pTask->SetLastError(QString());
        pTask->ResetResumeCount();
        pTask->SetLastSessionId(QString());
    }
    pTask->SetStopRequested(false);
    pTask->SetPauseRequested(false);
    pTask->SetArchived(false);   // 2026.08.22 回到当前表，清归档标记（随 sigChanged 落盘）
    pTask->SetStatus(ETaskStatus::Stopped);
    // 2026.08.21 历史任务再次执行不改名：任务已有正式名称，清自动命名标记，
    //   避免 Done 重跑回 Intake 后又触发 Agent 起名覆盖既有名称。
    pTask->SetAutoNamed(false);

    // 先选中并显示该任务（日志/阶段表立即就位），再启动——确保随后的 stage_start
    // 等事件能上屏，避免「启动了却看不到」(#7)。
    // SwitchAssistantArchive 须在 SelectTask 之前（其触发的 OnTaskSelected 因 key 相等早退）。
    // 2026.09.10 v2.2.153 直接切「当前任务」页并选中：选中即自动显示任务日志（载任务=
    //   任务日志的统一口径在 SetCurrentTask），不再停在助手日志。
    SwitchAssistantArchive(pTask);
    m_pTaskPanel->SetTasks(m_aTasks);
    m_pTaskPanel->SelectTask(pTask);
    m_pExecPanel->SetCurrentTask(pTask);   // SelectTask 未命中行时的兑底（正常路径重复 no-op）
    m_pOrch->StartTask(pTask);
    ReloadHistory();
}

void CMainWindow::SaveTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return;
    }
    m_pPm->InitTaskDir(a_pTask->Id());   // 确保任务目录存在
    a_pTask->SaveToFile(m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/task.json"));
    // 镜像写到本工程历史库（task.json 保留供阶段恢复，DB 供历史页展示）
    if (m_pHistoryDb) {
        m_pHistoryDb->Upsert(a_pTask);
    }
}

void CMainWindow::ReloadHistory() {
    // 2026.09.10 v2.2.153 选中保链：当前选中若指向历史副本（任务终态自动选中后的常态），
    //   旧副本随下方整批 delete 失效——记下 id，断档重建后按 id 回接新副本（m_pCur/
    //   面板选中/任务日志不悬垂不丢；重命名/移除/导入/另任务终态等所有 ReloadHistory
    //   路径统一受保护）。选中若是活跃副本则不受本函数影响（活跃副本不在此回收）。
    const QString sSelId = (m_pCur != nullptr && !m_aTasks.contains(m_pCur))
                               ? m_pCur->Id() : QString();
    // 释放上一批 DB 副本 → 重新从当前工程库加载（最新在上）。
    for (CTaskInfo* pTask : m_aHistoryTasks) {
        delete pTask;
    }
    m_aHistoryTasks.clear();
    if (m_pHistoryDb) {
        m_aHistoryTasks = m_pHistoryDb->LoadAll();
    }
    // 过滤：仍在活跃列表(m_aTasks)的任务不进历史表，避免当前表/历史表重复。
    // 历史表只收「已移出活跃列表」的任务：终态(Done/Failed) + 被「移除」归档的 Stopped 等。
    QSet<QString> setActiveIds;
    for (CTaskInfo* pTask : m_aTasks) {
        if (pTask != nullptr) {
            setActiveIds.insert(pTask->Id());
        }
    }
    QList<CTaskInfo*> aFiltered;
    for (CTaskInfo* pTask : m_aHistoryTasks) {
        if (pTask == nullptr) { continue; }
        if (setActiveIds.contains(pTask->Id())) { continue; }   // 活跃任务跳过
        aFiltered.append(pTask);
    }
    if (m_pTaskPanel) {
        m_pTaskPanel->SetHistoryTasks(aFiltered);
    }
    // 按 id 回接：新历史批次找回同 id 副本，指针回接 + 面板选中/日志就位（OnTaskSelected
    //   内 SwitchAssistantArchive 因 key 相等早退，助手档零扰动）。任务被删的路径
    //   （OnDeleteTask）已在删除前清好选中，不会命中丢不到的情形；万一丢不到（异常态）
    //   兑底置空，绝不留悬垂指针。
    if (!sSelId.isEmpty()) {
        CTaskInfo* pRebind = nullptr;
        for (CTaskInfo* p : m_aHistoryTasks) {
            if (p != nullptr && p->Id() == sSelId) { pRebind = p; break; }
        }
        if (pRebind != nullptr) {
            m_pCur = pRebind;   // 同 id 指针回接（非切档，key 不变）
            if (m_pTaskPanel) {
                m_pTaskPanel->SelectHistoryTask(pRebind);
            }
        } else {
            m_pCur = nullptr;
        }
    }
}

// —— 助手会话状态持久化（#1）：多助手 v2 档 ↔ .autodev/assistant.json（或 tasks/<id>/）——
void CMainWindow::SaveAssistantState(bool a_bForce) {
    if (m_bProjectSwitching && !a_bForce) {
        return;   // 2026-10-04 切工程隔离窗：m_pPm 已指向新工程而面板可能仍持旧工程
                  //   助手，此刻任何落盘都会把旧工程助手整版写进新工程 assistant.json
                  //   （BigModel 实害）。唯一放行 = OpenProjectDir 的显式终快照。
    }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty() ||
        m_pOrch == nullptr || m_pExecPanel == nullptr) {
        return;
    }
    // 2026-10-02 分组落盘（跨档幽灵配套）：面板助手按 sOwnerKey 分组——
    //   · 视图档（owner == m_pExecPanel->ViewKey()）全量覆盖（面板持有该档完整集合，
    //     增删改以面板为准）；
    //   · 外档（owner ≠ 视图 key，只含运行中幽灵）合并覆盖：读该档现文件，按 id 用
    //     面板幽灵的最新块/会话链覆写同名项，其余 id 原样保留（不丢该档未载入的助手）。
    //   每组独立写自己的档文件；无该组面板成员则不写（未动过的档保持原样）。
    auto fnWriteGroup = [this](const QString& a_sKey, const QJsonArray& a_aGroup) {
        if (a_aGroup.isEmpty()) {
            return;
        }
        QJsonArray aFinal = a_aGroup;
        QString sActive;
        if (a_sKey == m_pExecPanel->ViewKey()) {
            // 2026-10-19 v4.4.390 防抹守卫（周报工程实害：面板重置态的默认空助手
            //   随一次落盘把 231 块实档整版覆盖成 311 字节空档，对话不可再生——
            //   ResetAssistants/ClearAllBlocks→默认助手 + 其后任意 layout 落盘即触发）。
            //   判据：待写视图档 =「仅一个从未使用过的默认助手」（0 块 + 未绑后端 +
            //   缺省名），而盘上现档任一助手带对话块 → 判定重置态误写，跳过本次落盘
            //   （数据保住；合法"删光助手"流程同样会被拦——重启复活最后一份档案，
            //   相比整档蒸发可接受）。备份防线见 RotateStateBackup（.bak/.bak2 两代）。
            //   2026-10-19 v4.4.393 加固（HotNews 实害：双实例并发下 7.5MB 实档连烧
            //   .bak/.bak2 两代）：①重置判据从「仅一个」放宽为「全部为未用过的默认
            //   助手」（重置态可能带多个空助手）；②守卫核对旧档时同样会撞上他实例
            //   并发写的共享冲突——旧档在场却读不出/解析失败必须 fail-closed 拒绝
            //   覆盖，宁可少写一次，不可在无法核对旧档时盲目整版覆盖。
            auto fnTrivialDefault = [](const QJsonArray& a_a) {
                if (a_a.isEmpty()) {
                    return false;
                }
                for (const QJsonValue& v : a_a) {
                    const QJsonObject o = v.toObject();
                    if (!o.value(QStringLiteral("blocks")).toArray().isEmpty()
                            || !o.value(QStringLiteral("sid")).toString().isEmpty()
                            || !o.value(QStringLiteral("backend")).toString().isEmpty()
                            || !o.value(QStringLiteral("name")).toString()
                                    .startsWith(QStringLiteral("助手"))) {
                        return false;
                    }
                }
                return true;
            };
            auto fnHasBlocks = [](const QJsonArray& a_a) {
                for (const QJsonValue& v : a_a) {
                    if (!v.toObject().value(QStringLiteral("blocks")).toArray().isEmpty()) {
                        return true;
                    }
                }
                return false;
            };
            if (fnTrivialDefault(aFinal)) {
                const QString sOldFile = a_sKey.isEmpty()
                    ? m_pPm->AssistantStateFile()
                    : m_pPm->TaskAssistantFile(a_sKey);
                const QByteArray aOldBytes = a_sKey.isEmpty()
                    ? m_pPm->ReadAssistantState()
                    : m_pPm->ReadTaskAssistantState(a_sKey);
                const QJsonDocument stOld = QJsonDocument::fromJson(aOldBytes);
                const bool bOldHasBlocks = stOld.isObject()
                        && fnHasBlocks(stOld.object()
                                  .value(QStringLiteral("assistants")).toArray());
                const QFileInfo stOldInfo(sOldFile);
                const bool bOldUnreadable = stOldInfo.exists() && stOldInfo.size() > 0
                        && (aOldBytes.isEmpty() || !stOld.isObject());
                if (bOldHasBlocks || bOldUnreadable) {
                    qWarning() << "[AssistantState] 防抹守卫拦截：默认空助手整版覆盖实档（key="
                               << a_sKey << "，旧档含对话块=" << bOldHasBlocks
                               << "，旧档在场读不出=" << bOldUnreadable << "），本次落盘跳过";
                    return;
                }
            }
            sActive = m_pExecPanel->ActiveAsstId();   // 本档：当前激活助手
        } else {
            // 外档：合并盘上现档（面板幽灵覆写同名 id，其余保留）；
            //   active 取盘上值（幽灵档的激活态无意义），无盘档回退首个。
            const QByteArray aOld = a_sKey.isEmpty()
                ? m_pPm->ReadAssistantState()
                : m_pPm->ReadTaskAssistantState(a_sKey);
            QJsonArray aMerged;
            QJsonObject oOldRoot;
            const QJsonDocument stOld = QJsonDocument::fromJson(aOld);
            if (stOld.isObject()) {
                oOldRoot = stOld.object();
                sActive = oOldRoot.value(QStringLiteral("active")).toString();
            }
            QHash<QString, bool> aNewIds;
            for (const QJsonValue& v : aFinal) {
                aNewIds.insert(v.toObject().value(QStringLiteral("id")).toString(), true);
            }
            if (oOldRoot.contains(QStringLiteral("assistants"))) {
                for (const QJsonValue& v : oOldRoot.value(QStringLiteral("assistants")).toArray()) {
                    const QString sId = v.toObject().value(QStringLiteral("id")).toString();
                    if (!aNewIds.contains(sId)) {
                        aMerged.append(v);   // 盘上其余 id 保留
                    }
                }
            }
            aMerged = aFinal + aMerged;   // 面板幽灵在前（覆写版优先）
            aFinal = aMerged;
            if (sActive.isEmpty()) {
                sActive = aFinal.at(0).toObject().value(QStringLiteral("id")).toString();
            }
        }
        QJsonObject oRoot;
        oRoot[QStringLiteral("version")]     = 2;
        oRoot[QStringLiteral("backend")]     = CAgentRunnerFactory::BackendName();
        oRoot[QStringLiteral("active")]      = sActive;
        oRoot[QStringLiteral("assistants")]  = aFinal;
        const QByteArray aData = QJsonDocument(oRoot).toJson(QJsonDocument::Compact);
        if (a_sKey.isEmpty()) {
            m_pPm->WriteAssistantState(aData);
        } else {
            m_pPm->WriteTaskAssistantState(a_sKey, aData);
        }
    };

    // 会话现值回填（全量探测：本档 + 幽灵；SyncOrchToPanel 按 id 幂等）。
    const QList<CExecutionPanel::SAsst> aSnapProbe = m_pExecPanel->AllAsstsSnapshot();
    for (const CExecutionPanel::SAsst& st : aSnapProbe) {
        SyncOrchToPanel(st.sId);
        m_pExecPanel->UpdateAsstBackend(st.sId, m_pOrch->AsstAdhocBackend(st.sId));
    }

    // 视图档（全量覆盖）
    fnWriteGroup(m_pExecPanel->ViewKey(), m_pExecPanel->AssistantsToJson());
    // 外档幽灵（各归各档，合并覆盖）
    QSet<QString> aDone;
    for (const CExecutionPanel::SAsst& st : aSnapProbe) {
        const QString sKey = st.sOwnerKey;
        if (sKey != m_pExecPanel->ViewKey() && !aDone.contains(sKey)) {
            aDone.insert(sKey);
            fnWriteGroup(sKey, m_pExecPanel->AssistantsJsonOf(sKey));
        }
    }
}

void CMainWindow::LoadAssistantState(bool a_bProjectSwitch) {
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty() ||
        m_pOrch == nullptr || m_pExecPanel == nullptr) {
        return;
    }
    // 2026-10-02 跨档幽灵：视图 key 跟 m_pCur（载入档）；档重建前摘下全部运行中助手
    //   （跨档幽灵 + 本档运行者——切回归属档也不能杀在跑轮次，v3.2.259 E2E 实测误杀），
    //   载完挂回：外档者粗体 Tab 继续跨档可见，本档者转正常。
    //   2026-10-04 a_bProjectSwitch（切工程）：不摘不挂——外工程助手跟到新工程只会以
    //   空 ownerKey 冒充本地助手落盘串档；会话已由 OpenProjectDir 提前整组释放，
    //   面板随后整体重建，无需保留任何运行中 Tab。
    m_pExecPanel->SetViewKey(m_pCur != nullptr ? m_pCur->Id() : QString());
    const QString sOldTaskName = (m_pCur != nullptr) ? m_pCur->Name() : QString();
    const QList<CExecutionPanel::SAsst> aPhantoms = a_bProjectSwitch
        ? QList<CExecutionPanel::SAsst>()
        : m_pExecPanel->TakeRunningForReload();
    QSet<QString> aKeepIds;
    for (const CExecutionPanel::SAsst& st : aPhantoms) {
        aKeepIds.insert(st.sId);
    }
    // 助手档按任务分档：选中任务读其私有档，否则读工程全局档（与 SaveAssistantState 对称）。
    const bool bTaskKey = (m_pCur != nullptr);
    QByteArray aData = bTaskKey ? m_pPm->ReadTaskAssistantState(m_pCur->Id())
                                : m_pPm->ReadAssistantState();
    // 2026-10-19 v4.4.393 读失败≠无档：他实例并发写档案（WriteOnly|Truncate 相位）
    //   的共享冲突是瞬态的——空读且文件在场时小睡重读一次（40ms，UI 线程既有阻塞
    //   惯例内），仍读不出才按无档处理。HotNews 7.5MB 实害链第一环正是「读瞬态
    //   失败→ResetAssistants→防抖落盘默认空助手」；防抹守卫兜底之外，此处掐断
    //   触发源头（重置面板的诱因本身就不该发生）。
    if (aData.isEmpty()) {
        const QString sStateFile = bTaskKey ? m_pPm->TaskAssistantFile(m_pCur->Id())
                                            : m_pPm->AssistantStateFile();
        const QFileInfo stStateInfo(sStateFile);
        if (stStateInfo.exists() && stStateInfo.size() > 0) {
            QThread::msleep(40);
            aData = bTaskKey ? m_pPm->ReadTaskAssistantState(m_pCur->Id())
                             : m_pPm->ReadAssistantState();
            if (aData.isEmpty()) {
                qWarning() << "[AssistantState] 档案在场却读不出（他实例并发写？）：" << sStateFile;
            }
        }
    }
    if (aData.isEmpty()) {
        // 无存档（该任务还没用过助手 / 工程首次打开）：显式清空当前助手态。
        //   不能直接 return——否则上一档（别的任务或全局）的会话链/对话块会串进这一档，
        //   且随后的变更保存会把串来的内容写进本档文件。
        m_pOrch->ResetAdhocSessions(aKeepIds);   // 空档清会话（幽灵会话保留）
        m_pExecPanel->ResetAssistants();
        m_pExecPanel->SetAdhocContext(QString(), 0);   // 激活助手复位后其快照归零
        if (!aPhantoms.isEmpty()) {
            m_pExecPanel->AttachForeignAssistants(aPhantoms, sOldTaskName);
        }
        return;
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(aData);
    if (!oDoc.isObject()) {
        // 2026-10-19 v4.4.393 非空却解析失败（读到半截/损坏）：原先裸 return 会让
        //   面板残留上一档内容，随后的落盘把旧档对话串写进本档文件（跨档串档）。
        //   改按无档同路清空面板；盘上损坏档案由防抹守卫拦住默认态覆盖，待面板
        //   产生真实内容再落盘时自然修复。
        qWarning() << "[AssistantState] 档案解析失败，按无档处理（bytes="
                   << aData.size() << "）";
        m_pOrch->ResetAdhocSessions(aKeepIds);
        m_pExecPanel->ResetAssistants();
        m_pExecPanel->SetAdhocContext(QString(), 0);
        if (!aPhantoms.isEmpty()) {
            m_pExecPanel->AttachForeignAssistants(aPhantoms, sOldTaskName);
        }
        return;
    }
    const QJsonObject oRoot = oDoc.object();
    // 2026.09.20 v2.4.193 粘性后端（取代旧「后端不符则清 sid/撤销栈」守卫）：逐助手
    //   读 per-assistant "backend" 键（旧档无此键 → 回退档根 backend 键），随会话绑定
    //   （LoadOrchFromPanel → LoadAsstAdhoc）——助手继续用它当时的 Harness，与当前
    //   全局设置无关；未绑定（无键且根也无）的助手首次提交时才绑当前全局。
    const QString sBackend = oRoot.value(QStringLiteral("backend")).toString();
    if (oRoot.contains(QStringLiteral("assistants"))) {
        // v2 多助手档：逐助手恢复（粘性后端：无 per-assistant 键的旧助手回退档根
        //   backend——即它们当时的后端；当前全局设置不影响已存档的助手绑定）。
        QJsonArray aAssts = oRoot.value(QStringLiteral("assistants")).toArray();
        for (QJsonValueRef v : aAssts) {
            if (v.isObject()) {
                QJsonObject o = v.toObject();
                if (!o.contains(QStringLiteral("backend"))) {
                    o[QStringLiteral("backend")] = sBackend;   // 旧档回退档根后端
                }
                v = o;
            }
        }
        m_pExecPanel->LoadAssistantsFromJson(
            aAssts, oRoot.value(QStringLiteral("active")).toString());
    } else {
        // 旧版单助手档 → 兼容为单助手「助手」（旧任务打开后第一个助手即显示历史日志）。
        // 2026.09.20 v2.4.193 粘性后端：旧守卫（后端不符则丢 sid/stack）取消——助手
        //   绑定档根 backend（它当时的 Harness），sid/stack 全量保留。
        const QString sSid = oRoot.value(QStringLiteral("sid")).toString();
        const QJsonArray aStack = oRoot.value(QStringLiteral("stack")).toArray();
        QJsonObject oOne;
        oOne[QStringLiteral("name")]      = QStringLiteral("助手");
        oOne[QStringLiteral("backend")]   = sBackend;   // 粘性绑定档根后端
        oOne[QStringLiteral("sid")]       = sSid;
        oOne[QStringLiteral("guideHash")] = oRoot.value(QStringLiteral("guideHash")).toString();
        oOne[QStringLiteral("stack")]     = aStack;
        oOne[QStringLiteral("blocks")]    = oRoot.value(QStringLiteral("blocks")).toArray();
        // #1 上下文占用恢复：新版有 ctxFill 直接还原；旧档无此字段但有 sid → 未知（—）。
        oOne[QStringLiteral("ctxFill")] = oRoot.contains(QStringLiteral("ctxFill"))
            ? oRoot.value(QStringLiteral("ctxFill")).toDouble()
            : (sSid.isEmpty() ? 0.0 : -1.0);
        QJsonArray aOne;
        aOne.append(oOne);
        m_pExecPanel->LoadAssistantsFromJson(aOne, QString());
    }
    // 统一收尾：2026.09.03 多助手并发——全部助手的会话链装进编排器（per-助手 常驻），
    //   进度条按激活助手快照还原。
    const QString sAct = m_pExecPanel->ActiveAsstId();
    m_pOrch->ResetAdhocSessions(aKeepIds);   // 先清旧档会话（含 runner，防档串；运行中全保留）
    if (!aKeepIds.isEmpty()) {
        m_pExecPanel->DropAsstEntries(QStringList(aKeepIds.begin(), aKeepIds.end()));   // 保留 id 的文件版让位
    }
    const QJsonArray aAll = m_pExecPanel->AssistantsToJson();
    for (const QJsonValue& v : aAll) {
        if (!v.isObject()) { continue; }
        const QJsonObject o = v.toObject();
        const QString sId = o.value(QStringLiteral("id")).toString();
        if (sId.isEmpty() || aKeepIds.contains(sId)) { continue; }   // 保留 id：会话活着，不覆盖
        m_pOrch->LoadAsstAdhoc(sId,
                               o.value(QStringLiteral("sid")).toString(),
                               o.value(QStringLiteral("stack")).toArray(),
                               o.value(QStringLiteral("guideHash")).toString(),
                               o.value(QStringLiteral("backend")).toString());   // 2026.09.20 粘性后端
    }
    if (!sAct.isEmpty()) {
        m_pExecPanel->SetAdhocContext(sAct, m_pExecPanel->AsstCtxFill(sAct));
    }
    if (!aPhantoms.isEmpty()) {
        m_pExecPanel->AttachForeignAssistants(aPhantoms, sOldTaskName);
    }
    // 2026-10-15 模型恢复后同步上下文窗口（用户反馈：重启后进度条仍显 128K，右键
    //   重选模型才对）：助手档案不存模型名，恢复后 sModel 为空/旧值，而共享表推送
    //   只在构造期跑过一次（当时面板尚未创建被跳过）。此处助手已全部就位，补一次
    //   推送 → SetSharedModels 把非覆盖助手 sModel 归一到激活条目并全量刷新进度条，
    //   分母即条目 iCtx（ContextWindowForModel 条目优先）。幂等，切工程/切任务同链。
    ApplySharedModelsToRunners();
}

void CMainWindow::SyncOrchToPanel(const QString& a_sAsstId) {
    // 2026.09.03 多助手并发：编排器会话 per-助手 常驻——按 id 拉现值回填面板镜像
    //   （SAsst 的 sid/栈/guideHash 仅作持久化快照，运行权威在编排器）。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr || a_sAsstId.isEmpty()) {
        return;
    }
    m_pExecPanel->UpdateAsstSession(a_sAsstId, m_pOrch->AsstAdhocSid(a_sAsstId),
                                    m_pOrch->AsstAdhocGuideHash(a_sAsstId),
                                    m_pOrch->AsstAdhocStackToJson(a_sAsstId));
}

void CMainWindow::LoadOrchFromPanel(const QString& a_sAsstId) {
    // 2026.09.03 多助手并发：会话常驻编排器，切换不再换载。此函数仅在档恢复（面板
    //   先于编排器建好助手模型）时把面板镜像装进编排器——LoadAssistantState 调用。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr || a_sAsstId.isEmpty()) {
        return;
    }
    m_pOrch->LoadAsstAdhoc(a_sAsstId, m_pExecPanel->AsstSid(a_sAsstId),
                           m_pExecPanel->AsstStack(a_sAsstId),
                           m_pExecPanel->AsstGuideHash(a_sAsstId),
                           m_pExecPanel->AsstBackend(a_sAsstId));   // 2026.09.20 粘性后端
    // 2026-10-07 对话级模型覆盖随档恢复（面板是持久化正本，会话侧存快照）。
    const QPair<SDshModelEntry, bool> stOv = m_pExecPanel->AsstModelOverride(a_sAsstId);
    m_pOrch->SetAsstModelOverride(a_sAsstId, stOv.first, stOv.second);
}

void CMainWindow::OnAsstSwitched(const QString& a_sOldId, const QString& a_sNewId) {
    // 2026.09.03 多助手并发：会话 per-助手 常驻编排器，切 Tab 不再换载——仅把旧助手
    //   的编排器现值同步回面板镜像（供随时落盘），新助手无需装载（其会话一直在）。
    Q_UNUSED(a_sNewId);
    if (m_pOrch == nullptr || m_pExecPanel == nullptr || a_sOldId == a_sNewId) {
        return;
    }
    SyncOrchToPanel(a_sOldId);
}

void CMainWindow::OnAsstAdded(const QString& a_sId) {
    Q_UNUSED(a_sId);
    SaveAssistantState();   // 新助手入档（其会话为空，首条指令前不投喂 guide 之外的旧状态）
}

void CMainWindow::OnAsstRemoved(const QString& a_sId, const QString& a_sNewActive) {
    Q_UNUSED(a_sNewActive);
    // 2026.09.09 彻底删除（右键菜单）：编排器侧该助手会话（在跑 runner/sid/撤销栈/
    //   回灌缓存）随面板一并释放（RemoveAsstAdhoc 内含兑底停），随后落盘——被删者
    //   已不在面板列表，assistant.json 自此不含它，重启不会复活。
    if (m_pOrch != nullptr && !a_sId.isEmpty()) {
        m_pOrch->RemoveAsstAdhoc(a_sId);
    }
    SaveAssistantState();
}

void CMainWindow::OnAsstForeignActivated(const QString& a_sOwnerKey) {
    // 2026-10-02 点击外档幽灵 Tab：切回归属（任务选中/取消到工程档）。
    //   SwitchAssistantArchive 同档早退保证幂等（重复点击/重建期重入无循环）。
    if (m_pExecPanel == nullptr || m_pTaskPanel == nullptr) {
        return;
    }
    CTaskInfo* pTarget = nullptr;
    if (!a_sOwnerKey.isEmpty()) {
        for (CTaskInfo* p : m_aTasks) {
            if (p != nullptr && p->Id() == a_sOwnerKey) {
                pTarget = p;
                break;
            }
        }
    }
    if (pTarget != nullptr) {
        m_pTaskPanel->SelectTask(pTarget);   // 内部 emit sigTaskSelected → OnTaskSelected 全链
    } else {
        // 归属工程档（或任务已删）：取消选中回工程档（面板空白点击同链）。
        m_pTaskPanel->DeselectAll();
    }
}

void CMainWindow::OnAsstForeignFinished(const QString& a_sAsstId) {
    // 2026-10-02 外档幽灵轮次结束：最终块（含 done/收尾事件）落其归属档 → 面板除名 →
    //   编排器会话释放（后续该任务再选中时经 LoadAsstAdhoc 从档恢复会话链）。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr) {
        return;
    }
    SyncOrchToPanel(a_sAsstId);
    SaveAssistantState();   // 分组落盘：该幽灵最新块随其档合并写入
    m_pExecPanel->RemoveAsstEntry(a_sAsstId);
    m_pOrch->RemoveAsstAdhoc(a_sAsstId);
}

void CMainWindow::OnAdhocRunFinished(const QString& a_sAsstId) {
    // 2026.09.03 多助手并发：该助手一轮结束——编排器会话现值回填其面板镜像（供落盘）。
    //   面板运行标记由其后连接的 OnAdhocFinished(id) 清除；无换载步骤。
    if (m_pOrch == nullptr || m_pExecPanel == nullptr || a_sAsstId.isEmpty()) {
        return;
    }
    SyncOrchToPanel(a_sAsstId);
    // 2026-10-02 v3.0.220 挂起切换机制随「跨档幽灵」移除（档切换即时完成，无收尾补切）。
    //   外档幽灵的收尾（最终块落其档 + 除名）走面板 sigAsstForeignFinished（OnAsstForeignFinished）。
}

void CMainWindow::SwitchAssistantArchive(CTaskInfo* a_pNewTask) {
    // 助手档切换原语（跟着任务走）：旧档落盘 → 切 m_pCur → 恢复新档。
    //   所有改变 m_pCur 的路径必须经此函数（而非裸赋值），否则旧档永不落盘、
    //   且 OnTaskSelected 里按 m_pCur 推导的存档 key 会把 A 任务的对话写进 B 任务的档。
    //   2026-10-02 跨档幽灵（取代 v3.0.220 挂起切换）：运行中助手不再阻塞档切换——
    //   旧档（含运行助手最新块）落盘后立即切新档，运行中助手以「外档幽灵」保留在面板
    //   （粗体 Tab + 任务名 tooltip、输入禁用、点击切回），轮次继续跑、块持续落其档。
    const QString sOldKey = (m_pCur != nullptr) ? m_pCur->Id() : QString();
    const QString sNewKey = (a_pNewTask != nullptr) ? a_pNewTask->Id() : QString();
    if (sOldKey == sNewKey) {
        m_pCur = a_pNewTask;   // 同档（重复点选 / SelectTask 触发的二次 OnTaskSelected）：早退
        return;
    }
    // 1) 旧档最终落盘（分组：视图档全量 + 外档幽灵合并；运行中助手的最新块随组写入）。
    SaveAssistantState();
    // 2) 切换 + 恢复新档（LoadAssistantState 内摘/挂幽灵 + 保留其会话；无档显式清空）。
    m_pCur = a_pNewTask;
    LoadAssistantState();
}

void CMainWindow::ClearTasks() {
    m_pCur = nullptr;
    // 切工程：停止旧工程可能仍在跑的助手会话，并禁用输入栏（OpenProjectDir 成功后再启用）。
    //   注：OpenProjectDir 已在切档前先行 StopAdhoc（⏹ 痕迹落旧工程档），此处为兜底 no-op；
    //   保持此顺序不变——若此处真停了在跑会话，尾块会因 m_pCur 已空被写进新工程全局档。
    if (m_pOrch != nullptr) {
        m_pOrch->StopAllAdhoc();   // 2026.09.03 多助手并发：整组停
        m_pOrch->ResetAdhocSessions();   // 会话表清空（新工程经 LoadAsstAdhoc 重装）
    }
    // 2026.08.15 两表清空 + 选中复位：旧任务对象随后 delete，若面板 m_pCur 悬垂，
    //   可能命中回收地址误匹配新工程任务（幽灵选中，携带旧阶段表/产物显示）。须在 delete 前。
    if (m_pTaskPanel != nullptr) {
        m_pTaskPanel->ClearAll();
    }
    if (m_pExecPanel != nullptr) {
        m_pExecPanel->SetAdhocEnabled(false);
        m_pExecPanel->SetCurrentTask(nullptr);   // 阶段表/阶段产物/日志/费用栏全清
        m_pExecPanel->ClearAllBlocks();   // #6 清空旧工程对话流（任务 + 助手），防新旧混淆
    }
    if (m_pPm != nullptr) {
        m_pPm->SetTaskConfigOverlay(QString());   // 2026.09.09 v2.1.143 切工程：旧任务覆盖档失效
    }
    for (CTaskInfo* pTask : m_aTasks) {
        delete pTask;
    }
    m_aTasks.clear();
}

void CMainWindow::closeEvent(QCloseEvent* a_pEvent) {
    // 2026.09.18 v2.3.175 退出确认（用户需求：任务/助手在跑时关窗弹提示，明确说明当前
    //   工作、缺省选「否」）：命中任一运行态（流水线 runner/任一助手/改码前备份）先弹
    //   确认框；取消则忽略关闭事件。后续退出路径（落盘/持久化）保持原有行为不变。
    //   2026-10-08 审查窗口多开后：正在跑的手动代码审查窗口一并计入。
    QStringList aLines;
    bool bReviewBusy = false;
    for (const CCodeReviewDialog* pDlg : findChildren<CCodeReviewDialog*>()) {
        if (pDlg != nullptr && pDlg->IsReviewRunning()) {
            bReviewBusy = true;
            aLines.append(QStringLiteral("• 代码审查「%1」正在进行")
                              .arg(pDlg->ReviewTargetName()));
        }
    }
    const bool bOrchBusy = (m_pOrch != nullptr
            && (m_pOrch->IsRunnerBusy() || m_pOrch->IsAnyAdhocRunning()
                || m_pOrch->IsBackupRunning()));
    if (bOrchBusy || bReviewBusy) {
        if (m_pOrch != nullptr) {
            if (m_pOrch->IsRunnerBusy()) {
                QString sLine = QStringLiteral("• 任务正在运行");
                if (const CTaskInfo* pRun = m_pOrch->RunningTask()) {
                    sLine = QStringLiteral("• 任务「%1」正在运行").arg(pRun->Name());
                    if (const SStageDef* pDef = FindStage(pRun->CurrentStage())) {
                        sLine += QStringLiteral("（阶段：%1）").arg(pDef->sNameZh);
                    }
                }
                aLines.prepend(sLine);
            }
        }
        if (m_pOrch != nullptr && m_pExecPanel != nullptr) {
            QStringList aAsstLines;
            for (const QString& sId : m_pOrch->RunningAdhocIds()) {
                aAsstLines.append(QStringLiteral("• 助手「%1」正在运行")
                                      .arg(m_pExecPanel->AsstDisplayName(sId)));
            }
            if (!aAsstLines.isEmpty()) {
                aLines = aAsstLines + aLines;
            }
        }
        if (m_pOrch != nullptr && m_pOrch->IsBackupRunning()) {
            aLines.prepend(QStringLiteral("• 改码前备份正在进行"));
        }
        QMessageBox oBox(this);
        oBox.setIcon(QMessageBox::Warning);
        oBox.setWindowTitle(QStringLiteral("退出 AutoCode"));
        oBox.setText(QStringLiteral(
            "当前有正在进行的工作：\n%1\n"
            "退出将中断上述工作（中断的任务可稍后从任务列表「再次执行」继续，"
            "助手对话已实时落盘不受影响）。\n\n确定要退出 AutoCode 吗？")
            .arg(aLines.join(QChar('\n'))));
        QPushButton* pBtnExit = oBox.addButton(
            QStringLiteral("退出"), QMessageBox::YesRole);
        QPushButton* pBtnCancel = oBox.addButton(
            QStringLiteral("取消"), QMessageBox::NoRole);
        oBox.setDefaultButton(pBtnCancel);   // 缺省选择：否（不退出）
        oBox.setEscapeButton(pBtnCancel);    // Esc 同取消（误触保护）
        oBox.exec();
        if (oBox.clickedButton() != pBtnExit) {
            a_pEvent->ignore();
            return;
        }
    }
    // #1/#6 关闭前落盘助手状态 + 刷新所有挂起脏任务的对话流（去抖 timer 可能尚未到期）。
    if (m_pPm != nullptr && !m_pPm->ProjectDir().isEmpty() && m_pExecPanel != nullptr) {
        SaveAssistantState();
        for (CTaskInfo* pTask : m_aTasks) {
            if (pTask != nullptr && m_pExecPanel->HasTaskBlocks(pTask->Id())) {
                const QByteArray aData =
                    QJsonDocument(m_pExecPanel->TaskBlocksToJson(pTask->Id())).toJson(QJsonDocument::Compact);
                m_pPm->WriteTaskChat(pTask->Id(), aData);
            }
        }
    }
    // 关闭时持久化窗口几何。最大化态存 normalGeometry()，否则会存成铺满全屏的尺寸，
    // 导致下次「还原」得到一个全屏大小的普通窗口。
    if (m_pPm != nullptr) {
        const bool bMax = isMaximized();
        const QRect stGeo = bMax ? normalGeometry() : geometry();
        m_pPm->SaveWindowGeometry(stGeo, bMax);   // 落盘失败内部静默，不阻断退出
        // FR-4：持久化主分隔条（左任务栏 / 右执行面板）比例；saveState 存相对比例，
        // 与窗口尺寸无关，最大化态下保存亦正确。下次启动 CreateUi 中 restoreState 恢复。
        if (m_pMainSplit != nullptr) {
            m_pPm->SaveSplitterState(m_pMainSplit->saveState());
        }
        // 2026.08.18 任务列表高度分隔条（左栏内部）位置一并持久化。
        if (m_pTaskPanel != nullptr) {
            m_pPm->SaveVSplitState(m_pTaskPanel->VSplitState());
        }
        // 2026.08.29 布局碎片：历史任务列宽 / 左栏 Tab 页 / 阶段产物分割条。
        if (m_pTaskPanel != nullptr) {
            m_pPm->SaveLayoutValue(QStringLiteral("TaskCols"),
                                   m_pTaskPanel->TaskColsState());
            m_pPm->SaveLayoutValue(QStringLiteral("TaskTab"),
                                   QString::number(m_pTaskPanel->TaskTabIndex()));
        }
        if (m_pExecPanel != nullptr) {
            m_pPm->SaveLayoutValue(
                QStringLiteral("ArtifactsSplit"),
                QString::fromLatin1(m_pExecPanel->ArtifactsSplitState().toBase64()));
            // 2026-10-16 重命名弹窗几何一并落盘（从未弹过=空串，下次跳过）。
            m_pPm->SaveLayoutValue(
                QStringLiteral("RenameDlgGeo"),
                QString::fromLatin1(m_pExecPanel->RenameDlgGeo().toBase64()));
        }
    }
    // 2026-10-15 退出前显式停全部 runner（用户实害：关闭全部 AutoCode 后 Agent 进程
    //   仍残留——旧路径从不调 Stop，各 runner QProcess 析构不杀进程，Hermes -z、
    //   pi cli.js 及其 kernel/辅助 python 孙进程全部成孤儿）。Stop 系静默收尾不发
    //   sigFinished，无副作用；StopAll 含备份线程 join（退出弹窗已涵盖备份中场景）。
    if (m_pOrch != nullptr) {
        m_pOrch->StopAllAdhoc();
        m_pOrch->StopAll();
    }
    // 2026-09-26 内置沙箱收尾：停钩子/扫描，全部停录像并还原幽灵窗口——
    //   不还原会留下隐形窗口直到重启（用户无感知点击穿透假死）。
    if (m_pSbx != nullptr) {
        m_pSbx->Shutdown();
    }
    QMainWindow::closeEvent(a_pEvent);
}

bool CMainWindow::IsGeometryOnScreen(const QRect& a_stGeo) {
    // 几何非法、或与所有屏幕的可用区（排除任务栏）都不相交 → 视为屏幕外。
    if (!a_stGeo.isValid() || a_stGeo.width() <= 0 || a_stGeo.height() <= 0) {
        return false;
    }
    const QList<QScreen*> aScreens = QGuiApplication::screens();
    for (const QScreen* pSc : aScreens) {
        if (pSc != nullptr && pSc->availableGeometry().intersects(a_stGeo)) {
            return true;
        }
    }
    return false;
}

