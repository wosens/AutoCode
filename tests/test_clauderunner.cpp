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

int main(int a_iArgc, char* a_pArgv[]) {
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
