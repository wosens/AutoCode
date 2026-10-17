#pragma once

// ---------------------------------------------------------------------------
// CChatDocPool —— 对话流 Assistant 正文 QTextDocument LRU 池（2026-10-17 自绘三期）。
// 自 HarnessAgent v1.9.44 ChatDocPool 移植（doc/对话流视图自绘改造方案.md §7.3）：
// 集中管理条目正文 document：视口带（±2 屏行距）内预材化、距视口超 6 屏行距逐出、
// 上限 MIN(64 docs, 32MB)；宽度变化池内 document setTextWidth 重排并回调
// fnDocReady（delegate 校正实高）。不做位图缓存、不碰模型数据（取正文经视图注入
// 的 fnFetchText 回调，池不识 CChatModel 类型——分层逃生舱约束，设计 §4.2）。
// 逐出保留 delegate 端已校正高度缓存（估高同款锁形，滚动条不呼吸）。
// 无 Q_OBJECT（QObject 基类仅为 parent 链自动清理），LRU 双向键链表 + QHash。
// ---------------------------------------------------------------------------

#include <QHash>
#include <QObject>
#include <QString>
#include <functional>

class QTextDocument;

class CChatDocPool : public QObject
{
public:
    explicit CChatDocPool(QObject* a_pParent = nullptr);
    ~CChatDocPool() override;

    QTextDocument* Acquire(int a_iItem, const QString& a_sMarkdown, int a_iWidth);
    // 池命中时校验内容快照（流式追加正文变化 → setMarkdown 重材化 + fnDocReady 高度
    // 回调，实时刷新；否则已材化条目冻结在首帧片段）与宽度（不同宽重排）；
    // 未命中同步材化（setMarkdown+setTextWidth）入池。超限按 LRU 逐出至满足两上限
    // （永不逐出本次 Acquire 的条目，除非它自身独占超限）。
    // 返回值与传给 fnDocReady 的 doc 均为局部指针快照：命中路径不持 QHash 条目引用
    // 跨回调/逐出的 remove（回填搬移/重入插入 rehash 会使引用失效乃至字节级损坏，
    // 快照后正确性由构造保证）

    QTextDocument* Peek(int a_iItem) const;         // 只看不材化（链接 hit-test 用）
    void OnBand(int a_iFirstRow, int a_iLastRow, int a_iViewportY, int a_iViewportH);
    // 滚动 16ms 节流驱动：±2 屏（行距）预材化 / >6 屏（行距）逐出（像素距离的行距近似）
    void OnWidthChanged(int a_iNewWidth);           // 池内 doc 逐个 setTextWidth 重排→fnDocReady
                                                    // （先重排收集快照、遍历外统一回调：隔离重入）
    void Clear();                                   // 模型 Reset/视图重载时清池（doc 全删）
    int DocCount() const { return static_cast<int>(m_aDocs.size()); }
    qint64 EstBytes() const { return m_iEstTotal; } // est = 文本字节×2 + 4KB/doc 摊销

    std::function<void(int a_iItem, QTextDocument* a_pDoc)> fnDocReady;
    // 材化/重排完成 → delegate 校正高度（只传行号与 doc，不跨 Reset 持指针）。
    // 契约：回调内禁止销毁本池/调用 Clear；其余池接口可重入——Acquire 已局部快照、
    // OnWidthChanged 已先收集后回调，重入不破坏正确性
    std::function<QString(int a_iItem)> fnFetchText;
    // 条目→Markdown 正文（视图注入；返回空串 = 该条目无需 doc，非 Assistant 条目）

    static constexpr int k_iMaxDocs = 64;
    static constexpr qint64 k_iMaxBytes = 32 * 1024 * 1024;

private:
    void touch(int a_iItem);                        // LRU 提到 MRU 端（仅限已在链表中的条目）
    void pushMru(int a_iItem);                      // 未入链条目接到 MRU 端（新插入/touch 摘出后）
    void evictLru(int a_iSkipItem);                 // 逐出 LRU 端条目（delete doc）直至满足两上限

    struct SEntry {
        QTextDocument* pDoc = nullptr;
        QString sMd;                                // 材化时的正文快照（命中比对：流式增长→重材化）
        int iWidth = 0;                             // 材化/重排时的文本宽（不同宽=失效重排）
        qint64 iEst = 0;                            // est = sText 字节×2 + 4KB 摊销
        int iPrev = -1, iNext = -1;                 // 双向键链表（-1=端）
    };
    QHash<int, SEntry> m_aDocs;                     // 条目行号 → 池条目
    int m_iKeyMru = -1, m_iKeyLru = -1;             // MRU/LRU 端键
    qint64 m_iEstTotal = 0;
    int m_iCurWidth = 0;                            // 当前文本宽（OnBand 预材化用）
};
