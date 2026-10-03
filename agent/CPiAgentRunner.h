#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "CAgentRunnerBase.h"
// 2026.08.20 自定义大模型条目结构（Pi 后端多条目管理经 ~/.pi/agent/models.json）。
#include "../core/CModelEntry.h"

// PIAgent（pi coding-agent CLI）NDJSON 流式后端 runner（2026.08.19 第三后端接入）。
//
// 进程模型：**一阶段一进程**，2026.09.22 起双形态——
//   · RPC 形态（优先）：`node <cli.js> --mode rpc ...` 常驻 JSONL 双向管道。prompt 经
//     stdin 命令帧 {"type":"prompt"} 投递；事件流与一次性 --mode json 完全同构
//     （toJsonEvent 同一实现，HandleLine 零改动复用）；轮结束信号 = agent_settled
//     （pi 源码 _runAgentPrompt 的 finally——完整收敛含自动重试/压缩续跑后只发一次），
//     收到即 closeWriteChannel 触发 pi 落盘优雅退出(exit 0)，走既有成功收尾路径。
//     **轮中注入（本形态的意义）**：InjectUserTurn → {"type":"steer"} 命令帧，
//     pi 把用户消息投进正在运行的 agent 迭代（实测：工具执行中注入，下一模型迭代
//     即收到，与 claude stream-json 注入同效）；Stop 语义不变（置终态+kill）。
//   · 一次性形态（回退）：`node <cli.js> -p --mode json ...`，stdout 为 NDJSON 事件流
//     （首行 session 头），进程退出即结束——RPC 探测失败/运行期早夭时自动回退。
//   能力探测 RpcCapable()（进程内缓存）：cli.js 同级 modes/rpc/rpc-mode.js 存在
//   （源码树状 dist），或 cli.js 自身含 "extension_ui_request" 协议字面量（npm 单文件
//   bundle）；运行期早夭（未见会话即非零退出）→ 进程级记忆禁用 RPC 并当轮自动
//   重拉一次性形态（升级/半安装场景自愈，用户无感）。pi 版本考古：--mode 解析与
//   "rpc" 值同一提交引入，无「认识 --mode 不认识 rpc」的中间版本，回退只需覆盖
//   「不认识 --mode」（commander 报错早退非零）。
//
// 关键口径（实测/源码核实，见任务目录 pi-json-sampling.md）：
//   - usage 字段（pi packages/ai Usage）：input/output/cacheRead/cacheWrite（累计口径）；
//     上下文占用 = input+cacheRead+cacheWrite（同 claude 三输入项口径）；缺字段容错为 0。
//   - 计费：pi 虽报 usage.cost，按设计决策不采用 → cost 恒 0（费用列显示 "-"）。
//   - AssistantMessage.stopReason=="length" → subtype="error_max_turns"（复用 claude
//     词汇，上层 LastWasMaxTurns 自动续跑零改动）。
//   - 一次性：prompt 经 stdin 投递 + closeWriteChannel；RPC：JSON 命令帧（见上）。
//   - 会话恢复：pi 磁盘会话跨进程可续——resumeSid 经 --session-id 传入（优于 DSH）；
//     带 resumeSid 时**禁加** --no-session（否则会话不落盘）。RPC 形态无 session 头
//     事件，会话 id 改经启动即发的 get_state 命令回读（response.data.sessionId）。
//   - API key 经 QProcessEnvironment 按 provider 映射注入（不进命令行/日志，NFR-4）。
//
// claude 专属参数降级：a_iMaxTurns / a_iAutocompactK / a_bEnableAskUser 忽略
// （pi 自带自动 compaction、无 ask_user 工具，StartStage 非默认值时提示一次）；
// a_sDisallowedTools → --exclude-tools；a_bFork 忽略（--session-id 下无 fork 概念）。
class CPiAgentRunner : public CAgentRunnerBase {
    Q_OBJECT

public:
    explicit CPiAgentRunner(QObject* a_pParent = nullptr);
    ~CPiAgentRunner() override;

    // —— 进程级全局配置（config.ini [Agent] Pi*；所有 Pi runner 实例共享）——
    //   root = PIAgent 仓库根（其下 pi\ 为 monorepo）；node 留空走 PATH 的 "node"；
    //   provider/model 默认 anthropic / 空（空 model 用 pi 默认）；thinking 空=默认。
    static void    SetPiRootPath(const QString& a_sRoot);
    static QString PiRootPath();
    static void    SetPiNodePath(const QString& a_sPath);
    static QString PiNodePath();
    static void    SetPiProvider(const QString& a_sProvider);
    static QString PiProvider();
    static void    SetPiModel(const QString& a_sModel);
    static QString PiModel();
    static void    SetPiApiKey(const QString& a_sKey);   // 明文存 config.ini（tooltip 已告知）
    static QString PiApiKey();
    static void    SetPiThinking(const QString& a_sLevel);   // off/low/medium/high…；空=默认
    static QString PiThinking();

    // —— 计费单价（$/百万 token，2026.08.20）：pi 协议只报 token 分量不报金额，
    //    客户端按单价折算（同 DSH 口径）；默认对齐 claude 后端 Sonnet 档价目。
    //    实际网关计费不同则改 config.ini [Agent] PiPrice*。全 0 = 不折算（费用列回 "-"）。
    static void SetPiPriceInPerM(double a_d);
    static void SetPiPriceCacheReadPerM(double a_d);
    static void SetPiPriceCacheWritePerM(double a_d);
    static void SetPiPriceOutPerM(double a_d);

    // —— 自定义大模型表（config.ini [PiModels]；2026.08.20，激活条目经
    //    ~/.pi/agent/models.json 的 autocode-<n> provider 槽位生效）——
    //   每条目 = 一个独立 provider（autocode-1..N，含 baseUrl/api/apiKey/models），
    //   Set 时即合成写 models.json（保留用户手写的非 autocode-* provider）；
    //   激活条目覆盖 --provider/--model 固定路由（env 注入 API key 同时旁路，
    //   密钥改走 models.json）。表空 / 无激活 = 走 Provider/模型固定路由。
    static void SetPiModels(const QList<SDshModelEntry>& a_aModels, int a_iActive);
    static QString PiActiveProvider();    // 激活条目 provider 键（autocode-<n>）；无激活=空
    static QString PiActiveModelId();     // 激活条目模型 id；无激活=空
    static QString PiModelsJsonPath();
    // 2026.09.09 v2.1.133 测试隔离：单测/端到端测试把 models.json 指到临时路径，
    //   杜绝测试写盘污染真实 ~/.pi（该类误踩已三犯：陈旧表被测试还原动作带回）。空=真实路径。
    static void SetModelsJsonPathOverrideForTest(const QString& a_sPath);    // ~/.pi/agent/models.json
    static bool    WritePiModelsJson();   // 合并写 models.json（true=写成功/无需写）
    static QString PiSettingsJsonPath();  // ~/.pi/agent/settings.json
    // 2026.08.28 429 TPM/RPM 限流自愈：pi 轮级自动重试（settings.retry，默认 3 次、
    //   基准退避 2s → 2s/4s/8s 全程 ~15s）对分钟级滚动配额限流无效——重试全落在
    //   配额未恢复的窗口内，3 连击全 429 后 pi 放弃，AutoCode 收「模型接口错误
    //   （本轮无输出）：429 …」（如 deepseek "inference tpm exhausted"）。幂等补写
    //   retry 段 baseDelayMs=15000（15s/30s/60s 指数退避，跨过配额滚动窗口）；
    //   settings.json 已有 retry 段（用户显式配置/先前已写）则不动；StartStage
    //   每次调用（同 models.json 自愈口径，防多实例竞态）。
    static bool    EnsurePiRetrySettings();

    // 2026.08.20 联网安装（npm 全局）探测：npm root -g 下
    //   node_modules/@earendil-works/pi-coding-agent/dist/cli.js；命中返回绝对路径。
    //   进程内缓存一次（设置页检测与 cli.js 解析兜底共用）。
    static QString NpmGlobalCliPath();

    // —— 2026.09 版本管理（任务 23183ccb）：Pi 安装形态与本地版本 ——
    enum EPiForm { EPiFormNone = 0, EPiFormSource, EPiFormNpm };
    // 形态探测：PiRootPath() 非空且 <root>/pi/packages/coding-agent/package.json 存在
    //   → Source；否则 NpmGlobalCliPath() 命中且其同目录 package.json 存在 → Npm；
    //   否则 None。优先级与 ResolveCliJsPath（root 布局 → npm 全局）一致。
    //   注：探测的是 package.json，运行时用 dist/cli.js——正常安装下两者同版本，
    //   半构建态（构建中断/进行中）可能不一致。
    static EPiForm DetectPiForm();
    // 本地版本：a_sRoot 非空→只读 <a_sRoot>/pi/packages/coding-agent/package.json
    //   （miss→空串，不回退）；空→按 DetectPiForm() 分派（Source=根包 /
    //   Npm=npm 全局包，包路径＝cli.js 同目录 package.json / None=空串）。
    static QString ResolvePiVersion(const QString& a_sRoot = QString());

    // 三个生效路径/探测（仿 CDeepSeekRunner 同名方法口径）。
    static QString ResolveNodePath();   // 显式键 → "node"
    static QString ResolveCliJsPath();  // root + \pi\packages\coding-agent\dist\cli.js
    static bool    LooksLikePiRoot(const QString& a_sRoot);   // 判定文件存在
    static QString ProbeSiblingRoot();  // exeDir{,/\..,/\../..} × \PIAgent
    static QString PiConfigError();     // 空=可用；非空=指引（含 /pi 安装提示）
    static QString PiInstallDiagnosis();   // 半安装态诊断（目录在、cli.js 缺）

    QString BackendName() const override { return QStringLiteral("pi"); }

    // 2026.09.22 RPC 形态能力探测（进程内缓存，见类头注释）：cli.js 同级
    //   modes/rpc/rpc-mode.js（源码树状 dist）或 cli.js 含 "extension_ui_request"
    //   协议字面量（npm 单文件 bundle）；运行期早夭后进程级记忆禁用（s_bRpcRuntimeBroken）。
    static bool RpcCapable();
    static void ResetRpcCapCacheForTest();   // 单测：清探测缓存（含运行期禁用记忆）

    // --session-id 磁盘会话跨进程可续（fork 虽忽略，同 sid 续跑仍携带全部上下文）。
    bool KeepsContextAcrossTurns() const override { return true; }

    // 启动一次阶段执行：每次冷启动一个 pi 子进程，进程退出即本轮结束。
    void StartStage(const QString& a_sWorkDir,
                    const QString& a_sPrompt,
                    const QString& a_sDisallowedTools,
                    int a_iMaxTurns,
                    const QString& a_sResumeSid,
                    bool a_bFork,
                    int a_iAutocompactK,
                    bool a_bEnableAskUser) override;

    // 终止当前执行：静默收尾（置终态 → kill → 不发 sigFinished，同 DSH/claude 口径）。
    void Stop() override;

    // 进程存活且本轮未报告 finish。
    bool IsRunning() const override;

    // 孤儿宽限不移植：node 直起（无 cmd.exe 中间层），进程死即真死。
    bool IsGraceActive() const override { return false; }

    // 一次性进程无轮次中插话：恒 false 安全 no-op。
    bool IsRetryPending() const override;

    // 2026.09.22 RPC 形态：运行中且轮未 settle → steer 命令帧（立即生效，实测
    //   工具执行中注入、下一模型迭代收到）；一次性形态仍 no-op false（上层排队）。
    bool InjectUserTurn(const QString& a_sText) override;

private slots:
    void slotOnStdOut();
    void slotOnStdErr();
    void slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus);
    void slotOnErrorOccurred(QProcess::ProcessError a_eError);

private:
    void Reset();                            // 本轮状态清零（StartStage 开头调用）
    // 2026.09.22 RPC/一次性双形态：StartStage 只存回退参数+定形态，实现移 Impl。
    void StartStageImpl(const QString& a_sWorkDir, const QString& a_sPrompt,
                        const QString& a_sDisallowedTools, int a_iMaxTurns,
                        const QString& a_sResumeSid, bool a_bFork,
                        int a_iAutocompactK, bool a_bEnableAskUser, bool a_bRpc);
    void SendRpcFrame(const QJsonObject& a_oCmd);   // JSON 命令帧写 stdin（LF 结尾）
    double ComputeTurnCost() const;          // 轮内 token 分量 × 单价折算（$/M）
    void HandleLine(const QByteArray& a_sLine);   // NDJSON 事件分发
    void HandleMessageEnd(const QJsonObject& oMsg);
    void FinishFromAgentEnd(const QJsonArray& aMessages);   // agent_end → 记录待报文本
    void EmitFailed(const QString& a_sMsg);
    void CheckStall();                       // 停滞巡检（30s 拍，同 claude/DSH 侧）
    void ApplyUsage(const QJsonObject& oUsage);   // usage 三输入项/输出累计 + 上下文占用

    QProcess*   m_pProc = nullptr;
    QByteArray  m_buf;                    // stdout 跨 readyRead 行缓冲
    bool        m_bFinished = false;      // 本轮已发 sigFinished（防重复）
    bool        m_bAgentEnded = false;    // 已收 agent_end（进程退出即按成功收尾）
    bool        m_bSawSession = false;    // 本轮已确认会话 id（一次性=session 头；RPC=get_state 回读）
    // —— 2026.09.22 RPC 形态状态 ——
    bool        m_bRpcMode = false;       // 本轮走 RPC 形态（事件帧含 response/agent_settled）
    bool        m_bRpcSettled = false;    // 已收 agent_settled（轮完整收敛，已关写通道）
    struct SRetryParams {                 // RPC 早夭自动回退重拉的一次性形态参数
        QString sWorkDir, sPrompt, sDisallowed, sResumeSid;
        int iMaxTurns = 30; bool bFork = false; int iAutocompactK = 0; bool bEnableAskUser = false;
    } m_stRetry;
    QString     m_sTurnText;              // 本轮最后一条 assistant 文本
    QString     m_sLastStopReason;        // 最后一条 assistant 的 stopReason
    // 2026.08.22 最后一次 API 调用错误（message_end stopReason=="error" 的 errorMessage）：
    //   网关 429/500 等场景 pi 仍正常退出（exit 0）不落 stderr——此前静默吞掉，
    //   UI 只见「无文本输出」，用户无从得知真实原因。现暂存并在收尾时带出。
    QString     m_sLastApiError;
    QString     m_sResumeSid;             // 本轮传入的 resumeSid（重试/记录用）
    qint64      m_iSumPlainInTok   = 0;   // 本轮 Σ 纯输入（计费分量）
    qint64      m_iSumCacheReadTok = 0;   // 本轮 Σ 缓存读
    qint64      m_iSumCacheWriteTok = 0;  // 本轮 Σ 缓存写
    qint64      m_iSumInTok  = 0;         // 本轮 Σ input（含 cache 读/写）
    qint64      m_iSumOutTok = 0;         // 本轮 Σ output
    qint64      m_iLiveEmitIn = 0;    // 2026.08.27 已 sigUsageDelta 实时上抛的输入累计（防收尾双计）
    qint64      m_iLiveEmitOut = 0;   // 同上，输出累计
    QStringList m_aStderrTail;            // stderr 尾部（失败消息附带）
    QElapsedTimer m_actClock;             // 距上次收到输出（stall 巡检）
    // 2026.08.29 流式心跳：距上次「完整事件」（message_end/工具/turn_start）——
    //   message_update 增量不算（thinking=high 下单条消息可流式十几分钟，UI 只显示
    //   完整消息，期间看起来像卡死；心跳提示恢复可见性，见 CheckStall）。
    QElapsedTimer m_tNoMsg;
    qint64        m_iNoMsgHintMs = -1;      // 上次心跳提示时的无消息时长（60s 节流）
    QTimer*     m_pStallTimer = nullptr;  // 停滞巡检定时器（30s 一拍）
};
