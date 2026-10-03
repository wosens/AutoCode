#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QStringList>
#include <functional>

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
        EProcDshTag      = 20    // git ls-remote --tags（DSH 更新前 tag 解析）
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

    // 池化启动：杀同键旧进程 → 起新 QProcess（父=this）+ iTimeoutMs 单发 QTimer，
    //   finished/FailedToStart/超时 kill 收敛到 a_fnOnDone(退出码, 合并输出) 一次。
    void StartPooledProcess(int a_iKey, const QString& a_sProgram, const QStringList& a_aArgs,
                            int a_iTimeoutMs, std::function<void(int, const QString&)> a_fnOnDone);
    void KillPooledProcess(int a_iKey);   // 杀同键在飞进程+停表（重入防护）

    void SetLocalCell(int a_iRow, const QString& a_sText);    // 当前版本列
    void SetLatestCell(int a_iRow, const QString& a_sText);   // 最新版本列
    void SetVerdictCell(int a_iRow, const QString& a_sText);  // 结论列
    void RefreshVerdicts();       // 按 m_asLocalVer/m_asLatestVer 重填结论列
    void UpdateInfoLine();        // 信息栏：查询时刻/提示文案

    QTableWidget* m_pTable = nullptr;      // 5 行 × 5 列（2026.09 增 Hermes 行 5）
    QPushButton* m_aBtnUpdate[6] = {};     // [1..5] 行内更新按钮；[0] 弃用（后端号
                                           //   1..5 直接作下标，防越界占位）
    QPushButton* m_pBtnRefresh = nullptr;  // 刷新版本（本地重读）
    QPushButton* m_pBtnQuery   = nullptr;  // 查询最新版(联网)（手动触发，D2）
    QPushButton* m_pBtnClose   = nullptr;  // 关闭
    QLabel*      m_pLblInfo    = nullptr;  // 查询时刻/失败提示/刷新闭环提示
    QString      m_asLocalVer[6];          // 会话内存本地版本（NFR-2 不落盘）
    QString      m_asLatestVer[6];         // 会话内存最新版（失败保留上次已知值）
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
