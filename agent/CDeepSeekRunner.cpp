#include "CDeepSeekRunner.h"

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
#include <QTimer>
#include <QUuid>

#include "CClaudeRunner.h"   // ContextWindowForModel：DSH 侧窗口兜底探测复用同一实现

// —— 进程级全局配置（static：所有 DSH runner 实例共享，主窗口启动/设置变更时写）——
static QString s_sDshRootPath;    // DSH 仓库根（deepseek-harness 目录；空=未配置/未探测到）
static QString s_sDshNodePath;    // node.exe 绝对路径（空=走 PATH 的 "node"）
static QString s_sDshBinJsPath;   // bin.js 绝对路径（空=按根目录推导；显式键优先，旧档回退）
static QString s_sDshCordisPath;  // cordis.yml 绝对路径（空=按根目录推导；显式键优先，旧档回退）
static QString s_sDshProvider;    // provider 名（空回退 glm-custom）
static QString s_sDshModel;       // 模型名（空回退 glm-5.2）
// 计费单价（$/百万 token；DSH 协议只报 token 不报金额，客户端折算）。
//   默认对齐 claude 后端经同一网关显示的 Sonnet 档价目；全 0 = 不折算（费用列回 "-"）。
static double s_dDshPriceInPerM        = 3.0;
static double s_dDshPriceCacheReadPerM = 0.3;
static double s_dDshPriceCacheWritePerM = 3.75;
static double s_dDshPriceOutPerM       = 15.0;
// 自定义大模型表 + 激活下标（-1 = 未启用；条目字段注入见 EnsureProcess env 段）
static QVector<CDeepSeekRunner::SCustomModel> s_aCustomModels;
static int s_iCustomActive = -1;
// 2026.08.19 热生效支持：活实例登记（配置变更时寻址各进程）+ 冷启动签名（激活条目
//   全字段；变化才重启，设置页反复确定/无关改动不打扰既有会话）。
static QSet<CDeepSeekRunner*> s_aLiveRunners;
static QString s_sCustomBootSig;

// 根目录内固定相对布局（bin.js 与 cordis.yml 的推导位置）。
static constexpr const char* kSzDshBinJsRel  = "/packages/examples/jsonrpc-demo/lib/bin.js";
static constexpr const char* kSzDshCordisRel = "/examples/autocode-agent/cordis.yml";

void CDeepSeekRunner::SetDshRootPath(const QString& a_sRoot) {
    s_sDshRootPath = QDir::fromNativeSeparators(a_sRoot.trimmed());
    while (s_sDshRootPath.endsWith(QLatin1Char('/'))) {
        s_sDshRootPath.chop(1);
    }
}

QString CDeepSeekRunner::DshRootPath() {
    return s_sDshRootPath;
}

QString CDeepSeekRunner::ResolveNodePath() {
    // 留空走 PATH：QProcess 直起可执行名由系统搜路径（本机装了 node 即零配置）。
    return s_sDshNodePath.isEmpty() ? QStringLiteral("node") : s_sDshNodePath;
}

QString CDeepSeekRunner::ResolveBinJsPath() {
    if (!s_sDshBinJsPath.isEmpty()) {
        return s_sDshBinJsPath;
    }
    return s_sDshRootPath.isEmpty() ? QString()
                                    : s_sDshRootPath + QString::fromLatin1(kSzDshBinJsRel);
}

QString CDeepSeekRunner::ResolveCordisPath() {
    if (!s_sDshCordisPath.isEmpty()) {
        return s_sDshCordisPath;
    }
    return s_sDshRootPath.isEmpty() ? QString()
                                    : s_sDshRootPath + QString::fromLatin1(kSzDshCordisRel);
}

bool CDeepSeekRunner::LooksLikeDshRoot(const QString& a_sRoot) {
    const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    return !sRoot.isEmpty()
        && QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshBinJsRel))
        && QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshCordisRel));
}

QString CDeepSeekRunner::DshConfigError() {
    // 2026.08.18 DSH 可用性统一校验：启动警告 / 设置保存兜底 / StartStage 前置三处
    //   共用同一口径——用户在任何入口看到的都是同一条完整处理指引，不会只报
    //   「路径无效」却不说怎么办。返回空串 = 可用。
    const QString sNode = ResolveNodePath();
    if (sNode == QStringLiteral("node")) {
        if (QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            return QStringLiteral(
                "node 未配置且 PATH 中找不到 node.exe。\n"
                "处理：安装 Node.js 后重启 AutoCode（node 路径留空即走 PATH），或在"
                "「设置 → DeepSeekHarness 参数 → node 路径」填 node.exe 绝对路径。");
        }
    } else if (!QFile::exists(sNode)) {
        return QStringLiteral(
            "node 路径无效：%1。\n"
            "处理：「设置 → DeepSeekHarness 参数 → node 路径」改填有效的 node.exe"
            "（留空即走 PATH）。").arg(sNode);
    }
    const QString sBin = ResolveBinJsPath();
    if (sBin.isEmpty() || !QFile::exists(sBin)) {
        return QStringLiteral(
            "DSH 运行时 bin.js 无效：%1。\n"
            "处理：「设置 → DeepSeekHarness 参数」点「安装 DeepSeekHarness…」一键安装"
            "（完成后点「重新检测」，根目录自动填入）；或把「DSH 根目录」填为 "
            "deepseek-harness 仓库根（bin.js 按固定布局自动推导）。").arg(
                sBin.isEmpty() ? QStringLiteral("(空)") : sBin);
    }
    const QString sCordis = ResolveCordisPath();
    if (sCordis.isEmpty() || !QFile::exists(sCordis)) {
        return QStringLiteral(
            "DSH 插件组合 cordis.yml 无效：%1（组合文件须位于 DSH 仓库 examples/ 树内，"
            "裸插件名才能解析 node_modules）。\n"
            "处理：重跑「安装 DeepSeekHarness…」（会把组合文件部署到固定布局）；或把正本 "
            "AutoCode 仓库 agent/dsh/cordis.autocode.yml 复制为 "
            "<DSH根>/examples/autocode-agent/cordis.yml。").arg(
                sCordis.isEmpty() ? QStringLiteral("(空)") : sCordis);
    }
    return QString();
}

QString CDeepSeekRunner::ProbeSiblingRoot() {
    // install_env.bat 把 DSH 装在 exe 同级目录内的 DeepSeekHarness\deepseek-harness
    //   （绿色便携：随 Execute 程序目录整体拷走）；这里同布局探测（exe 同级/上级 ×
    //   两种目录名，exe 同级最优先；上两级为旧布局兼容回退），命中返回 canonical 根。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    const QStringList aNames = {
        QStringLiteral("/DeepSeekHarness/deepseek-harness"), QStringLiteral("/deepseek-harness")
    };
    for (const QString& sBase : aBases) {
        for (const QString& sName : aNames) {
            const QString sRoot = QDir(sBase + sName).canonicalPath();
            if (LooksLikeDshRoot(sRoot)) {
                return sRoot;
            }
        }
    }
    return QString();
}

QString CDeepSeekRunner::DshInstallDiagnosis() {
    // 半安装态诊断：探测布局同 ProbeSiblingRoot，但目录存在即停下来报缺项——
    //   bin.js 缺 = 克隆/构建没完成；cordis.yml 缺 = 程序目录没带 dsh\ 正本、
    //   组合文件没部署（旧版安装脚本此处只警告仍报「安装完成」，2026.08.18 已改硬错误）。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    const QStringList aNames = {
        QStringLiteral("/DeepSeekHarness/deepseek-harness"), QStringLiteral("/deepseek-harness")
    };
    for (const QString& sBase : aBases) {
        for (const QString& sName : aNames) {
            if (!QDir(sBase + sName).exists()) {
                continue;
            }
            const QString sRoot = QDir(sBase + sName).canonicalPath();
            const bool bBin = QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshBinJsRel));
            const bool bCordis = QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshCordisRel));
            if (bBin && bCordis) {
                continue;   // 完整根（探测应已命中），继续看其他候选
            }
            QString sMissing;
            if (!bBin) {
                sMissing = QStringLiteral("bin.js（packages\\examples\\jsonrpc-demo\\lib\\ 下）——"
                                          "克隆或 pnpm 构建未完成，重跑「安装 DeepSeekHarness…」续装");
            }
            if (!bCordis) {
                if (!sMissing.isEmpty()) {
                    sMissing += QStringLiteral("；");
                }
                sMissing += QStringLiteral("cordis.yml（examples\\autocode-agent\\ 下）——程序目录缺 "
                                           "dsh\\cordis.autocode.yml，从完整程序目录拷来 dsh 子目录后"
                                           "重跑安装，或手动把该文件复制到上述路径");
            }
            return QStringLiteral("发现安装目录但未装全：%1\n缺：%2").arg(
                QDir::toNativeSeparators(sRoot), sMissing);
        }
    }
    return QStringLiteral("各探测位置（程序目录同级/上级）均无 DeepSeekHarness 安装目录——"
                          "克隆可能失败（网络/Git），请查看安装窗口的输出按提示处理后重装。");
}

void CDeepSeekRunner::SetDshNodePath(const QString& a_sPath) {
    s_sDshNodePath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshNodePath() {
    return s_sDshNodePath;
}

void CDeepSeekRunner::SetDshBinJsPath(const QString& a_sPath) {
    s_sDshBinJsPath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshBinJsPath() {
    return s_sDshBinJsPath;
}

void CDeepSeekRunner::SetDshCordisPath(const QString& a_sPath) {
    s_sDshCordisPath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshCordisPath() {
    return s_sDshCordisPath;
}

void CDeepSeekRunner::SetDshProvider(const QString& a_sProvider) {
    s_sDshProvider = a_sProvider.trimmed().isEmpty() ? QStringLiteral("glm-custom")
                                                     : a_sProvider.trimmed();
}

QString CDeepSeekRunner::DshProvider() {
    return s_sDshProvider.isEmpty() ? QStringLiteral("glm-custom") : s_sDshProvider;
}

void CDeepSeekRunner::SetDshModel(const QString& a_sModel) {
    s_sDshModel = a_sModel.trimmed().isEmpty() ? QStringLiteral("glm-5.2")
                                               : a_sModel.trimmed();
}

QString CDeepSeekRunner::DshModel() {
    return s_sDshModel.isEmpty() ? QStringLiteral("glm-5.2") : s_sDshModel;
}

void CDeepSeekRunner::SetDshPriceInPerM(double a_d) {
    s_dDshPriceInPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceCacheReadPerM(double a_d) {
    s_dDshPriceCacheReadPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceCacheWritePerM(double a_d) {
    s_dDshPriceCacheWritePerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceOutPerM(double a_d) {
    s_dDshPriceOutPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetCustomModels(const QVector<SCustomModel>& a_aModels, int a_iActive) {
    // 2026.08.19 热生效：先比「影响冷启动 env 的签名」（激活条目全字段，无激活=空串）。
    //   变化才重启空闲进程——设置页反复「确定」或只改非激活条目时不动既有会话。
    const int iNewActive = (a_iActive >= 0 && a_iActive < a_aModels.size()) ? a_iActive : -1;
    QString sNewSig;
    if (iNewActive >= 0) {
        const SCustomModel& stM = a_aModels.at(iNewActive);
        sNewSig = stM.sDisp + QChar(1) + stM.sBaseURL + QChar(1) + stM.sApiKey + QChar(1)
            + stM.sApi + QChar(1) + stM.sModelId + QChar(1) + QString::number(stM.iCtx)
            + QChar(1) + QString::number(stM.iMaxTok);
    }
    const bool bCfgChanged = (sNewSig != s_sCustomBootSig);
    s_aCustomModels = a_aModels;
    s_iCustomActive = iNewActive;
    s_sCustomBootSig = sNewSig;
    if (!bCfgChanged) {
        return;
    }
    for (CDeepSeekRunner* pR : s_aLiveRunners) {
        if (pR != nullptr) {
            pR->RestartForCfgChange();
        }
    }
}

void CDeepSeekRunner::RestartForCfgChange() {
    // 空闲（无轮次/非握手/非重试等待）→ 立即静默杀进程（kill 引发的 finished 由
    //   m_bTurnReported 吞掉，同 Stop 口径；sid 集随 MarkProcDead 作废，下轮换新 sid）。
    //   忙碌 → 只挂脏标，FinishTurn 落幕再杀（本轮结果完整送达，下一轮新配置冷启动）。
    if (m_eRunState == RunPrompting || m_eProcState == ProcBooting || m_bRetryPending) {
        m_bCfgDirty = true;
        m_iCfgDirtyGen = m_iProcGen;   // 记当代进程，收尾时只杀它
        return;
    }
    m_bCfgDirty = false;
    m_bTurnReported = true;   // 吞 kill 的 finished（下次 StartStage/ResetTurnState 复位）
    if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
    MarkProcDead();
}

bool CDeepSeekRunner::CustomActiveValid() {
    return s_iCustomActive >= 0 && s_iCustomActive < s_aCustomModels.size();
}

CDeepSeekRunner::CDeepSeekRunner(QObject* a_pParent)
    : CAgentRunnerBase(a_pParent) {
    s_aLiveRunners.insert(this);   // 2026.08.19 热生效：登记活实例（dtor 摘除）
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput, this, &CDeepSeekRunner::slotOnStdOut);
    // stderr 逐行上抛落 run.log（[stderr] 前缀）+ 尾部保留（失败消息附带）——同 claude 口径。
    connect(m_pProc, &QProcess::readyReadStandardError, this, &CDeepSeekRunner::slotOnStdErr);
    connect(m_pProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CDeepSeekRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred, this, &CDeepSeekRunner::slotOnErrorOccurred);
    // 停滞巡检定时器：常驻创建、Prompting 时武装（30s 一拍，超 StallMinutes 判挂起）。
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, [this]() {
        CheckStall();
    });
}

CDeepSeekRunner::~CDeepSeekRunner() {
    s_aLiveRunners.remove(this);
    Stop();
}

QString CDeepSeekRunner::NewSid() {
    // DSH 对 sid 无格式要求（探针用 autocode-probe-<hex>）；加 dsh- 前缀便于在
    // 任务元数据里一眼区分后端来源。
    return QStringLiteral("dsh-") + QUuid::createUuid().toString(QUuid::Id128);
}

void CDeepSeekRunner::ResetTurnState() {
    // 轮次统计清零（StartStage/InjectUserTurn 开头调用）。进程/会话/模型/窗口缓存
    //   是跨轮稳定量，不在此清（长驻设计）。
    m_bTurnReported = false;
    m_bTurnEnded = false;
    m_bRetryPending = false;
    m_iBootRetry = 0;
    m_iFastRetry = 0;
    m_sTurnText.clear();
    m_sTurnReasonKind.clear();
    m_iSumInTok = 0;
    m_iSumOutTok = 0;
    m_iSumPlainInTok = 0;
    m_iSumCacheReadTok = 0;
    m_iSumCacheWriteTok = 0;
    m_sLastSubtype.clear();
    m_iLastReqCount = 0;
    m_iLastContextFill = 0;
    m_aStderrTail.clear();
    m_buf.clear();
}

double CDeepSeekRunner::ComputeTurnCost() const {
    // 轮内 token 分量 × 单价折算（$/M）：DSH 协议只报 token 不报金额（与 claude CLI
    //   自带 total_cost_usd 不同），同一网关下按对齐价目估算；单价全 0 → 0（费用列回 "-"）。
    return (static_cast<double>(m_iSumPlainInTok) * s_dDshPriceInPerM
          + static_cast<double>(m_iSumCacheReadTok) * s_dDshPriceCacheReadPerM
          + static_cast<double>(m_iSumCacheWriteTok) * s_dDshPriceCacheWritePerM
          + static_cast<double>(m_iSumOutTok) * s_dDshPriceOutPerM) / 1000000.0;
}

void CDeepSeekRunner::StartStage(const QString& a_sWorkDir,
                                 const QString& a_sPrompt,
                                 const QString& a_sDisallowedTools,
                                 int a_iMaxTurns,
                                 const QString& a_sResumeSid,
                                 bool a_bFork,
                                 int a_iAutocompactK,
                                 bool a_bEnableAskUser) {
    ResetTurnState();

    // claude 专属参数降级提示（仅非默认值出现时提示一次，默认路径零噪音）。
    if (!a_sDisallowedTools.isEmpty() || a_iMaxTurns != 30 || a_iAutocompactK > 0
        || a_bEnableAskUser) {
        emit sigInfo(QStringLiteral(
            "[system/dsh] claude 专属参数（disallowedTools/maxTurns/autocompact/ask_user）"
            "在 DSH 后端不适用，已忽略（组合内置 compaction 自动压缩、工具默认放行）"));
    }

    // 启动前校验：缺失直接报失败（带完整处理指引，不拉进程）。统一走 DshConfigError，
    //   与启动警告/设置保存兜底同一口径——用户看到的第一条消息就说清怎么处理。
    const QString sCfgErr = DshConfigError();
    if (!sCfgErr.isEmpty()) {
        EmitFailed(sCfgErr);
        return;
    }

    // 会话判定：resumeSid 在本进程创建过且非 fork → 复用（同进程多轮=协议内续跑）；
    //   否则换新 sid（跨进程恢复是协议限制，上下文由上层 prompt 回灌弥补）。
    if (!a_sResumeSid.isEmpty() && !a_bFork
        && m_aLiveSids.contains(a_sResumeSid)
        && m_eProcState == ProcReady) {
        m_sSessionId = a_sResumeSid;
    } else {
        if (!a_sResumeSid.isEmpty()) {
            emit sigInfo(QStringLiteral(
                "[system/dsh] resumeSid=%1 不属于当前 DSH 进程（跨进程恢复是协议限制），"
                "换新会话续跑，上下文依赖 prompt 回灌").arg(a_sResumeSid));
        }
        m_sSessionId = NewSid();
        m_aLiveSids.insert(m_sSessionId);
    }
    m_sLastSessionId = m_sSessionId;
    m_sPromptWorkDir = a_sWorkDir;
    m_sPendingPrompt = a_sPrompt;
    m_eRunState = RunPrompting;   // 冷启动窗口也算占用（IsRunnerBusy 立即可见）

    if (m_eProcState == ProcReady) {
        BeginPrompt(m_sPendingPrompt);
        return;
    }
    EnsureProcess(a_sWorkDir);   // 失败走 EmitFailed（m_bTurnReported 守卫放行）
}

bool CDeepSeekRunner::EnsureProcess(const QString& a_sWorkDir) {
    if (m_eProcState == ProcBooting || m_eProcState == ProcReady) {
        return true;
    }
    // 部署位组合文件对账：模板（exe 同级 dsh\cordis.autocode.yml）有更新而部署副本
    //   还是旧的（老安装/手工拷贝），冷启动前覆盖——旧副本缺 autocode-active 槽位时
    //   自定义模型会静默失效。进程未运行（本函数前置条件），覆盖安全；失败不阻断
    //   （部署目录可能只读，旧副本仍可用默认路由）。
    SyncCordisFromMaster();
    // 收尾旧进程（Dead 残留状态清理；正常路径进程已退出）。
    if (m_pProc->state() != QProcess::NotRunning) {
        if (!m_pProc->waitForFinished(3000)) {
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
        }
    }
    // 环境注入：DSH_CWD（bash/fs-local 工作目录）+ DSH_SESSION_ROOT（会话 jsonl 落盘，
    //   放工程 .autodev 下与任务产物同域，不污染工程根）。
    QProcessEnvironment oEnv = QProcessEnvironment::systemEnvironment();
    oEnv.insert(QStringLiteral("DSH_CWD"), a_sWorkDir);
    oEnv.insert(QStringLiteral("DSH_SESSION_ROOT"),
                a_sWorkDir + QStringLiteral("/.autodev/dsh-sessions"));
    // 2026.08.18 自定义大模型：激活条目注入 llm-pi-ai 的 env 槽位（组合 cordis.yml 的
    //   autocode-active provider 逐字段读这些变量；密钥值走 credentials-local 的 env
    //   阶梯最优先级）。未启用（表空/Active 无效）时不设——组合回落 ?? 兜底默认值。
    if (CustomActiveValid()) {
        const SCustomModel& stM = s_aCustomModels.at(s_iCustomActive);
        oEnv.insert(QStringLiteral("DSH_LLM_NAME"),
                    stM.sDisp.isEmpty() ? QStringLiteral("AutoCode Active Model") : stM.sDisp);
        oEnv.insert(QStringLiteral("DSH_LLM_BASE_URL"), stM.sBaseURL);
        oEnv.insert(QStringLiteral("DSH_LLM_API"),
                    stM.sApi.isEmpty() ? QStringLiteral("anthropic-messages") : stM.sApi);
        oEnv.insert(QStringLiteral("DSH_LLM_MODEL"), stM.sModelId);
        oEnv.insert(QStringLiteral("DSH_LLM_CTX"), QString::number(stM.iCtx));
        oEnv.insert(QStringLiteral("DSH_LLM_MAXTOK"), QString::number(stM.iMaxTok));
        oEnv.insert(QStringLiteral("AUTOCODE_DSH_API_KEY"), stM.sApiKey);
        if (stM.iCtx > 0) {
            m_iCtxWindow = stM.iCtx;   // 窗口种子（request/context 上报后覆盖）
        }
    }
    // 技能库目录随用户走（组合 yml 的 customSkillDirs 从这里取；跨机器不再写死路径）。
    const QString sSkillsDir = QDir::homePath() + QStringLiteral("/.claude/skills");
    if (QDir(sSkillsDir).exists()) {
        oEnv.insert(QStringLiteral("DSH_SKILLS_DIR"), QDir::toNativeSeparators(sSkillsDir));
    }
    m_pProc->setProcessEnvironment(oEnv);
    m_pProc->setWorkingDirectory(a_sWorkDir);
    ++m_iProcGen;   // 2026.08.19 进程代次 +1（热生效守卫：脏标只杀配置变更前的旧进程）
    m_eProcState = ProcBooting;
    m_uptime.restart();
    m_actClock.restart();
    // node 直起（不经 cmd.exe：参数化传参，无 shim 引号重解析问题；Windows 路径含
    //   空格安全）。stdin 全程不关（铁律：bin.js 对 stdin end 敏感）。
    m_pProc->start(ResolveNodePath(), QStringList{ResolveBinJsPath(), ResolveCordisPath()});
    if (!m_pProc->waitForStarted(10000)) {
        m_eProcState = ProcDead;
        EmitFailed(QStringLiteral("无法启动 DSH 进程：%1 %2 %3").arg(
            DshNodePath(), DshBinJsPath(), DshCordisPath()));
        return false;
    }
    // initialize 握手：cwd/provider/model（maxTokens 不传——组合 maxTokensAsSuccess=false，
    //   token 封顶以 turn/end reason=max-tokens 上报，续跑语义靠它）。
    m_iInitId = m_iNextRpcId++;
    QJsonObject oParams;
    oParams.insert(QStringLiteral("cwd"), a_sWorkDir);
    oParams.insert(QStringLiteral("provider"), EffectiveProvider());
    oParams.insert(QStringLiteral("model"), EffectiveModel());
    SendRequest(QStringLiteral("initialize"), oParams, m_iInitId);
    // 握手超时 30s：重试一次（重启进程），再失败判死。重启由本 lambda 独占——
    //   m_bRebooting 拦住 kill 触发的 slotOnFinished Booting 重试分支（防双重重启）。
    QTimer::singleShot(30000, this, [this]() {
        if (m_eProcState != ProcBooting || m_bTurnReported) {
            return;
        }
        if (m_iBootRetry >= 1) {
            EmitFailed(QStringLiteral("DSH initialize 握手超时（30 秒×2），进程已终止。"
                                      "请检查组合文件与网络（中继 %1）").arg(DshProvider()));
            return;
        }
        ++m_iBootRetry;
        emit sigInfo(QStringLiteral("↻ DSH initialize 握手超时（30 秒），重启进程重试…"));
        m_bRebooting = true;
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
        m_bRebooting = false;
        MarkProcDead();
        EnsureProcess(m_sPromptWorkDir);
    });
    return true;
}

QString CDeepSeekRunner::EffectiveProvider() const {
    // 自定义模型启用 → 固定发组合 yml 的 env 槽位名；否则走 [Agent] DshProvider。
    return CustomActiveValid() ? QStringLiteral("autocode-active") : DshProvider();
}

QString CDeepSeekRunner::EffectiveModel() const {
    return CustomActiveValid() ? s_aCustomModels.at(s_iCustomActive).sModelId : DshModel();
}

void CDeepSeekRunner::SyncCordisFromMaster() {
    // 模板（exe 同级 dsh\cordis.autocode.yml，构建从仓库 agent/dsh 同步）与部署副本
    //   （ResolveCordisPath）逐字节比对，不同则覆盖部署副本。部署副本是生成物（头注释
    //   标明正本位置），覆盖安全；只读部署目录等失败仅提示不阻断（旧副本仍可跑默认路由）。
    const QString sMaster = QCoreApplication::applicationDirPath()
        + QStringLiteral("/dsh/cordis.autocode.yml");
    const QString sDeployed = ResolveCordisPath();
    if (sMaster.isEmpty() || sDeployed.isEmpty() || !QFile::exists(sMaster)) {
        return;
    }
    QFile oMaster(sMaster);
    if (!oMaster.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray aMaster = oMaster.readAll();
    oMaster.close();
    QFile oDeployed(sDeployed);
    if (oDeployed.open(QIODevice::ReadOnly)) {
        const QByteArray aOld = oDeployed.readAll();
        oDeployed.close();
        if (aOld == aMaster) {
            return;   // 已同步
        }
    }
    if (oDeployed.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        oDeployed.write(aMaster);
        oDeployed.close();
        emit sigInfo(QStringLiteral("[system/dsh] 组合文件已从程序目录模板刷新：%1")
                         .arg(QDir::toNativeSeparators(sDeployed)));
    } else {
        emit sigInfo(QStringLiteral("[system/dsh] 组合文件模板有更新但部署副本写入失败"
                                    "（可能只读）：%1").arg(QDir::toNativeSeparators(sDeployed)));
    }
}

void CDeepSeekRunner::SendRequest(const QString& a_sMethod, const QJsonObject& oParams,
                                  int a_iId) {
    if (m_pProc->state() == QProcess::NotRunning) {
        return;   // 进程已死：写入无意义（退出路径自会收尾）
    }
    QJsonObject oFrame;
    oFrame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    oFrame.insert(QStringLiteral("id"), a_iId > 0 ? a_iId : m_iNextRpcId++);
    oFrame.insert(QStringLiteral("method"), a_sMethod);
    if (!oParams.isEmpty()) {
        oFrame.insert(QStringLiteral("params"), oParams);
    }
    const QByteArray bufFrame = QJsonDocument(oFrame).toJson(QJsonDocument::Compact) + '\n';
    m_pProc->write(bufFrame);
    m_pProc->waitForBytesWritten(500);   // 及时冲出（铁律：绝不 closeWriteChannel）
}

void CDeepSeekRunner::SendPrompt(const QString& a_sText) {
    QJsonObject oBlock;
    oBlock.insert(QStringLiteral("type"), QStringLiteral("text"));
    oBlock.insert(QStringLiteral("text"), a_sText);
    QJsonArray aBlocks;
    aBlocks.append(oBlock);
    QJsonObject oParams;
    oParams.insert(QStringLiteral("sessionId"), m_sSessionId);
    oParams.insert(QStringLiteral("contentBlocks"), aBlocks);
    m_iPromptId = m_iNextRpcId++;   // 记下 id 供响应配对（受理即回，错误即失败）
    SendRequest(QStringLiteral("session/prompt"), oParams, m_iPromptId);
}

void CDeepSeekRunner::MarkProcDead() {
    // 进程死亡的统一落点：置 Dead + 清本进程 sid 集合。集合标记的是「当前活进程
    //   创建过的 sid」——新进程复用旧 sid 会被持久化层判 id collision（磁盘 jsonl
    //   不是新 live 会话的前缀），必须随进程死亡一并作废。
    m_eProcState = ProcDead;
    m_aLiveSids.clear();
}

void CDeepSeekRunner::BeginPrompt(const QString& a_sPrompt) {
    // 武装计时 + 投递（Ready 直发 / 握手后补发 / 注入轮三路径共用；文本显式传入）。
    m_actClock.restart();
    m_uptime.restart();
    if (m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    SendPrompt(a_sPrompt);
}

void CDeepSeekRunner::Stop() {
    // 静默收尾（同 claude 语义）：先置终态再 kill——kill 触发的 finished/事件一律吞掉，
    //   不发 sigFinished；上层 StopTask 直接落 Stopped。DSH 无逐轮取消，只能杀进程；
    //   进程死后下轮 StartStage 冷启动换新 sid（协议限制，StartStage 内有提示）。
    m_bTurnReported = true;
    m_bRetryPending = false;
    m_eRunState = RunIdle;
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
    MarkProcDead();
}

bool CDeepSeekRunner::InjectUserTurn(const QString& a_sText) {
    // DSH 协议无「轮次中插话」：仅在进程 Ready 且无轮次进行中时，作为新一轮
    //   session/prompt（同 sid 续轮）发出；否则安全 no-op 返回 false。
    if (a_sText.isEmpty() || m_eProcState != ProcReady || m_eRunState != RunIdle
        || m_sSessionId.isEmpty()) {
        return false;
    }
    ResetTurnState();
    m_eRunState = RunPrompting;
    BeginPrompt(a_sText);   // 2026.08.18 实测修复：必须显式传注入文本（曾误发上一轮 m_sPendingPrompt 旧值）
    return true;
}

void CDeepSeekRunner::slotOnStdOut() {
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

void CDeepSeekRunner::slotOnStdErr() {
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

void CDeepSeekRunner::HandleLine(const QByteArray& a_sLine) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        emit sigInfo(QString::fromUtf8(a_sLine));   // 非 JSON 行原样上抛（排查用）
        return;
    }
    const QJsonObject oRoot = stDoc.object();
    // 响应（有 id 且含 result/error）与通知（有 method）分流。
    if (oRoot.contains(QStringLiteral("id"))
        && (oRoot.contains(QStringLiteral("result")) || oRoot.contains(QStringLiteral("error")))) {
        HandleResponse(oRoot.value(QStringLiteral("id")).toInt(), oRoot);
        return;
    }
    if (oRoot.contains(QStringLiteral("method"))) {
        HandleNotify(oRoot);
        return;
    }
    emit sigInfo(QString::fromUtf8(a_sLine));
}

void CDeepSeekRunner::HandleResponse(int a_iId, const QJsonObject& oRoot) {
    if (a_iId == m_iInitId) {
        // initialize 响应：error 说明组合/配置问题（重试无益，直接失败）。
        if (oRoot.contains(QStringLiteral("error"))) {
            const QJsonObject oErr = oRoot.value(QStringLiteral("error")).toObject();
            m_bRebooting = true;   // kill 触发的 slotOnFinished 走 Booting 重试分支，拦下
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
            m_bRebooting = false;
            MarkProcDead();
            EmitFailed(QStringLiteral("DSH initialize 失败：%1 %2").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString(),
                oErr.value(QStringLiteral("message")).toString()));
            return;
        }
        m_eProcState = ProcReady;
        const QJsonObject oInfo = oRoot.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("serverInfo")).toObject();
        emit sigInfo(QStringLiteral("[system/dsh-ready] server=%1/%2 provider=%3 model=%4").arg(
            oInfo.value(QStringLiteral("name")).toString(),
            oInfo.value(QStringLiteral("version")).toString(),
            DshProvider(), DshModel()));
        // 冷启动路径：握手成功后补发暂存的 prompt（Ready 直发路径不走这里）。
        if (m_eRunState == RunPrompting && !m_sPendingPrompt.isEmpty()) {
            BeginPrompt(m_sPendingPrompt);
        }
        return;
    }
    if (a_iId == m_iPromptId) {
        // session/prompt 响应：仅代表「受理」；轮次完成靠事件流（turn/end + idle）。
        if (oRoot.contains(QStringLiteral("error"))) {
            const QJsonObject oErr = oRoot.value(QStringLiteral("error")).toObject();
            EmitFailed(QStringLiteral("DSH session/prompt 失败：%1 %2").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString(),
                oErr.value(QStringLiteral("message")).toString()));
        }
        return;
    }
    // 未知响应（不应出现）：上抛便于排查。
    emit sigInfo(QString::fromUtf8(QJsonDocument(oRoot).toJson(QJsonDocument::Compact)));
}

void CDeepSeekRunner::HandleNotify(const QJsonObject& oRoot) {
    const QString sMethod = oRoot.value(QStringLiteral("method")).toString();
    const QJsonObject oParams = oRoot.value(QStringLiteral("params")).toObject();
    if (sMethod == QStringLiteral("session.event")) {
        HandleSessionEvent(oParams);
        return;
    }
    if (sMethod == QStringLiteral("session.status")) {
        // idle 到达且 turn 已结束 → 收尾（idle 实测在 turn/end 后到达，trailing 事件已吃完）。
        const QString sStatus = oParams.value(QStringLiteral("status")).toString();
        if (sStatus == QStringLiteral("idle") && m_bTurnEnded) {
            FinishTurn();
        }
        return;
    }
    if (sMethod == QStringLiteral("subagent.started")
        || sMethod == QStringLiteral("subagent.finished")) {
        emit sigInfo(QStringLiteral("[system/%1] session=%2").arg(
            sMethod.section(QLatin1Char('.'), -1),
            oParams.value(QStringLiteral("sessionId")).toString()));
        return;
    }
    // 未知通知：原样上抛（协议容错——rc.7 无兼容承诺，未知 method 不崩）。
    emit sigInfo(QString::fromUtf8(QJsonDocument(oRoot).toJson(QJsonDocument::Compact)));
}

void CDeepSeekRunner::HandleSessionEvent(const QJsonObject& oParams) {
    // 只处理当前会话的事件（多 runner 共存时 DSH 各自独立进程，正常只会有自己 sid；
    //   防御性过滤仍保留——sid 不匹配直接忽略）。
    const QString sSid = oParams.value(QStringLiteral("sessionId")).toString();
    if (sSid != m_sSessionId) {
        return;
    }
    const QJsonObject oEvent = oParams.value(QStringLiteral("event")).toObject();
    const QString sType = oEvent.value(QStringLiteral("type")).toString();
    // payload 直接内联（阶段 0 实测无 data 包裹；带 data 键的形态亦兼容）。
    QJsonObject oData = oEvent.value(QStringLiteral("data")).toObject();
    if (oData.isEmpty()) {
        oData = oEvent;
    }

    if (sType == QStringLiteral("assistant/chunk")) {
        return;   // token 级流：量大且无复盘价值，源头丢弃（同 claude thinking_tokens 口径）
    }
    if (sType == QStringLiteral("assistant/message")) {
        ++m_iLastReqCount;   // 每条 assistant 消息 = 一次 API 调用（同 claude 口径）
        // usage：轮内 Σ 输入/输出（账单口径含 cache 读/写）；上下文占用取本条的
        //   input 三项之和（脏值 >2×窗口丢弃——中继偶发异常巨大 usage，claude 侧实测）。
        const QJsonObject oUsage = oData.value(QStringLiteral("usage")).toObject();
        if (!oUsage.isEmpty()) {
            const qint64 iPlainIn = oUsage.value(QStringLiteral("inputTokens")).toInteger();
            const qint64 iCacheRead = oUsage.value(QStringLiteral("cacheReadTokens")).toInteger();
            const qint64 iCacheWrite = oUsage.value(QStringLiteral("cacheWriteTokens")).toInteger();
            const qint64 iIn = iPlainIn + iCacheRead + iCacheWrite;
            const qint64 iOut = oUsage.value(QStringLiteral("outputTokens")).toInteger()
                + oUsage.value(QStringLiteral("reasoningTokens")).toInteger();
            m_iSumInTok += iIn;
            m_iSumOutTok += iOut;
            // 计费分量分开累计（三项单价不同，见 ComputeTurnCost）
            m_iSumPlainInTok += iPlainIn;
            m_iSumCacheReadTok += iCacheRead;
            m_iSumCacheWriteTok += iCacheWrite;
            const qint64 iWinCap = 2 * (m_iCtxWindow > 0
                ? m_iCtxWindow : CClaudeRunner::ContextWindowForModel(m_sLastModel));
            if (iIn > 0 && iIn != m_iLastContextFill && iIn <= iWinCap) {
                m_iLastContextFill = iIn;
                emit sigContextFill(iIn);
            }
        }
        // 文本块上抛 + 记本轮最后文本（thinking 等其他块跳过）。
        const QJsonArray aContent = oData.value(QStringLiteral("message")).toObject()
                                        .value(QStringLiteral("content")).toArray();
        for (const QJsonValue& v : aContent) {
            const QJsonObject oItem = v.toObject();
            if (oItem.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                const QString sText = oItem.value(QStringLiteral("text")).toString();
                if (!sText.isEmpty()) {
                    m_sTurnText = sText;
                    emit sigAssistantText(sText);
                }
            }
        }
        return;
    }
    if (sType == QStringLiteral("tool/call")) {
        const QString sArgs = oData.value(QStringLiteral("arguments")).toString();
        emit sigToolUse(oData.value(QStringLiteral("name")).toString(),
                        sArgs.isEmpty() ? QStringLiteral("{}") : sArgs);
        return;
    }
    if (sType == QStringLiteral("tool/result")) {
        // message.content 兼容字符串 / [{type:text,text:...}] 两种形态；error 附加。
        const QJsonValue vContent = oData.value(QStringLiteral("message")).toObject()
                                        .value(QStringLiteral("content"));
        QString sResult;
        if (vContent.isString()) {
            sResult = vContent.toString();
        } else if (vContent.isArray()) {
            for (const QJsonValue& vc : vContent.toArray()) {
                sResult += vc.toObject().value(QStringLiteral("text")).toString();
            }
        }
        const QJsonObject oErr = oData.value(QStringLiteral("error")).toObject();
        if (!oErr.isEmpty()) {
            sResult += QStringLiteral(" [错误:%1]").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString());
        }
        if (sResult.size() > 400) {
            sResult = sResult.left(400) + QStringLiteral("...");
        }
        emit sigToolResult(sResult);
        return;
    }
    if (sType == QStringLiteral("user/message")) {
        return;   // 用户轮回显（AutoCode 侧自知内容），忽略
    }
    if (sType == QStringLiteral("turn/end")) {
        m_bTurnEnded = true;
        m_sTurnReasonKind = oData.value(QStringLiteral("reason")).toObject()
                                .value(QStringLiteral("kind")).toString();
        // 完成判定严格门控 idle：turn/end 只说明轮次逻辑结束，server 内部收尾可能
        //   未完，此刻投递下一轮有竞态风险。idle 正常在 turn/end 后数秒内到达；
        //   5s 未到仅提示（idle 永不到的异常形态交 stall 巡检兜底 kill，不在此收尾）。
        QTimer::singleShot(5000, this, [this]() {
            if (m_bTurnEnded && m_eRunState == RunPrompting && !m_bTurnReported) {
                emit sigInfo(QStringLiteral(
                    "[system/dsh-idle-wait] turn/end 已到达 5 秒仍未见 idle，继续等待（stall 兜底）"));
            }
        });
        return;
    }
    if (sType == QStringLiteral("request/context")) {
        // provider/model/上下文窗口（比模型名猜测准，优先缓存）。
        const QString sProvider = oData.value(QStringLiteral("provider")).toString();
        const QString sModel = oData.value(QStringLiteral("model")).toString();
        if (!sModel.isEmpty()) {
            m_sLastModel = sProvider.isEmpty() ? sModel
                                               : sProvider + QLatin1Char('/') + sModel;
        }
        const qint64 iWin = oData.value(QStringLiteral("contextWindow")).toInteger();
        if (iWin > 0) {
            m_iCtxWindow = iWin;
        }
        return;
    }
    // 未知事件：[system/<type>] 上抛（不上屏只落日志的口径与 claude system 行一致）。
    emit sigInfo(QStringLiteral("[system/dsh-%1] session=%2").arg(sType, sSid));
}

void CDeepSeekRunner::FinishTurn() {
    if (m_eRunState != RunPrompting || m_bTurnReported || !m_bTurnEnded) {
        return;   // 幂等守卫：idle 与 5s 兜底双入口只收一次；turn/end 未到不收尾
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    m_eRunState = RunIdle;
    m_bTurnReported = true;
    const QString sKind = m_sTurnReasonKind;
    bool bOk = false;
    QString sSub;
    QString sMsg = m_sTurnText;
    if (sKind == QStringLiteral("completed")) {
        bOk = true;
        sSub = QStringLiteral("success");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("(本轮无文本输出)");
        }
    } else if (sKind == QStringLiteral("max-tokens")) {
        // 复用 claude 词汇：上层 LastWasMaxTurns() → 自动以同 sid 再 StartStage =
        // 同进程续轮（DSH 强项：上下文完整保留）。
        sSub = QStringLiteral("error_max_turns");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("达到输出 token 上限（max-tokens），转自动续跑");
        }
    } else if (sKind == QStringLiteral("aborted")) {
        sSub = QStringLiteral("aborted");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("轮次被中止（aborted）");
        }
    } else {
        sSub = sKind.isEmpty() ? QStringLiteral("error") : sKind;
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("无错误文本（turn/end reason=%1）").arg(sSub);
        }
    }
    m_sLastSubtype = sSub;
    emit sigFinished(bOk, sMsg, m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sSessionId);
    // 2026.08.19 配置热生效：轮次中收到自定义模型变更（RestartForCfgChange 挂的脏标），
    //   本轮已完整送达，落幕后杀旧进程——下一轮以新配置冷启动（免重启 AutoCode）。
    //   两类竞态守卫：①emit 期间编排器已冷启动新代进程（=新配置）→ 不动；②emit 期间
    //   编排器在旧进程上同步续跑了下一轮 → 保留脏标，那一轮落幕后（下次 FinishTurn）再杀。
    if (m_bCfgDirty) {
        if (m_iProcGen != m_iCfgDirtyGen) {
            m_bCfgDirty = false;   // 已是新代进程（新配置冷启动过），无事可做
        } else if (m_eRunState == RunPrompting || m_eProcState == ProcBooting) {
            ;   // 旧进程已续跑新一轮：不杀（打断本轮），脏标留到下次 FinishTurn
        } else {
            m_bCfgDirty = false;
            if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
                m_pProc->kill();
                m_pProc->waitForFinished(3000);
                MarkProcDead();
            }
        }
    }
}

void CDeepSeekRunner::CheckStall() {
    // 停滞检测（同 claude 语义）：进程活着但距上次收到任何输出超 [Agent]
    //   StallMinutes（默认 10 分钟）→ 判挂起，kill 进程并以 subtype="stall" 报失败，
    //   上层凭 lastSessionId 自动续跑（DSH 侧进程已死 → 换新 sid，StartStage 有提示）。
    if (m_eRunState != RunPrompting || m_bTurnReported || m_bRetryPending) {
        return;   // 无轮次 / 已收尾 / 重试等待中
    }
    if (m_pProc->state() == QProcess::NotRunning) {
        return;   // 进程已退出，交给 finished 路径
    }
    const qint64 iLimitMs = static_cast<qint64>(CClaudeRunner::StallMinutes()) * 60000;
    if (m_actClock.elapsed() < iLimitMs) {
        return;
    }
    if (m_pStallTimer != nullptr) {
        m_pStallTimer->stop();
    }
    m_sLastSubtype = QStringLiteral("stall");
    emit sigInfo(QStringLiteral("⚠ 停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），"
                                "终止 DSH 进程并转自动续跑…").arg(CClaudeRunner::StallMinutes()));
    // 先置终态再 kill（静默吞掉后续事件，同 Stop）。
    m_bTurnReported = true;
    m_eRunState = RunIdle;
    m_pProc->kill();
    m_pProc->waitForFinished(3000);
    MarkProcDead();
    emit sigFinished(false,
                     QStringLiteral("停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），进程已终止")
                         .arg(CClaudeRunner::StallMinutes()),
                     m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sSessionId);
}

void CDeepSeekRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    Q_UNUSED(a_eStatus);
    const EProcState ePrev = m_eProcState;
    MarkProcDead();
    if (m_bTurnReported) {
        return;   // Stop/stall/已收尾：静默吞掉
    }
    if (m_bRebooting) {
        return;   // 握手超时/init 错误路径的主动重启：重启由发起方独占，此处静默
    }
    if (ePrev == ProcBooting) {
        // 握手阶段自行退出：进程参数/组合加载问题——重启重试一次，再失败判死。
        if (m_iBootRetry < 1) {
            ++m_iBootRetry;
            emit sigInfo(QStringLiteral("↻ DSH 进程在握手阶段退出（退出码=%1），重启重试…").arg(a_iExitCode));
            QTimer::singleShot(5000, this, [this]() {
                if (!m_bTurnReported) {
                    EnsureProcess(m_sPromptWorkDir);
                }
            });
            return;
        }
        EmitFailed(QStringLiteral("DSH 进程在握手阶段反复退出（退出码=%1），"
                                  "请检查 node/bin.js/cordis.yml 路径与组合文件").arg(a_iExitCode));
        return;
    }
    if (m_eRunState != RunPrompting) {
        return;   // Idle 时的意外退出：不属本轮执行，下轮 StartStage 冷启动自愈
    }
    // Prompting 中退出且未收 turn/end：快速失败自动重试（同 claude——短命退出多为
    //   瞬态故障，5s 后重启进程 + 重发 prompt；上限 2 次）。
    if (m_uptime.isValid() && m_uptime.elapsed() < 15000 && m_iFastRetry < 2) {
        ++m_iFastRetry;
        m_bRetryPending = true;
        emit sigInfo(QStringLiteral("DSH 进程 %1 秒内即退出（退出码=%2），5 秒后自动重试（第 %3/2 次）")
                         .arg(m_uptime.elapsed() / 1000.0, 0, 'f', 1)
                         .arg(a_iExitCode)
                         .arg(m_iFastRetry));
        QTimer::singleShot(5000, this, [this]() {
            RelaunchStage();
        });
        return;
    }
    EmitFailed(QStringLiteral("DSH 进程异常退出，退出码=%1").arg(a_iExitCode));
}

void CDeepSeekRunner::RelaunchStage() {
    // 快速失败重试：重启进程 + 握手 + 重发 prompt（期间被 Stop/新一轮 StartStage 则作废）。
    m_bRetryPending = false;
    if (m_bTurnReported || m_sPendingPrompt.isEmpty()) {
        return;
    }
    emit sigInfo(QStringLiteral("↻ 自动重试 DSH 调用（第 %1/2 次）…").arg(m_iFastRetry));
    m_sLastSubtype.clear();
    EnsureProcess(m_sPromptWorkDir);   // 握手响应到达后自动 BeginPrompt 补发
}

void CDeepSeekRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    if (m_bTurnReported) {
        return;
    }
    QString sMsg;
    switch (a_eError) {
        case QProcess::FailedToStart: sMsg = QStringLiteral("DSH 启动失败（node 路径无效或无执行权限）"); break;
        case QProcess::Crashed:       sMsg = QStringLiteral("DSH 进程崩溃"); break;
        case QProcess::Timedout:      sMsg = QStringLiteral("DSH 进程超时"); break;
        case QProcess::ReadError:     sMsg = QStringLiteral("读取 DSH 输出错误"); break;
        case QProcess::WriteError:    sMsg = QStringLiteral("写入 DSH 输入错误"); break;
        default:                      sMsg = QStringLiteral("DSH 进程未知错误"); break;
    }
    // FailedToStart 不会触发 finished 信号，须在此直接收尾；其余错误等 finished 路径
    //   （CrashExit 会在 slotOnFinished 里走快速重试/EmitFailed）。
    if (a_eError == QProcess::FailedToStart) {
        m_eProcState = ProcDead;
        EmitFailed(sMsg);
    }
}

void CDeepSeekRunner::EmitFailed(const QString& a_sMsg) {
    if (m_bTurnReported) {
        return;
    }
    // 失败消息附带 stderr 尾部 + sid（上层失败后仍可续跑）——同 claude 口径。
    QString sMsg = a_sMsg;
    if (!m_aStderrTail.isEmpty()) {
        QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
        if (sTail.size() > 800) {
            sTail = sTail.left(800) + QStringLiteral("…");
        }
        sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
    }
    m_bTurnReported = true;
    m_eRunState = RunIdle;
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    emit sigInfo(QStringLiteral("[错误] ") + sMsg);
    emit sigFinished(false, sMsg, m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sSessionId);
}
