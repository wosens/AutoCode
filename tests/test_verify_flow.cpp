#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "core/StageDef.h"
#include "core/CStageMachine.h"
#include "core/TaskTypes.h"
#include "project/CGuideFingerprintScanner.h"

// 本轮（be3fd900）新增/修改的纯逻辑单元测试。
// 覆盖范围：
//   AC2.1/AC2.4 —— Verify 阶段元数据完整、工具集不低于 Develop。
//   AC2.2/AC-N1 —— Verify 编入 Feature / Bugfix 两流程（CodeReview 后、Test 前）；既有 1~13 编号与流转不变。
//   AC1.2      —— 指纹 Diff 命中 新增 / 修改 / 删除 三类。
//   AC1.4      —— 无变化时 WriteIncremental 不改指纹表（逐字节保留未变行）。
//   AC1.5      —— ScanProject 排除 .autodev/build/Execute/doc 等目录。
// 不覆盖：UI（FR3/FR4，依赖 GUI）、验证阶段实际执行（FR2.3，需跑 Claude + 真实工程编译）。
class CTestVerifyFlow : public QObject {
    Q_OBJECT

private:
    // 构造一个临时工程根（含 .autodev），返回其绝对路径。调用方负责清理。
    QString MakeSandbox() {
        const QString sRoot = QDir::tempPath() + QStringLiteral("/ac_verify_test_") +
            QString::number(reinterpret_cast<quintptr>(this));
        QDir().mkpath(sRoot);
        QDir().mkpath(sRoot + QStringLiteral("/.autodev"));
        return sRoot;
    }

    static void Wipe(const QString& a_sDir) {
        QDir(a_sDir).removeRecursively();
    }

    // 在工程根写一个文件（自动建父目录），返回正斜杠相对路径。
    static QString WriteFile(const QString& a_sRoot, const QString& a_sRel,
                             const QByteArray& a_sData) {
        const QString sAbs = a_sRoot + QLatin1Char('/') + a_sRel;
        QFileInfo(sAbs).dir().mkpath(QStringLiteral("."));
        QFile f(sAbs);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(a_sData);
        f.close();
        return a_sRel;
    }

    // 构造一段最小但合法的 guide.md（含指纹表 BEGIN..END 段），写入 <root>/.autodev/guide.md。
    // a_aFp 为指纹表行（已格式化或原文）。返回 guide 全文。
    static QString WriteGuideWithFp(const QString& a_sRoot,
                                    const QList<SFileFingerprint>& a_aFp) {
        const QString sGuidePath = a_sRoot + QStringLiteral("/.autodev/guide.md");
        QString sBody;
        sBody += QStringLiteral("# 工程指引\n\n一些描述文字。\n\n");
        sBody += CGuideFingerprintScanner::s_sBeginMarker + QStringLiteral("\n");
        sBody += QStringLiteral("```autocode-fp\n");
        sBody += QStringLiteral("# 相对路径 | 字节数 | 修改时间(UTC ms) | hash(可选)\n");
        for (const SFileFingerprint& rFp : a_aFp) {
            sBody += FmtFpLine(rFp) + QStringLiteral("\n");
        }
        sBody += QStringLiteral("```\n");
        sBody += CGuideFingerprintScanner::s_sEndMarker + QStringLiteral("\n");
        QFile f(sGuidePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(sBody.toUtf8());
        f.close();
        return sBody;
    }

    // 直接构造一个指纹（避免依赖文件系统 mtime 精度）。
    static SFileFingerprint MkFp(const QString& a_sPath, qint64 a_iSize, qint64 a_iMsUtc) {
        SFileFingerprint stFp;
        stFp.sRelPath = a_sPath;
        stFp.iSize    = a_iSize;
        stFp.dtMtime  = QDateTime::fromMSecsSinceEpoch(a_iMsUtc);
        return stFp;
    }

    // 与扫描器 FormatFpLine 同格式（私有不可访问，测试内自行复刻）：路径 | 字节数 | UTC ms | hash(-)
    static QString FmtFpLine(const SFileFingerprint& a_stFp) {
        const qint64 iMs = a_stFp.dtMtime.isValid()
            ? a_stFp.dtMtime.toUTC().toMSecsSinceEpoch() : 0;
        const QString sHash = a_stFp.sHash.isEmpty()
            ? QStringLiteral("-") : a_stFp.sHash;
        return QStringLiteral("%1 | %2 | %3 | %4")
            .arg(a_stFp.sRelPath).arg(a_stFp.iSize).arg(iMs).arg(sHash);
    }

private slots:
    // ============ AC2.1 / AC2.4：Verify 阶段定义 ============

    // Verify 阶段存在于定义表，枚举值=14（不动 1~13）。
    void testVerifyStageEnumAndDef() {
        QCOMPARE(int(EStage::Verify), 14);
        const SStageDef* pDef = FindStage(EStage::Verify);
        QVERIFY2(pDef != nullptr, "Verify stage missing from GetStageDefs()");
        QCOMPARE(pDef->sId, QStringLiteral("verify"));
        QCOMPARE(pDef->sNameZh, QStringLiteral("验证"));
        QCOMPARE(pDef->sPromptFile, QStringLiteral("14_verify.md"));
        QCOMPARE(pDef->sOutput, QStringLiteral("verify.md"));
        QVERIFY(!pDef->sOutput.isEmpty());
    }

    // FindStageById("verify") 可命中。
    void testFindStageByIdVerify() {
        const SStageDef* pDef = FindStageById(QStringLiteral("verify"));
        QVERIFY(pDef != nullptr);
        QCOMPARE(pDef->eStage, EStage::Verify);
    }

    // 既有 1~13 阶段编号与 id 全部保持（NFR1：不破坏既有阶段）。
    void testLegacyStagesUnchanged() {
        QCOMPARE(int(EStage::Intake), 1);
        QCOMPARE(int(EStage::ConfirmReq), 2);
        QCOMPARE(int(EStage::ReviewReq), 3);
        QCOMPARE(int(EStage::Design), 4);
        QCOMPARE(int(EStage::ReviewDesign), 5);
        QCOMPARE(int(EStage::Plan), 6);
        QCOMPARE(int(EStage::ReviewPlan), 7);
        QCOMPARE(int(EStage::Develop), 8);
        QCOMPARE(int(EStage::CodeReview), 9);
        QCOMPARE(int(EStage::Test), 10);
        QCOMPARE(int(EStage::RootCause), 11);
        QCOMPARE(int(EStage::RootCauseVerify), 12);
        QCOMPARE(int(EStage::BugFix), 13);
        // 定义表至少 14 行。
        QVERIFY(GetStageDefs().size() >= 14);
    }

    // AC2.4：Verify 工具集 deny 名单不低于 Develop（Develop deny=""，Verify 也应为空，不禁基础工具）。
    void testVerifyToolsNotBelowDev() {
        const QString sDev    = ToolsDev();
        const QString sVerify = ToolsVerify();
        // Develop 禁用工具集为空串；Verify 必须 <= Develop 权限收紧度，即 Verify 也禁用更少或同等。
        // 这里语义：Verify 的 deny 列表不应比 Develop 多禁任何基础工具（Edit/Bash 等）。
        QVERIFY2(!sVerify.contains(QLatin1String("Edit")),
                 "Verify must allow Edit (insert/cleanup debug code)");
        QVERIFY2(!sVerify.contains(QLatin1String("Bash")),
                 "Verify must allow Bash (compile/run)");
        Q_UNUSED(sDev);
    }

    // ============ AC2.2 / AC-N1：Verify 编入流程 ============

    // Feature 流：Verify 在 CodeReview 后、Test 前。
    void testVerifyInFeatureFlow() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Feature);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify not in Feature flow");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must sit between CodeReview and Test");
        QCOMPARE(CStageMachine::NextStage(ETaskType::Feature, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Verify);
        QCOMPARE(CStageMachine::NextStage(ETaskType::Feature, EStage::Verify).value_or(EStage::Intake),
                 EStage::Test);
        QVERIFY(CStageMachine::StageInFlow(ETaskType::Feature, EStage::Verify));
    }

    // Bugfix 流：Verify 在 CodeReview 后、Test 前（同上）。
    void testVerifyInBugfixFlow() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Bugfix);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify not in Bugfix flow");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must sit between CodeReview and Test");
        QCOMPARE(CStageMachine::NextStage(ETaskType::Bugfix, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Verify);
        QCOMPARE(CStageMachine::NextStage(ETaskType::Bugfix, EStage::Verify).value_or(EStage::Intake),
                 EStage::Test);
    }

    // AC-N1：Verify 不在 RejectTarget 路由表内 → 走 default 返回自身（不被当成评审驳回源）。
    void testVerifyNotInRejectTarget() {
        QCOMPARE(CStageMachine::RejectTarget(EStage::Verify), EStage::Verify);
        // 既有驳回路由不变。
        QCOMPARE(CStageMachine::RejectTarget(EStage::CodeReview), EStage::Develop);
        QCOMPARE(CStageMachine::RejectTarget(EStage::ReviewReq), EStage::ConfirmReq);
        QCOMPARE(CStageMachine::RejectTarget(EStage::Test), EStage::Develop);
    }

    // Feature 流首阶段仍为 Intake（既有行为不变）。
    void testFirstStageUnchanged() {
        QCOMPARE(CStageMachine::FirstStage(ETaskType::Feature), EStage::Intake);
        QCOMPARE(CStageMachine::FirstStage(ETaskType::Bugfix), EStage::Intake);
    }

    // ============ AC1.2：指纹 Diff 三类变化 ============

    // 构造 prev/cur 纯内存指纹列表，Diff 应分别命中 新增/修改/删除。
    void testDiffThreeClasses() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)
              << MkFp(QStringLiteral("src/b.cpp"), 200, 2000000)
              << MkFp(QStringLiteral("src/c.cpp"), 300, 3000000);

        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("src/a.cpp"), 100, 1000000)   // 不变
             << MkFp(QStringLiteral("src/b.cpp"), 999, 2000000)   // size 变 → 修改
             << MkFp(QStringLiteral("src/d.cpp"), 400, 4000000);  // 新增（c 被删）

        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QCOMPARE(stCh.aAdded.size(), 1);
        QCOMPARE(stCh.aAdded.first().sRelPath, QStringLiteral("src/d.cpp"));
        QCOMPARE(stCh.aModified.size(), 1);
        QCOMPARE(stCh.aModified.first().sRelPath, QStringLiteral("src/b.cpp"));
        QCOMPARE(stCh.aDeleted.size(), 1);
        QCOMPARE(stCh.aDeleted.first(), QStringLiteral("src/c.cpp"));
    }

    // mtime 变（size 不变）也应判为修改。
    void testDiffMtimeChangeIsModified() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("x.h"), 50, 1111111);
        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("x.h"), 50, 9999999);
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QCOMPARE(stCh.aModified.size(), 1);
        QVERIFY(stCh.aAdded.isEmpty());
        QVERIFY(stCh.aDeleted.isEmpty());
    }

    // 完全一致 → 三类全空。
    void testDiffNoChange() {
        QList<SFileFingerprint> aPrev;
        aPrev << MkFp(QStringLiteral("a"), 1, 1) << MkFp(QStringLiteral("b"), 2, 2);
        QList<SFileFingerprint> aCur;
        aCur << MkFp(QStringLiteral("a"), 1, 1) << MkFp(QStringLiteral("b"), 2, 2);
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrev, aCur);
        QVERIFY(stCh.aAdded.isEmpty());
        QVERIFY(stCh.aModified.isEmpty());
        QVERIFY(stCh.aDeleted.isEmpty());
    }

    // ============ AC1.2：端到端 Scan → Parse → Diff（真文件系统）============

    void testScanParseDiffEndToEnd() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("main.cpp"), "int main(){return 0;}");
        WriteFile(sRoot, QStringLiteral("lib/util.h"), "#pragma once\n");

        // 用扫描结果回填 guide 指纹表（模拟上次记录的状态）。
        QList<SFileFingerprint> aPrev = CGuideFingerprintScanner::ScanProject(sRoot);
        QCOMPARE(aPrev.size(), 2);
        WriteGuideWithFp(sRoot, aPrev);

        // 制造变化：新增 add.cpp、修改 main.cpp、删除 util.h。
        QFile::remove(sRoot + QStringLiteral("/lib/util.h"));
        WriteFile(sRoot, QStringLiteral("main.cpp"), "int main(){return 1;} // changed");
        WriteFile(sRoot, QStringLiteral("add.cpp"), "int extra;");

        QList<SFileFingerprint> aCur = CGuideFingerprintScanner::ScanProject(sRoot);
        QList<SFileFingerprint> aPrevParsed = CGuideFingerprintScanner::ParseFingerprintTable(
            [&]() { QFile f(sRoot + QStringLiteral("/.autodev/guide.md")); f.open(QIODevice::ReadOnly); QString s = QTextStream(&f).readAll(); return s; }());
        const SGuideChangeSet stCh = CGuideFingerprintScanner::Diff(aPrevParsed, aCur);

        QVERIFY2(stCh.aAdded.size() >= 1, "add.cpp should be detected as added");
        QVERIFY2(!stCh.aDeleted.isEmpty(), "util.h should be detected as deleted");
        QVERIFY2(stCh.aDeleted.contains(QStringLiteral("lib/util.h")), "util.h missing from deleted");
        QVERIFY2(!stCh.aModified.isEmpty(), "main.cpp should be detected as modified");
        Wipe(sRoot);
    }

    // ============ AC1.5：ScanProject 排除生成物/工具目录 ============

    void testScanExcludesBuildDirs() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("real.cpp"), "x");
        // 这些目录下的文件不应被计入工程变更。
        WriteFile(sRoot, QStringLiteral("build/obj.o"), "build artifact");
        WriteFile(sRoot, QStringLiteral("Execute/foo.exe"), "exe");
        WriteFile(sRoot, QStringLiteral(".autodev/guide.md"), "g");
        WriteFile(sRoot, QStringLiteral("doc/readme.txt"), "doc");
        WriteFile(sRoot, QStringLiteral(".git/config"), "git");
        WriteFile(sRoot, QStringLiteral(".vs/sln.docstate"), "vs");

        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        QStringList aPaths;
        for (const SFileFingerprint& r : aFp) {
            aPaths << r.sRelPath;
        }
        QVERIFY2(aPaths.contains(QStringLiteral("real.cpp")), "real.cpp must be scanned");
        for (const QString& s : aPaths) {
            QVERIFY2(!s.startsWith(QStringLiteral("build/")),
                     qPrintable(QStringLiteral("build/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral("Execute/")),
                     qPrintable(QStringLiteral("Execute/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".autodev/")),
                     qPrintable(QStringLiteral(".autodev/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral("doc/")),
                     qPrintable(QStringLiteral("doc/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".git/")),
                     qPrintable(QStringLiteral(".git/ must be excluded, got: ") + s));
            QVERIFY2(!s.startsWith(QStringLiteral(".vs/")),
                     qPrintable(QStringLiteral(".vs/ must be excluded, got: ") + s));
        }
        Wipe(sRoot);
    }

    // AC1.5 细节：同前缀名不应误伤（buildfoo 不等于 build）。
    void testScanExcludesDoNotOvermatch() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("buildutil/x.cpp"), "x");   // build+util，非 build/
        WriteFile(sRoot, QStringLiteral("buildfoo/y.cpp"), "y");    // build+foo
        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        QStringList aPaths;
        for (const SFileFingerprint& r : aFp) {
            aPaths << r.sRelPath;
        }
        QVERIFY2(aPaths.contains(QStringLiteral("buildutil/x.cpp")),
                 "buildutil/ must NOT be mistaken for build/");
        QVERIFY2(aPaths.contains(QStringLiteral("buildfoo/y.cpp")),
                 "buildfoo/ must NOT be mistaken for build/");
        Wipe(sRoot);
    }

    // ============ AC1.4：无变化 → WriteIncremental 不改指纹表（逐字节保留）============

    void testWriteIncrementalNoChangePreservesTable() {
        const QString sRoot = MakeSandbox();
        WriteFile(sRoot, QStringLiteral("a.cpp"), "aaaa");
        WriteFile(sRoot, QStringLiteral("b.cpp"), "bbbb");

        QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(sRoot);
        const QString sGuideBefore = WriteGuideWithFp(sRoot, aFp);

        // 重新扫描（无任何文件变化）→ Diff 全空 → WriteIncremental 应保持指纹表字节不变。
        QList<SFileFingerprint> aCur = CGuideFingerprintScanner::ScanProject(sRoot);
        SGuideChangeSet stEmpty;   // 三类全空
        const QString sGuidePath = sRoot + QStringLiteral("/.autodev/guide.md");
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(sGuidePath, aCur, stEmpty,
                                                                    QStringLiteral("2026-08-13T00:00:00"));
        QVERIFY2(bOk, "WriteIncremental should succeed on a valid guide with table");

        QFile f2(sGuidePath);
        f2.open(QIODevice::ReadOnly | QIODevice::Text);
        const QString sGuideAfter = QTextStream(&f2).readAll();

        // 指纹表段（BEGIN..END）必须与写前逐字节一致。
        auto ExtractTable = [](const QString& a_s) {
            const int iB = a_s.indexOf(CGuideFingerprintScanner::s_sBeginMarker);
            const int iE = a_s.indexOf(CGuideFingerprintScanner::s_sEndMarker);
            if (iB < 0 || iE <= iB) {
                return QString();
            }
            return a_s.mid(iB, (iE + CGuideFingerprintScanner::s_sEndMarker.length()) - iB);
        };
        QCOMPARE(ExtractTable(sGuideBefore), ExtractTable(sGuideAfter));
        Wipe(sRoot);
    }

    // ============ 健壮性：异常输入安全返回 ============

    // guide 不存在 → WriteIncremental 返回 false（调用方走全量）。
    void testWriteIncrementalMissingFile() {
        SGuideChangeSet stCh;
        stCh.aAdded << MkFp(QStringLiteral("z.cpp"), 1, 1);
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(
            QDir::tempPath() + QStringLiteral("/ac_verify_nonexistent_") +
                QString::number(reinterpret_cast<quintptr>(this)) + QStringLiteral("/guide.md"),
            {}, stCh, QStringLiteral("ts"));
        QVERIFY2(!bOk, "WriteIncremental on missing guide must return false");
    }

    // guide 存在但无 BEGIN..END 段 → 返回 false。
    void testWriteIncrementalNoTableReturnsFalse() {
        const QString sRoot = MakeSandbox();
        const QString sGuidePath = sRoot + QStringLiteral("/.autodev/guide.md");
        QFile f(sGuidePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("# 工程指引\n\n没有指纹表的 guide。\n");
        f.close();

        SGuideChangeSet stCh;
        stCh.aAdded << MkFp(QStringLiteral("z.cpp"), 1, 1);
        const bool bOk = CGuideFingerprintScanner::WriteIncremental(sGuidePath, {}, stCh, QStringLiteral("ts"));
        QVERIFY2(!bOk, "WriteIncremental on guide without table must return false");
        Wipe(sRoot);
    }

    // ParseFingerprintTable：空串 / 无标记 → 空表（不抛）。
    void testParseEmptyAndMissing() {
        QVERIFY(CGuideFingerprintScanner::ParseFingerprintTable(QString()).isEmpty());
        QVERIFY(CGuideFingerprintScanner::ParseFingerprintTable(
            QStringLiteral("no markers here")).isEmpty());
    }

    // ScanProject：目录不存在 → 空表（不抛）。
    void testScanMissingDirEmpty() {
        const QList<SFileFingerprint> aFp = CGuideFingerprintScanner::ScanProject(
            QDir::tempPath() + QStringLiteral("/ac_verify_definitely_missing_") +
            QString::number(reinterpret_cast<quintptr>(this)));
        QVERIFY(aFp.isEmpty());
    }
    // ============ 2026.08.15 流程档位：ResolveFlow / FilterInputs / 流重载 NextStage ============

    // 空 custom → 完整规范流（旧任务兼容，序一致）。
    void testResolveFlowEmptyIsCanonical() {
        const QList<EStage> aResolved = ResolveFlow(ETaskType::Feature, {});
        QCOMPARE(aResolved, CStageMachine::GetFlowStages(ETaskType::Feature));
        QCOMPARE(aResolved.first(), EStage::Intake);
        QCOMPARE(aResolved.last(), EStage::Test);
    }

    // 精简勾选（收集+确认+开发+审查+验证+测试）→ 6 阶段且按规范序；设计/计划/前置评审被跳过。
    void testResolveFlowSimplePreset() {
        const QList<EStage> aCustom = { EStage::Intake, EStage::ConfirmReq, EStage::Develop,
                                        EStage::CodeReview, EStage::Verify, EStage::Test };
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature, aCustom);
        QCOMPARE(aFlow.size(), 6);
        QCOMPARE(aFlow, aCustom);
        QVERIFY(!aFlow.contains(EStage::Design));
        QVERIFY(!aFlow.contains(EStage::Plan));
        QVERIFY(!aFlow.contains(EStage::ReviewReq));
        // 流重载：ConfirmReq 直接推进 Develop（跳过中间阶段）。
        QCOMPARE(CStageMachine::NextStage(aFlow, EStage::ConfirmReq).value_or(EStage::Intake),
                 EStage::Develop);
        // 末阶段 Test → nullopt（任务完成）。
        QVERIFY(!CStageMachine::NextStage(aFlow, EStage::Test).has_value());
    }

    // Intake 强制包含：用户清单漏勾 Intake 也会被补回首阶段。
    void testResolveFlowForcesIntake() {
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Develop, EStage::CodeReview });
        QCOMPARE(aFlow.first(), EStage::Intake);
        QVERIFY(aFlow.contains(EStage::Develop));
    }

    // Bugfix 自定义流：跳过 RootCauseVerify 后 RootCause 直接进 BugFix。
    void testResolveFlowBugfixSkipsVerify() {
        const QList<EStage> aFlow = ResolveFlow(
            ETaskType::Bugfix, { EStage::Intake, EStage::ConfirmReq, EStage::RootCause,
                                 EStage::BugFix, EStage::CodeReview, EStage::Verify, EStage::Test });
        QCOMPARE(aFlow.size(), 7);
        QVERIFY(!aFlow.contains(EStage::RootCauseVerify));
        QCOMPARE(CStageMachine::NextStage(aFlow, EStage::RootCause).value_or(EStage::Intake),
                 EStage::BugFix);
    }

    // PrevStage：流内前驱；首阶段/不在流内 → nullopt。
    void testPrevStage() {
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Intake, EStage::Develop, EStage::CodeReview });
        QCOMPARE(CStageMachine::PrevStage(aFlow, EStage::CodeReview).value_or(EStage::Intake),
                 EStage::Develop);
        QVERIFY(!CStageMachine::PrevStage(aFlow, EStage::Intake).has_value());
        QVERIFY(!CStageMachine::PrevStage(aFlow, EStage::Plan).has_value());
    }

    // FilterInputs：Develop 输入按流过滤——完整流输入不变；精简流（无 Plan/Design）
    // → 只剩 requirements.md，被滤项回带。
    void testFilterInputsByFlow() {
        const SStageDef* pDev = FindStage(EStage::Develop);
        QVERIFY(pDev != nullptr);
        const QStringList aFull = FilterInputs(*pDev, CStageMachine::GetFlowStages(ETaskType::Feature));
        QCOMPARE(aFull, pDev->aInputs);
        const QList<EStage> aSimple = ResolveFlow(ETaskType::Feature,
            { EStage::Intake, EStage::ConfirmReq, EStage::Develop, EStage::CodeReview,
              EStage::Verify, EStage::Test });
        QStringList aDropped;
        const QStringList aKept = FilterInputs(*pDev, aSimple, &aDropped);
        QCOMPARE(aKept, QStringList{ QStringLiteral("requirements.md") });
        QVERIFY(aDropped.contains(QStringLiteral("plan.md")));
        QVERIFY(aDropped.contains(QStringLiteral("design.md")));
    }

    // FilterInputs 兜底：连 ConfirmReq 都没勾 → 全滤空时兜底 intake.md（防 EnsureStageInputs 弹回）。
    void testFilterInputsFallbackIntake() {
        const SStageDef* pDev = FindStage(EStage::Develop);
        QVERIFY(pDev != nullptr);
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
                                                { EStage::Intake, EStage::Develop });
        const QStringList aKept = FilterInputs(*pDev, aFlow);
        QCOMPARE(aKept, QStringList{ QStringLiteral("intake.md") });
    }

    // FilterInputs：CodeReview 输入 plan.md，无 Plan 阶段时滤空 → 兜底 requirements.md（ConfirmReq 在流内）。
    void testFilterInputsCodeReview() {
        const SStageDef* pCr = FindStage(EStage::CodeReview);
        QVERIFY(pCr != nullptr);
        const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
            { EStage::Intake, EStage::ConfirmReq, EStage::Develop, EStage::CodeReview,
              EStage::Verify, EStage::Test });
        const QStringList aKept = FilterInputs(*pCr, aFlow);
        QCOMPARE(aKept, QStringList{ QStringLiteral("requirements.md") });
    }
};

QTEST_MAIN(CTestVerifyFlow)
#include "test_verify_flow.moc"
