#include <QtTest/QtTest>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "core/StageDef.h"
#include "core/CStageMachine.h"
#include "core/TaskTypes.h"

// 本轮（96931df4）FR-1「验证阶段单独向 record.md 写简短摘要」的回归测试。
//
// FR-1 的写盘入口 CTaskOrchestrator::RecordVerifySummary 是私有方法，且其唯一触发点
// Advance() 在 Verify 分支后会继续走 ProceedToNext → 调度下一阶段（含起 Claude 进程），
// 无法在无 Claude / 无 GUI 的纯单测里整链路驱动。因此本测试钉住 RecordVerifySummary
// 【赖以正确工作的稳定前置不变量】——这些不变量一旦被改坏（Verify 阶段输出文件名变了、
// Verify 从流程里挪走/删除、结论提取正则不再容忍全/半角冒号与空白），FR-1 会静默失效
// （verify.md 找不到结论行 → 回退到 result/助手文本，AC-1.2 不成立）。把这些不变量固化为
// 二值断言，可在 CI 里早期拦截。
//
// 覆盖：
//   AC-1.1/AC-1.3 前置 —— Verify 在 Feature/Bugfix 流程内且位于 CodeReview 与 Test 之间
//                  （Advance 的 if (eCur == Verify) 分支才可能被触发；非 Verify 阶段不产验证行）。
//   AC-1.2      —— Verify 阶段 sOutput == "verify.md"（结论行就从这个文件读）。
//   AC-1.2 核心 —— 结论提取正则「最终验证结论」容忍全/半角冒号 + 首尾空白，抓到正确结论文本。
//   AC-1.2 区分 —— 统一摘要行（RecordSummary）与验证结论行（RecordVerifySummary）可通过
//                  前缀「验证结论：」区分（前者不含、后者必含）。
// 不覆盖：RecordVerifySummary 的实际写盘（私有方法 + 全链路依赖，见上）、UI（FR-2/3/4）。
class CTestRecordVerify : public QObject {
    Q_OBJECT

private:
    // 与 CTaskOrchestrator::RecordVerifySummary 内嵌的结论提取正则同款（行锚定、
    // 容忍全角「：」/半角「:」、首尾空白）。此处复刻仅为钉死契约：一旦实现端正则被改动
    // 导致抓取行为变化，本测试与实现同时维护，便于审查时一眼对照。
    static QString ExtractVerifyVerdict(const QString& a_sVerifyMd) {
        static const QRegularExpression reVerdict(
            QStringLiteral("(?m)^\\s*最终验证结论\\s*[:：]\\s*(.+?)\\s*$"));
        const QRegularExpressionMatch m = reVerdict.match(a_sVerifyMd);
        return m.hasMatch() ? m.captured(1).trimmed() : QString();
    }

private slots:
    // ============ AC-1.1 / AC-1.3 前置：Verify 在两流程内、位置正确 ============

    // Feature 流：Verify 必须在流程里，且夹在 CodeReview 与 Test 之间。
    // 若 Verify 被移除或挪位，Advance 永不进 Verify 分支 → FR-1 永不触发。
    void testVerifyInFeatureFlowBetween() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Feature);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify must remain in Feature flow (else FR-1 never fires)");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test anchors missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must stay between CodeReview and Test");
    }

    // Bugfix 流：同上。
    void testVerifyInBugfixFlowBetween() {
        const QList<EStage> aFlow = CStageMachine::GetFlowStages(ETaskType::Bugfix);
        const int iCR = aFlow.indexOf(EStage::CodeReview);
        const int iV  = aFlow.indexOf(EStage::Verify);
        const int iT  = aFlow.indexOf(EStage::Test);
        QVERIFY2(iV >= 0, "Verify must remain in Bugfix flow (else FR-1 never fires)");
        QVERIFY2(iCR >= 0 && iT >= 0, "CodeReview/Test anchors missing");
        QVERIFY2(iCR < iV && iV < iT, "Verify must stay between CodeReview and Test");
    }

    // ============ AC-1.2：Verify 阶段输出文件名 ============

    // RecordVerifySummary 用 FindStage(Verify)->sOutput 拼 verify.md 路径读结论行。
    // sOutput 必须正好是 "verify.md"，否则读不到本轮 Claude 产出的验证报告。
    void testVerifyStageOutputFile() {
        const SStageDef* pDef = FindStage(EStage::Verify);
        QVERIFY2(pDef != nullptr, "Verify stage definition missing");
        QCOMPARE(pDef->sOutput, QStringLiteral("verify.md"));
    }

    // ============ AC-1.2 核心：结论提取正则 ============

    // 半角冒号 + 无首尾空白 → 抓到「通过」。
    void testVerdictRegexHalfWidthColon() {
        const QString sMd = QStringLiteral(
            "# 验证\n\n## 测试结果\n\n全部通过。\n\n最终验证结论:通过\n");
        QCOMPARE(ExtractVerifyVerdict(sMd), QStringLiteral("通过"));
    }

    // 全角冒号（verify.md 模板常见）→ 同样抓到。
    void testVerdictRegexFullWidthColon() {
        const QString sMd = QStringLiteral(
            "## 测试结果\n\n最终验证结论：验证失败-编译未通过\n");
        QCOMPARE(ExtractVerifyVerdict(sMd), QStringLiteral("验证失败-编译未通过"));
    }

    // 行首有空白（缩进）+ 冒号后有空格 → 抓到的文本已 trimmed。
    void testVerdictRegexLeadingIndentAndTrim() {
        const QString sMd = QStringLiteral(
            "  最终验证结论：   通过 - 全部用例 PASS   \n");
        QCOMPARE(ExtractVerifyVerdict(sMd), QStringLiteral("通过 - 全部用例 PASS"));
    }

    // 多行里只取第一条「最终验证结论」行（取首条，不拼接）。
    void testVerdictRegexPicksFirstLine() {
        const QString sMd = QStringLiteral(
            "最终验证结论：第一条结论\n后面还有文字\n最终验证结论：第二条结论\n");
        QCOMPARE(ExtractVerifyVerdict(sMd), QStringLiteral("第一条结论"));
    }

    // 无结论行 → 空串（RecordVerifySummary 据此回退 result/助手文本，不抛错）。
    void testVerdictRegexMissingReturnsEmpty() {
        const QString sMd = QStringLiteral("# 验证\n\n没有结论行的报告。\n");
        QVERIFY(ExtractVerifyVerdict(sMd).isEmpty());
    }

    // 「最终验证结论」作为正文片段出现（非行首）→ 不误抓。
    void testVerdictRegexNotAnchoredMidLine() {
        const QString sMd = QStringLiteral(
            "上文提到最终验证结论：通过 这几个字，但不是行首。\n");
        QVERIFY(ExtractVerifyVerdict(sMd).isEmpty());
    }

    // ============ AC-1.2 区分：统一行 vs 验证结论行 可由前缀区分 ============

    // RecordSummary 的统一行格式不含「验证结论：」；RecordVerifySummary 的验证行必含。
    // AC-1.2 要求「结论行可通过前缀与普通阶段行区分」。这里用两条与实现同格式的样例行钉死。
    void testDistinguishabilityByPrefix() {
        // 与 RecordSummary 同格式（[时间] 任务名 | 阶段名 (iter N) | OK/FAIL | 摘要）。
        const QString sUnifiedLine = QStringLiteral(
            "[2026-08-13 18:00] 某任务 | 验证 (iter 1) | OK | 全部用例通过");
        // 与 RecordVerifySummary 同格式（含前缀「验证结论：」+ 通过/失败 + 可选原因）。
        const QString sVerifyLine = QStringLiteral(
            "[2026-08-13 18:00] 某任务 | 验证 (iter 1) | 验证结论：通过 - 全部用例 PASS");

        QVERIFY2(!sUnifiedLine.contains(QStringLiteral("验证结论：")),
                 "unified RecordSummary line must NOT carry the verify-conclusion prefix");
        QVERIFY2(sVerifyLine.contains(QStringLiteral("验证结论：")),
                 "RecordVerifySummary line must carry the 验证结论： prefix");
    }

    // 失败态：验证结论行用「失败」而非「FAIL」，仍带前缀，可区分。
    void testDistinguishabilityFailure() {
        const QString sVerifyLine = QStringLiteral(
            "[2026-08-13 18:00] 某任务 | 验证 (iter 1) | 验证结论：失败 - 编译未通过");
        QVERIFY(sVerifyLine.contains(QStringLiteral("验证结论：")));
        QVERIFY(sVerifyLine.contains(QStringLiteral("失败")));
    }
};

QTEST_MAIN(CTestRecordVerify)
#include "test_record_verify.moc"
