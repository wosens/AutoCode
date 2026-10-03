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
#include <QTemporaryDir>
#include <QMenu>        // 2026.09.26 打开方式 .bat 精简口径回归
#include <QDialog>      // 2026.10.02 属性兑底对话框回归
#include <QLabel>
#include <QCheckBox>
#include <QMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>   // 2026.09.23 刷新回归：临时事件文件残留断言
#include <QProcess>
#include <QGuiApplication>
#include <QClipboard>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTreeView>        // 2026.09.22 展开态回归
#include <QAbstractItemModel>
#include "ui/CTaskListPanel.h"
#include "ui/CProjectExplorerPanel.h"
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
    // 2026.09.17 打开方式 shell 同源枚举 + 程序图标回归（定义见文件后段）。
    void testOpenWithShellEnumeration();
    void testOpenWithIconsAttached();
    // 2026.09.26 v3.1.244 回归：.bat/.cmd「打开方式」精简口径（首项=CMD 运行哨兵、
    //   条目数收敛不爆全量；普通扩展名不受影响、无哨兵）。
    void openWithBatCurated();
    // 2026.09.19 剪切/复制/粘贴与 Windows 资源管理器联动：CF_HDROP+Preferred
    //   DropEffect 双向跨进程（PowerShell 与资源管理器用同一套剪贴板 API）。
    void explorerClipboardInterop();
    // 2026.09.21 多根节点（关联项目）：添加/去重/移动/移除 + 顶层行数与模型同步。
    void explorerMultiRootAddMoveRemove();
    // 2026.09.22 展开态回归：首次展开未列出的子目录（源 forceSort+delayedSort →
    //   layoutChanged）与监视目录新增文件，均不得折叠已展开节点（旧代码把
    //   layoutChanged 翻译成整体 reset → 全树折叠，点击目录“打不开还收起全部”）。
    void explorerExpandSurvivesFirstListing();
    // 2026.09.21 根顺序持久化：<工程>/.autodev/explorer.json 随工程走、坏档过滤、
    //   换工程不携带；重开面板还原顺序。
    void explorerRootsPersist();
    // 2026.09.22 展开态自愈：目录被删建/重命名后新节点行按记住的路径重新展开
    //   （用户反馈：文件夹总会莫名其妙的自动全部折叠）。
    void explorerExpandSurvivesChurn();
    // 2026.09.21 复制相对路径：根节点→直接复制名称；普通文件→相对所在根
    //   （关联根内相对关联根）。
    void explorerCopyRelRootName();
    // 2026.09.23 右键「刷新」：目标推导（选中文件→父目录/目录→自身/无选中→全部根，
    //   已展开子目录并入）+ 事件补齐端到端（外部建文件后 RefreshSelected 树内可见、
    //   临时事件文件不残留）。
    void explorerRefreshTargets();
    // 2026.09.26 v3.1.247 回归：新建文件夹（默认名/重名 (n)/目标推导/空目标回根/无效目标空串）。
    void explorerNewFolder();
    // 2026.10.02 属性兑底对话框：文件/目录两形态字段填充（名称/类型/位置/大小/
    //   时间/只读隐藏勾选）；SHObjectProperties 主路径为 shell 弹窗不可自动化。
    void explorerPropertiesDialog();
    void explorerRefreshNudge();
    // 2026.09.23 显示隐藏清单配置化：默认隐藏 build；SetHiddenDirs 立即对已加载树
    //   重隐/重显（清空=全可见）。
    void explorerHiddenDirsConfigurable();

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

// 2026.09.17 打开方式菜单 shell 同源枚举回归（实害：HKCR-only 旧实现只出
//   QtCreator，VS/VSCode/Qoder/Trae/记事本/写字板全丢）：SHAssocEnumHandlers
//   至少能枚出一个处理器且带本地化 UIName；.txt 推荐口径下记事本类处理器
//   在任何 Windows 上都应存在（跨机器可移植断言）。
void TestTaskListPanel::testOpenWithShellEnumeration() {
    QString sFirst;
    const int nRecommended = CProjectExplorerPanel::ProbeAssocHandlers(
        QStringLiteral(".txt"), 0, &sFirst);
    QVERIFY2(nRecommended >= 1,
             "SHAssocEnumHandlers(.txt, RECOMMENDED) 应至少返回一个处理器");
    QVERIFY2(!sFirst.trimmed().isEmpty(), "首个处理器应有非空 UIName");
    const int nAll = CProjectExplorerPanel::ProbeAssocHandlers(
        QStringLiteral(".txt"), 1, nullptr);
    QVERIFY2(nAll >= nRecommended,
             "全量口径枚举数不应少于推荐口径");
}

// 2026.09.17 打开方式菜单项程序图标：GetIconLocation → 系统 shell 图标链路，
// 至少一项带非空图标（.cpp 的 VS/记事本类处理器都有 exe 图标；跨机器可移植）。
void TestTaskListPanel::testOpenWithIconsAttached() {
    QString sFirst;
    int nIconified = 0;
    const int n = CProjectExplorerPanel::ProbeAssocHandlers(
        QStringLiteral(".cpp"), 0, &sFirst, &nIconified);
    QVERIFY2(n >= 1, "枚举应产出至少一个处理器");
    QVERIFY2(nIconified >= 1,
             "至少一项应携带程序真实图标（GetIconLocation→shell 图标链路）");
}

// 2026.09.26 v3.1.244 .bat/.cmd 打开方式精简口径回归：
//   ① .bat/.cmd 菜单首项=「用 CMD 运行」（data 为负哨兵，非 COM 索引）；
//   ② 条目数收敛（全量 NONE 口径不再兜底——旧版把全机自荐处理器堆进来几十项）；
//   ③ 普通扩展名（.txt）不受影响：无哨兵、仍有处理器条目。
void TestTaskListPanel::openWithBatCurated() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sBat = oTmp.path() + QStringLiteral("/probe.bat");
    const QString sCmd = oTmp.path() + QStringLiteral("/probe.cmd");
    QFile fBat(sBat);
    QVERIFY(fBat.open(QIODevice::WriteOnly));
    fBat.write("@echo off\r\n");
    fBat.close();
    QFile fCmd(sCmd);
    QVERIFY(fCmd.open(QIODevice::WriteOnly));
    fCmd.write("@echo off\r\n");
    fCmd.close();

    CProjectExplorerPanel panel;
    for (const QString& sFile : { sBat, sCmd }) {
        QMenu menu;
        panel.BuildOpenWithMenu(&menu, sFile);
        const QList<QAction*> aActs = menu.actions();
        int nItems = 0;
        int iFirstData = 0;
        QString sFirstText;
        for (QAction* pAct : aActs) {
            if (pAct->isSeparator()) { continue; }
            if (nItems == 0) {
                sFirstText = pAct->text();
                iFirstData = pAct->data().userType() == QMetaType::Int
                                 ? pAct->data().toInt() : 0;
            }
            ++nItems;
        }
        QVERIFY2(nItems >= 1, "bat/cmd 菜单至少有 CMD 项");
        QVERIFY2(sFirstText == QStringLiteral("用 CMD 运行"),
                 qPrintable(QStringLiteral("首项应为 CMD 运行，实际=") + sFirstText));
        QVERIFY2(iFirstData < 0, "CMD 项 data 应为负哨兵（非 COM 索引）");
        QVERIFY2(nItems <= 14,
                 qPrintable(QStringLiteral("条目数应收敛（≤14），实际=")
                            + QString::number(nItems)));
        menu.clear();   // 释放 COM 处理器（析构前清干净）
    }

    // 普通扩展名不受影响：无负哨兵、仍有条目
    const QString sTxt = oTmp.path() + QStringLiteral("/probe.txt");
    QFile fTxt(sTxt);
    QVERIFY(fTxt.open(QIODevice::WriteOnly));
    fTxt.write("hello");
    fTxt.close();
    {
        QMenu menu;
        panel.BuildOpenWithMenu(&menu, sTxt);
        const QList<QAction*> aActs = menu.actions();
        int nItems = 0;
        bool bHasSentinel = false;
        for (QAction* pAct : aActs) {
            if (pAct->isSeparator()) { continue; }
            ++nItems;
            if (pAct->data().userType() == QMetaType::Int
                    && pAct->data().toInt() < 0) {
                bHasSentinel = true;
            }
        }
        QVERIFY2(!bHasSentinel, ".txt 不应出现 CMD 哨兵项");
        QVERIFY2(nItems >= 1, ".txt 仍应有打开方式条目");
        menu.clear();
    }
}


// 2026.09.26 v3.1.247 新建文件夹回归：
//   ① 无选中（目标空）→ 工程根下创建「新建文件夹」；再次创建 → 「新建文件夹 (2)」；
//   ② 显式目标目录 → 该目录内创建；选中文件场景（目标=父目录）由调用方推导，
//      公开接口直接验显式目录；③ 不存在的目标目录 → 空串且不崩。
void TestTaskListPanel::explorerNewFolder() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sRoot = oTmp.path();
    QDir().mkpath(sRoot + QStringLiteral("/sub"));

    CProjectExplorerPanel panel;
    panel.SetRoot(sRoot);
    QTest::qWait(200);

    // ① 无选中 → 工程根；重名自动 (2)
    const QString sN1 = panel.CreateNewFolderAt(QString());
    QVERIFY(!sN1.isEmpty());
    QVERIFY(QDir(sN1).exists());
    QCOMPARE(QDir(sN1).dirName(), QStringLiteral("新建文件夹"));
    const QString sN2 = panel.CreateNewFolderAt(QString());
    QVERIFY(!sN2.isEmpty());
    QVERIFY(sN2 != sN1);
    QCOMPARE(QDir(sN2).dirName(), QStringLiteral("新建文件夹 (2)"));

    // ② 显式子目录 → 子目录内创建；同名计数独立
    const QString sN3 = panel.CreateNewFolderAt(sRoot + QStringLiteral("/sub"));
    QVERIFY(!sN3.isEmpty());
    QVERIFY(QDir(sN3).exists());
    QCOMPARE(QDir(sN3).dirName(), QStringLiteral("新建文件夹"));
    QVERIFY(sN3.startsWith(sRoot + QStringLiteral("/sub")));

    // ③ 无效目标 → 空串
    QVERIFY(panel.CreateNewFolderAt(sRoot + QStringLiteral("/不存在")).isEmpty());
}

// 2026.09.19 剪切/复制/粘贴与 Windows 资源管理器联动（CF_HDROP 双向跨进程互操作）。
//   PowerShell 的 Get-Clipboard -Format FileDropList / Set-Clipboard -Path 与资源管理器
//   用同一套 shell 剪贴板机制：能用 PowerShell 读写，资源管理器同样能收到/发出。
void TestTaskListPanel::explorerClipboardInterop() {
#ifdef Q_OS_WIN
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sRoot = oTmp.path();
    QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/src")));
    QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/dst")));
    const QString sA = sRoot + QStringLiteral("/src/a.txt");
    const QString sB = sRoot + QStringLiteral("/src/b.log");
    {
        QFile fA(sA); QVERIFY(fA.open(QIODevice::WriteOnly)); fA.write("aaa");
        QFile fB(sB); QVERIFY(fB.open(QIODevice::WriteOnly)); fB.write("bbb");
    }

    // —— 1. 复制（Copy 效果）写入 → 本进程读回：多文件、非剪切。（剪贴板偶被其他
    //   进程短暂占用，整体写读回合重试三次兑底——ctest 连跑下曾现一次冲突。） ——
    bool bWrote = false;
    for (int iTry = 0; iTry < 3 && !bWrote; ++iTry) {
        bWrote = CProjectExplorerPanel::WriteFileClipboard({ sA, sB }, /*a_bCut=*/false);
        if (!bWrote) { QTest::qWait(200); }
    }
    QVERIFY2(bWrote, "剪贴板写入重试后仍失败（被其他进程持续占用）");
    bool bCut = true;
    QStringList aBack = CProjectExplorerPanel::ReadFileClipboard(&bCut);
    QCOMPARE(aBack.size(), 2);
    QVERIFY(aBack.contains(QDir::toNativeSeparators(sA))
            || aBack.contains(QDir::fromNativeSeparators(sA)));
    QVERIFY(!bCut);

    // —— 2. 跨进程可见性：PowerShell 读 FileDropList（资源管理器同链路）。 ——
    QProcess stPs;
    stPs.start(QStringLiteral("powershell"),
               { QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-Command"),
                 QStringLiteral("(Get-Clipboard -Format FileDropList) -join '|'") });
    QVERIFY2(stPs.waitForStarted(10000), "PowerShell 启动失败");
    QVERIFY(stPs.waitForFinished(30000));
    const QString sPs = QString::fromUtf8(stPs.readAllStandardOutput().trimmed());
    QVERIFY2(sPs.contains(QStringLiteral("a.txt"), Qt::CaseInsensitive)
             && sPs.contains(QStringLiteral("b.log"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("PowerShell 未读到 CF_HDROP：%1").arg(sPs)));

    // —— 3. 剪切（Move 效果）写入 → 读回应报剪切。 ——
    QVERIFY(CProjectExplorerPanel::WriteFileClipboard({ sA }, /*a_bCut=*/true));
    bCut = false;
    aBack = CProjectExplorerPanel::ReadFileClipboard(&bCut);
    QCOMPARE(aBack.size(), 1);
    QVERIFY(bCut);

    // —— 4. 反向：PowerShell Set-Clipboard -Path（资源管理器复制的同款格式）→ 本进程读。 ——
    {
        QProcess stSet;
        stSet.start(QStringLiteral("powershell"),
                   { QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                     QStringLiteral("-Command"),
                     QStringLiteral("Set-Clipboard -LiteralPath '%1'; 'ok'")
                         .arg(QDir::toNativeSeparators(sB)) });
        QVERIFY2(stSet.waitForStarted(10000), "PowerShell 启动失败");
        QVERIFY(stSet.waitForFinished(30000));
        bCut = true;
        aBack = CProjectExplorerPanel::ReadFileClipboard(&bCut);
        QVERIFY2(aBack.size() == 1
                 && (aBack.first().endsWith(QStringLiteral("b.log"), Qt::CaseInsensitive)),
                 qPrintable(QStringLiteral("未读到 PowerShell 写入的文件：%1")
                                .arg(aBack.join(QStringLiteral("|")))));
        QVERIFY(!bCut);   // Set-Clipboard 为复制语义（无 Move 效果标记）
    }

    // —— 5. 落盘语义（TransferItem，与拖拽同口径）： ——
    //   复制：源保留；同名自动 " (2)"；目录禁入自身子树；跨目录同盘移动。
    const QString sDstDir = sRoot + QStringLiteral("/dst");
    QCOMPARE(CProjectExplorerPanel::TransferItem(sA, sDstDir, /*move=*/false),
             CProjectExplorerPanel::XferOk);
    QVERIFY(QFile::exists(sDstDir + QStringLiteral("/a.txt")));
    QVERIFY(QFile::exists(sA));   // 复制源保留
    // 同名再粘 → " (2)"。
    QCOMPARE(CProjectExplorerPanel::TransferItem(sA, sDstDir, /*move=*/false),
             CProjectExplorerPanel::XferOk);
    QVERIFY(QFile::exists(sDstDir + QStringLiteral("/a (2).txt")));
    // 目录移入自身子树 → 失败。
    QVERIFY(QDir().mkpath(sRoot + QStringLiteral("/sub/inner")));
    QCOMPARE(CProjectExplorerPanel::TransferItem(sRoot + QStringLiteral("/sub"),
                                                 sRoot + QStringLiteral("/sub/inner"),
                                                 /*move=*/true),
             CProjectExplorerPanel::XferFail);
    // 同盘移动：源消失目标出现。
    QCOMPARE(CProjectExplorerPanel::TransferItem(sB, sDstDir, /*move=*/true),
             CProjectExplorerPanel::XferOk);
    QVERIFY(!QFile::exists(sB));
    QVERIFY(QFile::exists(sDstDir + QStringLiteral("/b.log")));

    // 收尾：清空测试写入的剪贴板（勿把测试文件清单留给用户）。
    QGuiApplication::clipboard()->clear();
#endif // Q_OS_WIN
}

// 2026.09.21 多根节点（关联项目）：添加/去重/移动/移除 + 顶层行数与模型同步。
void TestTaskListPanel::explorerMultiRootAddMoveRemove() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj  = oTmp.path() + QStringLiteral("/proj");
    const QString sOther = oTmp.path() + QStringLiteral("/other");
    const QString sThird = oTmp.path() + QStringLiteral("/third");
    for (const QString& s : {sProj, sOther, sThird}) {
        QVERIFY(QDir().mkpath(s));
    }
    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    qApp->processEvents();
    panel.SetRoot(sProj);
    QCOMPARE(panel.RootCount(), 1);
    QCOMPARE(panel.RootPathAt(0), QDir::cleanPath(sProj));
    // 拒绝：不存在的目录 / 与工程根重复。
    QVERIFY(!panel.AddExtraRoot(oTmp.path() + QStringLiteral("/nope")));
    QVERIFY(!panel.AddExtraRoot(sProj));
    // 添加两个关联根：选中落到新根行。
    QVERIFY(panel.AddExtraRoot(sOther));
    QCOMPARE(panel.RootCount(), 2);
    QCOMPARE(panel.RootPathAt(1), QDir::cleanPath(sOther));
    QVERIFY(panel.ExplorerModel() != nullptr);
    QCOMPARE(panel.ExplorerModel()->rowCount(), 2);
    QVERIFY(panel.AddExtraRoot(sThird));
    QCOMPARE(panel.RootCount(), 3);
    QCOMPARE(panel.SelectedRootRow(), 2);
    // 重复添加再拒；嵌套根（父/子方向）同样拒绝（QModelIndex 等值不含 parent，
    //   嵌套根会使同一源节点双索引串位 → 堆损坏）。
    QVERIFY(!panel.AddExtraRoot(sOther));
    QVERIFY(!panel.AddExtraRoot(sProj + QStringLiteral("/sub")));
    QVERIFY(QDir().mkpath(sProj + QStringLiteral("/sub")));
    QVERIFY(!panel.AddExtraRoot(sProj + QStringLiteral("/sub")));
    QVERIFY(!panel.AddExtraRoot(oTmp.path()));   // oTmp 包含工程根（父方向嵌套）
    // 上移两步置顶（展开态保留由 beginMoveRows 保证；这里验证顺序与选中跟随）。
    QVERIFY(panel.MoveSelectedRoot(-1));
    QCOMPARE(panel.SelectedRootRow(), 1);
    QVERIFY(panel.MoveSelectedRoot(-1));
    QCOMPARE(panel.SelectedRootRow(), 0);
    QCOMPARE(panel.RootPathAt(0), QDir::cleanPath(sThird));
    QCOMPARE(panel.RootPathAt(1), QDir::cleanPath(sProj));
    QCOMPARE(panel.RootPathAt(2), QDir::cleanPath(sOther));
    QVERIFY(!panel.MoveSelectedRoot(-1));   // 已在顶
    // 下移两步回底。
    QVERIFY(panel.MoveSelectedRoot(+1));
    QVERIFY(panel.MoveSelectedRoot(+1));
    QCOMPARE(panel.SelectedRootRow(), 2);
    QCOMPARE(panel.RootPaths(),
             QStringList({ QDir::cleanPath(sProj), QDir::cleanPath(sOther),
                           QDir::cleanPath(sThird) }));
    // 未选根行时移动无效：选一个子节点（工程根下建文件）。
    QVERIFY(QFile(sProj + QStringLiteral("/f.txt")).open(QIODevice::WriteOnly));
    QTRY_VERIFY(panel.SelectPath(sProj + QStringLiteral("/f.txt")));
    QCOMPARE(panel.SelectedRootRow(), -1);
    QVERIFY(!panel.MoveSelectedRoot(-1));
    // 移除：仅额外根可移，顺序保持；工程根不可移。
    QVERIFY(panel.SelectRoot(1));
    QVERIFY(panel.RemoveExtraRoot(sOther));
    QCOMPARE(panel.RootCount(), 2);
    QCOMPARE(panel.RootPathAt(1), QDir::cleanPath(sThird));
    QVERIFY(!panel.RemoveExtraRoot(sProj));
}

// 2026.09.22 展开态回归：首次展开未列出的子目录时，QFileSystemModel 会走
//   directoryChanged→forceSort+delayedSort→layoutChanged（Qt 6.8 源码实证）；
//   旧代理把它翻译成整体 reset → QTreeView 清空 expandedIndexes → 全树折叠、
//   被点的目录也弹回（用户实害“点子文件夹不展开反而全收起”）。
//   监视目录新增文件同样触发 delayedSort，一并覆盖。修复后 layoutChanged 按
//   持久索引重映射转发，展开/选中不丢。
void TestTaskListPanel::explorerExpandSurvivesFirstListing() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    const QString sSub1 = sProj + QStringLiteral("/sub1");
    const QString sSub1a = sSub1 + QStringLiteral("/sub1a");
    QVERIFY(QDir().mkpath(sSub1a));   // proj/sub1/sub1a 三级
    QVERIFY(QFile(sProj + QStringLiteral("/a.txt")).open(QIODevice::WriteOnly));
    QVERIFY(QFile(sSub1 + QStringLiteral("/b.txt")).open(QIODevice::WriteOnly));
    QVERIFY(QFile(sSub1a + QStringLiteral("/c.txt")).open(QIODevice::WriteOnly));

    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    qApp->processEvents();
    panel.SetRoot(sProj);

    QTreeView* pTree = panel.findChild<QTreeView*>();
    QVERIFY(pTree != nullptr);
    QAbstractItemModel* pM = pTree->model();
    const QModelIndex stRootRow = pM->index(0, 0);
    QVERIFY(stRootRow.isValid());
    QVERIFY(pTree->isExpanded(stRootRow));   // SetRoot 已展开工程根行

    // 按名找子行。
    auto fnChildByName = [&](const QModelIndex& stParent,
                             const QString& sName) -> QModelIndex {
        for (int i = 0; i < pM->rowCount(stParent); ++i) {
            const QModelIndex c = pM->index(i, 0, stParent);
            if (c.data().toString() == sName) {
                return c;
            }
        }
        return QModelIndex();
    };
    // 持久索引：源重排后代 changePersistentIndex 会原地更新它（与视图展开集同机制），
    //   断言不受行号移动影响。根的子级异步列出（gatherer 线程），QTRY 等行到达。
    QModelIndex stSub1Raw;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stSub1Raw = fnChildByName(stRootRow, QStringLiteral("sub1"))).isValid(),
        8000);
    QPersistentModelIndex stSub1(stSub1Raw);
    QVERIFY(stSub1.isValid());

    // ① 首次展开 sub1（未列出过的目录）：listing 异步回来 → forceSort →
    //   layoutChanged。旧代码此处 reset → sub1 弹回、根也收起。
    pTree->expand(stSub1);
    QTest::qWait(400);
    QVERIFY(pTree->isExpanded(stRootRow));
    QVERIFY(pTree->isExpanded(stSub1));

    // ② 再展开第二层 sub1a（又一次首列→layoutChanged）。
    QPersistentModelIndex stSub1a(
        fnChildByName(stSub1, QStringLiteral("sub1a")));
    QVERIFY(stSub1a.isValid());
    pTree->expand(stSub1a);
    QTest::qWait(400);
    QVERIFY(pTree->isExpanded(stRootRow));
    QVERIFY(pTree->isExpanded(stSub1));
    QVERIFY(pTree->isExpanded(stSub1a));

    // ③ 监视目录新增文件（无需点击）：已展开层级不得折叠。
    QVERIFY(QFile(sSub1 + QStringLiteral("/new1.txt")).open(QIODevice::WriteOnly));
    QTRY_VERIFY_WITH_TIMEOUT(
        fnChildByName(stSub1, QStringLiteral("new1.txt")).isValid(), 8000);
    QTest::qWait(300);   // 文件到达后 0ms 延迟排序落地
    QVERIFY(pTree->isExpanded(stRootRow));
    QVERIFY(pTree->isExpanded(stSub1));
    QVERIFY(pTree->isExpanded(stSub1a));
}

// 2026.09.22 展开态自愈：Agent 任务频繁写盘，目录可能被外部删建/重命名——新节点的
//   展开态按 Qt 语义必然丢失（QPersistentModelIndex 不跨删建存活）。面板应按记住的
//   路径在新行出现后重新展开；用户主动收起的目录则不得被强行重新展开。
void TestTaskListPanel::explorerExpandSurvivesChurn() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    const QString sSub1 = sProj + QStringLiteral("/sub1");
    const QString sSub1a = sSub1 + QStringLiteral("/sub1a");
    QVERIFY(QDir().mkpath(sSub1a));
    QVERIFY(QFile(sSub1a + QStringLiteral("/c.txt")).open(QIODevice::WriteOnly));

    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    qApp->processEvents();
    panel.SetRoot(sProj);

    QTreeView* pTree = panel.findChild<QTreeView*>();
    QVERIFY(pTree != nullptr);
    QAbstractItemModel* pM = pTree->model();
    const QModelIndex stRootRow = pM->index(0, 0);
    QVERIFY(stRootRow.isValid());
    auto fnChildByName = [&](const QModelIndex& stParent,
                             const QString& sName) -> QModelIndex {
        for (int i = 0; i < pM->rowCount(stParent); ++i) {
            const QModelIndex c = pM->index(i, 0, stParent);
            if (c.data().toString() == sName) {
                return c;
            }
        }
        return QModelIndex();
    };
    QModelIndex stSub1Raw;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stSub1Raw = fnChildByName(stRootRow, QStringLiteral("sub1"))).isValid(),
        8000);
    pTree->expand(stSub1Raw);
    QModelIndex stSub1aRaw;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stSub1aRaw = fnChildByName(stSub1Raw, QStringLiteral("sub1a"))).isValid(),
        8000);
    pTree->expand(stSub1aRaw);
    QTest::qWait(400);   // listing + 延迟排序落地
    QVERIFY(pTree->isExpanded(stSub1Raw));
    QVERIFY(pTree->isExpanded(stSub1aRaw));

    // ① 删建 sub1a（Agent 写盘常态）：新节点行回来后应按记住的路径自动重新展开。
    QVERIFY(QDir(sSub1a).removeRecursively());
    QTest::qWait(600);   // 监视器感知删除（行移除）
    QVERIFY(QDir().mkpath(sSub1a));
    QVERIFY(QFile(sSub1a + QStringLiteral("/d.txt")).open(QIODevice::WriteOnly));
    QModelIndex stAgain;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stAgain = fnChildByName(stSub1Raw, QStringLiteral("sub1a"))).isValid(),
        10000);
    QTRY_VERIFY_WITH_TIMEOUT(pTree->isExpanded(stAgain), 10000);   // 自愈：新行自动重新展开
    QVERIFY(pTree->isExpanded(stSub1Raw));

    // ② 用户主动收起后删建：不得被强行重新展开。
    pTree->collapse(stAgain);
    QTest::qWait(100);
    QVERIFY(QDir(sSub1a).removeRecursively());
    QTest::qWait(600);
    QVERIFY(QDir().mkpath(sSub1a));
    QVERIFY(QFile(sSub1a + QStringLiteral("/e.txt")).open(QIODevice::WriteOnly));
    QModelIndex stAgain2;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stAgain2 = fnChildByName(stSub1Raw, QStringLiteral("sub1a"))).isValid(),
        10000);
    QTest::qWait(600);   // 重放窗口过后仍应保持收起
    QTest::qWait(600);
    QVERIFY(!pTree->isExpanded(stAgain2));
}

// 2026.09.21 根顺序持久化：<工程>/.autodev/explorer.json 随工程走、坏档过滤、
//   换工程不携带；重开面板还原顺序。
void TestTaskListPanel::explorerRootsPersist() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj  = oTmp.path() + QStringLiteral("/proj");
    const QString sOther = oTmp.path() + QStringLiteral("/other");
    QVERIFY(QDir().mkpath(sProj));
    QVERIFY(QDir().mkpath(sOther));
    const QString sJson = sProj + QStringLiteral("/.autodev/explorer.json");
    {
        CProjectExplorerPanel panel;
        panel.SetRoot(sProj);
        QVERIFY(panel.AddExtraRoot(sOther));
        QVERIFY(panel.MoveSelectedRoot(-1));   // other 置顶：[other, proj]
        QVERIFY(QFile::exists(sJson));
    }
    // 重开面板：顺序还原。
    {
        CProjectExplorerPanel panel;
        panel.SetRoot(sProj);
        QCOMPARE(panel.RootCount(), 2);
        QCOMPARE(panel.RootPathAt(0), QDir::cleanPath(sOther));
        QCOMPARE(panel.RootPathAt(1), QDir::cleanPath(sProj));
    }
    // 换工程（无 explorer.json）：单根，不携带旧工程的关联根。
    {
        const QString sProj2 = oTmp.path() + QStringLiteral("/proj2");
        QVERIFY(QDir().mkpath(sProj2));
        CProjectExplorerPanel panel;
        panel.SetRoot(sProj2);
        QCOMPARE(panel.RootCount(), 1);
    }
    // 坏档（含不存在目录）：加载时过滤。
    {
        const QString sProj3 = oTmp.path() + QStringLiteral("/proj3");
        QVERIFY(QDir().mkpath(sProj3));
        QDir().mkpath(sProj3 + QStringLiteral("/.autodev"));
        QFile f(sProj3 + QStringLiteral("/.autodev/explorer.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QJsonObject o;
        o.insert(QStringLiteral("version"), 1);
        o.insert(QStringLiteral("roots"),
                 QJsonArray::fromStringList(
                     { sProj3 + QStringLiteral("/ghost"), sProj3 }));
        f.write(QJsonDocument(o).toJson());
        f.close();
        CProjectExplorerPanel panel;
        panel.SetRoot(sProj3);
        QCOMPARE(panel.RootCount(), 1);
        QCOMPARE(panel.RootPathAt(0), QDir::cleanPath(sProj3));
    }
}

// 2026.09.21 复制相对路径：根节点→直接复制名称；普通文件→相对所在根
//   （关联根内相对关联根）。
void TestTaskListPanel::explorerCopyRelRootName() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    const QString sSub  = sProj + QStringLiteral("/a");
    QVERIFY(QDir().mkpath(sSub));
    {
        QFile f(sSub + QStringLiteral("/b.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    panel.SetRoot(sProj);
    // 根节点：复制相对路径 → 直接是名称（旧版给 "."）。
    QVERIFY(panel.SelectRoot(0));
    panel.CopyRelativePath();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("proj"));
    // 普通文件：相对工程根；SelectPath 首次可能因源模型异步加载未命中，重试。
    QTRY_VERIFY(panel.SelectPath(sSub + QStringLiteral("/b.txt")));
    panel.CopyRelativePath();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("a\\b.txt"));
    // 关联根的文件：相对关联根（不是工程根）。
    const QString sOther = oTmp.path() + QStringLiteral("/other");
    QVERIFY(QDir().mkpath(sOther + QStringLiteral("/deep")));
    {
        QFile f(sOther + QStringLiteral("/deep/c.md"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QVERIFY(panel.AddExtraRoot(sOther));
    QTRY_VERIFY(panel.SelectPath(sOther + QStringLiteral("/deep/c.md")));
    panel.CopyRelativePath();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("deep\\c.md"));
    // 收尾：清剪贴板。
    QGuiApplication::clipboard()->clear();
}

// 2026.09.23 右键「刷新」目标推导：选中文件→父目录、目录→自身、无选中→全部根；
//   记忆中已展开的子目录（watcher 实际监视面）并入目标。
void TestTaskListPanel::explorerRefreshTargets() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    const QString sSub  = sProj + QStringLiteral("/sub");
    const QString sDeep = sSub + QStringLiteral("/deep");
    QVERIFY(QDir().mkpath(sDeep));
    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    panel.SetRoot(sProj);
    // 沿途展开 sProj→sSub→sDeep（展开记忆随之建立）。
    QTRY_VERIFY(panel.SelectPath(sDeep));
    QAbstractItemModel* pM = panel.ExplorerModel();
    QVERIFY(pM != nullptr);

    // ① 选中文件（sDeep 下建文件并选中）→ 父目录 sDeep；不并入其它。
    {
        QFile f(sDeep + QStringLiteral("/f.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QTRY_VERIFY(panel.SelectPath(sDeep + QStringLiteral("/f.txt")));
    QStringList aT = panel.RefreshTargets();
    QVERIFY(aT.contains(QDir::cleanPath(sDeep)));
    QVERIFY(!aT.contains(QDir::cleanPath(sProj)));
    QVERIFY(!aT.contains(QDir::cleanPath(sSub)));

    // ② 选中根行（目录）→ 自身 + 全部已展开子目录（sSub、sDeep）。
    QVERIFY(panel.SelectRoot(0));
    aT = panel.RefreshTargets();
    QVERIFY(aT.contains(QDir::cleanPath(sProj)));
    QVERIFY(aT.contains(QDir::cleanPath(sSub)));
    QVERIFY(aT.contains(QDir::cleanPath(sDeep)));

    // ③ 无选中 → 全部根 + 已展开子目录。
    QTreeView* pTree = panel.findChild<QTreeView*>();
    QVERIFY(pTree != nullptr);
    pTree->setCurrentIndex(QModelIndex());
    aT = panel.RefreshTargets();
    QVERIFY(aT.contains(QDir::cleanPath(sProj)));
    QVERIFY(aT.contains(QDir::cleanPath(sSub)));
    panel.hide();
}

// 2026.09.23 刷新端到端：外部新建文件（watcher 事件可能丢）→ RefreshSelected 强制
//   重列 → 树内可见；且事件临时文件不残留。
// 2026.10.02 v3.1.25X 属性兑底对话框：StartProperties 主路径 SHObjectProperties
//   为 shell 模态弹窗不可自动化，回归锁兑底面 BuildPropertiesDialog——文件/
//   目录两形态的字段填充（名称/类型/位置/大小/修改时间/只读隐藏勾选）与
//   目录递归统计（文件数/字节数）。
void TestTaskListPanel::explorerPropertiesDialog() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sRoot = oTmp.path();
    // 造文件（固定内容 10 字节）与子目录（内含 2 个文件共 20 字节）。
    QVERIFY(QFile(sRoot + QStringLiteral("/a.txt")).open(QIODevice::WriteOnly));
    {
        QFile f(sRoot + QStringLiteral("/a.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("0123456789");
    }
    QDir().mkpath(sRoot + QStringLiteral("/sub"));
    for (const char* sz : {"xx", "yy"}) {
        QFile f(sRoot + QStringLiteral("/sub/") + QString::fromLatin1(sz) + QStringLiteral(".dat"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("0123456789");
    }

    // ① 文件形态：名称/类型/大小/时间/勾选均填充。
    QDialog* pDlg = CProjectExplorerPanel::BuildPropertiesDialog(
        sRoot + QStringLiteral("/a.txt"), nullptr);
    QVERIFY(pDlg != nullptr);
    auto pLblName = pDlg->findChild<QLabel*>(QStringLiteral("propLblName"));
    auto pLblType = pDlg->findChild<QLabel*>(QStringLiteral("propLblType"));
    auto pLblLoc  = pDlg->findChild<QLabel*>(QStringLiteral("propLblLocation"));
    auto pLblSize = pDlg->findChild<QLabel*>(QStringLiteral("propLblSize"));
    auto pLblMod  = pDlg->findChild<QLabel*>(QStringLiteral("propLblModified"));
    auto pChkRO   = pDlg->findChild<QCheckBox*>(QStringLiteral("propChkReadOnly"));
    auto pChkHid  = pDlg->findChild<QCheckBox*>(QStringLiteral("propChkHidden"));
    QVERIFY(pLblName && pLblType && pLblLoc && pLblSize && pLblMod && pChkRO && pChkHid);
    QVERIFY(pLblName->toolTip().contains(QStringLiteral("a.txt")));
    QVERIFY(pLblType->text().contains(QStringLiteral("txt")));
    QVERIFY(pLblLoc->text().contains(QDir(sRoot).dirName()));
    QVERIFY(pLblSize->text().contains(QStringLiteral("10")));   // 千分位不分隔两位数
    QVERIFY(!pLblMod->text().isEmpty());
    QVERIFY(!pChkRO->isChecked());                              // 新建文件默认非只读
    QVERIFY(!pChkHid->isChecked());
    delete pDlg;

    // ② 目录形态：递归统计 2 文件 20 字节；类型=文件夹。
    QDialog* pDlg2 = CProjectExplorerPanel::BuildPropertiesDialog(
        sRoot + QStringLiteral("/sub"), nullptr);
    QVERIFY(pDlg2 != nullptr);
    auto pLblType2 = pDlg2->findChild<QLabel*>(QStringLiteral("propLblType"));
    auto pLblSize2 = pDlg2->findChild<QLabel*>(QStringLiteral("propLblSize"));
    QVERIFY(pLblType2 != nullptr && pLblSize2 != nullptr);
    QCOMPARE(pLblType2->text(), QStringLiteral("文件夹"));
    QVERIFY(pLblSize2->text().contains(QStringLiteral("2")));   // 2 个文件
    QVERIFY(pLblSize2->text().contains(QStringLiteral("20")));  // 共 20 字节
    delete pDlg2;
}

void TestTaskListPanel::explorerRefreshNudge() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    const QString sSub  = sProj + QStringLiteral("/sub");
    QVERIFY(QDir().mkpath(sSub));
    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    panel.SetRoot(sProj);
    QTRY_VERIFY(panel.SelectPath(sProj));   // 选中工程根行
    QTreeView* pTree = panel.findChild<QTreeView*>();
    QVERIFY(pTree != nullptr);
    QAbstractItemModel* pM = pTree->model();
    const QModelIndex stRootRow = pM->index(0, 0);
    QVERIFY(stRootRow.isValid());
    auto fnChildByName = [&](const QModelIndex& stParent,
                             const QString& sName) -> QModelIndex {
        for (int i = 0; i < pM->rowCount(stParent); ++i) {
            const QModelIndex c = pM->index(i, 0, stParent);
            if (c.data().toString() == sName) {
                return c;
            }
        }
        return QModelIndex();
    };

    // 外部新建文件后手动刷新 → 重列补齐，树内出现新行。
    {
        QFile f(sProj + QStringLiteral("/late.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    panel.RefreshSelected();
    QTRY_VERIFY_WITH_TIMEOUT(fnChildByName(stRootRow, QStringLiteral("late.txt")).isValid(),
                             6000);
    QVERIFY(!QFileInfo::exists(sProj + QStringLiteral("/.autocode-refresh.tmp")));

    // 已展开子目录同样被重列（SetRoot 只展开根行本身；显式展开 sub 并记住）：
    //   外部在 sub 内建文件后刷新根 → sub 的重列作为目标之一被触发 → inner.txt 出现。
    QModelIndex stSub;
    QTRY_VERIFY_WITH_TIMEOUT(
        (stSub = fnChildByName(stRootRow, QStringLiteral("sub"))).isValid(), 8000);
    pTree->expand(stSub);
    QTest::qWait(100);
    {
        QFile f(sSub + QStringLiteral("/inner.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    panel.RefreshSelected();
    QTRY_VERIFY_WITH_TIMEOUT(fnChildByName(stRootRow, QStringLiteral("sub"))
                                 .isValid(), 8000);
    stSub = fnChildByName(stRootRow, QStringLiteral("sub"));
    QTRY_VERIFY_WITH_TIMEOUT(fnChildByName(stSub, QStringLiteral("inner.txt")).isValid(),
                             6000);
    panel.hide();
}

// 2026.09.23 显示隐藏清单配置化回归：默认隐藏 build（内置清单）；SetHiddenDirs
//   立即对已加载树重隐/重显——删掉 build 即可见、清空全可见、加回 src 则 src 隐藏。
//   背景：用户反馈 Compositer 的 build 目录看不到，误以为与设置里的排除清单同源——
//   实为两套机制（备份排除管备份范围，资源管理器显示隐藏清单硬编码），现配置化收口。
void TestTaskListPanel::explorerHiddenDirsConfigurable() {
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sProj = oTmp.path() + QStringLiteral("/proj");
    QVERIFY(QDir().mkpath(sProj + QStringLiteral("/build")));
    QVERIFY(QDir().mkpath(sProj + QStringLiteral("/src")));
    QVERIFY(QFile(sProj + QStringLiteral("/build/o.txt")).open(QIODevice::WriteOnly));
    QVERIFY(QFile(sProj + QStringLiteral("/src/a.cpp")).open(QIODevice::WriteOnly));

    CProjectExplorerPanel panel;
    panel.resize(400, 500);
    panel.show();
    panel.SetRoot(sProj);
    QTreeView* pTree = panel.findChild<QTreeView*>();
    QVERIFY(pTree != nullptr);
    QAbstractItemModel* pM = pTree->model();
    const QModelIndex stRootRow = pM->index(0, 0);
    QVERIFY(stRootRow.isValid());
    auto fnRowByName = [&](const QString& sName) -> int {
        for (int i = 0; i < pM->rowCount(stRootRow); ++i) {
            if (pM->index(i, 0, stRootRow).data().toString() == sName) {
                return i;
            }
        }
        return -1;
    };
    int iBuild = -1;
    int iSrc = -1;
    QTRY_VERIFY_WITH_TIMEOUT((iBuild = fnRowByName(QStringLiteral("build"))) >= 0, 8000);
    QTRY_VERIFY_WITH_TIMEOUT((iSrc = fnRowByName(QStringLiteral("src"))) >= 0, 8000);

    // ① 缺省（内置清单）：build 隐藏，src 可见。
    QVERIFY(pTree->isRowHidden(iBuild, stRootRow));
    QVERIFY(!pTree->isRowHidden(iSrc, stRootRow));

    // ② 下发不含 build 的清单 → build 行立即重显。
    panel.SetHiddenDirs(QStringList{QStringLiteral(".git"),
                                    QStringLiteral("node_modules")});
    QVERIFY(!pTree->isRowHidden(fnRowByName(QStringLiteral("build")), stRootRow));
    QVERIFY(!pTree->isRowHidden(fnRowByName(QStringLiteral("src")), stRootRow));

    // ③ 清空 = 全可见（本就无隐藏项，状态不变）。
    panel.SetHiddenDirs(QStringList());
    QVERIFY(!pTree->isRowHidden(fnRowByName(QStringLiteral("build")), stRootRow));

    // ④ 加回 src → src 行立即隐藏（新清单即时生效）。
    panel.SetHiddenDirs(QStringList{QStringLiteral("src")});
    QVERIFY(pTree->isRowHidden(fnRowByName(QStringLiteral("src")), stRootRow));
    QVERIFY(!pTree->isRowHidden(fnRowByName(QStringLiteral("build")), stRootRow));
    panel.hide();
}

QTEST_MAIN(TestTaskListPanel)
#include "test_tasklistpanel.moc"
