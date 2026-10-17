// test_orchconfig —— 任务编排设计（2026-10-14 功能菜单「任务编排设计」）核心链路回归：
//   COrchConfig 种子/落盘/加载/动态注册 + ResolveFlow 配置序保持 + 自定义阶段
//   （EStage≥100）流解析 + SeedDefaults 不回写动态阶段（曾致 deploy 被当内置
//   持久化、文件自污染 15 阶段）。
// 运行期写 <exe>/prompts/orchestration.json，结尾还原默认编排（14 阶段无自定义），
//   不影响后续测试；开头删除残留文件，保证「缺文件自动种子」路径每次可回归。
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QDir>

#include "core/COrchConfig.h"
#include "core/StageDef.h"
#include "core/CStageMachine.h"

static int s_iFail = 0;
#define CHECK(cond, msg) \
    do { if (cond) { qDebug() << "PASS:" << msg; } \
         else { qDebug() << "FAIL:" << msg; ++s_iFail; } } while (0)

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // ⓪ 清残留（上次异常中断可能留下带自定义阶段的文件），保证种子路径可回归
    QFile::remove(COrchConfig::ConfigPath());

    // ① 单例加载：缺文件自动种子默认编排 + 落盘
    COrchConfig* p = COrchConfig::Instance();
    CHECK(p->aStages.size() == 14, "seed stages = 14 builtin");
    CHECK(p->aTypes.size() == 2, "seed types = feature/bugfix");
    CHECK(p->aTiers.size() == 3, "seed tiers = simple/standard/strict");
    CHECK(QFile::exists(COrchConfig::ConfigPath()), "orchestration.json created");
    const SOrchType* pF0 = p->FindTypeCfg("feature");
    CHECK(pF0 != nullptr && pF0->aStages.size() == 11, "feature flow = 11 stages (含验证)");
    CHECK(pF0 != nullptr && pF0->aStages.first() == "intake" && pF0->aStages.last() == "test",
          "feature flow order intake..test");

    // ② 动态注册前：自定义枚举不可见；注册一个自定义阶段
    SOrchStage stCustom;
    stCustom.sId = "deploy"; stCustom.sName = QStringLiteral("部署");
    stCustom.sPromptFile = "15_deploy.md";
    stCustom.bBuiltin = false; stCustom.bDefault = true;
    stCustom.iEnum = p->NextCustomEnum();
    CHECK(stCustom.iEnum == 100, "first custom enum = 100");
    stCustom.sOutput = "deploy.md";
    p->aStages.append(stCustom);
    // feature 类型流尾部追加 deploy
    SOrchType* pF = p->FindTypeCfg("feature");
    pF->aStages.append("deploy");
    CHECK(p->Save(), "save config");
    p->ApplyToRuntime();
    CHECK(FindStage(static_cast<EStage>(100)) != nullptr
              && FindStage(static_cast<EStage>(100))->sNameZh == QStringLiteral("部署"),
          "dynamic stage registered");
    CHECK(FindStageById("deploy") != nullptr, "find custom by id");

    // ③ 加载重读（模拟重启）+ 动态阶段可见
    COrchConfig* p2 = COrchConfig::Instance();
    CHECK(p2->FindStageCfg("deploy") != nullptr, "custom stage persisted");
    CHECK(FindStage(static_cast<EStage>(100)) != nullptr, "registered after reload");

    // ④ ResolveFlow：配置序保持（Intake 首、去重、未知值剔除）
    const QList<EStage> aFlow = ResolveFlow(ETaskType::Feature,
        { EStage::Intake, EStage(static_cast<EStage>(100)), EStage::Develop,
          EStage::Develop, static_cast<EStage>(999) });
    CHECK(aFlow.size() == 3
              && aFlow.at(0) == EStage::Intake
              && aFlow.at(1) == static_cast<EStage>(100)
              && aFlow.at(2) == EStage::Develop,
          "resolve keeps config order, dedups, drops unknown");

    // ⑤ 自定义阶段定义字段 roundtrip（输出 deploy.md → FilterInputs 生产者映射）
    {
        const SStageDef* pD = FindStage(static_cast<EStage>(100));
        CHECK(pD != nullptr && pD->sPromptFile == "15_deploy.md"
                  && pD->sOutput == "deploy.md", "custom def fields roundtrip");
    }

    // ⑥ 还原默认编排（不污染部署目录配置）：SeedDefaults 只认内置 1~14——
    //    动态表里已注册 deploy 时也不得把它当内置种回去（文件自污染回归锚点）
    p2->SeedDefaults();
    p2->Save();
    p2->ApplyToRuntime();
    CHECK(FindStage(static_cast<EStage>(100)) == nullptr, "unregistered after restore");

    // ⑦ 终态复核：重读文件 = 14 阶段、无 deploy、feature 流 11（幂等干净收尾）
    CHECK(p2->Load(), "reload after restore");
    CHECK(p2->aStages.size() == 14
              && p2->FindStageCfg("deploy") == nullptr, "file clean after restore");
    CHECK(p2->FindTypeCfg("feature")->aStages.size() == 11, "feature flow restored");

    qDebug() << (s_iFail == 0 ? "ALL PASS" : "HAS FAILURES");
    return s_iFail == 0 ? 0 : 1;
}
