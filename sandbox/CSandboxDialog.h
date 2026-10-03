#pragma once

// 内置测试沙箱管理对话框（2026-09-26）：非模态挂在主窗口（功能→测试沙箱...）。
// 展示：收纳窗口表（标题/进程/PID/尺寸/收纳时间/录像状态）、状态行、
// 操作：截图/还原所选/全部还原/关闭所选/打开录像目录。1s 定时 + sigChanged 刷新。

#include <QDialog>
#include <QTableWidget>

class CSandboxManager;
class QLabel;
class QPushButton;

class CSandboxDialog : public QDialog {
    Q_OBJECT

public:
    explicit CSandboxDialog(CSandboxManager* a_pMgr, QWidget* a_pParent = nullptr);

private slots:
    void OnRefresh();
    void OnCapture();
    void OnRestore();
    void OnRestoreAll();
    void OnCloseWin();
    void OnOpenRecDir();

private:
    void RefreshTable();
    QList<HWND> SelectedHwnds() const;

    CSandboxManager* m_pMgr = nullptr;
    QLabel*          m_pLblStatus = nullptr;
    QTableWidget*    m_pTable = nullptr;
    QPushButton*     m_pBtnCapture = nullptr;
    QPushButton*     m_pBtnRestore = nullptr;
    QPushButton*     m_pBtnRestoreAll = nullptr;
    QPushButton*     m_pBtnClose = nullptr;
};
