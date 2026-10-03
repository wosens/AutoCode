#include "CRecentChipsPanel.h"
// ============================================================================
// 「最近打开」按钮面板实现（设计定稿见头注；2026.09.26 v3.1.239）。
// 自绘按钮 CRecentChipButton 定义在本文件（AUTOMOC：文末 #include .moc）。
// ============================================================================
#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

#include <QFontMetrics>
#include <QGuiApplication>

// —— 面板几何常量（11 号字体口径）——
static constexpr int kChipH      = 30;   // 按钮高
static constexpr int kChipHPad   = 10;   // 按钮左右内边距
static constexpr int kSpacing    = 6;    // 按钮间 / 行间距
static constexpr int kHostMargin = 8;    // 滚动内容区边距
static constexpr int kStatusH    = 26;   // 状态栏高
static constexpr int kMaxVisRows = 9;    // 可视行数封顶（每行 4~6 个 → 约 40~54 可视）
static constexpr int kPanelW     = 560;  // 面板固定宽（钳到屏宽-40）

// ============================================================================
// CRecentChipButton —— 自绘按钮：平时底色=打开次数渐变色，悬停=蓝底白字 +
//   假粗体（2x2 四次绘制，字号/字宽不变 → 布局零抖动，用户方案 4）。
// ============================================================================
class CRecentChipButton : public QWidget {
    Q_OBJECT
public:
    CRecentChipButton(const QString& a_sName, const QString& a_sPath,
                      int a_iOpens, bool a_bMore, QWidget* a_pParent)
        : QWidget(a_pParent), m_sName(a_sName), m_sPath(a_sPath),
          m_iOpens(a_iOpens), m_bMore(a_bMore) {
        setFixedHeight(kChipH);
        setCursor(Qt::PointingHandCursor);
        if (a_bMore) {
            setToolTip(QStringLiteral("打开完整列表（搜索 / 多选删除 / 打开计数）"));
        } else {
            setToolTip(QStringLiteral("路径：%1\n已打开 %2 次（每次成功打开工程时累加 1）")
                           .arg(a_sPath)
                           .arg(a_iOpens));
        }
    }

signals:
    void Clicked();
    void HoverIn(const QString& sPath);
    void HoverOut();

protected:
    void enterEvent(QEnterEvent* a_pEv) override {
        QWidget::enterEvent(a_pEv);
        m_bHover = true;
        update();
        emit HoverIn(m_sPath);
    }
    void leaveEvent(QEvent* a_pEv) override {
        QWidget::leaveEvent(a_pEv);
        m_bHover = false;
        update();
        emit HoverOut();
    }
    void mouseReleaseEvent(QMouseEvent* a_pEv) override {
        QWidget::mouseReleaseEvent(a_pEv);
        if (rect().contains(a_pEv->pos())) {
            emit Clicked();   // 菜单语义：释放触发（按压拖出可取消）
        }
    }
    void paintEvent(QPaintEvent* a_pEv) override {
        QWidget::paintEvent(a_pEv);
        QPainter stP(this);
        stP.setRenderHint(QPainter::TextAntialiasing, true);
        stP.setPen(Qt::NoPen);
        stP.setBrush(m_bHover ? QColor(0x25, 0x63, 0xeb)                // 悬停蓝（全局主色）
                              : CRecentChipsPanel::ChipColorForCount(m_iOpens));
        stP.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 4, 4);
        stP.setPen(m_bHover ? Qt::white : QColor(0x1f, 0x29, 0x37));
        // -2：假粗体 2x2 偏移（右/下各 +1px）的绘制余量
        const QString sTxt = stP.fontMetrics().elidedText(
            m_sName, Qt::ElideRight, width() - 2 * kChipHPad - 2);
        const QRect rcTxt = rect().adjusted(kChipHPad, 0, -kChipHPad - 2, 0);
        if (m_bHover) {
            // 假粗体（用户方案 4）：2x2 范围绘制四次——视觉加粗而字号/字宽不变，
            //   悬停不引起任何布局抖动（真 setFont(Bold) 会变宽挤压同行）。
            for (int iDx = 0; iDx <= 1; ++iDx) {
                for (int iDy = 0; iDy <= 1; ++iDy) {
                    stP.drawText(rcTxt.translated(iDx, iDy),
                                 Qt::AlignVCenter | Qt::AlignLeft, sTxt);
                }
            }
        } else {
            stP.drawText(rcTxt, Qt::AlignVCenter | Qt::AlignLeft, sTxt);
        }
    }

private:
    QString m_sName;
    QString m_sPath;
    int     m_iOpens = 1;
    bool    m_bMore  = false;
    bool    m_bHover = false;
};

// ============================================================================
// CRecentChipsPanel
// ============================================================================
CRecentChipsPanel::CRecentChipsPanel(FOpenProject a_fnOpen, FMore a_fnMore,
                                     QWidget* a_pParent)
    : QWidget(a_pParent), m_fnOpen(std::move(a_fnOpen)), m_fnMore(std::move(a_fnMore)) {
    QFont stF = font();
    stF.setPointSize(11);   // 全局 UI 11 号字体
    setFont(stF);

    m_pScroll = new QScrollArea(this);
    m_pScroll->setFrameShape(QFrame::NoFrame);
    m_pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScroll->setWidgetResizable(true);
    m_pHost = new QWidget();
    m_pHost->setContentsMargins(kHostMargin, kHostMargin, kHostMargin, kHostMargin);
    m_pHost->installEventFilter(this);   // 视口宽变化（滚动条出现/消失）→ 重排
    m_pScroll->setWidget(m_pHost);

    m_pLblStat = new QLabel(this);
    m_pLblStat->setFixedHeight(kStatusH);
    m_pLblStat->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_pLblStat->setTextInteractionFlags(Qt::NoTextInteraction);
    m_pLblStat->setStyleSheet(QStringLiteral(
        "background:#f3f4f6;border-top:1px solid #e0e3e8;"
        "padding:1px 8px;color:#4b5563;"));

    auto* pLay = new QVBoxLayout(this);
    pLay->setContentsMargins(0, 0, 0, 0);
    pLay->setSpacing(0);
    pLay->addWidget(m_pScroll, 1);
    pLay->addWidget(m_pLblStat);
}

void CRecentChipsPanel::Refresh(const QStringList& a_aPaths, const QList<int>& a_aOpens) {
    for (CRecentChipButton* pChip : m_aChips) {
        // 2026.09.28 deleteLater 替代 delete：Refresh 可能被芯片 Clicked 的同步调用链
        //   （点芯片→开工程→MRU 回写→刷新）触发，直接 delete 会销毁正在发射信号的
        //   sender（mouseReleaseEvent 仍在栈上），返回后访问已释放内存（UAF/崩溃）。
        pChip->deleteLater();
    }
    m_aChips.clear();
    m_aNames.clear();

    auto* pMore = new CRecentChipButton(QStringLiteral("更多..."), QString(), 0, true, m_pHost);
    connect(pMore, &CRecentChipButton::Clicked, this, [this]() {
        if (m_fnMore) { m_fnMore(); }
    });
    connect(pMore, &CRecentChipButton::HoverIn, this, [this](const QString&) {
        m_pLblStat->setText(QStringLiteral("打开完整列表（搜索 / 多选删除 / 打开计数）"));
        m_pLblStat->setToolTip(QString());
    });
    connect(pMore, &CRecentChipButton::HoverOut, this, [this]() {
        m_pLblStat->setText(m_sStatDefault);
        m_pLblStat->setToolTip(QString());
    });
    m_aChips.append(pMore);
    m_aNames.append(QStringLiteral("更多..."));

    for (int i = 0; i < a_aPaths.size(); ++i) {
        const QString& sPath = a_aPaths.at(i);
        const int iOpens = (i < a_aOpens.size() && a_aOpens.at(i) > 0)
                               ? a_aOpens.at(i) : 1;
        const QString sName = QDir(sPath).dirName();
        auto* pChip = new CRecentChipButton(sName, sPath, iOpens, false, m_pHost);
        connect(pChip, &CRecentChipButton::Clicked, this, [this, sPath]() {
            if (m_fnOpen) { m_fnOpen(sPath); }
        });
        connect(pChip, &CRecentChipButton::HoverIn, this, [this](const QString& sP) {
            m_pLblStat->setText(m_pLblStat->fontMetrics().elidedText(
                sP, Qt::ElideMiddle, m_pLblStat->width() - 16));
            m_pLblStat->setToolTip(sP);   // 状态栏省略时 tooltip 兜底全文
        });
        connect(pChip, &CRecentChipButton::HoverOut, this, [this]() {
            m_pLblStat->setText(m_sStatDefault);
            m_pLblStat->setToolTip(QString());
        });
        m_aChips.append(pChip);
        m_aNames.append(sName);
    }

    m_sStatDefault = QStringLiteral("共 %1 个工程 · 悬停按钮显示路径 · 滚轮查看更多")
                         .arg(a_aPaths.size());
    m_pLblStat->setText(m_sStatDefault);

    // 固定尺寸（用户规则 5）：宽=固定值（钳屏宽），高按行数封顶（约 40~54 个可视）。
    int iW = kPanelW;
    if (QScreen* pSc = screen(); pSc != nullptr) {
        iW = qMin(iW, pSc->availableGeometry().width() - 40);
    }
    const int iRows = LayoutChips(m_aNames, QFontMetrics(font()),
                                  iW - 2 * kHostMargin, kSpacing, kChipHPad).size();
    const int iNatH = 2 * kHostMargin + iRows * kChipH + (iRows > 0 ? (iRows - 1) * kSpacing : 0)
                      + kStatusH;
    const int iCapH = 2 * kHostMargin + kMaxVisRows * kChipH + (kMaxVisRows - 1) * kSpacing
                      + kStatusH;
    setFixedSize(iW, qMin(iNatH, iCapH));
    Relayout();
}

QString CRecentChipsPanel::StatusText() const {
    return m_pLblStat != nullptr ? m_pLblStat->text() : QString();
}

QWidget* CRecentChipsPanel::ChipAt(int a_iIdx) const {
    return m_aChips.value(a_iIdx, nullptr);
}

bool CRecentChipsPanel::eventFilter(QObject* a_pObj, QEvent* a_pEv) {
    if (a_pObj == m_pHost && a_pEv->type() == QEvent::Resize) {
        Relayout();   // 视口宽变化（滚动条出现/消失）→ 按实宽重排
    }
    return QWidget::eventFilter(a_pObj, a_pEv);
}

void CRecentChipsPanel::Relayout() {
    if (m_pHost == nullptr) {
        return;
    }
    const int iAvail = m_pHost->width() - m_pHost->contentsMargins().left()
                                        - m_pHost->contentsMargins().right();
    const QList<SChipRow> aRows = LayoutChips(m_aNames, QFontMetrics(font()),
                                              iAvail, kSpacing, kChipHPad);
    const int iML = m_pHost->contentsMargins().left();
    int iY = m_pHost->contentsMargins().top();
    for (const SChipRow& stRow : aRows) {
        int iX = iML;
        for (int k = 0; k < stRow.aBtnW.size(); ++k) {
            CRecentChipButton* pChip = m_aChips.value(stRow.iFirst + k, nullptr);
            if (pChip != nullptr) {
                pChip->setGeometry(iX, iY, stRow.aBtnW.at(k), kChipH);
            }
            iX += stRow.aBtnW.at(k) + kSpacing;
        }
        iY += kChipH + kSpacing;
    }
    m_pHost->setMinimumHeight(iY - kSpacing + m_pHost->contentsMargins().bottom());
}

QList<CRecentChipsPanel::SChipRow> CRecentChipsPanel::LayoutChips(
        const QStringList& a_aNames, const QFontMetrics& a_stFm,
        int a_iAvailW, int a_iSpacing, int a_iHPad) {
    QList<SChipRow> aRows;
    if (a_iAvailW <= 0) {
        return aRows;
    }
    const int iCap = qMax(1, a_iAvailW / 2);   // 用户规则 3：单按钮宽上限=客户区一半
    const int iN = a_aNames.size();
    int i = 0;
    while (i < iN) {
        // 1) 贪心装行（保持 MRU 顺序）：+2 = 假粗体 2x2 偏移余量；
        //    iSumPure 只计最小宽（不含行内间距，间距在预算侧统一扣）
        QList<int> aMinW;
        int iSumPure = 0;
        while (i + aMinW.size() < iN) {
            const int iMinW = a_stFm.horizontalAdvance(a_aNames.at(i + aMinW.size()))
                              + 2 * a_iHPad + 2;
            if (!aMinW.isEmpty()
                    && iSumPure + iMinW + a_iSpacing * aMinW.size() > a_iAvailW) {
                break;
            }
            iSumPure += iMinW;
            aMinW.append(iMinW);
        }
        const int n = aMinW.size();
        // 2) 行内富余按最小宽比例分配（标题完整显示 + 行满占满整宽）
        const int iInner = a_iAvailW - a_iSpacing * (n - 1);   // 扣行内间距后的内容预算
        const int iExtra = qMax(0, iInner - iSumPure);
        QList<int> aW;
        int iAlloc = 0;
        int iMaxIdx = 0;
        for (int k = 0; k < n; ++k) {
            const int iWk = aMinW.at(k) + (iSumPure > 0 ? iExtra * aMinW.at(k) / iSumPure : 0);
            aW.append(iWk);
            iAlloc += iWk;
            if (aMinW.at(k) > aMinW.at(iMaxIdx)) { iMaxIdx = k; }
        }
        if (n > 0 && iAlloc < iInner) {
            aW[iMaxIdx] += iInner - iAlloc;   // 整数化零头归最宽者（保证行满）
        }
        // 3) 单按钮宽钳到上限（触发时行尾留白，用户规则 3）；超长独行仍可省略号兜底
        for (int k = 0; k < n; ++k) {
            aW[k] = qBound(1, qMin(aW.at(k), iCap), a_iAvailW);
        }
        SChipRow stRow;
        stRow.iFirst = i;
        stRow.aBtnW = aW;
        aRows.append(stRow);
        i += n;
    }
    return aRows;
}

QColor CRecentChipsPanel::ChipColorForCount(int a_iCount) {
    // 用户规则 7：1~5 基色不变 → (5,100] R 升 G/B 降，100 次=(255,0,0) →
    //   (100,255] R 降至 (100,0,0)，255 次=(100,0,0) → >255 恒 (100,0,0)。
    static const QColor k_clBase(225, 230, 238);   // 基色：中性浅灰蓝
    if (a_iCount <= 5) {
        return k_clBase;
    }
    if (a_iCount <= 100) {
        const double fT = double(a_iCount - 5) / 95.0;
        return QColor(k_clBase.red() + int((255 - k_clBase.red()) * fT),
                      int(double(k_clBase.green()) * (1.0 - fT)),
                      int(double(k_clBase.blue()) * (1.0 - fT)));
    }
    if (a_iCount <= 255) {
        const double fT = double(a_iCount - 100) / 155.0;
        return QColor(255 - int(155 * fT), 0, 0);
    }
    return QColor(100, 0, 0);
}

#include "CRecentChipsPanel.moc"
