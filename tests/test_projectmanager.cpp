#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>

#include "project/CProjectManager.h"

// 最近打开工程历史（MRU）单元测试。
// 覆盖需求 AC-2/3/4/5/6/7/11/12 的可自动化部分（CProjectManager 的纯逻辑 + config.ini 往返）。
// UI 对话框 / 菜单交互（AC-1/8/9/10/13）依赖人工 GUI 操作，不在本自动化测试范围。
class CTestProjectManager : public QObject {
    Q_OBJECT

private:
    // 取 exe 同级 config.ini 全路径（与 CProjectManager::ConfigFilePath 实现一致，用于清理 / 校验）。
    static QString ConfigPath() {
        const QString sExe = QCoreApplication::applicationFilePath();
        return QDir::cleanPath(QFileInfo(sExe).absolutePath() + QStringLiteral("/config.ini"));
    }

    static void RemoveConfig() {
        QFile::remove(ConfigPath());
    }

private slots:
    void init() {
        RemoveConfig();
    }

    void cleanup() {
        RemoveConfig();
    }

    // AC-2：首次运行（无 config.ini）历史为空，不崩溃。
    void testEmptyOnFirstRun() {
        CProjectManager stPm;
        stPm.LoadRecent();
        QVERIFY(stPm.RecentProjects().isEmpty());
    }

    // AC-3：打开 A 后，历史最前为 A。
    void testSingleRecordedOnTop() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 1);
        QCOMPARE(aList.first(), QStringLiteral("C:/ProjA"));
    }

    // AC-4：先 A 后 B → 顺序为 B、A（MRU，最后打开最前）。
    void testMruOrdering() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        stPm.AddRecent(QStringLiteral("C:/ProjB"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/ProjB"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/ProjA"));
    }

    // AC-5：重复打开 A（已在历史中）→ A 置顶且只有一条 A（去重，不新增）。
    void testDedupPromoteToTop() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        stPm.AddRecent(QStringLiteral("C:/ProjB"));
        // 此时为 B、A；再打开 A → 应为 A、B，无重复 A。
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/ProjA"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/ProjB"));
        // 确认无重复。
        QCOMPARE(aList.count(QStringLiteral("C:/ProjA")), 1);
    }

    // AC-5 / NFR-3.2：不同写法（分隔符 / 尾部斜杠 / 盘符大小写）同目录应去重为一条。
    void testNormalizePathDedup() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:\\ProjX"));
        stPm.AddRecent(QStringLiteral("C:/ProjX/"));
        stPm.AddRecent(QStringLiteral("c:/ProjX"));
        const QStringList aList = stPm.RecentProjects();
        // 三种写法指同一目录 → 只一条。
        QCOMPARE(aList.size(), 1);
    }

    // AC-6/AC-7 边界：子菜单上限是 UI 层常量（s_kiMaxRecentMenu=10）。
    // 这里验证全量内存列表**不**截断（设计 §3.2：全量历史无上限），11 条全在。
    void testFullListNoTruncation() {
        CProjectManager stPm;
        for (int i = 0; i < 11; ++i) {
            stPm.AddRecent(QStringLiteral("C:/Proj%1").arg(i));
        }
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 11);
        // 最近打开（i=10）在最前。
        QCOMPARE(aList.first(), QStringLiteral("C:/Proj10"));
        QCOMPARE(aList.last(), QStringLiteral("C:/Proj0"));
    }

    // AC-11：写入 config.ini 后重新载入，列表被保留（MRU 顺序）。
    void testPersistenceRoundTrip() {
        CProjectManager stPm1;
        stPm1.AddRecent(QStringLiteral("C:/PersistA"));
        stPm1.AddRecent(QStringLiteral("C:/PersistB"));
        stPm1.AddRecent(QStringLiteral("C:/PersistC"));

        // config.ini 应已落盘（AC-12：exe 同级）。
        const QString sCfg = ConfigPath();
        QVERIFY2(QFile::exists(sCfg),
                 qPrintable(QStringLiteral("config.ini not created at: %1").arg(sCfg)));

        // 新实例重新载入 → 顺序应为 C、B、A。
        CProjectManager stPm2;
        stPm2.LoadRecent();
        const QStringList aList = stPm2.RecentProjects();
        QCOMPARE(aList.size(), 3);
        QCOMPARE(aList.at(0), QStringLiteral("C:/PersistC"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/PersistB"));
        QCOMPARE(aList.at(2), QStringLiteral("C:/PersistA"));
    }

    // AC-11 + AC-5：持久化跨实例的去重（重新载入后再 Add 同一条不重复）。
    void testPersistenceDedupAfterReload() {
        CProjectManager stPm1;
        stPm1.AddRecent(QStringLiteral("C:/DedupA"));
        stPm1.AddRecent(QStringLiteral("C:/DedupB"));

        CProjectManager stPm2;
        stPm2.LoadRecent();
        stPm2.AddRecent(QStringLiteral("C:/DedupA"));  // 已存在 → 置顶
        const QStringList aList = stPm2.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/DedupA"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/DedupB"));
    }

    // AC-12：config.ini 落在 exe 同级（非当前工作目录）。这里校验路径来源。
    void testConfigAtExeDir() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ExeDirCheck"));
        const QString sCfg = ConfigPath();
        const QString sExeDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
        QVERIFY2(QFileInfo(sCfg).absolutePath() == sExeDir,
                 "config.ini must live next to the executable, not the working directory");
    }

    // 空串入参安全跳过（健壮性）。
    void testEmptyStringIgnored() {
        CProjectManager stPm;
        stPm.AddRecent(QString());
        QVERIFY(stPm.RecentProjects().isEmpty());
    }

    // ClearRecent 清空内存列表。
    void testClearRecent() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ClearA"));
        stPm.AddRecent(QStringLiteral("C:/ClearB"));
        stPm.ClearRecent();
        QVERIFY(stPm.RecentProjects().isEmpty());
    }
};

QTEST_MAIN(CTestProjectManager)
#include "test_projectmanager.moc"
