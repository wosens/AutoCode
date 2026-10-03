#include "ui/CAsstTabBar.h"

#include <QPainter>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QApplication>
#include <QStyle>
#include <QStyleOptionTab>

namespace {
constexpr int kCloseAreaW = 22;     // 关闭「x」热区宽（Tab 头右缘，含点击余量）
constexpr int kCloseIconPx = 14;    // 关闭「x」图标边长
constexpr int kDotAreaW = 2;        // 运行闪点占位宽（Tab 头左缘；2026.09.23 v3.0.221 用户反馈
                                    //   闪点后有一段空格：旧值 16 实测文本起点 = 16(左内边距)
                                    //   +16(占位) +4(原生按钮间隙)，点右缘到文本空隙达 21px≈
                                    //   两个汉字宽；改 2 后文本落 x=22、空隙 7px≈一个空格宽，
                                    //   闪点[6,15]仍完全落在 16px 左内边距区内不压字（windowsvista
                                    //   与 windows11 双样式渲染探针实测一致））
constexpr int kDotPx = 9;           // 运行闪点圆点直径
constexpr int kBlinkMs = 500;       // 闪点换色间隔（与旧 QLabel 方案节拍一致）
// 闪点颜色序列（500ms/拍循环，全周期 2s）：绿→青→蓝→紫——浅/深色主题均可辨，
//   且与空闲状态点色（灰/绿/黄/红）语义错开：闪=正在跑，静态色=已结束。
const char* const k_aszDotColors[] = { "#22c55e", "#06b6d4", "#3b82f6", "#8b5cf6" };
constexpr int k_iDotColorCount = static_cast<int>(sizeof(k_aszDotColors) / sizeof(k_aszDotColors[0]));
constexpr double kFloatOpacity = 0.75;      // 拖拽浮动 Tab 不透明度
} // namespace

CAsstTabBar::CAsstTabBar(QWidget* a_pParent)
    : QTabBar(a_pParent)
{
    m_blinkTimer.setInterval(kBlinkMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this]() { update(); });
    // 多行接管：不再要基类的滚动箭头（放不下改为换行）；开鼠标跟踪供悬停
    setUsesScrollButtons(false);
    setMouseTracking(true);
    // 基类 setCurrentIndex 只按它自己的单行 rect 标脏重绘，多行下会漏擦旧选中态
    // ——选中变化时全量重绘兜底
    connect(this, &QTabBar::currentChanged, this, [this]() { update(); });
}

// ── 宿主/判定器/运行态 ─────────────────────────────────────────────────────

QTabWidget* CAsstTabBar::HostTabs() const
{
    // setTabBar 会把本栏 reparent 给 QTabWidget —— 宿主即父对象
    return qobject_cast<QTabWidget*>(parentWidget());
}

int CAsstTabBar::IndexOfPage(QWidget* a_pPage) const
{
    if (a_pPage == nullptr) {
        return -1;
    }
    if (QTabWidget* pH = HostTabs()) {
        return pH->indexOf(a_pPage);
    }
    return -1;
}

void CAsstTabBar::SetClosableChecker(const std::function<bool(int a_iIndex)>& a_fn)
{
    m_fnClosable = a_fn;
    update();
}

void CAsstTabBar::SetPageRunning(QWidget* a_pPage, bool a_bRunning)
{
    const bool bWas = m_runPages.contains(a_pPage);
    if (bWas == a_bRunning || a_pPage == nullptr) {
        return;                        // 状态未变不折腾（面板刷新路径高频到达）
    }
    if (a_bRunning)
        m_runPages.insert(a_pPage);
    else
        m_runPages.remove(a_pPage);
    SyncBlinkTimer();
    ApplyLayout();   // 闪点占位变宽 → 理想宽变化，重排分行
}

void CAsstTabBar::SetPageForeign(QWidget* a_pPage, bool a_bForeign)
{
    if (a_pPage == nullptr) {
        return;
    }
    const bool bWas = m_foreignPages.contains(a_pPage);
    if (bWas == a_bForeign) {
        return;
    }
    if (a_bForeign)
        m_foreignPages.insert(a_pPage);
    else
        m_foreignPages.remove(a_pPage);
    update();   // 仅给制样式（粗体），不影响布局尺寸口径（名字不变宽度近似，避免重排抖动）
}

void CAsstTabBar::SyncBlinkTimer()
{
    if (m_runPages.isEmpty()) {
        m_blinkTimer.stop();
        m_blinkElapsed.invalidate();
    } else if (!m_blinkTimer.isActive()) {
        m_blinkElapsed.restart();
        m_blinkTimer.start();
    }
}

bool CAsstTabBar::TabHasClose(int a_iIndex) const
{
    // 「x」显隐全由宿主判定器裁决（助手页 true；日志/产物/「+」/预览 false）
    return a_iIndex >= 0 && a_iIndex < count() && m_fnClosable
           && m_fnClosable(a_iIndex);
}

bool CAsstTabBar::TabRunning(int a_iIndex) const
{
    if (a_iIndex < 0 || a_iIndex >= count()) {
        return false;
    }
    const QWidget* pW = HostTabs() ? HostTabs()->widget(a_iIndex) : nullptr;
    return pW != nullptr && m_runPages.contains(const_cast<QWidget*>(pW));
}

// ── 多行布局 ────────────────────────────────────────────────────────────────

QVector<int> CAsstTabBar::NaturalOrder() const
{
    QVector<int> order;
    order.reserve(count());
    for (int i = 0; i < count(); ++i)
        if (!isTabVisible(i)) continue; else
        order << i;
    return order;
}

QRect CAsstTabBar::CloseRectOf(const QRect& a_rcTab) const
{
    // Tab 头右缘 kCloseAreaW 宽热区内居中放 kCloseIconPx 图标
    const int iY = a_rcTab.center().y() - kCloseIconPx / 2;
    const int iX = a_rcTab.right() - kCloseAreaW + (kCloseAreaW - kCloseIconPx) / 2 + 1;
    return QRect(iX, iY, kCloseIconPx, kCloseIconPx);
}

int CAsstTabBar::IdealTabWidth(int a_iIndex) const
{
    if (a_iIndex < 0 || a_iIndex >= count())
        return 0;
    // 宽度直接取基类 tabSizeHint 的原生口径——它含 PM_TabBarTabHSpace 等样式
    //   内边距、图标与字体度量（HarnessAgent v1.9.23 教训：手算比原生窄、文字
    //   压自画「x」）；运行闪点位同法补上（起跑/停跑 Tab 宽度小幅变化）
    int iW = QTabBar::tabSizeHint(a_iIndex).width();
    if (TabHasClose(a_iIndex))
        iW += kCloseAreaW + 4;   // 自画关闭位（基类内置按钮已摘除）+ 镜像原生每按钮 +4 padding
    if (TabRunning(a_iIndex))
        iW += kDotAreaW + 4;     // 自画闪点位（同上）
    return iW;
}

int CAsstTabBar::RowHeight() const
{
    // 行高取各 Tab 原生 tabSizeHint 高的最大值（fm.height() + PM_TabBarTabVSpace
    //   等；HarnessAgent v1.9.23 教训：手算 fm.height() 偏矮，绘制扣 vpadding 后
    //   文字上下被裁）
    int iH = 0;
    for (int i = 0; i < count(); ++i)
        if (!isTabVisible(i)) continue; else
        iH = qMax(iH, QTabBar::tabSizeHint(i).height());
    if (iH <= 0) {                // 无 Tab 时的兜底（与单行口径一致）
        QStyleOptionTab stOpt;
        stOpt.text = QStringLiteral("Ag");
        const QSize sz = style()->sizeFromContents(QStyle::CT_TabBarTab, &stOpt,
                                                   QSize(24, fontMetrics().height()), this);
        iH = qMax(sz.height(), 18);
    }
    return iH;
}

CAsstTabBar::LayoutResult CAsstTabBar::CalcLayout(const QVector<int>& a_order) const
{
    LayoutResult stRes;
    const int n = count();
    stRes.rectOf.resize(n);
    stRes.iRowH = RowHeight();
    // 未定宽（首 show 前）按极宽假定：先给单行口径的 hint，show 后 resizeEvent
    // 按真实宽修正——首帧观感与原生单行一致，宽度不够时立即换行
    const int iAvail = width() > 0 ? width() : 1000000;
    int x = 0, y = 0, iRow = 0;
    for (const int i : a_order) {
        if (i < 0 || i >= n)
            continue;
        const int w = qMin(IdealTabWidth(i), iAvail);
        if (x > 0 && x + w > iAvail) {          // 本行放不下 → 换行（行首超宽项不再换）
            x = 0;
            y += stRes.iRowH;
            ++iRow;
        }
        stRes.rectOf[i] = QRect(x, y, qMin(w, iAvail - x), stRes.iRowH);
        x += w;
    }
    stRes.iRows = iRow + 1;
    return stRes;
}

void CAsstTabBar::ApplyLayout()
{
    const int iOldH = m_layout.iRows * m_layout.iRowH;
    m_naturalOrder = NaturalOrder();
    m_layout = CalcLayout(m_naturalOrder);
    if (m_layout.iRows * m_layout.iRowH != iOldH)
        updateGeometry();   // 总高变了：让 QTabWidget/上层布局重取 sizeHint 修正页区
    update();
}

QSize CAsstTabBar::sizeHint() const
{
    // 宽度 = 单行自然宽（各 Tab 理想宽之和）——与任何几何无关，无反馈回路。
    // QTabWidget 定 bar 几何的规则（QCommonStyle SE_TabWidgetTabBar）是
    // min(bar sizeHint 宽, 可用宽)：窗口够宽 → bar = 自然宽 = 单行（缺省一行）；
    // 窗口变窄 → bar 钳到可用宽，resizeEvent 按真实宽贪心分行（放满一行再换下一
    // 行）。两个历史坑都避开（HarnessAgent v1.9.22）：固定小宽 → bar 被钉死窄栏
    // 疯狂竖叠；读父宽/自身宽 → 布局请求依赖几何形成正反馈，窗口被撑到数万像素
    int iW = 0;
    for (int i = 0; i < count(); ++i)
        if (!isTabVisible(i)) continue; else
        iW += IdealTabWidth(i);
    // 高度按当前宽分行是真实诉求（多行要占行数×行高），只影响高度不影响宽度
    const LayoutResult stLy = CalcLayout(NaturalOrder());
    return QSize(qMax(iW, 40), stLy.iRows * stLy.iRowH);
}

QSize CAsstTabBar::minimumSizeHint() const
{
    const LayoutResult stLy = CalcLayout(NaturalOrder());
    // 最小宽固定小值（多行布局任意窄都能换行铺排），高度同上保留——原实现取
    //   父宽/自身宽，窗口被撑宽一次后最小宽即锁死、缩不回去
    return QSize(120, stLy.iRows * stLy.iRowH);
}

QRect CAsstTabBar::TabRectAt(int a_iIndex) const
{
    return (a_iIndex >= 0 && a_iIndex < m_layout.rectOf.size())
               ? m_layout.rectOf.at(a_iIndex) : QRect();
}

int CAsstTabBar::TabAtPos(const QPoint& a_pt) const
{
    for (int i = 0; i < m_layout.rectOf.size(); ++i)
        if (isTabVisible(i) && m_layout.rectOf.at(i).contains(a_pt))
            return i;
    return -1;
}

// ── 绘制 ────────────────────────────────────────────────────────────────────

void CAsstTabBar::paintEvent(QPaintEvent* a_pEvent)
{
    Q_UNUSED(a_pEvent);
    const bool bDrag = m_bDragging && m_iDragFrom >= 0;
    const LayoutResult& ly = bDrag ? m_dragLayout : m_layout;
    const QVector<int>& order = bDrag ? m_dragOrder : m_naturalOrder;
    QPainter stPainter(this);
    stPainter.setClipRect(rect());
    // 底基线：非文档模式时末行下缘一条分隔线，衔接 QTabWidget 页面边框
    if (drawBase() && !documentMode())
        stPainter.fillRect(QRect(0, ly.iRows * ly.iRowH - 1, width(), 1),
                           palette().dark().color());
    QStyleOptionTab::TabPosition eFloatPos = QStyleOptionTab::OnlyOneTab;
    for (int iRow = 0; iRow < ly.iRows; ++iRow) {
        QVector<int> rowTabs;          // 该行各 Tab 索引（顺序即行内显示序）
        for (const int i : order) {
            if (ly.rectOf.at(i).top() == iRow * ly.iRowH)
                rowTabs << i;
        }
        for (int k = 0; k < rowTabs.size(); ++k) {
            const QStyleOptionTab::TabPosition ePos =
                (k == 0 && k == rowTabs.size() - 1) ? QStyleOptionTab::OnlyOneTab
                : (k == 0)                             ? QStyleOptionTab::Beginning
                : (k == rowTabs.size() - 1)            ? QStyleOptionTab::End
                                                       : QStyleOptionTab::Middle;
            const int i = rowTabs.at(k);
            if (bDrag && i == m_iDragFrom) {   // 被拖项：占位留洞，改画浮动 Tab
                eFloatPos = ePos;
                continue;
            }
            PaintTab(stPainter, i, ly.rectOf.at(i), ePos, false);
        }
    }
    if (bDrag) {
        // 浮动 Tab：保持拖起时抓取点的相对偏移，半透明跟随光标
        const QRect rcHole = m_dragLayout.rectOf.at(m_iDragFrom);
        const QRect rcFloat(m_ptDragCur - m_ptDragGrab, rcHole.size());
        PaintTab(stPainter, m_iDragFrom, rcFloat, eFloatPos, true);
    }
}

void CAsstTabBar::PaintTab(QPainter& a_pPainter, int a_iIndex, const QRect& a_rcTab,
                           QStyleOptionTab::TabPosition a_ePos, bool a_bFloating)
{
    if (!a_rcTab.isValid())
        return;
    const bool bRun = TabRunning(a_iIndex);
    const bool bForeign = m_foreignPages.contains(PageOfIndex(a_iIndex));   // 2026-10-02 外档幽灵：标题粗体（文本自绘——QStyleOptionTab 无 per-tab font）
    QStyleOptionTab stOpt;
    initStyleOption(&stOpt, a_iIndex);
    stOpt.rect = a_rcTab;
    stOpt.position = a_ePos;
    // 向样式声明两侧按钮占位（基类按钮已摘除、实际画的是自绘「x」/闪点），
    // 原生 tabLayout 据此把文字收进安全区，避免尾部压「x」。头部：闪点由 16px
    // 左内边距区容纳（v3.0.221 实测：占位 16→2 后文本落 x=22、点不压字）
    QString sForeignText;
    if (bForeign) {
        sForeignText = stOpt.text;   // 外档：文本剥离，样式画完页框后粗体自绘
        stOpt.text.clear();
    }
    if (TabHasClose(a_iIndex))
        stOpt.rightButtonSize = QSize(kCloseAreaW, kCloseIconPx);
    if (bRun)
        stOpt.leftButtonSize = QSize(kDotAreaW, kDotPx);
    if (a_iIndex == currentIndex())
        stOpt.state |= QStyle::State_Selected;
    else
        stOpt.state &= ~QStyle::State_Selected;
    if (a_iIndex == m_iHover && !m_bDragging)
        stOpt.state |= QStyle::State_MouseOver;
    else
        stOpt.state &= ~QStyle::State_MouseOver;
    a_pPainter.save();
    if (a_bFloating)
        a_pPainter.setOpacity(kFloatOpacity);
    style()->drawControl(QStyle::CE_TabBarTab, &stOpt, &a_pPainter, this);
    if (!sForeignText.isEmpty()) {
        // 2026-10-02 外档幽灵标题：粗体自绘（闪点右侧→「x」左侧，省略号截断；
        //   选中态用高亮前景，与原生观感一致）。
        QFont fBold = font();
        fBold.setBold(true);
        a_pPainter.save();
        a_pPainter.setFont(fBold);
        a_pPainter.setPen((a_iIndex == currentIndex())
                              ? palette().highlightedText().color()
                              : palette().buttonText().color());
        const int iL = a_rcTab.left() + (bRun ? kDotAreaW : 8);
        const int iR = a_rcTab.right() - (TabHasClose(a_iIndex) ? kCloseAreaW : 6) + 1;
        const QRect rcText(iL, a_rcTab.top(), qMax(1, iR - iL), a_rcTab.height());
        a_pPainter.drawText(rcText, Qt::AlignVCenter | Qt::AlignHCenter,
                            a_pPainter.fontMetrics().elidedText(
                                sForeignText, Qt::ElideRight, rcText.width()));
        a_pPainter.restore();
    }
    // 关闭「x」：基类内置按钮已摘除，此处按多行 rect 自画（浮动 Tab 亦画，保持同观感）
    if (TabHasClose(a_iIndex)) {
        QStyleOption stBtn;
        stBtn.initFrom(this);
        stBtn.rect = CloseRectOf(a_rcTab);
        if (a_iIndex == m_iHover && m_bHoverClose && !m_bDragging)
            stBtn.state |= QStyle::State_MouseOver;
        style()->drawPrimitive(QStyle::PE_IndicatorTabClose, &stBtn, &a_pPainter, this);
    }
    a_pPainter.restore();
    // 运行闪点：左缘彩色圆点按 elapsed 相位取色（500ms/拍、4 色循环，与旧
    //   QLabel tabButton 方案同节拍同色序；浮动 Tab 亦画，保持同观感）
    if (!bRun)
        return;
    const qint64 iMs = m_blinkElapsed.isValid() ? m_blinkElapsed.elapsed() : 0;
    const QColor clrDot = QColor(QLatin1String(
        k_aszDotColors[static_cast<int>((iMs / kBlinkMs) % k_iDotColorCount)]));
    QRectF rcDot(a_rcTab.left() + 6, a_rcTab.center().y() - kDotPx / 2.0, kDotPx, kDotPx);
    a_pPainter.save();
    a_pPainter.setRenderHint(QPainter::Antialiasing, true);
    a_pPainter.setPen(Qt::NoPen);
    a_pPainter.setBrush(clrDot);
    a_pPainter.drawEllipse(rcDot);
    a_pPainter.restore();
}

// ── 布局刷新钩子 ────────────────────────────────────────────────────────────

void CAsstTabBar::resizeEvent(QResizeEvent* a_pEvent)
{
    QTabBar::resizeEvent(a_pEvent);
    ApplyLayout();                    // 宽度变了 → 重新分行
}

void CAsstTabBar::showEvent(QShowEvent* a_pEvent)
{
    QTabBar::showEvent(a_pEvent);
    ApplyLayout();                    // 首次可见：构造期假定宽 → 真实宽修正
}

void CAsstTabBar::tabInserted(int a_iIndex)
{
    QTabBar::tabInserted(a_iIndex);
    // 摘除基类为 closable 页创建的内置关闭按钮（位置由基类单行布局决定，与多行
    // 不一致会叠在第一行）；关闭「x」由本类按判定器自画。幂等
    setTabButton(a_iIndex, QTabBar::RightSide, nullptr);
    setTabButton(a_iIndex, QTabBar::LeftSide, nullptr);
    ApplyLayout();
}

void CAsstTabBar::tabRemoved(int a_iIndex)
{
    QTabBar::tabRemoved(a_iIndex);
    m_iHover = -1;
    m_bHoverClose = false;
    m_iPress = -1;
    m_bPressClose = false;
    m_bPressMoved = false;
    if (m_bDragging) {                // 拖拽中被外部删页（如软关助手）：放弃本次拖拽
        if (m_bGrabbed)
            releaseMouse();
        m_bGrabbed = false;
        m_bDragging = false;
        m_iDragFrom = m_iDragTo = -1;
        m_dragOrder.clear();
        m_dragLayout = LayoutResult();
    }
    ApplyLayout();
}

void CAsstTabBar::NotifyTabsChanged()
{
    // 改名/自动重命名/预览换文件标题等 → 理想宽变化，重排分行（Qt6 QTabBar 没有
    //   文本变化虚拟钩子，宿主改完 setTabText 后显式调本接口）
    ApplyLayout();
}

// ── 交互（多行自实现：选中 / 悬停 / 关闭 / 双击 / 拖拽重排）─────────────────

void CAsstTabBar::UpdateHover(const QPoint& a_pt)
{
    const int iIdx = TabAtPos(a_pt);
    const bool bClose = iIdx >= 0 && TabHasClose(iIdx)
                        && CloseRectOf(m_layout.rectOf.at(iIdx)).contains(a_pt);
    if (iIdx != m_iHover || bClose != m_bHoverClose) {
        m_iHover = iIdx;
        m_bHoverClose = bClose;
        update();
    }
}

void CAsstTabBar::mousePressEvent(QMouseEvent* a_pEvent)
{
    if (a_pEvent->button() != Qt::LeftButton) {
        QTabBar::mousePressEvent(a_pEvent);
        return;
    }
    const QPoint pt = a_pEvent->position().toPoint();
    const int iIdx = TabAtPos(pt);
    m_iPress = iIdx;
    m_ptPress = pt;
    m_bPressMoved = false;
    m_bPressClose = iIdx >= 0 && TabHasClose(iIdx)
                    && CloseRectOf(m_layout.rectOf.at(iIdx)).contains(pt);
    if (iIdx >= 0 && !m_bPressClose && iIdx != currentIndex())
        setCurrentIndex(iIdx);        // press 即选中（与基类一致）；「x」热区不选中
    update();
}

void CAsstTabBar::mouseMoveEvent(QMouseEvent* a_pEvent)
{
    const QPoint pt = a_pEvent->position().toPoint();
    if (m_bDragging) {
        UpdateDrag(pt);
        return;
    }
    UpdateHover(pt);
    if (m_iPress >= 0 && !m_bPressClose && isMovable()
        && (pt - m_ptPress).manhattanLength() > QApplication::startDragDistance()) {
        m_bPressMoved = true;
        StartDrag(pt);
    }
}

void CAsstTabBar::mouseReleaseEvent(QMouseEvent* a_pEvent)
{
    if (m_bDragging) {
        if (m_bGrabbed) {
            releaseMouse();
            m_bGrabbed = false;
        }
        const int iFrom = m_iDragFrom;
        const int iTo = m_iDragTo;
        m_bDragging = false;
        m_iDragFrom = m_iDragTo = -1;
        m_dragOrder.clear();
        m_dragLayout = LayoutResult();
        m_iPress = -1;
        m_bPressClose = false;
        m_bPressMoved = false;
        if (iFrom >= 0 && iTo >= 0 && iTo != iFrom)
            moveTab(iFrom, iTo);      // → emit tabMoved → QTabWidget 同步页序
        ApplyLayout();   // 2026.09.28 移到 moveTab 之后：m_layout 按各索引当前内容算宽，
                         //   先 Apply 再 moveTab 会按旧索引-内容映射算宽（宽文本进窄格被截）
        update();
        return;
    }
    if (m_iPress >= 0 && m_bPressClose && !m_bPressMoved) {
        const QPoint pt = a_pEvent->position().toPoint();
        const QRect rcTab = m_layout.rectOf.value(m_iPress);
        if (rcTab.contains(pt) && CloseRectOf(rcTab).contains(pt))
            emit tabCloseRequested(m_iPress);   // 完整按-放都在「x」热区才关（防误触）
    }
    m_iPress = -1;
    m_bPressClose = false;
    m_bPressMoved = false;
    update();
}

void CAsstTabBar::mouseDoubleClickEvent(QMouseEvent* a_pEvent)
{
    // 双击按多行布局自查命中 → 复用基类信号（面板接它做助手 Tab 改名）。
    //   基类实现按它自己的单行布局命中，多行下会点错页，故整体接管；
    //   首击的选中仍走 mousePressEvent（与原生一致）
    if (a_pEvent->button() == Qt::LeftButton) {
        const int iIdx = TabAtPos(a_pEvent->position().toPoint());
        if (iIdx >= 0)
            emit tabBarDoubleClicked(iIdx);
        return;
    }
    QTabBar::mouseDoubleClickEvent(a_pEvent);
}

void CAsstTabBar::leaveEvent(QEvent* a_pEvent)
{
    if (!m_bDragging && m_iHover >= 0) {
        m_iHover = -1;
        m_bHoverClose = false;
        update();
    }
    QTabBar::leaveEvent(a_pEvent);
}

// ── 拖拽重排（movable，多行版）─────────────────────────────────────────────

void CAsstTabBar::StartDrag(const QPoint& a_pt)
{
    m_bDragging = true;
    m_iDragFrom = m_iPress;
    m_iDragTo = m_iDragFrom;          // 初始不动（moveTab(from, from)）
    m_ptDragCur = a_pt;
    m_ptDragGrab = a_pt - m_layout.rectOf.at(m_iDragFrom).topLeft();
    m_dragOrder = m_naturalOrder;
    m_dragLayout = m_layout;
    grabMouse();
    m_bGrabbed = true;
    UpdateDrag(a_pt);
}

void CAsstTabBar::UpdateDrag(const QPoint& a_pt)
{
    m_ptDragCur = a_pt;
    const int iTo = DropInsertAt(a_pt);
    if (iTo >= 0 && iTo != m_iDragTo) {
        m_iDragTo = iTo;
        // 重建插入后顺序：抽出被拖项再插到 iTo，其余 Tab 按此顺序滑移让位
        m_dragOrder.clear();
        for (int i = 0; i < count(); ++i)
        if (!isTabVisible(i)) continue; else
            if (i != m_iDragFrom)
                m_dragOrder << i;
        m_dragOrder.insert(iTo, m_iDragFrom);
        m_dragLayout = CalcLayout(m_dragOrder);
    }
    update();
}

int CAsstTabBar::DropInsertAt(const QPoint& a_pt) const
{
    const int n = m_dragOrder.size();
    if (n <= 0 || m_iDragFrom < 0)
        return -1;
    // 光标定行（越界钳到首/末行），再在行内按 x 找缝：命中项左半 → 其前，
    // 右半 → 其后，行末空档 → 行末之后；缝换算为「抽出被拖项后的插入位」
    const int iRow = qBound(0, a_pt.y() / m_dragLayout.iRowH, m_dragLayout.iRows - 1);
    int iFirst = -1;                  // 行首项在 m_dragOrder 中的位
    QVector<int> rowPos;              // 该行各项在 m_dragOrder 中的位（行内显示序）
    for (int d = 0; d < n; ++d) {
        if (m_dragLayout.rectOf.at(m_dragOrder.at(d)).top() == iRow * m_dragLayout.iRowH) {
            if (iFirst < 0)
                iFirst = d;
            rowPos << d;
        }
    }
    if (iFirst < 0)
        iFirst = 0;
    int s = iFirst + rowPos.size();   // 缺省缝 = 行末之后
    for (int k = 0; k < rowPos.size(); ++k) {
        const QRect& r = m_dragLayout.rectOf.at(m_dragOrder.at(rowPos.at(k)));
        if (a_pt.x() < r.center().x()) {
            s = iFirst + k;
            break;
        }
    }
    const int dFrom = m_dragOrder.indexOf(m_iDragFrom);
    return qBound(0, s > dFrom ? s - 1 : s, n - 1);
}

// ── tooltip：按多行布局命中（基类按它自己的单行布局定位会错乱）────────────

bool CAsstTabBar::event(QEvent* a_pEvent)
{
    if (a_pEvent->type() == QEvent::ToolTip) {
        auto* pHelp = static_cast<QHelpEvent*>(a_pEvent);
        const int iIdx = TabAtPos(pHelp->pos());
        const QString sTip = iIdx >= 0 ? tabToolTip(iIdx) : QString();
        if (!sTip.isEmpty())
            QToolTip::showText(pHelp->globalPos(), sTip, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QTabBar::event(a_pEvent);
}
