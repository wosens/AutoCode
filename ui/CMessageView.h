#pragma once

#include <QScrollArea>

class QVBoxLayout;
class CMessageBlock;

// 对话流容器：QScrollArea + 垂直布局，承放 CMessageBlock。
//   - 跟随尾部：新块到达时若用户停在底部则自动滚到底；用户手动上滚则暂停跟随（Claude Code 行为）
//   - 上限保护：block 超过阈值时从顶部丢弃最旧（widget 比 text 行重，阈值保守）
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

private slots:
    void slotScrolled(int a_iValue);   // 跟随尾部启发式

private:
    QWidget*     m_pContent  = nullptr;
    QVBoxLayout* m_pLayout   = nullptr;
    bool         m_bFollow   = true;    // 是否自动跟随到底部
};
