#pragma once

#include <QDialog>
#include <QList>
#include <QMap>

#include "project/CProjectManager.h"   // SBackupVersion / SDiffResult

class QTableWidget;
class QSplitter;
class QLabel;
class QPushButton;
class CProjectManager;

// 统一增量备份的回退界面（doc/增量备份方案.md v3.1 §5）：master-detail。
//   左：记录点列表（#/时间/类型/名称摘要/改/删/增 7 列）；右：选中记录点相对
//   上一记录点的文件变动明细（3 列）。构造时一次性 List + 相邻 Diff 缓存。
// 纯展示与选择，不执行任何文件操作：accept() 后由主窗回退引擎执行，
// SelectedSeq() 返回选中序号（legacy 版本带 bLegacy 标记，引擎按其分派旧恢复路径）。
class CRollbackDialog : public QDialog {
    Q_OBJECT

public:
    // a_sOwner：任务 id / "global"（助手档）。a_pPm：工程管理器（只读使用）。
    // a_sDispName：标题显示名（任务名等）；空则按 owner 推导（global→工程助手）。
    explicit CRollbackDialog(const QString& a_sOwner, CProjectManager* a_pPm,
                             QWidget* a_pParent = nullptr,
                             const QString& a_sDispName = QString());
    ~CRollbackDialog() override;

    // 取选中版本；用户取消（拒绝/关闭）返回空指针。
    const SBackupVersion* SelectedVersion() const { return m_pSelected; }
    int SelectedSeq() const { return m_pSelected ? m_pSelected->iSeq : -1; }

protected:
    void hideEvent(QHideEvent* a_pEvent) override;   // 统一持久化窗口/分割条/列宽
    // 2026.08.28 布局激活后刷新冻结值：构造期（未显示）sizes() 是 sizeHint 级假值
    //   （旧版曾把 71/170 这种未激活值存档，恢复后分割条不可用）；首次显示/
    //   窗口 resize 后的真实分配才是该冻结的布局，点选记录点不再弹回旧位置。
    void showEvent(QShowEvent* a_pEvent) override;
    void resizeEvent(QResizeEvent* a_pEvent) override;

private slots:
    void OnSelectionChanged();
    // 2026.08.24 文件列表右键菜单（「查看变更」）。
    void OnFilesContextMenu(const QPoint& a_pos);
    void OnFilesCellDoubleClicked(int a_iRow, int a_iCol);

private:
    void BuildUi();
    void LoadData();
    void FillVersionTable();
    void FillFilesTable(int a_iRow);
    // 2026.08.24 弹出双栏对比（CDiffViewDialog）：读两个记录点中该文件的内容。
    void ShowFileDiff(int a_iRow);
    void SaveLayoutState();
    void RestoreLayoutState();
    // kind 派生显示（§5.3）。
    static QString KindLabel(const SBackupVersion& a_st);
    static QString DisplayTitle(const SBackupVersion& a_st);
    static QString FormatSize(qint64 a_iBytes);

    QString            m_sOwner;
    QString            m_sDispName;   // 2026.08.22 标题显示名（任务名；空=按 owner 推导）
    CProjectManager*   m_pPm       = nullptr;
    QList<SBackupVersion> m_aVersions;             // 倒序（最新在前）
    bool m_bLayoutSaved = false;   // 2026.08.22 hide/析构双路径防重复写盘
    QMap<int, SDiffResult> m_aDiffBySeq;           // seq → 相对上一记录点的 diff
    QList<SDiffEntry>      m_aShownEntries;        // 2026.08.24 右表当前展示的 diff 条目（排序后，行号对齐）
    const SBackupVersion* m_pSelected = nullptr;

    QLabel*      m_pLblSummary = nullptr;
    QSplitter*   m_pSplit      = nullptr;
    QTableWidget* m_pTableVersions = nullptr;
    QTableWidget* m_pTableFiles    = nullptr;
    QLabel*      m_pLblFilesCaption = nullptr;
    QPushButton* m_pBtnRestore = nullptr;
    QList<int>   m_aSplitSizes;   // 2026.08.22 冻结分割条：选中记录点后内容变化不许推动布局
};
