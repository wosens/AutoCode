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

#include "CMessageView.h"

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
// SBlockData ↔ JSON
// ---------------------------------------------------------------------------
QJsonObject BlockToJson(const SBlockData& a_stData) {
    QJsonObject o;
    if (!a_stData.sViewId.isEmpty()) {
        o[QStringLiteral("view")] = a_stData.sViewId;   // 评审多视角标识（向后兼容：旧记录无此键）
    }
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            o[QStringLiteral("kind")] = QStringLiteral("assistant");
            o[QStringLiteral("text")] = a_stData.sText;
            // 产出后端随块持久化（旧记录无此键，读取容错为空）
            if (!a_stData.sBackend.isEmpty()) {
                o[QStringLiteral("backend")] = a_stData.sBackend;
            }
            break;
        case CMessageBlock::ToolUse:
            o[QStringLiteral("kind")]   = QStringLiteral("tool_use");
            o[QStringLiteral("tool")]   = a_stData.sTool;
            o[QStringLiteral("input")]  = a_stData.sInput;
            o[QStringLiteral("result")] = a_stData.sResult;
            break;
        case CMessageBlock::Event:
            o[QStringLiteral("kind")]      = QStringLiteral("event");
            o[QStringLiteral("eventKind")] = a_stData.sEventKind;
            o[QStringLiteral("text")]      = a_stData.sText;
            break;
        case CMessageBlock::Error:
            o[QStringLiteral("kind")] = QStringLiteral("error");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
        case CMessageBlock::System:
        default:
            o[QStringLiteral("kind")] = QStringLiteral("system");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
    }
    return o;
}

SBlockData BlockFromJson(const QJsonObject& a_o) {
    SBlockData st;
    st.sViewId = a_o.value(QStringLiteral("view")).toString();
    const QString sKind = a_o.value(QStringLiteral("kind")).toString();
    if (sKind == QStringLiteral("assistant")) {
        st.eKind = CMessageBlock::Assistant;
        st.sText = a_o.value(QStringLiteral("text")).toString();
        st.sBackend = a_o.value(QStringLiteral("backend")).toString();   // 产出后端（旧记录无→空）
    } else if (sKind == QStringLiteral("tool_use")) {
        st.eKind   = CMessageBlock::ToolUse;
        st.sTool   = a_o.value(QStringLiteral("tool")).toString();
        st.sInput  = a_o.value(QStringLiteral("input")).toString();
        st.sResult = a_o.value(QStringLiteral("result")).toString();
    } else if (sKind == QStringLiteral("event")) {
        st.eKind      = CMessageBlock::Event;
        st.sEventKind = a_o.value(QStringLiteral("eventKind")).toString();
        st.sText      = a_o.value(QStringLiteral("text")).toString();
    } else if (sKind == QStringLiteral("error")) {
        st.eKind = CMessageBlock::Error;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    } else {
        st.eKind = CMessageBlock::System;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    }
    return st;
}

// ---------------------------------------------------------------------------
// 视图绑定
// ---------------------------------------------------------------------------
void CStreamPresenter::Bind(CMessageView* a_pView) {
    if (a_pView == nullptr || a_pView == m_pView) {
        return;
    }
    Unbind();   // 换绑/复绑：先弃旧视图件（共享视图场景下防残留）
    m_pView = a_pView;
    // 到顶补建 / 超上限丢块同步：view 作 context（视图销毁自动断线；Unbind 显式断开）。
    //   lambda 捕获 this：本对象均在堆上且生命周期覆盖绑定视图（任务流随面板、助手流随 SAsst）。
    m_aConnTop = QObject::connect(a_pView, &CMessageView::sigTopReached,
                                  a_pView, [this]() { PrependOlder(); });
    m_aConnDrop = QObject::connect(a_pView, &CMessageView::sigBlockDropped,
                                   a_pView, [this](CMessageBlock* a_pBlock) { OnBlockDropped(a_pBlock); });
}

void CStreamPresenter::Unbind() {
    if (m_pView != nullptr) {
        QObject::disconnect(m_aConnTop);
        QObject::disconnect(m_aConnDrop);
        m_pView->Clear();   // 弃件（数据在 m_aBlocks 常驻，重绑时 Rebuild 重建）
    }
    m_pView = nullptr;
    m_aWidgets.clear();
    m_iFrontier = 0;
    m_iRebuildIdx = 0;
    ++m_uGen;               // 作废仍在排队的旧帧（重建尚未跑完就切走/关 Tab）
    m_bRebuilding = false;
    m_fnRebuildDone = nullptr;
}

// ---------------------------------------------------------------------------
// 追加 / 回填
// ---------------------------------------------------------------------------
void CStreamPresenter::Append(const SBlockData& a_stData) {
    m_aBlocks.append(a_stData);
    if (m_fnDirty) {
        m_fnDirty();   // 数据变化 → 外层落盘路由（任务 chat.json / 助手 assistant.json）
    }
    if (m_pView == nullptr || m_bRebuilding) {
        return;   // 纯数据态（未绑定）或分帧重建中（帧循环按实时列表顺序补件，防重复上屏）
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
            if (CMessageBlock* pW = WidgetAt(i)) {
                pW->SetResult(a_sResult);   // 已建件：即时刷新（未建件等补建时由数据呈现）
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
    if (m_pView == nullptr) {
        if (a_fnDone) {
            a_fnDone();   // 纯数据态：无件可建，直接回调（保持完成语义一致）
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
    while (m_iRebuildIdx < m_aBlocks.size() && iCreated < k_iRebuildChunk) {
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
        if (tFrame.elapsed() >= k_iFrameBudgetMs && m_iRebuildIdx < m_aBlocks.size()) {
            break;   // 本帧预算耗尽（尾部追加对齐无损，中断任意位置安全）
        }
    }
    if (m_iRebuildIdx < m_aBlocks.size()) {
        // 帧回调以哨兵为 context（流寿命可短于视图——切档销毁流后未决帧经 QPointer 自弃）
        QPointer<QObject> pGuard = m_pGuard;
        QTimer::singleShot(0, m_pGuard, [this, pGuard, a_uGen]() {
            if (pGuard.isNull()) { return; }
            RebuildChunk(a_uGen);
        });
        return;
    }
    m_bRebuilding = false;
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
    if (m_iFrontier > a_iTargetIdx && m_iFrontier > 0) {
        PrependOlder();   // 一轮 ≈100 块同步突发
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

// ---------------------------------------------------------------------------
// 内部工具（自面板原实现迁入，逻辑不变）
// ---------------------------------------------------------------------------
CMessageBlock* CStreamPresenter::CreateWidget(const SBlockData& a_stData,
                                              bool a_bAsyncMarkdown) const {
    auto* pW = new CMessageBlock(a_stData.eKind, m_pView);   // parent=视图（AddBlock 会 reparent 进内容区）
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            // 2026.09.04 批量路径异步预排版：Markdown parse+初排版在子线程（主线程不卡），
            //   落地前纯文本占位。预估宽度 = 视口宽 - 块左右边距（CMessageBlock 内容区
            //   margin 由其样式定，取 2×12 常量近似；落地后按真实宽度微调，误差可收敛）。
            if (a_bAsyncMarkdown) {
                const int iVpW = (m_pView != nullptr && m_pView->viewport() != nullptr)
                                     ? m_pView->viewport()->width() : 0;
                pW->SetAssistantTextAsync(a_stData.sText,
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
    const SBlockData& stPrev = aBlocks.at(a_iNewIdx - 1);
    if (stPrev.eKind != a_stData.eKind || stPrev.sViewId != a_stData.sViewId) {
        return false;   // 前一块不同类（或评审视角不同）：不合并
    }
    if (iRel > aWidgets.size() || aWidgets.isEmpty()) {
        return false;   // widget 列表未跟上（异常防御）
    }
    CMessageBlock* pW = aWidgets.at(iRel - 1);
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
