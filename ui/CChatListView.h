#pragma once

// ---------------------------------------------------------------------------
// CChatListView —— 对话流 QListView 派生视图（2026-10-17 自绘三期，方案 §6；
// 参照 HarnessAgent v1.9.44 ChatListView 移植并适配 AutoCode 视觉常量；
// 2026-10-23 借鉴 HarnessAgent v1.9.50 条目选择复制与浮层稳定性两修）。
// 虚拟化：模型持全量条目、仅可视区绘制，滚动全程零消息子 QWidget（复制覆盖层
// 为瞬时交互件，方案 §8 例外口径）。构造即建 delegate+doc 池并接线（唯一 wiring
// 点）：pool.fnDocReady → delegate.OnDocReady；delegate.fnHeightCorrected → 锚定
// 补偿 + model->TouchSizeHint；滚动条 valueChanged → 16ms 节流 → pool.OnBand
//（±2 屏材化 / >6 屏逐出）；resize → delegate.InvalidateWidth。
// 贴底跟随复刻 CMessageView 口径（valueChanged max-4 贴底、rangeChanged
// autoScroll&&atBottom 滚底）；JumpToItem = ScrollBlockToTop 等价（滚到视口顶部
// + #fffbeb 高亮 1200ms 自清）。双击任意块浮只读 QTextBrowser 复制覆盖层
//（2026-10-22 全块型开放：Assistant=池 Markdown doc 保持源格式，其余=纯文本
// 自建 doc，层内拖选 + Ctrl+C；ToolUse/System 折叠头区例外——双击归折叠语义），
// Assistant 路径覆盖层持池 doc：任何滚动（含贴底自动滚）即收起——防带外/LRU 逐出悬垂 UAF，
// 亦防 viewport 子控件不随内容滚动的浮层错位。
// 2026-10-23 条目选择复制（借鉴 HarnessAgent v1.9.50）：ExtendedSelection+整行
// +框选（点击/Shift/Ctrl/空白处框选），选中底色由 delegate 按 State_Selected 绘制
//（浅蓝 #dbeafe 不遮字）；Ctrl+C / 右键菜单「复制/全选」把所选条目纯文本
//（Assistant=源文本，其余=PlainTextAt 口径）按行号升序拼进剪贴板——与双击浮层
// 互补：浮层=单条目精细文本选择，条目选择=多条目整块搬运。
// 无 Q_OBJECT（信号经 lambda 连接，同 CChatDocPool 先例）。
// ---------------------------------------------------------------------------

#include <QListView>
#include <QPointer>
#include <QTimer>

class CChatModel;
class CChatDelegate;
class CChatDocPool;
class QTextBrowser;

class CChatListView : public QListView
{
public:
    explicit CChatListView(QWidget* a_pParent = nullptr);
    ~CChatListView() override;                 // 2026-10-23：先收浮层再入基类析构（防池删 doc
                                               // 先于 viewport 删浮层的析构序 UAF）

    void SetModel(CChatModel* a_pModel);       // 绑模型 + 接 modelReset 清池/失效缓存
    void SetAutoScroll(bool a_bOn);            // 批量装载关跟随；恢复后调用方再 ScrollToBottom
    void ScrollToBottom();
    bool JumpToItem(int a_iItemRow);           // 条目下标：滚到视口顶部 + 高亮 1200ms（提示词跳转）
    void ApplyFont(const QFont& a_oFont);      // 视图字体变更（11pt 回填）：转发 delegate + 缓存失效
    void CopySelection();                      // 所选条目纯文本按行号升序 → 剪贴板（Ctrl+C/右键共用；测试直调）
    CChatModel* Model() const { return m_pModel; }
    CChatDelegate* Delegate() const { return m_pDelegate; }
    CChatDocPool* DocPool() const { return m_pPool; }   // 测试观察口
    bool OverlayVisible() const;              // 复制层在席（测试观察口）
    QString OverlayText() const;              // 复制层内容（测试观察口；空=未开)
    QString OverlayStyleSheet() const;        // 复制层 QSS（测试观察口：锁底色不透明防重影）

protected:
    void resizeEvent(QResizeEvent* a_ev) override;          // 宽度变化 → InvalidateWidth
    void mouseDoubleClickEvent(QMouseEvent* a_ev) override; // 任意块 → 浮复制覆盖层（头区归折叠）
    void mousePressEvent(QMouseEvent* a_ev) override;       // 覆盖层打开时点击条目外收起
    void keyPressEvent(QKeyEvent* a_ev) override;           // Ctrl+C → CopySelection（条目选择复制）
    void contextMenuEvent(QContextMenuEvent* a_ev) override;// 右键「复制/全选」（条目级选择）
    void focusOutEvent(QFocusEvent* a_ev) override;         // 焦点移到其他控件 → 取消条目选择
    bool viewportEvent(QEvent* a_ev) override;              // ToolTip → delegate（工具卡头摘要）

private:
    void updateBand();                        // 视口行区间 → pool.OnBand（滚动节流驱动）
    void onHeightCorrected(int a_iRow, int a_iDeltaY);      // 实高校正：TouchSizeHint + 视口上方锚定补偿
    void openOverlay(const QModelIndex& a_idx);
    void closeOverlay();
    // 2026-10-23 v5.0.416 栈溢出修复（三连崩 0xC00000FD，切助手必现）：统一宽度重排入口。
    // Qt 的 QAbstractScrollArea::viewportEvent 把 viewport 的 QEvent::Resize 直接
    // return QFrame::event(e) 转进部件 resizeEvent——重排链（InvalidateWidth→池逐条
    // reformat→fnDocReady→OnDocReady→onHeightCorrected→visualRect 同步布局→滚动条
    // 显隐翻转→viewport 宽 ±10→viewport Resize）会同步重入重排，无限递归直到栈耗尽。
    // 挡板：重排执行期不重入，记 pending；链条退出后延迟一拍处理（滚动条几何已收敛，
    // 同宽自然跳过）。a_bForce=true 绕过同宽检查（模型重绑/字体变更：池/缓存已失效
    // 必须重排，即使宽度未变）。
    void ApplyWidth(int a_iWidth, bool a_bForce = false);

    CChatModel* m_pModel = nullptr;           // 不持有（面板/助手页持有，parent 链清理）
    CChatDelegate* m_pDelegate = nullptr;     // 本视图持有（子对象）
    CChatDocPool* m_pPool = nullptr;          // 本视图持有（子对象，doc 以池为 parent）
    bool m_bAutoScroll = true;                // 贴底跟随总开关（false = 批量装载中）
    bool m_bAtBottom = true;                  // 滚动条贴底时新内容自动滚底
    QPointer<QTextBrowser> m_pOverlay;        // 复制覆盖层（QPointer 防悬垂）
    int m_iOverlayRow = -1;
    QTimer* m_pBandTimer = nullptr;           // 滚动 16ms 节流器
    int m_iWidthLast = 0;                     // 上次注入 delegate 的估宽（去重）
    bool m_bInWidthReformat = false;          // 宽度重排链执行中（防 viewportEvent 转发重入）
    int  m_iPendingWidth = -1;                // 重排期间到达的新宽度（链条退出后延迟处理）
    int  m_iDeferLoops = 0;                   // 延迟收敛限流（震荡 >32 拍强制吸收当前宽）
};
