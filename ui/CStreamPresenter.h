#pragma once

#include <QString>
#include <QList>
#include <QPointer>
#include <functional>
#include <memory>

#include "SBlockData.h"      // 2026-10-17 SBlockData/BlockToJson 自本头拆出（解 CChatModel 环）
#include "CMessageView.h"    // 2026-10-09 QPointer<CMessageView> 需完整类型（前向声明下 qpointer.h static_cast 编译错）
#include "CChatModel.h"      // 2026-10-17 QPointer<CChatModel> 完整类型（同上先例）
#include "CChatListView.h"   // 2026-10-17 QPointer<CChatListView> 完整类型（同上先例）

// =============================================================================
// CStreamPresenter：一条对话流的呈现器（2026.09.03 收拢任务日志与助手日志的双份平行实现）。
//
// 之前两套镜像代码（面板成员 m_aBlocks/m_aCurWidgets/… + SAsst::aBlocks/aWidgets/…）功能
// 完全一致，每次修改（合并卡片/分帧重建/反向分页/结果回填）都要双改，遗漏即行为分叉。
// 现统一收拢：数据列表 + 并行块 widget + 反向分页前沿 + 分帧重建状态 + 视图绑定。
//
// 两种用法：
//   任务日志：全部任务各一个 presenter（堆上，QHash<QString, CStreamPresenter*>），共享同一
//             个 m_pView——切任务时旧流 Unbind（弃件保数据）→ 新流 Bind+Rebuild；
//   助手日志：每个助手一个 presenter（SAsst::pStream，shared_ptr），独占自己的 Tab 视图，
//             建 Tab 时 Bind、关 Tab 时 Unbind，数据常驻。
//
// 非 QObject：到顶补建/丢块同步经 view 作 context 的 lambda 连接（视图销毁自动断开），
// 数据变化经 fnDirty 回调路由（任务→MarkTaskDirty；助手→sigAdhocBlocksChanged）。
//
// 反向分页（2026.09.03）：初始只挂最近 k_iViewInitial 块（[0,frontier) 未建件），滚动到顶
// 自动补建更早 k_iViewPrepend 块/次（视口锚定不跳动）；长会话活件数≈窗口+翻页量。
// =============================================================================
class CStreamPresenter {
public:
    // fnDirty：数据变化（追加/结果回填）回调——外层落盘路由，创建时注入
    explicit CStreamPresenter(std::function<void()> a_fnDirty = nullptr)
        : m_fnDirty(std::move(a_fnDirty)) {}
    ~CStreamPresenter() {
        Unbind();
        delete m_pGuard;   // 哨兵先于成员析构结束——排队中的帧/重试经 QPointer 感知并自弃
    }

    CStreamPresenter(const CStreamPresenter&) = delete;             // 持有视图连接与件列表，
    CStreamPresenter& operator=(const CStreamPresenter&) = delete;  // 禁拷贝（流唯一，按址/共享指针传递）

    // —— 视图绑定 ——
    void Bind(CMessageView* a_pView);   // 绑定视图（接线分页信号；重复绑定先解绑旧视图）
    // 2026-10-17 自绘三期（doc/对话流视图自绘改造方案.md §9.2）：ModelView 引擎
    //   绑定——挂 (CChatModel, CChatListView) 自绘链。数据正本/落盘/fnDirty 与 Legacy
    //   完全共用（两引擎同档互切零迁移）；窗口机制退役（模型持全量投影，视图天然
    //   虚拟化只画可视区）。绑定后调用方应跟 Rebuild 投影数据。
    void Bind(CChatModel* a_pModel, CChatListView* a_pView2);
    void Unbind();                      // 解绑：断线 + 清视图块 + 弃件保数据 + 复位前沿/作废在途帧
    CMessageView* View() const { return m_pView; }
    bool Bound() const { return m_pView != nullptr || m_pModel != nullptr; }
    // —— ModelView 引擎观察口（测试/面板判定用；Legacy 恒 false）——
    bool ModelView() const { return m_pModel != nullptr; }
    CChatModel* ChatModel() const { return m_pModel; }
    CChatListView* ChatView() const { return m_pView2; }

    // —— 数据 ——
    QList<SBlockData>& Blocks() { return m_aBlocks; }
    const QList<SBlockData>& Blocks() const { return m_aBlocks; }
    void SetBlocks(const QList<SBlockData>& a_aBlocks) { m_aBlocks = a_aBlocks; }   // 载入/换数据（绑定时调用方应跟 Rebuild）
    int Count() const { return m_aBlocks.size(); }
    int WidgetCount() const { return m_aWidgets.size(); }   // 已建件数（0=未建/已弃件，切档惰性重建判定用；ModelView 恒 0——用 Projected()）
    // 2026-10-17 ModelView 引擎：数据是否已投影进模型（Rebuild 置位、Unbind 摘除）——
    //   面板切 Tab 惰性重建判定的 MV 口径（Legacy 的 WidgetCount()==0 等价物）。
    bool Projected() const { return m_bProjected; }
    int Frontier() const { return m_iFrontier; }   // [0,frontier) 尚未建件（测试/诊断）

    // —— 上屏 ——
    // 追加一条：入数据 + fnDirty；（绑定且非重建中）连续同类并入折叠卡或新建块上屏。
    void Append(const SBlockData& a_stData);
    // 重建视图：初始窗口=最近 k_iViewInitial 块，分帧渐进（防数百块同步创建卡 UI）；
    // a_fnDone=完成回调（助手流借此刷新提示词下拉索引；任务流传空）。
    void Rebuild(std::function<void()> a_fnDone = nullptr);
    bool Rebuilding() const { return m_bRebuilding; }
    // 反向分页：补建更早一段前插视图（到顶信号触发）；返回是否补了（前沿已到 0 则 false）
    bool PrependOlder();
    // 2026-10-15 底部分页：跳转窗口尾截断（窗口尾早于数据尾）时滚到底分轮补建更新
    //   一段（与 PrependOlder 对称）；返回是否补了（无尾隙/在途 false）。
    bool AppendNewer();
    // 数据下标 → 块 widget（未建件区间返回 nullptr）
    CMessageBlock* WidgetAt(int a_iIdx) const;
    // 结果回填：从尾回溯找最后一个未闭合 ToolUse（可选视角过滤）写数据 + 刷件 + fnDirty；
    // 返回数据下标，-1=没找到（调用方自行兜底为系统行）。
    int BackfillResult(const QString& a_sResult, const QString& a_sViewId = QString());
    // 跳转覆盖：目标块早于前沿时分轮补建（每轮一帧突发，交还事件循环防长冻）至覆盖后回调；
    // 已覆盖则同步回调。目标越界/未绑定不回调。
    // 2026-10-15 远跳优化：目标距前沿超过一轮补建量（k_iViewPrepend）时不再逐轮前插
    //   （长会话 O(N) 建件 + 每帧全树 adjustSize ≈ O(N²)，点旧提示词界面卡顿数秒~
    //   数十秒），改为整窗跳建：弃全部件、直接建 [目标, 目标+k_iViewInitial)，完成
    //   后目标块滚到视口顶——响应恒为 O(窗口) 与距离无关。窗口尾之后的块数据-only，
    //   向下滚到底由 AppendNewer 分轮补建。
    void EnsureCoverage(int a_iTargetIdx, std::function<void(CMessageBlock*)> a_fnDone);

private:
    void RebuildChunk(quint32 a_uGen);                      // 分帧重建一帧（代际号不符=被顶替，立即终止）
    void JumpRebuild(int a_iTargetIdx, std::function<void(CMessageBlock*)> a_fnDone);   // 2026-10-15 远跳整窗跳建
    void BuildOlderChunk(QList<CMessageBlock*>& a_aNewWidgets);   // 补本帧 8 块 [frontier-8, frontier)（块内合并仍生效）
    void PrependFrame(quint32 a_uGen);                      // 2026.09.04 到顶补建分帧驱动（8 块/帧至本轮目标）
    void NewerFrame(quint32 a_uGen);                        // 2026-10-15 到底补建分帧驱动（8 块/帧至本轮目标）
    void BuildNewerChunk();                                 // 2026-10-15 补本帧 8 块 [窗口尾, 窗口尾+8)（尾部追加，合并仍生效）
    void OnBlockDropped(CMessageBlock* a_pBlock);           // 视图超上限丢最旧块 → 同步并行列表+推进前沿
    // 数据快照 → 块 widget（parent=绑定视图）。a_bAsyncMarkdown=true（批量路径：重建/
    //   补建）时 Assistant 块走子线程预排版（主线程只落地）；流式追加传 false（单块
    //   同步渲染即到即显，无闪烁收益优先）。
    CMessageBlock* CreateWidget(const SBlockData& a_stData, bool a_bAsyncMarkdown = false) const;
    // 连续同类块并入上一折叠卡（工具×折叠卡/系统折叠组）。a_iBase：aWidgets[k] 对应数据下标
    // base+k（尾部追加传前沿、补建块传本轮块首），相对寻址兼容反向分页。
    static bool TryMergeWidget(const QList<SBlockData>& aBlocks, QList<CMessageBlock*>& aWidgets,
                               int a_iNewIdx, const SBlockData& a_stData, int a_iBase);

    QObject*              m_pGuard = new QObject;  // 生命周期哨兵：singleShot 帧回调的 context
                                                  //   （流本体非 QObject 且寿命可短于视图——队列中
                                                  //   未决帧经 QPointer 判活，流亡即弃，防悬垂解引）
    // 2026-10-09 悬垂视图修复：QPointer——拆页路径（跨档幽灵 TakeRunningForReload/
    //   RemoveAsstEntry/DropAsstEntries 的 pPage->deleteLater）销毁视图后流对象仍存活，
    //   裸指针复绑（Bind→Unbind→m_pView->Clear）踩已释放内存（foreignPhantom 实崩栈：
    //   AttachForeignAssistants→CreateAsstTab→CreateAsstWidgets→Bind→Unbind→Clear）；
    //   QPointer 销毁自动置空，判空路径自然回退纯数据态，无需逐拆页点补 Unbind。
    QPointer<CMessageView> m_pView;                    // 绑定视图（空=纯数据态：任务未选中/助手 Tab 已关）
    // —— 2026-10-17 自绘三期 ModelView 引擎挂点（与 m_pView 互斥；数据/落盘共用）——
    //   QPointer（同 m_pView 先例）：拆页路径（跨档幽灵 TakeRunningForReload 的
    //   pPage->deleteLater）销毁视图/模型后流对象仍存活，裸指针复绑踩已释放内存；
    //   QPointer 销毁自动置空，判空路径自然回退纯数据态。
    QPointer<CChatModel>    m_pModel;       // 绑定的显示条目模型（不持有：面板/助手页持有）
    QPointer<CChatListView> m_pView2;       // 绑定的自绘视图（不持有：同上）
    std::function<void()> m_fnDirty;               // 数据变化回调（落盘路由）
    QList<SBlockData>     m_aBlocks;               // 对话流数据（唯一正本）
    QList<CMessageBlock*> m_aWidgets;              // 并行块 widget（aWidgets[k] ↔ 数据下标 frontier+k）
    int                   m_iFrontier = 0;         // 反向分页前沿：[0,frontier) 未建件
    int                   m_iRebuildIdx = 0;       // 分帧重建进度
    bool                  m_bProjected = false;    // 2026-10-17 ModelView：数据已投影进模型（Rebuild 置位/Unbind 摘除）
    quint32               m_uGen = 0;              // 分帧重建代际号（Rebuild/Unbind 时 +1 作废旧帧）
    bool                  m_bRebuilding = false;   // 分帧重建中（Append 只入数据，帧循环按序补件）
    bool                  m_bPrepending = false;   // 2026.09.04 到顶补建分帧进行中（挡重复到顶信号）
    int                   m_iPrependTarget = 0;    // 本轮补建目标前沿（frontier 推进到此即收尾）
    // —— 2026-10-15 远跳窗口 / 底部分页 ——
    bool                  m_bAppendingNewer = false;   // 到底补建分帧进行中（挡重复到底信号）
    int                   m_iNewerTarget = 0;      // 本轮到底补建目标窗口尾（推进到此即收尾）
    int                   m_iJumpTarget = -1;      // 跳建完成后滚到顶的目标数据下标（-1=普通重建跟到尾）
    int                   m_iJumpEnd = INT_MAX;    // 本轮重建窗口终点（普通重建=INT_MAX 跟到数据尾）
    std::function<void(CMessageBlock*)> m_fnJumpDone;   // 跳建完成回调（参数=目标块件；滚到顶用）
    std::function<void()> m_fnRebuildDone;         // 当前重建的完成回调
    QMetaObject::Connection m_aConnTop;            // sigTopReached 接线（Unbind 断开）
    QMetaObject::Connection m_aConnBottom;         // sigBottomReached 接线（Unbind 断开；2026-10-15）
    QMetaObject::Connection m_aConnDrop;           // sigBlockDropped 接线（Unbind 断开）
};

using CStreamPresenterPtr = std::shared_ptr<CStreamPresenter>;   // SAsst 按此持有（SAsst 值拷贝场景共享同一流）
