#pragma once

// ---------------------------------------------------------------------------
// CChatModel —— 对话流 Model/View 自绘渲染链的显示条目模型（2026-10-17 自绘三期，
// doc/对话流视图自绘改造方案.md §5；参照 HarnessAgent v1.9.44 ChatModel 移植并
// 适配本工程 SBlockData 数据源）。
// 职责：把 CStreamPresenter 的原始块序列（SBlockData×N，正本不动）折叠为显示条目
// （SDisplayItem×M，M ≤ N）——合并口径与 Legacy 引擎 TryMergeWidget 逐条对齐：
//   连续 ToolUse（中间穿插的 Assistant 文本不打断，2026.10.15 口径；viewId 需一致）
//     → 合并卡（aTools 逐条快照，结果回填改这里）；
//   连续 System（紧邻，中间任何其它块打断）→ 组卡（PrettyLegacyJsonLine 清洗，
//     空串行整体丢弃：不建不并不断链）；
//   Assistant / Event / Error → 1:1。
// 数据正本仍在 CStreamPresenter::m_aBlocks（落盘 chat.json/assistant.json 零迁移）：
// 本模型只是显示投影，RebuildFrom 可随时全量重建；交互态（折叠/高亮）为内存态
// 不落盘。无 Q_OBJECT（同 CChatDocPool 先例）：只覆写基类虚函数并用其自带信号
// （dataChanged/rowsInserted/modelReset），折叠/高亮经 role 读写，免 QtMoc。
// ---------------------------------------------------------------------------

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "CMessageBlock.h"   // EKind（复用现有枚举，SBlockData 同源）
#include "SBlockData.h"      // 数据源结构（2026-10-17 自 CStreamPresenter.h 拆出，解环）

struct SDisplayItem;   // 完整定义在本文件末（CChatModel 内仅指针/引用使用）

class CChatModel : public QAbstractListModel {
public:
    enum ERole {
        KindRole = Qt::UserRole + 1,   // int CMessageBlock::EKind —— delegate 分发 paint
        TextRole,                      // QString Assistant 正文 Markdown / Event/System/Error 文本
        SpeakerRole,                   // QString Assistant 产出后端名（块到达时快照，不随后端设置漂移）
        ToolEntriesRole,               // QVariantList[{tool,input,result,error}] —— 合并卡逐条快照
        EventKindRole,                 // QString Event 徽章 kind（user/stage_done/…）
        CollapsedRole,                 // bool ToolUse/System 折叠态；setData 可写
        HighlightRole,                 // bool 跳转高亮（视图写、1200ms 自清；不落盘）
        BeginRole,                     // int 条目起始数据下标（= presenter.m_aBlocks 下标）
    };

    explicit CChatModel(QObject* a_pParent = nullptr);

    // —— 喂入（CStreamPresenter 接缝；数据正本在 presenter，模型只做投影）——
    void RebuildFrom(const QList<SBlockData>& a_aBlocks);   // 载档/切档全量重建（一次线性折叠）
    void AppendBlock(const SBlockData& a_stData);           // 流式追加（增量合并，O(1) 摊销）
    // 结果回填（实时路径）：数据下标 → 定位条目与卡内条目 → 写快照 + 错误标红 +
    //   单条卡自动展开（对齐 Legacy CMessageBlock::SetResult(a_bAutoExpandOnError=true)
    //   口径：仅单条卡自动展开，×N 合并卡只标红）。未命中返回 false。
    bool BackfillResult(int a_iDataIdx, const QString& a_sResult);
    void ClearDisplay();                          // 空白视图（Unbind 切档用；beginResetModel+清条目）

    // —— 查询（视图/跳转/测试）——
    int ItemCount() const { return static_cast<int>(m_aItems.size()); }
    int ItemAtDataIdx(int a_iDataIdx) const;      // 数据下标→条目下标（映射表直查；越界/被丢弃行 -1）
    const SDisplayItem* PeekItem(int a_iIdx) const;   // 条目直读（delegate/测试断言用；越界 nullptr）
                                                     // 返回指针仅限当次使用：禁止跨 Append/Rebuild 持有
    int DataCount() const { return m_aIdx2Item.size(); }   // 已喂块数（与 presenter.Blocks() 锁步）
    void TouchSizeHint(int a_iIdx);               // 高度校正后由视图调：dataChanged(idx,{SizeHintRole})

    // QAbstractListModel 覆写
    int rowCount(const QModelIndex& a_parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& a_index, int a_iRole) const override;
    bool setData(const QModelIndex& a_index, const QVariant& a_value, int a_iRole) override;

private:
    SDisplayItem& appendItem(CMessageBlock::EKind a_eKind);   // beginInsertRows+尾插（iBegin=当前块计数）
    void emitChanged(int a_iIdx);                          // 内容变化：dataChanged（空 roles = 全角色）
    SDisplayItem* itemAt(int a_iIdx);                      // PeekItem 的可变重载（setData 等内部写路径用）

    QList<SDisplayItem> m_aItems;   // 显示条目序列（M ≤ N，合并判定在模型层完成）
    QList<int>          m_aIdx2Item;// 数据下标 → 条目下标（逐块记录；被丢弃行 -1）
    int m_iSys = -1;                // 追加中的 System 组卡下标（-1=无；非 System 块打断）
    int m_iTool = -1;               // 追加中的 ToolUse 合并卡下标（-1=无；Assistant 文本不打断）
};

// 工具卡条目快照（合并卡逐条；iData=对应数据下标，结果回填定位用）
struct SToolEntry {
    QString sTool, sInput, sResult;
    bool bError = false;            // 结果经 IsErrorResult 判错（卡头换肤+自动展开依据）
    int iData = -1;                 // 本条对应的原始数据下标
};

// 显示条目（内存结构，不落盘；对齐方案 §4.3 并按本工程块型裁剪）
struct SDisplayItem {
    CMessageBlock::EKind eKind = CMessageBlock::Assistant;
    int iBegin = 0;                     // 起始数据下标（1:1 条目即自身）
    QString sText;                      // Assistant 正文 Markdown / Event/System/Error 文本
    QString sEventKind;                 // Event 徽章 kind
    QString sBackend;                   // Assistant 产出后端（建条目时快照，空→"Agent"）
    QString sViewId;                    // 评审视角标识（合并卡 viewId 一致性判定基准）
    QList<SToolEntry> aTools;           // ToolUse 合并卡条目（单卡=1 条）
    bool bCollapsed = true;             // ToolUse 折叠态（实时错误单条卡自动置 false）
    bool bHadError = false;             // 合并卡任一条目出错（折叠头换肤依据）
    QStringList aSysLines;              // System 组卡行（PrettyLegacyJsonLine 清洗后）
    bool bSysCollapsed = true;          // System 折叠态
    bool bHighlight = false;            // 跳转高亮（1200ms 自清）
};
