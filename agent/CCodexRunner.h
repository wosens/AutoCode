#pragma once

#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "CAgentRunnerBase.h"
#include "../core/CModelEntry.h"   // SDshModelEntry（自定义大模型表，2026.08.21）

class CCodexProxy;   // 2026.08.21 本地 max_tokens 注入代理（StartStage 起停）

// CodexHarness（OpenAI Codex CLI 0.87.0）JSONL 流式后端 runner（第四后端，2026.08）。
//
// 进程模型同 claude/pi 后端：**一阶段一进程**。StartStage 每次启动
// `node <root>\node_modules\@openai\codex\bin\codex.js exec --json
//  --skip-git-repo-check -C <workDir> -s danger-full-access -`（prompt 经 stdin，
// "-" 表示从 stdin 读 prompt），stdout 为 JSONL 事件流（thread.started/turn.started/
// item.completed/turn.completed/error），逐行解析映射到基类信号；进程退出即结束。
//
// 关键口径（0.87.0 实测，见任务 memory/design）：
//   - 版本锁 0.87.0：v0.88+ 已移除 wire_api="chat"（本机网关仅 chat completions 可用），
//     版本不符时检测/启动给出明确指引（NFR-4）。
//   - usage（turn.completed.usage）：input_tokens/cached_input_tokens/output_tokens，
//     累计上抛供 token 列展示；费用不折算 cost=0（费用列 "-"）。
//   - agent_message 整块在进程正常退出后 sigAssistantText（一条消息一块，同 Pi 口径）。
//   - API key 只写 ~/.codex/auth.json（requires_openai_auth=true 时 codex 自取），
//     绝不进命令行/环境变量/日志（NFR-3）。
//   - resume 不做（一阶段一进程基线）；thread_id 仅 sigInfo 展示。
//
// claude 专属参数降级：a_iMaxTurns/a_iAutocompactK/a_bEnableAskUser/a_bFork/
// a_sResumeSid/a_sDisallowedTools 忽略（非默认值时 sigInfo 提示一次）。
// InjectUserTurn 安全 no-op 返回 false（一次性进程）。
class CCodexRunner : public CAgentRunnerBase {
    Q_OBJECT

public:
    explicit CCodexRunner(QObject* a_pParent = nullptr);
    ~CCodexRunner() override;

    // 2026.08.21 代理实时计量已随 sigUsageDelta 即时累计进任务，
    // 收尾时基类口径（assistant 消息条数）与实时累计二选一，防双计。
    int LastReqCount() const override { return m_bUsageLive ? 0 : m_iLastReqCount; }

    // —— 进程级全局配置（config.ini [Agent] Codex*；所有 Codex runner 实例共享）——
    //   root = exe 同级 Execute\CodexHarness（npm 工程根）；node 留空走 PATH "node"；
    //   provider 留空 = "autocode"（~/.codex/config.toml 的 [model_providers.autocode] 槽位）；
    //   wireApi = "chat"/"responses"，默认 chat（v0.88+ 仅 responses）。
    static void    SetCodexRoot(const QString& a_sRoot);
    static QString CodexRoot();
    static void    SetCodexNodePath(const QString& a_sPath);
    static QString CodexNodePath();
    static void    SetCodexProvider(const QString& a_sProvider);
    static QString CodexProvider();
    static void    SetCodexModel(const QString& a_sModel);
    static QString CodexModel();
    static void    SetCodexApiKey(const QString& a_sKey);   // 明文存 config.ini（tooltip 告知）
    static QString CodexApiKey();
    static void    SetCodexBaseUrl(const QString& a_sUrl);
    static QString CodexBaseUrl();
    static void    SetCodexWireApi(const QString& a_sApi);
    static QString CodexWireApi();

    // —— 自定义大模型表（config.ini [CodexModels]；2026.08.21，口径同 Pi）——
    //   激活条目（下标 ≥0）覆盖 Provider/模型/BaseUrl/WireApi/APIKey 固定路由：
    //   provider=autocode-<n> 槽位写 ~/.codex/config.toml，key 写 auth.json。
    //   Set 时立即合成写 ~/.codex（启动路径与设置保存路径共用，保证磁盘正本一致）。
    static void    SetCodexModels(const QList<SDshModelEntry>& a_aModels, int a_iActive);
    static QList<SDshModelEntry> CodexModels();
    static int     CodexActiveModel();
    static QString CodexActiveProvider();   // 空 = 未启用自定义路由（走固定键）

    // —— 探测/诊断（仿 CPiAgentRunner 同名方法口径）——
    static QString ExpectedCodexVersion();   // 版本锁："0.87.0"
    static QString ResolveNodePath();
    static QString ResolveCodexJsPath();     // <root>\node_modules\@openai\codex\bin\codex.js
    static QString ResolveCodexVersion();    // 读包内 package.json 的 version；未装=空
    static bool    LooksLikeCodexRoot(const QString& a_sRoot);
    static QString ProbeSiblingRoot();       // exeDir{,/\..,/\../..} × \CodexHarness
    static QString CodexConfigError();       // 空=可用；非空=中文指引
    static QString CodexInstallDiagnosis();  // 半安装态诊断

    // —— ~/.codex 写入（设置页保存时调用）——
    // config.toml 行级手术（顶层 model/model_provider 原位替换或首节前插入；删除重建
    // 末尾 [model_providers.<provider>] 节；其余用户内容逐字保留；.bak 备份 + .tmp 原子
    // 替换）+ auth.json 仅置 OPENAI_API_KEY（其余键保留）。a_sCodexHome 空 = ~/.codex
    // （注入参数供单测离线验证）。错误/日志不含 key 明文。
    static bool WriteCodexUserConfig(QString* a_pErr = nullptr,
                                     const QString& a_sCodexHome = QString());

    QString BackendName() const override { return QStringLiteral("codex"); }

    void StartStage(const QString& a_sWorkDir,
                    const QString& a_sPrompt,
                    const QString& a_sDisallowedTools,
                    int a_iMaxTurns,
                    const QString& a_sResumeSid,
                    bool a_bFork,
                    int a_iAutocompactK,
                    bool a_bEnableAskUser) override;
    void Stop() override;
    bool IsRunning() const override;
    bool IsGraceActive() const override { return false; }
    bool IsRetryPending() const override;        // 恒 false（一次性进程）
    bool InjectUserTurn(const QString& a_sText) override;   // 安全 no-op

private slots:
    void slotOnStdOut();
    void slotOnStdErr();
    void slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus);
    void slotOnErrorOccurred(QProcess::ProcessError a_eError);

private:
    void Reset();
    void HandleLine(const QByteArray& a_sLine);   // JSONL 事件分发
    void HandleItemCompleted(const QJsonObject& oItem);
    void ApplyUsage(const QJsonObject& oUsage);
    void EmitFailed(const QString& a_sMsg);
    void CheckStall();

    QProcess*   m_pProc = nullptr;
    QByteArray  m_buf;                    // stdout 跨 readyRead 行缓冲
    bool        m_bFinished = false;      // 本轮已发 sigFinished（防重复）
    bool        m_bSawThread = false;     // 已收 thread.started（thread_id 已展示）
    bool        m_bSawAgentMsg = false;   // 已收 agent_message（成功判定）
    QString     m_sTurnText;              // 本轮 assistant 文本（整块）
    QString     m_sErrorEvent;            // error 事件文本（失败消息附带）
    qint64      m_iSumInTok  = 0;         // Σ input_tokens + cached_input_tokens
    qint64      m_iSumOutTok = 0;         // Σ output_tokens
    QStringList m_aStderrTail;            // stderr 尾部（失败消息附带）
    QElapsedTimer m_actClock;             // 距上次收到输出（stall 巡检）
    QTimer*     m_pStallTimer = nullptr;  // 停滞巡检定时器（30s 一拍）
    CCodexProxy* m_pProxy = nullptr;      // 2026.08.21 输出上限代理（http+chat 生效）
    bool m_bUsageLive = false;            // 2026.08.21 代理已上抛真实计量（收尾报 0 防双计）
};
