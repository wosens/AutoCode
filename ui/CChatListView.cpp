#include "CChatListView.h"

#include "CChatDelegate.h"
#include "CChatDocPool.h"
#include "CChatModel.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolTip>

#include <functional>

namespace {

// 复制覆盖层：只读 QTextBrowser，Esc 收起（fnEsc 回调视图）；瞬时交互件，
// 不违背「滚动全程子 QWidget 数=0」（方案 §8：双击 Assistant 才浮一个编辑器）。
// 样式限定 QTextBrowser 选择器（防级联子控件，CMessageBlock 先例）。
class COverlayBrowser : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;
    std::function<void()> fnEsc;

protected:
    void keyPressEvent(QKeyEvent* a_ev) override
    {
        if (a_ev->key() == Qt::Key_Escape) {
            if (fnEsc)
                fnEsc();
            return;
        }
        QTextBrowser::keyPressEvent(a_ev);
    }
};

} // namespace

CChatListView::CChatListView(QWidget* a_pParent)
    : QListView(a_pParent)
{
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setResizeMode(QListView::Adjust);
    setSpacing(10);                          // CMessageView 块间 spacing 口径
    setUniformItemSizes(false);              // 条目高度各异（方案 §6）
    setViewportMargins(12, 12, 12, 12);      // CMessageView 内容 margins 口径
    // 视口白底 + 竖直滚动条 10px（CMessageView QSS 同款；限定 QListView 防级联）
    setStyleSheet(QStringLiteral(
        "QListView { background: #ffffff; border: none; }"
        "QScrollBar:vertical { width: 10px; }"));

    m_pPool = new CChatDocPool(this);
    m_pDelegate = new CChatDelegate(m_pPool, font(), this);
    setItemDelegate(m_pDelegate);

    // —— 接线（唯一 wiring 点，方案 §4.2）——
    m_pPool->fnDocReady = [this](int a_iRow, QTextDocument* a_pDoc) {
        if (m_pDelegate)
            m_pDelegate->OnDocReady(a_iRow, a_pDoc,
                                    m_pModel ? m_pModel->PeekItem(a_iRow) : nullptr);
    };
    m_pDelegate->fnHeightCorrected = [this](int a_iRow, int a_iDeltaY) {
        onHeightCorrected(a_iRow, a_iDeltaY);
    };
    m_pPool->fnFetchText = [this](int a_iRow) -> QString {
        if (m_pModel == nullptr)
            return QString();
        const SDisplayItem* pIt = m_pModel->PeekItem(a_iRow);
        return (pIt != nullptr && pIt->eKind == CMessageBlock::Assistant)
            ? pIt->sText : QString();
    };

    m_pBandTimer = new QTimer(this);
    m_pBandTimer->setSingleShot(true);
    m_pBandTimer->setInterval(16);           // 滚动 16ms 节流 → 视口带材化/逐出
    connect(m_pBandTimer, &QTimer::timeout, this, [this] { updateBand(); });

    // 贴底时新内容自动滚底；用户上翻则不抢滚动（复刻 CMessageView 跟随启发式）
    QScrollBar* pBar = verticalScrollBar();
    connect(pBar, &QScrollBar::valueChanged, this, [this](int val) {
        // 滚动即收覆盖层（与 Esc/点击条目外同口径）：覆盖层持有池 doc，滚动触发
        // OnBand 带外/LRU 容量逐出会 delete 该 doc → 覆盖层悬垂 UAF；且 viewport
        // 子控件不随内容滚动、浮层会错位——收起一并消除
        if (m_pOverlay != nullptr && m_pOverlay->isVisible())
            closeOverlay();
        m_bAtBottom = val >= verticalScrollBar()->maximum() - 4;
        m_pBandTimer->start();               // start() 重启即节流
    });
    connect(pBar, &QScrollBar::rangeChanged, this, [this](int, int iMax) {
        if (m_bAutoScroll && m_bAtBottom)
            verticalScrollBar()->setValue(iMax);
    });
}

void CChatListView::SetModel(CChatModel* a_pModel)
{
    if (m_pModel == a_pModel) {
        return;   // 同模型重复绑定（任务侧共享视图随流换绑）：lambda 连接的
                  //   UniqueConnection 不去重（Qt 对无 == 的仿函数恒新增），防叠加
    }
    if (m_pModel != nullptr)
        disconnect(m_pModel, nullptr, this, nullptr);   // 换模型：断旧模型信号残留
    m_pModel = a_pModel;
    setModel(a_pModel);
    if (a_pModel == nullptr)
        return;
    // 模型 Reset（切任务重载）→ 收覆盖层（其 doc 将被池删除）→ 清池 → 高度缓存全
    // 失效。modelReset 是基类自带信号：模型层与池互不知晓，分层约束保持。
    connect(a_pModel, &QAbstractItemModel::modelReset, this, [this] {
        closeOverlay();
        if (m_pPool)
            m_pPool->Clear();
        if (m_pDelegate)
            m_pDelegate->InvalidateWidth(qMax(0, m_iWidthLast));
    });
    const int iW = viewport()->width();
    if (m_pDelegate && iW > 0) {
        m_iWidthLast = iW;
        m_pDelegate->InvalidateWidth(iW);
    }
    updateBand();                            // 立即材化当前视口带
}

void CChatListView::SetAutoScroll(bool a_bOn)
{
    m_bAutoScroll = a_bOn;
}

void CChatListView::ApplyFont(const QFont& a_oFont)
{
    setFont(a_oFont);                        // 事件/编辑器继承
    if (m_pDelegate != nullptr) {
        m_pDelegate->SetFont(a_oFont);       // 度量基准 + 高度缓存全失效
        if (m_iWidthLast > 0)
            m_pDelegate->InvalidateWidth(m_iWidthLast);   // 惰性重估 + 池重排
    }
}

void CChatListView::ScrollToBottom()
{
    // 先触发执行挂起的布局（QListView 惰性布局下 maximum 可能仍是旧值）
    if (m_pModel != nullptr && m_pModel->rowCount() > 0)
        visualRect(m_pModel->index(m_pModel->rowCount() - 1, 0));
    m_bAtBottom = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

bool CChatListView::JumpToItem(int a_iItemRow)
{
    if (m_pModel == nullptr || a_iItemRow < 0 || a_iItemRow >= m_pModel->rowCount())
        return false;
    closeOverlay();                          // 覆盖层持有池 doc，先收再滚（防悬垂）
    m_bAtBottom = false;                     // 跳转后不再贴底粘滞（ScrollBlockToTop 口径）
    scrollTo(m_pModel->index(a_iItemRow, 0), QAbstractItemView::PositionAtTop);   // 滚到视口顶部
    m_pModel->setData(m_pModel->index(a_iItemRow, 0), true, CChatModel::HighlightRole);
    QTimer::singleShot(1200, this, [this, a_iItemRow] {          // 高亮 1200ms 自清（不落盘）
        if (m_pModel != nullptr && a_iItemRow < m_pModel->rowCount())
            m_pModel->setData(m_pModel->index(a_iItemRow, 0), false, CChatModel::HighlightRole);
    });
    return true;
}

void CChatListView::updateBand()
{
    if (m_pModel == nullptr || m_pModel->rowCount() == 0 || m_pPool == nullptr)
        return;
    const QRect rcVis = viewport()->rect();
    if (rcVis.height() <= 0)
        return;
    QModelIndex idxTop = indexAt(QPoint(2, 2));
    if (!idxTop.isValid()) {
        // 视口顶端落在 margins/空白：从首行兜底（首行必在或模型为空已排除）
        idxTop = m_pModel->index(0, 0);
    }
    QModelIndex idxBot = indexAt(QPoint(2, rcVis.height() - 2));
    if (!idxBot.isValid())
        idxBot = m_pModel->index(m_pModel->rowCount() - 1, 0);
    m_pPool->OnBand(idxTop.row(), idxBot.row(),
                    verticalScrollBar()->value(), rcVis.height());
}

void CChatListView::onHeightCorrected(int a_iRow, int a_iDeltaY)
{
    Q_UNUSED(a_iDeltaY);
    if (m_pModel == nullptr || a_iRow < 0 || a_iRow >= m_pModel->rowCount())
        return;
    const QModelIndex idx = m_pModel->index(a_iRow, 0);
    const int iTopBefore = visualRect(idx).top();      // 旧布局下的条目位置（视口坐标）
    m_pModel->TouchSizeHint(a_iRow);                   // SizeHintRole 失效 → 布局重算
    if (iTopBefore < 0) {
        // 视口上方条目高度变化：重排后把滚动值加上位置增量，抵消整体下移，
        // 视口内容视觉零位移（方案 §7.4 锚定补偿——仅视口上方条目补偿）
        const int iTopAfter = visualRect(idx).top();   // 触发执行挂起的布局（新尺寸生效）
        QScrollBar* pBar = verticalScrollBar();
        pBar->setValue(pBar->value() + (iTopAfter - iTopBefore));
    }
}

void CChatListView::resizeEvent(QResizeEvent* a_ev)
{
    QListView::resizeEvent(a_ev);
    const int iW = viewport()->width();
    if (m_pDelegate != nullptr && iW > 0 && iW != m_iWidthLast) {
        // 宽变即全条目重排：与滚动同口径先收覆盖层（旧几何浮层错位；池宽度失效
        // 虽不删 doc，统一收层口径消除不一致）
        if (m_pOverlay != nullptr && m_pOverlay->isVisible())
            closeOverlay();
        m_iWidthLast = iW;
        m_pDelegate->InvalidateWidth(iW);    // 高度缓存全失效 + 池内 doc 重排回调
    }
}

void CChatListView::openOverlay(const QModelIndex& a_idx)
{
    const SDisplayItem* pIt = m_pModel ? m_pModel->PeekItem(a_idx.row()) : nullptr;
    if (pIt == nullptr || m_pPool == nullptr)
        return;
    closeOverlay();
    const QRect rc = visualRect(a_idx);
    if (!rc.isValid())
        return;
    QFont fSp = font();                      // 与 paintAssistant 同款 speaker 行度量
    fSp.setWeight(QFont::DemiBold);
    const int iSpeakerH = QFontMetrics(fSp).height();
    const int iDocW = qMax(16, rc.width() - 13);
    // 池保留 doc 所有权（覆盖层关闭只收不删；同宽渲染视觉无缝，方案 §8）
    QTextDocument* pDoc = m_pPool->Acquire(a_idx.row(), pIt->sText, iDocW);
    if (pDoc == nullptr)
        return;
    auto* pOv = new COverlayBrowser(viewport());
    m_pOverlay = pOv;
    m_iOverlayRow = a_idx.row();
    pOv->setDocument(pDoc);
    pOv->setReadOnly(true);
    pOv->setFrameShape(QFrame::NoFrame);
    pOv->setLineWrapMode(QTextEdit::WidgetWidth);
    pOv->setOpenExternalLinks(true);         // 对齐 Legacy QTextBrowser 口径
    pOv->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QPalette pal = pOv->palette();           // 只动 Base，不碰 Window（右键菜单口径）
    pal.setColor(QPalette::Base, QColor(0xff, 0xff, 0xeb));   // #fffbeb 浮层底（示意当前条目）
    pOv->setPalette(pal);
    pOv->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: transparent; border: none; }"));
    const int iDocH = qRound(pDoc->size().height());
    QRect rcOv(rc.left() + 13, rc.top() + iSpeakerH + 4, iDocW, iDocH + 4);
    // 视口底部仅露窄条：可视高不足下限不开层（原 24px 强制高会使浮层整体在视口
    // 外不可见却仍吞掉一次点击；收层后由滚动/点击重开）
    const int iAvailH = viewport()->height() - rcOv.top() - 2;
    if (iAvailH < 24) {
        closeOverlay();
        return;
    }
    rcOv.setHeight(qMin(rcOv.height(), qMax(24, iAvailH)));
    pOv->setGeometry(rcOv);
    pOv->fnEsc = [this] { closeOverlay(); };
    pOv->show();
    pOv->setFocus();
}

void CChatListView::closeOverlay()
{
    if (m_pOverlay == nullptr)
        return;
    // 换回自持有的空 doc 再销毁：防 QTextBrowser 析构带走池内 doc（所有权在池）
    m_pOverlay->setDocument(new QTextDocument(m_pOverlay));
    m_pOverlay->hide();
    m_pOverlay->deleteLater();
    m_pOverlay = nullptr;                    // QPointer 双保险
    m_iOverlayRow = -1;
}

void CChatListView::mouseDoubleClickEvent(QMouseEvent* a_ev)
{
    if (m_pOverlay != nullptr && m_pOverlay->isVisible()) {
        closeOverlay();
        return;
    }
    const QModelIndex idx = indexAt(a_ev->position().toPoint());
    if (idx.isValid() && m_pModel != nullptr) {
        const SDisplayItem* pIt = m_pModel->PeekItem(idx.row());
        // 双击 Assistant → 浮只读复制覆盖层（方案 §8：自绘后承接原生拖选 + Ctrl+C）
        if (pIt != nullptr && pIt->eKind == CMessageBlock::Assistant
            && !pIt->sText.isEmpty()) {
            openOverlay(idx);
            return;
        }
    }
    QListView::mouseDoubleClickEvent(a_ev);
}

void CChatListView::mousePressEvent(QMouseEvent* a_ev)
{
    // 覆盖层打开时点击条目外：收起并吞掉本次点击（方案 §8 退出条件）
    if (m_pOverlay != nullptr && m_pOverlay->isVisible()) {
        closeOverlay();
        return;
    }
    QListView::mousePressEvent(a_ev);
}

bool CChatListView::viewportEvent(QEvent* a_ev)
{
    if (a_ev->type() == QEvent::ToolTip && m_pDelegate != nullptr) {
        auto* pH = static_cast<QHelpEvent*>(a_ev);
        const QModelIndex idx = indexAt(pH->pos());
        if (idx.isValid()) {
            const QString sTip = m_pDelegate->ToolTipAt(idx, pH->pos());
            if (!sTip.isEmpty()) {
                QToolTip::showText(pH->globalPos(), sTip, this);
                return true;
            }
        }
    }
    return QListView::viewportEvent(a_ev);
}
