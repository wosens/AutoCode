#include "CPiAgentRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
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
// 2026.09.22 RPC 形态能力缓存（进程内）：0=未探测，1=可用，-1=不可用；
//   运行期早夭（未见会话即非零退出）另置 s_bRpcRuntimeBroken（探测命中也不再试）。
static int  s_iRpcCapable = 0;
static bool s_bRpcRuntimeBroken = false;

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

static QString s_sModelsJsonPathOverride;   // 2026.09.09 v2.1.133 测试隔离（空=真实路径）
void CPiAgentRunner::SetModelsJsonPathOverrideForTest(const QString& a_sPath) {
    s_sModelsJsonPathOverride = a_sPath;
}
QString CPiAgentRunner::PiModelsJsonPath() {
    return s_sModelsJsonPathOverride.isEmpty()
        ? QDir::homePath() + QStringLiteral("/.pi/agent/models.json")
        : s_sModelsJsonPathOverride;
}

bool CPiAgentRunner::WritePiModelsJson() {
    // 合并写：保留用户手写的非 autocode-* provider 与其他顶层键；autocode-* 槽位
    //   全删重建（下标随表变化，旧槽位即失效）。条目 → provider：
    //     { baseUrl, api, apiKey（空则占位 "-"，本地网关忽略鉴权的场景 pi 才放行）,
    //       models:[{ id, name, contextWindow, maxTokens }] }
    const QString sPath = PiModelsJsonPath();
    QDir().mkpath(QFileInfo(sPath).absolutePath());
    QJsonObject oRoot;
    CFrwDevice stFile(sPath);
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
    // 2026.09.09 v2.1.140 多实例加固（勿回退）：①内容比对——磁盘正本与目标字节一致
    //   时跳过写盘（同配置多实例的热重载/StartStage 自愈常态零写盘，减少共享文件抖动）；
    //   ②原子替换——旧版 truncate+写非原子，并发实例的 pi 进程启动可能读到半截
    //   JSON 解析失败 → 全槽丢失（历史「Unknown provider autocode-n」竞态的深层根源），
    //   改 Hermes EnsureHermesHomeConfig 同款 .tmp→remove→rename 口径。
    const QByteArray aNew = QJsonDocument(oRoot).toJson(QJsonDocument::Indented);
    {
        CFrwDevice stCur(sPath);
        if (stCur.open(QIODevice::ReadOnly)) {
            const QByteArray aCur = stCur.readAll();
            stCur.close();
            if (aCur == aNew) {
                return true;   // 已一致，零写盘
            }
        }
    }
    const QString sTmp = sPath + QStringLiteral(".tmp");
    {
        QFile stTmp(sTmp);
        if (!stTmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        if (stTmp.write(aNew) != aNew.size()) {
            stTmp.close();
            QFile::remove(sTmp);
            return false;
        }
        stTmp.close();
    }
    // rename 原子替换；目标已被并发实例抢先替换时 remove/rename 失败——兑底删临时
    //   文件（对方写入与本方目标来自同一内存表，最终状态仍正确）。
    if (!QFile::remove(sPath) && QFile::exists(sPath)) {
        QFile::remove(sTmp);
        return false;
    }
    if (!QFile::rename(sTmp, sPath)) {
        QFile::remove(sTmp);
        return false;
    }
    return true;
}

QString CPiAgentRunner::PiSettingsJsonPath() {
    return QDir::homePath() + QStringLiteral("/.pi/agent/settings.json");
}

bool CPiAgentRunner::EnsurePiRetrySettings() {
    // 2026.08.28 429 限流自愈（幂等合并写，同 models.json 口径）：
    //   pi 轮级自动重试对 429/5xx 瞬态错误默认开启（settings.retry.enabled 默认 true、
    //   maxRetries 默认 3），但基准退避 2s（2s/4s/8s 全程 ~15s）对分钟级滚动 TPM/RPM
    //   配额限流无效——三次重试全落在配额未恢复窗口内，连击失败后 pi 放弃整轮。
    //   补写 baseDelayMs=15000：15s/30s/60s 指数退避跨过配额滚动窗，重试成功率大幅
    //   提升；错误分类正则（pi ai/utils/retry.ts）对 "429" 命中可重试、对
    //   "insufficient_quota/quota exceeded/billing" 判不可重试（配额耗尽类不盲等）。
    //   已有 retry 段（用户手工配置/先前已写）一律不动，尊重显式配置。
    const QString sPath = PiSettingsJsonPath();
    QDir().mkpath(QFileInfo(sPath).absolutePath());
    QJsonObject oRoot;
    bool bParsed = false;
    CFrwDevice stIn(sPath);
    if (stIn.open(QIODevice::ReadOnly)) {
        QJsonParseError stErr;
        const QJsonDocument oDoc = QJsonDocument::fromJson(stIn.readAll(), &stErr);
        stIn.close();
        if (stErr.error == QJsonParseError::NoError && oDoc.isObject()) {
            oRoot = oDoc.object();
            bParsed = true;
        }
    }
    if (bParsed && oRoot.contains(QStringLiteral("retry"))) {
        return true;   // 已配置：不动（用户显式配置优先）
    }
    // 文件不存在/解析失败（坏档重建即修复——pi 对坏 settings.json 自身无法启动）/无段：
    //   保留其余顶层键，补写 retry 段。
    QJsonObject oRetry;
    oRetry.insert(QStringLiteral("enabled"), true);
    oRetry.insert(QStringLiteral("maxRetries"), 3);
    oRetry.insert(QStringLiteral("baseDelayMs"), 15000);
    oRoot.insert(QStringLiteral("retry"), oRetry);
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

// —— 2026.09 版本管理：Pi 安装形态与本地版本（头文件注释为准） ——

CPiAgentRunner::EPiForm CPiAgentRunner::DetectPiForm() {
    // 优先级与 ResolveCliJsPath 一致：root 固定布局 → npm 全局。
    const QString sRoot = PiRootPath();
    if (!sRoot.isEmpty()
        && QFileInfo::exists(
            sRoot + QStringLiteral("/pi/packages/coding-agent/package.json"))) {
        return EPiFormSource;
    }
    const QString sNpmCli = NpmGlobalCliPath();
    if (!sNpmCli.isEmpty()) {
        const QString sPkg = QFileInfo(sNpmCli).absolutePath()
                           + QStringLiteral("/package.json");
        if (QFileInfo::exists(sPkg)) {
            return EPiFormNpm;
        }
    }
    return EPiFormNone;
}

QString CPiAgentRunner::ResolvePiVersion(const QString& a_sRoot) {
    // 读指定 package.json 的 version（open 失败/解析错/无 version 字段→空串）。
    auto fnReadVer = [](const QString& a_sPkg) -> QString {
        CFrwDevice stFile(a_sPkg);
        if (!stFile.open(QIODevice::ReadOnly)) {
            return QString();
        }
        QJsonParseError stErr;
        const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll(), &stErr);
        stFile.close();
        if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
            return QString();
        }
        return oDoc.object().value(QStringLiteral("version")).toString();
    };
    if (!a_sRoot.isEmpty()) {
        // 显式根：只读该根的根包（miss→空串，不回退其它形态）。
        const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
        return fnReadVer(sRoot + QStringLiteral("/pi/packages/coding-agent/package.json"));
    }
    switch (DetectPiForm()) {
        case EPiFormSource:
            return fnReadVer(PiRootPath()
                + QStringLiteral("/pi/packages/coding-agent/package.json"));
        case EPiFormNpm:
            return fnReadVer(QFileInfo(NpmGlobalCliPath()).absolutePath()
                + QStringLiteral("/package.json"));
        default:
            return QString();
    }
}

QString CPiAgentRunner::ResolveNodePath() {
    // 留空走 PATH：QProcess 直起可执行名由系统搜路径（同 DSH 口径）。
    return s_sPiNodePath.isEmpty() ? QStringLiteral("node") : s_sPiNodePath;
}

bool CPiAgentRunner::RpcCapable() {
    // 2026.09.22 RPC 形态探测（进程内缓存）：优先文件布局探测，退回 cli.js 内容
    //   探测。两种 dist 形态：源码仓库/npm 包均为 tsc 树状 dist（cli.js 同级
    //   modes/rpc/rpc-mode.js）；若未来改单文件 bundle，则协议字面量
    //   "extension_ui_request" 必在 cli.js 文本内。运行期早夭记忆优先于探测结果。
    if (s_bRpcRuntimeBroken) {
        return false;
    }
    if (s_iRpcCapable != 0) {
        return s_iRpcCapable > 0;
    }
    const QString sCli = ResolveCliJsPath();
    if (sCli.isEmpty() || !QFile::exists(sCli)) {
        s_iRpcCapable = -1;
        return false;
    }
    const QString sDir = QFileInfo(sCli).absolutePath();
    if (QFileInfo::exists(sDir + QStringLiteral("/modes/rpc/rpc-mode.js"))) {
        s_iRpcCapable = 1;
        return true;
    }
    QFile fCli(sCli);
    if (fCli.open(QIODevice::ReadOnly)
        && fCli.readAll().contains("extension_ui_request")) {
        s_iRpcCapable = 1;
        return true;
    }
    s_iRpcCapable = -1;
    return false;
}

void CPiAgentRunner::ResetRpcCapCacheForTest() {
    s_iRpcCapable = 0;
    s_bRpcRuntimeBroken = false;
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
    m_bRpcSettled = false;   // 2026.09.22 RPC 轮收敛标记复位（m_bRpcMode 由 Impl 定）
    m_sTurnText.clear();
    m_sLastStopReason.clear();
    m_sLastApiError.clear();   // 2026.08.22 API 错误透传（见头文件注释）
    m_sResumeSid.clear();
    m_iSumPlainInTok = 0;
    m_iSumCacheReadTok = 0;
    m_iSumCacheWriteTok = 0;
    m_iSumInTok = 0;
    m_iSumOutTok = 0;
    m_iLiveEmitIn = 0;    // 2026.08.27 实时计量已发累计复位
    m_iLiveEmitOut = 0;
    m_sLastSubtype.clear();
    m_iLastReqCount = 0;
    m_iLastContextFill = 0;
    m_sLastModel.clear();
    m_aStderrTail.clear();
    m_buf.clear();
    m_iNoMsgHintMs = -1;   // 2026.08.29 流式心跳提示节流复位
}

void CPiAgentRunner::StartStage(const QString& a_sWorkDir,
                                const QString& a_sPrompt,
                                const QString& a_sDisallowedTools,
                                int a_iMaxTurns,
                                const QString& a_sResumeSid,
                                bool a_bFork,
                                int a_iAutocompactK,
                                bool a_bEnableAskUser) {
    // 2026.09.22 双形态入口：存回退参数（RPC 早夭时当轮重拉一次性形态，见
    //   slotOnFinished），形态由能力探测决定。
    m_stRetry.sWorkDir = a_sWorkDir;
    m_stRetry.sPrompt = a_sPrompt;
    m_stRetry.sDisallowed = a_sDisallowedTools;
    m_stRetry.iMaxTurns = a_iMaxTurns;
    m_stRetry.sResumeSid = a_sResumeSid;
    m_stRetry.bFork = a_bFork;
    m_stRetry.iAutocompactK = a_iAutocompactK;
    m_stRetry.bEnableAskUser = a_bEnableAskUser;
    StartStageImpl(a_sWorkDir, a_sPrompt, a_sDisallowedTools, a_iMaxTurns,
                   a_sResumeSid, a_bFork, a_iAutocompactK, a_bEnableAskUser,
                   RpcCapable());
}

void CPiAgentRunner::StartStageImpl(const QString& a_sWorkDir,
                                    const QString& a_sPrompt,
                                    const QString& a_sDisallowedTools,
                                    int a_iMaxTurns,
                                    const QString& a_sResumeSid,
                                    bool a_bFork,
                                    int a_iAutocompactK,
                                    bool a_bEnableAskUser,
                                    bool a_bRpc) {
    Q_UNUSED(a_bFork);
    Reset();
    m_bRpcMode = a_bRpc;

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

    // 离线约束（NFR-5）：跳过扩展/主题发现扫描，且避免工程本地扩展被静默加载
    //   （安全面收敛）；--offline 跳过更新检查。
    //   2026.08.24 技能放开（便携口径，不写用户主目录）：不再传 --no-skills——pi
    //   默认发现 ~/.pi/agent/skills、<工程>/.pi/skills；同时经 --skill 依次挂载
    //   CAgentRunnerBase::SharedSkillDirs()：<exe>\skills（便携）与 ~/.claude/skills。
    QStringList aArgs;
    aArgs << ResolveCliJsPath();
    if (m_bRpcMode) {
        // 2026.09.22 RPC 形态（见类头注释）：JSONL 双向管道，prompt/steer 经命令帧。
        aArgs << QStringLiteral("--mode") << QStringLiteral("rpc");
    } else {
        aArgs << QStringLiteral("-p")
              << QStringLiteral("--mode") << QStringLiteral("json");
    }
    aArgs << QStringLiteral("--offline")
          << QStringLiteral("--no-extensions")
          << QStringLiteral("--no-themes")
          // 非交互全自动：忽略（而非信任）工程本地可执行内容，避免无头模式挂起。
          << QStringLiteral("--no-approve");
    const QStringList aSkillDirs = CAgentRunnerBase::SharedSkillDirs();
    for (const QString& sDir : aSkillDirs) {
        aArgs << QStringLiteral("--skill") << sDir;
    }
    // 2026.08.20 自定义大模型激活条目覆盖固定路由：provider=autocode-<n>、model=条目
    //   模型 id（apiKey 走 models.json，不走 env 注入）；无激活 → Provider/模型 键。
    // 2026-10-07 对话级模型覆盖：会话级下标优先于全局激活（per-runner 实例槽，不动
    //   静态表/全局配置——右键「选择模型」仅本助手生效的链路终点）。RPC 形态每轮
    //   起新进程、一次性形态同理，覆盖下一轮即生效。
    const int iOvProv = ModelOvIdxIn(s_aPiModels.size());
    const int iUseProv = (iOvProv >= 0) ? iOvProv : s_iPiActiveModel;
    const bool bCustomActive = (iUseProv >= 0 && iUseProv < s_aPiModels.size());
    const QString sCustomProvider = bCustomActive
        ? QStringLiteral("autocode-%1").arg(iUseProv + 1) : QString();
    // 2026.08.21 自愈：多实例/热重载竞态可能把 models.json 的 autocode-* 槽位清掉
    //   （磁盘正本与内存表失配 → pi 报 Unknown provider "autocode-n"）。每次启动前
    //   幂等重写一遍（保留用户手写 provider，代价极低），保证槽位与激活路由一致。
    //   （models.json 写全槽位、内容与激活/覆盖下标无关 → 并发不同模型的助手间无竞态）
    if (bCustomActive) {
        WritePiModelsJson();
    }
    // 2026.08.28 429 限流自愈：无条件（固定路由同样会 429/5xx）幂等补写 settings.json
    //   的 retry 段（已有段不动；详见 EnsurePiRetrySettings 注释）。
    EnsurePiRetrySettings();
    aArgs << QStringLiteral("--provider")
          << (bCustomActive ? sCustomProvider : PiProvider());
    const QString sModelArg = bCustomActive
        ? s_aPiModels.at(iUseProv).sModelId : PiModel();
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
    // 2026-09-12 手工审批弹窗通道（AUTOCODE_ASK_EXE/ASK_PIPE，全后端统一）
    InjectAskEnv(oEnv);
    // 2026.08.22 中文编码根治（同 CCodexRunner）：子进程全链路 UTF-8，防 GBK 乱码/挂起。
    oEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    oEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    oEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    oEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    // 2026.08.28 助手提速：长缓存保留（cache_control.ttl=1h）。Anthropic 临时缓存默认 5 分钟
    //   过期——用户每步间隔常超 5 分钟，过期后网关全量重算整个上下文（慢 prefill 网关上
    //   几十秒/步）；长保留把窗口拉到 1 小时（网关不认 ttl 也无害，实测请求正常）。
    oEnv.insert(QStringLiteral("PI_CACHE_RETENTION"), QStringLiteral("long"));
    if (!PiApiKey().isEmpty() && !bCustomActive) {
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
    // 2026.08.24 全局规则运行时注入（便携口径，不复制进 ~/.pi）：pi 的全局上下文
    //   文件是 <agentDir>/AGENTS.md，为免向用户主目录写文件，改为把共享全局规则
    //   （<exe>\CLAUDE.md 或 ~/.claude/CLAUDE.md）直接前置进本阶段 prompt。
    //   工程根的 CLAUDE.md pi 原生发现，无需处理。
    QString sPrompt = a_sPrompt;
    const QString sGlobalCtx = CAgentRunnerBase::SharedGlobalContextContent();
    if (!sGlobalCtx.isEmpty()) {
        sPrompt.prepend(QStringLiteral(
            "以下是全局工程约定（CLAUDE.md），在整个任务中始终遵守：\n\n%1\n\n---\n\n")
            .arg(sGlobalCtx));
        emit sigInfo(QStringLiteral("[system/pi] 已注入全局规则（%1 字符）")
                         .arg(QString::number(sGlobalCtx.size())));
    }
    if (!aSkillDirs.isEmpty()) {
        emit sigInfo(QStringLiteral("[system/pi] 已挂载技能目录：%1")
            .arg(aSkillDirs.join(QStringLiteral("；"))));
    }
    emit sigInfo(QStringLiteral("[system/pi] 启动：%1 %2%3").arg(
        QDir::toNativeSeparators(ResolveCliJsPath()),
        QDir::toNativeSeparators(a_sWorkDir),
        m_bRpcMode ? QStringLiteral("（RPC 形态，支持运行中追加指令）") : QString()));
    m_pProc->start(ResolveNodePath(), aArgs);
    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("无法启动 PIAgent 进程：%1 %2").arg(
            ResolveNodePath(), ResolveCliJsPath()));
        return;
    }
    m_actClock.restart();
    m_tNoMsg.start();   // 2026.08.29 完整事件计时（心跳提示用）
    m_iNoMsgHintMs = -1;
    if (m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    if (m_bRpcMode) {
        // RPC：先回读会话 id（此形态无 session 头事件），再投 prompt 命令帧；
        //   写通道保持打开——轮中 steer / 收尾优雅退出都依赖它。
        SendRpcFrame(QJsonObject{{QStringLiteral("id"), QStringLiteral("sid")},
                                 {QStringLiteral("type"), QStringLiteral("get_state")}});
        SendRpcFrame(QJsonObject{{QStringLiteral("id"), QStringLiteral("go")},
                                 {QStringLiteral("type"), QStringLiteral("prompt")},
                                 {QStringLiteral("message"), sPrompt}});
    } else {
        // 一次性：prompt 经 stdin 投递后关闭写通道（实测可行，见 pi-json-sampling.md §5）。
        m_pProc->write(sPrompt.toUtf8());
        m_pProc->closeWriteChannel();
    }
}

void CPiAgentRunner::SendRpcFrame(const QJsonObject& a_oCmd) {
    // JSONL 命令帧：严格 LF 结尾（pi jsonl 读帧器只认 \n）。
    if (m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return;
    }
    m_pProc->write(QJsonDocument(a_oCmd).toJson(QJsonDocument::Compact) + '\n');
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
    // 2026.09.22 RPC 形态轮中注入：运行中、轮未 settle（settle 后新帧只入 pi 待发
    //   队列不再属于本轮——返回 false 交上层排队走下一轮，与 claude result 后口径
    //   一致）。实测：工具执行中注入，下一模型迭代即收到（与 claude stream-json 同效）。
    if (!m_bRpcMode || m_bFinished || m_bRpcSettled || a_sText.trimmed().isEmpty()) {
        return false;
    }
    if (m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return false;
    }
    SendRpcFrame(QJsonObject{{QStringLiteral("id"), QStringLiteral("steer1")},
                             {QStringLiteral("type"), QStringLiteral("steer")},
                             {QStringLiteral("message"), a_sText}});
    return true;   // 乐观确认：帧已投递（pi steer 无同步失败语义；拒绝响应另行提示）
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
        // 2026.08.27 悬空 sid 场景友好化：pi 对不存在的 --session-id 会打英文 Warning
        //   并自动用该 id 新建会话（实测自愈，不失败）——补中文说明，用户可知上下文
        //   已重新开始（工程自其他电脑拷贝时常见）。
        const QString sRaw = QString::fromUtf8(a_sLine);
        if (sRaw.startsWith(QStringLiteral("Warning: No project session found"))) {
            emit sigInfo(QStringLiteral("↻ 会话在本机不存在（工程可能自其他电脑拷贝），"
                                        "pi 已自动新建会话续跑（上下文自本条重新开始）"));
        } else {
            emit sigInfo(sRaw);   // 非 JSON 行原样上抛（排查用）
        }
        return;
    }
    const QJsonObject oRoot = stDoc.object();
    const QString sType = oRoot.value(QStringLiteral("type")).toString();

    if (sType == QStringLiteral("session")) {
        // 会话头（一次性形态）：回读真实 session id（供存档/续跑；resumeSid 生效时二者一致）。
        m_sLastSessionId = oRoot.value(QStringLiteral("id")).toString();
        m_bSawSession = true;   // 本轮已确认 sid，失败收尾可安全携带
        return;
    }
    // —— 2026.09.22 RPC 形态专属帧（一次性形态不会出现这些 type）——
    if (m_bRpcMode) {
        if (sType == QStringLiteral("response")) {
            // 命令响应：get_state 回读会话 id；prompt 提交失败即收尾；steer 拒绝仅提示。
            const QString sCmd = oRoot.value(QStringLiteral("command")).toString();
            const bool bOk = oRoot.value(QStringLiteral("success")).toBool();
            if (sCmd == QStringLiteral("get_state")) {
                const QString sSid = oRoot.value(QStringLiteral("data")).toObject()
                    .value(QStringLiteral("sessionId")).toString();
                if (bOk && !sSid.isEmpty()) {
                    m_sLastSessionId = sSid;
                    m_bSawSession = true;
                }
            } else if (sCmd == QStringLiteral("prompt") && !bOk) {
                m_pProc->kill();
                m_pProc->waitForFinished(3000);
                EmitFailed(QStringLiteral("pi RPC prompt 提交失败：%1")
                               .arg(oRoot.value(QStringLiteral("error")).toString()));
            } else if (sCmd == QStringLiteral("steer") && !bOk) {
                emit sigInfo(QStringLiteral("[system/pi] 追加指令未被 pi 接受：%1")
                                 .arg(oRoot.value(QStringLiteral("error")).toString()));
            }
            return;
        }
        if (sType == QStringLiteral("agent_settled")) {
            // 轮完整收敛（pi 源码 _runAgentPrompt 的 finally：含自动重试/压缩续跑后
            //   仅发一次，不会在重试间隙提前触发）。优雅收尾：关写通道 → pi stdin
            //   end 落盘退出(0) → slotOnFinished 既有成功路径发 sigFinished。
            if (!m_bRpcSettled) {
                m_bRpcSettled = true;
                if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
                    m_pProc->closeWriteChannel();
                }
            }
            return;
        }
        if (sType == QStringLiteral("extension_ui_request")) {
            // --no-extensions 下不应出现（防御）：自动取消，防 select/confirm/input
            //   等待回复的 UI 请求挂起轮次。
            SendRpcFrame(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("extension_ui_response")},
                {QStringLiteral("id"), oRoot.value(QStringLiteral("id")).toString()},
                {QStringLiteral("cancelled"), true}});
            return;
        }
        if (sType == QStringLiteral("queue_update")) {
            return;   // steer 入队/投递可见性：不上屏（InjectUserTurn 侧已有反馈）
        }
    }
    if (sType == QStringLiteral("message_update")) {
        // 流式文本增量（delta-only）：2026.08.20 起不再逐 Δ 上抛——AutoCode 的对话块
        //   以「一条 assistant 消息 = 一块」为口径（同 claude/DSH 的 message_end 整块），
        //   逐 Δ 发会把一句话拆成上百个小块（消息视图输出碎裂）。改为在 message_end
        //   里取最终完整文本一次性上抛（见 HandleMessageEnd）。
        // 2026.08.27 帧自带累计 usage（本消息生成中实时刷新「本轮 ↑↓」；绝对值不入账）。
        const QJsonObject oU = oRoot.value(QStringLiteral("usage")).toObject();
        if (!oU.isEmpty()) {
            const qint64 iIn = oU.value(QStringLiteral("input")).toInteger()
                + oU.value(QStringLiteral("cacheRead")).toInteger()
                + oU.value(QStringLiteral("cacheWrite")).toInteger();
            const qint64 iOut = oU.value(QStringLiteral("output")).toInteger();
            if (iIn > 0 || iOut > 0) {
                emit sigLivePartial(m_iLiveEmitIn + iIn, m_iLiveEmitOut + iOut);
            }
        }
        return;
    }
    if (sType == QStringLiteral("message_end")) {
        m_tNoMsg.restart();   // 完整消息 = 实质进展（心跳提示清零）
        HandleMessageEnd(oRoot.value(QStringLiteral("message")).toObject());
        return;
    }
    if (sType == QStringLiteral("tool_execution_start")
        || sType == QStringLiteral("tool_execution_end")
        || sType == QStringLiteral("turn_start")) {
        m_tNoMsg.restart();   // 工具/新轮次 = 实质进展
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
    // 2026.08.28 轮级自动重试进度透传（429/5xx 瞬态）：pi 重试期间无其他输出，
    //   不透传则 UI 看起来像卡死（配合 EnsurePiRetrySettings 的长退避尤其必要）。
    if (sType == QStringLiteral("auto_retry_start")) {
        const int iAttempt = oRoot.value(QStringLiteral("attempt")).toInt();
        const int iMax = oRoot.value(QStringLiteral("maxAttempts")).toInt();
        const qint64 iDelayMs = oRoot.value(QStringLiteral("delayMs")).toInteger();
        QString sWhy = oRoot.value(QStringLiteral("errorMessage")).toString();
        if (sWhy.size() > 200) {
            sWhy = sWhy.left(200) + QStringLiteral("…");
        }
        emit sigInfo(QStringLiteral("↻ 模型接口瞬态错误（429/5xx 类），%1 秒后自动重试"
                                    "（第 %2/%3 次）：%4")
                         .arg(QString::number(iDelayMs / 1000))
                         .arg(QString::number(iAttempt))
                         .arg(QString::number(iMax))
                         .arg(sWhy));
        return;
    }
    if (sType == QStringLiteral("auto_retry_end")) {
        if (oRoot.value(QStringLiteral("success")).toBool()) {
            emit sigInfo(QStringLiteral("↻ 自动重试成功，继续本轮…"));
        } else {
            QString sFinal = oRoot.value(QStringLiteral("finalError")).toString();
            if (sFinal.size() > 200) {
                sFinal = sFinal.left(200) + QStringLiteral("…");
            }
            emit sigInfo(QStringLiteral("⚠ 自动重试未能恢复：%1").arg(sFinal));
        }
        return;
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
    //   （如网关 429 限流 / 500 内部错误），content 为空。
    //   2026.08.28 调整：不再实时上抛「本轮无输出」警告（pi 对 429/5xx 会自动重试，
    //   auto_retry_start/end 事件另行提示；实时警告与重试提示交替出现且语义误导）。
    //   只暂存 m_sLastApiError：最终失败由收尾 sigFinished 带出；不可重试类错误
    //   （配额耗尽等，无 auto_retry 事件）同样在收尾带出。成功消息到达时清陈旧错误。
    if (m_sLastStopReason == QStringLiteral("error")) {
        const QString sErr = oMsg.value(QStringLiteral("errorMessage")).toString();
        if (!sErr.isEmpty()) {
            m_sLastApiError = sErr;
        }
    } else {
        m_sLastApiError.clear();
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
    // 2026.08.27 逐消息实时计量上抛（本轮 ↑↓ 显示）；收尾 sigFinished 据已发累计报 0 防双计。
    if (iIn > 0 || iOut > 0) {
        m_iLiveEmitIn += iIn;
        m_iLiveEmitOut += iOut;
        emit sigUsageDelta(iIn, iOut);
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
    // 2026.09.22 RPC 早夭自动回退：未见会话即非零退出 = pi 不认识 --mode rpc
    //   （旧版 commander 报错早退 / 半安装态）。进程级记忆禁用 RPC，当轮用一次性
    //   形态重拉同一 prompt（升级/安装异常自愈，用户无感）。已见会话的非零退出
    //   （真配置错/中途崩溃）不回退——走下方既有失败路径带真因。
    if (m_bRpcMode && !m_bSawSession && a_iExitCode != 0) {
        s_bRpcRuntimeBroken = true;
        emit sigInfo(QStringLiteral(
            "[system/pi] RPC 模式不可用（pi 版本过旧或安装异常），自动回退一次性模式"));
        const SRetryParams st = m_stRetry;
        StartStageImpl(st.sWorkDir, st.sPrompt, st.sDisallowed, st.iMaxTurns,
                       st.sResumeSid, st.bFork, st.iAutocompactK, st.bEnableAskUser,
                       /*a_bRpc=*/false);
        return;
    }
    if (a_iExitCode == 0 && m_bAgentEnded) {
        // 正常收敛：agent_end 已到、进程干净退出。stopReason=="length"（输出 token
        //   上限）复用 claude 词汇 error_max_turns，上层 LastWasMaxTurns 自动续跑
        //   （pi 磁盘会话凭 --session-id 完整恢复上下文，续跑体验优于 DSH）。
        m_bFinished = true;
        // 2026.08.28 最后一条 assistant 为 API 错误（429/5xx 自动重试耗尽或不可重试类）
        //   时本轮未收敛：改报失败（旧版报 true + 轮内陈旧文本，上层误判阶段成功、
        //   产出空产物继续推进）。失败消息带真因与限流应对指引。
        if (m_sLastStopReason == QStringLiteral("error")) {
            m_sLastSubtype = QStringLiteral("api_error");
            QString sErr = m_sLastApiError.isEmpty()
                ? QStringLiteral("（无错误详情）") : m_sLastApiError;
            if (sErr.size() > 800) {
                sErr = sErr.left(800) + QStringLiteral("…");
            }
            QString sHint;
            // 2026.08.28 指引分类：quota/套餐额度耗尽类优先判（其消息常含 429 前缀，
            //   误落限流分支会提示“稍等重试”白等；实测 SenseNova "token plan
            //   entitlement exhausted"/"quota_exceeded_error" 即此类，等待无效）。
            if (sErr.contains(QLatin1String("quota_exceeded"), Qt::CaseInsensitive)
                || sErr.contains(QLatin1String("entitlement"), Qt::CaseInsensitive)
                || sErr.contains(QLatin1String("insufficient_quota"))
                || sErr.contains(QLatin1String("quota exceeded"), Qt::CaseInsensitive)
                || sErr.contains(QLatin1String("out of budget"), Qt::CaseInsensitive)) {
                sHint = QStringLiteral("\n处理：套餐/账户 token 额度已耗尽（quota 类错误，"
                                       "非临时限流——等待与重试无效）。请在模型服务平台"
                                       "充值/领取额度，或切换其他模型条目"
                                       "（设置 → PiAgent 参数 → 自定义大模型）。");
            } else if (sErr.contains(QLatin1String("429"))
                || sErr.contains(QLatin1String("tpm"), Qt::CaseInsensitive)
                || sErr.contains(QLatin1String("rpm"), Qt::CaseInsensitive)
                || sErr.contains(QLatin1String("rate"), Qt::CaseInsensitive)) {
                sHint = QStringLiteral("\n处理：模型/网关限流（每分钟 token 配额耗尽）——"
                                       "稍等 1~2 分钟后重试；频繁触发请减小上下文"
                                       "（清理会话/换新助手）或换配额更高的模型。"
                                       "（pi 已自动长退避重试 3 次）");
            } else {
                sHint = QStringLiteral("\n处理：检查网关/模型服务可用性与模型条目配置"
                                       "（设置 → PiAgent 参数）。 pi 已自动重试后仍失败。");
            }
            emit sigFinished(false,
                             QStringLiteral("模型接口错误（自动重试后仍失败）：%1").arg(sErr) + sHint,
                             (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                             (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                             ComputeTurnCost(), m_sLastSessionId);
            return;
        }
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
            // 2026.08.29 空输出+输出量逼近上限的对症指引：thinking=high 时模型可把整轮
            //   maxTokens 预算耗在思考段（实测拳皇任务：out=32768 正好打满、16.7 分钟
            //   无任何正文/工具），空转一轮还烧钱——指路「调低思考级别或调大最大输出」。
            if (m_sLastApiError.isEmpty() && m_iSumOutTok >= 30000) {
                sMsg += QStringLiteral("\n⚠ 本轮输出 %1 token 已逼近「最大输出」上限且无正文产出——"
                                       "疑似思考段耗尽了整个输出预算。处理：设置 → PiAgent 参数 "
                                       "→ 把「思考级别」调低（medium/low），或调大条目「最大输出」"
                                       "（网关上限 131072）后重发。")
                            .arg(QString::number(m_iSumOutTok));
            }
        }
        emit sigFinished(true, sMsg,
                         (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                         (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                         ComputeTurnCost(), m_sLastSessionId);
        return;
    }
    if (a_iExitCode == 0 && !m_bAgentEnded) {
        // 退出码 0 但未见 agent_end（流被截断/版本差异）：按已有文本乐观收尾；
        //   但最后一条消息为 API 错误时同上改报失败（重试耗尽/不可重试均未收敛）。
        m_bFinished = true;
        if (m_sLastStopReason == QStringLiteral("error")) {
            m_sLastSubtype = QStringLiteral("api_error");
            emit sigFinished(false,
                QStringLiteral("模型接口错误：%1")
                    .arg(m_sLastApiError.isEmpty() ? QStringLiteral("（无错误详情）")
                                                   : m_sLastApiError.left(800)),
                (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                ComputeTurnCost(), m_sLastSessionId);
            return;
        }
        m_sLastSubtype = QStringLiteral("success");
        emit sigFinished(true,
                         m_sTurnText.isEmpty()
                             ? (!m_sLastApiError.isEmpty()
                                    ? QStringLiteral("模型接口错误，本轮无输出：%1").arg(m_sLastApiError)
                                    : QStringLiteral("(本轮无文本输出)"))
                             : m_sTurnText,
                         (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                         (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                         ComputeTurnCost(), m_sLastSessionId);
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
    // 2026.08.29 流式心跳提示：进程活着、增量字节仍在流（m_actClock 新鲜）但超过
    //   3 分钟没有任何完整事件（message_end/工具/turn_start）——典型为 thinking=high
    //   下模型长时间深度思考（实测单条消息流式 16 分钟打满 maxTokens），UI 只显示
    //   完整消息、流式 usage 帧多为 0，用户侧看起来就是「卡死」。每 60s 补一条
    //   可见提示（不终止进程；硬停滞检测只认总输出静默，不受影响）。
    if (m_tNoMsg.isValid() && m_tNoMsg.elapsed() > 180000
        && m_actClock.elapsed() < 45000) {
        const qint64 iNoMsg = m_tNoMsg.elapsed();
        if (m_iNoMsgHintMs < 0 || iNoMsg - m_iNoMsgHintMs >= 60000) {
            m_iNoMsgHintMs = iNoMsg;
            emit sigInfo(QStringLiteral("⏳ 模型仍在流式生成（已 %1 分钟无完整消息/工具事件，"
                                        "输出仍在到达）——疑似深度思考中，并非卡死；"
                                        "如需中断可点停止，或将思考级别调低/调大「最大输出」")
                             .arg(QString::number(iNoMsg / 60000)));
        }
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
