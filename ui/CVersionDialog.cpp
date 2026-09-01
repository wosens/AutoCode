// CVersionDialog.cpp —— 后端版本管理对话框（任务 23183ccb）。
// T3：骨架 + 本地版本加载（DSH/Pi/Codex 同步直读，Claude 异步探测 + npm 回退）
//     + 纯函数（PickLatestTag/TagToVersion/Verdict，T6 单测靶点）。
// T4：QueryLatestAll 联网查询与 OnUpdateBackend 更新分派（占位实装见各 TODO）。

#include "CVersionDialog.h"

#include "CClaudeRunner.h"
#include "CCodexRunner.h"
#include "CDeepSeekRunner.h"
#include "CPiAgentRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

// 行号 ↔ 后端号恒等（1..4 行序即后端号，表格行直接用后端号索引）。
const int kiColBackend = 0;   // 后端
const int kiColLocal   = 1;   // 当前版本
const int kiColLatest  = 2;   // 最新版本
const int kiColVerdict = 3;   // 结论
const int kiColAction  = 4;   // 操作

const int kiClaudeLocalTimeoutMs = 10 * 1000;   // Claude 本地探测硬超时
const int kiQueryTimeoutMs       = 15 * 1000;   // 联网查询硬超时（npm/ls-remote）

// package.json → version（open 失败/解析错/无字段→空串）。
QString fnReadPkgVersion(const QString& a_sPkgPath) {
    QFile stFile(a_sPkgPath);
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

    m_pTable = new QTableWidget(4, 5, this);
    m_pTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("后端") << QStringLiteral("当前版本")
        << QStringLiteral("最新版本") << QStringLiteral("结论") << QStringLiteral("操作"));
    m_pTable->setVerticalHeaderLabels(QStringList()
        << QStringLiteral("1") << QStringLiteral("2") << QStringLiteral("3") << QStringLiteral("4"));
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    m_pTable->verticalHeader()->setVisible(true);
    const int aiColWidth[5] = { 130, 120, 120, 200, 90 };
    for (int iCol = 0; iCol < 5; ++iCol) {
        m_pTable->setColumnWidth(iCol, aiColWidth[iCol]);
    }

    const QString asNames[5] = {
        QString(),
        QStringLiteral("Claude Code"),
        QStringLiteral("DeepSeekHarness"),
        QStringLiteral("PIAgent"),
        QStringLiteral("CodexHarness"),
    };
    for (int iBackend = 1; iBackend <= 4; ++iBackend) {
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

    resize(760, 320);
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
    UpdateInfoLine();
}

void CVersionDialog::OnUpdateBackend(int a_iBackend) {
    // 分派表见 design §3.3：Claude=npm 全局装/更一体；DSH=bat /update-dsh <tag>；
    //   Pi 按 D5 裁决按 DetectPiForm() 分派（Source→bat /pi 重部署本机源仓 /
    //   Npm→npm 全局重装）；Codex=绕 bat 直发 npm 本地工程重装锁定版
    //   （工作目录=CodexRoot，提权：程序目录位于受保护路径时 npm 写入需管理员，
    //   design §3.3 全表 runas 口径，评审 v3 M3）。
    const QString s_asWhat[5] = {
        QString(), QStringLiteral("更新 Claude Code"),
        QStringLiteral("更新 DeepSeekHarness（fetch+reset 到远端最新 tag，"
                       "git reset --hard 会丢弃仓库内已跟踪文件的本地修改）"),
        QStringLiteral("更新 PIAgent"),
        QStringLiteral("更新 CodexHarness（修复重装锁定版 %1）")
            .arg(CCodexRunner::ExpectedCodexVersion()),
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
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 Claude Code"),
                QStringLiteral("npm install -g @anthropic-ai/claude-code@latest"))) {
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
            if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 PIAgent"),
                    QStringLiteral("npm install -g @earendil-works/pi-coding-agent"))) {
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
        // 源码形态：重部署本机源仓（exe 上级 PIAgent）→ 先确认源仓已拉新，
        //   否则 robocopy 复制到的仍是旧源（design §3.3 前置文案显性化）。
        if (!QFile::exists(sBat)) {
            QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整。").arg(sBatNative));
            return;
        }
        const int iRet = QMessageBox::question(this, QStringLiteral("更新 PIAgent"),
            QStringLiteral("源码形态：将重部署本机源仓（exe 上级 PIAgent）到程序目录。\n"
                           "请先在源仓拉取最新代码（git pull），否则版本不变。\n\n"
                           "仍要继续吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (iRet != QMessageBox::Yes) {
            return;
        }
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 PIAgent"),
                QStringLiteral("\"%1\" /pi").arg(sBatNative))) {
            QMessageBox::warning(this, QStringLiteral("更新 PIAgent"),
                QStringLiteral("未能以管理员权限启动更新（UAC 被取消或被拒）。"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("更新 PIAgent"),
            QStringLiteral("已在管理员窗口中启动重部署（源码形态）。\n\n"
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
        if (!LaunchCmdWindow(true, QStringLiteral("AutoCode - 更新 CodexHarness"),
                QStringLiteral("npm install @openai/codex@%1 --save-exact")
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
        ? QStringLiteral("更新本机 DeepSeekHarness（fetch+reset 到远端最新 tag）")
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
    for (int iBackend = 1; iBackend <= 4; ++iBackend) {
        // Claude 行本地值未知且探测在飞：冻结结论为「读取中…」，避免短暂显示
        //   「与远端不同：→ x.y.z」误读（评审 v1 n3 瞬态）。
        if (iBackend == 1 && m_asLocalVer[1].isEmpty()
            && m_aProc.contains(EProcClaudeLocal)) {
            SetVerdictCell(1, QStringLiteral("读取中…"));
            continue;
        }
        SetVerdictCell(iBackend, Verdict(m_asLocalVer[iBackend], m_asLatestVer[iBackend], iBackend));
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

QString CVersionDialog::Verdict(const QString& a_sLocal, const QString& a_sLatest,
                                int a_iBackend) {
    // 优先级链：相等 → backend==4 锁定 → latest 空 → 新版 → 其它不等。
    const QString sLocal = a_sLocal.trimmed();
    const QString sLatest = a_sLatest.trimmed();
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
