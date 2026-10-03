// test_reviewsdk.cpp —— ReviewSdk 客户端接入回归（2026-09-30）
// 覆盖：动态加载/版本/语言清单/语言判定；ReviewContent 全链（mock LLM 固定应答
// → findings JSON 解析）；模型失败错误码；未知语言返回码；RelativeToRoot 纯函数。
// 不依赖真实后端（LLM 回调用 canned 响应）；dll 缺失时用例跳过（返回 0）。

#include <QtTest/QtTest>
#include <QDir>
#include <QTemporaryDir>

#include "review/CReviewSdkClient.h"

namespace {
// 固定应答的 mock LLM（thread_local 保活借用指针——SDK 即时拷贝）。
thread_local QByteArray s_tlResp;
int s_iMockCalls = 0;

const char* MockLlmOk(void*, const char*, const char**) {
    ++s_iMockCalls;
    s_tlResp = "[{\"file\":\"demo.cpp\",\"line\":12,\"severity\":\"high\","
               "\"rule\":\"bugs\",\"message\":\"空指针解引用\","
               "\"suggestion\":\"判空后再访问\"}]";
    return s_tlResp.constData();
}
const char* MockLlmFail(void*, const char*, const char** a_ppszErr) {
    if (a_ppszErr != nullptr) {
        *a_ppszErr = "mock 模型故障";
    }
    return nullptr;
}
} // namespace

class TestReviewSdk : public QObject {
    Q_OBJECT

private:
    QString m_sLoadErr;   // 加载失败原因（dll 缺失时 QSKIP 说明用）

private slots:
    void initTestCase() {
        if (!CReviewSdkClient::Get()->EnsureLoaded(&m_sLoadErr)) {
            qInfo("ReviewSdk.dll 不可用，跳过：%s", qPrintable(m_sLoadErr));
        }
    }

    void loadAndMeta() {
        if (!CReviewSdkClient::Get()->IsLoaded()) {
            QSKIP("dll 未部署");
        }
        QVERIFY(CReviewSdkClient::Get()->Version().startsWith(QStringLiteral("v")));
        const QStringList aLangs = CReviewSdkClient::Get()->SupportedLanguages();
        QVERIFY(aLangs.contains(QStringLiteral("cpp")));
        QVERIFY(aLangs.contains(QStringLiteral("python")));
        QVERIFY(aLangs.size() >= 10);
        QCOMPARE(CReviewSdkClient::Get()->LanguageForFile(QStringLiteral("C:/x/main.CPP")),
                 QStringLiteral("cpp"));
        QCOMPARE(CReviewSdkClient::Get()->LanguageForFile(QStringLiteral("C:/x/a.py")),
                 QStringLiteral("python"));
        QVERIFY(CReviewSdkClient::Get()->LanguageForFile(QStringLiteral("C:/x/a.json"))
                    .isEmpty());
        const QStringList aRules = CReviewSdkClient::Get()->RulesForLanguage(QStringLiteral("cpp"));
        QVERIFY(aRules.size() >= 2);   // cpp 规则 + 跨语言三件套（官方 smoke 口径）
        QVERIFY(CReviewSdkClient::Get()->RulesForLanguage(QStringLiteral("")).isEmpty());
    }

    void reviewContentWithMock() {
        if (!CReviewSdkClient::Get()->IsLoaded()) {
            QSKIP("dll 未部署");
        }
        const int iBefore = s_iMockCalls;
        QString sFindings, sErr;
        const int iRc = CReviewSdkClient::Get()->ReviewContent(
            QStringLiteral("cpp"), QStringLiteral("src/demo.cpp"),
            QStringLiteral("void f() { int* p = nullptr; *p = 1; }"),
            &MockLlmOk, nullptr, sFindings, sErr);
        QCOMPARE(iRc, AUTOCODE_LCR_OK);
        QVERIFY(sErr.isEmpty());
        QCOMPARE(s_iMockCalls, iBefore + 1);   // 恰好调用一次（官方契约）
        QVERIFY(sFindings.contains(QStringLiteral("空指针解引用")));
        QVERIFY(sFindings.contains(QStringLiteral("demo.cpp")));
    }

    void reviewContentLlmFailure() {
        if (!CReviewSdkClient::Get()->IsLoaded()) {
            QSKIP("dll 未部署");
        }
        QString sFindings, sErr;
        const int iRc = CReviewSdkClient::Get()->ReviewContent(
            QStringLiteral("cpp"), QStringLiteral("src/demo.cpp"),
            QStringLiteral("int main(){return 0;}"),
            &MockLlmFail, nullptr, sFindings, sErr);
        QCOMPARE(iRc, AUTOCODE_LCR_ERR_LLM);
        QVERIFY(sErr.contains(QStringLiteral("mock 模型故障")));
    }

    void reviewContentUnknownLanguage() {
        if (!CReviewSdkClient::Get()->IsLoaded()) {
            QSKIP("dll 未部署");
        }
        QString sFindings, sErr;
        const int iRc = CReviewSdkClient::Get()->ReviewContent(
            QStringLiteral("cobol"), QStringLiteral("src/a.cbl"),
            QStringLiteral("DISPLAY 'X'"), &MockLlmOk, nullptr, sFindings, sErr);
        QCOMPARE(iRc, AUTOCODE_LCR_ERR_NO_RULES);
    }

    void relativeToRoot() {
        const QString sRoot = QStringLiteral("E:/proj/demo");
        QCOMPARE(CReviewSdkClient::RelativeToRoot(sRoot, QStringLiteral("E:/proj/demo/src/a.cpp")),
                 QStringLiteral("src/a.cpp"));
        QCOMPARE(CReviewSdkClient::RelativeToRoot(sRoot, QStringLiteral("E:\\proj\\demo\\b.h")),
                 QStringLiteral("b.h"));
        // 根本身/根外 → 回退清理后的绝对路径（不产出空串/错误前缀）。
        QCOMPARE(CReviewSdkClient::RelativeToRoot(sRoot, QStringLiteral("E:/other/c.cpp")),
                 QStringLiteral("E:/other/c.cpp"));
        QCOMPARE(CReviewSdkClient::RelativeToRoot(sRoot, sRoot), sRoot);
    }
};

QTEST_MAIN(TestReviewSdk)
#include "test_reviewsdk.moc"
