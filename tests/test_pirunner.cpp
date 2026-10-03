// CPiAgentRunner 离线单元测试（不联网、不依赖真实 PIAgent 安装，挂 ctest）。
//
// 覆盖（针对 2026.08.19 第三后端接入）：
//   1. 工厂三元后端：BackendName/BackendDisplayName/IsPi 分派 + Create 返回 CPiAgentRunner。
//   2. 静态配置推导：root → cli.js 路径拼接、node 留空回退 PATH、LooksLikePiRoot 判定。
//   3. PiConfigError：根目录未配置/半安装（cli.js 缺失）时给出带指引的非空错误。
//   4. StartStage 前置校验：配置缺失 → 不拉进程直接 sigFinished(false)，且
//      session id 为空（m_bSawSession 修复：早期失败不误报上一轮旧 sid）。
//   5. Reset 语义：连续两次失败 StartStage 均不带 sid。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>   // 2026.09.19 Claude 自愈竞态回归：兑底超时

#include "agent/CAgentRunnerBase.h"
#include "agent/CAgentRunnerFactory.h"
#include "agent/CClaudeRunner.h"
#include "agent/CDeepSeekRunner.h"
#include "agent/CPiAgentRunner.h"

static QTextStream g_out(stdout);
static int g_iFail = 0;

static void Check(bool a_bCond, const QString& a_sWhat) {
    if (!a_bCond) {
        ++g_iFail;
        g_out << "  [FAIL] " << a_sWhat << Qt::endl;
    }
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // —— 1. 工厂三元后端 ——
    {
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendPi);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("pi"),
              QStringLiteral("BackendName(pi) 应为 pi"));
        Check(CAgentRunnerFactory::IsPi() && !CAgentRunnerFactory::IsDsh(),
              QStringLiteral("IsPi/IsDsh 判定错误"));
        Check(CAgentRunnerFactory::BackendDisplayName() == QStringLiteral("PIAgent"),
              QStringLiteral("BackendDisplayName 应为 PIAgent"));
        CAgentRunnerBase* p = CAgentRunnerFactory::Create();
        Check(p && p->BackendName() == QStringLiteral("pi"),
              QStringLiteral("Create() 应返回 pi 后端 runner"));
        Check(dynamic_cast<CPiAgentRunner*>(p) != nullptr,
              QStringLiteral("Create() 动态类型应为 CPiAgentRunner"));
        delete p;
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("claude"),
              QStringLiteral("BackendName(claude) 应为 claude"));
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendDsh);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("dsh"),
              QStringLiteral("BackendName(dsh) 应为 dsh"));
    }

    // —— 2/3. 静态配置推导 + PiConfigError（用临时目录构造半安装态）——
    QTemporaryDir oTmp;
    Check(oTmp.isValid(), QStringLiteral("临时目录创建失败"));
    const QString sFakeRoot = oTmp.path() + QStringLiteral("/PIAgent");
    QDir().mkpath(sFakeRoot + QStringLiteral("/pi/packages/coding-agent/dist"));
    {
        CPiAgentRunner::SetPiRootPath(QString());
        CPiAgentRunner::SetPiNodePath(QString());
        Check(CPiAgentRunner::ResolveNodePath() == QStringLiteral("node"),
              QStringLiteral("node 留空应回退 PATH 的 node"));
        Check(!CPiAgentRunner::LooksLikePiRoot(sFakeRoot),
              QStringLiteral("cli.js 缺失时 LooksLikePiRoot 应为 false"));
        Check(!CPiAgentRunner::PiConfigError().isEmpty(),
              QStringLiteral("半安装态 PiConfigError 应非空"));
        // 写入判定文件后 root 推导与判定应闭环。
        const QString sCli = sFakeRoot
            + QStringLiteral("/pi/packages/coding-agent/dist/cli.js");
        QFile fCli(sCli);
        Check(fCli.open(QIODevice::WriteOnly), QStringLiteral("写 cli.js 失败"));
        fCli.write(QStringLiteral("// stub\n").toUtf8());
        fCli.close();
        CPiAgentRunner::SetPiRootPath(sFakeRoot);
        Check(CPiAgentRunner::LooksLikePiRoot(sFakeRoot)
                  && !CPiAgentRunner::LooksLikePiRoot(QStringLiteral("Z:/no-such")),
              QStringLiteral("LooksLikePiRoot 判定错误"));
        Check(QDir::fromNativeSeparators(CPiAgentRunner::ResolveCliJsPath()) == sCli,
              QStringLiteral("root 推导 cli.js 不符: %1")
                  .arg(CPiAgentRunner::ResolveCliJsPath()));
        Check(!CPiAgentRunner::PiConfigError().isEmpty()
                  || !QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty(),
              QStringLiteral("（信息性）node 可用性"));
        // node 显式键无效路径必须报错。
        CPiAgentRunner::SetPiNodePath(sFakeRoot + QStringLiteral("/no-node.exe"));
        Check(!CPiAgentRunner::PiConfigError().isEmpty(),
              QStringLiteral("node 路径无效时 PiConfigError 应非空"));
        CPiAgentRunner::SetPiNodePath(QString());
    }

    // —— 4/5. StartStage 前置校验失败路径：不拉进程、sigFinished(false, sid 空) ——
    {
        CPiAgentRunner::SetPiRootPath(QString());   // 根目录未配置 → 前置校验失败
        CPiAgentRunner::SetPiNodePath(QStringLiteral("Z:/definitely/no/node.exe"));
        CPiAgentRunner runner;
        int iFinished = 0;
        QString sSid;
        bool bOk = true;
        QObject::connect(&runner, &CAgentRunnerBase::sigFinished,
                         [&](bool ok, const QString&, qint64, qint64, double,
                             const QString& sid) {
                             ++iFinished;
                             bOk = ok;
                             sSid = sid;
                             QCoreApplication::exit(0);
                         });
        // 连续两轮失败启动：均应立即失败且 sid 为空（验证 m_bSawSession 修复语义）。
        for (int i = 0; i < 2; ++i) {
            iFinished = 0;
            runner.StartStage(oTmp.path(), QStringLiteral("hello"),
                              QString(), 30, QString(), false, 0, false);
            Check(iFinished == 1,
                  QStringLiteral("第 %1 轮：配置缺失应同步发一次 sigFinished").arg(i + 1));
            Check(!bOk, QStringLiteral("第 %1 轮：bOk 应为 false").arg(i + 1));
            Check(sSid.isEmpty(),
                  QStringLiteral("第 %1 轮：早期失败 sid 应为空").arg(i + 1));
        }
        Check(!runner.IsRunning(), QStringLiteral("失败后 IsRunning 应为 false"));
        CPiAgentRunner::SetPiRootPath(QString());
    }

    // —— 6. 自定义大模型表（2026.08.20）：激活路由 + models.json 合成写 ——
    //   备份 ~/.pi/agent/models.json（存在时），测试后还原——只动 autocode-* 槽位也
    //   保平安（用户手写 provider 必须原样保留）。
    {
        // 2026.09.09 v2.1.133 测试隔离：models.json 指临时文件（v2.1.132 及此前直接写真实
        //   ~/.pi/agent/models.json——备份还原在并发/中断下三度把陈旧表带回现场）。
        const QString sRealJson = CPiAgentRunner::PiModelsJsonPath();
        const QString sJsonPath = QDir::temp().filePath(QStringLiteral("autocode-pi-models-test.json"));
        CPiAgentRunner::SetModelsJsonPathOverrideForTest(sJsonPath);
        QByteArray baBackup;
        const bool bHadFile = QFile::exists(sJsonPath);
        if (bHadFile) {
            QFile fBak(sJsonPath);
            fBak.open(QIODevice::ReadOnly);
            baBackup = fBak.readAll();
            fBak.close();
        }
        // 预置一个用户手写 provider：合成写后必须保留。
        {
            QDir().mkpath(QFileInfo(sJsonPath).absolutePath());
            QJsonObject oMyOllama;
            oMyOllama.insert(QStringLiteral("baseUrl"),
                             QStringLiteral("http://localhost:11434/v1"));
            QJsonObject oProvSeed;
            oProvSeed.insert(QStringLiteral("my-ollama"), oMyOllama);
            QJsonObject oSeed;
            oSeed.insert(QStringLiteral("providers"), oProvSeed);
            QFile fSeed(sJsonPath);
            fSeed.open(QIODevice::WriteOnly);
            fSeed.write(QJsonDocument(oSeed).toJson());
            fSeed.close();
        }
        QList<SDshModelEntry> aModels;
        SDshModelEntry stE;
        stE.sDisp    = QStringLiteral("公司中转 GLM-5.2");
        stE.sBaseURL = QStringLiteral("http://192.168.118.128:9000/claude");
        stE.sApiKey  = QStringLiteral("e94331c8-3aea-4914-bcce-af879209939a");
        stE.sApi     = QStringLiteral("anthropic-messages");
        stE.sModelId = QStringLiteral("glm-5.2");
        stE.iCtx     = 200000;
        stE.iMaxTok  = 32768;
        aModels.append(stE);
        SDshModelEntry stE2 = stE;
        stE2.sDisp    = QStringLiteral("本地 Ollama");
        stE2.sBaseURL = QStringLiteral("http://127.0.0.1:11434/v1");
        stE2.sApi     = QStringLiteral("openai-completions");
        stE2.sModelId = QStringLiteral("qwen2.5:14b");
        stE2.sApiKey.clear();
        aModels.append(stE2);

        Check(CPiAgentRunner::PiActiveProvider().isEmpty()
                  && CPiAgentRunner::PiActiveModelId().isEmpty(),
              QStringLiteral("无激活时 ActiveProvider/ModelId 应为空"));
        CPiAgentRunner::SetPiModels(aModels, 0);
        Check(CPiAgentRunner::PiActiveProvider() == QStringLiteral("autocode-1"),
              QStringLiteral("激活第 1 条 → provider 键 autocode-1"));
        Check(CPiAgentRunner::PiActiveModelId() == QStringLiteral("glm-5.2"),
              QStringLiteral("激活第 1 条 → 模型 glm-5.2"));
        CPiAgentRunner::SetPiModels(aModels, 1);
        Check(CPiAgentRunner::PiActiveProvider() == QStringLiteral("autocode-2"),
              QStringLiteral("激活第 2 条 → provider 键 autocode-2"));
        CPiAgentRunner::SetPiModels(aModels, -1);
        Check(CPiAgentRunner::PiActiveProvider().isEmpty(),
              QStringLiteral("取消激活 → provider 键为空"));

        // models.json 合成写：槽位/字段/保留外部 provider/空 key 占位。
        CPiAgentRunner::SetPiModels(aModels, 0);
        QFile fJson(sJsonPath);
        Check(fJson.open(QIODevice::ReadOnly), QStringLiteral("models.json 应已写盘"));
        const QJsonObject oRoot = QJsonDocument::fromJson(fJson.readAll()).object();
        fJson.close();
        const QJsonObject oProv =
            oRoot.value(QStringLiteral("providers")).toObject();
        Check(oProv.contains(QStringLiteral("my-ollama")),
              QStringLiteral("用户手写 provider 必须保留"));
        const QJsonObject oA1 =
            oProv.value(QStringLiteral("autocode-1")).toObject();
        Check(oA1.value(QStringLiteral("baseUrl")).toString()
                  == QStringLiteral("http://192.168.118.128:9000/claude"),
              QStringLiteral("autocode-1 baseUrl 不符"));
        Check(oA1.value(QStringLiteral("api")).toString()
                  == QStringLiteral("anthropic-messages"),
              QStringLiteral("autocode-1 api 应为 anthropic-messages"));
        Check(oA1.value(QStringLiteral("apiKey")).toString()
                  == QStringLiteral("e94331c8-3aea-4914-bcce-af879209939a"),
              QStringLiteral("autocode-1 apiKey 不符"));
        const QJsonArray aModels1 =
            oA1.value(QStringLiteral("models")).toArray();
        Check(aModels1.size() == 1
                  && aModels1.at(0).toObject().value(QStringLiteral("id")).toString()
                         == QStringLiteral("glm-5.2"),
              QStringLiteral("autocode-1 models[0].id 应为 glm-5.2"));
        const QJsonObject oA2 =
            oProv.value(QStringLiteral("autocode-2")).toObject();
        Check(oA2.value(QStringLiteral("apiKey")).toString()
                  == QStringLiteral("-"),
              QStringLiteral("空 key 应写占位 \"-\""));
        Check(oA2.value(QStringLiteral("api")).toString()
                  == QStringLiteral("openai-completions"),
              QStringLiteral("autocode-2 api 应为 openai-completions"));

        // 清空表 → autocode-* 槽位应被清掉（外部 provider 仍在）。
        CPiAgentRunner::SetPiModels(QList<SDshModelEntry>(), -1);
        QFile fJson2(sJsonPath);
        fJson2.open(QIODevice::ReadOnly);
        const QJsonObject oRoot2 = QJsonDocument::fromJson(fJson2.readAll()).object();
        fJson2.close();
        const QJsonObject oProv2 =
            oRoot2.value(QStringLiteral("providers")).toObject();
        Check(!oProv2.contains(QStringLiteral("autocode-1"))
                  && !oProv2.contains(QStringLiteral("autocode-2"))
                  && oProv2.contains(QStringLiteral("my-ollama")),
              QStringLiteral("清空表应清 autocode-* 槽位且保留外部 provider"));

        // 2026.09.09 v2.1.140 比对跳过：内容一致时 WritePiModelsJson 零写盘——
        //   只读属性拦截：未跳过则写路径必失败（truncate 打不开/原子替换 remove 不动
        //   只读目标）返回 false；跳过则不碰文件返回 true。
        CPiAgentRunner::SetPiModels(aModels, 0);
        QFile fBefore(sJsonPath);
        fBefore.open(QIODevice::ReadOnly);
        const QByteArray aBefore = fBefore.readAll();
        fBefore.close();
        QFile::setPermissions(sJsonPath,
            QFileDevice::Permissions(QFileDevice::ReadOwner | QFileDevice::ReadUser
                                     | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        Check(CPiAgentRunner::WritePiModelsJson(),
              QStringLiteral("内容一致时应跳过写盘（只读属性下仍返回 true）"));
        QFile::setPermissions(sJsonPath,
            QFileDevice::Permissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                     | QFileDevice::ReadUser | QFileDevice::WriteUser
                                     | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        QFile::remove(sJsonPath + QStringLiteral(".tmp"));   // 若未跳过可能残留 tmp
        QFile fAfter(sJsonPath);
        fAfter.open(QIODevice::ReadOnly);
        const QByteArray aAfter = fAfter.readAll();
        fAfter.close();
        Check(aAfter == aBefore, QStringLiteral("跳过写盘时文件字节应不变"));

        CPiAgentRunner::SetModelsJsonPathOverrideForTest(QString());   // 结束隔离
        // 真实 models.json 全程未触碰，无需还原（保留原块以防其他用例依赖）。
        if (false && bHadFile) {
            QFile fRest(sJsonPath);
            fRest.open(QIODevice::WriteOnly | QIODevice::Truncate);
            fRest.write(baBackup);
            fRest.close();
        } else {
            QFile::remove(sJsonPath);
        }
    }

    // —— 7. 2026.09.19 Claude 悬空 resume 自愈竞态（PixelPorter 助手2 实害回归） ——
    //   复现条件：result 错误事件到达时进程仍存活（真实 claude 在 stderr 报
    //   「No conversation found with session ID」后会短暂存活）。自愈路径 kill()
    //   在 Windows 上先触发 errorOccurred(Crashed) 后 finished——旧版
    //   slotOnErrorOccurred 守卫漏 m_bQuietRestart → BeginOrphanGrace（sid 已被自愈
    //   清空）→ EmitFailed「claude 进程崩溃＋旧进程 stderr 尾」抢在重拉之前宣告失败，
    //   「自动新建会话重发本轮」的承诺永远兑现不了。修复后：竞态被吞，剥 --resume 重拉
    //   真实发生（stub 第二轮再报同错 → 正常失败路径，真因保留）。
    {
        const QString sStub = oTmp.path() + QStringLiteral("/claude_stub.cmd");
        {
            QFile fStub(sStub);
            Check(fStub.open(QIODevice::WriteOnly), QStringLiteral("写 claude stub 失败"));
            // stderr 报悬空会话 + stream-json result 错误行后存活 ~5s
            //   （保证 result 处理时 kill() 撞上活进程，稳定触发竞态分支）。
            fStub.write(QStringLiteral("@echo off\r\n").toUtf8());
            fStub.write(QStringLiteral(
                "echo No conversation found with session ID: 01deadbeef-0000-0000-0000-000000000000 1>&2\r\n").toUtf8());
            fStub.write(QStringLiteral(
                "echo {\"type\":\"result\",\"subtype\":\"error_during_execution\","
                "\"is_error\":true,\"result\":\"\","
                "\"session_id\":\"01deadbeef-0000-0000-0000-000000000000\","
                "\"total_cost_usd\":0}\r\n").toUtf8());
            fStub.write(QStringLiteral("ping -n 6 127.0.0.1 >nul\r\n").toUtf8());
            fStub.write(QStringLiteral("exit /b 1\r\n").toUtf8());
            fStub.close();
        }
        CClaudeRunner::SetClaudeCmd(sStub);
        CClaudeRunner runner;
        QStringList aInfos;
        int iFinished = 0;
        QString sFinMsg;
        QObject::connect(&runner, &CAgentRunnerBase::sigInfo,
                         [&](const QString& s) { aInfos << s; });
        QObject::connect(&runner, &CAgentRunnerBase::sigFinished,
                         [&](bool, const QString& msg, qint64, qint64, double, const QString&) {
                             ++iFinished;
                             sFinMsg = msg;
                             QCoreApplication::exit(0);
                         });
        QTimer::singleShot(60000, []() { QCoreApplication::exit(2); });   // 兕底超时
        runner.StartStage(oTmp.path(), QStringLiteral("继续"), QString(), 30,
                          QStringLiteral("01deadbeef-0000-0000-0000-000000000000"),
                          false, 0, false);
        QCoreApplication::exec();
        const QString sJoined = aInfos.join(QStringLiteral("\n"));
        Check(iFinished == 1, QStringLiteral("应恰好一次收尾（竞态修复后不再双重失败），实际 %1")
                                   .arg(iFinished));
        Check(!sFinMsg.contains(QStringLiteral("进程崩溃")),
              QStringLiteral("自愈 kill 触发的 errorOccurred(Crashed) 应被吞——不得误报「进程崩溃」，实际：%1")
                  .arg(sFinMsg.left(120)));
        Check(sJoined.contains(QStringLiteral("会话在本机不存在")),
              QStringLiteral("应出现自愈提示（会话在本机不存在）"));
        Check(sJoined.contains(QStringLiteral("新建会话重发本轮")),
              QStringLiteral("剥 --resume 的重拉应真实发生（旧版被 EmitFailed 抢先短路）"));
        Check(sFinMsg.contains(QStringLiteral("No conversation found")),
              QStringLiteral("重拉后的失败仍应带真因（stub 二次报错），实际：%1")
                  .arg(sFinMsg.left(120)));
        CClaudeRunner::SetClaudeCmd(QStringLiteral("claude"));   // 复位全局静态
    }

    // —— 8. 2026.09.20 Claude 良性 stderr 过滤（MPlatform_QT 实害：GLM 网关别名
    //   每轮必报 unrecognized_model 但请求照常成功）：噪声行不上屏不入尾，
    //   其余 stderr 照旧透传，收尾成功不受影响。——
    {
        const QString sStub2 = oTmp.path() + QStringLiteral("/claude_stub2.cmd");
        {
            QFile fStub(sStub2);
            Check(fStub.open(QIODevice::WriteOnly), QStringLiteral("写 claude stub2 失败"));
            fStub.write(QStringLiteral("@echo off\r\n").toUtf8());
            // 噪声行 + 一条正常 stderr（应保留） + 成功 result。
            fStub.write(QStringLiteral(
                "echo [claude-code:unrecognized_model] {\"model\":\"glm-5.3[1m]\",\"query_source\":\"sdk\"} 1>&2\r\n").toUtf8());
            fStub.write(QStringLiteral(
                "echo [claude-code:some_other_diag] kept 1>&2\r\n").toUtf8());
            fStub.write(QStringLiteral(
                "echo {\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
                "\"result\":\"stub 完成\",\"session_id\":\"stub2-sid\",\"total_cost_usd\":0}\r\n").toUtf8());
            fStub.close();
        }
        CClaudeRunner::SetClaudeCmd(sStub2);
        CClaudeRunner runner2;
        QStringList aInfos2;
        int iFinished2 = 0;
        bool bOk2 = false;
        QObject::connect(&runner2, &CAgentRunnerBase::sigInfo,
                         [&](const QString& s) { aInfos2 << s; });
        QObject::connect(&runner2, &CAgentRunnerBase::sigFinished,
                         [&](bool ok, const QString&, qint64, qint64, double, const QString&) {
                             ++iFinished2;
                             bOk2 = ok;
                             QCoreApplication::exit(0);
                         });
        QTimer::singleShot(30000, []() { QCoreApplication::exit(2); });
        runner2.StartStage(oTmp.path(), QStringLiteral("你好"), QString(), 30,
                          QString(), false, 0, false);
        QCoreApplication::exec();
        Check(iFinished2 == 1 && bOk2,
              QStringLiteral("良性噪声过滤不应影响收尾成功"));
        const QString sJoined2 = aInfos2.join(QStringLiteral("\n"));
        Check(!sJoined2.contains(QStringLiteral("unrecognized_model")),
              QStringLiteral("unrecognized_model 噪声行不应上屏"));
        Check(sJoined2.contains(QStringLiteral("[claude-code:some_other_diag]")),
              QStringLiteral("其余 stderr 诊断行应照旧透传"));
        CClaudeRunner::SetClaudeCmd(QStringLiteral("claude"));
    }

    // —— 9. 2026.09.22 Pi RPC 形态端到端（真 node + JS stub cli.js，不联网）：
    //   A. 能力探测（cli.js 同级 modes/rpc/rpc-mode.js 标记命中/缺失）；
    //   B. RPC 正常轮：get_state 回读 sid、prompt 经命令帧投递、轮中 steer 立即
    //      生效（stub 等到 steer 才 settle——验「注入到达正在运行的轮」而非排队）、
    //      agent_settled 优雅退出 + 成功收尾；
    //   C. RPC 早夭自动回退：stub 模拟旧版不认识 --mode rpc（未见会话即非零早退）→
    //      当轮自动重拉一次性形态成功、提示「自动回退」、一次性路径收 session 头 sid。
    {
        if (QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            g_out << "  [SKIP] 9. Pi RPC e2e（本机无 node）" << Qt::endl;
        } else {
            const QString sRealRoot = CPiAgentRunner::PiRootPath();
            const QString sRealNode = CPiAgentRunner::PiNodePath();
            CPiAgentRunner::ResetRpcCapCacheForTest();
            // 沙箱安装树：<tmp>/rpc_root/PIAgent/pi/packages/coding-agent/dist/
            //   {cli.js（stub，源码见 build/pi_rpc_stub.cli.js）,
            //    modes/rpc/rpc-mode.js（探测标记）}。
            const QString sRoot = oTmp.path() + QStringLiteral("/rpc_root");
            const QString sDist = sRoot
                + QStringLiteral("/pi/packages/coding-agent/dist");
            QDir().mkpath(sDist + QStringLiteral("/modes/rpc"));
            const QString sCliJs = sDist + QStringLiteral("/cli.js");
            const QString sMark = sDist + QStringLiteral("/modes/rpc/rpc-mode.js");
            const QString sStubJs = QStringLiteral(R"PISTUB(#!/usr/bin/env node
const fs = require('fs');
const args = process.argv.slice(2);
const out = o => process.stdout.write(JSON.stringify(o) + '\n');
let buf = '';
if (args.includes('rpc')) {
  // —— RPC 形态 stub：break_rpc.flag 存在时模拟旧版不认识 --mode（早退非零）——
  if (fs.existsSync(__dirname + '/break_rpc.flag')) {
    process.stderr.write("error: unknown option '--mode'\n");
    process.exit(1);
  }
  let steered = false, prompted = false;
  const finish = () => {
    out({type: 'queue_update', steering: [], followUp: []});
    out({type: 'message_end', message: {role: 'assistant', stopReason: 'stop',
      model: 'stub-glm', usage: {input: 50, output: 20, cacheRead: 0, cacheWrite: 0},
      content: [{type: 'text', text: '最终回复（含追加）'}]}});
    out({type: 'turn_end'});
    out({type: 'agent_end', messages: [], willRetry: false});
    out({type: 'agent_settled'});
  };
  process.stdin.on('data', d => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i); buf = buf.slice(i + 1);
      if (!line.trim()) continue;
      let c; try { c = JSON.parse(line); } catch { continue; }
      if (c.type === 'get_state') {
        out({id: c.id, type: 'response', command: 'get_state', success: true,
             data: {sessionId: 'rpcstub-sid-0001', isStreaming: prompted}});
      } else if (c.type === 'prompt') {
        prompted = true;
        fs.writeFileSync(__dirname + '/rpc_prompt.txt', c.message);
        out({id: c.id, type: 'response', command: 'prompt', success: true});
        out({type: 'agent_start'});
        out({type: 'turn_start'});
        out({type: 'message_end', message: {role: 'assistant', stopReason: 'stop',
          model: 'stub-glm', usage: {input: 100, output: 10, cacheRead: 5, cacheWrite: 0},
          content: [{type: 'text', text: '处理中'}]}});
        // 不 settle：等 steer 到达才收尾（验注入到达正在运行的轮）
        if (steered) finish();
      } else if (c.type === 'steer') {
        steered = true;
        fs.writeFileSync(__dirname + '/steer_seen.txt', c.message);
        out({type: 'queue_update', steering: [c.message], followUp: []});
        if (prompted) finish();
      }
    }
  });
  process.stdin.on('end', () => process.exit(0));
  process.stdin.resume();
} else if (args.includes('-p')) {
  // —— 一次性形态 stub（回退路径）——
  let all = '';
  process.stdin.on('data', d => all += d.toString());
  process.stdin.on('end', () => {
    fs.writeFileSync(__dirname + '/oneshot_prompt.txt', all);
    out({type: 'session', id: 'oneshot-sid-0002'});
    out({type: 'message_end', message: {role: 'assistant', stopReason: 'stop',
      model: 'stub-glm', usage: {input: 30, output: 8, cacheRead: 0, cacheWrite: 0},
      content: [{type: 'text', text: '一次性回退成功'}]}});
    out({type: 'agent_end', messages: []});
    process.exit(0);
  });
  process.stdin.resume();
}
)PISTUB");
            {
                QFile fCli(sCliJs);
                Check(fCli.open(QIODevice::WriteOnly),
                      QStringLiteral("写 rpc stub cli.js 失败"));
                fCli.write(sStubJs.toUtf8());
                fCli.close();
            }
            auto fnWriteMark = [&sMark]() {
                QFile fMark(sMark);
                fMark.open(QIODevice::WriteOnly);
                fMark.write("// marker\n");
                fMark.close();
            };
            fnWriteMark();
            CPiAgentRunner::SetPiRootPath(sRoot);
            CPiAgentRunner::SetPiNodePath(QString());   // 走 PATH 的 node

            // A. 能力探测：标记文件命中/缺失两态。
            Check(CPiAgentRunner::RpcCapable(),
                  QStringLiteral("标记文件存在时 RpcCapable 应为 true"));
            QFile::remove(sMark);
            CPiAgentRunner::ResetRpcCapCacheForTest();
            Check(!CPiAgentRunner::RpcCapable(),
                  QStringLiteral("标记文件移除后 RpcCapable 应为 false"));
            fnWriteMark();
            CPiAgentRunner::ResetRpcCapCacheForTest();

            // B. RPC 正常轮 + 轮中注入（stub 收到 steer 才 settle）。
            {
                CPiAgentRunner runner;
                int iFin = 0;
                bool bOk = false;
                bool bInjected = false;
                QString sMsg, sSid;
                QObject::connect(&runner, &CAgentRunnerBase::sigAssistantText,
                                 [&](const QString& s) {
                    // 首条 assistant 文本 = 轮进行中 → 注入（一次性形态此处恒 false）。
                    if (!bInjected && s == QStringLiteral("处理中")) {
                        bInjected = runner.InjectUserTurn(
                            QStringLiteral("追加：按新增口径完成"));
                    }
                });
                QObject::connect(&runner, &CAgentRunnerBase::sigFinished,
                                 [&](bool ok, const QString& msg, qint64, qint64,
                                    double, const QString& sid) {
                    ++iFin;
                    bOk = ok;
                    sMsg = msg;
                    sSid = sid;
                    QCoreApplication::exit(0);
                });
                QTimer::singleShot(60000, []() { QCoreApplication::exit(2); });
                runner.StartStage(oTmp.path(), QStringLiteral("RPC注入测试"),
                                  QString(), 30, QString(), false, 0, false);
                QCoreApplication::exec();
                Check(iFin == 1 && bOk,
                      QStringLiteral("RPC 轮应成功收尾（iFin=%1）").arg(iFin));
                Check(sMsg.contains(QStringLiteral("最终回复（含追加）")),
                      QStringLiteral("收尾文本应为 settle 后末条 assistant，实际：%1")
                          .arg(sMsg.left(80)));
                Check(sSid == QStringLiteral("rpcstub-sid-0001"),
                      QStringLiteral("RPC 会话 id 应经 get_state 回读，实际：%1").arg(sSid));
                Check(bInjected,
                      QStringLiteral("轮中 InjectUserTurn 应成功（steer 帧已投递）"));
                QFile fSteer(sDist + QStringLiteral("/steer_seen.txt"));
                const bool bSteerSeen = fSteer.open(QIODevice::ReadOnly)
                    && QString::fromUtf8(fSteer.readAll())
                           .contains(QStringLiteral("按新增口径完成"));
                fSteer.close();
                Check(bSteerSeen,
                      QStringLiteral("stub 应在轮运行中收到 steer 帧（非排队下一轮）"));
                QFile fPrompt(sDist + QStringLiteral("/rpc_prompt.txt"));
                const bool bPromptSeen = fPrompt.open(QIODevice::ReadOnly)
                    && QString::fromUtf8(fPrompt.readAll())
                           .contains(QStringLiteral("RPC注入测试"));
                fPrompt.close();
                Check(bPromptSeen,
                      QStringLiteral("prompt 应经 RPC 命令帧投递（message 字段）"));
            }

            // C. RPC 早夭自动回退（模拟旧版不认识 --mode rpc）。
            {
                QFile fBreak(sDist + QStringLiteral("/break_rpc.flag"));
                fBreak.open(QIODevice::WriteOnly);
                fBreak.write("1");
                fBreak.close();
                CPiAgentRunner::ResetRpcCapCacheForTest();
                CPiAgentRunner runner;
                int iFin = 0;
                bool bOk = false;
                QString sMsg, sSid;
                QStringList aInfos;
                QObject::connect(&runner, &CAgentRunnerBase::sigInfo,
                                 [&](const QString& s) { aInfos << s; });
                QObject::connect(&runner, &CAgentRunnerBase::sigFinished,
                                 [&](bool ok, const QString& msg, qint64, qint64,
                                    double, const QString& sid) {
                    ++iFin;
                    bOk = ok;
                    sMsg = msg;
                    sSid = sid;
                    QCoreApplication::exit(0);
                });
                QTimer::singleShot(60000, []() { QCoreApplication::exit(2); });
                runner.StartStage(oTmp.path(), QStringLiteral("RPC回退测试"),
                                  QString(), 30, QString(), false, 0, false);
                QCoreApplication::exec();
                Check(iFin == 1 && bOk,
                      QStringLiteral("早夭后应自动回退一次性形态并成功（iFin=%1）")
                          .arg(iFin));
                Check(aInfos.join(QStringLiteral("\n"))
                          .contains(QStringLiteral("自动回退一次性模式")),
                      QStringLiteral("应提示自动回退一次性模式"));
                Check(sMsg.contains(QStringLiteral("一次性回退成功")),
                      QStringLiteral("回退路径收尾文本应来自一次性 stub，实际：%1")
                          .arg(sMsg.left(80)));
                Check(sSid == QStringLiteral("oneshot-sid-0002"),
                      QStringLiteral("回退路径会话 id 应来自 session 头，实际：%1")
                          .arg(sSid));
                QFile fPrompt(sDist + QStringLiteral("/oneshot_prompt.txt"));
                const bool bPromptSeen = fPrompt.open(QIODevice::ReadOnly)
                    && QString::fromUtf8(fPrompt.readAll())
                           .contains(QStringLiteral("RPC回退测试"));
                fPrompt.close();
                Check(bPromptSeen,
                      QStringLiteral("回退重拉应投递原 prompt（stdin 原文）"));
                QFile::remove(sDist + QStringLiteral("/break_rpc.flag"));
            }

            // 复位全局静态（后续用例/真实进程不受沙箱影响）。
            CPiAgentRunner::SetPiRootPath(sRealRoot);
            CPiAgentRunner::SetPiNodePath(sRealNode);
            CPiAgentRunner::ResetRpcCapCacheForTest();
        }
    }

    g_out << (g_iFail == 0 ? "test_pirunner PASS" : "test_pirunner FAIL") << Qt::endl;
    return g_iFail == 0 ? 0 : 1;
}
