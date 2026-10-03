// 内置测试沙箱管理对话框实现（说明见同名 .h）。
#include "CSandboxDialog.h"
#include "CSandboxManager.h"
#include "CSandboxCore.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QCoreApplication>

CSandboxDialog::CSandboxDialog(CSandboxManager* a_pMgr, QWidget* a_pParent)
    : QDialog(a_pParent), m_pMgr(a_pMgr) {
    setWindowTitle(QStringLiteral("测试沙箱"));
    setMinimumSize(760, 380);
    setSizeGripEnabled(true);

    auto* pLay = new QVBoxLayout(this);

    m_pLblStatus = new QLabel(this);
    m_pLblStatus->setWordWrap(true);
    pLay->addWidget(m_pLblStatus);

    m_pTable = new QTableWidget(this);
    m_pTable->setColumnCount(6);
    m_pTable->setHorizontalHeaderLabels({
        QStringLiteral("窗口标题"), QStringLiteral("进程"), QStringLiteral("PID"),
        QStringLiteral("尺寸"), QStringLiteral("收纳时间"), QStringLiteral("录像") });
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->setAlternatingRowColors(true);
    m_pTable->horizontalHeader()->setStretchLastSection(false);
    m_pTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pTable->verticalHeader()->setVisible(false);
    pLay->addWidget(m_pTable, 1);

    auto* pRow = new QWidget(this);
    auto* pLayBtns = new QHBoxLayout(pRow);
    pLayBtns->setContentsMargins(0, 0, 0, 0);
    m_pBtnCapture = new QPushButton(QStringLiteral("截图…"), pRow);
    m_pBtnCapture->setToolTip(QStringLiteral("对选中窗口离屏截图，PNG 存 exe\\sandbox\\shot"));
    m_pBtnRestore = new QPushButton(QStringLiteral("还原所选"), pRow);
    m_pBtnRestore->setToolTip(QStringLiteral("取消幽灵化：窗口恢复可见可交互（并结束该窗口录像）"));
    m_pBtnRestoreAll = new QPushButton(QStringLiteral("全部还原"), pRow);
    m_pBtnClose = new QPushButton(QStringLiteral("关闭所选窗口"), pRow);
    auto* pBtnRecDir = new QPushButton(QStringLiteral("打开录像目录"), pRow);
    connect(m_pBtnCapture, &QPushButton::clicked, this, &CSandboxDialog::OnCapture);
    connect(m_pBtnRestore, &QPushButton::clicked, this, &CSandboxDialog::OnRestore);
    connect(m_pBtnRestoreAll, &QPushButton::clicked, this, &CSandboxDialog::OnRestoreAll);
    connect(m_pBtnClose, &QPushButton::clicked, this, &CSandboxDialog::OnCloseWin);
    connect(pBtnRecDir, &QPushButton::clicked, this, &CSandboxDialog::OnOpenRecDir);
    pLayBtns->addWidget(m_pBtnCapture);
    pLayBtns->addWidget(m_pBtnRestore);
    pLayBtns->addWidget(m_pBtnRestoreAll);
    pLayBtns->addWidget(m_pBtnClose);
    pLayBtns->addStretch(1);
    pLayBtns->addWidget(pBtnRecDir);
    pLay->addWidget(pRow);

    // 1s 刷新 + 管理器变化即时刷新。
    auto* pTimer = new QTimer(this);
    pTimer->setInterval(1000);
    connect(pTimer, &QTimer::timeout, this, &CSandboxDialog::OnRefresh);
    pTimer->start();
    if (m_pMgr != nullptr) {
        connect(m_pMgr, &CSandboxManager::sigChanged, this, &CSandboxDialog::OnRefresh);
    }
    OnRefresh();
}

void CSandboxDialog::RefreshTable() {
    const QList<CSandboxManager::SRow> aRows =
        (m_pMgr != nullptr) ? m_pMgr->Rows() : QList<CSandboxManager::SRow>();
    m_pTable->setRowCount(aRows.size());
    for (int i = 0; i < aRows.size(); ++i) {
        const CSandboxManager::SRow& st = aRows.at(i);
        RECT rc = {};
        GetWindowRect(st.h, &rc);
        const QStringList aVals = {
            st.sTitle.isEmpty() ? QStringLiteral("(无标题)") : st.sTitle,
            QFileInfo(st.sExe).fileName(),
            QString::number(st.dwPid),
            QStringLiteral("%1x%2").arg(rc.right - rc.left).arg(rc.bottom - rc.top),
            QDateTime::fromMSecsSinceEpoch(st.iGhostAtMs).toString(QStringLiteral("HH:mm:ss")),
            st.bRecording ? QStringLiteral("● 录制中") : QStringLiteral("—")
        };
        for (int c = 0; c < aVals.size(); ++c) {
            auto* pItem = new QTableWidgetItem(aVals.at(c));
            if (c == 1 || c == 2 || c == 3 || c == 4) {
                pItem->setTextAlignment(Qt::AlignCenter);
            }
            pItem->setData(Qt::UserRole,
                           QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(st.h)));
            m_pTable->setItem(i, c, pItem);
        }
    }
    if (m_pMgr != nullptr) {
        m_pLblStatus->setText(QStringLiteral(
            "Agent 启动的 GUI 窗口自动收纳于此（隐藏、点击穿透、不打扰用户键鼠）。"
            "Agent 侧经 MCP 工具 sandbox_* 或 --sandbox 命令操控/截图/查看录像。\n"
            "总开关：%1｜录像：%2（%3 fps）｜在册 %4 个｜录像目录 %5")
            .arg(m_pMgr->IsEnabled() ? QStringLiteral("启用") : QStringLiteral("停用"))
            .arg(m_pMgr->IsRecordingOn() ? QStringLiteral("开") : QStringLiteral("关"))
            .arg(m_pMgr->Fps()).arg(aRows.size()).arg(QDir::toNativeSeparators(m_pMgr->RecordDir())));
    }
}

void CSandboxDialog::OnRefresh() {
    RefreshTable();
}

QList<HWND> CSandboxDialog::SelectedHwnds() const {
    QList<HWND> aOut;
    for (const QModelIndex& stIdx : m_pTable->selectionModel()->selectedRows()) {
        if (const QTableWidgetItem* pItem = m_pTable->item(stIdx.row(), 0)) {
            const qulonglong u = pItem->data(Qt::UserRole).toULongLong();
            if (u != 0) {
                aOut.append(reinterpret_cast<HWND>(static_cast<uintptr_t>(u)));
            }
        }
    }
    return aOut;
}

void CSandboxDialog::OnCapture() {
    if (m_pMgr == nullptr) {
        return;
    }
    const QList<HWND> aSel = SelectedHwnds();
    if (aSel.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("截图"), QStringLiteral("请先选择窗口"));
        return;
    }
    QStringList aSaved;
    for (HWND h : aSel) {
        QJsonObject oArgs;
        oArgs.insert(QStringLiteral("hwnd"),
                     QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(h), 0, 16));
        QString sOut;
        m_pMgr->ExecuteTry(QStringLiteral("capture"), oArgs, sOut);
        aSaved.append(sOut);
    }
    QMessageBox::information(this, QStringLiteral("截图"), aSaved.join(QChar('\n')));
    RefreshTable();
}

void CSandboxDialog::OnRestore() {
    if (m_pMgr == nullptr) {
        return;
    }
    const QList<HWND> aSel = SelectedHwnds();
    for (HWND h : aSel) {
        m_pMgr->RestoreWindowEx(h);
    }
    RefreshTable();
}

void CSandboxDialog::OnRestoreAll() {
    if (m_pMgr == nullptr) {
        return;
    }
    QString sOut;
    m_pMgr->ExecuteTry(QStringLiteral("restore_all"), QJsonObject(), sOut);
    RefreshTable();
}

void CSandboxDialog::OnCloseWin() {
    if (m_pMgr == nullptr) {
        return;
    }
    const QList<HWND> aSel = SelectedHwnds();
    if (aSel.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("关闭窗口"),
            QStringLiteral("确定关闭选中的 %1 个沙箱窗口吗？").arg(aSel.size()))
            != QMessageBox::Yes) {
        return;
    }
    for (HWND h : aSel) {
        QJsonObject oArgs;
        oArgs.insert(QStringLiteral("hwnd"),
                     QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(h), 0, 16));
        QString sOut;
        m_pMgr->ExecuteTry(QStringLiteral("close"), oArgs, sOut);
    }
    RefreshTable();
}

void CSandboxDialog::OnOpenRecDir() {
    if (m_pMgr == nullptr) {
        return;
    }
    QDir().mkpath(m_pMgr->RecordDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_pMgr->RecordDir()));
}
