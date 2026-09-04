#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QListWidgetItem;
class QDialogButtonBox;
class QLineEdit;

// 最近打开工程历史对话框：全量列出历史（MRU），双击 / 确认返回选中路径，
// 取消 / 关闭返回空。仅持有 QStringList 做展示，不直接访问 CProjectManager。
// 2026.08.20：列表 11 号字体；支持多选（ExtendedSelection）；右键菜单可删除选中
// 条目（仅从列表移除并经 RemovedPaths() 上报，由调用方落 MRU 持久化）。
class CRecentProjectsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent = nullptr);

    // exec() 返回 Accepted 时取选中路径；Rejected / 空选择返回空串。
    // 多选时返回当前项（current item）路径；无当前项取第一个选中项。
    QString SelectedPath() const;

    // 本次会话中被右键删除的工程路径（顺序无关）；调用方据此从 MRU 移除并落盘。
    QStringList RemovedPaths() const;

private slots:
    void OnItemDoubleClicked();              // 双击 = 确认
    void OnContextMenu(const QPoint& a_pos); // 右键菜单：删除选中
    void OnDeleteSelected();                 // 执行删除（列表内移除 + 记录）
    void OnSearchChanged(const QString& a_sText); // 搜索框内容变化 → 实时重筛列表

private:
    void ApplyPath(QListWidgetItem* a_pItem); // 记录确认路径（空安全）
    void RebuildList(const QString& a_sFilter); // 依据过滤词重建列表（排除已删条目）

    QListWidget*      m_pList = nullptr;
    QLineEdit*        m_pSearch = nullptr;   // 顶部搜索框（实时过滤）
    QStringList       m_aAll;                // 全量历史快照（构造入参，过滤/重建的数据源）
    QDialogButtonBox* m_pBtns = nullptr;
    QString           m_sSelected;
    QStringList       m_aRemoved;             // 右键删除的路径集合
};
