// CClaudeRunner 集成测试（联网，真实调用本机 claude CLI —— 走 ~/.claude/settings.json
// 配置的网关，本机为内网 GLM 中转，不挂 ctest 手动跑）。
//
// 背景（2026.09.23 v3.1.225 修复的回归锚）：内网 GLM 网关的 Anthropic 兼容层在
//   message_start / assistant 帧里 usage 恒全 0，真实 input/cache 三项只在末条
//   message_delta 携带——修复前 CClaudeRunner 只认 assistant 帧，上下文占用全程
//   0（用户实测「claude 后端上下文一直显示 0」）。修复后 message_delta 分支补读
//   三项刷新 m_iLastContextFill 并发射 sigContextFill。
//
// 覆盖：sigContextFill 运行中发射且 >0（修复点）/ sigFinished 元数据
//   （subtype/reqCount/model）/ LastContextFill 收尾仍 >0。
//
// 用法: test_clauderunner.exe（无参数；需本机 claude CLI 可用且已配置网关）
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QTextStream>
#include <QFile>

#include "agent/CClaudeRunner.h"

static QTextStream g_out(stdout);
static int g_iFail = 0;
static bool g_bCtxSeen = false;        // 运行中收到过 sigContextFill(>0)
static qint64 g_iCtxLast = 0;
static QString g_sWorkDir;
static CClaudeRunner* g_pRunner = nullptr;
static QTimer* g_pGuard = nullptr;

static void Fail(const QString& a_sWhy) {
    ++g_iFail;
    g_out << "  [FAIL] " << a_sWhy << Qt::endl;
}

namespace {
void Check2(bool a_b, const QString& a_sWhy) {
    if (!a_b) { Fail(a_sWhy); } else { g_out << "  [ok] " << a_sWhy << Qt::endl; }
}

// 2026-10-08 stub 模式（-stub，挂 ctest 不联网）：claude 命令指向批处理桩——
//   落盘进程 env 后回一条合法 result 行收尾。验证共享大模型表 env 注入：
//   实例槽覆盖优先（ANTHROPIC_BASE_URL/MODEL 按覆盖条目），覆盖无效时按
//   「跟随开关 + 共享表当前条目」，开关关则完全不注入（走系统 env）。
int RunStubMode(int a_iArgc, char* a_pArgv[]) {
    QCoreApplication app(a_iArgc, a_pArgv);
    const QString sDir = QDir::tempPath() + QStringLiteral("/autocode-claude-stub");
    QDir(sDir).removeRecursively();
    QDir().mkpath(sDir);
    const QString sStub = sDir + QStringLiteral("/claude_stub.cmd");
    const QString sEnvTxt = sDir + QStringLiteral("/env.txt");
    {
        QFile f(sStub);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write(QStringLiteral("set ANTHROPIC > \"%1\"\n")
                    .arg(sEnvTxt).toUtf8());
        f.write(QStringLiteral("echo {\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\",\"session_id\":\"stub-sid-env\",\"total_cost_usd\":0,\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}\n").toUtf8());
        f.close();
    }
    CClaudeRunner::SetClaudeCmd(sStub);
    SDshModelEntry stOv;
    stOv.sDisp = QStringLiteral("覆盖模型");
    stOv.sBaseURL = QStringLiteral("http://127.0.0.1:9/ov");
    stOv.sApiKey = QStringLiteral("ov-key");
    stOv.sApi = QStringLiteral("anthropic-messages");
    stOv.sModelId = QStringLiteral("ov-claude");
    SDshModelEntry stG;
    stG.sBaseURL = QStringLiteral("http://127.0.0.1:9/g");
    stG.sApiKey = QStringLiteral("g-key");
    stG.sApi = QStringLiteral("anthropic-messages");
    stG.sModelId = QStringLiteral("g-claude");
    QList<SDshModelEntry> aTbl = { stG, stOv };
    CClaudeRunner::SetSharedModelTable(aTbl, /*active=*/0);
    CClaudeRunner::SetFollowSharedModels(true);
    auto fnEnvOf = [&sEnvTxt](const QString& a_sKey) {
        QFile f(sEnvTxt);
        if (!f.open(QIODevice::ReadOnly)) { return QString(); }
        const QString sAll = QString::fromUtf8(f.readAll());
        f.close();
        const QStringList aLines = sAll.split(QLatin1Char('\n'));
        for (const QString& sLine : aLines) {
            if (sLine.startsWith(a_sKey + QLatin1Char('='))) {
                return sLine.mid(a_sKey.size() + 1).trimmed();
            }
        }
        return QString();
    };
    auto fnRun = [&app, &sDir](CClaudeRunner& a_Runner, const QString& a_sTag) {
        int iFin = 0;
        QObject::connect(&a_Runner, &CAgentRunnerBase::sigFinished,
                         &a_Runner, [&](bool, const QString&, qint64, qint64, double,
                                        const QString&) { ++iFin; QCoreApplication::exit(0); });
        QTimer::singleShot(30000, []() { QCoreApplication::exit(2); });
        a_Runner.StartStage(sDir, QStringLiteral("env测试"), QString(), 40,
                            QString(), false, 0, false);
        QCoreApplication::exec();
        if (iFin != 1) { Fail(a_sTag + QStringLiteral("：未正常收尾")); }
    };
    {   // A. 实例槽覆盖优先
        CClaudeRunner runner;
        runner.SetStageModelOverride(1, stOv);
        fnRun(runner, QStringLiteral("覆盖轮"));
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_MODEL")) == QStringLiteral("ov-claude"),
               QStringLiteral("覆盖条目应注入 ANTHROPIC_MODEL=ov-claude"));
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_BASE_URL")).contains(QStringLiteral("127.0.0.1:9/ov")),
               QStringLiteral("覆盖条目应注入 BASE_URL"));
    }
    {   // B. 无覆盖 + 跟随开 → 共享表当前条目
        CClaudeRunner runner;
        fnRun(runner, QStringLiteral("跟随轮"));
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_MODEL")) == QStringLiteral("g-claude"),
               QStringLiteral("跟随开应注入当前条目 g-claude"));
    }
    {   // C. 跟随关 → 不注入（系统 env 无这些键）
        CClaudeRunner::SetFollowSharedModels(false);
        CClaudeRunner runner;
        fnRun(runner, QStringLiteral("关闭轮"));
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_MODEL")).isEmpty(),
               QStringLiteral("跟随关不应注入 ANTHROPIC_MODEL"));
    }
    return g_iFail == 0 ? 0 : 1;
}
}   // namespace

int main(int a_iArgc, char* a_pArgv[]) {
    for (int i = 1; i < a_iArgc; ++i) {
        if (QLatin1String(a_pArgv[i]) == QLatin1String("-stub")) {
            return RunStubMode(a_iArgc, a_pArgv);
        }
    }
    QCoreApplication app(a_iArgc, a_pArgv);

    // 干净工作目录
    g_sWorkDir = QDir::tempPath() + QStringLiteral("/autocode-claude-test");
    QDir(g_sWorkDir).removeRecursively();
    QDir().mkpath(g_sWorkDir);

    g_pRunner = new CClaudeRunner(&app);
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigInfo, &app,
                     [](const QString& a_sLine) { g_out << "  [info] " << a_sLine << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigAssistantText, &app,
                     [](const QString& a_sText) { g_out << "  [assistant] " << a_sText << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigToolUse, &app,
                     [](const QString& a_sTool, const QString& a_sInput) {
                         g_out << "  [tool_use] " << a_sTool << " " << a_sInput.left(120) << Qt::endl; });
    QObject::connect(g_pRunner, &CAgentRunnerBase::sigContextFill, &app,
                     [](qint64 a_iFill) {
                         g_bCtxSeen = true;
                         g_iCtxLast = a_iFill;
                         g_out << "  [ctx_fill] " << a_iFill << Qt::endl;
                     });
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
            Fail(QStringLiteral("未成功完成（subtype=%1）").arg(g_pRunner->LastSubtype()));
            app.exit(1);
            return;
        }
        // 修复点：运行中必须收到过 sigContextFill(>0)。GLM 网关下 assistant 帧 usage
        //   全 0，该信号只能来自 message_delta 兜底；真 Anthropic 下 assistant 帧即可。
        if (!g_bCtxSeen) {
            Fail(QStringLiteral("运行中未收到任何 sigContextFill(>0)——GLM 网关 message_delta "
                                "兜底失效或端点 usage 全缺"));
        }
        if (g_iCtxLast != g_pRunner->LastContextFill()) {
            Fail(QStringLiteral("LastContextFill(%1) 与最后一次信号(%2)不一致")
                     .arg(g_pRunner->LastContextFill()).arg(g_iCtxLast));
        }
        if (g_pRunner->LastContextFill() <= 0) {
            Fail(QStringLiteral("收尾上下文占用 %1 <= 0").arg(g_pRunner->LastContextFill()));
        }
        if (g_pRunner->LastReqCount() < 1) {
            Fail(QStringLiteral("reqCount=0（assistant 帧未计数）"));
        }
        // 2026.09.23 修复点②：result 已无顶层 model 字段，model 应来自 init 行 /
        //   modelUsage 回退，收尾不得为空（窗口口径依赖它）。
        if (g_pRunner->LastModel().isEmpty()) {
            Fail(QStringLiteral("LastModel 为空（init/modelUsage 捕获失效）"));
        }
        g_out << "\n== 汇总: " << (g_iFail == 0 ? QStringLiteral("ALL PASS")
                                                : QStringLiteral("%1 FAIL").arg(g_iFail))
              << Qt::endl;
        app.exit(g_iFail == 0 ? 0 : 1);
    });

    g_pGuard = new QTimer(&app);
    g_pGuard->setSingleShot(true);
    QObject::connect(g_pGuard, &QTimer::timeout, &app, [&]() {
        Fail(QStringLiteral("超时（180s）"));
        g_pRunner->Stop();
        app.exit(1);
    });

    g_out << "== test_clauderunner 工作目录: " << g_sWorkDir << Qt::endl;
    g_pGuard->start(180000);
    g_pRunner->StartStage(g_sWorkDir,
                          QStringLiteral("只回复两个字母：OK。不要使用任何工具，不要输出其他内容。"),
                          QString(), 2, QString(), false, 0, false);
    return app.exec();
}
