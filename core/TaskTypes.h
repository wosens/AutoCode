#pragma once

#include <QString>

// 任务类型
enum class ETaskType {
    Feature,  // 新功能：intake→confirm→review_req→design→review_design→plan→review_plan→develop→code_review→test
    Bugfix    // 修缺陷：intake→confirm→root_cause→root_cause_verify→bug_fix→code_review→test
};

// 任务状态
enum class ETaskStatus {
    Pending,
    Running,
    Paused,
    Stopped,
    Done,
    Failed
};

// 14 个阶段（1~13 为原有阶段，编号与流转不变；14=验证，新增独立阶段）
enum class EStage {
    Intake           = 1,
    ConfirmReq       = 2,
    ReviewReq        = 3,
    Design           = 4,
    ReviewDesign     = 5,
    Plan             = 6,
    ReviewPlan       = 7,
    Develop          = 8,
    CodeReview       = 9,
    Test             = 10,
    RootCause        = 11,
    RootCauseVerify  = 12,
    BugFix           = 13,
    Verify           = 14   // 验证：编译运行被开发软件，插调试→编译→读日志→视觉验证→清理，写 verify.md
};

// 评审决策（MVP 两态：Pass / Reject；Iterate 语义归入 Reject，回到上游生产阶段）
enum class EReviewDecision {
    Pass,
    Reject
};

// 流程档位：任务创建时的阶段勾选预设 + 评审策略（聚合方式 / 轻量评审轮次 / 审查视角数）。
//   简单 = 精简阶段清单（跳设计/计划与前置评审）+ CodeReview 单视角；
//   标准 = 完整阶段清单 + 评审多数决（默认，向后兼容旧任务）；
//   严格 = 完整阶段清单 + 评审一票否决（关键任务逃生门，行为同旧版）。
// 档位只决定默认勾选与评审策略；实际阶段清单以 task.json 的 flowStages 为权威，可自由勾选。
enum class ETier {
    Simple   = 0,
    Standard = 1,
    Strict   = 2
};

inline QString ToString(ETaskType a_eType) {
    switch (a_eType) {
        case ETaskType::Feature: return "feature";
        case ETaskType::Bugfix:  return "bugfix";
    }
    return "feature";
}

inline QString ToString(ETier a_eTier) {
    switch (a_eTier) {
        case ETier::Simple:   return "simple";
        case ETier::Standard: return "standard";
        case ETier::Strict:   return "strict";
    }
    return "standard";
}

inline QString ToString(ETaskStatus a_eStatus) {
    switch (a_eStatus) {
        case ETaskStatus::Pending: return QStringLiteral("待运行");
        case ETaskStatus::Running: return QStringLiteral("运行中");
        case ETaskStatus::Paused:  return QStringLiteral("已暂停");
        case ETaskStatus::Stopped: return QStringLiteral("已停止");
        case ETaskStatus::Done:    return QStringLiteral("已完成");
        case ETaskStatus::Failed:  return QStringLiteral("失败");
    }
    return {};
}
