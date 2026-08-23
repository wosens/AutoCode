#pragma once

#include <QObject>
#include <QString>

// Agent 后端 runner 抽象基类（2026.08.18 多后端支持：Claude Code / DeepSeekHarness）。
//
// 背景：编排器/项目管理器共 5 处持有 runner（任务/评审 fan-out/ad-hoc 助手/guide 生成/
// guide 同步），此前全部硬编码 CClaudeRunner。为支持设置里切换 Agent 后端，把对上层
// 有意义的面（6 信号 + 生命周期接口 + Last* 元信息）上移到本基类，5 处持有点改持
// 基类指针，创建统一走 CAgentRunnerFactory。
//
// 信号签名与语义逐字继承自原 CClaudeRunner.h（claude 后端零回归是硬门禁）：
// 后端实现负责把自己的原生事件流映射到这组信号（如 DSH 的 turn/end → sigFinished）。
//
// 生命周期约定沿用原语义：一次 StartStage() 对应一次执行；Finished 后对象可复用或丢弃。
class CAgentRunnerBase : public QObject {
    Q_OBJECT

public:
    explicit CAgentRunnerBase(QObject* a_pParent = nullptr);
    ~CAgentRunnerBase() override;

    // 后端标识（"claude" / "dsh"）：日志展示与后端漂移检测用。
    virtual QString BackendName() const = 0;

    // 启动一次阶段执行（参数语义见 CClaudeRunner 同名函数；claude 专属参数在后端
    // 不支持时降级忽略，由各实现自行说明）。
    virtual void StartStage(const QString& a_sWorkDir,
                            const QString& a_sPrompt,
                            const QString& a_sDisallowedTools,
                            int a_iMaxTurns = 30,
                            const QString& a_sResumeSid = QString(),
                            bool a_bFork = false,
                            int a_iAutocompactK = 0,
                            bool a_bEnableAskUser = false) = 0;

    // 终止当前执行
    virtual void Stop() = 0;

    virtual bool IsRunning() const = 0;

    // 崩溃宽限期进行中（IsRunning()==false 但仍占用 runner；停止判定需覆盖）。
    virtual bool IsGraceActive() const = 0;

    // 快速失败重试等待中（同上，仍占用 runner）。
    virtual bool IsRetryPending() const = 0;

    // 向当前运行中的进程追加一条用户轮次；未运行或失败时安全 no-op 返回 false。
    virtual bool InjectUserTurn(const QString& a_sText) = 0;

    // 最近一次 result 的元信息（供编排器判定 max-turns 与续跑）。
    QString LastSubtype() const { return m_sLastSubtype; }
    QString LastSessionId() const { return m_sLastSessionId; }
    // subtype == "error_max_turns"：命中轮次上限（可续跑）。DSH 后端把 turn/end 的
    // max-tokens 映射到同一词汇，上层 LastWasMaxTurns 续跑逻辑零改动。
    bool LastWasMaxTurns() const { return m_sLastSubtype == QStringLiteral("error_max_turns"); }
    // subtype == "stall"：停滞检测命中（watchdog 终止，可凭 lastSessionId 续跑）。
    bool LastWasStall() const { return m_sLastSubtype == QStringLiteral("stall"); }
    // 当前上下文占用（最后一条 assistant 消息的 input + cache 三项之和），
    // 供 ad-hoc agent 的「上下文大小」显示与压缩阈值判定。
    qint64 LastContextFill() const { return m_iLastContextFill; }
    // 本次运行的 API 请求数（assistant 消息条数，StartStage 时清零）——任务统计用。
    // 置虚：Codex 后端经本地代理拿到真实请求数并已实时上抛（sigUsageDelta），
    // 阶段收尾时须报 0 防与实时累计重复（2026.08.21）。
    virtual int LastReqCount() const { return m_iLastReqCount; }
    QString LastModel() const { return m_sLastModel; }   // 最近 result 的 model 字段（供查上下文窗口）

signals:
    void sigInfo(const QString& a_sLine);                              // 系统/原始信息
    void sigAssistantText(const QString& a_sText);                     // 助手文本输出
    void sigToolUse(const QString& a_sTool, const QString& a_sInput);  // 工具调用
    void sigToolResult(const QString& a_sResult);                      // 工具结果（截断）
    void sigContextFill(qint64 a_iFill);   // 运行中上下文占用（每条 assistant 消息的 usage；值变化才发）
    // 运行中真实计量增量（每完成一次 API 请求发一次；Codex 经本地代理解析网关
    // usage 上抛，2026.08.21）：编排器即时累计进任务行（费用/输入/输出/请求列）。
    void sigUsageDelta(qint64 a_iInTok, qint64 a_iOutTok);
    void sigFinished(bool a_bOk, const QString& a_sResult,            // 一次执行结束
                     qint64 a_iInTok, qint64 a_iOutTok,
                     double a_dCost, const QString& a_sSessionId);

protected:
    // 最近一次执行的元信息状态（各后端实现写入；Reset 时清零）。
    QString     m_sLastSubtype;         // 最近 result 的 subtype（success / error_max_turns / ...）
    QString     m_sLastSessionId;       // 最近 result 的 session_id（供续跑）
    qint64      m_iLastContextFill = 0; // 当前上下文占用（最后一条 assistant 的三项 input token 之和）
    int         m_iLastReqCount = 0;    // 本次运行 API 请求数（assistant 消息条数）
    QString     m_sLastModel;           // 最近 result 的 model 字段（供上层查上下文窗口）
};
