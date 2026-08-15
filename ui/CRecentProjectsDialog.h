#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QDialogButtonBox;

// 最近打开工程历史对话框：全量列出历史（MRU），双击 / 确认返回选中路径，
// 取消 / 关闭返回空。仅持有 QStringList 做展示，不直接访问 CProjectManager。
class CRecentProjectsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent = nullptr);

    // exec() 返回 Accepted 时取选中路径；Rejected / 空选择返回空串。
    QString SelectedPath() const;

private slots:
    void OnItemDoubleClicked();   // 双击 = 确认

private:
    QListWidget*      m_pList = nullptr;
    QDialogButtonBox* m_pBtns = nullptr;
    QString           m_sSelected;
};
