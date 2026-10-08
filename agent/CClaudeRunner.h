#pragma once

#include <QObject>
#include <QProcess>
#include <QByteArray>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>

#include "CAgentRunnerBase.h"

// 封装单次 `claude` 进程调用：通过 stdin 投递 prompt，
// 流式解析 stream-json 输出，转为 Qt 信号上抛。
//
// 2026.08.18 改继承 CAgentRunnerBase（多后端支持）：6 信号、Last* 元信息 getter
// 与状态成员上移基类，本类只保留 claude CLI 专属实现（进程参数/解析/宽限/重试）。
//
// 生命周期：一次 StartStage() 对应一次 claude 进程；Finished 后对象可复用或丢弃。
class CClaudeRunner : public CAgentRunnerBase {
    Q_OBJECT

public:
    explicit CClaudeRunner(QObject* a_pParent = nullptr);
    ~CClaudeRunner() override;

    // —— 进程级全局配置（config.ini [Agent] ClaudeCmd；所有 runner 实例共享）——
    // 2026.08.15：claude 命令名可配（默认 "claude"，依赖 PATH）。换特定版本/网关时改
    //   config.ini 即可，无需改码。空串回退默认。
    static void SetClaudeCmd(const QString& a_sCmd);
    static QString ClaudeCmd();

    // 2026-10-08 共享大模型表消费（设置→大模型「Claude Code 后端跟随本表」，任务/
    //   助手全覆盖）：跟随开关开时，无实例槽覆盖的 claude 轮次按共享表当前条目
    //   注入 ANTHROPIC_BASE_URL/API_KEY/MODEL（仅 anthropic-messages 兼容条目；
    //   开关关/条目非兼容 = 完全走 claude 自身配置，行为与旧版一致）。
    static void SetSharedModelTable(const QList<SDshModelEntry>& a_aModels, int a_iActive);
    static void SetFollowSharedModels(bool a_bOn);
    // 2026-10-08 Claude 专属参数（设置→Claude Code 页；与共享表无关，设了就注入）：
    //   思考预算 MAX_THINKING_TOKENS（<=0=不注入）+ 小模型 ANTHROPIC_SMALL_FAST_MODEL
    //   （空=不注入；claude 后台小任务的中转映射，网关需同时提供该模型名）。
    static void SetMaxThinkingTokens(int a_iTok);
    static void SetSmallFastModel(const QString& a_sModel);

    // 2026.08.15 [Agent] AskTimeoutSec：ask_user 等用户作答的超时（秒，默认 600）。
    //   MCP ask 服务跑在独立子进程（--mcp-ask-server），经环境变量 AUTOCODE_ASK_TIMEOUT_SEC
    //   传递（挂载 ask_user 的 StartStage 启动时注入）；非正数回退默认。
    static void SetAskTimeoutSec(int a_iSec);
    static int  AskTimeoutSec();
    // 2026.08.17 [Agent] StallMinutes：停滞检测阈值（分钟，默认 10）——进程活着但持续
    //   零输出（疑似网络/中继挂起）达到阈值 → kill 并以 subtype="stall" 报失败，
    //   上层凭 lastSessionId 自动续跑。挂载 ask_user 的调用（交互式）不启用停滞检测。
    static void SetStallMinutes(int a_iMin);
    static int  StallMinutes();

    // —— 2026.09 版本管理（任务 23183ccb）：Claude Code 版本解析三件套 ——
    //   （「后端版本管理」对话框与设置页状态行用；Codex/DSH/Pi 侧见各自 Runner。）
    // 纯解析：从 "2.1.240 (Claude Code)" 类输出取首个 x.y.z 起始的版本串；无匹配→空。
    static QString ParseClaudeVersionOutput(const QString& a_sOut);
    // npm 全局 @anthropic-ai/claude-code 包 package.json 绝对路径（npm root -g 定位，
    //   进程内缓存一次；npm 不可用/包未装→空串）。仅返回路径，version 由调用方读。
    static QString NpmGlobalClaudePkgPath();
    // 本机 Claude Code 版本："x.y.z" 或空串（CLI 与 npm 双源皆失败时不可区分未装/未知，
    //   界面统一显示「未安装/未知」）。链：cmd /c <ClaudeCmd> --version（5s 启动/10s
    //   结束，样板同 ClaudeSupportsAutocompact）→ 失败回退 npm 全局包 package.json。
    //   注：内部两步均为同步子进程，供异步驱动方（CVersionDialog 的 QProcess 探测）
    //   之外的测试/极端场景直接调用。
    static QString ResolveClaudeVersion();

    // 模型名 → 上下文窗口 token 上限（未命中回退 200K）。2026.08.15 从
    //   CExecutionPanel::ContextLimitForModel 下沉到 agent 层：编排器计算
    //   自动压缩比例阈值也要用（面板委托到此处，单一实现）。
    static qint64 ContextWindowForModel(const QString& a_sModel);

    QString BackendName() const override { return QStringLiteral("claude"); }

    // --resume + --fork-session 产生携带完整上下文的新 sid 分支：跨轮上下文原生延续。
    bool KeepsContextAcrossTurns() const override { return true; }

    // 启动一次阶段执行
    //   a_sWorkDir       claude 的工作目录（工程根），相对路径 .autodev/... 落在此处
    //   a_sPrompt        投递给 claude 的完整 prompt（经 stdin 传入）
    //   a_sDisallowedTools  --disallowedTools 参数（deny 黑名单），如 "Bash,Edit"
    //   a_iMaxTurns      --max-turns 兜底
    // （默认参数在基类声明处，经基类指针调用时取基类默认值）
    void StartStage(const QString& a_sWorkDir,
                    const QString& a_sPrompt,
                    const QString& a_sDisallowedTools,
                    int a_iMaxTurns,
                    const QString& a_sResumeSid,
                    bool a_bFork,            // --fork-session：建新 sid 分支（ad-hoc 撤销链用）
                    int a_iAutocompactK,     // >0 → --autocompact <n>k 自动压缩阈值（ad-hoc 用）
                    bool a_bEnableAskUser) override;  // true → 挂载 ask_user MCP server（ad-hoc 运行时提问）

    // 终止当前执行（kill 进程）
    void Stop() override;

    bool IsRunning() const override;

    // 2026.08.15 崩溃宽限期进行中（cmd.exe 已死、孤儿 node 落盘判定中）：
    // 此时 IsRunning()==false 但仍需 Stop/停止判定覆盖（否则停止请求会被宽限期后的自动续跑覆盖）。
    bool IsGraceActive() const override { return m_bGraceActive; }

    // 2026.08.15 快速失败重试等待中（5 秒延迟未到）：同上，IsRunning()==false 但仍占用 runner。
    bool IsRetryPending() const override { return m_bRetryPending; }

    // FR-3：向当前运行中的 claude 进程 stdin 追加一条用户轮次（Q2 默认=a「追加用户消息」）。
    //   仅 IsRunning() 时尝试写入；因 -p 一次性模式 prompt 投递后已 closeWriteChannel，
    //   实际能否作为新一轮 user turn 生效取决于协议（R1 已知限制）。返回是否写入成功。
    //   未运行或写入失败时安全 no-op，返回 false（不抛、不影响运行）。
    bool InjectUserTurn(const QString& a_sText) override;

private slots:
    void slotOnStdOut();
    void slotOnStdErr();
    void slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus);
    void slotOnErrorOccurred(QProcess::ProcessError a_eError);

private:
    // 2026-10-08 共享大模型表快照 + 跟随开关（CMainWindow 重应用时刷新；见公共段注释）。
    static QList<SDshModelEntry> s_aSharedModels;
    static int  s_iSharedActive;
    static bool s_bFollowSharedModels;
    // 2026-10-08 Claude 专属参数（见公共段 SetMaxThinkingTokens/SetSmallFastModel）。
    static int     s_iMaxThinkTok;
    static QString s_sSmallFastModel;

    void HandleLine(const QByteArray& a_sLine, int a_iEpoch);   // a_iEpoch=所属进程代次（识破旧进程残留行）
    void EmitFailed(const QString& a_sMsg);
    void Reset();
    // 2026.08.17 停滞检测巡检（30s 一拍）：进程活着但距上次收到任何输出超过阈值 →
    //   kill 并以 subtype="stall" 发 sigFinished（上层凭 lastSessionId 自动续跑）。
    void CheckStall();
    // 2026.08.15 崩溃误报修复：cmd.exe 被硬杀（QProcess::Crashed）时 node 可能仍在以孤儿进程
    //   运行并最终以 max-turns 正常收敛（a0c96b4d 验证阶段实测：cmd 死后 90 秒 node 仍在写
    //   transcript）。此时启动宽限定时器轮询 transcript 新增内容里的 max_turns_reached 标记，
    //   命中则按「达到轮次上限」发 sigFinished（上层自动 --resume 续跑），超时才真正判死。
    void BeginOrphanGrace(const QString& a_sWhy);
    void PollOrphanTranscript();
    QString FindTranscriptPath();   // 按 sid 定位 ~/.claude/projects/*/<sid>.jsonl（命中缓存）
    // 2026.08.15 快速失败自动重试：进程在短时间内非零退出且未收到 result（max-turns 后 0 秒间隔
    //   立即 --resume 的场景实测两次瞬态 exit 1：03:19 七秒死 / 12:39 一秒死；隔一段时间手动
    //   同参数续跑均成功——疑似上一进程会话清理/后台任务收尾未完）。原参数延迟重拉。
    void RelaunchStage();
    // 2026.08.19 运行中介入：把一段用户文本编码成 stream-json 输入帧（user message）
    //   写入 stdin（+换行）。--input-format stream-json 模式下 stdin 全程保持打开，
    //   运行中追加 user 帧 = claude 在当前轮内收到新的用户输入（转向/补充指令）。
    void WriteUserFrame(const QString& a_sText);

    QStringList m_aLastArgs;       // 最近一次启动的完整参数（重试用）
    QString     m_sLastPrompt;     // 最近一次投递的 prompt（重试用）
    QString     m_sLastWorkDir;    // 最近一次工作目录（重试用）
    QElapsedTimer m_uptime;        // 本次进程存活计时（判快速失败）
    int         m_iFastRetry = 0;  // 本次 StartStage 已自动重试次数（上限 2）
    bool        m_bRetryPending = false;  // 快速失败重试等待中（Stop/抢占判定覆盖用）
    // 2026.08.27 悬空 resume sid 自愈：--resume 的会话在本机不存在（工程自其他电脑拷贝，
    //   会话数据在 ~/.claude 不随工程走）时 claude 报「No conversation found with session
    //   ID」整轮失败——检测到即从参数剥离 --resume/--fork-session 杀进程新建会话重发本轮
    //   （仅一次，m_bResumeDropped 防循环）；m_bQuietRestart 标记 kill 收尾走静默重拉。
    bool        m_bResumeDropped  = false;
    bool        m_bQuietRestart   = false;

    QProcess*  m_pProc     = nullptr;
    QByteArray m_buf;          // 跨 readyRead 的行缓冲
    bool       m_bFinished = false;  // 是否已发 sigFinished（防重复）
    // 2026.08.19 进程代次（Reset 时 +1）：result 行收尾要在 emit sigFinished **之后**才关
    //   stdin（让 claude 收到 EOF 干净退出），而 emit 是同步的——上层槽可能在槽内同步
    //   StartStage（max-turns 自动续跑）已拉起新进程；凭代次守卫确保只关旧进程的 stdin。
    int        m_iProcEpoch = 0;
    // m_sLastSubtype / m_sLastSessionId / m_iLastContextFill / m_iLastReqCount /
    // m_sLastModel 已上移 CAgentRunnerBase（protected，Reset 时清零）
    qint64    m_iLiveEmitIn = 0;    // 2026.08.27 已经 sigUsageDelta 实时上抛的输入累计（防收尾双计）
    qint64    m_iLiveEmitOut = 0;   // 同上，输出累计
    qint64    m_iStreamIn = 0;      // 2026.08.27 流式中消息输入（stream_event/message_start）
    qint64    m_iStreamOut = 0;     // 流式中消息累计输出（stream_event/message_delta）
    QString    m_sMcpConfigPath;  // ask_user 模式下生成的 --mcp-config 临时文件（Reset 清理）
    QTimer*    m_pGraceTimer = nullptr;   // 崩溃宽限定时器（3s 一轮，上限 80 轮 = 240s）
    bool       m_bGraceActive = false;    // 宽限期进行中（抑制重复失败 / Stop 静默收尾用）
    int        m_iGraceTicks = 0;         // 宽限期已轮询次数
    qint64     m_iTranscriptSeen = 0;     // transcript 已读到的字节偏移（基线 = StartStage 时文件大小）
    QString    m_sTranscriptPath;         // 会话 transcript 路径缓存（首次命中后固定）
    // 2026.08.27 空结果失败真因挖掘：读 transcript 尾部找错误条目（isApiErrorMessage /
    //   error 级 system 行），返回拼接的错误文本（无则空串）——stderr 为空时（异机
    //   headless 失败常见）唯一可靠的信息源。
    QString MineTranscriptErrorTail();
    QStringList m_aStderrTail;            // 最近若干条 stderr 行（失败消息附带，防真因丢失）
    // 2026.09.22 进程树整杀（2d9e4c0c 实证：cmd 被 kill 后 node 以孤儿进程继续运行，
    //   任务判死后仍写出 design.md，编排器状态与磁盘事实脱节；旧注释见 BeginOrphanGrace）。
    //   Windows Job Object（KILL_ON_JOB_CLOSE）：cmd+node 整树同生共死，
    //   TerminateJobObject 一次杀干净，程序退出无残留。HANDLE 存 void* 避免头文件引 windows.h。
    void* m_hJob = nullptr;              // Job 句柄（runner 生命周期内复用，析构时关闭）
    bool  EnsureJobObject();             // 创建 Job（幂等）并把当前子进程 cmd 挂入（子孙全进树）
    void  KillProcessTree();             // 整树终止（TerminateJobObject；无 Job 退回 m_pProc->kill）
    // 2026.08.17 停滞检测：m_actClock 在每次收到 stdout/stderr 字节时重启，30s 巡检一次，
    //   超阈值且进程仍活着 → 挂起判定（subtype="stall"）。挂载 ask_user 的调用
    //   （等待用户作答可静默很久）不启用。
    QElapsedTimer m_actClock;             // 距上次收到子进程输出的计时
    QTimer*    m_pStallTimer = nullptr;   // 停滞巡检定时器（30s 一拍）
    bool       m_bStallWatch = false;     // 本轮调用是否启用停滞检测
};
