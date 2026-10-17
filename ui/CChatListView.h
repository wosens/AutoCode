#pragma once

// ---------------------------------------------------------------------------
// CChatListView —— 对话流 QListView 派生视图（2026-10-17 自绘三期，方案 §6；
// 参照 HarnessAgent v1.9.44 ChatListView 移植并适配 AutoCode 视觉常量）。
// 虚拟化：模型持全量条目、仅可视区绘制，滚动全程零消息子 QWidget（复制覆盖层
// 为瞬时交互件，方案 §8 例外口径）。构造即建 delegate+doc 池并接线（唯一 wiring
// 点）：pool.fnDocReady → delegate.OnDocReady；delegate.fnHeightCorrected → 锚定
// 补偿 + model->TouchSizeHint；滚动条 valueChanged → 16ms 节流 → pool.OnBand
//（±2 屏材化 / >6 屏逐出）；resize → delegate.InvalidateWidth。
// 贴底跟随复刻 CMessageView 口径（valueChanged max-4 贴底、rangeChanged
// autoScroll&&atBottom 滚底）；JumpToItem = ScrollBlockToTop 等价（滚到视口顶部
// + #fffbeb 高亮 1200ms 自清）。双击 Assistant 浮只读 QTextBrowser 复制覆盖层，
// 覆盖层持有池 doc：任何滚动（含贴底自动滚）即收起——防带外/LRU 逐出悬垂 UAF，
// 亦防 viewport 子控件不随内容滚动的浮层错位。
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

    void SetModel(CChatModel* a_pModel);       // 绑模型 + 接 modelReset 清池/失效缓存
    void SetAutoScroll(bool a_bOn);            // 批量装载关跟随；恢复后调用方再 ScrollToBottom
    void ScrollToBottom();
    bool JumpToItem(int a_iItemRow);           // 条目下标：滚到视口顶部 + 高亮 1200ms（提示词跳转）
    void ApplyFont(const QFont& a_oFont);      // 视图字体变更（11pt 回填）：转发 delegate + 缓存失效
    CChatModel* Model() const { return m_pModel; }
    CChatDelegate* Delegate() const { return m_pDelegate; }
    CChatDocPool* DocPool() const { return m_pPool; }   // 测试观察口

protected:
    void resizeEvent(QResizeEvent* a_ev) override;          // 宽度变化 → InvalidateWidth
    void mouseDoubleClickEvent(QMouseEvent* a_ev) override; // Assistant → 浮复制覆盖层
    void mousePressEvent(QMouseEvent* a_ev) override;       // 覆盖层打开时点击条目外收起
    bool viewportEvent(QEvent* a_ev) override;              // ToolTip → delegate（工具卡头摘要）

private:
    void updateBand();                        // 视口行区间 → pool.OnBand（滚动节流驱动）
    void onHeightCorrected(int a_iRow, int a_iDeltaY);      // 实高校正：TouchSizeHint + 视口上方锚定补偿
    void openOverlay(const QModelIndex& a_idx);
    void closeOverlay();

    CChatModel* m_pModel = nullptr;           // 不持有（面板/助手页持有，parent 链清理）
    CChatDelegate* m_pDelegate = nullptr;     // 本视图持有（子对象）
    CChatDocPool* m_pPool = nullptr;          // 本视图持有（子对象，doc 以池为 parent）
    bool m_bAutoScroll = true;                // 贴底跟随总开关（false = 批量装载中）
    bool m_bAtBottom = true;                  // 滚动条贴底时新内容自动滚底
    QPointer<QTextBrowser> m_pOverlay;        // 复制覆盖层（QPointer 防悬垂）
    int m_iOverlayRow = -1;
    QTimer* m_pBandTimer = nullptr;           // 滚动 16ms 节流器
    int m_iWidthLast = 0;                     // 上次注入 delegate 的估宽（去重）
};
