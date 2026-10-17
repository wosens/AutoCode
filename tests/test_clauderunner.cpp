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
#include <QJsonDocument>
#include <QJsonObject>

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
    const QString sPathTxt = sDir + QStringLiteral("/path.txt");
    const QString sArgsTxt = sDir + QStringLiteral("/args.txt");
    const QString sAuthStub = sDir + QStringLiteral("/claude_auth_stub.cmd");
    {   // 2026.10.11 鉴权失败诊断 stub：回一条含鉴权错误的 assistant 文本，验证
        //   诊断事件行（注入 key 指纹/非法字符提示）被追发。
        QFile f(sAuthStub);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write(QStringLiteral("echo {\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Failed to authenticate. API Error: 401 Invalid API key format\"}]}}\n").toUtf8());
        f.write(QStringLiteral("echo {\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\",\"session_id\":\"stub-sid-auth\",\"total_cost_usd\":0,\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}\n").toUtf8());
        f.close();
    }
    {
        QFile f(sStub);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write(QStringLiteral("set ANTHROPIC > \"%1\"\n")
                    .arg(sEnvTxt).toUtf8());
        // 2026.10.10 合并 PATH 回归：落盘子进程实际拿到的 PATH。
        f.write(QStringLiteral("set PATH > \"%1\"\n")
                    .arg(sPathTxt).toUtf8());
        // 2026.10.10 --settings 注入回归：落盘完整命令行（验证本轮启动参数携带
        //   本轮刚生成的临时 settings 文件，修「成员被 Reset 先清空、aArgs 预取
        //   恒空、文件永不传给 claude」的一轮滞后）。
        f.write(QStringLiteral("echo %* > \"%1\"\n")
                    .arg(sArgsTxt).toUtf8());
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
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL")) == QStringLiteral("ov-claude")
                   && fnEnvOf(QStringLiteral("ANTHROPIC_DEFAULT_HAIKU_MODEL")) == QStringLiteral("ov-claude")
                   && fnEnvOf(QStringLiteral("ANTHROPIC_DEFAULT_OPUS_MODEL")) == QStringLiteral("ov-claude"),
               QStringLiteral("辅助档位三 env 应同源注入（子代理/后台任务不走官方模型名）"));
        Check2(fnEnvOf(QStringLiteral("ANTHROPIC_AUTH_TOKEN")) == QStringLiteral("ov-key"),
               QStringLiteral("AUTH_TOKEN 应同值双注入（Bearer 类中转兼容）"));
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
    {   // H/I. --settings 注入回归（2026.10.10 修一轮滞后）：本轮启动参数必须携带本轮
        //    刚生成的临时 settings 文件（此前 Reset 先清空成员、aArgs 预取恒空，文件
        //    永不传给 claude → env 注入被用户 settings.json 顶掉，改了网关仍连旧地址）。
        auto fnArgsOf = [&sArgsTxt]() {
            QFile f(sArgsTxt);
            const QString s = f.open(QIODevice::ReadOnly)
                ? QString::fromLocal8Bit(f.readAll()) : QString();
            f.close();
            return s;
        };
        auto fnFileJson = [](const QString& a_sArgsTxt) {
            // 从落盘命令行里抠 --settings 后面的路径，读回 JSON env 块。
            QFile f(a_sArgsTxt);
            const QString sCmd = f.open(QIODevice::ReadOnly)
                ? QString::fromLocal8Bit(f.readAll()) : QString();
            f.close();
            const int iFlag = sCmd.indexOf(QLatin1String("--settings"));
            if (iFlag < 0) { return QJsonObject(); }
            const QString sRest = sCmd.mid(iFlag + 10).trimmed();
            const int iEnd = sRest.indexOf(QLatin1Char(' '));
            QString sPath = (iEnd < 0 ? sRest : sRest.left(iEnd)).trimmed();
            if (sPath.startsWith(QLatin1Char('"')) && sPath.endsWith(QLatin1Char('"'))) {
                sPath = sPath.mid(1, sPath.size() - 2);
            }
            QFile fSet(sPath);
            if (!fSet.open(QIODevice::ReadOnly)) { return QJsonObject(); }
            const QJsonDocument oDoc = QJsonDocument::fromJson(fSet.readAll());
            fSet.close();
            return oDoc.object().value(QStringLiteral("env")).toObject();
        };
        CClaudeRunner::SetFollowSharedModels(true);
        {   // H. 跟随轮启动参数应携带 --settings <本轮文件>，内容=当前条目
            CClaudeRunner runner;
            fnRun(runner, QStringLiteral("settings轮"));
            const QJsonObject oEnvSet = fnFileJson(sArgsTxt);
            Check2(!oEnvSet.isEmpty(),
                   QStringLiteral("本轮启动参数应携带 --settings 且文件可读回"));
            Check2(oEnvSet.value(QStringLiteral("ANTHROPIC_BASE_URL")).toString()
                       == QStringLiteral("http://127.0.0.1:9/g"),
                   QStringLiteral("settings 文件 BASE_URL 应=当前条目（不滞后）"));
            Check2(oEnvSet.value(QStringLiteral("ANTHROPIC_AUTH_TOKEN")).toString()
                       == QStringLiteral("g-key"),
                   QStringLiteral("settings 文件应含 AUTH_TOKEN（Bearer 类中转兼容）"));
        }
        {   // I. 改地址同轮生效（核心回归）：换激活条目内容 → 下一轮 --settings 文件
            //    立即携带新 URL（不再传上一轮旧文件）。
            aTbl[0].sBaseURL = QStringLiteral("http://127.0.0.1:9/g2-new");
            aTbl[0].sModelId = QStringLiteral("g-claude-2");
            CClaudeRunner::SetSharedModelTable(aTbl, /*active=*/0);
            CClaudeRunner runner;
            fnRun(runner, QStringLiteral("改地址轮"));
            const QJsonObject oEnvSet = fnFileJson(sArgsTxt);
            Check2(oEnvSet.value(QStringLiteral("ANTHROPIC_BASE_URL")).toString()
                       == QStringLiteral("http://127.0.0.1:9/g2-new"),
                   QStringLiteral("改地址后本轮 --settings 应立即携带新 URL"));
            Check2(oEnvSet.value(QStringLiteral("ANTHROPIC_MODEL")).toString()
                       == QStringLiteral("g-claude-2"),
                   QStringLiteral("改地址后本轮 --settings 应立即携带新模型名"));
            // env 注入同源（双通道一致）。
            Check2(fnEnvOf(QStringLiteral("ANTHROPIC_BASE_URL")).contains(QStringLiteral("g2-new")),
                   QStringLiteral("改地址后 env 注入应同步新 URL"));
            aTbl[0].sBaseURL = QStringLiteral("http://127.0.0.1:9/g");
            aTbl[0].sModelId = QStringLiteral("g-claude");
            CClaudeRunner::SetSharedModelTable(aTbl, 0);   // 还原
        }
        {   // J. 鉴权失败自诊断（2026.10.11）：claude 回鉴权错误文本 → 追发诊断行
            //    （含注入 key 指纹）；Key 含非法字符时注入后即预警。
            CClaudeRunner::SetClaudeCmd(sAuthStub);
            QString sDiagLine, sPreLine;
            {
                CClaudeRunner runner;
                QObject::connect(&runner, &CAgentRunnerBase::sigInfo, &runner,
                                 [&](const QString& a_s) {
                                     if (a_s.contains(QStringLiteral("鉴权失败诊断"))) {
                                         sDiagLine = a_s;
                                     }
                                     if (a_s.contains(QStringLiteral("网关直探"))) {
                                         sDiagLine += QStringLiteral(" | ") + a_s;
                                     }
                                 });
                fnRun(runner, QStringLiteral("鉴权诊断轮"));
                Check2(sDiagLine.contains(QStringLiteral("长度 5"))
                           && sDiagLine.contains(QStringLiteral("g-ke…-key"))
                           && sDiagLine.contains(QStringLiteral("格式合法")),
                       QStringLiteral("鉴权失败应追发诊断（key 指纹/格式）实际：%1")
                           .arg(sDiagLine.left(160)));
                // 2026.10.11 网关直探：诊断后异步 POST 注入的接入点（stub 表指向
                //   127.0.0.1:9 → 连接拒绝快速失败），事件循环退出前应收到探测结果行。
                QTimer::singleShot(6000, []() { QCoreApplication::exit(0); });
                QCoreApplication::exec();
                Check2(sDiagLine.contains(QStringLiteral("网关直探"))
                           && sDiagLine.contains(QStringLiteral("127.0.0.1:9")),
                       QStringLiteral("诊断应附带网关直探结果行实际：%1")
                           .arg(sDiagLine.left(200)));
            }
            {   // K. 注入 Key 含非法字符（Bearer 前缀）→ 注入即预警
                aTbl[0].sApiKey = QStringLiteral("Bearer e943-abc");
                CClaudeRunner::SetSharedModelTable(aTbl, 0);
                QString sWarnLine;
                CClaudeRunner runner;
                QObject::connect(&runner, &CAgentRunnerBase::sigInfo, &runner,
                                 [&](const QString& a_s) {
                                     if (a_s.contains(QStringLiteral("注入的 Key 含非法字符"))) {
                                         sWarnLine = a_s;
                                     }
                                 });
                fnRun(runner, QStringLiteral("非法 Key 轮"));
                Check2(sWarnLine.contains(QStringLiteral("401 Invalid API key format")),
                       QStringLiteral("非法 Key 应注入即预警实际：%1").arg(sWarnLine.left(120)));
                aTbl[0].sApiKey = QStringLiteral("g-key");
                CClaudeRunner::SetSharedModelTable(aTbl, 0);   // 还原
            }
            CClaudeRunner::SetClaudeCmd(sStub);
        }
    }
    {   // D. 子进程 PATH = 注册表合并环境（2026.10.10 MergedPathEnvironment 回归）：
        //    stub 轮实际拿到的 PATH 应与 MergedPathEnvironment() 计算值一致，
        //    且包含 System32（合并后命令搜索域完整的基本健全性）。
        QFile f(sPathTxt);
        const bool bOpen = f.open(QIODevice::ReadOnly);
        QString sSubPath;
        if (bOpen) {
            // cmd `set PATH >` 按控制台码页（本地 8 位）落盘，且 set 为前缀匹配会把
            // PATHEXT 一并列出——只取首行再剥 PATH= 前缀。
            sSubPath = QString::fromLocal8Bit(f.readAll());
            f.close();
            const int iNl = sSubPath.indexOf(QLatin1Char('\n'));
            if (iNl >= 0) {
                sSubPath = sSubPath.left(iNl);
            }
            if (sSubPath.startsWith(QStringLiteral("PATH="), Qt::CaseInsensitive)) {
                sSubPath = sSubPath.mid(5);
            }
            sSubPath = sSubPath.trimmed();
        }
        const QString sMerged = CAgentRunnerBase::MergedPathEnvironment()
                                    .value(QStringLiteral("PATH"));
        Check2(bOpen && sSubPath == sMerged,
               QStringLiteral("子进程 PATH 应等于合并环境 PATH"));
        Check2(sSubPath.contains(QStringLiteral("System32"), Qt::CaseInsensitive),
               QStringLiteral("合并 PATH 应含 System32"));
    }
    {   // E. 裸命令名预检（2026.10.10）：命令不在（含注册表合并后的）PATH 且兕底探测
        //    落空（测试注入恒空）→ StartStage 同步立即可操作报错（含「未在 PATH 中找到」
        //    指引），不进三连快速失败重试、不落地启动。
        CClaudeRunner::s_fnProbeFallback = [](QString*) { return QString(); };
        CClaudeRunner::SetClaudeCmd(QStringLiteral("autocode_missing_claude_cmd_xyz"));
        CClaudeRunner runner;
        QString sFailMsg;
        QObject::connect(&runner,
                         qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                             &CAgentRunnerBase::sigFinished),
                         &runner, [&](bool, const QString& a_sMsg, qint64, qint64, double,
                                      const QString&) { sFailMsg = a_sMsg; });
        runner.StartStage(sDir, QStringLiteral("预检轮"), QString(), 40,
                          QString(), false, 0, false);
        // EmitFailed 同步发射（同线程直连），StartStage 返回即已送达。
        Check2(sFailMsg.contains(QStringLiteral("未在 PATH 中找到")),
               QStringLiteral("预检应报「未在 PATH 中找到」实际报：%1")
                   .arg(sFailMsg.left(120)));
        Check2(sFailMsg.contains(QStringLiteral("环境诊断")) && sFailMsg.contains(QStringLiteral("npm prefix")),
               QStringLiteral("报错应附环境诊断（node/npm/prefix/where 现场一次性收集）"));
        CClaudeRunner::s_fnProbeFallback = nullptr;   // 钩子还原
    }
    {   // F. 绝对路径预检（2026.10.10）：配置路径不存在且兕底探测落空（注入恒空）→
        //    同步精确报错（含「路径不存在」），不再让 cmd 甩模糊的「系统找不到指定的路径」。
        CClaudeRunner::s_fnProbeFallback = [](QString*) { return QString(); };
        CClaudeRunner::SetClaudeCmd(QStringLiteral("C:/no_such_dir_xyz/claude.exe"));
        CClaudeRunner runner;
        QString sFailMsg;
        QObject::connect(&runner,
                         qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                             &CAgentRunnerBase::sigFinished),
                         &runner, [&](bool, const QString& a_sMsg, qint64, qint64, double,
                                      const QString&) { sFailMsg = a_sMsg; });
        runner.StartStage(sDir, QStringLiteral("路径预检轮"), QString(), 40,
                          QString(), false, 0, false);
        Check2(sFailMsg.contains(QStringLiteral("路径不存在")),
               QStringLiteral("绝对路径预检应报「路径不存在」实际报：%1")
                   .arg(sFailMsg.left(120)));
        CClaudeRunner::s_fnProbeFallback = nullptr;   // 钩子还原
    }
    {   // G. 兕底探测自动切换（2026.10.10）：配置路径失效但探测命中（注入返回 stub 命令）
        //    → 不报错、事件行含「已自动探测到可用安装」、替换后整轮 stub 照常跑通。
        CClaudeRunner::s_fnProbeFallback = [&sStub](QString* a_psFrom) {
            if (a_psFrom != nullptr) { *a_psFrom = QStringLiteral("测试注入"); }
            return sStub;
        };
        CClaudeRunner::SetClaudeCmd(QStringLiteral("C:/no_such_dir_xyz/claude.exe"));
        CClaudeRunner runner;
        QString sSysLine;
        bool bFailed = false;
        QObject::connect(&runner, &CAgentRunnerBase::sigInfo, &runner,
                         [&](const QString& a_s) { if (a_s.contains(QStringLiteral("已自动探测到可用安装"))) sSysLine = a_s; });
        QObject::connect(&runner,
                         qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                             &CAgentRunnerBase::sigFinished),
                         &runner, [&](bool a_bOk, const QString&, qint64, qint64, double,
                                      const QString&) { if (!a_bOk) bFailed = true; });
        runner.StartStage(sDir, QStringLiteral("兕底轮"), QString(), 40,
                          QString(), false, 0, false);
        Check2(!sSysLine.isEmpty() && sSysLine.contains(QStringLiteral("测试注入")),
               QStringLiteral("兕底轮应发自动切换事件行（含来源）实际：%1")
                   .arg(sSysLine.left(100)));
        Check2(!bFailed, QStringLiteral("兕底切换后 stub 轮不应失败"));
        CClaudeRunner::s_fnProbeFallback = nullptr;   // 钩子还原
    }
    {   // I. 共享表条目 iCtx 窗口优先（2026-10-14/15）：同名多级条目按槽位/激活项取值；
        //    无条目/条目 iCtx 非法（≤0）→ 名字探测兑底（glm=128K）。
        {
            SDshModelEntry stA;   // M0：激活条目，glm-5.3 设 990K
            stA.sDisp = QStringLiteral("glm-5.3");
            stA.sModelId = QStringLiteral("glm-5.3");
            stA.iCtx = 990000;
            SDshModelEntry stB;   // M1：同名副本条目，glm-5.3 设 1M
            stB.sDisp = QStringLiteral("glm-5.3-副本");
            stB.sModelId = QStringLiteral("glm-5.3");
            stB.iCtx = 1000000;
            SDshModelEntry stC;   // M2：不同模型
            stC.sDisp = QStringLiteral("glm-4.6");
            stC.sModelId = QStringLiteral("glm-4.6");
            stC.iCtx = 200000;
            CClaudeRunner::SetSharedModelTable({ stA, stB, stC }, 0);
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("autocode-2/glm-5.3")) == 1000000,
                   QStringLiteral("槽位前缀命中第 2 条（1M 副本）应返回 1000000"));
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("autocode-1/glm-5.3")) == 990000,
                   QStringLiteral("槽位前缀命中第 1 条（激活 990K）应返回 990000"));
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("glm-5.3")) == 990000,
                   QStringLiteral("无前缀 → 激活条目优先（990K 而非首个之外的同名 1M）"));
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("autocode-9/glm-5.3")) == 990000,
                   QStringLiteral("槽位越界 → 退回激活/同名兑底（990K）"));
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("claude-sonnet-4-5")) == 200000,
                   QStringLiteral("claude 不在表内 → 兑底 200K 不受条目影响"));
            stA.iCtx = 0;   // 激活条目非法 + 无前缀 → 取同名下一条有效值
            CClaudeRunner::SetSharedModelTable({ stA, stB, stC }, 0);
            Check2(CClaudeRunner::ContextWindowForModel(
                       QStringLiteral("glm-5.3")) == 1000000,
                   QStringLiteral("激活条目 iCtx≤0 → 同名下一条有效值 1M"));
        }
    }
    CClaudeRunner::SetClaudeCmd(sStub);   // 恢复正常 stub 配置（后续场景/再入安全）
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
