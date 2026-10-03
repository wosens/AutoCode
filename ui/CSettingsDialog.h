#pragma once

#include <QDialog>
#include <QList>

#include "CProjectManager.h"   // SDshModelEntry（自定义大模型表）
#include "CVersionDialog.h"    // FBusyCheck（后端版本管理入口性判据注入）

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QTreeWidget;
class QPushButton;
class CProjectManager;

// 设置对话框（2026.08.15）：集中编辑 exe 同级 config.ini 的可调参数，
// 「功能」菜单 →「设置...」打开；确定时由 CMainWindow 统一落盘并应用到运行期。
// 覆盖分组：
//   - 运行策略 [Policy]：单阶段最大迭代 / 续跑上限 / 评审续跑上限
//   - Agent    [Agent]：claude 命令 / ask_user 提问超时 / 联网检索开关
//   - 评审     [Review]：多视角并行视角数
//   - 助手     [Adhoc]：autocompact 压缩比例（窗口百分比）；上下文窗口上限设置项
//             2026.09.27 已删（入口收敛到上下文进度条右键，未设按模型自动探测）
//   - 目录排除 [Scan]/[Backup]：指纹扫描 / 备份恢复的排除目录（逗号分隔；空=内置默认）
class CSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // a_pPm 仅用于构造时读当前值（非拥有；空指针时全部用默认值）。
    //   a_fnBackendBusy：后端版本管理对话框的 D6 运行中拦截判据（由 CMainWindow
    //   注入；空=放行，测试场景）。
    // a_sTaskScope：2026.09.09 v2.1.143 任务级配置覆盖——并空时选中任务名，标题提示
    //   本次读/写均落在任务档（.autodev/tasks/<id>/config.ini）；空=全局 config.ini。
    explicit CSettingsDialog(CProjectManager* a_pPm, QWidget* a_pParent = nullptr,
                             CVersionDialog::FBusyCheck a_fnBackendBusy = {},
                             const QString& a_sTaskScope = QString());

    // —— 确定后由主窗口读取并落盘/应用 ——
    int     MaxIter() const;
    int     MaxResume() const;
    int     ReviewResume() const;
    int     ReviewParallel() const;
    QString ClaudeCmd() const;
    int     AskTimeoutSec() const;
    bool    WebSearch() const;
    // 2026.08.26 专家助手开关（[Adhoc] ExpertAssistant）：缺省启用，可手工禁用。
    bool    ExpertAssistant() const;
    // 2026.08.18 Agent 后端与 DSH 参数（全局单选；1=claude，2=dsh）。
    int     AgentBackend() const;
    QString DshNodePath() const;
    QString DshRoot() const;
    // 2026.09.27 DSH 额外可写目录（[Dsh] ExtraWriteDirs；';' 分隔；沙箱 DACL 放行）。
    QStringList DshExtraWriteDirs() const;
    QString DshProvider() const;
    QString DshModel() const;
    // 2026.08.18 DSH 计费单价（$/百万 token）：DSH 只报 token，客户端按单价折算费用。
    double  DshPriceInPerM() const;
    double  DshPriceCacheReadPerM() const;
    double  DshPriceCacheWritePerM() const;
    double  DshPriceOutPerM() const;
    // 2026.08.18 自定义大模型表（[DshModels]）：条目列表与激活下标（-1=未启用）。
    //   激活条目生效时覆盖 Provider/模型两行（组合走 autocode-active env 槽位）。
    QList<SDshModelEntry> DshModels() const;
    int     DshActiveModel() const;
    // 2026.08.19 PIAgent 第三后端参数（[Agent] Pi*）。
    QString PiNodePath() const;
    QString PiRoot() const;
    QString PiProvider() const;
    QString PiThinking() const;
    // 2026.08.20 PIAgent 计费单价（$/百万 token）：pi 只报 token，客户端按单价折算费用。
    double  PiPriceInPerM() const;
    double  PiPriceCacheReadPerM() const;
    double  PiPriceCacheWritePerM() const;
    double  PiPriceOutPerM() const;
    // 2026.08.20 PiAgent 自定义大模型表（[PiModels] + ~/.pi/agent/models.json）。
    QList<SDshModelEntry> PiModels() const;
    int     PiActiveModel() const;
    // CodexHarness 第四后端参数（[Agent] Codex*；OpenAI Codex CLI 0.87.0）。
    QString CodexNodePath() const;
    QString CodexRoot() const;
    QString CodexProvider() const;
    QString CodexWireApi() const;
    // 2026.08.21 Codex 自定义大模型表（[CodexModels]；口径同 Pi 同名对）。
    QList<SDshModelEntry> CodexModels() const;
    int     CodexActiveModel() const;
    // Hermes 第五后端参数（任务 3213cdbf，[Agent] Hermes* / [HermesModels]）。
    QString HermesNodePath() const;
    QString HermesRoot() const;
    double  HermesPriceInPerM() const;
    double  HermesPriceCacheReadPerM() const;
    double  HermesPriceCacheWritePerM() const;
    double  HermesPriceOutPerM() const;
    QList<SDshModelEntry> HermesModels() const;
    int     HermesActiveModel() const;
    int     AutoCompactPct() const;
    // 2026.09.23 助手单轮 --max-turns（默认 40，10..200）——大重构触顶可放大。
    int     AdhocMaxTurns() const;
    // 2026.09.23 v3.0.219 工作量评估开关（缺省开；关=恒用基值）。
    bool    AdhocMaxTurnsAuto() const;
    QString ScanExcludeDirs() const;
    QString BackupExcludeDirs() const;
    QString ExplorerHiddenDirs() const;   // 2026.09.23 资源管理器隐藏目录（全量清单原文）
    // 2026-09-26 内置测试沙箱（[Sandbox]：Agent 后代 GUI 窗口自动收纳/录像）。
    bool SandboxEnable() const;
    bool SandboxAutoGhost() const;
    bool SandboxRecord() const;
    int  SandboxFps() const;
    int  SandboxRetainDays() const;

    // 2026.08.27 窗口几何持久化：accept / reject / 标题栏关闭都经 done()，在此统一
    //   落盘 [SettingsWindow]（exe 同级 config.ini）；下次打开恢复位置与大小。
    //   2026.09.26 修「每开一次下漂 31px」：保存改用框架原点坐标（详见 done() 注释），
    //   与构造期 move() 的建窗前框架语义同坐标系，往返零漂移。
    void done(int a_r) override;

private:
    void SetupUi();
    void UpdateDshEnabled();   // 后端下拉联动：DSH 参数子组按选择启用/禁用
    // 2026.09 后端版本管理入口（任务 23183ccb）：Agent 组按钮 → CVersionDialog。
    void OnVersionManage();
    // 2026.09.10 公共模型配置（四后端共享同一份模型列表与当前项）：
    //   构造回填后把四张表合并归一到四个成员（两两相等）；任一页的增/改/删/
    //   设为当前经 ShareModelsFromPage 同步到其余三页。保存时主窗口把同一份
    //   内容写回四张节（[DshModels]/[PiModels]/[CodexModels]/[HermesModels]）。
    void UnifySharedModels();
    void ShareModelsFromPage(int a_iPage);   // 0=DSH 1=Pi 2=Codex 3=Hermes
    // 2026.08.18 自定义大模型列表维护（DSH 页）：列表文本/按钮态刷新 + 增改删/设为当前。
    void RefreshDshModelList();
    // 2026.08.19 列表改 QTreeWidget 后的当前行号（-1=无/占位行）。
    int CurDshModelRow() const;
    void OnDshModelAdd();
    void OnDshModelEdit();
    // 2026.08.27 复制选中条目：预填编辑对话框（显示名加“-副本”后缀，可改），
    //   确认后追加到列表末尾——快速建同网关的多个模型。
    void OnDshModelCopy();
    void OnDshModelDel();
    void OnDshModelSetActive();
    // DSH 安装状态探测：根目录框显式值 → 空则 ProbeSiblingRoot 探测；命中且框为空时
    //   自动填入（确定落盘即回写 config.ini）。更新状态标签/安装按钮/DSH 参数与后端
    //   下拉的可用性。a_bSilent=false 时弹结果提示（「重新检测」按钮用）。
    void DetectDshInstalled(bool a_bSilent);
    void OnInstallDsh();       // 以管理员权限启动 install_env.bat /dsh 单装 DeepSeekHarness
    // 2026.08.19 PIAgent 安装探测/单装（口径同 DSH：DetectPiInstalled 静默/手动两态；
    //   OnInstallPi 提权运行 install_env.bat /pi，robocopy 本机源到 exe 同级 PIAgent\pi）。
    void UpdatePiEnabled();
    void DetectPiInstalled(bool a_bSilent);
    void OnInstallPi();
    void OnInstallPiNpm();       // 2026.08.20 联网安装：npm install -g pi-coding-agent
    // 2026.08.20 PiAgent 自定义大模型列表维护（口径同 DSH 页同名四方法）。
    void RefreshPiModelList();
    int  CurPiModelRow() const;
    void OnPiModelAdd();
    void OnPiModelEdit();
    void OnPiModelCopy();   // 2026.08.27 复制选中条目（口径同 DSH 页）
    void OnPiModelDel();
    void OnPiModelSetActive();
    // CodexHarness 安装探测/单装（口径同 Pi：DetectCodexInstalled 静默/手动两态；
    //   OnInstallCodex 提权运行 install_env.bat /codex，npm 本地安装到 exe 同级
    //   Execute\CodexHarness）。
    void UpdateCodexEnabled();
    void DetectCodexInstalled(bool a_bSilent);
    void OnInstallCodex();
    void OnInstallCodexLocal();   // 2026.08.21 本机源离线安装（robocopy，无需管理员）
    // 2026.08.21 Codex 自定义大模型列表维护（口径同 Pi 页同名四方法）。
    void RefreshCodexModelList();
    int  CurCodexModelRow() const;
    void OnCodexModelAdd();
    void OnCodexModelEdit();
    void OnCodexModelCopy();   // 2026.08.27 复制选中条目（口径同 DSH 页）
    void OnCodexModelDel();
    void OnCodexModelSetActive();
    // Hermes 第五后端（任务 3213cdbf）：安装探测（口径同 Pi；自动安装待澄清 3
    //   落地，安装按钮不设——只检测）+ 模型列表维护（口径同 Pi 页）。
    void UpdateHermesEnabled();
    void DetectHermesInstalled(bool a_bSilent);
    // 2026.09 Hermes 本机源离线安装（robocopy 到 exe 同级 HermesHarness；口径同
    //   Codex 本机源安装——无 npm 包源/仓库发布形态，装更一体走本机源重部署）。
    void OnInstallHermesLocal();
    // 2026.09 Hermes 联网安装（git）：提权运行 install_env.bat /hermes <仓库地址>
    //   （口径同 DSH 安装——git 浅克隆 + pnpm 依赖构建；已装时 fetch+reset 更新）。
    void OnInstallHermesNet();
    void RefreshHermesModelList();
    int  CurHermesModelRow() const;
    void OnHermesModelAdd();
    void OnHermesModelEdit();
    void OnHermesModelCopy();
    void OnHermesModelDel();
    void OnHermesModelSetActive();

    QSpinBox*   m_pSpinMaxIter      = nullptr;
    QSpinBox*   m_pSpinMaxResume    = nullptr;
    QSpinBox*   m_pSpinReviewResume = nullptr;
    QSpinBox*   m_pSpinReviewPar    = nullptr;
    QComboBox*  m_pComboBackend     = nullptr;
    QLineEdit*  m_pEditClaudeCmd    = nullptr;
    QSpinBox*   m_pSpinAskTimeout   = nullptr;
    QLabel*     m_pLblDshState      = nullptr;
    QPushButton* m_pBtnDetectDsh    = nullptr;
    QPushButton* m_pBtnInstallDsh   = nullptr;
    bool        m_bDshInstalled     = false;
    QLineEdit*  m_pEditDshNode      = nullptr;
    QLineEdit*  m_pEditDshRoot      = nullptr;
    QLineEdit*  m_pEditDshWsRoot    = nullptr;   // 2026.09.27 额外可写目录（';' 分隔）
    QLineEdit*  m_pEditDshProvider  = nullptr;
    QDoubleSpinBox* m_pSpinDshPriceIn        = nullptr;   // 计费单价：纯输入
    QDoubleSpinBox* m_pSpinDshPriceCacheRead = nullptr;   // 计费单价：缓存读
    QDoubleSpinBox* m_pSpinDshPriceCacheWrite = nullptr;  // 计费单价：缓存写
    QDoubleSpinBox* m_pSpinDshPriceOut       = nullptr;   // 计费单价：输出
    // 2026.08.18 自定义大模型表（多条目 + 激活下标；[DshModels] 落盘由主窗口完成）。
    QList<SDshModelEntry> m_aDshModels;
    int         m_iDshActiveModel = -1;      // -1=未启用（走 Provider/模型固定路由）
    QTreeWidget* m_pTreeDshModels = nullptr;   // 2026.08.19 多列信息表（原 QListWidget 单行文本）
    QPushButton* m_pBtnModelAdd    = nullptr;
    QPushButton* m_pBtnModelCopy   = nullptr;   // 2026.08.27 复制选中条目（新增…后侧）
    QPushButton* m_pBtnModelEdit   = nullptr;
    QPushButton* m_pBtnModelDel    = nullptr;
    QPushButton* m_pBtnModelActive = nullptr;
    QCheckBox*  m_pChkWebSearch     = nullptr;
    QCheckBox*  m_pChkExpertAssistant = nullptr;   // 2026.08.26 专家助手开关
    // 2026.08.19 PIAgent Tab 控件（仿 DSH Tab：安装状态行 + node/root/provider/model/
    //   apiKey/thinking 编辑组；thinking 为下拉，空项=默认）。
    QLabel*     m_pLblPiState       = nullptr;
    QPushButton* m_pBtnDetectPi     = nullptr;
    QPushButton* m_pBtnInstallPi    = nullptr;
    QPushButton* m_pBtnInstallPiNpm = nullptr;   // 2026.08.20 联网安装（npm 全局）
    bool        m_bPiInstalled      = false;
    QLineEdit*  m_pEditPiNode       = nullptr;
    QLineEdit*  m_pEditPiRoot       = nullptr;
    QLineEdit*  m_pEditPiProvider   = nullptr;
    QComboBox*  m_pComboPiThinking  = nullptr;
    QDoubleSpinBox* m_pSpinPiPriceIn        = nullptr;   // 2026.08.20 计费单价：纯输入
    QDoubleSpinBox* m_pSpinPiPriceCacheRead = nullptr;   // 计费单价：缓存读
    QDoubleSpinBox* m_pSpinPiPriceCacheWrite = nullptr;  // 计费单价：缓存写
    QDoubleSpinBox* m_pSpinPiPriceOut       = nullptr;   // 计费单价：输出
    // 2026.08.20 PiAgent 自定义大模型表（[PiModels]；多条目 + 激活下标，经
    //   ~/.pi/agent/models.json 的 autocode-<n> provider 槽位生效）。
    QList<SDshModelEntry> m_aPiModels;
    int         m_iPiActiveModel = -1;    // -1=未启用（走 Provider/模型固定路由）
    QTreeWidget* m_pTreePiModels = nullptr;
    QPushButton* m_pBtnPiModelAdd    = nullptr;
    QPushButton* m_pBtnPiModelCopy   = nullptr;   // 2026.08.27 复制选中条目
    QPushButton* m_pBtnPiModelEdit   = nullptr;
    QPushButton* m_pBtnPiModelDel    = nullptr;
    QPushButton* m_pBtnPiModelActive = nullptr;
    // CodexHarness 第四后端（[Agent] Codex*）：安装状态行 + 表单七项（Root/Node/
    //   Provider/Model/ApiKey/BaseUrl/WireApi 下拉）。
    QLabel*     m_pLblCodexState   = nullptr;
    QPushButton* m_pBtnDetectCodex = nullptr;
    QPushButton* m_pBtnInstallCodex = nullptr;
    bool        m_bCodexInstalled = false;
    // 2026.08.21 配置里已存的后端值：不可用时不再静默弹回 claude（用户选择优先，
    //   状态页红字提示 + 启动/保存有 CodexConfigError 兜底），仅拦「主动新切到」。
    int         m_iCfgBackend = 1;
    QLineEdit*  m_pEditCodexNode   = nullptr;
    QLineEdit*  m_pEditCodexRoot   = nullptr;
    QLineEdit*  m_pEditCodexProvider = nullptr;
    QComboBox*  m_pComboCodexWireApi = nullptr;
    // 2026.08.21 Codex 自定义大模型表（[CodexModels]；生效经 ~/.codex 的
    //   autocode-<n> provider 槽位，CCodexRunner 合成写）+ 本机源离线安装按钮。
    QList<SDshModelEntry> m_aCodexModels;
    int         m_iCodexActiveModel = -1;    // -1=未启用（走固定路由）
    QTreeWidget* m_pTreeCodexModels = nullptr;
    QPushButton* m_pBtnCodexModelAdd    = nullptr;
    QPushButton* m_pBtnCodexModelCopy   = nullptr;   // 2026.08.27 复制选中条目
    QPushButton* m_pBtnCodexModelEdit   = nullptr;
    QPushButton* m_pBtnCodexModelDel    = nullptr;
    QPushButton* m_pBtnCodexModelActive = nullptr;
    QPushButton* m_pBtnInstallCodexLocal = nullptr;
    // Hermes 第五后端（[Agent] Hermes*）：安装状态行 + 表单五项（Root/Node/Model/
    // ApiKey/BaseUrl）+ 计费单价四项 + 模型树（[HermesModels]）。
    QLabel*     m_pLblHermesState   = nullptr;
    QPushButton* m_pBtnDetectHermes = nullptr;
    QPushButton* m_pBtnInstallHermesLocal = nullptr;   // 本机源安装（离线，robocopy）
    QPushButton* m_pBtnInstallHermesNet = nullptr;     // 联网安装（git 克隆 + pnpm 构建）
    bool        m_bHermesInstalled = false;
    QLineEdit*  m_pEditHermesNode   = nullptr;
    QLineEdit*  m_pEditHermesRoot   = nullptr;
    QDoubleSpinBox* m_pSpinHermesPriceIn        = nullptr;
    QDoubleSpinBox* m_pSpinHermesPriceCacheRead = nullptr;
    QDoubleSpinBox* m_pSpinHermesPriceCacheWrite = nullptr;
    QDoubleSpinBox* m_pSpinHermesPriceOut       = nullptr;
    QList<SDshModelEntry> m_aHermesModels;
    int         m_iHermesActiveModel = -1;   // -1=未启用（走 HermesModel 固定路由）
    QTreeWidget* m_pTreeHermesModels = nullptr;
    QPushButton* m_pBtnHermesModelAdd    = nullptr;
    QPushButton* m_pBtnHermesModelCopy   = nullptr;
    QPushButton* m_pBtnHermesModelEdit   = nullptr;
    QPushButton* m_pBtnHermesModelDel    = nullptr;
    QPushButton* m_pBtnHermesModelActive = nullptr;
    QSpinBox*   m_pSpinCompactPct   = nullptr;
    QSpinBox*   m_pSpinMaxTurns     = nullptr;   // 2026.09.23 助手轮次上限（[Adhoc] MaxTurns）
    QCheckBox*  m_pChkMaxTurnsAuto = nullptr;   // 2026.09.23 工作量评估开关（[Adhoc] MaxTurnsAuto）
    QLineEdit*  m_pEditScanDirs     = nullptr;
    QLineEdit*  m_pEditBackupDirs   = nullptr;
    QLineEdit*  m_pEditExplHidden   = nullptr;   // 2026.09.23 资源管理器隐藏目录（[Explorer] HiddenDirs）
    // 2026-09-26 内置测试沙箱（[Sandbox]）
    QCheckBox*  m_pChkSbxEnable   = nullptr;   // 总开关
    QCheckBox*  m_pChkSbxAuto     = nullptr;   // 自动收纳新窗口
    QCheckBox*  m_pChkSbxRecord   = nullptr;   // 自动录像
    QSpinBox*   m_pSpinSbxFps     = nullptr;   // 录制帧率
    QSpinBox*   m_pSpinSbxRetain  = nullptr;   // 录像保留天数
    // 2026.08.27 几何持久化用的配置管理器引用（非拥有；构造时留存，done() 落盘用）。
    CProjectManager* m_pPm = nullptr;
    // 2026.09 后端版本管理 D6 判据（构造注入透传给 CVersionDialog；空=放行）。
    CVersionDialog::FBusyCheck m_fnBackendBusy;
};
