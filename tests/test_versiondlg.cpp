// CVersionDialog 纯函数 + agent 层版本解析 API 离线单元测试（不联网、不弹窗、
// 不构造对话框，挂 ctest）。任务 23183ccb T6 六组用例（迭代 1 补齐 ④~⑥）：
//   1. PickLatestTag：核心降序 → 预发布等级降序（无>rc>beta>alpha>dev）→ 后缀
//      自然数降序（rc.10>rc.9 数值序）；"<sha>\trefs/tags/<tag>" 行形态；^{} 尾注
//      行滤除；裸 tag 名兼容；全无效→空串。
//   2. TagToVersion：dsh-/v 前缀剥离；无前缀原样；无效输入原样返回。
//   3. Verdict：相等→已是最新；backend==4→锁定文案；latest 空→未查询；
//      可解析且更新→有新版本；其余不等→与远端不同。
//   4. ResolveDshVersion：QTemporaryDir 四态（有效/缺 version 字段/坏 JSON/
//      路径不存在）+ 静态根空参分派（SetDshRootPath 先存后还原）。
//   5. ResolvePiVersion：临时根造 pi/packages/coding-agent/package.json；
//      a_sRoot 直读 + SetPiRootPath 形态分派（先存后无条件还原）。
//   6. ParseClaudeVersionOutput："2.1.240 (Claude Code)"→2.1.240 等形态。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include "agent/CClaudeRunner.h"
#include "agent/CDeepSeekRunner.h"
#include "agent/CPiAgentRunner.h"
#include "ui/CVersionDialog.h"

static QTextStream g_out(stdout);
static int g_iFail = 0;

static void Check(bool a_bCond, const QString& a_sWhat) {
    if (!a_bCond) {
        ++g_iFail;
        g_out << "  [FAIL] " << a_sWhat << Qt::endl;
    }
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // —— 1. PickLatestTag ——
    {
        // 乱序集合：0.1.1 核心胜 0.1.0；同核心 rc(4) 胜 alpha(2)。
        Check(CVersionDialog::PickLatestTag({
                  QStringLiteral("dsh-v0.1.0-rc.1"),
                  QStringLiteral("dsh-v0.1.0"),
                  QStringLiteral("dsh-v0.1.1-alpha.3"),
                  QStringLiteral("dsh-v0.1.1-rc.7"),
              }) == QStringLiteral("dsh-v0.1.1-rc.7"),
              QStringLiteral("乱序集合应选 dsh-v0.1.1-rc.7"));
        // 同核心同等级：后缀自然数序 rc.10 > rc.9（字符串序会错判）。
        Check(CVersionDialog::PickLatestTag({
                  QStringLiteral("dsh-v0.2.0-rc.9"),
                  QStringLiteral("dsh-v0.2.0-rc.10"),
              }) == QStringLiteral("dsh-v0.2.0-rc.10"),
              QStringLiteral("rc.10 应胜 rc.9（数值序）"));
        // 同核心：无后缀（正式版）> rc 预发布。
        Check(CVersionDialog::PickLatestTag({
                  QStringLiteral("dsh-v0.3.0-rc.5"),
                  QStringLiteral("dsh-v0.3.0"),
              }) == QStringLiteral("dsh-v0.3.0"),
              QStringLiteral("正式版应胜 rc 预发布"));
        // 核心版本多数值段：0.1.10 > 0.1.9（QVersionNumber 数值语义）。
        Check(CVersionDialog::PickLatestTag({
                  QStringLiteral("dsh-v0.1.9"),
                  QStringLiteral("dsh-v0.1.10"),
              }) == QStringLiteral("dsh-v0.1.10"),
              QStringLiteral("0.1.10 应胜 0.1.9（数值段序）"));
        // ls-remote 真实输出形态："<sha>\trefs/tags/<tag>" + ^{} 尾注行滤除。
        Check(CVersionDialog::PickLatestTag({
                  QStringLiteral("a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2\trefs/tags/dsh-v0.1.0"),
                  QStringLiteral("f6e5d4c3b2a1f6e5d4c3b2a1f6e5d4c3b2a1f6e5d4\trefs/tags/dsh-v0.1.2-alpha.3"),
                  QStringLiteral("112233445566778899aabbccddeeff0011223344\trefs/tags/dsh-v0.1.2-alpha.3^{}"),
              }) == QStringLiteral("dsh-v0.1.2-alpha.3"),
              QStringLiteral("ls-remote 形态应剥 sha 前缀选 dsh-v0.1.2-alpha.3（^{} 行滤除）"));
        // 裸 tag 名（无 sha/refs 前缀）兼容。
        Check(CVersionDialog::PickLatestTag({QStringLiteral("v1.2.3")})
                  == QStringLiteral("v1.2.3"),
              QStringLiteral("裸 tag 名应原样可解析"));
        // 全无效（空行/非版本 tag）→ 空串。
        Check(CVersionDialog::PickLatestTag({
                  QString(),
                  QStringLiteral("refs/tags/not-a-version"),
                  QStringLiteral("random"),
              }).isEmpty(),
              QStringLiteral("全无效输入应返回空串"));
    }

    // —— 2. TagToVersion ——
    {
        Check(CVersionDialog::TagToVersion(QStringLiteral("dsh-v0.1.2-alpha.3"))
                  == QStringLiteral("0.1.2-alpha.3"),
              QStringLiteral("dsh-v 前缀应剥到 0.1.2-alpha.3"));
        Check(CVersionDialog::TagToVersion(QStringLiteral("v1.2.3"))
                  == QStringLiteral("1.2.3"),
              QStringLiteral("v 前缀应剥到 1.2.3"));
        Check(CVersionDialog::TagToVersion(QStringLiteral("1.2.3"))
                  == QStringLiteral("1.2.3"),
              QStringLiteral("无前缀原样返回"));
        Check(CVersionDialog::TagToVersion(QStringLiteral("banana"))
                  == QStringLiteral("banana"),
              QStringLiteral("无效输入原样返回"));
    }

    // —— 3. Verdict ——
    {
        Check(CVersionDialog::Verdict(QStringLiteral("0.84.3"),
                                      QStringLiteral("0.84.3"), 3)
                  == QStringLiteral("已是最新"),
              QStringLiteral("相等应判已是最新"));
        Check(CVersionDialog::Verdict(QStringLiteral("0.84.3"),
                                      QStringLiteral("0.84.4"), 3)
                  == QStringLiteral("有新版本 0.84.4"),
              QStringLiteral("可解析且更新应判有新版本"));
        Check(CVersionDialog::Verdict(QStringLiteral("0.87.0"),
                                      QStringLiteral("0.151.0"), 4)
                  == QStringLiteral("已锁定 0.87.0（v0.88+ 已移除 wire_api=chat，不提供升级）"),
              QStringLiteral("backend=4 应恒显锁定文案"));
        Check(CVersionDialog::Verdict(QStringLiteral("0.1.0"), QString(), 2)
                  == QStringLiteral("未查询"),
              QStringLiteral("latest 空应判未查询"));
        Check(CVersionDialog::Verdict(QString(), QString(), 3)
                  == QStringLiteral("未查询"),
              QStringLiteral("双侧空应判未查询"));
        Check(CVersionDialog::Verdict(QStringLiteral("banana"),
                                      QStringLiteral("apple"), 2)
                  == QStringLiteral("与远端不同：banana → apple"),
              QStringLiteral("不可解析且不等应判与远端不同"));
        // 2026.09.01 DSH 上限锁：本地超基线→不兼容指引回退；远端超基线→不算有新版本。
        Check(CVersionDialog::Verdict(QStringLiteral("0.1.2-alpha.3"),
                                      QStringLiteral("0.1.2-alpha.3"), 2)
                  .contains(QStringLiteral("不兼容")),
              QStringLiteral("DSH 本地超基线应判不兼容并指引回退"));
        Check(CVersionDialog::Verdict(QStringLiteral("0.1.0"),
                                      QStringLiteral("0.1.2-alpha.3"), 2)
                  .contains(QStringLiteral("不受支持")),
              QStringLiteral("DSH 远端超基线应判不受支持而非有新版本"));
        Check(CVersionDialog::Verdict(QStringLiteral("0.1.0"),
                                      QStringLiteral("0.1.0"), 2)
                  == QStringLiteral("已是最新"),
              QStringLiteral("DSH 本地==远端==基线仍应判已是最新"));
        // 2026.09.21 Hermes 发布 tag 版本号比对（同其余 Harness 发布语义）：
        //   local=git describe 日历版本核心，latest=远端最大发布 tag 核心；
        //   HEAD sha 不再进结论。附：PickLatestHermesTag / ParseHermesDescribe 纯函数。
        const QString sShaTag = QStringLiteral("29112bef099274229cadff79cdff7bf7b99c4b77");
        const QStringList aTagLines = {
            QStringLiteral("9d05f3721d69df570fde4c9378d5ed252f53ca88\trefs/tags/backup/opentui-prestrip-20260616-1950"),
            QStringLiteral("a08725e52a17964b3c91b12fa434f4cba45c4521\trefs/tags/premerge-oh-god"),
            QStringLiteral("9949d0d324a3a06ac238e01dd1c2103dbca09900\trefs/tags/v2026.9.7"),
            QStringLiteral("c3f1b2d0e4a5968778695a4b3c2d1e0f9a8b7c6d\trefs/tags/v2026.9.14"),
            QStringLiteral("%1\trefs/tags/v2026.9.14^{}").arg(sShaTag),
            QStringLiteral("86960cdbb0148145890e2ee90b4e157fa899f6e1\trefs/tags/v2026.4.8^{}"),
            QStringLiteral("not-a-sha\trefs/tags/v2026.99.99"),
        };
        QString sPickedSha;
        Check(CVersionDialog::PickLatestHermesTag(aTagLines, &sPickedSha)
                  == QStringLiteral("2026.9.14"),
              QStringLiteral("Hermes tag 列表应取最大版本并排除杂项/坏行"));
        Check(sPickedSha == sShaTag,
              QStringLiteral("附注 tag 应取 ^{} 剥离行的 commit sha"));
        Check(CVersionDialog::PickLatestHermesTag({QStringLiteral("垃圾行")}, nullptr).isEmpty(),
              QStringLiteral("无有效 tag 应返回空"));
        Check(CVersionDialog::ParseHermesDescribe(
                  QStringLiteral("v2026.9.14-4881-gc4f5dd0dad"))
                  == QStringLiteral("2026.9.14"),
              QStringLiteral("describe 输出应截断 -N-g 后缀"));
        Check(CVersionDialog::ParseHermesDescribe(QStringLiteral("v0.21.3"))
                  == QStringLiteral("0.21.3"),
              QStringLiteral("describe 纯 tag 名应去 v 前缀"));
        Check(CVersionDialog::ParseHermesDescribe(QString()).isEmpty(),
              QStringLiteral("describe 空输出应返回空"));
        Check(CVersionDialog::Verdict(QStringLiteral("2026.9.14"),
                  QStringLiteral("2026.9.14"), 5)
                  == QStringLiteral("已是最新（v2026.9.14）"),
              QStringLiteral("Hermes 本地==远端发布版应判已最新（纯版本号）"));
        Check(CVersionDialog::Verdict(QStringLiteral("2026.9.7"),
                  QStringLiteral("2026.9.14"), 5, QStringLiteral("10:28"))
                  == QStringLiteral("有新版本 v2026.9.14（本地上次同步 10:28）"),
              QStringLiteral("Hermes 落后应判有新版本并附同步时刻尾注"));
        Check(CVersionDialog::Verdict(QStringLiteral("2026.9.7"),
                  QStringLiteral("2026.9.14"), 5)
                  == QStringLiteral("有新版本 v2026.9.14"),
              QStringLiteral("Hermes 落后无同步时刻时应无尾注"));
        Check(CVersionDialog::Verdict(QString(), QStringLiteral("2026.9.14"), 5)
                  == QStringLiteral("远端最新发布 v2026.9.14（本地非 git 部署，无法比对）"),
              QStringLiteral("Hermes 非 git 部署应明示无法比对（发布版口径）"));
        Check(CVersionDialog::Verdict(QStringLiteral("2026.9.14"), QString(), 5)
                  == QStringLiteral("未查询"),
              QStringLiteral("Hermes latest 空仍判未查询"));
        Check(CVersionDialog::Verdict(QStringLiteral("2026.10.1"),
                  QStringLiteral("2026.9.14"), 5)
                  == QStringLiteral("与远端不同：2026.10.1 → 2026.9.14"),
              QStringLiteral("Hermes 本地>远端（查询竞态）应落通用与远端不同"));
    }

    // —— 4. ResolveDshVersion（QTemporaryDir 四态 + 静态根空参分派）——
    {
        QTemporaryDir stTmp;
        Check(stTmp.isValid(), QStringLiteral("临时目录应可用"));
        const QString sRoot = stTmp.path();
        const auto fnWritePkg = [&sRoot](const QString& a_sJson) -> bool {
            QFile stFile(sRoot + QStringLiteral("/package.json"));
            if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return false;
            }
            stFile.write(a_sJson.toUtf8());
            stFile.close();
            return true;
        };
        // 态①：有效 package.json → 读出 version。
        Check(fnWritePkg(QStringLiteral("{\"name\":\"dsh\",\"version\":\"0.1.0-rc.7\"}")),
              QStringLiteral("写入有效 package.json 应成功"));
        Check(CDeepSeekRunner::ResolveDshVersion(sRoot)
                  == QStringLiteral("0.1.0-rc.7"),
              QStringLiteral("有效 package.json 应读出 0.1.0-rc.7"));
        // 态②：缺 version 字段 → 空串。
        Check(fnWritePkg(QStringLiteral("{\"name\":\"dsh\"}")),
              QStringLiteral("写入无 version 字段 package.json 应成功"));
        Check(CDeepSeekRunner::ResolveDshVersion(sRoot).isEmpty(),
              QStringLiteral("缺 version 字段应返回空串"));
        // 态③：坏 JSON → 空串。
        Check(fnWritePkg(QStringLiteral("{not-json")),
              QStringLiteral("写入坏 JSON 应成功"));
        Check(CDeepSeekRunner::ResolveDshVersion(sRoot).isEmpty(),
              QStringLiteral("坏 JSON 应返回空串"));
        // 态④：路径不存在 → 空串（不回退不崩溃）。
        Check(CDeepSeekRunner::ResolveDshVersion(
                  sRoot + QStringLiteral("/nope")).isEmpty(),
              QStringLiteral("不存在的根应返回空串"));
        // 空参分派：静态根有效→读静态根；静态根空→空串（先存后无条件还原）。
        Check(fnWritePkg(QStringLiteral("{\"version\":\"0.2.0\"}")),
              QStringLiteral("回写有效 package.json 应成功"));
        const QString sSavedDsh = CDeepSeekRunner::DshRootPath();
        CDeepSeekRunner::SetDshRootPath(sRoot);
        Check(CDeepSeekRunner::ResolveDshVersion() == QStringLiteral("0.2.0"),
              QStringLiteral("空参应分派到静态根读 0.2.0"));
        CDeepSeekRunner::SetDshRootPath(QString());
        Check(CDeepSeekRunner::ResolveDshVersion().isEmpty(),
              QStringLiteral("静态根空时空参应返回空串"));
        CDeepSeekRunner::SetDshRootPath(sSavedDsh);   // 无条件还原
        Check(CDeepSeekRunner::DshRootPath() == sSavedDsh,
              QStringLiteral("DshRootPath 应已还原"));
    }

    // —— 5. ResolvePiVersion（临时根直读 + SetPiRootPath 形态分派，先存后还原）——
    {
        QTemporaryDir stTmp;
        Check(stTmp.isValid(), QStringLiteral("临时目录应可用"));
        const QString sRoot = stTmp.path();
        const QString sPkgDir = sRoot + QStringLiteral("/pi/packages/coding-agent");
        Check(QDir().mkpath(sPkgDir), QStringLiteral("造源码布局应成功"));
        {
            QFile stFile(sPkgDir + QStringLiteral("/package.json"));
            Check(stFile.open(QIODevice::WriteOnly),
                  QStringLiteral("打开根包 package.json 应成功"));
            stFile.write(QStringLiteral("{\"version\":\"9.9.9\"}").toUtf8());
            stFile.close();
        }
        // a_sRoot 非空：直读该根（miss 不回退）。
        Check(CPiAgentRunner::ResolvePiVersion(sRoot) == QStringLiteral("9.9.9"),
              QStringLiteral("临时根直读应得 9.9.9"));
        Check(CPiAgentRunner::ResolvePiVersion(
                  sRoot + QStringLiteral("/nope")).isEmpty(),
              QStringLiteral("不存在的根应返回空串"));
        // 空参分派：SetPiRootPath 指临时根→Source 形态命中（不触 npm 探测）。
        const QString sSavedPi = CPiAgentRunner::PiRootPath();
        CPiAgentRunner::SetPiRootPath(sRoot);
        Check(CPiAgentRunner::DetectPiForm() == CPiAgentRunner::EPiFormSource,
              QStringLiteral("临时根应判 Source 形态"));
        Check(CPiAgentRunner::ResolvePiVersion() == QStringLiteral("9.9.9"),
              QStringLiteral("Source 形态空参应读根包 9.9.9"));
        CPiAgentRunner::SetPiRootPath(sSavedPi);   // 无条件还原
        Check(CPiAgentRunner::PiRootPath() == sSavedPi,
              QStringLiteral("PiRootPath 应已还原"));
    }

    // —— 6. ParseClaudeVersionOutput ——
    {
        Check(CClaudeRunner::ParseClaudeVersionOutput(
                  QStringLiteral("2.1.240 (Claude Code)"))
                  == QStringLiteral("2.1.240"),
              QStringLiteral("实测形态应解析出 2.1.240"));
        Check(CClaudeRunner::ParseClaudeVersionOutput(
                  QStringLiteral("claude-cli 1.2.34 (Claude Code 2.x)"))
                  == QStringLiteral("1.2.34"),
              QStringLiteral("带前缀形态应解析出 1.2.34"));
        Check(CClaudeRunner::ParseClaudeVersionOutput(
                  QStringLiteral("2.1.240.1 (x)"))
                  == QStringLiteral("2.1.240.1"),
              QStringLiteral("四段版本应原样取全"));
        Check(CClaudeRunner::ParseClaudeVersionOutput(QString()).isEmpty(),
              QStringLiteral("空串应返回空"));
        Check(CClaudeRunner::ParseClaudeVersionOutput(
                  QStringLiteral("no version here")).isEmpty(),
              QStringLiteral("无版本串应返回空"));
    }

    // —— 7. BuildAutoFixPrompt（2026.10.21 后端自愈提示词组装）——
    {
        const QString sPrompt = CVersionDialog::BuildAutoFixPrompt(
            2, QStringLiteral("DeepSeekHarness"),
            QStringLiteral("E:/fake/dsh-root"),
            QStringLiteral("initialize 被拒：no adapter registered"),
            QStringLiteral("C:/fake/exe-dir/harness.md"));
        Check(sPrompt.contains(QStringLiteral("DeepSeekHarness 后端出现故障")),
              QStringLiteral("提示词应含故障后端与「出现故障」"));
        Check(sPrompt.contains(QStringLiteral("no adapter registered")),
              QStringLiteral("提示词应带探测失败详情"));
        Check(sPrompt.contains(QStringLiteral("E:/fake/dsh-root")),
              QStringLiteral("提示词应带安装位置"));
        Check(sPrompt.contains(QStringLiteral("C:/fake/exe-dir/harness.md")),
              QStringLiteral("提示词应带 harness.md 绝对路径"));
        Check(sPrompt.contains(QStringLiteral("GLM_CUSTOM_API_KEY")),
              QStringLiteral("DSH 专属提示应含环境键线索"));
        // 空详情/空位置兑底：不产生空段，且仍引用 harness.md。
        const QString sBare = CVersionDialog::BuildAutoFixPrompt(
            1, QStringLiteral("Claude Code"), QString(), QString(),
            QStringLiteral("C:/fake/harness.md"));
        Check(sBare.contains(QStringLiteral("可用性验证未通过")),
              QStringLiteral("空详情应落「可用性验证未通过」兑底"));
        Check(sBare.contains(QStringLiteral("按 harness.md 探测")),
              QStringLiteral("空位置应落探测指引兑底"));
        Check(!sBare.contains(QStringLiteral("GLM_CUSTOM_API_KEY")),
              QStringLiteral("Claude 行不应混入 DSH 专属提示"));
    }

    if (g_iFail == 0) {
        g_out << "test_versiondlg: ALL PASSED" << Qt::endl;
        return 0;
    }
    g_out << "test_versiondlg: " << g_iFail << " FAILED" << Qt::endl;
    return 1;
}
