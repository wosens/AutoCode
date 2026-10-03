#pragma once

#include <QObject>
#include <QThread>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QString>

// stdin 读线程：按行读取 claude 发来的 JSON-RPC 消息。
// Windows GUI 子系统下 QSocketNotifier 对管道 stdin 支持不稳，改用独立线程阻塞读，
// 收到一行经队列连接送到主线程解析；stdin 关闭(EOF)时通知退出。
class CStdInReaderThread : public QThread {
    Q_OBJECT

public:
    explicit CStdInReaderThread(QObject* a_pParent = nullptr);

signals:
    void sigLineReady(const QByteArray& a_sLine);   // 一行原始 JSON（已去换行）
    void sigEof();                                   // stdin 关闭 → 退出信号

protected:
    void run() override;
};

// MCP ask-user server：AutoCode.exe --mcp-ask-server 模式入口。
// 经 stdio 跑 JSON-RPC 2.0（MCP 协议，newline-delimited JSON），向 Claude Code 暴露
// 单个工具 ask_user。收到 tools/call(ask_user) 时，经 QLocalSocket 把提问转发到
// 主窗口（QLocalServer 监听中），等用户作答后回 tool_result，Claude 在同一 -p 进程内继续。
//
// 注意：stdout 只允许写 JSON-RPC；任何调试输出走 stderr，否则破坏协议。
// 生命周期：由 main 在 QCoreApplication 下构造，Start() 启动读线程；stdin 关闭即退出。
class CMcpAskServer : public QObject {
    Q_OBJECT

public:
    explicit CMcpAskServer(QObject* a_pParent = nullptr);
    ~CMcpAskServer() override;

    // 启动 stdin 读线程（非阻塞，立即返回）。
    void Start();

private slots:
    void OnStdInLine(const QByteArray& a_sLine);   // 一行 JSON 到达 → 解析分派
    void OnStdInEof();                              // stdin 关闭 → 退出 app

private:
    void HandleMessage(const QJsonObject& a_oMsg);
    // tools/call(ask_user)：经 QLocalSocket 转发提问，同步等回答后回 tool_result。
    void HandleAskUser(const QJsonValue& a_vId, const QJsonObject& a_oArgs);
    // 2026-09-26 tools/call(sandbox_*)：内置测试沙箱工具，经同一管道 {type:sbx}
    //   转发主实例 CSandboxManager::Execute，回 tool_result。
    void HandleSandboxCall(const QJsonValue& a_vId, const QString& a_sCmd,
                           const QJsonObject& a_oArgs);
    void WriteJson(const QJsonObject& a_oMsg);     // 写一行 JSON 到 stdout（立即 flush）

    CStdInReaderThread* m_pReader = nullptr;
};
