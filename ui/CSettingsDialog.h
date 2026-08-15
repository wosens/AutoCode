#pragma once

#include <QDialog>

class QSpinBox;
class QLineEdit;
class QCheckBox;
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
    int     AutoCompactPct() const;
    qint64  ContextLimit() const;
    QString ScanExcludeDirs() const;
    QString BackupExcludeDirs() const;

private:
    void SetupUi();

    QSpinBox*   m_pSpinMaxIter      = nullptr;
    QSpinBox*   m_pSpinMaxResume    = nullptr;
    QSpinBox*   m_pSpinReviewResume = nullptr;
    QSpinBox*   m_pSpinReviewPar    = nullptr;
    QLineEdit*  m_pEditClaudeCmd    = nullptr;
    QSpinBox*   m_pSpinAskTimeout   = nullptr;
    QCheckBox*  m_pChkWebSearch     = nullptr;
    QSpinBox*   m_pSpinCompactPct   = nullptr;
    QSpinBox*   m_pSpinCtxLimit     = nullptr;
    QLineEdit*  m_pEditScanDirs     = nullptr;
    QLineEdit*  m_pEditBackupDirs   = nullptr;
};
