#pragma once

// ============================================================================
// COrchConfig —— 任务编排配置（2026-10-14 功能菜单「任务编排设计」数据层）
// ----------------------------------------------------------------------------
// 需求：任务类型 / 流程档位 / 执行阶段全部可由用户编排（增删改 + 缺省选择 +
//   阶段提示词内容编辑），新建任务按编排后的最新版本显示与执行；新阶段提示词
//   仍存 prompts 目录。
// 落地：
//   - 配置持久化 <exe>/prompts/orchestration.json（随 Execute 目录整体拷贝便携）；
//   - 内置 14 阶段 / feature / bugfix / 三档位作为种子（首次缺文件自动生成）；
//   - 自定义阶段（EStage≥100）经 ApplyToRuntime → StageDef::RegisterDynamicStages
//     注册进运行期定义表，FindStage / FilterInputs / 提示词加载即刻可用；
//   - 单例：进程内一处可变（设计窗口直接编辑实例并即时落盘），新建任务对话框
//     每次打开读同实例 = 永远最新版本；跨进程靠 json 文件。
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

#include "core/TaskTypes.h"

// —— 单个执行阶段的编排条目 ——
struct SOrchStage {
    QString     sId;            // 英文 id（intake / custom_report ...，全表唯一）
    QString     sName;          // 中文显示名
    QString     sPromptFile;    // prompts 目录下提示词模板文件名
    bool        bDefault  = true;   // 「缺省选择」：档位未指定预设时该阶段是否默认勾选
    bool        bBuiltin  = true;   // 内置阶段（引擎字段不可改、不可删除）
    int         iEnum     = 0;      // EStage 值：内置 1~14；自定义自动分配 ≥100（持久化，任务流引用）
    // —— 引擎字段（自定义阶段生效；内置阶段以 StageDef 静态表为准，只读展示）——
    QString     sTools;         // --disallowedTools deny 黑名单（空=全功能）
    QStringList aInputs;        // 输入文件（相对任务私有目录）
    QString     sOutput;        // 输出文件名（空=无落盘产物，直接改业务代码）
};

// —— 任务类型（编排层显示名 + 基础引擎类型 + 有序阶段流）——
struct SOrchType {
    QString     sId;        // 英文 id（feature / custom_refactor ...）
    QString     sName;      // 显示名
    QString     sBase;      // 基础引擎类型："feature" / "bugfix"（决定驳回路由等引擎行为）
    QStringList aStages;    // 有序阶段 id 流（config 序即执行序）
};

// —— 流程档位（默认勾选预设 + 基础引擎档位）——
struct SOrchTier {
    QString     sId;        // 英文 id（simple / standard / strict ...）
    QString     sName;      // 显示名
    QString     sBase;      // 基础引擎档位："simple" / "standard" / "strict"（决定评审策略）
    QStringList aStages;    // 默认勾选阶段 id 集（与任务类型流取交集生效；空=按阶段 bDefault）
};

class COrchConfig {
public:
    // 进程内单例：首次调用自动 Load（缺文件则种子默认编排并落盘 + 注册动态阶段）。
    static COrchConfig* Instance();

    // 配置文件绝对路径：<exe>/prompts/orchestration.json。
    static QString ConfigPath();

    bool Load();     // 读 json；文件缺失/损坏 → SeedDefaults + Save
    bool Save();     // 写 json（UTF-8，无 BOM——与 prompts/*.md 同口径）
    void SeedDefaults();   // 从内置阶段表 + 规范流生成默认编排

    // 自定义阶段 → StageDef 动态注册表（启动 Load 后 / 设计窗口每次保存后调用）。
    void ApplyToRuntime();

    // —— 查询辅助 ——
    SOrchStage* FindStageCfg(const QString& a_sId);
    SOrchType*  FindTypeCfg(const QString& a_sId);
    SOrchTier*  FindTierCfg(const QString& a_sId);
    // 阶段 id → EStage 值（内置查静态表，自定义查配置；未知返回 0）
    int  EnumForStageId(const QString& a_sId) const;
    // EStage 值 → 阶段 id（反向；未知返回空串）
    QString StageIdForEnum(int a_iEnum) const;
    // 下一个自定义阶段 EStage 值（≥100，取现有自定义最大值 +1）
    int  NextCustomEnum() const;
    // 基础类型/档位字符串映射（未知回退 Feature / Standard）
    static ETaskType BaseToType(const QString& a_sBase);
    static ETier     BaseToTier(const QString& a_sBase);

    int  iVersion = 1;

    QList<SOrchStage> aStages;   // 全部阶段（含内置 + 自定义；表序即主序）
    QList<SOrchType>  aTypes;    // 任务类型清单
    QList<SOrchTier>  aTiers;    // 流程档位清单

private:
    COrchConfig() = default;
    static COrchConfig* s_pInst;
};
