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

    // 监听 127.0.0.1:0（随机端口）；a_sTargetBase = 网关 base_url（仅 http）。
    // 已在监听时先关旧再重听（一阶段一起停）。失败返回 false。
    bool Start(const QUrl& a_sTargetBase, int a_iMaxTokens);

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

private:
    struct SConn {
        QTcpSocket* pClient = nullptr;
        QTcpSocket* pUp = nullptr;
        QByteArray buf;        // 请求侧未消费字节（头/体积积累）
        QByteArray reqHead;    // 已解析的请求头（含末尾空行）
        bool bHeadDone = false;
        bool bForwarded = false;   // 请求已发上游（其后上游→客户端纯管道）
    };

    void CleanupConn(SConn* a_pConn);
    void TryForward(SConn* a_pConn);
    void FailConn(SConn* a_pConn, const QString& a_sWhy);

    QTcpServer* m_pServer = nullptr;
    QList<SConn*> m_aConns;
    QUrl  m_urlTarget;
    int   m_iMaxTokens = 32768;
    quint16 m_uPort = 0;
    QByteArray m_aUpTail;   // 上游→客户端流的滚动尾巴（usage JSON 跨块拼接用）

    void ScanUsage(const QByteArray& a_pChunk);
};
