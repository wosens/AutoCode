#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>

#include "project/CProjectManager.h"

// ============================================================================
// FR-6 代码回退（备份 / 恢复 / 版本清单）单元测试 — 任务 406807ec
// ----------------------------------------------------------------------------
// 覆盖可自动化的验收点（纯本地 Qt 文件系统操作，不依赖 GUI / Claude）：
//   AC-6.1 —— BackupSources 在 .autodev/tasks/<id>/backups/<seq>/ 生成全量源码快照
//            + <seq>.meta.json；版本序号单调递增；排除 .autodev/build/Execute/.git/.vs/x64
//            （含防递归：.autodev 自身被排除，backups 不会被再次备份）。
//   AC-6.2 —— 首次备份版本 summary 为空；后续版本经 SetBackupSummary 回填后非空。
//   ListBackups —— 按序号倒序（最新在前）返回；正确解析 seq/time/stage/summary。
//   AC-6.4 / AC-N —— RestoreSources 把工程源码恢复为指定版本；排除目录在恢复后仍保留；
//            恢复后 _restore_tmp 中转目录被清理；恢复无效版本返回 false 不抛。
//
// 不覆盖：UI（FR-6.4 回退按钮 / 对话框 / 二次确认，依赖 GUI）、备份阻塞 UI（NFR-3）、
//         恢复失败注入（无法在不改业务代码的前提下可靠制造 MirrorTree 失败）。
// ============================================================================
class CTestRollback : public QObject {
    Q_OBJECT

private:
    // 构造一个临时工程根（含 .autodev 与一个任务目录），返回其绝对路径。调用方负责清理。
    QString MakeSandbox() {
        // 用 this 指针做唯一性后缀，避免并发 / 多次运行碰撞。
        const QString sRoot = QDir::tempPath() + QStringLiteral("/ac_rollback_test_") +
            QString::number(reinterpret_cast<quintptr>(this));
        QDir().rmpath(sRoot);   // 清掉残留
        QDir().mkpath(sRoot);
        return sRoot;
    }

    static void Wipe(const QString& a_sDir) {
        QDir(a_sDir).removeRecursively();
    }

    // 在工程根写一个文件（自动建父目录）。
    static void WriteFile(const QString& a_sRoot, const QString& a_sRel,
                          const QByteArray& a_sData) {
        const QString sAbs = a_sRoot + QLatin1Char('/') + a_sRel;
        QFileInfo(sAbs).dir().mkpath(QStringLiteral("."));
        QFile f(sAbs);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(a_sData);
        f.close();
    }

    // 读文件全部内容（不存在返回空 QByteArray）。
    static QByteArray ReadFile(const QString& a_sAbs) {
        QFile f(a_sAbs);
        if (!f.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        return f.readAll();
    }

    // 比较两个目录树（相对路径集合 + 每个文件字节），排除 a_aSkipTopLevel 中的顶层目录。
    // 返回不一致的描述；一致返回空串。
    static QString CompareTrees(const QString& a_sDirA, const QString& a_sDirB,
                                const QStringList& a_aSkipTopLevel) {
        const QStringList aRel = CollectRel(a_sDirA, a_aSkipTopLevel);
        const QStringList bRel = CollectRel(a_sDirB, a_aSkipTopLevel);
        if (aRel.size() != bRel.size()) {
            return QStringLiteral("file count differs: A=%1 B=%2")
                .arg(aRel.size()).arg(bRel.size());
        }
        for (const QString& sRel : aRel) {
            if (!bRel.contains(sRel)) {
                return QStringLiteral("only in A: %1").arg(sRel);
            }
            const QByteArray aData = ReadFile(a_sDirA + QLatin1Char('/') + sRel);
            const QByteArray bData = ReadFile(a_sDirB + QLatin1Char('/') + sRel);
            if (aData != bData) {
                return QStringLiteral("content differs: %1").arg(sRel);
            }
        }
        // 反向检查（catch only-in-B）。
        for (const QString& sRel : bRel) {
            if (!aRel.contains(sRel)) {
                return QStringLiteral("only in B: %1").arg(sRel);
            }
        }
        return QString();
    }

    // 收集目录下所有文件的相对路径（正斜杠），跳过 a_aSkipTopLevel 顶层目录。
    static QStringList CollectRel(const QString& a_sRoot, const QStringList& a_aSkipTopLevel) {
        QStringList aOut;
        const QDir stRoot(a_sRoot);
        if (!stRoot.exists()) {
            return aOut;
        }
        CollectRelInner(stRoot, QString(), aOut, a_aSkipTopLevel);
        aOut.sort();
        return aOut;
    }

    static void CollectRelInner(const QDir& a_sDir, const QString& a_sPrefix,
                                QStringList& a_sOut, const QStringList& a_sSkipTopLevel) {
        // 文件。
        const QStringList aFiles = a_sDir.entryList(QDir::Files);
        for (const QString& sFile : aFiles) {
            a_sOut << (a_sPrefix.isEmpty() ? sFile : a_sPrefix + QLatin1Char('/') + sFile);
        }
        // 子目录。
        const QStringList aDirs = a_sDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sSub : aDirs) {
            // 仅顶层一级生效：prefix 为空时检查跳过清单。
            if (a_sPrefix.isEmpty() &&
                a_sSkipTopLevel.contains(sSub, Qt::CaseInsensitive)) {
                continue;
            }
            QDir stChild(a_sDir);
            stChild.cd(sSub);
            CollectRelInner(stChild,
                            a_sPrefix.isEmpty() ? sSub : a_sPrefix + QLatin1Char('/') + sSub,
                            a_sOut, a_sSkipTopLevel);
        }
    }

    // 工程源码排除清单（与 CProjectManager::BackupExcludeDirs 一致），用于对比树。
    static QStringList ExcludeList() {
        return {QStringLiteral(".autodev"), QStringLiteral("build"),
                QStringLiteral("Execute"), QStringLiteral(".git"),
                QStringLiteral(".vs"), QStringLiteral("x64")};
    }

private slots:
    // ============ AC-6.1：BackupSources 生成快照目录 + 元数据，序号单调递增 ============

    void testBackupCreatesVersionDirAndMeta() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task001");
        WriteFile(sRoot, QStringLiteral("main.cpp"), "int main(){return 0;}");
        WriteFile(sRoot, QStringLiteral("lib/util.h"), "#pragma once\n");

        CProjectManager stPm;
        QVERIFY2(stPm.Open(sRoot), "Open should succeed");
        QVERIFY2(stPm.EnsureAutoDev(), "EnsureAutoDev should succeed");

        const int iSeq1 = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        QVERIFY2(iSeq1 > 0, "first backup should return seq > 0");
        QCOMPARE(iSeq1, 1);

        // 版本目录与 meta 文件均存在。
        const QString sVerDir = stPm.TaskDir(sTaskId) + QStringLiteral("/backups/0001");
        QVERIFY2(QDir(sVerDir).exists(), "backup version dir 0001 must exist");
        QVERIFY2(QFile::exists(sVerDir + QStringLiteral(".meta.json")),
                 "0001.meta.json must exist");

        Wipe(sRoot);
    }

    // 序号单调递增：连续备份 → 0001、0002、0003。
    void testBackupSequenceIncrement() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task002");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();

        QCOMPARE(stPm.BackupSources(sTaskId, QStringLiteral("Develop")), 1);
        QCOMPARE(stPm.BackupSources(sTaskId, QStringLiteral("CodeReview")), 2);
        QCOMPARE(stPm.BackupSources(sTaskId, QStringLiteral("BugFix")), 3);

        Wipe(sRoot);
    }

    // AC-6.1 备份范围：排除 .autodev/build/Execute/.git/.vs/x64（防递归 + 防递归无限增长）。
    void testBackupExcludesListedDirs() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task003");
        WriteFile(sRoot, QStringLiteral("real.cpp"), "real source");
        // 这些目录下的文件不应被备份。
        WriteFile(sRoot, QStringLiteral(".autodev/tasks/task003/run.log"), "log");
        WriteFile(sRoot, QStringLiteral("build/obj.o"), "build artifact");
        WriteFile(sRoot, QStringLiteral("Execute/app.exe"), "exe");
        WriteFile(sRoot, QStringLiteral(".git/config"), "git cfg");
        WriteFile(sRoot, QStringLiteral(".vs/state"), "vs state");
        WriteFile(sRoot, QStringLiteral("x64/intermediate.obj"), "intermediate");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();

        const int iSeq = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        QVERIFY2(iSeq > 0, "backup should succeed");

        const QString sVerDir = stPm.TaskDir(sTaskId) +
            QStringLiteral("/backups/0001");
        const QStringList aRel = CollectRel(sVerDir, {});
        // #2 备份已改为 zstd 逐文件压缩：real.cpp 落盘为 real.cpp.zst（旧版未压缩时为 real.cpp）。
        QVERIFY2(aRel.contains(QStringLiteral("real.cpp.zst")) ||
                 aRel.contains(QStringLiteral("real.cpp")),
                 "real.cpp must be backed up (as .zst)");
        for (const QString& s : aRel) {
            QVERIFY2(!s.startsWith(QStringLiteral(".autodev/")),
                     qPrintable(QStringLiteral(".autodev/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral("build/")),
                     qPrintable(QStringLiteral("build/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral("Execute/")),
                     qPrintable(QStringLiteral("Execute/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".git/")),
                     qPrintable(QStringLiteral(".git/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".vs/")),
                     qPrintable(QStringLiteral(".vs/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral("x64/")),
                     qPrintable(QStringLiteral("x64/ must be excluded, got: ") + s));
        }
        Wipe(sRoot);
    }

    // 防递归专项：backups 自身位于 .autodev 内（已随 .autodev 排除），
    // 备份后的版本目录里不应再出现 backups 子树（否则说明递归）。
    void testBackupNoRecursiveBackupTree() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task004");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        // 连续两次备份，第二次时 .autodev/tasks/.../backups 已存在内容。
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        const QString sVer2 = stPm.TaskDir(sTaskId) +
            QStringLiteral("/backups/0002");
        // 第二版快照里不该再含 .autodev（否则递归）。
        QVERIFY2(!QDir(sVer2 + QStringLiteral("/.autodev")).exists(),
                 "backup snapshot must NOT contain .autodev (recursion guard)");
        Wipe(sRoot);
    }

    // 工程根未就绪 / 任务 id 空 → 返回 0（健壮性）。
    void testBackupFailsWhenNotOpened() {
        CProjectManager stPm;   // 未 Open
        QCOMPARE(stPm.BackupSources(QStringLiteral("t"), QStringLiteral("Develop")), 0);

        const QString sRoot = MakeSandbox();
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        // 任务 id 空。
        QCOMPARE(stPm.BackupSources(QString(), QStringLiteral("Develop")), 0);
        Wipe(sRoot);
    }

    // ============ AC-6.2：首次备份无 summary；后续回填 ============

    void testFirstBackupHasNoSummary() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task010");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        // meta.json 首版不应含 "summary" 键（按实现：构造 oMeta 时不写 summary）。
        const QString sMeta = stPm.TaskDir(sTaskId) +
            QStringLiteral("/backups/0001.meta.json");
        QByteArray buf = ReadFile(sMeta);
        QVERIFY2(!buf.isEmpty(), "meta.json must be readable");
        if (buf.startsWith("\xEF\xBB\xBF")) {
            buf = buf.mid(3);
        }
        QVERIFY2(!buf.contains("\"summary\""),
                 "first backup meta must NOT contain summary field");
        QVERIFY2(buf.contains("\"stage\""), "meta must contain stage");

        // ListBackups 读出的首版 summary 应为空。
        const QList<SBackupVersion> aList = stPm.ListBackups(sTaskId);
        QCOMPARE(aList.size(), 1);
        QVERIFY2(aList.first().sSummary.isEmpty(),
                 "first backup summary must be empty via ListBackups");
        QCOMPARE(aList.first().sStage, QStringLiteral("Develop"));
        Wipe(sRoot);
    }

    // AC-6.2：SetBackupSummary 回填后，ListBackups 能读到非空摘要。
    void testSetBackupSummaryBackfill() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task011");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        const int iSeq1 = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        const int iSeq2 = stPm.BackupSources(sTaskId, QStringLiteral("CodeReview"));

        // 仅给第二版回填摘要（首版保持空，符合 FR-6.3）。
        QVERIFY2(stPm.SetBackupSummary(sTaskId, iSeq2, QStringLiteral("修复滚动 bug")),
                 "SetBackupSummary should succeed for seq2");

        const QList<SBackupVersion> aList = stPm.ListBackups(sTaskId);
        QCOMPARE(aList.size(), 2);
        // 倒序：seq2 在前。
        QCOMPARE(aList.at(0).iSeq, iSeq2);
        QCOMPARE(aList.at(0).sSummary, QStringLiteral("修复滚动 bug"));
        QCOMPARE(aList.at(1).iSeq, iSeq1);
        QVERIFY2(aList.at(1).sSummary.isEmpty(), "first backup summary must stay empty");
        Wipe(sRoot);
    }

    // SetBackupSummary 对非法入参（空 id / 非法 seq / 不存在 meta）返回 false 不抛。
    void testSetBackupSummaryInvalid() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task012");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        QVERIFY2(!stPm.SetBackupSummary(QString(), 1, QStringLiteral("x")),
                 "empty task id should fail");
        QVERIFY2(!stPm.SetBackupSummary(sTaskId, 0, QStringLiteral("x")),
                 "non-positive seq should fail");
        QVERIFY2(!stPm.SetBackupSummary(sTaskId, 9999, QStringLiteral("x")),
                 "missing meta should fail");
        Wipe(sRoot);
    }

    // ============ ListBackups：倒序 + 解析 ============

    void testListBackupsOrdering() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task020");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "a");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        stPm.BackupSources(sTaskId, QStringLiteral("BugFix"));
        stPm.BackupSources(sTaskId, QStringLiteral("CodeReview"));

        const QList<SBackupVersion> aList = stPm.ListBackups(sTaskId);
        QCOMPARE(aList.size(), 3);
        // 倒序：3、2、1。
        QCOMPARE(aList.at(0).iSeq, 3);
        QCOMPARE(aList.at(1).iSeq, 2);
        QCOMPARE(aList.at(2).iSeq, 1);
        QCOMPARE(aList.at(0).sStage, QStringLiteral("CodeReview"));
        QVERIFY2(!aList.at(0).sTime.isEmpty(), "time must be set");
        Wipe(sRoot);
    }

    // ListBackups：无备份 / 空 id → 空列表（不抛）。
    void testListBackupsEmpty() {
        const QString sRoot = MakeSandbox();
        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        QVERIFY(stPm.ListBackups(QStringLiteral("nope")).isEmpty());
        QVERIFY(stPm.ListBackups(QString()).isEmpty());
        Wipe(sRoot);
    }

    // ============ AC-6.4 / AC-N：RestoreSources 恢复正确 + 排除项保留 + 中转清理 ============

    void testRestoreRevertsToVersion() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task030");

        // 第一版内容。
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v1-a");
        WriteFile(sRoot, QStringLiteral("b.cpp"), "v1-b");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        const int iSeq1 = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        // 改成第二版内容（a 变、b 删、c 新增）。
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v2-a-changed");
        QFile::remove(sRoot + QStringLiteral("/b.cpp"));
        WriteFile(sRoot, QStringLiteral("c.cpp"), "v2-c-new");
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        // 恢复到第一版。
        QString sErr;
        const bool bOk = stPm.RestoreSources(sTaskId, iSeq1, sErr);
        QVERIFY2(bOk, qPrintable(QStringLiteral("RestoreSources failed: ") + sErr));

        // 工程源码（非排除目录）应与第一版一致：a.cpp=v1-a、b.cpp=v1-b、无 c.cpp。
        QCOMPARE(ReadFile(sRoot + QStringLiteral("/a.cpp")), QByteArray("v1-a"));
        QCOMPARE(ReadFile(sRoot + QStringLiteral("/b.cpp")), QByteArray("v1-b"));
        QVERIFY2(!QFile::exists(sRoot + QStringLiteral("/c.cpp")),
                 "c.cpp should be gone after restore to v1");
        Wipe(sRoot);
    }

    // AC-N：恢复不破坏排除目录（.autodev 含 backups / build 等应原样保留）。
    void testRestorePreservesExcludedDirs() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task031");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v1");
        WriteFile(sRoot, QStringLiteral("build/keep.o"), "build data");
        WriteFile(sRoot, QStringLiteral("Execute/app.exe"), "exe");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        const int iSeq1 = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        // 再改 a.cpp 制造差异。
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v2");

        QString sErr;
        QVERIFY2(stPm.RestoreSources(sTaskId, iSeq1, sErr), "restore should succeed");

        // 排除目录内容应在恢复后保留。
        QVERIFY2(QFile::exists(sRoot + QStringLiteral("/build/keep.o")),
                 "build/ must survive restore");
        QVERIFY2(QFile::exists(sRoot + QStringLiteral("/Execute/app.exe")),
                 "Execute/ must survive restore");
        // .autodev/tasks/.../backups 整套仍存在（ListBackups 仍可用）。
        QVERIFY2(stPm.ListBackups(sTaskId).size() >= 1,
                 "backups metadata must survive restore");
        // a.cpp 恢复为 v1。
        QCOMPARE(ReadFile(sRoot + QStringLiteral("/a.cpp")), QByteArray("v1"));
        Wipe(sRoot);
    }

    // AC-N：恢复成功后 _restore_tmp 中转目录被清理（不留残骸）。
    void testRestoreCleansTempDir() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task032");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v1");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        const int iSeq1 = stPm.BackupSources(sTaskId, QStringLiteral("Develop"));
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v2");

        QString sErr;
        QVERIFY2(stPm.RestoreSources(sTaskId, iSeq1, sErr), "restore should succeed");

        const QString sTmp = stPm.TaskDir(sTaskId) +
            QStringLiteral("/backups/_restore_tmp");
        QVERIFY2(!QDir(sTmp).exists(),
                 "_restore_tmp must be cleaned up after successful restore");
        Wipe(sRoot);
    }

    // RestoreSources 非法入参：未打开工程 / 空 id / 非法 seq / 不存在版本 → 返回 false 带错误。
    void testRestoreInvalidInput() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task033");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v1");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));

        QString sErr;
        QVERIFY2(!stPm.RestoreSources(sTaskId, 9999, sErr),
                 "non-existent version should fail");
        QVERIFY2(!sErr.isEmpty(), "error message must be set on failure");

        QVERIFY2(!stPm.RestoreSources(sTaskId, 0, sErr),
                 "non-positive seq should fail");
        QVERIFY2(!stPm.RestoreSources(QString(), 1, sErr),
                 "empty task id should fail");

        // 未 Open 的实例。
        CProjectManager stPm2;
        QVERIFY2(!stPm2.RestoreSources(sTaskId, 1, sErr),
                 "restore without Open should fail");
        Wipe(sRoot);
    }

    // 恢复后继续备份：序号应继续递增（不与既有版本冲突）。
    void testRestoreThenBackupContinuesIncrement() {
        const QString sRoot = MakeSandbox();
        const QString sTaskId = QStringLiteral("task034");
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v1");

        CProjectManager stPm;
        stPm.Open(sRoot);
        stPm.EnsureAutoDev();
        stPm.BackupSources(sTaskId, QStringLiteral("Develop"));  // 1
        WriteFile(sRoot, QStringLiteral("a.cpp"), "v2");

        QString sErr;
        QVERIFY(stPm.RestoreSources(sTaskId, 1, sErr));   // 回到 v1
        // 继续备份 → 序号 2（不是 1）。
        QCOMPARE(stPm.BackupSources(sTaskId, QStringLiteral("Develop")), 2);
        QCOMPARE(stPm.ListBackups(sTaskId).size(), 2);
        Wipe(sRoot);
    }
};

QTEST_MAIN(CTestRollback)
#include "test_rollback.moc"
