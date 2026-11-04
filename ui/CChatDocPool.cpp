#include "CChatDocPool.h"

#include <QList>
#include <QPair>
#include <QTextDocument>

CChatDocPool::CChatDocPool(QObject* a_pParent)
    : QObject(a_pParent)
{
}

CChatDocPool::~CChatDocPool()
{
    // 析构路径不回调：池是视图子对象，运行于 ~QObject 子对象删除序列，回调
    //（fnDocAboutToDelete 捕获视图 this → closeOverlay 访问 verticalScrollBar/
    //浮层）可能碰到已删兄弟控件——关停前由属主显式 Clear 摘引用，此处只释放内存。
    fnDocAboutToDelete = nullptr;
    fnDocReady = nullptr;
    fnFetchText = nullptr;
    Clear();
}

void CChatDocPool::touch(int a_iItem)
{
    // 已在 MRU 端则零成本
    if (m_iKeyMru == a_iItem)
        return;
    // 摘出（只对已在链表中的条目有效；新插入的条目不经此处——见 Acquire 的 pushMru）。
    // find+判空：operator[] 缺键会静默插入坏条目打断 LRU 链，逐出从此失效
    auto it = m_aDocs.find(a_iItem);
    Q_ASSERT(it != m_aDocs.end());
    if (it == m_aDocs.end())
        return;
    SEntry& e = it.value();
    if (e.iPrev >= 0)
        m_aDocs[e.iPrev].iNext = e.iNext;
    else
        m_iKeyLru = e.iNext;
    if (e.iNext >= 0)
        m_aDocs[e.iNext].iPrev = e.iPrev;
    else
        m_iKeyMru = e.iPrev;
    pushMru(a_iItem);
}

void CChatDocPool::pushMru(int a_iItem)
{
    // 接到 MRU 端（条目当前不在链表中：新插入或已被 touch 摘出）
    auto it = m_aDocs.find(a_iItem);
    Q_ASSERT(it != m_aDocs.end());
    if (it == m_aDocs.end())
        return;
    SEntry& e = it.value();
    e.iPrev = m_iKeyMru;
    e.iNext = -1;
    if (m_iKeyMru >= 0)
        m_aDocs[m_iKeyMru].iNext = a_iItem;
    else
        m_iKeyLru = a_iItem;                // 链表原本为空
    m_iKeyMru = a_iItem;
}

void CChatDocPool::evictLru(int a_iSkipItem)
{
    while (m_aDocs.size() > k_iMaxDocs || m_iEstTotal > k_iMaxBytes) {
        if (m_iKeyLru < 0 || (m_aDocs.size() == 1 && m_iKeyLru == a_iSkipItem))
            break;                          // 只剩保护对象：不再逐出（接受单条超限）
        const int iKey = m_iKeyLru;
        if (iKey == a_iSkipItem) {
            // 跳过保护对象，逐出次旧（MRU 方向找第一个非保护条目）
            const int iNext = m_aDocs[iKey].iNext;
            if (iNext < 0)
                break;
            const int iVictim = iNext;
            QTextDocument* const pDoc = m_aDocs[iVictim].pDoc;   // 快照：回调可重入池
            const int iPrev = m_aDocs[iVictim].iPrev;
            const int iNextV = m_aDocs[iVictim].iNext;
            m_iEstTotal -= m_aDocs[iVictim].iEst;
            if (fnDocAboutToDelete)          // 删除前通知（2026-10-23：浮层摘引用防 UAF）
                fnDocAboutToDelete(iVictim);
            delete pDoc;                             // 逐出即 delete（只用局部快照）
            m_aDocs.remove(iVictim);
            m_aDocs[iPrev].iNext = iNextV;          // iPrev ≥ 0（victim 前方必有保护对象）
            if (iNextV >= 0)
                m_aDocs[iNextV].iPrev = iPrev;
            else
                m_iKeyMru = iPrev;
            continue;
        }
        QTextDocument* const pDocLru = m_aDocs[iKey].pDoc;   // 快照：回调可重入池（rehash
        const int iNextKey = m_aDocs[iKey].iNext;            // 后条目引用悬垂，禁跨回调用）
        m_iEstTotal -= m_aDocs[iKey].iEst;
        if (fnDocAboutToDelete)              // 删除前通知（2026-10-23：浮层摘引用防 UAF）
            fnDocAboutToDelete(iKey);
        delete pDocLru;
        m_aDocs.remove(iKey);
        m_iKeyLru = iNextKey;
        if (iNextKey < 0)
            m_iKeyMru = -1;                 // 池空
        else
            m_aDocs[iNextKey].iPrev = -1;
    }
}

QTextDocument* CChatDocPool::Acquire(int a_iItem, const QString& a_sMarkdown, int a_iWidth)
{
    if (a_iWidth > 0)
        m_iCurWidth = a_iWidth;
    auto it = m_aDocs.find(a_iItem);
    if (it != m_aDocs.end()) {
        SEntry& e = it.value();
        // 先取局部指针快照——下方 fnDocReady 回调与 evictLru 的 remove 都可能改动
        // m_aDocs（回调重入插入 → rehash/搬移条目存储；逐出 remove 的回填修复），
        // 持 QHash 条目引用跨这些步骤读取/返回即悬垂或字节级损坏。返回值正确性由
        // 局部快照构造保证，不依赖「回调链不重入池」约定
        QTextDocument* const pRet = e.pDoc;
        // 命中校验：内容/宽度任一变化都须重材化并回调高度校正，否则流式追加的
        // 条目正文与高度冻结在首次材化时刻（仅 Reset/滚出带外自愈）
        const bool bMdDiff = (e.sMd != a_sMarkdown);
        const bool bWidthDiff = (a_iWidth > 0 && e.iWidth != a_iWidth);
        if (bMdDiff) {
            m_iEstTotal -= e.iEst;
            pRet->setMarkdown(a_sMarkdown);
            if (!bWidthDiff && e.iWidth > 0)
                pRet->setTextWidth(e.iWidth);     // setMarkdown 清宽：宽度未变时恢复原口径
                                                // （宽度也变则跳过旧宽重排，下步新宽一次到位）
            e.sMd = a_sMarkdown;
            e.iEst = static_cast<qint64>(a_sMarkdown.size()) * 2 + 4096;
            m_iEstTotal += e.iEst;
        }
        if (bWidthDiff) {
            pRet->setTextWidth(a_iWidth);         // 宽度失效：重排
            e.iWidth = a_iWidth;
        }
        if (bMdDiff || bWidthDiff) {
            if (fnDocReady)
                fnDocReady(a_iItem, pRet);        // 重材化/重排 → delegate 校正实高
            evictLru(a_iItem);                    // 内容增长后 est 变大，复查两上限
        }
        touch(a_iItem);                           // 内部自查找，不持上面的引用
        return pRet;
    }
    SEntry e;
    e.pDoc = new QTextDocument(this);       // 池为 doc parent（Clear/逐出即 delete）
    e.pDoc->setMarkdown(a_sMarkdown);
    if (a_iWidth > 0)
        e.pDoc->setTextWidth(a_iWidth);
    e.sMd = a_sMarkdown;
    e.iWidth = a_iWidth;
    e.iEst = static_cast<qint64>(a_sMarkdown.size()) * 2 + 4096;
    m_aDocs.insert(a_iItem, e);
    m_iEstTotal += e.iEst;
    QTextDocument* const pRet = e.pDoc;     // 先取局部指针快照（与命中路径同款纪律：
                                            // fnDocReady 回调链若重入池（插入/逐出）
                                            // 不持 QHash 条目引用跨 remove/rehash）
    pushMru(a_iItem);                       // 新条目不在链表中，直接接 MRU 端
    // 2026-10-22 首次材化也回调高度校正（实害：一次性整段回复（claude/pi/hermes
    // 单条 sigAssistantText）只有一次 Acquire，旧版仅内容变化路径回调 → 算术估高
    // 永不被实高校正，doc 绘制溢出行矩形，后续「已完成」等事件行叠画在回复尾部上。
    // 回调链只碰 delegate 高度缓存 + 视图布局（TouchSizeHint→dataChanged 为惰性
    // 重排），不重入池插入/逐出——契约「其余池接口可重入」不受影响。
    if (fnDocReady)
        fnDocReady(a_iItem, pRet);
    evictLru(a_iItem);                      // 新条目受保护，逐出旧条目至上限内
    return pRet;
}

QTextDocument* CChatDocPool::Peek(int a_iItem) const
{
    auto it = m_aDocs.constFind(a_iItem);
    return it != m_aDocs.constEnd() ? it.value().pDoc : nullptr;
}

void CChatDocPool::OnBand(int a_iFirstRow, int a_iLastRow, int a_iViewportY, int a_iViewportH)
{
    Q_UNUSED(a_iViewportY);
    Q_UNUSED(a_iViewportH);
    if (m_iCurWidth <= 0)
        return;
    const int iRowsVisible = qMax(1, a_iLastRow - a_iFirstRow + 1);
    const int iKeep = iRowsVisible * 6;     // >6 屏（行距近似）外逐出
    const int iPrefetch = iRowsVisible * 2; // ±2 屏（行距近似）预材化（拖帧缓解）
    // 逐出带外条目（键=行号单调，直接按行号窗口判）
    QList<int> aDrop;
    for (auto it = m_aDocs.begin(); it != m_aDocs.end(); ++it) {
        if (it.key() < a_iFirstRow - iKeep || it.key() > a_iLastRow + iKeep)
            aDrop.append(it.key());
    }
    for (int k : aDrop) {
        // 快照后回调再删（同 evictLru 纪律）：OnBand 删除路径同样遵守
        //   fnDocAboutToDelete 契约（旧版漏调——浮层裸持被删 doc 即 UAF，正是
        //   v5.0.412 要防的形态）；回调可重入池，禁持条目引用跨回调。
        QTextDocument* const pDoc = m_aDocs[k].pDoc;
        const int iPrev = m_aDocs[k].iPrev, iNext = m_aDocs[k].iNext;
        m_iEstTotal -= m_aDocs[k].iEst;
        if (fnDocAboutToDelete)
            fnDocAboutToDelete(k);
        delete pDoc;
        m_aDocs.remove(k);
        if (iPrev >= 0)
            m_aDocs[iPrev].iNext = iNext;
        else
            m_iKeyLru = iNext;
        if (iNext >= 0)
            m_aDocs[iNext].iPrev = iPrev;
        else
            m_iKeyMru = iPrev;
    }
    // 带内预材化（±2 屏）：只材化 Assistant 正文（fnFetchText 返回空串即跳过）
    if (!fnFetchText)
        return;
    for (int r = a_iFirstRow - iPrefetch; r <= a_iLastRow + iPrefetch; ++r) {
        if (r < 0 || m_aDocs.contains(r))
            continue;
        const QString sMd = fnFetchText(r);
        if (sMd.isEmpty())
            continue;
        Acquire(r, sMd, m_iCurWidth);       // 内部触发 fnDocReady → delegate 校正高度
    }
}

void CChatDocPool::OnWidthChanged(int a_iNewWidth)
{
    m_iCurWidth = a_iNewWidth;
    if (a_iNewWidth <= 0)
        return;
    // 先重排收集、遍历外统一回调（防御性加固）：回调链若重入池（插入/逐出/清洗）
    // 会使遍历迭代器失效——快照 (行号, doc) 隔离，正确性不依赖回调自律
    QList<QPair<int, QTextDocument*>> aReady;
    for (auto it = m_aDocs.begin(); it != m_aDocs.end(); ++it) {
        SEntry& e = it.value();
        if (e.iWidth == a_iNewWidth)
            continue;
        e.pDoc->setTextWidth(a_iNewWidth);
        e.iWidth = a_iNewWidth;
        aReady.append(qMakePair(it.key(), e.pDoc));
    }
    if (fnDocReady) {
        for (const auto& pr : aReady)
            fnDocReady(pr.first, pr.second);   // 重排后实高变化 → delegate 校正
    }
}

void CChatDocPool::Clear()
{
    // 2026-10-23 删除前通知（fnDocAboutToDelete）：浮层等引用者在 delete 前摘引用
    //（防 v5.0.412 类 UAF：绘制/析构访问已删 doc）。先快照 (key, doc) 列表再逐条
    //「回调→delete」：回调链重入池（插入/逐出/清洗）会使哈希迭代器失效，禁止跨
    //回调持有迭代器/条目引用。
    QList<QPair<int, QTextDocument*>> aAll;
    aAll.reserve(m_aDocs.size());
    for (auto it = m_aDocs.begin(); it != m_aDocs.end(); ++it) {
        aAll.append(qMakePair(it.key(), it.value().pDoc));
    }
    for (const auto& pr : aAll) {
        if (fnDocAboutToDelete)
            fnDocAboutToDelete(pr.first);
        delete pr.second;             // doc 以池为 parent，显式 delete 后析构不再二次释放
    }
    m_aDocs.clear();
    m_iKeyMru = m_iKeyLru = -1;
    m_iEstTotal = 0;
}
