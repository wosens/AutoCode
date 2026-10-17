// CStreamPresenter：任务日志与助手日志共用的对话流呈现器（2026.09.03 收拢双份平行实现，
//   详见头文件注释）。本文件全部逻辑自 CExecutionPanel.cpp 原两套镜像函数合并迁入：
//   Append（AppendData/AppendAdhocBlock）、Rebuild/RebuildChunk（RebuildView/RebuildChunk/
//   RebuildAsstView/RebuildAdhocChunk）、PrependOlder（PrependOlderBlocks/PrependAsstOlder/
//   BuildOlderChunk）、BackfillResult（OnToolResult/OnAdhocToolResult 回填段）、
//   EnsureCoverage（EnsureAsstCoverage）、OnBlockDropped（两处 sigBlockDropped lambda）、
//   CreateWidget/TryMergeWidget/BlockToJson/BlockFromJson（面板原私有工具）。
#include "CStreamPresenter.h"

#include <QTimer>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <climits>   // 2026-10-15 INT_MAX（普通重建窗口终点哨兵）

#include "CMessageView.h"
#include "CChatModel.h"
#include "CChatListView.h"

namespace {
// 渐进重建每帧创建的块数上限 + 帧耗时预算：单块成本约几毫秒~十几毫秒（Markdown
//   排版/工具卡构建），大块/慢机时固定 20 块/帧可拖到上百毫秒（2026.09.04 用户实测
//   切档仍卡）——改为预算制：累计超 k_iFrameBudgetMs 即让出事件循环，单帧卡顿有硬上限，
//   块数上限只作快机封顶；事件循环可穿插处理用户输入，滚动/点击不再被长帧卡住。
constexpr int k_iRebuildChunk = 20;      // 每帧块数上限（快机封顶）
constexpr int k_iFrameBudgetMs = 8;      // 每帧耗时预算（超时让出；60fps≈16ms/帧留余量）
// 反向分页：切档/重建时初始只挂最近一段块，滚动到顶再补建更早一段——活件数从
// "全量 N"降为"≈200+翻页量"，长会话切档重建/窗口缩放的 O(N) 重排开销大幅下降。
constexpr int k_iViewInitial = 200;   // 初始窗口：只建最近 N 块
constexpr int k_iViewPrepend = 100;   // 滚到顶每次补建更早 N 块（一轮总目标）
constexpr int k_iPrependChunk = 8;    // 2026.09.04 补建分帧：每帧只建 8 块（旧一次建 100 块
                                      //   同步卡主线程数百 ms——"滚到顶卡很久"主因；8 块/帧
                                      //   ≈ 十几 ms，100 块 13 帧渐进补齐，视口锚定不跳动）
} // namespace

// ---------------------------------------------------------------------------
// SBlockData ↔ JSON：2026-10-17 随 SBlockData 拆出为 SBlockData.h 内联（本文件原
//   定义迁出，chat.json/assistant.json 序列化口径零变化）。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 视图绑定
// ---------------------------------------------------------------------------
void CStreamPresenter::Bind(CMessageView* a_pView) {
    if (a_pView == nullptr || a_pView == m_pView) {
        return;
    }
    Unbind();   // 换绑/复绑：先弃旧视图件（共享视图场景下防残留；含 ModelView 挂点）
    m_pView = a_pView;
    // 到顶补建 / 超上限丢块同步：view 作 context（视图销毁自动断线；Unbind 显式断开）。
    //   lambda 捕获 this：本对象均在堆上且生命周期覆盖绑定视图（任务流随面板、助手流随 SAsst）。
    m_aConnTop = QObject::connect(a_pView, &CMessageView::sigTopReached,
                                  a_pView, [this]() { PrependOlder(); });
    // 2026-10-15 底部分页：滚到底且存在尾隙（远跳窗口后）时补建更新一段；常态（窗口
    //   对齐数据尾）收到底信号即空转，无副作用。
    m_aConnBottom = QObject::connect(a_pView, &CMessageView::sigBottomReached,
                                     a_pView, [this]() { AppendNewer(); });
    m_aConnDrop = QObject::connect(a_pView, &CMessageView::sigBlockDropped,
                                   a_pView, [this](CMessageBlock* a_pBlock) { OnBlockDropped(a_pBlock); });
}

void CStreamPresenter::Bind(CChatModel* a_pModel, CChatListView* a_pView2) {
    // 2026-10-17 自绘三期：ModelView 引擎绑定（doc/对话流视图自绘改造方案.md §9.2）。
    //   模型/视图均由外层（面板/助手页）持有并随页生灭；本流只挂指针——Unbind 摘挂
    //   （模型空白化防旧流内容残留），Rebuild 时按 m_aBlocks 全量投影。无逐信号接线：
    //   追加经模型 rowsInserted 由视图自跟随（贴底启发式在 CChatListView 内）。
    if (a_pModel == nullptr || a_pView2 == nullptr
        || (a_pModel == m_pModel && a_pView2 == m_pView2)) {
        return;
    }
    Unbind();
    m_pModel = a_pModel;
    m_pView2 = a_pView2;
    a_pView2->SetModel(a_pModel);   // 视图↔模型接线 + modelReset 清池（幂等）
}

void CStreamPresenter::Unbind() {
    if (m_pView != nullptr) {
        QObject::disconnect(m_aConnTop);
        QObject::disconnect(m_aConnBottom);
        QObject::disconnect(m_aConnDrop);
        m_pView->Clear();   // 弃件（数据在 m_aBlocks 常驻，重绑时 Rebuild 重建）
    }
    // 2026-10-17 ModelView 摘挂：模型空白化（视图清场语义与 Legacy Clear 等价；
    //   模型/视图本体归外层所有，下次 Bind 别的流照常复用）。
    if (m_pModel != nullptr) {
        m_pModel->ClearDisplay();
    }
    m_pModel = nullptr;
    m_pView2 = nullptr;
    m_pView = nullptr;
    m_aWidgets.clear();
    m_iFrontier = 0;
    m_iRebuildIdx = 0;
    ++m_uGen;               // 作废仍在排队的旧帧（重建尚未跑完就切走/关 Tab）
    m_bRebuilding = false;
    m_bProjected = false;   // 2026-10-17 ModelView：摘挂后待重投影（重绑 Rebuild 置回）
    m_fnRebuildDone = nullptr;
    // 2026-10-15 远跳/底部分页状态复位（在途帧已被 ++m_uGen 作废）。
    m_bPrepending = false;
    m_bAppendingNewer = false;
    m_iJumpTarget = -1;
    m_iJumpEnd = INT_MAX;
    m_fnJumpDone = nullptr;
}

// ---------------------------------------------------------------------------
// 追加 / 回填
// ---------------------------------------------------------------------------
void CStreamPresenter::Append(const SBlockData& a_stData) {
    m_aBlocks.append(a_stData);
    if (m_fnDirty) {
        m_fnDirty();   // 数据变化 → 外层落盘路由（任务 chat.json / 助手 assistant.json）
    }
    // 2026-10-17 ModelView 引擎：模型增量投影（O(1) 摊销），视图贴底跟随在
    //   CChatListView 内（rowsInserted + 贴底启发式）；无建件/尾隙概念。
    //   未投影窗口（Bind 后未 Rebuild——跨档幽灵 Tab 建页即 Bind 不 Rebuild）：
    //   与 Legacy「未重建前不显示」同语义——只入数据不投影，切 Tab 惰性 Rebuild
    //   全量对齐（期间投影会造成模型内数据下标错位、回填 miss）。
    if (m_pModel != nullptr) {
        if (m_bProjected) {
            m_pModel->AppendBlock(a_stData);
        }
        return;
    }
    if (m_pView == nullptr || m_bRebuilding) {
        return;   // 纯数据态（未绑定）或分帧重建中（帧循环按实时列表顺序补件，防重复上屏）
    }
    // 2026-10-15 尾隙守卫：远跳窗口后窗口尾早于数据尾（[窗口尾, 数据尾) 未建件）——
    //   新块只入数据不建件（滚到底由 AppendNewer 分轮补建），否则并行列表尾部错位
    //   （WidgetAt 会张冠李戴、BackfillResult 刷错卡）。
    if (m_iFrontier + m_aWidgets.size() < m_aBlocks.size() - 1) {
        return;
    }
    // 连续工具调用 / 连续系统行：并入上一张折叠卡，否则新建上屏
    if (!TryMergeWidget(m_aBlocks, m_aWidgets, m_aBlocks.size() - 1, a_stData, m_iFrontier)) {
        if (CMessageBlock* pW = CreateWidget(a_stData)) {
            m_aWidgets.append(pW);
            m_pView->AddBlock(pW);   // 滚动跟随决策在视图内（m_bFollow 机制）
        }
    }
}

int CStreamPresenter::BackfillResult(const QString& a_sResult, const QString& a_sViewId) {
    // 从尾回溯找最后一个未闭合 ToolUse（且视角匹配：并发评审下按 sViewId 过滤防串配；
    //   助手流不传视角=任意匹配——助手块恒空视角，与原启发式一致）。
    for (int i = m_aBlocks.size() - 1; i >= 0; --i) {
        if (m_aBlocks[i].eKind == CMessageBlock::ToolUse && m_aBlocks[i].sResult.isEmpty()
            && m_aBlocks[i].sViewId == a_sViewId) {
            m_aBlocks[i].sResult = a_sResult;
            if (m_pModel != nullptr && m_bProjected) {
                // 2026-10-17 ModelView 引擎：模型条目快照回填（错误标红/单条卡自动
                //   展开在模型内，口径同 Legacy SetResult(a_bAutoExpandOnError=true)）；
                //   未投影窗口（Bind 未 Rebuild）模型无对应条目——miss 无害，数据已写。
                m_pModel->BackfillResult(i, a_sResult);
            } else if (CMessageBlock* pW = WidgetAt(i)) {
                pW->SetResult(a_sResult, /*a_bAutoExpandOnError=*/true);   // 已建件：即时刷新（实时错误单条卡自动展开；恢复/合并路径见 CreateWidget/TryMergeWidget 默认不展开）
            }
            if (m_fnDirty) {
                m_fnDirty();   // 回填结果也落盘
            }
            return i;
        }
    }
    return -1;
}

CMessageBlock* CStreamPresenter::WidgetAt(int a_iIdx) const {
    const int iRel = a_iIdx - m_iFrontier;   // 并行列表相对下标（[0,frontier) 未建件）
    return (iRel >= 0 && iRel < m_aWidgets.size()) ? m_aWidgets.at(iRel) : nullptr;
}

// ---------------------------------------------------------------------------
// 分帧重建
// ---------------------------------------------------------------------------
void CStreamPresenter::Rebuild(std::function<void()> a_fnDone) {
    if (m_pView == nullptr && m_pModel == nullptr) {
        if (a_fnDone) {
            a_fnDone();   // 纯数据态：无件可建，直接回调（保持完成语义一致）
        }
        return;
    }
    // 2026-10-17 ModelView 引擎：全量投影一次成型（纯数据折叠 O(N)，万条 <30ms），
    //   滚动条范围同步确定（估高纯算术）；完成回调异步发出（保持与 Legacy 分帧
    //   收敛等价的时序语义——调用方 RebuildPromptList 等在完成后刷新）。
    if (m_pModel != nullptr) {
        m_bProjected = true;                  // 2026-10-17 切 Tab 惰性重建判定口径
        m_pModel->RebuildFrom(m_aBlocks);
        if (m_pView2 != nullptr) {
            m_pView2->SetAutoScroll(true);   // 重建等同「跳到最新」，强制跟随到底
            m_pView2->ScrollToBottom();
        }
        if (a_fnDone) {
            QPointer<QObject> pGuard = m_pGuard;
            QTimer::singleShot(0, m_pGuard, [pGuard, fnDone = std::move(a_fnDone)]() {
                if (pGuard.isNull()) { return; }
                fnDone();
            });
        }
        return;
    }
    // 重建等同「跳到最新」，强制跟随到底。初始窗口=最近 k_iViewInitial 块（更早的滚动到顶补建），
    //   长会话切档重建成本从 O(N) 降为 O(200+新增)。
    m_pView->ResetFollow();
    m_pView->Clear();
    m_aWidgets.clear();
    m_iRebuildIdx = qMax(0, m_aBlocks.size() - k_iViewInitial);
    m_iFrontier   = m_iRebuildIdx;   // [0, frontier) 未建件
    ++m_uGen;   // 作废仍在排队的旧帧（上次重建尚未跑完就切走/关 Tab）
    m_bRebuilding = true;   // Append 新块交分帧循环按序创建，防重复上屏
    m_fnRebuildDone = std::move(a_fnDone);
    // 2026-10-15 远跳状态复位：本次普通重建跟到数据尾（在途跳建帧已被 ++m_uGen 作废）。
    m_iJumpTarget = -1;
    m_iJumpEnd = INT_MAX;
    m_fnJumpDone = nullptr;
    m_bPrepending = false;
    m_bAppendingNewer = false;
    RebuildChunk(m_uGen);
}

void CStreamPresenter::RebuildChunk(quint32 a_uGen) {
    if (a_uGen != m_uGen || m_pView == nullptr) {
        return;   // 已被新重建/解绑顶替，本帧作废
    }
    int iCreated = 0;
    QElapsedTimer tFrame;
    tFrame.start();   // 2026.09.04 帧预算：单块成本差异大（纯文本 vs 重 Markdown/工具卡），
                      //   固定块数在慢内容上单帧上百 ms；累计超预算立即让出，帧内卡顿有硬上限
    // 2026-10-15 窗口终点：普通重建=INT_MAX（跟到实时数据尾，新到块也补件）；远跳
    //   跳建=目标+初始窗口（之后的块数据-only，滚到底 AppendNewer 分轮补）。
    const int iEnd = qMin(m_aBlocks.size(), m_iJumpEnd);
    while (m_iRebuildIdx < iEnd && iCreated < k_iRebuildChunk) {
        // 连续工具调用 / 连续系统行：历史重载同样折叠
        if (!TryMergeWidget(m_aBlocks, m_aWidgets, m_iRebuildIdx,
                            m_aBlocks.at(m_iRebuildIdx), m_iFrontier)) {
            if (CMessageBlock* pW = CreateWidget(m_aBlocks.at(m_iRebuildIdx),
                                                 /*a_bAsyncMarkdown=*/true)) {
                m_aWidgets.append(pW);
                m_pView->AddBlock(pW, /*a_bBatch=*/true);   // 批量：不逐块滚底
            }
        }
        ++m_iRebuildIdx;
        ++iCreated;
        if (tFrame.elapsed() >= k_iFrameBudgetMs && m_iRebuildIdx < iEnd) {
            break;   // 本帧预算耗尽（尾部追加对齐无损，中断任意位置安全）
        }
    }
    if (m_iRebuildIdx < iEnd) {
        // 帧回调以哨兵为 context（流寿命可短于视图——切档销毁流后未决帧经 QPointer 自弃）
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard, [this, pGuard, a_uGen]() {
            if (pGuard.isNull()) { return; }
            RebuildChunk(a_uGen);
        });
        return;
    }
    m_bRebuilding = false;
    if (m_iJumpTarget >= 0) {
        // 2026-10-15 远跳跳建完成：目标块滚到视口顶（ScrollBlockToTop 内会停跟随），
        //   不走滚底；窗口尾隙交给底部分页。
        const int iTarget = m_iJumpTarget;
        m_iJumpTarget = -1;
        m_iJumpEnd = INT_MAX;
        auto fnDone = std::move(m_fnJumpDone);
        m_fnJumpDone = nullptr;
        if (fnDone) {
            fnDone(WidgetAt(iTarget));
        }
        return;
    }
    m_pView->ScrollToBottomRobust();   // 多帧滚到最底（Markdown 高度异步收敛）
    if (m_fnRebuildDone) {
        std::function<void()> fn = m_fnRebuildDone;
        m_fnRebuildDone = nullptr;
        fn();
    }
}

// ---------------------------------------------------------------------------
// 反向分页：到顶补建更早段
// ---------------------------------------------------------------------------
bool CStreamPresenter::PrependOlder() {
    if (m_pModel != nullptr) {
        return false;   // 2026-10-17 ModelView：模型持全量条目，无翻页概念（窗口机制退役）
    }
    // 2026.09.04 分帧补建：滚到顶一轮目标 k_iViewPrepend(100) 块，按 k_iPrependChunk(8)
    //   块/帧串行补齐（singleShot(0) 续帧交还事件循环）。旧实现一次同步建 100 块卡主线程
    //   数百 ms——"滚到顶卡很久"主因。补建中重复到顶信号忽略；Rebuild/Unbind 的 ++m_uGen
    //   作废在途帧（帧回调复位 m_bPrepending，不产生死锁）。
    if (m_pView == nullptr || m_bRebuilding || m_iFrontier <= 0 || m_bPrepending) {
        return false;
    }
    m_bPrepending = true;
    m_iPrependTarget = qMax(0, m_iFrontier - k_iViewPrepend);
    PrependFrame(m_uGen);
    return true;
}

void CStreamPresenter::PrependFrame(quint32 a_uGen) {
    if (a_uGen != m_uGen) {
        m_bPrepending = false;   // 已被新重建/解绑顶替：终止本轮（下次到顶重新开始）
        return;
    }
    if (m_pView == nullptr || m_bRebuilding || m_iFrontier <= m_iPrependTarget) {
        m_bPrepending = false;   // 本轮补齐 / 状态变化：收尾
        return;
    }
    QList<CMessageBlock*> aNew;
    BuildOlderChunk(aNew);
    m_pView->PrependBlocks(aNew);   // 锚定原首块前插，视口内容不动（逐帧累积向上生长）
    if (m_iFrontier > m_iPrependTarget) {
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard, [this, pGuard, a_uGen]() {
            if (pGuard.isNull()) { return; }
            PrependFrame(a_uGen);
        });
        return;
    }
    m_bPrepending = false;   // 一轮 100 块补齐
}

bool CStreamPresenter::AppendNewer() {
    if (m_pModel != nullptr) {
        return false;   // 2026-10-17 ModelView：全量常驻，无尾隙可补（窗口机制退役）
    }
    // 2026-10-15 底部分页：窗口尾截断（远跳窗口后 [窗口尾, 数据尾) 未建件）且滚到
    //   底时分轮补建更新一段（与 PrependOlder 对称）：一轮 k_iViewPrepend 块，按
    //   k_iPrependChunk 块/帧串行补齐。常态（窗口对齐数据尾）无隙可补直接 false。
    if (m_pView == nullptr || m_bRebuilding || m_bAppendingNewer) {
        return false;
    }
    const int iWinEnd = m_iFrontier + m_aWidgets.size();
    if (iWinEnd >= m_aBlocks.size()) {
        return false;   // 无尾隙
    }
    m_bAppendingNewer = true;
    m_iNewerTarget = qMin(m_aBlocks.size(), iWinEnd + k_iViewPrepend);
    NewerFrame(m_uGen);
    return true;
}

void CStreamPresenter::NewerFrame(quint32 a_uGen) {
    if (a_uGen != m_uGen) {
        m_bAppendingNewer = false;   // 已被新重建/跳建/解绑顶替：终止本轮（下次到底重新开始）
        return;
    }
    const int iWinEnd = m_iFrontier + m_aWidgets.size();
    if (m_pView == nullptr || m_bRebuilding || iWinEnd >= m_iNewerTarget
        || iWinEnd >= m_aBlocks.size()) {
        m_bAppendingNewer = false;   // 本轮补齐 / 状态变化：收尾
        return;
    }
    BuildNewerChunk();
    const int iEnd2 = m_iFrontier + m_aWidgets.size();
    if (iEnd2 < m_iNewerTarget && iEnd2 < m_aBlocks.size()) {
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard, [this, pGuard, a_uGen]() {
            if (pGuard.isNull()) { return; }
            NewerFrame(a_uGen);
        });
        return;
    }
    m_bAppendingNewer = false;   // 一轮补齐（或已到数据尾）
}

void CStreamPresenter::BuildNewerChunk() {
    // 2026-10-15 本帧 8 块 [窗口尾, 窗口尾+8)：连续区间全建完才入列（与 BuildOlderChunk
    //   同口径，并行列表与数据下标不断裂）；尾部追加经 AddBlock(batch) 不逐块滚底，
    //   超上限从顶丢块由 OnBlockDropped 同步推进前沿，对齐保持。块内合并（工具×折叠卡/
    //   系统组）并入尾卡时 TryMergeWidget 内部自推并行列表槽位，与 Append 同口径。
    const int iFrom = m_iFrontier + m_aWidgets.size();
    const int iTo = qMin(m_aBlocks.size(),
                         qMin(m_iNewerTarget, iFrom + k_iPrependChunk));
    for (int i = iFrom; i < iTo; ++i) {
        if (!TryMergeWidget(m_aBlocks, m_aWidgets, i, m_aBlocks.at(i), m_iFrontier)) {
            if (CMessageBlock* pW = CreateWidget(m_aBlocks.at(i),
                                                 /*a_bAsyncMarkdown=*/true)) {
                m_aWidgets.append(pW);
                m_pView->AddBlock(pW, /*a_bBatch=*/true);
            }
        }
    }
}

void CStreamPresenter::BuildOlderChunk(QList<CMessageBlock*>& a_aNewWidgets) {
    // 2026.09.04 补建分帧：每帧只建 [frontier-k_iPrependChunk, frontier)（连续区间全建
    //   完才前插+推进前沿——并行列表与数据下标的连续对齐不允许中途断裂，与尾部追加
    //   不同，不能用预算中断）。一轮总目标 k_iViewPrepend 块由 PrependFrame 串行多帧
    //   补齐；帧边界两张独立卡（不回并，罕见且无害）。
    const int iLo = qMax(0, m_iFrontier - k_iPrependChunk);
    QList<CMessageBlock*> aChunkWidgets;   // 与本帧数据块一一对应（并入=重复指针）
    for (int i = iLo; i < m_iFrontier; ++i) {
        if (!TryMergeWidget(m_aBlocks, aChunkWidgets, i, m_aBlocks.at(i), iLo)) {
            if (CMessageBlock* pW = CreateWidget(m_aBlocks.at(i),
                                                 /*a_bAsyncMarkdown=*/true)) {
                aChunkWidgets.append(pW);
                a_aNewWidgets.append(pW);
            }
        }
    }
    // 前插对齐并行列表（倒序逐个 prepend 保持帧内顺序）；推进前沿。
    //   跨帧/跨轮边界不回并——两张独立卡。
    for (int k = aChunkWidgets.size() - 1; k >= 0; --k) {
        m_aWidgets.prepend(aChunkWidgets.at(k));
    }
    m_iFrontier = iLo;
}

void CStreamPresenter::OnBlockDropped(CMessageBlock* a_pBlock) {
    // 丢的必是已建件最旧者（布局首块，对应数据下标=前沿）；合并卡头部连续同指针一并剥。
    while (!m_aWidgets.isEmpty() && m_aWidgets.first() == a_pBlock) {
        m_aWidgets.removeFirst();
        ++m_iFrontier;
    }
}

// ---------------------------------------------------------------------------
// 跳转覆盖（提示词下拉跳到更早提问用）
// ---------------------------------------------------------------------------
void CStreamPresenter::EnsureCoverage(int a_iTargetIdx, std::function<void(CMessageBlock*)> a_fnDone) {
    // 2026-10-17 ModelView 引擎：全量常驻 → 数据下标直映射条目、O(1) 跳转（方案
    //   §9.3「EnsureCoverage 壳转发」口径）——目标滚到视口顶 + 高亮由 JumpToItem
    //   完成；回调参数=nullptr（无块概念，调用方判空跳过 ScrollBlockToTop）。
    if (m_pModel != nullptr) {
        if (a_iTargetIdx < 0 || a_iTargetIdx >= m_aBlocks.size()) {
            return;   // 目标越界：不回调（与 Legacy 越界口径一致）
        }
        if (m_pView2 != nullptr) {
            m_pView2->JumpToItem(m_pModel->ItemAtDataIdx(a_iTargetIdx));
        }
        if (a_fnDone) {
            a_fnDone(nullptr);
        }
        return;
    }
    if (m_pView == nullptr || a_iTargetIdx < 0 || a_iTargetIdx >= m_aBlocks.size()) {
        return;   // 未绑定/目标越界：不回调（调用方按需自行兜底）
    }
    if (m_bRebuilding) {
        // 分帧重建在途：下一帧重试（重建必终止，不会死循环；gen 顶替由 RebuildChunk 自防）
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(30, m_pGuard,
                           [this, pGuard, a_iTargetIdx, a_fnDone]() {
                               if (pGuard.isNull()) { return; }
                               EnsureCoverage(a_iTargetIdx, a_fnDone);
                           });
        return;
    }
    // 2026-10-15 尾隙（目标在窗口尾之后、数据尾之前）：分轮向尾补建至覆盖后回调
    //   （滚到底同款分帧，与前沿方向对称）。
    if (a_iTargetIdx >= m_iFrontier + m_aWidgets.size()) {
        m_iNewerTarget = qMin(m_aBlocks.size(),
                              qMax(a_iTargetIdx + 1,
                                   m_iFrontier + m_aWidgets.size() + k_iViewPrepend));
        if (!m_bAppendingNewer) {
            m_bAppendingNewer = true;
            NewerFrame(m_uGen);
        }
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard,
                           [this, pGuard, a_iTargetIdx, a_fnDone]() {
                               if (pGuard.isNull()) { return; }
                               EnsureCoverage(a_iTargetIdx, a_fnDone);
                           });
        return;
    }
    if (m_iFrontier > a_iTargetIdx && m_iFrontier > 0) {
        // 2026-10-15 远跳分流：距离超一轮补建量 → 整窗跳建 O(初始窗口)（逐轮前插要建完
        //   前沿到目标的全部件 + 每帧全树 adjustSize，长会话卡顿数秒~数十秒）；近距仍
        //   走原前插路径（锚定不跳变，体验连续）。
        if (m_iFrontier - a_iTargetIdx > k_iViewPrepend) {
            JumpRebuild(a_iTargetIdx, std::move(a_fnDone));
            return;
        }
        PrependOlder();   // 一轮 ≈100 块分帧突发
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard,
                           [this, pGuard, a_iTargetIdx, a_fnDone]() {
                               if (pGuard.isNull()) { return; }
                               EnsureCoverage(a_iTargetIdx, a_fnDone);
                           });
        return;
    }
    if (a_fnDone) {
        a_fnDone(WidgetAt(a_iTargetIdx));
    }
}

void CStreamPresenter::JumpRebuild(int a_iTargetIdx,
                                    std::function<void(CMessageBlock*)> a_fnDone) {
    // 2026-10-15 远跳整窗跳建：弃全部件，直接建 [目标, 目标+k_iViewInitial)，完成后
    //   目标块滚到视口顶（回调链同前插路径：fnDone(WidgetAt(target)) → ScrollBlockToTop）
    //   ——成本只与窗口大小（200）成正比，与会话总长/跳转距离无关。窗口尾之后的块
    //   不建件（数据常驻）：向下滚到底由 AppendNewer 分轮补建；期间新到块走 Append
    //   尾隙守卫（数据-only）。在途补建/重建/重试帧由 ++m_uGen 统一作废。
    ++m_uGen;
    m_bPrepending = false;          // 在途到顶补建随代际顶替终止
    m_bAppendingNewer = false;      // 在途到底补建同理
    m_pView->Clear();               // 弃全部件（数据不动）
    m_aWidgets.clear();
    m_iFrontier = a_iTargetIdx;     // 窗口起点 = 目标（目标即视口顶）
    m_iRebuildIdx = a_iTargetIdx;
    m_iJumpTarget = a_iTargetIdx;
    m_iJumpEnd = qMin(m_aBlocks.size(), a_iTargetIdx + k_iViewInitial);
    m_bRebuilding = true;           // 复用分帧重建管线（帧预算/代际号/批量建件）
    m_fnRebuildDone = nullptr;      // 被顶替的普通重建回调不再发（跳建自带完成链）
    m_fnJumpDone = std::move(a_fnDone);
    RebuildChunk(m_uGen);
}

// ---------------------------------------------------------------------------
// 内部工具（自面板原实现迁入，逻辑不变）
// ---------------------------------------------------------------------------
CMessageBlock* CStreamPresenter::CreateWidget(const SBlockData& a_stData,
                                              bool a_bAsyncMarkdown) const {
    auto* pW = new CMessageBlock(a_stData.eKind, m_pView);   // parent=视图（AddBlock 会 reparent 进内容区）
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            // 2026.10.16 批量路径改视口惰性：建件只记文本+估高（零排版成本），滚入
            //   视口带才 Materialize（占位+子线程预排版）——原 SetAssistantTextAsync
            //   对窗口内全部块同步 setPlainText 占位，200 块全量纯文本排版仍是
            //   主线程大头；流式单块仍同步即到即显（a_bAsyncMarkdown=false 路径）。
            if (a_bAsyncMarkdown) {
                const int iVpW = (m_pView != nullptr && m_pView->viewport() != nullptr)
                                     ? m_pView->viewport()->width() : 0;
                pW->SetAssistantTextDeferred(a_stData.sText,
                                             iVpW > 40 ? qreal(iVpW - 24) : 776.0);
            } else {
                pW->SetAssistantText(a_stData.sText);
            }
            pW->SetSpeakerBackend(a_stData.sBackend);   // 块内产出后端（空→默认 Agent）
            break;
        case CMessageBlock::ToolUse:
            pW->SetTool(a_stData.sTool, a_stData.sInput);
            if (!a_stData.sResult.isEmpty()) {
                pW->SetResult(a_stData.sResult);   // 重建时已有结果（回填过的）
            }
            break;
        case CMessageBlock::Event:
            pW->SetEvent(a_stData.sEventKind, a_stData.sText);
            break;
        case CMessageBlock::System:
        case CMessageBlock::Error:
            pW->SetSystemText(a_stData.sText);
            break;
    }
    return pW;
}

// 连续工具调用 / 连续系统行合并上屏：command/读写等工具常成串出现（数十条），逐条渲染
//   既刷屏又拖慢滚动；合并为一张折叠卡（标题首条摘要 + ×N，展开看全部）。系统行同理。
// a_iBase：aWidgets[k] 对应数据下标 a_iBase+k（默认 0 = 全量对齐旧语义；反向分页后
//   尾部追加传前沿、补建块传本轮块首——尾部分并与回填均相对寻址）。
bool CStreamPresenter::TryMergeWidget(const QList<SBlockData>& aBlocks,
                                      QList<CMessageBlock*>& aWidgets,
                                      int a_iNewIdx, const SBlockData& a_stData, int a_iBase) {
    const int iRel = a_iNewIdx - a_iBase;   // 新块在 widgets 列表中的相对下标
    if ((a_stData.eKind != CMessageBlock::ToolUse &&
         a_stData.eKind != CMessageBlock::System) || iRel <= 0) {
        return false;   // 仅工具卡与系统组参与连续合并
    }
    // 2026.10.15 文本不打断工具合并（用户实害：DSH 后端每次工具调用前都
    //   输出一段思维链文本，旧口径“前一块必须是工具块”被它们逐条打断
    //   → 整版工具逐卡铺开刷屏（Claude 等连发工具则能合并成 ×N 组卡）。
    //   ToolUse 新块改为向前跳过 Assistant 文本块找最近同类组卡（用户提示词是
    //   Event 块、事件/错误/系统行仍打断——跨提问轮不会误并；组卡位置不变，
    //   穿插的文本仍在原位，工具全部收进上方组卡展开可见）。System 组保持
    //   紧邻合并（系统行无穿插彨态，跳过文本会把不相干行归错组）。
    int iPrev = a_iNewIdx - 1;
    if (a_stData.eKind == CMessageBlock::ToolUse) {
        while (iPrev >= 0 && aBlocks.at(iPrev).eKind == CMessageBlock::Assistant) {
            --iPrev;   // 跳过中间的 Agent 文本（含思维链）
        }
    }
    if (iPrev < 0) {
        return false;   // 前面全是文本（无组卡可并）
    }
    const SBlockData& stPrev = aBlocks.at(iPrev);
    if (stPrev.eKind != a_stData.eKind || stPrev.sViewId != a_stData.sViewId) {
        return false;   // 找到的组卡不同类（或评审视角不同）：不合并
    }
    const int iPrevRel = iPrev - a_iBase;
    if (iPrevRel < 0 || iPrevRel >= aWidgets.size() || aWidgets.isEmpty()) {
        return false;   // widget 列表未跟上（异常防御；含合并占位对齐校验）
    }
    CMessageBlock* pW = aWidgets.at(iPrevRel);
    if (a_stData.eKind == CMessageBlock::ToolUse) {
        if (pW->ToolCount() <= 0) {
            return false;
        }
        pW->AddMergedTool(a_stData.sTool, a_stData.sInput);
        if (!a_stData.sResult.isEmpty()) {
            pW->SetResult(a_stData.sResult);
        }
    } else {
        pW->AppendSystemLine(a_stData.sText);   // 连续系统行并入折叠组
    }
    aWidgets.append(pW);   // 同一 widget 占多个块位：结果回填/提示词跳转的索引对齐仍成立
    return true;
}
