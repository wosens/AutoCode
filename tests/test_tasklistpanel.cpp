// tests/test_tasklistpanel.cpp —— CTaskListPanel 左栏两表切换口径回归（2026.09.10 v2.2.153）。
// 用户需求口径：①再次执行任务 = 直接切「当前任务」页选中该任务（选中信号 → 主窗口
//   OnTaskSelected → SetCurrentTask → 自动选中任务日志，不再停助手日志）；②任务终态 =
//   切「历史任务」页选中刚结束的任务（SelectHistoryTask，与 SelectTask 对称）。
// 本文件锁面板层三件事：切页、发选中信号、两表互斥；主窗口侧链路由
// test_execpanel::setCurrentTaskSelectsLogPage 锁终点（载任务=日志页）。
#include <QtTest/QtTest>
#include <QTabWidget>
#include <QListWidget>
#include <QTreeWidget>
#include "ui/CTaskListPanel.h"
#include "core/CTaskInfo.h"

class TestTaskListPanel : public QObject {
    Q_OBJECT

private slots:
    // 再次执行/启动：SelectTask 切「当前任务」页 + 选中 + 发 sigTaskSelected。
    void selectTaskSwitchesToCurrentTab();
    // 任务终态：SelectHistoryTask 切「历史任务」页 + 选中 + 发信号 + 清当前表互斥选中。
    void selectHistoryTaskSwitchesTab();
    // 终态链模拟：任务从当前表迁出 → 历史表重建出新副本（DB 副本与活跃副本非同对象）
    //   → SelectHistoryTask 选中新副本（OnTaskFinished 实际时序）。
    void finishFlowSelectsHistoryCopy();

private:
    QTabWidget* TabsOf(CTaskListPanel& a_p) { return a_p.findChild<QTabWidget*>(); }
    QListWidget* CurListOf(CTaskListPanel& a_p) { return a_p.findChild<QListWidget*>(); }
    QTreeWidget* HisTreeOf(CTaskListPanel& a_p) { return a_p.findChild<QTreeWidget*>(); }
};

void TestTaskListPanel::selectTaskSwitchesToCurrentTab() {
    CTaskListPanel panel;
    panel.resize(600, 800);
    panel.show();
    QTabWidget* pTabs = TabsOf(panel);
    QListWidget* pList = CurListOf(panel);
    QTreeWidget* pTree = HisTreeOf(panel);
    QVERIFY(pTabs != nullptr && pList != nullptr && pTree != nullptr);

    CTaskInfo cur;
    cur.SetId(QStringLiteral("sel-c-1"));
    cur.SetName(QStringLiteral("当前任务1"));
    cur.SetStatus(ETaskStatus::Pending);   // 非终态 → 当前表
    CTaskInfo his;
    his.SetId(QStringLiteral("sel-h-1"));
    his.SetName(QStringLiteral("历史任务1"));
    his.SetStatus(ETaskStatus::Done);      // 终态 → 历史表
    QList<CTaskInfo*> aCur{&cur};
    QList<CTaskInfo*> aHis{&his};
    panel.SetTasks(aCur);
    panel.SetHistoryTasks(aHis);

    // 前置：模拟用户正停在「历史任务」页（再次执行前通常刚在看历史）。
    pTabs->setCurrentWidget(pTree);
    QCOMPARE(pTabs->currentWidget(), static_cast<QWidget*>(pTree));

    CTaskInfo* pGot = nullptr;
    connect(&panel, &CTaskListPanel::sigTaskSelected,
            &panel, [&pGot](CTaskInfo* p) { pGot = p; });

    panel.SelectTask(&cur);
    // 切回「当前任务」页 + 选中该行 + 发过信号（主窗口链：信号 → OnTaskSelected → 任务日志）。
    QCOMPARE(pTabs->currentWidget(), static_cast<QWidget*>(pList));
    QCOMPARE(pGot, &cur);
    QCOMPARE(panel.CurrentTask(), &cur);
    QVERIFY(pList->currentItem() != nullptr);
    panel.hide();
}

void TestTaskListPanel::selectHistoryTaskSwitchesTab() {
    CTaskListPanel panel;
    panel.resize(600, 800);
    panel.show();
    QTabWidget* pTabs = TabsOf(panel);
    QListWidget* pList = CurListOf(panel);
    QTreeWidget* pTree = HisTreeOf(panel);
    QVERIFY(pTabs != nullptr && pList != nullptr && pTree != nullptr);

    CTaskInfo cur;
    cur.SetId(QStringLiteral("his-c-1"));
    cur.SetName(QStringLiteral("当前任务1"));
    cur.SetStatus(ETaskStatus::Pending);
    CTaskInfo his;
    his.SetId(QStringLiteral("his-h-1"));
    his.SetName(QStringLiteral("历史任务1"));
    his.SetStatus(ETaskStatus::Done);
    QList<CTaskInfo*> aCur{&cur};
    QList<CTaskInfo*> aHis{&his};
    panel.SetTasks(aCur);
    panel.SetHistoryTasks(aHis);

    // 前置：任务运行期视角，停在「当前任务」页并选中运行中任务。
    panel.SelectTask(&cur);
    QCOMPARE(pTabs->currentWidget(), static_cast<QWidget*>(pList));

    CTaskInfo* pGot = nullptr;
    connect(&panel, &CTaskListPanel::sigTaskSelected,
            &panel, [&pGot](CTaskInfo* p) { pGot = p; });

    // 任务终态：SelectHistoryTask 应切「历史任务」页并选中刚结束的任务。
    panel.SelectHistoryTask(&his);
    QCOMPARE(pTabs->currentWidget(), static_cast<QWidget*>(pTree));
    QCOMPARE(pGot, &his);
    QCOMPARE(panel.CurrentTask(), &his);
    QVERIFY(pTree->currentItem() != nullptr);
    // 两表互斥：当前表程序性选中被清（否则同一任务双表同时高亮）。
    QVERIFY(pList->currentItem() == nullptr);
    panel.hide();
}

void TestTaskListPanel::finishFlowSelectsHistoryCopy() {
    CTaskListPanel panel;
    panel.resize(600, 800);
    panel.show();
    QTabWidget* pTabs = TabsOf(panel);
    QListWidget* pList = CurListOf(panel);
    QTreeWidget* pTree = HisTreeOf(panel);
    QVERIFY(pTabs != nullptr && pList != nullptr && pTree != nullptr);

    // 运行期：任务在当前表（非终态）。
    CTaskInfo running;
    running.SetId(QStringLiteral("fin-1"));
    running.SetName(QStringLiteral("终态任务"));
    running.SetStatus(ETaskStatus::Running);
    QList<CTaskInfo*> aRun{&running};
    panel.SetTasks(aRun);
    panel.SelectTask(&running);   // 启动时选中（真实链路）

    // 终态（OnTaskFinished 时序）：活跃副本迁出当前表 → ReloadHistory 从 DB 重建出
    // 全新副本对象（与活跃副本非同指针）→ SelectHistoryTask 选中新副本。
    CTaskInfo hisCopy;
    hisCopy.SetId(QStringLiteral("fin-1"));         // 同 id 不同对象（DB 副本）
    hisCopy.SetName(QStringLiteral("终态任务"));
    hisCopy.SetStatus(ETaskStatus::Done);
    QList<CTaskInfo*> aEmpty;
    panel.SetTasks(aEmpty);                  // 当前表少一行
    panel.SetHistoryTasks({&hisCopy});       // 历史表多出该终态任务

    CTaskInfo* pGot = nullptr;
    connect(&panel, &CTaskListPanel::sigTaskSelected,
            &panel, [&pGot](CTaskInfo* p) { pGot = p; });

    panel.SelectHistoryTask(&hisCopy);
    QCOMPARE(pTabs->currentWidget(), static_cast<QWidget*>(pTree));   // 切历史页
    QCOMPARE(pGot, &hisCopy);                                         // 选中刚结束任务
    QCOMPARE(panel.CurrentTask(), &hisCopy);
    panel.hide();
}

QTEST_MAIN(TestTaskListPanel)
#include "test_tasklistpanel.moc"
