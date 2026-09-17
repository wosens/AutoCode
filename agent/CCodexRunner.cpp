#include "CCodexRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>

#include "CClaudeRunner.h"   // StallMinutes：巡检阈值复用同一实现
#include "CCodexProxy.h"     // 2026.08.21 本地 max_tokens 注入代理

// —— 进程级全局配置（static：所有 Codex runner 实例共享，主窗口启动/设置变更时写）——
static QString s_sCodexRoot;      // npm 工程根（Execute\CodexHarness；空=未配置）
static QString s_sCodexNodePath;  // node.exe 路径（空=走 PATH 的 "node"）
static QString s_sCodexProvider;  // provider id（空="autocode"，~/.codex 固有槽位）
static QString s_sCodexModel;     // 模型 id（必填，缺项时 CodexConfigError 报指引）
static QString s_sCodexApiKey;    // API key（明文存 config.ini；仅写入 auth.json）
static QString s_sCodexBaseUrl;   // 网关 base_url（必填）
static QString s_sCodexWireApi;   // "chat"/"responses"（默认 chat）
// 2026.08.21 自定义大模型表（[CodexModels]）：激活条目覆盖固定路由（同 Pi 口径）。
static QList<SDshModelEntry> s_aCodexModels;
static int s_iCodexActiveModel = -1;   // 激活条目下标（-1=走固定键路由）

// 生效值解析（自定义路由激活时取激活条目，否则取固定键）：
//   provider=autocode-<n>（n=下标+1，槽位语义同 pi 的 models.json）；wireApi 取条目
//   sApi（chat/responses）；key 取条目 sApiKey（空则回退固定 CodexApiKey）。
static const SDshModelEntry* CodexActiveEntry() {
    return (s_iCodexActiveModel >= 0 && s_iCodexActiveModel < s_aCodexModels.size())
        ? &s_aCodexModels.at(s_iCodexActiveModel) : nullptr;
}
static QString CodexEffProvider() {
    return CodexActiveEntry() != nullptr ? CCodexRunner::CodexActiveProvider()
                                         : CCodexRunner::CodexProvider();
}
static QString CodexEffModel() {
    return CodexActiveEntry() != nullptr ? CodexActiveEntry()->sModelId
                                         : CCodexRunner::CodexModel();
}static QString CodexEffBaseUrl() {
    return CodexActiveEntry() != nullptr ? CodexActiveEntry()->sBaseURL
                                         : CCodexRunner::CodexBaseUrl();
}
static QString CodexEffWireApi() {
    if (CodexActiveEntry() != nullptr) {
        const QString sApi = CodexActiveEntry()->sApi.trimmed().toLower();
        if (sApi == QLatin1String("responses")) {
            return QStringLiteral("responses");
        }
        return QStringLiteral("chat");
    }
    const QString s = CCodexRunner::CodexWireApi();
    return s.isEmpty() ? QStringLiteral("chat") : s;
}
static QString CodexEffApiKey() {
    if (CodexActiveEntry() != nullptr && !CodexActiveEntry()->sApiKey.isEmpty()) {
        return CodexActiveEntry()->sApiKey;
    }
    return CCodexRunner::CodexApiKey();
}
// 2026.08.21 生效上下文窗口（token）：激活条目 iCtx（下限钳 8000 防误填 0）；
//   固定路由无对应键，取条目默认量级 200000。写入 config.toml 顶层
//   model_context_window——codex 对自定义模型不知道真实窗口时，估算超限即报
//   「ran out of room in the model's context window」提前终止。
static qint64 CodexEffContextWindow() {
    const qint64 iCtx = CodexActiveEntry() != nullptr ? CodexActiveEntry()->iCtx : 200000;
    return iCtx > 8000 ? iCtx : 200000;
}
// 2026.08.21 生效单次最大输出（token）：条目「最大输出」iMaxTok，钳
//   [1024, 131072]（0/误填兜底 32768）。codex 0.87 chat 请求体不带 max_tokens，
//   网关默认输出上限 4096——长输出轮被截断（finish_reason=length）且 codex 误报
//   「ran out of room in the model's context window」；经 CCodexProxy 本地注入。
static int CodexEffMaxTokens() {
    int iTok = CodexActiveEntry() != nullptr ? CodexActiveEntry()->iMaxTok : 0;
    if (iTok <= 0) {
        iTok = 32768;
    }
    return iTok < 1024 ? 1024 : (iTok > 131072 ? 131072 : iTok);
}

// 根目录内固定相对布局（npm 本地工程安装：node_modules\@openai\codex）。
static constexpr const char* kSzCodexJsRel     = "/node_modules/@openai/codex/bin/codex.js";
static constexpr const char* kSzCodexPkgRel    = "/node_modules/@openai/codex/package.json";

void CCodexRunner::SetCodexRoot(const QString& a_sRoot) {
    s_sCodexRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    while (s_sCodexRoot.endsWith(QLatin1Char('/'))) {
        s_sCodexRoot.chop(1);
    }
}

QString CCodexRunner::CodexRoot() {
    return s_sCodexRoot;
}

void CCodexRunner::SetCodexNodePath(const QString& a_sPath) {
    s_sCodexNodePath = a_sPath.trimmed();
}

QString CCodexRunner::CodexNodePath() {
    return s_sCodexNodePath;
}

void CCodexRunner::SetCodexProvider(const QString& a_sProvider) {
    s_sCodexProvider = a_sProvider.trimmed();
}

QString CCodexRunner::CodexProvider() {
    return s_sCodexProvider.isEmpty() ? QStringLiteral("autocode") : s_sCodexProvider;
}

void CCodexRunner::SetCodexModel(const QString& a_sModel) {
    s_sCodexModel = a_sModel.trimmed();
}

QString CCodexRunner::CodexModel() {
    return s_sCodexModel;
}

void CCodexRunner::SetCodexApiKey(const QString& a_sKey) {
    s_sCodexApiKey = a_sKey.trimmed();
}

QString CCodexRunner::CodexApiKey() {
    return s_sCodexApiKey;
}

void CCodexRunner::SetCodexBaseUrl(const QString& a_sUrl) {
    s_sCodexBaseUrl = a_sUrl.trimmed();
}

QString CCodexRunner::CodexBaseUrl() {
    return s_sCodexBaseUrl;
}

void CCodexRunner::SetCodexWireApi(const QString& a_sApi) {
    const QString sApi = a_sApi.trimmed().toLower();
    s_sCodexWireApi = (sApi == QLatin1String("responses")) ? QStringLiteral("responses")
                                                           : QStringLiteral("chat");
}

QString CCodexRunner::CodexWireApi() {
    return s_sCodexWireApi.isEmpty() ? QStringLiteral("chat") : s_sCodexWireApi;
}

// —— 自定义大模型表（2026.08.21；口径同 Pi）——
// 注意：本方法纯内存（不做 IO）：~/.codex 的合成写由 CMainWindow 在设置保存与
//   启动两处显式调 WriteCodexUserConfig 完成（避免单测/探测路径误写真实 ~/.codex）。
void CCodexRunner::SetCodexModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    s_aCodexModels = a_aModels;
    s_iCodexActiveModel = (a_iActive >= 0 && a_iActive < a_aModels.size()) ? a_iActive : -1;
}

QList<SDshModelEntry> CCodexRunner::CodexModels() {
    return s_aCodexModels;
}

int CCodexRunner::CodexActiveModel() {
    return s_iCodexActiveModel;
}

QString CCodexRunner::CodexActiveProvider() {
    return (s_iCodexActiveModel >= 0 && s_iCodexActiveModel < s_aCodexModels.size())
        ? QStringLiteral("autocode-%1").arg(s_iCodexActiveModel + 1) : QString();
}

QString CCodexRunner::ExpectedCodexVersion() {
    return QStringLiteral("0.87.0");
}

QString CCodexRunner::ResolveNodePath() {
    return s_sCodexNodePath.isEmpty() ? QStringLiteral("node") : s_sCodexNodePath;
}

QString CCodexRunner::ResolveCodexJsPath() {
    if (s_sCodexRoot.isEmpty()) {
        return QString();
    }
    return s_sCodexRoot + QString::fromLatin1(kSzCodexJsRel);
}

QString CCodexRunner::ResolveCodexVersion() {
    if (s_sCodexRoot.isEmpty()) {
        return QString();
    }
    QFile stFile(s_sCodexRoot + QString::fromLatin1(kSzCodexPkgRel));
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
}

bool CCodexRunner::LooksLikeCodexRoot(const QString& a_sRoot) {
    const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    return !sRoot.isEmpty()
        && QFileInfo::exists(sRoot + QString::fromLatin1(kSzCodexPkgRel));
}

// install_env.bat /codex 把 npm 工程装在 exe 同级 Execute\CodexHarness；探测基
//   统一在此定义（探测/诊断共用，避免两处漂移）。
static QStringList CodexProbeBases() {
    const QString sExeDir = QCoreApplication::applicationDirPath();
    return {
        sExeDir + QStringLiteral("/Execute"),
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
}

QString CCodexRunner::ProbeSiblingRoot() {
    // 按 CodexProbeBases 顺序探测（exe 同级 Execute 最优先，上级为兼容回退）。
    for (const QString& sBase : CodexProbeBases()) {
        const QString sRoot = QDir(sBase + QStringLiteral("/CodexHarness")).canonicalPath();
        if (LooksLikeCodexRoot(sRoot)) {
            return sRoot;
        }
    }
    return QString();
}

QString CCodexRunner::CodexConfigError() {
    // 可用性统一校验（启动警告/设置保存兜底/StartStage 前置三处同一口径，仿 Pi/DSH）。
    const QString sNode = ResolveNodePath();
    if (sNode == QStringLiteral("node")) {
        if (QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            return QStringLiteral(
                "node 未配置且 PATH 中找不到 node.exe。\n"
                "处理：安装 Node.js（≥20）后重启 AutoCode（node 路径留空即走 PATH），或在"
                "「设置 → CodexHarness 参数 → node 路径」填 node.exe 绝对路径。");
        }
    } else if (!QFile::exists(sNode)) {
        return QStringLiteral(
            "node 路径无效：%1。\n"
            "处理：「设置 → CodexHarness 参数 → node 路径」改填有效的 node.exe"
            "（留空即走 PATH）。").arg(sNode);
    }
    if (s_sCodexRoot.isEmpty() || !LooksLikeCodexRoot(s_sCodexRoot)) {
        return QStringLiteral(
            "CodexHarness 未安装或根目录未配置：%1。\n"
            "处理：「设置 → CodexHarness 参数」点「安装 CodexHarness…」"
            "（install_env.bat /codex，npm 本地安装 @openai/codex 到 exe 同级"
            " Execute\\CodexHarness）；完成后点「重新检测」，根目录自动填入。")
            .arg(s_sCodexRoot.isEmpty() ? QStringLiteral("(空)") : s_sCodexRoot);
    }
    const QString sVer = ResolveCodexVersion();
    if (sVer.isEmpty()) {
        return QStringLiteral(
            "CodexHarness 安装不完整：%1 下未读到 @openai\\codex\\package.json。\n"
            "处理：重跑「安装 CodexHarness…」续装。").arg(s_sCodexRoot);
    }
    if (sVer != ExpectedCodexVersion()) {
        // ≥0.88 已移除 wire_api="chat"（本机网关仅 chat completions 可用）→ 明确指引。
        const QStringList aVer = sVer.split(QLatin1Char('.'));
        const int iMajor = aVer.size() > 0 ? aVer.at(0).toInt() : 0;
        const int iMinor = aVer.size() > 1 ? aVer.at(1).toInt() : 0;
        const QString sExtra = (iMajor > 0 || iMinor >= 88)
            ? QStringLiteral("（v0.88+ 已移除 wire_api=\"chat\"，仅 responses 可用；"
                             "本后端按 chat 网关接入，请安装 0.87.0）")
            : QString();
        return QStringLiteral(
            "Codex CLI 版本不符：当前 %1，本后端锁定 %2。%3\n"
            "处理：「设置 → CodexHarness 参数」点「安装 CodexHarness…」"
            "重装 --save-exact 锁定版本。").arg(sVer, ExpectedCodexVersion(), sExtra);
    }
    // 表单缺项（生效 model/baseUrl 必填；apiKey 空=走 ~/.codex 既有登录态）。
    // 2026.08.21 自定义模型激活时生效值来自条目，固定键允许为空。
    if (CodexEffModel().isEmpty()) {
        return QStringLiteral(
            "CodexHarness 模型未配置。\n"
            "处理：「设置 → CodexHarness 参数」自定义大模型列表里新增一条并"
            "「设为当前」。");
    }
    if (CodexEffBaseUrl().isEmpty()) {
        return QStringLiteral(
            "CodexHarness 网关地址未配置。\n"
            "处理：「设置 → CodexHarness 参数」自定义大模型列表条目里填接入点"
            "（如 http://192.168.x.x:9000/v1）并「设为当前」。");
    }
    return QString();
}

QString CCodexRunner::CodexInstallDiagnosis() {
    for (const QString& sBase : CodexProbeBases()) {
        const QString sRoot = QDir(sBase + QStringLiteral("/CodexHarness")).canonicalPath();
        if (sRoot.isEmpty() || !QDir(sRoot).exists()) {
            continue;
        }
        if (LooksLikeCodexRoot(sRoot)) {
            continue;   // 完整根（探测应已命中）
        }
        return QStringLiteral(
            "发现安装目录但未装全：%1\n"
            "缺：node_modules\\@openai\\codex\\package.json——npm install 未完成，"
            "重跑「安装 CodexHarness…」续装").arg(QDir::toNativeSeparators(sRoot));
    }
    return QStringLiteral("各探测位置（程序目录同级/上级）均无 CodexHarness 安装目录——"
                          "可能是 npm install 未执行或失败，请查看安装窗口的输出按提示"
                          "处理后重装。");
}

// —— ~/.codex 写入（设置页保存时调用；a_sCodexHome 空 = ~/.codex，注入供单测）——
static QString CodexHomeDir(const QString& a_sCodexHome) {
    return a_sCodexHome.isEmpty() ? QDir::homePath() + QStringLiteral("/.codex")
                                  : QDir::fromNativeSeparators(a_sCodexHome.trimmed());
}

static bool WriteCodexToml(const QString& a_sHome, QString* a_pErr) {
    const QString sPath = a_sHome + QStringLiteral("/config.toml");
    // 读全文（不存在=空文件；CRLF/LF 均按行保留原样逐字拼回）。
    QString sAll;
    {
        QFile stFile(sPath);
        if (stFile.open(QIODevice::ReadOnly)) {
            sAll = QString::fromUtf8(stFile.readAll());
            stFile.close();
        }
    }
    QStringList aLines = sAll.split(QLatin1Char('\n'));
    // 末尾换行归一：空尾行去掉，写回时统一补一个换行。
    while (!aLines.isEmpty() && aLines.last().isEmpty()) {
        aLines.removeLast();
    }
    // 备份（存在时）。
    if (QFile::exists(sPath)) {
        QFile::remove(sPath + QStringLiteral(".autocode.bak"));
        QFile::copy(sPath, sPath + QStringLiteral(".autocode.bak"));
    }
    // 1) 顶层键 model/model_provider：存在则原位替换（仅首个 "[" 节标题之前，
    //   避免误改节内同名键），不存在则在该节标题前插入。值取「生效路由」：
    //   自定义模型激活时 = autocode-<n> 槽位 + 条目模型，否则 = 固定键（2026.08.21）。
    const QString sProvider = CodexEffProvider();
    const QString sModel = CodexEffModel();
    const QString sBase = CodexEffBaseUrl();
    const QString sWire = CodexEffWireApi();
    const auto fnEsc = [](const QString& a_sVal) {
        // TOML 双引号串转义（值含 "/\ 时防生成非法 TOML）。
        QString s = a_sVal;
        s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return s;
    };
    const int iFirstSection = [&aLines]() -> int {
        for (int i = 0; i < aLines.size(); ++i) {
            if (aLines.at(i).trimmed().startsWith(QLatin1Char('['))) {
                return i;
            }
        }
        return aLines.size();
    }();
    auto fnTopKey = [&aLines, &fnEsc, iFirstSection](const QString& a_sKey,
                                                     const QString& a_sVal) {
        const QString sPrefix = a_sKey + QStringLiteral(" =");
        for (int i = 0; i < iFirstSection && i < aLines.size(); ++i) {
            if (aLines.at(i).trimmed().startsWith(sPrefix)) {
                aLines[i] = QStringLiteral("%1 = \"%2\"").arg(a_sKey, fnEsc(a_sVal));
                return true;
            }
        }
        // 首个节标题前插入（无节=文件末尾）。
        int iIns = iFirstSection;
        aLines.insert(iIns, QStringLiteral("%1 = \"%2\"").arg(a_sKey, fnEsc(a_sVal)));
        return false;
    };
    fnTopKey(QStringLiteral("model"), sModel);
    fnTopKey(QStringLiteral("model_provider"), sProvider);
    // 2026.08.21 model_context_window：整数键（不带引号），告诉 codex 生效模型的
    //   真实窗口——chat wire_api 无自动压缩，窗口未知/偏小时长阶段会在中途被
    //   「ran out of room in the model's context window」终止。
    {
        const QString sCtx = QStringLiteral("model_context_window = %1")
                                 .arg(CodexEffContextWindow());
        const QString sPrefix = QStringLiteral("model_context_window =");
        bool bReplaced = false;
        for (int i = 0; i < iFirstSection && i < aLines.size(); ++i) {
            if (aLines.at(i).trimmed().startsWith(sPrefix)) {
                aLines[i] = sCtx;
                bReplaced = true;
                break;
            }
        }
        if (!bReplaced) {
            aLines.insert(iFirstSection, sCtx);
        }
    }
    // 2) 删除既有 [model_providers.<provider>] 节（该标题行到下一个 "[" 或 EOF）。
    const QString sSection = QStringLiteral("[model_providers.%1]").arg(sProvider);
    for (int i = 0; i < aLines.size(); ++i) {
        if (aLines.at(i).trimmed() != sSection) {
            continue;
        }
        int j = i + 1;
        while (j < aLines.size() && !aLines.at(j).trimmed().startsWith(QLatin1Char('['))) {
            ++j;
        }
        aLines.remove(i, j - i);
        break;   // 只删第一个同名节（重复节本就非法）
    }
    // 3) 文件末尾追加重建（值经 fnEsc 转义，防 "/\ 生成非法 TOML）。
    aLines << QString()
           << sSection
           << QStringLiteral("name = \"%1\"").arg(fnEsc(sProvider))
           << QStringLiteral("wire_api = \"%1\"").arg(fnEsc(sWire))
           << QStringLiteral("requires_openai_auth = true")
           << QStringLiteral("base_url = \"%1\"").arg(fnEsc(sBase));
    // 4) 原子写：.tmp → rename 替换（目标存在时先删，Windows rename 不覆盖）。
    const QString sTmp = sPath + QStringLiteral(".autocode.tmp");
    {
        QFile stTmp(sTmp);
        if (!stTmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (a_pErr != nullptr) {
                *a_pErr = QStringLiteral("无法写入临时文件 %1（权限不足？）")
                    .arg(QDir::toNativeSeparators(sTmp));
            }
            return false;
        }
        stTmp.write((aLines.join(QLatin1Char('\n')) + QStringLiteral("\n")).toUtf8());
        stTmp.close();
    }
    QFile::remove(sPath);
    if (!QFile::rename(sTmp, sPath)) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("原子替换失败：%1")
                .arg(QDir::toNativeSeparators(sPath));
        }
        return false;
    }
    return true;
}

static bool WriteCodexAuthJson(const QString& a_sHome, QString* a_pErr) {
    const QString sPath = a_sHome + QStringLiteral("/auth.json");
    QJsonObject oRoot;
    bool bParsedOk = !QFile::exists(sPath);
    {
        QFile stFile(sPath);
        if (stFile.open(QIODevice::ReadOnly)) {
            QJsonParseError stErr;
            const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll(), &stErr);
            stFile.close();
            if (stErr.error == QJsonParseError::NoError && oDoc.isObject()) {
                oRoot = oDoc.object();   // 其余键（tokens 等）逐字保留
                bParsedOk = true;
            }
        }
    }
    // apiKey 为空 = 沿用 ~/.codex 既有登录态：不覆写 OPENAI_API_KEY（避免把既有
    //   非空 key 清成空串），auth.json 不存在时也不新建空文件；既有文件非法
    //   JSON（无法解析出既有键）时同样不动它，避免整体重写毁掉无法解析的凭据。
    // 2026.08.21 生效 key：自定义模型激活时取条目 key（条目 key 空回退固定键）。
    const QString sApiKey = CodexEffApiKey();
    if (sApiKey.isEmpty() && !bParsedOk) {
        return true;
    }
    if (!sApiKey.isEmpty()) {
        oRoot.insert(QStringLiteral("OPENAI_API_KEY"), sApiKey);
    }
    QSaveFile stOut(sPath);
    if (!stOut.open(QIODevice::WriteOnly)) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("无法写入 auth.json（权限不足？）");
        }
        return false;
    }
    stOut.write(QJsonDocument(oRoot).toJson(QJsonDocument::Indented));
    if (!stOut.commit()) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("auth.json 落盘失败（磁盘满/被占用？）");
        }
        return false;
    }
    return true;
}

bool CCodexRunner::WriteCodexUserConfig(QString* a_pErr, const QString& a_sCodexHome) {
    const QString sHome = CodexHomeDir(a_sCodexHome);
    if (!QDir().mkpath(sHome)) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("无法创建目录 %1").arg(QDir::toNativeSeparators(sHome));
        }
        return false;
    }
    // 表单缺项前置校验（生效 model/baseUrl 缺→报错，避免写出半截配置）。
    // 2026.08.21 自定义模型激活时条目本身已含 model/baseUrl（LoadCodexModels 拒残缺条目），
    //   固定键允许为空（路由被条目覆盖，不再参与）。
    if (CodexEffModel().isEmpty() || CodexEffBaseUrl().isEmpty()) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("模型/BaseUrl 未配置，未写入 ~/.codex（请先补全表单）");
        }
        return false;
    }
    if (!WriteCodexToml(sHome, a_pErr)) {
        return false;
    }
    if (!WriteCodexAuthJson(sHome, a_pErr)) {
        return false;
    }
    return true;   // 日志只说「已写入」，不含 key 明文（NFR-3）
}

CCodexRunner::CCodexRunner(QObject* a_pParent)
    : CAgentRunnerBase(a_pParent) {
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput, this, &CCodexRunner::slotOnStdOut);
    connect(m_pProc, &QProcess::readyReadStandardError, this, &CCodexRunner::slotOnStdErr);
    connect(m_pProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CCodexRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred, this, &CCodexRunner::slotOnErrorOccurred);
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, [this]() {
        CheckStall();
    });
}

CCodexRunner::~CCodexRunner() {
    Stop();
}

void CCodexRunner::Reset() {
    m_bFinished = false;
    m_bSawThread = false;
    m_bSawAgentMsg = false;
    m_sTurnText.clear();
    m_sErrorEvent.clear();
    m_iSumInTok = 0;
    m_iSumOutTok = 0;
    m_iLastContextFill = 0;
    m_iLastReqCount = 0;
    m_bUsageLive = false;   // 2026.08.21 代理计量标志随阶段复位
    m_sLastModel.clear();
    m_sLastSessionId.clear();
    m_aStderrTail.clear();
    m_buf.clear();
}

void CCodexRunner::StartStage(const QString& a_sWorkDir,
                              const QString& a_sPrompt,
                              const QString& a_sDisallowedTools,
                              int a_iMaxTurns,
                              const QString& a_sResumeSid,
                              bool a_bFork,
                              int a_iAutocompactK,
                              bool a_bEnableAskUser) {
    Q_UNUSED(a_sResumeSid);
    Q_UNUSED(a_bFork);
    Reset();

    // claude 专属参数降级提示（非默认值出现时提示一次，默认路径零噪音）。
    if (a_iMaxTurns != 30 || a_iAutocompactK > 0 || a_bEnableAskUser
        || !a_sDisallowedTools.trimmed().isEmpty()) {
        emit sigInfo(QStringLiteral(
            "[system/codex] claude 专属参数（maxTurns/autocompact/ask_user/disallowed"
            "Tools）在 CodexHarness 后端不适用，已忽略（Codex 沙箱策略统一 "
            "danger-full-access，工具集由各阶段 prompt 约束）"));
    }

    // 启动前校验：缺失直接报失败（带完整处理指引，不拉进程）。
    const QString sCfgErr = CodexConfigError();
    if (!sCfgErr.isEmpty()) {
        EmitFailed(sCfgErr);
        return;
    }

    // 收尾上一进程（复用对象的连续阶段；正常路径进程已退出）。
    if (m_pProc->state() != QProcess::NotRunning) {
        m_bFinished = true;   // 吞掉旧进程的事件（防御）
        if (!m_pProc->waitForFinished(3000)) {
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
        }
        m_bFinished = false;
    }

    // 自定义路由激活：启动前幂等自愈写 ~/.codex（口径同 Pi 的 WritePiModelsJson；
    // codex 每次进程启动都重读 config.toml/auth.json，槽位必须在场）。
    if (CodexActiveEntry() != nullptr) {
        WriteCodexUserConfig();
    }

    // —— 组装命令行（node 直起；prompt 走 stdin 的 "-" 模式）——
    QStringList aArgs;
    aArgs << ResolveCodexJsPath()
          << QStringLiteral("exec")
          << QStringLiteral("--json")
          << QStringLiteral("--skip-git-repo-check")
          << QStringLiteral("-C") << QDir::toNativeSeparators(a_sWorkDir)
          << QStringLiteral("-s") << QStringLiteral("danger-full-access")
          // 覆盖级配置：provider 槽位 + 模型（生效路由 = 激活自定义条目或固定键；
          // key 不进命令行）。2026.08.21 自定义路由激活时先自愈重写 ~/.codex
          // （config.toml 槽位 + auth.json），防多实例竞态写空后 provider 失配。
          << QStringLiteral("-c") << QStringLiteral("model_provider=%1").arg(CodexEffProvider())
          << QStringLiteral("-c") << QStringLiteral("model=%1").arg(CodexEffModel());
    // 2026.08.21 本地代理注入 max_tokens：codex chat 请求体不带 max_tokens，
    //   网关默认输出 4096 截断（长输出轮必挂且被 codex 误报为上下文耗尽）。
    //   仅 http + chat 生效（代理为明文管道；https/responses 保持原路由）。
    //   base_url 以命令行覆盖级注入，~/.codex/config.toml 仍存真实网关地址
    //   （代理随进程起停，不落盘）。
    const QUrl urlBase(CodexEffBaseUrl());
    if (urlBase.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
        && CodexEffWireApi() == QLatin1String("chat")) {
        if (m_pProxy == nullptr) {
            m_pProxy = new CCodexProxy(this);
            // 2026.08.21 真实计量：代理从网关响应解析 usage（含 stream_options
            //   include_usage 补发），实时累计 token/请求数并上抛（任务统计列）。
            connect(m_pProxy, &CCodexProxy::sigUsage, this,
                    [this](qint64 a_iIn, qint64 a_iOut) {
                        m_bUsageLive = true;
                        m_iSumInTok += a_iIn;
                        m_iSumOutTok += a_iOut;
                        if (a_iIn > 0 && a_iIn != m_iLastContextFill) {
                            m_iLastContextFill = a_iIn;
                            emit sigContextFill(a_iIn);
                        }
                        emit sigUsageDelta(a_iIn, a_iOut);
                    });
        }
        if (m_pProxy->Start(urlBase, CodexEffMaxTokens())) {
            aArgs << "-c" << QStringLiteral("model_providers.%1.base_url=%2")
                         .arg(CodexEffProvider(), m_pProxy->ProxyBaseUrl());
            emit sigInfo(QStringLiteral("[system/codex] 本地输出上限代理：127.0.0.1:%1 → %2"
                                        "（注入 max_tokens=%3，防网关默认 4096 截断）")
                             .arg(QString::number(m_pProxy->Port()), urlBase.toString(),
                                  QString::number(CodexEffMaxTokens())));
        } else {
            emit sigInfo(QStringLiteral("[system/codex] ⚠ 本地代理监听失败，走直连路由"
                                        "（长输出可能被网关按默认上限截断）"));
        }
    } else if (m_pProxy != nullptr) {
        m_pProxy->Shutdown();   // https/responses 路由：不走代理
    }
    aArgs << QStringLiteral("-");
    m_pProc->setWorkingDirectory(a_sWorkDir);
    // 2026.08.22 中文编码根治（一）：给 codex 子进程注入 UTF-8 环境变量——
    //   Windows 默认 GBK（ACP=936），codex 派生的 powershell/python 子进程把中文
    //   命令/输出按 GBK 解释，出现乱码「?」甚至管道挂起（10 分钟停滞的根因）。
    //   PYTHONUTF8/PYTHONIOENCODING 让 python 全链路 UTF-8；LANG/LC_ALL 影响部分
    //   CLI 工具的输出编码。PowerShell 的输出编码由启动时的环境与 profile 决定，
    //   见 README「彻底解决中文编码」一节（profile + 系统 UTF-8 代码页）。
    QProcessEnvironment oEnv = QProcessEnvironment::systemEnvironment();
    // 2026-09-12 手工审批弹窗通道（AUTOCODE_ASK_EXE/ASK_PIPE，全后端统一）
    InjectAskEnv(oEnv);
    oEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    oEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    oEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    oEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    m_pProc->setProcessEnvironment(oEnv);
    // 2026.08.24 全局规则运行时注入（便携口径，不写 ~/.codex）：codex 只认
    //   AGENTS.md（二进制实测无 CLAUDE.md 候选），为免向用户主目录复制文件，改为把
    //   共享全局规则（<exe>\CLAUDE.md 或 ~/.claude/CLAUDE.md）直接前置进本阶段 prompt。
    QString sPrompt = a_sPrompt;
    // 2026.09.14 技能清单前置（codex 无原生技能机制，语义同全局规则注入）：
    //   列名称/描述/SKILL.md 路径，命中时模型自行读取——与 claude/pi/dsh 的技能发现对齐。
    const QString sSkillCatalog = CAgentRunnerBase::SharedSkillsCatalog();
    if (!sSkillCatalog.isEmpty()) {
        sPrompt.prepend(sSkillCatalog + QStringLiteral("\n\n---\n\n"));
        emit sigInfo(QStringLiteral("[system/codex] 已注入技能清单（%1 字符）")
                         .arg(QString::number(sSkillCatalog.size())));
    }
    const QString sGlobalCtx = CAgentRunnerBase::SharedGlobalContextContent();
    if (!sGlobalCtx.isEmpty()) {
        sPrompt.prepend(QStringLiteral(
            "以下是全局工程约定（CLAUDE.md），在整个任务中始终遵守：\n\n%1\n\n---\n\n")
            .arg(sGlobalCtx));
        emit sigInfo(QStringLiteral("[system/codex] 已注入全局规则（%1 字符）")
                         .arg(QString::number(sGlobalCtx.size())));
    }
    // 工程 CLAUDE.md 提示：codex 只发现工程根 AGENTS.md（不读 CLAUDE.md）。工程只有
    //   CLAUDE.md 时在 prompt 前加一行指引，让模型先读它（零配置、低开销）。
    if (QFileInfo::exists(a_sWorkDir + QStringLiteral("/CLAUDE.md"))
        && !QFileInfo::exists(a_sWorkDir + QStringLiteral("/AGENTS.md"))) {
        sPrompt.prepend(QStringLiteral(
            "注意：工程根目录存在 CLAUDE.md（本工程无 AGENTS.md）。开工前先用文件读取"
            "工具完整阅读 CLAUDE.md 并在整个任务中遵守其中的约定。\n\n"));
        emit sigInfo(QStringLiteral(
            "[system/codex] 工程根存在 CLAUDE.md（无 AGENTS.md），已指示先阅读"));
    }
    emit sigInfo(QStringLiteral("[system/codex] 启动：%1 %2").arg(
        QDir::toNativeSeparators(ResolveCodexJsPath()),
        QDir::toNativeSeparators(a_sWorkDir)));
    m_pProc->start(ResolveNodePath(), aArgs);
    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("无法启动 CodexHarness 进程：%1 %2\n%3").arg(
            ResolveNodePath(), QDir::toNativeSeparators(ResolveCodexJsPath()),
            CodexConfigError()));
        return;
    }
    m_actClock.restart();
    if (m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    // prompt 经 stdin 投递后关闭写通道（"-" = 从 stdin 读 prompt，--help 实测确认）。
    m_pProc->write(sPrompt.toUtf8());
    m_pProc->closeWriteChannel();
}

void CCodexRunner::Stop() {
    // 静默收尾（同 claude/DSH/Pi 语义）：先置终态再 kill，不发 sigFinished。
    m_bFinished = true;
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
    if (m_pProxy != nullptr) {
        m_pProxy->Shutdown();   // 本地代理随进程收尾（下轮 StartStage 重新起）
    }
}

bool CCodexRunner::IsRunning() const {
    return m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning && !m_bFinished;
}

bool CCodexRunner::IsRetryPending() const {
    return false;   // 一次性进程不做快速失败重试（失败直接报上层）
}

bool CCodexRunner::InjectUserTurn(const QString& a_sText) {
    Q_UNUSED(a_sText);
    return false;   // 一次性进程无轮次中插话（安全 no-op）
}

void CCodexRunner::slotOnStdOut() {
    m_actClock.restart();
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

void CCodexRunner::slotOnStdErr() {
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

void CCodexRunner::HandleLine(const QByteArray& a_sLine) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        emit sigInfo(QString::fromUtf8(a_sLine));   // 非 JSON 行原样上抛（排查用）
        return;
    }
    const QJsonObject oRoot = stDoc.object();
    const QString sType = oRoot.value(QStringLiteral("type")).toString();

    if (sType == QStringLiteral("thread.started")) {
        const QString sTid = oRoot.value(QStringLiteral("thread_id")).toString();
        m_sLastSessionId = sTid;
        m_bSawThread = true;
        emit sigInfo(QStringLiteral("[system/codex] Codex 会话 %1…").arg(
            sTid.left(8)));   // 仅展示，不持久化续接（Q4：不做 resume）
        return;
    }
    if (sType == QStringLiteral("turn.started")) {
        return;   // 无展示意义
    }
    if (sType == QStringLiteral("item.completed")) {
        HandleItemCompleted(oRoot.value(QStringLiteral("item")).toObject());
        return;
    }
    if (sType == QStringLiteral("turn.completed")) {
        ApplyUsage(oRoot.value(QStringLiteral("usage")).toObject());
        return;
    }
    if (sType == QStringLiteral("error")) {
        // 记录文本，随 sigFinished 失败上抛（进程随后多以非零码退出）。
        m_sErrorEvent = oRoot.value(QStringLiteral("message")).toString();
        if (m_sErrorEvent.isEmpty()) {
            m_sErrorEvent = QString::fromUtf8(
                QJsonDocument(oRoot).toJson(QJsonDocument::Compact));
        }
        emit sigInfo(QStringLiteral("[system/codex] error：") + m_sErrorEvent);
        return;
    }
    // turn.failed / thread 等其余事件：未识别不丢，sigInfo 容错（NFR：schema 漂移防御）。
    emit sigInfo(QStringLiteral("[system/codex] 事件：") + sType);
}

void CCodexRunner::HandleItemCompleted(const QJsonObject& oItem) {
    const QString sKind = oItem.value(QStringLiteral("type")).toString();
    if (sKind == QStringLiteral("agent_message")) {
        // 逐条流式上抛 sigAssistantText，同时记入 m_sTurnText（多条取最后一条为
        //   最终答复，随 sigFinished 上抛）。
        const QString sText = oItem.value(QStringLiteral("text")).toString();
        if (!sText.isEmpty()) {
            if (!m_bUsageLive) {
                ++m_iLastReqCount;   // 一条 agent_message ≈ 一轮 API 调用（无代理时的兜底口径）
            }
            m_sTurnText = sText;
            m_bSawAgentMsg = true;
            emit sigAssistantText(sText);
        }
        return;
    }
    if (sKind == QStringLiteral("command_execution")) {
        emit sigToolUse(QStringLiteral("command"),
                        oItem.value(QStringLiteral("command")).toString().left(200));
        // exit_code 有值时补一条结果（无输出文本类字段）。
        if (oItem.contains(QStringLiteral("exit_code"))) {
            emit sigToolResult(QStringLiteral("exit_code=%1").arg(
                oItem.value(QStringLiteral("exit_code")).toVariant().toInt()));
        }
        return;
    }
    if (sKind == QStringLiteral("file_change")) {
        const QJsonArray aChanges = oItem.value(QStringLiteral("changes")).toArray();
        emit sigToolUse(QStringLiteral("file_change"),
                        QString::fromUtf8(QJsonDocument(aChanges)
                                              .toJson(QJsonDocument::Compact)).left(200));
        return;
    }
    if (sKind == QStringLiteral("mcp_tool_call")) {
        emit sigToolUse(oItem.value(QStringLiteral("server")).toString()
                            + QLatin1Char('/') + oItem.value(QStringLiteral("tool")).toString(),
                        oItem.value(QStringLiteral("arguments")).toString().left(200));
        return;
    }
    if (sKind == QStringLiteral("web_search")) {
        emit sigToolUse(QStringLiteral("web_search"),
                        oItem.value(QStringLiteral("query")).toString());
        return;
    }
    if (sKind == QStringLiteral("reasoning")) {
        return;   // 内部推理摘要不上抛（同各后端口径）
    }
    // 未识别 type：sigInfo 容错上抛原始 type（schema 漂移不静默丢事件）。
    emit sigInfo(QStringLiteral("[system/codex] item.completed(type=%1)").arg(sKind));
}

void CCodexRunner::ApplyUsage(const QJsonObject& oUsage) {
    if (oUsage.isEmpty()) {
        return;
    }
    // 2026.08.21 代理在流（真实网关 usage）：事件口径为 0 且会与代理累计双计，跳过。
    if (m_bUsageLive) {
        return;
    }
    const qint64 iIn = oUsage.value(QStringLiteral("input_tokens")).toInteger()
                     + oUsage.value(QStringLiteral("cached_input")).toInteger()
                     + oUsage.value(QStringLiteral("cached_input_tokens")).toInteger();
    const qint64 iOut = oUsage.value(QStringLiteral("output_tokens")).toInteger();
    m_iSumInTok += iIn;
    m_iSumOutTok += iOut;
    // 上下文占用（脏值 >2×窗口丢弃，同 Pi/claude 口径；模型名 codex 侧事件不带，
    //   用 provider/model 组合兜底供上层查窗口）。
    if (m_sLastModel.isEmpty()) {
        m_sLastModel = CodexProvider() + QLatin1Char('/') + CodexModel();
    }
    const qint64 iWinCap = 2 * CClaudeRunner::ContextWindowForModel(m_sLastModel);
    if (iIn > 0 && iIn != m_iLastContextFill && iIn <= iWinCap) {
        m_iLastContextFill = iIn;
        emit sigContextFill(iIn);
    }
}

void CCodexRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    if (m_pProxy != nullptr) {
        m_pProxy->Shutdown();   // 进程退出：本地代理随阶段收尾（下轮重起）
    }
    Q_UNUSED(a_eStatus);
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_bFinished) {
        return;   // Stop/stall/已收尾：静默吞掉
    }
    if (a_iExitCode == 0 && m_bSawAgentMsg) {
        m_bFinished = true;
        m_sLastSubtype = QStringLiteral("success");
        emit sigFinished(true,
                         m_sTurnText.isEmpty() ? QStringLiteral("(本轮无文本输出)")
                                               : m_sTurnText,
                         (m_bUsageLive ? 0 : m_iSumInTok), (m_bUsageLive ? 0 : m_iSumOutTok), 0.0, m_sLastSessionId);
        return;
    }
    if (a_iExitCode == 0 && !m_bSawAgentMsg) {
        // 退出码 0 但无 agent_message（流被截断/事件缺失）：error 事件非空则按失败
        //   上抛，否则以空文本占位成功收尾。
        m_bFinished = true;
        if (!m_sErrorEvent.isEmpty()) {
            m_sLastSubtype = QStringLiteral("error");
            EmitFailed(QStringLiteral("CodexHarness 无 agent_message 输出；error：%1")
                           .arg(m_sErrorEvent));
            return;
        }
        m_sLastSubtype = QStringLiteral("success");
        emit sigFinished(true, QStringLiteral("(本轮无文本输出)"),
                         (m_bUsageLive ? 0 : m_iSumInTok), (m_bUsageLive ? 0 : m_iSumOutTok), 0.0, m_sLastSessionId);
        return;
    }
    // 非零退出：统一错误路径（error 事件 + stderr 尾部 + 指引，NFR-3）。
    QString sMsg = QStringLiteral("CodexHarness 进程异常退出，退出码=%1").arg(a_iExitCode);
    if (!m_sErrorEvent.isEmpty()) {
        sMsg += QStringLiteral("；error：%1").arg(m_sErrorEvent);
    }
    if (!m_aStderrTail.isEmpty()) {
        QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
        if (sTail.size() > 800) {
            sTail = sTail.left(800) + QStringLiteral("…");
        }
        sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
    }
    // 2026.08.21 上下文耗尽专项指引：chat wire_api 无自动压缩（compact 需 responses
    //   API），长阶段单线程跑满窗口必报此错——通用配置指引对此无效，给出对症动作。
    const QString sErrAll = m_sErrorEvent + QStringLiteral(" ")
        + m_aStderrTail.join(QStringLiteral(" "));
    if (sErrAll.contains(QStringLiteral("ran out of room in the model's context window"),
                         Qt::CaseInsensitive)) {
        sMsg += QStringLiteral(
            "\n处理：chat 模式下该报错多为**单轮输出超限**——网关默认输出上限 4096 token，"
            "codex 把截断（finish_reason=length）误报为上下文耗尽。可："
            "① 设置 → CodexHarness 自定义模型表，调大该条目「最大输出」列"
            "（AutoCode 已自动经本地代理注入 max_tokens，重启后生效）；"
            "② 若确为输入超窗：核对条目「上下文」列与模型真实窗口一致后拆小阶段重跑。");
    } else {
        sMsg += QStringLiteral("\n处理：检查 CodexHarness 根目录/模型/BaseUrl 与 API key 配置"
                               "（设置 → CodexHarness 参数，保存时写入 ~/.codex）；"
                               "或重跑「安装 CodexHarness…」。");
    }
    EmitFailed(sMsg);
}

void CCodexRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    if (m_bFinished) {
        return;
    }
    QString sMsg;
    switch (a_eError) {
        case QProcess::FailedToStart: sMsg = QStringLiteral("CodexHarness 启动失败（node 路径无效或无执行权限）"); break;
        case QProcess::Crashed:       sMsg = QStringLiteral("CodexHarness 进程崩溃"); break;
        case QProcess::Timedout:      sMsg = QStringLiteral("CodexHarness 进程超时"); break;
        case QProcess::ReadError:     sMsg = QStringLiteral("读取 CodexHarness 输出错误"); break;
        case QProcess::WriteError:    sMsg = QStringLiteral("写入 CodexHarness 输入错误"); break;
        default:                      sMsg = QStringLiteral("CodexHarness 进程未知错误"); break;
    }
    if (a_eError == QProcess::FailedToStart) {
        EmitFailed(sMsg + QStringLiteral("\n") + CodexConfigError());
    } else if (!m_bFinished) {
        emit sigInfo(QStringLiteral("[system/codex] 进程错误：%1（等待退出收尾…）").arg(sMsg));
    }
}

void CCodexRunner::CheckStall() {
    // 停滞检测（同 Pi 语义）：进程活着但距上次收到任何输出超 [Agent] StallMinutes
    //   → 判挂起，kill + subtype="stall" 报失败（codex 无跨进程 resume，上层人工重试）。
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
                                "终止 CodexHarness 进程…").arg(CClaudeRunner::StallMinutes()));
    m_pProc->kill();
    m_pProc->waitForFinished(3000);
    emit sigFinished(false,
                     QStringLiteral("停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），进程已终止")
                         .arg(CClaudeRunner::StallMinutes()),
                     (m_bUsageLive ? 0 : m_iSumInTok), (m_bUsageLive ? 0 : m_iSumOutTok), 0.0, m_sLastSessionId);
}

void CCodexRunner::EmitFailed(const QString& a_sMsg) {
    if (m_bFinished) {
        return;
    }
    m_bFinished = true;
    if (!m_bSawThread) {
        m_sLastSessionId.clear();   // 早期失败不带旧 tid（口径同 Pi m_bSawSession 修复）
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    emit sigInfo(QStringLiteral("[错误] ") + a_sMsg);
    emit sigFinished(false, a_sMsg, (m_bUsageLive ? 0 : m_iSumInTok), (m_bUsageLive ? 0 : m_iSumOutTok), 0.0, m_sLastSessionId);
}
