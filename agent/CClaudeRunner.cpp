#include "CClaudeRunner.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QPointer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

CClaudeRunner::CClaudeRunner(QObject* a_pParent)
    : CAgentRunnerBase(a_pParent) {
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput, this, &CClaudeRunner::slotOnStdOut);
    // 2026.08.15：stderr 必须读取并落日志——claude CLI 的报错原因（API 错误等）只打在 stderr，
    //   此前从不读取，exit 1 时只能看到裸退出码（a0c96b4d 开发阶段 03:19 失败真因即丢失）；
    //   且不读会积满管道缓冲、可能反向阻塞子进程。
    connect(m_pProc, &QProcess::readyReadStandardError, this, &CClaudeRunner::slotOnStdErr);
    connect(m_pProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CClaudeRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred, this, &CClaudeRunner::slotOnErrorOccurred);
    // 2026.08.17 停滞巡检定时器：常驻创建、按轮次启停（默认不跑，StartStage 武装时才 start）。
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, [this]() {
        CheckStall();
    });
}

CClaudeRunner::~CClaudeRunner() {
    Stop();
#ifdef Q_OS_WIN
    // 2026.09.22 KILL_ON_JOB_CLOSE 兑底：关闭 Job 句柄 → 树内仍存活的孤儿（cmd 已死
    //   而 node 未收尾的场景）一并终结，程序退出无残留。
    if (m_hJob != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_hJob));
        m_hJob = nullptr;
    }
#endif
}

// —— 进程树整杀（2026.09.22，2d9e4c0c 教训）——
bool CClaudeRunner::EnsureJobObject() {
#ifdef Q_OS_WIN
    if (m_hJob == nullptr) {
        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob == nullptr) {
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION stInfo = {};
        stInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                     &stInfo, sizeof(stInfo))) {
            CloseHandle(hJob);
            return false;
        }
        m_hJob = hJob;
    }
    // 把当前子进程（cmd.exe）挂入 Job：其子孙（node 等）全部进树，随 Job 同生共死。
    if (m_pProc == nullptr || m_pProc->processId() == 0) {
        return false;
    }
    HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(m_pProc->processId()));
    if (hProc == nullptr) {
        return false;
    }
    const BOOL bOk = AssignProcessToJobObject(static_cast<HANDLE>(m_hJob), hProc);
    CloseHandle(hProc);
    return bOk != FALSE;
#else
    return false;
#endif
}

void CClaudeRunner::KillProcessTree() {
    // 优先整树终止（cmd+node 一锅端）；Job 不可用（创建失败/非 Windows）退回单点 kill（旧行为）。
#ifdef Q_OS_WIN
    if (m_hJob != nullptr && TerminateJobObject(static_cast<HANDLE>(m_hJob), 0)) {
        return;
    }
#endif
    if (m_pProc != nullptr) {
        m_pProc->kill();
    }
}

// —— 进程级全局配置（static：所有 runner 实例共享，主窗口启动/设置变更时写）——
static QString s_sClaudeCmd;
static int     s_iAskTimeoutSec = 0;   // 0 = 未设置，回退默认 600
static int     s_iStallMinutes = 0;    // 2026.08.17 0 = 未设置，回退默认 10 分钟

// 2026.08.27 旧版 CLI 兼容：探测当前 claude 是否支持 --autocompact（v2.1.221+）。
//   其他电脑装旧版 Claude Code 时带该参会 unknown option 退出码 1。
//   探测一次缓存；探测本身失败（超时/未装）按支持处理（保持本机现状，不退化）。
static bool ClaudeSupportsAutocompact() {
    static int s_iProbe = 0;           // 0=未探测 1=支持 -1=不支持
    if (s_iProbe == 0) {
        QProcess oProbe;
        // 2026.10.10 探测同用合并 PATH（异地新机装完 claude 未重启 AutoCode 也能探到）。
        oProbe.setProcessEnvironment(CAgentRunnerBase::MergedPathEnvironment());
        oProbe.start(QStringLiteral("cmd.exe"),
                     {QStringLiteral("/c"), CClaudeRunner::ClaudeCmd(), QStringLiteral("--help")});
        if (!oProbe.waitForStarted(5000) || !oProbe.waitForFinished(15000)) {
            return true;               // 探测失败不缓存，下次再试；按支持（现状）
        }
        const QByteArray aOut = oProbe.readAllStandardOutput()
                             + oProbe.readAllStandardError();
        s_iProbe = aOut.contains("autocompact") ? 1 : -1;
    }
    return s_iProbe != -1;
}

// 2026.08.27 探测 --include-partial-messages（流式分块，供本轮计量实时刷新）：同上
//   一次性探测缓存；旧 CLI 无此参不带（不影响功能，仅实时性回退为逐消息）。复用同一次
//   --help 输出，两次探测合并为一次进程拉起。
static bool s_bPartialProbeDone = false;
static bool s_bPartialSupported = false;
static bool ClaudeSupportsPartialMessages() {
    if (!s_bPartialProbeDone) {
        QProcess oProbe;
        // 2026.10.10 探测同用合并 PATH（同 ClaudeSupportsAutocompact）。
        oProbe.setProcessEnvironment(CAgentRunnerBase::MergedPathEnvironment());
        oProbe.start(QStringLiteral("cmd.exe"),
                     {QStringLiteral("/c"), CClaudeRunner::ClaudeCmd(), QStringLiteral("--help")});
        if (!oProbe.waitForStarted(5000) || !oProbe.waitForFinished(15000)) {
            return false;              // 探测失败不缓存；不带（保守，功能不受影响）
        }
        const QByteArray aOut = oProbe.readAllStandardOutput()
                             + oProbe.readAllStandardError();
        s_bPartialSupported = aOut.contains("include-partial-messages");
        s_bPartialProbeDone = true;
    }
    return s_bPartialSupported;
}

QList<SDshModelEntry> CClaudeRunner::s_aSharedModels;
int  CClaudeRunner::s_iSharedActive = -1;
bool CClaudeRunner::s_bFollowSharedModels = false;
int  CClaudeRunner::s_iMaxThinkTok = 0;
QString CClaudeRunner::s_sSmallFastModel;

void CClaudeRunner::SetMaxThinkingTokens(int a_iTok) { s_iMaxThinkTok = a_iTok; }
void CClaudeRunner::SetSmallFastModel(const QString& a_sModel) { s_sSmallFastModel = a_sModel.trimmed(); }

void CClaudeRunner::SetSharedModelTable(const QList<SDshModelEntry>& a_aModels,
                                        int a_iActive) {
    s_aSharedModels = a_aModels;
    s_iSharedActive = a_iActive;
}

void CClaudeRunner::SetFollowSharedModels(bool a_bOn) {
    s_bFollowSharedModels = a_bOn;
}

bool CClaudeRunner::FollowSharedModels() {
    return s_bFollowSharedModels;
}

void CClaudeRunner::SetClaudeCmd(const QString& a_sCmd) {
    // 2026.08.15 [Agent] ClaudeCmd：空串/全空白回退默认 "claude"（依赖 PATH）。
    s_sClaudeCmd = a_sCmd.trimmed().isEmpty() ? QStringLiteral("claude")
                                             : a_sCmd.trimmed();
}

QString CClaudeRunner::ClaudeCmd() {
    // 未初始化（启动未加载 config）时按默认值。
    if (s_sClaudeCmd.isEmpty()) {
        return QStringLiteral("claude");
    }
    return s_sClaudeCmd;
}

void CClaudeRunner::SetAskTimeoutSec(int a_iSec) {
    // 2026.08.15 [Agent] AskTimeoutSec：非正数回退默认 600 秒。
    s_iAskTimeoutSec = (a_iSec > 0) ? a_iSec : 600;
}

int CClaudeRunner::AskTimeoutSec() {
    return s_iAskTimeoutSec > 0 ? s_iAskTimeoutSec : 600;
}

void CClaudeRunner::SetStallMinutes(int a_iMin) {
    // 2026.08.17 [Agent] StallMinutes：非法值（<1 或 >120）回退默认 10 分钟。
    s_iStallMinutes = (a_iMin >= 1 && a_iMin <= 120) ? a_iMin : 10;
}

int CClaudeRunner::StallMinutes() {
    return s_iStallMinutes > 0 ? s_iStallMinutes : 10;
}

qint64 CClaudeRunner::ContextWindowForModel(const QString& a_sModel) {
    // 按模型名粗分上下文窗口（token）；未命中回退 200K。具体窗口以模型官方规格为准
    // （新模型可在此补充映射）。原实现于 CExecutionPanel::ContextLimitForModel，
    // 2026.08.15 下沉到 agent 层供编排器复用。
    // 2026-10-14/15 条目优先：共享模型表（设置→模型页）里用户为条目显式设置的 iCtx
    //   是权威值优先使用——glm-5.3 等新模型规格不在下方名字探测表内（恒 128K），
    //   用户在条目里设 1M 后 UI/闸门/压缩阈值应同步（实测反馈：条目设 1000000
    //   主界面进度条仍显示 128K，即取值处均未读条目值）。同名模型可有多条目
    //   （不同网关/副本，Ctx 可不同），匹配优先级：
    //   ① provider 槽位前缀（pi/codex 回报 "autocode-1/glm-5.3"）→ 表内第 N-1 条
    //     （与 ~/.pi/agent/models.json 槽位同源；尾部模型名一致才认，表重排后尖链）；
    //   ② 无前缀 → 激活条目优先（当前在用），否则首个同名条目；
    //   ③ 条目未命中或 iCtx 非法（≤0）→ 名字粗分探测兑底。
    const QString sFull = a_sModel.trimmed();
    const QString sTail = [&sFull]() {
        const int iPos = sFull.lastIndexOf(QLatin1Char('/'));
        return (iPos >= 0 ? sFull.mid(iPos + 1) : sFull).toLower();
    }();
    if (!sTail.isEmpty() && !s_aSharedModels.isEmpty()) {
        static const QRegularExpression reSlot(QStringLiteral("^autocode-(\\d+)/"));
        const QRegularExpressionMatch mSlot = reSlot.match(sFull);
        const int iSlot = mSlot.hasMatch() ? mSlot.captured(1).toInt() : 0;   // 0=无槽位前缀
        const auto fnCtxOf = [&sTail](const SDshModelEntry& stE) -> qint64 {
            return (stE.sModelId.trimmed().toLower() == sTail && stE.iCtx > 0)
                       ? stE.iCtx : 0;
        };
        // ① 槽位命中（下标有效且模型名一致）；② 激活条目；③ 首个同名条目
        if (iSlot >= 1 && iSlot <= s_aSharedModels.size()) {
            const qint64 iCtx = fnCtxOf(s_aSharedModels.at(iSlot - 1));
            if (iCtx > 0) {
                return iCtx;
            }
        }
        if (s_iSharedActive >= 0 && s_iSharedActive < s_aSharedModels.size()) {
            const qint64 iCtx = fnCtxOf(s_aSharedModels.at(s_iSharedActive));
            if (iCtx > 0) {
                return iCtx;
            }
        }
        for (const SDshModelEntry& stE : s_aSharedModels) {
            const qint64 iCtx = fnCtxOf(stE);
            if (iCtx > 0) {
                return iCtx;
            }
        }
    }
    const QString s = a_sModel.toLower().trimmed();
    if (s.contains(QStringLiteral("glm"))) {
        return 128000;   // GLM-4.x 系列（按公开规格 128K）
    }
    if (s.contains(QStringLiteral("claude"))) {
        return 200000;   // Claude Sonnet/Opus/Haiku 4 系列（公开 200K）
    }
    return 200000;       // 未知模型兜底
}

// —— 2026.09 版本管理：Claude 版本解析三件套（实现，头文件注释为准） ——

QString CClaudeRunner::ParseClaudeVersionOutput(const QString& a_sOut) {
    // 实测输出形态 "2.1.240 (Claude Code)"：取首个 \d+\.\d+\.\d+ 起始的版本串
    //   （允许第四段数字，如 2.1.240.1；无匹配返回空）。
    static const QRegularExpression re(
        QStringLiteral("\\d+\\.\\d+\\.\\d+(?:\\.\\d+)*"));
    const QRegularExpressionMatch m = re.match(a_sOut);
    return m.hasMatch() ? m.captured(0) : QString();
}

QString CClaudeRunner::NpmGlobalClaudePkgPath() {
    // 一次性探测缓存（独立于 Pi 侧 s_sPiNpmCliCache——不同包）；npm root -g 定位全局
    //   node_modules 根（对 npm 前缀配置免疫），拼包内 package.json 后校验存在
    //   （同款处理见 CPiAgentRunner::NpmGlobalCliPath）。
    static QString s_sClaudePkgCache;
    static bool s_bClaudePkgProbed = false;
    if (s_bClaudePkgProbed) {
        return s_sClaudePkgCache;
    }
    s_bClaudePkgProbed = true;
    // 2026.10.10 npm.cmd 定位：继承 PATH 找不到时用合并 PATH（注册表）兑底——
    //   异地新机装完环境未重启 AutoCode 时 findExecutable 只看旧 PATH。
    QString sNpm = QStandardPaths::findExecutable(QStringLiteral("npm.cmd"));
    if (sNpm.isEmpty()) {
        const QStringList aPathDirs = CAgentRunnerBase::MergedPathEnvironment()
                                          .value(QStringLiteral("PATH"))
                                          .split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& sDir : aPathDirs) {
            const QString sTry = QDir::fromNativeSeparators(sDir.trimmed())
                                 + QStringLiteral("/npm.cmd");
            if (QFileInfo::exists(sTry)) {
                sNpm = sTry;
                break;
            }
        }
    }
    if (sNpm.isEmpty()) {
        return QString();
    }
    QProcess stProc;
    stProc.setProcessEnvironment(CAgentRunnerBase::MergedPathEnvironment());
    stProc.start(sNpm, { QStringLiteral("root"), QStringLiteral("-g") });
    if (!stProc.waitForStarted(5000) || !stProc.waitForFinished(10000)) {
        return QString();
    }
    const QString sRoot = QString::fromUtf8(stProc.readAllStandardOutput()).trimmed();
    if (sRoot.isEmpty()) {
        return QString();
    }
    const QString sPkg = QDir::fromNativeSeparators(sRoot)
        + QStringLiteral("/@anthropic-ai/claude-code/package.json");
    if (!QFile::exists(sPkg)) {
        return QString();
    }
    s_sClaudePkgCache = QDir::toNativeSeparators(sPkg);
    return s_sClaudePkgCache;
}

QString CClaudeRunner::ProbeClaudeCliFallback(QString* a_psFrom) {
    if (s_fnProbeFallback != nullptr) {
        return s_fnProbeFallback(a_psFrom);
    }
    // 候选①：npm 全局真身（NpmGlobalClaudePkgPath 已缓存 npm root -g 探测，
    //   对 npm 前缀配置免疫；bin/claude.exe 是 2.x 包内真身）。
    const QString sPkgJson = NpmGlobalClaudePkgPath();
    if (!sPkgJson.isEmpty()) {
        const QString sExe = QDir::fromNativeSeparators(QFileInfo(sPkgJson).absolutePath())
                             + QStringLiteral("/bin/claude.exe");
        if (QFileInfo::exists(sExe)) {
            if (a_psFrom != nullptr) {
                *a_psFrom = QStringLiteral("npm 全局安装");
            }
            return QDir::toNativeSeparators(sExe);
        }
    }
    // 候选②：claude 2.x 原生安装器默认位置（原生安装不走 npm，PATH 注册时机不定）。
    const QString sNative = QDir::homePath() + QStringLiteral("/.local/bin/claude.exe");
    if (QFileInfo::exists(sNative)) {
        if (a_psFrom != nullptr) {
            *a_psFrom = QStringLiteral("原生安装器默认位置");
        }
        return QDir::toNativeSeparators(sNative);
    }
    // 候选③：where claude 逐行筛——.exe 直接用；.cmd shim 必须同目录 node_modules
    //   下包真身存在（防「shim 在/包不在」假阳性，dell 机实害形态），命中用真身。
    QProcess stWhere;
    stWhere.setProcessEnvironment(CAgentRunnerBase::MergedPathEnvironment());
    stWhere.start(QStringLiteral("cmd.exe"),
                  { QStringLiteral("/c"), QStringLiteral("where"), QStringLiteral("claude") });
    if (stWhere.waitForStarted(3000) && stWhere.waitForFinished(5000)
        && stWhere.exitCode() == 0) {
        const QStringList aLines = QString::fromLocal8Bit(stWhere.readAllStandardOutput())
                                       .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (QString sLine : aLines) {
            sLine = sLine.trimmed();
            if (sLine.isEmpty()) {
                continue;
            }
            if (sLine.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
                && QFileInfo::exists(sLine)) {
                if (a_psFrom != nullptr) {
                    *a_psFrom = QStringLiteral("PATH 搜索");
                }
                return QDir::toNativeSeparators(sLine);
            }
            if (sLine.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive)) {
                const QString sReal = QDir::fromNativeSeparators(QFileInfo(sLine).absolutePath())
                    + QStringLiteral("/node_modules/@anthropic-ai/claude-code/bin/claude.exe");
                if (QFileInfo::exists(sReal)) {
                    if (a_psFrom != nullptr) {
                        *a_psFrom = QStringLiteral("npm shim 校验通过");
                    }
                    return QDir::toNativeSeparators(sReal);
                }
            }
        }
    }
    return QString();
}

std::function<QString(QString*)> CClaudeRunner::s_fnProbeFallback = nullptr;

QString CClaudeRunner::ResolveClaudeVersion() {
    // 链①：cmd /c <ClaudeCmd> --version（Windows 上 claude 是 npm shim claude.cmd，
    //   必须经 cmd /c 启动；5s 启动/10s 结束）→ ParseClaudeVersionOutput。
    QProcess stProc;
    // 2026.10.10 版本探测同用合并 PATH（安装后未重启 AutoCode 也能解析出版本）。
    stProc.setProcessEnvironment(CAgentRunnerBase::MergedPathEnvironment());
    stProc.start(QStringLiteral("cmd.exe"),
                 { QStringLiteral("/c"), ClaudeCmd(), QStringLiteral("--version") });
    if (stProc.waitForStarted(5000) && stProc.waitForFinished(10000)) {
        const QString sOut = QString::fromUtf8(stProc.readAllStandardOutput())
                           + QString::fromUtf8(stProc.readAllStandardError());
        const QString sVer = ParseClaudeVersionOutput(sOut);
        if (!sVer.isEmpty()) {
            return sVer;
        }
    }
    // 链②回退：npm 全局包 package.json 的 version。
    const QString sPkg = NpmGlobalClaudePkgPath();
    if (sPkg.isEmpty()) {
        return QString();
    }
    CFrwDevice stFile(sPkg);
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

void CClaudeRunner::Reset() {
    // 清理上一轮的 ask_user MCP 配置临时文件（若存在）
    if (!m_sMcpConfigPath.isEmpty()) {
        QFile::remove(m_sMcpConfigPath);
        m_sMcpConfigPath.clear();
    }
    if (!m_sModelSettingsPath.isEmpty()) {
        QFile::remove(m_sModelSettingsPath);
        m_sModelSettingsPath.clear();
    }
    // 2026.10.11 鉴权自诊断状态复位（新一轮可再发）
    m_sDiagKey.clear();
    m_sDiagModelSrc.clear();
    m_sDiagBase.clear();
    m_sDiagModel.clear();
    m_bAuthDiagDone = false;
    m_bProbeLargeSent = false;
    // 崩溃宽限期状态复位（上一轮宽限未收尾的极端场景兜底）
    if (m_pGraceTimer != nullptr) {
        m_pGraceTimer->stop();
    }
    // 2026.08.17 停滞检测复位：定时器停 + 解除武装（新一轮 StartStage 按 ask_user 与否重新武装）。
    if (m_pStallTimer != nullptr) {
        m_pStallTimer->stop();
    }
    m_bStallWatch = false;
    m_bGraceActive = false;
    m_iGraceTicks = 0;
    m_iTranscriptSeen = 0;
    m_sTranscriptPath.clear();
    m_aStderrTail.clear();
    m_aLastArgs.clear();       // 快速失败重试状态一并复位（新一轮从零计）
    m_bResumeDropped = false;  // 2026.08.27 悬空 resume 自愈状态复位
    m_bQuietRestart  = false;
    ++m_iProcEpoch;            // 2026.08.19 进程代次 +1（result 收尾关旧进程 stdin 的守卫）
    m_sLastPrompt.clear();
    m_sLastWorkDir.clear();
    m_iFastRetry = 0;
    m_bRetryPending = false;
    m_buf.clear();
    m_bFinished = false;
    m_sLastSubtype.clear();
    m_sLastSessionId.clear();
    m_iLastContextFill = 0;
    m_iLastReqCount = 0;   // 2026.08.15 本次运行 API 请求数（assistant 消息条数）
    m_iLiveEmitIn = 0;     // 2026.08.27 实时计量已发累计（防与收尾 sigFinished 双计）
    m_iLiveEmitOut = 0;
    m_iStreamIn = 0;       // 2026.08.27 流式中消息的输入/输出（message_start/delta）
    m_iStreamOut = 0;
}

void CClaudeRunner::StartStage(const QString& a_sWorkDir,
                               const QString& a_sPrompt,
                               const QString& a_sDisallowedTools,
                               int a_iMaxTurns,
                               const QString& a_sResumeSid,
                               bool a_bFork,
                               int a_iAutocompactK,
                               bool a_bEnableAskUser) {
    // 2026.08.17 竞态修复（d7f99cac 01:29 实测）：上一进程的收尾等待必须放在 Reset() **之前**。
    //   waitForFinished 期间 Qt 会同步 emit 旧进程的 finished(旧退出码)——若此时状态已被 Reset
    //   清掉（m_bFinished=false、m_uptime 仍是旧进程寿命），旧退出信号会被当成新进程失败
    //   （elapsed 巨大 → 跳过快速重试直接 EmitFailed），而真正的新进程成了无人跟踪的孤儿
    //   （其 result 行被 m_bFinished 守卫吞掉）。先等再 Reset，旧信号撞上上一轮的 m_bFinished=true
    //   被正常吞掉，状态机干净地属于新进程。
    if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
        if (!m_pProc->waitForFinished(5000)) {
            KillProcessTree();   // 2026.09.22 整树终止：只杀 cmd 会泄漏 node 孤儿（2d9e4c0c）
            m_pProc->waitForFinished(3000);
        }
    }
    Reset();

    // 2026.08.15 续跑 sid 先行记录：进程早夭（未到 result 行）时也能定位 transcript /
    //   供上层 --resume。transcript 基线 = 当前文件大小，宽限期只认这之后新增的内容
    //   （防上一轮遗留的 max_turns_reached 旧标记被误判为本轮收敛，a0c96b4d 03:19 场景）。
    m_sLastSessionId = a_sResumeSid;
    if (!a_sResumeSid.isEmpty()) {
        const QString sPath = FindTranscriptPath();
        if (!sPath.isEmpty()) {
            m_iTranscriptSeen = QFileInfo(sPath).size();
        }
    }

    // Windows 上 claude 是 claude.cmd（npm shim），必须经 cmd /c 启动
    // --dangerously-skip-permissions：编排器全自动运行，跳过每次工具调用的权限确认
    //   （否则 Write/Edit/Bash 每步都要人工确认，全自动流程会被阻塞）。
    //   注意：bypass 模式下 --allowedTools 白名单被绕过、失效；纵深防御改由 --disallowedTools
    //   deny 黑名单实现（deny 在 bypass 下仍生效——实测：deny Bash 后 agent 报告无 Bash 工具）。
    QStringList aArgs;
    // 2026.08.15：claude 命令名取全局配置（[Agent] ClaudeCmd，默认 "claude"）。
    aArgs << QStringLiteral("/c") << ClaudeCmd()
          << QStringLiteral("-p");
    // 续跑：命中 max-turns 或编译失败后，凭上次 session_id 恢复同一会话上下文。
    //   docs：--resume 在无头 -p 模式可用；需在原工程目录起（StartStage 已设 workingDir=ProjectDir），
    //   且续跑调用仍带一份新的 --max-turns。
    if (!a_sResumeSid.isEmpty()) {
        aArgs << QStringLiteral("--resume") << a_sResumeSid;
        // --fork-session 仅在 resume 基础上有效：产生新 sid 分支，旧 sid 保持不变（ad-hoc 撤销链）。
        if (a_bFork) {
            aArgs << QStringLiteral("--fork-session");
        }
    }
    // ask_user 提问能力：开启时写临时 mcp-config 文件挂载 ask_user MCP server。
    //   deny 模式下 mcp__autocode__ask_user 不在禁用清单即默认可用，无需再追加到工具列表。
    //   临时文件而非 JSON 内联：cmd /c 会重解析引号，JSON 内大量双引号经 cmd 极易出错。
    bool bMcpReady = false;
    if (a_bEnableAskUser) {
        static int s_iSeq = 0;
        const QString sPath = QDir::tempPath() + QStringLiteral("/autocode_mcp_%1_%2.json")
            .arg(QCoreApplication::applicationPid()).arg(++s_iSeq);
        QJsonObject oEnv;
        oEnv.insert(QStringLiteral("AUTOCODE_ASK_PIPE"),
            QStringLiteral("AutoCodeMcpAsk-%1").arg(QCoreApplication::applicationPid()));
        // 2026.08.15 [Agent] AskTimeoutSec 经子进程环境变量传递（默认 600 秒）。
        oEnv.insert(QStringLiteral("AUTOCODE_ASK_TIMEOUT_SEC"),
                    QString::number(AskTimeoutSec()));
        QJsonObject oServer;
        oServer.insert(QStringLiteral("type"), QStringLiteral("stdio"));
        oServer.insert(QStringLiteral("command"), QCoreApplication::applicationFilePath());
        QJsonArray aSrvArgs;
        aSrvArgs.append(QStringLiteral("--mcp-ask-server"));
        oServer.insert(QStringLiteral("args"), aSrvArgs);
        oServer.insert(QStringLiteral("env"), oEnv);
        QJsonObject oServers;
        oServers.insert(QStringLiteral("autocode"), oServer);
        QJsonObject oCfg;
        oCfg.insert(QStringLiteral("mcpServers"), oServers);
        QFile stCfg(sPath);
        if (stCfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            stCfg.write(QJsonDocument(oCfg).toJson(QJsonDocument::Compact));
            stCfg.close();
            m_sMcpConfigPath = sPath;
            bMcpReady = true;
        }
    }

    aArgs << QStringLiteral("--dangerously-skip-permissions")
          << QStringLiteral("--output-format") << QStringLiteral("stream-json")
          // 2026.08.19 输入同样走 stream-json：prompt 以 user JSON 帧从 stdin 投递，且
          //   **不关闭写通道**——claude 收完一轮 result 后继续等下一条输入，运行中可经
          //   InjectUserTurn 追加 user 帧实现真正的轮中转向（原 -p 文本模式 + 立即关 stdin
          //   是一次性管道，轮中写入必然失败）。result 行收到后（见 HandleLine）再关 stdin，
          //   claude 收到 EOF 即干净退出，生命周期与旧模式等价。
          << QStringLiteral("--input-format") << QStringLiteral("stream-json")
          << QStringLiteral("--verbose");
    // 2026.10.10 模型注入临时 settings（dell 实害终审：用户 ~/.claude/settings.json 的
    //   env 块（BASE_URL=智谱官方+余额不足 token）在 claude 启动时顶掉进程注入的环境
    //   变量——env 注入对该形态无效。--settings 命令行级配置文件优先级高于用户
    //   settings.json，生成临时文件把模型/网关/鉴权写入——注入必生效、不动用户文件
    //   保通用性。m_sModelSettingsPath 随 Reset/析构清理（同 mcp-config 口径）。
    // 2026.10.10 修一轮滞后（目标机实害：改了网关地址 claude 仍连旧地址）：此处不能
    //   预先取 m_sModelSettingsPath——Reset() 已在本轮开头把上一轮文件删除并清空成员，
    //   本轮的新文件要等下方注入块（pUse 解析后）才生成。所以 --settings 参数改由
    //   注入块在生成/决定不注入后同步进 aArgs（见 fnSyncSettingsArg），保证本轮
    //   启动参数携带本轮配置，模型变更立即生效。
    // deny 黑名单：--dangerously-skip-permissions(bypass) 下 allow 白名单失效、deny 仍生效，
    //   故用 --disallowedTools 显式禁用各阶段不该有的工具；空串=不禁任何工具（Develop/Verify 全开）。
    if (!a_sDisallowedTools.isEmpty()) {
        aArgs << QStringLiteral("--disallowedTools") << a_sDisallowedTools;
    }
    aArgs << QStringLiteral("--max-turns") << QString::number(a_iMaxTurns);
    // 2026.08.27 流式分块：支持则开启（本轮计量随生成实时刷新；仅显示层增强）。
    if (ClaudeSupportsPartialMessages()) {
        aArgs << QStringLiteral("--include-partial-messages");
    }
    if (a_iAutocompactK > 0 && ClaudeSupportsAutocompact()) {
        // --autocompact <tokens>：上下文逼近窗口时自动压缩兜底（v2.1.221+，ad-hoc 长会话用）。
        //   2026.08.27 旧版 CLI 无此选项，探测不支持则不带（否则 unknown option 秒退）。
        aArgs << QStringLiteral("--autocompact") << QStringLiteral("%1k").arg(a_iAutocompactK);
    }
    if (bMcpReady) {
        // 挂载 ask_user MCP server：claude 需向用户提问时调 mcp__autocode__ask_user。
        // 2026-09-26 追加内置沙箱工具说明（sandbox_* 与 ask_user 同一 mcp-config 挂载，
        //   工具常驻可见；此处点名确保 GUI 测试场景模型直接联想，不靠技能清单）。
        aArgs << QStringLiteral("--mcp-config") << m_sMcpConfigPath
              << QStringLiteral("--append-system-prompt")
              << QStringLiteral("当你需要向用户确认选择、澄清需求或在多方案间做决定时，"
                                "调用 mcp__autocode__ask_user 工具向用户提问，不要自行假设用户意图。"
                                "GUI/界面测试一律用内置沙箱工具 mcp__autocode__sandbox_*："
                                "启动被测 GUI 程序优先用 sandbox_launch（exe=全路径，主进程"
                                "代启动、窗口弹出即自动收纳）；先 sandbox_windows 列窗口，"
                                "sandbox_capture 截图（PNG 路径，"
                                "用读图工具查看），sandbox_click/type/keys 消息级操控"
                                "（后台点击不抢焦点，不打扰用户），sandbox_tree 取控件树；"
                                "禁止用 powershell Start-Process / cmd start 启动被测程序"
                                "（中间 shell 秒退切断进程树，测试窗口会裸奍弹到用户桌面）；"
                                "若目标窗口不在 sandbox_windows 列表里（AutoCode 重启过/"
                                "进程早于本轮启动），先 sandbox_add <hwnd> 收进沙箱再操控；"
                                "沙箱只收纳本工程程序，系统应用（浏览器/记事本/Office 等）"
                                "不会也不允许被收纳，不要对它们 add 或操控，也不要动"
                                "用户自己打开的窗口；"
                                "禁止物理操控鼠标键盘，也禁止绕开沙箱用 python/ctypes "
                                "自制 Win32 驱动操控可见窗口。测试过程已自动录像，无需额外操作。");
    }

    m_pProc->setWorkingDirectory(a_sWorkDir);
    // 2026.10.10 --settings 参数同步器（修一轮滞后，见上方 aArgs 处注释）：注入块
    //   生成新临时文件后调它把本轮路径写进启动参数；决定不注入/写文件失败时传空
    //   路径清理残留参数（否则上一轮的旧配置文件会顶着本轮的「走 claude 自身配置」
    //   决策继续生效）。必须在 m_pProc->start 之前调（aArgs 尚未用）。
    auto fnSyncSettingsArg = [&aArgs](const QString& a_sFile) {
        const int iIdx = aArgs.indexOf(QStringLiteral("--settings"));
        if (!a_sFile.isEmpty()) {
            const QString sNative = QDir::toNativeSeparators(a_sFile);
            if (iIdx >= 0 && iIdx + 1 < aArgs.size()) {
                aArgs[iIdx + 1] = sNative;
            } else {
                aArgs << QStringLiteral("--settings") << sNative;
            }
        } else if (iIdx >= 0) {
            aArgs.removeAt(iIdx);            // "--settings"
            if (iIdx < aArgs.size()) {
                aArgs.removeAt(iIdx);        // 路径参数
            }
        }
    };
    // 2026.08.22 中文编码根治（同 CCodexRunner）：子进程全链路 UTF-8，防 GBK 乱码/挂起。
    {
        QProcessEnvironment oEnv = CAgentRunnerBase::MergedPathEnvironment();
        oEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
        oEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        oEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
        oEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
        bool bSettingsFileOk = false;   // 2026.10.10 本轮 --settings 文件就位（压制用户 settings.json env；
        //                             注入块置位，尾部覆盖检测分口径提示用）
        // 2026-10-08 共享大模型表消费（任务/助手全覆盖）：本助手实例槽覆盖优先，
        //   其次「Claude 后端跟随本表」开关 + 共享表当前条目。仅 anthropic-messages
        //   兼容条目注入（claude CLI 只说 anthropic 协议；chat/openai 条目跳过并留痕，
        //   走 claude 自身配置兜底）。每轮新进程 per-进程 env，无共享文件竞态。
        {
            const SDshModelEntry* pUse = nullptr;
            QString sSrc;
            if (HasStageModelOverride() && !m_stModelOv.sModelId.isEmpty()) {
                pUse = &m_stModelOv;
                sSrc = QStringLiteral("本助手覆盖");
            } else if (s_bFollowSharedModels && s_iSharedActive >= 0
                       && s_iSharedActive < s_aSharedModels.size()) {
                pUse = &s_aSharedModels[s_iSharedActive];
                sSrc = QStringLiteral("全局共享表");
            }
            if (pUse != nullptr) {
                const QString sApi = pUse->sApi.trimmed().toLower();
                if (sApi.isEmpty() || sApi == QLatin1String("anthropic-messages")) {
                    oEnv.insert(QStringLiteral("ANTHROPIC_BASE_URL"), pUse->sBaseURL);
                    if (!pUse->sApiKey.isEmpty()) {
                        oEnv.insert(QStringLiteral("ANTHROPIC_API_KEY"), pUse->sApiKey);
                        // 2026.10.10 双鉴权注入（dell 实害终审：API_KEY 走 x-api-key 头、
                        //   AUTH_TOKEN 走 Bearer 头——中转只认 Bearer 时 x-api-key 必 1113；
                        //   同值双注入两种认证头都通，用户无需手改 claude 配置保通用性）。
                        oEnv.insert(QStringLiteral("ANTHROPIC_AUTH_TOKEN"), pUse->sApiKey);
                    }
                    oEnv.insert(QStringLiteral("ANTHROPIC_MODEL"), pUse->sModelId);
                    // 2026.10.10 辅助调用同源（本机 ~/.claude/settings.json 全套 env 先例：
                    //   claude 的后台小任务/子代理按 sonnet/haiku/opus 档位名取模型，只注入
                    //   ANTHROPIC_MODEL 时它们仍打官方模型名——网关不识别/计费不覆盖
                    //   （dell 实害：主请求 OK、辅助请求报 1113）。三档位全部注入同源模型。
                    oEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"), pUse->sModelId);
                    oEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_HAIKU_MODEL"), pUse->sModelId);
                    oEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_OPUS_MODEL"), pUse->sModelId);
                    // 2026.10.10 --settings 临时文件（env 注入会被用户 settings.json
                    //   顶掉，见 aArgs 处注释）：env 块同源写入，命令行级优先必生效。
                    //   写成后立即同步进本轮启动参数（fnSyncSettingsArg，修上一轮仅
                    //   在 aArgs 组装时预取、而成员已被 Reset 清空导致永不携带的滞后）。
                    {
                        static int s_iSeq = 0;
                        m_sModelSettingsPath = QDir::tempPath()
                            + QStringLiteral("/autocode_claude_model_%1_%2.json")
                                  .arg(QCoreApplication::applicationPid()).arg(++s_iSeq);
                        QJsonObject oSetEnv;
                        oSetEnv.insert(QStringLiteral("ANTHROPIC_BASE_URL"), pUse->sBaseURL);
                        if (!pUse->sApiKey.isEmpty()) {
                            oSetEnv.insert(QStringLiteral("ANTHROPIC_API_KEY"), pUse->sApiKey);
                            oSetEnv.insert(QStringLiteral("ANTHROPIC_AUTH_TOKEN"), pUse->sApiKey);
                        }
                        oSetEnv.insert(QStringLiteral("ANTHROPIC_MODEL"), pUse->sModelId);
                        oSetEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"), pUse->sModelId);
                        oSetEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_HAIKU_MODEL"), pUse->sModelId);
                        oSetEnv.insert(QStringLiteral("ANTHROPIC_DEFAULT_OPUS_MODEL"), pUse->sModelId);
                        QJsonObject oSet;
                        oSet.insert(QStringLiteral("env"), oSetEnv);
                        QFile fSet(m_sModelSettingsPath);
                        if (fSet.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            fSet.write(QJsonDocument(oSet).toJson(QJsonDocument::Compact));
                            fSet.close();
                            bSettingsFileOk = true;
                            fnSyncSettingsArg(m_sModelSettingsPath);
                        } else {
                            m_sModelSettingsPath.clear();   // 写不了退回纯 env 注入
                            fnSyncSettingsArg(QString());   // 清残留 --settings 参数
                        }
                    }
                    emit sigInfo(QStringLiteral("[claude-model] %1%4 生效：%2（%3）")
                        .arg(sSrc, pUse->sModelId, pUse->sBaseURL,
                             sSrc == QStringLiteral("本助手覆盖")
                                 ? QStringLiteral("（对话级覆盖——设置页改全局当前模型"
                                                   "不影响本助手；右键上下文条→选择模型→"
                                                   "跟随全局设置 可清除）")
                                 : QString()));
                    // 2026.10.11 注入 key 快照 + 格式预检（目标机 401 Invalid API key
                    //   format 实害：同配置在其它机器正常，问题必在 key 值形态或本机
                    //   claude 用户级凭据——当场暴露，不等失败后猜）：claude/网关
                    //   仅认字母数字加减写下划线；带 Bearer/sk- 前缀、引号、空格等
                    //   必被拒（401 Invalid API key format，实测网关双文案均为此形态）。
                    m_sDiagKey = pUse->sApiKey;
                    m_sDiagModelSrc = sSrc;
                    m_sDiagBase = pUse->sBaseURL;   // 2026.10.11 鉴权/连接失败诊断的网关直探锚点
                    m_sDiagModel = pUse->sModelId;
                    if (!pUse->sApiKey.isEmpty()) {
                        static const QRegularExpression reClean(
                            QStringLiteral("^[A-Za-z0-9_-]+$"));
                        if (!reClean.match(pUse->sApiKey).hasMatch()) {
                            emit sigInfo(QStringLiteral(
                                "[claude-model] ⚠ 注入的 Key 含非法字符（仅允许字母/数字/"
                                "-/_）——网关必拒（401 Invalid API key format）。常见"
                                "原因：Key 值误带 Bearer/sk- 前缀、引号或空格；请到"
                                "「设置 → 大模型」检查条目「%1」的 Key")
                                .arg(pUse->sModelId));
                        }
                    }
                } else {
                    fnSyncSettingsArg(QString());   // 本轮不注入：清残留 --settings 参数
                    emit sigInfo(QStringLiteral(
                        "[claude-model] %1 条目「%2」API 类型为 %3（非 anthropic-messages），"
                        "本轮跳过注入、走 claude 自身配置")
                        .arg(sSrc, pUse->sModelId, pUse->sApi));
                }
            } else {
                fnSyncSettingsArg(QString());   // 本轮不注入：清残留 --settings 参数
                // 2026.10.10 未注入可见性（dell 实害：跟随关/表空时静默走 claude 自身
                //   配置，用户误以为用了模型表——429 余额不足排查半天）。
                emit sigInfo(QStringLiteral(
                    "[claude-model] 本轮未注入模型配置（无实例覆盖且跟随开关关/共享表无"
                    "激活条目），走 claude 自身配置（~/.claude/settings.json）——"
                    "若模型/网关不对请检查「设置 → 大模型表」"));
            }
        }
        // 2026-10-08 Claude 专属参数（与共享表无关，设了就注入）。
        if (s_iMaxThinkTok > 0) {
            oEnv.insert(QStringLiteral("MAX_THINKING_TOKENS"), QString::number(s_iMaxThinkTok));
        }
        if (!s_sSmallFastModel.isEmpty()) {
            oEnv.insert(QStringLiteral("ANTHROPIC_SMALL_FAST_MODEL"), s_sSmallFastModel);
        }
        m_pProc->setProcessEnvironment(oEnv);
        // 2026.10.10 自身配置覆盖检测（dell 实害推断：~/.claude/settings.json 的 env 块
        //   在 claude 启动时会合并/覆盖进程环境——里面有智谱的 ANTHROPIC_BASE_URL/KEY
        //   时，AutoCode 注入的模型表 env 被顶掉，请求仍打旧端点报 1113/余额不足）。
        //   读 settings.json 的 env 对象，列出 ANTHROPIC_* 覆盖键提醒用户；文件不存在/
        //   解析失败/无覆盖键则静默。
        {
            const QString sSettings = QDir::homePath()
                + QStringLiteral("/.claude/settings.json");
            QFile fSettings(sSettings);
            if (fSettings.open(QIODevice::ReadOnly)) {
                QJsonParseError stErr;
                const QJsonDocument oDoc = QJsonDocument::fromJson(
                    fSettings.readAll(), &stErr);
                fSettings.close();
                if (stErr.error == QJsonParseError::NoError && oDoc.isObject()) {
                    const QJsonObject oEnvCfg = oDoc.object()
                                                    .value(QStringLiteral("env"))
                                                    .toObject();
                    QStringList aOverride;
                    QString sUrl, sModel;
                    for (auto it = oEnvCfg.begin(); it != oEnvCfg.end(); ++it) {
                        if (!it.key().startsWith(QStringLiteral("ANTHROPIC_"))) {
                            continue;
                        }
                        aOverride << it.key();
                        // 2026.10.10 关键值直显（dell 排障：用户翻 settings.json 才知道
                        //   claude 实际用的端点/模型）——BASE_URL/模型全文，key/token 打码。
                        const QString sVal = it.value().toString();
                        if (it.key() == QStringLiteral("ANTHROPIC_BASE_URL")) {
                            sUrl = sVal;
                        } else if (it.key() == QStringLiteral("ANTHROPIC_MODEL")) {
                            sModel = sVal;
                        }
                    }
                    if (!aOverride.isEmpty()) {
                        QString sDetail = aOverride.join(QStringLiteral(", "));
                        if (!sUrl.isEmpty()) {
                            sDetail += QStringLiteral("；BASE_URL=%1").arg(sUrl);
                        }
                        if (!sModel.isEmpty()) {
                            sDetail += QStringLiteral("；MODEL=%1").arg(sModel);
                        } else {
                            sDetail += QStringLiteral("；未配 ANTHROPIC_MODEL（主请求走 claude "
                                                      "内置默认模型名）");
                        }
                        // 2026.10.10 分口径提示（目标机实害：--settings 文件已压制用户
                        //   settings.json env 时仍报「会顶掉」，用户误以为 claude 在用
                        //   旧地址）：本轮 --settings 文件就位 = 覆盖键被压制（AutoCode
                        //   注入优先生效，提示改为可见性说明）；未就位（未注入/写文件
                        //   失败）= 覆盖键真实生效，保留「会顶掉」警示。
                        if (bSettingsFileOk) {
                            emit sigInfo(QStringLiteral(
                                "[claude-model] 提示：claude 自身配置（%1）的 env 块含 %2 个 "
                                "ANTHROPIC_* 键（%3）；本轮已由 --settings 命令行级配置压制，"
                                "实际用 AutoCode 注入的模型/网关，该文件不生效")
                                .arg(QDir::toNativeSeparators(sSettings))
                                .arg(aOverride.size())
                                .arg(sDetail.left(400)));
                        } else {
                            emit sigInfo(QStringLiteral(
                                "[claude-model] 注意：claude 自身配置（%1）的 env 块含 %2 个 "
                                "ANTHROPIC_* 覆盖键，会在启动时顶掉本轮注入的模型/网关——"
                                "%3。若模型/端点不符请清理或同步该文件")
                                .arg(QDir::toNativeSeparators(sSettings))
                                .arg(aOverride.size())
                                .arg(sDetail.left(400)));
                        }
                    }
                }
            }
        }
    }
    // 2026.10.10 预检：ClaudeCmd 为裸命令名时，用与子进程相同的合并 PATH 验证可见性；
    //   不可见直接给可操作报错（而不是三连快速失败重试后只甩裸退出码——异地新机
    //   「装完环境发提示词仍失败」时用户至少能看到确切原因与处置路径）。
    //   绝对/相对路径配置不预检（用户自管路径，行为与旧版一致）。
    const QString sCmdPreflight = ClaudeCmd();
    QString sEffective = sCmdPreflight;
    QString sProbeFrom;
    bool bMissing = false;
    bool bWhereVisible = false;   // 裸名在 PATH 可见（区分「不可见」与「可见但死 shim」报错文案）
    if (!sCmdPreflight.contains(QLatin1Char('/'))
        && !sCmdPreflight.contains(QLatin1Char('\\'))) {
        // 2026.10.10 裸名双重预检（dell 实害：npm prefix 变更后 %APPDATA%\\npm 遗留
        //   死 shim——where 命中≠可用，启动即「系统找不到指定的路径」三连重试）：
        //   ①where 可见；②试运行 --version（rc==0 才算真可用，死 shim 秒败被拦）。
        //   任一不达 → missing → 走兕底探测链。
        QProcess stWhere;
        stWhere.setProcessEnvironment(m_pProc->processEnvironment());
        stWhere.start(QStringLiteral("cmd.exe"),
                      { QStringLiteral("/c"), QStringLiteral("where"), sCmdPreflight });
        const bool bVisible = stWhere.waitForStarted(3000) && stWhere.waitForFinished(5000)
                              && stWhere.exitCode() == 0;
        bMissing = !bVisible;
        bWhereVisible = bVisible;
        // 进程内验证缓存：同名验证通过过即不再重复（每轮 StartStage 免多拉一次
        //   CLI，1~2 秒/轮的可感知代价只付一次；命令替换/修复后仍能跑则验证仍真）。
        static QString s_sPreflightVerified;
        if (bVisible && sCmdPreflight != s_sPreflightVerified) {
            QProcess stVer;
            stVer.setProcessEnvironment(m_pProc->processEnvironment());
            stVer.start(QStringLiteral("cmd.exe"),
                        { QStringLiteral("/c"), sCmdPreflight, QStringLiteral("--version") });
            bMissing = !(stVer.waitForStarted(3000) && stVer.waitForFinished(15000)
                         && stVer.exitCode() == 0);
            if (!bMissing) {
                s_sPreflightVerified = sCmdPreflight;
            }
        }
    } else {
        // 2026.10.10 绝对/相对路径预检（异地部署机 dell 实害）：目录不存在时 cmd
        //   只报模糊的「系统找不到指定的路径。」退出码 1（对照实验：文件不存在才是
        //   「不是内部或外部命令」），真因埋没。启动前 QFileInfo::exists 验证。
        bMissing = !QFileInfo::exists(sCmdPreflight);
    }
    if (bMissing) {
        // 2026.10.10 兕底探测（用户需求：确保找到正确的 claude code cli）：配置失效
        //   时自动定位本机可用安装（npm 全局真身→原生安装器→PATH shim 真身校验），
        //   命中则本轮替换生效并事件行告知（不回写 config，固化由用户自行决定）；
        //   全落空才 EmitFailed（按裸名/路径分场景给精确指引）。
        sEffective = ProbeClaudeCliFallback(&sProbeFrom);
    }
    if (bMissing && sEffective.isEmpty()) {
        // 2026.10.10 环境自诊断（dell 实害：异地机拿不到 cmd 现场，两轮日志往返仍缺
        //   npm/prefix 状态）：报错时自动收集 node/npm/prefix/npm root -g/where 命令
        //   输出附在错误尾部——一次报错自带全部定位现场。
        QString sDiag;
        {
            QProcess stDiag;
            auto fnRun = [&stDiag, this](const QString& a_sCmd, const QStringList& a_aArgs) {
                stDiag.setProcessEnvironment(m_pProc->processEnvironment());
                stDiag.start(a_sCmd, a_aArgs);
                if (!stDiag.waitForStarted(2000) || !stDiag.waitForFinished(8000)) {
                    return QStringLiteral("(超时/失败)");
                }
                return QString::fromLocal8Bit(
                    stDiag.readAllStandardOutput() + stDiag.readAllStandardError())
                    .trimmed()
                    .replace(QLatin1Char('\n'), QStringLiteral(" ; "));
            };
            sDiag += QStringLiteral("\n——环境诊断：")
                + QStringLiteral("\nnode -v：") + fnRun(QStringLiteral("node.exe"), { QStringLiteral("-v") })
                + QStringLiteral("\nnpm -v：") + fnRun(QStringLiteral("cmd.exe"),
                    { QStringLiteral("/c"), QStringLiteral("npm"), QStringLiteral("-v") })
                + QStringLiteral("\nnpm prefix：") + fnRun(QStringLiteral("cmd.exe"),
                    { QStringLiteral("/c"), QStringLiteral("npm"), QStringLiteral("config"), QStringLiteral("get"), QStringLiteral("prefix") })
                + QStringLiteral("\nnpm root -g：") + fnRun(QStringLiteral("cmd.exe"),
                    { QStringLiteral("/c"), QStringLiteral("npm"), QStringLiteral("root"), QStringLiteral("-g") })
                + QStringLiteral("\nwhere claude：") + fnRun(QStringLiteral("cmd.exe"),
                    { QStringLiteral("/c"), QStringLiteral("where"), QStringLiteral("claude") })
                + QStringLiteral("\nnpm config list：") + fnRun(QStringLiteral("cmd.exe"),
                    { QStringLiteral("/c"), QStringLiteral("npm"), QStringLiteral("config"),
                      QStringLiteral("list") }).left(600)
                + QStringLiteral("\n工程目录 .npmrc：")
                + [a_sWorkDir]() {
                      const QString sN = QDir::fromNativeSeparators(a_sWorkDir)
                                         + QStringLiteral("/.npmrc");
                      QFile f(sN);
                      if (!f.exists()) { return QStringLiteral("（无）"); }
                      if (!f.open(QIODevice::ReadOnly)) { return sN + QStringLiteral("（存在但读不了）"); }
                      const QString sC = QString::fromUtf8(f.readAll()).trimmed();
                      f.close();
                      return sN + QStringLiteral(" → ")
                             + sC.left(200).replace(QLatin1Char('\n'), QStringLiteral(" ; "));
                  }();
        }
        if (!sCmdPreflight.contains(QLatin1Char('/'))
            && !sCmdPreflight.contains(QLatin1Char('\\'))) {
            if (bWhereVisible) {
                // 死 shim 形态（dell 实害）：where 命中但 --version 验败，兕底探测也空。
                EmitFailed(QStringLiteral(
                    "%1 命令在 PATH 中存在但无法执行（死 shim：指向的包已不在，常见于"
                    "npm prefix 变更后 %APPDATA%\\npm 目录遗留），且未探测到其他可用安装。\n"
                    "处理：管理员 cmd 运行 del \"%APPDATA%\\npm\\%1*\" 后重跑环境安装，"
                    "或重装：npm install -g @anthropic-ai/claude-code。")
                    .arg(sCmdPreflight) + sDiag);
            } else {
                EmitFailed(QStringLiteral(
                    "未在 PATH 中找到 %1 命令（已合并注册表 Machine/User PATH 仍未见，\n"
                    "npm 全局/原生安装位置也未探测到可用安装）。\n"
                    "常见原因：①Claude Code 未安装或安装未完成；②安装在非标准位置。\n"
                    "处理：cmd 窗口运行「%1 --version」验证；若能找到，把该绝对路径填到"
                    "「设置 → Agent」的 Claude 命令后重试；找不到则重跑环境安装。")
                    .arg(sCmdPreflight) + sDiag);
            }
        } else {
            EmitFailed(QStringLiteral(
                "配置的 Claude 命令路径不存在：%1\n"
                "（npm 全局/原生安装位置也未探测到可用安装。）\n"
                "常见原因：①该机器上 Claude Code 未安装在此位置（npm 全局装在 npm 目录下"
                "node_modules\\@anthropic-ai\\claude-code，原生安装器装在"
                "%USERPROFILE%\\.local\\bin\\claude.exe）；②路径拼写错误。\n"
                "处理：cmd 窗口运行「where claude」找真实位置，填到「设置 → Agent」的"
                "Claude 命令；或改填裸名 claude（自动按合并 PATH 查找）。")
                .arg(sCmdPreflight) + sDiag);
        }
        return;
    }
    if (bMissing) {
        emit sigInfo(QStringLiteral(
            "[system] 配置的 Claude 命令（%1）不可用，已自动探测到可用安装：%2（来源：%3）。"
            "本轮生效；如需固化请到「设置 → Agent」修改 Claude 命令。")
            .arg(sCmdPreflight, sEffective, sProbeFrom));
        if (aArgs.size() > 1) {
            aArgs[1] = sEffective;   // aArgs 形如 {"/c", <cmd>, "-p", ...}
        }
    }
    // 上一进程收尾等待已移至函数开头（Reset() 之前，2026.08.17 竞态修复），此处直接启动。
    m_aLastArgs    = aArgs;      // 快速失败重试：保存本次启动参数
    m_sLastPrompt  = a_sPrompt;
    m_sLastWorkDir = a_sWorkDir;
    m_pProc->start(QStringLiteral("cmd.exe"), aArgs);

    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("无法启动 claude 进程，请确认 claude 已在 PATH 中。"));
        return;
    }
    m_uptime.restart();
    EnsureJobObject();   // 2026.09.22 cmd 挂入 Job（KILL_ON_JOB_CLOSE）：子孙全进树，可整杀、退出无孤儿
    // 2026.08.17 停滞检测武装：非 ask_user 调用才启用（ask_user 等用户作答可静默很久，
    //   误杀会把提问中的会话拦腰截断）。收到任何输出即重置计时（见 slotOnStdOut/StdErr）。
    m_bStallWatch = !a_bEnableAskUser;
    m_actClock.restart();
    if (m_bStallWatch && m_pStallTimer != nullptr) {
        m_pStallTimer->start();
    }

    // prompt 以 stream-json user 帧投递（避免命令行长度限制）；stdin 保持打开——
    //   运行中可继续追加 user 帧（InjectUserTurn），result 行收尾时再关（见 HandleLine）。
    WriteUserFrame(a_sPrompt);
}

void CClaudeRunner::WriteUserFrame(const QString& a_sText) {
    // {"type":"user","message":{"role":"user","content":[{"type":"text","text":...}]}}
    //   一行一帧（stream-json 输入协议）；JSON 编码内嵌任意文本（引号/换行均安全）。
    if (m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return;
    }
    QJsonObject oText;
    oText.insert(QStringLiteral("type"), QStringLiteral("text"));
    oText.insert(QStringLiteral("text"), a_sText);
    QJsonArray aContent;
    aContent.append(oText);
    QJsonObject oMsg;
    oMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    oMsg.insert(QStringLiteral("content"), aContent);
    QJsonObject oFrame;
    oFrame.insert(QStringLiteral("type"), QStringLiteral("user"));
    oFrame.insert(QStringLiteral("message"), oMsg);
    const QByteArray bufFrame = QJsonDocument(oFrame).toJson(QJsonDocument::Compact)
                              + '\n';
    m_pProc->write(bufFrame);
    m_pProc->waitForBytesWritten(100);   // 尽快压出管道（非阻塞上限 100ms）
}

void CClaudeRunner::RelaunchStage() {
    // 快速失败自动重试（延迟 5s 后原参数重拉）：期间被 Stop/新一轮 StartStage 则作废。
    m_bRetryPending = false;
    if (m_bFinished || m_bGraceActive || m_aLastArgs.isEmpty()) {
        return;
    }
    if (m_bResumeDropped && m_iFastRetry == 0) {
        emit sigInfo(QStringLiteral("↻ 新建会话重发本轮（原会话在本机不存在）…"));
    } else {
        emit sigInfo(QStringLiteral("↻ 自动重试 claude 调用（第 %1/2 次）…").arg(m_iFastRetry));
    }
    m_buf.clear();
    m_aStderrTail.clear();   // 2026.09.19 重拉清旧进程 stderr 尾：防下一轮失败消息拼上已死进程的
                             //   过期报错（PixelPorter 实害：新会话进程的失败带旧 sid 的 No conversation found）
    m_sLastSubtype.clear();
    m_pProc->setWorkingDirectory(m_sLastWorkDir);
    m_pProc->start(QStringLiteral("cmd.exe"), m_aLastArgs);
    if (!m_pProc->waitForStarted(10000)) {
        EmitFailed(QStringLiteral("重试亦无法启动 claude 进程，请确认 claude 已在 PATH 中。"));
        return;
    }
    m_uptime.restart();
    // 2026.08.17 重拉同样重置停滞计时（延续本轮武装状态；定时器上一轮已被 Reset 停掉则补启）。
    m_actClock.restart();
    if (m_bStallWatch && m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    // 2026.08.19 与 StartStage 同口径：user 帧 + 保持 stdin 打开（见 WriteUserFrame）。
    WriteUserFrame(m_sLastPrompt);
}

void CClaudeRunner::Stop() {
    // 2026.08.15 主动停止全程静默：先置终态再 kill——kill 触发的 Crashed/finished 信号一律吞掉，
    //   不发 sigFinished（旧路径会把用户停止误报成「claude 进程崩溃 + 阶段失败」）；
    //   宽限期一并取消。停止后的任务终态由上层（StopTask）直接落 Stopped。
    m_bFinished = true;
    m_bRetryPending = false;   // 待触发的重试 singleShot 到点后会被 m_bFinished 守卫拦下
    if (m_pGraceTimer != nullptr && m_pGraceTimer->isActive()) {
        m_pGraceTimer->stop();
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {   // 2026.08.17 停止时停巡检
        m_pStallTimer->stop();
    }
    m_bGraceActive = false;
    if (m_pProc && m_pProc->state() != QProcess::NotRunning) {
        KillProcessTree();   // 2026.09.22 整树终止：cmd+node 一并终结，停止无孤儿
        m_pProc->waitForFinished(3000);
    }
}

bool CClaudeRunner::IsRunning() const {
    return m_pProc && m_pProc->state() != QProcess::NotRunning;
}

bool CClaudeRunner::InjectUserTurn(const QString& a_sText) {
    // 2026.08.19 FR-3 真正落地：--input-format stream-json 模式下 stdin 全程打开，
    //   运行中向其追加一帧 user message = claude 在当前轮内收到新的用户输入
    //   （转向 / 补充指令，非等本轮结束）。仅在本轮尚未收 result（m_bFinished=false）
    //   时接受——result 已到说明本轮结束，此时返回 false 交上层排队走下一轮。
    if (a_sText.isEmpty() || m_bFinished) {
        return false;
    }
    if (m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return false;   // 未运行：安全 no-op
    }
    WriteUserFrame(a_sText);
    return true;
}

void CClaudeRunner::slotOnStdOut() {
    m_actClock.restart();   // 2026.08.17 收到输出 = 进程活着，停滞计时清零
    // 2026.09.23 代次快照（双 result 守卫配套）：本批字节全部属于快照时刻的进程代次。
    //   批内 result#1 可能同步触发上层续跑（StartStage→Reset 代次+1），批内残留的旧
    //   进程 result 行凭此识破（逐行传入 HandleLine 比对）。
    const int iEpoch = m_iProcEpoch;
    m_buf += m_pProc->readAllStandardOutput();

    int iIdx = 0;
    while ((iIdx = m_buf.indexOf('\n')) >= 0) {
        QByteArray sLine = m_buf.left(iIdx);
        m_buf = m_buf.mid(iIdx + 1);
        sLine = sLine.trimmed();
        if (!sLine.isEmpty()) {
            HandleLine(sLine, iEpoch);
        }
    }
}

void CClaudeRunner::slotOnStdErr() {
    // 2026.08.15 stderr 捕获：逐行上抛落 run.log（[stderr] 前缀），并保留尾部若干条
    //   （EmitFailed 时附在失败消息里——此前 exit 1 真因只打 stderr，全部丢失）。
    m_actClock.restart();   // 2026.08.17 stderr 字节同样算活跃（重试警告/进度条等）
    const QByteArray bufData = m_pProc->readAllStandardError();
    const QList<QByteArray> aLines = bufData.split('\n');
    for (const QByteArray& szRaw : aLines) {
        // 2026.08.27 中文 Windows 上 npm shim（claude.cmd）的报错按 GBK 输出，先 UTF-8 解、
        //   出现替换符则回退本地 8 位编码（GBK）重解，消除 stderr 尾部乱码。
        QString sLine = QString::fromUtf8(szRaw);
        if (sLine.contains(QChar(0xFFFD))) {
            sLine = QString::fromLocal8Bit(szRaw);
        }
        sLine = sLine.trimmed();
        if (sLine.isEmpty()) {
            continue;
        }
        // 2026.09.20 良性诊断过滤（MPlatform_QT 实害：GLM 网关别名下每轮必报
        //   「[claude-code:unrecognized_model] {"model":"glm-5.3[1m]",...}」——
        //   claude CLI 对非 Anthropic 名册的模型别名恒打此提示，请求照常成功；
        //   已实测去掉 [1M] 后缀/换任意别名均同样提示，属网关接入的正常噪声。
        //   过滤后不入 stderr 尾（防污染失败消息）也不上屏。其余 stderr 照旧全量透传。
        if (sLine.startsWith(QStringLiteral("[claude-code:unrecognized_model]"))) {
            continue;
        }
        m_aStderrTail.append(sLine);
        if (m_aStderrTail.size() > 15) {
            m_aStderrTail.removeFirst();
        }
        emit sigInfo(QStringLiteral("[stderr] ") + sLine);
    }
}

void CClaudeRunner::HandleLine(const QByteArray& a_sLine, int a_iEpoch) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        // 非 JSON 行：作为原始信息上抛（便于排查）
        emit sigInfo(QString::fromUtf8(a_sLine));
        return;
    }

    const QJsonObject oRoot = stDoc.object();
    const QString sType = oRoot.value(QStringLiteral("type")).toString();

    if (sType == QStringLiteral("system")) {
        const QString sSub = oRoot.value(QStringLiteral("subtype")).toString();
        // thinking_tokens：思考期间的临时 token 计数（约 2 条/秒），无复盘价值且会被
        // 上层逐条落 run.log / 上屏（3 视角评审并行时曾把 UI 刷到完全卡死）——源头丢弃。
        if (sSub == QStringLiteral("thinking_tokens")) {
            return;
        }
        const QString sSid = oRoot.value(QStringLiteral("session_id")).toString();
        // 2026.08.15 sid 运行中即捕获（init 行最早携带）：进程早夭无 result 时，
        //   崩溃宽限期仍能定位 transcript 判真因，上层也能拿到 sid 续跑。
        if (!sSid.isEmpty() && m_sLastSessionId.isEmpty()) {
            m_sLastSessionId = sSid;
        }
        // 2026.09.23 model 运行中即捕获（init 行携带，CLI 2.1.278 实测）：result 已无顶层
        //   model 字段，不在此补取则整个运行期的窗口口径（ContextWindowForModel，供
        //   message_delta/assistant 帧脏值守卫）只能走空模型 200K 兜底——GLM（128K）
        //   等小窗口模型下脏值上限被放宽到 2×200K。
        const QString sInitModel = oRoot.value(QStringLiteral("model")).toString();
        if (!sInitModel.isEmpty()) {
            m_sLastModel = sInitModel;
        }
        emit sigInfo(QStringLiteral("[system/%1] session=%2").arg(sSub, sSid));
        return;
    }

    if (sType == QStringLiteral("stream_event")) {
        // 2026.08.27 流式 usage（--include-partial-messages）：message_start 带本消息输入
        //   （input+缓存三项），message_delta 的 usage.output_tokens 为本消息累计输出——
        //   据此实时刷新「本轮 ↑↓」显示（sigLivePartial 绝对值 = 已结算 + 流式中，不入账）。
        const QJsonObject oEv = oRoot.value(QStringLiteral("event")).toObject();
        const QString sEv = oEv.value(QStringLiteral("type")).toString();
        if (sEv == QStringLiteral("message_start")) {
            const QJsonObject oU = oEv.value(QStringLiteral("message")).toObject()
                                     .value(QStringLiteral("usage")).toObject();
            m_iStreamIn = oU.value(QStringLiteral("input_tokens")).toInteger()
                + oU.value(QStringLiteral("cache_creation_input_tokens")).toInteger()
                + oU.value(QStringLiteral("cache_read_input_tokens")).toInteger();
        } else if (sEv == QStringLiteral("message_delta")) {
            const QJsonObject oU = oEv.value(QStringLiteral("usage")).toObject();
            const qint64 iOut = oU.value(QStringLiteral("output_tokens")).toInteger();
            m_iStreamOut = iOut;
            // 2026.09.23 GLM 网关上下文兜底：内网 GLM 中转（Anthropic 兼容层）实测
            //   message_start/assistant 帧的 usage 恒全 0，真实 input/cache 三项只在末条
            //   message_delta 携带——只认 assistant 帧则上下文占用全程 0。此处一并读取三项
            //   刷新占用，守卫与 assistant 帧完全一致（>0、值变化、≤2×窗口）：真 Anthropic
            //   下 message_delta 无 input 字段（三项和 0 被拦）或与 assistant 帧同值
            //   （变化守卫拦下），均无副作用；旧 CLI 无流式事件时退化为原行为。
            const qint64 iFill = oU.value(QStringLiteral("input_tokens")).toInteger()
                + oU.value(QStringLiteral("cache_creation_input_tokens")).toInteger()
                + oU.value(QStringLiteral("cache_read_input_tokens")).toInteger();
            const qint64 iWinCap = 2 * ContextWindowForModel(m_sLastModel);
            if (iFill > 0 && iFill != m_iLastContextFill && iFill <= iWinCap) {
                m_iLastContextFill = iFill;
                emit sigContextFill(iFill);
            }
            emit sigLivePartial(m_iLiveEmitIn + m_iStreamIn, m_iLiveEmitOut + m_iStreamOut);
        } else if (sEv == QStringLiteral("message_stop")) {
            // 本消息收口：交给 assistant 完整帧的 sigUsageDelta 入账，流式部分归零。
            m_iStreamIn = 0;
            m_iStreamOut = 0;
        }
        return;
    }

    if (sType == QStringLiteral("assistant")) {
        const QJsonObject oMsg = oRoot.value(QStringLiteral("message")).toObject();
        // 2026.08.15 请求数统计：每条 assistant 消息 = 一次 API 调用（含缓存命中的调用）。
        ++m_iLastReqCount;
        // 运行中上下文占用刷新：每条 assistant 消息自带 usage（input+cache 三项，口径与
        // result 一致），逐条更新并上抛——UI 进度条无需等到 result 才显示（此前全程 0）。
        {
            const QJsonObject oUsage = oMsg.value(QStringLiteral("usage")).toObject();
            const qint64 iFill = oUsage.value(QStringLiteral("input_tokens")).toInteger()
                + oUsage.value(QStringLiteral("cache_creation_input_tokens")).toInteger()
                + oUsage.value(QStringLiteral("cache_read_input_tokens")).toInteger();
            // 2026.08.15 脏数据防护：GLM 中转偶发返回异常巨大的 usage（实测单条 1.4M/存档出现过
            //   1.9M，远超窗口）。超过窗口 2 倍视为脏值丢弃（保留上一条真实值），防假占用进面板
            //   并被持久化到 assistant.json 反复还原（7ce53172 实测「上下文 1.9M」）。
            const qint64 iWinCap = 2 * ContextWindowForModel(m_sLastModel);
            if (iFill > 0 && iFill != m_iLastContextFill && iFill <= iWinCap) {
                m_iLastContextFill = iFill;
                emit sigContextFill(iFill);
            }
            // 2026.08.27 逐消息实时计量上抛（本轮 ↑↓ 显示）：本条 API 调用的
            //   输入（含 cache 三项）+ 输出。收尾 sigFinished 据已发累计报 0 防双计。
            const qint64 iMsgOut = oUsage.value(QStringLiteral("output_tokens")).toInteger();
            if (iFill > 0 || iMsgOut > 0) {
                m_iLiveEmitIn += iFill;
                m_iLiveEmitOut += iMsgOut;
                emit sigUsageDelta(iFill, iMsgOut);
            }
        }
        const QJsonArray aContent = oMsg.value(QStringLiteral("content")).toArray();
        for (const QJsonValue& v : aContent) {
            const QJsonObject oItem = v.toObject();
            const QString sKind = oItem.value(QStringLiteral("type")).toString();
            if (sKind == QStringLiteral("text")) {
                const QString sTxt = oItem.value(QStringLiteral("text")).toString();
                emit sigAssistantText(sTxt);
                // 2026.10.11 鉴权失败自诊断：claude 输出鉴权错误（401/Invalid key/
                //   authenticate）时，一次性追发注入 key 指纹 + 本机凭据源清单
                //   （目标机实害：同配置其它机器正常，差异必在本机 claude 用户级状态）。
                if (!m_bAuthDiagDone
                    && (sTxt.contains(QStringLiteral("Failed to authenticate"),
                                      Qt::CaseInsensitive)
                        || sTxt.contains(QStringLiteral("Invalid API key"),
                                         Qt::CaseInsensitive)
                        || sTxt.contains(QStringLiteral("API Error: 401"))
                        || sTxt.contains(QStringLiteral("ECONNRESET"))
                        || sTxt.contains(QStringLiteral("Connection dropped"))
                        || sTxt.contains(QStringLiteral("Connection error"))
                        || sTxt.contains(QStringLiteral("fetch failed")))) {
                    m_bAuthDiagDone = true;
                    QString sFp = QStringLiteral("（未注入）");
                    if (!m_sDiagKey.isEmpty()) {
                        const int iK = m_sDiagKey.size();
                        const QString sHead = m_sDiagKey.left(4);
                        const QString sTail = m_sDiagKey.right(4);
                        static const QRegularExpression reClean(
                            QStringLiteral("^[A-Za-z0-9_-]+$"));
                        sFp = QStringLiteral("长度 %1，%2…%3，%4")
                                  .arg(iK).arg(sHead, sTail)
                                  .arg(reClean.match(m_sDiagKey).hasMatch()
                                           ? QStringLiteral("格式合法")
                                           : QStringLiteral("⚠ 含非法字符（仅允许字母/数字/-/_）"));
                    }
                    QStringList aSrc;
                    {   // ⓪ apiKeyHelper（优先级高于 env 注入；Windows 下脚本输出带 CRLF/
                        //   额外字符会变成非法 key → 401 Invalid API key format，dell 盲区）
                        const QString sS0 = QDir::homePath()
                            + QStringLiteral("/.claude/settings.json");
                        QFile fS0(sS0);
                        if (fS0.open(QIODevice::ReadOnly)) {
                            const QJsonDocument oD0 = QJsonDocument::fromJson(fS0.readAll());
                            fS0.close();
                            const QString sHelper = oD0.object()
                                .value(QStringLiteral("apiKeyHelper")).toString();
                            if (!sHelper.isEmpty()) {
                                aSrc << QStringLiteral("⚠ ~/.claude/settings.json 配置了 "
                                                       "apiKeyHelper：%1——它优先于 AutoCode 注入，"
                                                       "脚本输出的多余字符（CRLF/引号）会变成非法 key！")
                                          .arg(sHelper);
                            }
                        }
                    }
                    {   // ① 用户级 settings.json 的 env ANTHROPIC_* 键
                        const QString sS = QDir::homePath()
                            + QStringLiteral("/.claude/settings.json");
                        QFile fS(sS);
                        if (fS.open(QIODevice::ReadOnly)) {
                            const QJsonDocument oD = QJsonDocument::fromJson(fS.readAll());
                            fS.close();
                            const QStringList aKeys = oD.object()
                                .value(QStringLiteral("env")).toObject().keys();
                            int iAnth = 0;
                            for (const QString& s : aKeys) {
                                if (s.startsWith(QStringLiteral("ANTHROPIC_"))) { ++iAnth; }
                            }
                            if (iAnth > 0) {
                                aSrc << QStringLiteral("~/.claude/settings.json env 含 %1 个"
                                                       " ANTHROPIC_* 键（已被本轮 --settings "
                                                       "压制，正常不生效）").arg(iAnth);
                            }
                        }
                    }
                    {   // ② 登录凭据（/login managed key）
                        const QString sC = QDir::homePath()
                            + QStringLiteral("/.claude/.credentials.json");
                        if (QFile::exists(sC)) {
                            aSrc << QStringLiteral("~/.claude/.credentials.json 存在"
                                                   "（曾 /login 登录；注入 env 正常会优先，"
                                                   "若仍 401 请检查其内容）");
                        }
                    }
                    {   // ③ 企业托管配置
                        const QString sM = QStringLiteral("C:/ProgramData/ClaudeCode/"
                                                        "managed-settings.json");
                        if (QFile::exists(sM)) {
                            aSrc << QStringLiteral("⚠ 存在企业托管配置 %1（优先级最高，"
                                                   "可能改写鉴权；若含 apiKeyHelper 同样会顶掉"
                                                   "注入）")
                                      .arg(QDir::toNativeSeparators(sM));
                        }
                    }
                    {   // ④ 本进程环境残留（机器级 ANTHROPIC_* 环境变量）
                        const QProcessEnvironment oSys = QProcessEnvironment::systemEnvironment();
                        for (const QString& s : { QStringLiteral("ANTHROPIC_API_KEY"),
                                                  QStringLiteral("ANTHROPIC_AUTH_TOKEN") }) {
                            if (oSys.contains(s)) {
                                aSrc << QStringLiteral("⚠ 机器环境变量 %1 已设置（注入值"
                                                       "会覆盖；若显示 401 请核对两处值）").arg(s);
                            }
                        }
                    }
                    emit sigInfo(QStringLiteral(
                        "[claude-model] 鉴权失败诊断：本轮注入（%1）Key 指纹：%2；"
                        "%3。排查：①若指纹含非法字符→设置→大模型改 Key（勿带 Bearer/"
                        "sk- 前缀引号空格）；②确认 AutoCode 为最新版（标题栏 v4.1.329+，"
                        "旧版 --settings 未随启动参数传递）；③命令行验证：set "
                        "ANTHROPIC_BASE_URL=<接入点> && set ANTHROPIC_API_KEY=<Key> && "
                        "claude -p hi")
                        .arg(m_sDiagModelSrc.isEmpty()
                                 ? QStringLiteral("无") : m_sDiagModelSrc)
                        .arg(sFp)
                        .arg(aSrc.isEmpty()
                                 ? QStringLiteral("本机无用户级凭据源覆盖")
                                 : aSrc.join(QStringLiteral("；"))));
                    // 2026.10.11 本机网关直探（目标机 401/ECONNRESET 终审定位：开发机同 key
                    //   同网关全 200、目标机 Pi 通而 claude 拒——唯一未测变量=目标机网络路径）
                    //   AutoCode 亲自从本机 POST 注入的接入点（同 key 同 UA，小包流式）：
                    //   200=网络通而 claude 进程被本机拦截（代理/安全软件）；401/reset=网络
                    //   路径拦截大包或源 IP 受限。异步发射结果，不阻塞 UI。附带代理环境变量检测。
                    if (!m_sDiagBase.isEmpty()) {
                        if (m_pNetProbe == nullptr) {
                            m_pNetProbe = new QNetworkAccessManager(this);
                            // 直连探测（不走系统代理）：与 pi 同口径；若代理是元凶，
                            //   「直探通而 claude 不通」正好构成判定证据。
                            m_pNetProbe->setProxy(QNetworkProxy::NoProxy);
                            // 2026.10.11 管理器级 finished（reply 级 connect 实测不达，
                            //   管理器级稳）：一次性探测结果 → 诊断事件行。
                            connect(m_pNetProbe,
                                    &QNetworkAccessManager::finished,
                                    this, [this](QNetworkReply* a_pR) {
                                const qint64 iMs =
                                    QDateTime::currentMSecsSinceEpoch() - m_iProbeT0;
                                const QNetworkReply::NetworkError eErr = a_pR->error();
                                const int iStatus = a_pR->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                                const QString sBody2 = QString::fromUtf8(
                                    a_pR->read(160)).simplified();
                                QString sVerdict;
                                const bool bWasLarge2 = m_bProbeLargeSent;
                                if (eErr == QNetworkReply::NoError && iStatus == 200) {
                                    sVerdict = bWasLarge2
                                        ? QStringLiteral(
                                            "大包也通——本机链路无体积拦截，claude 失败"
                                            "另有原因（把本行+失败原文反馈开发）")
                                        : QStringLiteral(
                                            "小包通且 key 有效（等待大包探测对照）");
                                } else if (eErr == QNetworkReply::OperationCanceledError) {
                                    sVerdict = bWasLarge2
                                        ? QStringLiteral(
                                            "⚠ 大包超时而小包通——出口链路拦截大体积上传"
                                            "（上网行为管理/防火墙）实锤：换内网接入点/443"
                                            "端口/联系 IT 白名单")
                                        : QStringLiteral(
                                            "本机到网关超时（网络不通/被防火墙丢包）");
                                } else if (iStatus == 401) {
                                    sVerdict = bWasLarge2
                                        ? QStringLiteral(
                                            "⚠ 大包 401 而小包通——出口链路对大体积上传"
                                            "注入错误响应（上网行为管理/防火墙）实锤："
                                            "换内网接入点/443 端口/联系 IT 白名单")
                                        : QStringLiteral(
                                            "网关拒绝本机这个 key（源 IP 受限/key 绑定/"
                                            "中转层拦截——在网关侧排查；401 文案对合法但"
                                            "未注册的 key 也报 Invalid API key format）");
                                } else if (iStatus == 403) {
                                    sVerdict = QStringLiteral(
                                        "网关 403（客户端校验/来源受限——网关侧排查）");
                                } else if (eErr == QNetworkReply::RemoteHostClosedError) {
                                    sVerdict = bWasLarge2
                                        ? QStringLiteral(
                                            "⚠ 大包被中途断开而小包通——出口拦截大体积"
                                            "上传实锤（与 claude 的 ECONNRESET 同源）")
                                        : QStringLiteral(
                                            "连接被中途断开（链路层拦截/不稳定）");
                                } else {
                                    sVerdict = QStringLiteral(
                                        "非 200（网络错误=%1，响应：%2）——本机到网关链路异常")
                                        .arg(static_cast<int>(eErr))
                                        .arg(sBody2.left(80));
                                }
                                QString sProxy2;
                                {
                                    const QProcessEnvironment oSys =
                                        QProcessEnvironment::systemEnvironment();
                                    for (const QString& s : {
                                             QStringLiteral("HTTP_PROXY"),
                                             QStringLiteral("HTTPS_PROXY"),
                                             QStringLiteral("ALL_PROXY"),
                                             QStringLiteral("http_proxy"),
                                             QStringLiteral("https_proxy") }) {
                                        const QString sV = oSys.value(s);
                                        if (!sV.isEmpty()) {
                                            sProxy2 += QStringLiteral(" %1=%2").arg(s, sV);
                                        }
                                    }
                                }
                                emit sigInfo(QStringLiteral(
                                    "[claude-model] 网关直探（%6）：POST %1/v1/messages → "
                                    "HTTP %2（%3 ms）；%4%5")
                                    .arg(m_sDiagBase, QString::number(iStatus),
                                         QString::number(iMs), sVerdict,
                                         sProxy2.isEmpty()
                                             ? QString()
                                             : QStringLiteral(
                                                   "；⚠ 检测到代理环境变量：%1")
                                                   .arg(sProxy2),
                                         m_bProbeLargeSent
                                             ? QStringLiteral("大包77KB")
                                             : QStringLiteral("小包")));
                                // 2026.10.11 大包级联（目标机终审实锤：小包 200 而 claude
                                //   实际 77KB 大包 401——出口行为管理/防火墙按体积拦
                                //   非标端口大上传，注入 401 或掐断）：小包结束后自动
                                //   补发 77KB 同款请求；大包失败+小包成功=出口拦截铁证。
                                if (!m_bProbeLargeSent) {
                                    m_bProbeLargeSent = true;
                                    QNetworkRequest stReqBig((QUrl(
                                        m_sDiagBase + QStringLiteral("/v1/messages"))));
                                    stReqBig.setHeader(
                                        QNetworkRequest::ContentTypeHeader,
                                        QStringLiteral("application/json"));
                                    stReqBig.setRawHeader("anthropic-version",
                                                          "2023-06-01");
                                    stReqBig.setRawHeader(
                                        "User-Agent",
                                        "claude-cli/2.1.296 (external, sdk-cli)");
                                    if (!m_sDiagKey.isEmpty()) {
                                        stReqBig.setRawHeader(
                                            "x-api-key", m_sDiagKey.toUtf8());
                                        stReqBig.setRawHeader(
                                            "Authorization",
                                            ("Bearer " + m_sDiagKey).toUtf8());
                                    }
                                    const QString sPad(77000, QLatin1Char('a'));
                                    const QByteArray sBodyBig = QStringLiteral(
                                        "{\"model\":\"%1\",\"max_tokens\":8,"
                                        "\"stream\":true,\"messages\":[{"
                                        "\"role\":\"user\",\"content\":[{"
                                        "\"type\":\"text\",\"text\":\"hi %2\"}]}]}")
                                        .arg(m_sDiagModel.isEmpty()
                                                 ? QStringLiteral("glm-5.3")
                                                 : m_sDiagModel,
                                             sPad)
                                        .toUtf8();
                                    m_iProbeT0 = QDateTime::currentMSecsSinceEpoch();
                                    QNetworkReply* pR2 = m_pNetProbe->post(stReqBig, sBodyBig);
                                    // 2026.10.11 崩溃修复（336 版 UAF 实锤）：reply 在
                                    //   管理器级 finished 里被 deleteLater，裸指针捕获的
                                    //   超时定时器晚到即读已释放对象。QPointer 守卫代之。
                                    const QPointer<QNetworkReply> pGuard2 = pR2;
                                    QTimer::singleShot(30000, this, [pGuard2]() {
                                        if (QNetworkReply* a_pR2 = pGuard2.data()) {
                                            if (a_pR2->isRunning()) {
                                                a_pR2->abort();
                                            }
                                        }
                                    });
                                }
                                a_pR->deleteLater();
                            });
                        }
                        QString sUrl = m_sDiagBase;
                        while (sUrl.endsWith(QLatin1Char('/'))) {
                            sUrl.chop(1);
                        }
                        sUrl += QStringLiteral("/v1/messages");
                        QNetworkRequest stReq((QUrl(sUrl)));
                        stReq.setHeader(QNetworkRequest::ContentTypeHeader,
                                        QStringLiteral("application/json"));
                        stReq.setRawHeader("anthropic-version", "2023-06-01");
                        stReq.setRawHeader("User-Agent",
                                           "claude-cli/2.1.296 (external, cli)");
                        if (!m_sDiagKey.isEmpty()) {
                            stReq.setRawHeader("x-api-key", m_sDiagKey.toUtf8());
                            stReq.setRawHeader("Authorization",
                                ("Bearer " + m_sDiagKey).toUtf8());
                        }
                        const QByteArray sBody = QStringLiteral(
                            "{\"model\":\"%1\",\"max_tokens\":8,\"stream\":true,"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}")
                            .arg(m_sDiagModel.isEmpty()
                                     ? QStringLiteral("glm-5.3") : m_sDiagModel)
                            .toUtf8();
                        // 代理环境检测（claude 尊重代理 env；pi 不走 → "pi 通 claude 不通"差异源）
                        QString sProxy;
                        {
                            const QProcessEnvironment oSys =
                                QProcessEnvironment::systemEnvironment();
                            for (const QString& s : { QStringLiteral("HTTP_PROXY"),
                                                      QStringLiteral("HTTPS_PROXY"),
                                                      QStringLiteral("ALL_PROXY"),
                                                      QStringLiteral("http_proxy"),
                                                      QStringLiteral("https_proxy") }) {
                                const QString sV = oSys.value(s);
                                if (!sV.isEmpty()) {
                                    sProxy += QStringLiteral(" %1=%2").arg(s, sV);
                                }
                            }
                        }
                        m_iProbeT0 = QDateTime::currentMSecsSinceEpoch();
                        QNetworkReply* pR = m_pNetProbe->post(stReq, sBody);
                        // 2026.10.11 崩溃修复：同上，QPointer 防 reply 先行销毁的 UAF
                        //   （dump 实证：336 版 15s 定时器触发时 reply 已 deleteLater）
                        const QPointer<QNetworkReply> pGuard = pR;
                        QTimer::singleShot(15000, this, [pGuard]() {
                            if (QNetworkReply* a_pR2 = pGuard.data()) {
                                if (a_pR2->isRunning()) {
                                    a_pR2->abort();
                                }
                            }
                        });
                        // 2026.10.11 node 直探（终审定位：目标机实测 QNAM 直探 200 而 claude
                        //   仍 401——claude 本体是 node.exe，安全软件/终端管控常按进程过滤
                        //   HTTP。用 node.exe 发同款请求：QNAM 200 + node 401/reset = 铁证
                        //   进程级拦截；两者都 200 = 拦截不存在，问题在 claude 自身逻辑）。
                        {
                            const QString sJs = QDir::tempPath()
                                + QStringLiteral("/autocode_node_probe.js");
                            {
                                QFile fJs(sJs);
                                if (fJs.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                                    fJs.write(QStringLiteral(
                                        "const u=process.env.AUTOCODE_PROBE_URL||'';"
                                        "const k=process.env.AUTOCODE_PROBE_KEY||'';"
                                        "const m=process.env.AUTOCODE_PROBE_MODEL||'glm-5.3';"
                                        "fetch(u,{method:'POST',headers:{"
                                        "'content-type':'application/json',"
                                        "'x-api-key':k,'anthropic-version':'2023-06-01',"
                                        "'user-agent':'claude-cli/2.1.296 (external, cli)'},"
                                        "body:JSON.stringify({model:m,max_tokens:8,"
                                        "messages:[{role:'user',content:'hi'}]})})"
                                        ".then(async r=>{console.log('HTTP '+r.status+' '"
                                        "+(await r.text()).slice(0,100).replace(/\\s+/g,' '))})"
                                        ".catch(e=>console.log('ERR '"
                                        "+((e.cause&&e.cause.code)||e.message)))").toUtf8());
                                    fJs.close();
                                }
                            }
                            QProcessEnvironment oPE =
                                CAgentRunnerBase::MergedPathEnvironment();
                            oPE.insert(QStringLiteral("AUTOCODE_PROBE_URL"), sUrl);
                            oPE.insert(QStringLiteral("AUTOCODE_PROBE_KEY"), m_sDiagKey);
                            oPE.insert(QStringLiteral("AUTOCODE_PROBE_MODEL"),
                                       m_sDiagModel.isEmpty()
                                           ? QStringLiteral("glm-5.3") : m_sDiagModel);
                            const qint64 iT0n = QDateTime::currentMSecsSinceEpoch();
                            auto* pProc = new QProcess(this);
                            pProc->setProcessEnvironment(oPE);
                            connect(pProc, &QProcess::finished, this,
                                    [this, pProc, iT0n](
                                        int a_iCode, QProcess::ExitStatus) {
                                const QString sOut = QString::fromUtf8(
                                    pProc->readAllStandardOutput()).simplified();
                                const QString sErr = QString::fromUtf8(
                                    pProc->readAllStandardError()).simplified();
                                const qint64 iMs =
                                    QDateTime::currentMSecsSinceEpoch() - iT0n;
                                QString sLine = !sOut.isEmpty() ? sOut : sErr;
                                QString sJudge;
                                if (sOut.contains(QStringLiteral("HTTP 200"))) {
                                    sJudge = QStringLiteral(
                                        "（node 同样直连成功——本机无进程级拦截，"
                                        "问题在 claude 自身配置逻辑）");
                                } else if (sOut.contains(QStringLiteral("HTTP 401"))) {
                                    sJudge = QStringLiteral(
                                        "（⚠ node.exe 被拦截改写——安全软件/终端管控"
                                        "对 node.exe 的 HTTP 过滤实锤，退出码 %1）").arg(a_iCode);
                                } else if (sOut.contains(QStringLiteral("ERR "))) {
                                    sJudge = QStringLiteral(
                                        "（⚠ node.exe 网络层失败——连接被安全软件/防火墙"
                                        "针对进程掐断，退出码 %1）").arg(a_iCode);
                                } else {
                                    sJudge = QStringLiteral("（node 探测异常，退出码 %1）")
                                                 .arg(a_iCode);
                                }
                                emit sigInfo(QStringLiteral(
                                    "[claude-model] node 直探（与 claude 同进程类型）："
                                    "%1（%2 ms）%3")
                                    .arg(sLine.left(120),
                                         QString::number(iMs), sJudge));
                                pProc->deleteLater();
                            });
                            // 2026.10.11 崩溃修复：pProc 在 finished 里 deleteLater，
                            //   30s 超时定时器裸捕获同属 UAF，QPointer 守卫代之
                            const QPointer<QProcess> pProcGuard = pProc;
                            QTimer::singleShot(30000, this, [pProcGuard]() {
                                if (QProcess* a_pProc = pProcGuard.data()) {
                                    if (a_pProc->state() != QProcess::NotRunning) {
                                        a_pProc->kill();
                                    }
                                }
                            });
                            pProc->start(QStringLiteral("cmd.exe"),
                                         { QStringLiteral("/c"),
                                           QStringLiteral("node"),
                                           QDir::toNativeSeparators(sJs) });
                        }
                    }
                }
            } else if (sKind == QStringLiteral("tool_use")) {
                const QString sTool = oItem.value(QStringLiteral("name")).toString();
                const QJsonObject oInput = oItem.value(QStringLiteral("input")).toObject();
                const QString sInput = QString::fromUtf8(
                    QJsonDocument(oInput).toJson(QJsonDocument::Compact));
                emit sigToolUse(sTool, sInput);
            }
        }
        return;
    }

    if (sType == QStringLiteral("user")) {
        // 工具结果回显
        const QJsonObject oMsg = oRoot.value(QStringLiteral("message")).toObject();
        const QJsonArray aContent = oMsg.value(QStringLiteral("content")).toArray();
        for (const QJsonValue& v : aContent) {
            const QJsonObject oItem = v.toObject();
            if (oItem.value(QStringLiteral("type")).toString() == QStringLiteral("tool_result")) {
                const QJsonValue vContent = oItem.value(QStringLiteral("content"));
                QString sResult;
                if (vContent.isString()) {
                    sResult = vContent.toString();
                } else if (vContent.isArray()) {
                    // [{type:text,text:...}]
                    for (const QJsonValue& vc : vContent.toArray()) {
                        sResult += vc.toObject().value(QStringLiteral("text")).toString();
                    }
                }
                if (sResult.size() > 400) {
                    sResult = sResult.left(400) + QStringLiteral("...");
                }
                emit sigToolResult(sResult);
            }
        }
        return;
    }

    if (sType == QStringLiteral("result")) {
        // 2026.09.23 双 result 守卫（LayoutDesign「UI 已停、实仍在跑」实测根因）：claude 打满
        //   收尾会连发两条 result——先 error_max_turns，随后把 "Continue from where you left
        //   off." / "No response requested." 秒答包装成第二条 success result。两条可能同批到达，
        //   而第 1 条的收尾处理可能已同步触发上层续跑（StartStage→Reset 代次+1、m_bFinished 清零），
        //   残留的 success result 就会被误判成新轮「秒回成功」→ 整轮被错判「已完成」（压栈+done+
        //   面板停），而真正的新进程还在后续长跑（实测又干了 11 分钟）。两道闸：
        //   ① 代次不符 = 行属已被替换的旧进程，丢弃；
        //   ② 本轮已收尾（m_bFinished）后同代次再到的 result（如预算耗尽不续跑时），丢弃。
        if (m_iProcEpoch != a_iEpoch) {
            emit sigInfo(QStringLiteral("◌ 忽略旧进程残留 result 行（subtype=%1，进程已被续跑重启）")
                             .arg(oRoot.value(QStringLiteral("subtype")).toString()));
            return;
        }
        if (m_bFinished) {
            emit sigInfo(QStringLiteral("◌ 忽略本轮收尾后的残留 result 行（subtype=%1，已按先前收尾结果处理）")
                             .arg(oRoot.value(QStringLiteral("subtype")).toString()));
            return;
        }
        const bool bErr = oRoot.value(QStringLiteral("is_error")).toBool();
        const QString sSub = oRoot.value(QStringLiteral("subtype")).toString();
        const QString sResult = oRoot.value(QStringLiteral("result")).toString();
        const double dCost = oRoot.value(QStringLiteral("total_cost_usd")).toDouble();
        const QString sSid = oRoot.value(QStringLiteral("session_id")).toString();
        m_sLastModel = oRoot.value(QStringLiteral("model")).toString();   // 供上层查上下文窗口口径
        if (m_sLastModel.isEmpty()) {
            // 2026.09.23 result 顶层 model 字段已取消（CLI 2.1.278 实测，仅剩按模型名
            //   分组的 modelUsage）：回退取其首个键（即本次模型；多模型 fan-out 取任一
            //   仍优于空串兜底）。
            const QJsonObject oMU = oRoot.value(QStringLiteral("modelUsage")).toObject();
            if (!oMU.isEmpty()) {
                m_sLastModel = oMU.keys().constFirst();
            }
        }

        const QJsonObject oUsage = oRoot.value(QStringLiteral("usage")).toObject();
        const qint64 iInRaw  = oUsage.value(QStringLiteral("input_tokens")).toInteger();
        const qint64 iOutTok = oUsage.value(QStringLiteral("output_tokens")).toInteger();
        const qint64 iCacheCreate = oUsage.value(QStringLiteral("cache_creation_input_tokens")).toInteger();
        const qint64 iCacheRead   = oUsage.value(QStringLiteral("cache_read_input_tokens")).toInteger();

        // 注意：result 的 usage 是本次进程**全部 API 调用** input 侧的累计总和（实测 a91aab86 轮
        //   21 次调用累计 1,365,471，与 result 完全一致），远大于单次调用的上下文、可超模型窗口，
        //   不能当「上下文大小」用。真实上下文占用以最后一条 assistant 消息的 usage 为准
        //   （单次调用的 input+cache 三项之和）；仅在全程没收到过 assistant usage 的极端场景回退 result。
        // 2026.08.15 「输入」口径修正：未缓存输入 + 缓存写 + 缓存读（与账单/控制台一致）。
        //   此前只报未缓存 input，而 agent 会话绝大部分输入走缓存，导致界面显示远小于真实消耗
        //   （费用 total_cost_usd 本就含缓存项，不受影响）。
        const qint64 iInTok = iInRaw + iCacheCreate + iCacheRead;
        // 2026.08.15 兜底加窗限：result 的输入是全程累计（长对话动辄上百万），只有小到不超窗口
        //   时才可能是真实上下文；超窗口不采用（保持 0/上一条真实值），防「上下文 1.9M」假显示。
        if (m_iLastContextFill == 0 && iInTok > 0
            && iInTok <= ContextWindowForModel(m_sLastModel)) {
            m_iLastContextFill = iInTok;   // 三项之和（与上面同口径）
        }

        const bool bOk = !bErr && (sSub == QStringLiteral("success"));
        // 2026.09.23 空转秒回抑制（HotNews「UI 已停、实仍在跑」实测根因）：对 max-turns
        //   打满的会话 --resume+fork 续跑时，claude CLI 实测会在启动后 1~2s 先回一条
        //   result(success) 空结果——全程无 assistant 消息/工具调用/token 产出、费用 0，
        //   续跑 prompt 还压在其内部输入队列未消费，随后才真正开始干活（实测续跑进程
        //   又持续工作了 10 分钟以上）。若照常收尾，上层会把整轮判成「已完成」（面板
        //   状态图标/Tab 闪点停止、输入栏解禁），而进程还在跑并持续吐日志 →「UI 已停、
        //   实仍在跑」假象。此处识别该退化结果：不置收尾态、不关 stdin、继续等真正的
        //   收尾；若进程随后直接退出，slotOnFinished 的 exit-0 兜底语义不变（照常收尾），
        //   只增强不回退。判定条件取「零产出交集」，真实秒答必有 assistant 消息/输出
        //   token，不会误伤。
        if (bOk && sResult.trimmed().isEmpty() && dCost <= 0.0
                && m_uptime.isValid() && m_uptime.elapsed() < 5000
                && m_iLastReqCount == 0 && m_iStreamOut == 0 && m_iLiveEmitOut == 0) {
            m_sLastSessionId = sSid;   // 记录 fork 出的会话：进程早夭时上层仍可凭此续跑
            emit sigInfo(QStringLiteral("◌ 进程启动 %1s 即回空 result（无任何产出），判定为退化收尾信号，继续等待实际工作完成…")
                             .arg(m_uptime.elapsed() / 1000.0, 0, 'f', 1));
            return;
        }
        // 2026.08.27 实时计量已逐消息上抛（sigUsageDelta）时，收尾 token 报 0 防与
        //   编排器实时累计双计（同 Codex 口径）；费用/会话 id 照报。未发过（无 assistant
        //   消息的极端流）仍报 result 累计。
        const qint64 iFinIn  = (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : iInTok;
        const qint64 iFinOut = (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : iOutTok;
        m_sLastSubtype   = sSub;     // 供编排器区分 max-turns / 真错误
        m_sLastSessionId = sSid;     // 供编排器 --resume 续跑
        m_bFinished = true;
        // 2026.09.26 零 usage 诊断（放空 result 抑制之后，避免退化收尾误报）：
        //   assistant 帧/message_delta/result 兑底全没拿到时上下文将显示「—」——
        //   日志写明原因指向（网关/中转未回传 usage），用户不再对着 0/— 猜。
        if (m_iLastContextFill <= 0) {
            emit sigInfo(QStringLiteral("[claude] 本轮未收到任何 usage 数据，上下文占用不可知（显示 —）；"
                                       "常见原因：网关/中转层未回传 usage 字段"));
        }
        // 2026.08.27 悬空 resume sid 自愈：--resume 的会话在本机不存在（claude 报
        //   「No conversation found with session ID」，工程自其他电脑拷贝时必现——
        //   助手档 sid 随工程走、会话数据在 ~/.claude 不随工程走）→ 不发失败，从参数
        //   剥离 --resume/--fork-session 杀进程，静默新建会话重发本轮（仅一次）。
        if (!bOk && !m_bResumeDropped
            && m_aLastArgs.contains(QStringLiteral("--resume"))
            && m_aStderrTail.join(QStringLiteral(" "))
                   .contains(QStringLiteral("No conversation found with session ID"),
                              Qt::CaseInsensitive)) {
            m_bResumeDropped = true;
            m_bQuietRestart  = true;
            m_bFinished      = false;
            m_sLastSubtype.clear();
            m_sLastSessionId.clear();   // 新轮换新 sid
            for (int i = 0; i < m_aLastArgs.size();) {
                if (m_aLastArgs.at(i) == QStringLiteral("--resume")) {
                    if (i + 1 < m_aLastArgs.size()) {
                        m_aLastArgs.removeAt(i + 1);
                    }
                    m_aLastArgs.removeAt(i);
                } else if (m_aLastArgs.at(i) == QStringLiteral("--fork-session")) {
                    m_aLastArgs.removeAt(i);
                } else {
                    ++i;
                }
            }
            emit sigInfo(QStringLiteral("↻ 会话在本机不存在（工程可能自其他电脑拷贝，"
                                        "会话数据不随工程走），自动新建会话重发本轮…"));
            if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
                KillProcessTree();   // 收尾信号走 slotOnFinished 的 QuietRestart 分支重拉（整树终止防新旧双跑）
            }
            return;
        }
        // 2026.08.19 stream-json 输入模式：本轮结束（result）后关闭 stdin，claude 收到
        //   EOF 即干净退出，生命周期与旧「一次性管道」等价。emit 是同步调用——上层槽
        //   可能已同步 StartStage（max-turns 自动续跑）拉起新进程，凭代次守卫只关旧进程。
        const int iEpoch = m_iProcEpoch;
        // 2026.08.15 空原因修复（7ce53172 实测）：失败（is_error / subtype≠success）且 result
        //   为空时（max-turns / 中继 API 错误均可能出现），用 subtype 兜底成非空原因，
        //   避免上层显示「助手调用失败：」后面空白。
        QString sMsg = sResult;
        if (!bOk && sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("无错误文本（subtype=%1）")
                       .arg(sSub.isEmpty() ? QStringLiteral("unknown") : sSub);
            // 2026.08.27 真因挖掘①：会话 transcript 尾部的错误条目（isApiErrorMessage/
            //   error 级 system 行）——异机 headless 失败而交互/VSCode 正常时 stderr
            //   常为空，transcript 是唯一可靠信息源。
            const QString sTrErr = MineTranscriptErrorTail();
            if (!sTrErr.trimmed().isEmpty()) {
                sMsg += QStringLiteral("；会话错误记录：%1").arg(sTrErr);
            }
            // 2026.08.27 真因挖掘②：stderr 尾部（同 EmitFailed 口径）。
            if (!m_aStderrTail.isEmpty()) {
                QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
                if (sTail.size() > 800) {
                    sTail = sTail.left(800) + QStringLiteral("…");
                }
                sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
            }
            // 常见根因（新机器部署 Claude Code 后端）：未登录/无凭证/网关未配置。
            const QString sTailLow = m_aStderrTail.join(QStringLiteral(" ")).toLower();
            if (sTailLow.contains(QStringLiteral("api key"))
                || sTailLow.contains(QStringLiteral("401"))
                || sTailLow.contains(QStringLiteral("unauthorized"))
                || sTailLow.contains(QStringLiteral("login"))
                || sTailLow.contains(QStringLiteral("onboarding"))
                || sTailLow.contains(QStringLiteral("credential"))) {
                sMsg += QStringLiteral("；处理：在该电脑终端运行一次 claude 交互式完成登录/认证"
                                       "（或配置 ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN"
                                       " 指向可用网关）后再试");
            }
            // 2026.08.27 悬空 resume 兕底提示（自动降级已用尽/未命中时）：会话 id 不跨机。
            if (m_aStderrTail.join(QStringLiteral(" "))
                    .contains(QStringLiteral("No conversation found with session ID"),
                               Qt::CaseInsensitive)) {
                sMsg += QStringLiteral("；处理：该会话 id 在本机不存在（工程/助手档自其他电脑"
                                       "拷贝时常见，会话数据不随工程走），正常应已自动新建会话"
                                       "重试；若仍失败，请新建一个助手再发");
            }
        }
        emit sigFinished(bOk, sMsg, iFinIn, iFinOut, dCost, sSid);
        if (m_iProcEpoch == iEpoch && m_pProc != nullptr
            && m_pProc->state() != QProcess::NotRunning) {
            m_pProc->closeWriteChannel();   // 见上方代次守卫说明
        }
        return;
    }

    // 未知类型：原样上抛
    emit sigInfo(QString::fromUtf8(a_sLine));
}

void CClaudeRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    // 2026.08.27 悬空 resume 自愈的静默重拉：kill 收尾不发失败，短延迟原 prompt 重发
    //   （参数已剥离 --resume）。
    if (m_bQuietRestart) {
        m_bQuietRestart = false;
        QTimer::singleShot(300, this, [this]() { RelaunchStage(); });
        return;
    }
    if (m_bFinished || m_bGraceActive) {
        return;  // 已由 result 行发出 / 宽限期判定中
    }
    if (a_iExitCode == 0) {
        m_bFinished = true;
        emit sigFinished(true, QString(), 0, 0, 0.0, QString());
        return;
    }
    // 2026.08.15 区分两种异常退出：
    //   NormalExit + 非零码：npm shim（claude.cmd）同步等待 node，cmd 正常退出说明 node 已终止
    //     （a0c96b4d 开发阶段 03:19：API 瞬态错误 exit 1）——无孤儿，立即判失败，真因看附带的 stderr。
    //   CrashExit：cmd.exe 被硬杀而 node 可能仍以孤儿进程运行（a0c96b4d 验证阶段 10:03：cmd 死后
    //     90 秒 node 仍在写 transcript 并最终以 max-turns 收敛）——转宽限期轮询判真因。
    if (a_eStatus == QProcess::CrashExit) {
        BeginOrphanGrace(QStringLiteral("claude 进程异常终止（CrashExit，退出码=%1）").arg(a_iExitCode));
        return;
    }
    // 2026.08.15 快速失败自动重试：短命非零退出且无 result——多为 max-turns 后 0 秒续跑撞上
    //   上一进程会话清理（后台任务收尾/会话文件落盘）的瞬态错误，隔几秒原参数重拉即可恢复。
    //   重试 2 次仍快速失败才判死（真因看附带的 stderr 尾部）。
    if (m_uptime.isValid() && m_uptime.elapsed() < 15000 && m_iFastRetry < 2) {
        // 2026.08.27 兜底：stderr 明确报 unknown option '--autocompact'（探测失效/版本变更）
        //   → 从重试参数中剔除后重拉，不做无用动重试。
        const QString sTail = m_aStderrTail.join(QStringLiteral(" "));
        if (sTail.contains(QStringLiteral("unknown option"), Qt::CaseInsensitive)
            && sTail.contains(QStringLiteral("autocompact"), Qt::CaseInsensitive)) {
            for (int i = 0; i + 1 < m_aLastArgs.size(); ++i) {
                if (m_aLastArgs.at(i) == QStringLiteral("--autocompact")) {
                    m_aLastArgs.removeAt(i + 1);
                    m_aLastArgs.removeAt(i);
                    emit sigInfo(QStringLiteral("[system] 当前 claude CLI 不支持 --autocompact，已自动剔除并重试"));
                    break;
                }
            }
        }
        ++m_iFastRetry;
        m_bRetryPending = true;
        emit sigInfo(QStringLiteral("claude 进程 %1 秒内即退出（退出码=%2），5 秒后自动重试（第 %3/2 次）")
                         .arg(m_uptime.elapsed() / 1000.0, 0, 'f', 1)
                         .arg(a_iExitCode)
                         .arg(m_iFastRetry));
        QTimer::singleShot(5000, this, [this]() {
            RelaunchStage();
        });
        return;
    }
    EmitFailed(QStringLiteral("claude 进程异常退出，退出码=%1").arg(a_iExitCode));
}

void CClaudeRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    // 2026.09.19 勿回退（PixelPorter 实害：pi 切 claude 后首轮「会话在本机不存在」自愈必失败）：
    //   Qt 的 kill()（TerminateProcess）在 Windows 上先发 errorOccurred(Crashed)、后发
    //   finished(CrashExit)——自愈路径在 HandleLine 里已置 m_bFinished=false 并 kill()，本槽的
    //   (m_bFinished||m_bGraceActive) 守卫双双放行 → BeginOrphanGrace（sid 已被自愈清空）→
    //   EmitFailed("claude 进程崩溃"+旧进程 stderr 尾) 抢在 QuietRestart 重拉之前宣告失败。
    //   m_bQuietRestart 在 kill() 前置位、slotOnFinished 的重拉分支才复位——此处一并
    //   拦截即消除竞态（Stop() 走 m_bFinished=true 前置，同口径）。
    if (m_bQuietRestart || m_bFinished || m_bGraceActive) {
        return;
    }
    QString sMsg;
    switch (a_eError) {
        case QProcess::FailedToStart: sMsg = QStringLiteral("claude 启动失败（未在 PATH 找到 claude.cmd）"); break;
        case QProcess::Crashed:       sMsg = QStringLiteral("claude 进程崩溃"); break;
        case QProcess::Timedout:      sMsg = QStringLiteral("claude 进程超时"); break;
        case QProcess::ReadError:     sMsg = QStringLiteral("读取 claude 输出错误"); break;
        case QProcess::WriteError:    sMsg = QStringLiteral("写入 claude 输入错误"); break;
        default:                      sMsg = QStringLiteral("claude 进程未知错误"); break;
    }
    if (a_eError == QProcess::Crashed) {
        // CrashExit 场景：node 可能仍在运行，转宽限期（finished 信号到达时同理，双入口均守 m_bGraceActive）
        BeginOrphanGrace(sMsg);
    } else {
        EmitFailed(sMsg);
    }
}

void CClaudeRunner::BeginOrphanGrace(const QString& a_sWhy) {
    // 崩溃宽限期入口：cmd.exe 死了但 node（真 claude）可能还在跑。无 sid（连 init 都没到）
    // 时无从定位 transcript，直接判失败。宽限 3s/轮 × 80 轮 = 最长 240s（实测孤儿在 ~90s 后
    // 才写收敛标记，最后一轮 API 生成可能更久，留足余量）。
    if (m_bFinished || m_bGraceActive) {
        return;
    }
    if (m_sLastSessionId.isEmpty()) {
        EmitFailed(a_sWhy);
        return;
    }
    m_bGraceActive = true;
    m_iGraceTicks = 0;
    if (m_pGraceTimer == nullptr) {
        m_pGraceTimer = new QTimer(this);
        m_pGraceTimer->setInterval(3000);
        connect(m_pGraceTimer, &QTimer::timeout, this, [this]() {
            PollOrphanTranscript();
        });
    }
    emit sigInfo(a_sWhy + QStringLiteral("；claude(node) 可能仍在运行，等待其 transcript 落盘判定真因（最长 240 秒）…"));
    m_pGraceTimer->start();
    PollOrphanTranscript();   // 立即先探一次（孤儿已收敛的场景零延迟命中）
}

void CClaudeRunner::PollOrphanTranscript() {
    if (!m_bGraceActive) {
        return;
    }
    ++m_iGraceTicks;
    const QString sPath = FindTranscriptPath();
    if (!sPath.isEmpty()) {
        CFrwDevice stFile(sPath);
        if (stFile.open(QIODevice::ReadOnly)) {
            const qint64 iSize = stFile.size();
            if (iSize > m_iTranscriptSeen) {
                stFile.seek(m_iTranscriptSeen);
                const QByteArray bufNew = stFile.readAll();
                m_iTranscriptSeen = iSize;
                // 只在「基线之后新增」的内容里找标记：续跑场景上一轮遗留的旧 max_turns_reached
                //   不会误判（其在基线之前）；命中即孤儿进程以轮次上限正常收敛（max_turns_reached
                //   是 CLI 停轮时的固定落盘标记，其后可能还有 last-prompt 等元数据行，故搜整段而非末行）。
                if (bufNew.contains("max_turns_reached")) {
                    if (m_pGraceTimer != nullptr) {
                        m_pGraceTimer->stop();
                    }
                    m_bGraceActive = false;
                    m_sLastSubtype = QStringLiteral("error_max_turns");   // 上层 LastWasMaxTurns() → 自动 --resume
                    m_bFinished = true;
                    emit sigInfo(QStringLiteral("✓ 孤儿进程已以轮次上限收敛（transcript 出现 max_turns_reached），按可续跑处理"));
                    emit sigFinished(false,
                                    QStringLiteral("进程异常终止，但会话已达轮次上限并正常收敛，转自动续跑"),
                                    0, 0, 0.0, m_sLastSessionId);
                    return;
                }
            }
        }
    }
    if (m_iGraceTicks >= 80) {
        if (m_pGraceTimer != nullptr) {
            m_pGraceTimer->stop();
        }
        m_bGraceActive = false;
        EmitFailed(QStringLiteral("claude 进程崩溃，且宽限期内 transcript 未出现收敛标记"));
    }
}

QString CClaudeRunner::MineTranscriptErrorTail() {
    // 2026.08.27 空结果失败（error_during_execution 等）真因挖掘：会话 transcript
    //   （~/.claude/projects/<工程>/*/<sid>.jsonl）按事件逐行落盘，API 错误条目带
    //   isApiErrorMessage:true 且含真实错误文本，error 级 system 行亦有描述——
    //   stderr 为空（异机 headless 失败常见，交互/VSCode 正常）时这里是唯一可靠
    //   信息源。取尾部 64KB 倒序找最多 3 条错误条目，找不到返回空串（静默降级）。
    const QString sPath = FindTranscriptPath();
    if (sPath.isEmpty()) {
        return QString();
    }
    CFrwDevice stFile(sPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const qint64 iSize = stFile.size();
    const qint64 iTail = qMin<qint64>(iSize, 64 * 1024);
    if (iTail > 0) {
        stFile.seek(iSize - iTail);
    }
    const QList<QByteArray> aLines = stFile.readAll().split('\n');
    QStringList aErrs;
    auto fnExtractText = [](const QJsonObject& a_o) -> QString {
        // 从事件对象抽可读文本：优先 message.content[] 的 text 项，其次 content/文本字符串字段。
        const QJsonObject oMsg = a_o.value(QStringLiteral("message")).toObject();
        const QJsonValue vContent = oMsg.contains(QStringLiteral("content"))
            ? oMsg.value(QStringLiteral("content"))
            : a_o.value(QStringLiteral("content"));
        QStringList aParts;
        if (vContent.isArray()) {
            for (const QJsonValue& v : vContent.toArray()) {
                const QString sT = v.toObject().value(QStringLiteral("text")).toString();
                if (!sT.trimmed().isEmpty()) {
                    aParts << sT;
                }
            }
        } else if (vContent.isString()) {
            aParts << vContent.toString();
        }
        return aParts.join(QStringLiteral(" "));
    };
    for (int i = aLines.size() - 1; i >= 0 && aErrs.size() < 3; --i) {
        const QByteArray szLine = aLines.at(i).trimmed();
        if (szLine.isEmpty()) {
            continue;
        }
        const QJsonDocument stDoc = QJsonDocument::fromJson(szLine);
        if (!stDoc.isObject()) {
            continue;
        }
        const QJsonObject o = stDoc.object();
        const bool bApiErr = o.value(QStringLiteral("isApiErrorMessage")).toBool();
        const QString sLevel = o.value(QStringLiteral("level")).toString();
        const QString sSub   = o.value(QStringLiteral("subtype")).toString();
        const bool bErrLine  = bApiErr
            || sLevel.compare(QStringLiteral("error"), Qt::CaseInsensitive) == 0
            || sSub.contains(QStringLiteral("error"), Qt::CaseInsensitive);
        if (!bErrLine) {
            continue;
        }
        QString sText = fnExtractText(o);
        if (sText.trimmed().isEmpty()) {
            // 结构抽不到（未知事件形态）：退回原始行内 "text"/"content" 字段片段。
            const QString sRaw = QString::fromUtf8(szLine);
            static const QRegularExpression re(
                QStringLiteral("\"(?:text|content|message)\"\s*:\s*\"([^\"]{20,400})\""));
            const auto m = re.match(sRaw);
            sText = m.hasMatch() ? m.captured(1) : sRaw.left(300);
        }
        if (!sText.trimmed().isEmpty()) {
            aErrs.prepend(sText.left(400));
        }
    }
    return aErrs.join(QStringLiteral(" | "));
}

QString CClaudeRunner::FindTranscriptPath() {
    // 按 sid 在 ~/.claude/projects/<工程>/*/<sid>.jsonl 定位会话 transcript。
    //   目录名是工程路径的字符替换（':'、'\\'、'+' 等非字母数字全变 '-'），不重造变换规则，
    //   直接遍历各工程子目录按文件名精确匹配（目录数有限，只在启动/崩溃判定时调用，开销可忽略）。
    if (m_sLastSessionId.isEmpty() || !m_sTranscriptPath.isEmpty()) {
        return m_sTranscriptPath;
    }
    const QString sFileName = m_sLastSessionId + QStringLiteral(".jsonl");
    QDir dirProjects(QDir::homePath() + QStringLiteral("/.claude/projects"));
    const QFileInfoList aSubs = dirProjects.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& stSub : aSubs) {
        const QString sPath = stSub.absoluteFilePath() + QLatin1Char('/') + sFileName;
        if (QFile::exists(sPath)) {
            m_sTranscriptPath = sPath;
            return m_sTranscriptPath;
        }
    }
    return QString();
}

void CClaudeRunner::CheckStall() {
    // 2026.08.17 停滞检测：进程活着但距上次收到任何输出超过 [Agent] StallMinutes（默认 10 分钟）
    //   → 判挂起（网络/中继断连卡死的真实形态：进程不死、也永不产出）。终止进程并以
    //   subtype="stall" 发失败，上层凭 lastSessionId 自动 --resume 续跑——用户无感接续。
    //   阈值刻意保守：分钟级构建等长工具执行期间本就零输出，10 分钟下限不误伤。
    if (!m_bStallWatch || m_bFinished || m_bGraceActive || m_bRetryPending) {
        return;   // 未武装 / 已收尾 / 宽限判定中 / 重试等待中
    }
    if (m_pProc == nullptr || m_pProc->state() == QProcess::NotRunning) {
        return;   // 进程已退出，交给 finished 路径
    }
    const qint64 iLimitMs = static_cast<qint64>(StallMinutes()) * 60000;
    if (m_actClock.elapsed() < iLimitMs) {
        return;
    }
    if (m_pStallTimer != nullptr) {
        m_pStallTimer->stop();
    }
    m_bStallWatch = false;
    m_sLastSubtype = QStringLiteral("stall");   // 上层 LastWasStall() → 自动 --resume
    emit sigInfo(QStringLiteral("⚠ 停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），"
                                "终止进程并转自动续跑…").arg(StallMinutes()));
    // 先置终态再 kill：kill 触发的 Crashed/finished 信号一律吞掉（同 Stop 静默模式），
    //   避免误入孤儿宽限期（node 已随挂起失去意义，无需等待 transcript）。
    m_bFinished = true;
    KillProcessTree();   // 2026.09.22 整树终止：挂起的 cmd+node 一并终结，无孤儿残留
    m_pProc->waitForFinished(3000);
    emit sigFinished(false,
                     QStringLiteral("停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），进程已终止")
                         .arg(StallMinutes()),
                     0, 0, 0.0, m_sLastSessionId);
}

void CClaudeRunner::EmitFailed(const QString& a_sMsg) {
    if (m_bFinished || m_bGraceActive) {
        return;
    }
    // 2026.08.15 失败消息附带 stderr 尾部：claude CLI 的报错原因（API 错误/认证/网络等）
    //   只打在 stderr，此前丢失只剩裸退出码；sid 一并上抛，上层失败后仍可 --resume。
    QString sMsg = a_sMsg;
    if (!m_aStderrTail.isEmpty()) {
        QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
        if (sTail.size() > 800) {
            sTail = sTail.left(800) + QStringLiteral("…");
        }
        sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
        // 2026.10.10 路径类失败提示：cmd 本地化报「系统找不到指定的路径/文件」时，
        //   启动链里存在失效路径（claude.cmd shim 引用的 node/cli.js 缺失等）——
        //   给出可操作指引，避免只剩裸报错无从下手。
        if (sTail.contains(QStringLiteral("找不到指定的路径"))
            || sTail.contains(QStringLiteral("找不到文件"))
            || sTail.contains(QStringLiteral("不是内部或外部命令"))) {
            sMsg += QStringLiteral("\n提示：claude 启动链中存在失效路径/缺失命令。"
                "请在 cmd 窗口运行「%1 --version」验证：报错则重跑环境安装"
                "（install_env.bat）；正常则把 where 结果的绝对路径填到"
                "「设置 → Agent」的 Claude 命令后重试。")
                .arg(ClaudeCmd());
        }
    }
    m_bFinished = true;
    emit sigInfo(QStringLiteral("[错误] ") + sMsg);
    emit sigFinished(false, sMsg, 0, 0, 0.0, m_sLastSessionId);
}
