#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "CAgentRunnerBase.h"

// DeepSeekHarness（DSH）JSON-RPC 后端 runner（2026.08.18 多后端支持）。
//
// 进程模型与 claude 后端根本不同：**长驻进程**。首次 StartStage 冷启动
// `node <bin.js> <cordis.yml>`，经 stdio 换行分帧 JSON-RPC 2.0 通信
// （initialize 握手 → session/prompt → 事件流通知），阶段完成后进程不退出，
// 复用同一进程/同一 sessionId 即延续多轮会话（同进程多轮记忆，阶段 0 实测通过）。
//
// 与 claude 后端的语义对齐（上层编排器零改动）：
//   - 完成判定：turn/end(reason.kind) + session.status idle 双条件（idle 未到挂
//     5s 兜底），映射到 sigFinished；reason=max-tokens → subtype="error_max_turns"
//     （复用 claude 词汇，上层 LastWasMaxTurns 自动续跑零改动——同进程同 sid 再
//     prompt 即完美续跑）。
//   - usage：assistant/message 逐条自带 usage（inputTokens/outputTokens/
//     cacheReadTokens[/cacheWriteTokens]），Σ 为轮内输入/输出；上下文占用取最后
//     一条的 input 三项之和（脏值 >2×窗口丢弃，同 claude 口径）。
//   - Stop 静默收尾（先置终态再 kill，不发 sigFinished，上层 StopTask 落 Stopped）。
//   - stall 巡检（30s 拍，零输出超 [Agent] StallMinutes → kill + subtype="stall"，
//     上层凭 sessionId 续跑）。快速失败重试（prompt 中短命退出 → 5s 后重启进程
//     重发，上限 2 次）。孤儿宽限不移植（IsGraceActive 恒 false）。
//
// 两条实证铁律（阶段 0 验证，违反即挂）：
//   1. stdin 全程保持打开——bin.js 对 stdin end 敏感（收到即 disposeAndExit），
//      绝不能 closeWriteChannel（与 claude 后端习惯相反）。
//   2. cordis.yml 的裸插件名从配置文件所在目录解析 node_modules，组合文件必须
//      位于 DSH 仓库 examples/ 树内（路径由 [Agent] DshCordisPath 指向运行副本）。
//
// 跨进程会话限制（协议）：SDK server 只 agents.create 不 resume，持久化层要求
// 磁盘日志是 live 会话的前缀才采纳，否则 id collision。故 resumeSid 仅在本进程
// 创建过（m_aLiveSids）时复用；跨进程（AutoCode 重启/进程被杀后续跑）自动换新
// sid 并提示（上下文靠上层 prompt 回灌，协议所限）。
//
// claude 专属参数降级：a_sDisallowedTools / a_iMaxTurns / a_iAutocompactK /
// a_bEnableAskUser 在本后端忽略（组合已带 compaction-basic 自动压缩；工具默认
// 放行语义与 claude 全开一致）；a_bFork 语义映射为「以新 sid 分支」（DSH 无
// fork 概念，效果等价：旧 sid 完整保留，撤销链回退用）。
class CDeepSeekRunner : public CAgentRunnerBase {
    Q_OBJECT

public:
    explicit CDeepSeekRunner(QObject* a_pParent = nullptr);
    ~CDeepSeekRunner() override;

    // —— 进程级全局配置（config.ini [Agent] Dsh*；所有 DSH runner 实例共享）——
    // 2026.08.18 多后端：node.exe / bin.js / cordis.yml 三路径 + provider/model。
    //   路径默认空（未配置时 StartStage 前置校验直接报失败并提示去设置页配置）；
    //   provider/model 默认 glm-custom / glm-5.2（内网中继 anthropic-messages API）。
    // 2026.08.18 轻量自动配置：DSH 根目录单字段（deepseek-harness 仓库根）。root 非空时
    //   按固定布局推导 bin.js（packages/examples/jsonrpc-demo/lib/bin.js）与 cordis.yml
    //   （examples/autocode-agent/cordis.yml）；显式路径键非空时优先（旧档回退）。
    //   node 留空时走 PATH 的 "node"（本机装了 node 即零配置）。
    static void SetDshRootPath(const QString& a_sRoot);
    static QString DshRootPath();
    // 三个生效路径（显式键 → root 推导 → node 回退 "node"）。
    static QString ResolveNodePath();
    static QString ResolveBinJsPath();
    static QString ResolveCordisPath();
    // 候选根目录判定：推导出的 bin.js 与 cordis.yml 都存在才认（=「完整可用」；
    //   启动/设置页探测同级 DSH 仓库、设置页后端下拉可用性都用它）。
    static bool LooksLikeDshRoot(const QString& a_sRoot);
    // 2026.08.18 可用性统一校验（启动警告/设置保存兜底/StartStage 前置三处同一口径）：
    //   返回空串 = 可用；非空 = 第一处问题的完整处理指引（含一键安装路径）。
    static QString DshConfigError();
    // 探测 exe 同级/上级的 DSH 仓库根（exeDir{, /.., /../..} × {/DeepSeekHarness/
    //   deepseek-harness, /deepseek-harness}；install_env.bat 的固定安装布局：exe
    //   同级子目录最优先 = 随程序目录整体拷走的绿色便携位，上两级为旧布局兼容）。
    //   命中返回 canonical 根，未找到返回空串。无副作用、可反复调用。
    static QString ProbeSiblingRoot();
    // 2026.08.18 半安装态诊断（「重新检测」失败提示用）：按探测布局找**目录存在但
    //   bin.js/cordis.yml 缺其一**的根，报具体缺项与处理指引；各处均无目录则提示
    //   看安装窗口输出（克隆/构建失败）。
    static QString DshInstallDiagnosis();

    static void SetDshNodePath(const QString& a_sPath);
    static QString DshNodePath();
    static void SetDshBinJsPath(const QString& a_sPath);
    static QString DshBinJsPath();
    static void SetDshCordisPath(const QString& a_sPath);
    static QString DshCordisPath();
    static void SetDshProvider(const QString& a_sProvider);
    static QString DshProvider();
    static void SetDshModel(const QString& a_sModel);
    static QString DshModel();
    // 2026.08.18 DSH 计费估算：DSH 协议只报 token 分量不报金额，按单价（$/百万 token）
    //   折算——默认对齐 claude 后端经同一网关显示的 Sonnet 档价目（3/0.3/3.75/15），
    //   实际网关计费不同则改 config.ini [Agent] DshPrice*。全 0 = 不折算（费用列回 "-"）。
    static void SetDshPriceInPerM(double a_d);
    static void SetDshPriceCacheReadPerM(double a_d);
    static void SetDshPriceCacheWritePerM(double a_d);
    static void SetDshPriceOutPerM(double a_d);

    // —— 自定义大模型（2026.08.18 多条目管理，跨机器自配 LLM）——
    // 组合 cordis.yml 的 llm-pi-ai 内置 env 驱动槽位 "autocode-active"：本表激活条目
    //   在冷启动时注入 DSH_LLM_BASE_URL/API/MODEL/CTX/MAXTOK/NAME + 密钥值
    //   AUTOCODE_DSH_API_KEY（credentials-local 的 env 阶梯最优先），initialize 的
    //   provider/model 自动改发 autocode-active/<条目模型>。表空或 Active 无效 =
    //   未启用，走 [Agent] DshProvider/DshModel 旧路径（glm-custom 固定路由）。
    struct SCustomModel {
        QString sDisp;      // 显示名（进 DSH_LLM_NAME）
        QString sBaseURL;   // 接入点（anthropic-messages 兼容）
        QString sApiKey;    // 密钥明文（随进程 env 注入；config.ini 也是明文，tooltip 已告知）
        QString sApi;       // API 类型（默认 anthropic-messages）
        QString sModelId;   // 模型 id（initialize 的 model）
        qint64 iCtx         = 200000;   // 上下文窗口（token）
        int    iMaxTok      = 32768;    // 单次最大输出
    };
    static void SetCustomModels(const QVector<SCustomModel>& a_aModels, int a_iActive);
    static bool CustomActiveValid();   // 激活条目有效（表非空且下标合法）

    QString BackendName() const override { return QStringLiteral("dsh"); }

    // 启动一次阶段执行：进程未就绪则冷启动（握手后自动投递 prompt），Ready 则
    //   直接同进程投递（resumeSid 复用/新建见类注释）。
    void StartStage(const QString& a_sWorkDir,
                    const QString& a_sPrompt,
                    const QString& a_sDisallowedTools,
                    int a_iMaxTurns,
                    const QString& a_sResumeSid,
                    bool a_bFork,
                    int a_iAutocompactK,
                    bool a_bEnableAskUser) override;

    // 终止当前执行：静默收尾（置终态 → kill 进程 → 不发 sigFinished）。
    //   DSH 协议无逐轮取消，Stop 只能杀进程；进程死后下轮 StartStage 冷启动
    //   并换新 sid（跨进程恢复是协议限制）。
    void Stop() override;

    // 轮次进行中（prompt 已投递、turn/end 尚未确认）。进程 Ready 但无轮次时不
    //   算占用——上层 IsRunnerBusy 依赖此语义。
    bool IsRunning() const override { return m_eRunState == RunPrompting; }

    // 孤儿宽限不移植：node 直起（无 cmd.exe 中间层），进程死即真死。
    bool IsGraceActive() const override { return false; }

    // 快速失败重试等待中（5 秒延迟未到）：仍占用 runner。
    bool IsRetryPending() const override { return m_bRetryPending; }

    // 向当前会话追加一轮用户文本：仅进程 Ready 且无轮次进行中时作为新一轮
    //   session/prompt 发出（DSH 不支持轮次中插话，运行中返回 false 安全 no-op）。
    bool InjectUserTurn(const QString& a_sText) override;

private slots:
    void slotOnStdOut();
    void slotOnStdErr();
    void slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus);
    void slotOnErrorOccurred(QProcess::ProcessError a_eError);

private:
    // 2026.08.19 配置热生效：自定义模型表/激活条目变更（SetCustomModels 检出）→ 空闲
    //   （无轮次、非握手中）实例立即静默杀进程，下次 StartStage 以新配置冷启动；轮次
    //   进行中则挂 m_bCfgDirty，FinishTurn 落幕后杀，本轮不受打扰（免重启 AutoCode）。
    void RestartForCfgChange();
    bool m_bCfgDirty = false;   // 轮次中遇配置变更，待收尾后重启进程
    int  m_iCfgDirtyGen = -1;  // 挂脏标时的进程代次（收尾杀进程只杀旧代，防误杀新配置进程）
    int  m_iProcGen = 0;       // 进程代次（EnsureProcess 冷启动 +1；区分新旧进程）

    // 进程态（长驻生命周期）：NotCreated → Booting（握手中）→ Ready ⇄ Dead（退出/被杀）。
    enum EProcState { ProcNotCreated, ProcBooting, ProcReady, ProcDead };
    // 轮次态：Idle（可接活）→ Prompting（prompt 已发，等 turn/end+idle）→ Idle。
    enum ERunState { RunIdle, RunPrompting };

    bool EnsureProcess(const QString& a_sWorkDir);   // 冷启动+握手（幂等：Booting/Ready 直接返回）
    QString EffectiveProvider() const;   // 激活自定义条目 ? "autocode-active" : DshProvider()
    QString EffectiveModel() const;      // 激活自定义条目 ? 条目模型 : DshModel()
    void SyncCordisFromMaster();         // exe 同级 dsh\cordis.autocode.yml → 部署位（防模板演进后旧档不同步）
    void SendRequest(const QString& a_sMethod, const QJsonObject& oParams,
                     int a_iId = 0);                 // 写一帧（stdin 不关；id=0 自动发号）
    void SendPrompt(const QString& a_sText);         // session/prompt（同 sid 即续轮）
    void BeginPrompt(const QString& a_sPrompt);      // 武装计时器 + 投递（显式传文本，防误用 m_sPendingPrompt 旧值）
    void MarkProcDead();                             // 置 ProcDead + 清 m_aLiveSids（防新进程复用旧 sid 撞持久化层）
    void HandleLine(const QByteArray& a_sLine);      // JSON-RPC 分帧分发
    void HandleResponse(int a_iId, const QJsonObject& oRoot);
    void HandleNotify(const QJsonObject& oRoot);
    void HandleSessionEvent(const QJsonObject& oParams);   // session.event → 信号映射
    void FinishTurn();                               // turn/end(+idle/兜底) → sigFinished（幂等）
    void CheckStall();                               // 同名约定沿用基类注释（claude 侧为公有保护）
    void EmitFailed(const QString& a_sMsg);
    void RelaunchStage();                            // 快速失败重试：重启进程 + 重发 prompt
    void ResetTurnState();                           // 轮次统计清零（进程/会话/模型不动）
    double ComputeTurnCost() const;                  // 轮内 token 分量 × 单价折算（$/M）
    QString NewSid();                                // "dsh-" + uuid（进程内唯一即可）

    QProcess*   m_pProc = nullptr;
    EProcState  m_eProcState = ProcNotCreated;
    ERunState   m_eRunState  = RunIdle;
    QByteArray  m_buf;                    // stdout 跨 readyRead 行缓冲
    int         m_iNextRpcId = 1;         // JSON-RPC 请求 id 发号器
    int         m_iInitId    = 0;         // initialize 请求 id（响应配对）
    int         m_iPromptId  = 0;         // 当前 session/prompt 请求 id
    int         m_iBootRetry = 0;         // 握手超时已重试次数（上限 1）
    bool        m_bRebooting = false;     // 主动重启中（拦 slotOnFinished 的 Booting 重试分支，防双重重启）
    int         m_iFastRetry = 0;         // prompt 中退出已重试次数（上限 2）
    bool        m_bRetryPending = false;  // 快速失败重试等待中
    bool        m_bTurnReported = false;  // 本轮已发 sigFinished（防重复，同 claude m_bFinished）
    bool        m_bTurnEnded = false;     // 已收 turn/end（等 idle 收尾）
    QString     m_sSessionId;             // 当前会话 sid
    QString     m_sPromptWorkDir;         // 本轮工作目录（冷启动/重试路径用）
    QString     m_sPendingPrompt;         // 握手中暂存的 prompt（Ready 后投递）
    QString     m_sTurnText;              // 本轮最后一条 assistant 文本（sigFinished result 用）
    QString     m_sTurnReasonKind;        // turn/end 的 reason.kind
    qint64      m_iSumInTok  = 0;         // 轮内 Σ input（含 cache 读/写，账单口径）
    qint64      m_iSumOutTok = 0;         // 轮内 Σ output
    // 计费分量（usage 四项拆开累计，单价各不同；m_iSumInTok = 三输入项之和）
    qint64      m_iSumPlainInTok  = 0;    // 纯 inputTokens
    qint64      m_iSumCacheReadTok  = 0;  // cacheReadTokens
    qint64      m_iSumCacheWriteTok = 0;  // cacheWriteTokens
    qint64      m_iCtxWindow = 0;         // request/context 上报的窗口（0=未知，回退模型探测）
    QSet<QString> m_aLiveSids;            // 本进程创建过的 sid（跨进程 resume 判定）
    QStringList m_aStderrTail;            // stderr 尾部（失败消息附带）
    QElapsedTimer m_uptime;               // 本轮 prompt 起始计时（快速失败判定：短命退出才重试）
    QElapsedTimer m_actClock;             // 距上次收到输出（stall 巡检）
    QTimer*     m_pStallTimer = nullptr;  // 停滞巡检（30s 一拍）
};
