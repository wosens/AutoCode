#include "CHermesRunner.h"

#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QCoreApplication>

// node 直起用（进程模型同 pi/codex：node <cli.js>）。
#include "CClaudeRunner.h"   // StallMinutes：巡检阈值复用同一实现

// 钳位常量单点收口见 CHermesRunner.h 的 ClampMaxTok/ClampCtx
//   （131072 = 网关 2^17 max_tokens 上限，超限引发全请求失败，pi 先例；
//    200000 = 当前工程实证安全 contextWindow；真实窗口待澄清 6 后只改 header 一处）。

// —— 进程级静态配置（纯内存；CMainWindow 启动/设置保存路径回填）——
static QString s_sHermesRoot;
static QString s_sHermesNodePath;
// 2026-10-08 思考强度（agent.reasoning_effort；空=不托管）——类静态成员
//   （CHermesRunner.h 声明），此处定义（旧文件级 static 与成员重名致链接缺定义）。
QString CHermesRunner::s_sHermesEffort;
static QString s_sHermesModel;
static QString s_sHermesApiKey;
static QString s_sHermesBaseUrl;
// 激活模型条目的 API 协议（anthropic-messages / openai-completions，2026.09.21）：
//   空/未知 = OpenAI wire（历史行为）；anthropic* 时 config.yaml 补写 api_mode。
static QString s_sHermesApi;
// 2026-10-07 对话级模型覆盖的并发守卫记录：最近一次 Hermes 启动写入共享配置的
//   模型身份 + 时刻（UI 线程串行维护；紧邻的不同身份启动延迟重入，见 StartStage）。
static qint64 s_llHermesCfgMs = 0;
static QString s_sHermesCfgIdent;
// 计费单价（$/百万 token，口径同 DSH/Pi）：Hermes 只报 token 分量，客户端折算。
static double s_dHermesPriceInPerM        = 3.0;
static double s_dHermesPriceCacheReadPerM = 0.3;
static double s_dHermesPriceCacheWritePerM = 3.75;
static double s_dHermesPriceOutPerM       = 15.0;
// 激活模型条目回填的窗口/输出上限（写 ~/.hermes 前钳位）。
static qint64 s_iHermesCtx = 200000;
static int    s_iHermesMaxTok = 32768;

void CHermesRunner::SetHermesRoot(const QString& a_sRoot) { s_sHermesRoot = a_sRoot.trimmed(); }
QString CHermesRunner::HermesRoot() { return s_sHermesRoot; }
void CHermesRunner::SetHermesEffort(const QString& a_sLevel) {
    s_sHermesEffort = a_sLevel.trimmed().toLower();
}


QString CHermesRunner::HermesGitRepoDir() {
    // 2026.09.20 版本管理 Hermes 最新版比对：定位部署根下真正含 .git 的仓目录
    //   （探测序同 ResolveHermesVersion 的形态判定）。
    const QString sCli = ResolveHermesCli();
    if (sCli.isEmpty()) {
        return QString();
    }
    QStringList aCandidates;
    if (sCli.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        const QDir dirCli = QFileInfo(sCli).dir();                    // .../venv/Scripts
        aCandidates << dirCli.filePath(QStringLiteral("../.."))       // venv 上级（平铺）
                    << dirCli.filePath(QStringLiteral("../../../..")); // root/hermes-agent
    } else {
        aCandidates << QFileInfo(sCli).dir().filePath(QStringLiteral(".."));  // dist 上级=根
    }
    for (const QString& sDir : aCandidates) {
        if (QFileInfo::exists(QDir(sDir).filePath(QStringLiteral(".git")))) {
            return QDir::cleanPath(sDir);
        }
    }
    return QString();
}
void CHermesRunner::SetHermesNodePath(const QString& a_sPath) { s_sHermesNodePath = a_sPath.trimmed(); }
QString CHermesRunner::HermesNodePath() { return s_sHermesNodePath; }
void CHermesRunner::SetHermesModel(const QString& a_sModel) { s_sHermesModel = a_sModel.trimmed(); }
QString CHermesRunner::HermesModel() { return s_sHermesModel; }
void CHermesRunner::SetHermesApiKey(const QString& a_sKey) { s_sHermesApiKey = a_sKey.trimmed(); }
QString CHermesRunner::HermesApiKey() { return s_sHermesApiKey; }
void CHermesRunner::SetHermesBaseUrl(const QString& a_sUrl) { s_sHermesBaseUrl = a_sUrl.trimmed(); }
QString CHermesRunner::HermesBaseUrl() { return s_sHermesBaseUrl; }

// 2026.09.21 协议回填：CMainWindow 启动/设置保存两路径传激活条目 sApi。
void CHermesRunner::SetHermesApi(const QString& a_sApi) { s_sHermesApi = a_sApi.trimmed(); }
QString CHermesRunner::HermesApi() { return s_sHermesApi; }

// anthropic 协议端点判定（兼容 anthropic-messages / anthropic / anthropic_messages）。
static bool HermesApiIsAnthropic(const QString& a_sApi) {
    return a_sApi.compare(QStringLiteral("anthropic-messages"), Qt::CaseInsensitive) == 0
        || a_sApi.compare(QStringLiteral("anthropic_messages"), Qt::CaseInsensitive) == 0
        || a_sApi.compare(QStringLiteral("anthropic"), Qt::CaseInsensitive) == 0;
}

void CHermesRunner::SetHermesPriceInPerM(double a_d) { s_dHermesPriceInPerM = qMax(0.0, a_d); }
void CHermesRunner::SetHermesPriceCacheReadPerM(double a_d) { s_dHermesPriceCacheReadPerM = qMax(0.0, a_d); }
void CHermesRunner::SetHermesPriceCacheWritePerM(double a_d) { s_dHermesPriceCacheWritePerM = qMax(0.0, a_d); }
void CHermesRunner::SetHermesPriceOutPerM(double a_d) { s_dHermesPriceOutPerM = qMax(0.0, a_d); }

void CHermesRunner::SetHermesRuntimeLimits(qint64 a_iCtx, int a_iMaxTok) {
    s_iHermesCtx = ClampCtx(a_iCtx > 0 ? a_iCtx : 200000);
    s_iHermesMaxTok = ClampMaxTok(a_iMaxTok > 0 ? a_iMaxTok : 32768);
}

// —— ~/.hermes 运行时配置（FR6/FR9）——
// 测试注入优先级：显式参数 > SetHermesHomeOverride 覆盖（离线单测不碰真实 ~/.hermes）>
//   QDir::homePath()/.hermes（生产默认）。
static QString s_sHermesHomeOverride;

void CHermesRunner::SetHermesHomeOverride(const QString& a_sDir) {
    s_sHermesHomeOverride = a_sDir.trimmed();
}

static QString HermesHomeDir(const QString& a_sHermesHome) {
    if (!a_sHermesHome.isEmpty()) {
        return a_sHermesHome;
    }
    if (!s_sHermesHomeOverride.isEmpty()) {
        return s_sHermesHomeOverride;
    }
    return QDir::homePath() + QStringLiteral("/.hermes");
}

QJsonObject CHermesRunner::BuildRuntimeConfigJson(const SDshModelEntry* a_pOv) {
    // 写入内容是 config 静态键的确定性纯函数（多实例后写者不产生损坏内容的前提）。
    // 2026-10-07 对话级模型覆盖：a_pOv 非空时 model/baseUrl/apiKey 取覆盖条目
    //   （窗口/输出上限仍走全局钳位静态——覆盖条目与共享表同源，值本就一致）。
    QJsonObject oCfg;
    oCfg.insert(QStringLiteral("model"),
                (a_pOv != nullptr && !a_pOv->sModelId.isEmpty()) ? a_pOv->sModelId : s_sHermesModel);
    oCfg.insert(QStringLiteral("baseUrl"),
                (a_pOv != nullptr && !a_pOv->sBaseURL.isEmpty()) ? a_pOv->sBaseURL : s_sHermesBaseUrl);
    oCfg.insert(QStringLiteral("apiKey"),
                (a_pOv != nullptr && !a_pOv->sApiKey.isEmpty()) ? a_pOv->sApiKey : s_sHermesApiKey);
    oCfg.insert(QStringLiteral("contextWindow"), static_cast<double>(s_iHermesCtx));
    // maxTokens 钳位（AC3 口径=钳位 131072，与设置页保存双侧收口）。
    oCfg.insert(QStringLiteral("maxTokens"), static_cast<double>(s_iHermesMaxTok));
    return oCfg;
}

bool CHermesRunner::EnsureRuntimeConfig(const QString& a_sHermesHome,
                                        const SDshModelEntry* a_pOv) {
    // 幂等自愈写 ~/.hermes/agent/config.json：
    //   1) 读旧值——内容与目标一致则短路返回（AC6 幂等；不重写 mtime）；
    //   2) 不一致——写同目录临时文件 + QFile::rename 原子替换。多实例首写竞态下
    //      写入内容是 config 键的确定性纯函数，rename 失败/被抢先时兜底删除临时
    //      文件（最终内容仍一致，NFR4）。
    const QString sHome = HermesHomeDir(a_sHermesHome);
    const QString sAgentDir = sHome + QStringLiteral("/agent");
    const QString sCfgPath = sAgentDir + QStringLiteral("/config.json");

    const QJsonDocument oDoc(BuildRuntimeConfigJson(a_pOv));
    const QByteArray aTarget = oDoc.toJson(QJsonDocument::Indented);

    CFrwDevice fOld(sCfgPath);
    if (fOld.open(QIODevice::ReadOnly)) {
        if (fOld.readAll() == aTarget) {
            return true;   // 读-比-短路：内容一致，无需写
        }
        fOld.close();
    }
    if (!QDir().mkpath(sAgentDir)) {
        return false;
    }
    const QString sTmp = sCfgPath + QStringLiteral(".tmp");
    {
        QFile fTmp(sTmp);
        if (!fTmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        if (fTmp.write(aTarget) != aTarget.size()) {
            fTmp.close();
            QFile::remove(sTmp);
            return false;
        }
        fTmp.close();
    }
    // rename 原子替换；目标已被并发实例抢先替换时 rename 失败——兜底删临时文件，
    //   对方写入内容与本方目标一致（同一纯函数），最终状态仍正确。
    if (!QFile::remove(sCfgPath) && QFile::exists(sCfgPath)) {
        QFile::remove(sTmp);
        return false;
    }
    if (!QFile::rename(sTmp, sCfgPath)) {
        QFile::remove(sTmp);
        return false;
    }
    return true;
}

QString CHermesRunner::ResolveHermesHomeForExe(const QString& a_sCli) {
    // venv 内 exe 定位不到部署根时 hermes 自身有默认 home 逻辑
    //   （%LOCALAPPDATA%\hermes），这里只负责"config.yaml 所在候选根"。
    if (!a_sCli.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return QString();
    }
    const QDir dirExe = QFileInfo(a_sCli).dir();                    // .../venv/Scripts
    const QStringList aRootCands = {
        dirExe.filePath(QStringLiteral("../../../..")),   // root/hermes-agent/venv/Scripts
        dirExe.filePath(QStringLiteral("../../..")),      // root/venv/Scripts
    };
    for (const QString& sR : aRootCands) {
        if (QFile::exists(QDir(sR).filePath(QStringLiteral("config.yaml")))) {
            return QDir(sR).absolutePath();
        }
    }
    return QString();
}

// —— config.yaml model 段文本级注入辅助（YAML 子集：顶级段 + 缩进标量）——
namespace {
// model 段内我们接管的四个键（default 是 hermes 的模型键；model 别名键不接管，
//   hermes 侧 default 优先，两键并存时我们的 default 生效）。
const char* const kHermesModelKeys[] = { "default", "provider", "base_url", "api_key",
                                         "api_mode" };

// 顶级键行（无缩进 ident:...）判定；注释/空行/列表项不算。
bool IsTopLevelKeyLine(const QString& a_sLine) {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_-]*\\s*:"));
    return re.match(a_sLine).hasMatch();
}

// 缩进键行匹配（注释行 # 开头不命中——正则要求空白后紧跟键名）。
QRegularExpressionMatch MatchModelKey(const QString& a_sLine) {
    static const QRegularExpression re(
        QStringLiteral("^(\\s*)(default|provider|base_url|api_key|api_mode)\\s*:\\s*(.*)$"));
    return re.match(a_sLine);
}

// YAML 标量解析：去成对引号/截行内注释/trim（含 CR）。
QString YamlScalar(QString a_sVal) {
    QString sV = a_sVal.trimmed();
    if (sV.startsWith(QLatin1Char('"'))) {
        const int iEnd = sV.indexOf(QLatin1Char('"'), 1);
        return iEnd > 0 ? sV.mid(1, iEnd - 1) : sV.mid(1);
    }
    if (sV.startsWith(QLatin1Char('\''))) {
        const int iEnd = sV.indexOf(QLatin1Char('\''), 1);
        return iEnd > 0 ? sV.mid(1, iEnd - 1) : sV.mid(1);
    }
    const int iHash = sV.indexOf(QStringLiteral(" #"));
    if (iHash >= 0) {
        sV.truncate(iHash);
    }
    return sV.trimmed();
}

// agent: 段 reasoning_effort 读取/写管理（2026-10-08 思考强度）。
//   读 = agent: 段内首个 reasoning_effort: 行的标量（段缺/键缺 = 空）。
QString ReadHermesAgentEffort(const QStringList& a_aLines) {
    bool bInAgent = false;
    for (const QString& sLine : a_aLines) {
        if (IsTopLevelKeyLine(sLine)) {
            bInAgent = sLine.startsWith(QStringLiteral("agent"));
            continue;
        }
        if (!bInAgent) {
            continue;
        }
        const QString sT = sLine.trimmed();
        if (sT.startsWith(QStringLiteral("reasoning_effort"))) {
            return sT.mid(17).split(QLatin1Char('#')).first().trimmed();
        }
    }
    return QString();
}

// 写：agent: 段内 reasoning_effort 键 upsert（a_sEffort 空 = 删键回 hermes 默认；
//   false/minimal..ultra 原词写入）。段缺 = 文末追加 agent: + 缩进键；段在 = 沿用
//   段内首个保留行缩进插入/替换，多余同名键删除（同 model 段防后位覆盖口径）。
QString BuildHermesAgentEffortText(const QStringList& a_aLines, const QString& a_sEffort) {
    int iAgent = -1;
    for (int i = 0; i < a_aLines.size(); ++i) {
        if (a_aLines.at(i).startsWith(QStringLiteral("agent"))
            && IsTopLevelKeyLine(a_aLines.at(i))) {
            iAgent = i;
            break;
        }
    }
    int iSegEnd = a_aLines.size();
    QString sIndent = QStringLiteral("  ");
    if (iAgent >= 0) {
        for (int i = iAgent + 1; i < a_aLines.size(); ++i) {
            if (IsTopLevelKeyLine(a_aLines.at(i))) {
                iSegEnd = i;
                break;
            }
        }
        for (int i = iAgent + 1; i < iSegEnd; ++i) {
            const QString sT = a_aLines.at(i).trimmed();
            if (!sT.isEmpty() && !sT.startsWith(QLatin1Char('#'))) {
                sIndent = a_aLines.at(i).left(a_aLines.at(i).size() - sT.size());
                break;
            }
        }
    }
    QStringList aOut;
    bool bDone = false;
    for (int i = 0; i < a_aLines.size(); ++i) {
        const bool bInSeg = (iAgent >= 0 && i > iAgent && i < iSegEnd);
        const QString sT = a_aLines.at(i).trimmed();
        if (bInSeg && sT.startsWith(QStringLiteral("reasoning_effort"))) {
            if (!bDone && !a_sEffort.isEmpty()) {
                aOut << sIndent + QStringLiteral("reasoning_effort: ") + a_sEffort;
                bDone = true;
            }
            continue;
        }
        aOut << a_aLines.at(i);
    }
    if (!bDone && !a_sEffort.isEmpty()) {
        if (iAgent < 0) {
            if (!aOut.isEmpty() && !aOut.last().trimmed().isEmpty()) {
                aOut << QString();
            }
            aOut << QStringLiteral("agent:");
        }
        aOut << sIndent + QStringLiteral("reasoning_effort: ") + a_sEffort;
    }
    return aOut.join(QLatin1Char('\n'));
}

// 提取 model: 段四键当前值（后位同名键覆盖——YAML 语义；段外/缺键 → 空值）。
QMap<QString, QString> ReadHermesModelKeys(const QStringList& a_aLines) {
    QMap<QString, QString> oVals;
    bool bInModel = false;
    for (const QString& sLine : a_aLines) {
        if (IsTopLevelKeyLine(sLine)) {
            bInModel = sLine.startsWith(QStringLiteral("model"));
            continue;
        }
        if (!bInModel) {
            continue;
        }
        const QRegularExpressionMatch oM = MatchModelKey(sLine);
        if (oM.hasMatch()) {
            oVals.insert(oM.captured(2), YamlScalar(oM.captured(3)));
        }
    }
    return oVals;
}

// 重建全文：model 段内删四键旧键行（非注释），紧跟 model: 行后插入目标四键
//（缩进取段内首个保留非注释行，缺省 2 空格）；无 model: 段则文末追加。
QString BuildHermesYamlText(const QStringList& a_aLines,
                            const QMap<QString, QString>& a_oTargets) {
    int iModel = -1;
    for (int i = 0; i < a_aLines.size(); ++i) {
        if (a_aLines.at(i).startsWith(QStringLiteral("model"))
            && IsTopLevelKeyLine(a_aLines.at(i))) {
            iModel = i;
            break;
        }
    }
    int iSegEnd = a_aLines.size();
    QString sIndent = QStringLiteral("  ");
    if (iModel >= 0) {
        for (int i = iModel + 1; i < a_aLines.size(); ++i) {
            if (IsTopLevelKeyLine(a_aLines.at(i))) {
                iSegEnd = i;
                break;
            }
        }
        for (int i = iModel + 1; i < iSegEnd; ++i) {
            const QString sT = a_aLines.at(i).trimmed();
            if (!sT.isEmpty() && !sT.startsWith(QLatin1Char('#'))) {
                sIndent = a_aLines.at(i).left(a_aLines.at(i).size() - sT.size());
                break;
            }
        }
    }
    QStringList aOut;
    bool bInserted = false;
    for (int i = 0; i < a_aLines.size(); ++i) {
        if (iModel >= 0 && i > iModel && i < iSegEnd) {
            const QString sT = a_aLines.at(i).trimmed();
            if (!sT.isEmpty() && !sT.startsWith(QLatin1Char('#'))
                && MatchModelKey(a_aLines.at(i)).hasMatch()) {
                continue;   // 删四键旧行（含后位覆盖键——2026.09 踩坑：只追加会被段尾旧键覆盖）
            }
        }
        aOut << a_aLines.at(i);
        if (iModel >= 0 && i == iModel && !bInserted) {
            bInserted = true;
            for (const char* sK : kHermesModelKeys) {
                const QString sKey = QString::fromLatin1(sK);
                aOut << sIndent + sKey + QStringLiteral(": \"")
                        + a_oTargets.value(sKey).replace(QLatin1Char('"'),
                                                         QStringLiteral("\\\""))
                        + QLatin1Char('"');
            }
        }
    }
    if (iModel < 0) {
        while (!aOut.isEmpty() && aOut.last().isEmpty()) {
            aOut.removeLast();
        }
        if (!aOut.isEmpty()) {
            aOut << QString();   // 与前文空一行
        }
        aOut << QStringLiteral("model:");
        for (const char* sK : kHermesModelKeys) {
            const QString sKey = QString::fromLatin1(sK);
            aOut << sIndent + sKey + QStringLiteral(": \"")
                    + a_oTargets.value(sKey).replace(QLatin1Char('"'),
                                                     QStringLiteral("\\\""))
                    + QLatin1Char('"');
        }
        aOut << QString();   // 末尾换行
    }
    return aOut.join(QLatin1Char('\n'));
}

// 2026.09.05 Hermes 轮次上限兜底文本本地化：hermes-agent 跑满 max_iterations
//   （默认 90；AutoCode 的 maxTurns 对 Hermes 无效）后自动再发一次「总结进度」
//   请求，该请求再失败（典型 Connection error——模型网关不可达）时把英文兜底串
//   当最终回复返回（chat_completion_helpers.py handle_max_iterations 的 except
//   分支）。用户看到裸英文无从排查 → 命中即替换为中文提示（保留轮数与原始
//   错误细节，并说明已完成工作已落盘、可重发继续）；未命中原样返回。
//   调用点三处：assistant 事件 / result 事件 / exe 形态（-z 纯文本）收尾。
QString LocalizeHermesCapText(const QString& a_sText) {
    const QString sTrim = a_sText.trimmed();
    static const QRegularExpression reCapSum(
        QStringLiteral("^I reached the maximum iterations \\((\\d+)\\) "
                       "but couldn't summarize\\. Error: (.+)"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch oM = reCapSum.match(sTrim);
    if (oM.hasMatch()) {
        return QStringLiteral(
                   "⚠ Hermes 已达轮次上限（%1 轮），收尾总结请求失败：%2。\n"
                   "此前已完成的工作均已保存；检查网络/模型网关后重发指令即可继续。")
            .arg(oM.captured(1), oM.captured(2).trimmed());
    }
    // 同链路姊妹分支：总结请求成功但模型返回空（connection 正常、内容为空）。
    if (sTrim.startsWith(QStringLiteral(
            "I reached the iteration limit and couldn't generate a summary."))) {
        return QStringLiteral(
            "⚠ Hermes 已达轮次上限，未能生成收尾总结（模型返回空）。\n"
            "此前已完成的工作均已保存；重发指令即可继续。");
    }
    // 2026.09.21 端点协议失配 404（他机 GLM-5.3 实录：模型表选中 anthropic-
    //   messages 端点 http://gw:9000/claude，旧版 Hermes runner 恒按 OpenAI wire
    //   发 <base>/chat/completions → 网关 404 Route not found）。命中即附中文
    //   排障指引（现版已自动写 api_mode，此提示留给旧端点/手填 URL 场景）。
    if (sTrim.contains(QStringLiteral("HTTP 404"))
        && sTrim.contains(QStringLiteral("not found"), Qt::CaseInsensitive)) {
        return QStringLiteral(
                   "✗ 模型端点返回 404（路由不存在）：%1\n"
                   "常见原因：接入点与 API 协议不匹配——anthropic-messages 端点"
                   "（如 .../claude）只收 Anthropic Messages 请求，OpenAI 协议端点"
                   "（如 .../api/v1）只收 chat/completions 请求。\n"
                   "处理：到设置页模型列表换用与当前后端协议一致的条目（或改接入点"
                   "路径）。升级 AutoCode 后 Hermes 后端会按条目协议自动适配。")
            .arg(sTrim);
    }
    return a_sText;
}
}   // namespace

bool CHermesRunner::EnsureHermesHomeConfig(const QString& a_sYamlPath,
                                           const SDshModelEntry* a_pOv) {
    // 静态配置三值不全（模型条目未回填）→ 不注入（更早的 HermesConfigError 拦截）。
    // 2026-10-07 对话级模型覆盖：a_pOv 非空时三值/协议取覆盖条目（与全局同表同源，
    //   覆盖只改本进程读取窗口的内容——StartStage 的延迟重入守卫保证读不串台）。
    const QString sM = (a_pOv != nullptr && !a_pOv->sModelId.isEmpty()) ? a_pOv->sModelId : s_sHermesModel;
    const QString sU = (a_pOv != nullptr && !a_pOv->sBaseURL.isEmpty()) ? a_pOv->sBaseURL : s_sHermesBaseUrl;
    const QString sK = (a_pOv != nullptr && !a_pOv->sApiKey.isEmpty()) ? a_pOv->sApiKey : s_sHermesApiKey;
    const QString sA = (a_pOv != nullptr && !a_pOv->sApi.isEmpty()) ? a_pOv->sApi : s_sHermesApi;
    if (sM.isEmpty() || sU.isEmpty() || sK.isEmpty()) {
        return false;
    }
    CFrwDevice fYaml(a_sYamlPath);
    QString sTxt;
    if (fYaml.open(QIODevice::ReadOnly)) {
        sTxt = QString::fromUtf8(fYaml.readAll());
        fYaml.close();
    }
    QMap<QString, QString> oTargets;
    oTargets.insert(QStringLiteral("default"), sM);
    oTargets.insert(QStringLiteral("provider"), QStringLiteral("custom"));
    oTargets.insert(QStringLiteral("base_url"), sU);
    oTargets.insert(QStringLiteral("api_key"), sK);
    // 2026.09.21 协议感知：anthropic 端点写 api_mode（hermes 主运行时
    //   _resolve_plain_custom_api_mode 读该键切 Anthropic 传输，请求
    //   <base>/v1/messages——与 Claude Code 同链路）；OpenAI 端点写显式
    //   chat_completions（网关 /claude 路由 404 实录后双保险，语义同缺省）。
    oTargets.insert(QStringLiteral("api_mode"),
                    HermesApiIsAnthropic(sA)
                        ? QStringLiteral("anthropic_messages")
                        : QStringLiteral("chat_completions"));
    // 读-比-短路：model 段四键已是目标值且思考强度键同步 → 不写（保 mtime）。
    //   2026-10-08 思考强度：agent.reasoning_effort 与 model 段同一次写管；
    //   空值 = 删键（回 hermes 默认），false = 显式禁用，minimal..ultra 原词。
    const QString sEffort = s_sHermesEffort;
    const QStringList aLines = sTxt.split(QLatin1Char('\n'));
    if (ReadHermesModelKeys(aLines) == oTargets
        && ReadHermesAgentEffort(aLines) == sEffort) {
        return true;
    }
    const QString sNew = BuildHermesAgentEffortText(
        BuildHermesYamlText(aLines, oTargets).split(QLatin1Char('\n')), sEffort);
    // 临时文件 + rename 原子替换（口径同 EnsureRuntimeConfig；写入内容是静态
    //   配置键的确定性纯函数，多实例首写竞态下 rename 失败时兜底删临时文件）。
    const QString sTmp = a_sYamlPath + QStringLiteral(".tmp");
    {
        QFile fTmp(sTmp);
        if (!fTmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        if (fTmp.write(sNew.toUtf8()) != sNew.toUtf8().size()) {
            fTmp.close();
            QFile::remove(sTmp);
            return false;
        }
        fTmp.close();
    }
    if (!QFile::remove(a_sYamlPath) && QFile::exists(a_sYamlPath)) {
        QFile::remove(sTmp);
        return false;
    }
    if (!QFile::rename(sTmp, a_sYamlPath)) {
        QFile::remove(sTmp);
        return false;
    }
    return true;
}

// 2026.09.03 v2.0.105 软编码版：补丁正本 = exe 同级 hermes_oneshot_patch.py
//   （改补丁无需重编 AutoCode），经 install_env.bat /hermes-patch 由部署根
//   venv python 幂等敷设；选择 Hermes 后端保存时 CMainWindow 触发一次，此处
//   StartStage 兜底（hermes 更新 reset 冲掉补丁后自动重敷）。本函数只查标记、
//   缺则同步调 bat（秒级、仅未敷设时到达），不再内嵌补丁文本（v2.0.104 已退役）。
//   oneshot.py 含标记即视为已敷设（幂等口径与补丁脚本一致）。
namespace {
// v2.0.109 PreviewSuffix 已随 v2.0.111 工具卡化退役（preview 直传 sigToolUse）。

bool OneshotHasEventPatch(const QString& a_sPyPath) {
    CFrwDevice fPy(a_sPyPath);
    if (!fPy.open(QIODevice::ReadOnly)) {
        return false;
    }
    // 2026.09.03 v2.0.108 检 v2 标记；v2.0.109 升 v3；v2.0.114 升 v4（v4 增
    //   中间段文本按 assistant 消息上抛）；v2.0.117 升 v5（-z @<file> 提示词
    //   展开——Windows 命令行 32767 上限）。只含旧版标记视为未敷设——
    //   EnsureHermesOneshotPatch 重调 bat，敷设器自动原地升级。
    const bool bHit = QString::fromUtf8(fPy.readAll())
                          .contains(QStringLiteral("AutoCode events patch v5"));
    fPy.close();
    return bHit;
}

// v2.0.105 方案 A：会话状态库 wal/-shm 的最后修改时间（取较新者；均无效返回
//   无效 QDateTime——未建库/已 checkpoint 删除时不误判心跳）。
QDateTime HermesWalMtime(const QString& a_sHome) {
    const QDateTime tWal = QFileInfo(a_sHome + QStringLiteral("/state.db-wal")).lastModified();
    const QDateTime tShm = QFileInfo(a_sHome + QStringLiteral("/state.db-shm")).lastModified();
    if (tWal.isValid() && tShm.isValid()) {
        return qMax(tWal, tShm);
    }
    return tWal.isValid() ? tWal : tShm;
}
}   // namespace

bool CHermesRunner::EnsureHermesOneshotPatch(const QString& a_sHome) {
    const QString sPy = a_sHome + QStringLiteral("/hermes-agent/hermes_cli/oneshot.py");
    if (OneshotHasEventPatch(sPy)) {
        return true;   // 已敷设（幂等）
    }
    // exe 同级 install_env.bat；测试 exe（Execute/Release/）补探一级父目录。
    const QString sAppDir = QCoreApplication::applicationDirPath();
    const QStringList aCands = {
        sAppDir + QStringLiteral("/install_env.bat"),
        QDir(sAppDir + QStringLiteral("/..")).absolutePath()
            + QStringLiteral("/install_env.bat"),
    };
    QString sBat;
    for (const QString& s : aCands) {
        if (QFile::exists(s)) {
            sBat = s;
            break;
        }
    }
    if (sBat.isEmpty()) {
        return false;
    }
    QProcess::execute(QStringLiteral("cmd"),
                      { QStringLiteral("/c"), sBat,
                        QStringLiteral("/hermes-patch"), a_sHome });
    return OneshotHasEventPatch(sPy);   // 敷设后复检
}

// —— CLI 解析（探测序：HermesRoot → HERMES_HOME → exe 同级 HermesHarness → PATH）——
// 两种安装形态并存：
//   A. 旧 Node Harness：<root>\dist\cli.js（node 直起，NDJSON 事件流）；
//   B. Python hermes-agent（2026.09 起本机实际形态）：<root>\hermes-agent\venv\Scripts\
//      hermes.exe 或 <root>\venv\Scripts\hermes.exe（exe 直起 -z 一次性模式）。
static QString FindHermesExeUnderRoot(const QString& a_sRoot) {
    if (a_sRoot.trimmed().isEmpty()) {
        return QString();
    }
    const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    const QStringList aCandidates = {
        sRoot + QStringLiteral("/hermes-agent/venv/Scripts/hermes.exe"),
        sRoot + QStringLiteral("/venv/Scripts/hermes.exe"),
    };
    for (const QString& s : aCandidates) {
        if (QFile::exists(s)) {
            return QFileInfo(s).absoluteFilePath();
        }
    }
    return QString();
}

QString CHermesRunner::ResolveHermesCli() {
    // 1) 显式根目录：形态 A <root>\dist\cli.js → 形态 B <root>\...\hermes.exe。
    if (!s_sHermesRoot.isEmpty()) {
        const QString sCli = s_sHermesRoot + QStringLiteral("/dist/cli.js");
        if (QFile::exists(sCli)) {
            return QFileInfo(sCli).absoluteFilePath();
        }
        const QString sExe = FindHermesExeUnderRoot(s_sHermesRoot);
        if (!sExe.isEmpty()) {
            return sExe;
        }
    }
    // 1.5) HERMES_HOME 环境变量（install_env.bat / 手动安装写入 HKCU）：
    //      形态 B 根目录的事实标准锚点，设置页未填根目录时的主探测来源。
    const QString sEnvRoot = qEnvironmentVariable("HERMES_HOME");
    if (!sEnvRoot.isEmpty()) {
        const QString sExe = FindHermesExeUnderRoot(sEnvRoot);
        if (!sExe.isEmpty()) {
            return sExe;
        }
    }
    // 2) exe 同级 HermesHarness（便携部署，install 布局同 Pi/Codex）：
    //    形态 A <dir>\dist\cli.js；形态 B <dir>\hermes-agent\venv\Scripts\hermes.exe
    //    或旧平铺 <dir>\venv\Scripts\hermes.exe（install_env.bat 2026.09.13 起
    //    Python 形态自动安装到同级并 setx HERMES_HOME，此处兜底使其无需配置即命中）。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QString sSiblingRoot = sExeDir + QStringLiteral("/HermesHarness");
    const QString sSibling = sSiblingRoot + QStringLiteral("/dist/cli.js");
    if (QFile::exists(sSibling)) {
        return QFileInfo(sSibling).absoluteFilePath();
    }
    const QString sSiblingExe = FindHermesExeUnderRoot(sSiblingRoot);
    if (!sSiblingExe.isEmpty()) {
        return sSiblingExe;
    }
    // 3) PATH 上的 hermes.cmd（npm 全局安装形态）：.cmd 是 npm 生成的 shell 垫片，
    //    不能直接交给 node 拉起（node 直跑 .cmd 必败）——解析垫片内容提取真实
    //    cli.js 绝对路径（cmd-shim 内嵌 "%~dp0\..\..\node_modules\..." 或展开后的
    //    绝对路径）；解析不出可用绝对路径则返回空串走失败提示，绝不回退 .cmd 本身。
    const QString sCmd = QStandardPaths::findExecutable(QStringLiteral("hermes.cmd"));
    if (!sCmd.isEmpty()) {
        CFrwDevice fCmd(sCmd);
        if (fCmd.open(QIODevice::ReadOnly)) {
            const QString sTxt = QString::fromLocal8Bit(fCmd.readAll());
            const QRegularExpression reJsPath(QStringLiteral("\"([^\"]+\\.js)\""));
            QRegularExpressionMatchIterator it = reJsPath.globalMatch(sTxt);
            while (it.hasNext()) {
                const QString sCli = QDir::fromNativeSeparators(it.next().captured(1));
                if (QFileInfo(sCli).isAbsolute() && QFile::exists(sCli)) {
                    return QFileInfo(sCli).absoluteFilePath();
                }
            }
        }
    }
    // 4) PATH 上的 hermes.exe（形态 B 加入 PATH 后的标准形态）：Windows 下
    //    findExecutable("hermes") 按 PATHEXT 命中 hermes.exe，可 exe 直起。
    const QString sOnPath = QStandardPaths::findExecutable(QStringLiteral("hermes"));
    if (!sOnPath.isEmpty() && QFileInfo(sOnPath).isExecutable()) {
        return QFileInfo(sOnPath).absoluteFilePath();
    }
    return QString();
}

bool CHermesRunner::LooksLikeHermesRoot(const QString& a_sDir) {
    return !a_sDir.trimmed().isEmpty()
        && (QFile::exists(a_sDir.trimmed() + QStringLiteral("/dist/cli.js"))
            || !FindHermesExeUnderRoot(a_sDir).isEmpty());
}

QString CHermesRunner::ResolveHermesVersion() {
    // 形态 A cli <root>\dist\cli.js → <root>\package.json 的 version。
    // 形态 B <...>\venv\Scripts\hermes.exe → venv 上级（hermes-agent 仓根）
    //   pyproject.toml 的 `version = "x.y.z"`（正则单行提取）。
    //   npm 全局 hermes.cmd 形态与读不出 → 空串，「未知」口径显示。
    const QString sCli = ResolveHermesCli();
    if (sCli.isEmpty()) {
        return QString();
    }
    if (sCli.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        const QDir dirCli = QFileInfo(sCli).dir();                    // .../venv/Scripts
        const QStringList aPkgDirs = {
            dirCli.filePath(QStringLiteral("../..")),                 // venv 上级
            dirCli.filePath(QStringLiteral("../../../..")),           // root/hermes-agent 形态
        };
        static const QRegularExpression reVer(
            QStringLiteral("^\\s*version\\s*=\\s*\"([^\"]+)\""));
        for (const QString& sDir : aPkgDirs) {
            CFrwDevice stPy(QDir(sDir).filePath(QStringLiteral("pyproject.toml")));
            if (!stPy.open(QIODevice::ReadOnly)) {
                continue;
            }
            while (!stPy.atEnd()) {
                const QRegularExpressionMatch oM = reVer.match(
                    QString::fromUtf8(stPy.readLine()));
                if (oM.hasMatch()) {
                    stPy.close();
                    return oM.captured(1);
                }
            }
            stPy.close();
        }
        return QString();
    }
    const QString sPkg = QFileInfo(sCli).dir()
        .filePath(QStringLiteral("../package.json"));
    CFrwDevice stFile(sPkg);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll());
    stFile.close();
    if (!oDoc.isObject()) {
        return QString();
    }
    return oDoc.object().value(QStringLiteral("version")).toString();
}

QString CHermesRunner::ResolveNodePath() {
    if (!s_sHermesNodePath.isEmpty() && QFile::exists(s_sHermesNodePath)) {
        return s_sHermesNodePath;
    }
    return QStringLiteral("node");   // PATH 解析（不存在时启动报错带指引）
}

QString CHermesRunner::HermesConfigError() {
    if (ResolveHermesCli().isEmpty()) {
        return QStringLiteral(
            "未找到 Hermes 可执行体（探测序：设置页 Hermes 根目录 → HERMES_HOME → "
            "exe 同级 HermesHarness\\dist\\cli.js → PATH 的 hermes）。请安装 Hermes "
            "（Harness 含 dist\\cli.js，或 Python 版含 hermes-agent\\venv\\Scripts\\"
            "hermes.exe）或在设置页填写根目录。");
    }
    if (s_sHermesModel.isEmpty()) {
        return QStringLiteral("Hermes 模型未配置（设置页 Hermes 参数 → 自定义大模型列表「设为当前」一条）。");
    }
    return QString();
}

// —— 生命周期 ——
CHermesRunner::CHermesRunner(QObject* a_pParent) : CAgentRunnerBase(a_pParent) {
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, &CHermesRunner::CheckStall);
}

CHermesRunner::~CHermesRunner() {
    Stop();
}

void CHermesRunner::Reset() {
    m_buf.clear();
    m_bFinished = false;
    m_bSawSession = false;
    m_bSawAssistant = false;
    m_sLastSubtype.clear();
    m_sLastSessionId.clear();
    m_sLastModel.clear();
    m_sLastEventText.clear();
    m_iLastContextFill = 0;
    m_iLastReqCount = 0;
    m_iSumPlainInTok = 0;
    m_iSumCacheReadTok = 0;
    m_iSumCacheWriteTok = 0;
    m_iSumOutTok = 0;
    m_aStderrTail.clear();
    m_bExeForm = false;      // 2026.09.02 exe 形态纯文本回复缓冲（见头注释）
    m_aPlainOut.clear();
    m_sExeHome.clear();      // 2026.09.03 wal 心跳旁路状态（v2.0.105 方案 A）
    m_tLastWalMod = QDateTime();
    m_iLastProgHintMs = 0;   // 2026.09.03 v2.0.108 progress 提示节流重置
    if (!m_sPromptTmpFile.isEmpty()) {   // 2026.09.04 v2.0.117 长 prompt 临时文件清理
        QFile::remove(m_sPromptTmpFile);
        m_sPromptTmpFile.clear();
    }
}

void CHermesRunner::StartStage(const QString& a_sWorkDir,
                               const QString& a_sPrompt,
                               const QString& a_sDisallowedTools,
                               int a_iMaxTurns,
                               const QString& a_sResumeSid,
                               bool a_bFork,
                               int a_iAutocompactK,
                               bool a_bEnableAskUser) {
    Reset();   // a_sWorkDir：工程根（exe 形态注入 TERMINAL_CWD 锚定工具路径，见下）
    ++m_uStageSeq;   // 2026-10-07 代序（延迟重入守卫：迟发回调发现代序已变/已被 Stop 即放弃）
    emit sigInfo(QStringLiteral("[hermes] 启动阶段（prompt %1 字符）")
                     .arg(a_sPrompt.size()));
    // 2026.09.14 技能清单前置（hermes 技能只发现 HERMES_HOME/skills，不认 ~/.claude/skills；
    //   CLI 无按路径挂载参数）：语义同 codex——列名称/描述/SKILL.md 路径，命中时模型自行读取。
    //   在 Reset() 之后改局部副本，不影响基线 prompt。
    QString sPrompt = a_sPrompt;
    const QString sSkillCatalog = CAgentRunnerBase::SharedSkillsCatalog();
    if (!sSkillCatalog.isEmpty()) {
        sPrompt.prepend(sSkillCatalog + QStringLiteral("\n\n---\n\n"));
        emit sigInfo(QStringLiteral("[system/hermes] 已注入技能清单（%1 字符）")
                         .arg(QString::number(sSkillCatalog.size())));
    }
    // 2026.09.13 全局规则注入补齐（勿回退）：hermes 与 codex 同无 CLAUDE.md 自动加载，
    //   v2.3.163 只给 codex 做了全局规则前置，hermes 漏做——用户实测「助手不自动使用
    //   ui-sandbox 技能」根因之二：hermes 助手连「UI 测试→ui-sandbox，禁物理鼠标键盘」
    //   的铁律文本都拿不到。口径同 codex：共享全局规则前置进本阶段 prompt。
    const QString sGlobalCtx = CAgentRunnerBase::SharedGlobalContextContent();
    if (!sGlobalCtx.isEmpty()) {
        sPrompt.prepend(QStringLiteral(
            "以下是全局工程约定（CLAUDE.md），在整个任务中始终遵守：\n\n%1\n\n---\n\n")
            .arg(sGlobalCtx));
        emit sigInfo(QStringLiteral("[system/hermes] 已注入全局规则（%1 字符）")
                         .arg(QString::number(sGlobalCtx.size())));
    }
    // claude 专属参数降级提示（非默认值时提示一次，口径同 Codex）。
    if (a_iMaxTurns != 30 || a_iAutocompactK > 0 || a_bEnableAskUser
        || a_bFork || !a_sResumeSid.isEmpty() || !a_sDisallowedTools.isEmpty()) {
        emit sigInfo(QStringLiteral(
            "[hermes] 提示：maxTurns/disallowedTools/resumeSid/fork/autocompactK/"
            "askUser 为 claude 专属参数，Hermes 后端忽略。"));
    }

    // CLI 探测（先行——运行时配置注入按形态分派）：失败带具体缺失路径报
    //   sigFinished(false)，不静默回退（AC5 前置）。
    const QString sCli = ResolveHermesCli();
    if (sCli.isEmpty()) {
        EmitFailed(HermesConfigError());
        return;
    }
    const bool bExeForm = sCli.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
    m_bExeForm = bExeForm;   // 2026.09.02 HandleLine/slotOnFinished 按形态分派纯文本回复

    // 运行时配置注入按形态分派（便携口径，2026.09 v1.4.97 勿回退）：
    //   形态 A（Node Harness）幂等写 ~/.hermes/agent/config.json——旧 Harness
    //   固定读取处（失败不阻断，HERMES_* 环境变量双保险兜住）；
    //   形态 B（exe）不再写 ~/.hermes——hermes-agent 只认部署根 config.yaml
    //   （EnsureHermesHomeConfig，见下 env 段），写用户主目录属纯垃圾，违反
    //   「AutoCode 自身配置随程序目录/部署根走，不落用户主目录」的便携约定。
    // 2026-10-07 对话级模型覆盖（右键「选择模型」仅本助手生效，不动全局静态）：
    //   覆盖条目快照在实例槽（编排器每轮现读共享表按身份解析注入）；本进程的
    //   env 与 config 写入均按覆盖值。hermes 的模型通道是共享配置文件
    //   （形态 A=~/.hermes/agent/config.json、形态 B=部署根 config.yaml）且进程
    //   在 spawn 后异步读取 → 并发不同模型的 Hermes 助手在彼此「写→读」窗口内
    //   互相串台。守卫：记录最近一次写入的模型身份，紧邻的不同身份启动延迟 4s
    //   重入（python 启动读取窗口远小于该值；同身份不延迟——全局并发行为不变）。
    const SDshModelEntry* pOv = HasStageModelOverride() ? &m_stModelOv : nullptr;
    const QString sWantIdent = (pOv != nullptr)
        ? ModelEntryIdentity(*pOv)
        : (s_sHermesModel.trimmed().toLower() + QChar(1) + s_sHermesBaseUrl.trimmed().toLower());
    const qint64 llNow = QDateTime::currentMSecsSinceEpoch();
    if (!s_sHermesCfgIdent.isEmpty() && s_sHermesCfgIdent != sWantIdent
        && llNow - s_llHermesCfgMs < 4000) {
        const quint32 uSeq = m_uStageSeq;
        const int iWait = int(4200 - (llNow - s_llHermesCfgMs));
        emit sigInfo(QStringLiteral("[hermes] 另一 Hermes 会话刚写入不同模型配置，"
                                    "延迟 %1ms 启动以避开其读取窗口。").arg(iWait));
        const QString sW = a_sWorkDir, sP = a_sPrompt, sT = a_sDisallowedTools,
                      sR = a_sResumeSid;
        const int iT = a_iMaxTurns, iK = a_iAutocompactK;
        const bool bF = a_bFork, bA = a_bEnableAskUser;
        QTimer::singleShot(iWait, this, [this, uSeq, sW, sP, sT, iT, sR, bF, iK, bA]() {
            if (uSeq != m_uStageSeq || m_bFinished) {
                return;   // 延迟期间有新 StartStage（代序已变）或已被 Stop
            }
            StartStage(sW, sP, sT, iT, sR, bF, iK, bA);
        });
        return;
    }
    s_sHermesCfgIdent = sWantIdent;
    s_llHermesCfgMs = llNow;
    if (!bExeForm && !EnsureRuntimeConfig(QString(), pOv)) {
        emit sigInfo(QStringLiteral("[hermes] ~/.hermes 运行时配置写入失败"
                                    "（继续启动，走 HERMES_* 环境变量兜底）。"));
    }
    QString sNode;
    if (!bExeForm) {
        sNode = ResolveNodePath();
        if (sNode != s_sHermesNodePath
            && QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            EmitFailed(QStringLiteral("未找到 node.exe（Hermes CLI 经 node 启动；请在设置页"
                                      "填写 node 路径或安装 Node.js 到 PATH）。"));
            return;
        }
    }

    if (m_pProc != nullptr) {
        m_pProc->deleteLater();
    }
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput,
            this, &CHermesRunner::slotOnStdOut);
    connect(m_pProc, &QProcess::readyReadStandardError,
            this, &CHermesRunner::slotOnStdErr);
    connect(m_pProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &CHermesRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred,
            this, &CHermesRunner::slotOnErrorOccurred);

    // 环境注入（FR8）：四个变量按独立键无条件注入（不继承宿主值——宿主可能是
    //   GBK 控制台环境，继承会把子进程 Python IO 拽回 ANSI 导致输出乱码）；
    //   HERMES_* 与 ~/.hermes 双保险（密钥绝不进命令行/日志）。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // 2026-09-12 手工审批弹窗通道（AUTOCODE_ASK_EXE/ASK_PIPE，全后端统一）
    InjectAskEnv(env);
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    // 2026-10-07 对话级覆盖优先（per-进程 env，形态 A 的双保险通道不受共享文件影响）。
    env.insert(QStringLiteral("HERMES_BASE_URL"),
               (pOv != nullptr && !pOv->sBaseURL.isEmpty()) ? pOv->sBaseURL : s_sHermesBaseUrl);
    env.insert(QStringLiteral("HERMES_API_KEY"),
               (pOv != nullptr && !pOv->sApiKey.isEmpty()) ? pOv->sApiKey : s_sHermesApiKey);
    env.insert(QStringLiteral("HERMES_MODEL"),
               (pOv != nullptr && !pOv->sModelId.isEmpty()) ? pOv->sModelId : s_sHermesModel);
    // 形态 B（exe 直起）：注入 HERMES_HOME（venv 内的 exe 可能定位不到部署根）
    //   并幂等注入部署根 config.yaml model 段；
    //   -z 一次性模式吃掉 prompt，输出纯文本回复（HandleLine 积累、收尾整段上抛
    //   为 assistant 文本，2026.09 v1.4.99；无 usage/session 事件，token 统计
    //   为 0 属该形态已知降级）。
    // ⚠ 勿回退（2026.09 v1.8.29 修）：不再注入 HERMES_INFERENCE_MODEL——hermes -z
    //   收到该变量会对模型名跑 detect_provider_for_model 目录侦测，网关模型名
    //   （如 glm-5.3）命中 OpenRouter 目录 z-ai/glm-5.3 → provider 被劫持成
    //   openrouter（无 OPENROUTER_API_KEY）→ "No LLM provider configured"
    //   退出 code 1（实测复现）。模型/网关/密钥改经 config.yaml model 段传达
    //   （EnsureHermesHomeConfig）；不带 env 模型时 oneshot 走 config 默认值
    //   "use my defaults" 路径，不触发目录侦测。HERMES_MODEL/HERMES_API_KEY/
    //   HERMES_BASE_URL 保留（TUI gateway 路径认，无害）。
    if (bExeForm) {
        const QString sHome = ResolveHermesHomeForExe(sCli);
        if (!sHome.isEmpty()) {
            env.insert(QStringLiteral("HERMES_HOME"), sHome);
            // 幂等注入失败提示不阻断：config.yaml 可能已手工配好或介质只读。
            if (!EnsureHermesHomeConfig(sHome + QStringLiteral("/config.yaml"), pOv)) {
                emit sigInfo(QStringLiteral("[hermes] 部署根 config.yaml model 段注入"
                                            "未完成（若已手工配置可忽略）。"));
            }
            // 2026.09.03 v2.0.103 oneshot 事件补丁：幂等敷设 + 环境开关——上游 -z
            //   刻意全静音（无 CLI 开关），补丁接通 tool_gen_callback 往真 stdout
            //   打 NDJSON 事件（tool/result 携 usage/session/api_calls），本类
            //   HandleLine 按原 JSON 契约解析（工具卡/心跳/token 统计/上下文条）。
            //   v2.0.105 改软编码：敷设经 install_env.bat /hermes-patch（选择后端
            //   时 CMainWindow 已触发，此处兜底 hermes 更新后的重敷）；失败仅提示
            //   不阻断：回退上游静默形态，回复照常、用量缺省 0。
            if (!EnsureHermesOneshotPatch(sHome)) {
                // 2026.09.08 v2.1.126 失败原因自含：敷设后复检仍非 v5 多为 exe 侧
                //   hermes_oneshot_patch.py 旧版（01.更新程序.bat v2.1.126 起同步该文件，
                //   旧副本不同步 → 部署标记永远到不了 v5，每轮重敷设且缺 @file 投递
                //   展开——长 prompt>12000 字符会以 "@<路径>" 原文喂给模型）。
                emit sigInfo(QStringLiteral(
                    "[hermes] oneshot 事件补丁未敷设到 v5（install_env.bat /hermes-patch"
                    "未成功，或 exe 同级 hermes_oneshot_patch.py 为旧版），本轮可能无工具/用量事件；"
                    "长提示词投递也可能异常——请从完整程序目录更新该 py 后重试。"));
            }
            env.insert(QStringLiteral("HERMES_ONESHOT_EVENTS"), QStringLiteral("1"));
            // v2.0.105 方案 A：部署根记入 wal 心跳旁路（CheckStall 巡检用）。
            m_sExeHome = sHome;
            m_tLastWalMod = HermesWalMtime(sHome);
        }
        // 2026.09.05 v2.0.119 修 Hermes exe 形态工作目录漂移（用户报障：「任务一直
        //   无法生成 intake.md」）：hermes-agent -z 一次性模式的 terminal/read_file/
        //   patch 等工具**不认进程 cwd**，统一锚定 HOME——实测最小复现：任意 cwd 下
        //   `hermes.exe -z "跑 pwd"` 恒返回 /c/Users/Administrator，相对路径
        //   `.autodev/tasks/<id>/intake.md` 全部写到 C:\Users\Administrator\.autodev\下，
        //   AutoCode 在工程目录校验失败 → 阶段反复回退 → 「多次回退仍未生成
        //   intake.md」升级暂停（实害任务：拳皇 aea501f7，5 轮全部写错位）。注入
        //   TERMINAL_CWD（hermes 官方工作区锚点，file_tools/_get_env_config/
        //   LocalEnvironment 全链路读取，要求绝对路径、非 "."/"auto" 等 sentinel）
        //   钉到工程根；实测注入后 pwd 与相对路径读写均正确落工程目录。形态 A
        //   （Node Harness）尊重进程 cwd（2026.09.01 买证），不注入避免改变其行为。
        if (!a_sWorkDir.isEmpty() && QDir::isAbsolutePath(a_sWorkDir)) {
            env.insert(QStringLiteral("TERMINAL_CWD"),
                       QDir::toNativeSeparators(a_sWorkDir));
        }
    }
    m_pProc->setProcessEnvironment(env);
    m_pProc->setWorkingDirectory(a_sWorkDir.isEmpty() ? QDir::currentPath() : a_sWorkDir);
    if (bExeForm) {
        // 2026.09.04 v2.0.117：Windows CreateProcess 命令行 32767 字符上限——
        //   长 prompt（回灌缓存，实测大工程会话轻易超限报「文件名或扩展名
        //   太长」）改写 UTF-8 临时文件传 "@<path>"（补丁 v5 在 run_oneshot 入口
        //   展开）；短 prompt 仍走命令行。临时文件收尾/停止后删除。
        QString sPromptArg = sPrompt;
        if (!m_sPromptTmpFile.isEmpty()) {
            QFile::remove(m_sPromptTmpFile);
            m_sPromptTmpFile.clear();
        }
        if (sPrompt.size() > 12000) {
            QTemporaryFile fTmp(QDir::temp().absoluteFilePath(
                QStringLiteral("autocode-hermes-prompt-XXXXXX.txt")));
            fTmp.setAutoRemove(false);
            if (fTmp.open()) {   // QTemporaryFile 无参 open（默认读写截断）
                fTmp.write(sPrompt.toUtf8());
                fTmp.close();
                m_sPromptTmpFile = fTmp.fileName();
                sPromptArg = QStringLiteral("@") + QDir::toNativeSeparators(m_sPromptTmpFile);
            }   // 写失败回退原文（超限时启动报错自带真因）
        }
        m_pProc->start(sCli, QStringList()
                       << QStringLiteral("-z") << sPromptArg);
    } else {
        m_pProc->start(sNode, QStringList()
                       << QDir::toNativeSeparators(sCli)
                       << QStringLiteral("--mode") << QStringLiteral("json")
                       << QStringLiteral("-"));
    }

    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("Hermes 子进程启动失败（%1 %2）：%3").arg(
            bExeForm ? sCli : sNode, QDir::toNativeSeparators(sCli),
            m_pProc->errorString()));
        return;
    }
    if (!bExeForm) {
        // prompt 经 stdin 投递（同 pi/codex：不进命令行防长度/转义/日志泄露）。
        //   exe 形态 prompt 已随 -z 参数投递（-z 不支持 stdin 读入）。
        m_pProc->write(sPrompt.toUtf8());
        m_pProc->closeWriteChannel();
    }
    m_actClock.start();
    m_pStallTimer->start();
}

void CHermesRunner::Stop() {
    // 静默收尾（同 Pi/Codex 口径）：置终态 → kill → 不发 sigFinished。
    if (m_pProc == nullptr) {
        return;
    }
    m_bFinished = true;
    m_pStallTimer->stop();
    if (m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
}

bool CHermesRunner::IsRunning() const {
    return m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning && !m_bFinished;
}

bool CHermesRunner::IsRetryPending() const {
    return false;   // 一次性进程，无快速失败重试
}

bool CHermesRunner::InjectUserTurn(const QString& a_sText) {
    Q_UNUSED(a_sText);
    return false;   // 一次性进程无轮次中插话
}

// —— 事件流解析 ——
void CHermesRunner::slotOnStdOut() {
    if (m_pProc == nullptr) {
        return;
    }
    m_actClock.start();
    m_buf += m_pProc->readAllStandardOutput();
    int iIdx = -1;
    while ((iIdx = m_buf.indexOf('\n')) >= 0) {
        const QByteArray aLine = m_buf.left(iIdx).trimmed();
        m_buf.remove(0, iIdx + 1);
        if (!aLine.isEmpty()) {
            HandleLine(aLine);
        }
    }
}

void CHermesRunner::HandleLine(const QByteArray& a_sLine) {
    QJsonParseError stErr{};
    const QJsonDocument oDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        // 2026.09.02 exe 形态（hermes -z）：stdout 即回复正文（纯文本，实测
        //   v0.20.4）——积累到 m_aPlainOut 由 slotOnFinished 整段上抛
        //   sigAssistantText。不再 sigInfo 透传：旧口径把回复显示成灰字系统行
        //   且 m_bSawAssistant 恒假 → 误报「code 0：本轮无 assistant 输出」。
        if (m_bExeForm) {
            m_aPlainOut.append(QString::fromUtf8(a_sLine));
            return;
        }
        emit sigInfo(QString::fromUtf8(a_sLine));   // 非 JSON 行按原始信息展示（形态 A 前向兼容）
        return;
    }
    const QJsonObject o = oDoc.object();
    const QString sType = o.value(QStringLiteral("type")).toString();

    if (sType == QStringLiteral("session")) {
        m_bSawSession = true;
        m_sLastSessionId = o.value(QStringLiteral("sessionId")).toString();
        emit sigInfo(QStringLiteral("[hermes] 会话已启动（session %1）").arg(m_sLastSessionId));
        return;
    }
    if (sType == QStringLiteral("assistant")) {
        const QString sText = o.value(QStringLiteral("text")).toString();
        const QString sModel = o.value(QStringLiteral("model")).toString();
        if (!sModel.isEmpty()) {
            m_sLastModel = sModel;
        }
        ApplyUsage(o.value(QStringLiteral("usage")).toObject());
        ++m_iLastReqCount;   // assistant 消息条数 = API 请求数（基类口径）
        if (!sText.isEmpty()) {
            m_bSawAssistant = true;
            // 2026.09.05 轮次上限兜底英文 → 中文提示（见 LocalizeHermesCapText）。
            emit sigAssistantText(LocalizeHermesCapText(sText));
        }
        return;
    }
    if (sType == QStringLiteral("progress")) {
        // 2026.09.03 v2.0.108 生成阶段流式进度（补丁 v2 接 stream_delta_callback
        //   节流聚合 >=4s 一条）：刷新心跳（纯生成段 stdout 零输出是 -z 设计，
        //   此前只能靠 wal/3 倍墙兑底，现在生成期间心跳实时刷）+ 60s 节流提示行
        //   （对齐 pi「⏳ 仍在流式生成」v1.4.81 口径）；chars=本段累计（工具调用
        //   清零重计），tail=尾部窗口预览（清洗换行、截 48 字）。
        m_actClock.start();
        const qint64 iNow = QDateTime::currentMSecsSinceEpoch();
        // v2.0.113：节流 60s→15s+首条即显（用户实测纯生成轮长时间零反馈：
        //   glm 短段 2~3 字被 chars>=30 阈值吞、60s 节流下长生成轮一分钟才
        //   一行；现每段开始即见行，chars>=30 才带字数+预览，否则简版）。
        if (iNow - m_iLastProgHintMs >= 15000) {
            m_iLastProgHintMs = iNow;
            QString sTail = o.value(QStringLiteral("tail")).toString();
            sTail.replace(QLatin1Char('\n'), QLatin1Char(' '))
                 .replace(QLatin1Char('\r'), QLatin1Char(' '));
            if (sTail.size() > 48) {
                sTail = sTail.left(48) + QStringLiteral("…");
            }
            emit sigInfo(QStringLiteral("⏳ 模型生成中…")
                         + (o.value(QStringLiteral("chars")).toInt() >= 30
                                ? QStringLiteral(" 本段已输出 %1 字")
                                      .arg(o.value(QStringLiteral("chars")).toInt())
                                      + (sTail.isEmpty()
                                             ? QString()
                                             : QStringLiteral("（…") + sTail + QStringLiteral("…）"))
                                : QString()));
        }
        return;
    }
    if (sType == QStringLiteral("think")) {
        // 2026.09.04 v2.0.114 思考流（补丁 v3 "_thinking"/"reasoning.available"）：
        //   v4 中间段文本已按正式 assistant 块上抛且与思考内容重叠（实测成对
        //   同文），💭 行取消显示只刷停滞心跳（旧 v3 补丁无中间文本，此分支
        //   保留提示逻辑已无必要——补丁版本与 exe 同步升级，不留双轨）。
        m_actClock.start();
        return;
    }
    if (sType == QStringLiteral("tooldone")) {
        // 2026.09.04 v2.0.111 工具完成（补丁 v3 "tool.completed"：耗时/错误/
        //   结果摘要）：改走 sigToolResult 回填工具卡结果区（对齐 pi/dsh/
        //   claude 的过程体验——工具卡带结果+耗时；错误前缀 ✘ 供识别），
        //   刷新停滞心跳。
        m_actClock.start();
        QString sRes = o.value(QStringLiteral("result")).toString();
        const qint64 iMs = o.value(QStringLiteral("ms")).toInteger();
        const QString sDur = iMs > 0
            ? QStringLiteral("（%1s）").arg(iMs / 1000.0, 0, 'f', 1)
            : QString();
        if (o.value(QStringLiteral("err")).toBool()) {
            sRes = QStringLiteral("✘ 失败") + sDur + QStringLiteral("：") + sRes;
        } else {
            sRes = sDur + sRes;
        }
        if (sRes.size() > 1200) {
            sRes = sRes.left(1200) + QStringLiteral(" …[截断]");
        }
        emit sigToolResult(sRes);
        return;
    }
    if (sType == QStringLiteral("tool")) {
        const QJsonObject oIn = o.value(QStringLiteral("input")).toObject();
        const QString sPreview = o.value(QStringLiteral("preview")).toString();
        if (!oIn.isEmpty()) {
            // 形态 A JSON 契约（桩 CLI/Node）：完整入参工具卡。
            emit sigToolUse(o.value(QStringLiteral("tool")).toString(),
                            QString::fromUtf8(QJsonDocument(oIn).toJson(QJsonDocument::Compact)));
            return;
        }
        if (!sPreview.trimmed().isEmpty()) {
            // 2026.09.04 v2.0.111 补丁 v3 "tool.started" 带脱敏参数预览（如
            //   read_file config.yaml L501）→ 真正的工具卡（对齐 pi/dsh/claude
            //   过程体验）；v2.0.107 轻量系统行是无 preview 时代的过渡形态，
            //   降为兑底。
            m_actClock.start();
            emit sigToolUse(o.value(QStringLiteral("tool")).toString(), sPreview);
            return;
        }
        // v1/v2 补丁时代的 tool 事件（仅工具名）：轻量系统行兑底——连续
        //   System 行自动并入折叠组（v1.4.92 机制）。
        emit sigInfo(QStringLiteral("[hermes] ⚙ ")
                         + o.value(QStringLiteral("tool")).toString());
        return;
    }
    if (sType == QStringLiteral("result")) {
        m_sLastSubtype = o.value(QStringLiteral("subtype")).toString();
        const QString sSid = o.value(QStringLiteral("sessionId")).toString();
        if (!sSid.isEmpty()) {
            m_sLastSessionId = sSid;
        }
        ApplyUsage(o.value(QStringLiteral("usage")).toObject());
        // 2026.09.03 v2.0.103 exe 形态事件补丁上报的真实 API 请求数（exe 形态
        //   无逐条 assistant 事件，基类口径的清点不可用；无此键不覆盖）。
        const int iApiCalls = static_cast<int>(o.value(QStringLiteral("apiCalls")).toInt());
        if (iApiCalls > 0) {
            m_iLastReqCount = iApiCalls;
        }
        // 2026.09.05 轮次上限兜底英文 → 中文提示（见 LocalizeHermesCapText；
        //   影响 sigFinished 结果文本与自动命名/压缩摘要取值）。
        m_sLastEventText = LocalizeHermesCapText(
            o.value(QStringLiteral("text")).toString());
        return;   // 终态在进程退出时统一上抛（slotOnFinished）
    }
    // 未知事件：原始透传 sigInfo（前向兼容，真实格式落定后在此扩展分支）。
    emit sigInfo(QString::fromUtf8(a_sLine));
}

void CHermesRunner::ApplyUsage(const QJsonObject& oUsage) {
    // usage 分量（累计口径同 pi）：input/cacheRead/cacheWrite/output；缺字段容错为 0。
    const qint64 iIn   = static_cast<qint64>(oUsage.value(QStringLiteral("input")).toDouble());
    const qint64 iCRd  = static_cast<qint64>(oUsage.value(QStringLiteral("cacheRead")).toDouble());
    const qint64 iCWr  = static_cast<qint64>(oUsage.value(QStringLiteral("cacheWrite")).toDouble());
    const qint64 iOut  = static_cast<qint64>(oUsage.value(QStringLiteral("output")).toDouble());
    // Hermes 按「本轮增量」上报（每条 assistant 一份该请求 usage）→ 累加进总额。
    m_iSumPlainInTok   += iIn;
    m_iSumCacheReadTok += iCRd;
    m_iSumCacheWriteTok+= iCWr;
    m_iSumOutTok       += iOut;
    const qint64 iFill = iIn + iCRd + iCWr;   // 上下文占用（三输入项之和，同 claude 口径）
    if (iFill > 0) {
        m_iLastContextFill = iFill;
        emit sigContextFill(iFill);
    }
    if (iIn > 0 || iOut > 0) {
        emit sigUsageDelta(iIn, iOut);
    }
}

double CHermesRunner::ComputeTurnCost() const {
    // 费用 = 各分量 × 单价（$/M token）；四项全 0 = 不折算（费用列回 "-"）。
    return (static_cast<double>(m_iSumPlainInTok) * s_dHermesPriceInPerM
          + static_cast<double>(m_iSumCacheReadTok) * s_dHermesPriceCacheReadPerM
          + static_cast<double>(m_iSumCacheWriteTok) * s_dHermesPriceCacheWritePerM
          + static_cast<double>(m_iSumOutTok) * s_dHermesPriceOutPerM) / 1000000.0;
}

void CHermesRunner::slotOnStdErr() {
    if (m_pProc == nullptr) {
        return;
    }
    const QString sErr = QString::fromUtf8(m_pProc->readAllStandardError());
    m_aStderrTail.append(sErr.split(QLatin1Char('\n')));
    while (m_aStderrTail.size() > 20) {
        m_aStderrTail.removeFirst();   // 尾部 20 行（失败消息附带）
    }
    emit sigInfo(QStringLiteral("[hermes][stderr] ") + sErr.trimmed());
}

void CHermesRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    Q_UNUSED(a_eStatus);
    m_pStallTimer->stop();
    if (!m_sPromptTmpFile.isEmpty()) {   // 2026.09.04 v2.0.117 长 prompt 临时文件：进程已退出可删
        QFile::remove(m_sPromptTmpFile);
        m_sPromptTmpFile.clear();
    }
    if (m_bFinished) {
        return;   // Stop() 静默收尾 / 已报 finish
    }
    m_bFinished = true;
    // 清残留缓冲（进程退出前的最后一行可能无换行）。
    if (!m_buf.isEmpty()) {
        HandleLine(m_buf.trimmed());
        m_buf.clear();
    }
    // 2026.09.02 exe 形态收尾：-z 纯文本回复整段上抛为 assistant 输出（一次一块，
    //   Markdown 完整不拆行）；置 m_bSawAssistant 令成功判定通过。无任何输出则
    //   维持失败路径（真无输出，附 stderr 尾）；token/sid 为 0/空属已知降级。
    // 2026.09.03 v2.0.102：结果文本（m_sLastEventText）周设为同一段回复——旧口径
    //   exe 形态无 result 事件→结果恒为占位符"完成"，AutoNaming 取结果首行当
    //   任务名（实测任务被命名成「完成」）；压缩摘要（CompactSummarize）同理。
    if (m_bExeForm && !m_aPlainOut.isEmpty()) {
        m_bSawAssistant = true;
        // 2026.09.05 轮次上限兜底英文 → 中文提示（见 LocalizeHermesCapText）。
        m_sLastEventText = LocalizeHermesCapText(m_aPlainOut.join(QLatin1Char('\n')));
        emit sigAssistantText(m_sLastEventText);
        m_aPlainOut.clear();
    }
    const bool bOk = (a_iExitCode == 0)
        && (m_sLastSubtype.isEmpty() || m_sLastSubtype == QStringLiteral("success"))
        && m_bSawAssistant;
    QString sResult;
    if (bOk) {
        sResult = m_sLastEventText.isEmpty() ? QStringLiteral("完成") : m_sLastEventText;
    } else {
        sResult = QStringLiteral("Hermes 进程退出（code %1）：%2").arg(
            QString::number(a_iExitCode),
            m_sLastEventText.isEmpty()
                ? (m_bSawAssistant ? QStringLiteral("无 result 事件")
                                   : QStringLiteral("本轮无 assistant 输出"))
                : m_sLastEventText);
        // 失败消息自含真因（2026.09 v1.8.29，口径同 Claude EmitFailed/stderrTail）：
        //   hermes -z 的报错只走 stderr（agent 全程把 stdout 重定向 devnull），
        //   不带 stderr 尾巴时用户只看到「本轮无 assistant 输出」无从排查
        //   （如 "No LLM provider configured"）。sigInfo 已实时展示过，这里
        //   收尾再附一次保证 ✗ 失败块内自含。
        if (!m_aStderrTail.isEmpty()) {
            sResult += QStringLiteral("\nstderr：\n")
                     + m_aStderrTail.join(QLatin1Char('\n'));
        }
    }
    emit sigFinished(bOk, sResult, m_iSumPlainInTok + m_iSumCacheReadTok + m_iSumCacheWriteTok,
                     m_iSumOutTok, ComputeTurnCost(), m_sLastSessionId);
}

void CHermesRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    if (a_eError == QProcess::Crashed || m_bFinished) {
        return;   // kill 触发的 Crashed 不算失败（Stop 静默口径）
    }
    EmitFailed(QStringLiteral("Hermes 子进程错误（%1）：%2").arg(
        QString::number(static_cast<int>(a_eError)),
        m_pProc ? m_pProc->errorString() : QString()));
}

void CHermesRunner::CheckStall() {
    // 停滞检测（同 pi/codex 语义）：进程活着但距上次收到任何输出超 StallMinutes。
    if (m_pProc == nullptr || m_bFinished
        || m_pProc->state() == QProcess::NotRunning) {
        return;
    }
    // 停滞阈值：exe 形态（hermes -z）放宽 3 倍——2026.09.03 实测 ProgressMonior
    //   初始任务：guide 生成经 -z 跑 agent 工具循环，终答前 stdout 零输出，
    //   合法静默超过 10 分钟被误杀 → 初始任务暂停。-z 不像 JSON 流式后端每步
    //   都有事件刷新心跳；真挂场景（网关不可达/无 provider）通常快速报错退出，
    //   不依赖 stall 兕底，故该形态取 3 倍（默认 30 分钟）。
    // v2.0.105 方案 A 心跳旁路：会话库 state.db-wal/-shm 随 agent 活动增量落盘
    //   （实测每 10~17s 刷新），wal mtime 前移即刷新心跳——工具/落盘阶段的误杀
    //   由它兑底；纯长生成（无落盘）阶段仍靠 3 倍窗口。
    qint64 iLimitMs = static_cast<qint64>(CClaudeRunner::StallMinutes()) * 60000;
    if (m_bExeForm) {
        iLimitMs *= 3;
        if (!m_sExeHome.isEmpty()) {
            const QDateTime tWal = HermesWalMtime(m_sExeHome);
            if (tWal.isValid() && tWal != m_tLastWalMod) {
                m_tLastWalMod = tWal;      // 会话库在写 = agent 活着（零 stdout 也算心跳）
                m_actClock.start();
            }
        }
    }
    if (m_actClock.isValid() && m_actClock.elapsed() > iLimitMs) {
        m_sLastSubtype = QStringLiteral("stall");   // 复用基类词汇（watchdog 终止可续跑）
        const QString sSid = m_sLastSessionId;
        // 先置终态再终止进程（口径同 Codex CheckStall）：Stop 置 m_bFinished=true 后
        //   slotOnFinished 命中防重复保护直接返回，杜绝 stall 后 3s 内二次 sigFinished。
        // 2026.09.03 v2.0.106：消息显示生效阈值（exe 形态 = 基础 ×3；旧文案恒打基础值，
        //   30 分钟击杀时误号「10 分钟」，排查误导）。
        const int iEffMin = static_cast<int>(iLimitMs / 60000);
        Stop();
        emit sigFinished(false, QStringLiteral("Hermes 停滞检测命中（%1 分钟零输出），已终止。")
                         .arg(iEffMin),
                         m_iSumPlainInTok + m_iSumCacheReadTok + m_iSumCacheWriteTok,
                         m_iSumOutTok, ComputeTurnCost(), sSid);
    }
}

void CHermesRunner::EmitFailed(const QString& a_sMsg) {
    m_pStallTimer->stop();
    if (m_bFinished) {
        return;
    }
    m_bFinished = true;
    if (!m_aStderrTail.isEmpty()) {
        emit sigFinished(false, a_sMsg + QStringLiteral("\nstderr：\n") + m_aStderrTail.join(QLatin1Char('\n')),
                         0, 0, 0.0, QString());
    } else {
        emit sigFinished(false, a_sMsg, 0, 0, 0.0, QString());
    }
}
