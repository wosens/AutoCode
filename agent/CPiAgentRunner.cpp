#include "CPiAgentRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include "CClaudeRunner.h"   // StallMinutes / ContextWindowForModel：巡检与窗口兜底复用同一实现

// —— 进程级全局配置（static：所有 Pi runner 实例共享，主窗口启动/设置变更时写）——
static QString s_sPiRootPath;    // PIAgent 仓库根（含 pi\ monorepo；空=未配置/未探测到）
static QString s_sPiNodePath;    // node.exe 路径（空=走 PATH 的 "node"）
static QString s_sPiProvider;    // provider 名（空回退 anthropic）
static QString s_sPiModel;       // 模型 id（空=用 pi 默认模型）
static QString s_sPiApiKey;      // API key（明文；经进程 env 注入，不进命令行/日志）
static QString s_sPiThinking;    // thinking 级别（off/minimal/low/medium/high/xhigh/max；空=默认）
// 2026.08.20 自定义大模型表（[PiModels] 正本在 config.ini，这里存运行期副本：
//   激活条目 → --provider autocode-<n> --model <id>；条目本体合成写 models.json）。
static QList<SDshModelEntry> s_aPiModels;
static int  s_iPiActiveModel = -1;          // 激活条目下标（-1=无）
static QString s_sPiNpmCliCache;            // npm 全局 cli.js 探测缓存（空串=未命中；一次性）
// 2026.08.20 计费单价（$/百万 token）：pi 只报 token 不报金额，客户端按单价折算；
//   默认对齐 claude 后端 Sonnet 档价目（同 DSH 口径）。
static double s_dPiPriceInPerM        = 3.0;
static double s_dPiPriceCacheReadPerM = 0.3;
static double s_dPiPriceCacheWritePerM = 3.75;
static double s_dPiPriceOutPerM       = 15.0;

void CPiAgentRunner::SetPiPriceInPerM(double a_d) {
    s_dPiPriceInPerM = qMax(0.0, a_d);
}
void CPiAgentRunner::SetPiPriceCacheReadPerM(double a_d) {
    s_dPiPriceCacheReadPerM = qMax(0.0, a_d);
}
void CPiAgentRunner::SetPiPriceCacheWritePerM(double a_d) {
    s_dPiPriceCacheWritePerM = qMax(0.0, a_d);
}
void CPiAgentRunner::SetPiPriceOutPerM(double a_d) {
    s_dPiPriceOutPerM = qMax(0.0, a_d);
}

// 根目录内固定相对布局（判定文件 = pi monorepo 的 CLI 入口产物）。
static constexpr const char* kSzPiCliJsRel = "/pi/packages/coding-agent/dist/cli.js";

void CPiAgentRunner::SetPiRootPath(const QString& a_sRoot) {
    s_sPiRootPath = QDir::fromNativeSeparators(a_sRoot.trimmed());
    while (s_sPiRootPath.endsWith(QLatin1Char('/'))) {
        s_sPiRootPath.chop(1);
    }
}

QString CPiAgentRunner::PiRootPath() {
    return s_sPiRootPath;
}

void CPiAgentRunner::SetPiNodePath(const QString& a_sPath) {
    s_sPiNodePath = a_sPath.trimmed();
}

QString CPiAgentRunner::PiNodePath() {
    return s_sPiNodePath;
}

void CPiAgentRunner::SetPiProvider(const QString& a_sProvider) {
    s_sPiProvider = a_sProvider.trimmed();
}

QString CPiAgentRunner::PiProvider() {
    return s_sPiProvider.isEmpty() ? QStringLiteral("anthropic") : s_sPiProvider;
}

void CPiAgentRunner::SetPiModel(const QString& a_sModel) {
    s_sPiModel = a_sModel.trimmed();
}

QString CPiAgentRunner::PiModel() {
    return s_sPiModel;
}

void CPiAgentRunner::SetPiApiKey(const QString& a_sKey) {
    s_sPiApiKey = a_sKey.trimmed();
}

QString CPiAgentRunner::PiApiKey() {
    return s_sPiApiKey;
}

void CPiAgentRunner::SetPiThinking(const QString& a_sLevel) {
    s_sPiThinking = a_sLevel.trimmed().toLower();
}

QString CPiAgentRunner::PiThinking() {
    return s_sPiThinking;
}

// —— 自定义大模型表（2026.08.20）——
void CPiAgentRunner::SetPiModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    s_aPiModels = a_aModels;
    s_iPiActiveModel = (a_iActive >= 0 && a_iActive < a_aModels.size()) ? a_iActive : -1;
    // 立即合成写 models.json：设置保存与启动两处都走这里，保证磁盘正本与表一致
    //   （pi 每次打开 /model 重读，无需重启；本进程后续 StartStage 直接用内存副本）。
    WritePiModelsJson();
}

QString CPiAgentRunner::PiActiveProvider() {
    return (s_iPiActiveModel >= 0 && s_iPiActiveModel < s_aPiModels.size())
        ? QStringLiteral("autocode-%1").arg(s_iPiActiveModel + 1) : QString();
}

QString CPiAgentRunner::PiActiveModelId() {
    return (s_iPiActiveModel >= 0 && s_iPiActiveModel < s_aPiModels.size())
        ? s_aPiModels.at(s_iPiActiveModel).sModelId : QString();
}

QString CPiAgentRunner::PiModelsJsonPath() {
    return QDir::homePath() + QStringLiteral("/.pi/agent/models.json");
}

bool CPiAgentRunner::WritePiModelsJson() {
    // 合并写：保留用户手写的非 autocode-* provider 与其他顶层键；autocode-* 槽位
    //   全删重建（下标随表变化，旧槽位即失效）。条目 → provider：
    //     { baseUrl, api, apiKey（空则占位 "-"，本地网关忽略鉴权的场景 pi 才放行）,
    //       models:[{ id, name, contextWindow, maxTokens }] }
    const QString sPath = PiModelsJsonPath();
    QDir().mkpath(QFileInfo(sPath).absolutePath());
    QJsonObject oRoot;
    QFile stFile(sPath);
    if (stFile.open(QIODevice::ReadOnly)) {
        QJsonParseError stErr;
        const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll(), &stErr);
        stFile.close();
        if (stErr.error == QJsonParseError::NoError && oDoc.isObject()) {
            oRoot = oDoc.object();
        }
    }
    QJsonObject oProv = oRoot.value(QStringLiteral("providers")).toObject();
    const QStringList aOldKeys = oProv.keys();
    for (const QString& sKey : aOldKeys) {
        if (sKey.startsWith(QStringLiteral("autocode-"))) {
            oProv.remove(sKey);
        }
    }
    for (int i = 0; i < s_aPiModels.size(); ++i) {
        const SDshModelEntry& stE = s_aPiModels.at(i);
        QJsonObject oModel;
        oModel.insert(QStringLiteral("id"), stE.sModelId);
        oModel.insert(QStringLiteral("name"), stE.sDisp.isEmpty() ? stE.sModelId : stE.sDisp);
        oModel.insert(QStringLiteral("contextWindow"), static_cast<double>(stE.iCtx));
        oModel.insert(QStringLiteral("maxTokens"), stE.iMaxTok);
        QJsonObject oEntry;
        oEntry.insert(QStringLiteral("baseUrl"), stE.sBaseURL);
        oEntry.insert(QStringLiteral("api"),
                     stE.sApi.isEmpty() ? QStringLiteral("anthropic-messages") : stE.sApi);
        // 空 key 写占位 "-"：pi 把「无鉴权配置」的模型标记为不可用，占位值让条目
        //   始终可选（与 Ollama 等本地网关的官方推荐做法一致）。
        oEntry.insert(QStringLiteral("apiKey"),
                      stE.sApiKey.isEmpty() ? QStringLiteral("-") : stE.sApiKey);
        oEntry.insert(QStringLiteral("models"), QJsonArray{ oModel });
        oProv.insert(QStringLiteral("autocode-%1").arg(i + 1), oEntry);
    }
    oRoot.insert(QStringLiteral("providers"), oProv);
    QFile stOut(sPath);
    if (!stOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stOut.write(QJsonDocument(oRoot).toJson(QJsonDocument::Indented));
    return true;
}

QString CPiAgentRunner::NpmGlobalCliPath() {
    // 一次性探测（进程内缓存；npm 未装/未命中缓存空串）。走 npm root -g 拿全局
    //   node_modules 根（对 npm 前缀配置免疫），拼固定包内布局后校验存在。
    if (!s_sPiNpmCliCache.isEmpty()) {
        return s_sPiNpmCliCache;
    }
    const QString sNpm = QStandardPaths::findExecutable(QStringLiteral("npm.cmd"));
    if (sNpm.isEmpty()) {
        return QString();
    }
    QProcess stProc;
    stProc.start(sNpm, { QStringLiteral("root"), QStringLiteral("-g") });
    if (!stProc.waitForFinished(10000)) {
        return QString();
    }
    const QString sRoot = QString::fromUtf8(stProc.readAllStandardOutput()).trimmed();
    if (sRoot.isEmpty()) {
        return QString();
    }
    const QString sCli = QDir::fromNativeSeparators(sRoot)
        + QStringLiteral("/@earendil-works/pi-coding-agent/dist/cli.js");
    if (!QFile::exists(sCli)) {
        return QString();
    }
    s_sPiNpmCliCache = QDir::toNativeSeparators(sCli);
    return s_sPiNpmCliCache;
}

QString CPiAgentRunner::ResolveNodePath() {
    // 留空走 PATH：QProcess 直起可执行名由系统搜路径（同 DSH 口径）。
    return s_sPiNodePath.isEmpty() ? QStringLiteral("node") : s_sPiNodePath;
}

QString CPiAgentRunner::ResolveCliJsPath() {
    // 优先级：根目录固定布局 → npm 全局安装（2026.08.20 联网安装落点兜底）。
    if (!s_sPiRootPath.isEmpty()) {
        const QString sCli = s_sPiRootPath + QString::fromLatin1(kSzPiCliJsRel);
        if (QFile::exists(sCli)) {
            return sCli;
        }
    }
    return NpmGlobalCliPath();
}

bool CPiAgentRunner::LooksLikePiRoot(const QString& a_sRoot) {
    const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    return !sRoot.isEmpty()
        && QFileInfo::exists(sRoot + QString::fromLatin1(kSzPiCliJsRel));
}

QString CPiAgentRunner::ProbeSiblingRoot() {
    // install_env.bat /pi 把 pi 装在 exe 同级 PIAgent\pi（绿色便携布局）；这里同布局
    //   探测（exe 同级最优先，上级为兼容回退），命中返回 canonical 根（=PIAgent 目录）。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    for (const QString& sBase : aBases) {
        const QString sRoot = QDir(sBase + QStringLiteral("/PIAgent")).canonicalPath();
        if (LooksLikePiRoot(sRoot)) {
            return sRoot;
        }
    }
    return QString();
}

QString CPiAgentRunner::PiConfigError() {
    // 可用性统一校验（启动警告/设置保存兜底/StartStage 前置三处同一口径，仿 DSH）。
    const QString sNode = ResolveNodePath();
    if (sNode == QStringLiteral("node")) {
        if (QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            return QStringLiteral(
                "node 未配置且 PATH 中找不到 node.exe。\n"
                "处理：安装 Node.js 后重启 AutoCode（node 路径留空即走 PATH），或在"
                "「设置 → PiAgent 参数 → node 路径」填 node.exe 绝对路径。");
        }
    } else if (!QFile::exists(sNode)) {
        return QStringLiteral(
            "node 路径无效：%1。\n"
            "处理：「设置 → PiAgent 参数 → node 路径」改填有效的 node.exe（留空即走 PATH）。")
            .arg(sNode);
    }
    const QString sCli = ResolveCliJsPath();
    if (sCli.isEmpty() || !QFile::exists(sCli)) {
        return QStringLiteral(
            "PIAgent 运行时 cli.js 无效：%1。\n"
            "处理：「设置 → PiAgent 参数」点「安装 PIAgent…」（install_env.bat /pi，从本机"
            "已构建的 PIAgent 源码复制到 exe 同级 PIAgent\\pi）或「联网安装（npm）」"
            "（npm install -g @earendil-works/pi-coding-agent）；完成后点「重新检测」，"
            "根目录自动填入（npm 安装无需根目录）。")
            .arg(sCli.isEmpty() ? QStringLiteral("(空)") : sCli);
    }
    return QString();
}

QString CPiAgentRunner::PiInstallDiagnosis() {
    // 半安装态诊断：探测布局同 ProbeSiblingRoot，目录存在但 cli.js 缺 → 报缺项与指引。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    for (const QString& sBase : aBases) {
        const QString sRoot = QDir(sBase + QStringLiteral("/PIAgent")).canonicalPath();
        if (sRoot.isEmpty() || !QDir(sRoot).exists()) {
            continue;
        }
        if (LooksLikePiRoot(sRoot)) {
            continue;   // 完整根（探测应已命中），继续看其他候选
        }
        return QStringLiteral(
            "发现安装目录但未装全：%1\n"
            "缺：pi\\packages\\coding-agent\\dist\\cli.js——源码复制/构建未完成，"
            "重跑「安装 PIAgent…」续装").arg(QDir::toNativeSeparators(sRoot));
    }
    return QStringLiteral("各探测位置（程序目录同级/上级）均无 PIAgent 安装目录——"
                          "可能是源码目录缺失或复制失败，请查看安装窗口的输出按提示处理后重装。");
}

CPiAgentRunner::CPiAgentRunner(QObject* a_pParent)
    : CAgentRunnerBase(a_pParent) {
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput, this, &CPiAgentRunner::slotOnStdOut);
    connect(m_pProc, &QProcess::readyReadStandardError, this, &CPiAgentRunner::slotOnStdErr);
    connect(m_pProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CPiAgentRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred, this, &CPiAgentRunner::slotOnErrorOccurred);
    // 停滞巡检定时器：常驻创建、本轮运行时武装（30s 一拍，同 claude/DSH 侧）。
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, [this]() {
        CheckStall();
    });
}

CPiAgentRunner::~CPiAgentRunner() {
    Stop();
}

void CPiAgentRunner::Reset() {
    m_bFinished = false;
    m_bAgentEnded = false;
    m_bSawSession = false;
    m_sTurnText.clear();
    m_sLastStopReason.clear();
    m_sLastApiError.clear();   // 2026.08.22 API 错误透传（见头文件注释）
    m_sResumeSid.clear();
    m_iSumPlainInTok = 0;
    m_iSumCacheReadTok = 0;
    m_iSumCacheWriteTok = 0;
    m_iSumInTok = 0;
    m_iSumOutTok = 0;
    m_sLastSubtype.clear();
    m_iLastReqCount = 0;
    m_iLastContextFill = 0;
    m_sLastModel.clear();
    m_aStderrTail.clear();
    m_buf.clear();
}

void CPiAgentRunner::StartStage(const QString& a_sWorkDir,
                                const QString& a_sPrompt,
                                const QString& a_sDisallowedTools,
                                int a_iMaxTurns,
                                const QString& a_sResumeSid,
                                bool a_bFork,
                                int a_iAutocompactK,
                                bool a_bEnableAskUser) {
    Q_UNUSED(a_bFork);
    Reset();

    // claude 专属参数降级提示（仅非默认值出现时提示一次，默认路径零噪音）。
    if (a_iMaxTurns != 30 || a_iAutocompactK > 0 || a_bEnableAskUser) {
        emit sigInfo(QStringLiteral(
            "[system/pi] claude 专属参数（maxTurns/autocompact/ask_user）在 PIAgent 后端"
            "不适用，已忽略（pi 自带自动 compaction、无 ask_user 工具）"));
    }

    // 启动前校验：缺失直接报失败（带完整处理指引，不拉进程）。
    const QString sCfgErr = PiConfigError();
    if (!sCfgErr.isEmpty()) {
        EmitFailed(sCfgErr);
        return;
    }

    // 收尾上一进程（复用对象的连续阶段；正常路径进程已退出）。
    if (m_pProc->state() != QProcess::NotRunning) {
        m_bFinished = true;   // 吞掉旧进程的事件（不应出现，防御）
        if (!m_pProc->waitForFinished(3000)) {
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
        }
        m_bFinished = false;
    }

    // —— 组装命令行（node 直起，参数化传参，Windows 路径含空格安全）——
    QStringList aArgs;
    aArgs << ResolveCliJsPath()
          << QStringLiteral("-p")
          << QStringLiteral("--mode") << QStringLiteral("json")
          // 离线约束（NFR-5）：跳过扩展/技能/主题发现扫描，且避免工程本地扩展被
          //   静默加载（安全面收敛）；--offline 跳过更新检查。
          << QStringLiteral("--offline")
          << QStringLiteral("--no-extensions")
          << QStringLiteral("--no-skills")
          << QStringLiteral("--no-themes")
          // 非交互全自动：忽略（而非信任）工程本地可执行内容，避免无头模式挂起。
          << QStringLiteral("--no-approve");
    // 2026.08.20 自定义大模型激活条目覆盖固定路由：provider=autocode-<n>、model=条目
    //   模型 id（apiKey 走 models.json，不走 env 注入）；无激活 → Provider/模型 键。
    const bool bCustomActive = !PiActiveProvider().isEmpty();
    // 2026.08.21 自愈：多实例/热重载竞态可能把 models.json 的 autocode-* 槽位清掉
    //   （磁盘正本与内存表失配 → pi 报 Unknown provider "autocode-n"）。每次启动前
    //   幂等重写一遍（保留用户手写 provider，代价极低），保证槽位与激活路由一致。
    if (bCustomActive) {
        WritePiModelsJson();
    }
    aArgs << QStringLiteral("--provider")
          << (bCustomActive ? PiActiveProvider() : PiProvider());
    const QString sModelArg = bCustomActive ? PiActiveModelId() : PiModel();
    if (!sModelArg.isEmpty()) {
        aArgs << QStringLiteral("--model") << sModelArg;
    }
    if (!PiThinking().isEmpty()) {
        aArgs << QStringLiteral("--thinking") << PiThinking();
    }
    // deny 黑名单映射：--disallowedTools（逗号表）→ pi 的 --exclude-tools（同为逗号表）。
    const QString sExcl = a_sDisallowedTools.trimmed();
    if (!sExcl.isEmpty()) {
        aArgs << QStringLiteral("--exclude-tools") << sExcl;
    }
    // 会话恢复：pi 磁盘会话跨进程可续；带 resumeSid 时禁加 --no-session（否则不落盘）。
    m_sResumeSid = a_sResumeSid;
    if (!a_sResumeSid.isEmpty()) {
        aArgs << QStringLiteral("--session-id") << a_sResumeSid;
    }

    // 环境：继承 + 按 provider 注入 API key（NFR-4：不进命令行、不打日志）。
    //   2026.08.20 激活自定义条目时旁路 env 注入（密钥在 models.json 的 provider 配置里）。
    QProcessEnvironment oEnv = QProcessEnvironment::systemEnvironment();
    // 2026.08.22 中文编码根治（同 CCodexRunner）：子进程全链路 UTF-8，防 GBK 乱码/挂起。
    oEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    oEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    oEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    oEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    if (!PiApiKey().isEmpty() && PiActiveProvider().isEmpty()) {
        QString sEnvName;
        const QString sP = PiProvider().toLower();
        if (sP == QLatin1String("anthropic"))        { sEnvName = QStringLiteral("ANTHROPIC_API_KEY"); }
        else if (sP == QLatin1String("openai"))      { sEnvName = QStringLiteral("OPENAI_API_KEY"); }
        else if (sP == QLatin1String("google"))      { sEnvName = QStringLiteral("GOOGLE_API_KEY"); }
        else if (sP == QLatin1String("deepseek"))    { sEnvName = QStringLiteral("DEEPSEEK_API_KEY"); }
        else if (sP == QLatin1String("openrouter"))  { sEnvName = QStringLiteral("OPENROUTER_API_KEY"); }
        else if (sP == QLatin1String("xai"))         { sEnvName = QStringLiteral("XAI_API_KEY"); }
        else if (sP == QLatin1String("groq"))        { sEnvName = QStringLiteral("GROQ_API_KEY"); }
        else if (sP == QLatin1String("moonshot"))    { sEnvName = QStringLiteral("MOONSHOT_API_KEY"); }
        if (!sEnvName.isEmpty()) {
            oEnv.insert(sEnvName, PiApiKey());
        }
        // 未知名 provider：不注入，靠 pi 自身 auth 文件（设置页 tooltip 有指引）。
    }
    m_pProc->setProcessEnvironment(oEnv);
    m_pProc->setWorkingDirectory(a_sWorkDir);
    emit sigInfo(QStringLiteral("[system/pi] 启动：%1 %2").arg(
        QDir::toNativeSeparators(ResolveCliJsPath()),
        QDir::toNativeSeparators(a_sWorkDir)));
    m_pProc->start(ResolveNodePath(), aArgs);
    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("无法启动 PIAgent 进程：%1 %2").arg(
            ResolveNodePath(), ResolveCliJsPath()));
        return;
    }
    m_actClock.restart();
    if (m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    // prompt 经 stdin 投递后关闭写通道（实测可行，见 pi-json-sampling.md §5）。
    m_pProc->write(a_sPrompt.toUtf8());
    m_pProc->closeWriteChannel();
}

void CPiAgentRunner::Stop() {
    // 静默收尾（同 claude/DSH 语义）：先置终态再 kill，不发 sigFinished。
    m_bFinished = true;
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
}

bool CPiAgentRunner::IsRunning() const {
    return m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning && !m_bFinished;
}

bool CPiAgentRunner::IsRetryPending() const {
    return false;   // 一次性进程不做快速失败重试（pi 冷启动轻，失败直接报上层）
}

bool CPiAgentRunner::InjectUserTurn(const QString& a_sText) {
    Q_UNUSED(a_sText);
    return false;   // 一次性进程无轮次中插话（安全 no-op）
}

void CPiAgentRunner::slotOnStdOut() {
    m_actClock.restart();   // 收到输出 = 进程活着，停滞计时清零
    m_buf += m_pProc->readAllStandardOutput();
    int iIdx = 0;
    while ((iIdx = m_buf.indexOf('\n')) >= 0) {
        QByteArray sLine = m_buf.left(iIdx);
        m_buf = m_buf.mid(iIdx + 1);
        sLine = sLine.trimmed();
        if (!sLine.isEmpty()) {
            HandleLine(sLine);
        }
    }
}

void CPiAgentRunner::slotOnStdErr() {
    m_actClock.restart();   // stderr 字节同样算活跃
    const QByteArray bufData = m_pProc->readAllStandardError();
    const QList<QByteArray> aLines = bufData.split('\n');
    for (const QByteArray& szRaw : aLines) {
        const QString sLine = QString::fromUtf8(szRaw).trimmed();
        if (sLine.isEmpty()) {
            continue;
        }
        m_aStderrTail.append(sLine);
        if (m_aStderrTail.size() > 15) {
            m_aStderrTail.removeFirst();
        }
        emit sigInfo(QStringLiteral("[stderr] ") + sLine);
    }
}

void CPiAgentRunner::HandleLine(const QByteArray& a_sLine) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        emit sigInfo(QString::fromUtf8(a_sLine));   // 非 JSON 行原样上抛（排查用）
        return;
    }
    const QJsonObject oRoot = stDoc.object();
    const QString sType = oRoot.value(QStringLiteral("type")).toString();

    if (sType == QStringLiteral("session")) {
        // 会话头：回读真实 session id（供存档/续跑；resumeSid 生效时二者一致）。
        m_sLastSessionId = oRoot.value(QStringLiteral("id")).toString();
        m_bSawSession = true;   // 本轮已确认 sid，失败收尾可安全携带
        return;
    }
    if (sType == QStringLiteral("message_update")) {
        // 流式文本增量（delta-only）：2026.08.20 起不再逐 Δ 上抛——AutoCode 的对话块
        //   以「一条 assistant 消息 = 一块」为口径（同 claude/DSH 的 message_end 整块），
        //   逐 Δ 发会把一句话拆成上百个小块（消息视图输出碎裂）。改为在 message_end
        //   里取最终完整文本一次性上抛（见 HandleMessageEnd）。
        return;
    }
    if (sType == QStringLiteral("message_end")) {
        HandleMessageEnd(oRoot.value(QStringLiteral("message")).toObject());
        return;
    }
    if (sType == QStringLiteral("tool_execution_end")) {
        // 工具结果：result 容错为字符串/数组/对象（TextContent[] 等价拼接文本）。
        const QJsonValue vResult = oRoot.value(QStringLiteral("result"));
        QString sResult;
        if (vResult.isString()) {
            sResult = vResult.toString();
        } else if (vResult.isArray()) {
            for (const QJsonValue& v : vResult.toArray()) {
                sResult += v.toObject().value(QStringLiteral("text")).toString();
            }
        } else if (vResult.isObject()) {
            const QJsonObject oR = vResult.toObject();
            sResult = oR.value(QStringLiteral("content")).toString();
            if (sResult.isEmpty()) {
                sResult = QString::fromUtf8(
                    QJsonDocument(oR).toJson(QJsonDocument::Compact));
            }
        }
        if (oRoot.value(QStringLiteral("isError")).toBool()) {
            sResult += QStringLiteral(" [错误]");
        }
        if (sResult.size() > 400) {
            sResult = sResult.left(400) + QStringLiteral("...");
        }
        emit sigToolResult(sResult);
        return;
    }
    if (sType == QStringLiteral("agent_end")) {
        // 记录收尾数据，待进程干净退出后由 slotOnFinished 报 sigFinished(true)。
        FinishFromAgentEnd(oRoot.value(QStringLiteral("messages")).toArray());
        return;
    }
    // compaction_start/end、queue_update、agent/turn/message_start、
    //   tool_execution_start/update 等：忽略（compaction 可提示一条）。
    if (sType == QStringLiteral("compaction_start")) {
        emit sigInfo(QStringLiteral("[system/pi] 上下文自动压缩进行中…"));
    }
}

void CPiAgentRunner::HandleMessageEnd(const QJsonObject& oMsg) {
    if (oMsg.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")) {
        return;   // user/toolResult 回显不处理（AutoCode 侧自知内容）
    }
    ++m_iLastReqCount;   // 每条 assistant 消息 = 一次 API 调用（同 claude/DSH 口径）
    // provider/model（供上层查上下文窗口）。
    const QString sModel = oMsg.value(QStringLiteral("model")).toString();
    if (!sModel.isEmpty()) {
        const QString sProvider = oMsg.value(QStringLiteral("provider")).toString();
        m_sLastModel = sProvider.isEmpty() ? sModel
                                           : sProvider + QLatin1Char('/') + sModel;
    }
    ApplyUsage(oMsg.value(QStringLiteral("usage")).toObject());
    m_sLastStopReason = oMsg.value(QStringLiteral("stopReason")).toString();
    // 2026.08.22 API 调用失败透传：stopReason=="error" 时 pi 携带 errorMessage
    //   （如网关 429 限流 / 500 内部错误），content 为空——此前静默无输出。
    //   现实时上抛系统行 + 暂存供收尾消息带出。
    if (m_sLastStopReason == QStringLiteral("error")) {
        const QString sErr = oMsg.value(QStringLiteral("errorMessage")).toString();
        if (!sErr.isEmpty()) {
            m_sLastApiError = sErr;
            emit sigInfo(QStringLiteral("⚠ 模型接口错误（本轮无输出）：%1").arg(sErr));
        }
    }
    // content：text 块整块上抛（2026.08.20：流式 delta 不再逐发，此处按 claude/DSH
    //   口径一条消息一块）；toolCall 块 → sigToolUse（arguments 为对象 → 序列化 JSON）。
    const QJsonArray aContent = oMsg.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& v : aContent) {
        const QJsonObject oItem = v.toObject();
        const QString sKind = oItem.value(QStringLiteral("type")).toString();
        if (sKind == QStringLiteral("text")) {
            const QString sText = oItem.value(QStringLiteral("text")).toString();
            if (!sText.isEmpty()) {
                m_sTurnText = sText;
                emit sigAssistantText(sText);   // 整块一次（不再由 delta 流拼块）
            }
        } else if (sKind == QStringLiteral("toolCall")) {
            const QJsonObject oArgs = oItem.value(QStringLiteral("arguments")).toObject();
            emit sigToolUse(oItem.value(QStringLiteral("name")).toString(),
                            QJsonDocument(oArgs).toJson(QJsonDocument::Compact));
        }
    }
}

void CPiAgentRunner::ApplyUsage(const QJsonObject& oUsage) {
    // pi Usage 字段（源码权威，见 pi-json-sampling.md §2）：input/output/cacheRead/
    //   cacheWrite（累计口径）；缺字段容错为 0。reasoning 已含在 output 内，不重复加。
    if (oUsage.isEmpty()) {
        return;
    }
    const qint64 iPlainIn = oUsage.value(QStringLiteral("input")).toInteger();
    const qint64 iCacheRead = oUsage.value(QStringLiteral("cacheRead")).toInteger();
    const qint64 iCacheWrite = oUsage.value(QStringLiteral("cacheWrite")).toInteger();
    const qint64 iIn = iPlainIn + iCacheRead + iCacheWrite;
    const qint64 iOut = oUsage.value(QStringLiteral("output")).toInteger();
    // 计费分量分开累计（三项单价不同，见 ComputeTurnCost；2026.08.20）。
    m_iSumPlainInTok += iPlainIn;
    m_iSumCacheReadTok += iCacheRead;
    m_iSumCacheWriteTok += iCacheWrite;
    m_iSumInTok += iIn;
    m_iSumOutTok += iOut;
    // 上下文占用：本条三输入项之和（脏值 >2×窗口丢弃，同 claude/DSH 口径）。
    const qint64 iWinCap = 2 * CClaudeRunner::ContextWindowForModel(m_sLastModel);
    if (iIn > 0 && iIn != m_iLastContextFill && iIn <= iWinCap) {
        m_iLastContextFill = iIn;
        emit sigContextFill(iIn);
    }
}

double CPiAgentRunner::ComputeTurnCost() const {
    // 轮内 token 分量 × 单价折算（$/M）：pi 协议只报 token 不报金额（与 claude CLI
    //   自带 total_cost_usd 不同），按对齐价目估算；单价全 0 → 0（费用列回 "-"）。
    return (static_cast<double>(m_iSumPlainInTok) * s_dPiPriceInPerM
          + static_cast<double>(m_iSumCacheReadTok) * s_dPiPriceCacheReadPerM
          + static_cast<double>(m_iSumCacheWriteTok) * s_dPiPriceCacheWritePerM
          + static_cast<double>(m_iSumOutTok) * s_dPiPriceOutPerM) / 1000000.0;
}

void CPiAgentRunner::FinishFromAgentEnd(const QJsonArray& aMessages) {
    m_bAgentEnded = true;
    // 兜底：agent_end 前没有 message_end（异常流）时从 messages 尾部找最后一条 assistant。
    if (m_sTurnText.isEmpty()) {
        for (auto it = aMessages.end(); it != aMessages.begin();) {
            --it;
            const QJsonObject oMsg = it->toObject();
            if (oMsg.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")) {
                continue;
            }
            for (const QJsonValue& v : oMsg.value(QStringLiteral("content")).toArray()) {
                const QJsonObject oItem = v.toObject();
                if (oItem.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                    m_sTurnText = oItem.value(QStringLiteral("text")).toString();
                }
            }
            break;
        }
    }
}

void CPiAgentRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    Q_UNUSED(a_eStatus);
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_bFinished) {
        return;   // Stop/stall/已收尾：静默吞掉
    }
    if (a_iExitCode == 0 && m_bAgentEnded) {
        // 正常收敛：agent_end 已到、进程干净退出。stopReason=="length"（输出 token
        //   上限）复用 claude 词汇 error_max_turns，上层 LastWasMaxTurns 自动续跑
        //   （pi 磁盘会话凭 --session-id 完整恢复上下文，续跑体验优于 DSH）。
        m_bFinished = true;
        const bool bMaxLen = m_sLastStopReason == QStringLiteral("length");
        m_sLastSubtype = bMaxLen ? QStringLiteral("error_max_turns")
                                 : QStringLiteral("success");
        QString sMsg = m_sTurnText;
        if (sMsg.trimmed().isEmpty()) {
            // 2026.08.22 本轮全是 API 错误（429/500 等）时带出真实原因，不再只说「无输出」。
            sMsg = !m_sLastApiError.isEmpty()
                 ? QStringLiteral("模型接口错误，本轮无输出：%1").arg(m_sLastApiError)
                 : (bMaxLen ? QStringLiteral("达到输出 token 上限（stopReason=length），转自动续跑")
                            : QStringLiteral("(本轮无文本输出)"));
        }
        emit sigFinished(true, sMsg, m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sLastSessionId);
        return;
    }
    if (a_iExitCode == 0 && !m_bAgentEnded) {
        // 退出码 0 但未见 agent_end（流被截断/版本差异）：按已有文本乐观收尾。
        m_bFinished = true;
        m_sLastSubtype = QStringLiteral("success");
        emit sigFinished(true,
                         m_sTurnText.isEmpty()
                             ? (!m_sLastApiError.isEmpty()
                                    ? QStringLiteral("模型接口错误，本轮无输出：%1").arg(m_sLastApiError)
                                    : QStringLiteral("(本轮无文本输出)"))
                             : m_sTurnText,
                         m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sLastSessionId);
        return;
    }
    // 非零退出：统一错误路径（stderr 尾部 + 指引，NFR-3）。
    QString sMsg = QStringLiteral("PIAgent 进程异常退出，退出码=%1").arg(a_iExitCode);
    if (!m_aStderrTail.isEmpty()) {
        QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
        if (sTail.size() > 800) {
            sTail = sTail.left(800) + QStringLiteral("…");
        }
        sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
    }
    sMsg += QStringLiteral("\n处理：检查 PiAgent 根目录/provider/model 与 API key 配置"
                           "（设置 → PiAgent 参数）；或重跑「安装 PIAgent…」。");
    EmitFailed(sMsg);
}

void CPiAgentRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    if (m_bFinished) {
        return;
    }
    QString sMsg;
    switch (a_eError) {
        case QProcess::FailedToStart: sMsg = QStringLiteral("PIAgent 启动失败（node 路径无效或无执行权限）"); break;
        case QProcess::Crashed:       sMsg = QStringLiteral("PIAgent 进程崩溃"); break;
        case QProcess::Timedout:      sMsg = QStringLiteral("PIAgent 进程超时"); break;
        case QProcess::ReadError:     sMsg = QStringLiteral("读取 PIAgent 输出错误"); break;
        case QProcess::WriteError:    sMsg = QStringLiteral("写入 PIAgent 输入错误"); break;
        default:                      sMsg = QStringLiteral("PIAgent 进程未知错误"); break;
    }
    // FailedToStart 不触发 finished，须在此直接收尾；其余错误等 finished 路径。
    if (a_eError == QProcess::FailedToStart) {
        EmitFailed(sMsg);
    }
}

void CPiAgentRunner::CheckStall() {
    // 停滞检测（同 claude/DSH 语义）：进程活着但距上次收到任何输出超 [Agent]
    //   StallMinutes → 判挂起，kill + subtype="stall" 报失败（上层凭 lastSessionId
    //   经 --session-id 自动续跑，pi 磁盘会话跨进程可恢复）。
    if (m_bFinished || m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return;
    }
    const qint64 iLimitMs = static_cast<qint64>(CClaudeRunner::StallMinutes()) * 60000;
    if (m_actClock.elapsed() < iLimitMs) {
        return;
    }
    if (m_pStallTimer != nullptr) {
        m_pStallTimer->stop();
    }
    m_sLastSubtype = QStringLiteral("stall");
    m_bFinished = true;
    emit sigInfo(QStringLiteral("⚠ 停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），"
                                "终止 PIAgent 进程并转自动续跑…").arg(CClaudeRunner::StallMinutes()));
    m_pProc->kill();
    m_pProc->waitForFinished(3000);
    emit sigFinished(false,
                     QStringLiteral("停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），进程已终止")
                         .arg(CClaudeRunner::StallMinutes()),
                     m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sLastSessionId);
}

void CPiAgentRunner::EmitFailed(const QString& a_sMsg) {
    if (m_bFinished) {
        return;
    }
    m_bFinished = true;
    // 本轮未收到 session 头（启动失败/无 key 早退）时不携带上一轮旧 sid，
    // 避免上层据 sigFinished 误续跑到旧会话（评审 v3#1）。
    if (!m_bSawSession) {
        m_sLastSessionId.clear();
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    emit sigInfo(QStringLiteral("[错误] ") + a_sMsg);
    emit sigFinished(false, a_sMsg, m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sLastSessionId);
}
