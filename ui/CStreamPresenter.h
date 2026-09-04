#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <functional>
#include <memory>

#include "CMessageBlock.h"

class CMessageView;

// =============================================================================
// SBlockData：一条对话流中单个消息块的数据快照（2026.09.03 自 CExecutionPanel 嵌套
// 结构迁出为全局——任务日志与各助手日志共用 CStreamPresenter，数据结构须独立于面板）。
// 由流呈现器持有；面板按 taskId（任务流）/助手 id（助手流）路由落盘（chat.json/assistant.json）。
// =============================================================================
struct SBlockData {
    CMessageBlock::EKind eKind = CMessageBlock::Assistant;
    QString sText;        // Assistant/Event/System/Error 文本
    QString sTool;        // ToolUse 工具名
    QString sInput;       // ToolUse 入参 JSON
    QString sResult;      // ToolUse 结果（到达后回填）
    QString sEventKind;   // Event 的 kind
    QString sViewId;      // 评审多视角并行的视角标识（v1/v2/...；空=串行路径）
    QString sBackend;     // Assistant 块的产出后端显示名（消息到达时拍摄，随块持久化）
};

QJsonObject BlockToJson(const SBlockData& a_stData);   // SBlockData ↔ JSON（chat.json/assistant.json 共用）
SBlockData BlockFromJson(const QJsonObject& a_o);

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
    void Unbind();                      // 解绑：断线 + 清视图块 + 弃件保数据 + 复位前沿/作废在途帧
    CMessageView* View() const { return m_pView; }
    bool Bound() const { return m_pView != nullptr; }

    // —— 数据 ——
    QList<SBlockData>& Blocks() { return m_aBlocks; }
    const QList<SBlockData>& Blocks() const { return m_aBlocks; }
    void SetBlocks(const QList<SBlockData>& a_aBlocks) { m_aBlocks = a_aBlocks; }   // 载入/换数据（绑定时调用方应跟 Rebuild）
    int Count() const { return m_aBlocks.size(); }
    int WidgetCount() const { return m_aWidgets.size(); }   // 已建件数（0=未建/已弃件，切档惰性重建判定用）
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
    // 数据下标 → 块 widget（未建件区间返回 nullptr）
    CMessageBlock* WidgetAt(int a_iIdx) const;
    // 结果回填：从尾回溯找最后一个未闭合 ToolUse（可选视角过滤）写数据 + 刷件 + fnDirty；
    // 返回数据下标，-1=没找到（调用方自行兜底为系统行）。
    int BackfillResult(const QString& a_sResult, const QString& a_sViewId = QString());
    // 跳转覆盖：目标块早于前沿时分轮补建（每轮一帧突发，交还事件循环防长冻）至覆盖后回调；
    // 已覆盖则同步回调。目标越界/未绑定不回调。
    void EnsureCoverage(int a_iTargetIdx, std::function<void(CMessageBlock*)> a_fnDone);

private:
    void RebuildChunk(quint32 a_uGen);                      // 分帧重建一帧（代际号不符=被顶替，立即终止）
    void BuildOlderChunk(QList<CMessageBlock*>& a_aNewWidgets);   // 补本帧 8 块 [frontier-8, frontier)（块内合并仍生效）
    void PrependFrame(quint32 a_uGen);                      // 2026.09.04 到顶补建分帧驱动（8 块/帧至本轮目标）
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
    CMessageView*         m_pView = nullptr;       // 绑定视图（空=纯数据态：任务未选中/助手 Tab 已关）
    std::function<void()> m_fnDirty;               // 数据变化回调（落盘路由）
    QList<SBlockData>     m_aBlocks;               // 对话流数据（唯一正本）
    QList<CMessageBlock*> m_aWidgets;              // 并行块 widget（aWidgets[k] ↔ 数据下标 frontier+k）
    int                   m_iFrontier = 0;         // 反向分页前沿：[0,frontier) 未建件
    int                   m_iRebuildIdx = 0;       // 分帧重建进度
    quint32               m_uGen = 0;              // 分帧重建代际号（Rebuild/Unbind 时 +1 作废旧帧）
    bool                  m_bRebuilding = false;   // 分帧重建中（Append 只入数据，帧循环按序补件）
    bool                  m_bPrepending = false;   // 2026.09.04 到顶补建分帧进行中（挡重复到顶信号）
    int                   m_iPrependTarget = 0;    // 本轮补建目标前沿（frontier 推进到此即收尾）
    std::function<void()> m_fnRebuildDone;         // 当前重建的完成回调
    QMetaObject::Connection m_aConnTop;            // sigTopReached 接线（Unbind 断开）
    QMetaObject::Connection m_aConnDrop;           // sigBlockDropped 接线（Unbind 断开）
};

using CStreamPresenterPtr = std::shared_ptr<CStreamPresenter>;   // SAsst 按此持有（SAsst 值拷贝场景共享同一流）
