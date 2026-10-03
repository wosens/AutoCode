#pragma once

// ===========================================================================
// CAsstTabBar —— 助手 Tab 栏（自动多行 + 自绘关闭「x」+ 自绘运行闪点）
// 2026-09-22 自 HarnessAgent/CAgentTabBar（v1.9.19 多行 / v1.9.23 宽高口径）迁移：
// Qt QTabBar 原生只有单行（放不下出滚动箭头），布局在私有类里无法注入，故本类
// 接管布局与绘制：宽度足够 = 单行（观感与原生一致），不够 = 自动换行叠放（每行
// 左起排满即换行）。交互全部按多行布局自实现：
//   · 布局：逐 Tab 累加理想宽度（基类 tabSizeHint 原生口径 + 关闭/闪点位），超出
//     栏宽换行；行高取单行原生高度的最大值；
//   · 绘制：逐 Tab 构造 QStyleOptionTab 交当前 style 画 CE_TabBarTab（保持
//     观感，窄 Tab 文本自动省略）；可关页右侧以 PE_IndicatorTabClose 自画「x」
//     （哪些页可关由宿主面板经 SetClosableChecker 判定——AutoCode 只有助手页
//     有「x」，日志/产物/「+」/预览无），基类内置关闭按钮在 tabInserted 里摘除；
//     运行中的助手页左侧自绘彩色闪点（4 色 500ms/拍循环，接替旧 QLabel tabButton
//     方案——tabButton 控件由基类单行布局定位，多行下会错位）；
//   · 选中：press 即选中（与基类一致）；关闭热区 press 不选中、release 仍在
//     热区才 emit tabCloseRequested；
//   · 双击：mouseDoubleClickEvent 按多行命中自查 → emit tabBarDoubleClicked
//     （面板接它做助手改名；基类按自身单行布局命中的话会点错页）；
//   · 拖拽重排：按住拖动超阈值拖起（其余 Tab 按“插入后顺序”滑移让位留洞、
//     被拖 Tab 半透明跟随光标），松手经 moveTab 落位（emit tabMoved，
//     QTabWidget 监听该信号同步页序）；
//   · 基类 tabRect/tabAt 返回的是它自己单行布局的结果，与多行视觉不一致
//     ——外部定位一律改用本类 TabRectAt/TabAtPos（面板右键菜单与自测钩子）；
//     tooltip 亦在 event() 里按自绘布局命中自处理。
// 无 Q_OBJECT（无自有信号），基类信号照常 emit，既有 connect 全部不动。
//
// CAsstTabWidget：仅用 using 把 protected 的 setTabBar 解禁，供面板在建 Tab 页
// 前换装本 Tab 栏（QTabWidget 必须在加 Tab 前调用 setTabBar）。
// ===========================================================================

#include <QTabBar>
#include <QTabWidget>
#include <QElapsedTimer>
#include <QTimer>
#include <QSet>
#include <QVector>
#include <QStyleOptionTab>

#include <functional>

class QPainter;

class CAsstTabBar : public QTabBar
{
    Q_OBJECT   // qobject_cast 观察口（测试经 tabBar() 反查本类）需要；无自有信号
public:
    explicit CAsstTabBar(QWidget* a_pParent = nullptr);

    // 页（QTabWidget::widget）→ 是否可关（自绘「x」显隐）：面板注入判定器，
    //   索引对宿主 QTabWidget 实时反查（拖动/增删后无过期索引问题）。
    //   AutoCode 口径：助手页 true，日志/阶段产物/「+」/文件预览 false。
    void SetClosableChecker(const std::function<bool(int a_iIndex)>& a_fn);

    // 页（QTabWidget::widget）→ 运行中与否：true 把该页 Tab 加入运行集合
    //   （左侧画闪点 + 启 500ms 变色时钟），false 移出（恢复素面）。
    //   以页指针为键：比索引稳（拖动换位不失效），比助手名稳（改名不影响）。
    void SetPageRunning(QWidget* a_pPage, bool a_bRunning);
    bool PageRunning(QWidget* a_pPage) const { return m_runPages.contains(a_pPage); }

    // 2026-10-02 外档幽灵 Tab（任务助手跨档运行指示）：true = 该页标题粗体绘制
    //   （宿主面板另配 tooltip 注明所属任务；点击由面板接管切回任务）。
    void SetPageForeign(QWidget* a_pPage, bool a_bForeign);
    // index→页指针解析器（QTabBar 无 widget()，粗体绘制需反查页归属）：宿主注入。
    void SetPageResolver(const std::function<QWidget*(int)>& a_fn) { m_fnPageOf = a_fn; }

    // 多行布局下的定位/命中接口（替代基类单行 tabRect/tabAt）
    QRect TabRectAt(int a_iIndex) const;         // 无效索引返回空 rect
    int TabAtPos(const QPoint& a_pt) const;      // 未命中返回 -1
    int TabRowCount() const { return m_layout.iRows; }   // 当前行数（足够宽 = 1）
    QRect CloseRectOf(const QRect& a_rcTab) const;       // Tab 头右缘「x」热区
    void NotifyTabsChanged();    // 标题/图标变化（Qt6 QTabBar 无虚拟钩子）→ 重排分行

protected:
    void paintEvent(QPaintEvent* a_pEvent) override;
    void resizeEvent(QResizeEvent* a_pEvent) override;
    void showEvent(QShowEvent* a_pEvent) override;
    void leaveEvent(QEvent* a_pEvent) override;
    void mousePressEvent(QMouseEvent* a_pEvent) override;
    void mouseMoveEvent(QMouseEvent* a_pEvent) override;
    void mouseReleaseEvent(QMouseEvent* a_pEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* a_pEvent) override;
    void tabInserted(int a_iIndex) override;
    void tabRemoved(int a_iIndex) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool event(QEvent* a_pEvent) override;

private:
    // 一次分行布局的结果：rectOf 按 Tab 索引取可视 rect，iRows/iRowH 为行数/行高
    struct LayoutResult
    {
        QVector<QRect> rectOf;
        int iRows = 1;
        int iRowH = 24;
    };

    QTabWidget* HostTabs() const;                // 宿主 QTabWidget（本栏被其收养）
    int IndexOfPage(QWidget* a_pPage) const;     // 页 → Tab 索引（不在 = -1）
    bool TabHasClose(int a_iIndex) const;        // 关闭判定器裁决（缺省 false）
    bool TabRunning(int a_iIndex) const;         // 运行判定（页指针 ∈ 运行集合）
    QVector<int> NaturalOrder() const;           // 0..count-1 的自然显示顺序
    int IdealTabWidth(int a_iIndex) const;       // 单 Tab 理想宽（含文本与关闭/闪点位）
    int RowHeight() const;                       // 单行高（基类 tabSizeHint 口径）
    LayoutResult CalcLayout(const QVector<int>& a_order) const;   // 按顺序分行
public:
    void ApplyLayout();                          // 刷新常规布局，高度变化时重算几何
    void PaintTab(QPainter& a_pPainter, int a_iIndex, const QRect& a_rcTab,
                  QStyleOptionTab::TabPosition a_ePos, bool a_bFloating);
    void UpdateHover(const QPoint& a_pt);
    void StartDrag(const QPoint& a_pt);
    void UpdateDrag(const QPoint& a_pt);
    int DropInsertAt(const QPoint& a_pt) const;  // 拖拽光标 → 抽出后插入位（moveTab 语义）
    void SyncBlinkTimer();                       // 有运行页 → 启闪点时钟；全停 → 止

    QTimer m_blinkTimer;       // 闪点变色时钟（500ms/拍，只触发重绘）
    QElapsedTimer m_blinkElapsed;   // 闪点连续运行起点（取模得当前颜色相位）
    QSet<QWidget*> m_runPages; // 运行中的页集合（页指针键，拖动/改名不失效）
    QSet<QWidget*> m_foreignPages;   // 2026-10-02 外档幽灵页集合（标题粗体）
    std::function<QWidget*(int)> m_fnPageOf;   // index→page（宿主注入；空=无外档绘制）
    QWidget* PageOfIndex(int a_iIndex) const
    { return m_fnPageOf ? m_fnPageOf(a_iIndex) : nullptr; }

    std::function<bool(int)> m_fnClosable;   // 「x」显隐判定器（宿主面板注入）

    LayoutResult m_layout;     // 常规（非拖拽）布局
    QVector<int> m_naturalOrder;   // 自然显示顺序缓存（绘制/命中共用）
    int m_iHover = -1;         // 悬停中的 Tab（-1 无）
    bool m_bHoverClose = false;    // 悬停在「x」热区上
    int m_iPress = -1;         // 按下命中的 Tab（-1 无）
    bool m_bPressClose = false;    // 按下落在「x」热区（release 仍在 → 关页）
    bool m_bPressMoved = false;    // 按下后移动超阈值（已进入拖拽）
    QPoint m_ptPress;          // 按下位置（本地坐标）
    bool m_bDragging = false;  // 拖拽重排进行中
    bool m_bGrabbed = false;   // 拖拽已 grabMouse（对应必须 release）
    QPoint m_ptDragCur;        // 拖拽中光标（本地坐标，浮动 Tab 定位用）
    QPoint m_ptDragGrab;       // 拖起时光标相对被拖 Tab 左上角的偏移
    int m_iDragFrom = -1;      // 被拖 Tab 原索引
    int m_iDragTo = -1;        // 插入位（抽出后位置，moveTab(from, to) 直用）
    QVector<int> m_dragOrder;  // 插入后显示顺序（含被拖项，滑移让位画洞用）
    LayoutResult m_dragLayout; // 拖拽预览布局（被拖项 rect = 占位洞）
};

class CAsstTabWidget : public QTabWidget
{
public:
    explicit CAsstTabWidget(QWidget* a_pParent = nullptr)
        : QTabWidget(a_pParent) {}
    using QTabWidget::setTabBar;   // 解禁 protected setTabBar（加 Tab 前换装 CAsstTabBar）
};
