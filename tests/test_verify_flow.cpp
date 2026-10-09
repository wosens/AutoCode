#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "core/StageDef.h"
#include "core/CStageMachine.h"
#include "core/TaskTypes.h"
#include "project/CGuideFingerprintScanner.h"
#include "project/CProjectManager.h"
#include "orch/CTaskOrchestrator.h"
#include "agent/CHermesRunner.h"
#include "agent/CAgentRunnerFactory.h"
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QTest>

// 本轮（be3fd900）新增/修改的纯逻辑单元测试。
// 覆盖范围：
//   AC2.1/AC2.4 —— Verify 阶段元数据完整、工具集不低于 Develop。
//   AC2.2/AC-N1 —— Verify 编入 Feature / Bugfix 两流程（CodeReview 后、Test 前）；既有 1~13 编号与流转不变。
//   AC1.2      —— 指纹 Diff 命中 新增 / 修改 / 删除 三类。
//   AC1.4      —— 无变化时 WriteIncremental 不改指纹表（逐字节保留未变行）。
//   AC1.5      —— ScanProject 排除隐藏目录（.git/.vs 等）+ .autodev（2026.08.26 新缺省）。
// 不覆盖：UI（FR3/FR4，依赖 GUI）、验证阶段实际执行（FR2.3，需跑 Claude + 真实工程编译）。
class CTestVerifyFlow : public QObject {
    Q_OBJECT

private:
    // 构造一个临时工程根（含 .autodev），返回其绝对路径。调用方负责清理。
    QString MakeSandbox() {
        const QString sRoot = QDir::tempPath() + QStringLiteral("/ac_verify_test_") +
            QString::number(reinterpret_cast<quintptr>(this));
        QDir().mkpath(sRoot);
        QDir().mkpath(sRoot + QStringLiteral("/.autodev"));
        return sRoot;
    }

    static void Wipe(const QString& a_sDir) {
        QDir(a_sDir).removeRecursively();
    }

    // 在工程根写一个文件（自动建父目录），返回正斜杠相对路径。
    static QString WriteFile(const QString& a_sRoot, const QString& a_sRel,
                             const QByteArray& a_sData) {
        const QString sAbs = a_sRoot + QLatin1Char('/') + a_sRel;
        QFileInfo(sAbs).dir().mkpath(QStringLiteral("."));
        QFile f(sAbs);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(a_sData);
        f.close();
        return a_sRel;
    }

    // 构造一段最小但合法的 guide.md（含指纹表 BEGIN..END 段），写入 <root>/.autodev/guide.md。
    // a_aFp 为指纹表行（已格式化或原文）。返回 guide 全文。
    static QString WriteGuideWithFp(const QString& a_sRoot,
                                    const QList<SFileFingerprint>& a_aFp) {
        const QString sGuidePath = a_sRoot + QStringLiteral("/.autodev/guide.md");
        QString sBody;
        sBody += QStringLiteral("# 工程指引\n\n一些描述文字。\n\n");
        sBody += CGuideFingerprintScanner::s_sBeginMarker + QStringLiteral("\n");
        sBody += QStringLiteral("```autocode-fp\n");
        sBody += QStringLiteral("# 相对路径 | 字节数 | 修改时间(UTC ms) | hash(可选)\n");
        for (const SFileFingerprint& rFp : a_aFp) {
            sBody += FmtFpLine(rFp) + QStringLiteral("\n");
        }
        sBody += QStringLiteral("```\n");
        sBody += CGuideFingerprintScanner::s_sEndMarker + QStringLiteral("\n");
        QFile f(sGuidePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(sBody.toUtf8());
        f.close();
        return sBody;
    }

    // 直接构造一个指纹（避免依赖文件系统 mtime 精度）。
    static SFileFingerprint MkFp(const QString& a_sPath, qint64 a_iSize, qint64 a_iMsUtc) {
        SFileFingerprint stFp;
        stFp.sRelPath = a_sPath;
        stFp.iSize    = a_iSize;
        stFp.dtMtime  = QDateTime::fromMSecsSinceEpoch(a_iMsUtc);
        return stFp;
    }

    // 与扫描器 FormatFpLine 同格式（私有不可访问，测试内自行复刻）：路径 | 字节数 | UTC ms | hash(-)
    static QString FmtFpLine(const SFileFingerprint& a_stFp) {
        const qint64 iMs = a_stFp.dtMtime.isValid()
            ? a_stFp.dtMtime.toUTC().toMSecsSinceEpoch() : 0;
        const QString sHash = a_stFp.sHash.isEmpty()
            ? QStringLiteral("-") : a_stFp.sHash;
        return QStringLiteral("%1 | %2 | %3 | %4")
            .arg(a_stFp.sRelPath).arg(a_stFp.iSize).arg(iMs).arg(sHash);
    }

private slots:
    // ============ 2026.09.23 v3.0.219 工作量评估（EstimateAdhocTurns 纯启发式） ============
    //   只升不降、封顶 200；四信号（编号子任务/点名文件/重活关键词/长度）叠加单调。
    void testEstimateAdhocTurns() {
        // ① 小任务：短聊天文本 → 恒等于基值（不下调）。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("这个函数是干什么的？"), 40), 40);
        // ② 基值已高：小任务不缩水（用户底线优先）。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("看下这个报错"), 150), 150);
        // ③ 多编号子任务：每多一个 +12（3 条 → 40+24=64）。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("1.修改标题\n2.调整布局\n3.补充日志"), 40), 64);
        // ④ 点名源文件：每个 +10、去重（两个不同文件 → 40+8(重构)+20=68；重复同名只计一次）。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("重构 LayoutModel.cpp 和 DesignerWindow.cpp"), 40), 68);
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("改 main.cpp，再测 main.cpp"), 40), 50);
        // ⑤ 重活关键词：重构+自测+构建+编译 = 4 个 → 40+8*4=72（封顶 4）。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("重构这套代码，自测通过并验证构建编译都绿"), 40), 72);
        // ⑥ 长文本：约 1200 字说明 → 40 + 6*((1196-200)/400 整除=2) = 52。
        {
            const QString sLong = QStringLiteral("详细说明：")
                + QString(QStringLiteral("字")).repeated(1190);
            QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(sLong, 40), 52);
        }
        // ⑦ 叠加封顶：重活组合不破 200；基值 200 恒 200。
        {
            const int iEst = CTaskOrchestrator::EstimateAdhocTurns(
                QStringLiteral("1.重构\n2.重写\n3.全部\n4.批量\n5.自测\n6.构建")
                + QStringLiteral(" a.cpp b.h c.py d.js e.qml"), 40);
            QVERIFY(iEst <= 200);
        }
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(
            QStringLiteral("1.重构\n2.重写"), 200), 200);
        // ⑧ 空文本 → 基值。
        QCOMPARE(CTaskOrchestrator::EstimateAdhocTurns(QString(), 40), 40);
    }

    // ============ 2026.09.26 专家助手深度模式纯函数（验证轮结论/验收标准提取） ============
    //   ParseDeepVerdict：1=PASS / 0=FAIL（出同行原因）/ -1=无判定行；FAIL 优先。
    //   ExtractDeepCriteria：「## 验收标准」段提取，缺失回退整份计划，恒截 2000。
    void testDeepVerdictParse() {
        // ① 标准末行 PASS（含前置证据正文）。
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("逐项核对通过。\ncmake --build ok\nVERIFY:PASS")), 1);
        // ② 标准 FAIL + 同行原因提取。
        QString sReasons;
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("VERIFY:FAIL: 1) main.cpp 未包含头文件; 2) 测试未跑"),
            &sReasons), 0);
        QVERIFY(sReasons.contains(QStringLiteral("main.cpp")));
        QVERIFY(sReasons.contains(QStringLiteral("测试未跑")));
        // ③ FAIL 优先：原因文本里出现 passed 字样不得误判 PASS。
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("VERIFY:FAIL: 部分用例 passed 但整体未达标")), 0);
        // ④ 大小写/冒号前后空白容错。
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("verify : pass")), 1);
        // ⑤ 中文冒号分隔的 FAIL 原因。
        QString sZh;
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("VERIFY:FAIL：编译错误 2 个"), &sZh), 0);
        QVERIFY(sZh.contains(QStringLiteral("编译错误")));
        // ⑥ 无判定行 / 空文本 → -1。
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(
            QStringLiteral("全部通过，无问题")), -1);
        QCOMPARE(CTaskOrchestrator::ParseDeepVerdict(QString()), -1);
    }

    void testDeepCriteriaExtract() {
        const QString sPlan = QStringLiteral(
            "## 目标\n修复标题栏。\n\n## 实施步骤\n1. 改 CTitleBar.cpp\n\n"
            "## 验收标准\n1. 编译零错误\n2. 标题显示工程名\n\n## 其他\n占位");
        const QString sCrit = CTaskOrchestrator::ExtractDeepCriteria(sPlan);
        QVERIFY(sCrit.contains(QStringLiteral("编译零错误")));
        QVERIFY(sCrit.contains(QStringLiteral("标题显示工程名")));
        QVERIFY(!sCrit.contains(QStringLiteral("CTitleBar")));   // 不含实施步骤段
        QVERIFY(!sCrit.contains(QStringLiteral("占位")));        // 不含后续标题段
        // 缺验收标准段 → 回退整份计划。
        QVERIFY(CTaskOrchestrator::ExtractDeepCriteria(
            QStringLiteral("## 目标\n只写目标")).contains(QStringLiteral("只写目标")));
        // 空规划 → 占位提示（验证轮仍有可用输入）。
        QVERIFY(!CTaskOrchestrator::ExtractDeepCriteria(QString()).isEmpty());
        // 超长恒截 2000。
        const QString sLong = QStringLiteral("## 验收标准\n1. ")
            + QString(QStringLiteral("x")).repeated(5000);
        QVERIFY(CTaskOrchestrator::ExtractDeepCriteria(sLong).size() <= 2000);
    }

    // ============ 2026.10.02 专家模式查询跳过 + 辅助轮输出压缩 ============
    void testDeepQueryPromptHeuristic() {
        // ① 查询类（问句/查询动词）→ true：跳过深度链。
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("这个文件是干什么的？")));
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("帮我看看为什么编译不过")));
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("XX 和 YY 的区别是什么")));
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("分析一下这个函数的作用")));
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("目前支持哪些模型")));
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("解释一下这个配置项的用法")));
        // ② 改码类（含改码动词）→ false：进深度链。
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("修复登录页面的按钮")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("给配置增加一个开关")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("创建一个新的对话框")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("把这个函数优化一下")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("重构文件读取模块")));
        // ②b 名词化不误伤：「配置项的用法」是查询（配置不入改码表——作动词的
        //   「帮我配置模型」无查询信号仍正确判改码）。
        QVERIFY(CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("解释一下这个配置项的用法")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("帮我配置模型路由")));
        // ③ 混合句改码动词优先 → false。
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("解释一下然后修复这个 bug")));
        // ④ 无任何信号的中性请求/空文本 → false（保守走深度链，宁多验不漏验）。
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(
            QStringLiteral("处理这个任务")));
        QVERIFY(!CTaskOrchestrator::IsAdhocQueryPrompt(QString()));
    }

    void testDeepAuxTextCompress() {
        // ① 短文本原样返回（含首尾空白归一）。
        QCOMPARE(CTaskOrchestrator::CompressDeepAuxText(
            QStringLiteral("  短文本  ")), QStringLiteral("短文本"));
        // ② 临界值：360 字原样。
        const QString sEdge = QString(QStringLiteral("a")).repeated(360);
        QCOMPARE(CTaskOrchestrator::CompressDeepAuxText(sEdge), sEdge);
        // ③ 超长：保首 300 字 + 折叠尾注（含原始字数），总长有界。
        const QString sLong = QString(QStringLiteral("L")).repeated(5000);
        const QString sOut = CTaskOrchestrator::CompressDeepAuxText(sLong);
        QVERIFY(sOut.startsWith(QString(QStringLiteral("L")).repeated(300)));
        QVERIFY(sOut.contains(QStringLiteral("已折叠 5000 字")));
        QVERIFY(sOut.size() < 400);
        // ④ 空文本归空。
        QVERIFY(CTaskOrchestrator::CompressDeepAuxText(
            QStringLiteral("   ")).isEmpty());
    }

    // ============ 2026.09.20 助手粘性后端（v2.4.193，取代 v2.4.183 清链语义） ============
    //   会话绑定后端后，全局切换不再清链：LoadAsstAdhoc 绑 "pi" → 全局切 hermes(stub)
    //   → SubmitAdhoc 仍走 pi runner（配置缺失失败，错误含 PIAgent；stub 日志零调用）；
    //   未绑定会话则绑当前全局。AsstAdhocBackend 随 LoadAsstAdhoc 记录。
    void testBackendSwitchStickyBinding() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        QDir().mkpath(oTmp.path() + QStringLiteral("/.autodev"));
        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(oTmp.path()));
        CTaskOrchestrator stOrch(&stPm);

        // 绑定 "pi" + sid/撤销栈（模拟 pi 时代用过的助手）。
        QJsonArray aStack;
        {
            QJsonObject o1;
            o1[QStringLiteral("prompt")] = QStringLiteral("pi 时代的提问");
            o1[QStringLiteral("sid")] = QStringLiteral("pi-session-0001");
            aStack.append(o1);
        }
        stOrch.LoadAsstAdhoc(QStringLiteral("asst-a"),
                              QStringLiteral("pi-session-0001"),
                              aStack, QStringLiteral("hash-a"),
                              QStringLiteral("pi"));
        QCOMPARE(stOrch.AsstAdhocBackend(QStringLiteral("asst-a")), QStringLiteral("pi"));

        // 全局切到 hermes（stub 已配，pi 配置缺失）→ 提交仍走 pi。
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        int iFinished = 0;
        QStringList aEvents;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocFinished,
                         [&](const QString&) { ++iFinished; });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocEvent,
                         [&](const QString&, const QString& k, const QString& t) {
                             aEvents << k + QStringLiteral(":") + t;
                         });
        stOrch.SubmitAdhoc(QStringLiteral("asst-a"), QStringLiteral("hello"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        // pi runner 同步前置校验失败 → EmitFailed 直通（无需事件循环等待）。
        QVERIFY2(aEvents.join(QStringLiteral("|"))
                     .contains(QStringLiteral("PIAgent")),
                 qPrintable(QStringLiteral("粘性后端应由 pi runner 报配置错误，实际事件：%1")
                                .arg(aEvents.join(QStringLiteral("|")).left(200))));
        // 会话链未被清除（粘性：sid/撤销栈保留，等待回到 pi 环境后继续）。
        QCOMPARE(stOrch.AsstAdhocBackend(QStringLiteral("asst-a")), QStringLiteral("pi"));
        // 未绑定的新会话：绑当前全局（hermes）。
        QCOMPARE(stOrch.AsstAdhocBackend(QStringLiteral("asst-new")), QString());
        stOrch.LoadAsstAdhoc(QStringLiteral("asst-new"), QString(), QJsonArray(), QString());
        QCOMPARE(stOrch.AsstAdhocBackend(QStringLiteral("asst-new")), QString());

        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
    }

    // ============ 2026.09.20 多条排队指令一次性合并投送 ============
    //   stub hermes CLI：读 stdin（prompt）→ 追加到日志文件 → 吐 session/assistant/
    //   result(success) NDJSON。hermes 后端 InjectUserTurn 恒 false，运行中发送的
    //   提示词全部进队列；轮 1 成功收尾时队列应整体合并为一次 SubmitAdhoc——
    //   断言：总共恰好 2 次进程调用（1 首轮 + 1 合并轮，旧版为 1+N 次）；第二次
    //   收到的 prompt 含全部【指令 N】定界；sigAdhocPendingStarted 每条各发一次。
    void queuedPromptsSingleSubmission() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 合并投送用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/stub_calls.log");
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            // stdin 收完先记日志，延时 1.5s 再吐成功事件（保证测试的排队窗口内轮确实
            //   在跑——不延时时 stub 秒回，qWait(400) 后轮已收尾，提示词走了新轮路径而不是入队）。
            fCli.write(QStringLiteral(
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%1,'===\\n'+s+'\\n');"
                "setTimeout(()=>{"
                "console.log(JSON.stringify({type:'session',sessionId:'stub-sid-1'}));"
                "console.log(JSON.stringify({type:'assistant',text:'stub 完成',"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:'stub-sid-1',text:'stub 完成'}));"
                "},1500);"
                "});")
                .arg(QStringLiteral("'") + sLog + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));

        // 合并封装格式（纯函数）：逐条定界 + 顺序全部执行要求。
        const QString sMerged = CTaskOrchestrator::MergeQueuedPrompts(
            { QStringLiteral("第二条：修复登录页"), QStringLiteral("第三条：补充单元测试") });
        QVERIFY(sMerged.contains(QStringLiteral("2 条排队指令")));
        QVERIFY(sMerged.contains(QStringLiteral("【指令 1】\n第二条：修复登录页")));
        QVERIFY(sMerged.contains(QStringLiteral("【指令 2】\n第三条：补充单元测试")));

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        // 2026.09.26 锁定纯对话模式：本用例验证排队合并语义，深度链（规划/验证
        //   额外投递）会干扰投递次数断言；结束后复位 -1（按 config）。
        CTaskOrchestrator::SetAdhocDeepOverride(0);
        int iFinished = 0;
        QStringList aPendingStarted;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocFinished,
                         [&](const QString&) { ++iFinished; });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocPendingStarted,
                         [&](const QString&, const QString& s) { aPendingStarted << s; });

        // 首轮起跑后立刻排队两条（进程在跑 → InjectUserTurn=false → 入队）。
        stOrch.SubmitAdhoc(QStringLiteral("asst-q"), QStringLiteral("第一条：跑通构建"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        QTest::qWait(400);   // 令 stub 进程进入 Running（stdin 未关，轮未结束）
        stOrch.SubmitAdhoc(QStringLiteral("asst-q"), QStringLiteral("第二条：修复登录页"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        stOrch.SubmitAdhoc(QStringLiteral("asst-q"), QStringLiteral("第三条：补充单元测试"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        // 等全程收敛（首轮 + 合并轮，最终恰好一次 sigAdhocFinished）。
        for (int iWait = 0; iWait < 600 && iFinished < 1; ++iWait) {
            QTest::qWait(100);
        }
        QCOMPARE(iFinished, 1);

        // 每条排队指令各转正一次。
        QCOMPARE(aPendingStarted.size(), 2);
        QVERIFY(aPendingStarted.contains(QStringLiteral("第二条：修复登录页")));
        QVERIFY(aPendingStarted.contains(QStringLiteral("第三条：补充单元测试")));

        // stub 恰好被调 2 次（旧版 = 1+N）；第二次收到的是合并后的单次投送。
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const QString sCalls = QString::fromUtf8(fLog.readAll());
        fLog.close();
        QCOMPARE(sCalls.count(QStringLiteral("===")), 2);
        const int iSecond = sCalls.lastIndexOf(QStringLiteral("==="));
        const QString sSecondCall = sCalls.mid(iSecond);
        QVERIFY(sSecondCall.contains(QStringLiteral("【指令 1】")));
        QVERIFY(sSecondCall.contains(QStringLiteral("第二条：修复登录页")));
        QVERIFY(sSecondCall.contains(QStringLiteral("【指令 2】")));
        QVERIFY(sSecondCall.contains(QStringLiteral("第三条：补充单元测试")));
        // 注：第二次调用中可能出现首轮文本（跨轮上下文回灌的历史记录，非独立投送）；
        // 「一次性」由恰 2 次调用 + 全部【指令 N】同投一次共同判定。

        // 静态配置复位（不泄漏给同进程后续用例）。
        CTaskOrchestrator::SetAdhocDeepOverride(-1);
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }

    // ============ 2026.09.20 排队指令删除/编辑（编排器侧端到端） ============
    //   stub hermes CLI 运行中入队两条 → RemoveQueuedAdhoc 删一条 + EditQueuedAdhoc
    //   改一条 → 轮 1 结束后续发的就是「删除已生效 + 改写后文本」（单条直投）。
    void queuedPromptsRemoveEdit() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 删除/编辑用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/stub_calls.log");
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            fCli.write(QStringLiteral(
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%1,'===\\n'+s+'\\n');"
                "setTimeout(()=>{"
                "console.log(JSON.stringify({type:'session',sessionId:'stub-sid-1'}));"
                "console.log(JSON.stringify({type:'assistant',text:'stub 完成',"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:'stub-sid-1',text:'stub 完成'}));"
                "},1500);"
                "});")
                .arg(QStringLiteral("'") + sLog + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        // 2026.09.26 同 queuedPromptsSingleSubmission：锁定纯对话模式验证队列编辑语义。
        CTaskOrchestrator::SetAdhocDeepOverride(0);
        int iFinished = 0;
        QStringList aPendingStarted;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocFinished,
                         [&](const QString&) { ++iFinished; });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocPendingStarted,
                         [&](const QString&, const QString& s) { aPendingStarted << s; });

        const QString sQ2 = QStringLiteral("第二批：重构配置模块");
        const QString sQ3 = QStringLiteral("第三批：更新文档");
        const QString sQ3New = QStringLiteral("第三批（改）：更新文档并补充示例");
        stOrch.SubmitAdhoc(QStringLiteral("asst-re"), QStringLiteral("第一批：跑通构建"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        QTest::qWait(400);   // 首轮进程进入 Running（stub 延时 1.5s 才回包）
        stOrch.SubmitAdhoc(QStringLiteral("asst-re"), sQ2,
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        stOrch.SubmitAdhoc(QStringLiteral("asst-re"), sQ3,
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        // 队列内删除/改写（未发送）；未命中与空白改写拒绝。
        QVERIFY(stOrch.RemoveQueuedAdhoc(QStringLiteral("asst-re"), sQ2));
        QVERIFY(stOrch.EditQueuedAdhoc(QStringLiteral("asst-re"), sQ3, sQ3New));
        QVERIFY(!stOrch.RemoveQueuedAdhoc(QStringLiteral("asst-re"),
                                          QStringLiteral("不存在的指令")));
        QVERIFY(!stOrch.EditQueuedAdhoc(QStringLiteral("asst-re"), sQ3New,
                                        QStringLiteral("  ")));

        for (int iWait = 0; iWait < 600 && iFinished < 1; ++iWait) {
            QTest::qWait(100);
        }
        QCOMPARE(iFinished, 1);

        // 续发只转正了改写后的那一条。
        QCOMPARE(aPendingStarted.size(), 1);
        QCOMPARE(aPendingStarted.at(0), sQ3New);

        // stub 恰 2 次调用；第二次（续发）含改写后文本、不含被删与改前文本。
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const QString sCalls = QString::fromUtf8(fLog.readAll());
        fLog.close();
        QCOMPARE(sCalls.count(QStringLiteral("===")), 2);
        const int iSecond = sCalls.lastIndexOf(QStringLiteral("==="));
        const QString sSecondCall = sCalls.mid(iSecond);
        QVERIFY(sSecondCall.contains(sQ3New));
        QVERIFY(!sSecondCall.contains(sQ3));
        QVERIFY(!sSecondCall.contains(sQ2));

        CTaskOrchestrator::SetAdhocDeepOverride(-1);
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }

    // ============ 2026.09.26 专家助手深度模式端到端（stub hermes 三轮闭环） ============
//   桩 CLI 按提示词特征分流：规划轮→含验收标准的计划；执行轮→改码（带 tool 事件
//   触发 bTurnSawTool）；验证轮→VERIFY:PASS。断言事件链 deep_plan→deep_plan_done→
//   deep_verify→deep_verify_pass→done、恰 3 次进程投递、无回炉/重启/跳验。
    void deepModeHappyPath() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过深度链 stub 用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/stub_calls.log");
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            fCli.write(QStringLiteral(
                "process.on('uncaughtException',e=>{"
                "require('fs').writeFileSync('%1',''+e.stack);process.exit(99);});"
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%2,'===\\n'+s+'\\n');"
                "const sid='stub-'+require('crypto').randomBytes(3).toString('hex');"
                "let text='已完成';let withTool=false;"
                "if(s.includes('专家模式·验证轮')){text='核验证据：编译通过、用例全绿\\nVERIFY:PASS';}"
                "else if(s.includes('专家模式·规划轮')){text='## 目标\\n演示目标\\n"
                "## 实施步骤\\n1. 改 a.cpp\\n## 验收标准\\n1. 编译零错误\\n2. 行为符合预期';}"
                "else{withTool=true;}"
                "setTimeout(()=>{"
                "if(withTool){console.log(JSON.stringify({type:'tool',tool:'Edit',"
                "input:{file:'a.cpp'}}));}"
                "console.log(JSON.stringify({type:'session',sessionId:sid}));"
                "console.log(JSON.stringify({type:'assistant',text:text,"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:sid,text:text}));"
                "},300);"
                "});")
                .arg(oTmp.path() + QStringLiteral("/stub_err.log"),
                     QStringLiteral("'") + sLog + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        CTaskOrchestrator::SetAdhocDeepOverride(1);   // 强制深度链（不依赖机器 config）
        int iFinished = 0;
        QStringList aKinds;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocFinished,
                         [&](const QString&) { ++iFinished; });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocEvent,
                         [&](const QString&, const QString& k, const QString&) {
                             aKinds << k;
                         });
        stOrch.SubmitAdhoc(QStringLiteral("asst-deep"), QStringLiteral("给 a.cpp 加个入口函数"),
                           CTaskOrchestrator::PermAuto, QString(), nullptr);
        for (int iWait = 0; iWait < 300 && iFinished < 1; ++iWait) {
            QTest::qWait(100);
        }
        QCOMPARE(iFinished, 1);
        // 事件链：规划→规划完成→验证→验证通过→done；不得出现回炉/重启/失败/跳验。
        QVERIFY(aKinds.contains(QStringLiteral("deep_plan")));
        QVERIFY(aKinds.contains(QStringLiteral("deep_plan_done")));
        QVERIFY(aKinds.contains(QStringLiteral("deep_verify")));
        QVERIFY(aKinds.contains(QStringLiteral("deep_verify_pass")));
        QVERIFY(aKinds.contains(QStringLiteral("done")));
        QVERIFY(!aKinds.contains(QStringLiteral("deep_retry")));
        QVERIFY(!aKinds.contains(QStringLiteral("deep_restart")));
        QVERIFY(!aKinds.contains(QStringLiteral("deep_fail")));
        QVERIFY(!aKinds.contains(QStringLiteral("deep_skip_verify")));
        QVERIFY(!aKinds.contains(QStringLiteral("stage_failed")));
        // 首事件：deep_plan（工作量评估上调轮次预算时其前会多一条 turns_budget）。
        QVERIFY(aKinds.first() == QStringLiteral("deep_plan")
                || (aKinds.first() == QStringLiteral("turns_budget")
                    && aKinds.value(1) == QStringLiteral("deep_plan")));
        QCOMPARE(aKinds.last(), QStringLiteral("done"));
        // 恰 3 次进程投递：规划 + 执行 + 验证。
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const QString sCalls = QString::fromUtf8(fLog.readAll());
        fLog.close();
        QCOMPARE(sCalls.count(QStringLiteral("===")), 3);
        // 规划产物已传递给执行轮：第二次投递的提示词含「验收标准」。
        const int iFirst = sCalls.indexOf(QStringLiteral("==="));
        const int iSecondCall = sCalls.indexOf(QStringLiteral("==="), iFirst + 3);
        const int iThird = sCalls.indexOf(QStringLiteral("==="), iSecondCall + 3);
        QVERIFY(iThird > iSecondCall);
        QVERIFY(sCalls.mid(iSecondCall, iThird - iSecondCall)
                    .contains(QStringLiteral("验收标准")));

        CTaskOrchestrator::SetAdhocDeepOverride(-1);
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }

    // ============ AC2.1 / AC2.4：Verify 阶段定义 ============

    // Verify 阶段存在于定义表，枚举值=14（不动 1~13）。
    void testVerifyStageEnumAndDef() {
        QCOMPARE(int(EStage::Verify), 14);
        const SStageDef* pDef = FindStage(EStage::Verify);
        QVERIFY2(pDef != nullptr, "Verify stage missing from GetStageDefs()");
        QCOMPARE(pDef->sId, QStringLiteral("verify"));
        QCOMPARE(pDef->sNameZh, QStringLiteral("验证"));
        QCOMPARE(pDef->sPromptFile, QStringLiteral("14_verify.md"));
        QCOMPARE(pDef->sOutput, QStringLiteral("verify.md"));
        QVERIFY(!pDef->sOutput.isEmpty());
    }

    // FindStageById("verify") 可命中。
    void testFindStageByIdVerify() {
        const SStageDef* pDef = FindStageById(QStringLiteral("verify"));
        QVERIFY(pDef != nullptr);
        QCOMPARE(pDef->eStage, EStage::Verify);
    }

    // 既有 1~13 阶段编号与 id 全部保持（NFR1：不破坏既有阶段）。
    void testLegacyStagesUnchanged() {
        QCOMPARE(int(EStage::Intake), 1);
        QCOMPARE(int(EStage::ConfirmReq), 2);
        QCOMPARE(int(EStage::ReviewReq), 3);
        QCOMPARE(int(EStage::Design), 4);
        QCOMPARE(int(EStage::ReviewDesign), 5);
        QCOMPARE(int(EStage::Plan), 6);
        QCOMPARE(int(EStage::ReviewPlan), 7);
        QCOMPARE(int(EStage::Develop), 8);
        QCOMPARE(int(EStage::CodeReview), 9);
        QCOMPARE(int(EStage::Test), 10);
        QCOMPARE(int(EStage::RootCause), 11);
        QCOMPARE(int(EStage::RootCauseVerify), 12);
        QCOMPARE(int(EStage::BugFix), 13);
        // 定义表至少 14 行。
        QVERIFY(GetStageDefs().size() >= 14);
    }

    // AC2.4：Verify 工具集 deny 名单不低于 Develop（Develop deny=""，Verify 也应为空，不禁基础工具）。
    void testVerifyToolsNotBelowDev() {
        const QString sDev    = ToolsDev();
        const QString sVerify = ToolsVerify();
        // Develop 禁用工具集为空串；Verify 必须 <= Develop 权限收紧度，即 Verify 也禁用更少或同等。
        // 这里语义：Verify 的 deny 列表不应比 Develop 多禁任何基础工具（Edit/Bash 等）。
        QVERIFY2(!sVerify.contains(QLatin1String("Edit")),
                 "Verify must allow Edit (insert/cleanup debug code)");
        QVERIFY2(!sVerify.contains(QLatin1String("Bash")),
                 "Verify must allow Bash (compile/run)");
        Q_UNUSED(sDev);
    }

    // ============ AC2.2 / AC-N1：Verify 编入流程 ============

    // Feature 流：Verify 在 CodeReview 后、Test 前。
    void testVerifyInFeatureFlow() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Feature);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify not in Feature flow");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must sit between CodeReview and Test");
        QCOMPARE(CStageMachine::NextStage(ETaskType::Feature, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Verify);
        QCOMPARE(CStageMachine::NextStage(ETaskType::Feature, EStage::Verify).value_or(EStage::Intake),
                 EStage::Test);
        QVERIFY(CStageMachine::StageInFlow(ETaskType::Feature, EStage::Verify));
    }

    // Bugfix 流：Verify 在 CodeReview 后、Test 前（同上）。
    void testVerifyInBugfixFlow() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Bugfix);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify not in Bugfix flow");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must sit between CodeReview and Test");
        QCOMPARE(CStageMachine::NextStage(ETaskType::Bugfix, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Verify);
        QCOMPARE(CStageMachine::NextStage(ETaskType::Bugfix, EStage::Verify).value_or(EStage::Intake),
                 EStage::Test);
    }

    // AC-N1：Verify 不在 RejectTarget 路由表内 → 走 default 返回自身（不被当成评审驳回源）。
    void testVerifyNotInRejectTarget() {
        QCOMPARE(CStageMachine::RejectTarget(EStage::Verify), EStage::Verify);
        // 既有驳回路由不变。
        QCOMPARE(CStageMachine::RejectTarget(EStage::CodeReview), EStage::Develop);
        QCOMPARE(CStageMachine::RejectTarget(EStage::ReviewReq), EStage::ConfirmReq);
        QCOMPARE(CStageMachine::RejectTarget(EStage::Test), EStage::Develop);
    }

    // Feature 流首阶段仍为 Intake（既有行为不变）。
    void testFirstStageUnchanged() {
        QCOMPARE(CStageMachine::FirstStage(ETaskType::Feature), EStage::Intake);
        QCOMPARE(CStageMachine::FirstStage(ETaskType::Bugfix), EStage::Intake);
    }

    // ============ AC1.2：指纹 Diff 三类变化 ============

    // 构造 prev/cur 纯内存指纹列表，Diff 应分别命中 新增/修改/删除。
    void testDiffThreeClasses() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)
              << MkFp(QStringLiteral("src/b.cpp"), 200, 2000000)
              << MkFp(QStringLiteral("src/c.cpp"), 300, 3000000);

        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)   // 不变
             << MkFp(QStringLiteral("src/b.cpp"), 999, 2000000)   // size 变 → 修改
             << MkFp(QStringLiteral("src/d.cpp"), 400, 4000000);  // 新增（c 被删）

        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QCOMPARE(stCh.aAdded.size(), 1);
        QCOMPARE(stCh.aAdded.first().sRelPath, QStringLiteral("src/d.cpp"));
        QCOMPARE(stCh.aModified.size(), 1);
        QCOMPARE(stCh.aModified.first().sRelPath, QStringLiteral("src/b.cpp"));
        QCOMPARE(stCh.aDeleted.size(), 1);
        QCOMPARE(stCh.aDeleted.first(), QStringLiteral("src/c.cpp"));
    }

    // mtime 变（size 不变）也应判为修改。
    void testDiffMtimeChangeIsModified() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("x.h"), 50, 1111111);
        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("x.h"), 50, 9999999);
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QCOMPARE(stCh.aModified.size(), 1);
        QVERIFY(stCh.aAdded.isEmpty());
        QVERIFY(stCh.aDeleted.isEmpty());
    }

    // 完全一致 → 三类全空。
    void testDiffNoChange() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("a"), 1, 1) << MkFp(QStringLiteral("b"), 2, 2);
        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("a"), 1, 1) << MkFp(QStringLiteral("b"), 2, 2);
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QVERIFY(stCh.aAdded.isEmpty());
        QVERIFY(stCh.aModified.isEmpty());
        QVERIFY(stCh.aDeleted.isEmpty());
    }

    // ============ AC1.2：端到端 Scan → Parse → Diff（真文件系统）============

    void testScanParseDiffEndToEnd() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("main.cpp"), "int main(){return 0;}");
        WriteFile(sRoot, QStringLiteral("lib/util.h"), "#pragma once\n");

        // 用扫描结果回填 guide 指纹表（模拟上次记录的状态）。
        QList<SFileFingerprint> aPrev = CGuideFingerprintScanner::ScanProject(sRoot);
        QCOMPARE(aPrev.size(), 2);
        WriteGuideWithFp(sRoot, aPrev);

        // 制造变化：新增 add.cpp、修改 main.cpp、删除 util.h。
        QFile::remove(sRoot + QStringLiteral("/lib/util.h"));
        WriteFile(sRoot, QStringLiteral("main.cpp"), "int main(){return 1;} // changed");
        WriteFile(sRoot, QStringLiteral("add.cpp"), "int extra;");

        QList<SFileFingerprint> aCur = CGuideFingerprintScanner::ScanProject(sRoot);
        QList<SFileFingerprint> aPrevParsed = CGuideFingerprintScanner::ParseFingerprintTable(
            [&]() { QFile f(sRoot + QStringLiteral("/.autodev/guide.md")); f.open(QIODevice::ReadOnly); QString s = QTextStream(&f).readAll(); return s; }());
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrevParsed, aCur);

        QVERIFY2(stCh.aAdded.size() >= 1, "add.cpp should be detected as added");
        QVERIFY2(!stCh.aDeleted.isEmpty(), "util.h should be detected as deleted");
        QVERIFY2(stCh.aDeleted.contains(QStringLiteral("lib/util.h")), "util.h missing from deleted");
        QVERIFY2(!stCh.aModified.isEmpty(), "main.cpp should be detected as modified");
        Wipe(sRoot);
    }

    // ============ AC1.5（2026.08.26 修订）：只排隐藏目录（含 .git/.vs）+ .autodev ============

    void testScanExcludesBuildDirs() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("real.cpp"), "x");
        // 隐藏目录（.git/.vs/任意 '.' 开头）与 .autodev 不应计入。
        WriteFile(sRoot, QStringLiteral("build/obj.o"), "build artifact");
        WriteFile(sRoot, QStringLiteral("Execute/foo.exe"), "exe");
        WriteFile(sRoot, QStringLiteral(".autodev/guide.md"), "g");
        WriteFile(sRoot, QStringLiteral("doc/readme.txt"), "doc");
        WriteFile(sRoot, QStringLiteral(".git/config"), "git");
        WriteFile(sRoot, QStringLiteral(".vs/sln.docstate"), "vs");
        WriteFile(sRoot, QStringLiteral(".hidden/deep/x.cpp"), "hidden dir");
        WriteFile(sRoot, QStringLiteral("sub/.hiddendir/y.cpp"), "nested hidden dir");

        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        QStringList aPaths;
        for (const SFileFingerprint& r : aFp) {
            aPaths << r.sRelPath;
        }
        QVERIFY2(aPaths.contains(QStringLiteral("real.cpp")), "real.cpp must be scanned");
        // 2026.08.26 新语义：build/Execute/doc 不再缺省排除（用户需求：除隐藏外都不排除）。
        QVERIFY2(aPaths.contains(QStringLiteral("build/obj.o")), "build/ must be scanned (new default)");
        QVERIFY2(aPaths.contains(QStringLiteral("Execute/foo.exe")), "Execute/ must be scanned (new default)");
        QVERIFY2(aPaths.contains(QStringLiteral("doc/readme.txt")), "doc/ must be scanned (new default)");
        // 2026.08.26 二次修正：.autodev 也不排除（用户明确要求），扫描层照常入表。
        QVERIFY2(aPaths.contains(QStringLiteral(".autodev/guide.md")),
                 ".autodev/ must be scanned (user request); only guide.md is filtered at Diff level");
        for (const QString& s : aPaths) {
            QVERIFY2(!s.startsWith(QStringLiteral(".git/")),
                     qPrintable(QStringLiteral(".git/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".vs/")),
                     qPrintable(QStringLiteral(".vs/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".hidden/")),
                     qPrintable(QStringLiteral(".hidden/ must be excluded, got: ") + s));
            QVERIFY2(!s.contains(QStringLiteral("/.hiddendir/")),
                     qPrintable(QStringLiteral("nested hidden dir must be excluded, got: ") + s));
        }
        Wipe(sRoot);
    }

    // 2026.08.26：.autodev 入扫描，但指纹表自身 guide.md 不参与 Diff（防回写自我变更死循环）。
    void testDiffIgnoresGuideSelf() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)
              << MkFp(QStringLiteral(".autodev/guide.md"), 500, 1000000)
              << MkFp(QStringLiteral(".autodev/record.md"), 50, 1000000);
        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)          // 不变
             << MkFp(QStringLiteral(".autodev/guide.md"), 999, 9999999)  // 自身变化 → 应被过滤
             << MkFp(QStringLiteral(".autodev/record.md"), 80, 2000000); // 正常跟踪 → 修改
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QVERIFY(stCh.aAdded.isEmpty());
        QVERIFY(stCh.aDeleted.isEmpty());
        QCOMPARE(stCh.aModified.size(), 1);
        QCOMPARE(stCh.aModified.first().sRelPath, QStringLiteral(".autodev/record.md"));
    }

    // AC1.5 细节：同前缀名不应误伤（buildfoo 不等于 build）。
    void testScanExcludesDoNotOvermatch() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("buildutil/x.cpp"), "x");   // build+util，非 build/
        WriteFile(sRoot, QStringLiteral("buildfoo/y.cpp"), "y");    // build+foo
        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        QStringList aPaths;
        for (const SFileFingerprint& r : aFp) {
            aPaths << r.sRelPath;
        }
        QVERIFY2(aPaths.contains(QStringLiteral("buildutil/x.cpp")),
                 "buildutil/ must NOT be mistaken for build/");
        QVERIFY2(aPaths.contains(QStringLiteral("buildfoo/y.cpp")),
                 "buildfoo/ must NOT be mistaken for build/");
        Wipe(sRoot);
    }

    // ============ AC1.4：无变化 → WriteIncremental 不改指纹表（逐字节保留）============

    void testWriteIncrementalNoChangePreservesTable() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("a.cpp"), "aaaa");
        WriteFile(sRoot, QStringLiteral("b.cpp"), "bbbb");

        QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        const QString sGuideBefore = WriteGuideWithFp(sRoot, aFp);

        // 重新扫描（无任何文件变化）→ Diff 全空 → WriteIncremental 应保持指纹表字节不变。
        QList<SFileFingerprint> aCur = CGuideFingerprintScanner::ScanProject(sRoot);
        SGuideChangeSet stEmpty;   // 三类全空
        const QString sGuidePath = sRoot + QStringLiteral("/.autodev/guide.md");
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(sGuidePath, aCur, stEmpty,
                                                                    QStringLiteral("2026-08-13T00:00:00"));
        QVERIFY2(bOk, "WriteIncremental should succeed on a valid guide with table");

        QFile f2(sGuidePath);
        f2.open(QIODevice::ReadOnly | QIODevice::Text);
        const QString sGuideAfter = QTextStream(&f2).readAll();

        // 指纹表段（BEGIN..END）必须与写前逐字节一致。
        auto ExtractTable = [](const QString& a_s) {
            const int iB = a_s.indexOf(CGuideFingerprintScanner::s_sBeginMarker);
            const int iE = a_s.indexOf(CGuideFingerprintScanner::s_sEndMarker);
            if (iB < 0 || iE <= iB) {
                return QString();
            }
            return a_s.mid(iB, (iE + CGuideFingerprintScanner::s_sEndMarker.length()) - iB);
        };
        QCOMPARE(ExtractTable(sGuideBefore), ExtractTable(sGuideAfter));
        Wipe(sRoot);
    }

    // ============ 健壮性：异常输入安全返回 ============

    // guide 不存在 → WriteIncremental 返回 false（调用方走全量）。
    void testWriteIncrementalMissingFile() {
        SGuideChangeSet stCh;
        stCh.aAdded << MkFp(QStringLiteral("z.cpp"), 1, 1);
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(
            QDir::tempPath() + QStringLiteral("/ac_verify_nonexistent_") +
                QString::number(reinterpret_cast<quintptr>(this)) + QStringLiteral("/guide.md"),
            {}, stCh, QStringLiteral("ts"));
        QVERIFY2(!bOk, "WriteIncremental on missing guide must return false");
    }

    // guide 存在但无 BEGIN..END 段 → 返回 false。
    void testWriteIncrementalNoTableReturnsFalse() {
        const QString sRoot = MakeSandbox();
        const QString sGuidePath = sRoot + QStringLiteral("/.autodev/guide.md");
        QFile f(sGuidePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("# 工程指引\n\n没有指纹表的 guide。\n");
        f.close();

        SGuideChangeSet stCh;
        stCh.aAdded << MkFp(QStringLiteral("z.cpp"), 1, 1);
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(sGuidePath, {}, stCh, QStringLiteral("ts"));
        QVERIFY2(!bOk, "WriteIncremental on guide without table must return false");
        Wipe(sRoot);
    }

    // ParseFingerprintTable：空串 / 无标记 → 空表（不抛）。
    void testParseEmptyAndMissing() {
        QVERIFY(CGuideFingerprintScanner::ParseFingerprintTable(QString()).isEmpty());
        QVERIFY(CGuideFingerprintScanner::ParseFingerprintTable(
            QStringLiteral("no markers here")).isEmpty());
    }

    // ScanProject：目录不存在 → 空表（不抛）。
    void testScanMissingDirEmpty() {
        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(
            QDir::tempPath() + QStringLiteral("/ac_verify_definitely_missing_") +
            QString::number(reinterpret_cast<quintptr>(this)));
        QVERIFY(aFp.isEmpty());
    }
    // ============ 2026.08.15 流程档位：ResolveFlow / FilterInputs / 流重载 NextStage ============

    // 空 custom → 完整规范流（旧任务兼容，序一致）。
    void testResolveFlowEmptyIsCanonical() {
        const QList<EStage> aResolved = ResolveFlow(ETaskType::Feature, {});
        QCOMPARE(aResolved, CStageMachine::GetFlowStages(ETaskType::Feature));
        QCOMPARE(aResolved.first(), EStage::Intake);
        QCOMPARE(aResolved.last(), EStage::Test);
    }

    // 精简勾选（收集+确认+开发+审查+验证+测试）→ 6 阶段且按规范序；设计/计划/前置评审被跳过。
    void testResolveFlowSimplePreset() {
        const QList<EStage> aCustom = { EStage::Intake, EStage::ConfirmReq, EStage::Develop,
                                        EStage::CodeReview, EStage::Verify, EStage::Test };
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature, aCustom);
        QCOMPARE(aFlow.size(), 6);
        QCOMPARE(aFlow, aCustom);
        QVERIFY(!aFlow.contains(EStage::Design));
        QVERIFY(!aFlow.contains(EStage::Plan));
        QVERIFY(!aFlow.contains(EStage::ReviewReq));
        // 流重载：ConfirmReq 直接推进 Develop（跳过中间阶段）。
        QCOMPARE(CStageMachine::NextStage(aFlow, EStage::ConfirmReq).value_or(EStage::Intake),
                 EStage::Develop);
        // 末阶段 Test → nullopt（任务完成）。
        QVERIFY(!CStageMachine::NextStage(aFlow, EStage::Test).has_value());
    }

    // Intake 强制包含：用户清单漏勾 Intake 也会被补回首阶段。
    void testResolveFlowForcesIntake() {
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Develop, EStage::CodeReview });
        QCOMPARE(aFlow.first(), EStage::Intake);
        QVERIFY(aFlow.contains(EStage::Develop));
    }

    // Bugfix 自定义流：跳过 RootCauseVerify 后 RootCause 直接进 BugFix。
    void testResolveFlowBugfixSkipsVerify() {
        const QList<EStage> aFlow = ResolveFlow(
            ETaskType::Bugfix, { EStage::Intake, EStage::ConfirmReq, EStage::RootCause,
                                 EStage::BugFix, EStage::CodeReview, EStage::Verify, EStage::Test });
        QCOMPARE(aFlow.size(), 7);
        QVERIFY(!aFlow.contains(EStage::RootCauseVerify));
        QCOMPARE(CStageMachine::NextStage(aFlow, EStage::RootCause).value_or(EStage::Intake),
                 EStage::BugFix);
    }

    // PrevStage：流内前驱；首阶段/不在流内 → nullopt。
    void testPrevStage() {
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Intake, EStage::Develop, EStage::CodeReview });
        QCOMPARE(CStageMachine::PrevStage(aFlow, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Develop);
        QVERIFY(!CStageMachine::PrevStage(aFlow, EStage::Intake).has_value());
        QVERIFY(!CStageMachine::PrevStage(aFlow, EStage::Plan).has_value());
    }

    // FilterInputs：Develop 输入按流过滤——完整流输入不变；精简流（无 Plan/Design）
    // → 只剩 requirements.md，被滤项回带。
    void testFilterInputsByFlow() {
        const SStageDef* pDev = FindStage(EStage::Develop);
        QVERIFY(pDev != nullptr);
        const QStringList aFull = FilterInputs(*pDev, CStageMachine::GetFlowStages(ETaskType::Feature));
        QCOMPARE(aFull, pDev->aInputs);
        const QList<EStage> aSimple = ResolveFlow(ETaskType::Feature,
            { EStage::Intake, EStage::ConfirmReq, EStage::Develop, EStage::CodeReview,
              EStage::Verify, EStage::Test });
        QStringList aDropped;
        const QStringList aKept = FilterInputs(*pDev, aSimple, &aDropped);
        QCOMPARE(aKept, QStringList{ QStringLiteral("requirements.md") });
        QVERIFY(aDropped.contains(QStringLiteral("plan.md")));
        QVERIFY(aDropped.contains(QStringLiteral("design.md")));
    }

    // FilterInputs 兜底：连 ConfirmReq 都没勾 → 全滤空时兜底 intake.md（防 EnsureStageInputs 弹回）。
    void testFilterInputsFallbackIntake() {
        const SStageDef* pDev = FindStage(EStage::Develop);
        QVERIFY(pDev != nullptr);
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Intake, EStage::Develop });
        const QStringList aKept = FilterInputs(*pDev, aFlow);
        QCOMPARE(aKept, QStringList{ QStringLiteral("intake.md") });
    }

    // FilterInputs：CodeReview 输入 plan.md，无 Plan 阶段时滤空 → 兜底 requirements.md（ConfirmReq 在流内）。
    void testFilterInputsCodeReview() {
        const SStageDef* pCr = FindStage(EStage::CodeReview);
        QVERIFY(pCr != nullptr);
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
            { EStage::Intake, EStage::ConfirmReq, EStage::Develop, EStage::CodeReview,
              EStage::Verify, EStage::Test });
        const QStringList aKept = FilterInputs(*pCr, aFlow);
        QCOMPARE(aKept, QStringList{ QStringLiteral("requirements.md") });
    }

    // ============ 2026-09-22 助手自动重命名（AutoNameAsstNow 端到端） ============
    //   stub hermes CLI 返回两行（名称+说明）→ sigAsstAutoNameDone 回填：
    //   名称剥尾标点截 10 字、说明取第二行起并截 80 字；目标助手无既有会话时自动建会话执行。
    void asstAutoRenameFlow() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 助手自动重命名用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/rename_calls.log");
        // 名称故意 12 字（验截 10）+ 尾顿号（验 TrimNamePunct 剥尾标点）；说明带尾句号。
        const QString sStubName = QStringLiteral("修复登录页面键盘焦点问题：");
        const QString sStubDesc = QStringLiteral(
            "用户在该助手中主要定位并修复了登录页的键盘焦点 bug，并补充了回归测试。");
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            fCli.write(QStringLiteral(
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%1,'===\\n'+s+'\\n');"
                "console.log(JSON.stringify({type:'session',sessionId:'stub-rn-1'}));"
                "console.log(JSON.stringify({type:'assistant',text:'stub 完成',"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:'stub-rn-1',text:%2+'\\n'+%3}));"
                "});")
                .arg(QStringLiteral("'") + sLog + QStringLiteral("'"),
                     QStringLiteral("'") + sStubName + QStringLiteral("'"),
                     QStringLiteral("'") + sStubDesc + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        QString sDoneId;
        bool bDoneOk = false;
        QString sDoneName, sDoneDesc;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAsstAutoNameDone,
                         [&](const QString& sId, bool bOk, const QString& sName,
                             const QString& sDesc) {
            sDoneId = sId; bDoneOk = bOk; sDoneName = sName; sDoneDesc = sDesc;
        });
        // 目标助手无既有会话：直接建会话执行（不再要求先 Submit 过）。
        QVERIFY(stOrch.AutoNameAsstNow(QStringLiteral("asst-r"),
                                       QStringLiteral("1. 修复登录页\\n2. 补充单元测试")));
        for (int iWait = 0; iWait < 600 && sDoneId.isEmpty(); ++iWait) {
            QTest::qWait(100);
        }
        QCOMPARE(sDoneId, QStringLiteral("asst-r"));
        QVERIFY(bDoneOk);
        // 名称：12 字截 10（剥尾顿号在截断前，名称仍是左起 10 字）。
        QCOMPARE(sDoneName, sStubName.left(10));
        QVERIFY(sDoneName.size() <= 10);
        // 说明：第二行起合并，剥尾句号，含关键内容，不超 80 字。
        QVERIFY(sDoneDesc.contains(QStringLiteral("键盘焦点")));
        QVERIFY(!sDoneDesc.endsWith(QLatin1Char('。')));
        QVERIFY(sDoneDesc.size() <= 80);
        // 提示词确实携带【助手命名】与素材行。
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const QString sCalls = QString::fromUtf8(fLog.readAll());
        fLog.close();
        QVERIFY(sCalls.contains(QStringLiteral("【助手命名】")));
        QVERIFY(sCalls.contains(QStringLiteral("修复登录页")));

        // 静态配置复位（不泄漏给同进程后续用例）。
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }
    // ============ 2026-10-03 自动命名忙等待（AutoNameAsstNow 全忙排队至空闲） ============
    //   实害：HarnessGatewayPlugin 单助手工程——収尾自动命名在 sigAdhocFinished 同步链里
    //   跑，目标 runner 尚未退净被误判「忙」且无空闲会话可借 → 旧版直接 false 打
    //   「✗ 无可用的空闲助手会话」。本用例：慢 stub 轮占住会话 → AutoNameAsstNow 返回
    //   true（已受理）+ auto_name_wait 事件 + 不立即 done；轮结束退净后 3s 重查自动启动
    //   命名轮回填成功；StopAdhoc 取消排队（不再冒命名轮）。
    void autoNameBusyWaitUntilIdle() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 命名忙等待用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/busy_calls.log");
        // 慢 stub：读入 stdin 后延时 4s 才输出 result（模拟长轮/收尾退净窗口）；命名轮
        //   因提示词含【助手命名】走快路径立即回两行。
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            fCli.write(QStringLiteral(
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%1,'===\\n'+s+'\\n');"
                "const bName=s.includes('【助手命名】');"
                "const emit=()=>{"
                "console.log(JSON.stringify({type:'session',sessionId:'stub-bw-1'}));"
                "console.log(JSON.stringify({type:'assistant',text:'stub',"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:'stub-bw-1',text:bName?"
                "('延迟命名测试\\n'+'用户在该助手中验证忙等待机制。')"
                ":'已完成'}));};"
                "if(bName){emit();}else{setTimeout(emit,4000);}"
                "});")
                .arg(QStringLiteral("'") + sLog + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));
        CTaskOrchestrator::SetAdhocDeepOverride(0);   // 关专家模式：只读轮直投，不进深度链

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        QString sDoneId;
        int iDoneCount = 0;
        bool bDoneOk = false;
        QString sDoneName;
        bool bWaitEvent = false;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAsstAutoNameDone,
                         [&](const QString& sId, bool bOk, const QString& sName,
                             const QString&) {
            ++iDoneCount; sDoneId = sId; bDoneOk = bOk; sDoneName = sName;
        });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocEvent,
                         [&](const QString&, const QString& sKind, const QString&) {
            if (sKind == QStringLiteral("auto_name_wait")) {
                bWaitEvent = true;
            }
        });
        // ① 慢轮占住 asst-busy 会话（4s 后才结束）。
        stOrch.SubmitAdhoc(QStringLiteral("asst-busy"),
                           QStringLiteral("慢慢处理这个请求"),
                           CTaskOrchestrator::PermReadOnly);
        QTest::qWait(1500);   // 确保轮在跑（fnBusy 判忙）
        // ② 忙且无空闲会话可借：不再 false，置等待 + 事件行 + 返回 true。
        QVERIFY(stOrch.AutoNameAsstNow(QStringLiteral("asst-busy"),
                                       QStringLiteral("1. 忙等待验证")));
        QVERIFY(bWaitEvent);
        QCOMPARE(iDoneCount, 0);   // 未立即回填（在排队）
        // ③ 轮结束退净 + 3s 重查 → 命名轮自动启动并回填成功。
        for (int iWait = 0; iWait < 200 && iDoneCount == 0; ++iWait) {
            QTest::qWait(100);
        }
        QCOMPARE(iDoneCount, 1);
        QCOMPARE(sDoneId, QStringLiteral("asst-busy"));
        QVERIFY(bDoneOk);
        QCOMPARE(sDoneName, QStringLiteral("延迟命名测试"));
        // ④ Stop 取消：独立编排器实例（会话表内只有 asst-stop，无其他空闲会话可借，
        //   确保走排队路径而非借道 asst-busy 的空闲会话）→ 排队 → StopAdhoc → 重试点
        //   （3s）后不启动命名。
        {
            CTaskOrchestrator stOrch2(&stPm);
            QString sDoneId2;
            QObject::connect(&stOrch2, &CTaskOrchestrator::sigAsstAutoNameDone,
                             [&](const QString& sId, bool, const QString&, const QString&) {
                sDoneId2 = sId;
            });
            stOrch2.SubmitAdhoc(QStringLiteral("asst-stop"),
                                QStringLiteral("停止验证轮"),
                                CTaskOrchestrator::PermReadOnly);
            QTest::qWait(1500);
            QVERIFY(stOrch2.AutoNameAsstNow(QStringLiteral("asst-stop"),
                                            QStringLiteral("1. 停止取消验证")));
            stOrch2.StopAdhoc(QStringLiteral("asst-stop"));
            QTest::qWait(5000);   // 覆盖 3s 重查点
            QVERIFY(sDoneId2.isEmpty());   // 未回填（等待已被取消）
        }
        // 进程调用数恰 3：两轮 Submit + asst-busy 命名轮（asst-stop 被取消未起命名）。
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const int iCalls = QString::fromUtf8(fLog.readAll())
                               .count(QStringLiteral("==="));
        fLog.close();
        QCOMPARE(iCalls, 3);

        // 静态配置复位（不泄漏给同进程后续用例）。
        CTaskOrchestrator::SetAdhocDeepOverride(-1);
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }

    // ============ 2026-10-08 提示词超限提炼（RefinePromptAsstNow 全链） ============
    //   面板闸门超限上抛 → 一次性只读提炼轮（fork 分支）→ 提炼稿回投 SubmitAdhoc
    //   正常提交。stub：含【提示词提炼】的轮回「提炼后的短提示词」，其余回「普通完成」；
    //   断言：refine_start/refine_done 事件、恰 2 次进程调用、第 2 次提交的提示词
    //   = 提炼稿（不含提炼指令头）。
    void promptRefineRoundTrip() {
#ifdef Q_OS_WIN
        const QString sNode = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 提炼用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path() + QStringLiteral("/proj");
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        const QString sLog = oTmp.path() + QStringLiteral("/refine_calls.log");
        {
            QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
            QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
            fCli.write(QStringLiteral(
                "let s='';process.stdin.on('data',d=>s+=d);"
                "process.stdin.on('end',()=>{"
                "require('fs').appendFileSync(%1,'===\\n'+s+'\\n');"
                "const bRefine=s.includes('【提示词提炼】');"
                "console.log(JSON.stringify({type:'session',sessionId:'stub-rf-1'}));"
                "console.log(JSON.stringify({type:'assistant',text:'stub',"
                "model:'stub-model',usage:{input:10,output:5}}));"
                "console.log(JSON.stringify({type:'result',subtype:'success',"
                "sessionId:'stub-rf-1',text:bRefine?"
                "'提炼后的短提示词：分三步完成重构并自测':'普通完成'}));"
                "});")
                .arg(QStringLiteral("'") + sLog + QStringLiteral("'"))
                .toUtf8());
            fCli.close();
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));
        CTaskOrchestrator::SetAdhocDeepOverride(0);

        CProjectManager stPm(nullptr);
        QVERIFY(stPm.Open(sProj));
        CTaskOrchestrator stOrch(&stPm);
        bool bStartEvt = false;
        bool bDoneEvt = false;
        QStringList aEvtKinds;
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocEvent,
                         [&](const QString&, const QString& sKind, const QString& sMsg) {
            aEvtKinds << (sKind + QStringLiteral(" | ") + sMsg.left(400));
            if (sKind == QStringLiteral("refine_start")) { bStartEvt = true; }
            if (sKind == QStringLiteral("refine_done")) { bDoneEvt = true; }
        });
        // 原文 300 CJK（估算 300 > 上限 50）：触发提炼。
        const QString sOriginal(300, QChar(0x539F));   // 「原」×300
        stOrch.RefinePromptAsstNow(QStringLiteral("asst-rf"), sOriginal, 50,
                                   CTaskOrchestrator::PermAuto);
        QVERIFY(bStartEvt);   // 提炼轮同步受理并起跑
        // 等提炼轮 + 回投提交轮两次进程调用都落地（快 stub，秒级）。
        int iSegs = 0;
        for (int iWait = 0; iWait < 200; ++iWait) {
            QTest::qWait(100);
            QFile fLog(sLog);
            if (fLog.open(QIODevice::ReadOnly)) {
                iSegs = QString::fromUtf8(fLog.readAll())
                            .count(QStringLiteral("==="));
                fLog.close();
            }
            if (bDoneEvt && iSegs >= 2) { break; }
        }
        QVERIFY2(bDoneEvt, qPrintable(aEvtKinds.join(QStringLiteral("\n"))));
        QCOMPARE(iSegs, 2);   // 恰两次：提炼轮 + 提炼稿提交轮
        QFile fLog(sLog);
        QVERIFY(fLog.open(QIODevice::ReadOnly));
        const QStringList aSegs = QString::fromUtf8(fLog.readAll())
                                      .split(QStringLiteral("==="));
        fLog.close();
        QVERIFY(aSegs.size() >= 3);
        QVERIFY(aSegs.at(1).contains(QStringLiteral("【提示词提炼】")));   // 第 1 轮含指令头+原文
        QVERIFY(aSegs.at(1).contains(sOriginal.left(20)));
        QVERIFY(aSegs.at(2).contains(
            QStringLiteral("提炼后的短提示词")));   // 第 2 轮提交的是提炼稿
        QVERIFY(!aSegs.at(2).contains(QStringLiteral("【提示词提炼】")));

        // 静态配置复位。
        CTaskOrchestrator::SetAdhocDeepOverride(-1);
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesHomeOverride(QString());
#else
        QSKIP("非 Windows 平台跳过");
#endif // Q_OS_WIN
    }
};

QTEST_MAIN(CTestVerifyFlow)
#include "test_verify_flow.moc"
