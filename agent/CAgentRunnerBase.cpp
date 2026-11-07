#include "CAgentRunnerBase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <string>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QDateTime>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// 构造/析构放在 .cpp：锚定基类 vtable（纯虚接口 + QObject 混入，避免每个包含 TU 都内联展开）。
// 2026.10.15 进程树终止（说明见头文件）：taskkill /T /F 同步执行，超时 8s，
//   失败静默（直接子进程由调用方 m_pProc->kill() 兼容收尾）。
void CAgentRunnerBase::KillProcTree(quint64 a_uPid) {
    if (a_uPid == 0) {
        return;
    }
    QProcess oKill;
    oKill.start(QStringLiteral("taskkill"),
               {QStringLiteral("/PID"), QString::number(a_uPid),
                QStringLiteral("/T"), QStringLiteral("/F")});
    if (oKill.waitForStarted(2000)) {
        oKill.waitForFinished(8000);
    }
}

CAgentRunnerBase::CAgentRunnerBase(QObject* a_pParent)
    : QObject(a_pParent) {
}

CAgentRunnerBase::~CAgentRunnerBase() = default;

// —— 便携资产统一解析（2026.08.24）：实现说明见头文件同名声明注释 ——
QStringList CAgentRunnerBase::SharedSkillDirs() {
    QStringList aDirs;
    const QString sPortable = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/skills");
    const QString sClaude = QDir::homePath() + QStringLiteral("/.claude/skills");
    if (QFileInfo::exists(sPortable)) {
        aDirs << QDir::toNativeSeparators(sPortable);
    }
    if (QFileInfo::exists(sClaude)) {
        aDirs << QDir::toNativeSeparators(sClaude);
    }
    return aDirs;
}

// —— 子进程环境统一构造（2026.10.10，说明见头文件声明注释）——
QProcessEnvironment CAgentRunnerBase::MergedPathEnvironment() {
    QProcessEnvironment oEnv = QProcessEnvironment::systemEnvironment();
#ifdef _WIN32
    // 注册表两处 PATH 合并串缓存（30 秒 TTL：装完环境后无需重启 AutoCode 即可命中
    //   新 PATH 条目；过短则每次 StartStage 都击注册表，过长则装完后最多延迟半分钟）。
    static QString s_sMergedPathCache;
    static qint64 s_iCacheStampMs = 0;
    const qint64 kiNowMs = QDateTime::currentMSecsSinceEpoch();
    if (s_sMergedPathCache.isEmpty() || kiNowMs - s_iCacheStampMs > 30000) {
        // RegGetValueW：RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ 接受两种类型且自动展开
        //   %SystemRoot% 等引用（QSettings 不展开 REG_EXPAND_SZ，不能用）；缓冲区
        //   逐级加倍重试。任一读失败 → 本轮不合并（回退继承 PATH，行为同旧版）。
        auto fnRegPath = [](HKEY a_hRoot, const wchar_t* a_pSubKey) -> QString {
            DWORD dwNeed = 4096;
            for (int iTry = 0; iTry < 4; ++iTry) {
                QByteArray buf(int(dwNeed) + int(sizeof(wchar_t)), Qt::Uninitialized);
                DWORD dwSize = dwNeed;
                const LSTATUS iRc = RegGetValueW(a_hRoot, a_pSubKey, L"Path",
                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                    nullptr, buf.data(), &dwSize);
                if (iRc == ERROR_SUCCESS) {
                    // dwSize 对 REG_EXPAND_SZ 展开型返回的是原始（未展开）值大小，
                    //   可能大于实际写入的展开串长（实测 HKLM Path：原始 942 字符、
                    //   展开后 932）——按 dwSize 构造 QString 会读过尾，把缓冲区残余
                    //   垃圾/嵌入 NUL 带进 PATH，毁掉子进程环境块（CreateProcess 失败
                    //   或子进程拿到残废 env）。成功时缓冲区必有 NUL 结尾，以首个
                    //   NUL 为真实长度。
                    const wchar_t* pRaw = reinterpret_cast<const wchar_t*>(buf.constData());
                    int cch = 0;
                    while (pRaw[cch] != L'\0') {
                        ++cch;
                    }
                    return QString::fromWCharArray(pRaw, cch);
                }
                if (iRc != ERROR_MORE_DATA) {
                    return QString();
                }
                dwNeed = dwSize + 4096;
            }
            return QString();
        };
        const QString sMachine = fnRegPath(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
        const QString sUser = fnRegPath(HKEY_CURRENT_USER, L"Environment");
        const QString sInherited = oEnv.value(QStringLiteral("PATH"));
        QStringList aParts;
        for (const QString& sPart : { sMachine, sUser, sInherited }) {
            const QString sTrim = sPart.trimmed();
            if (!sTrim.isEmpty()) {
                aParts << sTrim;
            }
        }
        if (aParts.size() > 1) {   // 只有继承一处 = 无可合并，保持原样
            s_sMergedPathCache = aParts.join(QLatin1Char(';'));
        } else {
            s_sMergedPathCache.clear();
        }
        s_iCacheStampMs = kiNowMs;
    }
    if (!s_sMergedPathCache.isEmpty()) {
        oEnv.insert(QStringLiteral("PATH"), s_sMergedPathCache);
    }
    // 2026-11-03 Git Bash 前置（bash WSL 存根根治；探测见 GitBashRootDir）：
    //   C:\Windows\System32\bash.exe 是 WSL 存根——未装发行版的机器每条命令必败
    //   （「适用于 Linux 的 Windows 子系统没有已安装的分发版」）。Machine PATH 中
    //   System32 恒在最前，Git Bash 目录排后段时 bash/sed/grep/awk 全套命中存根或
    //   落空——Agent 全后端的 shell 工具链整体瘫痪（2026-10-17/10-30/11-03 三次
    //   跨会话实害：DataRepeater 等工程 Agent 误报「沙箱持续故障」、构建/回归
    //   多日无法执行的根因；机器环境不可控，便携应用必须在注入点自免疫）。
    //   把 Git Bash 的 usr/bin（真 bash + unix 工具链 sed/grep/awk/timeout）与
    //   bin（bash 壳 + git.exe）前置到 PATH 最前即根治；找不到 Git Bash 时不动
    //   PATH（与旧行为一致，无 Git 的机器本来也跑不了 agent shell）。
    const QString sGitRoot = GitBashRootDir();
    if (!sGitRoot.isEmpty()) {
        QString sPre;
        const QString sUsr = sGitRoot + QStringLiteral("/usr/bin");
        const QString sBin = sGitRoot + QStringLiteral("/bin");
        if (QFileInfo(sUsr + QStringLiteral("/bash.exe")).isFile()) {
            sPre += QDir::toNativeSeparators(sUsr) + QLatin1Char(';');
        }
        if (QFileInfo(sBin + QStringLiteral("/bash.exe")).isFile()) {
            sPre += QDir::toNativeSeparators(sBin) + QLatin1Char(';');
        }
        if (!sPre.isEmpty()) {
            oEnv.insert(QStringLiteral("PATH"),
                        sPre + oEnv.value(QStringLiteral("PATH")));
        }
    }
#else
    // 非 Windows：无注册表 PATH 语义，直接继承（保持行为一致）。
#endif
    return oEnv;
}

// —— Git Bash 安装根探测（2026-11-03 bash WSL 存根根治，说明见头文件声明）——
QString CAgentRunnerBase::GitBashRootDir() {
    static QString s_sRoot;
    static qint64 s_iEmptyStampMs = 0;   // 空结果 TTL（30 秒，装完 Git 不重启即生效）
    if (!s_sRoot.isEmpty()) {
        return s_sRoot;   // 命中永久缓存（安装根不会无故消失）
    }
    const qint64 kiNowMs = QDateTime::currentMSecsSinceEpoch();
    if (s_iEmptyStampMs != 0 && kiNowMs - s_iEmptyStampMs < 30000) {
        return QString();   // 刚探过为空：不重复击注册表/文件系统
    }
    // <root> 有效性：usr/bin/bash.exe（Git for Windows 完整 MSYS2 工具链 + 真 bash）
    //   或 bin/bash.exe（精简壳）任一存在即认；拒绝 Windows 目录（防病理注册表值）。
    QString sFound;
    auto fnTryRoot = [&sFound](const QString& a_sRoot) {
        const QString sRoot = QDir::fromNativeSeparators(
            QDir::cleanPath(a_sRoot.trimmed()));
        if (sRoot.isEmpty()
            || sRoot.startsWith(QStringLiteral("C:/Windows"), Qt::CaseInsensitive)) {
            return false;
        }
        if (QFileInfo(sRoot + QStringLiteral("/usr/bin/bash.exe")).isFile()
            || QFileInfo(sRoot + QStringLiteral("/bin/bash.exe")).isFile()) {
            sFound = sRoot;
            return true;
        }
        return false;
    };
    // ① 注册表 GitForWindows（官方安装器权威锚点；只读——便携铁律允许）。
    {
        QSettings oReg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\GitForWindows"),
                       QSettings::NativeFormat);
        if (!fnTryRoot(oReg.value(QStringLiteral("InstallPath")).toString())) {
            QSettings oReg2(QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\GitForWindows"),
                            QSettings::NativeFormat);
            fnTryRoot(oReg2.value(QStringLiteral("InstallPath")).toString());
        }
    }
    // ② PATH 里 git.exe 反推安装根（便携版/自定义盘符安装；git.exe 不受 WSL 存根
    //    影响，findExecutable 走本进程继承 PATH）。
    if (sFound.isEmpty()) {
        const QString sGit = QStandardPaths::findExecutable(QStringLiteral("git.exe"));
        if (!sGit.isEmpty()) {
            QDir oDir(QFileInfo(sGit).absolutePath());   // <root>/cmd|bin|usr/bin
            for (int i = 0; i < 2 && sFound.isEmpty(); ++i) {
                if (oDir.cdUp()) {
                    fnTryRoot(oDir.absolutePath());
                }
            }
        }
    }
    // ③ 常见安装路径兑底（绿色版无注册表且不在 PATH）。
    if (sFound.isEmpty()) {
        const QStringList aCands = {
            QStringLiteral("C:/Program Files/Git"),
            QStringLiteral("C:/Program Files (x86)/Git"),
            QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
                + QStringLiteral("/Programs/Git"),
        };
        for (const QString& sC : aCands) {
            if (fnTryRoot(sC)) {
                break;
            }
        }
    }
    if (sFound.isEmpty()) {
        s_iEmptyStampMs = kiNowMs;
    }
    s_sRoot = sFound;
    return s_sRoot;
}

// —— 2026-11-07 改进计划 §1B：执行通道探针（bash 可用性，说明见头文件声明）——
namespace {
// 进程级结论缓存（探针/只读两入口共享）：bash 可用性是机器事实，但会随安装/环境
//   变化——成功 10 分钟、失败 45 秒 TTL；a_bForce（面板“重新探测”）清缓存即刻重探。
struct SExecProbeCache {
    QMutex mtx;
    CAgentRunnerBase::SExecChannelProbe st;
    qint64 iStampMs = 0;
};
SExecProbeCache& ExecProbeCache() {
    static SExecProbeCache s;
    return s;
}
}   // namespace

CAgentRunnerBase::SExecChannelProbe CAgentRunnerBase::ProbeExecChannel(bool a_bForce) {
    SExecProbeCache& stCache = ExecProbeCache();
    QMutexLocker stLock(&stCache.mtx);
    const qint64 kiNowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 kiTtlMs = stCache.st.bOk ? 600000 : 45000;
    if (stCache.st.bKnown && !a_bForce
        && kiNowMs - stCache.iStampMs < kiTtlMs) {
        return stCache.st;
    }
    SExecChannelProbe st;
    st.bKnown = true;
#ifdef Q_OS_WIN
    // 与 Agent 同款通道验证 bash 可用性。关键：不用裸 "bash" 名解析——QProcess/
    //   CreateProcess 的可执行搜索不保证用子环境 PATH（实测 PATH 头部已是 Git
    //   usr/bin 仍解析到 System32 的 WSL 存根），必须用 Git Bash 根的绝对路径；
    //   无 Git Bash 时才兑底裸名（无 Git 的机器探针如实报错，与 Agent 体验一致）。
    QString sBash = QStringLiteral("bash");
    const QString sGitRoot = GitBashRootDir();
    if (!sGitRoot.isEmpty()) {
        const QString sGb = QDir::toNativeSeparators(
            sGitRoot + QStringLiteral("/usr/bin/bash.exe"));
        if (QFileInfo::exists(sGb)) {
            sBash = sGb;
        }
    }
    QProcess oProc;
    oProc.setProcessChannelMode(QProcess::MergedChannels);   // stdout+stderr 合并取证据
    oProc.setProcessEnvironment(MergedPathEnvironment());
    oProc.setWorkingDirectory(QDir::tempPath());
    oProc.start(sBash, {QStringLiteral("-c"), QStringLiteral("echo ok")});
    if (!oProc.waitForStarted(3000)) {
        st.bOk = false;
        st.sDetail = QStringLiteral(
            "bash 不可用（PATH 未找到可用的 bash）——请安装 Git for Windows"
            "（https://git-scm.com）或启用 WSL 发行版后重试");
    } else if (!oProc.waitForFinished(5000)) {
        oProc.kill();
        oProc.waitForFinished(2000);
        st.bOk = false;
        st.sDetail = QStringLiteral(
            "bash 启动后无响应（疑似 WSL 存根挂起/发行版损坏）——"
            "建议安装 Git for Windows 后重试");
    } else {
        const QString sOut = DecodeSubprocessText(oProc.readAll());
        if (oProc.exitStatus() == QProcess::NormalExit && oProc.exitCode() == 0
            && sOut.contains(QStringLiteral("ok"))) {
            st.bOk = true;
            st.sDetail = QStringLiteral("bash 通道正常");
        } else {
            st.bOk = false;
            st.sDetail = QStringLiteral(
                "bash 执行失败（退出码 %1）：%2——命令类验证不可用，请修复环境后重试")
                .arg(oProc.exitCode())
                .arg(sOut.simplified().left(160));
        }
    }
#else
    // 非 Windows：sh 即可（Agent 命令通道原生）。
    QProcess oProc;
    oProc.start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("echo ok")});
    if (oProc.waitForStarted(2000) && oProc.waitForFinished(4000)
        && oProc.exitCode() == 0) {
        st.bOk = true;
        st.sDetail = QStringLiteral("shell 通道正常");
    } else {
        st.bOk = false;
        st.sDetail = QStringLiteral("sh 不可用——命令类验证不可用");
    }
#endif
    stCache.st = st;
    stCache.iStampMs = kiNowMs;
    return st;
}

CAgentRunnerBase::SExecChannelProbe CAgentRunnerBase::ExecChannelCached() {
    // 只读缓存：不探测、不阻塞（bKnown=false = 无结论，调用方自行决定是否后台探）。
    SExecProbeCache& stCache = ExecProbeCache();
    QMutexLocker stLock(&stCache.mtx);
    return stCache.st;
}

// —— 2026-11-07 改进计划 §1D：子进程输出编码容错解码（说明见头文件声明）——
QString CAgentRunnerBase::DecodeSubprocessText(const QByteArray& a_aBytes) {
    // 纯 ASCII（含空串）快速路径：三种编码解码结果一致。
    bool bPureAscii = true;
    for (char c : a_aBytes) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            bPureAscii = false;
            break;
        }
    }
    if (bPureAscii) {
        return QString::fromLatin1(a_aBytes);
    }
#ifdef Q_OS_WIN
    // 严格 UTF-8（Agent CLI 均输出 UTF-8，占绝对多数）；非 UTF-8 再试 GB18030/ACP。
    //   写法同 core/CFileSearcher（std::wstring 缓冲 + MultiByteToWideChar 严格校验）。
    auto fnMbToW = [&](UINT a_uCp, QString* a_pOut) -> bool {
        const int nLen = int(a_aBytes.size());
        const int nW = MultiByteToWideChar(a_uCp, MB_ERR_INVALID_CHARS,
                                           a_aBytes.constData(), nLen, nullptr, 0);
        if (nW <= 0) {
            return false;
        }
        std::wstring sBuf(size_t(nW), L'\0');
        MultiByteToWideChar(a_uCp, MB_ERR_INVALID_CHARS,
                            a_aBytes.constData(), nLen, sBuf.data(), nW);
        *a_pOut = QString::fromStdWString(sBuf);
        return true;
    };
    QString s;
    if (fnMbToW(CP_UTF8, &s)) {
        return s;
    }
    if (fnMbToW(54936 /*GB18030：覆盖 GB2312/GBK，本地化报错主流*/, &s)) {
        return s;
    }
    if (fnMbToW(CP_ACP, &s)) {
        return s;
    }
#endif
    return QString::fromUtf8(a_aBytes);   // 最终兑底：有损解（替换符），不返回空。
}

QString CAgentRunnerBase::SharedSkillsCatalog() {
    return SharedSkillsCatalogFromDirs(SharedSkillDirs());
}

QString CAgentRunnerBase::SharedSkillsCatalogFromDirs(const QStringList& aDirs) {
    // 2026-09-26 头部铁律改为内置沙箱（主进程常驻能力，无需任何技能安装）：Agent
    //   启动的 GUI 窗口被 AutoCode 自动隐藏收纳（用户无感），Agent 用 sandbox_*
    //   MCP 工具（claude）或 "<AutoCode.exe>" --sandbox 命令（全后端）操控/截图。
    //   实测旧方案（只在清单里列 ui-sandbox 技能 + 铁律块依赖该技能存在）弱模型
    //   仍经常不走沙箱直接物理操控；改为无条件点名内置能力 + 内联 exe 全路径
    //   （cmd/bash 都可直接引用）。ui-sandbox 技能若存在仍钉选置顶作补充文档。
    struct SkillItem { QString sName; QString sDesc; QString sPath; };
    QList<SkillItem> aSkills;
    QSet<QString> oVisited;
    QList<QPair<QString, int>> aStack;   // <目录， 深度>
    for (const QString& sDir : aDirs) {
        aStack.append({ sDir, 0 });
    }
    const QStringList aJunk = { QStringLiteral("node_modules"), QStringLiteral("venv"),
                                QStringLiteral("__pycache__"), QStringLiteral("build"),
                                QStringLiteral("dist"), QStringLiteral(".git") };
    while (!aStack.isEmpty()) {
        const QPair<QString, int> stTop = aStack.takeLast();
        const QString sDir = stTop.first;
        const int iDepth = stTop.second;
        const QString sKey = QDir(sDir).absolutePath().toLower();
        if (oVisited.contains(sKey) || iDepth > 4) {
            continue;
        }
        oVisited.insert(sKey);
        const QString sSkillMd = sDir + QStringLiteral("/SKILL.md");
        if (QFileInfo::exists(sSkillMd)) {
            SkillItem stItem;
            stItem.sPath = QDir::toNativeSeparators(sSkillMd);
            stItem.sName = QFileInfo(sDir).fileName();
            // frontmatter 解析：只看前 4KB（--- 首尾行之间的 name/description 单行值）。
            CFrwDevice stFile(sSkillMd);
            if (stFile.open(QIODevice::ReadOnly)) {
                const QString sHead = QString::fromUtf8(stFile.read(4096));
                stFile.close();
                const QStringList aLines = sHead.split(QLatin1Char('\n'));
                bool bInFm = false;
                for (const QString& sLine : aLines) {
                    const QString sTrimmed = sLine.trimmed();
                    if (sTrimmed == QStringLiteral("---")) {
                        if (!bInFm) {
                            bInFm = true;
                            continue;
                        }
                        break;   // frontmatter 结束
                    }
                    if (!bInFm) {
                        continue;
                    }
                    if (sTrimmed.startsWith(QStringLiteral("name:"))) {
                        const QString sV = sTrimmed.mid(5).trimmed();
                        if (!sV.isEmpty()) {
                            stItem.sName = sV;
                        }
                    } else if (sTrimmed.startsWith(QStringLiteral("description:"))) {
                        QString sV = sTrimmed.mid(12).trimmed();
                        if (sV.size() > 120) {
                            sV = sV.left(120) + QStringLiteral("…");
                        }
                        stItem.sDesc = sV;
                    }
                }
            }
            aSkills.append(stItem);
        }
        if (iDepth < 4) {
            const QDir stDir(sDir);
            const QFileInfoList aSub = stDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
            for (const QFileInfo& stFi : aSub) {
                const QString sName = stFi.fileName();
                if (sName.startsWith(QLatin1Char('.')) || aJunk.contains(sName)) {
                    continue;
                }
                aStack.append({ stFi.absoluteFilePath(), iDepth + 1 });
            }
        }
    }
    if (aSkills.isEmpty()) {
        // 无任何技能也返回沙箱铁律块（内置能力不依赖技能安装）。
        QString sSbxOnly = QStringLiteral(
            "【GUI 测试铁律——AutoCode 内置沙箱】涉及程序界面/GUI/窗口验证的任务，一律使用"
            "内置测试沙箱（已内置，无需安装）：\n"
            "- 由你启动的 GUI 程序窗口会被自动隐藏收纳（不抢焦点、点击穿透，用户无感）；"
            "窗口对用户不可见，必须以截图驱动验证。\n"
            "- 物理模拟无效：Agent 运行期间宿主自动拦截并丢弃一切合成鼠标/键盘"
            "（SendInput/pyautogui/keybd_event 等物理操控一律无效），不要尝试——"
            "界面驱动只认 sandbox_* 工具（消息级，天然无扰）。\n"
            "- 例外（硬件级点击）：目标只认硬件输入（消息级无效/假阳性，如任务栏/托盘/"
            "shell 窗口）时，用 sandbox_hclick（宿主代发硬件点击，--sandbox hclick "
            "<hwnd> <x> <y>）——仍禁止自发 SendInput/pyautogui（必被拦截无效）。\n"

            "- claude 后端优先用 MCP 工具 mcp__autocode__sandbox_*"
            "（launch/windows/capture/click/type/keys/tree/restore/add…）。\n"
            "- 其余后端用命令行：\"%1\" --sandbox windows ｜ --sandbox capture <hwnd> ｜"
            " --sandbox click <hwnd> <x> <y> ｜ --sandbox type <hwnd> <文本> ｜"
            " --sandbox tree <hwnd>\n"
            "- 启动被测 GUI 程序：优先 --sandbox launch <exe全路径> [参数...]（主进程代启动，"
            "窗口弹出即自动收纳）；Bash 直接 ./xxx.exe & 仅限工程目录内的 exe——"
            "工程外副本（低完整性验证工作区等任何工程目录外路径）必须 sandbox launch"
            "拉起：链断后沙箱认不出工程外进程，窗口会裸弹用户桌面（实测实害：验证"
            "助手在工程外工作区反复重启被测程序弹窗骚扰）。禁止用 powershell "
            "Start-Process / cmd start / explorer 等中间人方式启动：中间 shell 秒退会切断"
            "进程树，沙箱认不出窗口，测试窗口会裸奍弹到用户桌面。也禁止把被测 exe 复制到"
            "工程外目录（如 %TEMP%）再启动：隔离验证请在工程目录内建 .autodev 子目录放"
            "副本，或直接 sandbox launch——工程外副本一旦链断沙箱认不出，窗口会弹到"
            "用户桌面打扰工作。\n"
            "- 验证文件内容用命令行读（Get-Content/type），不要开记事本/看图器等 GUI "
            "查看器；被测程序（含双击图标拉起的记事本等）每个用例验证完立即按 PID "
            "taskkill /PID <pid> /F 清理，不要留后台——本助手停止/退出后遗留进程会被 "
            "AutoCode 出生登记 janitor 兑底终结（根退出+超 10 分钟即杀）。\n"
            "- 截图返回 PNG 路径，用读图工具查看后再定位坐标点击。\n"
            "- 若目标窗口不在 sandbox_windows 列表里（AutoCode 重启过/进程早于本轮启动），"
            "先用 sandbox add <hwnd> 把它收进沙箱再操控，不要去动可见窗口。"
            "沙箱只收纳本工程程序：系统应用（浏览器/记事本/Office 等装在 Windows 或 "
            "Program Files 目录）不会也不允许被收纳，不要对它们 add 或操控。\n"
            "- 禁止物理操控鼠标键盘：SetCursorPos / mouse_event / SendInput / "
            "keybd_event / pyautogui 及 python/ctypes 自制 Win32 输入一律禁用"
            "——用户正在用这台电脑，真实输入会直接打断用户工作。拖拽/悬停等测试"
            "同样不得移动真实光标：用消息级链路（PostMessage WM_LBUTTONDOWN/"
            "WM_MOUSEMOVE/WM_LBUTTONUP 等）即可触发 TVN_BEGINDRAG 类拖拽。若你断定"
            "目标程序只认硬件级输入，必须停下输出证据让用户确认，不得擅自切换"
            "真实输入；测试全程禁止挪动/最小化/置顶用户桌面上不属于被测程序的其他"
            "窗口。\n"
            "- 禁止用系统应用当测试夹具（记事本/画图/计算器/浏览器/Office 等）："
            "系统应用按设计不可被沙箱收纳，窗口会直接弹在用户桌面打断工作。需要"
            "「一个窗口」做测试（枚举/截图/焦点验证等）时，用 sandbox launch 拉起"
            "工程内自建 exe——幽灵化窗口仍是真实窗口（EnumWindows 可见、可截图"
            "可点按），测试语义不变且用户无感。")
            .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
        return sSbxOnly;
    }
    // 2026.09.13 钉选：ui-sandbox 是 AutoCode GUI 测试主技能（全局 CLAUDE.md 铁律引用
    //   「UI 测试→ui-sandbox，禁物理鼠标键盘」），按名字升序截断时永远排在字母表尾部
    //   进不了清单——命中者置顶，永不被截断。命中多个则按扫描序全置顶。
    static const QSet<QString> sPinned = { QStringLiteral("ui-sandbox") };
    QList<SkillItem> aPinned;
    for (int i = aSkills.size() - 1; i >= 0; --i) {
        if (sPinned.contains(aSkills.at(i).sName.toLower())) {
            aPinned.prepend(aSkills.takeAt(i));
        }
    }
    if (!aPinned.isEmpty()) {
        aPinned += aSkills;
        aSkills = aPinned;
    }
    // 2026.09.13 GUI 测试强指令：旧版仅当本机存在 ui-sandbox 技能才加强令；2026-09-26
    //   起内置沙箱成为主通道，铁律块无条件输出（见函数头注释），技能仅作补充。
    bool bPinnedSandbox = false;
    for (const SkillItem& st : std::as_const(aPinned)) {
        if (st.sName.compare(QStringLiteral("ui-sandbox"), Qt::CaseInsensitive) == 0) {
            bPinnedSandbox = true;
            break;
        }
    }
    // 目录文本（两级清单）：头部铁律（内置沙箱） + ①详情区——总 12KB 预算内列名称/描述/
    //   SKILL.md 路径；②截断后的剩余技能压成「目录名」顿号名串兑底——旧版直接截断丢弃，
    //   字母序靠后的技能（如 ui-sandbox）在 codex/hermes 的注入清单里从未出现过，
    //   模型根本不知道它存在（用户实测「助手不自动使用沙箱技能」根因之一）。
    QString sOut = QStringLiteral(
        "以下是可选技能（Skills）清单。当任务与某条技能的名称/描述明确相关时，"
        "先用文件读取工具完整阅读其 SKILL.md，再按技能内容执行；"
        "未列出的技能不要臆造：\n");
    {
        const QString sSbx = QStringLiteral(
            "【GUI 测试铁律——AutoCode 内置沙箱】涉及程序界面/GUI/窗口验证的任务，一律使用"
            "内置测试沙箱（已内置，无需安装，优先于任何技能）：\n"
            "- 由你启动的 GUI 程序窗口会被自动隐藏收纳（不抢焦点、点击穿透，用户无感）；"
            "窗口对用户不可见，必须以截图驱动验证。\n"
            "- 物理模拟无效：Agent 运行期间宿主自动拦截并丢弃一切合成鼠标/键盘"
            "（SendInput/pyautogui/keybd_event 等物理操控一律无效），不要尝试——"
            "界面驱动只认 sandbox_* 工具（消息级，天然无扰）。\n"
            "- 例外（硬件级点击）：目标只认硬件输入（消息级无效/假阳性，如任务栏/托盘/"
            "shell 窗口）时，用 sandbox_hclick（宿主代发硬件点击，--sandbox hclick "
            "<hwnd> <x> <y>）——仍禁止自发 SendInput/pyautogui（必被拦截无效）。\n"

            "- claude 后端优先用 MCP 工具 mcp__autocode__sandbox_*"
            "（launch/windows/capture/click/type/keys/tree/restore/add…）。\n"
            "- 其余后端用命令行：\"%1\" --sandbox windows ｜ --sandbox capture <hwnd> ｜"
            " --sandbox click <hwnd> <x> <y> ｜ --sandbox type <hwnd> <文本> ｜"
            " --sandbox tree <hwnd>\n"
            "- 启动被测 GUI 程序：优先 sandbox launch <exe全路径> [参数...]（MCP 为 "
            "sandbox_launch；主进程代启动，窗口弹出即自动收纳）；Bash 直接 "
            "./xxx.exe & 仅限工程目录内的 exe——工程外副本（低完整性验证工作区等"
            "任何工程目录外路径）必须 sandbox launch 拉起：链断后沙箱认不出工程外"
            "进程，窗口会裸弹用户桌面（实测实害：验证助手在工程外工作区反复重启"
            "被测程序弹窗骚扰）。禁止用 powershell Start-Process / cmd start / "
            "explorer 等中间人方式启动：中间 shell 秒退会切断进程树，沙箱认不出"
            "窗口，测试窗口会裸奍弹到用户桌面。也禁止把被测 exe 复制到工程外目录"
            "（如 %TEMP%）再启动：隔离验证请在工程目录内建 .autodev 子目录放副本，"
            "或直接 sandbox launch——工程外副本一旦链断沙箱认不出，窗口会弹到"
            "用户桌面打扰工作。\n"
            "- 验证文件内容用命令行读（Get-Content/type），不要开记事本/看图器等 GUI "
            "查看器；被测程序（含双击图标拉起的记事本等）每个用例验证完立即按 PID "
            "taskkill /PID <pid> /F 清理，不要留后台——本助手停止/退出后遗留进程会被 "
            "AutoCode 出生登记 janitor 兑底终结（根退出+超 10 分钟即杀）。\n"
            "- 截图返回 PNG 路径，用读图工具查看后再定位坐标点击。\n"
            "- 若目标窗口不在 sandbox_windows 列表里（AutoCode 重启过/进程早于本轮启动），"
            "先用 sandbox add <hwnd> 把它收进沙箱再操控，不要去动可见窗口。"
            "沙箱只收纳本工程程序：系统应用（浏览器/记事本/Office 等）不会也不允许被"
            "收纳，不要对它们 add 或操控，也不要动用户自己打开的窗口。\n"
            "- 禁止物理操控鼠标键盘：SetCursorPos / mouse_event / SendInput / "
            "keybd_event / pyautogui 及 python/ctypes 自制 Win32 输入一律禁用——"
            "用户正在用这台电脑，真实输入会直接打断用户工作（实测实害：拖拽测试"
            "反复移动用户鼠标致无法正常办公）。拖拽/悬停等测试同样不得移动真实光标："
            "用消息级链路（PostMessage WM_LBUTTONDOWN/WM_MOUSEMOVE/WM_LBUTTONUP 等）"
            "即可触发 TVN_BEGINDRAG 类拖拽。若你断定目标程序只认硬件级输入，必须停下"
            "输出证据让用户确认，不得擅自切换真实输入；测试全程禁止挪动/最小化/置顶"
            "用户桌面上不属于被测程序的其他窗口。\n"
            "- 禁止用系统应用当测试夹具（记事本/画图/计算器/浏览器/Office 等）："
            "系统应用按设计不可被沙箱收纳，窗口会直接弹在用户桌面打断工作（实测"
            "实害：验证助手拉起记事本验证窗口列表，弹窗骚扰用户）。需要「一个窗口」"
            "做测试（枚举/截图/焦点验证等）时，用 sandbox launch 拉起工程内自建"
            "exe——幽灵化窗口仍是真实窗口（EnumWindows 可见、可截图可点按），测试"
            "语义不变且用户无感%2。\n")
            .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()),
                 bPinnedSandbox
                     ? QStringLiteral("；更细操作指引可读 ui-sandbox 技能的 SKILL.md")
                     : QString());
        sOut += sSbx;
    }
    qint64 iBytes = sOut.toUtf8().size();
    for (int i = 0; i < aSkills.size(); ++i) {
        const SkillItem& stItem = aSkills.at(i);
        const QString sLine = QStringLiteral("- %1：%2（SKILL.md：%3）\n")
                                  .arg(stItem.sName,
                                       stItem.sDesc.isEmpty() ? QStringLiteral("（无描述）") : stItem.sDesc,
                                       stItem.sPath);
        const qint64 iLine = sLine.toUtf8().size();
        if (iBytes + iLine > 12 * 1024) {
            // 剩余技能压成名字串（名字即目录名，模型可推导 SKILL.md 路径）
            QStringList aNames;
            for (int j = i; j < aSkills.size(); ++j) {
                aNames.append(aSkills.at(j).sName);
            }
            const QString sTail = QStringLiteral(
                "（技能过多已截断，以下其余技能仅有名字，各自同名子目录内即 SKILL.md：%1）\n")
                .arg(aNames.join(QStringLiteral("、")));
            if (iBytes + sTail.toUtf8().size() <= 16 * 1024) {
                sOut += sTail;
            }
            break;
        }
        sOut += sLine;
        iBytes += iLine;
    }
    return sOut.trimmed();
}

QString CAgentRunnerBase::SharedGlobalContextContent() {
    const QString sPortable = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/CLAUDE.md");
    const QString sClaude = QDir::homePath() + QStringLiteral("/.claude/CLAUDE.md");
    // 2026.09.13 两层拼接勿回退：exe 侧便携层是《部署环境约定》（config.ini GBK 禁令
    //   等），~/.claude/CLAUDE.md 是用户全局规则（含「UI 测试→ui-sandbox、禁物理键鼠」
    //   铁律）——两份是不同文档；旧版「取第一个命中」会让便携层把全局层整个遮蔽
    //   （本机实锤：只注 1.6KB 环境约定、UI 测试铁律从未到达 pi/dsh/codex/hermes）。
    //   改为两份独立校验后按序拼接（各自保留 64KB 单份异常上限，总量不再额外限）。
    QString sOut;
    const QStringList aCandidates = { sPortable, sClaude };
    for (const QString& sPath : aCandidates) {
        QFileInfo stInfo(sPath);
        if (!stInfo.exists() || !stInfo.isFile()) {
            continue;
        }
        // 上限保护（每份 64KB）：超大文件视为异常，不注入该份。
        if (stInfo.size() > 64 * 1024) {
            continue;
        }
        CFrwDevice stFile(sPath);
        if (!stFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString sContent = QString::fromUtf8(stFile.readAll()).trimmed();
        if (!sContent.isEmpty()) {
            if (!sOut.isEmpty()) {
                sOut += QStringLiteral("\n\n---\n\n");
            }
            sOut += sContent;
        }
    }
    return sOut;
}

// 2026-09-12 手工审批弹窗通道（实现见头文件声明注释）：管道名口径同
// CMainWindow::StartAskServer（AutoCodeMcpAsk-<pid>，PID 绑定本实例，多实例互不串扰）。
void CAgentRunnerBase::InjectAskEnv(QProcessEnvironment& a_oEnv) {
    a_oEnv.insert(QStringLiteral("AUTOCODE_ASK_EXE"),
                  QCoreApplication::applicationFilePath());
    a_oEnv.insert(QStringLiteral("AUTOCODE_ASK_PIPE"),
                  QStringLiteral("AutoCodeMcpAsk-%1").arg(QCoreApplication::applicationPid()));
}
