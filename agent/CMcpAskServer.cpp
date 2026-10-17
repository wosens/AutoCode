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
        // 2026-09-26 工具清单组装：ask_user + 内置沙箱 sandbox_*（常驻可见，Agent 无需
        //   另装技能；主实例未启用沙箱时调用返回明确错误文本，不破坏协议）。
        QJsonArray aTools;
        {
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
            aTools.append(oTool);
        }
        // —— sandbox_* 工具（统一约定：hwnd 可空=唯一沙箱窗口；坐标为客户区坐标）——
        // schema 程序化组装（属性名→类型），避免内联 JSON 转义脆弱。
        auto fnStrProp = [](const QString& a_sDesc = QString()) {
            QJsonObject oP;
            oP.insert(QStringLiteral("type"), QStringLiteral("string"));
            if (!a_sDesc.isEmpty()) {
                oP.insert(QStringLiteral("description"), a_sDesc);
            }
            return oP;
        };
        auto fnIntProp = []() {
            QJsonObject oP;
            oP.insert(QStringLiteral("type"), QStringLiteral("integer"));
            return oP;
        };
        auto fnBoolProp = []() {
            QJsonObject oP;
            oP.insert(QStringLiteral("type"), QStringLiteral("boolean"));
            return oP;
        };
        struct SToolDef {
            const char* szName;
            const char* szDesc;
            QJsonObject oSchema;
        };
        QList<SToolDef> aSbxTools;
        {
            auto fnMake = [&fnStrProp, &fnIntProp, &fnBoolProp](
                    const char* a_szName, const char* a_szDesc,
                    const QStringList& a_aProps, const QStringList& a_aRequired = {}) {
                // a_aProps 元素："hwnd" / "x:int" / "double:bool"...
                QJsonObject oProps;
                for (const QString& s : a_aProps) {
                    const QString sName = s.section(QLatin1Char(':'), 0, 0);
                    const QString sTy = s.section(QLatin1Char(':'), 1);
                    if (sTy == QLatin1String("int")) {
                        oProps.insert(sName, fnIntProp());
                    } else if (sTy == QLatin1String("bool")) {
                        oProps.insert(sName, fnBoolProp());
                    } else if (sName == QLatin1String("hwnd")) {
                        oProps.insert(sName, fnStrProp(
                            QStringLiteral("目标窗口 hwnd（空=唯一沙箱窗口）")));
                    } else {
                        oProps.insert(sName, fnStrProp());
                    }
                }
                QJsonObject oSchema;
                oSchema.insert(QStringLiteral("type"), QStringLiteral("object"));
                oSchema.insert(QStringLiteral("properties"), oProps);
                if (!a_aRequired.isEmpty()) {
                    QJsonArray aReq;
                    for (const QString& s : a_aRequired) {
                        aReq.append(s);
                    }
                    oSchema.insert(QStringLiteral("required"), aReq);
                }
                SToolDef st;
                st.szName = a_szName;
                st.szDesc = a_szDesc;
                st.oSchema = oSchema;
                return st;
            };
            aSbxTools.append(fnMake("sandbox_launch",
                "由沙箱代为启动被测 GUI 程序（启动被测程序的首选方式）：主进程亲自"
                "启动并登记，窗口弹出即自动隐藏收纳；不受中间 shell（Start-Process/"
                "cmd start）秒退断链影响。exe=程序全路径；args=命令行参数（可省）；"
                "workdir=工作目录（可省，默认 exe 所在目录）。",
                { QStringLiteral("exe"), QStringLiteral("args"),
                  QStringLiteral("workdir") },
                { QStringLiteral("exe") }));
            aSbxTools.append(fnMake("sandbox_windows",
                "列出内置沙箱中已收纳的窗口（Agent 启动的 GUI 程序会被自动隐藏收纳，"
                "不打扰用户键鼠）。返回 hwnd/pid/尺寸/标题/进程。GUI 测试第一步先调它。",
                {}));
            aSbxTools.append(fnMake("sandbox_capture",
                "对沙箱窗口离屏截图，返回 PNG 路径；用读图工具查看后再定位坐标点击。"
                "GUI 测试主手段：改一步截一张验证。",
                { QStringLiteral("hwnd"), QStringLiteral("path") }));
            aSbxTools.append(fnMake("sandbox_click",
                "向沙箱窗口投递鼠标点击（客户区坐标；后台点击不抢焦点）。"
                "double=true 双击；right=true 右键。",
                { QStringLiteral("hwnd"), QStringLiteral("x:int"),
                  QStringLiteral("y:int"), QStringLiteral("double:bool"),
                  QStringLiteral("right:bool") },
                { QStringLiteral("x"), QStringLiteral("y") }));
            aSbxTools.append(fnMake("sandbox_drag",
                "向沙箱窗口投递鼠标拖拽（客户区坐标，起止点）。",
                { QStringLiteral("hwnd"), QStringLiteral("x1:int"),
                  QStringLiteral("y1:int"), QStringLiteral("x2:int"),
                  QStringLiteral("y2:int"), QStringLiteral("ms:int") },
                { QStringLiteral("x1"), QStringLiteral("y1"),
                  QStringLiteral("x2"), QStringLiteral("y2") }));
            aSbxTools.append(fnMake("sandbox_type",
                "向沙箱窗口输入文本（逐字符 WM_CHAR，无需焦点）。",
                { QStringLiteral("hwnd"), QStringLiteral("text") },
                { QStringLiteral("text") }));
            aSbxTools.append(fnMake("sandbox_keys",
                "向沙箱窗口发送组合键，如 \"ctrl+s\"、\"alt+f4\"、\"enter\"。",
                { QStringLiteral("hwnd"), QStringLiteral("keys") },
                { QStringLiteral("keys") }));
            aSbxTools.append(fnMake("sandbox_command",
                "向沙箱窗口发送菜单命令（WM_COMMAND，cmd=菜单项 ID）。",
                { QStringLiteral("hwnd"), QStringLiteral("cmd:int") },
                { QStringLiteral("cmd") }));
            aSbxTools.append(fnMake("sandbox_button",
                "点击指定按钮控件（hwnd=按钮控件句柄，BM_CLICK）。",
                { QStringLiteral("hwnd") }));
            aSbxTools.append(fnMake("sandbox_tree",
                "转储沙箱窗口的 UIA 控件树：名称/类名/尺寸 + click=(x,y) 客户区坐标，"
                "按控件名定位后配合 sandbox_click 使用。",
                { QStringLiteral("hwnd") }));
            aSbxTools.append(fnMake("sandbox_add",
                "把指定窗口显式收进沙箱（自动收纳关闭时手工收纳）。",
                { QStringLiteral("hwnd") }, { QStringLiteral("hwnd") }));
            aSbxTools.append(fnMake("sandbox_restore",
                "取消沙箱隐藏：窗口恢复可见可交互（用户想亲自查看时）。",
                { QStringLiteral("hwnd") }));
            aSbxTools.append(fnMake("sandbox_restore_all",
                "还原沙箱中全部窗口（一次性恢复可见）。", {}));
            aSbxTools.append(fnMake("sandbox_close",
                "关闭沙箱窗口（WM_CLOSE，并结束其录像）。",
                { QStringLiteral("hwnd") }));
        }
        for (const SToolDef& st : aSbxTools) {
            QJsonObject oTool;
            oTool.insert(QStringLiteral("name"), QString::fromLatin1(st.szName));
            oTool.insert(QStringLiteral("description"), QString::fromUtf8(st.szDesc));
            oTool.insert(QStringLiteral("inputSchema"), st.oSchema);
            aTools.append(oTool);
        }
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
        // 2026-09-26 内置沙箱工具：sandbox_<cmd> → 主实例 CSandboxManager::Execute(cmd)。
        if (sName.startsWith(QStringLiteral("sandbox_"))) {
            HandleSandboxCall(vId, sName.mid(QStringLiteral("sandbox_").size()), oArgs);
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

// ---------------------------------------------------------------------------
// 2026-09-26 内置测试沙箱工具转发：{type:sbx,cmd,args} → 主实例 → {type:sbxres,ok,text}
// ---------------------------------------------------------------------------
void CMcpAskServer::HandleSandboxCall(const QJsonValue& a_vId, const QString& a_sCmd,
                                      const QJsonObject& a_oArgs) {
    auto fnRespond = [this, a_vId](const QString& a_sText, bool a_bError) {
        QJsonObject oResp;
        oResp.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        oResp.insert(QStringLiteral("id"), a_vId);
        oResp.insert(QStringLiteral("result"), MakeTextResult(a_sText, a_bError));
        WriteJson(oResp);
    };

    const QString sPipe = QString::fromLocal8Bit(qgetenv("AUTOCODE_ASK_PIPE"));
    if (sPipe.isEmpty()) {
        fnRespond(QStringLiteral("[沙箱不可用：未配置管道]"), true);
        return;
    }
    QLocalSocket sock;
    sock.connectToServer(sPipe);
    if (!sock.waitForConnected(3000)) {
        fnRespond(QStringLiteral("[沙箱不可用：无法连接主窗口]"), true);
        return;
    }
    QJsonObject oReq;
    oReq.insert(QStringLiteral("type"), QStringLiteral("sbx"));
    oReq.insert(QStringLiteral("cmd"), a_sCmd);
    oReq.insert(QStringLiteral("args"), a_oArgs);
    sock.write(QJsonDocument(oReq).toJson(QJsonDocument::Compact) + "\n");
    if (!sock.waitForBytesWritten(3000)) {
        fnRespond(QStringLiteral("[沙箱不可用：发送命令失败]"), true);
        return;
    }
    // 沙箱操作毫秒级（截图/点击）到秒级（控件树），180s 兑底足够。
    QElapsedTimer t;
    t.start();
    bool bGot = false, bOk = false;
    QString sText;
    while (!bGot && t.elapsed() < 180000) {
        if (sock.state() == QLocalSocket::UnconnectedState && sock.bytesAvailable() == 0) {
            break;
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
            if (o.value(QStringLiteral("type")).toString() == QStringLiteral("sbxres")) {
                bOk = o.value(QStringLiteral("ok")).toBool();
                sText = o.value(QStringLiteral("text")).toString();
                bGot = true;
                break;
            }
        }
        if (bGot) {
            break;
        }
        sock.waitForReadyRead(200);
    }
    sock.disconnectFromServer();
    if (!bGot) {
        fnRespond(QStringLiteral("[沙箱命令超时未响应]"), true);
        return;
    }
    fnRespond(sText, !bOk);
}
