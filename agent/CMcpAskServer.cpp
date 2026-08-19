#include "CMcpAskServer.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QCoreApplication>

#include <iostream>
#include <string>

namespace {

// 构造 ask_user 的 tool_result：纯文本 content；a_bError=true → 附 isError。
QJsonObject MakeTextResult(const QString& a_sText, bool a_bError) {
    QJsonObject oContent;
    oContent.insert(QStringLiteral("type"), QStringLiteral("text"));
    oContent.insert(QStringLiteral("text"), a_sText);
    QJsonArray aContent;
    aContent.append(oContent);
    QJsonObject oResult;
    oResult.insert(QStringLiteral("content"), aContent);
    if (a_bError) {
        oResult.insert(QStringLiteral("isError"), true);
    }
    return oResult;
}

} // namespace

// ---------------------------------------------------------------------------
// CStdInReaderThread
// ---------------------------------------------------------------------------
CStdInReaderThread::CStdInReaderThread(QObject* a_pParent)
    : QThread(a_pParent) {
}

void CStdInReaderThread::run() {
    // 阻塞按行读 stdin：claude 经 stdio transport 投来 newline-delimited JSON。
    // 管道关闭时 getline 返回 false（EOF）→ 退出循环 → 发 sigEof 触发 app.quit。
    std::string sLine;
    while (std::getline(std::cin, sLine)) {
        if (!sLine.empty()) {
            emit sigLineReady(QByteArray::fromStdString(sLine));
        }
    }
    emit sigEof();
}

// ---------------------------------------------------------------------------
// CMcpAskServer
// ---------------------------------------------------------------------------
CMcpAskServer::CMcpAskServer(QObject* a_pParent)
    : QObject(a_pParent) {
}

CMcpAskServer::~CMcpAskServer() {
    if (m_pReader != nullptr && m_pReader->isRunning()) {
        // 读线程阻塞在 getline：正常流程 claude 关闭 stdin 后会自然 EOF 退出；
        // 此处仅兜底等待 1s（QThread 析构前必须 join，避免 detached 仍跑）。
        m_pReader->wait(1000);
    }
}

void CMcpAskServer::Start() {
    m_pReader = new CStdInReaderThread(this);
    connect(m_pReader, &CStdInReaderThread::sigLineReady,
            this, &CMcpAskServer::OnStdInLine);
    connect(m_pReader, &CStdInReaderThread::sigEof,
            this, &CMcpAskServer::OnStdInEof);
    m_pReader->start();
}

void CMcpAskServer::OnStdInEof() {
    // stdin 关闭 = claude 已结束 → 退出事件循环（进程随之结束）
    if (QCoreApplication* pApp = qApp) {
        pApp->quit();
    }
}

void CMcpAskServer::OnStdInLine(const QByteArray& a_sLine) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        return;   // 非 JSON 行忽略，不破坏协议
    }
    HandleMessage(stDoc.object());
}

void CMcpAskServer::WriteJson(const QJsonObject& a_oMsg) {
    // stdout 写一行 JSON-RPC 并立即 flush。严禁 qDebug→stdout（破坏协议）。
    const QByteArray aBytes = QJsonDocument(a_oMsg).toJson(QJsonDocument::Compact);
    std::cout << aBytes.toStdString() << "\n";
    std::cout.flush();
}

void CMcpAskServer::HandleMessage(const QJsonObject& a_oMsg) {
    const QJsonValue vId = a_oMsg.value(QStringLiteral("id"));
    const QString sMethod = a_oMsg.value(QStringLiteral("method")).toString();
    const QJsonObject oParams = a_oMsg.value(QStringLiteral("params")).toObject();

    if (sMethod == QStringLiteral("initialize")) {
        QJsonObject oCaps;
        oCaps.insert(QStringLiteral("tools"), QJsonObject{});
        QJsonObject oInfo;
        oInfo.insert(QStringLiteral("name"), QStringLiteral("autocode-ask"));
        oInfo.insert(QStringLiteral("version"), QStringLiteral("1.0"));
        QJsonObject oResult;
        oResult.insert(QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05"));
        oResult.insert(QStringLiteral("capabilities"), oCaps);
        oResult.insert(QStringLiteral("serverInfo"), oInfo);

        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), vId);
        oResp.insert(QStringLiteral("result"), oResult);
        WriteJson(oResp);
        return;
    }

    // 通知（无 id）：initialized 等，一律不回包
    if (sMethod.startsWith(QStringLiteral("notifications/"))) {
        return;
    }

    if (sMethod == QStringLiteral("tools/list")) {
        QJsonObject oPropQ;
        oPropQ.insert(QStringLiteral("type"), QStringLiteral("string"));
        oPropQ.insert(QStringLiteral("description"), QStringLiteral("向用户提出的问题文本"));
        QJsonObject oItem;
        oItem.insert(QStringLiteral("type"), QStringLiteral("string"));
        QJsonObject oPropO;
        oPropO.insert(QStringLiteral("type"), QStringLiteral("array"));
        oPropO.insert(QStringLiteral("items"), oItem);
        oPropO.insert(QStringLiteral("description"),
            QStringLiteral("可选项；为空或省略表示让用户自由输入"));
        QJsonObject oProps;
        oProps.insert(QStringLiteral("question"), oPropQ);
        oProps.insert(QStringLiteral("options"), oPropO);
        QJsonArray aReq;
        aReq.append(QStringLiteral("question"));
        QJsonObject oSchema;
        oSchema.insert(QStringLiteral("type"), QStringLiteral("object"));
        oSchema.insert(QStringLiteral("properties"), oProps);
        oSchema.insert(QStringLiteral("required"), aReq);
        QJsonObject oTool;
        oTool.insert(QStringLiteral("name"), QStringLiteral("ask_user"));
        oTool.insert(QStringLiteral("description"),
            QStringLiteral("向用户提问以获取确认、选择或澄清。需要在多个方案间做决定、"
                           "或需要确认非显然的意图时调用，不要自行假设用户意图。"));
        oTool.insert(QStringLiteral("inputSchema"), oSchema);
        QJsonArray aTools;
        aTools.append(oTool);
        QJsonObject oResult;
        oResult.insert(QStringLiteral("tools"), aTools);

        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), vId);
        oResp.insert(QStringLiteral("result"), oResult);
        WriteJson(oResp);
        return;
    }

    if (sMethod == QStringLiteral("tools/call")) {
        const QString sName = oParams.value(QStringLiteral("name")).toString();
        const QJsonObject oArgs = oParams.value(QStringLiteral("arguments")).toObject();
        if (sName == QStringLiteral("ask_user")) {
            HandleAskUser(vId, oArgs);
            return;
        }
        QJsonObject oErr;
        oErr.insert(QStringLiteral("code"), -32602);
        oErr.insert(QStringLiteral("message"), QStringLiteral("未知工具：%1").arg(sName));
        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), vId);
        oResp.insert(QStringLiteral("error"), oErr);
        WriteJson(oResp);
        return;
    }

    if (!vId.isUndefined()) {
        QJsonObject oErr;
        oErr.insert(QStringLiteral("code"), -32601);
        oErr.insert(QStringLiteral("message"), QStringLiteral("未实现的方法：%1").arg(sMethod));
        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), vId);
        oResp.insert(QStringLiteral("error"), oErr);
        WriteJson(oResp);
    }
}

void CMcpAskServer::HandleAskUser(const QJsonValue& a_vId, const QJsonObject& a_oArgs) {
    const QString sQuestion = a_oArgs.value(QStringLiteral("question")).toString();
    const QJsonArray aOptions = a_oArgs.value(QStringLiteral("options")).toArray();

    // 写 tool_result 响应（统一出口）
    auto fnRespond = [this, a_vId](const QJsonObject& oResult) {
        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), a_vId);
        oResp.insert(QStringLiteral("result"), oResult);
        WriteJson(oResp);
    };

    const QString sPipe = QString::fromLocal8Bit(qgetenv("AUTOCODE_ASK_PIPE"));
    if (sPipe.isEmpty()) {
        fnRespond(MakeTextResult(QStringLiteral("[提问服务不可用：未配置管道]"), true));
        return;
    }

    QLocalSocket sock;
    sock.connectToServer(sPipe);
    if (!sock.waitForConnected(3000)) {
        fnRespond(MakeTextResult(QStringLiteral("[提问服务不可用：无法连接主窗口]"), true));
        return;
    }

    // 发送提问
    QJsonObject oAsk;
    oAsk.insert(QStringLiteral("type"), QStringLiteral("ask"));
    oAsk.insert(QStringLiteral("question"), sQuestion);
    oAsk.insert(QStringLiteral("options"), aOptions);
    const QByteArray aAskBytes = QJsonDocument(oAsk).toJson(QJsonDocument::Compact);
    sock.write(aAskBytes + "\n");
    if (!sock.waitForBytesWritten(3000)) {
        fnRespond(MakeTextResult(QStringLiteral("[提问服务不可用：发送提问失败]"), true));
        return;
    }

    // 同步等回答（默认 10 分钟，config.ini [Agent] AskTimeoutSec 经 mcp-config env 传入），
    // 每轮 waitForReadyRead(500) 轮询累计超时
    QElapsedTimer t;
    t.start();
    const qint64 iTimeoutSec = qEnvironmentVariableIntValue("AUTOCODE_ASK_TIMEOUT_SEC");
    const qint64 iTimeoutMs = (iTimeoutSec > 0 ? iTimeoutSec : 600) * 1000;
    QString sReply;
    bool bGot = false;
    bool bCancelled = false;

    while (!bGot && t.elapsed() < iTimeoutMs) {
        if (sock.state() == QLocalSocket::UnconnectedState && sock.bytesAvailable() == 0) {
            break;   // 主窗口主动断开
        }
        while (sock.canReadLine()) {
            const QByteArray aLine = sock.readLine().trimmed();
            if (aLine.isEmpty()) {
                continue;
            }
            const QJsonDocument stDoc = QJsonDocument::fromJson(aLine);
            if (!stDoc.isObject()) {
                continue;
            }
            const QJsonObject o = stDoc.object();
            const QString sType = o.value(QStringLiteral("type")).toString();
            if (sType == QStringLiteral("answer")) {
                sReply = o.value(QStringLiteral("text")).toString();
                bGot = true;
                break;
            }
            if (sType == QStringLiteral("cancel")) {
                bCancelled = true;
                bGot = true;
                break;
            }
        }
        if (bGot) {
            break;
        }
        sock.waitForReadyRead(500);   // 阻塞最多 500ms 或到数据
    }

    sock.disconnectFromServer();
    if (sock.state() != QLocalSocket::UnconnectedState) {
        sock.waitForDisconnected(1000);
    }

    if (!bGot) {
        fnRespond(MakeTextResult(QStringLiteral("[提问超时未响应]"), true));
        return;
    }
    if (bCancelled) {
        fnRespond(MakeTextResult(QStringLiteral("[用户取消了提问]"), true));
        return;
    }
    fnRespond(MakeTextResult(sReply, false));
}
