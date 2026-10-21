#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QStringList>
#include <functional>

class QProcessEnvironment;
class QTableWidget;
class QPushButton;
class QLabel;
class QProcess;
class QTimer;

// 后端版本管理对话框（2026.09 任务 23183ccb，D1=B 独立对话框）：
//   四 Agent 后端（1=Claude Code 2=DSH 3=Pi 4=Codex，后端号沿用全局约定）
//   当前版本 / 最新版本 / 结论 / 更新入口一览。
//   - 本地版本：DSH/Pi/Codex 纯文件读（毫秒级同步）；Claude 子进程异步探测
//     （cmd /c <ClaudeCmd> --version，10s 硬超时）+ npm 全局包回退。
//   - 最新版查询：手动触发，4 路异步（npm view ×3 + git ls-remote ×1，15s 硬超时），
//     结果只存会话内存（NFR-2 零 config.ini 键、几何不持久化）。
//   - 更新执行：ShellExecuteW runas + cmd /k 可见提权窗口（同既有安装按钮口径）。
//   - 异步模型：信号驱动 QProcess + QTimer（UI 线程事件循环），零新线程/零
//     Qt6::Concurrent（工程与部署面均无该模块）。
//
// 2026.10.21 后端可用性验证（用户需求）：
//   - 表增「可用」列 + 「验证所有后端」按钮（在「查询最新版」之后）：逐后端拉起
//     真实进程做进程级探测（CLI --version / 配置预检；DSH 为 JSON-RPC initialize
//     握手——它是长驻协议进程，--version 无法证明可服务）。结果只存会话内存
//     （同最新版查询口径，不落盘）。
//   - 行右键菜单「自动修复」：本行后端不可用时，选一个已验证可用的**其他**后端
//     经助手发送自愈提示词（含故障现象/安装位置/harness.md 索引），由 Agent 按
//     harness.md 的后端逻辑就地修复。提示词经 sigAutoFixRequested 交主窗口接线
//     （新建「后端自愈」助手并绑定修复后端，见 CMainWindow::OnBackendAutoFix）。
//
// 双入口：主窗功能菜单「后端版本管理...」与设置页 Agent 组按钮（T5 接线）。
// 窗口几何不持久化（瞬态语义）；关闭对话框＝放弃在飞查询（析构杀净全部子进程）。
class CVersionDialog : public QDialog {
    Q_OBJECT

public:
    // D6 运行中拦截判据（true=该后端有任务/助手在跑）。由 CMainWindow 注入
    //   （BackendBusyFn：该后端==当前全局后端 && IsRunnerBusy()||IsAdhocRunning()）；
    //   空（测试/无拦截场景）＝直接放行。
    using FBusyCheck = std::function<bool(int)>;

    explicit CVersionDialog(FBusyCheck a_fnBusy, QWidget* a_pParent = nullptr);
    ~CVersionDialog() override;

    // —— 纯函数（单测靶点，无 IO）——
    // ls-remote --tags 输出行集 → 最大版本 tag 全名（如 "dsh-v0.1.2-alpha.3"）。
    //   规则：滤 "^{}" 行；行形态 "<sha>\trefs/tags/<tag>" 或裸 tag 名均可；
    //   名匹配 ^(?:dsh-)?v?(\d+(?:\.\d+)*)([-.].*)?$；
    //   排序键 = QVersionNumber 核心降序 → 预发布等级降序（无>rc>beta>alpha>dev）
    //   → 后缀自然数降序（数字段按数值比较，rc.10 > rc.9）。无有效 tag→空串。
    static QString PickLatestTag(const QStringList& a_aLines);
    // "dsh-v0.1.2-alpha.3" → "0.1.2-alpha.3"（剥 dsh-/v 前缀）；无效→原样返回。
    static QString TagToVersion(const QString& a_sTag);
    // 对比结论文案（优先级链）：相等→"已是最新"；backend==4→恒"已锁定
    //   <ExpectedCodexVersion()>（v0.88+ 已移除 wire_api=chat，不提供升级）"；latest 空→"未查询"；
    //   可解析且 latest>local→"有新版本 <latest>"；其余不等→"与远端不同：
    //   <local> → <latest>"。
    // 2026.09.20 Hermes（backend=5）：git 部署无 npm 包，改用发布 tag 版本号比对
    //   （同 DSH tag 口径；2026.09.20 旧结论「无语义化远端版本」实为误判——上游
    //   有 34 个 v2026.M.D 日历发布 tag，之前只看到字母序在前面的 backup/ 杂项 tag）。
    // 2026.09.21 改版：本地=git describe 最近可达发布 tag（日历版本核心），远端=
    //   ls-remote --tags 最大版本 tag；比对/文案与其余 Harness 同为发布版本号语义，
    //   HEAD sha 不再进结论（main tip 高频提交刷屏，发布 tag 语义下 verdict 稳定）。
    //   a_sHermesSyncAt=本地仓上次 fetch 时刻文本（.git/FETCH_HEAD mtime），仅
    //   「有新版本」时尾注；缺省参数保持旧签名兼容（既有单测三参调用不变）。
    static QString Verdict(const QString& a_sLocal, const QString& a_sLatest, int a_iBackend,
                           const QString& a_sHermesSyncAt = QString());
    // Hermes 远端 tag 列表解析（T6 纯函数）：逐行 ls-remote --tags 输出，仅认
    //   v<数字(.数字)+> 发布 tag（排除 backup/、premerge 等杂项），同版本名优先
    //   剥离 ^{} 行的 commit sha（附注 tag 的实体）；取 QVersionNumber 最大者，
    //   返回版本核心（去 v 前缀，如 "2026.9.14"），a_pShaOut 回写其 commit sha；
    //   无有效 tag 返回空串。
    static QString PickLatestHermesTag(const QStringList& a_aLines, QString* a_pShaOut);
    // git describe 输出解析（T6 纯函数）："v2026.9.14" / "v2026.9.14-4881-gabc"
    //   → "2026.9.14"（去 v 前缀，截断 -N-g 后缀）；无版本核心 → 空串。
    static QString ParseHermesDescribe(const QString& a_sDescribe);

    // 2026.10.21 自愈提示词组装（纯函数，单测靶点）：故障后端号 + 显示名 + 安装位置
    //   + 故障现象 + harness.md 路径 → 完整自愈提示词（含读文档/定位/修复/验证/
    //   汇报五步要求）。a_sDetail 为探测失败原因（可空）。
    static QString BuildAutoFixPrompt(int a_iBackend, const QString& a_sName,
                                      const QString& a_sLocation, const QString& a_sDetail,
                                      const QString& a_sHarnessMdPath);

    // 2026.10.21 可用性探测态（公开枚举：外部经 ProbeStateOf 读行态）。
    enum EProbeState { EProbeIdle = 0, EProbeRunning, EProbeOk, EProbeFail };

    // 2026.10.21 探测详情访问器（ui-sandbox/单测钩子）：a_iBackend 行的验证态与
    //   不可用原因全文（不可变；不触碰 UI）。
    EProbeState ProbeStateOf(int a_iBackend) const { return m_aeProbe[a_iBackend]; }
    QString     ProbeDetailOf(int a_iBackend) const { return m_asProbeDetail[a_iBackend]; }

signals:
    // 2026.10.21 自动修复分派：a_iFaultBackend=故障后端号（1..5），a_iFixBackend=
    //   执行修复的后端号（已验证可用的其他后端；0=无可用——主窗口侧应提示先验证）。
    //   a_sPrompt=BuildAutoFixPrompt 组装的完整提示词。主窗口新建「后端自愈」助手、
    //   绑定修复后端并提交（复用助手粘性后端机制，修复过程全程可见可中断）。
    void sigAutoFixRequested(int a_iFaultBackend, int a_iFixBackend, const QString& a_sPrompt);

private:
    // QProcess 池键（用途枚举，风险 #8：同键先杀旧再启新，结果只经 finished 信号写行）。
    enum EProcKey {
        EProcNone        = 0,
        EProcClaudeLocal = 1,    // Claude 本地版本探测（cmd /c claude --version）
        EProcQueryClaude = 10,   // npm view @anthropic-ai/claude-code version
        EProcQueryPi     = 11,   // npm view @earendil-works/pi-coding-agent version
        EProcQueryCodex  = 12,   // npm view @openai/codex version
        EProcQueryDsh    = 13,   // git ls-remote --tags（DSH 最新版查询）
        EProcQueryHermes = 14,   // 2026.09.20 git ls-remote HEAD（Hermes 远端 sha 比对）
        EProcDshTag      = 20,   // git ls-remote --tags（DSH 更新前 tag 解析）
        EProcVerifyClaude = 30,  // 2026.10.21 可用性探测（Claude --version）
        EProcVerifyPi     = 31,  // （pi cli.js --version）
        EProcVerifyCodex  = 32,  // （codex.js --version）
        EProcVerifyHermes = 33   // （hermes --version）
    };

    void SetupUi();
    void RefreshLocalVersions();   // DSH/Pi/Codex 同步直读 + Claude 异步探测
    void QueryLatestAll();         // 4 路异步查询（手动触发）
    void OnClaudeLocalDone(int a_iExitCode, const QString& a_sOut);   // Claude 探测收尾
    void OnUpdateBackend(int a_iBackend);       // 更新分派（design §3.3 表）
    void ResolveDshTagThenUpdate();             // DSH：先异步 ls-remote 解析 tag 再提权
    void UpdateUpdateButtons();     // 行内更新按钮可用性（随本地安装态联动）
    // 可见 cmd /k 窗口执行（a_bRunAs=true 时提权 runas）；标题便于辨认多窗口。
    bool LaunchCmdWindow(bool a_bRunAs, const QString& a_sTitle, const QString& a_sCmdLine,
                         const QString& a_sWorkDir = QString());
    bool ConfirmIfBusy(int a_iBackend, const QString& a_sWhat);   // D6 提示确认
    // 2026.10.10 Node 预检闸（claude-code@2.x 引擎要求 Node>=22，旧 Node 残装时
    //   npm postinstall 新语法必解析失败）：PATH 上的 node -v 同步短探测
    //   （RunGitSync 同款口径，npm.cmd 解析的 node 同样来自 PATH，探测与执行一致）。
    //   返回 -1 = node 不在 PATH/探测失败；>=0 = 主版本（v0.12.2 → 0）。
    //   a_psVerOut 可空出参回写原始版本串（供提示文案展示）。
    static int ProbeNodeMajor(QString* a_psVerOut = nullptr);
    // npm 类更新前置闸：node 缺失或主版本 < a_iMinMajor 时弹窗给出
    //   「安装/升级 Node.js LTS」一键入口（install_env.bat /node）+「仍然继续更新」
    //   出口（保留旧行为）。返回 true=放行原更新流程；false=已拦截（升级中/取消）。
    bool EnsureNodeOk(int a_iMinMajor, const QString& a_sTitle);

    // 池化启动：杀同键旧进程 → 起新 QProcess（父=this）+ iTimeoutMs 单发 QTimer，
    //   finished/FailedToStart/超时 kill 收敛到 a_fnOnDone(退出码, 合并输出) 一次。
    void StartPooledProcess(int a_iKey, const QString& a_sProgram, const QStringList& a_aArgs,
                            int a_iTimeoutMs, std::function<void(int, const QString&)> a_fnOnDone);
    void KillPooledProcess(int a_iKey);   // 杀同键在飞进程+停表（重入防护）

    // —— 2026.10.21 可用性验证 ——
    // EProbeState：每行验证态（未验证/验证中/可用/不可用）；m_asProbeDetail 存不可用
    //   原因全文（单元格显摘要，tooltip 显全文）。
    void VerifyAll();                       // 「验证所有后端」入口：5 路异步探测
    void StartProbe(int a_iBackend);        // 单后端探测分派
    void OnProbeDone(int a_iBackend, bool a_bOk, const QString& a_sDetail);
    void SetAvailCell(int a_iRow, const QString& a_sText, const QString& a_sToolTip,
                      const QColor& a_stColor);
    void ShowRowContextMenu(const QPoint& a_stPos);   // 行右键菜单（验证/自动修复）
    // 各后端探测实现（返回 true=可用；a_sDetail 带原因/版本摘要）。
    bool ProbeByFiles(int a_iBackend, QString* a_sDetail);   // 纯文件/配置预检兑底
    // 池键（探测复用 StartPooledProcess；DSH 握手需写 stdin，单独状态机管理）。
    //   DSH 握手：m_pDshProbe 专用 QProcess + 行缓冲 + 30s 超时，收 id=1 响应判 ok。
    void StartDshHandshake();
    void FinishDshHandshake(bool a_bOk, const QString& a_sDetail);
    QProcess* m_pDshProbe = nullptr;
    QTimer*   m_pDshProbeTm = nullptr;
    QByteArray m_abDshProbeBuf;

    void SetLocalCell(int a_iRow, const QString& a_sText);    // 当前版本列
    void SetLatestCell(int a_iRow, const QString& a_sText);   // 最新版本列
    void SetVerdictCell(int a_iRow, const QString& a_sText);  // 结论列
    void RefreshVerdicts();       // 按 m_asLocalVer/m_asLatestVer 重填结论列
    void UpdateInfoLine();        // 信息栏：查询时刻/提示文案

    QTableWidget* m_pTable = nullptr;      // 5 行 × 6 列（2026.10.21 增「可用」列）
    QPushButton* m_aBtnUpdate[6] = {};     // [1..5] 行内更新按钮；[0] 弃用（后端号
                                           //   1..5 直接作下标，防越界占位）
    QPushButton* m_pBtnRefresh = nullptr;  // 刷新版本（本地重读）
    QPushButton* m_pBtnQuery   = nullptr;  // 查询最新版(联网)（手动触发，D2）
    QPushButton* m_pBtnVerify  = nullptr;  // 2026.10.21 验证所有后端（进程级探测）
    QPushButton* m_pBtnClose   = nullptr;  // 关闭
    QLabel*      m_pLblInfo    = nullptr;  // 查询时刻/失败提示/刷新闭环提示
    QString      m_asLocalVer[6];          // 会话内存本地版本（NFR-2 不落盘）
    QString      m_asLatestVer[6];         // 会话内存最新版（失败保留上次已知值）
    EProbeState  m_aeProbe[6] = {};        // 2026.10.21 每行验证态（下标=后端号）
    QString      m_asProbeDetail[6];       // 不可用原因全文（tooltip 用）
    bool         m_bProbeActive = false;   // 有探测在飞（信息栏状态）
    QDateTime    m_tProbeAt;               // 上次「验证所有后端」时刻（仅展示）
    QDateTime    m_tQueryAt;               // 「查询时刻」时点语义（D2 手动触发）
    QString      m_sHermesSyncAt;          // 2026.09.21 本地仓上次 fetch 时刻文本（同步
                                           //   查询 .git/FETCH_HEAD mtime；读不出为空）
    // 同步短命令捕获（本地 git 快调用 <1s；超时/失败 → 空串）。
    static QString RunGitSync(const QStringList& a_aArgs, const QString& a_sWorkDir);
    QHash<int, QProcess*> m_aProc;         // 信号驱动异步子进程池（键=EProcKey）
    QHash<int, QTimer*>   m_aTimer;        // 硬超时 kill 表（析构统一停净）
    bool          m_bTearingDown = false; // 析构期标志：短路业务回调（防析构中
                                           //   弹窗/同步 npm 回退卡关窗，评审 C1）
    FBusyCheck    m_fnBusy;                // D6 判据（空=放行）
};
