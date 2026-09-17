// CTaskOrchestrator + pi 后端 + 本地 spark 真机端到端（手动跑，不挂 ctest）。
//
// 场景复现（2026.09.09 用户连环报障的收口验证）：
//   1) 起会话（小提问拿 sid）；
//   2) 串行连发中等 payload（每轮 ~5k token）把会话逐步顶过服务端 n_ctx=65536
//      （pi+spark 系统+工具开销本身 ~55k，会话预算只剩 ~10k）——自校准：不知道
//      确切 token 配比，发到超限为止；
//   3) 触发 400 exceed_context_size → 断言 v2.1.132/133 自动压缩自愈链：
//      compact_start（摘要发往全新会话+内联记录）→ compact_done（重播种）→
//      自动重发原提问 → 最终 done（非终结性 stage_failed）。
//
// 现场保护：运行前备份 ~/.pi/agent/models.json，退出时还原（SetPiModels 会重写它）。
// 前置：本地 llama.cpp 127.0.0.1:8080 在跑（spark2.5-4b，n_ctx=65536）。
// 用法：Execute/Release/test_orchpi.exe [超时秒(默认 1200)] [中载荷轮数上限(默认 6)]
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTextStream>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <functional>

#include "orch/CTaskOrchestrator.h"
#include "project/CProjectManager.h"
#include "agent/CPiAgentRunner.h"
#include "agent/CAgentRunnerFactory.h"
#include "core/CModelEntry.h"

static QTextStream g_out(stdout);
static int g_iFail = 0;
static QStringList g_aEvents;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const int iTimeoutSec = (argc > 1) ? QByteArray(argv[1]).toInt() : 1200;
    const int iMaxGrowTurns = (argc > 2) ? QByteArray(argv[2]).toInt() : 6;
    QString sRealJson;      // 真实 models.json 路径与字节备份（退出还原）
    QByteArray baJsonBackup;
    int iRc = 0;
    {
        QTemporaryDir oTmp;
        if (!oTmp.isValid()) { g_out << "TEMP DIR FAIL\n"; return 1; }
        const QString sProj = oTmp.path();
        QDir().mkpath(sProj + QStringLiteral("/.autodev"));

        CProjectManager stPm(nullptr);
        if (!stPm.Open(sProj)) { g_out << "OPEN FAIL\n"; return 1; }

        // 2026.09.09 v2.1.133B 现场口径：pi 子进程读真实 ~/.pi/agent/models.json——
        // 隔离路径只改 AutoCode 写侧会造成两表错位（autocode-1 含义不一致→请求打错网关）。
        // 改为：读真实表现有 autocode-* 槽位原样保留 + 追加 spark 专用槽（末位激活），
        // 退出时字节还原原文件。
        sRealJson = QDir::homePath() + QStringLiteral("/.pi/agent/models.json");
        QList<SDshModelEntry> aLoad;
        {
            QFile fBak(sRealJson);
            if (fBak.open(QIODevice::ReadOnly)) { baJsonBackup = fBak.readAll(); fBak.close(); }
        }
        {
            const QJsonObject oRoot = QJsonDocument::fromJson(baJsonBackup).object();
            const QJsonObject oProv = oRoot.value(QStringLiteral("providers")).toObject();
            for (int i = 1; i <= 32; ++i) {
                const QString sKey = QStringLiteral("autocode-%1").arg(i);
                if (!oProv.contains(sKey)) { break; }
                const QJsonObject o = oProv.value(sKey).toObject();
                const QJsonObject oM = o.value(QStringLiteral("models")).toArray().at(0).toObject();
                SDshModelEntry st;
                st.sDisp = oM.value(QStringLiteral("name")).toString();
                st.sBaseURL = o.value(QStringLiteral("baseUrl")).toString();
                st.sApi = o.value(QStringLiteral("api")).toString();
                st.sModelId = oM.value(QStringLiteral("id")).toString();
                st.sApiKey = o.value(QStringLiteral("apiKey")).toString();
                st.iCtx = static_cast<int>(oM.value(QStringLiteral("contextWindow")).toDouble());
                st.iMaxTok = static_cast<int>(oM.value(QStringLiteral("maxTokens")).toDouble());
                aLoad.append(st);
            }
        }
        CAgentRunnerFactory::SetBackend(CAgentRunnerFactory::BackendPi);
        CPiAgentRunner::SetPiRootPath(QStringLiteral("E:/wosens/Programs/VisualC++/QT/PIAgent"));
        SDshModelEntry stE;
        stE.sDisp = QStringLiteral("spark-local-e2e");
        stE.sBaseURL = QStringLiteral("http://127.0.0.1:8080/v1");
        stE.sApi = QStringLiteral("openai-completions");
        stE.sModelId = QStringLiteral("spark2.5-4b");
        stE.sApiKey = QStringLiteral("sk-local-spark");
        stE.iCtx = 60000;
        stE.iMaxTok = 1024;
        aLoad.append(stE);
        CPiAgentRunner::SetPiModels(aLoad, aLoad.size() - 1);   // spark=末位槽激活

        CTaskOrchestrator stOrch(&stPm, nullptr);
        const QString sId = QStringLiteral("asst-e2e");

        int iTurn = 0;                 // 已提交轮数（含压缩重发前原提问）
        int iFinished = 0;             // sigAdhocFinished 次数
        bool bCompactSeen = false;
        bool bFinalDone = false;
        std::function<void()> submitNext;   // 先声明后赋值（finished 回调引用）

        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocEvent,
            [&](const QString&, const QString& sKind, const QString& sText) {
            g_aEvents << sKind;
            g_out << "[ev ] " << sKind << " | " << sText.left(110) << Qt::endl;
            if (sKind == QStringLiteral("compact_start")) { bCompactSeen = true; }
            if (sKind == QStringLiteral("compact_fail")) {
                g_out << "[FAIL] 压缩失败\n"; ++g_iFail; app.quit();
            }
        });
        QObject::connect(&stOrch, &CTaskOrchestrator::sigAdhocFinished,
            [&]() {
            ++iFinished;
            g_out << "[fin] #" << iFinished << Qt::endl;
            // 每次 finished 后推进下一轮（串行链，杜绝轮中注入路径）。
            if (bCompactSeen) {
                // 压缩链已收尾：done 事件即最终成功（自动重发在 fin 之后 singleShot(0)，
                // 故再等一次 finished；两次之内必达 done）。
                const int iCdi = g_aEvents.lastIndexOf(QStringLiteral("compact_done"));
                const int iDoneAfter = g_aEvents.lastIndexOf(QStringLiteral("done"));
                if (iCdi >= 0 && iDoneAfter > iCdi) {
                    bFinalDone = true; app.quit(); return;
                }
                if (iFinished > 5) { g_out << "[FAIL] 压缩后未收敛\n"; ++g_iFail; app.quit(); }
                return;   // 等自动重发轮的 finished
            }
            // 未压缩：继续顶窗口
            ++iTurn;
            if (iTurn > iMaxGrowTurns + 1) {
                g_out << "[FAIL] 连发 " << iMaxGrowTurns << " 轮仍未触发超限压缩\n"; ++g_iFail; app.quit(); return;
            }
            submitNext();
        });

        // 中等载荷：~8k 字（≈5k token）——服务窗口 65536、系统开销 ~55k，
        // 首轮即可过、累计 2~3 轮内必超。
        QString sBig;
        for (int i = 0; i < 320; ++i) {
            sBig += QStringLiteral("填充文本第%1句用于把会话逐步顶过模型窗口边界内容无意义仅计数。").arg(i);
        }
        submitNext = [&]() {
            const QString sText = (iTurn == 1)
                ? QStringLiteral("只回复两个字：成功")
                : QStringLiteral("请确认收到以下编号文本段（不必复述全文，回复编号范围即可）：%1")
                      .arg(sBig);
            g_out << "=== 提交轮 " << iTurn << " ===" << Qt::endl;
            stOrch.SubmitAdhoc(sId, sText, /*perm=*/0, QString());
        };

        QTimer::singleShot(0, [&]() { iTurn = 1; submitNext(); });
        QTimer::singleShot(iTimeoutSec * 1000, [&]() {
            g_out << "TIMEOUT\n"; ++g_iFail; app.quit();
        });
        iRc = app.exec();

        if (!bCompactSeen) { g_out << "[FAIL] 未出现 compact_start（超限未触发/未识别）\n"; ++g_iFail; }
        if (!bFinalDone)   { g_out << "[FAIL] 压缩后未到达最终 done\n"; ++g_iFail; }
    }

    if (!baJsonBackup.isEmpty()) {
        QFile fRest(sRealJson);
        if (fRest.open(QIODevice::WriteOnly | QIODevice::Truncate)) { fRest.write(baJsonBackup); fRest.close(); }
    }
    g_out << (g_iFail == 0 ? QStringLiteral("ALL PASS") : QString::number(g_iFail) + QStringLiteral(" FAILURES")) << Qt::endl;
    return (g_iFail == 0 && iRc == 0) ? 0 : 1;
}
