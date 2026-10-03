#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "CAgentRunnerBase.h"
// 2026.09 Hermes 第五后端（任务 3213cdbf）：自定义大模型条目复用 SDshModelEntry。
#include "../core/CModelEntry.h"

// Hermes 后端 runner（第五后端，2026.09 任务 3213cdbf）。
//
// 基线形态（design §1 D1，澄清 1-3 未落定前的保守假设）：独立 CLI 子进程 +
// 现有网关（OpenAI/Anthropic 兼容）调用，进程模型同 claude/pi/codex（一阶段一进程）。
// StartStage 每次启动 `node <cli.js> --mode json -`（prompt 经 stdin），stdout 为
// NDJSON 事件流，逐行解析映射到基类信号；进程退出即结束。
//
// 事件流契约（桩可执行体驱动开发，真实格式落定后仅改 HandleLine）：
//   {"type":"session","sessionId":"..."}                       会话头（AC5 启动事件）
//   {"type":"assistant","text":"...","model":"...",
//    "usage":{"input":n,"cacheRead":n,"cacheWrite":n,"output":n}}
//   {"type":"tool","tool":"bash","input":"..."}                工具调用
//   {"type":"result","subtype":"success|error|error_max_turns","sessionId":"..."}
//
// 关键口径：
//   - StartStage 首步幂等自愈写 ~/.hermes/agent/config.json（读-比-短路 +
//     临时文件 rename 原子替换，多实例首写竞态下内容是 config 键的确定性纯函数，
//     rename 失败/被抢先时兜底删临时文件，最终内容仍一致，NFR4）。
//   - 钳位单点收口：kHermesMaxTokCap=131072（网关 2^17 上限，pi 先例）、
//     kHermesCtxCap=200000（当前工程实证安全值；真实窗口待澄清 6 后只改此常量）。
//   - 环境注入（FR8）：PYTHONUTF8=1 / PYTHONIOENCODING=utf-8 / LANG=zh_CN.UTF-8 /
//     LC_ALL=zh_CN.UTF-8 按四个独立变量无条件注入（不依赖继承值），另注入
//     HERMES_BASE_URL / HERMES_API_KEY / HERMES_MODEL（与 ~/.hermes 双保险）。
//     API key 绝不进命令行/日志。
//   - exe 形态（Python hermes-agent）禁注 HERMES_INFERENCE_MODEL（2026.09 v1.8.29
//     修，勿回退）：hermes -z 收到该变量会对模型名跑 detect_provider_for_model
//     目录侦测，网关模型名（如 glm-5.3）命中 OpenRouter z-ai/glm-5.3 → provider
//     被劫持成 openrouter（无 key）→ "No LLM provider configured" 退出 1。模型/
//     网关/密钥改经部署根 config.yaml model 段注入（EnsureHermesHomeConfig），
//     不带 env 模型时 oneshot 走 config 默认值路径不触发侦测。
//   - resume 不跨进程（同 Codex 基线，澄清 5 结论支持后再加）。
//   - claude 专属参数降级：a_iMaxTurns/a_iAutocompactK/a_bEnableAskUser/a_bFork/
//     a_sResumeSid/a_sDisallowedTools 忽略（非默认值时 sigInfo 提示一次）。
//     InjectUserTurn 安全 no-op 返回 false（一次性进程）。
class CHermesRunner : public CAgentRunnerBase {
    Q_OBJECT

public:
    explicit CHermesRunner(QObject* a_pParent = nullptr);
    ~CHermesRunner() override;

    // —— 进程级全局配置（config.ini [Agent] Hermes*；所有 Hermes runner 实例共享；
    //    纯内存不落盘，由 CMainWindow 启动/设置保存路径回填——口径同 Pi/Codex）——
    static void    SetHermesRoot(const QString& a_sRoot);
    static QString HermesRoot();
    // 2026.09.20 版本管理「最新版本」比对用：部署根下的 git 仓目录——exe 形态
    //   （venv）= venv 上级（hermes-agent 仓根，含 root\hermes-agent 与平铺两布局）；
    //   Node 形态 = cli.js 所在根；robocopy 拷贝部署（无 .git）→ 空串（无法比对）。
    static QString HermesGitRepoDir();
    static void    SetHermesNodePath(const QString& a_sPath);
    static QString HermesNodePath();
    static void    SetHermesModel(const QString& a_sModel);
    static QString HermesModel();
    static void    SetHermesApiKey(const QString& a_sKey);   // 明文存 config.ini（tooltip 告知）
    static QString HermesApiKey();
    static void    SetHermesBaseUrl(const QString& a_sUrl);
    static QString HermesBaseUrl();
    // 2026.09.21 激活模型条目的 API 协议（sApi：anthropic-messages / openai-
    //   completions）：Hermes custom provider 默认走 OpenAI chat/completions wire，
    //   选中 Anthropic 协议端点（如网关 /claude 路由）时需在 config.yaml model
    //   段写 api_mode: anthropic_messages，否则 OpenAI 客户端往 <base>/chat/
    //   completions 发请求 → 网关 404「Route /claude/chat/completions not found」
    //   → 重试 3 次退出 code 2（他机 GLM-5.3 实录）。
    static void    SetHermesApi(const QString& a_sApi);
    static QString HermesApi();
    // 激活模型条目的 contextWindow/maxTokens 回填（设置页/启动路径传入）；
    //   写 ~/.hermes 前经 qMin 钳位（kHermesMaxTokCap/kHermesCtxCap 单点收口）。
    static void    SetHermesRuntimeLimits(qint64 a_iCtx, int a_iMaxTok);
    static void    SetHermesPriceInPerM(double a_d);
    static void    SetHermesPriceCacheReadPerM(double a_d);
    static void    SetHermesPriceCacheWritePerM(double a_d);
    static void    SetHermesPriceOutPerM(double a_d);

    // StartStage 幂等自愈写 ~/.hermes/agent/config.json（model/baseUrl/apiKey/
    //   contextWindow/maxTokens，写前 qMin 钳位）。a_sHermesHome 空 = ~/.hermes
    //   （注入参数供单测离线验证，不写真实主目录）。
    static bool EnsureRuntimeConfig(const QString& a_sHermesHome = QString());
    // exe 形态部署根探测：venv/Scripts 上溯候选根中含 config.yaml 者（即
    //   hermes-agent 的 HERMES_HOME）；找不到/非 exe 形态返回空串。
    static QString ResolveHermesHomeForExe(const QString& a_sCli);
    // exe 形态幂等注入部署根 config.yaml 的 model 段（default=模型 /
    //   provider="custom" / base_url / api_key 四键 + api_mode（仅选中
    //   anthropic-messages 协议端点时写入，2026.09.21；hermes _API_MODE_ALIASES
    //   认 anthropic-messages→anthropic_messages，主运行时
    //   _resolve_plain_custom_api_mode 读 model_cfg["api_mode"] 切 Anthropic
    //   传输——<base>/v1/messages，同 Claude Code 链路）；读-比-短路 + 临时文件
    //   rename 原子替换；段内旧同名键行全部移除后在段首插一组——防 YAML 同键
    //   后位覆盖前位失效）。返回 false = 静态配置不全或写入失败（调用方提示
    //   不阻断启动）。a_sYamlPath 显式传入（单测离线/StartStage 共用）。
    static bool EnsureHermesHomeConfig(const QString& a_sYamlPath);
    // 2026.09.03 v2.0.105 软编码事件补丁敷设（参数=部署根 home）：oneshot.py 已含
    //   "AutoCode events patch v1" 标记→true；缺则同步调 exe 同级 install_env.bat
    //   /hermes-patch <home>（补丁正本=exe 同级 hermes_oneshot_patch.py，venv python
    //   幂等敷设；选 Hermes 后端保存时 CMainWindow 触发，此处 StartStage 兜底——
    //   hermes 更新冲掉后自动重敷；锚点失配上游改版→bat 报错返回 false 安全回退
    //   静默形态仍可用）。v2.0.104 内嵌补丁文本版已退役。
    static bool EnsureHermesOneshotPatch(const QString& a_sHome);
    // 离线单测注入：覆盖默认 ~/.hermes（传空串还原生产默认）。
    static void SetHermesHomeOverride(const QString& a_sDir);
    // 生成的运行时配置 JSON 内容（钳位后；EnsureRuntimeConfig 的纯函数面，单测用）。
    static QJsonObject BuildRuntimeConfigJson();
    // 钳位单点（供测试与设置页共用口径）：maxTok>131072 → 131072；ctx>200000 → 200000。
    static int ClampMaxTok(int a_iMaxTok) { return qMin(a_iMaxTok, 131072); }
    static qint64 ClampCtx(qint64 a_iCtx) { return qMin(a_iCtx, 200000); }
    // Hermes CLI 解析（探测序：HermesRoot → HERMES_HOME → exe 同级 HermesHarness
    //   → PATH；Node Harness dist\cli.js 与 Python 版 hermes.exe 双形态）。
    static QString ResolveHermesCli();
    // 目录是否像 Hermes 根（<dir>\dist\cli.js 或 <dir>\...\venv\Scripts\hermes.exe
    //   存在；设置页安装源校验用）。
    static bool LooksLikeHermesRoot(const QString& a_sDir);
    // 本地版本直读：Node 形态回退一级 package.json → version；exe 形态读 venv
    //   上级 pyproject.toml 的 version = "x.y.z"；读不出 → 空串（「未知」口径）。
    static QString ResolveHermesVersion();
    // node.exe 解析：显式键 → "node"（PATH）。
    static QString ResolveNodePath();
    static QString HermesConfigError();   // 空=可用；非空=中文指引

    QString BackendName() const override { return QStringLiteral("hermes"); }

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
    void CheckStall();

private:
    void Reset();
    void HandleLine(const QByteArray& a_sLine);   // NDJSON 事件分发
    void ApplyUsage(const QJsonObject& oUsage);
    double ComputeTurnCost() const;
    void EmitFailed(const QString& a_sMsg);

    QProcess*   m_pProc = nullptr;
    QByteArray  m_buf;                    // stdout 跨 readyRead 行缓冲
    bool        m_bFinished = false;      // 本轮已发 sigFinished（防重复）
    bool        m_bSawSession = false;    // 已收 session 头（启动事件）
    bool        m_bSawAssistant = false;  // 已收 assistant 文本（成功判定）
    // 2026.09.02 exe 形态（hermes -z）纯文本回复：-z 的 stdout 是回复正文本身
    //   （非 NDJSON，实测 v0.20.4：`hermes -z "只回复两个字：收到"` → stdout
    //   "收到"、stderr 空、exit 0）。逐行积累、进程退出时整段上抛为 assistant
    //   文本——否则回复被当系统噪音 sigInfo 透传成灰字且 m_bSawAssistant 恒假，
    //   误报「Hermes 进程退出（code 0）：本轮无 assistant 输出」（实测 14:47）。
    bool        m_bExeForm = false;       // 本轮为 exe 形态（StartStage 探测后置位）
    QStringList m_aPlainOut;              // -z 纯文本回复行缓冲（收尾整段上抛）
    // 2026.09.03 v2.0.105（方案 A）停滞心跳旁路：-z 干活期间 stdout 全静默，但
    //   会话状态库 <home>/state.db-wal（及 -shm）随 agent 活动增量落盘（实测
    //   每 10~17s 刷新）；CheckStall 巡检发现 wal mtime 前移即视为存活刷新心跳
    //   （真挂=wal 停走，阈值窗口照常生效）。纯长生成阶段（无工具/落盘）仍靠
    //   阈值 3 倍窗口兑底。
    QString     m_sExeHome;               // exe 形态部署根（wal 心跳探测用）
    QDateTime   m_tLastWalMod;            // 上次巡检看到的 wal/-shm 最后修改时间
    // 2026.09.04 v2.0.117 长 prompt 临时文件（@file 投递，见 StartStage）：
    //   StartStage 写入、进程退出（slotOnFinished）/Stop/Reset 后删除。
    QString     m_sPromptTmpFile;
    // 2026.09.03 v2.0.108 progress 事件提示节流（60s）：生成过程可见性（对齐
    //   pi 流式心跳提示口径），StartStage/Reset 清零。
    qint64      m_iLastProgHintMs = 0;
    QString     m_sLastEventText;         // result 事件附带文本（失败消息附带）
    qint64      m_iSumPlainInTok  = 0;    // 计费分量分开累计（三项输入单价不同）
    qint64      m_iSumCacheReadTok = 0;
    qint64      m_iSumCacheWriteTok = 0;
    qint64      m_iSumOutTok = 0;
    QStringList m_aStderrTail;            // stderr 尾部（失败消息附带）
    QElapsedTimer m_actClock;             // 距上次收到输出（stall 巡检）
    QTimer*     m_pStallTimer = nullptr;  // 停滞巡检定时器（30s 一拍）
};
