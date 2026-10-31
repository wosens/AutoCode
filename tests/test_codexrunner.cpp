#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include "agent/CAgentRunnerBase.h"
#include "agent/CAgentRunnerFactory.h"
#include "agent/CClaudeRunner.h"
#include "agent/CCodexProxy.h"
#include "agent/CCodexRunner.h"
#include "agent/CDeepSeekRunner.h"
#include "agent/CPiAgentRunner.h"

// CCodexRunner 离线单元测试（不联网、不依赖真实 CodexHarness 安装，挂 ctest）。
//
// 覆盖（针对第四后端 CodexHarness 接入）：
//   1. 工厂分派：BackendName/BackendDisplayName/IsCodex + Create 返回 CCodexRunner。
//   2. 探测/诊断：临时目录构造未装/半装/版本不符场景，断言 ResolveCodexJsPath/
//      ResolveCodexVersion/CodexConfigError 指引文本。
//   3. StartStage 早期失败（node 指向不存在路径）→ sigFinished 失败、sid 空。
//   4. WriteCodexUserConfig（注入临时目录）：autocode 节生成、用户手写节/顶层键保留、
//      auth.json 键写入且错误消息不含 key 明文。
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

    // —— 1. 工厂分派 ——
    {
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendCodex);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("codex"),
              QStringLiteral("BackendName(codex) 应为 codex"));
        Check(CAgentRunnerFactory::IsCodex() && !CAgentRunnerFactory::IsPi()
                  && !CAgentRunnerFactory::IsDsh(),
              QStringLiteral("IsCodex/IsPi/IsDsh 判定错误"));
        Check(CAgentRunnerFactory::BackendDisplayName() == QStringLiteral("CodexHarness"),
              QStringLiteral("BackendDisplayName 应为 CodexHarness"));
        CAgentRunnerBase* p = CAgentRunnerFactory::Create();
        Check(p && p->BackendName() == QStringLiteral("codex"),
              QStringLiteral("Create() 应返回 codex 后端 runner"));
        Check(dynamic_cast<CCodexRunner*>(p) != nullptr,
              QStringLiteral("Create() 动态类型应为 CCodexRunner"));
        delete p;
        // 既有三后端不受影响（回归）。
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendClaude);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("claude"),
              QStringLiteral("BackendName(claude) 应为 claude"));
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendPi);
        Check(CAgentRunnerFactory::BackendName() == QStringLiteral("pi"),
              QStringLiteral("BackendName(pi) 应为 pi"));
    }

    // —— 2. 探测/诊断（临时目录构造各安装态）——
    QTemporaryDir oTmp;
    Check(oTmp.isValid(), QStringLiteral("临时目录创建失败"));
    const QString sPkgRel = QStringLiteral(
        "/node_modules/@openai/codex/package.json");
    const QString sJsRel = QStringLiteral(
        "/node_modules/@openai/codex/bin/codex.js");
    auto fnWritePkg = [&sPkgRel, &sJsRel](const QString& a_sRoot, const QString& a_sVer) {
        QDir().mkpath(a_sRoot + sPkgRel.left(sPkgRel.lastIndexOf('/')));
        QFile f(a_sRoot + sPkgRel);
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        f.write(QStringLiteral("{\"name\":\"@openai/codex\",\"version\":\"%1\"}")
                    .arg(a_sVer).toUtf8());
        f.close();
        QDir().mkpath(a_sRoot + sJsRel.left(sJsRel.lastIndexOf('/')));
        QFile fJs(a_sRoot + sJsRel);
        fJs.open(QIODevice::WriteOnly);
        fJs.write(QStringLiteral("// stub\n").toUtf8());
        fJs.close();
        return true;
    };
    {
        CCodexRunner::SetCodexRoot(QString());
        CCodexRunner::SetCodexNodePath(QString());
        CCodexRunner::SetCodexModel(QStringLiteral("gpt-test"));
        CCodexRunner::SetCodexBaseUrl(QStringLiteral("http://127.0.0.1:9000/v1"));
        Check(CCodexRunner::ResolveNodePath() == QStringLiteral("node"),
              QStringLiteral("node 留空应回退 PATH 的 node"));
        Check(!CCodexRunner::CodexConfigError().isEmpty(),
              QStringLiteral("根目录未配置时 CodexConfigError 应非空"));
        // 半安装态：目录在、包不在。
        const QString sHalf = oTmp.path() + QStringLiteral("/CodexHarness");
        QDir().mkpath(sHalf);
        CCodexRunner::SetCodexRoot(sHalf);
        Check(!CCodexRunner::LooksLikeCodexRoot(sHalf),
              QStringLiteral("半安装态 LooksLikeCodexRoot 应为 false"));
        Check(CCodexRunner::CodexConfigError().contains(QStringLiteral("未安装"))
                  || CCodexRunner::CodexConfigError().contains(QStringLiteral("不完整")),
              QStringLiteral("半安装态应给出安装指引"));
        // 版本不符：0.90.0 → 应提示 0.88+ 移除 chat wire_api。
        Check(fnWritePkg(sHalf, QStringLiteral("0.90.0")), QStringLiteral("写 0.90.0 失败"));
        CCodexRunner::SetCodexRoot(sHalf);
        Check(CCodexRunner::ResolveCodexVersion() == QStringLiteral("0.90.0"),
              QStringLiteral("ResolveCodexVersion 应读出 0.90.0"));
        const QString sVerErr = CCodexRunner::CodexConfigError();
        Check(sVerErr.contains(QStringLiteral("版本不符"))
                  && sVerErr.contains(QStringLiteral("0.87.0")),
              QStringLiteral("版本不符应含指引文本"));
        // 正确版本 0.87.0 + 表单齐备 → 可用。
        Check(fnWritePkg(sHalf, CCodexRunner::ExpectedCodexVersion()),
              QStringLiteral("写 0.87.0 失败"));
        Check(CCodexRunner::LooksLikeCodexRoot(sHalf),
              QStringLiteral("装全后 LooksLikeCodexRoot 应为 true"));
        Check(QDir::fromNativeSeparators(CCodexRunner::ResolveCodexJsPath())
                  == sHalf + sJsRel,
              QStringLiteral("root 推导 codex.js 不符: %1").arg(
                  CCodexRunner::ResolveCodexJsPath()));
        Check(CCodexRunner::CodexConfigError().isEmpty(),
              QStringLiteral("装全+表单齐备时 CodexConfigError 应为空"));
        // 表单缺项：model 空 → 报指引。
        CCodexRunner::SetCodexModel(QString());
        Check(!CCodexRunner::CodexConfigError().isEmpty(),
              QStringLiteral("model 缺失时 CodexConfigError 应非空"));
        CCodexRunner::SetCodexModel(QStringLiteral("gpt-test"));
        // node 显式无效路径 → 报错。
        CCodexRunner::SetCodexNodePath(sHalf + QStringLiteral("/no-node.exe"));
        Check(!CCodexRunner::CodexConfigError().isEmpty(),
              QStringLiteral("node 路径无效时 CodexConfigError 应非空"));
        CCodexRunner::SetCodexNodePath(QString());
    }

    // —— 3. StartStage 早期失败：不拉进程、sigFinished(false, sid 空) ——
    {
        CCodexRunner::SetCodexRoot(QString());   // 根目录未配置 → 前置校验失败
        CCodexRunner::SetCodexNodePath(QStringLiteral("Z:/definitely/no/node.exe"));
        CCodexRunner runner;
        int iFinished = 0;
        QString sSid;
        bool bOk = true;
        QString sMsg;
        QObject::connect(&runner, &CAgentRunnerBase::sigFinished,
                         [&](bool ok, const QString& msg, qint64, qint64, double,
                             const QString& sid) {
                             ++iFinished;
                             bOk = ok;
                             sSid = sid;
                             sMsg = msg;
                         });
        // 连续两轮失败启动：均应立即失败且 sid 为空（早期失败不带旧 tid）。
        for (int i = 0; i < 2; ++i) {
            iFinished = 0;
            runner.StartStage(oTmp.path(), QStringLiteral("hello"),
                              QString(), 30, QString(), false, 0, false);
            Check(iFinished == 1,
                  QStringLiteral("第 %1 轮：配置缺失应同步发一次 sigFinished").arg(i + 1));
            Check(!bOk, QStringLiteral("第 %1 轮：bOk 应为 false").arg(i + 1));
            Check(sSid.isEmpty(),
                  QStringLiteral("第 %1 轮：早期失败 sid 应为空").arg(i + 1));
            Check(sMsg.contains(QStringLiteral("处理")),
                  QStringLiteral("第 %1 轮：失败消息应带指引").arg(i + 1));
        }
        Check(!runner.IsRunning(), QStringLiteral("失败后 IsRunning 应为 false"));
        Check(!runner.IsRetryPending() && !runner.InjectUserTurn(QStringLiteral("x")),
              QStringLiteral("IsRetryPending/InjectUserTurn 应为安全 no-op"));
        CCodexRunner::SetCodexNodePath(QString());
    }

    // —— 4. WriteCodexUserConfig（注入临时目录；不碰真实 ~/.codex）——
    {
        const QString sHome = oTmp.path() + QStringLiteral("/fake-codex-home");
        QDir().mkpath(sHome);
        // 预置用户手写内容：顶层键 + 手写节 + 注释，必须逐字保留。
        {
            QFile f(sHome + QStringLiteral("/config.toml"));
            Check(f.open(QIODevice::WriteOnly), QStringLiteral("预置 config.toml 失败"));
            f.write(QStringLiteral(
                        "# user comment\n"
                        "model = \"gpt-user-old\"\n"
                        "approval_policy = \"never\"\n\n"
                        "[mcp_servers.my-server]\n"
                        "command = \"npx\"\n")
                        .toUtf8());
            f.close();
        }
        CCodexRunner::SetCodexProvider(QString());   // → autocode
        CCodexRunner::SetCodexModel(QStringLiteral("glm-test"));
        CCodexRunner::SetCodexBaseUrl(QStringLiteral("http://192.168.118.128:9000/v1"));
        CCodexRunner::SetCodexWireApi(QStringLiteral("chat"));
        CCodexRunner::SetCodexApiKey(QStringLiteral("sk-secret-123"));
        QString sErr;
        Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome),
              QStringLiteral("WriteCodexUserConfig 应成功: %1").arg(sErr));
        QString sToml;
        {
            QFile f(sHome + QStringLiteral("/config.toml"));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("config.toml 应已写盘"));
            sToml = QString::fromUtf8(f.readAll());
            f.close();
        }
        Check(sToml.contains(QStringLiteral("model = \"glm-test\"")),
              QStringLiteral("顶层 model 应原位替换为新值"));
        Check(sToml.contains(QStringLiteral("model_provider = \"autocode\"")),
              QStringLiteral("顶层 model_provider 应写入 autocode"));
        // 2026.08.21 model_context_window：固定路由默认 200000（整数键，不带引号）。
        Check(sToml.contains(QStringLiteral("model_context_window = 200000")),
              QStringLiteral("顶层 model_context_window 应写入默认 200000"));
        Check(sToml.contains(QStringLiteral("# user comment"))
                  && sToml.contains(QStringLiteral("approval_policy = \"never\""))
                  && sToml.contains(QStringLiteral("[mcp_servers.my-server]"))
                  && sToml.contains(QStringLiteral("command = \"npx\"")),
              QStringLiteral("用户手写内容必须逐字保留"));
        Check(sToml.contains(QStringLiteral("[model_providers.autocode]"))
                  && sToml.contains(QStringLiteral("wire_api = \"chat\""))
                  && sToml.contains(QStringLiteral("requires_openai_auth = true"))
                  && sToml.contains(QStringLiteral("base_url = \"http://192.168.118.128:9000/v1\"")),
              QStringLiteral("autocode provider 节应正确生成"));
        // 顶层键应位于首个节标题之前（TOML 顶层作用域）。
        Check(sToml.indexOf(QStringLiteral("model_provider =")) < sToml.indexOf('['),
              QStringLiteral("顶层键应插入在首个节标题之前"));
        // auth.json：OPENAI_API_KEY 写入；预置其他键保留。
        {
            QFile f(sHome + QStringLiteral("/auth.json"));
            QJsonObject oPre;
            oPre.insert(QStringLiteral("tokens"), QStringLiteral("keep-me"));
            f.open(QIODevice::WriteOnly);
            f.write(QJsonDocument(oPre).toJson());
            f.close();
        }
        Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome),
              QStringLiteral("二次写入应成功"));
        {
            QFile f(sHome + QStringLiteral("/auth.json"));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("auth.json 应已写盘"));
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            Check(o.value(QStringLiteral("OPENAI_API_KEY")).toString()
                      == QStringLiteral("sk-secret-123"),
                  QStringLiteral("OPENAI_API_KEY 应写入"));
            Check(o.value(QStringLiteral("tokens")).toString()
                      == QStringLiteral("keep-me"),
                  QStringLiteral("auth.json 其余键应保留"));
        }
        Check(QFile::exists(sHome + QStringLiteral("/config.toml.autocode.bak")),
              QStringLiteral("写前应留 .bak 备份"));
        // 缺项拒绝写：model 清空 → 报错且消息含指引、不含 key 明文。
        CCodexRunner::SetCodexModel(QString());
        QString sErr2;
        Check(!CCodexRunner::WriteCodexUserConfig(&sErr2, sHome),
              QStringLiteral("表单缺项应拒绝写入"));
        Check(sErr2.contains(QStringLiteral("未配置"))
                  && !sErr2.contains(QStringLiteral("sk-secret-123")),
              QStringLiteral("错误消息应含指引且不含 key 明文"));
        CCodexRunner::SetCodexModel(QStringLiteral("glm-test"));
    }

    // —— 5. 空 apiKey 不覆写既有登录态；Execute\CodexHarness 探测命中 ——
    {
        // auth.json 已有非空 OPENAI_API_KEY，apiKey 置空再保存 → 既有 key 必须保留。
        const QString sHome = oTmp.path() + QStringLiteral("/fake-codex-home");
        CCodexRunner::SetCodexApiKey(QString());
        QString sErr;
        Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome),
              QStringLiteral("空 apiKey 时 WriteCodexUserConfig 应成功"));
        {
            QFile f(sHome + QStringLiteral("/auth.json"));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("auth.json 应可读"));
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            Check(o.value(QStringLiteral("OPENAI_API_KEY")).toString()
                      == QStringLiteral("sk-secret-123"),
                  QStringLiteral("空 apiKey 不应覆写既有 OPENAI_API_KEY"));
        }
        // install_env.bat 落点 = exe 同级 Execute\CodexHarness，ProbeSiblingRoot 应命中。
        const QString sExeDir = QCoreApplication::applicationDirPath();
        const QString sExecRoot = sExeDir + QStringLiteral("/Execute/CodexHarness");
        QDir().mkpath(sExecRoot);
        Check(fnWritePkg(sExecRoot, CCodexRunner::ExpectedCodexVersion()),
              QStringLiteral("写 Execute/CodexHarness 安装态失败"));
        const QString sProbed = CCodexRunner::ProbeSiblingRoot();
        Check(sProbed == QDir(sExecRoot).canonicalPath(),
              QStringLiteral("ProbeSiblingRoot 应命中 exe 同级 Execute\\CodexHarness: %1")
                  .arg(sProbed));
        // 清理测试残留，不污染构建目录。
        QDir(sExeDir + QStringLiteral("/Execute")).removeRecursively();
    }

    // —— 6. 自定义大模型表（2026.08.21）：激活条目覆盖固定路由写 ~/.codex ——
    {
        const QString sHome = oTmp.path() + QStringLiteral("/fake-codex-home2");
        QDir().mkpath(sHome);
        // 固定键故意留 model/baseUrl（证明激活条目覆盖）；条目 key 空回退固定 key。
        CCodexRunner::SetCodexApiKey(QStringLiteral("sk-fixed-key"));
        CCodexRunner::SetCodexModel(QStringLiteral("glm-fixed"));
        CCodexRunner::SetCodexBaseUrl(QStringLiteral("http://fixed:9000/v1"));
        CCodexRunner::SetCodexWireApi(QStringLiteral("chat"));
        SDshModelEntry stE;
        stE.sDisp    = QStringLiteral("GLM-5.2 (网关B)");
        stE.sBaseURL = QStringLiteral("http://192.168.118.128:9000/v1");
        stE.sApiKey  = QStringLiteral("sk-entry-key");
        stE.sApi     = QStringLiteral("chat");
        stE.sModelId = QStringLiteral("glm-5.2");
        stE.iCtx     = 200000;
        stE.iMaxTok  = 32768;
        CCodexRunner::SetCodexModels(QList<SDshModelEntry>{ stE }, 0);   // 激活下标 0（纯内存，不写真实 ~/.codex）
        QString sToml;
        {
            QFile f(sHome + QStringLiteral("/config.toml"));
            // SetCodexModels 写的是真实 ~/.codex，这里对注入目录再显式写一次验证内容。
            QString sErr;
            Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome),
                  QStringLiteral("激活条目时 WriteCodexUserConfig 应成功: %1").arg(sErr));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("config.toml 应已写盘"));
            sToml = QString::fromUtf8(f.readAll());
            f.close();
        }
        Check(sToml.contains(QStringLiteral("model_provider = \"autocode-1\"")),
              QStringLiteral("激活条目应路由到 autocode-1 槽位"));
        Check(sToml.contains(QStringLiteral("model = \"glm-5.2\"")),
              QStringLiteral("顶层 model 应为条目模型 id"));
        Check(sToml.contains(QStringLiteral("[model_providers.autocode-1]"))
                  && sToml.contains(QStringLiteral("base_url = \"http://192.168.118.128:9000/v1\"")),
              QStringLiteral("autocode-1 节应为条目 baseUrl"));
        Check(!sToml.contains(QStringLiteral("glm-fixed"))
                  && !sToml.contains(QStringLiteral("http://fixed")),
              QStringLiteral("固定路由值不应出现在写出的 config.toml"));
        {
            QFile f(sHome + QStringLiteral("/auth.json"));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("auth.json 应已写盘"));
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            Check(o.value(QStringLiteral("OPENAI_API_KEY")).toString()
                      == QStringLiteral("sk-entry-key"),
                  QStringLiteral("条目 key 应写入 auth.json"));
        }
        Check(CCodexRunner::CodexActiveProvider() == QStringLiteral("autocode-1"),
              QStringLiteral("CodexActiveProvider 应为 autocode-1"));
        // 取消激活（-1）→ 回固定路由：槽位/顶层键回 autocode + 固定 model。
        CCodexRunner::SetCodexModels(QList<SDshModelEntry>{ stE }, -1);
        {
            QFile f(sHome + QStringLiteral("/config.toml"));
            QString sErr;
            Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome),
                  QStringLiteral("取消激活后写回固定路由应成功"));
            Check(f.open(QIODevice::ReadOnly), QStringLiteral("config.toml 应可读"));
            const QString s = QString::fromUtf8(f.readAll());
            f.close();
            Check(s.contains(QStringLiteral("model_provider = \"autocode\""))
                      && s.contains(QStringLiteral("model = \"glm-fixed\"")),
                  QStringLiteral("取消激活后应回固定路由"));
        }
        // 清理（SetCodexModels 纯内存无 IO，真实 ~/.codex 未被本用例触碰）。
        CCodexRunner::SetCodexModels(QList<SDshModelEntry>(), -1);
    }

    // —— anthropic-messages 端点兑底（2026.09.24 v3.1.231 搬机实害：四后端共享表
    //    把 Pi 风味条目 glm-5.3 @.../claude 设为当前，Codex 发 /chat/completions
    //    必 404）：激活条目为 Codex 不可用的 Anthropic 原生端点时，自动改用同表内
    //    同名模型 + chat 兼容条目；无兑底由 CodexConfigError 前置拦截。
    {
        // 前置：合法安装根（CodexConfigError 先查 node/根/版本）。
        const QString sRootC = oTmp.path() + QStringLiteral("/CodexCompatRoot");
        Check(fnWritePkg(sRootC, CCodexRunner::ExpectedCodexVersion()),
              QStringLiteral("写兑底用安装态失败"));
        CCodexRunner::SetCodexRoot(sRootC);
        CCodexRunner::SetCodexNodePath(QString());
        CCodexRunner::SetCodexApiKey(QStringLiteral("sk-fixed-key"));
        CCodexRunner::SetCodexModel(QStringLiteral("glm-fixed"));
        CCodexRunner::SetCodexBaseUrl(QStringLiteral("http://fixed:9000/v1"));
        CCodexRunner::SetCodexWireApi(QStringLiteral("chat"));
        auto fnAnthropicEntry = []() {
            SDshModelEntry stE;
            stE.sDisp    = QStringLiteral("glm-5.3 (Pi 风味)");
            stE.sBaseURL = QStringLiteral("http://192.168.118.128:9000/claude");
            stE.sApiKey  = QStringLiteral("sk-anthropic-key");
            stE.sApi     = QStringLiteral("anthropic-messages");
            stE.sModelId = QStringLiteral("glm-5.3");
            stE.iCtx     = 200000;
            stE.iMaxTok  = 32768;
            return stE;
        };
        // ① 有同名 chat 兑底 → 生效端点/wire/key 落兑底条目，配置不报警。
        {
            SDshModelEntry stChat = fnAnthropicEntry();
            stChat.sDisp    = QStringLiteral("glm-5.3 (chat)");
            stChat.sBaseURL = QStringLiteral("http://192.168.118.128:9000/api/v1");
            stChat.sApiKey  = QStringLiteral("sk-chat-key");
            stChat.sApi     = QStringLiteral("openai-completions");
            const QString sHome3 = oTmp.path() + QStringLiteral("/fake-codex-home3");
            QDir().mkpath(sHome3);
            CCodexRunner::SetCodexModels(
                QList<SDshModelEntry>{ fnAnthropicEntry(), stChat }, 0);
            QString sErr;
            Check(CCodexRunner::WriteCodexUserConfig(&sErr, sHome3),
                  QStringLiteral("兑底生效时 WriteCodexUserConfig 应成功: %1").arg(sErr));
            QFile f3(sHome3 + QStringLiteral("/config.toml"));
            Check(f3.open(QIODevice::ReadOnly), QStringLiteral("config.toml 应已写盘"));
            const QString sToml3 = QString::fromUtf8(f3.readAll());
            f3.close();
            Check(sToml3.contains(QStringLiteral("model = \"glm-5.3\"")),
                  QStringLiteral("顶层 model 应保持激活条目模型 id"));
            Check(sToml3.contains(QStringLiteral(
                      "base_url = \"http://192.168.118.128:9000/api/v1\"")),
                  QStringLiteral("base_url 应兑底到同名 chat 端点"));
            Check(!sToml3.contains(QStringLiteral("9000/claude")),
                  QStringLiteral("anthropic 端点不应出现在写出的 config.toml"));
            QFile fAuth3(sHome3 + QStringLiteral("/auth.json"));
            Check(fAuth3.open(QIODevice::ReadOnly), QStringLiteral("auth.json 应已写盘"));
            const QJsonObject o3 = QJsonDocument::fromJson(fAuth3.readAll()).object();
            fAuth3.close();
            Check(o3.value(QStringLiteral("OPENAI_API_KEY")).toString()
                      == QStringLiteral("sk-chat-key"),
                  QStringLiteral("兑底条目 key 应写入 auth.json"));
            Check(CCodexRunner::CodexConfigError().isEmpty(),
                  QStringLiteral("有兑底时 CodexConfigError 应为空: %1")
                      .arg(CCodexRunner::CodexConfigError()));
        }
        // ② 无同名 chat 兑底 → CodexConfigError 前置拦截并点名 anthropic-messages。
        {
            CCodexRunner::SetCodexModels(QList<SDshModelEntry>{ fnAnthropicEntry() }, 0);
            const QString sErr2 = CCodexRunner::CodexConfigError();
            Check(sErr2.contains(QStringLiteral("anthropic-messages")),
                  QStringLiteral("无兑底时应前置拦截并点名 anthropic-messages: %1").arg(sErr2));
        }
        // 清理。
        CCodexRunner::SetCodexModels(QList<SDshModelEntry>(), -1);
    }

    // —— 5. CCodexProxy::RewriteChatRequestBody（max_tokens 注入纯函数，2026.08.21）——
    {
        // chat/completions 载荷：注入 max_tokens（不存在时新增）。
        {
            const QByteArray sBody = R"({"model":"glm-5.2","messages":[{"role":"user","content":"hi"}],"stream":true})";
            const QByteArray sOut = CCodexProxy::RewriteChatRequestBody(sBody, 65536);
            Check(!sOut.isEmpty(), QStringLiteral("chat 载荷应被重写"));
            const QJsonObject o = QJsonDocument::fromJson(sOut).object();
            Check(o.value(QStringLiteral("max_tokens")).toInt() == 65536,
                  QStringLiteral("max_tokens 应注入 65536"));
            Check(o.value(QStringLiteral("stream")).toBool(),
                  QStringLiteral("重写不应破坏既有字段（stream=true）"));
        }
        // 已有 max_tokens：覆盖为新值。
        {
            const QByteArray sBody = R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":1024})";
            const QJsonObject o = QJsonDocument::fromJson(
                CCodexProxy::RewriteChatRequestBody(sBody, 32768)).object();
            Check(o.value(QStringLiteral("max_tokens")).toInt() == 32768,
                  QStringLiteral("既有 max_tokens 应被覆盖为 32768"));
        }
        // 非 chat 载荷 / 非法 JSON：返回空（不拦截）。
        Check(CCodexProxy::RewriteChatRequestBody("{\"a\":1}", 32768).isEmpty(),
              QStringLiteral("非 messages 载荷应返回空"));
        Check(CCodexProxy::RewriteChatRequestBody("not json", 32768).isEmpty(),
              QStringLiteral("非法 JSON 应返回空"));
    }

    // —— 6. CCodexProxy 端到端（本地回环：伪造网关 + 客户端经代理往返，2026.08.21）——
    {
        #include <QTcpServer>
        #include <QTcpSocket>
        #include <QTimer>
        #include <QUrl>
        // 伪造网关：收全请求 → 记录请求体 → 回固定响应并关连接。
        QTcpServer gw;
        QByteArray gwBody;
        int gwRequests = 0;
        QObject::connect(&gw, &QTcpServer::newConnection, &gw, [&]() {
            QTcpSocket* pSock = gw.nextPendingConnection();
            pSock->setParent(&gw);
            QObject::connect(pSock, &QTcpSocket::readyRead, pSock, [pSock, &gwBody, &gwRequests]() {
                pSock->setProperty("buf", pSock->property("buf").toByteArray()
                                          + pSock->readAll());
                const QByteArray sAll = pSock->property("buf").toByteArray();
                const int iBodyAt = sAll.indexOf("\r\n\r\n");
                if (iBodyAt < 0) {
                    return;
                }
                qint64 iLen = 0;
                for (const QByteArray& sLine : sAll.left(iBodyAt).split('\n')) {
                    if (sLine.trimmed().toLower().startsWith("content-length:")) {
                        iLen = sLine.mid(sLine.indexOf(':') + 1).trimmed().toLongLong();
                    }
                }
                if (sAll.size() < iBodyAt + 4 + int(iLen)) {
                    return;
                }
                gwBody = sAll.mid(iBodyAt + 4, int(iLen));
                ++gwRequests;
                // 响应体带 usage JSON（模拟网关 stream_options.include_usage 补发的计量）。
                const QByteArray sBodyOut = "data: {\"usage\":{\"prompt_tokens\":28,"
                    "\"completion_tokens\":14}}\n\nPONG_SSE!";
                const QByteArray sResp = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\nConnection: close\r\n"
                    "Content-Length: " + QByteArray::number(sBodyOut.size())
                    + "\r\n\r\n" + sBodyOut;
                pSock->write(sResp);
                pSock->flush();
                pSock->disconnectFromHost();
            });
        });
        Check(gw.listen(QHostAddress::LocalHost, 0),
              QStringLiteral("伪造网关应可监听回环"));
        CCodexProxy proxy;
        qint64 spyIn = -1, spyOut = -1;
        int spyCount = 0;
        qint64 spyEst = -1;
        int spyEstCount = 0;
        QObject::connect(&proxy, &CCodexProxy::sigUsage, &proxy,
                         [&](qint64 a_iIn, qint64 a_iOut) {
                             spyIn = a_iIn; spyOut = a_iOut; ++spyCount;
                         });
        // 2026-10-31 上下文估算信号（网关 usage 尾块被掐时的兑底数据源）。
        QObject::connect(&proxy, &CCodexProxy::sigPromptEstimate, &proxy,
                         [&](qint64 a_iTokens) {
                             spyEst = a_iTokens; ++spyEstCount;
                         });
        Check(proxy.Start(QUrl(QStringLiteral("http://127.0.0.1:%1/openai/v1")
                                   .arg(gw.serverPort())), 32768),
              QStringLiteral("代理 Start 应成功"));
        // 客户端 → 代理：chat/completions POST（无 max_tokens）。
        QTcpSocket cli;
        QByteArray cliRecv;
        QEventLoop loop;
        QTimer tmOut;
        tmOut.setSingleShot(true);
        QObject::connect(&tmOut, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&cli, &QTcpSocket::connected, &cli, [&]() {
            const QByteArray sBody = R"({"model":"glm-5.2","messages":[{"role":"user","content":"hi"}],"stream":true})";
            const QByteArray sHead = "POST /openai/v1/chat/completions HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                "Content-Length: " + QByteArray::number(sBody.size()) + "\r\n\r\n";
            cli.write(sHead + sBody);
        });
        QObject::connect(&cli, &QTcpSocket::readyRead, &cli, [&]() {
            cliRecv += cli.readAll();
            if (cliRecv.contains("PONG_SSE!")) {
                loop.quit();
            }
        });
        QObject::connect(&cli, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        cli.connectToHost(QHostAddress::LocalHost, int(proxy.Port()));
        tmOut.start(5000);
        loop.exec();
        Check(gwRequests == 1, QStringLiteral("网关应收到 1 个请求（实际 %1）").arg(gwRequests));
        const QJsonObject oSeen = QJsonDocument::fromJson(gwBody).object();
        Check(oSeen.value(QStringLiteral("max_tokens")).toInt() == 32768,
              QStringLiteral("网关侧请求体应含注入的 max_tokens=32768"));
        Check(oSeen.value(QStringLiteral("messages")).isArray(),
              QStringLiteral("转发请求应保留 messages"));
        Check(cliRecv.startsWith("HTTP/1.1 200 OK") && cliRecv.contains("PONG_SSE!"),
              QStringLiteral("客户端应原样收到网关响应（含 SSE 体）"));
        Check(spyCount == 1 && spyIn == 28 && spyOut == 14,
              QStringLiteral("代理应解析出 usage（次数=%1 in=%2 out=%3）")
                  .arg(spyCount).arg(spyIn).arg(spyOut));
        Check(oSeen.value(QStringLiteral("stream_options")).toObject()
                  .value(QStringLiteral("include_usage")).toBool(),
              QStringLiteral("代理应注入 stream_options.include_usage"));
        Check(spyEstCount == 1 && spyEst == qint64(gwBody.size()) / 4
                  && spyEst > 0,
              QStringLiteral("代理应发请求体估算（次数=%1 估=%2 期望=%3）")
                  .arg(spyEstCount).arg(spyEst).arg(gwBody.size() / 4));
        proxy.Shutdown();
    }

    // —— 7. SanitizeToolArguments（workdir 幻觉消毒纯函数，2026-10-31）——
    {
        const QString sBase = QCoreApplication::applicationDirPath();   // 真实存在目录
        QString sBad;
        // ① 幻觉绝对目录（OpenCompare 实害原样 E:\wososens）→ 剔除 workdir 键。
        {
            const QString sIn = QStringLiteral(
                "{\"command\":[\"powershell.exe\",\"-Command\",\"dir\"],"
                "\"timeout_ms\":30000,\"workdir\":\"E:\\\\wososens\"}");
            const QString sOut = CCodexProxy::SanitizeToolArguments(sIn, sBase, &sBad);
            const QJsonObject o = QJsonDocument::fromJson(sOut.toUtf8()).object();
            Check(!o.contains(QStringLiteral("workdir")),
                  QStringLiteral("幻觉绝对目录应剔除 workdir"));
            Check(o.value(QStringLiteral("command")).isArray()
                      && o.value(QStringLiteral("timeout_ms")).toInt() == 30000,
                  QStringLiteral("剔除 workdir 不应伤及其他字段"));
            Check(sBad == QStringLiteral("E:\\wososens"),
                  QStringLiteral("应回带被剔除目录（实际 %1）").arg(sBad));
        }
        // ② 存在的绝对目录（本程序目录）→ 放行原样。
        {
            const QString sIn = QStringLiteral(
                "{\"command\":[\"dir\"],\"workdir\":\"%1\"}").arg(sBase);
            sBad.clear();
            Check(CCodexProxy::SanitizeToolArguments(sIn, QString(), &sBad) == sIn
                      && sBad.isEmpty(),
                  QStringLiteral("存在的绝对目录应原样放行"));
        }
        // ③ 相对目录：基准空放行；基准给定且拼接后不存在 → 剔除。
        {
            const QString sIn = QStringLiteral("{\"command\":[\"dir\"],\"workdir\":\"src\"}");
            sBad.clear();
            Check(CCodexProxy::SanitizeToolArguments(sIn, QString(), &sBad) == sIn,
                  QStringLiteral("基准空时相对路径应放行"));
            const QString sOut = CCodexProxy::SanitizeToolArguments(
                sIn, QStringLiteral("Z:/__autocode_no_such__"), &sBad);
            Check(!QJsonDocument::fromJson(sOut.toUtf8()).object()
                      .contains(QStringLiteral("workdir"))
                      && sBad == QStringLiteral("src"),
                  QStringLiteral("基准下不存在的相对目录应剔除"));
        }
        // ④ 非 JSON / 无 workdir / 空 workdir → 原样。
        Check(CCodexProxy::SanitizeToolArguments("not json", sBase, &sBad) == "not json"
                  && sBad.isEmpty(),
              QStringLiteral("非 JSON 参数应原样"));
        Check(CCodexProxy::SanitizeToolArguments("{\"command\":[\"dir\"]}", sBase, &sBad)
                  == "{\"command\":[\"dir\"]}",
              QStringLiteral("无 workdir 应原样"));
    }

    // —— 8. BuildArgsDeltaChunk 形状（冲刷合成块可被 codex 归并）——
    {
        const QByteArray aChunk = CCodexProxy::BuildArgsDeltaChunk(
            0, 1, QStringLiteral("chatcmpl-x"), QStringLiteral("glm-5.3"),
            1793379882, QStringLiteral("{\"a\":1}"));
        Check(aChunk.startsWith("data: ") && aChunk.endsWith("\n\n"),
              QStringLiteral("合成块应为 data: 行"));
        const QJsonObject o = QJsonDocument::fromJson(
            aChunk.mid(6, aChunk.size() - 8)).object();
        const QJsonObject oTc = o.value(QStringLiteral("choices")).toArray().at(0)
            .toObject().value(QStringLiteral("delta")).toObject()
            .value(QStringLiteral("tool_calls")).toArray().at(0).toObject();
        Check(o.value(QStringLiteral("id")).toString() == QStringLiteral("chatcmpl-x")
                  && o.value(QStringLiteral("model")).toString() == QStringLiteral("glm-5.3"),
              QStringLiteral("合成块应带 id/model"));
        Check(oTc.value(QStringLiteral("index")).toInt() == 1
                  && oTc.value(QStringLiteral("function")).toObject()
                      .value(QStringLiteral("arguments")).toString()
                  == QStringLiteral("{\"a\":1}"),
              QStringLiteral("合成块应为指定 tc 索引携带完整 arguments"));
    }

    // —— 9. chunked SSE 消毒端到端（本地回环：幻觉 workdir 被剔 + 流序完整）——
    {
        #include <QTcpServer>
        #include <QTcpSocket>
        #include <QTimer>
        #include <QUrl>
        // 伪造网关：chunked SSE 分帧发工具调用增量（首块 id/name+空参，两块参数增量
        //   携带幻觉 workdir，finish 块收尾）——模拟 glm 真实输出形态。
        QTcpServer gw;
        QObject::connect(&gw, &QTcpServer::newConnection, &gw, [&]() {
            QTcpSocket* pSock = gw.nextPendingConnection();
            pSock->setParent(&gw);
            QObject::connect(pSock, &QTcpSocket::readyRead, pSock, [pSock]() {
                pSock->setProperty("buf", pSock->property("buf").toByteArray()
                                          + pSock->readAll());
                if (!pSock->property("buf").toByteArray().contains("\r\n\r\n")) {
                    return;   // 等头收全即回（不看请求体）
                }
                const auto frame = [](const QByteArray& aBody) {
                    return QByteArray::number(aBody.size(), 16) + "\r\n" + aBody + "\r\n";
                };
                QByteArray aBody;
                aBody += "data: {\"id\":\"c1\",\"created\":1,\"model\":\"glm-5.3\","
                         "\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
                         "[{\"index\":0,\"id\":\"call_x\",\"type\":\"function\","
                         "\"function\":{\"name\":\"shell\",\"arguments\":\"\"}}]}}]}\n\n";
                const auto argsChunkLine = [](const QString& a_sFrag) {
                    QJsonObject oFn;
                    oFn.insert(QStringLiteral("arguments"), a_sFrag);
                    QJsonObject oTc;
                    oTc.insert(QStringLiteral("index"), 0);
                    oTc.insert(QStringLiteral("function"), oFn);
                    QJsonObject oDelta;
                    oDelta.insert(QStringLiteral("tool_calls"), QJsonArray{ oTc });
                    QJsonObject oChoice;
                    oChoice.insert(QStringLiteral("index"), 0);
                    oChoice.insert(QStringLiteral("delta"), oDelta);
                    QJsonObject oRoot;
                    oRoot.insert(QStringLiteral("id"), QStringLiteral("c1"));
                    oRoot.insert(QStringLiteral("choices"), QJsonArray{ oChoice });
                    return "data: "
                        + QJsonDocument(oRoot).toJson(QJsonDocument::Compact) + "\n\n";
                };
                // 参数分两个增量：携幻觉 workdir 的前半 + 补齐后半（跨块拼接 + 嵌套转义）
                aBody += argsChunkLine(QStringLiteral(
                    "{\"command\":[\"dir\"],\"workdir\":\"E:\\\\wososens\""));
                aBody += argsChunkLine(QStringLiteral(",\"timeout_ms\":30000}"));
                aBody += "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{},"
                         "\"finish_reason\":\"tool_calls\"}]}\n\n";
                aBody += "data: [DONE]\n\n";
                const QByteArray sResp = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n" + frame(aBody) + "0\r\n\r\n";
                pSock->write(sResp);
                pSock->flush();
                pSock->disconnectFromHost();
            });
        });
        Check(gw.listen(QHostAddress::LocalHost, 0),
              QStringLiteral("消毒用伪造网关应可监听"));
        CCodexProxy proxy;
        QString sGuard;
        int iGuard = 0;
        QObject::connect(&proxy, &CCodexProxy::sigWorkdirGuard, &proxy,
                         [&](const QString& a_sBad) { sGuard = a_sBad; ++iGuard; });
        Check(proxy.Start(QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                                   .arg(gw.serverPort())), 32768,
                          QStringLiteral("E:/wosens/Programs/VisualC++/QT/OpenCompare")),
              QStringLiteral("消毒代理 Start 应成功"));
        QTcpSocket cli;
        QByteArray cliRecv;
        QEventLoop loop;
        QTimer tmOut;
        tmOut.setSingleShot(true);
        QObject::connect(&tmOut, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&cli, &QTcpSocket::connected, &cli, [&]() {
            const QByteArray sBody = R"({"model":"glm-5.3","messages":[{"role":"user","content":"hi"}],"stream":true})";
            const QByteArray sHead = "POST /v1/chat/completions HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                "Content-Length: " + QByteArray::number(sBody.size()) + "\r\n\r\n";
            cli.write(sHead + sBody);
        });
        QObject::connect(&cli, &QTcpSocket::readyRead, &cli, [&]() {
            cliRecv += cli.readAll();
        });
        QObject::connect(&cli, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        cli.connectToHost(QHostAddress::LocalHost, int(proxy.Port()));
        tmOut.start(5000);
        loop.exec();
        // ① chunked 帧合法：能解出全部 body 且含终止帧。
        const int iBodyAt = cliRecv.indexOf("\r\n\r\n");
        Check(iBodyAt > 0, QStringLiteral("应收到响应头"));
        QByteArray aBody, aRest = cliRecv.mid(iBodyAt + 4);
        bool bTerminal = false;
        while (!aRest.isEmpty()) {
            const int iCrlf = aRest.indexOf("\r\n");
            if (iCrlf < 0) { break; }
            const qint64 llSz = aRest.left(iCrlf).split(';').first().trimmed()
                                    .toLongLong(nullptr, 16);
            aRest.remove(0, iCrlf + 2);
            if (llSz == 0) { bTerminal = true; break; }
            aBody += aRest.left(int(llSz));
            aRest.remove(0, int(llSz) + 2);
        }
        Check(bTerminal && aBody.contains("[DONE]"),
              QStringLiteral("重编码后的 chunked 帧应可解析且含终止帧"));
        // ② 首块 id/name 即时透传且不含 arguments。
        Check(aBody.contains("\"call_x\"") && aBody.contains("\"shell\""),
              QStringLiteral("id/name 行应透传"));
        // ③ 冲刷的完整参数：含 command/timeout，不含幻觉 workdir。
        int iArgsJson = -1;
        {
            const QList<QByteArray> aLines = aBody.split('\n');
            for (const QByteArray& sLine : aLines) {
                if (!sLine.startsWith("data: ")) { continue; }
                const QByteArray sPay = sLine.mid(6).trimmed();
                if (sPay.isEmpty() || sPay == "[DONE]") { continue; }
                const QJsonObject o = QJsonDocument::fromJson(sPay).object();
                const auto aTcs = o.value(QStringLiteral("choices")).toArray().at(0)
                    .toObject().value(QStringLiteral("delta")).toObject()
                    .value(QStringLiteral("tool_calls")).toArray();
                if (aTcs.isEmpty()) { continue; }
                const QString sArgs = aTcs.at(0).toObject()
                    .value(QStringLiteral("function")).toObject()
                    .value(QStringLiteral("arguments")).toString();
                if (!sArgs.isEmpty()) {
                    const QJsonObject oArgs = QJsonDocument::fromJson(sArgs.toUtf8()).object();
                    if (oArgs.contains(QStringLiteral("command"))) {
                        iArgsJson = 1;
                        Check(oArgs.value(QStringLiteral("command")).toArray().at(0)
                                  .toString() == QStringLiteral("dir")
                              && oArgs.value(QStringLiteral("timeout_ms")).toInt() == 30000,
                              QStringLiteral("冲刷参数应含跨增量拼接的 command/timeout"));
                        Check(!oArgs.contains(QStringLiteral("workdir")),
                              QStringLiteral("冲刷参数不应再含幻觉 workdir"));
                    }
                }
            }
        }
        Check(iArgsJson == 1, QStringLiteral("应存在携带完整参数的冲刷块"));
        // ④ 拦截留痕信号。
        Check(iGuard == 1 && sGuard == QStringLiteral("E:\\wososens"),
              QStringLiteral("sigWorkdirGuard 应发 1 次且目录正确（次=%1 目录=%2）")
                  .arg(iGuard).arg(sGuard));
        proxy.Shutdown();
    }

    g_out << (g_iFail == 0 ? "test_codexrunner PASS" : "test_codexrunner FAIL") << Qt::endl;
    return g_iFail == 0 ? 0 : 1;
}
