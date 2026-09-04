#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QProcess>
#include <QPointer>
#include <QList>
#include <QPair>
#include <QSet>
#include <QHash>
#include <QJsonArray>
#include <QTimer>   // 2026.08.27 m_tGuideSyncIdle（助手 guide 同步空闲节流）
#include <atomic>   // 2026.08.22 m_bBackupRunning（回退互斥守卫读，备份线程写）
#include <thread>

#include "core/TaskTypes.h"

class CTaskInfo;
class CAgentRunnerBase;
class CProjectManager;
struct SStageDef;

// 任务编排器：持有 ClaudeRunner，按状态机逐阶段驱动任务，
// 解析评审决策（Pass/Reject）、控制迭代上限、落盘 record.md、处理暂停/停止。
//
// MVP：单任务串行（m_pCurTask）。多任务并发为后续 Phase。
class CTaskOrchestrator : public QObject {
    Q_OBJECT

public:
    explicit CTaskOrchestrator(CProjectManager* a_pPm, QObject* a_pParent = nullptr);
    ~CTaskOrchestrator() override;   // join 后台预备/备份线程（退出时防崩溃）

    void StartTask(CTaskInfo* a_pTask);   // 开始 / 从暂停恢复
    void PauseTask(CTaskInfo* a_pTask);   // 阶段边界暂停
    void StopTask(CTaskInfo* a_pTask);    // 终止当前阶段
    // 需求3：右键「再次执行」——仅重跑指定阶段，完成后停在该阶段（Paused），不推进后续、不判死。
    //   用于阶段表右键：重生成该阶段产物，不动后续已生成的产物、不自动往下跑。
    void RunSingleStage(CTaskInfo* a_pTask, EStage a_eStage);

    // FR-7 最低验收：停止当前任务 runner + 编译门并置空 m_pCurTask，
    // 使 OnRunnerFinished 早返回（不再回调到将被 delete 的任务），从而让
    // CMainWindow::ClearTasks 的 delete pTask 不触发悬垂访问。不阻塞 UI（不等进程退出）。
    void StopAll();

    // 是否有任务 runner 正在运行（MVP 单 runner 语义=全工程无并发）。
    // 用于 OnStartTask 堵抢占口子：A 运行中时拒绝启动 B，避免 m_pCurTask 被覆盖导致回调串扰。
    // 注：阶段二接入评审并行 runner 后，此处需扩展为「主 runner 忙 或 评审 runner 集合非空」。
    bool IsRunnerBusy() const;

    // —— 提示词 agent（独立于状态机的常驻助手会话，#1/#2/#3/#4）——
    // 2026.09.03 多助手并发：每个助手一个独立 SAdhocSession（runner 进程/会话链/撤销栈/
    //   回灌缓存/模式状态机），按助手 id 索引、常驻编排器（切助手不再换载）。跨助手提交
    //   各起各的进程立即运行，互不排队；同助手运行中追加仍为注入/续发语义。
    // 向指定助手投递一条指令；a_bReadOnly=true 时仅 Read/Grep/Glob（无 Edit/Write/Bash）。
    //   a_sTaskId 非空 = 当前选中任务：注入该任务 memory.md 经验 + 非只读轮附追加规则（记忆跟着任务走）。
    //   2026.09.02 a_pTranscript 非空 = 外部提交（面板传入权威对话记录，整体重建回灌缓存；
    //   空 = 编排器内部续发——排队指令/max-turns 续跑，沿用缓存并轮内追加）。
    void SubmitAdhoc(const QString& a_sAsstId, const QString& a_sText, bool a_bReadOnly,
                     const QString& a_sTaskId = QString(), const QStringList* a_pTranscript = nullptr);
    // 撤销指定助手最后一条提问（fork-session sid 链 O(1) 回退，#4）。
    void UndoAdhoc(const QString& a_sAsstId);
    // 手动压缩指定助手上下文（摘要→开新会话重播种，#3）。
    void CompactAdhoc(const QString& a_sAsstId);
    // 停止指定助手的当前调用（不影响任务 runner 与其他助手）。
    void StopAdhoc(const QString& a_sAsstId);
    // 停止全部助手（档切换/切工程/关闭前整组停；⏹ 痕迹各自落自己的流）。
    void StopAllAdhoc();
    bool IsAsstAdhocRunning(const QString& a_sAsstId) const;   // 指定助手是否在跑一轮
    bool IsAnyAdhocRunning() const;                            // 是否有任一助手在跑（旧 IsAdhocRunning 语义）

    // 2026.08.22 阶段后台备份线程（m_thBackup）是否运行中：CMainWindow::ExecuteRollback
    // 互斥守卫用（备份期间 IsRunnerBusy 可能为 false，需单独挡住，防回退与并发备份同写统一库）。
    bool IsBackupRunning() const { return m_bBackupRunning.load(); }

    // 2026.08.20 (Agent 起名) 右键「自动命名」/新建任务 Intake 后触发：任务内容 +
    //   guide.md 拼提示词经 adhoc 助手（只读、独立 AutoNaming 模式）交 Agent 生成一句话
    //   简短名称，OnAdhocFinished 清洗后 SetName 回填并发 sigAutoNameDone；助手忙时
    //   同步回退本地启发式 MaybeAutoNameTask。返回 true=已提交 Agent 或本地已命名。
    //   2026.09.03 多助手并发：命名借用 a_sAsstId 助手的会话链（fork 一次性分支，不动其
    //   撤销栈）；该助手忙则本地回退（其余助手照常并发，不受影响）。
    bool AutoNameNow(CTaskInfo* a_pTask, const QString& a_sAsstId = QString());

    // 设置评审阶段并行视角数（1=串行回退；>1=多视角并行 fan-out/fan-in）。
    void SetReviewParallel(int a_iN);

    // 2026.08.15 (联网开关) 设置阶段联网检索开关（config.ini [Agent] WebSearch）。
    //   false 时 LaunchStage 在 --disallowedTools 追加 WebSearch,WebFetch（全阶段生效）。
    void SetWebSearch(bool a_bOn) { m_bWebSearch = a_bOn; }

    // FR-3：向「当前运行中」的任务注入一条用户指令（Q2 默认=a 追加用户消息）。
    //   Running → runner->InjectUserTurn + StageEvent("injected")（对话流可见反馈）；
    //   非 Running → StageEvent("inject_ignored")（不静默丢弃）。确定性 C++ 路由（NFR-7）。
    void InjectTaskMessage(CTaskInfo* a_pTask, const QString& a_sText);

    // —— 助手会话链持久化（#1）：sid + (prompt,sid) 撤销栈的存取（2026.09.03 起按助手 id）——
    QString AsstAdhocSid(const QString& a_sAsstId) const;
    QString AsstAdhocGuideHash(const QString& a_sAsstId) const;   // 该助手上次投喂 guide.md 的哈希
    QJsonArray AsstAdhocStackToJson(const QString& a_sAsstId) const;
    // 助手组恢复（档载入/新建助手）时装入该助手的会话链（不发信号；首条提问前调用）。
    void LoadAsstAdhoc(const QString& a_sAsstId, const QString& a_sSid,
                       const QJsonArray& a_aStack, const QString& a_sGuideHash = QString());
    // 删除/丢弃助手时清理其会话（含在跑 runner；调用方 UI 已挡运行中删除，此处兜底停）。
    void RemoveAsstAdhoc(const QString& a_sAsstId);
    // 档切换/切工程：停全部助手并清空全部会话（新档经 LoadAsstAdhoc 重装）。
    void ResetAdhocSessions();

signals:
    void sigTaskChanged(CTaskInfo* a_pTask);
    void sigStageLog(CTaskInfo* a_pTask, const QString& a_sLine);       // 原始/系统日志流
    void sigStageEvent(CTaskInfo* a_pTask, const QString& a_sKind,     // 结构化事件(阶段生命周期)
                       const QString& a_sText);
    // 结构化对话流信号(携带完整内容,由 UI 决定折叠/截断)——
    // 用于把日志区渲染成 Claude Code 风格(Markdown 助手块 + 可折叠工具卡 + 事件徽章)
    // 末尾 a_sViewId 为评审多视角并行时的视角标识（v1/v2/...）；空=单 runner 串行路径（向后兼容）。
    void sigAssistantText(CTaskInfo* a_pTask, const QString& a_sText,           // 助手文本(全文)
                          const QString& a_sViewId = QString());
    void sigToolUse(CTaskInfo* a_pTask, const QString& a_sTool,                 // 工具调用(全文入参)
                    const QString& a_sInputJson, const QString& a_sViewId = QString());
    void sigToolResult(CTaskInfo* a_pTask, const QString& a_sResult,            // 工具结果(全文)
                       const QString& a_sViewId = QString());
    void sigTaskFinished(CTaskInfo* a_pTask, bool a_bOk);

    // —— 提示词 agent 专属信号（不经任务，路由到「助手」Tab）——
    void sigAdhocLog(const QString& a_sAsstId, const QString& a_sLine);           // 原始/系统日志
    void sigAdhocAssistantText(const QString& a_sAsstId, const QString& a_sText); // 助手文本
    void sigAdhocToolUse(const QString& a_sAsstId, const QString& a_sTool, const QString& a_sInput);
    void sigAdhocToolResult(const QString& a_sAsstId, const QString& a_sResult);
    void sigAdhocEvent(const QString& a_sAsstId, const QString& a_sKind, const QString& a_sText);  // 结构化事件徽章
    void sigAdhocContext(const QString& a_sAsstId, qint64 a_iFill);   // 上下文占用（token）；-1=未知
    void sigAdhocModel(const QString& a_sAsstId, const QString& a_sModel);   // 助手所用模型名（供面板查上下文窗口）
    // 2026.08.27 运行中实时计量（任务阶段/助手轮次通用）：本轮累计 输入/输出 token，
    //   每完成一次 API 请求发一次（Claude Code 式“当前指令消耗”直观反馈）；-1/-1=本轮结束清零。
    void sigLiveUsage(qint64 a_iIn, qint64 a_iOut);
    void sigAdhocUndo(const QString& a_sAsstId);                       // 撤销该助手最后一条提问（面板清尾块）
    void sigAdhocFinished(const QString& a_sAsstId);                   // 该助手一次调用结束（启用输入栏）
    void sigAdhocRunning(const QString& a_sAsstId);                    // 2026.08.19 一次助手调用开始（面板进运行态；自动续发排队指令时面板无法从 UI 事件感知）
    void sigAdhocStateChanged(const QString& a_sAsstId);               // 该助手 sid/撤销栈变化（#1 存盘触发）
    // 2026.08.15 一次成功助手调用的消耗（输入含缓存读/写，与账单同口径）+ 本次调用耗时（ms）
    //   + API 请求数，由 CMainWindow 归集到当前档任务（助手档按任务分档，消耗跟随档走）。
    void sigAdhocUsage(const QString& a_sAsstId, qint64 a_iIn, qint64 a_iOut, double a_dCost,
                       qint64 a_iElapsedMs, int a_iReqs);
    // 2026.08.20 Agent 自动命名：一次自动命名调用完成（含失败）→ 主窗口做 DB 副本
    //   镜像 + 刷新两表。a_bOk=false 时 a_sName 为空。
    void sigAutoNameDone(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sName);

private slots:
    void OnRunnerInfo(const QString& a_sLine);
    void OnRunnerAssistantText(const QString& a_sText);
    void OnRunnerToolUse(const QString& a_sTool, const QString& a_sInput);
    void OnRunnerToolResult(const QString& a_sResult);
    void OnRunnerFinished(bool a_bOk, const QString& a_sResult,
                          qint64 a_iIn, qint64 a_iOut, double a_dCost, const QString& a_sSid);

    // —— 提示词 agent 槽：仅 emit sigAdhoc*（带助手 id），不触碰 m_pCurTask、不调 Advance ——
    // 2026.09.03 多助手并发：id 由 per-会话 runner 连接处的 lambda 注入（见 EnsureAdhocRunner）。
    void OnAdhocInfo(const QString& a_sAsstId, const QString& a_sLine);
    void OnAdhocAssistantText(const QString& a_sAsstId, const QString& a_sText);
    void OnAdhocToolUse(const QString& a_sAsstId, const QString& a_sTool, const QString& a_sInput);
    void OnAdhocToolResult(const QString& a_sAsstId, const QString& a_sResult);
    void OnAdhocFinished(const QString& a_sAsstId, bool a_bOk, const QString& a_sResult,
                         qint64 a_iIn, qint64 a_iOut, double a_dCost, const QString& a_sSid);

private:
    void RunCurrentStage(CTaskInfo* a_pTask);
    // 阶段实际启动（组装 prompt + 起 runner）：RunCurrentStage 在改码阶段会先异步备份，
    // 备份完成后再回调到这里；非改码阶段直接调用。
    void LaunchStage(CTaskInfo* a_pTask, const SStageDef& a_stDef);
    void Advance(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sResult);
    QString BuildPrompt(CTaskInfo* a_pTask, const SStageDef& a_stDef);
    void RecordSummary(CTaskInfo* a_pTask, EStage a_eStage, bool bOk, const QString& a_sSummary);
    // 2026.08.20 (自动命名·本地兜底)：Agent 起名不可用（助手忙/工程未开）时的同步
    //   启发式：用户原始需求信息量最大行 → guide.md 首个非泛型标题，清洗截 24 字。
    //   返回是否成功命名。
    bool MaybeAutoNameTask(CTaskInfo* a_pTask);
    // FR-1：Verify 阶段额外向 record.md 写一条「验证结论：…」行（与 RecordSummary 并存）。
    // 结论来源优先取 verify.md 的「最终验证结论」行；缺失回退 a_sResult / m_sLastAssistantText。
    void RecordVerifySummary(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sResult);
    EReviewDecision ParseReviewDecision(CTaskInfo* a_pTask, EStage a_eReview, const QString& a_sRaw);
    // 2026.08.12 (修驳回死循环) 取最近一次评审输出(cr.md 等)的阻塞项摘要，供驳回/熔断事件文本带出。
    QString ExtractRejectDigest(CTaskInfo* a_pTask);
    // 2026.08.12 (修驳回死循环) 熔断时把驳回焦点 + 阶段历史写进 escalation.md，供用户接管决策。
    void WriteEscalationReport(CTaskInfo* a_pTask, EStage a_eBack);
    void EnsureRunner();

    // FR-6（Q6 默认=a 复用阶段摘要）：阶段成功结束时把摘要回填到本次备份版本（非首次版本）。
    //   （改码前备份已移入 RunCurrentStage 的后台线程路径，序号 m_iCurBackupSeq 在其 continuation 记录。）
    void MaybeBackfillBackupSummary(CTaskInfo* a_pTask, EStage a_eStage, const QString& a_sSummary);

    // 写一条带时间戳/分类的流水到任务私有 .autodev/tasks/<id>/run.log（#6 完整日志归档）。
    void WriteRunLog(CTaskInfo* a_pTask, const QString& a_sCategory, const QString& a_sText);
    // 同时落日志 + 发阶段事件信号（统一入口，避免每个 emit 处重复写日志）。
    void StageEvent(CTaskInfo* a_pTask, const QString& a_sKind, const QString& a_sText);
    // 阶段输入自愈（#5）：检查阶段声明输入是否存在，缺失则回退到生产阶段重跑。
    // 返回 true=输入就绪可继续；false=已回退并安排重跑，调用方应立即 return。
    bool EnsureStageInputs(CTaskInfo* a_pTask, const SStageDef& a_stDef);

    // —— 开发可靠性：续跑 / 编译门 / 动态预算 ——
    // 2026.08.15 策略参数配置化：原 static constexpr 常量改读 config.ini [Policy] 段
    //   （m_pPm 现读，设置对话框保存后即生效；m_pPm 为空时回退内置默认）。
    int MaxIter() const;          // 单阶段最大迭代（驳回重跑熔断阈值，默认 3）
    int MaxResume() const;        // 单阶段 --resume 续跑封顶（默认 3）
    int ReviewResume() const;     // (C) 轻量评审 max-turns 续跑封顶（默认 1，仅续跑 1 次即降级）
    void ReRunResume(CTaskInfo* a_pTask, const QString& a_sCont);      // --resume 续跑本阶段（带针对性 prompt）
    void ProceedToNext(CTaskInfo* a_pTask, const QString& a_sResult);  // 成功后推进：评审处理 + 下一阶段

    // —— 评审多视角并行（fan-out / fan-in）——
    // 启动 N 个只读评审 runner（同构多投票），各写独立 <out>_v<k>.md；全部完成 fan-in 聚合。
    void StartReviewParallel(CTaskInfo* a_pTask, const SStageDef& a_stDef, const QString& a_sPrompt);
    // 2026.08.15 (流程档位) 本轮评审实际视角数：简单档 CodeReview=1（串行路径），其余=全局配置。
    //   LaunchStage 分支与 StartReviewParallel 启动数都取它，防档位与配置打架。
    int ReviewViewCount(const CTaskInfo* a_pTask, EStage a_eStage) const;
    // 单视角 runner 完成：缓存结果，待完成集合空时 fan-in。不碰 m_pCurTask、不直接调 Advance（并发安全）。
    void OnReviewViewFinished(CTaskInfo* a_pTask, const QString& a_sViewId, bool a_bOk, const QString& a_sResult,
                              qint64 a_iIn, qint64 a_iOut, double a_dCost, const QString& a_sSid,
                              int a_iReqs);
    // 全部视角完成：聚合决策（严格档一票 Reject 即 Reject；标准/简单档多数决）、
    //   合并正文写回 sOutput（保 cr.md 回灌闭环；少数派驳回意见以专段携带）、推进。
    void FanInReview(CTaskInfo* a_pTask);
    // 评审阶段推进：Pass→NextStage；Reject→IncStageIter/RejectTarget/熔断（抽自 ProceedToNext 评审分支）。
    void FinishReviewStage(CTaskInfo* a_pTask, EReviewDecision a_eDec);
    // 终止所有评审 runner（Stop/StopAll 调用，防泄漏进程）。
    void StopReviewRunners();
    void RunCompileGate(CTaskInfo* a_pTask);                           // 异步跑构建校验（不阻塞 UI）
    void OnBuildFinished(int a_iExitCode);                             // 构建结束续接流程
    QString ResolveBuildCommand(CTaskInfo* a_pTask) const;             // 解析构建命令（build.txt → CMakePresets → .sln）
    // (A) 编译门结论持久化：落 build_verdict.md，供 CodeReview/Test 的 BuildPrompt 直读，
    //   消除 agent 自行 grep run.log 推断构建状态的自我怀疑（be3fd900 根因之一）。
    void WriteBuildVerdict(CTaskInfo* a_pTask, const QString& a_sStatus,
                           int a_iExitCode, const QString& a_sCmd, const QString& a_sTail);
    QString ReadBuildVerdict(CTaskInfo* a_pTask) const;
    QString LocateMsBuild() const;                                     // 定位 msbuild（PATH → vswhere）
    int EstimateDevTurns(CTaskInfo* a_pTask) const;                    // 按 plan.md 任务数估算开发轮次

    static bool IsReviewStage(EStage a_eStage);
    static bool IsResumeEligible(EStage a_eStage);   // 该阶段命中 max-turns 时可 --resume 续跑
    static bool IsLightReviewStage(EStage a_eStage);  // (C) 轻量评审（不含 Test）—— max-turns 降级而非判死
    // 2026.08.15 (流程档位) 任务的生效阶段流：ResolveFlow(type, task.flowStages)——
    //   清单为空即完整规范流（旧任务兼容）。编排器内所有流转/输入过滤/意见携带统一走它。
    static QList<EStage> TaskFlow(const CTaskInfo* a_pTask);
    // 2026.08.15 (评审多数决) 上游评审的少数派驳回意见段（不阻断）→ 下一生产阶段的软注入块。
    //   当前阶段在流中前一阶段是评审、且其产物含「## 少数派驳回意见」标记时返回非空。
    QString MinorityFeedbackBlock(CTaskInfo* a_pTask, const SStageDef& a_stDef);
    QString ReviewResumeHint(CTaskInfo* a_pTask, EStage a_eStage);  // (C) 评审续跑提示：催落盘结论
    // 2026.08.15 (驳回差异化引导) 生产阶段 → 驳回它的评审产物文件名（相对任务目录；非生产阶段返回空）。
    static QString RejectFeedbackFile(EStage a_eStage);
    // 2026.08.15 (驳回差异化引导) 按迭代次数递进的策略文本：每轮回炉给不同引导，防同思路死循环。
    static QString IterationStrategyHint(int a_iIter);
    int  MaxTurnsFor(CTaskInfo* a_pTask, EStage a_eStage) const;
    QString StageNameZh(EStage a_eStage) const;

    // —— 提示词 agent 内部 ——
    // 提示词 agent 运行模式：Idle=空闲；Submitting=用户提问中；
    // CompactSummarize=压缩第一步（生成摘要）；CompactReseed=压缩第二步（摘要投新会话）；
    // AutoNaming=自动命名（拼任务内容+guide.md 提示词交 Agent 起名，不动用户会话栈）。
    enum EAdhocMode { Idle, Submitting, CompactSummarize, CompactReseed, AutoNaming };

    // —— 助手会话（2026.09.03 多助手并发）：每助手独立 runner 进程 + 会话链 + 状态机 ——
    //   会话常驻 m_aAdhoc（id→堆指针，QHash 重哈希不移动指向体）；档切换/切工程整体
    //   Reset；后端漂移在 EnsureAdhocRunner 内按会话清链（runner 空闲时）。
    struct SAdhocSession {
        QString               sId;             // 助手 id（索引键）
        CAgentRunnerBase*     pRunner = nullptr;   // 该助手的 runner（独立 QProcess，懒建）
        QString               sSid;            // 会话链基点 sid（首条前为空）
        QString               sGuideHash;      // 上次投喂 guide.md 的 SHA256（变更重投判定）
        QList<QPair<QString, QString>> aStack; // (prompt, sid) 撤销链；撤销=弹栈回基点
        QStringList           aTranscript;     // 跨轮上下文回灌缓存（DSH/Codex/Hermes 用）
        bool                  bReadOnly = false;    // 本轮只读开关
        EAdhocMode            eMode = Idle;    // 异步流程状态机（多助手各一份，互不干扰）
        CTaskInfo*            pAutoNameTask = nullptr;   // AutoNaming 模式待命名任务
        QString               sPendingPrompt;  // Submitting 模式当前 prompt（压栈用）
        QString               sSummary;        // CompactSummarize 产出、CompactReseed 投递
        QString               sLastTaskId;     // 最近一次 Submit 的任务档 id（续发记忆沿用）
        QDateTime             dtStart;         // 本轮起点（耗时归集）
        int                   iResume = 0;     // max-turns 自动续跑计数（上限 2，新提问清零）
        QStringList           aPendingInjects; // 同助手运行中排队指令（本轮结束自动续发）
    };
    SAdhocSession* AdhocSess(const QString& a_sAsstId, bool a_bCreate = false);  // 取会话（bCreate=无则建）
    void EnsureAdhocRunner(SAdhocSession& a_S);   // 懒创建该会话 runner 并连信号（id 注入路由）
    void StartAdhocStage(SAdhocSession& a_S, const QString& a_sPrompt,
                         bool a_bDropLastUser = false);   // 按会话 sSid/bReadOnly 组装 argv 起一次；2026.09.02 a_bDropLastUser=回灌时剔除末尾本轮提问行（prompt 尾部已含原文，防重复）

    CProjectManager* m_pPm;
    CAgentRunnerBase* m_pRunner = nullptr;   // 2026.08.18 基类指针：后端经 CAgentRunnerFactory 全局单选
    CTaskInfo*       m_pCurTask = nullptr;
    QString          m_sLastAssistantText;  // 缓存最新助手文本，用作摘要来源
    bool             m_bSingleStageRerun = false;  // 需求3：单阶段重跑标志（Advance 完成后停原阶段，不推进）
    QDateTime        m_dtStageStart;        // 本轮阶段启动时刻，用于判定评审输出文件是否本轮更新
    int              m_iCurBackupSeq = 0;   // FR-6 本轮阶段改码前备份的版本序号（0=未备份，stage_done 回填摘要后清零）
    QProcess*        m_pBuildProc = nullptr;  // 编译门专用进程（异步跑 msbuild，不阻塞 UI）
    QByteArray       m_bufBuild;             // 编译门 stdout+stderr 累积缓冲
    QString          m_sLastBuildCmd;        // (A) 本轮编译门命令（RunCompileGate 解析，OnBuildFinished 写 verdict 用）

    // —— 启动路径重活后台线程化（防「启动/继续」点击卡死 UI）——
    // 启动预备（guide 指纹增量同步 + 任务目录初始化）与改码前全量备份均为纯文件 IO、
    // 可达秒级（千级文件扫描 / 整目录 zstd 拷贝），移出 UI 线程，完成后回主线程续跑。
    std::thread      m_thStartPrep;          // StartTask 预备线程（同时刻至多一个，guards 保证不重叠）
    std::thread      m_thBackup;             // RunCurrentStage 改码备份线程（同上）
    std::atomic_bool m_bBackupRunning = {false};   // 2026.08.22 m_thBackup 运行标志（StopAll join / 回退守卫）
    std::thread      m_thAdhocBackup;        // 2026.08.26 助手轮 turn 快照线程（大工程全量扫描+哈希可达数十秒，
                                             //   同步跑在 UI 线程会卡死；与 m_thBackup 独立防任务/助手并发重叠）

    // 2026.08.27 助手 guide 同步空闲节流：非只读轮结束不再立即 SyncGuideAsync（大工程下
    //   该调用要 Read 整份 guide+Edit，且与下一轮提示词并发抢网关——助手每步极慢主因
    //   之二）；改空闲 3 分钟同步，活跃期（新轮/agent 仍跑）持续顺延。
    QTimer m_tGuideSyncIdle;

    // 2026.08.26 助手轮 turn 快照改后台线程：不阻塞提交（失败仅事件提示，不阻断本轮）。
    void AsyncAdhocBackup(const QString& a_sAsstId, const QString& a_sOwner, const QString& a_sPrompt);

    // —— 提示词 agent 状态（2026.09.03 多助手并发：per-助手会话收拢进 SAdhocSession）——
    QHash<QString, SAdhocSession*> m_aAdhoc;                       // 助手 id → 会话（常驻；档切换 Reset）
    // 2026.08.27 本轮实时计量累计（任务阶段/助手轮次通用；阶段/轮次开始清零）
    qint64         m_iLiveIn  = 0;
    qint64         m_iLiveOut = 0;
    void ResetLiveUsage() { m_iLiveIn = 0; m_iLiveOut = 0; emit sigLiveUsage(0, 0); }
    // ↑ 以上 per-助手 状态（guideHash/只读/模式/命名任务/pending/摘要/撤销栈）已全部
    //   迁入 SAdhocSession（多助手各持一份）。
    // 2026.09.02 跨轮上下文回灌：最近对话记录（"用户：…"/"助手：…"，时间升序）。
    //   DSH/Codex/Hermes 每轮全新会话（KeepsContextAcrossTurns()==false），第 2 轮起
    //   由 StartAdhocStage 把此记录注入 prompt 头部恢复上下文；权威源=面板对话流块
    //   （每次外部提交整体重建，含重启后恢复的历史），轮内 assistant 行由
    //   OnAdhocAssistantText 追加、内部续发沿用缓存。claude/pi 原生续接，不注入。
    // 回灌块/追加行按会话操作（成员移入 SAdhocSession：aTranscript）
    QString        BuildAdhocHistoryBlock(const SAdhocSession& a_S, bool a_bDropLastUser) const;   // 回灌块（无历史/后端可续接→空串）
    void           AppendAdhocTranscriptLine(SAdhocSession& a_S, const QString& a_sLine);    // 追加一行 + 预算裁剪（保尾部）
    // 2026.08.19 运行中介入：
    //   任务侧——DSH 后端协议不支持轮中插话，InjectUserTurn 返回 false 时排队，
    //     LaunchStage 时前置并入下一阶段 prompt（换任务/停止即作废）。
    //   助手侧——运行中收到的追加指令：claude 后端走 InjectUserTurn 轮中注入即时不达此
    //     队列；DSH 后端排队，本轮成功收尾（OnAdhocFinished）后自动按同会话续发下一条。
    QStringList    m_aPendingTaskInjects;
    // （助手侧排队指令/最近任务档 id 已迁 SAdhocSession：aPendingInjects/sLastTaskId）

    // —— 评审多视角并行（fan-out / fan-in）状态 ——
    struct SViewResult {       // 单视角评审 runner 的完成结果
        bool            bOk    = false;
        EReviewDecision eDec   = EReviewDecision::Pass;  // 该视角决策（复用 ParseReviewDecision Tier1 正则）
        QString         sSummary;                        // 该视角摘要正文（fan-in 合并用）
        QString         sSid;
        double          dCost = 0.0;
        qint64          iIn   = 0;
        qint64          iOut  = 0;
    };
    int                         m_iReviewParallel = 3;     // 评审并行视角数（1=串行回退；默认 3）
    int                         m_iCurReviewViews = 0;     // 本轮 fan-out 实际视角数（档位可低于全局配置；fan-in 按它聚合）
    bool                        m_bWebSearch = true;       // 阶段联网检索开关（config.ini [Agent] WebSearch，默认开）
    QList<CAgentRunnerBase*>    m_aReviewRunners;         // fan-out 视角 runner（parent=this，Qt 托管生命周期）
    QSet<QString>               m_aPendingViews;          // 待完成视角集合（空→全部完成 fan-in）
    QHash<QString, SViewResult> m_aViewResults;           // viewId → 完成结果
};
