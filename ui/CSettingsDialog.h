#pragma once

#include <QDialog>
#include <QList>

#include "CProjectManager.h"   // SDshModelEntry（自定义大模型表）

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
//   - 助手     [Adhoc]：autocompact 压缩比例（窗口百分比）/ 上下文窗口上限（0=自动）
//   - 目录排除 [Scan]/[Backup]：指纹扫描 / 备份恢复的排除目录（逗号分隔；空=内置默认）
class CSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // a_pPm 仅用于构造时读当前值（非拥有；空指针时全部用默认值）。
    explicit CSettingsDialog(CProjectManager* a_pPm, QWidget* a_pParent = nullptr);

    // —— 确定后由主窗口读取并落盘/应用 ——
    int     MaxIter() const;
    int     MaxResume() const;
    int     ReviewResume() const;
    int     ReviewParallel() const;
    QString ClaudeCmd() const;
    int     AskTimeoutSec() const;
    bool    WebSearch() const;
    // 2026.08.18 Agent 后端与 DSH 参数（全局单选；1=claude，2=dsh）。
    int     AgentBackend() const;
    QString DshNodePath() const;
    QString DshRoot() const;
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
    int     AutoCompactPct() const;
    qint64  ContextLimit() const;
    QString ScanExcludeDirs() const;
    QString BackupExcludeDirs() const;

private:
    void SetupUi();
    void UpdateDshEnabled();   // 后端下拉联动：DSH 参数子组按选择启用/禁用
    // 2026.08.18 自定义大模型列表维护（DSH 页）：列表文本/按钮态刷新 + 增改删/设为当前。
    void RefreshDshModelList();
    // 2026.08.19 列表改 QTreeWidget 后的当前行号（-1=无/占位行）。
    int CurDshModelRow() const;
    void OnDshModelAdd();
    void OnDshModelEdit();
    void OnDshModelDel();
    void OnDshModelSetActive();
    // DSH 安装状态探测：根目录框显式值 → 空则 ProbeSiblingRoot 探测；命中且框为空时
    //   自动填入（确定落盘即回写 config.ini）。更新状态标签/安装按钮/DSH 参数与后端
    //   下拉的可用性。a_bSilent=false 时弹结果提示（「重新检测」按钮用）。
    void DetectDshInstalled(bool a_bSilent);
    void OnInstallDsh();       // 以管理员权限启动 install_env.bat /dsh 单装 DeepSeekHarness

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
    QLineEdit*  m_pEditDshProvider  = nullptr;
    QLineEdit*  m_pEditDshModel     = nullptr;
    QDoubleSpinBox* m_pSpinDshPriceIn        = nullptr;   // 计费单价：纯输入
    QDoubleSpinBox* m_pSpinDshPriceCacheRead = nullptr;   // 计费单价：缓存读
    QDoubleSpinBox* m_pSpinDshPriceCacheWrite = nullptr;  // 计费单价：缓存写
    QDoubleSpinBox* m_pSpinDshPriceOut       = nullptr;   // 计费单价：输出
    // 2026.08.18 自定义大模型表（多条目 + 激活下标；[DshModels] 落盘由主窗口完成）。
    QList<SDshModelEntry> m_aDshModels;
    int         m_iDshActiveModel = -1;      // -1=未启用（走 Provider/模型固定路由）
    QTreeWidget* m_pTreeDshModels = nullptr;   // 2026.08.19 多列信息表（原 QListWidget 单行文本）
    QPushButton* m_pBtnModelAdd    = nullptr;
    QPushButton* m_pBtnModelEdit   = nullptr;
    QPushButton* m_pBtnModelDel    = nullptr;
    QPushButton* m_pBtnModelActive = nullptr;
    QCheckBox*  m_pChkWebSearch     = nullptr;
    QSpinBox*   m_pSpinCompactPct   = nullptr;
    QSpinBox*   m_pSpinCtxLimit     = nullptr;
    QLineEdit*  m_pEditScanDirs     = nullptr;
    QLineEdit*  m_pEditBackupDirs   = nullptr;
};
