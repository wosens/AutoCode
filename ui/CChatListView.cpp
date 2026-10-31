#include "CChatListView.h"

#include "CChatDelegate.h"
#include "CChatDocPool.h"
#include "CChatModel.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolTip>

#include <algorithm>
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
    // 条目级选择复制（2026-10-23 借鉴 HarnessAgent v1.9.50）：Extended 多选 +
    // 整行 + 空白处框选；选中底色由 delegate 按 State_Selected 绘制（浅蓝
    // #dbeafe，不遮字）；Ctrl+C / 右键「复制/全选」走 CopySelection
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionRectVisible(true);
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
    // 池删除浮层行的 doc 前 → 先收浮层（摘引用后池才 delete；v5.0.412 实害：
    // 逐出/Clear 直接删 doc，浮层裸持引用绘制/析构即 UAF 崩。模型 Reset 路径
    // 原有 closeOverlay 不够——evictLru 随时可能删到浮层行）
    m_pPool->fnDocAboutToDelete = [this](int a_iItem) {
        if (m_pOverlay != nullptr && m_pOverlay->isVisible() && m_iOverlayRow == a_iItem)
            closeOverlay();
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

CChatListView::~CChatListView()
{
    // 2026-10-23 显式收浮层再入基类析构：成员析构序中池（children 后注册先删）
    // 先于 viewport（再删其子浮层）——浮层裸持池 doc 时 QTextBrowser 析构访问
    // 已删 doc = UAF；先收层把 doc 换成浮层自持空 doc，析构序炸弹拆除
    closeOverlay();
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
        // 池已清（doc 全删）：即使宽度未变也必须重排重材化——走统一入口强制档
        ApplyWidth(qMax(0, m_iWidthLast), /*a_bForce=*/true);
    });
    const int iW = viewport()->width();
    if (m_pDelegate && iW > 0)
        ApplyWidth(iW, /*a_bForce=*/true);   // 新模型：即使宽度同也重排（旧池 doc 作废）
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
            ApplyWidth(m_iWidthLast, /*a_bForce=*/true);   // 字体变：估高基准全换，强制重排
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

// 统一宽度重排入口（栈溢出修复：机制详见头文件注释）：重排链执行期的重入只记
// pending，链条退出后延迟一拍再处理——viewport Resize 的同步转发不再递归，
// 滚动条显隐震荡在事件循环迭代间收敛（同宽跳过）；>32 拍仍震荡则吸收当前宽
// 兑底（等下一次真实 resize），防 CPU 空转。
void CChatListView::ApplyWidth(int a_iWidth, bool a_bForce)
{
    if (m_pDelegate == nullptr || a_iWidth <= 0)
        return;
    if (!a_bForce && a_iWidth == m_iWidthLast)
        return;                              // 同宽：滚动条抖动的第二半拍，自然吸收
    if (m_bInWidthReformat) {
        m_iPendingWidth = a_iWidth;          // 重排中重入（viewportEvent 转发）：不递归，挂起
        return;
    }
    if (m_pOverlay != nullptr && m_pOverlay->isVisible())
        closeOverlay();                      // 旧几何浮层错位；统一收层口径
    m_iWidthLast = a_iWidth;
    m_bInWidthReformat = true;
    m_pDelegate->InvalidateWidth(a_iWidth);  // 高度缓存全失效 + 池内 doc 重排回调
    m_bInWidthReformat = false;
    if (m_iPendingWidth > 0 && m_iPendingWidth != m_iWidthLast) {
        m_iPendingWidth = -1;
        ++m_iDeferLoops;
        if (m_iDeferLoops > 32) {
            // 震荡不收敛兑底：吸收当前宽，等下一次真实 resize（内容布局已可用，
            // 仅估高/实高细节差留待后续校正）
            m_iDeferLoops = 0;
            m_iWidthLast = qMax(0, viewport()->width());
            return;
        }
        QTimer::singleShot(0, this, [this] {
            ApplyWidth(viewport()->width());   // 用当下实际宽（期间可能又变）
        });
    } else {
        m_iPendingWidth = -1;
        m_iDeferLoops = 0;                    // 收敛：清限流计数
    }
}

void CChatListView::resizeEvent(QResizeEvent* a_ev)
{
    QListView::resizeEvent(a_ev);
    // 宽变即全条目重排。注意 viewport 的 Resize 也经 viewportEvent 到达这里
    //（Qt 转发机制，滚动条显隐后 viewport 几何变化是常态来源）——统一走
    // ApplyWidth（重入挡板 + 延迟收敛）。
    ApplyWidth(viewport()->width());
}

void CChatListView::openOverlay(const QModelIndex& a_idx)
{
    const SDisplayItem* pIt = m_pModel ? m_pModel->PeekItem(a_idx.row()) : nullptr;
    if (pIt == nullptr)
        return;
    closeOverlay();
    // 打开复制浮层=用户交互意图：停贴底粘滞（closeOverlay 按当前位置还原）。
    // 否则开层后任何高度校正 → rangeChanged 自动滚底 → valueChanged 按「滚动即
    // 收层」把刚打开的浮层秒关（借鉴 HarnessAgent v1.9.50 T9/T15 修复）
    m_bAtBottom = false;
    const QRect rc = visualRect(a_idx);
    if (!rc.isValid() || rc.width() < 60)
        return;
    QFont fSp = font();                      // 与 paintAssistant 同款 speaker 行度量
    fSp.setWeight(QFont::DemiBold);
    const int iSpeakerH = QFontMetrics(fSp).height();
    const bool bAssistant = (pIt->eKind == CMessageBlock::Assistant);
    // 宽度口径与 paintAssistant 正在用的一致（借鉴 HarnessAgent v1.9.50）：
    // visualRect 与 paint 的 option.rect 同行同轮也可差 2×spacing（滚动条隐现等
    // 布局时序即可造成）——自算宽一旦偏离池 doc 现宽，Acquire 命中即重材化+
    // 高度校正→重排→滚底收层。Assistant 宽度优先取池 doc 现用 textWidth
    //（即 paint 正在用的宽，开层零重排零校正）；池未命中才退估宽；
    // 非 Assistant 为自建纯文本 doc 不受池影响，保持 visualRect 直接推导
    int iDocW = 0;
    if (bAssistant && m_pPool != nullptr) {
        if (QTextDocument* pOld = m_pPool->Peek(a_idx.row()))
            iDocW = qRound(pOld->textWidth());
    }
    if (iDocW <= 0)
        iDocW = qMax(16, rc.width() - (bAssistant ? 13 + 2 * spacing() : 8));
    // 文档源分派（2026-10-22 复制层全块型开放）：Assistant = 池 Markdown doc
    // （保持源格式，池持所有权，层关闭只收不删）；其余块型 = PlainTextAt 纯文本
    // 自建 doc（层持所有权，与池无关——池键约定只存 Assistant 正文）
    QTextDocument* pDoc = nullptr;
    if (bAssistant) {
        if (!pIt->sText.isEmpty() && m_pPool != nullptr)
            pDoc = m_pPool->Acquire(a_idx.row(), pIt->sText, iDocW);
    } else if (m_pDelegate != nullptr) {
        const QString sPlain = m_pDelegate->PlainTextAt(a_idx);
        if (!sPlain.isEmpty()) {
            pDoc = new QTextDocument();      // 建 overlay 后挂 parent（见下）
            pDoc->setPlainText(sPlain);
            pDoc->setTextWidth(iDocW);
        }
    }
    if (pDoc == nullptr)
        return;
    auto* pOv = new COverlayBrowser(viewport());
    m_pOverlay = pOv;
    m_iOverlayRow = a_idx.row();
    if (pDoc->parent() == nullptr)
        pDoc->setParent(pOv);                // 自建 doc 挂 overlay；池 doc 池为 parent 不动
    pOv->setDocument(pDoc);
    pOv->setReadOnly(true);
    pOv->setFrameShape(QFrame::NoFrame);
    pOv->setLineWrapMode(QTextEdit::WidgetWidth);
    pOv->setOpenExternalLinks(true);         // 对齐 Legacy QTextBrowser 口径
    pOv->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    pOv->setToolTip(QStringLiteral(
        "拖选文本后 Ctrl+C 复制；Esc / 滚动 / 点击条目外关闭"));
    QPalette pal = pOv->palette();           // 只动 Base，不碰 Window（右键菜单口径）
    pal.setColor(QPalette::Base, QColor(0xff, 0xff, 0xeb));   // #fffbeb 浮层底（示意当前条目）
    pOv->setPalette(pal);
    // 2026-10-26 重影修复（用户实害：双击提示词浮层文字与底下原文双影看不清无法拖选）：
    //   旧写法 QSS background:transparent 把上面 Palette 米黄底废掉（QSS 优先级高于
    //   Palette），浮层变透明——delegate 已绘制的原条目文字从浮层底下透出来，与浮层
    //   文字同位叠加成重影。底色必须不透明（QSS 与 Palette 同值双保险）。
    pOv->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: #fffbeb; border: none; }"));
    // 非 Assistant 无 speaker 行，层顶从条目顶 +2 起（Assistant 保持 speaker+4 口径）
    const int iTop = bAssistant ? (iSpeakerH + 4) : 2;
    const int iLeft = bAssistant ? 13 : 0;
    const int iDocH = qRound(pDoc->size().height());
    QRect rcOv(rc.left() + iLeft, rc.top() + iTop, iDocW, iDocH + 4);
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
    // 贴底粘滞还原（openOverlay 开层时置 false）：仍停在底部则恢复自动跟随，
    // 否则保持脱离（用户上翻态）——口径与 valueChanged 的 ±4 判定一致
    m_bAtBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
}

void CChatListView::mouseDoubleClickEvent(QMouseEvent* a_ev)
{
    if (m_pOverlay != nullptr && m_pOverlay->isVisible()) {
        closeOverlay();
        return;
    }
    const QPoint pos = a_ev->position().toPoint();
    const QModelIndex idx = indexAt(pos);
    if (idx.isValid() && m_pModel != nullptr) {
        const SDisplayItem* pIt = m_pModel->PeekItem(idx.row());
        // 2026-10-22 双击任意块 → 浮只读复制层（承接 Legacy 标签拖选 + Ctrl+C；
        // Assistant 走池 Markdown doc 保持源格式，其余块型走纯文本）。折叠头区
        // （ToolUse/System）例外：双击的两个 release 已各翻一次折叠，再开层会
        // 「既变了折叠又弹层」，头区双击保持纯折叠语义
        if (pIt != nullptr && m_pDelegate != nullptr && m_pDelegate->IsHeaderHit(idx, pos)) {
            QListView::mouseDoubleClickEvent(a_ev);
            return;
        }
        if (pIt != nullptr) {
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

void CChatListView::keyPressEvent(QKeyEvent* a_ev)
{
    // Ctrl+C（平台标准复制键）→ 所选条目纯文本进剪贴板（2026-10-23 条目选择
    // 复制，借鉴 HarnessAgent v1.9.50）；全选 Ctrl+A 由基类自带处理
    if (a_ev->matches(QKeySequence::Copy)) {
        CopySelection();
        return;
    }
    QListView::keyPressEvent(a_ev);
}

void CChatListView::contextMenuEvent(QContextMenuEvent* a_ev)
{
    // QAbstractScrollArea 把 viewport 的 ContextMenu 事件坐标映射到视图控件坐标系，
    // indexAt 吃 viewport 坐标——mapFrom 还原；右键未选条目即改为选中该条
    const QModelIndex idx = indexAt(viewport()->mapFrom(this, a_ev->pos()));
    QItemSelectionModel* pSel = selectionModel();
    if (idx.isValid() && pSel != nullptr && !pSel->isSelected(idx))
        pSel->select(idx, QItemSelectionModel::ClearAndSelect
                              | QItemSelectionModel::Rows);
    QMenu menu(this);
    QFont fMenu = menu.font();               // 界面新代码字体缺省 11pt（全局规）
    fMenu.setPointSize(11);
    menu.setFont(fMenu);
    QAction* pActCopy = menu.addAction(QStringLiteral("复制"));
    pActCopy->setEnabled(pSel != nullptr && !pSel->selectedRows().isEmpty());
    menu.addSeparator();
    QAction* pActAll = menu.addAction(QStringLiteral("全选"));
    const QAction* pAct = menu.exec(a_ev->globalPos());
    if (pAct == pActCopy)
        CopySelection();
    else if (pAct == pActAll)
        selectAll();
}

void CChatListView::focusOutEvent(QFocusEvent* a_ev)
{
    // 2026-10-23（用户需求）：焦点转移到其他控件 → 取消条目选择（浅蓝高亮不再残留）。
    // 例外：Qt::PopupFocusReason = 右键「复制/全选」菜单弹出时 Qt 把焦点交给 QMenu——
    //   此处若照清，菜单收起执行「复制」时选择已空，功能自废；菜单收起后焦点自动回本视图。
    // 覆盖层（openOverlay 里 pOv->setFocus）取得焦点同样走到这里：清条目高亮属预期
    //（浮层内做精细文本拖选时，被双击条目的整行高亮不该同时残留）。
    if (a_ev->reason() != Qt::PopupFocusReason) {
        clearSelection();
        setCurrentIndex(QModelIndex());
    }
    QListView::focusOutEvent(a_ev);
}

// 所选条目按行号升序拼接纯文本进剪贴板（2026-10-23 借鉴 HarnessAgent v1.9.50
// CopySelection）；无选中/文本全空不动剪贴板。Assistant=正文源文本（Markdown
// 原样，与浮层源格式同源）；其余块型=PlainTextAt 口径（Event/Error=正文、
// System=逐行、ToolUse=逐条【工具名】+入参/结果）。selectedRows 依赖
// SelectRows 行为；模型条目经 PeekItem 直读（复制时无并发喂入，指针仅当次使用安全）
void CChatListView::CopySelection()
{
    if (m_pModel == nullptr || selectionModel() == nullptr)
        return;
    QList<QModelIndex> aSel = selectionModel()->selectedRows();
    if (aSel.isEmpty())
        return;
    std::sort(aSel.begin(), aSel.end(),
              [](const QModelIndex& a, const QModelIndex& b) {
                  return a.row() < b.row();
              });
    QStringList aParts;
    for (const QModelIndex& idx : aSel) {
        const SDisplayItem* pIt = m_pModel->PeekItem(idx.row());
        if (pIt == nullptr)
            continue;
        const QString s = (pIt->eKind == CMessageBlock::Assistant)
            ? pIt->sText
            : (m_pDelegate != nullptr ? m_pDelegate->PlainTextAt(idx) : QString());
        if (!s.isEmpty())
            aParts.append(s);
    }
    if (aParts.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(aParts.join(QLatin1Char('\n')));
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

bool CChatListView::OverlayVisible() const
{
    return m_pOverlay != nullptr && m_pOverlay->isVisible();
}

QString CChatListView::OverlayText() const
{
    return (m_pOverlay != nullptr && m_pOverlay->isVisible())
        ? m_pOverlay->toPlainText() : QString();
}

QString CChatListView::OverlayStyleSheet() const
{
    return (m_pOverlay != nullptr) ? m_pOverlay->styleSheet() : QString();
}
