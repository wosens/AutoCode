#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryFile>

#include "core/CTaskInfo.h"
#include "db/CTaskHistoryDB.h"

// 注意：CTaskHistoryDB::DbPath() 固定用 <exe 目录>/history.db，
// 测试 exe 由 ctest 启动，其所在目录即 DB 落点。每个用例前后删除该文件保证隔离。

// 单元测试：CTaskHistoryDB 的 Upsert/LoadAll/Delete 往返 + 离散列正确性。
class CTestHistoryDb : public QObject {
    Q_OBJECT

private:
    static QString DbFilePath() {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/history.db");
    }
    static void RemoveDb() {
        QFile::remove(DbFilePath());
    }

private slots:
    void init() { RemoveDb(); }
    void cleanup() { RemoveDb(); }

    void testRoundTrip() {
        CTaskHistoryDB db;
        QVERIFY(db.Open());
        QVERIFY(QFileInfo::exists(DbFilePath()));

        // 构造一条任务并 Upsert
        CTaskInfo task;
        task.SetId(QStringLiteral("abc12345"));
        task.SetName(QStringLiteral("历史测试任务"));
        task.SetUserRequest(QStringLiteral("实现导出 CSV"));
        task.SetType(ETaskType::Feature);
        task.SetStatus(ETaskStatus::Done);
        task.SetProjectDir(QStringLiteral("D:/demo/proj"));
        task.SetCurrentStage(EStage::Intake);
        task.SetStageIter(3);
        task.SetUsage(0.42, 12345, 6789);
        task.SetLastError(QString());
        QVERIFY(db.Upsert(&task));

        // LoadAll：应拿到一条，且字段往返一致
        QList<CTaskInfo*> aAll = db.LoadAll();
        QCOMPARE(aAll.size(), 1);
        CTaskInfo* p = aAll.first();
        QCOMPARE(p->Id(),           QStringLiteral("abc12345"));
        QCOMPARE(p->Name(),         QStringLiteral("历史测试任务"));
        QCOMPARE(p->UserRequest(),  QStringLiteral("实现导出 CSV"));
        QCOMPARE(static_cast<int>(p->Type()),   static_cast<int>(ETaskType::Feature));
        QCOMPARE(static_cast<int>(p->Status()), static_cast<int>(ETaskStatus::Done));
        QCOMPARE(p->ProjectDir(),   QStringLiteral("D:/demo/proj"));
        QCOMPARE(static_cast<int>(p->CurrentStage()), static_cast<int>(EStage::Intake));
        QCOMPARE(p->StageIter(),    3);
        QCOMPARE(p->TotalCost(),    0.42);
        QCOMPARE(p->TotalInTokens(),  12345);
        QCOMPARE(p->TotalOutTokens(), 6789);
        QVERIFY(!p->CreatedAt().isNull());
        qDeleteAll(aAll);

        // 幂等：再 Upsert 同 id 仍是 1 条（INSERT OR REPLACE 覆盖）
        QVERIFY(db.Upsert(&task));
        QCOMPARE(db.LoadAll().size(), 1);
        qDeleteAll(db.LoadAll());   // LoadAll 第二次也安全
    }

    void testMultipleAndDelete() {
        CTaskHistoryDB db;
        QVERIFY(db.Open());

        for (int i = 0; i < 3; ++i) {
            CTaskInfo t;
            t.SetId(QStringLiteral("id%1").arg(i));
            t.SetName(QStringLiteral("T%1").arg(i));
            t.SetStatus(ETaskStatus::Failed);
            QVERIFY(db.Upsert(&t));
        }
        QList<CTaskInfo*> aAll = db.LoadAll();
        QCOMPARE(aAll.size(), 3);
        qDeleteAll(aAll);

        // 删中间一条 → 剩 2
        QVERIFY(db.Delete(QStringLiteral("id1")));
        QList<CTaskInfo*> aAfter = db.LoadAll();
        QCOMPARE(aAfter.size(), 2);
        QStringList aIds;
        for (CTaskInfo* p : aAfter) { aIds << p->Id(); }
        QVERIFY(!aIds.contains(QStringLiteral("id1")));
        qDeleteAll(aAfter);

        // 删不存在的 id 不报错，仍成功（SQLITE_DONE）
        QVERIFY(db.Delete(QStringLiteral("nope")));
    }

    void testDeleteEmptyId() {
        CTaskHistoryDB db;
        QVERIFY(db.Open());
        // 空 id 应安全返回 false，不崩溃
        QVERIFY(!db.Delete(QString()));
    }
};

QTEST_MAIN(CTestHistoryDb)
#include "test_historydb.moc"
