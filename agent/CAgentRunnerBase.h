#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QProcessEnvironment>
#include "core/CModelEntry.h"   // 2026-10-07 对话级模型覆盖槽（SDshModelEntry）

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

    // 2026.09.02 跨轮上下文能力（助手回灌依据）：true = 下一轮 StartStage 带
    // resumeSid 时上下文原生延续（claude --resume+--fork-session 复制上下文、
    // pi --session-id 磁盘会话跨进程续接）；false = 每轮全新会话（DSH fork 换新
    // sid、Codex/Hermes 忽略 resumeSid），编排器须把最近对话回灌进 prompt，
    // 否则第 2 轮起模型完全失忆（实测「作为默认值」轮只看到 4 个字）。
    // 默认 false：新后端未声明即按"无跨轮上下文"安全侧处理。
    virtual bool KeepsContextAcrossTurns() const { return false; }

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

    // 2026-10-07 对话级模型覆盖（用户需求：右键「选择模型」只对本助手生效，不改
    // 全局四张节/静态激活条目）：per-runner 实例槽（2026.09.03 起助手会话各持独立
    // runner，实例注入互不影响其他会话/任务侧）。语义 = 本实例后续每次 StartStage
    // 组装参数/配置时用 a_iIdx 指向的表内条目代替全局激活条目；-1 = 跟随全局。
    // 各后端实现自行消费：Pi（--provider autocode-<n> 下标）/Codex（-c 路由+窗口
    // +anthropic→chat 兑底）/DSH（env 槽位，进程保活时 key 变更自动重启）/Hermes
    // （HERMES_* env + config 写入用条目快照）；claude 不参与共享表，忽略。
    // 下标口径 = 该后端 runner 静态模型表（ApplySharedModelsToRunners 已把四表收敛
    // 为同一份共享表，四后端表内容/顺序一致）；条目快照供无表静态的单值后端用。
    void SetStageModelOverride(int a_iIdx, const SDshModelEntry& a_stE) {
        m_iModelOvIdx = a_iIdx;
        m_stModelOv = a_stE;
    }
    void ClearStageModelOverride() { m_iModelOvIdx = -1; }
    bool HasStageModelOverride() const { return m_iModelOvIdx >= 0; }

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

    // —— 便携资产统一解析（2026.08.24，三后端共享口径）——
    // 设计原则：**不向用户主目录写任何文件**（便携部署目标：程序目录拷到任意电脑即用）。
    // 共享技能目录（按优先序，只列存在者）：
    //   1) <exe 目录>\skills            —— 便携技能库，随程序目录走（部署时随身携带）；
    //   2) <用户主目录>\.claude\skills   —— 机器上已有 Claude Code 技能库时自动复用。
    // 各后端经原生机制挂载（pi: --skill；DSH: DSH_SKILLS_DIRS env → cordis customSkillDirs）。
    static QStringList SharedSkillDirs();
    // 全局规则内容（Claude Code 的 CLAUDE.md 语义）：<exe 目录>\CLAUDE.md（部署环境
    //   约定）与 <用户主目录>\.claude\CLAUDE.md（用户全局规则，含 UI 测试铁律）两份
    //   **按序拼接**（2026.09.13 勿回退：旧版取第一个命中会让便携层遮蔽全局层，
    //   UI 测试铁律从未到达 pi/dsh/codex/hermes）；两者都无返回空串。运行时注入
    //   prompt/persona，不复制进 ~/.pi、~/.dsh、~/.codex 等 harness 主目录。
    //   上限保护：每份超 64KB 视为异常不注入该份（防巨型文件挤占上下文）。
    static QString SharedGlobalContextContent();
    // 共享技能目录清单（2026.09.14）：扫描 SharedSkillDirs() 递归收集 SKILL.md，
    //   解析 frontmatter 的 name/description 生成紧凑目录文本（一条一行：名称—描述—路径），
    //   供「CLI 无原生技能机制」的后端（codex/hermes）经 prompt 前置注入——模型命中
    //   时用文件读取工具自行读 SKILL.md（同 Claude Code 的技能加载语义，但不写任何主目录）。
    //   2026.09.13 增强勿回退：①钉选技能（ui-sandbox，AutoCode GUI 测试主技能，全局
    //   CLAUDE.md 铁律引用）置顶永不被截断；②命中钉选时清单头部追加「GUI 测试铁律」
    //   一行（实测光有可选清单弱模型仍不联想，须点名指向）；③详情区 12KB 截断后剩余
    //   技能压成名字串兑底（名字即目录名，可推导 SKILL.md 路径），不再无声丢弃。
    //   无任何技能返回空串。
    static QString SharedSkillsCatalog();
    // 指定目录集的清单生成（测试入口）：语义同 SharedSkillsCatalog，目录由调用方给。
    static QString SharedSkillsCatalogFromDirs(const QStringList& aDirs);

signals:
    void sigInfo(const QString& a_sLine);                              // 系统/原始信息
    void sigAssistantText(const QString& a_sText);                     // 助手文本输出
    void sigToolUse(const QString& a_sTool, const QString& a_sInput);  // 工具调用
    void sigToolResult(const QString& a_sResult);                      // 工具结果（截断）
    void sigContextFill(qint64 a_iFill);   // 运行中上下文占用（每条 assistant 消息的 usage；值变化才发）
    // 运行中真实计量增量（每完成一次 API 请求发一次；Codex 经本地代理解析网关
    // usage 上抛，2026.08.21）：编排器即时累计进任务行（费用/输入/输出/请求列）。
    void sigUsageDelta(qint64 a_iInTok, qint64 a_iOutTok);
    // 2026.08.27 流式实时计量（绝对值：已结算累计 + 当前流式中消息的 usage）：显示专用，
    //   收尾统计不走此信号（防与 sigUsageDelta 双计）。仅支持流式 usage 的后端发。
    void sigLivePartial(qint64 a_iInTok, qint64 a_iOutTok);
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

    // 2026-10-07 对话级模型覆盖槽（见公共段 SetStageModelOverride 注释）。
    int             m_iModelOvIdx = -1;   // 覆盖下标（-1=跟随全局激活）
    SDshModelEntry m_stModelOv;           // 覆盖条目快照（单值静态后端用）
    // 覆盖下标对当前表长度的有效值（越界/未设回 -1）——各后端 StartStage 解析用。
    int ModelOvIdxIn(int a_iTableSize) const {
        return (a_iTableSize > 0 && m_iModelOvIdx >= 0 && m_iModelOvIdx < a_iTableSize)
            ? m_iModelOvIdx : -1;
    }

    // 2026-09-12 手工审批弹窗通道（全后端）：向子进程环境注入 AUTOCODE_ASK_EXE
    //   （AutoCode.exe 全路径）与 AUTOCODE_ASK_PIPE（本实例 AskServer 管道名）。
    //   助手在手工审批轮经 Bash 运行 "%AUTOCODE_ASK_EXE%" --ask <超时秒> "问题"
    //   → --ask 模式连本实例管道 → 主窗口弹 CAskUserDialog → stdout 回用户答复
    //   （退出码 0=已答复 / 2=取消 / 3=超时 / 4=服务不可用）。claude 后端另有
    //   ask_user MCP 工具不走此通道，但环境注入无害。
    static void InjectAskEnv(QProcessEnvironment& a_oEnv);
};
