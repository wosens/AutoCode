#include "CAboutDialog.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QIcon>
#include <QFont>
#include <core/CVersion.h>
#include "ui/CMenuIcon.h"

// 「关于」对话框（2026.10.16 v4.4.386 整版重设计——饱满/高级审美）：
//   ┌─────────────────────────────────────────────┐
//   │ 深蓝渐变品牌横幅：应用图标 + 名称 + 版本徽章   │
//   │               + 一句定位 slogan               │
//   ├─────────────────────────────────────────────┤
//   │ Agent 后端行：五品牌胶囊（品牌色小图标+名）    │
//   │ 核心能力：2×3 特性卡片（图标+标题+一句话）    │
//   ├─────────────────────────────────────────────┤
//   │ 作者 · 开发时间 · 版权           [ 确定 ]     │
//   └─────────────────────────────────────────────┘
// 沿用标准对话框框体（可拖动、系统阴影），不做无边框自绘；
// 正文 11pt 基准（应用名 26pt / 分节标题 11pt 粗 / 卡片副文案 9pt）。

namespace {

// 品牌胶囊（图标 + 后端名）
QWidget* MakeBrandChip(const QString& a_sIconKey20, const QString& a_sIconKey40,
                       const QString& a_sName) {
    QIcon oIcon(QStringLiteral(":/icons/brands/%1-20.png").arg(a_sIconKey20));
    oIcon.addFile(QStringLiteral(":/icons/brands/%1-40.png").arg(a_sIconKey40));
    auto* pLblIcon = new QLabel(nullptr);
    pLblIcon->setPixmap(oIcon.pixmap(QSize(20, 20)));
    auto* pLblName = new QLabel(a_sName, nullptr);
    QFont fnt = pLblName->font();
    fnt.setPointSize(9);
    fnt.setBold(true);
    pLblName->setFont(fnt);
    pLblName->setStyleSheet(QStringLiteral("color:#334155; background:transparent;"));
    auto* pH = new QHBoxLayout();
    pH->setContentsMargins(10, 6, 12, 6);
    pH->setSpacing(6);
    pH->addWidget(pLblIcon, 0, Qt::AlignVCenter);
    pH->addWidget(pLblName, 0, Qt::AlignVCenter);
    auto* pChip = new QWidget(nullptr);
    pChip->setLayout(pH);
    pChip->setStyleSheet(QStringLiteral(
        "QWidget { background:#ffffff; border:1px solid #E2E8F0; border-radius:16px; }"));
    return pChip;
}

// 特性卡片（图标 + 标题 + 一句话）
QWidget* MakeFeatureCard(const QString& a_sIcon, const QString& a_sTitle,
                          const QString& a_sDesc) {
    auto* pLblIcon = new QLabel(nullptr);
    pLblIcon->setPixmap(MenuIcon(a_sIcon).pixmap(QSize(24, 24)));

    auto* pLblTitle = new QLabel(a_sTitle, nullptr);
    QFont fntT = pLblTitle->font();
    fntT.setPointSize(11);
    fntT.setBold(true);
    pLblTitle->setFont(fntT);
    pLblTitle->setStyleSheet(QStringLiteral("color:#1E293B; background:transparent;"));

    auto* pLblDesc = new QLabel(a_sDesc, nullptr);
    pLblDesc->setWordWrap(true);
    QFont fntD = pLblDesc->font();
    fntD.setPointSize(9);
    pLblDesc->setFont(fntD);
    pLblDesc->setStyleSheet(QStringLiteral("color:#64748B; background:transparent;"));

    auto* pV = new QVBoxLayout();
    pV->setContentsMargins(14, 12, 14, 12);
    pV->setSpacing(4);
    pV->addWidget(pLblIcon, 0, Qt::AlignLeft);
    pV->addSpacing(2);
    pV->addWidget(pLblTitle, 0, Qt::AlignLeft);
    pV->addWidget(pLblDesc, 0, Qt::AlignLeft);
    pV->addStretch(1);

    auto* pCard = new QWidget(nullptr);
    pCard->setLayout(pV);
    pCard->setMinimumHeight(108);
    pCard->setStyleSheet(QStringLiteral(
        "QWidget { background:#ffffff; border:1px solid #E2E8F0; border-radius:10px; }"));
    return pCard;
}

// 分节小标题（左侧品牌蓝竖条 + 文本）
QWidget* MakeSectionLabel(const QString& a_sText) {
    auto* pBar = new QWidget(nullptr);
    pBar->setFixedSize(3, 13);
    pBar->setStyleSheet(QStringLiteral("background:#2563EB; border-radius:1px;"));
    auto* pLbl = new QLabel(a_sText, nullptr);
    QFont fnt = pLbl->font();
    fnt.setPointSize(11);
    fnt.setBold(true);
    pLbl->setFont(fnt);
    pLbl->setStyleSheet(QStringLiteral("color:#0F172A; background:transparent;"));
    auto* pH = new QHBoxLayout();
    pH->setContentsMargins(0, 0, 0, 0);
    pH->setSpacing(7);
    pH->addWidget(pBar);
    pH->addWidget(pLbl);
    pH->addStretch(1);
    auto* pW = new QWidget(nullptr);
    pW->setLayout(pH);
    pW->setStyleSheet(QStringLiteral("background:transparent;"));
    return pW;
}

} // namespace

CAboutDialog::CAboutDialog(QWidget* a_pParent) : QDialog(a_pParent) {
    setWindowTitle(QStringLiteral("关于 AutoCode"));
    setModal(true);
    setFixedWidth(560);
    setStyleSheet(QStringLiteral("QDialog { background:#F8FAFC; }"));

    // —— 顶部品牌横幅（深蓝渐变 + 图标 + 名称 + 版本徽章 + slogan）——
    auto* pBanner = new QWidget(this);
    pBanner->setObjectName(QStringLiteral("aboutBanner"));
    pBanner->setStyleSheet(QStringLiteral(
        "#aboutBanner { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,"
        " stop:0 #1E3A8A, stop:0.55 #2563EB, stop:1 #3B82F6);"
        " border-top-left-radius:8px; border-top-right-radius:8px; }"));

    auto* pLblIcon = new QLabel(pBanner);
    pLblIcon->setPixmap(QIcon(QStringLiteral(":/icons/app.ico")).pixmap(76, 76));

    auto* pLblName = new QLabel(QStringLiteral("AutoCode"), pBanner);
    QFont fntName = pLblName->font();
    fntName.setPointSize(26);
    fntName.setBold(true);
    pLblName->setFont(fntName);
    pLblName->setStyleSheet(QStringLiteral("color:#ffffff; background:transparent;"));

    auto* pLblVer = new QLabel(AutoCodeVersionText(), pBanner);
    QFont fntVer = pLblVer->font();
    fntVer.setPointSize(9);
    fntVer.setBold(true);
    pLblVer->setFont(fntVer);
    pLblVer->setStyleSheet(QStringLiteral(
        "color:#DBEAFE; background:rgba(255,255,255,0.16);"
        " border:1px solid rgba(255,255,255,0.28); border-radius:9px;"
        " padding:2px 10px;"));

    auto* pLblSlogan = new QLabel(
        QStringLiteral("AI 驱动的多后端自动化开发平台\n需求 · 设计 · 开发 · 审查 · 验证，一条流水线全自动推进"), pBanner);
    QFont fntSlogan = pLblSlogan->font();
    fntSlogan.setPointSize(10);
    pLblSlogan->setFont(fntSlogan);
    pLblSlogan->setStyleSheet(QStringLiteral(
        "color:rgba(255,255,255,0.88); background:transparent;"));

    auto* pVName = new QVBoxLayout();
    pVName->setSpacing(5);
    pVName->addWidget(pLblName);
    pVName->addWidget(pLblVer);
    pVName->addSpacing(3);
    pVName->addWidget(pLblSlogan);

    auto* pHBanner = new QHBoxLayout(pBanner);
    pHBanner->setContentsMargins(28, 24, 28, 24);
    pHBanner->setSpacing(22);
    pHBanner->addWidget(pLblIcon, 0, Qt::AlignVCenter);
    pHBanner->addLayout(pVName, 1);

    // —— Agent 后端行 ——
    auto* pRowBrand = new QHBoxLayout();
    pRowBrand->setSpacing(8);
    for (QWidget* pChip : {
             MakeBrandChip(QStringLiteral("vd-claude"), QStringLiteral("vd-claude"), QStringLiteral("Claude Code")),
             MakeBrandChip(QStringLiteral("vd-codex"), QStringLiteral("vd-codex"), QStringLiteral("Codex")),
             MakeBrandChip(QStringLiteral("vd-pi"), QStringLiteral("vd-pi"), QStringLiteral("PiAgent")),
             MakeBrandChip(QStringLiteral("vd-dsh"), QStringLiteral("vd-dsh"), QStringLiteral("DeepSeek")),
             MakeBrandChip(QStringLiteral("vd-hermes"), QStringLiteral("vd-hermes"), QStringLiteral("Hermes")),
         }) {
        pRowBrand->addWidget(pChip);
    }
    pRowBrand->addStretch(1);

    // —— 核心能力 2×3 卡片 ——
    QGridLayout* pGrid = new QGridLayout();
    pGrid->setSpacing(8);
    struct SFeat { const char* sIcon; const char* sTitle; const char* sDesc; };
    const SFeat aFeats[] = {
        { "ow-rerun",    "任务流水线",     "需求分析到验证的多阶段编排，失败自动重试与回退" },
        { "ow-chat",     "多助手并行",     "多助手独立会话与上下文，随时插入指令协同开发" },
        { "ow-diff",     "增量备份回退",   "每轮改码增量快照，任意历史版本一键查看与回退" },
        { "ow-review",   "代码审查",       "内置审查服务输出改进意见，按建议一键修复" },
        { "ow-shield",   "测试沙箱",       "窗口隔离与消息级自动化验证，录制回放测试过程" },
        { "ow-explorer", "工程管理",       "内置资源管理器、任务历史与最近打开，工程全景在握" },
    };
    for (int i = 0; i < 6; ++i) {
        pGrid->addWidget(MakeFeatureCard(QString::fromUtf8(aFeats[i].sIcon),
                                         QString::fromUtf8(aFeats[i].sTitle),
                                         QString::fromUtf8(aFeats[i].sDesc)),
                         i / 3, i % 3);
    }
    pGrid->setColumnStretch(0, 1);
    pGrid->setColumnStretch(1, 1);
    pGrid->setColumnStretch(2, 1);

    // —— 底部元信息 + 确定按钮 ——
    auto* pLblMeta = new QLabel(QStringLiteral(
        "作者：梅文海    ·    开发时间：2026 年 8 月    ·    基于 Qt 6.8 / C++ 构建"), this);
    QFont fntMeta = pLblMeta->font();
    fntMeta.setPointSize(9);
    pLblMeta->setFont(fntMeta);
    pLblMeta->setStyleSheet(QStringLiteral("color:#64748B; background:transparent;"));

    auto* pLblCopy = new QLabel(QStringLiteral("版权所有 © 2026 梅文海，保留所有权利。"), this);
    pLblCopy->setFont(fntMeta);
    pLblCopy->setStyleSheet(QStringLiteral("color:#94A3B8; background:transparent;"));

    auto* pBtnOk = new QPushButton(QStringLiteral("确定"), this);
    pBtnOk->setDefault(true);
    pBtnOk->setCursor(Qt::PointingHandCursor);
    pBtnOk->setMinimumSize(96, 30);
    pBtnOk->setStyleSheet(QStringLiteral(
        "QPushButton { background:#2563EB; color:#ffffff; border:none;"
        " border-radius:6px; font-weight:bold; }"
        "QPushButton:hover { background:#1D4ED8; }"
        "QPushButton:pressed { background:#1E40AF; }"));
    connect(pBtnOk, &QPushButton::clicked, this, &QDialog::accept);

    auto* pVBtn = new QVBoxLayout();
    pVBtn->setSpacing(2);
    pVBtn->addWidget(pLblMeta, 0, Qt::AlignRight);
    pVBtn->addWidget(pLblCopy, 0, Qt::AlignRight);
    auto* pHFoot = new QHBoxLayout();
    pHFoot->setSpacing(16);
    pHFoot->addLayout(pVBtn, 1);
    pHFoot->addWidget(pBtnOk, 0, Qt::AlignBottom);

    // —— 总装 ——
    auto* pBody = new QWidget(this);
    pBody->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* pVBody = new QVBoxLayout(pBody);
    pVBody->setContentsMargins(20, 14, 20, 0);
    pVBody->setSpacing(8);
    pVBody->addWidget(MakeSectionLabel(QStringLiteral("Agent 后端")));
    pVBody->addLayout(pRowBrand);
    pVBody->addSpacing(6);
    pVBody->addWidget(MakeSectionLabel(QStringLiteral("核心能力")));
    pVBody->addLayout(pGrid);

    auto* pVMain = new QVBoxLayout(this);
    pVMain->setContentsMargins(12, 12, 12, 14);
    pVMain->setSpacing(12);
    pVMain->addWidget(pBanner);
    pVMain->addWidget(pBody, 1);
    pVMain->addLayout(pHFoot);
}
