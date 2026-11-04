#pragma once

// 测试沙箱宿主门面（2026-11-01 沙箱动态库化；2026-11-02 双沙箱并行路由）。
// 职责：按 config.ini [Sandbox] 双槽配置（WindowType/VmType）从
//   <exe目录>\TestSandbox\<Type>.dll 动态加载**两个沙箱**并同时运行，
//   Execute 按命令路由：vm_* 前缀 → VM 槽（Linux microVM 硬隔离），
//   其余（windows/capture/click/launch/...）→ 窗口槽（Windows GUI 幽灵沙箱）。
//   两种沙箱互补而非互斥：Windows GUI 程序只能在窗口沙箱测（microVM guest 是
//   Linux，跑不了 PE/GUI），Linux 命令/不可信代码走 microVM 硬隔离。
//   单槽加载失败只降级该槽（窗口槽类型非 GhostSandbox 时回落 GhostSandbox 保
//   GUI 测试；VM 槽失败仅 vm_* 命令不可用），主程序其余功能不受影响（便携原则）。
// 兼容层：对外保留原 CSandboxManager 的方法形状（SSbxCfg/SetProjectRoot/
//   Execute/...），调用点几乎零改动。
// 进程单例：CProjectExplorerPanel（RegisterUserRoot）等无主窗口引用点的
//   豁免登记经 Instance() 取宿主；主窗口构造后立即 SetInstance。

#include <QObject>
#include <QLibrary>
#include <QString>
#include <QJsonObject>
#include <QList>
#include <windows.h>   // DWORD/HANDLE（RegisterUserRoot 豁免登记签名与原实现一致）

// 2026-10-31 编译解堵（另一会话动态库化半成品）：CreateDialog 是 windows.h 的宏
//   （CreateDialogW/A），与本类 Qt 方法名冲突致语法错误；必须在 windows.h 之后
//   解除（Qt 程序内无 Win32 CreateDialog 调用点，安全）。
#ifdef CreateDialog
#undef CreateDialog
#endif

class QDialog;
class QWidget;

class CSandboxHost : public QObject {
    Q_OBJECT

public:
    // 与原 CSandboxManager::SSbxCfg 同构（值对象转发给 dll 的 JSON 配置）。
    // 2026-11-02 双沙箱并行：窗口槽 + VM 槽各自开关/类型（自动按命令路由，
    //   不再是单选 Type——Windows GUI 与 Linux 命令两类测试需求互补）。
    struct SSbxCfg {
        bool bEnable    = true;
        bool bAutoGhost = true;
        bool bRecord    = true;
        int  iFps       = 5;
        int  iRetainDays = 7;
        // 2026-11-02 物理输入拦截：Agent 运行期吞掉合成鼠标/键盘（INJECTED），
        //   机制性根治 Agent 抢占用户鼠标键盘（提示词铁律对模型不可靠）。
        bool bBlockSynth = true;
        // —— 2026-11-02 双沙箱槽位 ——
        bool    bWindowEnable = true;                   // 窗口槽开关（GUI 测试主力）
        QString sWindowType   = QStringLiteral("GhostSandbox");
        bool    bVmEnable     = true;                   // VM 槽开关（硬隔离 Linux 命令）
        QString sVmType       = QStringLiteral("MsbSandbox");
    };

    // 枚举 exe 同级 TestSandbox\*.dll 里可用的沙箱（id+显示名；设置页展示用）。
    //   返回 (id, display) 列表；目录不存在/无可用 dll 返回空表。bOkAll=false
    //   表示存在 dll 但解析失败（设置页可提示）。
    struct SProvider {
        QString sId;          // dll 文件名主干（= sbx_id()）
        QString sDisplayName; // sbx_display_name()
    };
    static QList<SProvider> EnumSandboxes(QString* psErr = nullptr);

    explicit CSandboxHost(QObject* a_pParent = nullptr);
    ~CSandboxHost() override;

    // 加载并启动配置里的全部启用沙箱（类型/开关取自 a_stCfg 双槽字段）。
    //   a_psErr 汇总各槽错误（分号连接）；返回值 = 是否至少一个槽就绪。
    bool Load(const SSbxCfg& a_stCfg, QString* a_psErr = nullptr);

    // —— 以下门面方法签名对齐原 CSandboxManager（广播到全部已加载槽）——
    void SetConfig(const SSbxCfg& a_stCfg);
    void Start();                     // 仅在 Load 之外补启动场景（一般不直调）
    void Shutdown();
    void ReloadConfig();
    void SetProjectRoot(const QString& a_sRoot);
    void ReadoptProjectWindows();
    // 阻塞执行（可长阻塞，调用方负责线程化——CMainWindow 投线程池）。
    // 2026-11-02 按命令路由：vm_* → VM 槽；其余 → 窗口槽；对应槽未就绪时
    //   返回 false 且 a_sOut 带启用指引。
    bool Execute(const QString& a_sCmd, const QJsonObject& a_oArgs, QString& a_sOut);
    // 用户启动豁免登记（句柄移交窗口槽持有；无窗口沙箱时关掉句柄防泄漏）。
    static void RegisterUserRoot(DWORD a_dwPid, HANDLE a_hProc);
    // 管理对话框（窗口槽 dll 创建，宿主父窗口；未加载返回 nullptr）。
    QDialog* CreateDialog(QWidget* a_pParent);

    // 任一槽就绪即 true（旧调用点语义：沙箱能力可用）。
    bool IsLoaded() const;
    // 就绪槽 id 组合（如 "GhostSandbox+MsbSandbox"），状态展示用。
    QString LoadedId() const;
    QString LastError() const { return m_sErr; }

    // 进程单例（主窗口构造后 SetInstance；析构自动摘除）。
    static CSandboxHost* Instance();

private:
    // —— 沙箱槽位：一个 dll 实例的全部状态（2026-11-02 双槽并行）——
    struct SSlot {
        bool      bEnable = false;      // 配置开关
        QString   sType;                // 期望 dll 主干名
        QLibrary* pLib = nullptr;
        QString   sId;                  // 实际加载 id（= dll 主干名）
        QString   sErr;                 // 该槽最近错误
        // ABI 符号缓存（加载时一次 resolve）
        uint32_t  (*fnAbi)() = nullptr;
        const wchar_t* (*fnId)() = nullptr;
        const wchar_t* (*fnDisplayName)() = nullptr;
        const wchar_t* (*fnDescription)() = nullptr;
        bool (*fnStart)(const wchar_t*) = nullptr;
        bool (*fnReload)(const wchar_t*) = nullptr;
        void (*fnSetRoot)(const wchar_t*) = nullptr;
        void (*fnReadopt)() = nullptr;
        void (*fnShutdown)() = nullptr;
        wchar_t* (*fnExecute)(const wchar_t*, const wchar_t*, bool*) = nullptr;
        void (*fnFree)(wchar_t*) = nullptr;
        void (*fnRegUserRoot)(uint32_t, void*) = nullptr;
        void* (*fnCreateDialog)(void*) = nullptr;

        bool Ready() const { return pLib != nullptr && fnAbi != nullptr; }
        void ResetFnPtrs();   // 卸载/失败路径统一清零全部 fn*（防悬垂）
        void Unload();        // shutdown + unload + 清零（重载/析构共用）
    };

    static void SetInstance(CSandboxHost* a_p);   // 供测试重建
    template <typename T> T Resolve(SSlot& a_st, const char* a_sName);
    QString CfgJson() const;                       // SSbxCfg → JSON 串
    QString TakeW(const SSlot& a_st, wchar_t* a_p) const;  // ABI 返回串 → QString（并 sbx_free）
    // 加载单个沙箱 dll 进槽（成功返回空串，否则错误串）。
    QString LoadSingle(SSlot& a_st, const QString& a_sId, const SSbxCfg& a_stCfg);
    // 命令路由：vm_* 前缀 → VM 槽；其余 → 窗口槽。
    SSlot* RouteSlot(const QString& a_sCmd);
    // 槽未就绪时的用户指引文案（含启用路径与该槽错误）。
    QString SlotUnavailableText(const SSlot& a_st, const QString& a_sCmd) const;

    SSlot   m_stWin;         // 窗口槽（Windows GUI 幽灵沙箱）
    SSlot   m_stVm;          // VM 槽（Linux microVM 硬隔离）
    QString m_sErr;          // 最近一次 Load 汇总错误（诊断可见）
    SSbxCfg m_stCfg;
};
