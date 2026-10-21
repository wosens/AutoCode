#include <QtTest>
#include <QSignalSpy>

#include "ui/CChatModel.h"
#include "ui/CChatListView.h"
#include "ui/CStreamPresenter.h"

// =============================================================================
// test_chatmodel —— 对话流 Model/View 自绘引擎回归（2026-10-17 自绘三期，
// doc/对话流视图自绘改造方案.md §5.3：模型层可单测 + presenter ModelView 分支集成）。
//
// 覆盖：
//   1. 折叠口径与 Legacy TryMergeWidget 对齐——连续 ToolUse 合并（含 Assistant
//      文本穿插不打断的 2026.10.15 口径）、viewId 不同不并、System 紧邻成组
//      （其它块打断）、Assistant/Event/Error 1:1、裸 JSON progress 行丢弃不断链；
//   2. 数据下标→条目映射（ItemAtDataIdx）与回填定位（BackfillResult：错误标红、
//      单条卡自动展开、×N 合并卡不摊开）；
//   3. CStreamPresenter ModelView 分支：Bind/Rebuild 投影、Append 增量、
//      BackfillResult 壳、EnsureCoverage 壳转发（回调参数=nullptr）、Unbind 摘挂；
//   4. 折叠/高亮交互态 setData（CollapsedRole/HighlightRole）→ dataChanged。
// =============================================================================
class TestChatModel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {}
    void cleanupTestCase() {}

    // —— 1. 折叠口径 ——
    void mergeTools();
    void mergeToolsAcrossAssistant();     // 2026.10.15 思维链穿插不打断
    void mergeToolsViewIdSplit();         // 评审视角不同不并
    void mergeSystemGroup();
    void oneToOneKinds();
    void legacyJsonProgressDropped();

    // —— 2. 映射与回填 ——
    void itemAtDataIdx();
    void backfillResultErrorExpand();
    void backfillMergedCardNoExpand();

    // —— 3. presenter ModelView 分支 ——
    void presenterModelViewFlow();
    void presenterUnbindDetaches();

    // —— 4. 交互态 setData ——
    void setDataCollapsedAndHighlight();

private:
    static SBlockData mkTool(const QString& a_sTool, const QString& a_sIn,
                             const QString& a_sViewId = QString());
    static SBlockData mkAssistant(const QString& a_sText);
    static SBlockData mkSystem(const QString& a_sText);
};

SBlockData TestChatModel::mkTool(const QString& a_sTool, const QString& a_sIn,
                                 const QString& a_sViewId)
{
    SBlockData st;
    st.eKind = CMessageBlock::ToolUse;
    st.sTool = a_sTool;
    st.sInput = a_sIn;
    st.sViewId = a_sViewId;
    return st;
}

SBlockData TestChatModel::mkAssistant(const QString& a_sText)
{
    SBlockData st;
    st.eKind = CMessageBlock::Assistant;
    st.sText = a_sText;
    st.sBackend = QStringLiteral("DSH");
    return st;
}

SBlockData TestChatModel::mkSystem(const QString& a_sText)
{
    SBlockData st;
    st.eKind = CMessageBlock::System;
    st.sText = a_sText;
    return st;
}

// ---------------------------------------------------------------------------
// 1. 折叠口径
// ---------------------------------------------------------------------------
void TestChatModel::mergeTools()
{
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{\"file_path\":\"a.cpp\"}")));
    m.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{\"command\":\"dir\"}")));
    m.AppendBlock(mkTool(QStringLiteral("grep"), QStringLiteral("{\"pattern\":\"x\"}")));
    QCOMPARE(m.ItemCount(), 1);   // 三连工具 → 一张合并卡
    const SDisplayItem* pIt = m.PeekItem(0);
    QVERIFY(pIt != nullptr);
    QCOMPARE(pIt->eKind, CMessageBlock::ToolUse);
    QCOMPARE(pIt->aTools.size(), 3);
    QCOMPARE(pIt->aTools.at(0).sTool, QStringLiteral("read"));
    QCOMPARE(pIt->aTools.at(2).sTool, QStringLiteral("grep"));
    QVERIFY(pIt->bCollapsed);    // 无错误：默认折叠
}

void TestChatModel::mergeToolsAcrossAssistant()
{
    // 2026.10.15 口径：DSH 每次工具调用前输出思维链文本（Assistant 块），工具合并
    //   卡不因穿插文本打断（Event/System/Error 仍打断）。
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    m.AppendBlock(mkAssistant(QStringLiteral("我先读取文件看看…")));
    m.AppendBlock(mkTool(QStringLiteral("edit"), QStringLiteral("{}")));
    m.AppendBlock(mkAssistant(QStringLiteral("改好了")));
    m.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{}")));
    QCOMPARE(m.ItemCount(), 3);   // 1 张 ×3 合并卡 + 2 条 Assistant 文本
    QCOMPARE(m.PeekItem(0)->eKind, CMessageBlock::ToolUse);
    QCOMPARE(m.PeekItem(0)->aTools.size(), 3);
    QCOMPARE(m.PeekItem(1)->eKind, CMessageBlock::Assistant);
    QCOMPARE(m.PeekItem(2)->eKind, CMessageBlock::Assistant);

    // Event 块打断：工具链关闭，后续工具另开新卡
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("user");
    stEv.sText = QStringLiteral("下一条提问");
    m.AppendBlock(stEv);
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    QCOMPARE(m.ItemCount(), 5);   // +Event +新工具卡
    QCOMPARE(m.PeekItem(4)->eKind, CMessageBlock::ToolUse);
    QCOMPARE(m.PeekItem(4)->aTools.size(), 1);
}

void TestChatModel::mergeToolsViewIdSplit()
{
    // 评审并行视角：viewId 不同不并（Legacy TryMergeWidget stPrev.sViewId 校验口径）
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}"), QStringLiteral("v1")));
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}"), QStringLiteral("v2")));
    QCOMPARE(m.ItemCount(), 2);
    // 同视角第三条并入 v2 卡
    m.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{}"), QStringLiteral("v2")));
    QCOMPARE(m.ItemCount(), 2);
    QCOMPARE(m.PeekItem(1)->aTools.size(), 2);
}

void TestChatModel::mergeSystemGroup()
{
    CChatModel m;
    m.AppendBlock(mkSystem(QStringLiteral("[system] line1")));
    m.AppendBlock(mkSystem(QStringLiteral("[system] line2")));
    m.AppendBlock(mkSystem(QStringLiteral("[system] line3")));
    QCOMPARE(m.ItemCount(), 1);   // 三连系统行 → 一张组卡
    QCOMPARE(m.PeekItem(0)->aSysLines.size(), 3);
    QVERIFY(m.PeekItem(0)->bSysCollapsed);

    // Assistant 打断系统组（Legacy：System 仅紧邻合并）
    m.AppendBlock(mkAssistant(QStringLiteral("回复")));
    m.AppendBlock(mkSystem(QStringLiteral("[system] line4")));
    QCOMPARE(m.ItemCount(), 3);   // 组卡 + Assistant + 新组卡
    QCOMPARE(m.PeekItem(2)->aSysLines.size(), 1);
}

void TestChatModel::oneToOneKinds()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral("回复文本")));
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("stage_done");
    stEv.sText = QStringLiteral("完成");
    m.AppendBlock(stEv);
    SBlockData stErr;
    stErr.eKind = CMessageBlock::Error;
    stErr.sText = QStringLiteral("[错误] 失败");
    m.AppendBlock(stErr);
    QCOMPARE(m.ItemCount(), 3);
    QCOMPARE(m.PeekItem(0)->eKind, CMessageBlock::Assistant);
    QCOMPARE(m.PeekItem(0)->sBackend, QStringLiteral("DSH"));
    QCOMPARE(m.PeekItem(1)->eKind, CMessageBlock::Event);
    QCOMPARE(m.PeekItem(1)->sEventKind, QStringLiteral("stage_done"));
    QCOMPARE(m.PeekItem(2)->eKind, CMessageBlock::Error);
    QCOMPARE(m.DataCount(), 3);
}

void TestChatModel::legacyJsonProgressDropped()
{
    // v2.0.113 裸 JSON progress 行：清洗为空 → 整体丢弃（不建条目、不断系统组链——
    // Legacy 里 progress 归并进前组卡；但该 System 数据块仍打断工具合并链）
    CChatModel m;
    m.AppendBlock(mkSystem(QStringLiteral("[system] before")));
    m.AppendBlock(mkSystem(QStringLiteral("{\"type\": \"progress\", \"chars\": 30}")));
    m.AppendBlock(mkSystem(QStringLiteral("[system] after")));
    QCOMPARE(m.ItemCount(), 1);
    QCOMPARE(m.PeekItem(0)->aSysLines.size(), 2);   // before/after 同组，progress 蒸发
    QCOMPARE(m.DataCount(), 3);                      // 数据计数仍锁步（含被丢弃行）

    // 工具链打断口径（Legacy TryMergeWidget：回溯遇 System 数据块即分卡，
    //   progress 虽丢弃也是 System 块）：Tool→progress→Tool = 两张卡
    CChatModel m2;
    m2.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    m2.AppendBlock(mkSystem(QStringLiteral("{\"type\": \"progress\", \"chars\": 12}")));
    m2.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{}")));
    QCOMPARE(m2.ItemCount(), 2);   // 两张独立工具卡
    QCOMPARE(m2.PeekItem(0)->aTools.size(), 1);
    QCOMPARE(m2.PeekItem(1)->aTools.size(), 1);
}

// ---------------------------------------------------------------------------
// 2. 映射与回填
// ---------------------------------------------------------------------------
void TestChatModel::itemAtDataIdx()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral("a0")));
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));       // idx1
    m.AppendBlock(mkAssistant(QStringLiteral("a1")));                          // idx2（穿插）
    m.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{}")));       // idx3 → 并入卡
    m.AppendBlock(mkSystem(QStringLiteral("[system] s")));                     // idx4
    // 条目：[0]=Assistant(a0) [1]=ToolCard(1,3) [2]=Assistant(a1) [3]=Sys
    QCOMPARE(m.ItemCount(), 4);
    QCOMPARE(m.ItemAtDataIdx(0), 0);
    QCOMPARE(m.ItemAtDataIdx(1), 1);   // 工具卡
    QCOMPARE(m.ItemAtDataIdx(2), 2);   // 穿插文本自身条目
    QCOMPARE(m.ItemAtDataIdx(3), 1);   // 并入卡的后续工具
    QCOMPARE(m.ItemAtDataIdx(4), 3);
    QCOMPARE(m.ItemAtDataIdx(-1), -1);
    QCOMPARE(m.ItemAtDataIdx(99), -1);
    // RebuildFrom 后映射重算
    QList<SBlockData> aBlocks;
    aBlocks << mkAssistant(QStringLiteral("x")) << mkTool(QStringLiteral("t"), QStringLiteral("{}"));
    m.RebuildFrom(aBlocks);
    QCOMPARE(m.ItemCount(), 2);
    QCOMPARE(m.ItemAtDataIdx(1), 1);
}

void TestChatModel::backfillResultErrorExpand()
{
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("bash"), QStringLiteral("{}")));
    // 实时错误回填：条目标错 + 卡头换肤标记 + 单条卡自动展开（Legacy SetResult 口径）
    QVERIFY(m.BackfillResult(0, QStringLiteral("Error: command not found")));
    const SDisplayItem* pIt = m.PeekItem(0);
    QVERIFY(pIt != nullptr);
    QCOMPARE(pIt->aTools.at(0).sResult, QStringLiteral("Error: command not found"));
    QVERIFY(pIt->aTools.at(0).bError);
    QVERIFY(pIt->bHadError);
    QVERIFY(!pIt->bCollapsed);   // 自动展开
    // 越界/非工具条目回填 miss
    QVERIFY(!m.BackfillResult(99, QStringLiteral("x")));
}

void TestChatModel::backfillMergedCardNoExpand()
{
    // ×N 合并卡：错误只标红不摊开（2026.10.15 pdfplumber 实害口径）
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    QVERIFY(m.BackfillResult(1, QStringLiteral("[错误] 权限拒绝")));
    const SDisplayItem* pIt = m.PeekItem(0);
    QCOMPARE(pIt->aTools.size(), 3);
    QVERIFY(pIt->aTools.at(1).bError);
    QVERIFY(pIt->bHadError);
    QVERIFY(pIt->bCollapsed);    // 合并卡不摊开
    // 正常结果：无错误标记
    QVERIFY(m.BackfillResult(0, QStringLiteral("1 file read")));
    QVERIFY(!pIt->aTools.at(0).bError);
}

// ---------------------------------------------------------------------------
// 3. presenter ModelView 分支
// ---------------------------------------------------------------------------
void TestChatModel::presenterModelViewFlow()
{
    int iDirty = 0;
    CStreamPresenter st([&iDirty]() { ++iDirty; });   // fnDirty：数据变化回调
    CChatModel m;
    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    st.Bind(&m, &v);

    // Rebuild：SetBlocks 后投影（异步完成回调语义）
    QList<SBlockData> aBlocks;
    aBlocks << mkAssistant(QStringLiteral("旧回复"))
            << mkTool(QStringLiteral("read"), QStringLiteral("{}"));
    st.SetBlocks(aBlocks);
    bool bDone = false;
    st.Rebuild([&bDone]() { bDone = true; });
    QVERIFY(!bDone);                       // 异步 singleShot(0) 回调
    qApp->processEvents();                 // 派发
    QVERIFY(bDone);
    QVERIFY(st.ModelView());
    QVERIFY(st.Projected());
    QCOMPARE(m.ItemCount(), 2);

    // Append：数据 + 模型增量 + fnDirty
    st.Append(mkTool(QStringLiteral("bash"), QStringLiteral("{}")));
    qApp->processEvents();
    QCOMPARE(st.Count(), 3);
    QCOMPARE(m.ItemCount(), 2);            // bash 并入工具卡
    QCOMPARE(m.PeekItem(1)->aTools.size(), 2);
    QVERIFY(iDirty >= 1);

    // BackfillResult：数据 + 模型条目快照（尾回溯找最后未闭合工具 = 刚追加的 bash）
    const int iIdx = st.BackfillResult(QStringLiteral("Error: boom"));
    QCOMPARE(iIdx, 2);
    QVERIFY(m.PeekItem(1)->aTools.at(1).bError);

    // EnsureCoverage：壳转发——回调参数=nullptr（MV 无块概念，调用方跳过滚动）
    CMessageBlock* pGot = reinterpret_cast<CMessageBlock*>(quintptr(1));
    st.EnsureCoverage(0, [&pGot](CMessageBlock* a_pBlock) { pGot = a_pBlock; });
    QCOMPARE(pGot, nullptr);

    // 纯数据态判定：Bound 含 ModelView
    QVERIFY(st.Bound());
    QCOMPARE(st.WidgetCount(), 0);         // MV 恒 0（无件概念）

    st.Unbind();
}

void TestChatModel::presenterUnbindDetaches()
{
    // Unbind 摘挂：模型空白化（切档视图清场语义），模型/视图本体存活（外层所有）
    CStreamPresenter st;
    auto* pModel = new CChatModel;
    auto* pView = new CChatListView;
    pView->resize(600, 400);
    pView->SetModel(pModel);
    st.Bind(pModel, pView);
    st.SetBlocks({mkAssistant(QStringLiteral("数据"))});
    st.Rebuild();                    // 投影（Append 在未投影窗口只入数据，与 Legacy 同语义）
    qApp->processEvents();
    QCOMPARE(pModel->ItemCount(), 1);

    st.Unbind();
    QVERIFY(!st.ModelView());
    QVERIFY(!st.Projected());
    QCOMPARE(pModel->ItemCount(), 0);      // ClearDisplay 已空白化
    QVERIFY(pModel != nullptr && pView != nullptr);   // 本体不归流销毁

    // 复绑别的流照常投影（共享模型，任务侧多流复用同构）
    CStreamPresenter st2;
    st2.SetBlocks({mkAssistant(QStringLiteral("另一条"))});
    st2.Bind(pModel, pView);
    st2.Rebuild();
    qApp->processEvents();
    QCOMPARE(pModel->ItemCount(), 1);

    st2.Unbind();
    delete pView;
    delete pModel;   // 模型无 parent（SetModel 不收养），显式释放防泄漏
}

// ---------------------------------------------------------------------------
// 4. 交互态 setData
// ---------------------------------------------------------------------------
void TestChatModel::setDataCollapsedAndHighlight()
{
    CChatModel m;
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));
    QSignalSpy spy(&m, &CChatModel::dataChanged);
    const QModelIndex idx = m.index(0, 0);

    QVERIFY(m.setData(idx, false, CChatModel::CollapsedRole));
    QVERIFY(!m.PeekItem(0)->bCollapsed);
    QCOMPARE(idx.data(CChatModel::CollapsedRole).toBool(), false);
    QVERIFY(!spy.isEmpty());

    // 高亮（跳转）：写 + 读
    QVERIFY(m.setData(idx, true, CChatModel::HighlightRole));
    QVERIFY(m.PeekItem(0)->bHighlight);
    QCOMPARE(idx.data(CChatModel::HighlightRole).toBool(), true);

    // System 组卡的折叠态走 bSysCollapsed
    CChatModel m2;
    m2.AppendBlock(mkSystem(QStringLiteral("[system] s1")));
    m2.AppendBlock(mkSystem(QStringLiteral("[system] s2")));
    const QModelIndex idx2 = m2.index(0, 0);
    QVERIFY(m2.setData(idx2, false, CChatModel::CollapsedRole));
    QVERIFY(!m2.PeekItem(0)->bSysCollapsed);
    QVERIFY(m2.PeekItem(0)->bCollapsed);   // 工具折叠位未被误写
}

QTEST_MAIN(TestChatModel)
#include "test_chatmodel.moc"
