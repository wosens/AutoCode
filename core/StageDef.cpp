#include "StageDef.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

// —— 动态阶段表（2026-10-14 任务编排设计）：自定义阶段由 COrchConfig 注册 ——
namespace {
QVector<SStageDef> g_aDynStages;
bool g_bAllDirty = true;
QVector<SStageDef> g_aAllStages;
}   // namespace

void RegisterDynamicStages(const QVector<SStageDef>& a_aDyn) {
    g_aDynStages = a_aDyn;
    g_bAllDirty = true;   // 合表重建（注册仅发生在启动/设计保存两个低频时点；
}                        // FindStage 返回指针均为短生命周期使用，无悬垂风险）

void ClearDynamicStages() {
    g_aDynStages.clear();
    g_bAllDirty = true;
}

bool IsDynamicStage(EStage a_eStage) {
    return static_cast<int>(a_eStage) >= 100;
}

const QVector<SStageDef>& GetStageDefs() {
    static const QVector<SStageDef> s_aDefs = {
        // 1. 需求收集（共享）：读 guide，写 intake.md
        { EStage::Intake, "intake", QStringLiteral("需求收集"),
          true, true, ToolsDocs(), "01_intake.md",
          { QStringLiteral(".autodev/guide.md") },
          QStringLiteral("intake.md") },

        // 2. 确认需求（共享）：写 requirements.md
        { EStage::ConfirmReq, "confirm", QStringLiteral("确认需求"),
          true, true, ToolsDocs(), "02_confirm.md",
          { QStringLiteral("intake.md") },
          QStringLiteral("requirements.md") },

        // 3. 需求评审：只写 review_requirements.md（不改业务代码）
        { EStage::ReviewReq, "review_requirements", QStringLiteral("需求评审"),
          true, true, ToolsDocs(), "03_review_requirements.md",
          { QStringLiteral("requirements.md") },
          QStringLiteral("review_requirements.md") },

        // 4. 设计（Feature）：写 design.md
        { EStage::Design, "design", QStringLiteral("设计"),
          true, false, ToolsDocs(), "04_design.md",
          { QStringLiteral("requirements.md"), QStringLiteral(".autodev/guide.md") },
          QStringLiteral("design.md") },

        // 5. 设计评审（Feature）：写 review_design.md
        { EStage::ReviewDesign, "review_design", QStringLiteral("设计评审"),
          true, false, ToolsDocs(), "05_review_design.md",
          { QStringLiteral("design.md"), QStringLiteral("requirements.md") },
          QStringLiteral("review_design.md") },

        // 6. 计划（Feature）：写 plan.md
        { EStage::Plan, "plan", QStringLiteral("计划"),
          true, false, ToolsDocs(), "06_plan.md",
          { QStringLiteral("design.md"), QStringLiteral("requirements.md") },
          QStringLiteral("plan.md") },

        // 7. 计划评审（Feature）：写 review_plan.md
        { EStage::ReviewPlan, "review_plan", QStringLiteral("计划评审"),
          true, false, ToolsDocs(), "07_review_plan.md",
          { QStringLiteral("plan.md"), QStringLiteral("design.md") },
          QStringLiteral("review_plan.md") },

        // 8. 开发：改业务代码（Edit/Bash）
        { EStage::Develop, "develop", QStringLiteral("开发"),
          true, false, ToolsDev(), "08_develop.md",
          { QStringLiteral("plan.md"), QStringLiteral("design.md"), QStringLiteral("requirements.md") },
          QString() },

        // 9. 代码审查：写 cr.md
        { EStage::CodeReview, "code_review", QStringLiteral("代码审查"),
          true, true, ToolsDocs(), "09_code_review.md",
          { QStringLiteral("plan.md") },
          QStringLiteral("cr.md") },

        // 14. 验证（共享）：编译运行被开发软件，插调试→编译→读日志→视觉验证→清理；写 verify.md
        //    （表序=规范流序：CodeReview → Verify → Test；ResolveFlow 按表序过滤勾选阶段）
        { EStage::Verify, "verify", QStringLiteral("验证"),
          true, true, ToolsVerify(), "14_verify.md",
          { QStringLiteral("cr.md"), QStringLiteral("requirements.md"),
            QStringLiteral(".autodev/guide.md") },
          QStringLiteral("verify.md") },

        // 10. 测试：跑测试 + 写测试文件（无 Edit，不改业务代码）；报告与决策行追加进 cr.md
        //    （10_test.md 输出节明确要求）——sOutput 必须与之一致，否则
        //    ParseReviewDecision 读不到文末「决策：通过/驳回」行，会退到 Tier2 尾部关键词
        //    扫描，报告叙述里的「不构成本轮驳回」曾致通过被误判驳回、无限回炉（2026.10.14）。
        { EStage::Test, "test", QStringLiteral("测试"),
          true, true, ToolsTest(), "10_test.md",
          { QStringLiteral("cr.md") },
          QStringLiteral("cr.md") },

        // 11. 根因分析（Bugfix）：写 root_cause.md
        { EStage::RootCause, "root_cause", QStringLiteral("根因分析"),
          false, true, ToolsDocs(), "11_root_cause.md",
          { QStringLiteral("intake.md") },
          QStringLiteral("root_cause.md") },

        // 12. 根因核实（Bugfix）：写 review_root_cause.md
        { EStage::RootCauseVerify, "root_cause_verify", QStringLiteral("根因核实"),
          false, true, ToolsDocs(), "12_root_cause_verify.md",
          { QStringLiteral("root_cause.md") },
          QStringLiteral("review_root_cause.md") },

        // 13. bug 修复（Bugfix）：改业务代码
        { EStage::BugFix, "bug_fix", QStringLiteral("bug 修复"),
          false, true, ToolsDev(), "13_bug_fix.md",
          { QStringLiteral("root_cause.md") },
          QString() },
    };
    // 2026-10-14 合表：内置静态表 + 已注册动态阶段（脏标记重建；无动态时即内置表副本）
    if (g_bAllDirty) {
        g_aAllStages = s_aDefs;
        g_aAllStages += g_aDynStages;
        g_bAllDirty = false;
    }
    return g_aAllStages;
}

const SStageDef* FindStage(EStage a_eStage) {
    for (const SStageDef& sDef : GetStageDefs()) {
        if (sDef.eStage == a_eStage) {
            return &sDef;
        }
    }
    return nullptr;
}

const SStageDef* FindStageById(const QString& a_sId) {
    for (const SStageDef& sDef : GetStageDefs()) {
        if (sDef.sId == a_sId) {
            return &sDef;
        }
    }
    return nullptr;
}

QList<EStage> ResolveFlow(ETaskType a_eType, const QList<EStage>& a_aCustom) {
    // 空 → 该类型完整规范流（定义表序 + bFeature/bBugfix 成员标志，向后兼容）。
    if (a_aCustom.isEmpty()) {
        const bool bFeature = (a_eType == ETaskType::Feature);
        QList<EStage> aCanonical;
        for (const SStageDef& sDef : GetStageDefs()) {
            // 动态阶段双标志均 false，天然不进规范流（仅经任务自定义流生效）
            if ((bFeature && sDef.bFeature) || (!bFeature && sDef.bBugfix)) {
                aCanonical.append(sDef.eStage);
            }
        }
        return aCanonical;
    }
    // 2026-10-14 任务编排设计：非空 → 按存储顺序去重 + 有效性校验后原序返回
    //   （配置序即执行序；自定义阶段双标志 false，旧版按标志过滤会丢弃它们）。
    //   Intake 入口强制首位；未知阶段值（配置已删）静默跳过。
    QList<EStage> aOut;
    QSet<int> aSeen;
    for (EStage e : a_aCustom) {
        if (e == EStage::Intake || aSeen.contains(static_cast<int>(e))) {
            continue;
        }
        if (FindStage(e) == nullptr) {
            continue;   // 定义表中不存在（内置表 + 动态注册均无）→ 跳过
        }
        aSeen.insert(static_cast<int>(e));
        aOut.append(e);
    }
    aOut.prepend(EStage::Intake);
    return aOut;
}

QStringList FilterInputs(const SStageDef& a_stDef, const QList<EStage>& a_aFlow,
                         QStringList* a_pDropped) {
    if (a_pDropped != nullptr) { a_pDropped->clear(); }
    // 产物 → 产出阶段映射（sOutput 反查）；guide.md 等非阶段产物不在表中 → 恒保留。
    QHash<QString, EStage> aProducer;
    for (const SStageDef& sDef : GetStageDefs()) {
        if (!sDef.sOutput.isEmpty()) {
            aProducer.insert(sDef.sOutput, sDef.eStage);
        }
    }
    QStringList aKept;
    for (const QString& sInput : a_stDef.aInputs) {
        const auto it = aProducer.constFind(sInput);
        if (it == aProducer.constEnd() || a_aFlow.contains(it.value())) {
            aKept.append(sInput);   // 非阶段产物 或 产出阶段在流内 → 保留
        } else if (a_pDropped != nullptr) {
            a_pDropped->append(sInput);
        }
    }
    if (aKept.isEmpty()) {
        // 全被滤掉（如勾了 CodeReview 却没勾 Plan）：兜底一份有效输入防 EnsureStageInputs 弹回
        if (a_aFlow.contains(EStage::ConfirmReq)) {
            aKept.append(QStringLiteral("requirements.md"));
        } else {
            aKept.append(QStringLiteral("intake.md"));
        }
    }
    return aKept;
}

QString ToolsDocs() {
    // 文档/评审/分析阶段：可读代码、可写自己的报告，但不能 Edit 业务代码、不跑命令。
    //   deny 黑名单（bypass 下 allow 失效、deny 生效）：禁 Bash（不跑命令）+ Edit（不改业务代码）。
    return QStringLiteral("Bash,Edit");
}

QString ToolsDev() {
    // 开发阶段：可读写、可改业务代码、可跑命令——全功能，不禁任何工具（deny 空串）。
    return QStringLiteral("");
}

QString ToolsTest() {
    // 测试阶段：可读代码、可写测试文件、可跑命令，但不能改业务代码（禁 Edit）。
    return QStringLiteral("Edit");
}

QString ToolsVerify() {
    // 验证阶段：改代码(插/清调试)、跑命令(编译/运行)、读工程文件，工具权限不低于 Develop。
    // 远程视觉(analyze_image 类 MCP)与本地视觉技能(vision-local/screenshot)由 Claude 经 skill/MCP
    // 机制按需调用，不在此 --disallowedTools 显式禁名（与 Develop 一致：不禁基础工具）。
    return QStringLiteral("");
}

QString GetPromptsDir() {
    // 2026.08.15 便携化：只读 exe 同级 prompts/（由 CMake POST_BUILD 从源 prompts/ 同步）。
    //   不再向上回退读源码目录——运行期只依赖 exe 所在目录，Execute 整目录拷走即自包含。
    //   （目录即便不存在也原样返回，便于上层报错定位「模板缺失」。）
    return QCoreApplication::applicationDirPath() + QStringLiteral("/prompts");
}
