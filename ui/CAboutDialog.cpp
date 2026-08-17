#include "CAboutDialog.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QIcon>
#include <QFont>
#include <core/CVersion.h>

CAboutDialog::CAboutDialog(QWidget* a_pParent) : QDialog(a_pParent) {
    setWindowTitle(QStringLiteral("关于 AutoCode"));
    setModal(true);
    setMinimumWidth(400);

    // —— 上部：左应用图标 + 右（应用名 / 版本号 / 简介）——
    auto* pLblIcon = new QLabel(this);
    pLblIcon->setPixmap(QIcon(QStringLiteral(":/icons/app.ico")).pixmap(64, 64));
    pLblIcon->setFixedSize(72, 72);

    auto* pLblName = new QLabel(QStringLiteral("AutoCode"), this);
    QFont fntName = pLblName->font();
    fntName.setPointSize(18);
    fntName.setBold(true);
    pLblName->setFont(fntName);

    auto* pLblVer = new QLabel(AutoCodeVersionText(), this);
    QFont fntVer = pLblVer->font();
    fntVer.setPointSize(10);
    pLblVer->setFont(fntVer);
    pLblVer->setStyleSheet(QStringLiteral("color: #8a8a8a;"));   // 与标题栏版本号同款弱化

    auto* pLblDesc = new QLabel(QStringLiteral("AI 驱动的自动化开发任务编排工具\n基于 Claude Code 的多阶段流水线执行与评审"), this);
    pLblDesc->setStyleSheet(QStringLiteral("color: #505050;"));

    auto* pVInfo = new QVBoxLayout();
    pVInfo->setSpacing(4);
    pVInfo->addWidget(pLblName);
    pVInfo->addWidget(pLblVer);
    pVInfo->addSpacing(6);
    pVInfo->addWidget(pLblDesc);

    auto* pHTop = new QHBoxLayout();
    pHTop->setSpacing(16);
    pHTop->addWidget(pLblIcon, 0, Qt::AlignTop);
    pHTop->addLayout(pVInfo, 1);

    // —— 中部：凹陷分隔线 ——
    auto* pLine = new QFrame(this);
    pLine->setFrameShape(QFrame::HLine);
    pLine->setFrameShadow(QFrame::Sunken);

    // —— 下部：作者 / 开发时间 / 版权信息行 ——
    auto* pLblAuthor = new QLabel(QStringLiteral("作者：梅文海"), this);
    auto* pLblDate   = new QLabel(QStringLiteral("开发时间：2026 年 8 月"), this);
    auto* pLblCopy   = new QLabel(QStringLiteral("版权所有 © 2026 梅文海，保留所有权利。"), this);
    for (QLabel* pLbl : {pLblAuthor, pLblDate, pLblCopy}) {
        pLbl->setStyleSheet(QStringLiteral("color: #333333;"));
    }
    auto* pVMeta = new QVBoxLayout();
    pVMeta->setSpacing(3);
    pVMeta->addWidget(pLblAuthor);
    pVMeta->addWidget(pLblDate);
    pVMeta->addWidget(pLblCopy);

    // —— 右下确定按钮 ——
    auto* pBtnOk = new QPushButton(QStringLiteral("确定"), this);
    pBtnOk->setDefault(true);
    pBtnOk->setMinimumWidth(88);
    connect(pBtnOk, &QPushButton::clicked, this, &QDialog::accept);
    auto* pHBtn = new QHBoxLayout();
    pHBtn->addStretch(1);
    pHBtn->addWidget(pBtnOk);

    auto* pVMain = new QVBoxLayout(this);
    pVMain->setContentsMargins(24, 22, 24, 18);
    pVMain->setSpacing(12);
    pVMain->addLayout(pHTop);
    pVMain->addWidget(pLine);
    pVMain->addLayout(pVMeta);
    pVMain->addStretch(1);
    pVMain->addLayout(pHBtn);
}
