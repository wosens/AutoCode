#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QRect>

#include "CGuideFingerprintScanner.h"

class CAgentRunnerBase;
class CTaskInfo;

// 2026.08.18 自定义大模型条目（config.ini [DshModels]，DSH 后端多条目管理）。
//   字段与 CDeepSeekRunner::SCustomModel 一一对应（CMainWindow 负责转换注入）。
struct SDshModelEntry {
    QString sDisp;      // 显示名
    QString sBaseURL;   // 接入点（anthropic-messages 兼容）
    QString sApiKey;    // 密钥明文（tooltip 已告知明文存储）
    QString sApi;       // API 类型（默认 anthropic-messages）
    QString sModelId;   // 模型 id
    qint64  iCtx    = 200000;   // 上下文窗口（token）
    int     iMaxTok = 32768;    // 单次最大输出
};

// FR-6：代码回退的历史版本元数据（一个备份快照的摘要）。
struct SBackupVersion {
    int      iSeq = 0;        // 版本序号（单调递增，0001 起）
    QString  sTime;           // 备份时刻（ISO，如 2026-08-13T14:22:01）
    QString  sStage;          // 备份时所处阶段（Develop/BugFix/CodeReview…）
    QString  sSummary;        // 修改内容摘要；首次备份为空（无上一版可对比）
};

// 工程管理：负责工程根目录、.autodev 目录结构、guide.md / record.md 的维护，
// 以及任务私有产物目录的创建。guide.md 缺失时通过 claude 扫描生成。
class CProjectManager : public QObject {
    Q_OBJECT

public:
    explicit CProjectManager(QObject* a_pParent = nullptr);

    // 设置工程根目录（绝对路径）
    bool Open(const QString& a_sDir);
    QString ProjectDir() const { return m_sProjectDir; }

    // 确保 .autodev 与 .autodev/tasks 存在
    bool EnsureAutoDev();

    // .autodev/guide.md 是否存在
    bool HasGuide() const;

    // <root>/.autodev
    QString AutoDevDir() const;
    // <root>/.autodev/tasks/<id>
    QString TaskDir(const QString& a_sTaskId) const;
    // 创建任务私有目录
    bool InitTaskDir(const QString& a_sTaskId);

    // 扫描 .autodev/tasks/*/task.json，加载全部历史任务（返回对象无父，调用方接管生命周期）
    QList<CTaskInfo*> LoadAllTasks() const;

    // 读 guide.md 全文（不存在返回空）
    QString ReadGuide() const;
    // 向 .autodev/record.md 追加一行（自动加换行）
    bool AppendRecord(const QString& a_sLine);
    // 向 .autodev/tasks/<id>/run.log 追加一行（首次创建写 UTF-8 BOM，之后纯追加）。
    // 每个任务的完整运行流水（system/assistant/tool/event）落此，便于事后复盘改进。
    bool AppendTaskLog(const QString& a_sTaskId, const QString& a_sLine);

    // —— 对话流持久化文件（#1 助手 / #6 任务）——
    // 助手会话：.autodev/assistant.json（sid + 撤销栈 + 对话块）
    QString AssistantStateFile() const;
    bool WriteAssistantState(const QByteArray& a_sData) const;
    QByteArray ReadAssistantState() const;
    // 任务对话流：.autodev/tasks/<id>/chat.json（仅对话块，便于打开工程恢复日志区）
    QString TaskChatFile(const QString& a_sTaskId) const;
    bool WriteTaskChat(const QString& a_sTaskId, const QByteArray& a_sData) const;
    QByteArray ReadTaskChat(const QString& a_sTaskId) const;
    // 每任务助手存档：.autodev/tasks/<id>/assistant.json（sid + 撤销栈 + guideHash + 对话块 + ctxFill）。
    //   结构与工程级 assistant.json 完全一致；选中任务时助手状态存这里（跟着任务走），无任务回退工程级。
    QString TaskAssistantFile(const QString& a_sTaskId) const;
    bool WriteTaskAssistantState(const QString& a_sTaskId, const QByteArray& a_sData) const;
    QByteArray ReadTaskAssistantState(const QString& a_sTaskId) const;
    // 每任务经验记忆：.autodev/tasks/<id>/memory.md（任务各阶段/助手沉淀的经验，agent 自治维护，
    //   本层只读；写入由 prompt 中的追加规则驱动 agent 用 Edit 完成，任务目录已由 InitTaskDir 保证）。
    QString TaskMemoryFile(const QString& a_sTaskId) const;
    QString ReadTaskMemory(const QString& a_sTaskId) const;

    // 异步生成 guide.md：扫描工程结构调 claude
    void BuildGuideAsync();
    // 2026.08.18 guide 生成/同步 runner 任一运行中（后端切换保护与漂移重建判定用）。
    bool IsGuideBusy() const;

    // 需求4：按任务变更清单（changed_files.md）用独立 agent 增量更新 guide.md。
    //   a_sTaskId 非空 → 用该任务的 changed_files.md；空 → 取最新一份（手动触发）。
    //   无 guide / 无清单 / 上一次同步仍在跑 → 发 sigGuideSyncFinished(false,…) 并返回。
    void SyncGuideAsync(const QString& a_sTaskId);

    // 任务启动前增量同步 guide.md 的文件指纹表（FR1.2/FR1.3/FR1.4）：
    //   Scan 当前工程 → 读 guide → 解析既有指纹表 → Diff 出增/改/删 → 有变化才增量回写。
    //   全程吞错，失败不抛、不阻断任务启动（guide 缺失 / 无指纹段 / IO 错误 → 安全 return）。
    //   无变化时 guide.md 字节不变（FR1.4）。
    void SyncGuideIncremental();

    // 读 guide.md → 解析指纹表（供测试 / 外部观察）。guide 缺失 / 无指纹段 → 返回空表。
    QList<SFileFingerprint> ReadFingerprintTable() const;

    // ---- 最近打开工程历史（MRU）----
    // 记录一条最近打开的工程路径：规范化 → 去重 → 置顶（MRU）→ 持久化。
    // 成功打开工程后由 CMainWindow 调用。
    void AddRecent(const QString& a_sDir);
    // 当前 MRU 历史列表（内存，最前 = 最近打开）。
    QStringList RecentProjects() const;
    // 从 exe 同级 config.ini 载入历史到内存（程序启动时调一次）。
    void LoadRecent();
    // 清空历史（本期 UI 不暴露，预留）。
    void ClearRecent();

    // ---- 主窗口几何持久化（exe 同级 config.ini 的 [Window] 段）----
    // 保存窗口位置/大小/最大化态；落盘失败不抛、不退出（与 SaveRecent 一致）。
    bool SaveWindowGeometry(const QRect& a_stGeometry, bool a_bMaximized);
    // 载入窗口几何；无 config.ini / 缺键时返回 false（调用方用默认几何）。
    bool LoadWindowGeometry(QRect& a_stGeometry, bool& a_bMaximized);

    // ---- FR-4 界面布局持久化：主 QSplitter 比例（exe 同级 config.ini 的 [Layout] 段）----
    // 保存 QSplitter::saveState() 的字节数组（以 Base64 文本落盘）；失败不抛、不退出。
    bool SaveSplitterState(const QByteArray& a_stState);
    // 载入 splitter 状态；无 config.ini / 缺键 → 返回空（调用方判空跳过 restoreState）。
    QByteArray LoadSplitterState();

    // ---- 2026.08.18 任务列表高度持久化：左栏垂直 QSplitter（[Layout] VSplit）----
    // 任务列表/资源管理器之间的拖动分隔条位置，语义与主 Splitter 对完全一致（Base64 文本）。
    bool SaveVSplitState(const QByteArray& a_stState);
    QByteArray LoadVSplitState();

    // ---- FR-4 助手 autocompact：2026.08.15 起改为「比例制」（[Adhoc] AutoCompactPct）----
    // 旧 [Adhoc] AutoCompact（千 token 绝对阈值）已废弃删除。比例为上下文窗口的百分比
    // （10..95），编排器按「窗口 × 比例」换算成 --autocompact <n>k。
    // 落盘失败不抛；Load 无记录/非法值返回 75（默认）。
    bool SaveAutoCompactPct(int a_iPct);
    int  LoadAutoCompactPct() const;
    // 比例制换算：K（千 token）= 窗口 × 比例 ÷ 100 ÷ 1000。窗口优先 [Adhoc] ContextLimit
    // 用户覆盖，未设按模型探测（guide 同步等内置 runner 调 claude 时用）。
    int AutoCompactKFor(const QString& a_sModel) const;
    // 评审阶段并行视角数（[Review] Parallel）：1=串行回退；>1=多视角并行（默认 3）。
    // 无 config.ini/缺键 → 默认 3；非法值（<1 或 >8）兜底回 3。
    bool SaveReviewParallel(int a_iN);
    int  LoadReviewParallel();

    // ---- 阶段联网检索开关（config.ini 的 [Agent] WebSearch）----
    // 2026.08.15：控制各阶段 Claude 是否可用 WebSearch/WebFetch 联网收集信息。
    // 1=允许（默认）；0=全阶段禁用（编排器在 --disallowedTools 追加 WebSearch,WebFetch）。
    // 离线/内网环境改 0 即可，每个阶段启动时读取生效。
    bool SaveWebSearch(bool a_bOn);
    bool LoadWebSearch();

    // ---- 运行策略（config.ini 的 [Policy] 段，设置对话框可改）----
    // 单阶段最大迭代次数（驳回重跑熔断阈值，默认 3；合法 1..9）。
    bool SaveMaxIter(int a_iN);
    int  LoadMaxIter();
    // 驳回后单阶段最大续跑次数（默认 3；合法 1..9）。
    bool SaveMaxResume(int a_iN);
    int  LoadMaxResume();
    // 评审阶段驳回后最大续跑次数（默认 1；合法 1..9）。
    bool SaveReviewResume(int a_iN);
    int  LoadReviewResume();

    // ---- Agent 运行参数（config.ini 的 [Agent] 段，设置对话框可改）----
    // claude 命令（默认 "claude"；可用绝对路径指向指定安装）。
    bool    SaveClaudeCmd(const QString& a_sCmd);
    QString LoadClaudeCmd();
    // ask_user 提问等待超时秒数（默认 600；合法 30..86400，经 mcp-config env 传给 AskServer 子进程）。
    bool SaveAskTimeoutSec(int a_iSec);
    int  LoadAskTimeoutSec();
    // 2026.08.17 停滞检测阈值分钟数（[Agent] StallMinutes，默认 10；合法 1..120）：
    //   claude 进程活着但持续零输出达到阈值 → 判挂起，kill 后按会话断点自动续跑。
    int  LoadStallMinutes();

    // ---- 2026.08.18 Agent 后端与 DeepSeekHarness 参数（[Agent] 段，全局单选）----
    // 后端：claude=1（默认）/ dsh=2（DeepSeekHarness JSON-RPC）。
    bool SaveAgentBackend(int a_iBackend);
    int  LoadAgentBackend();
    // 2026.08.18 轻量自动配置：DSH 仓库根目录（deepseek-harness 目录；bin.js/cordis.yml 按
    //   固定布局由此推导；空=启动时探测 exe 同级 DeepSeekHarness 目录）。
    bool    SaveDshRoot(const QString& a_sRoot);
    QString LoadDshRoot();
    // DSH 三路径（node.exe / bin.js / cordis.yml；空=未配置/按根目录推导，StartStage 前置校验报失败）。
    bool    SaveDshNodePath(const QString& a_sPath);
    QString LoadDshNodePath();
    bool    SaveDshBinJsPath(const QString& a_sPath);
    QString LoadDshBinJsPath();
    bool    SaveDshCordisPath(const QString& a_sPath);
    QString LoadDshCordisPath();
    // DSH provider/model（默认 glm-custom / glm-5.2，内网中继 anthropic-messages API）。
    bool    SaveDshProvider(const QString& a_sProvider);
    QString LoadDshProvider();
    bool    SaveDshModel(const QString& a_sModel);
    QString LoadDshModel();
    // 2026.08.18 DSH 计费单价（$/百万 token）：DSH 只报 token，客户端按单价折算；
    //   默认对齐 claude 后端 Sonnet 档价目（同网关口径）。
    bool    SaveDshPriceInPerM(double a_d);
    double  LoadDshPriceInPerM();
    bool    SaveDshPriceCacheReadPerM(double a_d);
    double  LoadDshPriceCacheReadPerM();
    bool    SaveDshPriceCacheWritePerM(double a_d);
    double  LoadDshPriceCacheWritePerM();
    bool    SaveDshPriceOutPerM(double a_d);
    double  LoadDshPriceOutPerM();
    // 2026.08.18 自定义大模型表（[DshModels] Count/Active/M<i>*）。
    //   a_pActive 带出激活下标（-1 = 无；表空即未启用该功能）。
    QList<SDshModelEntry> LoadDshModels(int* a_pActive = nullptr);
    bool    SaveDshModels(const QList<SDshModelEntry>& a_aModels, int a_iActive);

    // ---- 扫描排除目录（config.ini 的 [Scan] ExcludeDirs，逗号分隔）----
    // 空 = 用内置默认清单（.autodev/build/Execute/.git/.vs/node_modules/doc）。
    bool    SaveScanExcludeDirs(const QString& a_sDirs);
    QString LoadScanExcludeDirs();

    // ---- 备份排除目录（config.ini 的 [Backup] ExcludeDirs，逗号分隔）----
    // 空 = 用内置默认清单（.autodev/build/Execute/.git/.vs/x64）。
    bool    SaveBackupExcludeDirs(const QString& a_sDirs);
    QString LoadBackupExcludeDirs() const;

    // ---- 助手上下文窗口上限持久化（config.ini 的 [Adhoc] ContextLimit）----
    // 0=未设（回退按模型自动探测）；LoadAdhocContextLimit 无记录/非法值返回 0。
    bool   SaveAdhocContextLimit(qint64 a_iLimit);
    qint64 LoadAdhocContextLimit() const;

    // ---- FR-6 代码回退：源码备份 / 恢复 / 版本清单（纯本地 Qt 文件系统操作）----
    // 全量镜像工程源码到 .autodev/tasks/<id>/backups/<seq>/，返回版本序号（0=失败）。
    //   排除 .autodev/build/Execute/.git/.vs/x64 等（防递归，NFR-5）。首次备份不写 summary。
    int  BackupSources(const QString& a_sTaskId, const QString& a_sStage);
    // 回填某版本的修改内容摘要（阶段结束时调，FR-6.3：首次版本无摘要）。
    bool SetBackupSummary(const QString& a_sTaskId, int a_iSeq, const QString& a_sSummary);
    // 列出全部历史版本（按序号倒序，最新在前）。
    QList<SBackupVersion> ListBackups(const QString& a_sTaskId) const;
    // 原子恢复：把工程目录源码恢复为指定版本（临时目录中转，任一步失败回滚到恢复前状态）。
    //   a_sErr 回带错误信息；成功返回 true。二次确认由 UI 层负责。
    bool RestoreSources(const QString& a_sTaskId, int a_iSeq, QString& a_sErr);

signals:
    void sigGuideLog(const QString& a_sLine);
    void sigGuideFinished(bool a_bOk, const QString& a_sMsg);
    void sigGuideSyncFinished(bool a_bOk, const QString& a_sMsg);   // 需求4 guide.md 增量同步结果

private:
    void OnGuideRunnerFinished(bool a_bOk, const QString& a_sResult,
                               qint64 a_iIn, qint64 a_iOut, double a_dCost, const QString& a_sSid);
    void OnGuideSyncRunnerFinished(bool a_bOk, const QString& a_sResult,
                                   qint64 a_iIn, qint64 a_iOut, double a_dCost, const QString& a_sSid);
    // 取 .autodev/tasks/*/changed_files.md 中 mtime 最新的那份路径（手动触发用）；无则空。
    QString FindLatestChangedFiles() const;
    // guide.md 增量同步水位线（.autodev/guide_sync.json）：已同步到的 record.md 行数。
    //   SyncGuideAsync 仅处理水位线之后的新增 record 行；BuildGuideAsync 全量重生成后重置为当前行数。
    QString GuideSyncFile() const;
    int  LoadGuideSyncLines() const;
    void SaveGuideSyncLines(int a_iLines);

    // 取 exe 同级 config.ini 全路径（GetModuleFileName 去文件名 + "config.ini"）。
    QString ConfigFilePath() const;
    // FR-6：取任务备份根目录 .autodev/tasks/<id>/backups。
    QString BackupsDir(const QString& a_sTaskId) const;
    // FR-6（Q4 默认）：备份/恢复时排除的目录名清单（含 .autodev 自身，防递归无限增长，NFR-5）。
    // 2026.08.15：改读 config.ini [Backup] ExcludeDirs（逗号分隔；缺键回退内置默认），
    //   每次备份/恢复时现读，设置对话框保存后即生效。
    QStringList BackupExcludeDirs() const;
    // 路径规范化：cleanPath + 统一为正斜杠 + 去尾部分隔符（用于去重比较）。
    static QString NormalizePath(const QString& a_sDir);
    // 把 m_aRecentProjects 落盘到 config.ini（落盘失败不抛、不退出）。
    void SaveRecent();

    QString       m_sProjectDir;
    CAgentRunnerBase* m_pGuideRunner = nullptr;       // 2026.08.18 基类指针：后端经 CAgentRunnerFactory 全局单选
    CAgentRunnerBase* m_pGuideSyncRunner = nullptr;    // 需求4 guide.md 增量同步专用 runner

    QStringList   m_aRecentProjects;   // MRU，最前为最近
};
