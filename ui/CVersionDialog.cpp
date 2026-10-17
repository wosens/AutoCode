// CVersionDialog.cpp —— 后端版本管理对话框（任务 23183ccb）。
// T3：骨架 + 本地版本加载（DSH/Pi/Codex 同步直读，Claude 异步探测 + npm 回退）
//     + 纯函数（PickLatestTag/TagToVersion/Verdict，T6 单测靶点）。
// T4：QueryLatestAll 联网查询与 OnUpdateBackend 更新分派（占位实装见各 TODO）。

#include "CVersionDialog.h"

#include "CClaudeRunner.h"
#include "CCodexRunner.h"
#include "CDeepSeekRunner.h"
#include "CHermesRunner.h"
#include "CPiAgentRunner.h"

#include <QCoreApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVersionNumber>
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

namespace {

// 行号 ↔ 后端号恒等（1..5 行序即后端号，表格行直接用后端号索引）。
const int kiColBackend = 0;   // 后端
const int kiColLocal   = 1;   // 当前版本
const int kiColLatest  = 2;   // 最新版本
const int kiColVerdict = 3;   // 结论
const int kiColAction  = 4;   // 操作

const int kiClaudeLocalTimeoutMs = 10 * 1000;   // Claude 本地探测硬超时
const int kiQueryTimeoutMs       = 15 * 1000;   // 联网查询硬超时（npm/ls-remote）

// package.json → version（open 失败/解析错/无字段→空串）。
QString fnReadPkgVersion(const QString& a_sPkgPath) {
    CFrwDevice stFile(a_sPkgPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll(), &stErr);
    stFile.close();
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        return QString();
    }
    return oDoc.object().value(QStringLiteral("version")).toString();
}

} // namespace

CVersionDialog::CVersionDialog(FBusyCheck a_fnBusy, QWidget* a_pParent)
    : QDialog(a_pParent), m_fnBusy(std::move(a_fnBusy)) {
    SetupUi();
    RefreshLocalVersions();
}

CVersionDialog::~CVersionDialog() {
    // 关闭＝放弃在飞查询：杀净全部子进程/停表（QProcess/QTimer 父子关系兜底，
    //   此处显式收敛避免退出路径残留孤儿 cmd/npm/git）。
    // 安全序（评审 C1/M1 根因修复）：Windows 上 QProcess::waitForFinished 会
    //   **同步** emit finished/errorOccurred（qprocess_win.cpp processFinished
    //   直调），若在 QHash 迭代中 kill+wait，回调内 m_aProc.take() 会打穿正在
    //   遍历的迭代器（UB）——故：①先置析构标志短路业务回调（防析构中弹错误
    //   框/同步跑 npm root -g 卡关窗）；②以键快照逐键 KillPooledProcess
    //   （其 take→disconnect→kill 顺序天然防嵌套 take，不再迭代哈希本体）。
    m_bTearingDown = true;
    const QList<int> aProcKeys = m_aProc.keys();
    for (const int iKey : aProcKeys) {
        KillPooledProcess(iKey);
    }
    const QList<int> aTimerKeys = m_aTimer.keys();
    for (const int iKey : aTimerKeys) {
        if (QTimer* pTimer = m_aTimer.take(iKey); pTimer != nullptr) {
            pTimer->stop();
            pTimer->deleteLater();
        }
    }
}

void CVersionDialog::SetupUi() {
    setWindowTitle(QStringLiteral("后端版本管理"));
    QFont stFont = font();
    stFont.setPointSize(11);
    setFont(stFont);

    m_pTable = new QTableWidget(5, 5, this);
    // 2026.09.20 列表字体显式 11 号（用户要求；不依赖继承，若全局默认变动仍恒 11）
    //   + 行高 28px（11 号字舒适行高，防视觉偏小拥挤），窗口高度随行高微增。
    m_pTable->setFont(stFont);
    m_pTable->verticalHeader()->setDefaultSectionSize(28);
    m_pTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("后端") << QStringLiteral("当前版本")
        << QStringLiteral("最新版本") << QStringLiteral("结论") << QStringLiteral("操作"));
    m_pTable->setVerticalHeaderLabels(QStringList()
        << QStringLiteral("1") << QStringLiteral("2") << QStringLiteral("3")
        << QStringLiteral("4") << QStringLiteral("5"));
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    m_pTable->verticalHeader()->setVisible(true);
    const int aiColWidth[5] = { 130, 120, 120, 200, 90 };
    for (int iCol = 0; iCol < 5; ++iCol) {
        m_pTable->setColumnWidth(iCol, aiColWidth[iCol]);
    }

    const QString asNames[6] = {
        QString(),
        QStringLiteral("Claude Code"),
        QStringLiteral("DeepSeekHarness"),
        QStringLiteral("PIAgent"),
        QStringLiteral("CodexHarness"),
        QStringLiteral("Hermes"),
    };
    for (int iBackend = 1; iBackend <= 5; ++iBackend) {
        const int iRow = iBackend - 1;
        QTableWidgetItem* pItem = new QTableWidgetItem(asNames[iBackend]);
        pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
        m_pTable->setItem(iRow, kiColBackend, pItem);
        SetLocalCell(iBackend, QStringLiteral("读取中…"));
        SetLatestCell(iBackend, QStringLiteral("未查询"));
        SetVerdictCell(iBackend, Verdict(QString(), QString(), iBackend));
        // 行内更新按钮：可用性随本地安装态联动（UpdateUpdateButtons）。
        m_aBtnUpdate[iBackend] = new QPushButton(QStringLiteral("更新"), m_pTable);
        m_aBtnUpdate[iBackend]->setEnabled(false);
        m_pTable->setCellWidget(iRow, kiColAction, m_aBtnUpdate[iBackend]);
        const int iCaptured = iBackend;
        connect(m_aBtnUpdate[iCaptured], &QPushButton::clicked, this,
                [this, iCaptured]() { OnUpdateBackend(iCaptured); });
    }

    m_pBtnRefresh = new QPushButton(QStringLiteral("刷新版本"), this);
    m_pBtnQuery   = new QPushButton(QStringLiteral("查询最新版(联网)"), this);
    m_pBtnClose   = new QPushButton(QStringLiteral("关闭"), this);

    QHBoxLayout* pBtnRow = new QHBoxLayout();
    pBtnRow->addWidget(m_pBtnRefresh);
    pBtnRow->addWidget(m_pBtnQuery);
    pBtnRow->addStretch(1);
    pBtnRow->addWidget(m_pBtnClose);

    m_pLblInfo = new QLabel(this);

    QVBoxLayout* pMain = new QVBoxLayout(this);
    pMain->addWidget(m_pTable);
    pMain->addLayout(pBtnRow);
    pMain->addWidget(m_pLblInfo);

    connect(m_pBtnRefresh, &QPushButton::clicked, this, [this]() {
        RefreshLocalVersions();
    });
    connect(m_pBtnQuery, &QPushButton::clicked, this, [this]() {
        QueryLatestAll();
    });
    connect(m_pBtnClose, &QPushButton::clicked, this, &QDialog::reject);

    resize(760, 380);   // 2026.09 增 Hermes 行 5 + 2026.09.20 行高 28px：随行数/行高加高
    UpdateInfoLine();
}

void CVersionDialog::RefreshLocalVersions() {
    // —— 行 1 Claude Code：异步探测（cmd /c claude --version）→ npm 包回退 ——
    KillPooledProcess(EProcClaudeLocal);
    SetLocalCell(1, QStringLiteral("读取中…"));
    StartPooledProcess(EProcClaudeLocal,
        QStringLiteral("cmd.exe"),
        QStringList() << QStringLiteral("/c") << CClaudeRunner::ClaudeCmd()
                      << QStringLiteral("--version"),
        kiClaudeLocalTimeoutMs,
        [this](int a_iExitCode, const QString& a_sOut) {
            OnClaudeLocalDone(a_iExitCode, a_sOut);
        });

    // —— 行 2 DeepSeekHarness：未安装（根未命中）≠ 未知（已装读不出）两态。
    //   半装态（package.json 在、bin.js/cordis.yml 缺）按未安装口径
    //   （LooksLikeDshRoot 判定，同设置页 DetectDshInstalled，评审 v3 建议）。
    const bool bDshInstalled = CDeepSeekRunner::LooksLikeDshRoot(CDeepSeekRunner::DshRootPath());
    const QString sDshVer = bDshInstalled ? CDeepSeekRunner::ResolveDshVersion() : QString();
    m_asLocalVer[2] = sDshVer;
    SetLocalCell(2, !bDshInstalled ? QStringLiteral("未安装")
                : sDshVer.isEmpty() ? QStringLiteral("未知")
                : QStringLiteral("v%1").arg(sDshVer));

    // —— 行 3 PIAgent：形态探测（Source 源码布局/Npm 全局包）→ 直读根包 ——
    const CPiAgentRunner::EPiForm eForm = CPiAgentRunner::DetectPiForm();
    QString sPiVer;
    if (eForm != CPiAgentRunner::EPiFormNone) {
        sPiVer = CPiAgentRunner::ResolvePiVersion();
    }
    m_asLocalVer[3] = sPiVer;
    SetLocalCell(3, eForm == CPiAgentRunner::EPiFormNone ? QStringLiteral("未安装")
                : sPiVer.isEmpty() ? QStringLiteral("未知")
                : QStringLiteral("v%1").arg(sPiVer));

    // —— 行 4 CodexHarness：根可解析（LooksLikeCodexRoot）→ 直读 package.json；
    //   仅配置了根但目录不像 Codex 根＝未安装（评审 m3：与按钮可用判定同口径）。
    const QString sCodexRoot = CCodexRunner::CodexRoot();
    const bool bCodexInstalled =
        !sCodexRoot.isEmpty() && CCodexRunner::LooksLikeCodexRoot(sCodexRoot);
    const QString sCodexVer = bCodexInstalled ? CCodexRunner::ResolveCodexVersion()
                                              : QString();
    m_asLocalVer[4] = sCodexVer;
    SetLocalCell(4, !bCodexInstalled ? QStringLiteral("未安装")
                : sCodexVer.isEmpty() ? QStringLiteral("未知")
                : QStringLiteral("v%1").arg(sCodexVer));

    // —— 行 5 Hermes（2026.09 增补）：ResolveHermesCli 探测（根目录/同级
    //   HermesHarness/PATH 三态，同设置页 DetectHermesInstalled）；
    //   2026.09.21 本地版本改优先取发布 tag（git describe 最近可达 v* tag，
    //   日历版本核心，与远端最新 tag 同口径可比）；describe 不可用（非 git
    //   部署/无 tag）回退包版本号直读（仅展示，Verdict 不参与比对）。
    const bool bHermesInstalled = !CHermesRunner::ResolveHermesCli().isEmpty();
    const QString sHermesRepo = bHermesInstalled ? CHermesRunner::HermesGitRepoDir()
                                                 : QString();
    // 比对用（日历版本核心）与显示用分离：非 git 部署的包版本号（如 0.21.3）
    //   与日历 tag（2026.9.14）不同构，绝不能进 m_asLocalVer[5] 参与比对。
    QString sHermesCalVer = sHermesRepo.isEmpty() ? QString()
        : ParseHermesDescribe(RunGitSync(
            QStringList() << QStringLiteral("describe") << QStringLiteral("--tags")
                          << QStringLiteral("--match") << QStringLiteral("v[0-9]*")
                          << QStringLiteral("--abbrev=0") << QStringLiteral("HEAD"),
            sHermesRepo));
    m_asLocalVer[5] = sHermesCalVer;   // 空=无法参与 tag 比对（Verdict 明示无法比对）
    const QString sHermesShow = !sHermesCalVer.isEmpty() ? sHermesCalVer
        : (bHermesInstalled ? CHermesRunner::ResolveHermesVersion() : QString());
    SetLocalCell(5, !bHermesInstalled ? QStringLiteral("未安装")
                : sHermesShow.isEmpty() ? QStringLiteral("未知")
                : QStringLiteral("v%1").arg(sHermesShow));

    // 结论列随本地版本联动（Claude 行待探测收尾回调补算）。
    m_asLocalVer[1].clear();
    RefreshVerdicts();
    UpdateUpdateButtons();
    UpdateInfoLine();
}

void CVersionDialog::OnClaudeLocalDone(int a_iExitCode, const QString& a_sOut) {
    QString sVer;
    if (a_iExitCode == 0) {
        // 形态 "claude-cli x.y.z (…)" / "x.y.z (…)"：单一实现，防双正则漂移
        //   （评审 m3/n1：与 CClaudeRunner::ResolveClaudeVersion 回退链同源）。
        sVer = CClaudeRunner::ParseClaudeVersionOutput(a_sOut);
    } else {
        // 探测失败（超时/杀/退出码≠0）→ npm 全局包直读（NpmGlobalClaudePkgPath
        //   自带 ≤10s 探测缓存；文件读失败→"未安装"口径）。
        const QString sPkg = CClaudeRunner::NpmGlobalClaudePkgPath();
        if (!sPkg.isEmpty()) {
            sVer = fnReadPkgVersion(sPkg);
        }
    }
    m_asLocalVer[1] = sVer;
    // Claude 显示口径：探测+回退链全失败→「未知」（cmd 名来自配置恒非空，
    //   实装“未安装”与“未知”合并，同设计 §3.2）。
    SetLocalCell(1, sVer.isEmpty() ? QStringLiteral("未知")
                : QStringLiteral("v%1").arg(sVer));
    RefreshVerdicts();
    UpdateUpdateButtons();
    UpdateInfoLine();
}

QString CVersionDialog::RunGitSync(const QStringList& a_aArgs, const QString& a_sWorkDir) {
    // 同步短命令捕获：仅本地 git 快调用（rev-parse/remote get-url，<1s）；
    //   3s 兑底超时防卡 UI。失败/超时 → 空串（调用方自行兑底）。
    QProcess stProc;
    stProc.setWorkingDirectory(a_sWorkDir);
    stProc.start(QStringLiteral("git.exe"), a_aArgs);
    if (!stProc.waitForStarted(2000) || !stProc.waitForFinished(3000)) {
        return QString();
    }
    return QString::fromUtf8(stProc.readAllStandardOutput())
        .split(QLatin1Char('\n')).first().trimmed();   // 首行（rev-parse/get-url 单行输出）
}

void CVersionDialog::QueryLatestAll() {
    // 手动触发的 4 路异步查询（D2：时刻语义，结果仅会话内存）。
    //   npm view ×3（Claude/Pi/Codex）+ git ls-remote ×1（DSH）；
    //   失败→该行保留上次已知值（首次失败维持「未查询」）。
    m_tQueryAt = QDateTime::currentDateTime();

    // npm view 输出解析：取**行首**语义化版本串（npm stderr 告警若含版本数字
    //   会误取行中数字，评审 v3 建议；MultilineOption 逐行锚定）。
    auto fnPickSemver = [](const QString& a_sOut) -> QString {
        static const QRegularExpression reVer(
            QStringLiteral("^\\s*(\\d+\\.\\d+\\.\\d+(?:[-+][\\w.]*)?)"),
            QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = reVer.match(a_sOut);
        return m.hasMatch() ? m.captured(1) : QString();
    };
    auto fnQueryDone = [this, fnPickSemver](int a_iBackend, int a_iExitCode,
                                            const QString& a_sOut, bool a_bIsGit) {
        if (a_iExitCode == 0) {
            QString sVer;
            if (a_bIsGit) {
                // ls-remote 输出 → 最大 tag → 剥前缀得版本号。
                const QString sTag = PickLatestTag(a_sOut.split(QLatin1Char('\n')));
                sVer = sTag.isEmpty() ? QString() : TagToVersion(sTag);
            } else {
                sVer = fnPickSemver(a_sOut);
            }
            if (!sVer.isEmpty()) {
                m_asLatestVer[a_iBackend] = sVer;
            }
        }
        // 失败/解析不出：保留旧值，只刷新展示（未查到时维持「未查询」）。
        SetLatestCell(a_iBackend, m_asLatestVer[a_iBackend].isEmpty()
            ? QStringLiteral("未查询")
            : QStringLiteral("v%1").arg(m_asLatestVer[a_iBackend]));
        RefreshVerdicts();
        UpdateInfoLine();
    };

    // Claude / Pi / Codex：npm view <pkg> version（npm 不在 PATH → FailedToStart
    //   → ec=-1 → 失败路径保留旧值，不弹窗，信息栏维持未查询口径）。
    StartPooledProcess(EProcQueryClaude, QStringLiteral("npm.cmd"),
        QStringList() << QStringLiteral("view") << QStringLiteral("@anthropic-ai/claude-code")
                      << QStringLiteral("version"),
        kiQueryTimeoutMs,
        [fnQueryDone](int a_iExitCode, const QString& a_sOut) {
            fnQueryDone(1, a_iExitCode, a_sOut, false);
        });
    StartPooledProcess(EProcQueryPi, QStringLiteral("npm.cmd"),
        QStringList() << QStringLiteral("view") << QStringLiteral("@earendil-works/pi-coding-agent")
                      << QStringLiteral("version"),
        kiQueryTimeoutMs,
        [fnQueryDone](int a_iExitCode, const QString& a_sOut) {
            fnQueryDone(3, a_iExitCode, a_sOut, false);
        });
    StartPooledProcess(EProcQueryCodex, QStringLiteral("npm.cmd"),
        QStringList() << QStringLiteral("view") << QStringLiteral("@openai/codex")
                      << QStringLiteral("version"),
        kiQueryTimeoutMs,
        [fnQueryDone](int a_iExitCode, const QString& a_sOut) {
            fnQueryDone(4, a_iExitCode, a_sOut, false);
        });
    // DSH：git ls-remote --tags <repo>（github 可达性/代理敏感，15s 硬超时兑底）。
    StartPooledProcess(EProcQueryDsh, QStringLiteral("git.exe"),
        QStringList() << QStringLiteral("ls-remote") << QStringLiteral("--tags")
                      << CDeepSeekRunner::DshRepoUrl(),
        kiQueryTimeoutMs,
        [fnQueryDone](int a_iExitCode, const QString& a_sOut) {
            fnQueryDone(2, a_iExitCode, a_sOut, true);
        });
    // 2026.09.21 Hermes：发布 tag 版本号比对（同 DSH tag 口径）——最新版 =
    //   git ls-remote --tags 最大 v* 发布 tag（此前 09.20 用 HEAD sha 比对，
    //   被 main tip 高频提交刷成永远「有更新」，且旧结论「无语义化远端版本」
    //   实为误判：上游有 34 个 v2026.M.D 发布 tag）。本地版本同步重读
    //   git describe（发布 tag 核心语以供 Verdict）；非 git 部署均明示无法比对。
    m_sHermesSyncAt.clear();
    QString sHermesUrl;
    const QString sHermesRepo = CHermesRunner::HermesGitRepoDir();
    if (!sHermesRepo.isEmpty()) {
        m_asLocalVer[5] = ParseHermesDescribe(RunGitSync(
            QStringList() << QStringLiteral("describe") << QStringLiteral("--tags")
                          << QStringLiteral("--match") << QStringLiteral("v[0-9]*")
                          << QStringLiteral("--abbrev=0") << QStringLiteral("HEAD"),
            sHermesRepo));
        SetLocalCell(5, m_asLocalVer[5].isEmpty()
            ? QStringLiteral("未知")
            : QStringLiteral("v%1").arg(m_asLocalVer[5]));
        sHermesUrl = RunGitSync(
            QStringList() << QStringLiteral("remote") << QStringLiteral("get-url")
                          << QStringLiteral("origin"),
            sHermesRepo);
        // 上次 fetch 时刻（.git/FETCH_HEAD mtime）：更新按钮的 git fetch 会刷新
        //   它——「有新版本」尾注提示同步时点，供用户判断是否需要再次更新。
        const QFileInfo stFh(QDir(sHermesRepo).filePath(QStringLiteral(".git/FETCH_HEAD")));
        if (stFh.exists()) {
            const QDateTime stAt = stFh.lastModified();
            m_sHermesSyncAt = stAt.date() == QDate::currentDate()
                ? stAt.toString(QStringLiteral("HH:mm"))
                : stAt.toString(QStringLiteral("MM-dd HH:mm"));
        }
    } else {
        m_asLocalVer[5].clear();
    }
    if (sHermesUrl.isEmpty()) {
        sHermesUrl = QStringLiteral("https://github.com/nousresearch/hermes-agent");
    }
    StartPooledProcess(EProcQueryHermes, QStringLiteral("git.exe"),
        QStringList() << QStringLiteral("ls-remote") << QStringLiteral("--tags")
                      << sHermesUrl,
        kiQueryTimeoutMs,
        [this](int a_iExitCode, const QString& a_sOut) {
            // 逐行 "<sha>\trefs/tags/v2026.9.14[^{}]"；取最大发布 tag 版本核心。
            QString sSha;
            const QString sVer = a_iExitCode == 0
                ? PickLatestHermesTag(a_sOut.split(QLatin1Char('\n')), &sSha)
                : QString();
            if (!sVer.isEmpty()) {
                m_asLatestVer[5] = sVer;
            }
            SetLatestCell(5, m_asLatestVer[5].isEmpty()
                ? QStringLiteral("未查询")
                : m_asLatestVer[5]);
            RefreshVerdicts();
            UpdateInfoLine();
        });
    UpdateInfoLine();
}

void CVersionDialog::OnUpdateBackend(int a_iBackend) {
    // 分派表见 design §3.3：Claude=npm 全局装/更一体；DSH=bat /update-dsh <tag>；
    //   Pi 按 D5 裁决按 DetectPiForm() 分派（Source→bat /pi 重部署本机源仓 /
    //   Npm→npm 全局重装）；Codex=绕 bat 直发 npm 本地工程重装锁定版
    //   （工作目录=CodexRoot，提权：程序目录位于受保护路径时 npm 写入需管理员，
    //   design §3.3 全表 runas 口径，评审 v3 M3）。
    const QString s_asWhat[6] = {
        QString(), QStringLiteral("更新 Claude Code"),
        QStringLiteral("更新 DeepSeekHarness（fetch+reset 到远端最新 tag，"
                       "git reset --hard 会丢弃仓库内已跟踪文件的本地修改）"),
        QStringLiteral("更新 PIAgent"),
        QStringLiteral("更新 CodexHarness（修复重装锁定版 %1）")
            .arg(CCodexRunner::ExpectedCodexVersion()),
        QStringLiteral("在线安装 Hermes Harness（git 克隆 + pnpm 构建）"),
    };
    if (!ConfirmIfBusy(a_iBackend, s_asWhat[a_iBackend])) {
        return;
    }

    const QString sBat = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/install_env.bat");
    const QString sBatNative = QDir::toNativeSeparators(sBat);

    switch (a_iBackend) {
    case 1:
        // npm 全局装/更一体：未安装也可直接装（无未装禁用概念）。
        // 2026.10.10 Node 预检闸：claude-code@2.x 引擎要求 Node>=22——旧 Node
        //   （如 32 位 0.x 残装）npm 装最新版后 postinstall 新语法必 SyntaxError，
        //   提前拦截并给一键升级入口（install_env.bat /node）。
        if (!EnsureNodeOk(22, QStringLiteral("更新 Claude Code"))) {
            return;
        }
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 Claude Code"),
                // 2026.10.10 验收包装：直发命令无 bat 段的 [OK]/[错误] echo，
                //   下方的完成提示无从判据——按 npm 退出码现场生成。
                QStringLiteral("npm install -g @anthropic-ai/claude-code@latest"
                               " && echo [OK] Claude Code 更新完成"
                               " || echo [错误] npm 安装失败，见上方报错"))) {
            QMessageBox::warning(this, QStringLiteral("更新 Claude Code"),
                QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
        } else {
            QMessageBox::information(this, QStringLiteral("更新 Claude Code"),
                QStringLiteral("已在管理员窗口中启动更新。\n\n"
                               "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                               "处理后重跑）；完成后回到本窗点「刷新版本」。"));
        }
        break;
    case 2:
        ResolveDshTagThenUpdate();
        break;
    case 3: {
        // D5 裁决：按 DetectPiForm() 自动分派——Source→bat /pi（robocopy 重部署
        //   本机源仓）；Npm→npm 全局重装；None 不应到达（按钮已禁用，双保险）。
        const CPiAgentRunner::EPiForm eForm = CPiAgentRunner::DetectPiForm();
        if (eForm == CPiAgentRunner::EPiFormNone) {
            QMessageBox::information(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("未检测到 PIAgent 安装（源码布局与 npm 全局均未命中）。\n"
                               "请先在设置页 Agent 组安装后再更新。"));
            return;
        }
        if (eForm == CPiAgentRunner::EPiFormNpm) {
            // npm 全局形态：更新 npm 全局包（不走 bat /pi——会把形态静默换成
            //   源码布局且可能降级，评审 M2/M3 根因）。
            // 2026.10.10 优先走 bat /pi-npm：直连 `cmd /k npm install -g` 继承
            //   主程序启动时的旧环境（本机刚装完 Node 未重启时 PATH 无 nodejs/
            //   npm）必报「找不到 npm」；bat 模式窗口内 :RefreshPath 读注册表
            //   PATH + 缺 Node 自动补装（同设置页「联网安装（npm）」口径）。
            //   npm 全局重装幂等即更新，形态不变。脚本缺失时退回旧直连。
            if (QFile::exists(sBat)) {
                if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 PIAgent"),
                        QStringLiteral("\"%1\" /pi-npm").arg(sBatNative))) {
                    QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                        QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
                    return;
                }
                QMessageBox::information(this, QStringLiteral("更新 PIAgent"),
                    QStringLiteral("已在管理员窗口中启动 npm 全局更新。\n\n"
                                   "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                                   "处理后重跑）；完成后回到本窗点「刷新版本」。"));
                return;
            }
            // 2026.10.10 Node 预检闸（pi-coding-agent 需现代 Node，同 Codex 口径 20）。
            if (!EnsureNodeOk(20, QStringLiteral("更新 PIAgent"))) {
                return;
            }
            if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 PIAgent"),
                    QStringLiteral("npm install -g @earendil-works/pi-coding-agent"
                                   " && echo [OK] PIAgent 更新完成"
                                   " || echo [错误] npm 安装失败，见上方报错"))) {
                QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                    QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
                return;
            }
            QMessageBox::information(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("已在管理员窗口中启动 npm 全局更新。\n\n"
                               "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                               "处理后重跑）；完成后回到本窗点「刷新版本」。"));
            return;
        }
        // 2026.09.20 源码形态：自动按当前配置的 Pi 根更新（用户需求：旧流程固定走 bat
        //   的 PI_SRC 兑底=exe 上级 PIAgent，外部源仓（如 QT\PIAgent）必报「源目录无效」）。
        //   bat /update-pi <根>：git pull --ff-only（保留本地提交）+ pnpm install/build
        //   （dist 不入库必须重建）+ exe 侧部署副本同步（与根不同目录时）。
        const QString sPiRoot = CPiAgentRunner::PiRootPath();
        if (sPiRoot.isEmpty() || !CPiAgentRunner::LooksLikePiRoot(sPiRoot)) {
            QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("当前配置的 PIAgent 根无效：\n%1\n\n"
                               "请到设置页 PIAgent 页确认根目录后重试。")
                    .arg(QDir::toNativeSeparators(sPiRoot)));
            return;
        }
        if (!QFile::exists(sBat)) {
            QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。").arg(sBatNative));
            return;
        }
        const int iRet = QMessageBox::question(this, QStringLiteral("更新 PIAgent"),
            QStringLiteral("将在当前 PIAgent 根的源仓执行更新：\n%1\n\n"
                           "git pull --ff-only（保留本地提交）→ pnpm install + build\n"
                           "（dist 不入库必须重建，数分钟）→ 同步 exe 侧部署副本（如不同目录）。\n\n"
                           "仍要继续吗？").arg(QDir::toNativeSeparators(sPiRoot)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (iRet != QMessageBox::Yes) {
            return;
        }
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 PIAgent"),
                QStringLiteral("\"%1\" /update-pi \"%2\"")
                    .arg(sBatNative, QDir::toNativeSeparators(sPiRoot)))) {
            QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("更新 PIAgent"),
            QStringLiteral("已在管理员窗口中启动更新（git pull + pnpm 重建）。\n\n"
                           "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                           "处理后重跑）；完成后回到本窗点「刷新版本」。"));
        break;
    }
    case 4: {
        // 绕过 bat：npm 本地工程重装锁定版本（工作目录=CodexRoot；提权口径同
        //   全表——受保护路径部署下 npm 写入需管理员，design §3.3/FR-3.1）。
        const QString sRoot = CCodexRunner::CodexRoot();
        if (sRoot.isEmpty() || !CCodexRunner::LooksLikeCodexRoot(sRoot)) {
            return;
        }
        // 2026.10.10 Node 预检闸（Codex CLI 要求 Node>=20，与
        //   install_env.bat /codex 段的版本检查同口径）。
        if (!EnsureNodeOk(20, QStringLiteral("更新 CodexHarness"))) {
            return;
        }
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 CodexHarness"),
                QStringLiteral("npm install @openai/codex@%1 --save-exact"
                               " && echo [OK] CodexHarness 更新完成"
                               " || echo [错误] npm 安装失败，见上方报错")
                    .arg(CCodexRunner::ExpectedCodexVersion()),
                sRoot)) {
            QMessageBox::warning(this, QStringLiteral("更新 CodexHarness"),
                QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("更新 CodexHarness"),
            QStringLiteral("已在管理员窗口中启动修复重装（锁定版 %1）。\n\n"
                           "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                           "处理后重跑）；完成后回到本窗点「刷新版本」。")
                .arg(CCodexRunner::ExpectedCodexVersion()));
        break;
    }
    case 5: {
        // 2026.09.20 已装态真更新（旧版禁用指设置页）：按当前配置的 Hermes 根跑
        //   bat /hermes-update <根>——既有布局自动 fetch+reset（Node 形态含 pnpm
        //   重建；Python 形态含补丁重敷），保留本地文件（DSH 同口径）；根无效时
        //   才指设置页。未安装态仍直达「在线安装」（口径同设置页 OnInstallHermesNet）。
        if (!CHermesRunner::ResolveHermesCli().isEmpty()) {
            const QString sHermesRoot = CHermesRunner::HermesRoot();
            if (sHermesRoot.isEmpty()
                    || !CHermesRunner::LooksLikeHermesRoot(sHermesRoot)) {
                QMessageBox::information(this, QStringLiteral("更新 Hermes"),
                    QStringLiteral("已检测到本机 Hermes，但配置的根目录无效：\n%1\n\n"
                                   "请到设置页 Hermes 页确认根目录；或用「联网安装（git）」"
                                   "在 exe 同级 HermesHarness 重装。")
                        .arg(QDir::toNativeSeparators(sHermesRoot)));
                return;
            }
            if (!QFile::exists(sBat)) {
                QMessageBox::warning(this, QStringLiteral("更新 Hermes"),
                    QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。")
                        .arg(sBatNative));
                return;
            }
            const int iRetH = QMessageBox::question(this, QStringLiteral("更新 Hermes"),
                QStringLiteral("将在当前 Hermes 根执行更新：\n%1\n\n"
                               "git fetch + reset --hard origin/HEAD（保留 node_modules/venv 等"
                               "本地文件）→ 依形态重建（Node：pnpm install+build；Python："
                               "补丁重敷 + hermes --version 验证）。\n\n"
                               "仍要继续吗？").arg(QDir::toNativeSeparators(sHermesRoot)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (iRetH != QMessageBox::Yes) {
                return;
            }
            if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 Hermes"),
                    QStringLiteral("\"%1\" /hermes-update \"%2\"")
                        .arg(sBatNative, QDir::toNativeSeparators(sHermesRoot)))) {
                QMessageBox::warning(this, QStringLiteral("更新 Hermes"),
                    QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
                return;
            }
            QMessageBox::information(this, QStringLiteral("更新 Hermes"),
                QStringLiteral("已在管理员窗口中启动更新。\n\n"
                               "窗口出现「[OK]」才算完成（出现 [错误] 按提示"
                               "处理后重跑，可续装）；完成后回到本窗点「刷新版本」。"));
            return;
        }
        bool bOk = false;
        const QString sRepo = QInputDialog::getText(this,
            QStringLiteral("Hermes 在线安装"),
            QStringLiteral("Hermes Harness 的 git 仓库地址或本机仓库路径："),
            QLineEdit::Normal,
            QStringLiteral("https://github.com/nousresearch/hermes-agent"), &bOk).trimmed();
        if (!bOk) {
            return;   // 用户取消
        }
        const bool bRemote = sRepo.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
                          || sRepo.startsWith(QStringLiteral("git@"));
        if (sRepo.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Hermes"),
                QStringLiteral("仓库地址不能为空（缺省源 https://github.com/"
                               "nousresearch/hermes-agent，或本机 Hermes 仓库路径）。"));
            return;
        }
        if (!bRemote) {
            const QString sGitDir = sRepo + QStringLiteral("/.git");
            const QString sCli = sRepo + QStringLiteral("/dist/cli.js");
            if (!QDir(sRepo).exists()
                    || (!QFileInfo::exists(sGitDir) && !QFileInfo::exists(sCli))) {
                QMessageBox::warning(this, QStringLiteral("Hermes"),
                    QStringLiteral("输入既不是 https:// 或 git@ 开头的 git URL，"
                                   "也不是有效的本机 Hermes 仓库目录（其下应有 "
                                   ".git 或 dist\\cli.js）：\n%1").arg(sRepo));
                return;
            }
        }
        if (!QFile::exists(sBat)) {
            QMessageBox::warning(this, QStringLiteral("Hermes"),
                QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。")
                    .arg(sBatNative));
            return;
        }
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 在线安装 Hermes"),
                QStringLiteral("\"%1\" /hermes \"%2\"").arg(sBatNative, sRepo))) {
            QMessageBox::warning(this, QStringLiteral("Hermes"),
                QStringLiteral("未能以管理员权限启动安装脚本（UAC 被取消或被拒）。"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("Hermes"),
            QStringLiteral("已在管理员窗口中启动在线安装。\n\n"
                           "窗口出现「[OK] Hermes 安装完成」才算装好（出现 [错误] "
                           "按提示处理后重跑，可续装）；完成后回到本窗点"
                           "「刷新版本」（exe 同级 HermesHarness 自动命中）。"));
        break;
    }
    default:
        break;
    }
}

void CVersionDialog::ResolveDshTagThenUpdate() {
    // 更新前置：异步 ls-remote 解析最新 tag（查询池与更新池分键，重入安全），
    //   成功后才提权拉起 /update-dsh <tag>（tag 由 AutoCode 侧决定，bat 不再解析）。
    const QString sBatNative = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/install_env.bat"));
    // 第 3 参传配置的 DSH 根（带引号，空则省略回落脚本同级推导）：多实例共享
    //   外部 DshRoot 时 bat 同级 DeepSeekHarness 目录可能不存在，更新目标必须
    //   与本地版本读数同源（config 的 DshRoot），否则更新后版本显示不同步。
    const QString sDshRootNative = QDir::toNativeSeparators(
        CDeepSeekRunner::DshRootPath());
    if (!QFile::exists(QCoreApplication::applicationDirPath()
                       + QStringLiteral("/install_env.bat"))) {
        QMessageBox::warning(this, QStringLiteral("更新 DeepSeekHarness"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。").arg(sBatNative));
        return;
    }
    m_pLblInfo->setText(
        QStringLiteral("正在解析 DeepSeekHarness 最新 tag（git ls-remote，最多 15 秒）…"));
    StartPooledProcess(EProcDshTag, QStringLiteral("git.exe"),
        QStringList() << QStringLiteral("ls-remote") << QStringLiteral("--tags")
                      << CDeepSeekRunner::DshRepoUrl(),
        kiQueryTimeoutMs,
        [this, sBatNative, sDshRootNative](int a_iExitCode, const QString& a_sOut) {
            UpdateInfoLine();
            if (a_iExitCode != 0) {
                QMessageBox::warning(this, QStringLiteral("更新 DeepSeekHarness"),
                    QStringLiteral("查询远端 tag 失败（网络/代理），未做任何修改。\n"
                                   "请稍后重试或检查网络后重试。"));
                return;
            }
            const QString sTag = PickLatestTag(a_sOut.split(QLatin1Char('\n')));
            if (sTag.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("更新 DeepSeekHarness"),
                    QStringLiteral("远端未找到可识别的版本 tag，未做任何修改。"));
                return;
            }
            // 2026.09.10 版本闸门更新：AutoCode 已双布局适配上游官方 SDK profile
            //   （Legacy ≤0.1.1 / Profile ≥0.1.2，EDshLayout 判型 + --profile sdk
            //   --patch 组合层），基线抬升到 dsh-v0.1.5-alpha.1。仍保留上限闸：
            //   高于基线的 tag 可能再次破坏兼容，一律阻断待适配。
            const QVersionNumber vTarget = QVersionNumber::fromString(
                TagToVersion(sTag));
            if (!vTarget.isNull()
                && vTarget > CDeepSeekRunner::SupportedDshVersion()) {
                QMessageBox::warning(this, QStringLiteral("更新 DeepSeekHarness"),
                    QStringLiteral(
                        "目标版本 %1 高于 AutoCode 集成受支持的 %2。\n\n"
                        "未做任何修改；待 AutoCode 适配新版后开放更新，或使用"
                        "基线版本 %2。")
                        .arg(sTag, CDeepSeekRunner::SupportedDshTag()));
                return;
            }
            const QString sCmdUpdate = sDshRootNative.isEmpty()
                ? QStringLiteral("\"%1\" /update-dsh %2").arg(sBatNative, sTag)
                : QStringLiteral("\"%1\" /update-dsh %2 \"%3\"")
                      .arg(sBatNative, sTag, sDshRootNative);
            if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 DeepSeekHarness"),
                    sCmdUpdate)) {
                QMessageBox::warning(this, QStringLiteral("更新 DeepSeekHarness"),
                    QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
                return;
            }
            QMessageBox::information(this, QStringLiteral("更新 DeepSeekHarness"),
                QStringLiteral("已在管理员窗口中启动更新（目标 tag：%1）。\n\n"
                               "窗口出现「[OK] 已更新…」才算完成（出现 [错误] 按提示"
                               "处理后重跑，可续修）；完成后回到本窗点「刷新版本」")
                    .arg(sTag));
        });
}

void CVersionDialog::UpdateUpdateButtons() {
    // 可用性规则（design §3.3）：未装后端禁用（走设置页安装按钮，避免双入口
    //   语义分叉）；装/更一体的入口恒可用（tooltip 注明未装则装最新）。
    m_aBtnUpdate[1]->setEnabled(true);   // Claude：npm 全局装/更一体
    m_aBtnUpdate[1]->setToolTip(
        QStringLiteral("npm 全局安装/更新一体（未安装则安装最新版）"));

    const bool bDshInstalled =
        CDeepSeekRunner::LooksLikeDshRoot(CDeepSeekRunner::DshRootPath());
    m_aBtnUpdate[2]->setEnabled(bDshInstalled);
    m_aBtnUpdate[2]->setToolTip(bDshInstalled
        ? QStringLiteral("更新本机 DeepSeekHarness（fetch+reset 到远端 tag；"
                         "高于受支持版 %1 的 tag 会被阻断——上游已移除"
                         "AutoCode 集成依赖的示例包）")
              .arg(CDeepSeekRunner::SupportedDshTag())
        : QStringLiteral("未安装：请先在设置页 Agent 组「安装 DeepSeekHarness…」"));

    // D5：Pi 按检测到的形态分派更新命令，未装两形态皆未命中→禁用。
    const bool bPiInstalled =
        CPiAgentRunner::DetectPiForm() != CPiAgentRunner::EPiFormNone;
    m_aBtnUpdate[3]->setEnabled(bPiInstalled);
    m_aBtnUpdate[3]->setToolTip(bPiInstalled
        ? QStringLiteral("按检测到的安装形态自动更新（源码重部署 / npm 全局重装）")
        : QStringLiteral("未安装：请先在设置页 Agent 组安装 PIAgent（源码或 npm 形态）"));

    const QString sCodexRoot = CCodexRunner::CodexRoot();
    const bool bCodexInstalled =
        !sCodexRoot.isEmpty() && CCodexRunner::LooksLikeCodexRoot(sCodexRoot);
    m_aBtnUpdate[4]->setEnabled(bCodexInstalled);
    m_aBtnUpdate[4]->setToolTip(bCodexInstalled
        ? QStringLiteral("修复重装锁定版 %1（npm 本地工程重装，工作目录=Codex 根）")
              .arg(CCodexRunner::ExpectedCodexVersion())
        : QStringLiteral("未安装：请先在设置页 Agent 组「安装 CodexHarness…」"));

    // Hermes：已装态「更新」直连 /hermes-update <配置根>（2026.09.20 真更新：旧版恒禁用
    //   指设置页）；未装时按钮改「在线安装」直达同一条 git 安装链路（OnUpdateBackend
    //   case 5，仓库地址点击时输入框现场给定，不落 config）。
    const bool bHermesInstalled = !CHermesRunner::ResolveHermesCli().isEmpty();
    m_aBtnUpdate[5]->setText(bHermesInstalled ? QStringLiteral("更新")
                                              : QStringLiteral("在线安装"));
    m_aBtnUpdate[5]->setEnabled(true);
    m_aBtnUpdate[5]->setToolTip(bHermesInstalled
        ? QStringLiteral("按当前配置的 Hermes 根更新：git fetch+reset（保留本地文件；"
                         "Node 形态含 pnpm 重建，Python 形态含补丁重敷）")
        : QStringLiteral("联网安装 Hermes Harness（git 克隆 + pnpm 构建，提权）；"
                         "仓库地址点击后输入（git URL 或本机仓库路径）"));
}

bool CVersionDialog::LaunchCmdWindow(bool a_bRunAs, const QString& a_sTitle,
                                     const QString& a_sCmdLine, const QString& a_sWorkDir) {
    // cmd /k title <标题> && <命令>：可见窗口保持打开（输出可查），标题便于
    //   辨认多窗口（同既有安装按钮 runas 口径；a_bRunAs=false 走 open）。
    const QString sParams = QStringLiteral("/k title %1 && %2").arg(a_sTitle, a_sCmdLine);
    HINSTANCE hInst = ShellExecuteW(nullptr,
        a_bRunAs ? L"runas" : L"open", L"cmd.exe",
        reinterpret_cast<LPCWSTR>(sParams.utf16()),
        a_sWorkDir.isEmpty()
            ? nullptr
            : reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(a_sWorkDir).utf16()),
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(hInst) > 32;
}

bool CVersionDialog::ConfirmIfBusy(int a_iBackend, const QString& a_sWhat) {
    // D6：该后端 == 当前全局后端 && (IsRunnerBusy() || IsAdhocRunning()) →
    //   阻断式确认（注入判据为空 = 直接放行，测试场景）。
    if (!m_fnBusy || !m_fnBusy(a_iBackend)) {
        return true;
    }
    const int iRet = QMessageBox::warning(this, QStringLiteral("后端运行中"),
        QStringLiteral("当前后端有任务或助手会话正在运行。\n%1 期间可能中断执行中的"
                       "任务，建议先等任务结束或手动停止会话。\n仍要继续吗？")
            .arg(a_sWhat),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return iRet == QMessageBox::Yes;
}

int CVersionDialog::ProbeNodeMajor(QString* a_psVerOut) {
    // 同步短探测 PATH 上的 node -v（≤3s 硬超时，RunGitSync 同款口径；npm.cmd
    //   解析的 node 同样来自 PATH，探测口径与 npm 实际执行一致）。
    QProcess stProc;
    stProc.start(QStringLiteral("node.exe"), QStringList() << QStringLiteral("-v"));
    if (!stProc.waitForStarted(2000) || !stProc.waitForFinished(3000)
        || stProc.exitCode() != 0) {
        return -1;   // 无 node / 超时 / 异常退出
    }
    const QString sOut = QString::fromUtf8(stProc.readAllStandardOutput()).trimmed();
    if (a_psVerOut != nullptr) {
        *a_psVerOut = sOut.isEmpty() ? QStringLiteral("未知") : sOut;
    }
    static const QRegularExpression reNodeVer(QStringLiteral("^v?(\\d+)"));
    const QRegularExpressionMatch mVer = reNodeVer.match(sOut);
    return mVer.hasMatch() ? mVer.captured(1).toInt() : -1;
}

bool CVersionDialog::EnsureNodeOk(int a_iMinMajor, const QString& a_sTitle) {
    // 2026.10.10 npm 类更新前置闸：目标机器常见「残装古董 Node」（如 32 位
    //   v0.12 + npm 2.7），npm 装最新包后 postinstall 新语法（const { spawnSync }
    //   = require('child_process')）旧 Node 解析不了必 SyntaxError（ELIFECYCLE
    //   中止）。此闸把失败提前到点更新之前，并给一键「安装/升级 Node.js LTS」
    //   （install_env.bat /node）+「仍然继续更新」出口（保留旧行为）。
    QString sVerRaw;
    const int iMajor = ProbeNodeMajor(&sVerRaw);
    if (iMajor >= a_iMinMajor) {
        return true;   // 版本达标：直接放行原更新
    }
    const QString sBat = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/install_env.bat");
    const QString sBatNative = QDir::toNativeSeparators(sBat);

    QMessageBox stBox(this);
    stBox.setWindowTitle(a_sTitle);
    stBox.setIcon(QMessageBox::Warning);
    stBox.setText(iMajor < 0
        ? QStringLiteral("未检测到 Node.js（PATH 中无 node）。\n\n"
                         "本更新走 npm 执行，需要 Node.js（%1 要求主版本 %2 以上）。")
              .arg(a_sTitle).arg(a_iMinMajor)
        : QStringLiteral("本机 Node.js 版本过低（%1），不满足 %2 的最低要求"
                         "（主版本 %3 以上）。\n\n"
                         "npm 会安装最新版并触发 postinstall 脚本，旧 Node 解析"
                         "不了新语法（SyntaxError），直接更新必然失败。")
              .arg(sVerRaw, a_sTitle).arg(a_iMinMajor));
    QPushButton* pBtnFix = stBox.addButton(QStringLiteral("安装/升级 Node.js LTS"),
                                           QMessageBox::AcceptRole);
    QPushButton* pBtnGo  = stBox.addButton(QStringLiteral("仍然继续更新"),
                                           QMessageBox::DestructiveRole);
    stBox.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    stBox.exec();
    if (stBox.clickedButton() == pBtnGo) {
        return true;   // 用户明确选择继续：保留旧直更新行为（自行承担失败）
    }
    if (stBox.clickedButton() != pBtnFix) {
        return false;   // 取消/关窗：不做任何事
    }
    if (!QFile::exists(sBat)) {
        QMessageBox::warning(this, a_sTitle,
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。").arg(sBatNative));
        return false;
    }
    if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 安装/升级 Node.js LTS"),
            QStringLiteral("\"%1\" /node").arg(sBatNative))) {
        QMessageBox::warning(this, a_sTitle,
            QStringLiteral("未能以管理员权限启动安装（UAC 被取消或被拒）。"));
        return false;
    }
    QMessageBox::information(this, a_sTitle,
        QStringLiteral("已在管理员窗口中安装/升级 Node.js LTS。\n\n"
                       "窗口出现「[OK] Node.js 已就绪」才算完成（若提示旧版遮蔽，"
                       "按指引卸载旧版 Node.js 后重跑）；完成后关闭并重开 AutoCode"
                       "（让新 PATH 生效），再回本窗点更新。"));
    return false;
}

void CVersionDialog::StartPooledProcess(int a_iKey, const QString& a_sProgram,
                                        const QStringList& a_aArgs, int a_iTimeoutMs,
                                        std::function<void(int, const QString&)> a_fnOnDone) {
    KillPooledProcess(a_iKey);

    QProcess* pProc = new QProcess(this);
    m_aProc.insert(a_iKey, pProc);

    // 一次性回调收敛标志（finished/FailedToStart/超时 kill 三源只放一次）。
    QSharedPointer<bool> spFired(new bool(false));
    auto fnEmit = [this, a_iKey, spFired, a_fnOnDone](int a_iExitCode) {
        if (m_bTearingDown || *spFired) {
            return;   // 析构期/重复触发：不再进业务回调（防析构中弹窗/同步 npm）
        }
        *spFired = true;
        QProcess* p = m_aProc.value(a_iKey, nullptr);
        const QString sOut = (p != nullptr)
            ? QString::fromUtf8(p->readAllStandardOutput() + p->readAllStandardError())
            : QString();
        KillPooledProcess(a_iKey);
        a_fnOnDone(a_iExitCode, sOut);
    };

    connect(pProc, &QProcess::finished, pProc,
        [fnEmit](int a_iExitCode, QProcess::ExitStatus) { fnEmit(a_iExitCode); });
    connect(pProc, &QProcess::errorOccurred, pProc, [fnEmit](QProcess::ProcessError) {
        fnEmit(-1);
    });

    QTimer* pTimer = new QTimer(this);
    pTimer->setSingleShot(true);
    m_aTimer.insert(a_iKey, pTimer);
    connect(pTimer, &QTimer::timeout, this, [this, a_iKey, pProc]() {
        if (pProc->state() != QProcess::NotRunning) {
            pProc->kill();   // kill → finished(CrashExit) → fnEmit(-1) 统一收敛
        }
    });

    pProc->start(a_sProgram, a_aArgs);
    if (!*spFired) {
        // FailedToStart 会在 start() 内同步 emit → fnEmit 已收敛（进程/表已随
        //   KillPooledProcess 杀净 deleteLater），此时不得再启表（否则表后触发
        //   时 lambda 捕获的 pProc 已悬空）。
        pTimer->start(a_iTimeoutMs);
    }
}

void CVersionDialog::KillPooledProcess(int a_iKey) {
    // 同键重入防护：先杀旧进程/停表再启新。**先 take 移出池、再 disconnect
    //   掐断全部信号、最后 kill/waitForFinished**——Windows 上 waitForFinished
    //   会同步 emit finished/errorOccurred，不断链则杀进程路径上的回调会重入
    //   （二次 take 打穿调用方迭代器 / 业务回调弹窗、同步 npm 回退卡 UI，
    //   评审 C1/M1 根因；断链后旧对象信号打到空处，天然安全）。
    if (QTimer* pTimer = m_aTimer.take(a_iKey); pTimer != nullptr) {
        pTimer->stop();
        pTimer->deleteLater();
    }
    if (QProcess* pProc = m_aProc.take(a_iKey); pProc != nullptr) {
        pProc->disconnect();
        if (pProc->state() != QProcess::NotRunning) {
            pProc->kill();
            pProc->waitForFinished(2000);
        }
        pProc->deleteLater();
    }
}

void CVersionDialog::SetLocalCell(int a_iRow, const QString& a_sText) {
    const int iRow = a_iRow - 1;
    QTableWidgetItem* pItem = m_pTable->item(iRow, kiColLocal);
    if (pItem == nullptr) {
        pItem = new QTableWidgetItem(a_sText);
        pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
        m_pTable->setItem(iRow, kiColLocal, pItem);
    } else {
        pItem->setText(a_sText);
    }
}

void CVersionDialog::SetLatestCell(int a_iRow, const QString& a_sText) {
    const int iRow = a_iRow - 1;
    QTableWidgetItem* pItem = m_pTable->item(iRow, kiColLatest);
    if (pItem == nullptr) {
        pItem = new QTableWidgetItem(a_sText);
        pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
        m_pTable->setItem(iRow, kiColLatest, pItem);
    } else {
        pItem->setText(a_sText);
    }
}

void CVersionDialog::SetVerdictCell(int a_iRow, const QString& a_sText) {
    const int iRow = a_iRow - 1;
    QTableWidgetItem* pItem = m_pTable->item(iRow, kiColVerdict);
    if (pItem == nullptr) {
        pItem = new QTableWidgetItem(a_sText);
        pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
        m_pTable->setItem(iRow, kiColVerdict, pItem);
    } else {
        pItem->setText(a_sText);
    }
}

void CVersionDialog::RefreshVerdicts() {
    for (int iBackend = 1; iBackend <= 5; ++iBackend) {
        // Claude 行本地值未知且探测在飞：冻结结论为「读取中…」，避免短暂显示
        //   「与远端不同：→ x.y.z」误读（评审 v1 n3 瞬态）。
        if (iBackend == 1 && m_asLocalVer[1].isEmpty()
            && m_aProc.contains(EProcClaudeLocal)) {
            SetVerdictCell(1, QStringLiteral("读取中…"));
            continue;
        }
        SetVerdictCell(iBackend, Verdict(m_asLocalVer[iBackend], m_asLatestVer[iBackend],
                                         iBackend,
                                         iBackend == 5 ? m_sHermesSyncAt : QString()));
    }
}

void CVersionDialog::UpdateInfoLine() {
    QString sInfo;
    if (m_tQueryAt.isValid()) {
        sInfo = QStringLiteral("查询时刻：%1（registry/远端时点值，非实时；仅本次会话有效）")
                    .arg(m_tQueryAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    } else {
        sInfo = QStringLiteral("尚未查询最新版（「查询最新版(联网)」需网络；结果不落盘）");
    }
    sInfo += QStringLiteral("；更新窗口关闭后请点「刷新版本」重新加载本地版本。");
    m_pLblInfo->setText(sInfo);
}

// —— 纯函数（T6 单测靶点，与 UI 解耦零 IO）——

QString CVersionDialog::PickLatestTag(const QStringList& a_aLines) {
    // tag 全名规则：^(?:dsh-)?v?(\d+(?:\.\d+)*)([-.].*)?$
    //   （dsh- 前缀可选——兼容仓库无前缀 tag；核心必为数字段；后缀可选）。
    static const QRegularExpression reTag(
        QStringLiteral("^(?:dsh-)?v?(\\d+(?:\\.\\d+)*)([-.].*)?$"));
    // 预发布等级：无后缀(5) > rc(4) > beta(3) > alpha(2) > dev(1) > 其它(0)。
    auto fnPreLevel = [](const QString& a_sSuf) -> int {
        if (a_sSuf.isEmpty()) {
            return 5;
        }
        const QString s = a_sSuf.toLower();
        if (s.contains(QLatin1String("rc"))) {
            return 4;
        }
        if (s.contains(QLatin1String("beta"))) {
            return 3;
        }
        if (s.contains(QLatin1String("alpha"))) {
            return 2;
        }
        if (s.contains(QLatin1String("dev"))) {
            return 1;
        }
        return 0;
    };
    // 后缀数字段（数值序）：rc.10 的 10 > rc.9 的 9（字符串序会错判）。
    auto fnSufNums = [](const QString& a_sSuf) -> QVector<qint64> {
        static const QRegularExpression reNum(QStringLiteral("\\d+"));
        QVector<qint64> aNums;
        auto it = reNum.globalMatch(a_sSuf);
        while (it.hasNext()) {
            bool bOk = false;
            const qint64 ll = it.next().captured(0).toLongLong(&bOk);
            aNums.append(bOk ? ll : 0);
        }
        return aNums;
    };

    QString sBestTag;
    QVector<int> aBestCore;
    int iBestPre = -1;
    QVector<qint64> aBestSuf;
    bool bHasBest = false;

    // 三向比较：<0=候选更旧，0=全等（保留首个），>0=候选更新。
    //   键序：核心降序 → 预发布等级降序 → 后缀数字段降序。
    auto fnCmp = [&aBestCore, &iBestPre, &aBestSuf](const QVector<int>& a_aCore,
                     int a_iPre, const QVector<qint64>& a_aSuf) -> int {
        const qsizetype nCore = qMin(a_aCore.size(), aBestCore.size());
        for (qsizetype i = 0; i < nCore; ++i) {
            if (a_aCore.at(i) != aBestCore.at(i)) {
                return a_aCore.at(i) < aBestCore.at(i) ? -1 : 1;
            }
        }
        if (a_aCore.size() != aBestCore.size()) {
            return a_aCore.size() < aBestCore.size() ? -1 : 1;
        }
        if (a_iPre != iBestPre) {
            return a_iPre < iBestPre ? -1 : 1;
        }
        const qsizetype nSuf = qMin(a_aSuf.size(), aBestSuf.size());
        for (qsizetype i = 0; i < nSuf; ++i) {
            if (a_aSuf.at(i) != aBestSuf.at(i)) {
                return a_aSuf.at(i) < aBestSuf.at(i) ? -1 : 1;
            }
        }
        if (a_aSuf.size() != aBestSuf.size()) {
            return a_aSuf.size() < aBestSuf.size() ? -1 : 1;
        }
        return 0;
    };

    for (const QString& szLine : a_aLines) {
        QString sLine = szLine.trimmed();
        if (sLine.isEmpty() || sLine.endsWith(QStringLiteral("^{}"))) {
            continue;   // 空行 / ls-remote annotated tag 双行形态的 ^{} 尾注行
        }
        const int iTab = sLine.lastIndexOf(QLatin1Char('\t'));
        if (iTab >= 0) {
            sLine = sLine.mid(iTab + 1);   // "<sha>\trefs/tags/<tag>" → tag 部分
        }
        if (sLine.startsWith(QStringLiteral("refs/tags/"))) {
            sLine = sLine.mid(QStringLiteral("refs/tags/").size());
        }
        const QRegularExpressionMatch m = reTag.match(sLine);
        if (!m.hasMatch()) {
            continue;
        }
        // 核心版本 "1.2.3" → {1,2,3}。
        QVector<int> aCore;
        const QStringList aParts = m.captured(1).split(QLatin1Char('.'));
        for (const QString& sPart : aParts) {
            aCore.append(sPart.toInt());
        }
        const QString sSuf = m.captured(2);
        const int iPre = fnPreLevel(sSuf);
        const QVector<qint64> aSuf = fnSufNums(sSuf);

        if (bHasBest && fnCmp(aCore, iPre, aSuf) <= 0) {
            continue;   // 不比当前最佳新：跳过（全等保留首个，防抖动）
        }
        bHasBest = true;
        sBestTag = sLine;
        aBestCore = aCore;
        iBestPre = iPre;
        aBestSuf = aSuf;
    }
    return sBestTag;
}

QString CVersionDialog::TagToVersion(const QString& a_sTag) {
    // 剥 dsh-/v 前缀；剥完不以数字核心开头 = 无效 → 原样返回。
    QString s = a_sTag.trimmed();
    if (s.startsWith(QStringLiteral("dsh-"), Qt::CaseInsensitive)) {
        s = s.mid(QStringLiteral("dsh-").size());
    }
    if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        s = s.mid(1);
    }
    static const QRegularExpression reCore(QStringLiteral("^\\d+(?:\\.\\d+)*"));
    return reCore.match(s).hasMatch() ? s : a_sTag.trimmed();
}

QString CVersionDialog::PickLatestHermesTag(const QStringList& a_aLines, QString* a_pShaOut) {
    // 逐行 ls-remote --tags 输出："<40位sha>\trefs/tags/v2026.9.14" 或附注 tag
    //   剥离行 "...v2026.9.14^{}"（指向 commit 实体）。同版本名同时出现轻量与
    //   剥离行时优先 ^{} 的 sha。杂项 tag（backup/、premerge 等）不匹配 v<数字>
    //   直接排除。取 QVersionNumber 最大者。
    static const QRegularExpression reLine(
        QStringLiteral("^([0-9a-f]{40})\\s+refs/tags/(v\\d+(?:\\.\\d+)+)(\\^\\{\\})?\\s*$"),
        QRegularExpression::MultilineOption);
    QString sBestVer;
    QString sBestSha;
    QVersionNumber vBest;
    for (const QString& sLine : a_aLines) {
        const QRegularExpressionMatch m = reLine.match(sLine.trimmed());
        if (!m.hasMatch()) {
            continue;
        }
        const QString sVer = m.captured(2).mid(1);   // 去前导 v
        const QVersionNumber vCur = QVersionNumber::fromString(sVer);
        const bool bPeeled = m.captured(3).isEmpty();
        if (vCur.isNull()) {
            continue;
        }
        if (vCur > vBest) {
            vBest = vCur;
            sBestVer = sVer;
            sBestSha = m.captured(1);
        } else if (vCur == vBest && !bPeeled) {
            // 同版本附注 tag：轻量行 sha=tag 对象，^{} 行 sha=commit 实体 → 覆盖。
            sBestSha = m.captured(1);
        }
    }
    if (sBestVer.isEmpty()) {
        return QString();
    }
    if (a_pShaOut != nullptr) {
        *a_pShaOut = sBestSha;
    }
    return sBestVer;
}

QString CVersionDialog::ParseHermesDescribe(const QString& a_sDescribe) {
    // "v2026.9.14" / "v2026.9.14-4881-gc4f5dd0dad" → "2026.9.14"；
    //   无 v<数字(.数字)+> 核心（如空串/致命错误文本）→ 空串。
    static const QRegularExpression reDescribe(
        QStringLiteral("^v?(\\d+(?:\\.\\d+)+)"));
    const QRegularExpressionMatch m = reDescribe.match(a_sDescribe.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

QString CVersionDialog::Verdict(const QString& a_sLocal, const QString& a_sLatest,
                                int a_iBackend, const QString& a_sHermesSyncAt) {
    // 优先级链：backend==2 超基线不兼容（先于“相等”——不兼容版本即便==远端
    //   也不能误导为“已是最新”）→ 相等 → backend==4 锁定 → latest 空 →
    //   新版 → 其它不等。
    const QString sLocal = a_sLocal.trimmed();
    const QString sLatest = a_sLatest.trimmed();
    // 2026.09.01 DSH 上限锁（上游 dsh-v0.1.2+ 移除示例包，集成不兼容）：本地已超基线
    //   → 指引回退；远端最新超基线 → 明示不提供升级（不算“有新版本”）。
    if (a_iBackend == 2) {
        const QVersionNumber vBase = CDeepSeekRunner::SupportedDshVersion();
        const QVersionNumber vLoc = QVersionNumber::fromString(sLocal);
        if (!vLoc.isNull() && vLoc > vBase) {
            return QStringLiteral("不兼容：高于受支持版 %1（上游已移除示例包），"
                                   "请用 /update-dsh %2 回退")
                .arg(vBase.toString(), CDeepSeekRunner::SupportedDshTag());
        }
        const QVersionNumber vLat = QVersionNumber::fromString(sLatest);
        if (!vLat.isNull() && vLat > vBase) {
            return QStringLiteral("最新版 %1 不受支持（上游移除示例包），保持 %2")
                .arg(sLatest, CDeepSeekRunner::SupportedDshTag());
        }
    }
    if (a_iBackend == 5) {
        // 2026.09.21 Hermes：发布 tag 版本号比对（同其余 Harness 的发布语义；
        //   a_sLocal=git describe 日历版本核心，a_sLatest=远端最大发布 tag 核心，
        //   HEAD sha 不再进结论）。相等=已最新；远端>本地=有新版本（附上次同步
        //   时刻尾注）；本地空=非 git 部署明示；本地>远端/解析失败=落入通用
        //   「与远端不同」；latest 空=落入通用「未查询」。
        if (!sLatest.isEmpty() && sLocal.isEmpty()) {
            return QStringLiteral("远端最新发布 v%1（本地非 git 部署，无法比对）")
                .arg(sLatest);
        }
        if (!sLatest.isEmpty() && !sLocal.isEmpty()) {
            const QVersionNumber vLoc = QVersionNumber::fromString(sLocal);
            const QVersionNumber vLat = QVersionNumber::fromString(sLatest);
            if (!vLoc.isNull() && !vLat.isNull() && vLoc == vLat) {
                return QStringLiteral("已是最新（v%1）").arg(sLatest);
            }
            if (!vLoc.isNull() && !vLat.isNull() && vLat > vLoc) {
                const QString sSync = a_sHermesSyncAt.trimmed().isEmpty()
                    ? QString()
                    : QStringLiteral("（本地上次同步 %1）").arg(a_sHermesSyncAt.trimmed());
                return QStringLiteral("有新版本 v%1%2").arg(sLatest, sSync);
            }
            // 本地>远端（describe tag 新于远端查询值，短暂竞态）或任一不可解析
            //   → 落入通用「与远端不同」。
        }
    }
    if (!sLocal.isEmpty() && sLocal == sLatest) {
        return QStringLiteral("已是最新");
    }
    if (a_iBackend == 4) {
        return QStringLiteral("已锁定 %1（v0.88+ 已移除 wire_api=chat，不提供升级）")
            .arg(CCodexRunner::ExpectedCodexVersion());   // 与命令行同源，防锁版漂移
    }
    if (sLatest.isEmpty()) {
        return QStringLiteral("未查询");
    }
    const QVersionNumber vLocal = QVersionNumber::fromString(sLocal);
    const QVersionNumber vLatest = QVersionNumber::fromString(sLatest);
    if (!vLocal.isNull() && vLatest > vLocal) {
        return QStringLiteral("有新版本 %1").arg(sLatest);
    }
    return QStringLiteral("与远端不同：%1 → %2").arg(sLocal, sLatest);
}
