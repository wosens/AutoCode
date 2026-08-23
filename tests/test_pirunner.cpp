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
        const QString sJsonPath = CPiAgentRunner::PiModelsJsonPath();
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

        // 还原现场（机器上有真实 models.json 时）。
        if (bHadFile) {
            QFile fRest(sJsonPath);
            fRest.open(QIODevice::WriteOnly | QIODevice::Truncate);
            fRest.write(baBackup);
            fRest.close();
        } else {
            QFile::remove(sJsonPath);
        }
    }

    g_out << (g_iFail == 0 ? "test_pirunner PASS" : "test_pirunner FAIL") << Qt::endl;
    return g_iFail == 0 ? 0 : 1;
}
