#include <QtTest>
#include <QSignalSpy>

#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextLayout>

#include "ui/CChatModel.h"
#include "ui/CChatDocPool.h"
#include "ui/CChatDelegate.h"
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
//   4. 折叠/高亮交互态 setData（CollapsedRole/HighlightRole）→ dataChanged；
//   5. 2026-10-22 双实害修复：①池首次材化即回调 fnDocReady（旧版一次性整段
//      回复估高永不校正 → 后续「已完成」事件叠画回复尾部）+ Event 估高=绘制高
//      （旧式欠 leading）+ Assistant 绘制裁剪到行矩形；②双击任意块开复制层
//      （Assistant=Markdown 源、其余=纯文本；ToolUse/System 折叠头区双击归折叠）。
//   6. 2026-10-23 条目选择复制（借鉴 HarnessAgent v1.9.50）：选择三件套
//      + CopySelection 行号升序拼接 + Ctrl+C 键径 + 无选中不动剪贴板。
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

    // —— 8. 2026-10-23 激活页首块投影修复（用户实害：发提示词后日志空白）——
    void freshBindFirstAppendProjects();
    void ghostPageStaysLazy();

    // —— 4. 交互态 setData ——
    void setDataCollapsedAndHighlight();

    // —— 5. 2026-10-22 首次材化高度校正 / 复制层全块型开放 ——
    void poolFirstMaterializeFiresDocReady();
    void eventEstimateMatchesPaintHeight();
    void firstPaintCorrectsAssistantHeight();
    void plainTextForOverlay();
    void overlayOpenAndHeaderGuard();

    // —— 7. 2026-10-23 栈溢出防回归（切助手三连崩 0xC00000FD）——
    void widthReformatNoRecurse();          // 临界震荡：重排链中滚动条翻转不递归

    // —— 6. 2026-10-23 条目选择复制（借鉴 HarnessAgent v1.9.50）——
    void itemSelectionAndCopy();

    // —— 7. 2026-10-23 池删除回调/析构防护（v5.0.412 崩溃实害）——
    void overlaySurvivesDocEviction();
    void overlayDestructorSafety();

    // —— 9. 2026-10-23 焦点移出清选择（用户需求；popup 豁免）——
    void selectionClearsOnFocusOut();

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

// ---------------------------------------------------------------------------
// 5. 2026-10-22 双实害修复回归
// ---------------------------------------------------------------------------
void TestChatModel::poolFirstMaterializeFiresDocReady()
{
    // 旧版 Acquire 仅「命中且内容/宽度变化」路径回调 fnDocReady——一次性整段回复
    //（claude/pi/hermes 单条 sigAssistantText）只有一次 Acquire（新建插入），
    // 算术估高永不被 doc 实高校正，后续条目叠画在回复尾部上（「已完成」实害）
    CChatDocPool pool;
    int iReady = 0;
    int iLastRow = -1;
    QTextDocument* pLast = nullptr;
    pool.fnDocReady = [&](int r, QTextDocument* pDoc) { ++iReady; iLastRow = r; pLast = pDoc; };
    QTextDocument* pDoc = pool.Acquire(0, QStringLiteral("# 标题\n\n正文"), 300);
    QVERIFY(pDoc != nullptr);
    QCOMPARE(iReady, 1);                    // 首次材化即回调
    QCOMPARE(iLastRow, 0);
    QVERIFY(pLast == pDoc);
    QVERIFY(pool.Acquire(0, QStringLiteral("# 标题\n\n正文"), 300) == pDoc);
    QCOMPARE(iReady, 1);                    // 同内容同宽：不重复回调
    pool.Acquire(0, QStringLiteral("# 标题\n\n正文增长"), 300);
    QCOMPARE(iReady, 2);                    // 内容变化 → 重材化回调（既有口径不变）
}

void TestChatModel::eventEstimateMatchesPaintHeight()
{
    // Event 无 doc 校正链兜底，估高必须与绘制严格同源。旧式 qMax(iFmH,16)+iWrapped-
    // lineSpacing 在字体 leading>0 时每行欠 leading px → 后续条目叠画本行尾部
    const QString sText = QStringLiteral(
        "已完成（16:47:43，耗时 1分23秒）——较长的正文确保换行路径也被覆盖，再补一段让 QTextLayout 至少两行。");
    QFont f = qApp->font();
    CChatDocPool pool;
    CChatDelegate d(&pool, f);
    SDisplayItem it;
    it.eKind = CMessageBlock::Event;
    it.sEventKind = QStringLiteral("done");
    it.sText = sText;
    const int iW = 373;
    const int iEst = d.EstimateHeight(it, iW);
    // 绘制高度镜像（与 delegate layoutWrapped 同式：逐行 lineWidth+累计 ln.height()）
    QFont fB = f;
    fB.setWeight(QFont::DemiBold);          // done=粗体徽章，估高同字体
    QTextLayout lay(QStringLiteral("🎉 ") + sText, fB);
    lay.beginLayout();
    int y = 0;
    for (;;) {
        QTextLine ln = lay.createLine();
        if (!ln.isValid())
            break;
        ln.setLineWidth(iW);
        ln.setPosition(QPointF(0, y));
        y += static_cast<int>(ln.height());
    }
    lay.endLayout();
    QCOMPARE(iEst, y);                      // 估高 == 绘制高（不再欠 leading）
}

void TestChatModel::firstPaintCorrectsAssistantHeight()
{
    // 重叠实害链回归：长 Markdown 回复首次绘制 → 池新建 doc → 首次材化回调 →
    // delegate 高度缓存 = doc 实高 + speaker + 边距（行高容纳全部绘制内容，
    // 后续「已完成」行不再叠画在回复尾部上）
    CChatModel m;
    QString sMd;
    for (int i = 0; i < 24; ++i)
        sMd += QStringLiteral("第 %1 段——这是一段足够长的中文正文，确保文档实高明显超过算术估高。\n\n").arg(i + 1);
    sMd += QStringLiteral("```cpp\nint main() { return 0; }\n```\n");
    m.AppendBlock(mkAssistant(sMd));
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("done");
    stEv.sText = QStringLiteral("已完成（16:47:43，耗时 1分23秒）");
    m.AppendBlock(stEv);

    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    v.grab();                               // 同步 paint：首行入视口 → 池首次材化
    qApp->processEvents();                  // dataChanged 重排落地

    QTextDocument* pDoc = v.DocPool()->Peek(0);
    QVERIFY(pDoc != nullptr);
    QFont fSp = v.font();
    fSp.setWeight(QFont::DemiBold);
    const int iReal = qRound(pDoc->size().height())
                    + QFontMetrics(fSp).height() + 4 + 2;   // OnDocReady 同式
    QCOMPARE(v.Delegate()->CachedHeight(0), iReal);   // 旧版此处=算术估高≠实高
    // 事件行紧随其后：行高也已就位（估==绘，不再欠 leading）
    QVERIFY(v.Delegate()->CachedHeight(1) > 0);
}

void TestChatModel::plainTextForOverlay()
{
    // 复制层非 Assistant 数据源：装饰（徽章/组头）不进文本，复制产物直接可用
    CChatModel m;
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("done");
    stEv.sText = QStringLiteral("已完成");
    m.AppendBlock(stEv);
    m.AppendBlock(mkSystem(QStringLiteral("[system] line1")));
    m.AppendBlock(mkSystem(QStringLiteral("[system] line2")));
    m.AppendBlock(mkTool(QStringLiteral("edit"),
                         QStringLiteral("{\"old_string\":\"a\",\"new_string\":\"b\"}")));
    CChatDocPool pool;
    CChatDelegate d(&pool, qApp->font());
    QCOMPARE(d.PlainTextAt(m.index(0, 0)), QStringLiteral("已完成"));
    QCOMPARE(d.PlainTextAt(m.index(1, 0)),
             QStringLiteral("[system] line1\n[system] line2"));
    const QString sTool = d.PlainTextAt(m.index(2, 0));
    QVERIFY(sTool.contains(QStringLiteral("【edit】")));   // 工具名+摘要头
    QVERIFY(sTool.contains(QStringLiteral("入参")));
    QVERIFY(sTool.contains(QStringLiteral("- a")));       // Edit diff 旧行
    QVERIFY(sTool.contains(QStringLiteral("+ b")));       // Edit diff 新行
    QVERIFY(sTool.contains(QStringLiteral("结果")));
    QVERIFY(d.PlainTextAt(m.index(999, 0)).isEmpty());     // 越界安全
    QVERIFY(d.PlainTextAt(m.index(0, 0)).contains(QStringLiteral("完成")));
}

void TestChatModel::overlayOpenAndHeaderGuard()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral(
        "回复正文第一段。\n\n第二段——用于复制层内容断言。\n\n- 列表项一\n- 列表项二\n")));
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("done");
    stEv.sText = QStringLiteral("已完成（16:47:43，耗时 1分23秒）");
    m.AppendBlock(stEv);
    m.AppendBlock(mkTool(QStringLiteral("read"),
                         QStringLiteral("{\"file_path\":\"a.cpp\"}")));

    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    v.show();
    qApp->processEvents();

    // ① 双击事件行 → 复制层开，纯文本=正文（徽章不进文本）
    v.scrollTo(m.index(1, 0), QAbstractItemView::PositionAtTop);
    qApp->processEvents();
    const QRect rcEv = v.visualRect(m.index(1, 0));
    QVERIFY(rcEv.isValid());
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcEv.topLeft() + QPoint(rcEv.width() / 2, rcEv.height() / 2));
    qApp->processEvents();
    QVERIFY(v.OverlayVisible());
    QCOMPARE(v.OverlayText(), QStringLiteral("已完成（16:47:43，耗时 1分23秒）"));
    // 重影回归（2026-10-26 实害：QSS background:transparent 把 Palette 米黄底废掉，
    //   浮层文字与 delegate 已绘制的原条目文字同位叠加成双影，看不清无法拖选）——
    //   底色必须不透明且为浮层米黄（QSS 优先级高于 Palette，两者都要锁）
    QVERIFY(!v.OverlayStyleSheet().contains(QStringLiteral("transparent")));
    QVERIFY(v.OverlayStyleSheet().contains(QStringLiteral("#fffbeb")));
    // 再双击任意处 → 收层（首个分支吞掉本次双击不重开）
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcEv.topLeft() + QPoint(rcEv.width() / 2, rcEv.height() / 2));
    qApp->processEvents();
    QVERIFY(!v.OverlayVisible());

    // ② 双击助手行 → 层内容含源文本片段（Markdown doc 渲染后的纯文本）
    v.scrollTo(m.index(0, 0), QAbstractItemView::PositionAtTop);
    qApp->processEvents();
    const QRect rcAs = v.visualRect(m.index(0, 0));
    QVERIFY(rcAs.isValid());
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcAs.topLeft() + QPoint(rcAs.width() / 2, 30));
    qApp->processEvents();
    QVERIFY(v.OverlayVisible());
    QVERIFY(v.OverlayText().contains(QStringLiteral("第二段")));
    QVERIFY(v.OverlayText().contains(QStringLiteral("列表项一")));
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcAs.topLeft() + QPoint(10, 10));
    qApp->processEvents();
    QVERIFY(!v.OverlayVisible());

    // ③ 工具卡：折叠头区双击不开层（双击归折叠语义）；卡身双击开层含入参/结果
    m.setData(m.index(2, 0), false, CChatModel::CollapsedRole);   // 展开使卡有非头区
    qApp->processEvents();
    v.scrollTo(m.index(2, 0), QAbstractItemView::PositionAtTop);
    qApp->processEvents();
    v.grab();                               // 同步 paint 记录折叠头命中区
    QRect rcTool = v.visualRect(m.index(2, 0));
    QVERIFY(rcTool.isValid() && rcTool.height() > 60);
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcTool.topLeft() + QPoint(rcTool.width() / 2, 10));   // 头区
    qApp->processEvents();
    QVERIFY(!v.OverlayVisible());
    rcTool = v.visualRect(m.index(2, 0));   // 头区双击翻两次折叠后重取几何
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcTool.topLeft() + QPoint(rcTool.width() / 2,
                                                 rcTool.height() - 12));    // 卡身
    qApp->processEvents();
    QVERIFY(v.OverlayVisible());
    QVERIFY(v.OverlayText().contains(QStringLiteral("入参")));
    QVERIFY(v.OverlayText().contains(QStringLiteral("file_path")));   // 入参 JSON 文本可复制
    QVERIFY(v.OverlayText().contains(QStringLiteral("结果")));
}

// ---------------------------------------------------------------------------
// 6. 2026-10-23 条目选择复制（借鉴 HarnessAgent v1.9.50）：
//    ExtendedSelection+整行+框选三件套；CopySelection 所选条目纯文本按行号
//    升序拼剪贴板（Assistant=源文本，其余=PlainTextAt 口径）；Ctrl+C 键径；
//    无选中不动剪贴板
// ---------------------------------------------------------------------------
void TestChatModel::itemSelectionAndCopy()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(
        QStringLiteral("助手回复源文本第一段。\n\n第二段——复制应保持源文本。")));
    SBlockData stEv;
    stEv.eKind = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("done");
    stEv.sText = QStringLiteral("已完成（17:01:00，耗时 5秒）");
    m.AppendBlock(stEv);
    m.AppendBlock(mkTool(QStringLiteral("read"),
                         QStringLiteral("{\"file_path\":\"a.cpp\"}")));

    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);

    // ① 视图选择三件套（Extended 多选 / 整行；selectionRectVisible 为只写属性
    //    无 getter，构造已设，不在此断言）
    QCOMPARE(v.selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(v.selectionBehavior(), QAbstractItemView::SelectRows);
    QVERIFY(v.selectionModel() != nullptr);

    QClipboard* pClip = QGuiApplication::clipboard();
    pClip->clear();

    // ② 无选中：CopySelection 不动剪贴板
    v.CopySelection();
    QVERIFY(pClip->text().isEmpty());

    // ③ 乱序选择（先选行2工具，再选行0助手、行1事件）→ 拼接严格按行号升序；
    //    Assistant=正文源文本、Event=正文、ToolUse=【工具名】逐条口径
    QItemSelectionModel* pSel = v.selectionModel();
    pSel->select(m.index(2, 0),
                 QItemSelectionModel::Select | QItemSelectionModel::Rows);
    pSel->select(m.index(0, 0),
                 QItemSelectionModel::Select | QItemSelectionModel::Rows);
    pSel->select(m.index(1, 0),
                 QItemSelectionModel::Select | QItemSelectionModel::Rows);
    v.CopySelection();
    const QString sCopied = pClip->text();
    QVERIFY(sCopied.contains(QStringLiteral("助手回复源文本第一段。")));
    QVERIFY(sCopied.contains(QStringLiteral("已完成（17:01:00，耗时 5秒）")));
    QVERIFY(sCopied.contains(QStringLiteral("【read】")));
    QVERIFY(sCopied.indexOf(QStringLiteral("助手回复"))
            < sCopied.indexOf(QStringLiteral("已完成")));   // 行0 在行1 前
    QVERIFY(sCopied.indexOf(QStringLiteral("已完成"))
            < sCopied.indexOf(QStringLiteral("【read】"))); // 行1 在行2 前

    // ④ Ctrl+C 键径：清板重选单条事件行，keyClick 后板=事件正文
    pClip->clear();
    pSel->clearSelection();
    v.show();
    v.setFocus();
    qApp->processEvents();
    pSel->select(m.index(1, 0), QItemSelectionModel::ClearAndSelect
                                    | QItemSelectionModel::Rows);
    QTest::keyClick(&v, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(pClip->text(), QStringLiteral("已完成（17:01:00，耗时 5秒）"));

    pClip->clear();   // 清板防污染后续用例/环境
}

// ---------------------------------------------------------------------------
// 7. 2026-10-23 池删除回调/析构防护（v5.0.412 实害：复制浮层裸持池 doc，
//    池逐出/Clear 直接删 doc → 浮层绘制/析构 UAF 崩，
//    Qt6Gui QTextDocument::documentLayout+0x11 0xC0000005）
// ---------------------------------------------------------------------------
void TestChatModel::overlaySurvivesDocEviction()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(
        QStringLiteral("被浮层引用的长回复——多段正文确保 doc 实高校正路径被走。\n\n第二段。\n\n第三段。")));
    for (int i = 1; i <= 70; ++i)
        m.AppendBlock(mkAssistant(QStringLiteral("filler %1 正文").arg(i)));

    CChatListView* pV = new CChatListView;
    pV->resize(600, 400);
    pV->SetModel(&m);
    pV->show();
    qApp->processEvents();

    // 双击首行 Assistant 开浮层（浮层 doc = 池行 0）
    pV->scrollTo(m.index(0, 0), QAbstractItemView::PositionAtTop);
    qApp->processEvents();
    const QRect rc = pV->visualRect(m.index(0, 0));
    QVERIFY(rc.isValid());
    QTest::mouseDClick(pV->viewport(), Qt::LeftButton, Qt::NoModifier,
                       rc.topLeft() + QPoint(rc.width() / 2, 30));
    qApp->processEvents();
    QVERIFY(pV->OverlayVisible());

    // 超限逐出：池上限 64，再 Acquire 70 个新行把行 0 挤到 LRU 尾
    //（浮层行开层时 touch 过在 MRU 端，循环新增后成为最旧被逐出）。
    // 旧代码：delete 直接执行 → 浮层悬垂引用；新代码：删除前回调收层
    CChatDocPool* pPool = pV->DocPool();
    for (int i = 1; i <= 70; ++i)
        pPool->Acquire(i, QStringLiteral("filler %1").arg(i), 400);
    QVERIFY(pPool->Peek(0) == nullptr);     // 行 0 doc 确实被逐出
    QVERIFY(!pV->OverlayVisible());         // 浮层在 delete 前已收（引用已摘）

    delete pV;
    qApp->processEvents();
}

void TestChatModel::overlayDestructorSafety()
{
    // 浮层开着直接销毁视图：显式析构先收层（浮层换自持空 doc）——
    // 拆除「池先删 doc（children 逆序）→ viewport 后删浮层 → QTextBrowser
    // 析构访问已删 doc」的析构序 UAF 炸弹
    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral("析构安全测试正文\n\n第二段正文。")));
    CChatListView* pV = new CChatListView;
    pV->resize(600, 400);
    pV->SetModel(&m);
    pV->show();
    qApp->processEvents();
    const QRect rc = pV->visualRect(m.index(0, 0));
    QVERIFY(rc.isValid());
    QTest::mouseDClick(pV->viewport(), Qt::LeftButton, Qt::NoModifier,
                       rc.topLeft() + QPoint(rc.width() / 2, 30));
    qApp->processEvents();
    QVERIFY(pV->OverlayVisible());
    delete pV;                               // 旧代码此处析构序 UAF 崩
    qApp->processEvents();
}

// —— 7. 栈溢出防回归（2026-10-23 切助手三连崩 0xC00000FD）——
// 机制：QAbstractScrollArea::viewportEvent 把 viewport 的 Resize 直接
// QFrame::event(e) 转进部件 resizeEvent。宽度重排链（InvalidateWidth→池逐条
// reformat→fnDocReady→OnDocReady→onHeightCorrected→visualRect 同步布局→滚动条
// 显隐翻转→viewport 宽 ±10→viewport Resize）会同步重入重排，旧代码无限递归
// 直到栈耗尽（probe_sorec 旧代码 0xC00000FD / 修复后 0x0 已实测对照）。
// 修复：ApplyWidth 统一入口重入挡板（m_bInWidthReformat）+ pending 延迟一拍收敛。
// 用例：临界震荡内容（估高总和<视口高而实高总和>视口高，滚动条在重排链中
// 翻转）+ 多次宽度变化，跑完不崩且宽度收敛即过。
void TestChatModel::widthReformatNoRecurse()
{
    CChatModel m;
    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    // 临界构造（probe_sorec 实测命中旧代码崩溃的参数）：长 CJK 行估高
    // 低估、doc 实排折行更高——估总高<视口高（滚动条隐藏）、实总高>视口高
    //（首条校正后滚动条显示），重排链中滚动条显隐翻转驱动宽度 ±10 震荡
    for (int i = 0; i < 6; ++i) {
        SBlockData st;
        st.eKind = CMessageBlock::Assistant;
        st.sText = QString(60, QChar(0x6D4B)) + QStringLiteral("\n\n")
                   + QString(60, QChar(0x8BD5)) + QStringLiteral("\n\n")
                   + QString(60, QChar(0x6587));
        st.sBackend = QStringLiteral("DSH");
        m.AppendBlock(st);
    }
    v.show();
    qApp->processEvents();
    // 多次宽度变化（真实场景：切助手/面板布局变化触发 resizeEvent）。
    // 旧代码在首个宽度变化即同步递归爆栈；修复后震荡在事件循环迭代间收敛。
    for (const int iW : {560, 620, 580, 600}) {
        v.resize(iW, 400);
        qApp->processEvents();
    }
    // 宽度收敛：重排基准应等于当下 viewport 宽（pending 延迟链全部消化）
    qApp->processEvents();
    QCOMPARE(v.width(), 600);
    QVERIFY(v.viewport()->width() > 0);
    QVERIFY(m.ItemCount() > 0);
}
void TestChatModel::ghostPageStaysLazy()
{
    // 载入即有数据的页（跨档幽灵/载档未预建）：Bind 不 Rebuild → Append 不增量投影
    //（旧守卫存在理由：模型空+数据 N 块，增量投影会造成下标错位/回填 miss）
    int iDirty = 0;
    CStreamPresenter st([&iDirty]() { ++iDirty; });
    CChatModel m;
    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    st.Bind(&m, &v);
    QList<SBlockData> aBlocks;
    aBlocks << mkAssistant(QStringLiteral("旧回复一"))
            << mkAssistant(QStringLiteral("旧回复二"))
            << mkAssistant(QStringLiteral("旧回复三"));
    st.SetBlocks(aBlocks);
    st.Append(mkAssistant(QStringLiteral("新块")));   // 未投影且有旧数据：只入数据
    QCOMPARE(st.Count(), 4);
    QCOMPARE(m.ItemCount(), 0);              // 模型保持空（惰性 Rebuild 语义不变）
    QVERIFY(!st.Projected());
    st.Rebuild();                            // 切到该页：全量对齐
    QCOMPARE(m.ItemCount(), 4);
    QVERIFY(st.Projected());
}

void TestChatModel::freshBindFirstAppendProjects()
{
    // 2026-10-23 实害修复回归：空数据建页（新建助手/新任务重置/重开空助手，
    // Bind 不 Rebuild）后首块到达——旧版只入数据不投影 → 激活页空白，
    // 单助手切日志页再切回也不触发自愈（sId==m_sActiveAsst 提前 return）
    // 只能干瞪眼。修复：模型必为空且数据仅此 1 块 → 投影下标天然对齐，
    // 置位进入持续同步态；后续块增量投影、回填命中。
    int iDirty = 0;
    CStreamPresenter st([&iDirty]() { ++iDirty; });
    CChatModel m;
    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    st.Bind(&m, &v);                        // 空数据建页（不 Rebuild）
    QVERIFY(!st.Projected());

    // 首块（用户提示词）→ 投影（旧版此处 m.ItemCount()==0 = 空白实害）
    SBlockData stUser;
    stUser.eKind = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    stUser.sText = QStringLiteral("[17:30:00] 帮我修复这个 bug");
    st.Append(stUser);
    QCOMPARE(m.ItemCount(), 1);             // 已投影可见
    QVERIFY(st.Projected());                // 进入持续同步态

    // 后续块：增量投影持续对齐（工具/回复/事件）
    st.Append(mkAssistant(QStringLiteral("好的，我来分析。")));
    st.Append(mkTool(QStringLiteral("read"), QStringLiteral("{\"file_path\":\"a.cpp\"}")));
    QCOMPARE(m.ItemCount(), 3);
    QCOMPARE(st.Count(), 3);

    // 回填命中（旧版未投影态 BackfillResult 静默 miss）
    const int iIdx = st.BackfillResult(QStringLiteral("ok"));
    QCOMPARE(iIdx, 2);
    QVERIFY(m.PeekItem(2)->aTools.at(0).bError == false);

    // 幂等：投影态下 Rebuild 仍全量对齐
    st.Rebuild();
    QCOMPARE(m.ItemCount(), 3);
}

// ---------------------------------------------------------------------------
// 9. 2026-10-23 焦点移出清选择（用户需求：焦点转移到其他控件 → 日志选择状态取消）。
//    右键菜单弹出（QMenu exec）会让视图收到 PopupFocusReason 的 FocusOut——豁免
//    不清，否则菜单里的「复制」无东西可复制（guard 的存在理由）
// ---------------------------------------------------------------------------
void TestChatModel::selectionClearsOnFocusOut()
{
    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral("选中转移测试正文")));
    m.AppendBlock(mkTool(QStringLiteral("read"), QStringLiteral("{}")));

    CChatListView v;
    v.resize(600, 400);
    v.SetModel(&m);
    v.show();
    v.setFocus();
    qApp->processEvents();
    QVERIFY(v.hasFocus());
    QItemSelectionModel* pSel = v.selectionModel();
    QVERIFY(pSel != nullptr);
    pSel->select(m.index(0, 0),
                 QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(!pSel->selectedRows().isEmpty());

    // ① 真实焦点转移到其他控件 → 选择取消（含当前索引）
    QWidget other;
    other.show();
    other.setFocus();
    qApp->processEvents();
    QVERIFY(pSel->selectedRows().isEmpty());
    QVERIFY(!v.currentIndex().isValid());

    // ② PopupFocusReason（右键菜单弹出）→ 选择保持：guard 不清
    v.setFocus();
    qApp->processEvents();
    pSel->select(m.index(1, 0),
                 QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(!pSel->selectedRows().isEmpty());
    QFocusEvent fePopup(QEvent::FocusOut, Qt::PopupFocusReason);
    QApplication::sendEvent(&v, &fePopup);
    QVERIFY(!pSel->selectedRows().isEmpty());

    // ③ 非 popup 焦点移出（等价点别的控件）→ 清
    QFocusEvent feOther(QEvent::FocusOut, Qt::MouseFocusReason);
    QApplication::sendEvent(&v, &feOther);
    QVERIFY(pSel->selectedRows().isEmpty());
}

QTEST_MAIN(TestChatModel)
#include "test_chatmodel.moc"
