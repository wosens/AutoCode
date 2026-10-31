#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

// CodexHarness 本地 HTTP 代理（第四后端，2026.08.21）。
//
// 背景（0.87.0 实测，见任务 memory）：codex chat wire_api 的请求体**从不携带
// max_tokens**，网关（one-api/new-api 类）默认把单次输出掐在 4096 token——
// 单轮长输出（写大文件的工具调用参数）被截断后 finish_reason=length，codex
// 将其误报为「ran out of room in the model's context window」直接终止。
//
// 修复：StartStage 起本代理（127.0.0.1 随机端口），命令行以
// `-c model_providers.<p>.base_url=http://127.0.0.1:<port>/<原路径>` 覆盖路由，
// 代理把 POST */chat/completions 请求体注入 max_tokens=<条目「最大输出」>
// 后原样转发网关，其余请求/响应字节流盲转发（含 SSE 流式响应）。
//
// 2026-10-31 响应侧 SSE workdir 消毒（OpenCompare 实害「os error 267 目录名称
//   无效」）：glm 系模型在 shell 工具调用里高频自填 workdir 字段且偶发凭记忆
//   幻觉拼写（实测 E:\wosens → E:\wososens），codex 按该不存在目录 spawn
//   powershell → CreateProcess 267 → 单条命令报 execution error，重则污染
//   stderr 尾部误导整轮失败归因。消毒策略：chunked+SSE 响应进入重写模式，
//   扣留 tool_calls 的 arguments 增量（id/name 行即时透传），在该 choice 的
//   finish_reason / [DONE] / 终止帧处冲刷——组装后的参数若含「不存在的
//   workdir」则整键剔除（codex 回退用会话 cwd），其余字节级透传；非 chunked
//   或非 SSE 响应保持盲转发。
//
// 约束：仅支持 http 目标（QTcpSocket 明文管道；https 不启用代理，保持原路由）。
class CCodexProxy : public QObject {
    Q_OBJECT

public:
    explicit CCodexProxy(QObject* a_pParent = nullptr);
    ~CCodexProxy() override;

    // 请求体重写（纯函数，单测可离线验证）：JSON 对象且含 "messages" 时
    // 置 max_tokens=a_iMaxTokens（已有值同样覆盖——codex 本就不发，防御性覆盖），
    // 并置 stream_options.include_usage=true（网关流式响应补发真实 usage 计量，
    // 供任务统计；2026.08.21）。解析失败返回空 QByteArray（调用方原样转发，不拦截）。
    static QByteArray RewriteChatRequestBody(const QByteArray& a_sBody, int a_iMaxTokens);

    // 2026-10-31 上下文估算（OpenCompare 实害：大会话流尾被网关掐断，usage 块
    //   永远到不了代理 → 上下文条全程瞎，实际 95 万 token 时条上只显 148）。
    //   代理本来就握有完整请求体（每次请求=全部历史+工具+指令，正是网关要
    //   tokenize 的 prompt），按 字节数/4 估算 prompt token（实测校准：EN 约
    //   4字节/token、CJK UTF-8 约 3~4.5字节/token，两者按字节口径都≈/4；
    //   OpenCompare 致死会话 3.24MB 正文估 81 万 vs codex 自计 95 万，偏差
    //   ~15% 含 reasoning/工具定义未计入——进度条口径足够）。网关 usage 正常
    //   到达时真实值后到覆盖估算（请求时估、响应尾真）。
    static qint64 EstimatePromptTokens(const QByteArray& a_sBody);

    // 2026-10-31 组装后的工具参数消毒（纯函数，单测可离线验证）：参数为 JSON
    //   对象且含 workdir 键时校验目录——绝对路径须存在；相对路径以 a_sBaseDir
    //   为基准（基准空则放行）。不存在 → 整键剔除（codex 回退会话 cwd），
    //   *a_pBadDir 回带被剔除的幻觉目录（仅剔除时）。非 JSON 对象 / 无 workdir /
    //   合法 → 原样返回。
    static QString SanitizeToolArguments(const QString& a_sArgs, const QString& a_sBaseDir,
                                         QString* a_pBadDir);

    // 2026-10-31 冲刷扣留参数用的合成 SSE 块：data: {"choices":[{"index":iChoice,
    //   "delta":{"tool_calls":[{"index":iTc,"function":{"arguments":sArgs}}]}}]}
    //   （id/model/created 有值时附带，与网关原块对齐）。
    static QByteArray BuildArgsDeltaChunk(int a_iChoice, int a_iTc, const QString& a_sId,
                                          const QString& a_sModel, qint64 a_llCreated,
                                          const QString& a_sArgs);

    // 监听 127.0.0.1:0（随机端口）；a_sTargetBase = 网关 base_url（仅 http）。
    // a_sWorkDir=本阶段会话工作目录（2026-10-31 相对 workdir 校验基准；空=放行）。
    // 已在监听时先关旧再重听（一阶段一起停）。失败返回 false。
    bool Start(const QUrl& a_sTargetBase, int a_iMaxTokens,
               const QString& a_sWorkDir = QString());

    quint16 Port() const { return m_uPort; }

    // codex 侧覆盖用的 base_url：http://127.0.0.1:<port>/<目标 path>（去尾部 '/'）。
    QString ProxyBaseUrl() const;

    // 关服务 + 断开全部连接（StartStage 收尾/Stop 调用；对象可复用再 Start）。
    void Shutdown();

private slots:
    void slotNewConnection();

signals:
    // 上游响应中解析到一次 usage（一次 chat/completions 请求的真实计量）：
    // 供 runner 累计输入/输出 token 与请求数（2026.08.21 任务统计显示）。
    void sigUsage(qint64 a_iInTok, qint64 a_iOutTok);
    // 2026-10-31 转发 chat/completions 请求时按请求体估算的 prompt token
    //   （上下文条数据源；网关掐断 usage 尾块时的可靠兑底，OpenCompare 实害）。
    //   每次请求一发；网关 usage 正常到达时真实值后到覆盖（请求时估、响应尾真）。
    void sigPromptEstimate(qint64 a_iTokens);
    // 2026-10-31 消毒剔除了一处幻觉 workdir（参数=被剔除的目录；每次剔除一发，
    //   runner 转 sigInfo 留痕，供用户知道有拦截发生）。
    void sigWorkdirGuard(const QString& a_sBadDir);

private:
    struct SHeldArgs {   // 2026-10-31 扣留中的 tool_call arguments 增量（按 choice/tc 索引归并）
        int     iChoice = 0;
        int     iTc = 0;
        QString sId;
        QString sModel;
        qint64  llCreated = 0;
        QString sArgs;
    };
    struct SConn {
        QTcpSocket* pClient = nullptr;
        QTcpSocket* pUp = nullptr;
        QByteArray buf;        // 请求侧未消费字节（头/体积积累）
        QByteArray reqHead;    // 已解析的请求头（含末尾空行）
        bool bHeadDone = false;
        bool bForwarded = false;   // 请求已发上游（其后上游→客户端纯管道）
        // —— 2026-10-31 响应侧状态（workdir 消毒）——
        int  eResp = 0;             // 0=头未收全 1=盲转发 2=chunked+SSE 重写
        QByteArray aRespHead;       // 头阶段积累（收全后判定模式并清空）
        QByteArray aUpIn;           // 重写模式：未解码的响应体字节（跨读保留）
        qint64 llChunkRemain = -1;  // chunked：当前帧剩余（-1=待尺寸行，-3=待帧尾 CRLF）
        QByteArray aSseLine;        // SSE 半行残余（跨帧拼接）
        QList<SHeldArgs> aHeld;     // 扣留的 arguments（finish/[DONE]/终止帧冲刷）
    };

    void CleanupConn(SConn* a_pConn);
    void TryForward(SConn* a_pConn);
    void FailConn(SConn* a_pConn, const QString& a_sWhy);
    // 2026-10-31 上游→客户端响应处理（头判定 + chunked 解码 + SSE 行消毒 + 重编码），
    //   返回应回写客户端的字节（含重排后的 chunked 帧）。
    QByteArray ProcessUpstream(SConn* a_pConn, const QByteArray& a_pChunk);
    // 单条 SSE 行处理（data 载荷含 tool_calls.arguments 时扣留/重组，其余透传）。
    QByteArray ProcessSseLine(SConn* a_pConn, const QByteArray& a_sLine);
    // 冲刷全部扣留参数（消毒 + 合成块 + chunked 包装）。
    QByteArray FlushHeld(SConn* a_pConn);

    QTcpServer* m_pServer = nullptr;
    QList<SConn*> m_aConns;
    QUrl  m_urlTarget;
    int   m_iMaxTokens = 32768;
    QString m_sWorkDir;      // 2026-10-31 会话工作目录（相对 workdir 校验基准）
    quint16 m_uPort = 0;
    QByteArray m_aUpTail;   // 上游→客户端流的滚动尾巴（usage JSON 跨块拼接用）

    void ScanUsage(const QByteArray& a_pChunk);
};
