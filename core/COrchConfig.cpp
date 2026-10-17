#include "COrchConfig.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include "core/CStageMachine.h"
#include "core/StageDef.h"

COrchConfig* COrchConfig::s_pInst = nullptr;

COrchConfig* COrchConfig::Instance() {
    if (s_pInst == nullptr) {
        s_pInst = new COrchConfig();
        s_pInst->Load();   // 缺文件自动种子默认编排并注册动态阶段
    }
    return s_pInst;
}

QString COrchConfig::ConfigPath() {
    return GetPromptsDir() + QStringLiteral("/orchestration.json");
}

// —— 基础映射 ——
ETaskType COrchConfig::BaseToType(const QString& a_sBase) {
    return a_sBase.compare(QLatin1String("bugfix"), Qt::CaseInsensitive) == 0
               ? ETaskType::Bugfix : ETaskType::Feature;
}

ETier COrchConfig::BaseToTier(const QString& a_sBase) {
    if (a_sBase.compare(QLatin1String("simple"), Qt::CaseInsensitive) == 0) {
        return ETier::Simple;
    }
    if (a_sBase.compare(QLatin1String("strict"), Qt::CaseInsensitive) == 0) {
        return ETier::Strict;
    }
    return ETier::Standard;
}

// —— 查询辅助 ——
SOrchStage* COrchConfig::FindStageCfg(const QString& a_sId) {
    for (SOrchStage& s : aStages) {
        if (s.sId == a_sId) {
            return &s;
        }
    }
    return nullptr;
}

SOrchType* COrchConfig::FindTypeCfg(const QString& a_sId) {
    for (SOrchType& t : aTypes) {
        if (t.sId == a_sId) {
            return &t;
        }
    }
    return nullptr;
}

SOrchTier* COrchConfig::FindTierCfg(const QString& a_sId) {
    for (SOrchTier& t : aTiers) {
        if (t.sId == a_sId) {
            return &t;
        }
    }
    return nullptr;
}

int COrchConfig::EnumForStageId(const QString& a_sId) const {
    // 内置：静态表反查（1~14）；自定义：配置 iEnum（≥100）
    if (const SStageDef* pDef = FindStageById(a_sId)) {
        return static_cast<int>(pDef->eStage);
    }
    for (const SOrchStage& s : aStages) {
        if (s.sId == a_sId && !s.bBuiltin) {
            return s.iEnum;
        }
    }
    return 0;
}

QString COrchConfig::StageIdForEnum(int a_iEnum) const {
    if (const SStageDef* pDef = FindStage(static_cast<EStage>(a_iEnum))) {
        return pDef->sId;
    }
    for (const SOrchStage& s : aStages) {
        if (s.iEnum == a_iEnum && !s.bBuiltin) {
            return s.sId;
        }
    }
    return QString();
}

int COrchConfig::NextCustomEnum() const {
    int iMax = 99;
    for (const SOrchStage& s : aStages) {
        if (!s.bBuiltin && s.iEnum > iMax) {
            iMax = s.iEnum;
        }
    }
    return iMax + 1;
}

// —— 默认编排种子：内置 14 阶段 + feature/bugfix 两类型 + 三档位 ——
void COrchConfig::SeedDefaults() {
    aStages.clear();
    aTypes.clear();
    aTiers.clear();

    // 阶段：内置静态表全量（只取非动态项——若此前进程已注册自定义阶段，
    //   GetStageDefs 合表含动态项，种子只认内置 1~14，防自定义阶段被当内置回写）
    for (const SStageDef& stDef : GetStageDefs()) {
        if (IsDynamicStage(stDef.eStage)) {
            continue;
        }
        SOrchStage st;
        st.sId         = stDef.sId;
        st.sName       = stDef.sNameZh;
        st.sPromptFile = stDef.sPromptFile;
        st.bDefault    = true;
        st.bBuiltin    = true;
        st.iEnum       = static_cast<int>(stDef.eStage);
        aStages.append(st);
    }

    // 类型：规范流（表序）转 id 序列
    auto fnToIds = [](const QList<EStage>& a_aFlow) {
        QStringList aIds;
        for (EStage e : a_aFlow) {
            if (const SStageDef* pDef = FindStage(e)) {
                aIds.append(pDef->sId);
            }
        }
        return aIds;
    };
    {
        SOrchType stF;
        stF.sId     = QStringLiteral("feature");
        stF.sName   = QStringLiteral("feature（新功能）");
        stF.sBase   = QStringLiteral("feature");
        stF.aStages = fnToIds(CStageMachine::GetFlowStages(ETaskType::Feature));
        aTypes.append(stF);
        SOrchType stB;
        stB.sId     = QStringLiteral("bugfix");
        stB.sName   = QStringLiteral("bugfix（修缺陷）");
        stB.sBase   = QStringLiteral("bugfix");
        stB.aStages = fnToIds(CStageMachine::GetFlowStages(ETaskType::Bugfix));
        aTypes.append(stB);
    }

    // 档位：简单 = 全集 - {需求评审/设计/设计评审/计划/计划评审/根因核实}
    //   （与任务类型流取交集后即原 Simple 预设：feature 跳设计链、bugfix 跳根因核实）；
    //   标准/严格 = 全集（差异只在评审策略 base）。
    {
        QStringList aAll;
        for (const SOrchStage& s : aStages) {
            aAll.append(s.sId);
        }
        QStringList aSimple;
        const QSet<QString> setSkip = {
            QStringLiteral("review_requirements"), QStringLiteral("design"),
            QStringLiteral("review_design"),       QStringLiteral("plan"),
            QStringLiteral("review_plan"),         QStringLiteral("root_cause_verify")
        };
        for (const QString& sId : aAll) {
            if (!setSkip.contains(sId)) {
                aSimple.append(sId);
            }
        }
        auto fnTier = [&aAll](const QString& sId, const QString& sName,
                              const QString& sBase, const QStringList& aStages) {
            SOrchTier t;
            t.sId = sId; t.sName = sName; t.sBase = sBase; t.aStages = aStages;
            return t;
        };
        aTiers.append(fnTier(QStringLiteral("simple"),
                             QStringLiteral("简单（精简阶段，代码审查单视角，评审多数决）"),
                             QStringLiteral("simple"), aSimple));
        aTiers.append(fnTier(QStringLiteral("standard"),
                             QStringLiteral("标准（完整阶段，评审多数决）"),
                             QStringLiteral("standard"), aAll));
        aTiers.append(fnTier(QStringLiteral("strict"),
                             QStringLiteral("严格（完整阶段，评审一票否决）"),
                             QStringLiteral("strict"), aAll));
    }
    iVersion = 1;
}

// —— 动态注册：自定义阶段进运行期定义表 ——
void COrchConfig::ApplyToRuntime() {
    QVector<SStageDef> aDyn;
    for (const SOrchStage& s : aStages) {
        if (s.bBuiltin || s.iEnum < 100) {
            continue;
        }
        SStageDef d;
        d.eStage      = static_cast<EStage>(s.iEnum);
        d.sId         = s.sId;
        d.sNameZh     = s.sName;
        d.bFeature    = false;   // 双标志 false：不进任何规范流，仅经任务自定义流生效
        d.bBugfix     = false;
        d.sTools      = s.sTools;
        d.sPromptFile = s.sPromptFile;
        d.aInputs     = s.aInputs;
        d.sOutput     = s.sOutput;
        aDyn.append(d);
    }
    RegisterDynamicStages(aDyn);
}

// —— 持久化 ——
bool COrchConfig::Save() {
    // 目录兜底（部署目录 prompts 恒在；此处防御性确保）
    const QString sDir = GetPromptsDir();
    if (!QDir().mkpath(sDir)) {
        return false;
    }
    QJsonObject oRoot;
    oRoot[QStringLiteral("version")] = iVersion;

    QJsonArray aSt;
    for (const SOrchStage& s : aStages) {
        QJsonObject o;
        o[QStringLiteral("id")]      = s.sId;
        o[QStringLiteral("name")]    = s.sName;
        o[QStringLiteral("prompt")]  = s.sPromptFile;
        o[QStringLiteral("default")] = s.bDefault;
        o[QStringLiteral("builtin")] = s.bBuiltin;
        o[QStringLiteral("enum")]    = s.iEnum;
        if (!s.bBuiltin) {
            o[QStringLiteral("tools")]  = s.sTools;
            o[QStringLiteral("inputs")] = QJsonArray::fromStringList(s.aInputs);
            o[QStringLiteral("output")] = s.sOutput;
        }
        aSt.append(o);
    }
    oRoot[QStringLiteral("stages")] = aSt;

    QJsonArray aTy;
    for (const SOrchType& t : aTypes) {
        QJsonObject o;
        o[QStringLiteral("id")]     = t.sId;
        o[QStringLiteral("name")]   = t.sName;
        o[QStringLiteral("base")]   = t.sBase;
        o[QStringLiteral("stages")] = QJsonArray::fromStringList(t.aStages);
        aTy.append(o);
    }
    oRoot[QStringLiteral("types")] = aTy;

    QJsonArray aTi;
    for (const SOrchTier& t : aTiers) {
        QJsonObject o;
        o[QStringLiteral("id")]     = t.sId;
        o[QStringLiteral("name")]   = t.sName;
        o[QStringLiteral("base")]   = t.sBase;
        o[QStringLiteral("stages")] = QJsonArray::fromStringList(t.aStages);
        aTi.append(o);
    }
    oRoot[QStringLiteral("tiers")] = aTi;

    // 原子写（QSaveFile：写临时 + 提交改名，失败不破坏旧文件）；UTF-8 无 BOM。
    QSaveFile f(ConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    f.write(QJsonDocument(oRoot).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool COrchConfig::Load() {
    QFile f(ConfigPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SeedDefaults();
        ApplyToRuntime();
        Save();
        return true;
    }
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(f.readAll(), &stErr);
    f.close();
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        SeedDefaults();   // 损坏 → 回默认（旧文件不覆盖，保存时才覆盖）
        ApplyToRuntime();
        Save();
        return true;
    }
    const QJsonObject oRoot = oDoc.object();
    iVersion = oRoot.value(QStringLiteral("version")).toInt(1);

    aStages.clear();
    const QJsonArray aSt = oRoot.value(QStringLiteral("stages")).toArray();
    for (const auto& v : aSt) {
        const QJsonObject o = v.toObject();
        SOrchStage s;
        s.sId         = o.value(QStringLiteral("id")).toString();
        s.sName       = o.value(QStringLiteral("name")).toString();
        s.sPromptFile = o.value(QStringLiteral("prompt")).toString();
        s.bDefault    = o.value(QStringLiteral("default")).toBool(true);
        s.bBuiltin    = o.value(QStringLiteral("builtin")).toBool(false);
        s.iEnum       = o.value(QStringLiteral("enum")).toInt(0);
        s.sTools      = o.value(QStringLiteral("tools")).toString();
        const QJsonArray aIn = o.value(QStringLiteral("inputs")).toArray();
        for (const auto& iv : aIn) {
            s.aInputs.append(iv.toString());
        }
        s.sOutput     = o.value(QStringLiteral("output")).toString();
        if (!s.sId.isEmpty() && s.iEnum > 0) {
            aStages.append(s);
        }
    }

    aTypes.clear();
    const QJsonArray aTy = oRoot.value(QStringLiteral("types")).toArray();
    for (const auto& v : aTy) {
        const QJsonObject o = v.toObject();
        SOrchType t;
        t.sId   = o.value(QStringLiteral("id")).toString();
        t.sName = o.value(QStringLiteral("name")).toString();
        t.sBase = o.value(QStringLiteral("base")).toString(QStringLiteral("feature"));
        const QJsonArray aS = o.value(QStringLiteral("stages")).toArray();
        for (const auto& sv : aS) {
            t.aStages.append(sv.toString());
        }
        if (!t.sId.isEmpty()) {
            aTypes.append(t);
        }
    }

    aTiers.clear();
    const QJsonArray aTi = oRoot.value(QStringLiteral("tiers")).toArray();
    for (const auto& v : aTi) {
        const QJsonObject o = v.toObject();
        SOrchTier t;
        t.sId   = o.value(QStringLiteral("id")).toString();
        t.sName = o.value(QStringLiteral("name")).toString();
        t.sBase = o.value(QStringLiteral("base")).toString(QStringLiteral("standard"));
        const QJsonArray aS = o.value(QStringLiteral("stages")).toArray();
        for (const auto& sv : aS) {
            t.aStages.append(sv.toString());
        }
        if (!t.sId.isEmpty()) {
            aTiers.append(t);
        }
    }

    // 兜底：阶段表为空视为损坏 → 整体重种子；类型/档位单独清空仅补对应默认
    //   （用户可只保留自定义类型/档位，但删空时新建任务无可用选项，回默认更稳）。
    if (aStages.isEmpty()) {
        SeedDefaults();
    } else {
        COrchConfig stSeed;
        stSeed.SeedDefaults();
        if (aTypes.isEmpty()) {
            aTypes = stSeed.aTypes;
        }
        if (aTiers.isEmpty()) {
            aTiers = stSeed.aTiers;
        }
    }

    ApplyToRuntime();
    return true;
}
