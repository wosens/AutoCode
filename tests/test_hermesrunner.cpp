// Hermes 第五后端离线单元测试（任务 3213cdbf，挂 ctest，offscreen 无头可跑）。
// 覆盖设计 AC 可自动化部分：
//   AC2  Agent Backend 扩 1..5：5 存取往返；越界（0/6）兜底回 1（非 qBound 夹边界）。
//   AC1  [Agent] Hermes* 键 + [HermesModels] 表往返（中文值 GBK 安全，Profile API）。
//   AC3  contextWindow/maxTokens 钳位（200000 / 131072）双侧收口。
//   AC4  Hermes 写入不破坏 config.ini 既有中文值（字节级不动其他节）。
//   FR6  EnsureRuntimeConfig 幂等（两次调用产物逐字节一致，注入独立 home 目录）。
//   AC5  stub CLI（node 跑临时 dist/cli.js，吐固定 NDJSON 事件流）→ 会话/assistant/
//        usage/结束成功判定。本机无 node 时跳过该用例（CI 兜底）。
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QDateTime>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include <windows.h>

#include "agent/CHermesRunner.h"
#include "agent/CAgentRunnerFactory.h"
#include "project/CProjectManager.h"

class CTestHermesRunner : public QObject {
    Q_OBJECT

private:
    // 测试 config.ini 与 CProjectManager::ConfigFilePath 同口径：exe 同级（build 目录
    //   内，与开发机 AutoCode/config.ini 隔离），init/cleanup 前后清场。
    static QString ConfigPath() {
        const QString sExe = QCoreApplication::applicationFilePath();
        return QDir::cleanPath(QFileInfo(sExe).absolutePath() + QStringLiteral("/config.ini"));
    }

    static QByteArray ReadConfigBytes() {
        QFile f(ConfigPath());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static void WriteRawBackend(int a_iVal) {
        const std::wstring wsPath = ConfigPath().toStdWString();
        WritePrivateProfileStringW(L"Agent", L"Backend",
            QString::number(a_iVal).toStdWString().c_str(), wsPath.c_str());
    }

    // 找本机 node（PATH 优先）。
    static QString FindNode() {
        QProcess stW;
        stW.start(QStringLiteral("where"), { QStringLiteral("node") });
        if (!stW.waitForFinished(5000)) {
            return QString();
        }
        const QString sFirst = QString::fromUtf8(stW.readAllStandardOutput())
            .section(QLatin1Char('\n'), 0, 0).trimmed();
        return sFirst;
    }

private slots:
    void init() {
        QFile::remove(ConfigPath());
        // 全局静态还原默认（home 覆盖清空 = 生产 ~/.hermes 路径）。
        CHermesRunner::SetHermesHomeOverride(QString());
        CHermesRunner::SetHermesRoot(QString());
        CHermesRunner::SetHermesNodePath(QString());
        CHermesRunner::SetHermesModel(QString());
        CHermesRunner::SetHermesApiKey(QString());
        CHermesRunner::SetHermesBaseUrl(QString());
        CHermesRunner::SetHermesApi(QString());   // 2026.09.21 协议静态项同步还原
    }

    void cleanup() {
        QFile::remove(ConfigPath());
        CHermesRunner::SetHermesHomeOverride(QString());
    }

    // AC2：Backend=5 存取往返；越界 0/6 兜底回 1。
    void testBackendRange5AndFallback() {
        CProjectManager stPm;
        stPm.SaveAgentBackend(5);
        QCOMPARE(stPm.LoadAgentBackend(), 5);
        stPm.SaveAgentBackend(0);
        QCOMPARE(stPm.LoadAgentBackend(), 1);
        stPm.SaveAgentBackend(6);
        QCOMPARE(stPm.LoadAgentBackend(), 1);
        // 原始手写越界值（绕过 Save 钳位）读侧同样兜底 1。
        WriteRawBackend(6);
        QCOMPARE(stPm.LoadAgentBackend(), 1);
        WriteRawBackend(0);
        QCOMPARE(stPm.LoadAgentBackend(), 1);
        WriteRawBackend(5);
        QCOMPARE(stPm.LoadAgentBackend(), 5);
        // 工厂侧：5 号后端名/显示名正确（既有 1..4 分支不动，仅追加）。
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendHermes);
        QVERIFY(CAgentRunnerFactory::IsHermes());
        QCOMPARE(CAgentRunnerFactory::BackendName(), QStringLiteral("hermes"));
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
    }

    // AC1：Hermes 表单键 + 模型表往返（含中文显示名/模型名，Profile API 保 GBK 安全）。
    void testHermesKeysAndModelsRoundTrip() {
        CProjectManager stPm;
        stPm.SaveHermesNodePath(QStringLiteral("D:\\n\\node.exe"));
        stPm.SaveHermesRoot(QStringLiteral("D:\\h\\HermesHarness"));
        stPm.SaveHermesModel(QStringLiteral("hermes-pro-中文"));
        stPm.SaveHermesApiKey(QStringLiteral("sk-test-123"));
        stPm.SaveHermesBaseUrl(QStringLiteral("http://192.168.118.128:9000"));
        stPm.SaveHermesPriceInPerM(3.25);
        stPm.SaveHermesPriceCacheReadPerM(0.31);
        stPm.SaveHermesPriceCacheWritePerM(3.76);
        stPm.SaveHermesPriceOutPerM(15.01);
        QCOMPARE(stPm.LoadHermesNodePath(), QStringLiteral("D:\\n\\node.exe"));
        QCOMPARE(stPm.LoadHermesRoot(), QStringLiteral("D:\\h\\HermesHarness"));
        QCOMPARE(stPm.LoadHermesModel(), QStringLiteral("hermes-pro-中文"));
        QCOMPARE(stPm.LoadHermesApiKey(), QStringLiteral("sk-test-123"));
        QCOMPARE(stPm.LoadHermesBaseUrl(), QStringLiteral("http://192.168.118.128:9000"));
        QCOMPARE(stPm.LoadHermesPriceInPerM(), 3.25);
        QCOMPARE(stPm.LoadHermesPriceCacheReadPerM(), 0.31);
        QCOMPARE(stPm.LoadHermesPriceCacheWritePerM(), 3.76);
        QCOMPARE(stPm.LoadHermesPriceOutPerM(), 15.01);

        QList<SDshModelEntry> aModels;
        SDshModelEntry stE1;
        stE1.sDisp     = QStringLiteral("内网网关-默认");
        stE1.sBaseURL  = QStringLiteral("http://192.168.118.128:9000");
        stE1.sApiKey   = QStringLiteral("sk-模型一");
        stE1.sApi      = QStringLiteral("anthropic-messages");
        stE1.sModelId  = QStringLiteral("hermes-sonnet-中文");
        stE1.iCtx      = 200000;
        stE1.iMaxTok   = 65536;
        aModels.append(stE1);
        SDshModelEntry stE2;
        stE2.sDisp     = QStringLiteral("备用");
        stE2.sBaseURL  = QStringLiteral("http://10.0.0.2:9000");
        stE2.sApiKey   = QString();
        stE2.sApi      = QStringLiteral("openai-responses");
        stE2.sModelId  = QStringLiteral("hermes-opus");
        stE2.iCtx      = 1000000;    // 超限值：存取原样（钳位在设置页/运行期收口）
        stE2.iMaxTok   = 999999;
        aModels.append(stE2);
        QVERIFY(stPm.SaveHermesModels(aModels, 1));
        int iActive = -1;
        const QList<SDshModelEntry> aBack = stPm.LoadHermesModels(&iActive);
        QCOMPARE(aBack.size(), 2);
        QCOMPARE(iActive, 1);
        QCOMPARE(aBack.at(0).sDisp, stE1.sDisp);
        QCOMPARE(aBack.at(0).sApiKey, stE1.sApiKey);
        QCOMPARE(aBack.at(0).sModelId, stE1.sModelId);
        QCOMPARE(aBack.at(0).iCtx, stE1.iCtx);
        QCOMPARE(aBack.at(0).iMaxTok, stE1.iMaxTok);
        QCOMPARE(aBack.at(1).sModelId, stE2.sModelId);
        QCOMPARE(aBack.at(1).iMaxTok, stE2.iMaxTok);
        // 空表 + 无激活。
        QVERIFY(stPm.SaveHermesModels(QList<SDshModelEntry>(), -1));
        QCOMPARE(stPm.LoadHermesModels(&iActive).size(), 0);
        QCOMPARE(iActive, -1);
    }

    // AC3：钳位常量单点（runner 头内收口）+ 运行期配置 JSON 双侧钳位。
    void testClampConstants() {
        QCOMPARE(CHermesRunner::ClampMaxTok(999999), 131072);
        QCOMPARE(CHermesRunner::ClampMaxTok(100), 100);
        QCOMPARE(CHermesRunner::ClampCtx(qint64(500000)), qint64(200000));
        QCOMPARE(CHermesRunner::ClampCtx(qint64(1000)), qint64(1000));
        CHermesRunner::SetHermesModel(QStringLiteral("m1"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw:9000"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-x"));
        CHermesRunner::SetHermesRuntimeLimits(qint64(500000), 999999);
        const QJsonObject oCfg = CHermesRunner::BuildRuntimeConfigJson();
        QCOMPARE(static_cast<qint64>(oCfg.value(QStringLiteral("contextWindow")).toDouble()),
                 qint64(200000));
        QCOMPARE(static_cast<int>(oCfg.value(QStringLiteral("maxTokens")).toDouble()), 131072);
        QCOMPARE(oCfg.value(QStringLiteral("model")).toString(), QStringLiteral("m1"));
        QCOMPARE(oCfg.value(QStringLiteral("baseUrl")).toString(), QStringLiteral("http://gw:9000"));
    }

    // AC4：Hermes 落盘不破坏 config.ini 既有节（含中文值）——字节级不动其他内容。
    void testGbkSafetyExistingSectionsUntouched() {
        CProjectManager stPm;
        // 既有内容：中文值的其他键 + 中文条目模型表（[PiModels] 口径）。
        stPm.SaveHermesModel(QStringLiteral("hermes-中文"));
        QList<SDshModelEntry> aPi;
        SDshModelEntry stPi;
        stPi.sDisp    = QStringLiteral("中文显示-勿动");
        stPi.sBaseURL = QStringLiteral("http://pi:1");
        stPi.sApiKey  = QStringLiteral("sk-密钥-勿动");
        stPi.sApi     = QStringLiteral("anthropic-messages");
        stPi.sModelId = QStringLiteral("pi-模型-勿动");
        stPi.iCtx     = 128000;
        stPi.iMaxTok  = 8192;
        aPi.append(stPi);
        stPm.SavePiModels(aPi, 0);
        const QByteArray aBefore = ReadConfigBytes();
        QVERIFY(!aBefore.isEmpty());
        // Hermes 再写一轮（表单 + 模型表 + 后端号）。
        stPm.SaveHermesApiKey(QStringLiteral("sk-新密钥"));
        QList<SDshModelEntry> aH;
        SDshModelEntry stH;
        stH.sDisp = QStringLiteral("新增条目");
        stH.sModelId = QStringLiteral("h-1");
        aH.append(stH);
        stPm.SaveHermesModels(aH, 0);
        stPm.SaveAgentBackend(5);
        const QByteArray aAfter = ReadConfigBytes();
        // Hermes 自身键值可变；[PiModels] 原节（中文条目）必须逐字节保留。
        QVERIFY(aAfter.contains("[PiModels]"));
        // 逐行核对：PiModels 节内每一行在写后内容中仍存在（Profile API 只动目标键）。
        const QByteArray aPiSec = aBefore.mid(aBefore.indexOf("[PiModels]"));
        for (const QByteArray& aLine : aPiSec.split('\n')) {
            const QByteArray aTrim = aLine.trimmed();
            if (aTrim.isEmpty() || aTrim.startsWith('[')) {
                continue;
            }
            QVERIFY2(aAfter.contains(aTrim),
                     QByteArray("PiModels 行被 Hermes 写入破坏: ") + aTrim);
        }
        // 中文读回无损。
        int iAct = -1;
        const QList<SDshModelEntry> aPiBack = stPm.LoadPiModels(&iAct);
        QCOMPARE(aPiBack.at(0).sDisp, QStringLiteral("中文显示-勿动"));
        QCOMPARE(aPiBack.at(0).sApiKey, QStringLiteral("sk-密钥-勿动"));
    }

    // FR6：EnsureRuntimeConfig 幂等（注入独立 home，两次产物逐字节一致；确定性纯函数）。
    void testEnsureRuntimeConfigIdempotent() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sHome = oTmp.path() + QStringLiteral("/hermes-home");
        CHermesRunner::SetHermesModel(QStringLiteral("m-幂等"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw2:9000"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-idem"));
        CHermesRunner::SetHermesRuntimeLimits(qint64(200000), 32768);
        QVERIFY(CHermesRunner::EnsureRuntimeConfig(sHome));
        QFile fCfg(sHome + QStringLiteral("/agent/config.json"));
        QVERIFY(fCfg.exists());
        QVERIFY(CHermesRunner::EnsureRuntimeConfig(sHome));   // 第二次（读-比-短路）
        QFile fCfg2(sHome + QStringLiteral("/agent/config.json"));
        QVERIFY(fCfg2.open(QIODevice::ReadOnly));
        const QByteArray aBytes = fCfg2.readAll();
        fCfg2.close();
        QVERIFY(CHermesRunner::EnsureRuntimeConfig(sHome));   // 第三次（内容应不变）
        QFile fCfg3(sHome + QStringLiteral("/agent/config.json"));
        QVERIFY(fCfg3.open(QIODevice::ReadOnly));
        QCOMPARE(fCfg3.readAll(), aBytes);
        const QJsonObject o = QJsonDocument::fromJson(aBytes).object();
        QCOMPARE(o.value(QStringLiteral("model")).toString(), QStringLiteral("m-幂等"));
        QCOMPARE(static_cast<int>(o.value(QStringLiteral("maxTokens")).toDouble()), 32768);
    }

    // AC5：stub CLI（node + 临时 dist/cli.js）→ session/assistant/usage 事件与成功收尾。
    void testStubCliEvents() {
        const QString sNode = FindNode();
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 用例（核心事件分派已有失败路径覆盖）");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        // stub CLI：读完 stdin（prompt），吐 4 事件 NDJSON 契约后退出 0。
        QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
        QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
        fCli.write("let s='';process.stdin.on('data',d=>s+=d);"
                   "process.stdin.on('end',()=>{"
                   "console.log(JSON.stringify({type:'session',sessionId:'stub-sid-1'}));"
                   "console.log(JSON.stringify({type:'progress',chars:42,tail:'流式预览尾部'}));"
                   "console.log(JSON.stringify({type:'assistant',text:'stub 你好',"
                   "model:'stub-model',usage:{input:10,cacheRead:2,cacheWrite:1,output:5}}));"
                   "console.log(JSON.stringify({type:'tool',tool:'read_file',"
                   "input:{path:'a.txt'}}));"
                   "console.log(JSON.stringify({type:'result',subtype:'success',"
                   "sessionId:'stub-sid-1',text:'stub 完成'}));"
                   "});");
        fCli.close();
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));
        QCOMPARE(CHermesRunner::ResolveHermesCli(),
                 QDir::cleanPath(sRoot + QStringLiteral("/dist/cli.js")));

        CHermesRunner stRunner;
        QSignalSpy spyInfo(&stRunner, &CAgentRunnerBase::sigInfo);
        QSignalSpy spyAssistant(&stRunner, &CAgentRunnerBase::sigAssistantText);
        QSignalSpy spyUsage(&stRunner, &CAgentRunnerBase::sigUsageDelta);
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        stRunner.StartStage(oTmp.path(), QStringLiteral("请回复 stub 你好"),
                            QString(), 1, QString(), false, 0, false);
        QVERIFY(spyFinished.wait(30000));
        QCOMPARE(spyFinished.size(), 1);
        const bool bOk = spyFinished.at(0).at(0).toBool();
        QVERIFY2(bOk, qPrintable(spyFinished.at(0).at(1).toString()));
        QCOMPARE(spyFinished.at(0).at(2).toLongLong(), qint64(13));   // in=10+2+1
        QCOMPARE(spyFinished.at(0).at(3).toLongLong(), qint64(5));    // out=5
        QCOMPARE(spyFinished.at(0).at(5).toString(), QStringLiteral("stub-sid-1"));
        QVERIFY(spyAssistant.size() >= 1);
        QCOMPARE(spyAssistant.at(0).at(0).toString(), QStringLiteral("stub 你好"));
        QVERIFY(spyUsage.size() >= 1);
        // v2.0.108：progress 事件→60s 节流提示行（首条必达）+ 不破坏后续事件流。
        bool bProgHint = false;
        for (const auto& v : spyInfo) {
            if (v.at(0).toString().contains(QStringLiteral("模型生成中"))) {
                bProgHint = true;
            }
        }
        QVERIFY2(bProgHint, "progress 事件应产生「模型生成中」提示行");
        // stub-sid-1 / 运行时配置已在注入 home 下生成（未碰真实 ~/.hermes）。
        QVERIFY(QFile::exists(oTmp.path() + QStringLiteral("/home/agent/config.json")));
        QVERIFY(!stRunner.IsRunning());
    }

    // —— 2026.09.05 轮次上限兜底文本本地化（用户实测 [00:47:43] 裸英文无从排查）——
    // stub 吐 Hermes 到顶兑底串（assistant + result 同文）→ 三个出口（assistant
    //   事件/result 事件/sigFinished 结果）均应替换为中文提示：含轮数 90 与原始
    //   错误 Connection error，且不再含英文原句；成功语义不变（exit 0、工作已落盘）。
    void testMaxIterFallbackLocalized() {
        const QString sNode = FindNode();
        if (sNode.isEmpty()) {
            QSKIP("本机无 node，跳过 stub CLI 用例");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sRoot = oTmp.path() + QStringLiteral("/HermesHarness");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dist")));
        QFile fCli(sRoot + QStringLiteral("/dist/cli.js"));
        QVERIFY(fCli.open(QIODevice::WriteOnly | QIODevice::Text));
        fCli.write("let s='';process.stdin.on('data',d=>s+=d);"
                   "process.stdin.on('end',()=>{"
                   "const t='I reached the maximum iterations (90) "
                   "but couldn\\'t summarize. Error: Connection error.';"
                   "console.log(JSON.stringify({type:'assistant',text:t,"
                   "model:'stub-model',usage:{input:7,output:3}}));"
                   "console.log(JSON.stringify({type:'result',subtype:'success',"
                   "sessionId:'stub-sid-cap',text:t}));"
                   "});");
        fCli.close();
        CHermesRunner::SetHermesRoot(sRoot);
        CHermesRunner::SetHermesNodePath(sNode);
        CHermesRunner::SetHermesModel(QStringLiteral("stub-model"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("sk-stub"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://stub:9000"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));

        CHermesRunner stRunner;
        QSignalSpy spyAssistant(&stRunner, &CAgentRunnerBase::sigAssistantText);
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        stRunner.StartStage(oTmp.path(), QStringLiteral("做个小事"),
                            QString(), 1, QString(), false, 0, false);
        QVERIFY(spyFinished.wait(30000));
        QCOMPARE(spyFinished.size(), 1);
        QVERIFY2(spyFinished.at(0).at(0).toBool(),
                 qPrintable(spyFinished.at(0).at(1).toString()));
        const QString sRes = spyFinished.at(0).at(1).toString();
        QVERIFY(sRes.contains(QStringLiteral("轮次上限")));
        QVERIFY(sRes.contains(QStringLiteral("90")));
        QVERIFY(sRes.contains(QStringLiteral("Connection error")));
        QVERIFY(!sRes.contains(QLatin1String("I reached")));
        QVERIFY(spyAssistant.size() >= 1);
        QVERIFY(spyAssistant.at(0).at(0).toString().contains(QStringLiteral("轮次上限")));
        QVERIFY(!spyAssistant.at(0).at(0).toString().contains(QLatin1String("I reached")));
    }

    // StartStage 前置校验失败路径：根目录半安装（缺 dist/cli.js）→ 不拉进程直接失败。
    void testStartStageConfigError() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        CHermesRunner::SetHermesRoot(oTmp.path());   // 存在但无 dist/cli.js
        CHermesRunner::SetHermesNodePath(QStringLiteral("node"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));
        QVERIFY(!CHermesRunner::HermesConfigError().isEmpty());
        CHermesRunner stRunner;
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        // 环境隔离（勿回退）：探测链 1.5/3/4 会命中本机 HERMES_HOME 与 PATH 上的真实
        //   hermes（2026.09.02 起机器级安装，install_env.bat 写 HKCU/PATH）——显式根无 CLI
        //   时短路失效 → 真实 hermes -z 启动 ~10s → 5s 超时假失败。配置错误路径在
        //   StartStage 内同步 emit，隔离后可立即还原环境。
        const QByteArray aOldHome = qgetenv("HERMES_HOME");
        const QByteArray aOldPath = qgetenv("PATH");
        qputenv("HERMES_HOME", "");
        qputenv("PATH", "C:\\Windows\\System32;C:\\Windows");
        stRunner.StartStage(oTmp.path(), QStringLiteral("hi"),
                            QString(), 1, QString(), false, 0, false);
        qputenv("HERMES_HOME", aOldHome);
        qputenv("PATH", aOldPath);
        QTRY_COMPARE_WITH_TIMEOUT(spyFinished.size(), 1, 5000);
        QCOMPARE(spyFinished.at(0).at(0).toBool(), false);
        QVERIFY(!stRunner.IsRunning());
    }

    // —— v1.8.29：exe 形态部署根 config.yaml model 段幂等注入 ——
    //（glm-5.3 被 OpenRouter 目录侦测劫持 provider → "No LLM provider configured"
    //   退出 1 的修复回归；含"段尾后位同键覆盖追加式注入"的真实踩坑场景）
    void testEnsureHermesHomeConfigRewrite() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sYaml = oTmp.path() + QStringLiteral("/config.yaml");
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("model:\n"
                    "  # Default model to use\n"
                    "  default: \"old-model\"\n"
                    "  # comments kept\n"
                    "  provider: \"auto\"\n"
                    "  base_url: \"https://openrouter.ai/api/v1\"\n"
                    "\n"
                    "terminal:\n"
                    "  shell: \"pwsh\"\n");
            f.close();
        }
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://192.168.118.128:9000/api/v1"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("key-1"));
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QFile f(sYaml);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString sTxt = QString::fromUtf8(f.readAll());
        f.close();
        // 四键各恰一处且为目标值（后位 openrouter 旧行已被移除而非覆盖失败）。
        QCOMPARE(sTxt.count(QStringLiteral("provider:")), 1);
        QCOMPARE(sTxt.count(QStringLiteral("base_url:")), 1);
        QCOMPARE(sTxt.count(QStringLiteral("api_key:")), 1);
        QVERIFY(sTxt.contains(QStringLiteral("default: \"glm-5.3\"")));
        QVERIFY(sTxt.contains(QStringLiteral("provider: \"custom\"")));
        QVERIFY(sTxt.contains(QStringLiteral("base_url: \"http://192.168.118.128:9000/api/v1\"")));
        QVERIFY(sTxt.contains(QStringLiteral("api_key: \"key-1\"")));
        QVERIFY(!sTxt.contains(QStringLiteral("openrouter.ai")));
        QVERIFY(!sTxt.contains(QStringLiteral("old-model")));
        // 其它段与注释保留。
        QVERIFY(sTxt.contains(QStringLiteral("terminal:")));
        QVERIFY(sTxt.contains(QStringLiteral("shell: \"pwsh\"")));
        QVERIFY(sTxt.contains(QStringLiteral("# comments kept")));
        // 幂等：再调一次产物逐字节一致（读-比-短路不重写）。
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QFile f2(sYaml);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        const QByteArray aBefore = f2.readAll();
        f2.close();
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QFile f3(sYaml);
        QVERIFY(f3.open(QIODevice::ReadOnly));
        QCOMPARE(f3.readAll(), aBefore);
        f3.close();
    }

    // config.yaml 不存在 → 创建仅含 model 段的最小档；静态配置不全 → 不建。
    void testEnsureHermesHomeConfigCreates() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sYaml = oTmp.path() + QStringLiteral("/config.yaml");
        // 三值不全（key 空）→ false 且不建文件。
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw:9000/v1"));
        CHermesRunner::SetHermesApiKey(QString());
        QVERIFY(!CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QVERIFY(!QFile::exists(sYaml));
        // 配齐 → 创建最小档；再调幂等。
        CHermesRunner::SetHermesApiKey(QStringLiteral("k"));
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QVERIFY(QFile::exists(sYaml));
        QFile f(sYaml);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray aFirst = f.readAll();
        f.close();
        QVERIFY(aFirst.contains("model:"));
        QVERIFY(aFirst.contains("provider: \"custom\""));
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        QFile f2(sYaml);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f2.readAll(), aFirst);
        f2.close();
    }

    // 2026.09.21 协议感知：anthropic-messages 端点 → model 段写 api_mode:
    //   "anthropic_messages"（hermes 主运行时据此切 Anthropic 传输——他机
    //   GLM-5.3 网关 /claude 路由 404 实录的修复）；openai/空协议 → 显式
    //   "chat_completions"；协议切换后重写（旧值行移除，恰一处）。
    void testEnsureHermesHomeConfigApiMode() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sYaml = oTmp.path() + QStringLiteral("/config.yaml");
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("k-1"));
        // anthropic-messages 端点（Claude Code 同链路）→ api_mode=anthropic_messages。
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw:9000/claude"));
        CHermesRunner::SetHermesApi(QStringLiteral("anthropic-messages"));
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString sTxt = QString::fromUtf8(f.readAll());
            f.close();
            QCOMPARE(sTxt.count(QStringLiteral("api_mode:")), 1);
            QVERIFY(sTxt.contains(QStringLiteral("api_mode: \"anthropic_messages\"")));
            QVERIFY(sTxt.contains(QStringLiteral("base_url: \"http://gw:9000/claude\"")));
        }
        // 幂等：同配置再调产物不变。
        QByteArray aBefore;
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::ReadOnly));
            aBefore = f.readAll();
            f.close();
        }
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), aBefore);
            f.close();
        }
        // 切回 openai 协议端点 → api_mode 重写为 chat_completions（旧行移除）。
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw:9000/api/v1"));
        CHermesRunner::SetHermesApi(QStringLiteral("openai-completions"));
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString sTxt = QString::fromUtf8(f.readAll());
            f.close();
            QCOMPARE(sTxt.count(QStringLiteral("api_mode:")), 1);
            QVERIFY(sTxt.contains(QStringLiteral("api_mode: \"chat_completions\"")));
            QVERIFY(!sTxt.contains(QStringLiteral("anthropic_messages")));
            QVERIFY(sTxt.contains(QStringLiteral("base_url: \"http://gw:9000/api/v1\"")));
        }
        // 空协议（历史条目）→ 同 openai 口径。
        CHermesRunner::SetHermesApi(QString());
        QVERIFY(CHermesRunner::EnsureHermesHomeConfig(sYaml));
        {
            QFile f(sYaml);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY(QString::fromUtf8(f.readAll())
                        .contains(QStringLiteral("api_mode: \"chat_completions\"")));
            f.close();
        }
        CHermesRunner::SetHermesApi(QString());   // 还原静态态，防污染后续用例
    }

    // v1.4.97 便携口径：exe 形态不再写 ~/.hermes（hermes-agent 只认部署根
    //   config.yaml，主目录写入属纯垃圾）。拷 ping.exe 伪装 venv\Scripts\
    //   hermes.exe 驱动 StartStage（收 -z 非法参数立即退出 code 1），断言注入 home
    //   覆盖位下未产生 agent/config.json（形态 A 的 testStubCliEvents 反向佐证）。
    void testExeFormSkipsHomeWrite() {
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sScripts = oTmp.path() + QStringLiteral("/hermes-agent/venv/Scripts");
        QVERIFY(QDir().mkpath(sScripts));
        QVERIFY(QFile::copy(QStringLiteral("C:/Windows/System32/ping.exe"),
                            sScripts + QStringLiteral("/hermes.exe")));
        CHermesRunner::SetHermesRoot(oTmp.path());
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("k"));
        CHermesRunner::SetHermesBaseUrl(QStringLiteral("http://gw:9000/v1"));
        CHermesRunner::SetHermesHomeOverride(oTmp.path() + QStringLiteral("/home"));
        CHermesRunner stRunner;
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        stRunner.StartStage(oTmp.path(), QStringLiteral("hi"),
                            QString(), 1, QString(), false, 0, false);
        QVERIFY(spyFinished.wait(30000));
        // 关键断言：exe 形态未写用户主目录（~/.hermes 覆盖位无 agent/config.json）。
        QVERIFY(!QFile::exists(oTmp.path() + QStringLiteral("/home/agent/config.json")));
        QVERIFY(!stRunner.IsRunning());
    }

    // 2026.09.02 exe 形态纯文本回复真机冒烟（v1.4.99 回归）：修复前 -z 的 stdout
    //   纯文本回复被 sigInfo 透传成灰字系统行且 m_bSawAssistant 恒假 →
    //   「Hermes 进程退出（code 0）：本轮无 assistant 输出」误报（实测 14:47
    //   「hi」轮，回复全文落在 system 块 569~583）。本用例探测本机真实部署
    //   （E:\...\QT\Hermes\hermes\hermes-agent\venv\Scripts\hermes.exe，
    //   与 v1.4.96 实测同源），不存在则 QSKIP（离线/其它机器可跳）；静态键设为
    //   与部署根 config.yaml 一致（EnsureHermesHomeConfig 读-比-短路零写入）。
    void testExeFormPlainReplySmoke() {
        const QString sReal = QStringLiteral(
            "E:/wosens/Programs/VisualC++/QT/Hermes/hermes/hermes-agent/venv/Scripts/hermes.exe");
        if (!QFile::exists(sReal)) {
            QSKIP("木机无真实 Hermes 部署，跳过真机冒烟");
        }
        CHermesRunner::SetHermesRoot(
            QStringLiteral("E:/wosens/Programs/VisualC++/QT/Hermes/hermes"));
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("e94331c8-3aea-4914-bcce-af879209939a"));
        CHermesRunner::SetHermesBaseUrl(
            QStringLiteral("http://192.168.118.128:9000/api/v1"));
        CHermesRunner::SetHermesHomeOverride(QString());   // 不碰 ~/.hermes
        CHermesRunner stRunner;
        QSignalSpy spyText(&stRunner, &CAgentRunnerBase::sigAssistantText);
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        stRunner.StartStage(QDir::currentPath(),
                            QStringLiteral("只回复两个字：收到"),
                            QString(), 5, QString(), false, 0, false);
        QVERIFY(spyFinished.wait(120000));
        // 关键断言：exit 0 + 纯文本回复 → 成功且上抛为 assistant 文本（修复前
        //   ok=false 且无 sigAssistantText）。
        QCOMPARE(spyFinished.at(0).at(0).toBool(), true);
        QVERIFY(spyText.count() >= 1);
        QVERIFY(!spyText.at(0).at(0).toString().trimmed().isEmpty());
        // v2.0.102：结果文本=纯文本回复（旧口径恒占位符"完成"，AutoNaming 取
        //   首行当任务名会被命名成「完成」）。
        QVERIFY(spyFinished.at(0).at(1).toString().trimmed()
                    .startsWith(QStringLiteral("收到")));
        // v2.0.103：事件补丁的 result 事件携真实 usage（实测 glm-5.3 网关上报
        //   input/output；无补丁/静默形态时恒 0——本用例需部署机在线验证）。
        QVERIFY(spyFinished.at(0).at(2).toLongLong() > 0);   // 输入 token > 0
        QVERIFY(spyFinished.at(0).at(3).toLongLong() > 0);   // 输出 token > 0
    }

    // 2026.09.05 v2.0.119 回归：exe 形态注入 TERMINAL_CWD（修工具锚点漂移——
    //   hermes-agent -z 的 terminal/read_file/patch 不认进程 cwd 恒锚 HOME，
    //   实害任务「一直无法生成 intake.md」：模型把 .autodev/tasks/<id>/intake.md
    //   写到 C:\Users\Administrator\.autodev\ 下，工程目录校验失败反复回退）。
    //   真机用例（无部署 QSKIP）：QTemporaryDir 造唯一名哨兵文件
    //   .autodev/probe-<rand>.md，让 Agent 用相对路径 read_file 读取；
    //   修复前：锚 HOME → File not found；修复后：TERMINAL_CWD 锚工程根 →
    //   回复哨兵内容。
    void testExeFormTerminalCwdAnchor() {
        const QString sReal = QStringLiteral(
            "E:/wosens/Programs/VisualC++/QT/Hermes/hermes/hermes-agent/venv/Scripts/hermes.exe");
        if (!QFile::exists(sReal)) {
            QSKIP("本机无真实 Hermes 部署，跳过真机冒烟");
        }
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sName = QStringLiteral("probe-%1.md")
                                  .arg(QDateTime::currentMSecsSinceEpoch()
                                           % 1000000);
        QVERIFY(QDir().mkpath(oTmp.path() + QStringLiteral("/.autodev")));
        QFile fProbe(oTmp.path() + QStringLiteral("/.autodev/") + sName);
        QVERIFY(fProbe.open(QIODevice::WriteOnly | QIODevice::Text));
        fProbe.write("CWD-ANCHOR-OK-7f3a");
        fProbe.close();
        CHermesRunner::SetHermesRoot(
            QStringLiteral("E:/wosens/Programs/VisualC++/QT/Hermes/hermes"));
        CHermesRunner::SetHermesModel(QStringLiteral("glm-5.3"));
        CHermesRunner::SetHermesApiKey(QStringLiteral("e94331c8-3aea-4914-bcce-af879209939a"));
        CHermesRunner::SetHermesBaseUrl(
            QStringLiteral("http://192.168.118.128:9000/api/v1"));
        CHermesRunner::SetHermesHomeOverride(QString());
        CHermesRunner stRunner;
        QSignalSpy spyFinished(&stRunner, &CAgentRunnerBase::sigFinished);
        stRunner.StartStage(oTmp.path(),
            QStringLiteral("用 read_file 工具读取相对路径 .autodev/%1，"
                           "然后只回复文件内容本身，不要任何其他文字。").arg(sName),
            QString(), 5, QString(), false, 0, false);
        QVERIFY(spyFinished.wait(120000));
        QCOMPARE(spyFinished.at(0).at(0).toBool(), true);
        // 关键断言：相对路径读到了哨兵内容（修复前锄 HOME → File not found）。
        QVERIFY(spyFinished.at(0).at(1).toString().contains(
                    QStringLiteral("CWD-ANCHOR-OK-7f3a")));
    }
    // 2026.09.14 共享技能清单生成（SharedSkillsCatalogFromDirs）：frontmatter
    //   name/description 解析、递归发现嵌套技能、隐藏/脏目录剪枝、无技能返空、截断保护。
    // 2026.09.13 全局规则两层拼接回归：exe 侧《部署环境约定》与 ~/.claude/CLAUDE.md
    //   （用户全局规则，含 UI 测试铁律）是两份不同文档，旧版取第一个命中会让便携层
    //   遮蔽全局层——本机实锤只注入了 1.6KB 环境约定，铁律从未到达 pi/dsh/codex/hermes。
    //   断言带存在性守卫：缺任一份的机器上不误报。
    void testSharedGlobalContextMerge() {
        const bool bHasPortable = QFileInfo::exists(
            QCoreApplication::applicationDirPath() + QStringLiteral("/CLAUDE.md"));
        const bool bHasHome = QFileInfo::exists(
            QDir::homePath() + QStringLiteral("/.claude/CLAUDE.md"));
        const QString sOut = CAgentRunnerBase::SharedGlobalContextContent();
        if (bHasPortable) {
            QVERIFY2(sOut.contains(QStringLiteral("运行环境约定")),
                     "exe 侧部署环境约定应被注入");
        }
        if (bHasHome) {
            QVERIFY2(sOut.contains(QStringLiteral("UI 测试")),
                     "用户全局规则（含 UI 测试铁律）应被注入而非被遮蔽");
        }
        if (bHasPortable && bHasHome) {
            QVERIFY2(sOut.contains(QStringLiteral("---")),
                     "两份并存时应以分隔线拼接");
        }
    }

    void testSharedSkillsCatalog() {
        QTemporaryDir stDir;
        QVERIFY(stDir.isValid());
        const QString sRoot = stDir.path();
        // ① 标准技能：name+description 双字段。
        const QString sA = sRoot + QStringLiteral("/ask/SKILL.md");
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/ask")));
        {
            QFile f(sA);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("---\nname: ask\ndescription: quick side question without losing context\n"
                    "---\n\n# ask\nbody here\n");
        }
        // ② 无 frontmatter：回退目录名。
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/plain")));
        {
            QFile f(sRoot + QStringLiteral("/plain/SKILL.md"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("# just body\n");
        }
        // ③ 嵌套技能（ECC 风格二级子目录）+ 隐藏/脏目录剪枝。
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/ECC/docs/skills/deep")));
        {
            QFile f(sRoot + QStringLiteral("/ECC/docs/skills/deep/SKILL.md"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("---\nname: deep-skill\ndescription: nested discovery works\n---\nbody\n");
        }
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/.hidden/skill")));
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/node_modules/pkg")));
        {
            QFile f(sRoot + QStringLiteral("/.hidden/skill/SKILL.md"));
            f.open(QIODevice::WriteOnly | QIODevice::Text);
            f.write("---\nname: hidden\n---\n");
            QFile f2(sRoot + QStringLiteral("/node_modules/pkg/SKILL.md"));
            f2.open(QIODevice::WriteOnly | QIODevice::Text);
            f2.write("---\nname: junk\n---\n");
        }
        const QString sOut = CAgentRunnerBase::SharedSkillsCatalogFromDirs({ sRoot });
        QVERIFY(!sOut.isEmpty());
        QVERIFY2(sOut.contains(QStringLiteral("ask")), "标准技能应在清单");
        QVERIFY2(sOut.contains(QStringLiteral("quick side question")), "description 应被提取");
        QVERIFY2(sOut.contains(QStringLiteral("deep-skill")), "嵌套技能应被递归发现");
        QVERIFY2(sOut.contains(QStringLiteral("plain")), "无 frontmatter 应回退目录名");
        QVERIFY2(!sOut.contains(QStringLiteral("hidden")), "隐藏目录应剪枝");
        QVERIFY2(!sOut.contains(QStringLiteral("junk")), "node_modules 应剪枝");
        QVERIFY2(sOut.contains(QStringLiteral("SKILL.md")), "应含 SKILL.md 路径供模型读取");
        // ④ 空目录集 → 2026-09-26 起不再空串：内置沙箱铁律块无条件注入
        //    （Agent 启动的 GUI 窗口自动收纳 + sandbox_* 命令，不依赖任何技能安装），
        //    但技能清单本体仍为空（无技能名/无 SKILL.md 路径）。
        {
            const QString sEmpty = CAgentRunnerBase::SharedSkillsCatalogFromDirs(QStringList());
            QVERIFY2(!sEmpty.isEmpty(), "内置沙箱铁律块应无条件注入");
            QVERIFY2(sEmpty.contains(QStringLiteral("内置沙箱")),
                     "空目录也应含沙箱铁律块");
            QVERIFY2(!sEmpty.contains(QStringLiteral("SKILL.md：")),
                     "无技能时不应有技能清单条目");
        }
        // ⑤ 不存在的目录 → 同 ④（仅沙箱铁律块，无技能清单）。
        {
            const QString sNone = CAgentRunnerBase::SharedSkillsCatalogFromDirs(
                { sRoot + QStringLiteral("/nope") });
            QVERIFY(!sNone.isEmpty());
            QVERIFY(!sNone.contains(QStringLiteral("SKILL.md：")));
        }
        // ⑥ 截断与钉选（2026.09.13）：详情预算 12KB 必然截断；钉选技能 ui-sandbox
        //    按字母序本排最后（真实机上排在第 289/294 位），旧版直接截断丢弃
        //    =「助手不自动使用沙箱技能」根因之一；现置顶永不被截，剩余技能压成名字串兑底。
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/many")));
        const QString sLongDesc(220, QLatin1Char('x'));
        for (int i = 0; i < 60; ++i) {
            const QString sName = QStringLiteral("skill%1").arg(i, 2, 10, QLatin1Char('0'));
            const QString sDir = sRoot + QStringLiteral("/many/") + sName;
            QVERIFY(QDir().mkpath(sDir));
            QFile f(sDir + QStringLiteral("/SKILL.md"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(QStringLiteral("---\nname: %1\ndescription: %2\n---\n")
                        .arg(sName, sLongDesc).toUtf8());
        }
        QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/many/ui-sandbox")));
        {
            QFile f(sRoot + QStringLiteral("/many/ui-sandbox/SKILL.md"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("---\nname: ui-sandbox\ndescription: hidden windows ui test sandbox\n---\n");
        }
        const QString sOut2 = CAgentRunnerBase::SharedSkillsCatalogFromDirs(
            { sRoot + QStringLiteral("/many") });
        QVERIFY(!sOut2.isEmpty());
        // 钉选项必须以详情行出现在截断标记之前（扫描是栈式倒序，字母序最尾的
        //   ui-sandbox 旧版必然被截掉；置顶后永在详情区且描述完整）。
        QVERIFY2(sOut2.indexOf(QStringLiteral("- ui-sandbox：")) >= 0
                 && sOut2.indexOf(QStringLiteral("- ui-sandbox："))
                        < sOut2.indexOf(QStringLiteral("仅有名字")),
                 "钉选技能应置顶进详情区（字母序本在最尾必被截）");
        QVERIFY2(sOut2.contains(QStringLiteral("hidden windows ui test sandbox")),
                 "钉选技能应保留完整描述");
        QVERIFY2(sOut2.contains(QStringLiteral("仅有名字")), "截断后应有名字串兑底");
        QVERIFY2(sOut2.contains(QStringLiteral("skill00")), "名字串应含被截技能名");
        QVERIFY2(sOut2.toUtf8().size() <= 16 * 1024, "总量不超 16KB 预算");
    }

};

QTEST_MAIN(CTestHermesRunner)
#include "test_hermesrunner.moc"
