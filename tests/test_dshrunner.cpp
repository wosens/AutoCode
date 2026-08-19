// CDeepSeekRunner 集成测试（联网，真实调用 DSH 组合 + glm-5.2 中继，不挂 ctest 手动跑）。
//
// 覆盖：冷启动握手 / 三信号流（sigAssistantText/sigToolUse/sigToolResult）/
// sigContextFill / sigFinished 元数据（subtype/reqCount/model/会话 sid）/
// 同进程 resumeSid 续会话（多轮记忆）/ InjectUserTurn（Idle 注入新一轮）/
// claude 专属参数降级提示不炸。
//
// 用法: test_dshrunner [node] [bin.js] [cordis.yml] [provider] [model]
//   缺省用本机实测路径（DSH 仓库 examples/autocode-agent 运行副本）。
// 工作目录 = %TEMP%/autocode-dsh-test（每轮清空重建）。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTextStream>

#include "agent/CDeepSeekRunner.h"

static QTextStream g_out(stdout);
static int g_iStep = 0;        // 1=首轮写文件 2=resume 续问 3=InjectUserTurn 注入
static int g_iFail = 0;
static QString g_sWorkDir;
static QString g_sSidStep1;
static CDeepSeekRunner* g_pRunner = nullptr;
static QTimer* g_pGuard = nullptr;

static void Fail(const QString& a_sWhy) {
    ++g_iFail;
    g_out << "  [FAIL] " << a_sWhy << Qt::endl;
}

static void ArmGuard() {
    // 每步 180s 超时保护（真实模型调用 + 工具执行，宽裕上限）。
    g_pGuard->start(180000);
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QString sNode = (argc > 1) ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral(R"(D:\Program Files\nodejs\node.exe)");
    const QString sBin = (argc > 2) ? QString::fromLocal8Bit(argv[2])
        : QStringLiteral(R"(E:\wosens\Programs\VisualC++\QT\DeepSeekHarness\deepseek-harness\packages\examples\jsonrpc-demo\lib\bin.js)");
    const QString sCordis = (argc > 3) ? QString::fromLocal8Bit(argv[3])
        : QStringLiteral(R"(E:\wosens\Programs\VisualC++\QT\DeepSeekHarness\deepseek-harness\examples\autocode-agent\cordis.yml)");
    CDeepSeekRunner::SetDshNodePath(sNode);
    CDeepSeekRunner::SetDshBinJsPath(sBin);
    CDeepSeekRunner::SetDshCordisPath(sCordis);

    // —— 步骤0（离线单测）：轻量自动配置的根目录推导 + node PATH 兜底 ——
    //   优先级：显式键 > root 推导；node 留空 → "node"。
    {
        g_out << "step0: root-derive + PATH fallback" << Qt::endl;
        const QString sRoot = QDir(sBin).absolutePath().section(
            QStringLiteral("/packages/"), 0, 0);   // 从已知 bin.js 反推仓库根
        CDeepSeekRunner::SetDshBinJsPath(QString());
        CDeepSeekRunner::SetDshCordisPath(QString());
        CDeepSeekRunner::SetDshRootPath(sRoot);
        if (CDeepSeekRunner::ResolveBinJsPath() != QDir::fromNativeSeparators(sBin)) {
            Fail(QStringLiteral("root 推导 bin.js 不符: %1").arg(CDeepSeekRunner::ResolveBinJsPath()));
        }
        if (CDeepSeekRunner::ResolveCordisPath() != QDir::fromNativeSeparators(sCordis)) {
            Fail(QStringLiteral("root 推导 cordis 不符: %1").arg(CDeepSeekRunner::ResolveCordisPath()));
        }
        if (!CDeepSeekRunner::LooksLikeDshRoot(sRoot) || CDeepSeekRunner::LooksLikeDshRoot(QStringLiteral("Z:/no-such"))) {
            Fail(QStringLiteral("LooksLikeDshRoot 判定错误"));
        }
        CDeepSeekRunner::SetDshNodePath(QString());
        if (CDeepSeekRunner::ResolveNodePath() != QStringLiteral("node")) {
            Fail(QStringLiteral("node 留空未回退 PATH"));
        }
        CDeepSeekRunner::SetDshNodePath(sNode);
        // 恢复显式键（后续联网步骤与旧行为一致）
        CDeepSeekRunner::SetDshRootPath(QString());
        CDeepSeekRunner::SetDshBinJsPath(sBin);
        CDeepSeekRunner::SetDshCordisPath(sCordis);
        if (!CDeepSeekRunner::DshConfigError().isEmpty()) {
            Fail(QStringLiteral("三路径齐备时 DshConfigError 应为空: %1")
                     .arg(CDeepSeekRunner::DshConfigError()));
        }
        g_out << "  step0 " << (g_iFail == 0 ? "PASS" : "FAIL") << Qt::endl;
    }

    CDeepSeekRunner::SetDshProvider((argc > 4) ? QString::fromLocal8Bit(argv[4])
                                               : QStringLiteral("glm-custom"));
    CDeepSeekRunner::SetDshModel((argc > 5) ? QString::fromLocal8Bit(argv[5])
                                            : QStringLiteral("glm-5.2"));

    // 干净工作目录
    g_sWorkDir = QDir::tempPath() + QStringLiteral("/autocode-dsh-test");
    QDir(g_sWorkDir).removeRecursively();
    QDir().mkpath(g_sWorkDir);

    g_pRunner = new CDeepSeekRunner(&app);
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigInfo, &app,
                     [](const QString& a_sLine) { g_out << "  [info] " << a_sLine << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigAssistantText, &app,
                     [](const QString& a_sText) { g_out << "  [assistant] " << a_sText << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigToolUse, &app,
                     [](const QString& a_sTool, const QString& a_sInput) {
                         g_out << "  [tool_use] " << a_sTool << " " << a_sInput.left(120) << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigToolResult, &app,
                     [](const QString& a_sResult) { g_out << "  [tool_result] " << a_sResult.left(120) << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigContextFill, &app,
                     [](qint64 a_iFill) { g_out << "  [ctx_fill] " << a_iFill << Qt::endl; });
    QObject::connect(g_pRunner,
                     qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                         &CAgentRunnerBase::sigFinished),
                     &app, [&](bool a_bOk, const QString& a_sResult, qint64 a_iIn, qint64 a_iOut,
                               double a_dCost, const QString& a_sSid) {
        g_pGuard->stop();
        g_out << QString("  [finished] ok=%1 subtype=%2 sid=%3 in=%4 out=%5 cost=%6 req=%7 model=%8")
                     .arg(a_bOk).arg(g_pRunner->LastSubtype(), a_sSid)
                     .arg(a_iIn).arg(a_iOut).arg(a_dCost).arg(g_pRunner->LastReqCount())
                     .arg(g_pRunner->LastModel())
              << Qt::endl
              << "  [result] " << a_sResult.left(200) << Qt::endl;
        if (!a_bOk) {
            Fail(QStringLiteral("步骤 %1 未成功完成").arg(g_iStep));
            app.exit(1);
            return;
        }
        if (g_iStep == 1) {
            const QString sOut = g_sWorkDir + QStringLiteral("/dsh_test_out.txt");
            if (!QFile::exists(sOut)) {
                Fail(QStringLiteral("步骤1：工具未写出 %1").arg(sOut));
            }
            if (g_pRunner->LastSubtype() != QStringLiteral("success")) {
                Fail(QStringLiteral("步骤1：subtype=%1 应为 success").arg(g_pRunner->LastSubtype()));
            }
            if (g_pRunner->LastReqCount() < 1) {
                Fail(QStringLiteral("步骤1：reqCount=0（assistant/message 未计数）"));
            }
            if (g_pRunner->LastModel().isEmpty()) {
                Fail(QStringLiteral("步骤1：model 为空（request/context 未捕获）"));
            }
            if (a_iIn <= 0 || a_iOut <= 0) {
                Fail(QStringLiteral("步骤1：token 统计异常 in=%1 out=%2").arg(a_iIn).arg(a_iOut));
            }
            g_sSidStep1 = a_sSid;
            // 步骤2：同进程 resumeSid 续会话（多轮记忆验证）
            g_iStep = 2;
            ArmGuard();
            g_pRunner->StartStage(g_sWorkDir,
                                  QStringLiteral("你刚才创建的文件叫什么名字？只回复文件名本身，不要其他内容。"),
                                  QString(), 30, g_sSidStep1, false, 0, false);
        } else if (g_iStep == 2) {
            if (!a_sResult.contains(QStringLiteral("dsh_test_out"), Qt::CaseInsensitive)) {
                Fail(QStringLiteral("步骤2：多轮记忆失效，回复不含文件名：%1").arg(a_sResult.left(80)));
            }
            if (a_sSid != g_sSidStep1) {
                Fail(QStringLiteral("步骤2：resumeSid 未复用（%1 != %2）").arg(a_sSid, g_sSidStep1));
            }
            // 步骤3：InjectUserTurn（Idle 注入新一轮，同会话）
            g_iStep = 3;
            ArmGuard();
            if (!g_pRunner->InjectUserTurn(QStringLiteral("1+1 等于几？只回复数字。"))) {
                Fail(QStringLiteral("步骤3：InjectUserTurn 返回 false（Idle 注入失败）"));
                app.exit(1);
                return;
            }
        } else if (g_iStep == 3) {
            if (!a_sResult.trimmed().startsWith(QStringLiteral("2"))) {
                Fail(QStringLiteral("步骤3：注入轮回复异常：%1").arg(a_sResult.left(60)));
            }
            g_out << "\n== 汇总: " << (g_iFail == 0 ? QStringLiteral("ALL PASS") : QStringLiteral("%1 FAIL").arg(g_iFail))
                  << Qt::endl;
            app.exit(g_iFail == 0 ? 0 : 1);
        }
    });

    g_pGuard = new QTimer(&app);
    g_pGuard->setSingleShot(true);
    QObject::connect(g_pGuard, &QTimer::timeout, &app, [&]() {
        Fail(QStringLiteral("步骤 %1 超时（180s）").arg(g_iStep));
        g_pRunner->Stop();
        app.exit(1);
    });

    g_out << "== test_dshrunner 工作目录: " << g_sWorkDir << Qt::endl;
    g_iStep = 1;
    ArmGuard();
    g_pRunner->StartStage(g_sWorkDir,
                          QStringLiteral("在当前工作目录创建文件 dsh_test_out.txt，内容为一行文本：DSH-RUNNER-OK。"
                                         "创建成功后只回复 OK 两个字母。"),
                          QString(), 30, QString(), false, 0, false);
    return app.exec();
}
