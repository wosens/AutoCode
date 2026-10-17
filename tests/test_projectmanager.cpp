#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonDocument>   // 2026-10-17 在席信号用例（手写带 procStartMs 的锁档）
#include <QJsonObject>
#include <QTemporaryDir>   // 2026.09.09 v2.1.143 任务级配置覆盖用例
#include <QHeaderView>     // 2026.09.11 模型表列宽用例
#include <QCheckBox>
#include <QTreeWidget>
#include <QWindow>          // 窗口级 QTest 鼠标事件（真实输入管线）
#include <QTabWidget>
#include <QComboBox>       // 2026-10-08 思考强度下拉用例
#include <QMainWindow>       // 2026.09.26 设置对话框几何漂移用例（父窗口）

#include <windows.h>       // WritePrivateProfileStringW（坏档构造，与生产代码同通道）
#include <tlhelp32.h>    // 2026-10-17 在席锁受保护进程 PID 复用用例（快照枚举 svchost）

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
    // 2026.09.20 打开次数统计：AddRecent 命中累加/新条目=1、MRU 顺序、
    //   存取往返（C<i> 键）、旧档无 C 键回退 1、RemoveRecent 随条目同删。
    void testRecentOpenCountTracking() {
        RemoveConfig();
        CProjectManager stPm;
        stPm.AddRecent(QStringLiteral("C:/CntA"));
        stPm.AddRecent(QStringLiteral("C:/CntA"));
        stPm.AddRecent(QStringLiteral("C:/CntA"));
        stPm.AddRecent(QStringLiteral("C:/CntB"));
        stPm.AddRecent(QStringLiteral("C:/CntC"));
        stPm.AddRecent(QStringLiteral("C:/CntC"));
        QStringList aList = stPm.RecentProjects();
        QList<int> aOpens = stPm.RecentOpenCounts();
        QCOMPARE(aList.size(), 3);
        QCOMPARE(aList.at(0), QStringLiteral("C:/CntC"));   // MRU：最近在前
        QCOMPARE(aList.at(1), QStringLiteral("C:/CntB"));
        QCOMPARE(aList.at(2), QStringLiteral("C:/CntA"));
        QCOMPARE(aOpens, (QList<int>() << 2 << 1 << 3));    // 同序计数

        // 存取往返：新实例 LoadRecent 读回同计数。
        CProjectManager stPm2;
        stPm2.LoadRecent();
        QCOMPARE(stPm2.RecentProjects(), aList);
        QCOMPARE(stPm2.RecentOpenCounts(), aOpens);

        // 再次打开 A：计数 3→4 且置顶。
        stPm2.AddRecent(QStringLiteral("C:/CntA"));
        QCOMPARE(stPm2.RecentProjects().at(0), QStringLiteral("C:/CntA"));
        QCOMPARE(stPm2.RecentOpenCounts().at(0), 4);

        // RemoveRecent：条目与计数同删。
        stPm2.RemoveRecent(QStringLiteral("C:/CntB"));
        QCOMPARE(stPm2.RecentProjects().size(), 2);
        QCOMPARE(stPm2.RecentOpenCounts().size(), 2);
        QVERIFY(!stPm2.RecentProjects().contains(QStringLiteral("C:/CntB")));
    }

    void testRecentOpenCountLegacyConfigDefaults() {
        // 旧档（无 C<i> 计数键）→ 全部回退 1。
        const QString sCfg = ConfigPath();
        QFile fCfg(sCfg);
        QVERIFY2(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(QStringLiteral("cannot write config: %1").arg(sCfg)));
        fCfg.write("[Recent]\r\nCount=2\r\n1=C:/LegA\r\n2=C:/LegB\r\n");
        fCfg.close();
        CProjectManager stPm;
        stPm.LoadRecent();
        QCOMPARE(stPm.RecentProjects().size(), 2);
        QCOMPARE(stPm.RecentOpenCounts(), (QList<int>() << 1 << 1));
        // 触发落盘（AddRecent 命中：LegB 计数 1→2 并置顶）→ 新实例读回补写出的 C 键。
        stPm.AddRecent(QStringLiteral("C:/LegB"));
        CProjectManager stPm2;
        stPm2.LoadRecent();
        QCOMPARE(stPm2.RecentProjects().at(0), QStringLiteral("C:/LegB"));
        QCOMPARE(stPm2.RecentOpenCounts(), (QList<int>() << 2 << 1));
    }

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

    // 2026-10-17 在席锁受保护进程 PID 复用误报回归（实害：机器重启残留锁 pid 被
    //   LocalSystem svchost 复用，OpenProcess 恒 ACCESS_DENIED，旧版"保守当活"
    //   误报「工程已被其他实例打开」）：快照查名 兑底 + 死进程快照不命中 +
    //   真实受保护 svchost 持锁应判陈旧不阻断。
    void testProjectInstanceLockProtectedPidReuse() {
        // ① 快照能查到活进程（自己）的映像名。
        QString sName;
        QVERIFY(CProjectManager::SnapshotImageNameOfPid(
                    static_cast<qint64>(::GetCurrentProcessId()), &sName));
        QVERIFY2(sName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive),
                 qPrintable(sName));
        // ② 已退出进程 → 快照不命中（短命子进程退干净后查）。 
        {
            QProcess stProc;
            stProc.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit")});
            QVERIFY2(stProc.waitForStarted(), "短命子进程启动失败");
            const qint64 iPid = static_cast<qint64>(stProc.processId());
            stProc.waitForFinished(5000);
            QTest::qSleep(300);   // 退出后句柄清理小窗口
            QVERIFY(!CProjectManager::SnapshotImageNameOfPid(iPid, nullptr));
        }
        // ③ 真实场景复现：找一个 OpenProcess 拒绝访问的 svchost（LocalSystem 服务），
        //    把它的 pid 写进锁 → 修复后应判陈旧不阻断（旧版恒误报在席）。
        //    本机/Admin 提权环境可能全部能打开 → QSKIP（逻辑已被 ①② 覆盖）。
        {
            qint64 iSvcPid = 0;
            HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W stPe = {};
                stPe.dwSize = sizeof(stPe);
                BOOL bOk = ::Process32FirstW(hSnap, &stPe);
                while (bOk && iSvcPid == 0) {
                    if (QString::fromWCharArray(stPe.szExeFile)
                            .compare(QStringLiteral("svchost.exe"), Qt::CaseInsensitive) == 0) {
                        HANDLE hTry = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                                    stPe.th32ProcessID);
                        if (hTry == nullptr && ::GetLastError() == ERROR_ACCESS_DENIED) {
                            iSvcPid = stPe.th32ProcessID;
                        } else if (hTry != nullptr) {
                            ::CloseHandle(hTry);
                        }
                    }
                    bOk = ::Process32NextW(hSnap, &stPe);
                }
                ::CloseHandle(hSnap);
            }
            if (iSvcPid == 0) {
                QSKIP("本环境找不到拒绝访问的 svchost（提权态），跳过真场景复现段");
            }
            CProjectManager::SProjectLock st;
            st.iPid = iSvcPid;
            st.sExe = QStringLiteral("E:/x/AutoCode1.exe");
            st.sStarted = QStringLiteral("2026-10-17 11:46:39");
            QVERIFY(!CProjectManager::IsLockHolderAlive(st));   // svchost ≠ AutoCode* → 陈旧
        }
    }

    // 2026-10-17 v4.4.387 在席信号（内核对象层）+ 创建时刻交叉验证回归：
    //   信号生死随进程（Hold/Drop/他实例持有判定）、信号单独在席（无锁文件）、
    //   自 pid 豁免、procStartMs 匹配/不匹配/缺失三态。
    void testProjectSeatEventLock() {
        CProjectManager stPm;
        QTemporaryDir oTmp;
        QVERIFY(oTmp.isValid());
        const QString sProj = oTmp.path();

        // ① 无持有者 → 不在席。
        QVERIFY(!CProjectManager::ProbeProjectSeatEvent(sProj));
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        // 路径规范化：大小写/斜杠差异同名。
        const QString sName = CProjectManager::ProjectSeatEventName(sProj);
        QVERIFY(!sName.isEmpty());
        QCOMPARE(CProjectManager::ProjectSeatEventName(sProj.toUpper()), sName);

        // ② 本实例持有/释放；他实例（第二个 CProjectManager）持有冲突。
        QVERIFY(stPm.HoldProjectSeatEvent(sProj));
        QVERIFY(CProjectManager::ProbeProjectSeatEvent(sProj));
        {
            CProjectManager stPm2;
            QVERIFY(!stPm2.HoldProjectSeatEvent(sProj));   // 他实例已持→不夺占
            QVERIFY(CProjectManager::ProbeProjectSeatEvent(sProj));
        }
        // 自 pid 规则：自己持信号+自己写的锁 → 不算他实例在席。
        CProjectManager::WriteProjectLock(sProj);
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        stPm.DropProjectSeatEvent();
        QVERIFY(!CProjectManager::ProbeProjectSeatEvent(sProj));
        // 写锁往返含 procStartMs（本进程创建时刻，±5 分钟内）。
        CProjectManager::SProjectLock st;
        QVERIFY(CProjectManager::ReadProjectLock(sProj, &st));
        QVERIFY(st.iProcStartMs > 0);
        QVERIFY(qAbs(st.iProcStartMs - QDateTime::currentMSecsSinceEpoch()) < 5 * 60 * 1000);
        CProjectManager::ReleaseProjectLock(sProj);

        // ③ 信号单独在席（模拟另一新实例：手建同名 Event，无锁文件）→ 拦 + 通用描述。
        {
            HANDLE hEvt = ::CreateEventW(nullptr, TRUE, TRUE,
                reinterpret_cast<const wchar_t*>(sName.utf16()));
            QVERIFY(hEvt != nullptr);
            QString sDesc;
            QVERIFY(CProjectManager::IsProjectHeldByOther(sProj, &sDesc));
            QVERIFY(sDesc.contains(QStringLiteral("在席信号")));
            ::CloseHandle(hEvt);
            QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));   // 信号消失即不在席（进程消亡同理）
        }

        // ④ procStartMs 交叉验证（伪 AutoCode 进程持锁，无信号→回退 PID 判活）：
        const QString sFake = oTmp.path() + QStringLiteral("/AutoCodeFake.exe");
        QVERIFY(QFile::copy(QStringLiteral("C:/Windows/System32/ping.exe"), sFake));
        QProcess stProc;
        stProc.start(sFake, {QStringLiteral("-n"), QStringLiteral("30"), QStringLiteral("127.0.0.1")});
        QVERIFY2(stProc.waitForStarted(), "伪 AutoCode 进程启动失败");
        const qint64 iPid = static_cast<qint64>(stProc.processId());
        auto fnWriteLock = [this, &sProj, iPid, &sFake](qint64 a_iProcStartMs) {
            QFile f(CProjectManager::ProjectLockPath(sProj));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QJsonObject o;
            o["pid"] = static_cast<double>(iPid);
            o["exe"] = sFake;
            o["started"] = QStringLiteral("2026-10-17 00:00:00");
            if (a_iProcStartMs >= 0) {
                o["procStartMs"] = static_cast<double>(a_iProcStartMs);
            }
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            f.close();
        };
        // 实际创建时刻 → 在席（同名 AutoCode 进程身份匹配）。
        const qint64 iFakeStart = [iPid]() {
            HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(iPid));
            if (h == nullptr) { return qlonglong(0); }
            FILETIME fc = {}, fe = {}, fk = {}, fu = {};
            const BOOL bOk = ::GetProcessTimes(h, &fc, &fe, &fk, &fu);
            ::CloseHandle(h);
            if (!bOk) { return qlonglong(0); }
            ULARGE_INTEGER ul = {};
            ul.LowPart = fc.dwLowDateTime;
            ul.HighPart = fc.dwHighDateTime;
            return static_cast<qint64>((ul.QuadPart - 116444736000000000ULL) / 10000ULL);
        }();
        QVERIFY(iFakeStart > 0);
        fnWriteLock(iFakeStart);
        QVERIFY(CProjectManager::IsProjectHeldByOther(sProj));
        // 创建时刻不一致（同名 AutoCode 进程复用 PID 的前任残留锁）→ 陈旧不拦。
        fnWriteLock(iFakeStart + 3600 * 1000);
        QVERIFY(!CProjectManager::IsProjectHeldByOther(sProj));
        // 旧档无 procStartMs → 跳过交叉验证仅按进程名（向后兼容）。
        fnWriteLock(-1);
        QVERIFY(CProjectManager::IsProjectHeldByOther(sProj));
        stProc.kill();
        stProc.waitForFinished(3000);
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

    // 2026.09.23 v3.1.229 启动链回归（用户报告搬机场景）：后端=codex 但 ini 无
    //   [CodexModels] 节（只复制了部分节的 config，如 Execute/config.ini 现场）
    //   ——启动 ApplySharedModelsToRunners 四表合并后 codex 侧应得到共享当前模型
    //   （基底表 Active 按身份映射，绝不因 codex 表空而报「模型未配置」）。
    //   旧 exe（v1.9.4-，无公共模型）会真的报模型未配置——本用例兼作新旧分水岭证据。
    void testStartupCodexMissingTableStillGetsModel() {
        // 合成现场态 ini：Backend=4 + [PiModels] 1 条 Active=0 + 无其余模型表。
        const QString sCfg = ConfigPath();
        QFile::remove(sCfg);
        {
            QFile f(sCfg);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("[Agent]\r\nBackend=4\r\n");
            f.write("[PiModels]\r\nCount=1\r\nActive=0\r\n"
                    "M0Disp=GLM-5.3\r\n"
                    "M0URL=http://192.168.118.128:9000/claude\r\n"
                    "M0Key=k\r\nM0Api=anthropic-messages\r\n"
                    "M0Model=glm-5.3\r\nM0Ctx=1000000\r\nM0Max=32768\r\n");
            f.close();
        }
        CProjectManager stPm;
        // 启动同序：四表实读（LoadCodexModels 回退全局）→ 合并（pref=codex）→ 应用 runner。
        int iDshAct = -1, iPiAct = -1, iCodexAct = -1, iHermesAct = -1;
        const QList<SDshModelEntry> aDsh    = stPm.LoadDshModels(&iDshAct);
        const QList<SDshModelEntry> aPi     = stPm.LoadPiModels(&iPiAct);
        const QList<SDshModelEntry> aCodex  = stPm.LoadCodexModels(&iCodexAct);
        const QList<SDshModelEntry> aHermes = stPm.LoadHermesModels(&iHermesAct);
        QVERIFY(aCodex.isEmpty());          // 现场态：codex 表确实空
        QCOMPARE(iCodexAct, -1);
        const QList<QList<SDshModelEntry>> aTables = { aPi, aDsh, aHermes, aCodex };
        const QList<int> aActives = { iPiAct, iDshAct, iHermesAct, iCodexAct };
        const SSharedModels stM = MergeSharedModelTables(aTables, aActives, /*pref codex=*/3);
        // 合并后共享表有 glm-5.3，当前项按基底（Pi）Active 映射到 0——codex 不缺模型。
        QCOMPARE(stM.aModels.size(), 1);
        QCOMPARE(stM.aModels.at(0).sModelId, QStringLiteral("glm-5.3"));
        QCOMPARE(stM.iActive, 0);
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
            // 2026-10-08 设置页模型表收敛：四后端页的模型树全部取消，唯一树在「大模型」页。
            QCOMPARE(aTrees.size(), 1);
            for (QTreeWidget* pTree : aTrees) {
                QCOMPARE(pTree->columnCount(), 7);
                QHeaderView* pHdr = pTree->header();
                for (int i = 0; i < pTree->columnCount(); ++i) {
                    QCOMPARE(pHdr->sectionResizeMode(i), QHeaderView::Interactive);
                }
                QVERIFY(pHdr->stretchLastSection());   // 末列「当前」填满剩余空间
            }
            // 2026-10-08 页序与归属：第 2 页 = 「模型」（2026-10-08 由「大模型」更名），唯一模型树在其中；
            //   claude 跟随开关已随 v3.3.274 后半段调整迁至 Claude Code 页（tab 2，claude 专属语义集中），
            //   四后端参数页不再含任何模型树。
            {
                const QList<QTabWidget*> aTabs = stDlg.findChildren<QTabWidget*>();
                QVERIFY(!aTabs.isEmpty());
                QTabWidget* pTabs = aTabs.first();
                QVERIFY(pTabs->count() >= 6);
                QCOMPARE(pTabs->tabText(1), QStringLiteral("模型"));
                QVERIFY(aTrees.first()->parentWidget() != nullptr);
                QWidget* pTreePage = aTrees.first()->parentWidget()->parentWidget();
                QCOMPARE(pTabs->widget(1), pTreePage);
                QCOMPARE(pTabs->tabText(2), QStringLiteral("Claude Code"));
                QVERIFY(pTabs->widget(2)->findChildren<QCheckBox*>().size() >= 1);   // claude 跟随开关（已迁入本页）
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
            // 2026-10-08 设置页模型表收敛：四后端页的模型树全部取消，唯一树在「大模型」页。
            QCOMPARE(aTrees.size(), 1);
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
        // ⑤ 列表高度随窗口高度自适应（2026-10-08）：拉高对话框 → 树同步长高；
        //   缩回 → 树同步变矮（树在布局里 stretch=1，页尾无 stretch 抢高度）。
        {
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            stDlg.resize(1000, 700);
            stDlg.show();
            QVERIFY(QTest::qWaitForWindowExposed(&stDlg));
            QApplication::processEvents();
            QTreeWidget* pTree = stDlg.findChildren<QTreeWidget*>().first();
            // 模型页非当前页时不参与布局（同②段），先切到前台再量高度。
            for (QWidget* pW = pTree->parentWidget(); pW != nullptr; pW = pW->parentWidget()) {
                if (auto* pTabW = qobject_cast<QTabWidget*>(pW)) {
                    pTabW->setCurrentWidget(pTree->parentWidget()->parentWidget());
                    break;
                }
            }
            QApplication::processEvents();
            const int iH700 = pTree->height();
            stDlg.resize(1000, 950);
            QApplication::processEvents();
            const int iH950 = pTree->height();
            QVERIFY2(iH950 - iH700 >= 200,
                     qPrintable(QStringLiteral("tree 700:%1 -> 950:%2").arg(iH700).arg(iH950)));
            stDlg.resize(1000, 700);
            QApplication::processEvents();
            QVERIFY2(pTree->height() <= iH700 + 2,
                     qPrintable(QStringLiteral("shrink back %1 vs %2")
                         .arg(pTree->height()).arg(iH700)));
        }
    }

    // 2026-10-08 四后端思考强度/思考预算设置接通（DSH/Codex/Hermes 下拉 + claude 预算框）：
    //   默认空/0、落盘回填、选项集合三段。接通前为半截工程（runner 注入/config 存取在，
    //   UI 与主窗口接线缺，设置页无任何入口）。
    void testBackendEffortSettings() {
        // ① 空配置 → 默认项（getter 空/0）。
        {
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            QVERIFY(stDlg.DshEffort().isEmpty());
            QVERIFY(stDlg.CodexEffort().isEmpty());
            QVERIFY(stDlg.HermesEffort().isEmpty());
            QCOMPARE(stDlg.ClaudeThinkTok(), 0);
        }
        // ② 落盘 → 新对话框回填。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveDshEffort(QStringLiteral("high")));
            QVERIFY(stPm.SaveCodexEffort(QStringLiteral("xhigh")));
            QVERIFY(stPm.SaveHermesEffort(QStringLiteral("ultra")));
            QVERIFY(stPm.SaveClaudeMaxThinkTok(8192));
            CSettingsDialog stDlg(&stPm);
            QCOMPARE(stDlg.DshEffort(), QStringLiteral("high"));
            QCOMPARE(stDlg.CodexEffort(), QStringLiteral("xhigh"));
            QCOMPARE(stDlg.HermesEffort(), QStringLiteral("ultra"));
            QCOMPARE(stDlg.ClaudeThinkTok(), 8192);
        }
        // ③ 选项集合：DSH（默认/off/low/high/max）、Codex（+minimal/medium/xhigh）、
        //   Hermes（false/minimal/low/medium/high/ultra）；轻量修改 setter 后 getter 回读。
        {
            CProjectManager stPm;
            CSettingsDialog stDlg(&stPm);
            QComboBox* pDsh = nullptr;
            QComboBox* pCodex = nullptr;
            QComboBox* pHermes = nullptr;
            for (QComboBox* pC : stDlg.findChildren<QComboBox*>()) {
                if (pC->count() == 5 && pC->itemText(1) == QStringLiteral("off")) { pDsh = pC; }
                if (pC->count() == 6 && pC->itemText(5) == QStringLiteral("xhigh")) { pCodex = pC; }
                if (pC->count() == 7 && pC->itemText(1) == QStringLiteral("false")) { pHermes = pC; }
            }
            QVERIFY(pDsh != nullptr);
            QVERIFY(pCodex != nullptr);
            QVERIFY(pHermes != nullptr);
            QCOMPARE(pDsh->itemText(4), QStringLiteral("max"));
            QCOMPARE(pCodex->itemText(1), QStringLiteral("minimal"));
            QCOMPARE(pHermes->itemText(6), QStringLiteral("ultra"));
            pDsh->setCurrentIndex(3);
            QCOMPARE(stDlg.DshEffort(), QStringLiteral("high"));
            pDsh->setCurrentIndex(0);
            QVERIFY(stDlg.DshEffort().isEmpty());
        }
    }

    // 2026-10-08 提示词长度闸门配置（[Adhoc] PromptCtxPctMax 默认 5 / PromptTokMax
    //   默认 0=不限）：空配置缺省、落盘回填两段。
    void testPromptGateSettings() {
        {
            CProjectManager stPm;
            QCOMPARE(stPm.LoadAdhocPromptCtxPctMax(), 5);
            QCOMPARE(stPm.LoadAdhocPromptTokMax(), 0);
            CSettingsDialog stDlg(&stPm);
            QCOMPARE(stDlg.AdhocPromptCtxPctMax(), 5);
            QCOMPARE(stDlg.AdhocPromptTokMax(), 0);
        }
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveAdhocPromptCtxPctMax(8));
            QVERIFY(stPm.SaveAdhocPromptTokMax(20000));
            QCOMPARE(stPm.LoadAdhocPromptCtxPctMax(), 8);
            QCOMPARE(stPm.LoadAdhocPromptTokMax(), 20000);
            CSettingsDialog stDlg(&stPm);
            QCOMPARE(stDlg.AdhocPromptCtxPctMax(), 8);
            QCOMPARE(stDlg.AdhocPromptTokMax(), 20000);
        }
    }

    // 2026.09.23 v3.0.217 回归：助手轮次上限配置化（[Adhoc] MaxTurns，默认 40，钼 10..200）
    //   —— LayoutDesign「打磨布局设计器」12 问 11 次触顶，上限可调的兑底。
    void testAdhocMaxTurnsConfig() {
        // ① 无 config.ini/缺键 → 默认 40。
        {
            CProjectManager stPm;
            QCOMPARE(stPm.LoadAdhocMaxTurns(), 40);
        }
        // ② 保存 → 读回（正常档 120）。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveAdhocMaxTurns(120));
            QCOMPARE(stPm.LoadAdhocMaxTurns(), 120);
        }
        // ③ 写入越界值被 Save 钼制：>200 → 200，<10 → 10。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveAdhocMaxTurns(500));
            QCOMPARE(stPm.LoadAdhocMaxTurns(), 200);
            QVERIFY(stPm.SaveAdhocMaxTurns(1));
            QCOMPARE(stPm.LoadAdhocMaxTurns(), 10);
        }
        // ④ 坏档（绕过 Save 直接写非法原始值）→ Load 兑底回 40。
        {
            const QString sCfg = ConfigPath();
            WritePrivateProfileStringW(L"Adhoc", L"MaxTurns", L"999",
                                       (LPCWSTR)sCfg.utf16());
            CProjectManager stPm;
            QCOMPARE(stPm.LoadAdhocMaxTurns(), 40);
        }
    }

    // 2026.09.23 v3.0.219 回归：工作量评估开关（[Adhoc] MaxTurnsAuto，缺省开）。
    void testAdhocMaxTurnsAutoConfig() {
        // ① 无 config.ini/缺键 → 默认开。
        {
            CProjectManager stPm;
            QVERIFY(stPm.LoadAdhocMaxTurnsAuto());
        }
        // ② 往返：关 → 读回关；再开 → 读回开。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveAdhocMaxTurnsAuto(false));
            QVERIFY(!stPm.LoadAdhocMaxTurnsAuto());
            QVERIFY(stPm.SaveAdhocMaxTurnsAuto(true));
            QVERIFY(stPm.LoadAdhocMaxTurnsAuto());
        }
    }

    // 2026.09.23 v3.1.224 回归：资源管理器显示隐藏目录（[Explorer] HiddenDirs）——
    //   缺键=内置默认（build,.git,.vs,node_modules）；显式配置整体覆盖；空串=全可见
    //   （键存在但值为空 ≠ 缺键，GetPrivateProfileStringW 天然区分）。
    void testExplorerHiddenDirsConfig() {
        // ① 无 config.ini/缺键 → 内置默认；静态默认同源。
        {
            CProjectManager stPm;
            QCOMPARE(stPm.LoadExplorerHiddenDirs(),
                     QStringLiteral("build,.git,.vs,node_modules"));
        }
        QCOMPARE(CProjectManager::DefaultExplorerHiddenDirs(),
                 QStringLiteral("build,.git,.vs,node_modules"));
        // ② 显式配置整体覆盖（用户从清单里删掉 build → build 可见）。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveExplorerHiddenDirs(QStringLiteral(".git,.vs,node_modules")));
            QCOMPARE(stPm.LoadExplorerHiddenDirs(), QStringLiteral(".git,.vs,node_modules"));
        }
        // ③ 空串 = 写空值（键存在）→ 全可见，不是回默认。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveExplorerHiddenDirs(QString()));
            QCOMPARE(stPm.LoadExplorerHiddenDirs(), QString());
        }
        // ④ 删键（手工/外部）→ 回内置默认。
        {
            CProjectManager stPm;
            QVERIFY(stPm.SaveExplorerHiddenDirs(QStringLiteral(".git")));
            const QString sCfg = ConfigPath();
            WritePrivateProfileStringW(L"Explorer", nullptr, nullptr,
                                       (LPCWSTR)sCfg.utf16());   // 整节删除
            QCOMPARE(stPm.LoadExplorerHiddenDirs(),
                     QStringLiteral("build,.git,.vs,node_modules"));
        }
    }

    // 2026.09.26 回归：设置对话框几何逐次漂移（用户报告「每次往下跑 31px」，
    //   根因=构造期 move 在原生窗未建时按 Qt6.8 posIncludesFrame 含框架语义记账）。
    //   修复=showEvent 回贴精确几何。三轮「构造→show→done 落盘→销毁」断言 y 逐轮相等；
    //   offscreen 无原生窗口漂移不发生，QSKIP（口径同 test_execpanel recentDialogGeometryNoDrift）。
    void settingsDialogGeometryNoDrift();
};

void CTestProjectManager::settingsDialogGeometryNoDrift() {
    if (QGuiApplication::platformName().compare(QLatin1String("offscreen"),
                                                Qt::CaseInsensitive) == 0) {
        QSKIP("真实窗口平台专用（offscreen 无原生窗口，漂移不发生）");
    }
    QMainWindow wndParent;
    wndParent.resize(1280, 800);
    wndParent.show();
    QVERIFY(QTest::qWaitForWindowExposed(&wndParent));

    int iPrevY = -1;
    for (int i = 0; i < 3; ++i) {
        CProjectManager stPm;
        CSettingsDialog stDlg(&stPm, &wndParent);
        stDlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stDlg));
        QTest::qWait(200);   // 布局激活/最小尺寸钳位/检测定时器首轮都安定
        const int iY = stDlg.y();
        const int iFy = stDlg.frameGeometry().y();
        qDebug() << "round" << i << "clientY=" << iY << "frameY=" << iFy
                 << "size=" << stDlg.size();
        stDlg.done(0);   // 与用户关闭同路径：done() 内保存 geometry() 后隐藏
        QTest::qWait(50);
        if (iPrevY >= 0) {
            QCOMPARE(iY, iPrevY);
        }
        iPrevY = iY;
    }
}

QTEST_MAIN(CTestProjectManager)
#include "test_projectmanager.moc"
