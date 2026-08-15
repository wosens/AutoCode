#include "CTitleBar.h"

#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QFont>
#include <QIcon>
#include <QPoint>
#include <QRect>

CTitleBar::CTitleBar(QWidget* a_pParent)
    : QWidget(a_pParent) {
    // 固定一行高（logical px，DPI 友好）。布局：标题 + 弹簧 + 三按钮。
    setFixedHeight(36);
    // QWidget 子类用 stylesheet 设 background 默认不绘制——必须开启 WA_StyledBackground，
    // 否则下述 #1f1f1f 深色背景不生效，标题栏透明露出父级浅色底，白字反而不清晰。
    setAttribute(Qt::WA_StyledBackground, true);

    auto* pH = new QHBoxLayout(this);
    pH->setContentsMargins(10, 0, 4, 0);
    pH->setSpacing(0);

    // FR-10：左上角应用图标（固定小尺寸，放在标题文本最左）。
    m_pIcon = new QLabel(this);
    m_pIcon->setPixmap(QIcon(QStringLiteral(":/icons/app.ico")).pixmap(20, 20));
    m_pIcon->setFixedSize(24, 24);
    m_pIcon->setScaledContents(true);
    pH->addWidget(m_pIcon);
    pH->addSpacing(6);

    m_pLblTitle = new QLabel(QStringLiteral("AutoCode"), this);
    QFont fntTitle = m_pLblTitle->font();
    fntTitle.setPointSize(11);
    fntTitle.setBold(true);
    m_pLblTitle->setFont(fntTitle);
    pH->addWidget(m_pLblTitle);
    pH->addStretch(1);

    const auto fnMakeBtn = [this](const QString& a_sText, const QString& a_sTip) {
        QToolButton* pBtn = new QToolButton(this);
        pBtn->setText(a_sText);
        pBtn->setToolTip(a_sTip);
        pBtn->setAutoRaise(true);
        pBtn->setFixedSize(46, 32);
        QFont f = pBtn->font();
        f.setPointSize(12);
        f.setBold(true);
        pBtn->setFont(f);
        return pBtn;
    };

    m_pBtnMin   = fnMakeBtn(QStringLiteral("—"),   QStringLiteral("最小化"));
    m_pBtnMax   = fnMakeBtn(QStringLiteral("❐"),   QStringLiteral("最大化"));
    m_pBtnClose = fnMakeBtn(QStringLiteral("✕"),   QStringLiteral("关闭"));

    pH->addWidget(m_pBtnMin);
    pH->addWidget(m_pBtnMax);
    pH->addWidget(m_pBtnClose);

    // 整条标题栏浅色背景 + 深色文字（与浅色主体统一），底部 1px 分隔线区分内容区；
    // 关闭按钮 hover 红（贴近 Windows 关闭键惯例）。
    setStyleSheet(QStringLiteral(
        "CTitleBar { background: #ffffff; border-bottom: 1px solid #e0e0e0; }"
        "QLabel { color: #1f1f1f; }"
        "QToolButton { color: #333333; border: none; }"
        "QToolButton:hover { background: #e5e5e5; }"
        "QToolButton:pressed { background: #d4d4d4; }"
        "QToolButton#close:hover { background: #e81123; color: #ffffff; }"
        "QToolButton#close:pressed { background: #f1707a; color: #ffffff; }"));

    m_pBtnClose->setObjectName(QStringLiteral("close"));

    connect(m_pBtnMin,   &QToolButton::clicked, this, &CTitleBar::sigMinimize);
    connect(m_pBtnMax,   &QToolButton::clicked, this, &CTitleBar::sigMaximizeToggle);
    connect(m_pBtnClose, &QToolButton::clicked, this, &CTitleBar::sigClose);

    SetMaximized(false);
}

void CTitleBar::SetMaximized(bool a_bMaximized) {
    // R6 性能保护：窗口态未变直接早退，避免 resizeEvent 兜底高频调用时反复 setText 抖动。
    if (m_bMaximized == a_bMaximized) {
        return;
    }
    m_bMaximized = a_bMaximized;
    if (m_pBtnMax != nullptr) {
        // FR-2（Q1 默认=a 直接互换）：最大化态显示 □、正常态显示 ❐（与旧方向反向）。
        m_pBtnMax->setText(a_bMaximized ? QStringLiteral("□") : QStringLiteral("❐"));
        m_pBtnMax->setToolTip(a_bMaximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
    }
}

CTitleBar::EHitZone CTitleBar::HitTest(const QPoint& a_stScreenPos) const {
    // FR-3：标题栏屏幕坐标矩形判定。先排除整体不在标题栏；再排按钮区（return false 交还 Qt）；
    //        其余空白为 Caption（可拖拽 + 双击最大化）。只做坐标比较，无 widget 树遍历。
    if (!rect().contains(mapFromGlobal(a_stScreenPos))) {
        return HitNone;
    }
    const auto fnInBtn = [a_stScreenPos](const QToolButton* a_pBtn) {
        return a_pBtn != nullptr &&
               a_pBtn->rect().contains(a_pBtn->mapFromGlobal(a_stScreenPos));
    };
    if (fnInBtn(m_pBtnMin) || fnInBtn(m_pBtnMax) || fnInBtn(m_pBtnClose)) {
        return HitButton;
    }
    return HitCaption;
}
