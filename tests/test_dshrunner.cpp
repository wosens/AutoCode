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
#include <QTemporaryDir>
#include <QEventLoop>
#include <QProcess>
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

// 2026.09.01 --selfheal 模式：端到端验证版本不兼容自愈——先用 git 把 DSH 真实
// 更新到 dsh-v0.1.2-alpha.3（重演事故：上游移除示例包），再 StartStage 触发自动
// 修复流水线（回滚基线+清空壳+清残留+pnpm 重建），断言：修复事件流出现、DSH 回到
// rc.7、最终 sigFinished(ok)。全程真实 git/pnpm，约 5~15 分钟。
static int RunSelfHealE2E(const QString& a_sRoot) {
    g_out << "== selfheal e2e: 破坏现场（git reset --hard dsh-v0.1.2-alpha.3）==" << Qt::endl;
    int iRc = QProcess::execute(QStringLiteral("git"),
        { QStringLiteral("-C"), a_sRoot, QStringLiteral("reset"),
          QStringLiteral("--hard"), QStringLiteral("dsh-v0.1.2-alpha.3") });
    if (iRc != 0) {
        g_out << "  [FAIL] 破坏现场失败（无该 tag？）rc=" << iRc << Qt::endl;
        return 1;
    }
    const QString sErrBefore = CDeepSeekRunner::DshConfigError();
    g_out << "  破坏后 DshConfigError: " << (sErrBefore.isEmpty()
        ? QStringLiteral("(空)") : sErrBefore.left(60)) << Qt::endl;
    if (!sErrBefore.contains(QStringLiteral("版本与 AutoCode 集成不兼容"))) {
        g_out << "  [FAIL] 探测未识别为不兼容" << Qt::endl;
        return 1;
    }
    g_sWorkDir = QDir::temp().filePath(QStringLiteral("autocode-selfheal-e2e"));
    QDir().mkpath(g_sWorkDir);
    QObject::connect(g_pRunner, &CDeepSeekRunner::sigInfo, [](const QString& s) {
        g_out << "  [info] " << s << Qt::endl;
    });
    int iOk = -1;
    QString sFinal;
    QObject::connect(g_pRunner, &CDeepSeekRunner::sigFinished,
        [&iOk, &sFinal](bool b, const QString& s, qint64, qint64, double,
                        const QString&) {
            iOk = b ? 1 : 0;
            sFinal = s;
        });
    QTimer::singleShot(0, []() {
        g_pRunner->StartStage(g_sWorkDir,
            QStringLiteral("只回复两个字：成功"), QString(), 30, QString(), false, 0, false);
    });
    QTimer stGuard;
    stGuard.setSingleShot(true);
    QEventLoop stLoop;
    QObject::connect(&stGuard, &QTimer::timeout, &stLoop, &QEventLoop::quit);
    QObject::connect(g_pRunner, &CDeepSeekRunner::sigFinished, &stLoop, &QEventLoop::quit);
    stGuard.start(25 * 60 * 1000);
    stLoop.exec();
    if (iOk != 1) {
        g_out << "  [FAIL] 自愈后未成功完成一轮: ok=" << iOk
              << " final=" << sFinal.left(200) << Qt::endl;
        return 1;
    }
    // 终态校验：版本回基线 + 组合包可达
    const QString sErrAfter = CDeepSeekRunner::DshConfigError();
    if (!sErrAfter.isEmpty()) {
        g_out << "  [FAIL] 修复后仍有配置错误: " << sErrAfter.left(120) << Qt::endl;
        return 1;
    }
    g_out << "  == selfheal e2e PASS ==" << Qt::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    // --selfheal <DSH根>：端到端自愈验证（真实破坏→自动修复→跑通一轮）
    if (argc > 2 && QByteArray(argv[1]) == "--selfheal") {
        const QString sRootArg = QString::fromLocal8Bit(argv[2]);
        CDeepSeekRunner::SetDshRootPath(sRootArg);
        CDeepSeekRunner::SetDshNodePath(QString());   // 走 PATH
        g_pRunner = new CDeepSeekRunner();
        return RunSelfHealE2E(sRootArg);
    }
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

    // —— 步骤0b（离线单测）：2026.09.01 自愈纯函数 ——
    //   credentials 坏格式解析/渲染 + 空壳目录计算 + 构建残留清理（临时目录）。
    {
        g_out << "step0b: self-heal pure functions" << Qt::endl;
        const int iFailBefore = g_iFail;
        QStringList aPairs;
        // 1) 块式包装（事故现场形态 version:1 + refs: 缩进块）
        const QString sBad = QStringLiteral("version: 1\nrefs:\n"
            "  GLM_CUSTOM_API_KEY: e94331c8-aaa\n"
            "  LUSTER_API_KEY: cr_bb\n");
        if (!CDeepSeekRunner::ParseWrappedCredentials(sBad, &aPairs)
            || aPairs.size() != 2
            || !aPairs[0].startsWith(QStringLiteral("GLM_CUSTOM_API_KEY	e94331c8-aaa"))) {
            Fail(QStringLiteral("块式包装解析不符: %1").arg(aPairs.join(QStringLiteral(";"))));
        }
        // 2) flow map 包装（值带引号）+ 平铺渲染往返
        const QString sBad2 = QStringLiteral("version: \"1\"\nrefs:\n"
            "  { GLM_CUSTOM_API_KEY: \"k1\", LUSTER_API_KEY: k2 }\n");
        QStringList aPairs2;
        if (!CDeepSeekRunner::ParseWrappedCredentials(sBad2, &aPairs2)
            || aPairs2.size() != 2
            || !aPairs2[0].contains(QStringLiteral("k1"))) {
            Fail(QStringLiteral("flow map 解析不符: %1").arg(aPairs2.join(QStringLiteral(";"))));
        }
        const QString sFlat = CDeepSeekRunner::RenderFlatCredentials(aPairs2);
        if (!sFlat.contains(QStringLiteral("GLM_CUSTOM_API_KEY: k1"))
            || !sFlat.contains(QStringLiteral("LUSTER_API_KEY: k2"))) {
            Fail(QStringLiteral("平铺渲染不符: %1").arg(sFlat));
        }
        // 3) 平铺 / 仅 version 无 refs → 不动（false）
        QStringList aP;
        if (CDeepSeekRunner::ParseWrappedCredentials(
                QStringLiteral("GLM_CUSTOM_API_KEY: v1\n"), &aP)) {
            Fail(QStringLiteral("平铺格式不应被识别为坏格式"));
        }
        if (CDeepSeekRunner::ParseWrappedCredentials(
                QStringLiteral("version: 1\n"), &aP)) {
            Fail(QStringLiteral("仅 version 无 refs 不应产出迁移对"));
        }
        // 4) 空壳目录 + 5) 构建残留清理（临时目录）
        QTemporaryDir stTmp;
        if (!stTmp.isValid()) {
            Fail(QStringLiteral("临时目录不可用"));
        } else {
            const QString sR = stTmp.path();
            QDir().mkpath(sR + QStringLiteral("/packages/bundle/sdk-minimal/node_modules/d"));
            QDir().mkpath(sR + QStringLiteral("/packages/bundle/base/src"));
            QDir().mkpath(sR + QStringLiteral("/vendor/loader"));
            QDir().mkpath(sR + QStringLiteral("/vendor/ghost"));
            QDir().mkpath(sR + QStringLiteral("/apps/cli"));
            QFile(sR + QStringLiteral("/packages/bundle/base/src/a.ts")).open(QIODevice::WriteOnly);
            QFile(sR + QStringLiteral("/vendor/loader/README.md")).open(QIODevice::WriteOnly);
            QFile(sR + QStringLiteral("/apps/cli/package.json")).open(QIODevice::WriteOnly);
            const QStringList aHollow = CDeepSeekRunner::ComputeHollowDirs(
                sR, QStringLiteral("packages/bundle/base/src/a.ts\n"
                    "vendor/loader/README.md\n"
                    "apps/cli/package.json\n"));
            const QString sAll = aHollow.join(QStringLiteral(";"));
            if (!sAll.contains(QStringLiteral("packages/bundle/sdk-minimal"))
                || !sAll.contains(QStringLiteral("vendor/ghost"))
                || sAll.contains(QStringLiteral("bundle/base"))
                || sAll.contains(QStringLiteral("vendor/loader"))
                || sAll.contains(QStringLiteral("apps/cli"))) {
                Fail(QStringLiteral("空壳目录计算不符: %1").arg(sAll));
            }
            QDir().mkpath(sR + QStringLiteral("/packages/bundle/base/lib"));
            QDir().mkpath(sR + QStringLiteral("/packages/bundle/base/node_modules/keep/lib"));
            QFile(sR + QStringLiteral("/packages/bundle/base/lib/a.js")).open(QIODevice::WriteOnly);
            QFile(sR + QStringLiteral("/packages/bundle/base/x.tsbuildinfo")).open(QIODevice::WriteOnly);
            QFile(sR + QStringLiteral("/packages/bundle/base/node_modules/keep/lib/b.js")).open(QIODevice::WriteOnly);
            CDeepSeekRunner::CleanBuildResidue(sR);
            if (QFile::exists(sR + QStringLiteral("/packages/bundle/base/lib/a.js"))
                || QFile::exists(sR + QStringLiteral("/packages/bundle/base/x.tsbuildinfo"))) {
                Fail(QStringLiteral("lib/tsbuildinfo 残留未清"));
            }
            if (!QFile::exists(sR + QStringLiteral("/packages/bundle/base/node_modules/keep/lib/b.js"))) {
                Fail(QStringLiteral("node_modules 内部被误清"));
            }
        }
        g_out << "  step0b " << (g_iFail == iFailBefore ? "PASS" : "FAIL") << Qt::endl;
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
