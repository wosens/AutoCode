#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryFile>
#include <QDir>

#include "core/CTaskInfo.h"
#include "db/CTaskHistoryDB.h"

// 注意：旧版全局库路径固定为 <exe 目录>/history.db（ctest exe 所在目录）。
// 2026.08.20 历史库按工程隔离：Open(path) 打开 <工程>/.autodev/history.db；
// 迁移用例在 exe 目录构造旧全局库 → 调 MigrateLegacyToProjects → 校验归位与收档。

// 单元测试：CTaskHistoryDB 的 Upsert/LoadAll/Delete 往返 + 离散列正确性 + 旧库迁移。
class CTestHistoryDb : public QObject {
    Q_OBJECT

private:
    static QString DbFilePath() {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/history.db");
    }
    static void RemoveDb() {
        QFile::remove(DbFilePath());
        QFile::remove(DbFilePath() + QStringLiteral(".migrated"));
    }

private slots:
    void init() { RemoveDb(); }
    void cleanup() { RemoveDb(); }

    void testRoundTrip() {
        CTaskHistoryDB db;
        QVERIFY(db.Open(QStringLiteral("projdb.db")));   // 显式路径（工程内库口径）
        QVERIFY(QFileInfo::exists(QStringLiteral("projdb.db")));

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

        // 2026.08.21 短连接（不锁工程文件/目录）：操作间隙不持有 sqlite 句柄——
        //   -wal/-shm 已收档；且 db 文件可被 rename（Windows：被打开的文件无法
        //   rename，成功即证明句柄已释放）；重命名回来后再操作仍通（句柄按需重开）。
        QVERIFY(!QFileInfo::exists(QStringLiteral("projdb.db-wal")));
        QVERIFY(!QFileInfo::exists(QStringLiteral("projdb.db-shm")));
        QVERIFY(QFile::rename(QStringLiteral("projdb.db"),
                              QStringLiteral("projdb_renamed.db")));
        QVERIFY(QFile::rename(QStringLiteral("projdb_renamed.db"),
                              QStringLiteral("projdb.db")));
        QVERIFY(db.Upsert(&task));
        QCOMPARE(db.LoadAll().size(), 1);
        qDeleteAll(db.LoadAll());

        db.Close();
        QFile::remove(QStringLiteral("projdb.db"));
    }

    void testMultipleAndDelete() {
        CTaskHistoryDB db;
        QVERIFY(db.Open(QStringLiteral("projdb2.db")));

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
        db.Close();
        QFile::remove(QStringLiteral("projdb2.db"));
    }

    void testDeleteEmptyId() {
        CTaskHistoryDB db;
        QVERIFY(db.Open(QStringLiteral("projdb3.db")));
        // 空 id 应安全返回 false，不崩溃
        QVERIFY(!db.Delete(QString()));
        db.Close();
        QFile::remove(QStringLiteral("projdb3.db"));
    }

    // 2026.08.20 迁移：旧全局库（exe 目录）→ 各工程 .autodev/history.db，
    //   迁完旧库收档 .migrated；无旧库时幂等返回 0。
    void testMigrateLegacy() {
        // 无旧库 → 幂等 0
        QCOMPARE(CTaskHistoryDB::MigrateLegacyToProjects(), 0);

        // 构造旧全局库：2 行属工程 A（反斜杠原生路径），1 行属工程 B，1 行工程不存在。
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProjA = QDir(oTmp.path()).filePath(QStringLiteral("projA"));
        const QString sProjB = QDir(oTmp.path()).filePath(QStringLiteral("projB"));
        QVERIFY(QDir().mkpath(sProjA));
        QVERIFY(QDir().mkpath(sProjB));
        {
            CTaskHistoryDB dbLegacy;
            QVERIFY(dbLegacy.Open());   // 无参 = 旧全局库（exe 目录）
            for (int i = 0; i < 2; ++i) {
                CTaskInfo t;
                t.SetId(QStringLiteral("a%1").arg(i));
                t.SetName(QStringLiteral("A 任务 %1").arg(i));
                t.SetStatus(ETaskStatus::Done);
                t.SetProjectDir(QDir::toNativeSeparators(sProjA));
                QVERIFY(dbLegacy.Upsert(&t));
            }
            CTaskInfo tB;
            tB.SetId(QStringLiteral("b0"));
            tB.SetName(QStringLiteral("B 任务"));
            tB.SetStatus(ETaskStatus::Failed);
            tB.SetProjectDir(sProjB);
            QVERIFY(dbLegacy.Upsert(&tB));
            CTaskInfo tGhost;   // 工程目录已不存在 → 留在收档文件
            tGhost.SetId(QStringLiteral("ghost"));
            tGhost.SetProjectDir(QStringLiteral("Z:/definitely/not/exist"));
            QVERIFY(dbLegacy.Upsert(&tGhost));
        }

        QCOMPARE(CTaskHistoryDB::MigrateLegacyToProjects(), 3);
        QVERIFY(!QFile::exists(DbFilePath()));
        QVERIFY(QFile::exists(DbFilePath() + QStringLiteral(".migrated")));

        // 工程 A 得 2 行、工程 B 得 1 行；字段往返（含原生路径规范化）。
        CTaskHistoryDB dbA;
        QVERIFY(dbA.Open(CTaskHistoryDB::ProjectDbPath(sProjA)));
        QCOMPARE(dbA.LoadAll().size(), 2);
        dbA.Close();
        CTaskHistoryDB dbB;
        QVERIFY(dbB.Open(CTaskHistoryDB::ProjectDbPath(sProjB)));
        QList<CTaskInfo*> aB = dbB.LoadAll();
        QCOMPARE(aB.size(), 1);
        QCOMPARE(aB.first()->Id(), QStringLiteral("b0"));
        qDeleteAll(aB);
        dbB.Close();

        // 再次迁移：旧库已收档 → 幂等 0（不会重复写入）。
        QCOMPARE(CTaskHistoryDB::MigrateLegacyToProjects(), 0);
    }

    // 2026.08.23 损坏自愈回归：库文件被写坏（多实例并发/异常退出）时 Open 应
    //   quick_check 体检 → 收档坏库 history.db.corrupt-<ts> → 原路径重建空库可用
    //   （终态任务只存于此库，损坏即历史任务整体凭空消失——task.json 回填自愈）。
    void testCorruptDbSelfHeal() {
        const QString sPath = QStringLiteral("projdb_corrupt.db");
        QFile::remove(sPath);
        {
            CTaskHistoryDB db;
            QVERIFY(db.Open(sPath));
            CTaskInfo t;
            t.SetId(QStringLiteral("c0"));
            t.SetName(QStringLiteral("坏库前任务"));
            t.SetStatus(ETaskStatus::Done);
            QVERIFY(db.Upsert(&t));
            QCOMPARE(db.LoadAll().size(), 1);
        }
        // 手工破坏：把页头/页中数据改字节（模拟真实 malformed：树页错乱）。
        {
            QFile f(sPath);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QByteArray a = f.readAll();
            for (int i = 200; i < qMin(9000, a.size()); i += 5) {
                // 跳过页 1 的 sqlite 文件头（0~99）防 open 直接失败；覆盖页 2/3 的
                // btree 页头与 cell 区（复现真实「Tree page btreeInitPage error」类损坏）。
                a[i] = static_cast<char>(0xA5);
            }
            f.seek(0);
            f.write(a);
            f.close();
        }
        CTaskHistoryDB db;
        QVERIFY(db.Open(sPath));   // 体检判损坏 → 收档重建，Open 仍成功
        QCOMPARE(db.LoadAll().size(), 0);   // 新库为空
        // 收档文件存在（唯一 .corrupt-*）
        QDir stDir = QFileInfo(sPath).absoluteDir();
        const QStringList aBaks = stDir.entryList({QFileInfo(sPath).fileName() + QStringLiteral(".corrupt-*")},
                                                  QDir::Files);
        QCOMPARE(aBaks.size(), 1);
        // 重建库可正常写入（后续 Upsert 回填自愈路径畅通）
        CTaskInfo t;
        t.SetId(QStringLiteral("c1"));
        t.SetName(QStringLiteral("自愈后任务"));
        t.SetStatus(ETaskStatus::Done);
        QVERIFY(db.Upsert(&t));
        QCOMPARE(db.LoadAll().size(), 1);
        db.Close();
        QFile::remove(sPath);
        QFile::remove(QDir(stDir).filePath(aBaks.first()));
    }
};

QTEST_MAIN(CTestHistoryDb)
#include "test_historydb.moc"
