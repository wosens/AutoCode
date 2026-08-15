#pragma once

#include <QDialog>
#include <QList>

#include "project/CProjectManager.h"   // SBackupVersion

class QListWidget;
class QListWidgetItem;
class QDialogButtonBox;

// FR-6：代码回退的历史版本选择对话框。构造时传入全部版本（倒序，最新在前），
// QListWidget 列出「序号 + 摘要/（首次备份） + 时间」；确认返回选中序号，取消返回 -1。
// 纯展示 + 选择，不触碰文件系统（恢复由 CProjectManager::RestoreSources 在主窗口执行）。
class CRollbackDialog : public QDialog {
    Q_OBJECT

public:
    explicit CRollbackDialog(const QList<SBackupVersion>& a_aVersions,
                             QWidget* a_pParent = nullptr);

    // 取选中版本序号；用户取消（拒绝/关闭）返回 -1。
    int SelectedSeq() const { return m_iSelected; }

private slots:
    void OnItemChanged(QListWidgetItem* a_pCur, QListWidgetItem* a_pPrev);
    void OnItemDoubleClicked(QListWidgetItem* a_pItem);

private:
    QList<SBackupVersion> m_aVersions;
    QListWidget*          m_pList     = nullptr;
    QDialogButtonBox*     m_pButtons  = nullptr;
    int                   m_iSelected = -1;
};
