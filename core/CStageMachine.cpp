#include "CStageMachine.h"

#include "StageDef.h"

namespace {
// Feature 流：1→2→3→4→5→6→7→8→9→14→10（CodeReview 后、Test 前插入 Verify=14）
const EStage g_aFeature[] = {
    EStage::Intake, EStage::ConfirmReq, EStage::ReviewReq,
    EStage::Design, EStage::ReviewDesign,
    EStage::Plan, EStage::ReviewPlan,
    EStage::Develop, EStage::CodeReview, EStage::Verify, EStage::Test
};
// Bugfix 流：1→2→11→12→13→9→14→10（CodeReview 后、Test 前插入 Verify=14）
const EStage g_aBugfix[] = {
    EStage::Intake, EStage::ConfirmReq,
    EStage::RootCause, EStage::RootCauseVerify, EStage::BugFix,
    EStage::CodeReview, EStage::Verify, EStage::Test
};

constexpr int g_nFeature = int(sizeof(g_aFeature) / sizeof(EStage));
constexpr int g_nBugfix  = int(sizeof(g_aBugfix) / sizeof(EStage));
}  // namespace

bool CStageMachine::StageInFlow(ETaskType a_eType, EStage a_eStage) {
    const QList<EStage> aFlow = GetFlowStages(a_eType);
    return aFlow.contains(a_eStage);
}

EStage CStageMachine::FirstStage(ETaskType /*a_eType*/) {
    // 两种任务类型都从 intake 开始
    return EStage::Intake;
}

QList<EStage> CStageMachine::GetFlowStages(ETaskType a_eType) {
    QList<EStage> aOut;
    const EStage* pFlow = (a_eType == ETaskType::Feature) ? g_aFeature : g_aBugfix;
    const int iCount    = (a_eType == ETaskType::Feature) ? g_nFeature : g_nBugfix;
    for (int i = 0; i < iCount; ++i) {
        aOut.append(pFlow[i]);
    }
    return aOut;
}

std::optional<EStage> CStageMachine::NextStage(ETaskType a_eType, EStage a_eCur) {
    const EStage* pFlow = (a_eType == ETaskType::Feature) ? g_aFeature : g_aBugfix;
    const int iCount    = (a_eType == ETaskType::Feature) ? g_nFeature : g_nBugfix;

    for (int i = 0; i < iCount; ++i) {
        if (pFlow[i] == a_eCur) {
            if (i + 1 < iCount) {
                return pFlow[i + 1];
            }
            return std::nullopt;  // 已是末阶段 → 任务完成
        }
    }
    return pFlow[0];  // 未找到（不应发生）→ 回首阶段
}

std::optional<EStage> CStageMachine::NextStage(const QList<EStage>& a_aFlow, EStage a_eCur) {
    if (a_aFlow.isEmpty()) {
        return EStage::Intake;   // 空流非法（不应发生）→ 回首阶段
    }
    const int iIdx = a_aFlow.indexOf(a_eCur);
    if (iIdx < 0) {
        return a_aFlow.first();  // 不在流内（不应发生）→ 回首阶段
    }
    if (iIdx + 1 < a_aFlow.size()) {
        return a_aFlow.at(iIdx + 1);
    }
    return std::nullopt;   // 已是末阶段 → 任务完成
}

std::optional<EStage> CStageMachine::PrevStage(const QList<EStage>& a_aFlow, EStage a_eCur) {
    const int iIdx = a_aFlow.indexOf(a_eCur);
    if (iIdx <= 0) {
        return std::nullopt;   // 首阶段 / 不在流内 → 无前一阶段
    }
    return a_aFlow.at(iIdx - 1);
}

EStage CStageMachine::RejectTarget(EStage a_eReviewStage) {
    switch (a_eReviewStage) {
        case EStage::ReviewReq:        return EStage::ConfirmReq;   // 需求评审 → 回确认需求
        case EStage::ReviewDesign:     return EStage::Design;       // 设计评审 → 回设计
        case EStage::ReviewPlan:       return EStage::Plan;         // 计划评审 → 回计划
        case EStage::RootCauseVerify:  return EStage::RootCause;    // 根因核实 → 回根因分析
        case EStage::CodeReview:       return EStage::Develop;      // 代码审查 → 回开发
        case EStage::Test:             return EStage::Develop;      // 测试失败 → 回开发
        default:                       return a_eReviewStage;
    }
}
