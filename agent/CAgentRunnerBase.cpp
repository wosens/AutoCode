#include "CAgentRunnerBase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>

// 构造/析构放在 .cpp：锚定基类 vtable（纯虚接口 + QObject 混入，避免每个包含 TU 都内联展开）。
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
            "- claude 后端优先用 MCP 工具 mcp__autocode__sandbox_*"
            "（windows/capture/click/type/keys/tree/restore…）。\n"
            "- 其余后端用命令行：\"%1\" --sandbox windows ｜ --sandbox capture <hwnd> ｜"
            " --sandbox click <hwnd> <x> <y> ｜ --sandbox type <hwnd> <文本> ｜"
            " --sandbox tree <hwnd>\n"
            "- 截图返回 PNG 路径，用读图工具查看后再定位坐标点击。\n"
            "- 禁止物理操控鼠标键盘，禁止 pyautogui/SendInput 类物理输入。")
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
            "- claude 后端优先用 MCP 工具 mcp__autocode__sandbox_*"
            "（windows/capture/click/type/keys/tree/restore…）。\n"
            "- 其余后端用命令行：\"%1\" --sandbox windows ｜ --sandbox capture <hwnd> ｜"
            " --sandbox click <hwnd> <x> <y> ｜ --sandbox type <hwnd> <文本> ｜"
            " --sandbox tree <hwnd>\n"
            "- 截图返回 PNG 路径，用读图工具查看后再定位坐标点击。\n"
            "- 禁止物理操控鼠标键盘，禁止 pyautogui/SendInput 类物理输入%2。\n")
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
