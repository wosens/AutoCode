// 单元测试（test_execpanel）：多助手 Tab「点击任务卡死」回归——模拟任务切换路径
// SwitchAssistantArchive → LoadAssistantState → CExecutionPanel::LoadAssistantsFromJson
// 重复切档（点任务）不应：① 死循环挂死；② 泄漏幽灵助手 Tab；③ 崩溃。
#include <QtTest/QtTest>
#include <QApplication>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonObject>
#include "ui/CExecutionPanel.h"

namespace {
QJsonObject MakeAsst(const QString& a_sName, int a_iBlocks) {
    QJsonArray aBlocks;
    for (int i = 0; i < a_iBlocks; ++i) {
        QJsonObject o;
        o["kind"] = (i % 3 == 0) ? "tool_use" : "assistant";
        o["text"] = QStringLiteral("第%1条").arg(i);
        o["tool"] = "Read";
        o["input"] = "{}";
        o["result"] = "ok";
        aBlocks.append(o);
    }
    QJsonObject o;
    o["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    o["name"] = a_sName;
    o["closed"] = false;
    o["blocks"] = aBlocks;
    return o;
}
QJsonArray RoundTrip(const QJsonArray& a_aAssts) {
    // 直接构造等价输入（面板 AssistantsToJson → 再次 Load 即一次切档）
    return a_aAssts;
}
} // namespace

class TestExecPanel : public QObject {
    Q_OBJECT
private slots:
    void taskSwitchLoop();
};

void TestExecPanel::taskSwitchLoop() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 初始载入：两个助手（一个带历史块，一个空），激活第一个。
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 45));   // 45 块 → 跨 3 个分帧
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 8));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(200);   // 分帧重建跑完

    // 关键回归：反复「点任务」= 反复切档（拆除重建 + 载入）。
    // 旧实现：RemoveAsstTab 摘除当前助手页时 Qt 自动把「+」占位页设为当前页 →
    // OnAsstTabCurrentChanged → OnAsstAddClicked 在拆除循环遍历 m_aAssts 期间
    // append 修改容器（迭代器失效 UB）且清一个生一个 → 死循环卡死；载入路径走
    // ResetAssistants 还会每切一次多出一个空「助手1」。
    const int iTabs0 = pTabs->count();
    for (int i = 0; i < 25; ++i) {
        panel.LoadAssistantsFromJson(RoundTrip(aAssts), QString());
        QTest::qWait(30);
    }
    // 断言：Tab 数不随切档次数增长（无幽灵页累积）。
    QCOMPARE(pTabs->count(), iTabs0);
    // 断言：助手数量保持 2（不含内部复位的默认助手）。
    QCOMPARE(panel.AsstCount(), 2);
    // 断言：序列化往返仍为 2 个助手。
    QCOMPARE(panel.AssistantsToJson().size(), 2);
    panel.hide();
}

QTEST_MAIN(TestExecPanel)
#include "test_execpanel.moc"
