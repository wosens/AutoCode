#include "CCodexProxy.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QRegularExpression>

// —— 请求体重写（纯函数）——
QByteArray CCodexProxy::RewriteChatRequestBody(const QByteArray& a_sBody, int a_iMaxTokens) {
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(a_sBody, &stErr);
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        return QByteArray();   // 非 JSON 对象：不拦截（调用方原样转发）
    }
    QJsonObject oRoot = oDoc.object();
    if (!oRoot.contains(QStringLiteral("messages"))) {
        return QByteArray();   // 非 chat/completions 类载荷：不动
    }
    oRoot.insert(QStringLiteral("max_tokens"), a_iMaxTokens);
    // 流式响应要求网关补发真实 usage（curl 实测网关支持 stream_options.include_usage；
    // codex 不带该参数 → turn.completed 的 usage 恒为 0，任务统计无从谈起）。
    QJsonObject oStreamOpts = oRoot.value(QStringLiteral("stream_options")).toObject();
    oStreamOpts.insert(QStringLiteral("include_usage"), true);
    oRoot.insert(QStringLiteral("stream_options"), oStreamOpts);
    return QJsonDocument(oRoot).toJson(QJsonDocument::Compact);
}

CCodexProxy::CCodexProxy(QObject* a_pParent)
    : QObject(a_pParent) {
    m_pServer = new QTcpServer(this);
    m_pServer->setProxy(QNetworkProxy::NoProxy);   // 本地回环监听不走系统代理（同上）
    connect(m_pServer, &QTcpServer::newConnection,
            this, &CCodexProxy::slotNewConnection);
}

CCodexProxy::~CCodexProxy() {
    Shutdown();
}

bool CCodexProxy::Start(const QUrl& a_sTargetBase, int a_iMaxTokens) {
    Shutdown();
    m_urlTarget = a_sTargetBase;
    m_iMaxTokens = a_iMaxTokens;
    m_aUpTail.clear();
    if (!m_pServer->listen(QHostAddress::LocalHost, 0)) {
        return false;
    }
    m_uPort = m_pServer->serverPort();
    return true;
}

QString CCodexProxy::ProxyBaseUrl() const {
    QString sPath = m_urlTarget.path();
    while (sPath.endsWith(QLatin1Char('/'))) {
        sPath.chop(1);
    }
    return QStringLiteral("http://127.0.0.1:%1%2").arg(QString::number(m_uPort), sPath);
}

void CCodexProxy::Shutdown() {
    if (m_pServer != nullptr && m_pServer->isListening()) {
        m_pServer->close();
    }
    const QList<SConn*> aConns = m_aConns;
    m_aConns.clear();
    for (SConn* pConn : aConns) {
        CleanupConn(pConn);
    }
    m_uPort = 0;
}

void CCodexProxy::slotNewConnection() {
    while (m_pServer->hasPendingConnections()) {
        QTcpSocket* pClient = m_pServer->nextPendingConnection();
        auto* pConn = new SConn;
        pConn->pClient = pClient;
        pClient->setParent(nullptr);
        m_aConns.append(pConn);

        connect(pClient, &QTcpSocket::readyRead, this, [this, pConn]() {
            if (!m_aConns.contains(pConn)) {
                return;
            }
            pConn->buf += pConn->pClient->readAll();
            TryForward(pConn);
        });
        connect(pClient, &QTcpSocket::disconnected, this, [this, pConn]() {
            if (!m_aConns.contains(pConn)) {
                return;
            }
            m_aConns.removeOne(pConn);
            CleanupConn(pConn);
        });
    }
}

void CCodexProxy::TryForward(SConn* a_pConn) {
    if (a_pConn->bForwarded) {
        return;   // 请求已发上游，其后只管上游→客户端管道
    }
    if (!a_pConn->bHeadDone) {
        const int iHeadEnd = a_pConn->buf.indexOf("\r\n\r\n");
        if (iHeadEnd < 0) {
            return;
        }
        a_pConn->reqHead = a_pConn->buf.left(iHeadEnd + 4);
        a_pConn->buf = a_pConn->buf.mid(iHeadEnd + 4);
        a_pConn->bHeadDone = true;
    }
    // 解析请求行 + Content-Length（大小写不敏感）。
    const QList<QByteArray> aHeadLines = a_pConn->reqHead.split('\n');
    if (aHeadLines.isEmpty()) {
        FailConn(a_pConn, QStringLiteral("空请求头"));
        return;
    }
    const QByteArray sReqLine = aHeadLines.first().trimmed();
    const QList<QByteArray> aParts = sReqLine.split(' ');
    if (aParts.size() < 2) {
        FailConn(a_pConn, QStringLiteral("请求行非法"));
        return;
    }
    const bool bPost = aParts.at(0).compare("POST", Qt::CaseInsensitive) == 0;
    const QByteArray sPath = aParts.at(1);
    qint64 iBodyWant = 0;
    bool bHasBody = false;
    for (int i = 1; i < aHeadLines.size(); ++i) {
        const QByteArray sLine = aHeadLines.at(i).trimmed();
        const int iColon = sLine.indexOf(':');
        if (iColon < 0) {
            continue;
        }
        if (sLine.left(iColon).compare("Content-Length", Qt::CaseInsensitive) == 0) {
            iBodyWant = sLine.mid(iColon + 1).trimmed().toLongLong();
            bHasBody = true;
        }
    }
    bool bChatCall = bPost && sPath.contains("chat/completions");
    if (!bHasBody || iBodyWant <= 0) {
        bChatCall = false;   // 无请求体（GET /models 等）：盲转发
    }
    if (bChatCall && a_pConn->buf.size() < iBodyWant) {
        return;   // 等请求体收全（Content-Length 口径；codex 不用 chunked 请求）
    }
    // —— 建上游连接 + 转发 ——
    QTcpSocket* pUp = new QTcpSocket(this);
    // 2026.08.21 直连网关：显式禁用代理——否则 QTcpSocket 会遵从 Windows 系统代理
    //   （如 Clash 的 127.0.0.1:7897），经 HTTP 代理隧道连内网网关报
    //   「The proxy type is invalid for this operation」→ 502（实测：curl 直连正常、
    //   程序内 100% 失败，即用户任务「502 Bad Gateway」的根因）。
    pUp->setProxy(QNetworkProxy::NoProxy);
    a_pConn->pUp = pUp;
    const QString sHost = m_urlTarget.host();
    const quint16 uPort = m_urlTarget.port(80);
    connect(pUp, &QTcpSocket::connected, this, [this, a_pConn, pUp, bChatCall, iBodyWant]() {
        if (!m_aConns.contains(a_pConn)) {
            return;
        }
        QByteArray sOut;
        if (bChatCall) {
            QByteArray sBody = a_pConn->buf.left(int(iBodyWant));
            const QByteArray sRewritten = RewriteChatRequestBody(sBody, m_iMaxTokens);
            if (!sRewritten.isEmpty()) {
                sBody = sRewritten;
            }
            a_pConn->buf.clear();
            // 重排头：Content-Length 换新值，Host 换目标主机，其余逐行透传。
            const QList<QByteArray> aHeadLines = a_pConn->reqHead.split('\n');
            QByteArray sNewHead;
            for (const QByteArray& sRaw : aHeadLines) {
                const QByteArray sTrim = sRaw.trimmed();   // 去行尾 \r/\n 统一重组
                if (sTrim.isEmpty()) {
                    continue;   // 头块末尾空行（重组统一补一个）
                }
                const int iColon = sTrim.indexOf(':');
                const bool bLen = iColon > 0
                    && sTrim.left(iColon).compare("Content-Length", Qt::CaseInsensitive) == 0;
                const bool bHost = iColon > 0
                    && sTrim.left(iColon).compare("Host", Qt::CaseInsensitive) == 0;
                if (bLen) {
                    sNewHead += "Content-Length: " + QByteArray::number(sBody.size()) + "\r\n";
                } else if (bHost) {
                    const QByteArray sHostHdr = m_urlTarget.port(80) == 80
                        ? m_urlTarget.host().toUtf8()
                        : m_urlTarget.host().toUtf8() + ':' + QByteArray::number(m_urlTarget.port(80));
                    sNewHead += "Host: " + sHostHdr + "\r\n";
                } else {
                    sNewHead += sTrim + "\r\n";
                }
            }
            sNewHead += "\r\n";   // 头/体分隔空行
            sOut = sNewHead + sBody;
        } else {
            sOut = a_pConn->reqHead + a_pConn->buf;
            a_pConn->buf.clear();
        }
        pUp->write(sOut);
        a_pConn->bForwarded = true;
    });
    connect(pUp, &QTcpSocket::readyRead, this, [this, a_pConn, pUp]() {
        if (!m_aConns.contains(a_pConn) || a_pConn->pClient == nullptr) {
            return;
        }
        const QByteArray aChunk = pUp->readAll();
        a_pConn->pClient->write(aChunk);   // 响应（含 SSE 流）原样回客户端
        ScanUsage(aChunk);                 // 顺带提取真实 usage（任务统计）
    });
    connect(pUp, &QTcpSocket::disconnected, this, [this, a_pConn]() {
        if (!m_aConns.contains(a_pConn)) {
            return;
        }
        // 上游关连接（网关 connection: close）：回写收尾后关客户端。
        if (a_pConn->pClient != nullptr) {
            a_pConn->pClient->flush();
            a_pConn->pClient->disconnectFromHost();
        }
    });
    connect(pUp, &QTcpSocket::errorOccurred, this,
            [this, a_pConn](QTcpSocket::SocketError) {
        if (!m_aConns.contains(a_pConn)) {
            return;
        }
        FailConn(a_pConn, QStringLiteral("上游连接失败"));
    });
    pUp->connectToHost(sHost, uPort);
}

void CCodexProxy::FailConn(SConn* a_pConn, const QString& a_sWhy) {
    // 先移出连接表再动 socket：disconnectFromHost()/abort() 可能**同步**发出
    // disconnected，触发 cli-disc 处理器先行清理（表内仍在→重复清理→UAF，
    // 2026.08.21 实测崩溃根因）。
    m_aConns.removeOne(a_pConn);
    if (a_pConn->pClient != nullptr) {
        a_pConn->pClient->write(
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        a_pConn->pClient->flush();
    }
    CleanupConn(a_pConn);
    Q_UNUSED(a_sWhy);
}

void CCodexProxy::ScanUsage(const QByteArray& a_pChunk) {
    // 拼滚动尾巴（usage JSON 可能被 TCP 分块切开），在尾部窗口内找 "usage" 对象：
    //   {"prompt_tokens":N,...,"completion_tokens":M,...}（SSE 末块 / 非流式 JSON 通用）。
    // 每找到一处 = 一次 chat/completions 的真实计量 → sigUsage 上抛（runner 累计）。
    m_aUpTail.append(a_pChunk);
    if (m_aUpTail.size() > 8192) {
        m_aUpTail = m_aUpTail.right(8192);
    }
    int iPos = 0;
    while (true) {
        const int iUsage = m_aUpTail.indexOf("\"usage\"", iPos);
        if (iUsage < 0) {
            break;
        }
        const int iObjEnd = m_aUpTail.indexOf('}', iUsage);
        if (iObjEnd < 0) {
            break;   // 对象未收全，留在尾巴等下一块
        }
        const QByteArray sObj = m_aUpTail.mid(iUsage, iObjEnd - iUsage);
        bool bIn = false, bOut = false;
        qint64 iIn = 0, iOut = 0;
        const QRegularExpression reIn(QStringLiteral("\"prompt_tokens\"\\s*:\\s*(\\d+)"));
        const QRegularExpression reOut(QStringLiteral("\"completion_tokens\"\\s*:\\s*(\\d+)"));
        const auto mIn = reIn.match(QString::fromLatin1(sObj));
        const auto mOut = reOut.match(QString::fromLatin1(sObj));
        if (mIn.hasMatch()) {
            iIn = mIn.captured(1).toLongLong();
            bIn = true;
        }
        if (mOut.hasMatch()) {
            iOut = mOut.captured(1).toLongLong();
            bOut = true;
        }
        if (bIn && bOut) {
            emit sigUsage(iIn, iOut);
        }
        iPos = iObjEnd + 1;
    }
    // 已消费部分出尾巴（保留最后 512 字节防跨块截断重复/漏配）。
    if (iPos > 512) {
        m_aUpTail = m_aUpTail.right(512);
    }
}

void CCodexProxy::CleanupConn(SConn* a_pConn) {
    // 先 disconnect() 再 abort()：abort 会**同步**发出 errorOccurred/disconnected
    // 信号，若不断开本类槽（lambda 捕获的 a_pConn），会嵌套重入 CleanupConn
    // 对同一连接双重释放（2026.08.21 实测崩溃根因）。
    if (a_pConn->pUp != nullptr) {
        QTcpSocket* const pUp = a_pConn->pUp;
        a_pConn->pUp = nullptr;
        pUp->disconnect();
        pUp->abort();
        pUp->deleteLater();
    }
    if (a_pConn->pClient != nullptr) {
        QTcpSocket* const pClient = a_pConn->pClient;
        a_pConn->pClient = nullptr;
        pClient->disconnect();
        pClient->abort();
        pClient->deleteLater();
    }
    delete a_pConn;
}
