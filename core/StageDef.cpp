#include "StageDef.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>

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

        // 10. 测试：跑测试 + 写测试文件（无 Edit，不改业务代码）
        { EStage::Test, "test", QStringLiteral("测试"),
          true, true, ToolsTest(), "10_test.md",
          { QStringLiteral("cr.md") },
          QString() },

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
    return s_aDefs;
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
    // 规范流 = 定义表序 + bFeature/bBugfix 成员标志（与 CStageMachine 的流数组一致）；
    //   此处自包含取流（不依赖 CStageMachine），避免拖累仅链 StageDef 的小目标（tests/工具）。
    const bool bFeature = (a_eType == ETaskType::Feature);
    if (a_aCustom.isEmpty()) {
        QList<EStage> aCanonical;
        for (const SStageDef& sDef : GetStageDefs()) {
            if ((bFeature && sDef.bFeature) || (!bFeature && sDef.bBugfix)) {
                aCanonical.append(sDef.eStage);
            }
        }
        return aCanonical;   // 未自定义 → 完整规范流（旧任务兼容）
    }
    QList<EStage> aOut;
    for (const SStageDef& sDef : GetStageDefs()) {
        if (!(bFeature ? sDef.bFeature : sDef.bBugfix)) {
            continue;
        }
        if (sDef.eStage == EStage::Intake || a_aCustom.contains(sDef.eStage)) {
            aOut.append(sDef.eStage);   // 按规范序取勾选值；Intake 为入口强制包含
        }
    }
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
