#pragma once

#include <optional>
#include <QList>

#include "TaskTypes.h"

// 纯静态状态机：决定阶段流转与驳回路由。无状态、可独立测试。
class CStageMachine {
public:
    static constexpr int k_iMaxIter = 3;  // 单阶段最多重跑次数，超过升级到用户

    // 正常流转的下一阶段；返回 std::nullopt 表示任务完成（无下一阶段）
    static std::optional<EStage> NextStage(ETaskType a_eType, EStage a_eCur);

    // —— 自定义阶段清单（2026-08-15 流程档位）：基于 ResolveFlow 结果的流转 ——
    // 在 a_aFlow 中找 a_eCur 的下一阶段；末阶段 → nullopt（任务完成）；
    //   a_eCur 不在流内（不应发生）→回首阶段。空流视为非法，同样回首阶段。
    static std::optional<EStage> NextStage(const QList<EStage>& a_aFlow, EStage a_eCur);

    // 阶段 a_eCur 在 a_aFlow 中的前一个阶段；a_eCur 是首阶段或不在流内 → nullopt。
    //   评审驳回回退时用：回炉目标被用户取消勾选时，退到生阶段的前一个流内阶段。
    static std::optional<EStage> PrevStage(const QList<EStage>& a_aFlow, EStage a_eCur);

    // 评审驳回时回到的上游生产阶段
    static EStage RejectTarget(EStage a_eReviewStage);

    // 判断某阶段是否属于指定任务类型的流程
    static bool StageInFlow(ETaskType a_eType, EStage a_eStage);

    // 该阶段的起始阶段（按任务类型）
    static EStage FirstStage(ETaskType a_eType);

    // 返回该任务类型的完整阶段序列（按流程顺序）
    static QList<EStage> GetFlowStages(ETaskType a_eType);
};
