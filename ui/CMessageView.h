#pragma once

#include <QScrollArea>

class QVBoxLayout;
class CMessageBlock;

// 对话流容器：QScrollArea + 垂直布局，承放 CMessageBlock。
//   - 跟随尾部：新块到达时若用户停在底部则自动滚到底；用户手动上滚则暂停跟随（Claude Code 行为）
//   - 上限保护：block 超过阈值时从顶部丢弃最旧（widget 比 text 行重，阈值保守）
//   - 反向分页（2026.09.03）：初始只挂最近一段块，滚动到顶发 sigTopReached 由外层
//     补建更早块并 PrependBlocks 锚定插回——活件数≈可视窗口+已翻页量，长会话不再全量建件
class CMessageView : public QScrollArea {
    Q_OBJECT

public:
    explicit CMessageView(QWidget* a_pParent = nullptr);

    // a_bBatch=true：批量重建路径——只入布局，跳过逐块滚底（每块滚底会触发全文档同步
    // 布局，N 块即 O(N²)，数百块时 UI 卡顿数秒）；批量完成后由调用方统一滚底一次。
    void AddBlock(CMessageBlock* a_pBlock, bool a_bBatch = false);
    void Clear();
    void ScrollToBottom();
    // 重建路径专用：Markdown 块高度异步收敛，分多帧（0/30/120ms）滚动确保滚到最底
    //（单次 adjustSize+maximum 在文档 layout 未完成时取到的 maximum 偏小，滚不到底）。
    void ScrollToBottomRobust();
    // FR-3：把指定块滚到视口最上方，并将水平滚动条归零（提示词下拉点击跳转用）。
    void ScrollBlockToTop(CMessageBlock* a_pBlock);
    // FR-4：重建型大批量追加前重置跟随标志（重建等同「跳到最新」，应强制跟随到底）。
    void ResetFollow() { m_bFollow = true; }

    // 反向分页：把一批更早的块插入视图顶部。锚定原首块（插前布局首块插后仍滚到
    //   视口顶），原可视内容纹丝不动，新块在其上方可继续上滚；分帧重锚吸收 Markdown
    //   高度异步收敛（120ms 内高度逐块落定，早锚会漂移几个像素/块）。
    void PrependBlocks(const QList<CMessageBlock*>& a_aBlocks);
    int BlockCount() const;   // 当前挂载块数（测试/外层分页判断用）

    // 2026.10.16 视口惰性排版驱动：把视口带内（上下各 2 屏余量）的惰性 Assistant 块
    //   Materialize（正文排版上屏），远离视口带（6 屏外）的 Dematerialize（弃正文
    //   保高度）。滚动/缩放/显示/分帧建件后由视图自动驱动（合并节流）；仅对批量
    //   建件的惰性块生效，流式块出生即排版不参与。
    void RefreshMaterialization();

protected:
    void resizeEvent(QResizeEvent* a_pEvent) override;   // 视口尺寸变化 → 惰性带重算
    void showEvent(QShowEvent* a_pEvent) override;       // 隐藏页（非当前 Tab）保持全惰性，切回时材化视口带

signals:
    void sigTopReached();   // 垂直滚动条到达 0（仅可滚内容且"非顶→顶"边沿触发一次）——外层补建更早块
    // 2026-10-15 底部分页：滚到底（可滚且"非底→底"边沿一次）——跳转窗口尾截断时外层
    //   补建更新一段（与到顶补建对称）；常态窗口对齐数据尾时外层收到也无事可做。
    void sigBottomReached();
    void sigBlockDropped(CMessageBlock* a_pBlock);   // 超上限丢弃最旧块前发出——外层同步并行 widget 列表

private slots:
    void slotScrolled(int a_iValue);   // 跟随尾部启发式

private:
    void QueueRefreshMaterialization();   // 惰性带重算排队（0ms 合并节流：滚动风暴/批量建件）

    QWidget*     m_pContent  = nullptr;
    QVBoxLayout* m_pLayout   = nullptr;
    bool         m_bFollow   = true;    // 是否自动跟随到底部
    bool         m_bTopNotified = false;   // 已发过 sigTopReached（在顶期间不重复发，离开顶部后复位）
    bool         m_bBottomNotified = false;   // 2026-10-15 已发过 sigBottomReached（在底期间不重复发，离开底部后复位）
    bool         m_bMatRefreshQueued = false;   // 2026.10.16 惰性带重算已排队（合并同帧多次触发）
};
