#include "CCodexProxy.h"

#include <QDir>
#include <QJsonArray>
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

qint64 CCodexProxy::EstimatePromptTokens(const QByteArray& a_sBody) {
    // 2026-10-31 上下文估算（口径标定见头文件注释）：请求体字节/4。非 JSON
    //   /非 chat 载荷同样按字节估（调用方仅在 chat 转发路径使用）。
    return a_sBody.size() / 4;
}

// —— 2026-10-31 响应侧 SSE workdir 消毒（OpenCompare 实害 os error 267）——

QString CCodexProxy::SanitizeToolArguments(const QString& a_sArgs, const QString& a_sBaseDir,
                                           QString* a_pBadDir) {
    if (a_pBadDir != nullptr) {
        a_pBadDir->clear();
    }
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(a_sArgs.toUtf8(), &stErr);
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        return a_sArgs;   // 非 JSON 对象（工具参数非惯例形态）：原样
    }
    QJsonObject o = oDoc.object();
    const QJsonValue v = o.value(QStringLiteral("workdir"));
    if (!v.isString()) {
        return a_sArgs;   // 无 workdir / 非字符串：无需消毒
    }
    QString sDir = v.toString().trimmed();
    if (sDir.isEmpty()) {
        return a_sArgs;
    }
    // 绝对路径直接验存在；相对路径以会话目录为基准（基准未知时放行，不误杀）。
    if (!QDir::isAbsolutePath(sDir) && a_sBaseDir.isEmpty()) {
        return a_sArgs;
    }
    const QString sAbs = QDir::isAbsolutePath(sDir)
                         ? sDir : QDir(a_sBaseDir).filePath(sDir);
    if (QDir(sAbs).exists()) {
        return a_sArgs;   // 目录存在（含模型正常切子目录）：放行
    }
    if (a_pBadDir != nullptr) {
        *a_pBadDir = sDir;
    }
    o.remove(QStringLiteral("workdir"));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QByteArray CCodexProxy::BuildArgsDeltaChunk(int a_iChoice, int a_iTc, const QString& a_sId,
                                             const QString& a_sModel, qint64 a_llCreated,
                                             const QString& a_sArgs) {
    QJsonObject oFn;
    oFn.insert(QStringLiteral("arguments"), a_sArgs);
    QJsonObject oTc;
    oTc.insert(QStringLiteral("index"), a_iTc);
    oTc.insert(QStringLiteral("function"), oFn);
    QJsonObject oDelta;
    oDelta.insert(QStringLiteral("tool_calls"), QJsonArray{ oTc });
    QJsonObject oChoice;
    oChoice.insert(QStringLiteral("index"), a_iChoice);
    oChoice.insert(QStringLiteral("delta"), oDelta);
    QJsonObject oRoot;
    if (!a_sId.isEmpty()) {
        oRoot.insert(QStringLiteral("id"), a_sId);
    }
    if (!a_sModel.isEmpty()) {
        oRoot.insert(QStringLiteral("model"), a_sModel);
    }
    if (a_llCreated > 0) {
        oRoot.insert(QStringLiteral("created"), double(a_llCreated));
    }
    oRoot.insert(QStringLiteral("object"), QStringLiteral("chat.completion.chunk"));
    oRoot.insert(QStringLiteral("choices"), QJsonArray{ oChoice });
    return QStringLiteral("data: %1\n\n")
               .arg(QString::fromUtf8(QJsonDocument(oRoot).toJson(QJsonDocument::Compact)))
               .toUtf8();
}

// chunked 帧包装（消毒路径重编码用；帧内容非空才发）。
static QByteArray WrapChunked(const QByteArray& a_sData) {
    if (a_sData.isEmpty()) {
        return QByteArray();
    }
    return QByteArray::number(a_sData.size(), 16) + "\r\n" + a_sData + "\r\n";
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

bool CCodexProxy::Start(const QUrl& a_sTargetBase, int a_iMaxTokens,
                        const QString& a_sWorkDir) {
    Shutdown();
    m_urlTarget = a_sTargetBase;
    m_iMaxTokens = a_iMaxTokens;
    m_sWorkDir = a_sWorkDir;
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
    // 等请求体收全（Content-Length 口径；codex 不用 chunked 请求）——对所有带体请求
    //   生效（旧版仅限 chat：其它 POST 在 body 未收全时即转发，connected 里 buf.clear()
    //   后到达的剩余请求体被 TryForward 早退丢弃，分段传输的大请求体被截断）。
    if (bHasBody && iBodyWant > 0 && a_pConn->buf.size() < iBodyWant) {
        return;
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
            // 2026-10-31 请求时估算 prompt token（上下文条兑底数据源）：网关对
            //   大会话流常在尾部 usage 块前掐断连接（OpenCompare 实害：193 次
            //   请求零 usage、实际 95 万 token 时条上只显 148）——估算仅作用于
            //   上下文显示/闸门，费用统计仍只认真实 usage（sigUsage），不双计。
            emit sigPromptEstimate(EstimatePromptTokens(sBody));
        } else {
            // 盲转发：Host 同样重写为上游目标（原值指向 127.0.0.1:代理端口，
            //   按 Host 做虚拟主机路由的网关会误路由/404），其余头逐行透传。
            const QList<QByteArray> aHeadLines = a_pConn->reqHead.split('\n');
            QByteArray sNewHead;
            for (const QByteArray& sRaw : aHeadLines) {
                const QByteArray sTrim = sRaw.trimmed();
                if (sTrim.isEmpty()) {
                    continue;
                }
                const int iColon = sTrim.indexOf(':');
                const bool bHost = iColon > 0
                    && sTrim.left(iColon).compare("Host", Qt::CaseInsensitive) == 0;
                if (bHost) {
                    const QByteArray sHostHdr = m_urlTarget.port(80) == 80
                        ? m_urlTarget.host().toUtf8()
                        : m_urlTarget.host().toUtf8() + ':' + QByteArray::number(m_urlTarget.port(80));
                    sNewHead += "Host: " + sHostHdr + "\r\n";
                } else {
                    sNewHead += sTrim + "\r\n";
                }
            }
            sNewHead += "\r\n";   // 头/体分隔空行
            sOut = sNewHead + a_pConn->buf;
            a_pConn->buf.clear();
        }
        pUp->write(sOut);
        a_pConn->bForwarded = true;
    });
    connect(pUp, &QTcpSocket::readyRead, this, [this, a_pConn, pUp]() {
        if (!m_aConns.contains(a_pConn) || a_pConn->pClient == nullptr) {
            return;
        }
        // 2026-10-31 chunked+SSE 响应进入消毒重写路径（workdir 幻觉拦截），
        //   其余（头未收全/非 chunked/非 SSE）盲转发字节级透传。
        const QByteArray aChunk = pUp->readAll();
        const QByteArray aOut = ProcessUpstream(a_pConn, aChunk);
        if (!aOut.isEmpty()) {
            a_pConn->bRespSent = true;   // 已回写过响应字节：FailConn 不再补 502
            a_pConn->pClient->write(aOut);
        }
    });
    connect(pUp, &QTcpSocket::disconnected, this, [this, a_pConn]() {
        if (!m_aConns.contains(a_pConn)) {
            return;
        }
        // 上游关连接（网关 connection: close）：重写模式下先冲刷扣留的
        //   tool_call 参数并补终止帧（codex 能拿到完整参数执行），再回写收尾。
        if (a_pConn->pClient != nullptr && a_pConn->eResp == 2) {
            a_pConn->pClient->write(FlushHeld(a_pConn) + "0\r\n\r\n");
        }
        if (a_pConn->pClient != nullptr) {
            a_pConn->pClient->flush();
            a_pConn->pClient->disconnectFromHost();
        }
    });
    connect(pUp, &QTcpSocket::errorOccurred, this,
            [this, a_pConn](QTcpSocket::SocketError a_eError) {
        if (!m_aConns.contains(a_pConn)) {
            return;
        }
        // 2026-10-31 网关正常关连接（Connection: close）在部分时序下会先报
        //   RemoteHostClosedError——若走 FailConn 会往已完整回写的响应尾部追加
        //   一段垃圾 502 响应，污染 codex 的 HTTP 流（疑似进程退出码 -1 成因）。
        //   远端关闭交由 disconnected 处理器（冲刷扣留 + 收尾）。
        if (a_eError == QTcpSocket::RemoteHostClosedError) {
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
        // 已回写过部分响应（上游中途 ConnectionResetError 等）时直接断链：
        //   此时再拼一段完整 502 到半截响应尾部会污染 codex 的 HTTP 流
        //   （同 RemoteHostClosedError 已修场景的姊妹形态）。
        if (!a_pConn->bRespSent) {
            a_pConn->pClient->write(
                "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            a_pConn->pClient->flush();
        }
    }
    CleanupConn(a_pConn);
    Q_UNUSED(a_sWhy);
}

QByteArray CCodexProxy::FlushHeld(SConn* a_pConn) {
    // 冲刷全部扣留的 tool_call arguments：逐个消毒（幻觉 workdir 剔除 + 留痕）
    //   后合成完整 arguments 块（codex 按 index 归并到已收到的 id/name 上）。
    //   先取副本再清空：sigWorkdirGuard 同步进入 runner/UI，防重入改表。  
    if (a_pConn->aHeld.isEmpty()) {
        return QByteArray();
    }
    const QList<SHeldArgs> aHeld = a_pConn->aHeld;
    a_pConn->aHeld.clear();
    QByteArray aOut;
    for (const SHeldArgs& st : aHeld) {
        QString sBad;
        const QString sArgs = SanitizeToolArguments(st.sArgs, m_sWorkDir, &sBad);
        if (!sBad.isEmpty()) {
            emit sigWorkdirGuard(sBad);
        }
        aOut += WrapChunked(BuildArgsDeltaChunk(st.iChoice, st.iTc, st.sId, st.sModel,
                                                st.llCreated, sArgs));
    }
    return aOut;
}

QByteArray CCodexProxy::ProcessSseLine(SConn* a_pConn, const QByteArray& a_sLine) {
    const QByteArray sTrim = a_sLine.trimmed();
    if (!sTrim.startsWith("data:")) {
        return a_sLine;   // 空行 / 注释 / event: 等透传
    }
    QByteArray sPayload = sTrim.mid(5);
    if (sPayload.startsWith(' ')) {
        sPayload.remove(0, 1);
    }
    const bool bDone = sPayload == "[DONE]";
    // finish 块（非 null）意味着本 choice 流收尾：先冲刷扣留参数再放行，保序。
    const bool bFinish = !bDone && sPayload.contains("\"finish_reason\"")
                         && !sPayload.contains("\"finish_reason\":null");
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(sPayload, &stErr);
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        return (bDone ? FlushHeld(a_pConn) : QByteArray()) + a_sLine;
    }
    QJsonObject oRoot = oDoc.object();
    const QJsonValue vChoices = oRoot.value(QStringLiteral("choices"));
    if (!vChoices.isArray()) {
        return (bDone || bFinish ? FlushHeld(a_pConn) : QByteArray()) + a_sLine;
    }
    QJsonArray aChoices = vChoices.toArray();
    bool bModified = false;
    for (int iC = 0; iC < aChoices.size(); ++iC) {
        QJsonObject oChoice = aChoices.at(iC).toObject();
        QJsonObject oDelta = oChoice.value(QStringLiteral("delta")).toObject();
        const QJsonValue vTcs = oDelta.value(QStringLiteral("tool_calls"));
        if (!vTcs.isArray()) {
            continue;
        }
        QJsonArray aTcsNew;
        bool bTcMod = false;
        for (int iT = 0; iT < vTcs.toArray().size(); ++iT) {
            QJsonObject oTc = vTcs.toArray().at(iT).toObject();
            QJsonObject oFn = oTc.value(QStringLiteral("function")).toObject();
            if (!oFn.contains(QStringLiteral("arguments"))) {
                aTcsNew.append(oTc);   // 无 arguments 增量（id/name 行）：原样
                continue;
            }
            // 扣留 arguments 增量（按 choice+tc 索引归并；首个块顺带记录 id/model）。
            const int iTcIdx = oTc.value(QStringLiteral("index")).toInt(iT);
            SHeldArgs* pHeld = nullptr;
            for (SHeldArgs& st : a_pConn->aHeld) {
                if (st.iChoice == iC && st.iTc == iTcIdx) {
                    pHeld = &st;
                    break;
                }
            }
            if (pHeld == nullptr) {
                SHeldArgs stNew;
                stNew.iChoice = iC;
                stNew.iTc = iTcIdx;
                stNew.sId = oRoot.value(QStringLiteral("id")).toString();
                stNew.sModel = oRoot.value(QStringLiteral("model")).toString();
                stNew.llCreated = qint64(oRoot.value(QStringLiteral("created")).toDouble());
                a_pConn->aHeld.append(stNew);
                pHeld = &a_pConn->aHeld.last();
            }
            pHeld->sArgs += oFn.value(QStringLiteral("arguments")).toString();
            // 重建本 tc 条目（去 arguments，保留 id/type/name 供 codex 归位）；
            //   剥后字段全空则整条丢弃（纯参数增量块不透传，冲刷时统一回填）。
            oFn.remove(QStringLiteral("arguments"));
            if (!oFn.isEmpty()) {
                oTc.insert(QStringLiteral("function"), oFn);
            } else {
                oTc.remove(QStringLiteral("function"));
            }
            if (!oTc.isEmpty()) {
                aTcsNew.append(oTc);
            }
            bTcMod = true;
        }
        if (!bTcMod) {
            continue;
        }
        if (aTcsNew.isEmpty()) {
            oDelta.remove(QStringLiteral("tool_calls"));
        } else {
            oDelta.insert(QStringLiteral("tool_calls"), aTcsNew);
        }
        oChoice.insert(QStringLiteral("delta"), oDelta);
        aChoices.replace(iC, oChoice);
        bModified = true;
    }
    QByteArray aFlush;
    if (bDone || bFinish) {
        aFlush = FlushHeld(a_pConn);
    }
    if (!bModified) {
        return aFlush + a_sLine;   // 与工具参数无关：字节级透传
    }
    oRoot.insert(QStringLiteral("choices"), aChoices);
    return aFlush + "data: "
           + QJsonDocument(oRoot).toJson(QJsonDocument::Compact) + "\n";
}

QByteArray CCodexProxy::ProcessUpstream(SConn* a_pConn, const QByteArray& a_pChunk) {
    if (a_pConn->eResp == 1) {
        ScanUsage(a_pConn, a_pChunk);
        return a_pChunk;   // 盲转发（非 chunked / 非 SSE / 帧异常降级）
    }
    QByteArray aOut;
    if (a_pConn->eResp == 0) {
        // —— 头阶段：收全 \r\n\r\n 后判定响应模式 ——
        a_pConn->aRespHead += a_pChunk;
        const int iEnd = a_pConn->aRespHead.indexOf("\r\n\r\n");
        if (iEnd < 0) {
            return QByteArray();
        }
        const QByteArray sHead = a_pConn->aRespHead.left(iEnd + 4);
        a_pConn->aUpIn = a_pConn->aRespHead.mid(iEnd + 4);
        a_pConn->aRespHead.clear();
        const QByteArray sHeadLow = sHead.toLower();
        const bool bChunked = sHeadLow.contains("transfer-encoding")
                              && sHeadLow.contains("chunked");
        const bool bSse = sHeadLow.contains("content-type")
                          && sHeadLow.contains("text/event-stream");
        aOut += sHead;
        a_pConn->eResp = (bChunked && bSse) ? 2 : 1;
        if (a_pConn->eResp == 1) {
            ScanUsage(a_pConn, a_pConn->aUpIn);
            aOut += a_pConn->aUpIn;
            a_pConn->aUpIn.clear();
            return aOut;
        }
    } else {
        a_pConn->aUpIn += a_pChunk;
    }
    // —— 重写模式：chunked 解码 → SSE 行消毒 → 重编码 ——
    QByteArray aBody;
    while (true) {
        if (a_pConn->llChunkRemain == -1) {
            const int iCrlf = a_pConn->aUpIn.indexOf("\r\n");
            if (iCrlf < 0) {
                break;   // 尺寸行未收全（跨读保留）
            }
            const QByteArray sSize = a_pConn->aUpIn.left(iCrlf).split(';').first().trimmed();
            a_pConn->aUpIn.remove(0, iCrlf + 2);
            bool bOk = false;
            const qint64 llSize = sSize.toLongLong(&bOk, 16);
            if (!bOk) {
                // 帧损坏（不应发生）：先补发已积累 body 再降级盲转发保连通
                a_pConn->eResp = 1;
                ScanUsage(a_pConn, a_pConn->aUpIn);
                aOut += WrapChunked(aBody) + a_pConn->aUpIn;
                a_pConn->aUpIn.clear();
                return aOut;
            }
            if (llSize == 0) {
                // 终止帧：先补发已消毒重编码的 body（终端路径从循环内 return，
                //   不得绕过尾部 WrapChunked）→ 冲刷扣留 → 回写 0 帧 + trailer
                //   原样 → 后续盲转发。
                aOut += WrapChunked(aBody) + FlushHeld(a_pConn)
                        + "0\r\n" + a_pConn->aUpIn;
                a_pConn->aUpIn.clear();
                a_pConn->eResp = 1;
                return aOut;
            }
            a_pConn->llChunkRemain = llSize;
        } else if (a_pConn->llChunkRemain == -3) {
            if (a_pConn->aUpIn.size() < 2) {
                break;   // 帧尾 CRLF 未到齐
            }
            a_pConn->aUpIn.remove(0, 2);
            a_pConn->llChunkRemain = -1;
            continue;
        }
        const qint64 llTake = qMin<qint64>(a_pConn->llChunkRemain,
                                           qint64(a_pConn->aUpIn.size()));
        if (llTake <= 0) {
            break;
        }
        const QByteArray aData = a_pConn->aUpIn.left(int(llTake));
        a_pConn->aUpIn.remove(0, int(llTake));
        a_pConn->llChunkRemain -= llTake;
        if (a_pConn->llChunkRemain == 0) {
            a_pConn->llChunkRemain = -3;   // 待帧尾 CRLF
        }
        ScanUsage(a_pConn, aData);   // 解码后的 SSE 字节喂 usage 扫描（不受分帧影响）
        a_pConn->aSseLine += aData;
        int iNl = 0;
        while ((iNl = a_pConn->aSseLine.indexOf('\n')) >= 0) {
            const QByteArray sLine = a_pConn->aSseLine.left(iNl + 1);
            a_pConn->aSseLine.remove(0, iNl + 1);
            aBody += ProcessSseLine(a_pConn, sLine);
        }
    }
    aOut += WrapChunked(aBody);
    return aOut;
}

void CCodexProxy::ScanUsage(SConn* a_pConn, const QByteArray& a_pChunk) {
    // 拼滚动尾巴（usage JSON 可能被 TCP 分块切开），在尾部窗口内找 "usage" 对象：
    //   {"prompt_tokens":N,...,"completion_tokens":M,...}（SSE 末块 / 非流式 JSON 通用）。
    // 每找到一处 = 一次 chat/completions 的真实计量 → sigUsage 上抛（runner 累计）。
    // 尾巴/偏移均挂连接（旧版类级共享：并发连接的上游字节互相污染，交错/残留
    //   会错检或重复计）；持久偏移 iUpTailScan 防已上报 usage 被后续数据块重复
    //   扫描重复 emit（旧版每轮从 0 重扫、仅消费超 512 字节才截断——首个 usage
    //   在 512 字节内时每个后续块都重报一次，用量/费用系统性虚高）。
    QByteArray& aTail = a_pConn->aUpTail;
    aTail.append(a_pChunk);
    while (true) {
        const int iUsage = aTail.indexOf("\"usage\"", a_pConn->iUpTailScan);
        if (iUsage < 0) {
            // 无未闭合对象：推进偏移，只保留 "usage" 词长-1 的跨界余量（词被 TCP
            //   分块切开时下一块仍能命中）。
            a_pConn->iUpTailScan = qMax(a_pConn->iUpTailScan, aTail.size() - 7);
            break;
        }
        const int iObjEnd = aTail.indexOf('}', iUsage);
        if (iObjEnd < 0) {
            a_pConn->iUpTailScan = iUsage;   // 对象未收全：钉在起始处等下一块
            break;
        }
        const QByteArray sObj = aTail.mid(iUsage, iObjEnd - iUsage);
        qint64 iIn = 0, iOut = 0;
        const QRegularExpression reIn(QStringLiteral("\"prompt_tokens\"\\s*:\\s*(\\d+)"));
        const QRegularExpression reOut(QStringLiteral("\"completion_tokens\"\\s*:\\s*(\\d+)"));
        const auto mIn = reIn.match(QString::fromLatin1(sObj));
        const auto mOut = reOut.match(QString::fromLatin1(sObj));
        if (mIn.hasMatch() && mOut.hasMatch()) {
            iIn = mIn.captured(1).toLongLong();
            iOut = mOut.captured(1).toLongLong();
            emit sigUsage(iIn, iOut);
        }
        a_pConn->iUpTailScan = iObjEnd + 1;
    }
    // 修剪前缀（偏移之前均已消费/无需重扫），防尾巴无限增长。
    if (a_pConn->iUpTailScan > 4096) {
        aTail = aTail.mid(a_pConn->iUpTailScan);
        a_pConn->iUpTailScan = 0;
    }
    if (aTail.size() > 16384) {   // 极端不收敛兑底（乱流无 '}'）
        aTail = aTail.right(8192);
        a_pConn->iUpTailScan = qBound(0, a_pConn->iUpTailScan, aTail.size());
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
