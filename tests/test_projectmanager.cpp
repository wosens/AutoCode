#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTemporaryDir>   // 2026.09.09 v2.1.143 任务级配置覆盖用例
#include <QHeaderView>     // 2026.09.11 模型表列宽用例
#include <QTreeWidget>
#include <QWindow>          // 窗口级 QTest 鼠标事件（真实输入管线）
#include <QTabWidget>

#include <windows.h>       // WritePrivateProfileStringW（坏档构造，与生产代码同通道）

#include "project/CProjectManager.h"
#include "ui/CSettingsDialog.h"

// 最近打开工程历史（MRU）单元测试。
// 覆盖需求 AC-2/3/4/5/6/7/11/12 的可自动化部分（CProjectManager 的纯逻辑 + config.ini 往返）。
// UI 对话框 / 菜单交互（AC-1/8/9/10/13）依赖人工 GUI 操作，不在本自动化测试范围。
class CTestProjectManager : public QObject {
    Q_OBJECT

private:
    // 取 exe 同级 config.ini 全路径（与 CProjectManager::ConfigFilePath 实现一致，用于清理 / 校验）。
    static QString ConfigPath() {
        const QString sExe = QCoreApplication::applicationFilePath();
        return QDir::cleanPath(QFileInfo(sExe).absolutePath() + QStringLiteral("/config.ini"));
    }

    static void RemoveConfig() {
        QFile::remove(ConfigPath());
    }

private slots:
    void init() {
        RemoveConfig();
    }

    void cleanup() {
        RemoveConfig();
    }

    // AC-2：首次运行（无 config.ini）历史为空，不崩溃。
    void testEmptyOnFirstRun() {
        CProjectManager stPm;
        stPm.LoadRecent();
        QVERIFY(stPm.RecentProjects().isEmpty());
    }

    // AC-3：打开 A 后，历史最前为 A。
    void testSingleRecordedOnTop() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 1);
        QCOMPARE(aList.first(), QStringLiteral("C:/ProjA"));
    }

    // AC-4：先 A 后 B → 顺序为 B、A（MRU，最后打开最前）。
    void testMruOrdering() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        stPm.AddRecent(QStringLiteral("C:/ProjB"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/ProjB"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/ProjA"));
    }

    // AC-5：重复打开 A（已在历史中）→ A 置顶且只有一条 A（去重，不新增）。
    void testDedupPromoteToTop() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        stPm.AddRecent(QStringLiteral("C:/ProjB"));
        // 此时为 B、A；再打开 A → 应为 A、B，无重复 A。
        stPm.AddRecent(QStringLiteral("C:/ProjA"));
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/ProjA"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/ProjB"));
        // 确认无重复。
        QCOMPARE(aList.count(QStringLiteral("C:/ProjA")), 1);
    }

    // AC-5 / NFR-3.2：不同写法（分隔符 / 尾部斜杠 / 盘符大小写）同目录应去重为一条。
    void testNormalizePathDedup() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:\\ProjX"));
        stPm.AddRecent(QStringLiteral("C:/ProjX/"));
        stPm.AddRecent(QStringLiteral("c:/ProjX"));
        const QStringList aList = stPm.RecentProjects();
        // 三种写法指同一目录 → 只一条。
        QCOMPARE(aList.size(), 1);
    }

    // AC-6/AC-7 边界：子菜单上限是 UI 层常量（s_kiMaxRecentMenu=10）。
    // 这里验证全量内存列表**不**截断（设计 §3.2：全量历史无上限），11 条全在。
    void testFullListNoTruncation() {
        CProjectManager stPm;
        for (int i = 0; i < 11; ++i) {
            stPm.AddRecent(QStringLiteral("C:/Proj%1").arg(i));
        }
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 11);
        // 最近打开（i=10）在最前。
        QCOMPARE(aList.first(), QStringLiteral("C:/Proj10"));
        QCOMPARE(aList.last(), QStringLiteral("C:/Proj0"));
    }

    // AC-11：写入 config.ini 后重新载入，列表被保留（MRU 顺序）。
    void testPersistenceRoundTrip() {
        CProjectManager stPm1;
        stPm1.AddRecent(QStringLiteral("C:/PersistA"));
        stPm1.AddRecent(QStringLiteral("C:/PersistB"));
        stPm1.AddRecent(QStringLiteral("C:/PersistC"));

        // config.ini 应已落盘（AC-12：exe 同级）。
        const QString sCfg = ConfigPath();
        QVERIFY2(QFile::exists(sCfg),
                 qPrintable(QStringLiteral("config.ini not created at: %1").arg(sCfg)));

        // 新实例重新载入 → 顺序应为 C、B、A。
        CProjectManager stPm2;
        stPm2.LoadRecent();
        const QStringList aList = stPm2.RecentProjects();
        QCOMPARE(aList.size(), 3);
        QCOMPARE(aList.at(0), QStringLiteral("C:/PersistC"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/PersistB"));
        QCOMPARE(aList.at(2), QStringLiteral("C:/PersistA"));
    }

    // AC-11 + AC-5：持久化跨实例的去重（重新载入后再 Add 同一条不重复）。
    void testPersistenceDedupAfterReload() {
        CProjectManager stPm1;
        stPm1.AddRecent(QStringLiteral("C:/DedupA"));
        stPm1.AddRecent(QStringLiteral("C:/DedupB"));

        CProjectManager stPm2;
        stPm2.LoadRecent();
        stPm2.AddRecent(QStringLiteral("C:/DedupA"));  // 已存在 → 置顶
        const QStringList aList = stPm2.RecentProjects();
        QCOMPARE(aList.size(), 2);
        QCOMPARE(aList.at(0), QStringLiteral("C:/DedupA"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/DedupB"));
    }

    // 2026.09.09 v2.1.139 回归①：LoadRecent 幂等——重复调用不得叠加（旧版不清空列表，
    //   v2.1.123 热同步多次调用后内存列表每次叠一遍全量，多实例共享 config 滚雪球到
    //   21744 条/1.1MB，启动两万余次 profile 读＝「主界面很久才出来」根因）。
    void testLoadRecentIdempotent() {
        CProjectManager stPm1;
        stPm1.AddRecent(QStringLiteral("C:/IdemA"));
        stPm1.AddRecent(QStringLiteral("C:/IdemB"));

        CProjectManager stPm2;
        for (int i = 0; i < 3; ++i) {
            stPm2.LoadRecent();   // 热同步场景：同实例反复重载
        }
        const QStringList aList = stPm2.RecentProjects();
        QCOMPARE(aList.size(), 2);   // 旧 bug：每次 LoadRecent 翻倍 → 8
        QCOMPARE(aList.at(0), QStringLiteral("C:/IdemB"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/IdemA"));
    }

    // 2026.09.09 v2.1.139 回归②：旧档污染自愈——config 里已存在重复键（叠加 bug 落盘
    //   残留）时 LoadRecent 按规范化路径去重保 MRU 首见；随后 AddRecent 移除全部匹配。
    void testLoadRecentDedupesPoisonedConfig() {
        // 手工构造带重复的 [Recent]（ASCII，直接文本写入即 profile 可读）。
        const QString sCfg = ConfigPath();
        QFile fCfg(sCfg);
        QVERIFY2(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(QStringLiteral("cannot write config: %1").arg(sCfg)));
        const QByteArray sBody =
            "[Recent]\r\n"
            "Count=5\r\n"
            "1=C:/PoiA\r\n"
            "2=C:/PoiB\r\n"
            "3=C:\\PoiA\\\r\n"      // 与 1 同目录（反斜杠+尾斜杠变体）
            "4=C:/PoiB\r\n"          // 与 2 完全相同
            "5=C:/PoiC\r\n";
        QVERIFY(fCfg.write(sBody) == sBody.size());
        fCfg.close();

        CProjectManager stPm;
        stPm.LoadRecent();
        QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 3);                      // 去重后 A,B,C
        QCOMPARE(aList.at(0), QStringLiteral("C:/PoiA"));   // 保留首见（MRU 最新）
        QCOMPARE(aList.at(1), QStringLiteral("C:/PoiB"));
        QCOMPARE(aList.at(2), QStringLiteral("C:/PoiC"));

        // AddRecent 再触一发：移除全部匹配后置顶，不得出现重复。
        stPm.AddRecent(QStringLiteral("C:/PoiB"));
        aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 3);
        QCOMPARE(aList.at(0), QStringLiteral("C:/PoiB"));
        QCOMPARE(aList.count(QStringLiteral("C:/PoiB")), 1);
    }

    // 2026.09.09 v2.1.140 回归：SaveRecent 整节替换——列表缩短后高位残留键必须被清掉
    //   （旧版逐键写只增不删，[Recent] 膨胀事故的 1.1MB 残体即此来源）。
    void testRecentSaveClearsStaleKeys() {
        // 预置污染档：Count=2 + 两个高位残留键（3/4）。
        const QString sCfg = ConfigPath();
        QFile fCfg(sCfg);
        QVERIFY2(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(QStringLiteral("cannot write config: %1").arg(sCfg)));
        const QByteArray sBody =
            "[Recent]\r\n"
            "Count=2\r\n"
            "1=C:/StaleA\r\n"
            "2=C:/StaleB\r\n"
            "3=C:/StaleC\r\n"
            "4=C:/StaleD\r\n";
        QVERIFY(fCfg.write(sBody) == sBody.size());
        fCfg.close();

        CProjectManager stPm;
        stPm.LoadRecent();   // 只载入 1..2；键 3/4 为残留
        stPm.AddRecent(QStringLiteral("C:/StaleA"));   // 置顶去重 → 2 条；整节替换落盘

        QFile fRead(sCfg);
        QVERIFY(fRead.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString sTxt = QString::fromLatin1(fRead.readAll());
        fRead.close();
        QVERIFY2(!sTxt.contains(QStringLiteral("StaleC"))
                     && !sTxt.contains(QStringLiteral("StaleD")),
                 "高位残留键应被整节替换清除");
        QCOMPARE(sTxt.count(QStringLiteral("C:/Stale")), 2);   // 只剩 A、B 各一次
        QVERIFY(sTxt.contains(QStringLiteral("Count=2")));
    }

    // 2026.09.09 v2.1.143 任务级配置覆盖：任务选中=读任务档优先/写落任务档，取消选中
    //   回全局；激活时合并回填全局缺键（只补缺、从不覆盖任务已有键）。
    void testTaskConfigOverlay() {
        QTemporaryDir stDir;
        CProjectManager stPm;
        QVERIFY(stPm.Open(stDir.path()));
        const QString sTaskId = QStringLiteral("ov-task");
        QVERIFY(stPm.InitTaskDir(sTaskId));

        // 全局基线
        stPm.SaveMaxIter(7);
        QCOMPARE(stPm.LoadMaxIter(), 7);
        QVERIFY(!stPm.TaskConfigOverlayActive());

        // 激活：合并回填 → 缺键读到全局值
        stPm.SetTaskConfigOverlay(sTaskId);
        QVERIFY(stPm.TaskConfigOverlayActive());
        QVERIFY(QFile::exists(stPm.TaskConfigFile()));
        QCOMPARE(stPm.LoadMaxIter(), 7);

        // 覆盖态写：落任务档，全局不动
        stPm.SaveMaxIter(9);
        QCOMPARE(stPm.LoadMaxIter(), 9);
        stPm.SetTaskConfigOverlay(QString());
        QCOMPARE(stPm.LoadMaxIter(), 7);   // 全局仍 7

        // 再次激活：不覆盖已有键 → 仍 9；删档重激活 → 合并回填回全局值
        stPm.SetTaskConfigOverlay(sTaskId);
        QCOMPARE(stPm.LoadMaxIter(), 9);
        QFile::remove(stPm.TaskConfigFile());
        stPm.SetTaskConfigOverlay(QString());
        stPm.SetTaskConfigOverlay(sTaskId);
        QCOMPARE(stPm.LoadMaxIter(), 7);

        // 模型表随覆盖走（v2.1.144 超集语义）：任务档写自己的表，同时并入全局
        //   （同身份更新/新身份追加，全局只增不减——缺省恒保有最多模型配置）。
        QList<SDshModelEntry> aModels;
        SDshModelEntry stE;
        stE.sDisp    = QStringLiteral("任务级模型");
        stE.sBaseURL = QStringLiteral("http://127.0.0.1:1/v1");
        stE.sApi     = QStringLiteral("openai-completions");
        stE.sModelId = QStringLiteral("task-model");
        stE.iCtx     = 100000;
        stE.iMaxTok  = 8192;
        aModels.append(stE);
        stPm.SavePiModels(aModels, 0);
        int iAct = -1;
        QCOMPARE(stPm.LoadPiModels(&iAct).size(), 1);   // 任务档=自己的表
        QCOMPARE(iAct, 0);
        stPm.SetTaskConfigOverlay(QString());
        QCOMPARE(stPm.LoadPiModels().size(), 1);   // 全局=超集（并入而非污染）
    }

    // 2026.09.09 v2.1.144 模型表超集同步 + 整表活回退：①任务档无本表时实时跟随全局；
    //   ②任务态保存并入全局（新身份追加/同身份更新/全局 Active 保持/从不删除）；
    //   ③全局原空表时采用任务表 Active（缺省自身可用）。
    void testTaskModelSupersetSync() {
        QTemporaryDir stDir;
        CProjectManager stPm;
        QVERIFY(stPm.Open(stDir.path()));
        QVERIFY(stPm.InitTaskDir(QStringLiteral("sup-task")));

        auto fnEntry = [](const QString& sId, const QString& sUrl, qint64 iCtx) {
            SDshModelEntry stE;
            stE.sDisp = sId; stE.sModelId = sId; stE.sBaseURL = sUrl;
            stE.sApi = QStringLiteral("openai-completions");
            stE.iCtx = iCtx; stE.iMaxTok = 4096;
            return stE;
        };

        // ①全局 2 条；任务档无表 → 活回退读到全局 2 条
        QList<SDshModelEntry> aG;
        aG.append(fnEntry(QStringLiteral("g1"), QStringLiteral("http://g/1"), 111));
        aG.append(fnEntry(QStringLiteral("g2"), QStringLiteral("http://g/2"), 222));
        stPm.SavePiModels(aG, 0);
        stPm.SetTaskConfigOverlay(QStringLiteral("sup-task"));
        QCOMPARE(stPm.LoadPiModels().size(), 2);   // 任务档无 Count → 回退全局

        // ②任务态保存 1 条新身份 + 1 条同身份（g1 改 Ctx）→ 任务表 2，全局=3（超集）
        QList<SDshModelEntry> aT;
        aT.append(fnEntry(QStringLiteral("t9"), QStringLiteral("http://t/9"), 999));
        aT.append(fnEntry(QStringLiteral("g1"), QStringLiteral("http://g/1"), 333));  // 同身份更新
        stPm.SavePiModels(aT, 0);
        QCOMPARE(stPm.LoadPiModels().size(), 2);   // 任务档=自己的表
        stPm.SetTaskConfigOverlay(QString());
        QList<SDshModelEntry> aGlobal = stPm.LoadPiModels();
        QCOMPARE(aGlobal.size(), 3);               // g1,g2,t9（只增不减）
        bool bHasT9 = false, bG1Updated = false;
        for (const SDshModelEntry& stOne : aGlobal) {
            if (stOne.sModelId == QLatin1String("t9")) { bHasT9 = true; }
            if (stOne.sModelId == QLatin1String("g1") && stOne.iCtx == 333) { bG1Updated = true; }
        }
        QVERIFY(bHasT9 && bG1Updated);

        // ③全局空表首配：任务态保存 → 全局采用任务表与 Active
        QFile::remove(ConfigPath());   // 清全局（重开不必要，直接删文件重写）
        stPm.SetTaskConfigOverlay(QStringLiteral("sup-task"));
        QList<SDshModelEntry> aT2;
        aT2.append(fnEntry(QStringLiteral("only"), QStringLiteral("http://o/1"), 555));
        stPm.SavePiModels(aT2, 0);
        stPm.SetTaskConfigOverlay(QString());
        int iGA = -1;
        QCOMPARE(stPm.LoadPiModels(&iGA).size(), 1);
        QCOMPARE(iGA, 0);   // 空表首配 → 全局 Active 采用任务值（缺省可用）
    }

    // 2026.09.09 v2.1.142 回归：Count 键丢失/被粘连（「[Recent]Count=52」整行被解析器
    //   当节头丢弃）但编号键仍在 → LoadRecent 顺序探测自愈重建，列表不凭空消失。
    void testLoadRecentCountKeyLostFallback() {
        const QString sCfg = ConfigPath();
        QFile fCfg(sCfg);
        QVERIFY2(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(QStringLiteral("cannot write config: %1").arg(sCfg)));
        const QByteArray sBody =
            "[Recent]Count=3\r\n"      // 粘连形态：Count 与节头同行（解析器丢弃）
            "1=C:/FallbackA\r\n"
            "2=C:/FallbackB\r\n";
        QVERIFY(fCfg.write(sBody) == sBody.size());
        fCfg.close();

        CProjectManager stPm;
        stPm.LoadRecent();
        const QStringList aList = stPm.RecentProjects();
        QCOMPARE(aList.size(), 2);   // 兑底探测重建（而非旧版 Count=0 → 空列表）
        QCOMPARE(aList.at(0), QStringLiteral("C:/FallbackA"));
        QCOMPARE(aList.at(1), QStringLiteral("C:/FallbackB"));
    }

    // AC-12：config.ini 落在 exe 同级（非当前工作目录）。这里校验路径来源。
    void testConfigAtExeDir() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ExeDirCheck"));
        const QString sCfg = ConfigPath();
        const QString sExeDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
        QVERIFY2(QFileInfo(sCfg).absolutePath() == sExeDir,
                 "config.ini must live next to the executable, not the working directory");
    }

    // 空串入参安全跳过（健壮性）。
    void testEmptyStringIgnored() {
        CProjectManager stPm;
        stPm.AddRecent(QString());
        QVERIFY(stPm.RecentProjects().isEmpty());
    }

    // ClearRecent 清空内存列表。
    void testClearRecent() {
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/ClearA"));
        stPm.AddRecent(QStringLiteral("C:/ClearB"));
        stPm.ClearRecent();
        QVERIFY(stPm.RecentProjects().isEmpty());
    }

    // 2026.08.19 [DshModels] 全字段往返（含中文显示名/密钥）：Save→Load 逐字段一致，
    //   激活下标还原；缩短到 0 条后残留键清理生效（读回空表 + 无激活）。
    //   背景反馈「显示名重启后变模型 ID」——本测试锁定显示名持久化链路不回归。
    void testDshModelsRoundTrip() {
        CProjectManager stPm;
        QList<SDshModelEntry> aModels;
        SDshModelEntry stE1;
        stE1.sDisp    = QStringLiteral("公司中转 GLM-5.2");
        stE1.sBaseURL = QStringLiteral("http://192.168.1.10:9000/claude");
        stE1.sApiKey  = QStringLiteral("sk-测试密钥123");
        stE1.sApi     = QStringLiteral("anthropic-messages");
        stE1.sModelId = QStringLiteral("glm-5.2");
        stE1.iCtx     = 128000;
        stE1.iMaxTok  = 65536;
        aModels.append(stE1);
        SDshModelEntry stE2;
        stE2.sDisp    = QStringLiteral("备用网关");
        stE2.sBaseURL = QStringLiteral("http://10.0.0.2:8080/v1");
        stE2.sApiKey.clear();
        stE2.sApi     = QStringLiteral("openai-completions");
        stE2.sModelId = QStringLiteral("model-b");
        stE2.iCtx     = 200000;
        stE2.iMaxTok  = 32768;
        aModels.append(stE2);
        QVERIFY(stPm.SaveDshModels(aModels, 1));

        int iActive = -99;
        const QList<SDshModelEntry> aBack = stPm.LoadDshModels(&iActive);
        QCOMPARE(aBack.size(), 2);
        QCOMPARE(iActive, 1);
        QCOMPARE(aBack.at(0).sDisp,    stE1.sDisp);
        QCOMPARE(aBack.at(0).sBaseURL, stE1.sBaseURL);
        QCOMPARE(aBack.at(0).sApiKey,  stE1.sApiKey);
        QCOMPARE(aBack.at(0).sApi,     stE1.sApi);
        QCOMPARE(aBack.at(0).sModelId, stE1.sModelId);
        QCOMPARE(aBack.at(0).iCtx,     stE1.iCtx);
        QCOMPARE(aBack.at(0).iMaxTok,  stE1.iMaxTok);
        QCOMPARE(aBack.at(1).sDisp,    stE2.sDisp);
        QCOMPARE(aBack.at(1).sApi,     stE2.sApi);

        // 缩短到 0 条：残留键清理后读回应为空表 + 无激活
        QVERIFY(stPm.SaveDshModels({}, -1));
        QCOMPARE(stPm.LoadDshModels(&iActive).size(), 0);
        QCOMPARE(iActive, -1);
    }

    // 2026.09.07 v2.1.123 多实例工程锁：写/读往返、自 pid 不算他实例在席、
    //   陈旧锁（死 PID/PID 被无关进程复用）自动失效、Release 只摘自己的锁、
    //   活的「伪 AutoCode 进程」持锁判在席（PID 复用复核按进程名放行）。
    void testProjectInstanceLock() {
        CProjectManager stPm;
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path();

        // ① 无锁 → 不在席。
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        // ② 写/读往返（自 pid + exe + started）。
        CProjectManager::WriteProjectLock(sProj);
        CProjectManager::SProjectLock st;
        QVERIFY(CProjectManager::ReadProjectLock(sProj, &st));
        QVERIFY(st.iPid > 0);
        QVERIFY(!st.sExe.isEmpty());
        QVERIFY(QFile::exists(CProjectManager::ProjectLockPath(sProj)));
        // 自 pid 持锁 → 对「他实例在席」判定为否（同进程重开不禁）。
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        // ③ Release 摘自己的锁。
        CProjectManager::ReleaseProjectLock(sProj);
        QVERIFY(!QFile::exists(CProjectManager::ProjectLockPath(sProj)));
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));

        // ④ 陈旧锁：死 PID（系统闲置小进程退掉后大概率不存在；取极大值 PID 不存在
        //    → OpenProcess 参数错 → 判死）。写入后应自动失效、不阻断。
        {
            QFile f(CProjectManager::ProjectLockPath(sProj));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QStringLiteral("{\"pid\":%1,\"exe\":\"C:\\\\x\\\\AutoCode.exe\","
                                   "\"started\":\"2026-01-01 00:00:00\"}")
                        .arg(99999999)
                        .toUtf8());
            f.close();
        }
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        // 陈旧锁 Release 不动（非自己 pid）；后继 WriteProjectLock 直接接管重写。
        CProjectManager::ReleaseProjectLock(sProj);
        QVERIFY(QFile::exists(CProjectManager::ProjectLockPath(sProj)));

        // ⑤ 活的伪 AutoCode 进程持锁 → 判在席（PID 复用复核按进程名 AutoCode* 放行）：
        //    拷 ping.exe 为 AutoCodeFake.exe（长 ping 保活）再手写其 pid 到锁。
        {
            const QString sFake = oTmp.path() + QStringLiteral("/AutoCodeFake.exe");
            QVERIFY(QFile::copy(QStringLiteral("C:/Windows/System32/ping.exe"), sFake));
            QProcess stProc;
            stProc.start(sFake, {QStringLiteral("-n"), QStringLiteral("30"),
                                 QStringLiteral("127.0.0.1")});
            QVERIFY2(stProc.waitForStarted(), "伪 AutoCode 进程启动失败");
            {
                QFile f(CProjectManager::ProjectLockPath(sProj));
                QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
                f.write(QStringLiteral("{\"pid\":%1,\"exe\":\"%2\","
                                       "\"started\":\"2026-01-01 00:00:00\"}")
                            .arg(static_cast<qint64>(stProc.processId()))
                            .arg(sFake)
                            .toUtf8());
                f.close();
            }
            QString sDesc;
            QVERIFY(CProjectManager::IsProjectHeldByOther(sProj, &sDesc));
            QVERIFY(sDesc.contains(QStringLiteral("AutoCodeFake.exe")));
            // 他实例锁 Release 不动（防误捕他实例在席状态）。
            CProjectManager::ReleaseProjectLock(sProj);
            QVERIFY(QFile::exists(CProjectManager::ProjectLockPath(sProj)));
            stProc.kill();   // 控制台进程不认 WM_CLOSE，用 TerminateProcess 保收尾干净
            stProc.waitForFinished(3000);
        }
        // ⑥ 空/非法锁文件：无 pid 键 → 读失败不阻断。
        {
            QFile f(CProjectManager::ProjectLockPath(sProj));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("{\"exe\":\"x\"}");
            f.close();
        }
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
    }

    // 2026.09.10 公共模型配置：MergeSharedModelTables 合并规则回归——基底选择/新身份
    //   追加/chat 归一/当前项按身份映射（切换后端共享同一份模型表，勿回退）。
    void testSharedModelMerge() {
        auto fnE = [](const QString& a_sDisp, const QString& a_sUrl, const QString& a_sApi,
                      const QString& a_sModel) {
            SDshModelEntry st;
            st.sDisp = a_sDisp;
            st.sBaseURL = a_sUrl;
            st.sApi = a_sApi;
            st.sModelId = a_sModel;
            return st;
        };
        // ① 四表均空 → 空共享表，无当前。
        {
            const SSharedModels stM = MergeSharedModelTables(
                { QList<SDshModelEntry>(), QList<SDshModelEntry>(),
                  QList<SDshModelEntry>(), QList<SDshModelEntry>() },
                { -1, -1, -1, -1 });
            QVERIFY(stM.aModels.isEmpty());
            QCOMPARE(stM.iActive, -1);
        }
        // ② 基底 = 最多条目的表（Pi 3 条 vs DSH 2 条），其余表新身份追加；同身份
        //   （URL+归一 Api+ModelId，忽略大小写）去重——DSH 的 glm 与 Pi 的 glm 同身份
        //   只留基底份；codex 的 chat 归一为 openai-completions 后与 Pi 的同名条目
        //   同身份去重；共享当前项 = 基底表 Active（按身份映射到合并后下标）。
        {
            QList<SDshModelEntry> aPi;
            aPi << fnE("glm", "http://gw/claude", "anthropic-messages", "glm-5.3")
                << fnE("spark", "http://127.0.0.1:8080/v1", "openai-completions", "spark")
                << fnE("kimi", "http://gw2/v1", "openai-completions", "kimi-k3");
            QList<SDshModelEntry> aDsh;
            aDsh << fnE("glm-dup", "http://GW/CLAUDE", "anthropic-messages", "GLM-5.3")
                << fnE("deepseek", "http://127.0.0.1:11434/v1", "openai-completions", "ds");
            QList<SDshModelEntry> aCodex;
            aCodex << fnE("spark-codex", "http://127.0.0.1:8080/v1", "chat", "spark");
            const SSharedModels stM = MergeSharedModelTables(
                { aPi, aDsh, QList<SDshModelEntry>(), aCodex }, { 1, 0, -1, 0 });
            QCOMPARE(stM.aModels.size(), 4);   // glm + spark + kimi（Pi 基底）+ deepseek（DSH 新身份）
            QCOMPARE(stM.aModels.at(0).sDisp, QStringLiteral("glm"));
            QCOMPARE(stM.aModels.at(1).sDisp, QStringLiteral("spark"));
            QCOMPARE(stM.aModels.at(2).sDisp, QStringLiteral("kimi"));
            QCOMPARE(stM.aModels.at(3).sDisp, QStringLiteral("deepseek"));
            QCOMPARE(stM.iActive, 1);          // Pi Active=1（spark）按身份映射仍为 1
        }
        // ③ 基底 Active 无效时按同序取首个有效（Pi -1 → DSH 的当前条目）；chat 归一
        //   写入合并表（codex 词汇不泄漏到 pi/dsh/hermes 侧）。
        {
            QList<SDshModelEntry> aPi;
            aPi << fnE("a", "http://x/1", "openai-completions", "m1");
            QList<SDshModelEntry> aCodex;
            aCodex << fnE("b", "http://x/2", "chat", "m2");
            const SSharedModels stM = MergeSharedModelTables(
                { aPi, QList<SDshModelEntry>(), QList<SDshModelEntry>(), aCodex },
                { -1, -1, -1, 0 });
            QCOMPARE(stM.aModels.size(), 2);
            QCOMPARE(stM.aModels.at(1).sApi, QStringLiteral("openai-completions"));   // chat 归一
            QCOMPARE(stM.iActive, 1);          // codex 的当前条目（m2，追加后下标 1）
        }
        // ④ 当前后端表优先：基底是 Pi（3 条），但调用方指定 DSH 表（2 条）的当前条目
        //   优先——任务级/后端级已选模型不被大表盖掉（磁盘四表收敛后各表 Active 相同，
        //   本优先级只在未收敛期起作用）。
        {
            QList<SDshModelEntry> aPi;
            aPi << fnE("glm", "http://gw/claude", "anthropic-messages", "glm-5.3")
                << fnE("spark", "http://127.0.0.1:8080/v1", "openai-completions", "spark")
                << fnE("kimi", "http://gw2/v1", "openai-completions", "kimi-k3");
            QList<SDshModelEntry> aDsh;
            aDsh << fnE("glm", "http://gw/claude", "anthropic-messages", "glm-5.3")
                << fnE("deepseek", "http://127.0.0.1:11434/v1", "openai-completions", "ds");
            const SSharedModels stM = MergeSharedModelTables(
                { aPi, aDsh, QList<SDshModelEntry>(), QList<SDshModelEntry>() },
                { 0, 1, -1, -1 }, /*a_iPrefTable=*/1);
            QCOMPARE(stM.aModels.size(), 4);   // glm（同身份去重）+ spark + kimi + deepseek
            QCOMPARE(stM.iActive, 3);          // DSH 的当前条目（ds，追加后下标 3）
        }
    }

    // 2026.09.11 模型表列宽可调+持久化（用户需求：鼠标可调每列表头宽度并记忆）：
    // ① 四棵模型树全列 Interactive（含原 Stretch 不可拖的第 1 列）且末列 stretch；
    // ② 真实鼠标拖拽：QTest 合成 press/move/release 在表头分隔线上，验证 QHeaderView
    //   鼠标处理链路真的拖宽了列（非仅 setColumnWidth API 层）；
    // ③ done() 落盘 [Layout] SettingsModelCols* → 重开恢复；
    // ④ 坏档（个数不符）整表自弃回缺省，不拉出 0 宽/巨宽列。
    void testSettingsModelColsResizableAndPersisted() {
        // ① 全列可调：四树 7 列全部 Interactive + stretchLastSection。
        {
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            const QList<QTreeWidget*> aTrees = stDlg.findChildren<QTreeWidget*>();
            QCOMPARE(aTrees.size(), 4);
            for (QTreeWidget* pTree : aTrees) {
                QCOMPARE(pTree->columnCount(), 7);
                QHeaderView* pHdr = pTree->header();
                for (int i = 0; i < pTree->columnCount(); ++i) {
                    QCOMPARE(pHdr->sectionResizeMode(i), QHeaderView::Interactive);
                }
                QVERIFY(pHdr->stretchLastSection());   // 末列「当前」填满剩余空间
            }
        }
        // ② 真实鼠标拖拽验证（QWindow 级 QTest 合成事件走真实输入管线，同真鼠标链路）：
        //    拖第 0 列右分隔线 +60px，第 0 列宽应从 130 变 ≈190（鼠标处理链路生效）。
        //    注：事件发到对话框窗口句柄、坐标 map 到窗口局部坐标系（widget 级直发在
        //    offscreen 下不进表头，窗口级给到 pickMouseReceiver 正常路由到 QHeaderView）。
        {
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            stDlg.resize(1000, 760);
            stDlg.show();   // 对话框整窗显示，四棵树都在已创建页里（QTabWidget 全页即时建）
            QVERIFY(QTest::qWaitForWindowExposed(&stDlg));
            QApplication::processEvents();
            QWindow* pWin = stDlg.windowHandle();
            QVERIFY(pWin != nullptr);
            const QList<QTreeWidget*> aTrees = stDlg.findChildren<QTreeWidget*>();
            const QList<int> aDef = {130, 190, 125, 100, 75, 75, 42};   // 各列缺省宽
            for (QTreeWidget* pTree : aTrees) {
                // QTabWidget 非当前页的控件是隐藏的（事件不投递），先把所在页切到前台
                for (QWidget* pW = pTree->parentWidget(); pW != nullptr; pW = pW->parentWidget()) {
                    if (auto* pTabW = qobject_cast<QTabWidget*>(pW)) {
                        pTabW->setCurrentWidget(pTree->parentWidget()->parentWidget());
                        break;
                    }
                }
                pTree->resize(900, 200);   // 铺开视图，表头分隔线可见可拖
                QApplication::processEvents();
                QHeaderView* pHdr = pTree->header();
                // 逐列拖每条分隔线（含接入点/请求地址列，iCol=1；末列 stretch 不拖）：
                //   每列先复位缺省再拖，避免前一次拖拽累计漂移边界位置。
                for (int iCol = 0; iCol < 6; ++iCol) {
                    for (int j = 0; j < 7; ++j) {
                        pTree->setColumnWidth(j, aDef[j]);
                    }
                    QApplication::processEvents();
                    const QPoint pGrip = pTree->mapTo(&stDlg,
                        QPoint(pHdr->x() + pHdr->sectionViewportPosition(iCol) + pHdr->sectionSize(iCol),
                               pHdr->y() + pHdr->height() / 2));
                    QTest::mouseMove(pWin, pGrip);
                    QTest::mousePress(pWin, Qt::LeftButton, {}, pGrip);
                    for (int iStep = 1; iStep <= 6; ++iStep) {
                        QTest::mouseMove(pWin, pGrip + QPoint(iStep * 10, 0));
                    }
                    QTest::mouseRelease(pWin, Qt::LeftButton, {}, pGrip + QPoint(60, 0));
                    QApplication::processEvents();
                    QVERIFY2(pTree->columnWidth(iCol) >= aDef[iCol] + 50,   // +60px 拖宽生效
                             qPrintable(QStringLiteral("col%1 drag: %2 -> %3")
                                 .arg(iCol).arg(aDef[iCol]).arg(pTree->columnWidth(iCol))));
                }
            }
        }
        // ③ 调宽 → done()（确定/取消/X 关闭统一口）落盘 → 新开对话框恢复。
        {
            {
                CProjectManager stPm;
                CSettingsDialog stDlg(&stPm);
                const QList<QTreeWidget*> aTrees = stDlg.findChildren<QTreeWidget*>();
                for (QTreeWidget* pTree : aTrees) {
                    pTree->setColumnWidth(0, 234);
                    pTree->setColumnWidth(3, 156);
                }
                stDlg.done(QDialog::Accepted);   // 走 done() 统一落盘
            }
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            const QList<QTreeWidget*> aTrees = stDlg.findChildren<QTreeWidget*>();
            QCOMPARE(aTrees.size(), 4);
            for (QTreeWidget* pTree : aTrees) {
                QCOMPARE(pTree->columnWidth(0), 234);
                QCOMPARE(pTree->columnWidth(3), 156);
            }
        }
        // ④ 坏档自弃：SettingsModelColsDsh 写成 3 个整数（≠6 列）→ 恢复时整表弃用，
        //   第 0 列回缺省 130；其余三键未写坏、不在本用例范围（init 已清空 config）。
        {
            const QString sCfg = ConfigPath();
            WritePrivateProfileStringW(L"Layout", L"SettingsModelColsDsh", L"1,2,3",
                                       (LPCWSTR)sCfg.utf16());
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            bool bFoundDsh = false;
            const QList<QTreeWidget*> aTrees = stDlg.findChildren<QTreeWidget*>();
            for (QTreeWidget* pTree : aTrees) {
                if (pTree->headerItem()->text(1) == QStringLiteral("接入点")) {
                    bFoundDsh = true;   // DSH 页第 1 列表头为「接入点」，其余页为「请求地址」
                    QCOMPARE(pTree->columnWidth(0), 130);   // 坏档弃用 → 回缺省
                }
            }
            QVERIFY(bFoundDsh);
        }
    }
};

QTEST_MAIN(CTestProjectManager)
#include "test_projectmanager.moc"
