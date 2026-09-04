#include "CMessageView.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QList>
#include <QTimer>
#include <QPointer>

#include "CMessageBlock.h"

namespace {
const int k_iMaxBlocks = 1500;   // 上限保护：超过则从顶部丢弃最旧
}

CMessageView::CMessageView(QWidget* a_pParent)
    : QScrollArea(a_pParent) {
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    // 助手/任务日志区：关闭横向滚动条，内容按视口宽度自动换行（CMessageBlock 内文本控件
    //   均已开 wordWrap / WidgetWidth，无需横向滚动）。
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(QStringLiteral("QScrollArea { background: #ffffff; border: none; }"
                                 "QScrollBar:vertical { width: 10px; }"));

    m_pContent = new QWidget(this);
    // 2026.08.26 选择器限定：无选择器样式表会级联到后代右键菜单等弹出控件（虽然白色
    //   无害，但控件级规则会覆盖全局 QMenu 样式表，一并规范掉）。
    m_pContent->setObjectName(QStringLiteral("msgContent"));
    m_pContent->setStyleSheet(
        QStringLiteral("QWidget#msgContent { background: #ffffff; }"));
    m_pLayout = new QVBoxLayout(m_pContent);
    m_pLayout->setContentsMargins(12, 12, 12, 12);
    m_pLayout->setSpacing(10);
    m_pLayout->addStretch(1);   // 顶部对齐：底部弹簧把内容顶到上沿

    setWidget(m_pContent);

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            this, &CMessageView::slotScrolled);
}

void CMessageView::AddBlock(CMessageBlock* a_pBlock, bool a_bBatch) {
    if (a_pBlock == nullptr) {
        return;
    }
    // 插在 stretch 之前（末尾追加新块，stretch 始终在最后）
    const int iIdx = m_pLayout->count() - 1;   // stretch 的位置
    m_pLayout->insertWidget(iIdx, a_pBlock);

    // 上限保护：丢弃最旧的（第一个非 stretch 项）
    // 非空 block 数 = count - 1（去掉 stretch）
    while (m_pLayout->count() - 1 > k_iMaxBlocks) {
        QLayoutItem* pItem = m_pLayout->takeAt(0);
        if (pItem != nullptr && pItem->widget() != nullptr) {
            QWidget* pW = pItem->widget();
            if (CMessageBlock* pBlock = qobject_cast<CMessageBlock*>(pW)) {
                emit sigBlockDropped(pBlock);
            }
            pW->deleteLater();
        }
        delete pItem;
    }

    if (m_bFollow && !a_bBatch) {
        ScrollToBottom();
    }
}

void CMessageView::Clear() {
    // 2026.09.04 从尾部摘除：takeAt(0) 头摘在 QVBoxLayout 内部列表上是 O(n)，
    //   千余块（上限 1500）整体 O(n²) 且集中在一次调用里——切档/解绑时主线程卡一截
    //   的隐藏元凶之一；尾摘 O(1)，总 O(n)。块销毁仍走 deleteLater 分批归还事件循环。
    while (m_pLayout->count() > 1) {   // 保留末尾 stretch（count-1 项）
        QLayoutItem* pItem = m_pLayout->takeAt(m_pLayout->count() - 2);
        if (pItem != nullptr && pItem->widget() != nullptr) {
            pItem->widget()->deleteLater();
        }
        delete pItem;
    }
}

int CMessageView::BlockCount() const {
    return (m_pLayout != nullptr) ? qMax(0, m_pLayout->count() - 1) : 0;   // 去掉尾部 stretch
}

void CMessageView::PrependBlocks(const QList<CMessageBlock*>& a_aBlocks) {
    // 反向分页：把更早的块按序插到布局顶（原首块之前）。滚动位置锥定在原首块——
    //   插后把它滚回视口顶，用户看到的内容不变，新块在其上方可继续上滚。
    if (a_aBlocks.isEmpty() || m_pLayout == nullptr) {
        return;
    }
    // 锚块 = 插入前的首个非 stretch 块（必存在：分页只在已有后续块时才补更早）。
    //   QPointer 防护：锥定重试帧问隔内若被上限丢弃（deleteLater 销毁），自动失效不再解引。
    QPointer<CMessageBlock> pAnchor;
    if (m_pLayout->count() > 1) {
        if (QLayoutItem* pItem = m_pLayout->itemAt(0); pItem != nullptr) {
            pAnchor = qobject_cast<CMessageBlock*>(pItem->widget());
        }
    }
    m_bFollow = false;   // 补历史 = 用户在上方浏览，暂停尾部跟随
    for (int i = a_aBlocks.size() - 1; i >= 0; --i) {
        m_pLayout->insertWidget(0, a_aBlocks.at(i));
    }
    if (pAnchor == nullptr) {
        return;   // 无锚（理论不达）：块已插入，滚动位置交给后续滚动/重建
    }
    // 与 ScrollBlockToTop 同款锥定手法：同步布局后把锚块滚回视口顶；多帧重锥吸收
    //   Markdown documentSizeChanged 的异步高度收敛（早锥会随高度落定向下漂移）。
    auto fnAnchor = [this, pAnchor]() {
        if (m_pContent == nullptr) {
            return;
        }
        m_pContent->adjustSize();
        const int iY = pAnchor->mapTo(m_pContent, QPoint(0, 0)).y();
        QScrollBar* pBar = verticalScrollBar();
        if (pBar != nullptr) {
            pBar->setValue(qBound(pBar->minimum(), iY, pBar->maximum()));
        }
    };
    // 多帧重锚吸收高度异步收敛：①Markdown documentSizeChanged 逐块落定；②2026.09.04
    //   起批量路径 Assistant 块子线程预排版落地波（占位纯文本→富文本高度跳变，线程池
    //   并行排队下持续数百 ms）——重锚帧拉长到 500ms 覆盖落地尾，早帧保即时稳定。
    fnAnchor();
    QTimer::singleShot(0, this, fnAnchor);
    QTimer::singleShot(60, this, fnAnchor);
    QTimer::singleShot(150, this, fnAnchor);
    QTimer::singleShot(300, this, fnAnchor);
    QTimer::singleShot(500, this, fnAnchor);
}

void CMessageView::ScrollToBottom() {
    // FR-4（重建路径修复）：先强制同步布局使 verticalScrollBar()->maximum() 反映新内容，
    // 再立即 setValue 滚到底，最后保留一次 singleShot(0) 兜底吸收极少数排队 layout。
    // 注意（R5）：不用 QCoreApplication::processEvents，避免重入 queued 事件；adjustSize 已能
    // 触发内容 widget 的同步尺寸重算。
    if (m_pContent != nullptr) {
        m_pContent->adjustSize();
    }
    QScrollBar* pBar = verticalScrollBar();
    if (pBar != nullptr) {
        pBar->setValue(pBar->maximum());
    }
    QTimer::singleShot(0, this, [this]() {
        QScrollBar* pBar2 = verticalScrollBar();
        if (pBar2 != nullptr) {
            pBar2->setValue(pBar2->maximum());
        }
    });
}

void CMessageView::ScrollToBottomRobust() {
    // 重建路径（批量恢复/切任务）专用：Markdown 块高度异步收敛，分多帧滚动覆盖各 layout
    // 完成时刻，确保真正滚到最底。流式 AddBlock 仍用单帧 ScrollToBottom，避免定时器堆积。
    auto fnBottom = [this]() {
        if (m_pContent != nullptr) {
            m_pContent->adjustSize();   // 同步内容尺寸，使 maximum() 反映真实高度
        }
        QScrollBar* pBar = verticalScrollBar();
        if (pBar != nullptr) {
            pBar->setValue(pBar->maximum());
        }
    };
    fnBottom();
    QTimer::singleShot(0, this, fnBottom);
    QTimer::singleShot(30, this, fnBottom);
    QTimer::singleShot(120, this, fnBottom);
}

void CMessageView::ScrollBlockToTop(CMessageBlock* a_pBlock) {
    // FR-3：把指定块滚到视口最上方，横纵滚动条位置归零（水平归零）。
    // 块是 m_pContent 的孙级 widget（经布局托管），mapTo(m_pContent) 得到块顶在内容坐标的 y。
    if (a_pBlock == nullptr || m_pContent == nullptr) {
        return;
    }
    m_pContent->adjustSize();   // 强制同步布局，使滚动条 maximum 反映真实内容尺寸
    const int iY = a_pBlock->mapTo(m_pContent, QPoint(0, 0)).y();
    auto fnScroll = [this, iY]() {
        QScrollBar* pV = verticalScrollBar();
        if (pV != nullptr) {
            pV->setValue(qBound(pV->minimum(), iY, pV->maximum()));
        }
        QScrollBar* pH = horizontalScrollBar();
        if (pH != nullptr) {
            pH->setValue(0);   // FR-3：横纵滚动条位置为 0
        }
        m_bFollow = false;   // 手动跳转后停止跟随，避免下一块到达又滚到底
    };
    fnScroll();
    QTimer::singleShot(0, this, fnScroll);   // 兜底吸收排队 layout
}

void CMessageView::slotScrolled(int a_iValue) {
    const QScrollBar* pBar = verticalScrollBar();
    const int iMax = pBar->maximum();
    // 接近底部（容差）→ 恢复跟随；否则停止跟随
    m_bFollow = (iMax - a_iValue) <= 24;
    // 反向分页：滚到顶（仅可滚且"非顶→顶"边沿发一次）→ 外层补建更早块；
    //   在顶停留期间不重复发，滚下去再滚回来才再次触发。无可滚内容（iMax=0）不发。
    if (a_iValue <= 0 && iMax > 0) {
        if (!m_bTopNotified) {
            m_bTopNotified = true;
            emit sigTopReached();
        }
    } else {
        m_bTopNotified = false;
    }
}
