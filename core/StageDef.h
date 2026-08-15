#pragma once

#include <QVector>
#include <QString>
#include <QStringList>
#include <QList>

#include "TaskTypes.h"

// 单个阶段的元数据：驱动 prompt 加载、工具集、IO 文件
struct SStageDef {
    EStage      eStage;
    QString     sId;          // 英文 id：intake / design ...
    QString     sNameZh;      // 中文名：需求收集 / 设计 ...
    bool        bFeature;     // 是否属于 Feature 流程
    bool        bBugfix;      // 是否属于 Bugfix 流程
    QString     sTools;       // 该阶段禁用的工具集（deny 黑名单 → --disallowedTools；bypass 模式下仍生效）
    QString     sPromptFile;  // prompts 目录下的模板文件名
    QStringList aInputs;      // 输入文件（相对任务私有目录 .autodev/tasks/<id>/）
    QString     sOutput;      // 输出文件名（空表示无落盘文件，如 develop 直接改业务代码）
};

// 13 阶段定义表
const QVector<SStageDef>& GetStageDefs();

// 按枚举 / id 查找
const SStageDef* FindStage(EStage a_eStage);
const SStageDef* FindStageById(const QString& a_sId);

// —— 自定义阶段清单（2026-08-15 流程档位）：清单是权威，档位只是预设 ——
// 解析任务的生效阶段流：a_aCustom 为空 → 返回该类型完整规范流（向后兼容）；
//   非空 → 按规范流顺序过滤出勾选的阶段（忽略非法值），Intake 为入口阶段强制包含在首位。
QList<EStage> ResolveFlow(ETaskType a_eType, const QList<EStage>& a_aCustom);

// 按生效流过滤某阶段的输入文件：aInputs 中仅保留「产物由流内阶段产出」的项
//   （产物 → 产出阶段反查 StageDef.sOutput；guide.md 等非阶段产物恒保留）。
//   过滤后为空 → 兜底 requirements.md（若 ConfirmReq 在流内）否则 intake.md，
//   保证任何勾选组合下阶段至少有一份有效输入、EnsureStageInputs 不弹回。
//   返回的 a_bDropped 回带被滤掉的输入名（供编排器注入「流水线说明」）。
QStringList FilterInputs(const SStageDef& a_stDef, const QList<EStage>& a_aFlow,
                         QStringList* a_pDropped = nullptr);

// 解析 prompts 目录（兼容开发期 exe 在 Execute/ 子目录 与 部署期 exe 同级）
QString GetPromptsDir();

// 工具集（deny 黑名单）：--dangerously-skip-permissions(bypass) 下 allow 白名单失效、deny 仍生效
//   （实测：CodeReview 白名单无 Bash 却跑了 7 次 Bash；改 deny 后 agent 明确报告无 Bash）。
//   故各阶段用 --disallowedTools 显式禁用「不该有」的工具，而非列举允许的工具。
QString ToolsDocs();    // 禁 "Bash,Edit"——文档/评审/分析：读代码+写报告，不改业务代码、不跑命令
QString ToolsDev();     // 禁 ""——开发：全功能（读写改+命令）
QString ToolsTest();    // 禁 "Edit"——测试：可跑命令/写测试，但不改业务代码
QString ToolsVerify();  // 禁 ""——验证：不低于 Develop（插/清调试、编译、运行、截图视觉）
