#include "CFatalWitness.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

QtMessageHandler s_pDefaultHandler = nullptr;   // Install 时捕获的默认 handler

// qFatal 见证 handler：位于 qFatal→qAbort→__fastfail 链最前端。
//   遗言落 exe 同级 dump\_qfatal_witness.txt（追加模式，多次死亡累积）。
//   只用 Win32 文件 API——fatal 时堆可能已损坏，禁止 QFile 等重活；
//   a_sMsg.toUtf8() 一次小分配可接受（Qt 内部 fatal 消息本身就是 QString）。
//   严禁往 stdout 写：--mcp-ask-server 模式 stdout 是 JSON-RPC 协议通道。
void FatalMsgHandler(QtMsgType a_eType, const QMessageLogContext& a_stCtx, const QString& a_sMsg)
{
    if (a_eType == QtFatalMsg) {
        char szExe[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, szExe, MAX_PATH) != 0) {
            char* pSlash = std::strrchr(szExe, '\\');
            if (pSlash) {
                *pSlash = '\0';
                char szPath[MAX_PATH];
                std::snprintf(szPath, MAX_PATH, "%s\\dump\\_qfatal_witness.txt", szExe);
                CreateDirectoryA((std::string(szExe) + "\\dump").c_str(), nullptr);
                HANDLE h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    const QByteArray aUtf8 = a_sMsg.toUtf8();
                    char szLine[2048];
                    const int iN = std::snprintf(szLine, sizeof(szLine),
                        "[%04d-%02d-%02d %02d:%02d:%02d] pid=%lu %s [%s:%u]\r\n",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                        (unsigned long)GetCurrentProcessId(),
                        aUtf8.constData(),
                        a_stCtx.file ? a_stCtx.file : "?", (unsigned)a_stCtx.line);
                    DWORD dwWr = 0;
                    WriteFile(h, szLine, iN > 0 ? (DWORD)iN : 0, &dwWr, nullptr);
                    CloseHandle(h);
                }
            }
        }
    }
    // 全部消息透传 Install 时捕获的默认 handler（qInstallMessageHandler 的返回值）。
    //   不在 handler 里调 qInstallMessageHandler(nullptr)：那会把本见证在第一条消息
    //   （任何 qDebug/qWarning）后就卸载，后续 qFatal 再无人记录。
    if (s_pDefaultHandler) {
        s_pDefaultHandler(a_eType, a_stCtx, a_sMsg);
    }
}

} // namespace

namespace CFatalWitness {

void Install() {
    s_pDefaultHandler = qInstallMessageHandler(FatalMsgHandler);
}

} // namespace CFatalWitness
