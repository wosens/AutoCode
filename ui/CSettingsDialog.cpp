#include "CSettingsDialog.h"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QProcess>
#include <QProgressDialog>
#include <QScreen>
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

#include "CDeepSeekRunner.h"
#include "CProjectManager.h"
#include "CCodexRunner.h"
#include "CPiAgentRunner.h"
#include "CHermesRunner.h"

// 2026.08.27 几何在屏校验（口径同 CMainWindow::IsGeometryOnScreen）：与任一屏幕的
//   可用区（排除任务栏）相交即视为在屏——换显示器/改分辨率后的陈旧坐标丢弃。
static bool IsGeoOnScreen(const QRect& a_stGeo) {
    if (!a_stGeo.isValid() || a_stGeo.width() <= 0 || a_stGeo.height() <= 0) {
        return false;
    }
    const QList<QScreen*> aScreens = QGuiApplication::screens();
    for (const QScreen* pSc : aScreens) {
        if (pSc != nullptr && pSc->availableGeometry().intersects(a_stGeo)) {
            return true;
        }
    }
    return false;
}

// —— 模型表列宽持久化（2026.09.11，口径同 CRollbackDialog::SaveLayoutState）——
// 显式整型串存 [Layout] SettingsModelCols{Dsh,Pi,Codex,Hermes}（ConfigFilePath 恒
// 全局 config.ini，界面态不参与任务级覆盖）；恢复严格校验个数 + 限幅 [24,2000]，
// 坏档整表自弃回缺省值。不用 QSettings（UTF-8 整文件重写会打乱 profile API 写下的
// ANSI 中文值，见 CRollbackDialog 同名注释）；写后冲刷 profile 缓存保证即时落盘。
static QString ReadProfileString(const QString& a_sFile, const wchar_t* a_pwsSect,
                                 const wchar_t* a_pwsKey) {
    wchar_t szBuf[2048] = {0};   // 列宽整型串最长数十字符，2K 富余
    const DWORD dw = GetPrivateProfileStringW(a_pwsSect, a_pwsKey, L"", szBuf, 2048,
                                              a_sFile.toStdWString().c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dw));
}

static QList<int> SplitInts(const QString& a_s, int a_iExpect) {
    // 恰好 a_iExpect 个整数才通过；空串/个数不符/含非数字字符返回空表（坏档自弃）。
    if (a_s.isEmpty()) {
        return {};
    }
    const QStringList aParts = a_s.split(QLatin1Char(','));
    if (aParts.size() != a_iExpect) {
        return {};
    }
    QList<int> aV;
    for (const QString& sP : aParts) {
        bool bOk = false;
        const int i = sP.toInt(&bOk);
        if (!bOk) {
            return {};
        }
        aV.append(i);
    }
    return aV;
}

static void SaveModelColWidths(CProjectManager* a_pPm, QTreeWidget* a_pTree,
                               const wchar_t* a_pwsKey) {
    if (a_pPm == nullptr || a_pTree == nullptr) {
        return;
    }
    const QString sCfg = a_pPm->ConfigFilePath();
    if (sCfg.isEmpty()) {
        return;
    }
    QString sVals;   // 前六列宽逗号串；末列「当前」stretchLastSection 恒填满不存
    for (int i = 0; i < a_pTree->columnCount() - 1; ++i) {
        if (!sVals.isEmpty()) {
            sVals += QLatin1Char(',');
        }
        sVals += QString::number(a_pTree->columnWidth(i));
    }
    WritePrivateProfileStringW(L"Layout", a_pwsKey, sVals.toStdWString().c_str(),
                               (LPCWSTR)sCfg.utf16());
    // 冲刷 profile 写缓存（WritePrivateProfileStringW 延迟落盘，见 CRollbackDialog 注释）。
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)sCfg.utf16());
}

static void RestoreModelColWidths(CProjectManager* a_pPm, QTreeWidget* a_pTree,
                                  const wchar_t* a_pwsKey) {
    if (a_pPm == nullptr || a_pTree == nullptr) {
        return;
    }
    const QString sCfg = a_pPm->ConfigFilePath();
    if (sCfg.isEmpty()) {
        return;
    }
    const QList<int> aCols = SplitInts(
        ReadProfileString(sCfg, L"Layout", a_pwsKey), a_pTree->columnCount() - 1);
    for (int i = 0; i < aCols.size(); ++i) {
        a_pTree->setColumnWidth(i, qBound(24, aCols.at(i), 2000));   // 同回退表限幅
    }
}

// 2026.08.18 自定义大模型条目编辑对话框（无 Q_OBJECT：纯表单 + exec/lambda 连接，
//   不进 moc/构建系统）。校验：模型 ID / 接入点必填（空则拦截 accept 并提示）；
//   其余空值在 Entry() 回默认（API 类型→anthropic-messages，显示名→模型 ID）。
class CDshModelEditDialog : public QDialog {
public:
    // a_pEntry = nullptr → 新增模式（全默认值）；非空 → 编辑模式回填。
    // a_bPi = true → PiAgent 条目模式（2026.08.20：标题/字段名按 Pi 语境，密钥落点
    //   ~/.pi/agent/models.json；表单控件与校验完全同 DSH）。
    // a_bCodex = true → CodexHarness 条目模式（2026.08.21：API 格式下拉换成
    //   wire_api（chat/responses），默认 chat；密钥落点 ~/.codex）。
    explicit CDshModelEditDialog(QWidget* a_pParent, const SDshModelEntry* a_pEntry,
                                 bool a_bPi = false, bool a_bCodex = false,
                                 bool a_bCopy = false, bool a_bHermes = false)
        : QDialog(a_pParent) {
        m_bCodex = a_bCodex;
        m_bHermes = a_bHermes;
        const QString sTail = a_bCodex ? QStringLiteral("（CodexHarness）")
                         : a_bPi ? QStringLiteral("（PiAgent）")
                         : a_bHermes ? QStringLiteral("（Hermes）") : QString();
        setWindowTitle((a_bCopy ? QStringLiteral("复制自定义模型")
                       : (a_pEntry != nullptr ? QStringLiteral("编辑自定义模型")
                                              : QStringLiteral("新增自定义模型"))) + sTail);
        setMinimumWidth(460);
        auto* pForm = new QFormLayout(this);
        m_pEditDisp = new QLineEdit(this);
        m_pEditDisp->setPlaceholderText(QStringLiteral("如：公司中转 GLM-5.2"));
        m_pEditDisp->setToolTip(QStringLiteral("列表中显示的名字（仅展示用）"));
        pForm->addRow(QStringLiteral("显示名："), m_pEditDisp);
        m_pEditURL = new QLineEdit(this);
        m_pEditURL->setPlaceholderText(QStringLiteral("http://192.168.x.x:9000/claude"));
        m_pEditURL->setToolTip(a_bPi
            ? QStringLiteral("请求地址（pi 侧 anthropic-messages 兼容网关的 baseUrl；"
                             "模型名称与请求地址为必填项）")
            : QStringLiteral(
                "接入点（anthropic-messages 兼容网关的 Base URL；模型 ID 与接入点为必填项）"));
        pForm->addRow(a_bPi ? QStringLiteral("请求地址：") : QStringLiteral("接入点："), m_pEditURL);
        m_pEditModel = new QLineEdit(this);
        m_pEditModel->setPlaceholderText(QStringLiteral("glm-5.2"));
        m_pEditModel->setToolTip(a_bPi
            ? QStringLiteral("模型名称（发给网关的 model 参数；必填）")
            : QStringLiteral("模型 ID（发给网关的 model 参数；必填）"));
        pForm->addRow(a_bPi ? QStringLiteral("模型名称：") : QStringLiteral("模型 ID："), m_pEditModel);
        m_pEditKey = new QLineEdit(this);
        m_pEditKey->setPlaceholderText(QStringLiteral("sk-…（网关不校验可留空）"));
        m_pEditKey->setToolTip(a_bPi
            ? QStringLiteral("API Key。注意：以明文写入 ~/.pi/agent/models.json 的 provider 配置，"
                             "请勿外传该文件；密钥不进命令行与日志")
            : a_bHermes
            ? QStringLiteral("API Key。注意：以明文保存在 config.ini 的 [HermesModels] 节与"
                             "~/.hermes/agent/config.json，请勿把该文件明文外传；"
                             "密钥不进命令行与日志")
            : QStringLiteral(
                "API 密钥。注意：以明文保存在 exe 同级 config.ini 的 [DshModels] 节，"
                "请勿把该文件明文外传到共享目录"));
        pForm->addRow(QStringLiteral("API Key："), m_pEditKey);
        // 2026.08.19 API 类型改下拉（不可自由输入）：候选=pi-ai 库支持的接入点形态，
        //   默认 anthropic-messages（内网中继标准形态）；旧档自由文本值不在候选中时
        //   动态补入并选中（不丢用户已存配置）。
        m_pEditApi = new QComboBox(this);
        if (a_bCodex) {
            // Codex 条目：API 格式 = wire_api（chat completions / responses），默认 chat。
            m_pEditApi->addItems(QStringList()
                << QStringLiteral("chat")
                << QStringLiteral("responses"));
            m_pEditApi->setToolTip(QStringLiteral(
                "wire_api（写入 ~/.codex/config.toml 的 provider 槽位；默认 chat ="
                " OpenAI chat completions 兼容网关，v0.87.0 支持；responses = 官方"
                " Responses API）"));
        } else {
            m_pEditApi->addItems(QStringList()
                << QStringLiteral("anthropic-messages")
                << QStringLiteral("openai-completions")
                << QStringLiteral("openai-responses")
                << QStringLiteral("google-generative-ai")
                << QStringLiteral("google-vertex"));
            m_pEditApi->setToolTip(QStringLiteral(
                "API 格式（Anthropic Messages 原生 = anthropic-messages；其余为 OpenAI 兼容等"
                " pi-ai 支持的接入形态）"));
        }
        pForm->addRow(a_bPi ? QStringLiteral("API 格式：") : QStringLiteral("API 类型："), m_pEditApi);
        m_pSpinCtx = new QSpinBox(this);
        m_pSpinCtx->setRange(1000, 10000000);
        m_pSpinCtx->setSingleStep(10000);
        m_pSpinCtx->setValue(200000);
        m_pSpinCtx->setToolTip(QStringLiteral(
            "该模型的上下文窗口（token），用于上下文占用估算与压缩阈值"));
        pForm->addRow(QStringLiteral("上下文窗口(token)："), m_pSpinCtx);
        m_pSpinMax = new QSpinBox(this);
        m_pSpinMax->setRange(1024, a_bHermes ? 131072 : 1000000);   // Hermes 钳位 131072（网关上限）
        m_pSpinMax->setSingleStep(1024);
        m_pSpinMax->setValue(32768);
        m_pSpinMax->setToolTip(QStringLiteral("单次请求最大输出 token（网关侧上限）"));
        pForm->addRow(QStringLiteral("最大输出(token)："), m_pSpinMax);
        auto* pBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        pBtns->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
        pBtns->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        connect(pBtns, &QDialogButtonBox::accepted, this, [this]() {
            if (m_pEditModel->text().trimmed().isEmpty() || m_pEditURL->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("自定义模型"),
                                     QStringLiteral("模型 ID 与接入点为必填项。"));
                return;
            }
            // 2026.09.09 v2.1.129 上下文超配提醒（第 4 次复发的 1M 类坑：条目配模型原生
            //   窗口而服务端真实窗口更小——pi 以为有 1M 永不自动压缩，对话涨满即 400
            //   exceed_context_size_error；实害：本地 llama.cpp n_ctx=65536 配 1000000）。
            //   非阻断（大窗口真实存在如 SenseNova 528K），仅提示核对服务端真实窗口。
            if (m_pSpinCtx->value() > 262144) {
                const QMessageBox::StandardButton eRet = QMessageBox::question(this,
                    QStringLiteral("自定义模型"),
                    QStringLiteral("上下文窗口填了 %1 token（>256K）。请确认这是"
                                   "【服务端真实可用的窗口】而非模型原生上限——配超真实窗口时"
                                   "pi 不会自动压缩，对话涨满后端即报 400 exceed_context_size_error。"
                                   "仍按此值保存吗？").arg(m_pSpinCtx->value()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (eRet != QMessageBox::Yes) {
                    return;
                }
            }
            accept();
        });
        connect(pBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);
        pForm->addRow(QString(), pBtns);
        if (a_pEntry != nullptr) {
            m_pEditDisp->setText(a_pEntry->sDisp);
            m_pEditURL->setText(a_pEntry->sBaseURL);
            m_pEditModel->setText(a_pEntry->sModelId);
            m_pEditKey->setText(a_pEntry->sApiKey);
            const int iApi = m_pEditApi->findText(a_pEntry->sApi);
            if (iApi >= 0) {
                m_pEditApi->setCurrentIndex(iApi);
            } else if (!a_pEntry->sApi.isEmpty()) {
                m_pEditApi->addItem(a_pEntry->sApi);   // 旧档自由文本值：补入保持可见可选
                m_pEditApi->setCurrentIndex(m_pEditApi->count() - 1);
            }
            m_pSpinCtx->setValue(static_cast<int>(qBound(qint64(1000), a_pEntry->iCtx, qint64(10000000))));
            m_pSpinMax->setValue(qBound(1024, a_pEntry->iMaxTok, 1000000));
        }
    }

    SDshModelEntry Entry() const {
        SDshModelEntry stE;
        stE.sDisp    = m_pEditDisp->text().trimmed();
        stE.sBaseURL = m_pEditURL->text().trimmed();
        stE.sApiKey  = m_pEditKey->text().trimmed();
        stE.sApi     = m_pEditApi->currentText().trimmed();   // 2026.08.19 下拉取值（不可输入）
        stE.sModelId = m_pEditModel->text().trimmed();
        if (stE.sApi.isEmpty()) {
            stE.sApi = m_bCodex ? QStringLiteral("chat")
                                : QStringLiteral("anthropic-messages");
        }
        if (stE.sDisp.isEmpty()) {
            stE.sDisp = stE.sModelId;
        }
        stE.iCtx    = m_pSpinCtx->value();
        stE.iMaxTok = m_pSpinMax->value();
        if (m_bHermes) {   // 双侧收口（与 CHermesRunner::ClampMaxTok 同口径）
            stE.iMaxTok = qMin(stE.iMaxTok, 131072);
        }
        return stE;
    }

private:
    QLineEdit* m_pEditDisp  = nullptr;
    QLineEdit* m_pEditURL   = nullptr;
    QLineEdit* m_pEditModel = nullptr;
    QLineEdit* m_pEditKey   = nullptr;
    QComboBox* m_pEditApi   = nullptr;   // 2026.08.19 API 类型下拉（原 QLineEdit 自由输入）
    QSpinBox*  m_pSpinCtx   = nullptr;
    QSpinBox*  m_pSpinMax   = nullptr;
    bool       m_bCodex     = false;   // 2026.08.21 Codex 条目模式（wire_api 下拉/默认值）
    bool       m_bHermes    = false;   // 任务 3213cdbf Hermes 条目模式（maxTok 上限 131072）
};

CSettingsDialog::CSettingsDialog(CProjectManager* a_pPm, QWidget* a_pParent,
                                 CVersionDialog::FBusyCheck a_fnBackendBusy,
                                 const QString& a_sTaskScope)
    : QDialog(a_pParent), m_fnBackendBusy(std::move(a_fnBackendBusy)) {
    m_pPm = a_pPm;   // 2026.08.27 几何持久化用（done() 落盘）；非拥有。
    SetupUi();
    // 2026.09.09 v2.2.145 任务级配置覆盖提示（改对话框内提示条，勿用标题后缀——
    //   标题后缀曾被用户误认成「多出来的 tab」）：选中任务时顶部黄色提示条注明
    //   本次读/写均落在任务档（.autodev/tasks/<id>/config.ini，缺键继承全局）。
    if (!a_sTaskScope.isEmpty()) {
        if (auto* pLay = qobject_cast<QVBoxLayout*>(layout()); pLay != nullptr) {
            auto* pLblScope = new QLabel(
                QStringLiteral("◆ 任务模式：本次设置保存到任务目录（任务：%1）；"
                               "未选任务时保存到程序目录。模型条目会同时并入全局缺省配置。")
                    .arg(a_sTaskScope), this);
            pLblScope->setWordWrap(true);
            pLblScope->setStyleSheet(
                QStringLiteral("background:#fff7d6;border:1px solid #e0c36a;"
                               "border-radius:4px;padding:6px 8px;color:#6b5900;"));
            pLay->insertWidget(0, pLblScope);   // 提示条插到最前（QTabWidget 之上）
        }
    }
    // ⚠ v2.1.143~v2.2.145 曾在此处多出一次 SetupUi()（构造双套 UI——顶部多一条空表
    //   Tab 条＝用户报的「多出来的 tab/模型全部清空」真因，v2.2.146 删除），勿面加。

    // 构造时回填当前值（a_pPm 为空 → 全默认，仅调试/极端场景）。
    const int iDefMaxIter = (a_pPm != nullptr) ? a_pPm->LoadMaxIter() : 3;
    const int iDefMaxResume = (a_pPm != nullptr) ? a_pPm->LoadMaxResume() : 3;
    const int iDefReviewResume = (a_pPm != nullptr) ? a_pPm->LoadReviewResume() : 1;
    const int iDefReviewPar = (a_pPm != nullptr) ? a_pPm->LoadReviewParallel() : 3;
    const QString sDefCmd = (a_pPm != nullptr) ? a_pPm->LoadClaudeCmd()
                                               : QStringLiteral("claude");
    const int iDefAskTimeout = (a_pPm != nullptr) ? a_pPm->LoadAskTimeoutSec() : 600;
    const bool bDefWeb = (a_pPm != nullptr) ? a_pPm->LoadWebSearch() : true;
    const int iDefPct = (a_pPm != nullptr) ? a_pPm->LoadAutoCompactPct() : 75;
    const bool bDefExpert = (a_pPm != nullptr) ? a_pPm->LoadExpertAssistant() : false;
    // 2026-10-08 claude 跟随共享表开关（大模型页）。
    const bool bDefClaudeFollow = (a_pPm != nullptr) ? a_pPm->LoadClaudeFollowShared() : false;
    const int iDefAdhocTurns = (a_pPm != nullptr) ? a_pPm->LoadAdhocMaxTurns() : 40;
    const bool bDefAdhocTurnsAuto = (a_pPm != nullptr) ? a_pPm->LoadAdhocMaxTurnsAuto() : true;
    const QString sDefScan = (a_pPm != nullptr) ? a_pPm->LoadScanExcludeDirs() : QString();
    const QString sDefBackup = (a_pPm != nullptr) ? a_pPm->LoadBackupExcludeDirs() : QString();
    // 2026.09.23 资源管理器隐藏目录：预填生效清单（缺键=内置默认，用户在此基础上增删）。
    const QString sDefExplHidden = (a_pPm != nullptr) ? a_pPm->LoadExplorerHiddenDirs()
                                                      : CProjectManager::DefaultExplorerHiddenDirs();
    // 2026-09-26 内置测试沙箱默认值（[Sandbox]）。
    const bool bDefSbxEnable = (a_pPm != nullptr) ? a_pPm->LoadSandboxEnable() : true;
    const bool bDefSbxAuto   = (a_pPm != nullptr) ? a_pPm->LoadSandboxAutoGhost() : true;
    const bool bDefSbxRec    = (a_pPm != nullptr) ? a_pPm->LoadSandboxRecord() : false;
    const int  iDefSbxFps    = (a_pPm != nullptr) ? a_pPm->LoadSandboxFps() : 5;
    const int  iDefSbxRetain = (a_pPm != nullptr) ? a_pPm->LoadSandboxRetainDays() : 7;
    // 2026.08.18 Agent 后端 + DSH 参数回填（默认 1=claude；路径空=未配置）。
    const int iDefBackend = (a_pPm != nullptr) ? a_pPm->LoadAgentBackend() : 1;
    m_iCfgBackend = iDefBackend;   // 2026.08.21 已存后端不可用时不静默弹回（见头文件注）
    const QString sDefDshNode = (a_pPm != nullptr) ? a_pPm->LoadDshNodePath() : QString();
    // 2026.08.18 轻量自动配置：根目录显式键优先；空则从旧档 bin.js 路径反推
    //   （三路径时代的配置符合固定布局 → 升级后免重填）。
    QString sDefDshRoot = (a_pPm != nullptr) ? a_pPm->LoadDshRoot() : QString();
    if (sDefDshRoot.isEmpty() && a_pPm != nullptr) {
        const QString sLegacyBin = QDir::fromNativeSeparators(a_pPm->LoadDshBinJsPath().trimmed());
        const QString sSuffix = QStringLiteral("/packages/examples/jsonrpc-demo/lib/bin.js");
        if (sLegacyBin.endsWith(sSuffix, Qt::CaseInsensitive)) {
            sDefDshRoot = QDir::toNativeSeparators(sLegacyBin.left(sLegacyBin.length() - sSuffix.length()));
        }
    }
    const QString sDefDshProvider = (a_pPm != nullptr) ? a_pPm->LoadDshProvider()
                                                       : QStringLiteral("glm-custom");
    // 2026.09.27 额外可写目录回填（';' 分隔原样展示）。
    const QStringList aDefDshExtra = (a_pPm != nullptr) ? a_pPm->LoadDshExtraWriteDirs()
                                                        : QStringList();
    const double dDefPriceIn = (a_pPm != nullptr) ? a_pPm->LoadDshPriceInPerM() : 3.0;
    const double dDefPriceCr = (a_pPm != nullptr) ? a_pPm->LoadDshPriceCacheReadPerM() : 0.3;
    const double dDefPriceCw = (a_pPm != nullptr) ? a_pPm->LoadDshPriceCacheWritePerM() : 3.75;
    const double dDefPriceOut = (a_pPm != nullptr) ? a_pPm->LoadDshPriceOutPerM() : 15.0;

    m_pSpinMaxIter->setValue(iDefMaxIter);
    m_pSpinMaxResume->setValue(iDefMaxResume);
    m_pSpinReviewResume->setValue(iDefReviewResume);
    m_pSpinReviewPar->setValue(iDefReviewPar);
    // 2026.08.18 修「打开设置 DSH 被弹回 claude」：构造序改为先填根目录并完成安装
    //   检测、再选配置里的后端。旧序 setCurrentIndex 先触发 UpdateDshEnabled，此时
    //   m_bDshInstalled 还是默认 false，DSH 选中态被联动逻辑立即弹回 claude。
    // 2026-10-08 同 Hermes：DSH node 路径输入框已随 v3.3.274 模型组重构拆除，
    //   回填补判空防构造即崩（下同 Pi/Codex）。
    if (m_pEditDshNode != nullptr) {
        m_pEditDshNode->setText(sDefDshNode);
    }
    m_pEditDshRoot->setText(sDefDshRoot);
    QStringList aExtraShow;
    for (const QString& s : aDefDshExtra) {
        aExtraShow.append(QDir::toNativeSeparators(s));
    }
    m_pEditDshWsRoot->setText(aExtraShow.join(QLatin1Char(';')));
    m_pEditDshProvider->setText(sDefDshProvider);
    // 2026-10-08 思考强度回填（空/未知档 → 首项「（默认）」）。
    {
        const QString sDefDshEffort = (a_pPm != nullptr) ? a_pPm->LoadDshEffort() : QString();
        m_pComboDshEffort->setCurrentIndex(
            m_pComboDshEffort->findText(sDefDshEffort) < 0 ? 0
                                                            : m_pComboDshEffort->findText(sDefDshEffort));
    }
    m_pSpinDshPriceIn->setValue(dDefPriceIn);
    m_pSpinDshPriceCacheRead->setValue(dDefPriceCr);
    m_pSpinDshPriceCacheWrite->setValue(dDefPriceCw);
    m_pSpinDshPriceOut->setValue(dDefPriceOut);
    DetectDshInstalled(true);
    // 2026.08.19 PIAgent 参数回填（[Agent] Pi*）：构造序理由同上——先填根目录并完成
    //   安装检测、再选配置里的后端，否则 setCurrentIndex(3) 先触发 UpdatePiEnabled，
    //   m_bPiInstalled 尚为 false 会把 pi 选中态立即弹回 claude。
    if (m_pEditPiNode != nullptr) {
        m_pEditPiNode->setText((a_pPm != nullptr) ? a_pPm->LoadPiNodePath() : QString());
    }
    m_pEditPiRoot->setText((a_pPm != nullptr) ? a_pPm->LoadPiRoot() : QString());
    m_pEditPiProvider->setText((a_pPm != nullptr) ? a_pPm->LoadPiProvider()
                                                  : QStringLiteral("anthropic"));
    const QString sDefPiThinking = (a_pPm != nullptr) ? a_pPm->LoadPiThinking() : QString();
    m_pComboPiThinking->setCurrentIndex(
        m_pComboPiThinking->findText(sDefPiThinking) < 0 ? 0
                                                         : m_pComboPiThinking->findText(sDefPiThinking));
    // 2026.08.20 PIAgent 计费单价回填（$/M token，[Agent] PiPrice*PerM）。
    if (m_pSpinPiPriceIn != nullptr) {
        m_pSpinPiPriceIn->setValue((a_pPm != nullptr) ? a_pPm->LoadPiPriceInPerM() : 3.0);
    }
    if (m_pSpinPiPriceCacheRead != nullptr) {
        m_pSpinPiPriceCacheRead->setValue((a_pPm != nullptr) ? a_pPm->LoadPiPriceCacheReadPerM() : 0.3);
    }
    if (m_pSpinPiPriceCacheWrite != nullptr) {
        m_pSpinPiPriceCacheWrite->setValue((a_pPm != nullptr) ? a_pPm->LoadPiPriceCacheWritePerM() : 3.75);
    }
    if (m_pSpinPiPriceOut != nullptr) {
        m_pSpinPiPriceOut->setValue((a_pPm != nullptr) ? a_pPm->LoadPiPriceOutPerM() : 15.0);
    }
    DetectPiInstalled(true);
    // CodexHarness 第四后端参数回填（[Agent] Codex*；构造序同 Pi：先检测后选后端）。
    if (m_pEditCodexNode != nullptr) {
        m_pEditCodexNode->setText((a_pPm != nullptr) ? a_pPm->LoadCodexNodePath() : QString());
    }
    m_pEditCodexRoot->setText((a_pPm != nullptr) ? a_pPm->LoadCodexRoot() : QString());
    m_pEditCodexProvider->setText((a_pPm != nullptr) ? a_pPm->LoadCodexProvider()
                                                     : QString());
    m_pComboCodexWireApi->setCurrentIndex(
        m_pComboCodexWireApi->findText((a_pPm != nullptr) ? a_pPm->LoadCodexWireApi()
                                                          : QStringLiteral("chat")) < 0
            ? 0 : m_pComboCodexWireApi->findText(
                      (a_pPm != nullptr) ? a_pPm->LoadCodexWireApi()
                                         : QStringLiteral("chat")));
    DetectCodexInstalled(true);
    // 2026-10-08 思考强度回填（空/未知档 → 首项「（默认）」）。
    {
        const QString sDefCodexEffort = (a_pPm != nullptr) ? a_pPm->LoadCodexEffort() : QString();
        m_pComboCodexEffort->setCurrentIndex(
            m_pComboCodexEffort->findText(sDefCodexEffort) < 0 ? 0
                                                               : m_pComboCodexEffort->findText(sDefCodexEffort));
    }
    // 2026.08.21 Codex 自定义大模型表回填（[CodexModels]；2026-10-08 列表 UI 收敛到大模型页）。
    m_aCodexModels = (a_pPm != nullptr) ? a_pPm->LoadCodexModels(&m_iCodexActiveModel)
                                        : QList<SDshModelEntry>();
    // 任务 3213cdbf Hermes 第五后端回填（[Agent] Hermes* / [HermesModels]）。
    // 2026-10-08 修构造即崩（v3.3.274 半截重构遗留）：Hermes node 路径输入框已随模型组
    //   拆除而不再创建（m_pEditHermesNode 恒 nullptr），此处旧回填无判空直解引用——
    //   主程序打开设置对话框同样必崩（同 2383/2590 两处既有判空口径补齐）。
    if (m_pEditHermesNode != nullptr) {
        m_pEditHermesNode->setText((a_pPm != nullptr) ? a_pPm->LoadHermesNodePath() : QString());
    }
    m_pEditHermesRoot->setText((a_pPm != nullptr) ? a_pPm->LoadHermesRoot() : QString());
    m_pSpinHermesPriceIn->setValue((a_pPm != nullptr) ? a_pPm->LoadHermesPriceInPerM() : 3.0);
    m_pSpinHermesPriceCacheRead->setValue(
        (a_pPm != nullptr) ? a_pPm->LoadHermesPriceCacheReadPerM() : 0.3);
    m_pSpinHermesPriceCacheWrite->setValue(
        (a_pPm != nullptr) ? a_pPm->LoadHermesPriceCacheWritePerM() : 3.75);
    m_pSpinHermesPriceOut->setValue((a_pPm != nullptr) ? a_pPm->LoadHermesPriceOutPerM() : 15.0);
    // 2026-10-08 思考强度回填（空/未知档 → 首项「（默认）」）。
    {
        const QString sDefHermesEffort = (a_pPm != nullptr) ? a_pPm->LoadHermesEffort() : QString();
        m_pComboHermesEffort->setCurrentIndex(
            m_pComboHermesEffort->findText(sDefHermesEffort) < 0 ? 0
                                                                 : m_pComboHermesEffort->findText(sDefHermesEffort));
    }
    DetectHermesInstalled(true);
    m_aHermesModels = (a_pPm != nullptr) ? a_pPm->LoadHermesModels(&m_iHermesActiveModel)
                                         : QList<SDshModelEntry>();
    // 2026.08.20 PiAgent 自定义大模型表回填（[PiModels]；2026-10-08 列表 UI 收敛到大模型页）。
    m_aPiModels = (a_pPm != nullptr) ? a_pPm->LoadPiModels(&m_iPiActiveModel)
                                     : QList<SDshModelEntry>();
    m_pComboBackend->setCurrentIndex(
        m_pComboBackend->findData(iDefBackend) < 0 ? 0 : m_pComboBackend->findData(iDefBackend));
    m_pEditClaudeCmd->setText(sDefCmd);
    m_pSpinAskTimeout->setValue(iDefAskTimeout);
    m_pChkWebSearch->setChecked(bDefWeb);
    m_pChkExpertAssistant->setChecked(bDefExpert);
    if (m_pChkClaudeFollow != nullptr) { m_pChkClaudeFollow->setChecked(bDefClaudeFollow); }
    // 2026-10-08 思考预算回填（0 = 默认不注入 MAX_THINKING_TOKENS）。
    if (m_pSpinClaudeThinkTok != nullptr) {
        m_pSpinClaudeThinkTok->setValue((a_pPm != nullptr) ? a_pPm->LoadClaudeMaxThinkTok() : 0);
    }
    m_pSpinCompactPct->setValue(iDefPct);
    // 2026.09.27 「上下文窗口上限」设置项已删：未设时按模型自动探测；手工覆盖
    //   入口保留在助手上下文进度条右键「设置上下文大小…」（同一 [Adhoc] ContextLimit）。
    m_pSpinMaxTurns->setValue(iDefAdhocTurns);
    m_pChkMaxTurnsAuto->setChecked(bDefAdhocTurnsAuto);
    // 2026-10-08 提示词长度闸门回填。
    if (m_pSpinPromptCtxPct != nullptr) {
        m_pSpinPromptCtxPct->setValue(
            (a_pPm != nullptr) ? a_pPm->LoadAdhocPromptCtxPctMax() : 5);
    }
    if (m_pSpinPromptTokMax != nullptr) {
        m_pSpinPromptTokMax->setValue(
            (a_pPm != nullptr) ? a_pPm->LoadAdhocPromptTokMax() : 0);
    }
    m_pEditScanDirs->setText(sDefScan);
    m_pEditBackupDirs->setText(sDefBackup);
    m_pEditExplHidden->setText(sDefExplHidden);
    // 2026-09-26 内置测试沙箱回填。
    m_pChkSbxEnable->setChecked(bDefSbxEnable);
    m_pChkSbxAuto->setChecked(bDefSbxAuto);
    m_pChkSbxRecord->setChecked(bDefSbxRec);
    m_pSpinSbxFps->setValue(iDefSbxFps);
    m_pSpinSbxRetain->setValue(iDefSbxRetain);
    // 2026.08.18 自定义大模型表回填（[DshModels]；空表 / 无激活 = 未启用，走固定路由）。
    m_aDshModels = (a_pPm != nullptr) ? a_pPm->LoadDshModels(&m_iDshActiveModel)
                                      : QList<SDshModelEntry>();
    RefreshDshModelList();
    // 2026.09.10 公共模型配置：四张表合并归一为一份共享表（列表 + 当前项）——
    //   四页展示同一份数据，切换后端无需重配模型（保存时同一份内容写回四张节）。
    UnifySharedModels();

    // 2026.08.27 窗口几何：优先恢复上次保存的（[SettingsWindow]）；无记录/已出屏则用
    //   默认尺寸。默认取 sizeHint（含最高页签的完整内容）夹在父窗口所在屏工作区 95% 内——
    //   QDialog::exec 的 adjustSize 会把窗口夹到屏幕 2/3，PiAgent 页内容较高时窗口被
    //   压到布局最小尺寸以下，出现按钮行叠进大模型列表（本次修复目标）。
    QRect stGeo;
    if (m_pPm != nullptr && m_pPm->LoadSettingsDialogGeometry(stGeo)
        && IsGeoOnScreen(stGeo)) {
        // 2026.09.25 修「逐次上漂」：setGeometry 改 move()+resize()。
        // 2026.09.26 修「每开一次下漂 31px」：Qt6.8 构造期（原生窗未建）move() 按
        //   posIncludesFrame「含框架」语义记账——建窗后窗口实际坐在 frame=请求y /
        //   client=请求y+31，而 y()/geometry() 在框架 strut 解析前谎报请求 y；
        //   done() 保存时若用客户区坐标（strut 解析后真值）与恢复侧（框架语义）
        //   混用 → 每开一次 +31 下漂（打点实证 geometry.y=115 vs frameGeometry.y=84）。
        //   两侧统一框架原点语义：保存 frameGeometry().x/y（见 done()），此处
        //   move(stGeo.topLeft()) 即框架原点 → 往返零漂移。尺寸为客户区尺寸不受影响。
        move(stGeo.topLeft());
        resize(stGeo.size());
    } else {
        QSize szDef = sizeHint();
        const QScreen* pSc = (parentWidget() != nullptr && parentWidget()->screen() != nullptr)
                                 ? parentWidget()->screen()
                                 : QGuiApplication::primaryScreen();
        if (pSc != nullptr) {
            const QRect rcWork = pSc->availableGeometry();
            szDef = szDef.boundedTo(QSize(rcWork.width() * 95 / 100,
                                          rcWork.height() * 95 / 100));
        }
        resize(szDef.expandedTo(QSize(620, 460)));
    }

    // 2026.09.11 模型表列宽恢复（[Layout] SettingsModelCols*；缺档/坏档用上方缺省值，
    //   四页各存各的——同一份共享数据、各视图宽度独立，口径同回退对话框两表）。
    RestoreModelColWidths(m_pPm, m_pTreeDshModels, L"SettingsModelColsDsh");
}

void CSettingsDialog::done(int a_r) {
    // 2026.08.27 几何落盘：accept / reject / 标题栏 X 关闭（触发 reject）都经 done()，
    //   在此统一保存；失败静默（下次用默认几何，不影响功能）。
    // 2026.09.11 模型表列宽同点落盘（用户拖表头调宽后，确定/取消/X 关闭都记忆）。
    if (m_pPm != nullptr) {
        // 2026.09.26 修「每开一次下漂 31px」：保存改用框架原点（frameGeometry 的
        //   x/y）+ 客户区尺寸——构造期 move() 在原生窗未建时按 Qt6.8
        //   posIncludesFrame「含框架」语义记账，建窗后窗口实际坐在 frame=请求y /
        //   client=请求y+31（打点实证：done() 时 geometry().y()=115 而
        //   frameGeometry().y()=84，两坐标系混用→逐次 +31 漂移）。恢复侧同样
        //   走建窗前 move()（框架语义），两侧同坐标系→精确往返零漂移。
        m_pPm->SaveSettingsDialogGeometry(
            QRect(frameGeometry().x(), frameGeometry().y(), width(), height()));
        SaveModelColWidths(m_pPm, m_pTreeDshModels, L"SettingsModelColsDsh");
    }
    QDialog::done(a_r);
}

void CSettingsDialog::SetupUi() {
    setWindowTitle(QStringLiteral("设置"));
    setMinimumWidth(520);

    // —— 运行策略（2026.09.27 三项合并同一行，压缩纵向占用）——
    // 2026.09.27 合并行辅助：往 HBox 追加「标签 + 控件」对（addWidget 自动接管
    //   父子，标签免父参；项间距离由调用方 addSpacing 控制）。
    auto fnAddPair = [](QHBoxLayout* a_pLay, const QString& a_sLbl, QWidget* a_pWgt,
                        int a_iStretch = 0) {
        a_pLay->addWidget(new QLabel(a_sLbl));
        a_pLay->addWidget(a_pWgt, a_iStretch);
    };
    auto* pGroupPolicy = new QGroupBox(QStringLiteral("运行策略"), this);
    auto* pLayPolicy = new QHBoxLayout(pGroupPolicy);
    m_pSpinMaxIter = new QSpinBox(pGroupPolicy);
    m_pSpinMaxIter->setRange(1, 9);
    m_pSpinMaxIter->setToolTip(QStringLiteral(
        "单个阶段被评审驳回后的最大重跑次数，超过则熔断暂停并升级到用户处理（config.ini [Policy] MaxIter）"));
    m_pSpinMaxResume = new QSpinBox(pGroupPolicy);
    m_pSpinMaxResume->setRange(1, 9);
    m_pSpinMaxResume->setToolTip(QStringLiteral(
        "阶段命中轮次上限后自动 --resume 续跑的封顶次数（[Policy] MaxResume）"));
    m_pSpinReviewResume = new QSpinBox(pGroupPolicy);
    m_pSpinReviewResume->setRange(1, 9);
    m_pSpinReviewResume->setToolTip(QStringLiteral(
        "轻量评审命中轮次上限后催促落盘结论的续跑封顶次数（[Policy] ReviewResume）"));
    fnAddPair(pLayPolicy, QStringLiteral("单阶段最大迭代次数："), m_pSpinMaxIter);
    pLayPolicy->addSpacing(12);
    fnAddPair(pLayPolicy, QStringLiteral("阶段续跑上限（次）："), m_pSpinMaxResume);
    pLayPolicy->addSpacing(12);
    fnAddPair(pLayPolicy, QStringLiteral("评审续跑上限（次）："), m_pSpinReviewResume);
    pLayPolicy->addStretch(1);

    // —— Agent（2026.09.27 后端/命令/超时三项合并同一行）——
    auto* pGroupAgent = new QGroupBox(QStringLiteral("Agent"), this);
    auto* pLayAgentCol = new QVBoxLayout(pGroupAgent);
    auto* pRowAgent = new QHBoxLayout();
    pLayAgentCol->addLayout(pRowAgent);
    // 2026.08.18 后端全局单选：所有用途（任务/评审/助手/guide）统一用同一后端。
    m_pComboBackend = new QComboBox(pGroupAgent);
    m_pComboBackend->addItem(QStringLiteral("Claude Code"), 1);
    m_pComboBackend->addItem(QStringLiteral("DeepSeekHarness"), 2);
    m_pComboBackend->addItem(QStringLiteral("PIAgent"), 3);
    m_pComboBackend->addItem(QStringLiteral("CodexHarness"), 4);
    m_pComboBackend->addItem(QStringLiteral("Hermes"), 5);   // 任务 3213cdbf 第五后端
    m_pComboBackend->setToolTip(QStringLiteral(
        "编排器使用的 Agent 后端（全局单选，[Agent] Backend）。\n"
        "DeepSeekHarness：JSON-RPC 完整接入（流式过程/token 统计/多轮会话），"
        "须在下方配置三项路径。\n"
        "切换对已有助手无影响（各助手继续用各自原后端，运行中也不变），"
        "仅新建助手/新任务用新后端；任务或 guide 运行中切换暂保留原值，空闲后生效。\n"
        "PIAgent：pi coding-agent CLI（--mode json 流式接入，磁盘会话跨进程续跑），"
        "参数在本页 PiAgent 页签配置"));
    fnAddPair(pRowAgent, QStringLiteral("Agent 后端："), m_pComboBackend);
    pRowAgent->addSpacing(12);
    connect(m_pComboBackend, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { UpdateDshEnabled(); UpdatePiEnabled(); UpdateCodexEnabled();
                             UpdateHermesEnabled(); });
    // 2026-10-08 全后端共用 node 路径（原四后端页各自字段收敛于此；[Agent] NodePath）。
    m_pEditNodePath = new QLineEdit(pGroupAgent);
    m_pEditNodePath->setPlaceholderText(QStringLiteral("node.exe 绝对路径；留空使用 PATH"));
    m_pEditNodePath->setToolTip(QStringLiteral(
        "node.exe 绝对路径（[Agent] NodePath；空 = 走 PATH 的 node）。DSH/PIAgent/"
        "CodexHarness/Hermes 四后端共用——2026-10-08 起统一在常用页配置（老配置的"
        "各后端专用 node 键自动迁移）"));
    fnAddPair(pRowAgent, QStringLiteral("node 路径："), m_pEditNodePath, 1);
    m_pChkWebSearch = new QCheckBox(QStringLiteral("允许阶段联网检索（WebSearch/WebFetch）"), pGroupAgent);
    m_pChkWebSearch->setToolTip(QStringLiteral(
        "关闭后所有阶段的 --disallowedTools 追加 WebSearch、WebFetch（离线/内网环境用；[Agent] WebSearch）。\n"
        "仅 Claude Code 后端生效（DSH 侧忽略该参数，该项禁用）"));
    // 2026-10-08 联网检索开关与版本管理按钮合并同一行（两者均短，压缩纵向占用）。
    auto* pRowAgentChk = new QHBoxLayout();
    pLayAgentCol->addLayout(pRowAgentChk);
    pRowAgentChk->addWidget(m_pChkWebSearch);
    pRowAgentChk->addSpacing(12);
    // 2026.09 后端版本管理入口（任务 23183ccb）：四后端当前/最新版本与更新一站式；
    //   与主窗功能菜单同源（D6 判据由 CMainWindow 注入透传）。
    //   2026-10-08 与联网检索开关合并同一行（两者均短，压缩纵向占用）。
    auto* pBtnVersion = new QPushButton(
        QStringLiteral("后端版本管理…"), pGroupAgent);
    pBtnVersion->setToolTip(QStringLiteral(
        "查看四 Agent 后端（Claude Code/DSH/PIAgent/Codex）当前版本与远端最新版，\n"
        "并提供各自更新入口（结果仅本次会话有效，不落盘）"));
    connect(pBtnVersion, &QPushButton::clicked, this, &CSettingsDialog::OnVersionManage);
    pRowAgentChk->addWidget(pBtnVersion);
    pRowAgentChk->addStretch(1);

    // —— 2026-10-08 Claude Code 独立页（用户需求：claude 专属参数集中一处，
    //   页序 = 常用/大模型/Claude Code/四后端参数页）——
    auto* pTabClaude = new QWidget(this);
    auto* pLayClaudeCol = new QVBoxLayout(pTabClaude);
    pLayClaudeCol->setContentsMargins(0, 0, 0, 0);
    auto* pGroupClaude = new QGroupBox(QStringLiteral("Claude Code 后端"), pTabClaude);
    auto* pLayClaudeColIn = new QVBoxLayout(pGroupClaude);
    auto* pRowClaudeCmd = new QHBoxLayout();
    pLayClaudeColIn->addLayout(pRowClaudeCmd);
    m_pEditClaudeCmd = new QLineEdit(pGroupClaude);
    m_pEditClaudeCmd->setPlaceholderText(QStringLiteral("claude"));
    m_pEditClaudeCmd->setToolTip(QStringLiteral(
        "claude 命令；默认 claude（PATH 查找），可填绝对路径指定安装（[Agent] ClaudeCmd）。\n"
        "仅在 Claude Code 后端使用"));
    fnAddPair(pRowClaudeCmd, QStringLiteral("claude 命令："), m_pEditClaudeCmd, 1);
    pRowClaudeCmd->addSpacing(12);
    m_pSpinAskTimeout = new QSpinBox(pGroupClaude);
    m_pSpinAskTimeout->setRange(30, 86400);
    m_pSpinAskTimeout->setSuffix(QStringLiteral(" 秒"));
    m_pSpinAskTimeout->setToolTip(QStringLiteral(
        "ask_user 提问等待用户回答的超时（[Agent] AskTimeoutSec，默认 600）。"));
    fnAddPair(pRowClaudeCmd, QStringLiteral("提问等待超时："), m_pSpinAskTimeout);
    auto* pRowClaudeThink = new QHBoxLayout();
    pLayClaudeColIn->addLayout(pRowClaudeThink);
    // 2026-10-08 思考预算（官方 env MAX_THINKING_TOKENS；0=不注入走 claude 默认）。
    m_pSpinClaudeThinkTok = new QSpinBox(pGroupClaude);
    m_pSpinClaudeThinkTok->setRange(0, 2000000);
    m_pSpinClaudeThinkTok->setSingleStep(1024);
    m_pSpinClaudeThinkTok->setSpecialValueText(QStringLiteral("默认（不注入）"));
    m_pSpinClaudeThinkTok->setToolTip(QStringLiteral(
        "思考预算 MAX_THINKING_TOKENS（[Agent] ClaudeMaxThinkTok，token 数；0=不注入走\n"
        "claude/模型默认）。设了就生效（与是否跟随共享模型表无关）。推理模型（如\n"
        "claude-sonnet-4.5+ / GLM-4.6 等）按预算思考，非推理模型忽略"));
    fnAddPair(pRowClaudeThink, QStringLiteral("思考预算："), m_pSpinClaudeThinkTok);
    pRowClaudeThink->addSpacing(12);
    // 2026-10-08 小模型映射（ANTHROPIC_SMALL_FAST_MODEL；空=不注入）。
    m_pEditClaudeSmallModel = new QLineEdit(pGroupClaude);
    m_pEditClaudeSmallModel->setPlaceholderText(QStringLiteral("空 = claude 默认小模型"));
    m_pEditClaudeSmallModel->setToolTip(QStringLiteral(
        "小模型名（[Agent] ClaudeSmallModel → env ANTHROPIC_SMALL_FAST_MODEL）。claude 后台\n"
        "小任务（标题生成/摘要等）用的小模型——走共享表中转网关时网关需同时提供该小模型名，\n"
        "否则可在此填网关支持的模型名映射过去；空 = 不注入（走 claude 自身配置）"));
    fnAddPair(pRowClaudeThink, QStringLiteral("小模型："), m_pEditClaudeSmallModel, 1);
    // 2026-10-08 跟随共享大模型表开关（自「大模型」页迁入，claude 专属语义集中本页）。
    m_pChkClaudeFollow = new QCheckBox(QStringLiteral(
        "后端跟随「模型」页共享表（需 Anthropic 兼容端点的条目）"), pGroupClaude);
    m_pChkClaudeFollow->setToolTip(QStringLiteral(
        "开：Claude Code 后端（任务+助手）使用共享表当前条目——每轮以环境变量注入\n"
        "接入点/密钥/模型（本助手右键覆盖优先）。仅 API 类型为 anthropic-messages 的条目\n"
        "生效，OpenAI 格式条目自动跳过（走 claude 自身配置）。\n"
        "关（默认）：claude 完全走自身订阅/网关配置，共享表不影响它。"));
    pLayClaudeColIn->addWidget(m_pChkClaudeFollow);
    pLayClaudeCol->addWidget(pGroupClaude);
    pLayClaudeCol->addStretch(1);


    // —— DeepSeekHarness 参数（后端选中 DSH 时才可用）——
    // 2026.08.18 轻量自动配置：路径收敛为两行——node（留空走 PATH）+ DSH 根目录
    //   （bin.js/cordis.yml 按固定布局自动推导，不再手填）。组合文件（cordis.yml）须位于
    //   DSH 仓库 examples/ 树内（裸插件名从该目录解析 node_modules，版本管理正本在
    //   AutoCode 仓库 agent/dsh/cordis.autocode.yml）。
    auto* pGroupDsh = new QGroupBox(QStringLiteral("DeepSeekHarness 参数"), this);
    auto* pFormDsh = new QFormLayout(pGroupDsh);
    auto fnAddPathRow = [this, pGroupDsh, pFormDsh](
            const QString& a_sLabel, QLineEdit** a_ppEdit, const QString& a_sTip, bool a_bDir) {
        auto* pRow = new QWidget(pGroupDsh);
        auto* pLay = new QHBoxLayout(pRow);
        pLay->setContentsMargins(0, 0, 0, 0);
        *a_ppEdit = new QLineEdit(pRow);
        (*a_ppEdit)->setToolTip(a_sTip);
        auto* pBtnBrowse = new QPushButton(QStringLiteral("浏览…"), pRow);
        connect(pBtnBrowse, &QPushButton::clicked, this, [this, a_ppEdit, a_bDir]() {
            const QString sSel = a_bDir
                ? QFileDialog::getExistingDirectory(this, QString(), (*a_ppEdit)->text())
                : QFileDialog::getOpenFileName(this, QString(), (*a_ppEdit)->text());
            if (!sSel.isEmpty()) {
                (*a_ppEdit)->setText(QDir::toNativeSeparators(sSel));
            }
        });
        pLay->addWidget(*a_ppEdit, 1);
        pLay->addWidget(pBtnBrowse);
        pFormDsh->addRow(a_sLabel, pRow);
    };
    fnAddPathRow(QStringLiteral("DSH 根目录："), &m_pEditDshRoot,
                 QStringLiteral("deepseek-harness 仓库根目录；bin.js 与 cordis.yml 按固定布局自动推导"
                                "（packages/examples/jsonrpc-demo/lib/bin.js、examples/autocode-agent/"
                                "cordis.yml）。留空时启动自动探测 exe 同级/上级的 DeepSeekHarness 目录"
                                "（[Agent] DshRoot）"), true);
    // 2026.09.27 额外可写目录（';' 分隔多条）：对每条授工程根派生的工作区写 SID 的
    //   ACE（纯 NTFS DACL 放行，零 harness 改动）。浏览=追加一条（不清空已有）。
    {
        auto* pRowWs = new QWidget(pGroupDsh);
        auto* pLayWs = new QHBoxLayout(pRowWs);
        pLayWs->setContentsMargins(0, 0, 0, 0);
        m_pEditDshWsRoot = new QLineEdit(pRowWs);
        m_pEditDshWsRoot->setPlaceholderText(
            QStringLiteral("留空=仅工程内可写；多条用 ; 分隔"));
        m_pEditDshWsRoot->setToolTip(QStringLiteral(
            "DSH 沙箱（workspace-write）可写范围之外的目录白名单（[Dsh] ExtraWriteDirs）。\n"
            "DSH 助手把产物（DLL/EXE 等）拷到关联项目目录（工程根之外）时被沙箱拒绝\n"
            "（[sandbox: file access denied under workspace-write mode]，且无人值守无\n"
            "提权通道）。把目标目录加进来即放行（AutoCode 对其授工作区写 SID 的 ACE，\n"
            "仅影响该目录树，其余范围围栏不变）。\n"
            "例：工程=插件框架\\流程编辑器，产物拷 插件框架\\Execute\\PlugIns，\n"
            "则把 插件框架\\Execute\\PlugIns 加进来。目录须已存在；多条用 ; 分隔。\n"
            "修改后即时生效（运行中助手的下一轮对话也会自愈补授）"));
        auto* pBtnAddWs = new QPushButton(QStringLiteral("添加…"), pRowWs);
        connect(pBtnAddWs, &QPushButton::clicked, this, [this]() {
            const QString sSel = QFileDialog::getExistingDirectory(this, QString());
            if (sSel.isEmpty()) {
                return;
            }
            const QString sNative = QDir::toNativeSeparators(sSel);
            const QString sCur = m_pEditDshWsRoot->text().trimmed();
            if (sCur.isEmpty()) {
                m_pEditDshWsRoot->setText(sNative);
            } else if (!sCur.split(QLatin1Char(';'), Qt::SkipEmptyParts)
                           .contains(sNative, Qt::CaseInsensitive)) {
                m_pEditDshWsRoot->setText(sCur + QLatin1Char(';') + sNative);
            }
        });
        pLayWs->addWidget(m_pEditDshWsRoot, 1);
        pLayWs->addWidget(pBtnAddWs);
        pFormDsh->addRow(QStringLiteral("额外可写目录："), pRowWs);
    }
    m_pEditDshProvider = new QLineEdit(pGroupDsh);
    m_pEditDshProvider->setPlaceholderText(QStringLiteral("glm-custom"));
    m_pEditDshProvider->setToolTip(QStringLiteral(
        "LLM provider 名（组合 llm-pi-ai 内联 providers 键；[Agent] DshProvider，默认 glm-custom）"));
    // 2026-10-08 思考强度（DSH initialize reasoningEffort；[Agent] DshEffort，空=默认不注入）。
    m_pComboDshEffort = new QComboBox(pGroupDsh);
    m_pComboDshEffort->addItem(QString());   // 空 = 默认
    m_pComboDshEffort->addItems(QStringList()
        << QStringLiteral("off") << QStringLiteral("low")
        << QStringLiteral("high") << QStringLiteral("max"));
    m_pComboDshEffort->setItemText(0, QStringLiteral("（默认）"));
    m_pComboDshEffort->setToolTip(QStringLiteral(
        "思考强度（握手 initialize 参数 reasoningEffort；[Agent] DshEffort，\n"
        "空 = 用 DSH 默认；off = 关推理。仅推理模型生效，下一轮对话/新任务即用"));
    // 2026-10-08 Provider+思考强度合并同一行（两短项，同 Pi 页范式，压缩纵向占用）。
    auto* pRowDshPmt = new QWidget(pGroupDsh);
    auto* pLayDshPmt = new QHBoxLayout(pRowDshPmt);
    pLayDshPmt->setContentsMargins(0, 0, 0, 0);
    pLayDshPmt->addWidget(m_pEditDshProvider, 2);
    pLayDshPmt->addSpacing(12);
    pLayDshPmt->addWidget(new QLabel(QStringLiteral("思考强度："), pRowDshPmt));
    pLayDshPmt->addWidget(m_pComboDshEffort, 1);
    pFormDsh->addRow(QStringLiteral("Provider："), pRowDshPmt);
    // 2026.08.18 DSH 计费单价（$/M token）：协议只报 token 不报金额，按单价折算，
    //   默认对齐 claude 后端 Sonnet 档价目（同网关口径）；全 0 = 不折算（费用列回 "-"）。
    auto* pRowPrice = new QWidget(pGroupDsh);
    auto* pLayPrice = new QHBoxLayout(pRowPrice);
    pLayPrice->setContentsMargins(0, 0, 0, 0);
    const QString sPriceTip = QStringLiteral(
        "DSH 协议只报 token 分量不报金额，费用 = 各分量 × 此单价累计（单位 $/百万 token）。\n"
        "默认对齐 Claude Code 后端 Sonnet 档价目（同网关同模型可比）；网关实际计费\n"
        "不同则按实际改。四项全 0 = 不折算，费用列显示“-”。[Agent] DshPrice*PerM");
    auto fnPriceSpin = [pRowPrice, pLayPrice, &sPriceTip](const QString& a_sLbl) {
        auto* pLbl = new QLabel(a_sLbl, pRowPrice);
        auto* pSpin = new QDoubleSpinBox(pRowPrice);
        pSpin->setRange(0.0, 1000.0);
        pSpin->setDecimals(2);
        pSpin->setSingleStep(0.05);
        pSpin->setToolTip(sPriceTip);
        pLayPrice->addWidget(pLbl);
        pLayPrice->addWidget(pSpin, 1);
        return pSpin;
    };
    m_pSpinDshPriceIn = fnPriceSpin(QStringLiteral("输入"));
    m_pSpinDshPriceCacheRead = fnPriceSpin(QStringLiteral("缓存读"));
    m_pSpinDshPriceCacheWrite = fnPriceSpin(QStringLiteral("缓存写"));
    m_pSpinDshPriceOut = fnPriceSpin(QStringLiteral("输出"));
    pFormDsh->addRow(QStringLiteral("计费单价($/M)："), pRowPrice);

    // —— 评审 / 助手（2026.09.27 数值三项合并一行；两开关合并一行）——
    //   「上下文窗口上限」设置行已删：自动压缩阈值与上下文进度条分母在未设时均按
    //   模型自动探测（各后端模型表自带窗口）；手工覆盖入口保留在助手上下文进度条
    //   右键「设置上下文大小…」（同一 [Adhoc] ContextLimit 键，已存值继续生效）。
    auto* pGroupReview = new QGroupBox(QStringLiteral("评审与助手"), this);
    auto* pLayReviewCol = new QVBoxLayout(pGroupReview);
    auto* pRowReview = new QHBoxLayout();
    pLayReviewCol->addLayout(pRowReview);
    m_pSpinReviewPar = new QSpinBox(pGroupReview);
    m_pSpinReviewPar->setRange(1, 8);
    m_pSpinReviewPar->setToolTip(QStringLiteral(
        "每个评审阶段并行拉起的视角数，1=串行回退（[Review] Parallel，默认 3）"));
    fnAddPair(pRowReview, QStringLiteral("评审并行视角数："), m_pSpinReviewPar);
    pRowReview->addSpacing(12);
    m_pSpinCompactPct = new QSpinBox(pGroupReview);
    m_pSpinCompactPct->setRange(10, 95);
    m_pSpinCompactPct->setSuffix(QStringLiteral(" %"));
    m_pSpinCompactPct->setToolTip(QStringLiteral(
        "上下文占用达到窗口的该比例时触发自动压缩；阈值 = 窗口 × 比例"
        "（[Adhoc] AutoCompactPct）。\n仅 Claude Code 后端生效（DSH 内置压缩策略，该项禁用）"));
    fnAddPair(pRowReview, QStringLiteral("自动压缩比例："), m_pSpinCompactPct);
    pRowReview->addSpacing(12);
    // 2026.09.23 助手轮次上限：单条提示词一次执行的最大迭代数（模型一轮工具调用）。
    //   大型多文件重构 40 轮常打满（表现为「达到轮次上限，自动续跑」），可按工作量放大；
    //   触顶时仍会自动续跑 2 次兑底，不会无限跑。
    m_pSpinMaxTurns = new QSpinBox(pGroupReview);
    m_pSpinMaxTurns->setRange(10, 200);
    m_pSpinMaxTurns->setValue(40);
    m_pSpinMaxTurns->setSuffix(QStringLiteral(" 轮"));
    m_pSpinMaxTurns->setToolTip(QStringLiteral(
        "助手单条提示词一次执行的最大轮次（每轮=模型思考+工具调用）；\n"
        "达到上限会自动续跑（最多 2 次）并提示「达到轮次上限」。\n"
        "大型多文件重构频繁触顶时调大此项（[Adhoc] MaxTurns，默认 40，10..200）。\n"
        "更根本的办法是把大任务拆成多条小提示词分步发送"));
    fnAddPair(pRowReview, QStringLiteral("助手轮次上限："), m_pSpinMaxTurns);
    pRowReview->addStretch(1);
    auto* pRowReviewChk = new QHBoxLayout();
    pLayReviewCol->addLayout(pRowReviewChk);
    // 2026.09.23 v3.0.219 工作量评估开关：按提示词难度自动上调（纯本地启发式，零 token）。
    m_pChkMaxTurnsAuto = new QCheckBox(QStringLiteral("按提示词工作量自动上调"), pGroupReview);
    m_pChkMaxTurnsAuto->setChecked(true);
    m_pChkMaxTurnsAuto->setToolTip(QStringLiteral(
        "发送前对提示词做本地工作量评估（零 token、毫秒级）：编号子任务数、点名的源文件数、\n"
        "重活关键词（重构/自测/构建…）与文本长度综合估算，必要时自动上调本轮轮次预算\n"
        "（日志会提示「工作量评估：轮次上限 40 → N」）；小任务不受影响，只升不降。\n"
        "取消勾选则恒用上方基值（[Adhoc] MaxTurnsAuto，默认开）"));
    pRowReviewChk->addWidget(m_pChkMaxTurnsAuto);
    pRowReviewChk->addSpacing(12);
    // 2026.08.26 专家助手开关：启用后助手提交改码提示词时自动验证（编译/测试）并回注重试。
    // 2026.09.26 深度模式实现接通：开关真正生效——开启后非只读提交自动走
    //   「规划→执行→验证」深度链（详见 CTaskOrchestrator::StartDeepPlan 链路）。
    m_pChkExpertAssistant = new QCheckBox(
        QStringLiteral("启用专家助手（深度模式）"), pGroupReview);
    m_pChkExpertAssistant->setToolTip(QStringLiteral(
        "启用后，助手每次非只读提交自动进入深度模式（用 token 换质量，重活免反复手发提示词）：\n"
        "① 规划Agent 先分析请求，产出实施计划与可验证的验收标准；\n"
        "② 执行Agent 照计划实施（未改动工程的纯问答自动跳过后续步骤）；\n"
        "③ 验证Agent 独立按验收标准实际编译/测试核验，未通过自动回炉修复（上限 [Policy] MaxIter 次）；\n"
        "④ 回炉/续跑耗尽自动重启整链，重新规划并携带失败教训（[Adhoc] ExpertRestarts，默认 1，0..5）；\n"
        "全部耗尽则保留会话与改动交还人工。禁用则回到纯对话模式（[Adhoc] ExpertAssistant，默认关闭，须手工勾选启用）"));
    pRowReviewChk->addWidget(m_pChkExpertAssistant);
    pRowReviewChk->addStretch(1);
    // 2026-10-08 提示词长度闸门（用户需求：提示词不超上下文 5% + token 上限，超限自动提炼）：
    //   发送前本地估算 token，超限 → 起一次性提炼 Agent 压缩后再发（对话流留事件行，
    //   原文首 200 字随事件留痕；提炼失败/仍超限则按提炼稿/原文发送并警告）。
    auto* pRowPromptLimit = new QHBoxLayout();
    pLayReviewCol->addLayout(pRowPromptLimit);
    m_pSpinPromptCtxPct = new QSpinBox(pGroupReview);
    m_pSpinPromptCtxPct->setRange(0, 50);
    m_pSpinPromptCtxPct->setValue(5);
    m_pSpinPromptCtxPct->setSuffix(QStringLiteral(" %"));
    m_pSpinPromptCtxPct->setSpecialValueText(QStringLiteral("不限"));
    m_pSpinPromptCtxPct->setToolTip(QStringLiteral(
        "发送提示词的上下文占比上限（[Adhoc] PromptCtxPctMax，默认 5，0=不限）：\n"
        "估算 token > 助手当前模型上下文窗口 × 该比例 → 不直接发送，自动起一次性\n"
        "提炼 Agent 压缩提示词后以提炼稿发送（对话流留事件行可追溯）"));
    fnAddPair(pRowPromptLimit, QStringLiteral("提示词占比上限："), m_pSpinPromptCtxPct);
    pRowPromptLimit->addSpacing(12);
    m_pSpinPromptTokMax = new QSpinBox(pGroupReview);
    m_pSpinPromptTokMax->setRange(0, 1000000);
    m_pSpinPromptTokMax->setValue(0);
    m_pSpinPromptTokMax->setSingleStep(1000);
    m_pSpinPromptTokMax->setSpecialValueText(QStringLiteral("不限"));
    m_pSpinPromptTokMax->setToolTip(QStringLiteral(
        "发送提示词的绝对 token 上限（[Adhoc] PromptTokMax，默认 0=不限）：\n"
        "与占比上限同时生效、取更小者；超限同样走自动提炼。token 为本地估算值\n"
        "（CJK 字符≈1、其余≈1/3），与模型实际计量存在小偏差"));
    fnAddPair(pRowPromptLimit, QStringLiteral("提示词 token 上限："), m_pSpinPromptTokMax);
    pRowPromptLimit->addStretch(1);

    // —— 目录排除（2026-10-08 指纹扫描/资源管理器两项合一行，备份恢复默认值最长保持整行）——
    auto* pGroupDirs = new QGroupBox(QStringLiteral("目录排除"), this);
    auto* pLayDirsCol = new QVBoxLayout(pGroupDirs);
    auto* pRowDirs1 = new QHBoxLayout();
    pLayDirsCol->addLayout(pRowDirs1);
    m_pEditScanDirs = new QLineEdit(pGroupDirs);
    m_pEditScanDirs->setPlaceholderText(QStringLiteral(".autodev"));
    m_pEditScanDirs->setToolTip(QStringLiteral(
        "guide 指纹扫描额外排除的目录名，逗号分隔；留空 = 内置默认。"
        "内置规则：隐藏文件/文件夹（'.' 开头，含 .git/.vs）始终排除，不可关闭；"
        ".autodev 正常扫描，仅指纹表自身 guide.md 不参与比对（防自我变更死循环）"));
    fnAddPair(pRowDirs1, QStringLiteral("指纹扫描排除："), m_pEditScanDirs, 1);
    pRowDirs1->addSpacing(12);
    m_pEditBackupDirs = new QLineEdit(pGroupDirs);
    m_pEditBackupDirs->setPlaceholderText(QStringLiteral(
        ".autodev,build,Execute,.git,.vs,x64"));
    m_pEditBackupDirs->setToolTip(QStringLiteral(
        "代码备份/恢复排除的目录名，逗号分隔；留空 = 内置默认（[Backup] ExcludeDirs）"));
    // 2026.09.23 资源管理器显示隐藏目录：只管树里显不显示，与扫描/备份排除无关。
    m_pEditExplHidden = new QLineEdit(pGroupDirs);
    m_pEditExplHidden->setPlaceholderText(QStringLiteral("build,.git,.vs,node_modules"));
    m_pEditExplHidden->setToolTip(QStringLiteral(
        "资源管理器树中隐藏的目录名，逗号分隔；清空 = 全部可见。\n"
        "只管内置资源管理器的显示，与备份排除/指纹扫描排除互不相干。\n"
        "（[Explorer] HiddenDirs；保存后立即生效，新建/刷新的目录也按新清单处理）"));
    fnAddPair(pRowDirs1, QStringLiteral("资源管理器隐藏目录："), m_pEditExplHidden, 1);
    auto* pRowDirs2 = new QHBoxLayout();
    pLayDirsCol->addLayout(pRowDirs2);
    fnAddPair(pRowDirs2, QStringLiteral("备份恢复排除："), m_pEditBackupDirs, 1);

    // —— 内置测试沙箱（2026-09-26）——：Agent 后代 GUI 窗口自动幽灵化收纳（隐藏 +
    //    点击穿透 + 无任务栏，用户无感），Agent 经 sandbox_* MCP 工具/--sandbox 命令
    //    操控/截图，全程自动 MP4 录像。 ——
    // —— 内置测试沙箱（2026-09-26；2026.09.27 开关两项/录像参数各合并一行）——：
    //    Agent 后代 GUI 窗口自动幽灵化收纳（隐藏 + 点击穿透 + 无任务栏，用户无感），
    //    Agent 经 sandbox_* MCP 工具/--sandbox 命令操控/截图，全程自动 MP4 录像。 ——
    auto* pGroupSbx = new QGroupBox(QStringLiteral("测试沙箱"), this);
    auto* pLaySbxCol = new QVBoxLayout(pGroupSbx);
    auto* pRowSbxChk = new QHBoxLayout();
    pLaySbxCol->addLayout(pRowSbxChk);
    m_pChkSbxEnable = new QCheckBox(QStringLiteral("启用内置测试沙箱"), pGroupSbx);
    m_pChkSbxEnable->setToolTip(QStringLiteral(
        "启用后：Agent（助手/任务）启动的 GUI 程序窗口自动隐藏收纳进沙箱——不抢焦点、"
        "点击穿透、不上任务栏，用户完全无感；Agent 用内置工具截图/点击/输入进行界面"
        "验证（sandbox_* MCP 工具或 --sandbox 命令）。\n禁用则窗口照常显示"
        "（[Sandbox] Enable，默认启用）"));
    pRowSbxChk->addWidget(m_pChkSbxEnable);
    pRowSbxChk->addSpacing(12);
    m_pChkSbxAuto = new QCheckBox(QStringLiteral("自动收纳 Agent 启动的新窗口"), pGroupSbx);
    m_pChkSbxAuto->setToolTip(QStringLiteral(
        "自动幽灵化：Agent 进程树新出现的顶层窗口自动进沙箱（总开关关闭时此开关"
        "无效，可用 --sandbox add 手工收纳）（[Sandbox] AutoGhost，默认开）"));
    pRowSbxChk->addWidget(m_pChkSbxAuto);
    pRowSbxChk->addStretch(1);
    m_pChkSbxRecord = new QCheckBox(QStringLiteral("录制测试过程（MP4，含时间戳与点击标记）"), pGroupSbx);
    m_pChkSbxRecord->setToolTip(QStringLiteral(
        "每个收纳窗口自动录像（PrintWindow 离屏截帧 + H264 编码），存"
        " exe\\sandbox\\rec\\；窗口还原/关闭时自动封盘（[Sandbox] Record，默认开）"));
    auto* pRowSbxRec = new QHBoxLayout();
    pLaySbxCol->addLayout(pRowSbxRec);
    // 2026-10-08 录制开关并入录像参数行（复选框+帧率+保留均短，合一行压缩纵向占用）。
    pRowSbxRec->addWidget(m_pChkSbxRecord);
    pRowSbxRec->addSpacing(12);
    m_pSpinSbxFps = new QSpinBox(pGroupSbx);
    m_pSpinSbxFps->setRange(1, 15);
    m_pSpinSbxFps->setSuffix(QStringLiteral(" fps"));
    m_pSpinSbxFps->setToolTip(QStringLiteral(
        "录像帧率；界面验证场景 5fps 足够（文件小、过程可回放）（[Sandbox] Fps，默认 5）"));
    fnAddPair(pRowSbxRec, QStringLiteral("录像帧率："), m_pSpinSbxFps);
    pRowSbxRec->addSpacing(12);
    m_pSpinSbxRetain = new QSpinBox(pGroupSbx);
    m_pSpinSbxRetain->setRange(1, 365);
    m_pSpinSbxRetain->setSuffix(QStringLiteral(" 天"));
    m_pSpinSbxRetain->setToolTip(QStringLiteral(
        "录像保留天数：启动时清理超期 MP4（[Sandbox] RetainDays，默认 7 天）"));
    fnAddPair(pRowSbxRec, QStringLiteral("录像保留："), m_pSpinSbxRetain);
    pRowSbxRec->addStretch(1);

    // —— 按钮 ——
    auto* pBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    pBtns->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    pBtns->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(pBtns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(pBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // —— Tab 分页（2026.08.18）：第一页「常用」= 运行策略/Agent/评审与助手/目录排除；
    //    第二页「DeepSeekHarness参数设置」= 安装状态行 + DSH 路径与 provider/model
    //    （未安装时整个参数组置灰，后端下拉的 DSH 项同时禁用；后端未选 DSH 时控件
    //    仍可预填，只是经 UpdateDshEnabled 置灰；切换后端立即启用）。 ——
    auto* pTabCommon = new QWidget(this);
    auto* pLayCommon = new QVBoxLayout(pTabCommon);
    pLayCommon->setContentsMargins(0, 0, 0, 0);
    pLayCommon->addWidget(pGroupPolicy);
    pLayCommon->addWidget(pGroupAgent);
    pLayCommon->addWidget(pGroupReview);
    pLayCommon->addWidget(pGroupDirs);
    pLayCommon->addWidget(pGroupSbx);
    pLayCommon->addStretch(1);

    auto* pTabDsh = new QWidget(this);
    auto* pLayDsh = new QVBoxLayout(pTabDsh);
    pLayDsh->setContentsMargins(0, 0, 0, 0);
    // 安装状态行（2026.08.18）：状态标签 + 重新检测 + 单独安装（未装 DSH 时由此一键安装，
    //   装到 exe 上两级 DeepSeekHarness\deepseek-harness 固定布局，探测/回填全自动）。
    auto* pRowDshState = new QWidget(pTabDsh);
    auto* pLayDshState = new QHBoxLayout(pRowDshState);
    pLayDshState->setContentsMargins(0, 0, 0, 0);
    m_pLblDshState = new QLabel(pRowDshState);
    m_pLblDshState->setWordWrap(true);
    m_pLblDshState->setToolTip(QStringLiteral(
        "安装判定：根目录框显式值（或自动探测的 exe 同级/上级 deepseek-harness）下"
        " packages/examples/jsonrpc-demo/lib/bin.js 与 examples/autocode-agent/cordis.yml"
        " 都存在才认已安装（缺组合文件属半安装，点「安装 DeepSeekHarness…」补齐）"));
    m_pBtnDetectDsh = new QPushButton(QStringLiteral("重新检测"), pRowDshState);
    m_pBtnDetectDsh->setToolTip(QStringLiteral(
        "安装完成后点击：重新探测安装状态，命中且根目录为空时自动填入"));
    connect(m_pBtnDetectDsh, &QPushButton::clicked, this, [this]() { DetectDshInstalled(false); });
    m_pBtnInstallDsh = new QPushButton(QStringLiteral("安装 DeepSeekHarness…"), pRowDshState);
    m_pBtnInstallDsh->setToolTip(QStringLiteral(
        "以管理员权限运行环境安装脚本（/dsh 单装模式）：克隆仓库 + pnpm 依赖构建 + "
        "部署 AutoCode 组合文件，完成后回到本页点「重新检测」"));
    connect(m_pBtnInstallDsh, &QPushButton::clicked, this, [this]() { OnInstallDsh(); });
    pLayDshState->addWidget(m_pLblDshState, 1);
    pLayDshState->addWidget(m_pBtnDetectDsh);
    pLayDshState->addWidget(m_pBtnInstallDsh);
    pLayDsh->addWidget(pRowDshState);
    pLayDsh->addWidget(pGroupDsh);

    // —— 自定义大模型（2026.08.18 多条目管理：跨机器自配 LLM）——
    //   条目经组合 cordis.yml 的 autocode-active env 槽位注入（DSH_LLM_* + 密钥值），
    //   激活条目覆盖上方 Provider/模型固定路由；表空则回退固定路由。
    // —— 2026-10-08 大模型独立页（用户需求：第二个 Tab 专门设置大模型；四后端参数页
    //   的模型表 UI 已全部取消，存储四节收敛/任务级覆盖机制不变）——
    auto* pTabModels = new QWidget(this);
    auto* pLayTabModels = new QVBoxLayout(pTabModels);
    pLayTabModels->setContentsMargins(0, 0, 0, 0);
    auto* pGroupModels = new QGroupBox(QStringLiteral("自定义大模型（全部后端共享）"), pTabModels);
    auto* pLayModels = new QVBoxLayout(pGroupModels);
    // 2026.08.19 列表改多列 QTreeWidget（显示名/接入点/API/模型ID/上下文/最大输出/当前），
    //   选中条目的信息一目了然；字体 11 号与全局统一（#7）。
    m_pTreeDshModels = new QTreeWidget(pGroupModels);
    m_pTreeDshModels->setColumnCount(7);
    m_pTreeDshModels->setHeaderLabels(QStringList()
        << QStringLiteral("显示名")
        << QStringLiteral("接入点")
        << QStringLiteral("API 类型")
        << QStringLiteral("模型 ID")
        << QStringLiteral("上下文")
        << QStringLiteral("最大输出")
        << QStringLiteral("当前"));
    m_pTreeDshModels->setRootIsDecorated(false);
    m_pTreeDshModels->setAlternatingRowColors(true);
    QHeaderView* pHdrModels = m_pTreeDshModels->header();
    // 2026.09.11 全列可拖分隔线调宽（原第 1 列 Stretch 不可拖，用户要求每列都能调）：
    //   改全部 Interactive + 末列「当前」stretchLastSection 填满剩余空间（同任务历史
    //   列表口径）；列宽经 [Layout] SettingsModelColsDsh 持久化（构造恢复/done() 落盘）。
    pHdrModels->setSectionsMovable(false);                        // 禁拖动换列序
    pHdrModels->setSectionResizeMode(QHeaderView::Interactive);   // 全列可拖分隔线调宽
    pHdrModels->setStretchLastSection(true);                      // 末列拉伸填满剩余空间
    pHdrModels->resizeSection(0, 130);
    pHdrModels->resizeSection(1, 190);   // 接入点（URL 较长，原 Stretch 列改固定缺省宽）
    pHdrModels->resizeSection(2, 125);
    pHdrModels->resizeSection(3, 100);
    pHdrModels->resizeSection(4, 75);
    pHdrModels->resizeSection(5, 75);
    pHdrModels->resizeSection(6, 42);
    QFont fntModels = m_pTreeDshModels->font();
    fntModels.setPointSize(11);
    m_pTreeDshModels->setFont(fntModels);
    QFont fntModelsHdr = pHdrModels->font();
    fntModelsHdr.setPointSize(11);
    pHdrModels->setFont(fntModelsHdr);
    m_pTreeDshModels->setMinimumHeight(120);
    m_pTreeDshModels->setToolTip(QStringLiteral(
        "自定义模型条目（2026-10-08 起独立「模型」页统一维护，全部后端共享同一份\n"
        "列表与「当前」模型：DSH/Pi/Codex/Hermes 直接使用；Claude Code 勾选下方跟随\n"
        "开关后也用本表。正本存 exe 同级 config.ini 的 [DshModels]/[PiModels]/\n"
        "[CodexModels]/[HermesModels] 四节同内容）。\n"
        "「当前」列打 ● 的条目生效（各后端参数页的 Provider/模型固定路由不参与）。\n"
        "双击条目可直接编辑；列宽拖表头分隔线可调，自动记忆（双击分隔线自适应内容）"));
    connect(m_pTreeDshModels, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { OnDshModelEdit(); });
    // 选中行变化只刷新按钮态（RefreshDshModelList 内部 blockSignals 重建，无重入）
    connect(m_pTreeDshModels, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { RefreshDshModelList(); });
    // 2026-10-08 列表高度随窗口高度自适应：树吃组内富余高度（stretch=1），
    //   组吃页内富余高度（stretch=1，页尾不再 addStretch 抢高度）；
    //   setMinimumHeight(120) 保底不被压没。
    pLayModels->addWidget(m_pTreeDshModels, 1);
    auto* pRowModelBtns = new QWidget(pGroupModels);
    auto* pLayModelBtns = new QHBoxLayout(pRowModelBtns);
    pLayModelBtns->setContentsMargins(0, 0, 0, 0);
    m_pBtnModelAdd = new QPushButton(QStringLiteral("新增…"), pRowModelBtns);
    m_pBtnModelAdd->setToolTip(QStringLiteral("新增一个自定义模型条目（对话框编辑各项后加入列表）"));
    connect(m_pBtnModelAdd, &QPushButton::clicked, this, [this]() { OnDshModelAdd(); });
    m_pBtnModelCopy = new QPushButton(QStringLiteral("复制…"), pRowModelBtns);
    m_pBtnModelCopy->setToolTip(QStringLiteral(
        "复制选中条目：预填编辑对话框（显示名加“-副本”，可改），确认后追加到列表末尾——"
        "适合在同一网关下快速建多个模型"));
    connect(m_pBtnModelCopy, &QPushButton::clicked, this, [this]() { OnDshModelCopy(); });
    m_pBtnModelEdit = new QPushButton(QStringLiteral("编辑…"), pRowModelBtns);
    m_pBtnModelEdit->setToolTip(QStringLiteral("编辑选中条目（双击条目同效）"));
    connect(m_pBtnModelEdit, &QPushButton::clicked, this, [this]() { OnDshModelEdit(); });
    m_pBtnModelDel = new QPushButton(QStringLiteral("删除"), pRowModelBtns);
    m_pBtnModelDel->setToolTip(QStringLiteral("删除选中条目；删的是「当前」条目时自动取消当前"));
    connect(m_pBtnModelDel, &QPushButton::clicked, this, [this]() { OnDshModelDel(); });
    m_pBtnModelActive = new QPushButton(QStringLiteral("设为当前"), pRowModelBtns);
    m_pBtnModelActive->setToolTip(QStringLiteral(
        "将选中条目设为当前生效模型（再点一次取消，回上方 Provider/模型固定路由）"));
    connect(m_pBtnModelActive, &QPushButton::clicked, this, [this]() { OnDshModelSetActive(); });
    pLayModelBtns->addWidget(m_pBtnModelAdd);
    pLayModelBtns->addWidget(m_pBtnModelCopy);
    pLayModelBtns->addWidget(m_pBtnModelEdit);
    pLayModelBtns->addWidget(m_pBtnModelDel);
    pLayModelBtns->addWidget(m_pBtnModelActive);
    pLayModelBtns->addStretch(1);
    pLayModels->addWidget(pRowModelBtns);
    auto* pLblModelsHint = new QLabel(QStringLiteral(
        "「当前」列打 ● 的条目生效（各后端参数页的 Provider/模型固定路由不参与）。\n"
        "保存后立即生效：空闲会话自动换新配置；进行中的轮次在该轮结束后切换（无需重启程序）。\n"
        "Claude Code 后端勾选下方开关后同样使用本表（仅 Anthropic 兼容端点的条目）。\n"
        "API Key 以明文存于 exe 同级 config.ini，注意文件外发安全。"), pGroupModels);
    pLblModelsHint->setWordWrap(true);
    pLayModels->addWidget(pLblModelsHint);
    pLayTabModels->addWidget(pGroupModels, 1);   // 2026-10-08 组随页伸展（树高度随窗口自适应）
    pLayDsh->addStretch(1);

    // —— PIAgent 参数（2026.08.19 第三后端，仿 DSH Tab 布局）——
    auto* pTabPi = new QWidget(this);
    auto* pLayPi = new QVBoxLayout(pTabPi);
    pLayPi->setContentsMargins(0, 0, 0, 0);
    // 安装状态行：状态标签 + 重新检测 + 单独安装（未装时由此一键安装，
    //   install_env.bat /pi 从本机 PIAgent 源码 robocopy 到 exe 同级 PIAgent\pi）。
    auto* pRowPiState = new QWidget(pTabPi);
    auto* pLayPiState = new QHBoxLayout(pRowPiState);
    pLayPiState->setContentsMargins(0, 0, 0, 0);
    m_pLblPiState = new QLabel(pRowPiState);
    m_pLblPiState->setWordWrap(true);
    m_pLblPiState->setToolTip(QStringLiteral(
        "安装判定：根目录框显式值（或自动探测的 exe 同级 PIAgent）下"
        " pi\\packages\\coding-agent\\dist\\cli.js 存在才认已安装"));
    m_pBtnDetectPi = new QPushButton(QStringLiteral("重新检测"), pRowPiState);
    m_pBtnDetectPi->setToolTip(QStringLiteral(
        "安装完成后点击：重新探测安装状态，命中且根目录为空时自动填入"));
    connect(m_pBtnDetectPi, &QPushButton::clicked, this, [this]() { DetectPiInstalled(false); });
    m_pBtnInstallPi = new QPushButton(QStringLiteral("安装 PIAgent…"), pRowPiState);
    m_pBtnInstallPi->setToolTip(QStringLiteral(
        "以管理员权限运行环境安装脚本（/pi 单装模式）：从本机已构建的 PIAgent 源码"
        "（exe 上级 PIAgent）robocopy 复制到 exe 同级 PIAgent\\pi，完成后回到本页点「重新检测」"));
    connect(m_pBtnInstallPi, &QPushButton::clicked, this, [this]() { OnInstallPi(); });
    // 2026.08.20 联网安装（npm）：无本机源码时的第二安装路径（npm 全局装
    //   @earendil-works/pi-coding-agent，检测自动命中 npm root -g 布局，无需根目录）。
    m_pBtnInstallPiNpm = new QPushButton(QStringLiteral("联网安装（npm）"), pRowPiState);
    m_pBtnInstallPiNpm->setToolTip(QStringLiteral(
        "以管理员权限联网安装：npm install -g @earendil-works/pi-coding-agent"
        "（需本机已装 Node.js 与 npm，且能访问 npm 源）。\n"
        "装完点「重新检测」：cli.js 从 npm 全局目录解析，PIAgent 根目录可留空"));
    connect(m_pBtnInstallPiNpm, &QPushButton::clicked, this, [this]() { OnInstallPiNpm(); });
    pLayPiState->addWidget(m_pLblPiState, 1);
    pLayPiState->addWidget(m_pBtnDetectPi);
    pLayPiState->addWidget(m_pBtnInstallPi);
    pLayPiState->addWidget(m_pBtnInstallPiNpm);
    pLayPi->addWidget(pRowPiState);

    auto* pGroupPi = new QGroupBox(QStringLiteral("PIAgent 参数"), pTabPi);
    auto* pFormPi = new QFormLayout(pGroupPi);
    auto fnAddPiPathRow = [this, pGroupPi, pFormPi](
            const QString& a_sLabel, QLineEdit** a_ppEdit, const QString& a_sTip, bool a_bDir) {
        auto* pRow = new QWidget(pGroupPi);
        auto* pLay = new QHBoxLayout(pRow);
        pLay->setContentsMargins(0, 0, 0, 0);
        *a_ppEdit = new QLineEdit(pRow);
        (*a_ppEdit)->setToolTip(a_sTip);
        auto* pBtnBrowse = new QPushButton(QStringLiteral("浏览…"), pRow);
        connect(pBtnBrowse, &QPushButton::clicked, this, [this, a_ppEdit, a_bDir]() {
            const QString sSel = a_bDir
                ? QFileDialog::getExistingDirectory(this, QString(), (*a_ppEdit)->text())
                : QFileDialog::getOpenFileName(this, QString(), (*a_ppEdit)->text());
            if (!sSel.isEmpty()) {
                (*a_ppEdit)->setText(QDir::toNativeSeparators(sSel));
            }
        });
        pLay->addWidget(*a_ppEdit, 1);
        pLay->addWidget(pBtnBrowse);
        pFormPi->addRow(a_sLabel, pRow);
    };
    fnAddPiPathRow(QStringLiteral("PIAgent 根目录："), &m_pEditPiRoot,
                   QStringLiteral("PIAgent 仓库根目录（其下 pi\\ 为 monorepo，cli.js 按固定布局"
                                  " pi\\packages\\coding-agent\\dist\\cli.js 推导）。留空时启动自动"
                                  "探测 exe 同级 PIAgent 目录（[Agent] PiRoot）"), true);
    // 2026.09 布局瘦身：Provider/模型/思考强度三短项并一行（设置窗高度优化，
    //   减两行；各控件 tooltip 不变）。
    auto* pRowPiPmt = new QWidget(pGroupPi);
    auto* pLayPiPmt = new QHBoxLayout(pRowPiPmt);
    pLayPiPmt->setContentsMargins(0, 0, 0, 0);
    m_pEditPiProvider = new QLineEdit(pRowPiPmt);
    m_pEditPiProvider->setPlaceholderText(QStringLiteral("anthropic"));
    m_pEditPiProvider->setToolTip(QStringLiteral(
        "LLM provider 名（如 anthropic/openai/google/deepseek/openrouter；[Agent] PiProvider，"
        "默认 anthropic）。API key 按该名映射环境变量注入；未覆盖的 provider 走 pi 自身登录"
        "（命令行 pi auth / /login）"));
    m_pComboPiThinking = new QComboBox(pRowPiPmt);
    m_pComboPiThinking->addItem(QString());   // 空 = 默认（不传 --thinking）
    m_pComboPiThinking->addItems(QStringList()
        << QStringLiteral("off") << QStringLiteral("minimal") << QStringLiteral("low")
        << QStringLiteral("medium") << QStringLiteral("high") << QStringLiteral("xhigh")
        << QStringLiteral("max"));
    m_pComboPiThinking->setItemText(0, QStringLiteral("（默认）"));
    m_pComboPiThinking->setToolTip(QStringLiteral(
        "思考强度（--thinking 级别，仅推理模型生效；[Agent] PiThinking，空 = 用模型默认）"));
    pLayPiPmt->addWidget(m_pEditPiProvider, 2);
    pLayPiPmt->addWidget(new QLabel(QStringLiteral("思考强度："), pRowPiPmt));
    pLayPiPmt->addWidget(m_pComboPiThinking, 1);
    pFormPi->addRow(QStringLiteral("Provider："), pRowPiPmt);
    // 2026.08.20 PIAgent 计费单价（$/M token）：pi 协议只报 token 分量不报金额，
    //   费用 = 各分量 × 此单价累计；默认对齐 claude 后端 Sonnet 档价目（同 DSH 口径）。
    auto* pRowPiPrice = new QWidget(pGroupPi);
    auto* pLayPiPrice = new QHBoxLayout(pRowPiPrice);
    pLayPiPrice->setContentsMargins(0, 0, 0, 0);
    const QString sPiPriceTip = QStringLiteral(
        "pi 协议只报 token 分量不报金额，费用 = 各分量 × 此单价累计（单位 $/百万 token）。\n"
        "默认对齐 Claude Code 后端 Sonnet 档价目（同网关同模型可比）；网关实际计费\n"
        "不同则按实际改。四项全 0 = 不折算，费用列显示“-”。[Agent] PiPrice*PerM");
    auto fnPiPriceSpin = [pRowPiPrice, pLayPiPrice, &sPiPriceTip](const QString& a_sLbl) {
        auto* pLbl = new QLabel(a_sLbl, pRowPiPrice);
        auto* pSpin = new QDoubleSpinBox(pRowPiPrice);
        pSpin->setRange(0.0, 1000.0);
        pSpin->setDecimals(2);
        pSpin->setSingleStep(0.05);
        pSpin->setToolTip(sPiPriceTip);
        pLayPiPrice->addWidget(pLbl);
        pLayPiPrice->addWidget(pSpin, 1);
        return pSpin;
    };
    m_pSpinPiPriceIn = fnPiPriceSpin(QStringLiteral("输入"));
    m_pSpinPiPriceCacheRead = fnPiPriceSpin(QStringLiteral("缓存读"));
    m_pSpinPiPriceCacheWrite = fnPiPriceSpin(QStringLiteral("缓存写"));
    m_pSpinPiPriceOut = fnPiPriceSpin(QStringLiteral("输出"));
    pFormPi->addRow(QStringLiteral("计费单价($/M)："), pRowPiPrice);
    pLayPi->addWidget(pGroupPi);

    auto* pLblPiHint = new QLabel(QStringLiteral(
        "PIAgent（pi coding-agent）以 --mode json 流式接入；会话落盘于 ~/.pi/agent/sessions，"
        "跨进程可凭 session id 续跑。\n计费：pi 只上报 token，费用列显示“-”。"
        "API Key 明文存 config.ini，注意文件外发安全。"), pTabPi);
    pLblPiHint->setWordWrap(true);
    pLayPi->addWidget(pLblPiHint);
    pLayPi->addStretch(1);

    // —— CodexHarness 第四后端（OpenAI Codex CLI 0.87.0，仿 Pi Tab 布局）——
    auto* pTabCodex = new QWidget(this);
    auto* pLayCodex = new QVBoxLayout(pTabCodex);
    pLayCodex->setContentsMargins(0, 0, 0, 0);
    auto* pRowCodexState = new QWidget(pTabCodex);
    auto* pLayCodexState = new QHBoxLayout(pRowCodexState);
    pLayCodexState->setContentsMargins(0, 0, 0, 0);
    m_pLblCodexState = new QLabel(pRowCodexState);
    m_pLblCodexState->setWordWrap(true);
    m_pBtnDetectCodex = new QPushButton(QStringLiteral("重新检测"), pRowCodexState);
    connect(m_pBtnDetectCodex, &QPushButton::clicked, this,
            [this]() { DetectCodexInstalled(false); });    m_pBtnInstallCodex = new QPushButton(QStringLiteral("安装 CodexHarness…"), pRowCodexState);
    m_pBtnInstallCodex->setToolTip(QStringLiteral(
        "以管理员权限运行环境安装脚本（/codex 单装模式）：npm 本地安装"
        " @openai/codex@0.87.0 --save-exact 到 exe 同级 Execute\\CodexHarness，"
        "完成后回到本页点「重新检测」"));
    connect(m_pBtnInstallCodex, &QPushButton::clicked, this, [this]() { OnInstallCodex(); });
    // 2026.08.21 本机源离线安装（robocopy）：npm 联网慢/被墙时的第二路径（口径同
    //   Pi 的本机源安装）：从本机已有 CodexHarness 仓库直接复制到 exe 同级
    //   Execute\CodexHarness，无需管理员权限、不依赖网络。
    m_pBtnInstallCodexLocal = new QPushButton(QStringLiteral("本机源安装（离线）"), pRowCodexState);
    m_pBtnInstallCodexLocal->setToolTip(QStringLiteral(
        "从本机已有的 CodexHarness 安装目录（含 node_modules\\@openai\\codex）robocopy"
        " 复制到 exe 同级 Execute\\CodexHarness。\n无需网络与管理员权限；npm 联网安装"
        "缓慢或卡死时优先用此路径。完成后自动重新检测"));
    connect(m_pBtnInstallCodexLocal, &QPushButton::clicked, this,
            [this]() { OnInstallCodexLocal(); });
    pLayCodexState->addWidget(m_pLblCodexState, 1);
    pLayCodexState->addWidget(m_pBtnDetectCodex);
    pLayCodexState->addWidget(m_pBtnInstallCodex);
    pLayCodexState->addWidget(m_pBtnInstallCodexLocal);
    pLayCodex->addWidget(pRowCodexState);

    auto* pGroupCodex = new QGroupBox(QStringLiteral("CodexHarness 参数"), pTabCodex);
    auto* pFormCodex = new QFormLayout(pGroupCodex);
    auto fnAddCodexPathRow = [this, pGroupCodex, pFormCodex](
            const QString& a_sLabel, QLineEdit** a_ppEdit, const QString& a_sTip, bool a_bDir) {
        auto* pRow = new QWidget(pGroupCodex);
        auto* pLay = new QHBoxLayout(pRow);
        pLay->setContentsMargins(0, 0, 0, 0);
        *a_ppEdit = new QLineEdit(pRow);
        (*a_ppEdit)->setToolTip(a_sTip);
        auto* pBtnBrowse = new QPushButton(QStringLiteral("浏览…"), pRow);
        connect(pBtnBrowse, &QPushButton::clicked, this, [this, a_ppEdit, a_bDir]() {
            const QString sSel = a_bDir
                ? QFileDialog::getExistingDirectory(this, QString(), (*a_ppEdit)->text())
                : QFileDialog::getOpenFileName(this, QString(), (*a_ppEdit)->text());
            if (!sSel.isEmpty()) {
                (*a_ppEdit)->setText(QDir::toNativeSeparators(sSel));
            }
        });
        pLay->addWidget(*a_ppEdit, 1);
        pLay->addWidget(pBtnBrowse);
        pFormCodex->addRow(a_sLabel, pRow);
    };
    fnAddCodexPathRow(QStringLiteral("根目录："), &m_pEditCodexRoot,
                   QStringLiteral("npm 工程根（其下 node_modules\\@openai\\codex）。留空时"
                                  "自动探测 exe 同级 Execute\\CodexHarness（[Agent] CodexRoot）。"
                                  "填本机已有安装（如全局 npm 目录/开发目录）亦可，输入即自动"
                                  "重检，无需再点「重新检测」"),
                   true);
    // 2026.08.21 根目录输入即时重检（静默）：检测通过后端下拉的 CodexHarness 项
    //   立即解锁可选——修「填了路径仍无法把 Agent 后端设为 Codex」。
    //   注意不设 isModified 守卫：浏览按钮/粘贴均为程序化 setText，之前被守卫
    //   吞掉导致「浏览选目录后仍不重检」；程序性回填由 DetectCodexInstalled 内
    //   blockSignals 防重入。
    connect(m_pEditCodexRoot, &QLineEdit::textChanged, this, [this]() {
        DetectCodexInstalled(true);
    });
    m_pEditCodexProvider = new QLineEdit(pGroupCodex);
    m_pEditCodexProvider->setPlaceholderText(QStringLiteral("autocode"));
    m_pEditCodexProvider->setToolTip(QStringLiteral(
        "provider id（写入 ~/.codex/config.toml 的 [model_providers.<provider>] 槽位；"
        "[Agent] CodexProvider，空 = autocode）。不与用户手写 provider 冲突时可用任意名"));
    m_pComboCodexWireApi = new QComboBox(pGroupCodex);
    m_pComboCodexWireApi->addItem(QStringLiteral("chat"));
    m_pComboCodexWireApi->addItem(QStringLiteral("responses"));
    m_pComboCodexWireApi->setToolTip(QStringLiteral(
        "wire_api（写入 ~/.codex 的 provider 槽位；[Agent] CodexWireApi，默认 chat）。"
        "v0.88+ 已移除 chat，本后端锁定 0.87.0"));
    // 2026-10-08 思考强度（官方键 model_reasoning_effort；[Agent] CodexEffort，空=默认）。
    m_pComboCodexEffort = new QComboBox(pGroupCodex);
    m_pComboCodexEffort->addItem(QString());   // 空 = 默认
    m_pComboCodexEffort->addItems(QStringList()
        << QStringLiteral("minimal") << QStringLiteral("low") << QStringLiteral("medium")
        << QStringLiteral("high") << QStringLiteral("xhigh"));
    m_pComboCodexEffort->setItemText(0, QStringLiteral("（默认）"));
    m_pComboCodexEffort->setToolTip(QStringLiteral(
        "思考强度（-c model_reasoning_effort；[Agent] CodexEffort，空 = 用 codex 默认）。\n"
        "仅推理模型生效，下一轮对话/新任务即用"));
    // 2026-10-08 Provider/WireApi/思考强度三项合一行（均短，压缩纵向占用）。
    auto* pRowCodexPmt = new QWidget(pGroupCodex);
    auto* pLayCodexPmt = new QHBoxLayout(pRowCodexPmt);
    pLayCodexPmt->setContentsMargins(0, 0, 0, 0);
    pLayCodexPmt->addWidget(m_pEditCodexProvider, 2);
    pLayCodexPmt->addSpacing(12);
    pLayCodexPmt->addWidget(new QLabel(QStringLiteral("WireApi："), pRowCodexPmt));
    pLayCodexPmt->addWidget(m_pComboCodexWireApi, 1);
    pLayCodexPmt->addSpacing(12);
    pLayCodexPmt->addWidget(new QLabel(QStringLiteral("思考强度："), pRowCodexPmt));
    pLayCodexPmt->addWidget(m_pComboCodexEffort, 1);
    pFormCodex->addRow(QStringLiteral("Provider："), pRowCodexPmt);
    pLayCodex->addWidget(pGroupCodex);

    auto* pLblCodexHint = new QLabel(QStringLiteral(
        "CodexHarness（OpenAI Codex CLI 0.87.0）以 exec --json 流式接入；"
        "保存时写入 ~/.codex（config.toml 行级手术保留用户手写内容 + auth.json）。"
        "计费：只上报 token，费用列显示“-”。API Key 明文存 config.ini，注意外发安全。"),
        pTabCodex);
    pLblCodexHint->setWordWrap(true);
    pLayCodex->addWidget(pLblCodexHint);
    pLayCodex->addStretch(1);

    auto* pTabs = new QTabWidget(this);
    // —— Hermes 第五后端 Tab（任务 3213cdbf，仿 Pi Tab 布局）——
    //   2026.09 补安装入口：本机源离线安装（robocopy，口径同 Codex 本机源——
    //   Hermes 无 npm 包/远端发布源，装更一体走本机源重部署，无需管理员/网络）。
    auto* pTabHermes = new QWidget(this);
    auto* pLayHermes = new QVBoxLayout(pTabHermes);
    pLayHermes->setContentsMargins(0, 0, 0, 0);
    auto* pRowHermesState = new QWidget(pTabHermes);
    auto* pLayHermesState = new QHBoxLayout(pRowHermesState);
    pLayHermesState->setContentsMargins(0, 0, 0, 0);
    m_pLblHermesState = new QLabel(pRowHermesState);
    m_pLblHermesState->setWordWrap(true);
    m_pBtnDetectHermes = new QPushButton(QStringLiteral("重新检测"), pRowHermesState);
    m_pBtnDetectHermes->setToolTip(QStringLiteral(
        "重新探测 Hermes CLI（探测序：根目录 → exe 同级 HermesHarness\\dist\\cli.js → PATH）"));
    connect(m_pBtnDetectHermes, &QPushButton::clicked, this,
            [this]() { DetectHermesInstalled(false); });
    m_pBtnInstallHermesLocal = new QPushButton(QStringLiteral("本机源安装（离线）"),
                                               pRowHermesState);
    m_pBtnInstallHermesLocal->setToolTip(QStringLiteral(
        "从本机已有的 Hermes Harness 源目录（其下 dist\\cli.js）robocopy 复制到"
        " exe 同级 HermesHarness。\n无需网络与管理员权限；已装时重跑 = 用源目录"
        "覆盖更新。\n完成后自动重新检测"));
    connect(m_pBtnInstallHermesLocal, &QPushButton::clicked, this,
            [this]() { OnInstallHermesLocal(); });
    // 2026.09 联网安装（git）：提权运行 install_env.bat /hermes <仓库地址>（口径同
    //   DSH 的「安装 DeepSeekHarness」：git 浅克隆 + pnpm 依赖构建，已装时为
    //   fetch+reset 更新）。仓库地址点击按钮时输入框现场给定（不设配置项）。
    m_pBtnInstallHermesNet = new QPushButton(QStringLiteral("联网安装（git）"),
                                             pRowHermesState);
    m_pBtnInstallHermesNet->setToolTip(QStringLiteral(
        "以管理员权限联网安装：点击后输入 Hermes Harness 的 git 仓库地址（或本机"
        " 仓库路径），git 克隆到 exe 同级 HermesHarness，并 pnpm 安装依赖 + 构建"
        " dist\\cli.js。\n"
        "已装时重跑 = git 拉取更新（fetch+reset，保留本地文件）。\n"
        "需 Node.js 与 Git（缺失时脚本会自动安装）；完成后回到本页点「重新检测」"));
    connect(m_pBtnInstallHermesNet, &QPushButton::clicked, this,
            [this]() { OnInstallHermesNet(); });
    pLayHermesState->addWidget(m_pLblHermesState, 1);
    pLayHermesState->addWidget(m_pBtnDetectHermes);
    pLayHermesState->addWidget(m_pBtnInstallHermesLocal);
    pLayHermesState->addWidget(m_pBtnInstallHermesNet);
    pLayHermes->addWidget(pRowHermesState);
    auto* pGroupHermes = new QGroupBox(QStringLiteral("Hermes 参数"), pTabHermes);
    auto* pFormHermes = new QFormLayout(pGroupHermes);
    auto fnAddHermesPathRow = [this, pGroupHermes, pFormHermes](
            const QString& a_sLabel, QLineEdit** a_ppEdit, const QString& a_sTip, bool a_bDir) {
        auto* pRow = new QWidget(pGroupHermes);
        auto* pLay = new QHBoxLayout(pRow);
        pLay->setContentsMargins(0, 0, 0, 0);
        *a_ppEdit = new QLineEdit(pRow);
        (*a_ppEdit)->setToolTip(a_sTip);
        auto* pBtnBrowse = new QPushButton(QStringLiteral("浏览…"), pRow);
        connect(pBtnBrowse, &QPushButton::clicked, this, [this, a_ppEdit, a_bDir]() {
            const QString sSel = a_bDir
                ? QFileDialog::getExistingDirectory(this, QString(), (*a_ppEdit)->text())
                : QFileDialog::getOpenFileName(this, QString(), (*a_ppEdit)->text());
            if (!sSel.isEmpty()) {
                (*a_ppEdit)->setText(QDir::toNativeSeparators(sSel));
            }
        });
        pLay->addWidget(*a_ppEdit, 1);
        pLay->addWidget(pBtnBrowse);
        pFormHermes->addRow(a_sLabel, pRow);
    };
    fnAddHermesPathRow(QStringLiteral("Hermes 根目录："), &m_pEditHermesRoot,
                       QStringLiteral("Hermes Harness 根目录（其下 dist\\cli.js 为入口）。"
                                      "留空时自动探测 exe 同级 HermesHarness"
                                      "（[Agent] HermesRoot）"), true);
    // 2026.09 联网安装（git）：Hermes 无 npm 包/固定远端发布源，仓库地址不设
    //   持久配置项（口径对齐 DSH/Pi 等其他后端设置页均无此项）——点击
    //   「联网安装（git）」时经 QInputDialog 现场输入（git URL 或本机仓库路径）。
    // 计费单价（$/M token，口径同 Pi/Codex）。
    auto* pRowHermesPrice = new QWidget(pGroupHermes);
    auto* pLayHermesPrice = new QHBoxLayout(pRowHermesPrice);
    pLayHermesPrice->setContentsMargins(0, 0, 0, 0);
    const QString sHermesPriceTip = QStringLiteral(
        "Hermes 只报 token 分量，费用 = 各分量 × 此单价累计（$/百万 token）。\n"
        "四项全 0 = 不折算（费用列显示“-”）。[Agent] HermesPrice*PerM");
    auto fnHermesPriceSpin = [pRowHermesPrice, pLayHermesPrice,
                              &sHermesPriceTip](const QString& a_sLbl) {
        auto* pLbl = new QLabel(a_sLbl, pRowHermesPrice);
        auto* pSpin = new QDoubleSpinBox(pRowHermesPrice);
        pSpin->setRange(0.0, 1000.0);
        pSpin->setDecimals(2);
        pSpin->setSingleStep(0.05);
        pSpin->setToolTip(sHermesPriceTip);
        pLayHermesPrice->addWidget(pLbl);
        pLayHermesPrice->addWidget(pSpin, 1);
        return pSpin;
    };
    m_pSpinHermesPriceIn = fnHermesPriceSpin(QStringLiteral("输入"));
    m_pSpinHermesPriceCacheRead = fnHermesPriceSpin(QStringLiteral("缓存读"));
    m_pSpinHermesPriceCacheWrite = fnHermesPriceSpin(QStringLiteral("缓存写"));
    m_pSpinHermesPriceOut = fnHermesPriceSpin(QStringLiteral("输出"));
    pFormHermes->addRow(QStringLiteral("计费单价($/M)："), pRowHermesPrice);
    // 2026-10-08 思考强度（config.yaml agent.reasoning_effort；[Agent] HermesEffort，空=不托管）。
    m_pComboHermesEffort = new QComboBox(pGroupHermes);
    m_pComboHermesEffort->addItem(QString());   // 空 = 默认
    m_pComboHermesEffort->addItems(QStringList()
        << QStringLiteral("false") << QStringLiteral("minimal") << QStringLiteral("low")
        << QStringLiteral("medium") << QStringLiteral("high") << QStringLiteral("ultra"));
    m_pComboHermesEffort->setItemText(0, QStringLiteral("（默认）"));
    m_pComboHermesEffort->setToolTip(QStringLiteral(
        "思考强度（部署根 config.yaml agent.reasoning_effort；[Agent] HermesEffort，\n"
        "空 = 不托管该键用 hermes 默认；false = 关推理。仅推理模型生效"));
    pFormHermes->addRow(QStringLiteral("思考强度："), m_pComboHermesEffort);
    pLayHermes->addWidget(pGroupHermes);
    // 2026-10-08 后端提示（原标签定义在已删除的模型组块内，就地重建）。
    auto* pLblHermesHint = new QLabel(QStringLiteral(
        "Hermes（hermes-agent）以 JSONL 流式接入；会话落盘于 HERMES_HOME，"
        "跨进程可凭 session id 续跑。"
        "模型/密钥经「模型」页共享表配置"
        "（跟随当前条目），API Key 明文存 config.ini，注意文件外发安全。"), pTabHermes);
    pLblHermesHint->setWordWrap(true);
    pLayHermes->addWidget(pLblHermesHint);
    pLayHermes->addStretch(1);

    pTabs->addTab(pTabCommon, QStringLiteral("常用"));
    pTabs->addTab(pTabModels, QStringLiteral("模型"));   // 2026-10-08 页名「大模型」→「模型」（用户需求）
    pTabs->addTab(pTabClaude, QStringLiteral("Claude Code"));
    pTabs->addTab(pTabDsh, QStringLiteral("DeepSeekHarness参数设置"));
    pTabs->addTab(pTabPi, QStringLiteral("PiAgent参数设置"));
    pTabs->addTab(pTabCodex, QStringLiteral("CodexHarness参数设置"));
    pTabs->addTab(pTabHermes, QStringLiteral("Hermes参数设置"));

    auto* pLayoutMain = new QVBoxLayout(this);
    pLayoutMain->addWidget(pTabs, 1);
    pLayoutMain->addWidget(pBtns);
}

void CSettingsDialog::UpdateDshEnabled() {
    // 后端下拉联动 × 安装状态：选 DeepSeekHarness 且已安装才启用参数子组
    //   （控件在 SetupUi 全部建好后调用）。未安装时后端下拉的 DSH 项一并禁用，
    //   防止选了跑不起来的后端（当前已选中则弹回 claude）。
    const bool bDsh = (m_pComboBackend != nullptr && m_pComboBackend->currentData() == 2)
        && m_bDshInstalled;
    // 2026.08.18 自定义大模型激活时，Provider/模型 不参与路由（组合改走 autocode-active
    //   槽位）→ 置灰防误导；取消当前 / 删光条目后自动恢复（增删改与设为当前都会经
    //   UpdateDshEnabled 重算）。
    const bool bCustomActive = m_iDshActiveModel >= 0 && m_iDshActiveModel < m_aDshModels.size();
    if (m_pEditDshNode != nullptr)     { m_pEditDshNode->setEnabled(true); }   // 2026.08.21 始终可编辑（同 Codex 死锁修复）
    if (m_pEditDshRoot != nullptr)     { m_pEditDshRoot->setEnabled(true); }
    if (m_pEditDshProvider != nullptr) { m_pEditDshProvider->setEnabled(bDsh && !bCustomActive); }
    if (m_pSpinDshPriceIn != nullptr)        { m_pSpinDshPriceIn->setEnabled(bDsh); }
    if (m_pSpinDshPriceCacheRead != nullptr) { m_pSpinDshPriceCacheRead->setEnabled(bDsh); }
    if (m_pSpinDshPriceCacheWrite != nullptr) { m_pSpinDshPriceCacheWrite->setEnabled(bDsh); }
    if (m_pSpinDshPriceOut != nullptr)       { m_pSpinDshPriceOut->setEnabled(bDsh); }
    // 2026.09.09 v2.1.138 自定义大模型列表不再要求选中本后端（用户需求：未选为当前
    //   后端也要能编辑条目）：模型表是纯 config 数据（[DshModels]），不依赖后端选中/
    //   安装状态，可提前预配；单价/Provider 等运行参数仍随选中联动不变。按钮组由
    //   UpdateDshModelButtons 经树 isEnabled() 派生，树恒启用后随选中行联动。
    if (m_pTreeDshModels != nullptr)   { m_pTreeDshModels->setEnabled(true); }
    // 2026.08.18 后端专属项反向联动：DSH/PIgent 后端下这四项对运行无效果——claude 命令/
    //   提问等待超时只进 CClaudeRunner（DSH/Pi 用 node 启动、无 ask_user MCP 工具），
    //   联网检索开关与自动压缩比只折算进 claude 专属启动参数（两后端侧忽略并降级提示）。
    //   选中即禁用防误导，切回 claude 自动恢复。（2026.09.27 后端中立的「上下文窗口
    //   上限」设置项已删，入口收敛到上下文进度条右键，此处不再涉及。）
    const int iCurBackend = m_pComboBackend != nullptr ? m_pComboBackend->currentData().toInt() : 1;
    const bool bClaudeOnly = (iCurBackend == 1);
    if (m_pEditClaudeCmd != nullptr)  { m_pEditClaudeCmd->setEnabled(bClaudeOnly); }
    if (m_pSpinAskTimeout != nullptr) { m_pSpinAskTimeout->setEnabled(bClaudeOnly); }
    if (m_pChkWebSearch != nullptr)   { m_pChkWebSearch->setEnabled(bClaudeOnly); }
    if (m_pSpinCompactPct != nullptr) { m_pSpinCompactPct->setEnabled(bClaudeOnly); }
    if (m_pComboBackend != nullptr && m_pComboBackend->count() > 1) {
        // 2026.08.18 修「DSH 项恒不可选」：QComboBox 的 UserRole-1 内部角色收的是
        //   Qt::ItemFlags 掩码——旧代码传 bool，true=1 只含 Selectable 缺 Enabled(0x20)，
        //   false=0 两位全无，检测结果再好 DSH 也永远禁用。改走 QStandardItem::setFlags
        //   正道（默认模型即 QStandardItemModel），非标准模型才用掩码值兜底。
        // 2026.08.19 同口径扩展 PIAgent 项（index 2，按 m_bPiInstalled 联动）。
        auto fnSetItemFlags = [this](int a_iRow, bool a_bEnabled) {
            const Qt::ItemFlags eFlags = a_bEnabled
                ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled) : Qt::NoItemFlags;
            auto* pStdModel = qobject_cast<QStandardItemModel*>(m_pComboBackend->model());
            if (pStdModel != nullptr) {
                if (QStandardItem* pItem = pStdModel->item(a_iRow); pItem != nullptr) {
                    pItem->setFlags(eFlags);
                }
            } else {
                m_pComboBackend->model()->setData(m_pComboBackend->model()->index(a_iRow, 0),
                                                  static_cast<int>(eFlags), Qt::UserRole - 1);
            }
        };
        fnSetItemFlags(1, m_bDshInstalled);   // DeepSeekHarness
        fnSetItemFlags(2, m_bPiInstalled);    // PIAgent
        if (!m_bDshInstalled && m_pComboBackend->currentData() == 2
            && m_pComboBackend->currentData().toInt() != m_iCfgBackend) {
            m_pComboBackend->setCurrentIndex(0);   // 主动新切到不可用 DSH 才弹回；已存配置保留（2026.08.21）
        }
    }
    // 列表与按钮态随组级开关 / 选中行 / 当前标记刷新（内部 blockSignals 重建，无重入）。
    RefreshDshModelList();
}

// 2026.08.18 自定义大模型列表刷新：重建多列条目行（当前列 ● 标记），空表给占位行；
//   末尾按选中行与组级开关更新按钮态（选中态变化 / 增删改 / 设为当前后都走这里）。
//   重建全程 blockSignals——currentItemChanged 也接到本函数，靠屏蔽防环。
// 2026.09.10 公共模型配置：四张模型表合并归一——四个成员变同一份共享表
//   （列表 + 当前项两两相等），四页展示/编辑的是同一份数据。合并规则见
//   CModelEntry.h MergeSharedModelTables（基底 = 最多条目的表，其余表新身份追加；
//   传参优先级 Pi > DSH > Hermes > Codex——并列时 Pi 表定基底序）。
void CSettingsDialog::UnifySharedModels() {
    const QList<QList<SDshModelEntry>> aTables = {
        m_aPiModels, m_aDshModels, m_aHermesModels, m_aCodexModels };
    const QList<int> aActives = {
        m_iPiActiveModel, m_iDshActiveModel, m_iHermesActiveModel, m_iCodexActiveModel };
    // 共享当前项优先取当前选中后端自己表的激活条目（磁盘四表收敛后各表 Active
    //   本就相同，该优先级只在未收敛期起作用——保留用户在当前后端已选的模型）。
    int iPrefTable = -1;
    const int iCurBackend = (m_pComboBackend != nullptr)
        ? m_pComboBackend->currentData().toInt() : 1;
    if (iCurBackend == 3) {
        iPrefTable = 0;         // Pi
    } else if (iCurBackend == 2) {
        iPrefTable = 1;         // DSH
    } else if (iCurBackend == 5) {
        iPrefTable = 2;         // Hermes
    } else if (iCurBackend == 4) {
        iPrefTable = 3;         // Codex
    }
    const SSharedModels stM = MergeSharedModelTables(aTables, aActives, iPrefTable);
    m_aPiModels = stM.aModels;
    m_aDshModels = stM.aModels;
    m_aHermesModels = stM.aModels;
    m_aCodexModels = stM.aModels;
    m_iPiActiveModel = stM.iActive;
    m_iDshActiveModel = stM.iActive;
    m_iHermesActiveModel = stM.iActive;
    m_iCodexActiveModel = stM.iActive;
    RefreshDshModelList();   // 2026-10-08 起唯一模型树（独立「大模型」页）
}

// 2026.09.10 公共模型配置：任一页的模型改动（增/改/删/复制/设为当前）落地后，
//   把该页的列表与当前项同步到其余三页并刷新——四页始终是同一份数据，
//   保存时主窗口把同一份内容写回四张节，切换后端即用同一份列表/当前项。
//   （不能反向调 UnifySharedModels：其余三页还是改动前的旧副本，合并会把
//   刚删掉的条目从旧副本里补回来。）
void CSettingsDialog::ShareModelsFromPage(int a_iPage) {
    QList<SDshModelEntry>* apLists[4] = {
        &m_aDshModels, &m_aPiModels, &m_aCodexModels, &m_aHermesModels };
    int* apiActives[4] = {
        &m_iDshActiveModel, &m_iPiActiveModel, &m_iCodexActiveModel, &m_iHermesActiveModel };
    if (a_iPage < 0 || a_iPage >= 4) {
        return;
    }
    const QList<SDshModelEntry> aSrc = *apLists[a_iPage];
    const int iSrcActive = *apiActives[a_iPage];
    for (int i = 0; i < 4; ++i) {
        if (i == a_iPage) {
            continue;
        }
        *apLists[i] = aSrc;
        *apiActives[i] = iSrcActive;
    }
    // 只刷新其余三页（本页刷新由各自 handler 的 UpdateXxxEnabled 收尾）。
    RefreshDshModelList();   // 2026-10-08 唯一模型树（四页副本仍同步，供保存链路）
}

void CSettingsDialog::RefreshDshModelList() {
    if (m_pTreeDshModels == nullptr) {
        return;
    }
    const int iSel = CurDshModelRow();
    m_pTreeDshModels->blockSignals(true);
    m_pTreeDshModels->clear();
    for (int i = 0; i < m_aDshModels.size(); ++i) {
        const SDshModelEntry& stE = m_aDshModels.at(i);
        auto* pItem = new QTreeWidgetItem(QStringList()
            << stE.sDisp
            << stE.sBaseURL
            << stE.sApi
            << stE.sModelId
            << QString::number(stE.iCtx)
            << QString::number(stE.iMaxTok)
            << (i == m_iDshActiveModel ? QStringLiteral("●") : QString()));
        // Key 是敏感信息不进列：tooltip 只报「已配置/未配置」。
        pItem->setToolTip(0, QStringLiteral("双击编辑；API Key：%1").arg(
            stE.sApiKey.isEmpty() ? QStringLiteral("未配置")
                                  : QStringLiteral("已配置（明文存 config.ini）")));
        pItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(6, Qt::AlignCenter);
        m_pTreeDshModels->addTopLevelItem(pItem);
    }
    if (m_aDshModels.isEmpty()) {
        auto* pItem = new QTreeWidgetItem(QStringList{
            QStringLiteral("（空：使用上方 Provider 固定路由）")});
        pItem->setDisabled(true);
        m_pTreeDshModels->addTopLevelItem(pItem);
    }
    if (iSel >= 0 && iSel < m_pTreeDshModels->topLevelItemCount()) {
        m_pTreeDshModels->setCurrentItem(m_pTreeDshModels->topLevelItem(iSel));
    }
    m_pTreeDshModels->blockSignals(false);
    const int iRow = CurDshModelRow();
    const bool bHasSel = (iRow >= 0 && iRow < m_aDshModels.size());
    const bool bEnabled = m_pTreeDshModels->isEnabled();   // 组级开关（UpdateDshEnabled 已设）
    if (m_pBtnModelAdd != nullptr)  { m_pBtnModelAdd->setEnabled(bEnabled); }
    if (m_pBtnModelCopy != nullptr) { m_pBtnModelCopy->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnModelEdit != nullptr) { m_pBtnModelEdit->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnModelDel != nullptr)  { m_pBtnModelDel->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnModelActive != nullptr) {
        m_pBtnModelActive->setEnabled(bEnabled && bHasSel);
        m_pBtnModelActive->setText((bHasSel && iRow == m_iDshActiveModel)
            ? QStringLiteral("取消当前") : QStringLiteral("设为当前"));
    }
}

// 2026.08.19 QTreeWidget 当前行号（-1=无选中/占位行；供增改删/设为当前共用）。
int CSettingsDialog::CurDshModelRow() const {
    if (m_pTreeDshModels == nullptr) {
        return -1;
    }
    return m_pTreeDshModels->indexOfTopLevelItem(m_pTreeDshModels->currentItem());
}

void CSettingsDialog::OnDshModelAdd() {
    CDshModelEditDialog dlg(this, nullptr);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aDshModels.append(dlg.Entry());
    if (m_aDshModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iDshActiveModel = 0;
    }
    ShareModelsFromPage(0);   // 2026.09.10 公共模型配置：同步其余三页
    RefreshDshModelList();   // 先重建（新行落位），再选中末行
    if (m_pTreeDshModels->topLevelItemCount() > 0) {
        m_pTreeDshModels->setCurrentItem(
            m_pTreeDshModels->topLevelItem(m_pTreeDshModels->topLevelItemCount() - 1));
    }
    UpdateDshEnabled();   // 按钮态 +（当前条目变化时的）Provider/模型置灰联动
}

// 2026.08.27 复制选中条目：预填编辑对话框（显示名加“-副本”可改），确认后追加末尾。
void CSettingsDialog::OnDshModelCopy() {
    const int iRow = CurDshModelRow();
    if (iRow < 0 || iRow >= m_aDshModels.size()) {
        return;   // 空表占位行 / 无选中
    }
    SDshModelEntry stCopy = m_aDshModels.at(iRow);
    stCopy.sDisp += QStringLiteral("-副本");
    CDshModelEditDialog dlg(this, &stCopy, false, false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aDshModels.append(dlg.Entry());
    if (m_aDshModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iDshActiveModel = 0;
    }
    ShareModelsFromPage(0);   // 2026.09.10 公共模型配置：同步其余三页
    RefreshDshModelList();
    if (m_pTreeDshModels->topLevelItemCount() > 0) {
        m_pTreeDshModels->setCurrentItem(
            m_pTreeDshModels->topLevelItem(m_pTreeDshModels->topLevelItemCount() - 1));
    }
    UpdateDshEnabled();
}

void CSettingsDialog::OnDshModelEdit() {
    const int iRow = CurDshModelRow();
    if (iRow < 0 || iRow >= m_aDshModels.size()) {
        return;   // 空表占位行 / 无选中
    }
    CDshModelEditDialog dlg(this, &m_aDshModels[iRow]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aDshModels[iRow] = dlg.Entry();
    ShareModelsFromPage(0);   // 2026.09.10 公共模型配置：同步其余三页
    UpdateDshEnabled();
}

void CSettingsDialog::OnDshModelDel() {
    const int iRow = CurDshModelRow();
    if (iRow < 0 || iRow >= m_aDshModels.size()) {
        return;
    }
    const SDshModelEntry& stE = m_aDshModels.at(iRow);
    if (QMessageBox::question(this, QStringLiteral("自定义模型"),
            QStringLiteral("删除条目「%1」？").arg(stE.sDisp)) != QMessageBox::Yes) {
        return;
    }
    m_aDshModels.removeAt(iRow);
    if (m_iDshActiveModel == iRow) {
        m_iDshActiveModel = -1;          // 删的正是「当前」→ 取消当前
    } else if (m_iDshActiveModel > iRow) {
        --m_iDshActiveModel;             // 其后条目下标前移
    }
    if (m_aDshModels.size() == 1) {      // 仅剩一条 → 自动设为缺省
        m_iDshActiveModel = 0;
    }
    ShareModelsFromPage(0);   // 2026.09.10 公共模型配置：同步其余三页
    UpdateDshEnabled();
}

void CSettingsDialog::OnDshModelSetActive() {
    const int iRow = CurDshModelRow();
    if (iRow < 0 || iRow >= m_aDshModels.size()) {
        return;
    }
    m_iDshActiveModel = (m_iDshActiveModel == iRow) ? -1 : iRow;   // 再点一次取消
    ShareModelsFromPage(0);   // 2026.09.10 公共模型配置：同步其余三页
    UpdateDshEnabled();
}

void CSettingsDialog::DetectDshInstalled(bool a_bSilent) {
    // 根目录框显式值优先；空或失效（键指向的目录没有 bin.js/cordis.yml —— 典型：
    //   config.ini 随发布包带到别的机器，显式键还是装机机的绝对路径）时回退探测
    //   exe 同级/上级固定布局，命中并填回输入框（确定后由主窗口落盘，实现「安装
    //   完成 → 自动回写 config.ini」+「搬机后死键自愈」两个闭环）。
    QString sRoot = m_pEditDshRoot->text().trimmed();
    if (!CDeepSeekRunner::LooksLikeDshRoot(sRoot)) {
        const QString sProbe = CDeepSeekRunner::ProbeSiblingRoot();
        if (CDeepSeekRunner::LooksLikeDshRoot(sProbe)) {
            sRoot = sProbe;
            m_pEditDshRoot->setText(QDir::toNativeSeparators(sRoot));
        }
    }
    m_bDshInstalled = !sRoot.isEmpty() && CDeepSeekRunner::LooksLikeDshRoot(sRoot);
    if (m_pLblDshState != nullptr) {
        if (m_bDshInstalled) {
            // FR-1.3：状态行在「安装状态+路径」基础上追加版本（T1 现成 API，
            //   读不出→版本未知；空→不回退，口径同对话框「未知」）。
            const QString sVer = CDeepSeekRunner::ResolveDshVersion(sRoot);
            m_pLblDshState->setText(QStringLiteral("已安装：")
                + QDir::toNativeSeparators(sRoot)
                + (sVer.isEmpty() ? QStringLiteral("，版本未知")
                                  : QStringLiteral("，版本 %1").arg(sVer)));
            m_pLblDshState->setStyleSheet(QStringLiteral("color: #1a7f37;"));
        } else {
            m_pLblDshState->setText(QStringLiteral(
                "未安装（可选组件；点右侧「安装 DeepSeekHarness…」一键安装）"));
            m_pLblDshState->setStyleSheet(QStringLiteral("color: #c03030;"));
        }
    }
    if (m_pBtnInstallDsh != nullptr) {
        m_pBtnInstallDsh->setEnabled(!m_bDshInstalled);
    }
    UpdateDshEnabled();
    if (!a_bSilent) {
        QMessageBox::information(this, QStringLiteral("DeepSeekHarness"),
            m_bDshInstalled
                ? QStringLiteral("检测到 DeepSeekHarness 已安装：\n%1\n\n"
                                 "「确定」保存设置后即可选用该后端。")
                      .arg(QDir::toNativeSeparators(sRoot))
                // 2026.08.18 失败提示带诊断：装完了却检测不到时，直接指出半安装缺项
                //   （bin.js=克隆/构建未完成；cordis.yml=程序目录缺 dsh\ 正本）与处理
                //   指引，不再让用户对着泛泛的「未检测到」无从下手。
                : QStringLiteral("未检测到可用的 DeepSeekHarness 安装。\n\n%1\n\n"
                                 "处理后点「重新检测」；也可把「DSH 根目录」手动填为 "
                                 "deepseek-harness 仓库根再试。")
                      .arg(CDeepSeekRunner::DshInstallDiagnosis()));
    }
}

void CSettingsDialog::OnInstallDsh() {
    // 提权启动 exe 同级 install_env.bat 的 /dsh 单装模式（克隆 + pnpm 构建 + 部署
    //   组合文件到固定布局）。cmd /k 保持窗口：失败时输出可查。
    const QString sScript = QCoreApplication::applicationDirPath()
        + QStringLiteral("/install_env.bat");
    if (!QFile::exists(sScript)) {
        QMessageBox::warning(this, QStringLiteral("DeepSeekHarness"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整（安装脚本随"
                           "构建同步到 exe 同级）。").arg(QDir::toNativeSeparators(sScript)));
        return;
    }
    const QString sArgs = QStringLiteral("/k \"")
        + QDir::toNativeSeparators(sScript) + QStringLiteral("\" /dsh");
    const HINSTANCE hInst = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                                           sArgs.toStdWString().c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {   // <=32 = 提权被拒/失败
        QMessageBox::warning(this, QStringLiteral("DeepSeekHarness"),
            QStringLiteral("未能以管理员权限启动安装脚本（UAC 被取消或被拒）。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("DeepSeekHarness"),
        QStringLiteral("已在管理员窗口中启动安装，请在该窗口查看进度。\n\n"
                       "窗口出现「[OK] DeepSeekHarness 安装完成」才算装好（出现 [错误] "
                       "按提示处理后重跑，可续装）；完成后回到本页点「重新检测」，"
                       "检测通过即可选用该后端（根目录自动填入，无需手填）。"));
}

void CSettingsDialog::OnVersionManage() {
    // 独立对话框（D1=B）：本地版本直读 + 联网查询 + 更新分派；D6 判据由主窗
    //   构造时注入透传（空=放行）。
    CVersionDialog stDlg(m_fnBackendBusy, this);
    stDlg.exec();
}

void CSettingsDialog::UpdatePiEnabled() {
    // 2026.08.19 PIAgent 后端联动（口径同 UpdateDshEnabled）：四项 claude 专属项已在
    //   UpdateDshEnabled 统一按 currentData==1 联动，这里只负责 PIAgent 组合项的可用性
    //   与弹回（探测通过/根目录手动有效才可选，否则弹回 claude 防选死项）。
    //   2026.08.20 增：自定义大模型激活时 Provider/模型/API Key 三行置灰防误导
    //   （激活条目经 ~/.pi/agent/models.json 路由，这三项不参与）；模型列表组随组级开关。
    if (m_pComboBackend == nullptr || m_pComboBackend->count() <= 2) {
        return;
    }
    const bool bPi = (m_pComboBackend->currentData() == 3) && m_bPiInstalled;
    const bool bPiCustomActive = m_iPiActiveModel >= 0 && m_iPiActiveModel < m_aPiModels.size();
    // 2026.08.21 路径类输入始终可编辑（同 Codex 死锁修复）：Pi 下拉项未装被禁选时
    //   根目录也填不了，互为前提走不通。
    if (m_pEditPiNode != nullptr)    { m_pEditPiNode->setEnabled(true); }
    if (m_pEditPiRoot != nullptr)    { m_pEditPiRoot->setEnabled(true); }
    if (m_pEditPiProvider != nullptr){ m_pEditPiProvider->setEnabled(bPi && !bPiCustomActive); }
    if (m_pComboPiThinking != nullptr){ m_pComboPiThinking->setEnabled(bPi); }
    if (m_pSpinPiPriceIn != nullptr)        { m_pSpinPiPriceIn->setEnabled(bPi); }
    if (m_pSpinPiPriceCacheRead != nullptr) { m_pSpinPiPriceCacheRead->setEnabled(bPi); }
    if (m_pSpinPiPriceCacheWrite != nullptr){ m_pSpinPiPriceCacheWrite->setEnabled(bPi); }
    if (m_pSpinPiPriceOut != nullptr)       { m_pSpinPiPriceOut->setEnabled(bPi); }
    // 2026.09.09 v2.1.138 自定义大模型列表不再要求选中/安装本后端（同 DSH 口径）：
    //   [PiModels] 为纯 config 数据，未选 pi 时也可增改删/设为当前（提前预配）。
    auto* pStdModel = qobject_cast<QStandardItemModel*>(m_pComboBackend->model());
    const Qt::ItemFlags eFlags = m_bPiInstalled
        ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled) : Qt::NoItemFlags;
    if (pStdModel != nullptr) {
        if (QStandardItem* pItem = pStdModel->item(2); pItem != nullptr) {
            pItem->setFlags(eFlags);
        }
    } else {
        m_pComboBackend->model()->setData(m_pComboBackend->model()->index(2, 0),
                                          static_cast<int>(eFlags), Qt::UserRole - 1);
    }
    if (!m_bPiInstalled && m_pComboBackend->currentData() == 3
        && m_pComboBackend->currentData().toInt() != m_iCfgBackend) {
        m_pComboBackend->setCurrentIndex(0);   // 主动新切到不可用 Pi 才弹回；已存配置保留（2026.08.21）
    }
}


void CSettingsDialog::OnInstallPiNpm() {
    // 2026.08.20 联网安装：提权 npm install -g（npm.cmd 需管理员写全局 node_modules；
    //   cmd /k 保持窗口看进度）。装完点「重新检测」→ NpmGlobalCliPath 命中即可用。
    const HINSTANCE hInst = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
        QStringLiteral("/k npm install -g @earendil-works/pi-coding-agent")
            .toStdWString().c_str(),
        nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        QMessageBox::warning(this, QStringLiteral("PIAgent"),
            QStringLiteral("未能以管理员权限启动 npm 安装（UAC 被取消或被拒）。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("PIAgent"),
        QStringLiteral("已在管理员窗口中启动 npm 联网安装，请在该窗口查看进度。\n\n"
                       "安装完成后回到本页点「重新检测」（npm 全局安装无需填 PIAgent 根目录，"
                       "检测自动从 npm 全局目录解析 cli.js）。"));
}

void CSettingsDialog::DetectPiInstalled(bool a_bSilent) {
    // 探测安装：根目录框显式值有效 → 装好；显式值失效（典型：config.ini 随发布包
    //   带到别的机器，显式键还是装机机的绝对路径——2026.09.23 v3.1.230 用户报告
    //   搬机后「显示已安装但根目录还是源机路径」）→ 同 DSH 口径回退探测 exe 同级
    //   PIAgent，命中即回填输入框；仍无效再兑 npm 全局布局（cli.js 解析在
    //   CPiAgentRunner::ResolveCliJsPath 同口径自动回退，根目录可留空）。框空时
    //   探测命中也自动填入（原行为）。
    bool bRootSet = m_pEditPiRoot != nullptr && !m_pEditPiRoot->text().trimmed().isEmpty();
    QString sExplicit = bRootSet ? m_pEditPiRoot->text().trimmed() : QString();
    if (bRootSet && !CPiAgentRunner::LooksLikePiRoot(sExplicit)) {
        bRootSet = false;   // 显式键失效：转入探测（同 DSH 搬机死键自愈口径）
        sExplicit.clear();
    }
    if (bRootSet) {
        m_bPiInstalled = CPiAgentRunner::LooksLikePiRoot(sExplicit);
    } else {
        const QString sProbed = CPiAgentRunner::ProbeSiblingRoot();
        m_bPiInstalled = !sProbed.isEmpty();
        if (m_bPiInstalled && m_pEditPiRoot != nullptr) {
            // 自动回填 blockSignals：防 textChanged 即时重检对此 setText 重入（同 Codex）。
            m_pEditPiRoot->blockSignals(true);
            m_pEditPiRoot->setText(QDir::toNativeSeparators(sProbed));   // 死键替换/空框回填
            m_pEditPiRoot->blockSignals(false);
            // 持久化由 CMainWindow（保存路径的探测自愈）与启动回填负责——对话框层
            //   不落盘（与 DSH 同口径，避免在取消时也写 config.ini）。
        }
    }
    const QString sRoot = (bRootSet ? sExplicit : m_pEditPiRoot->text().trimmed());
    // 2026.08.20 联网安装（npm 全局）兜底：根目录/兄弟探测未命中时查 npm 全局布局
    //   （cli.js 解析在 CPiAgentRunner::ResolveCliJsPath 同口径自动回退）。
    const QString sNpmCli = m_bPiInstalled ? QString() : CPiAgentRunner::NpmGlobalCliPath();
    if (!sNpmCli.isEmpty()) {
        m_bPiInstalled = true;
    }
    if (m_pLblPiState != nullptr) {
        if (m_bPiInstalled && !sNpmCli.isEmpty()) {
            // FR-1.3：npm 分支追加版本（ResolvePiVersion() 按形态自动分派读包；
            //   读不出→版本未知）。
            const QString sVer = CPiAgentRunner::ResolvePiVersion();
            m_pLblPiState->setText(
                QStringLiteral("已安装 ✓（npm 全局：%1，%2）")
                    .arg(sNpmCli,
                         sVer.isEmpty() ? QStringLiteral("版本未知")
                                        : QStringLiteral("版本 %1").arg(sVer)));
            m_pLblPiState->setStyleSheet(QStringLiteral("color: green;"));
        } else if (m_bPiInstalled) {
            // FR-1.3：root 分支追加版本（显式根直读，读不到不回退）。
            const QString sVer = CPiAgentRunner::ResolvePiVersion(sRoot);
            m_pLblPiState->setText(
                QStringLiteral("已安装 ✓（%1，%2）")
                    .arg(QDir::toNativeSeparators(sRoot),
                         sVer.isEmpty() ? QStringLiteral("版本未知")
                                        : QStringLiteral("版本 %1").arg(sVer)));
            m_pLblPiState->setStyleSheet(QStringLiteral("color: green;"));
        } else if (!sRoot.isEmpty()) {
            m_pLblPiState->setText(QStringLiteral("安装未完成：目录存在但缺 pi\\packages\\"
                                                  "coding-agent\\dist\\cli.js（%1）").arg(
                QDir::toNativeSeparators(sRoot)));
            m_pLblPiState->setStyleSheet(QStringLiteral("color: #b00;"));
        } else {
            m_pLblPiState->setText(QStringLiteral("未安装（未配置根目录，且未探测到 exe 同级"
                                                  " PIAgent 安装）"));
            m_pLblPiState->setStyleSheet(QStringLiteral("color: #b00;"));
        }
    }
    // 2026.08.21 已装不隐藏只置灰（口径同 DSH）：隐藏会让用户以为没有安装功能。
    if (m_pBtnInstallPi != nullptr) {
        m_pBtnInstallPi->setEnabled(!m_bPiInstalled);   // 装好后置灰，主路径回归「重新检测」
    }
    if (m_pBtnInstallPiNpm != nullptr) {
        m_pBtnInstallPiNpm->setEnabled(!m_bPiInstalled);
    }
    UpdatePiEnabled();
    if (!a_bSilent) {
        if (m_bPiInstalled) {
            const QString sWhere = !sNpmCli.isEmpty()
                ? QStringLiteral("npm 全局安装，cli.js = %1").arg(sNpmCli)
                : QStringLiteral("%1").arg(QDir::toNativeSeparators(
                    sRoot.isEmpty() ? CPiAgentRunner::ProbeSiblingRoot() : sRoot));
            QMessageBox::information(this, QStringLiteral("PIAgent"),
                QStringLiteral("PIAgent 安装检测通过（%1）。").arg(sWhere));
        } else {
            QMessageBox::warning(this, QStringLiteral("PIAgent"),
                QStringLiteral("未检测到可用的 PIAgent 安装。\n\n%1").arg(
                    sRoot.isEmpty() ? CPiAgentRunner::PiInstallDiagnosis()
                                    : QStringLiteral("请核对根目录（判定文件：pi\\packages\\"
                                                     "coding-agent\\dist\\cli.js）。")));
        }
    }
}

void CSettingsDialog::OnInstallPi() {
    // 提权启动 exe 同级 install_env.bat 的 /pi 单装模式：从本机 PIAgent 源码（exe 上级
    //   PIAgent，可用环境变量 PI_SRC 覆盖）robocopy 复制到 exe 同级 PIAgent\pi。
    //   cmd /k 保持窗口：robocopy/失败输出可查。
    const QString sScript = QCoreApplication::applicationDirPath()
        + QStringLiteral("/install_env.bat");
    if (!QFile::exists(sScript)) {
        QMessageBox::warning(this, QStringLiteral("PIAgent"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整（安装脚本随"
                           "构建同步到 exe 同级）。").arg(QDir::toNativeSeparators(sScript)));
        return;
    }
    const QString sArgs = QStringLiteral("/k \"")
        + QDir::toNativeSeparators(sScript) + QStringLiteral("\" /pi");
    const HINSTANCE hInst = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                                           sArgs.toStdWString().c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {   // <=32 = 提权被拒/失败
        QMessageBox::warning(this, QStringLiteral("PIAgent"),
            QStringLiteral("未能以管理员权限启动安装脚本（UAC 被取消或被拒）。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("PIAgent"),
        QStringLiteral("已在管理员窗口中启动安装，请在该窗口查看进度。\n\n"
                       "窗口出现「[OK] PIAgent 安装完成」才算装好（出现 [错误] "
                       "按提示处理后重跑，可续装）；完成后回到本页点「重新检测」，"
                       "检测通过即可选用该后端（根目录自动填入，无需手填）。"));
}

int CSettingsDialog::MaxIter() const {
    return m_pSpinMaxIter->value();
}

int CSettingsDialog::MaxResume() const {
    return m_pSpinMaxResume->value();
}

int CSettingsDialog::ReviewResume() const {
    return m_pSpinReviewResume->value();
}

int CSettingsDialog::ReviewParallel() const {
    return m_pSpinReviewPar->value();
}

QString CSettingsDialog::ClaudeCmd() const {
    return m_pEditClaudeCmd->text().trimmed();
}

int CSettingsDialog::AskTimeoutSec() const {
    return m_pSpinAskTimeout->value();
}

int CSettingsDialog::AgentBackend() const {
    return m_pComboBackend->currentData().toInt();
}

QString CSettingsDialog::DshNodePath() const {
    // 2026-10-08 同回填处：node 输入框已拆除，判空兑空串（保存链不再崩）。
    return m_pEditDshNode != nullptr ? m_pEditDshNode->text().trimmed() : QString();
}

QString CSettingsDialog::DshRoot() const {
    return m_pEditDshRoot->text().trimmed();
}

QStringList CSettingsDialog::DshExtraWriteDirs() const {
    // 2026.09.27 额外可写目录（';' 分隔解析；空项剔除）。
    if (m_pEditDshWsRoot == nullptr) {
        return QStringList();
    }
    QStringList aDirs;
    for (const QString& s : m_pEditDshWsRoot->text().split(QLatin1Char(';'),
                                                           Qt::SkipEmptyParts)) {
        const QString sT = s.trimmed();
        if (!sT.isEmpty() && !aDirs.contains(sT)) {
            aDirs.append(sT);
        }
    }
    return aDirs;
}

QString CSettingsDialog::DshProvider() const {
    return m_pEditDshProvider->text().trimmed();
}


double CSettingsDialog::DshPriceInPerM() const {
    return m_pSpinDshPriceIn->value();
}

double CSettingsDialog::DshPriceCacheReadPerM() const {
    return m_pSpinDshPriceCacheRead->value();
}

double CSettingsDialog::DshPriceCacheWritePerM() const {
    return m_pSpinDshPriceCacheWrite->value();
}

double CSettingsDialog::DshPriceOutPerM() const {
    return m_pSpinDshPriceOut->value();
}

QList<SDshModelEntry> CSettingsDialog::DshModels() const {
    return m_aDshModels;
}

int CSettingsDialog::DshActiveModel() const {
    return m_iDshActiveModel;
}

// 2026.08.19 PIAgent 参数 getters（保存路径在 CMainWindow::OnSettings 统一落盘）。
QString CSettingsDialog::PiNodePath() const {
    return m_pEditPiNode != nullptr ? m_pEditPiNode->text().trimmed() : QString();
}

QString CSettingsDialog::PiRoot() const {
    return m_pEditPiRoot != nullptr ? m_pEditPiRoot->text().trimmed() : QString();
}

QString CSettingsDialog::PiProvider() const {
    const QString s = m_pEditPiProvider != nullptr ? m_pEditPiProvider->text().trimmed()
                                                   : QString();
    return s.isEmpty() ? QStringLiteral("anthropic") : s;
}


QString CSettingsDialog::PiThinking() const {
    // 下拉首项「（默认）」＝ 空（不传 --thinking）。
    if (m_pComboPiThinking == nullptr || m_pComboPiThinking->currentIndex() <= 0) {
        return QString();
    }
    return m_pComboPiThinking->currentText();
}

QString CSettingsDialog::DshEffort() const {
    // 2026-10-08 思考强度（首项「（默认）」＝ 空，不注入 reasoningEffort）。
    if (m_pComboDshEffort == nullptr || m_pComboDshEffort->currentIndex() <= 0) {
        return QString();
    }
    return m_pComboDshEffort->currentText();
}

QString CSettingsDialog::CodexEffort() const {
    // 2026-10-08 思考强度（首项「（默认）」＝ 空，不传 -c model_reasoning_effort）。
    if (m_pComboCodexEffort == nullptr || m_pComboCodexEffort->currentIndex() <= 0) {
        return QString();
    }
    return m_pComboCodexEffort->currentText();
}

QString CSettingsDialog::HermesEffort() const {
    // 2026-10-08 思考强度（首项「（默认）」＝ 空，不托管 config.yaml agent.reasoning_effort）。
    if (m_pComboHermesEffort == nullptr || m_pComboHermesEffort->currentIndex() <= 0) {
        return QString();
    }
    return m_pComboHermesEffort->currentText();
}

int CSettingsDialog::ClaudeThinkTok() const {
    // 2026-10-08 思考预算（0 = 默认不注入 MAX_THINKING_TOKENS）。
    return m_pSpinClaudeThinkTok != nullptr ? m_pSpinClaudeThinkTok->value() : 0;
}

double CSettingsDialog::PiPriceInPerM() const {
    return m_pSpinPiPriceIn != nullptr ? m_pSpinPiPriceIn->value() : 0.0;
}

int CSettingsDialog::AdhocPromptCtxPctMax() const {
    // 2026-10-08 提示词占比上限（0=不限）。
    return m_pSpinPromptCtxPct != nullptr ? m_pSpinPromptCtxPct->value() : 5;
}

int CSettingsDialog::AdhocPromptTokMax() const {
    // 2026-10-08 提示词 token 上限（0=不限）。
    return m_pSpinPromptTokMax != nullptr ? m_pSpinPromptTokMax->value() : 0;
}

double CSettingsDialog::PiPriceCacheReadPerM() const {
    return m_pSpinPiPriceCacheRead != nullptr ? m_pSpinPiPriceCacheRead->value() : 0.0;
}

double CSettingsDialog::PiPriceCacheWritePerM() const {
    return m_pSpinPiPriceCacheWrite != nullptr ? m_pSpinPiPriceCacheWrite->value() : 0.0;
}

double CSettingsDialog::PiPriceOutPerM() const {
    return m_pSpinPiPriceOut != nullptr ? m_pSpinPiPriceOut->value() : 0.0;
}

// 2026.08.20 PiAgent 自定义大模型表 getters（主窗口落盘 [PiModels] 并应用到 runner）。
QList<SDshModelEntry> CSettingsDialog::PiModels() const {
    return m_aPiModels;
}

int CSettingsDialog::PiActiveModel() const {
    return m_iPiActiveModel;
}

bool CSettingsDialog::WebSearch() const {
    return m_pChkWebSearch->isChecked();
}

bool CSettingsDialog::ExpertAssistant() const {
    return (m_pChkExpertAssistant != nullptr) && m_pChkExpertAssistant->isChecked();
}

bool CSettingsDialog::ClaudeFollowShared() const {
    // 2026-10-08 claude 后端跟随共享大模型表开关（大模型页复选框）。
    return (m_pChkClaudeFollow != nullptr) && m_pChkClaudeFollow->isChecked();
}

bool CSettingsDialog::AdhocMaxTurnsAuto() const {
    // 2026.09.23 v3.0.219 工作量评估开关（未创建兑底开）。
    return (m_pChkMaxTurnsAuto == nullptr) || m_pChkMaxTurnsAuto->isChecked();
}

int CSettingsDialog::AdhocMaxTurns() const {
    // 2026.09.23 助手轮次上限（未创建兑底 40，钳 10..200）。
    return (m_pSpinMaxTurns != nullptr) ? qBound(10, m_pSpinMaxTurns->value(), 200) : 40;
}

int CSettingsDialog::AutoCompactPct() const {
    return m_pSpinCompactPct->value();
}

QString CSettingsDialog::ScanExcludeDirs() const {
    return m_pEditScanDirs->text().trimmed();
}

QString CSettingsDialog::BackupExcludeDirs() const {
    return m_pEditBackupDirs->text().trimmed();
}

QString CSettingsDialog::ExplorerHiddenDirs() const {
    return m_pEditExplHidden != nullptr ? m_pEditExplHidden->text().trimmed() : QString();
}

// —— 内置测试沙箱（2026-09-26；未创建控件时兜底默认值）——
bool CSettingsDialog::SandboxEnable() const {
    return (m_pChkSbxEnable == nullptr) || m_pChkSbxEnable->isChecked();
}

bool CSettingsDialog::SandboxAutoGhost() const {
    return (m_pChkSbxAuto == nullptr) || m_pChkSbxAuto->isChecked();
}

bool CSettingsDialog::SandboxRecord() const {
    return (m_pChkSbxRecord == nullptr) || m_pChkSbxRecord->isChecked();
}

int CSettingsDialog::SandboxFps() const {
    return (m_pSpinSbxFps != nullptr) ? qBound(1, m_pSpinSbxFps->value(), 15) : 5;
}

int CSettingsDialog::SandboxRetainDays() const {
    return (m_pSpinSbxRetain != nullptr) ? qBound(1, m_pSpinSbxRetain->value(), 365) : 7;
}

// —— CodexHarness 第四后端（2026 第四后端接入；口径同 Pi 同名方法）——
QString CSettingsDialog::CodexNodePath() const {
    return m_pEditCodexNode != nullptr ? m_pEditCodexNode->text().trimmed() : QString();
}

QString CSettingsDialog::CodexRoot() const {
    return m_pEditCodexRoot != nullptr ? m_pEditCodexRoot->text().trimmed() : QString();
}

QString CSettingsDialog::CodexProvider() const {
    const QString s = m_pEditCodexProvider != nullptr
        ? m_pEditCodexProvider->text().trimmed() : QString();
    return s.isEmpty() ? QStringLiteral("autocode") : s;
}


QString CSettingsDialog::CodexWireApi() const {
    return m_pComboCodexWireApi != nullptr ? m_pComboCodexWireApi->currentText()
                                           : QStringLiteral("chat");
}

// 2026.08.21 Codex 自定义大模型表 getters（主窗口落盘 [CodexModels] 并应用到 runner）。
QList<SDshModelEntry> CSettingsDialog::CodexModels() const {
    return m_aCodexModels;
}

int CSettingsDialog::CodexActiveModel() const {
    return m_iCodexActiveModel;
}

void CSettingsDialog::UpdateCodexEnabled() {
    // 后端联动（口径同 UpdatePiEnabled）：Codex 参数组按「选中且已安装」启用；
    // 下拉 CodexHarness 项按安装态启停（index 3），未装时选中弹回 claude。
    if (m_pComboBackend == nullptr || m_pComboBackend->count() <= 3) {
        return;
    }
    const bool bCodexSel = (m_pComboBackend->currentData() == 4);
    const bool bCodex = bCodexSel && m_bCodexInstalled;
    // 2026.08.21 自定义路由激活时固定路由五项不参与（置灰，口径同 Pi）。
    const bool bCodexCustomActive = m_iCodexActiveModel >= 0
        && m_iCodexActiveModel < m_aCodexModels.size();
    // 2026.08.21 修死锁：node/根目录此前要求「选中 Codex 且已安装」才可编辑，但
    //   Codex 下拉项未装时被禁选、不填根目录又检测不到已装——互为前提谁也走不通。
    //   路径类输入改为始终可编辑（填本机已有安装路径即触发即时重检解锁下拉项）。
    if (m_pEditCodexNode != nullptr)     { m_pEditCodexNode->setEnabled(true); }
    if (m_pEditCodexRoot != nullptr)     { m_pEditCodexRoot->setEnabled(true); }
    if (m_pEditCodexProvider != nullptr) { m_pEditCodexProvider->setEnabled(bCodex && !bCodexCustomActive); }
    if (m_pComboCodexWireApi != nullptr) { m_pComboCodexWireApi->setEnabled(bCodex && !bCodexCustomActive); }
    // 2026.09.09 v2.1.138 自定义大模型列表不再要求选中本后端（v14:45 曾放宽到
    //   「选中 Codex 即可用」，现彻底放开）：[CodexModels] 为纯 config 数据。
    auto* pStdModel = qobject_cast<QStandardItemModel*>(m_pComboBackend->model());
    const Qt::ItemFlags eFlags = m_bCodexInstalled
        ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled) : Qt::NoItemFlags;
    if (pStdModel != nullptr) {
        if (QStandardItem* pItem = pStdModel->item(3); pItem != nullptr) {
            pItem->setFlags(eFlags);
        }
    } else {
        m_pComboBackend->model()->setData(m_pComboBackend->model()->index(3, 0),
                                          static_cast<int>(eFlags), Qt::UserRole - 1);
    }
    if (!m_bCodexInstalled && m_pComboBackend->currentData() == 4
        && m_pComboBackend->currentData().toInt() != m_iCfgBackend) {
        m_pComboBackend->setCurrentIndex(0);   // 主动新切到不可用 Codex 才弹回；已存配置保留（2026.08.21）
    }
}

void CSettingsDialog::DetectCodexInstalled(bool a_bSilent) {
    // 探测安装：根目录框显式值优先（node_modules\@openai\codex\package.json 在 → 装好），
    //   否则 ProbeSiblingRoot 探测 exe 同级 CodexHarness，命中且框为空自动填入。
    const bool bRootSet = m_pEditCodexRoot != nullptr
        && !m_pEditCodexRoot->text().trimmed().isEmpty();
    const QString sExplicit = bRootSet ? m_pEditCodexRoot->text().trimmed() : QString();
    if (bRootSet) {
        m_bCodexInstalled = CCodexRunner::LooksLikeCodexRoot(sExplicit);
    } else {
        const QString sProbed = CCodexRunner::ProbeSiblingRoot();
        m_bCodexInstalled = !sProbed.isEmpty();
        if (m_bCodexInstalled && m_pEditCodexRoot != nullptr) {
            // 自动回填 blockSignals：防 textChanged 即时重检对此 setText 重入。
            m_pEditCodexRoot->blockSignals(true);
            m_pEditCodexRoot->setText(QDir::toNativeSeparators(sProbed));   // 自动回填
            m_pEditCodexRoot->blockSignals(false);
        }
    }
    const QString sRoot = bRootSet ? sExplicit
        : (m_pEditCodexRoot != nullptr ? m_pEditCodexRoot->text().trimmed() : QString());
    const QString sVer = m_bCodexInstalled ? CCodexRunner::ResolveCodexVersion() : QString();
    if (m_bCodexInstalled && !sVer.isEmpty() && sVer != CCodexRunner::ExpectedCodexVersion()) {
        m_bCodexInstalled = false;   // 版本不符按不可用处理（0.87.0 锁定）
    }
    if (m_pLblCodexState != nullptr) {
        if (m_bCodexInstalled) {
            m_pLblCodexState->setText(QStringLiteral("已安装 ✓（%1，版本 %2）").arg(
                QDir::toNativeSeparators(sRoot), sVer));
            m_pLblCodexState->setStyleSheet(QStringLiteral("color: green;"));
        } else if (!sVer.isEmpty() && sVer != CCodexRunner::ExpectedCodexVersion()) {
            m_pLblCodexState->setText(QStringLiteral(
                "版本不符：当前 v%1，本后端锁定 v%2（v0.88+ 已移除 wire_api=\"chat\"，"
                "仅 responses 可用；本后端按 chat 网关接入）。请重跑「安装 CodexHarness…」")
                .arg(sVer, CCodexRunner::ExpectedCodexVersion()));
            m_pLblCodexState->setStyleSheet(QStringLiteral("color: #b00;"));
        } else if (!sRoot.isEmpty()) {
            m_pLblCodexState->setText(QStringLiteral(
                "安装未完成：%1 下未找到 node_modules\\@openai\\codex。重跑"
                "「安装 CodexHarness…」续装").arg(QDir::toNativeSeparators(sRoot)));
            m_pLblCodexState->setStyleSheet(QStringLiteral("color: #b00;"));
        } else {
            m_pLblCodexState->setText(QStringLiteral(
                "未安装（未配置根目录，且未探测到 exe 同级 Execute\\CodexHarness 安装）"));
            m_pLblCodexState->setStyleSheet(QStringLiteral("color: #b00;"));
        }
    }
    if (m_pBtnInstallCodex != nullptr) {
        m_pBtnInstallCodex->setEnabled(!m_bCodexInstalled);   // 2026.08.21 置灰不隐藏（同 DSH/Pi）
    }
    if (m_pBtnInstallCodexLocal != nullptr) {
        m_pBtnInstallCodexLocal->setEnabled(!m_bCodexInstalled);
    }
    UpdateCodexEnabled();
    if (!a_bSilent) {
        if (m_bCodexInstalled) {
            QMessageBox::information(this, QStringLiteral("CodexHarness"),
                QStringLiteral("CodexHarness 安装检测通过（%1，版本 %2）。").arg(
                    QDir::toNativeSeparators(sRoot), sVer));
        } else {
            QMessageBox::warning(this, QStringLiteral("CodexHarness"),
                QStringLiteral("未检测到可用的 CodexHarness 安装。\n\n%1").arg(
                    !sVer.isEmpty() && sVer != CCodexRunner::ExpectedCodexVersion()
                        ? QStringLiteral("版本 %1 ≠ 锁定版本 %2。").arg(
                              sVer, CCodexRunner::ExpectedCodexVersion())
                        : CCodexRunner::CodexInstallDiagnosis()));
        }
    }
}

void CSettingsDialog::OnInstallCodex() {
    // 提权启动 exe 同级 install_env.bat 的 /codex 单装模式：npm 本地安装
    //   @openai/codex@0.87.0 --save-exact 到 exe 同级 Execute\CodexHarness。
    const QString sScript = QCoreApplication::applicationDirPath()
        + QStringLiteral("/install_env.bat");
    if (!QFile::exists(sScript)) {
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整（安装脚本随"
                           "构建同步到 exe 同级）。").arg(QDir::toNativeSeparators(sScript)));
        return;
    }
    const QString sArgs = QStringLiteral("/k \"")
        + QDir::toNativeSeparators(sScript) + QStringLiteral("\" /codex");
    const HINSTANCE hInst = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                                           sArgs.toStdWString().c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("未能以管理员权限启动安装脚本（UAC 被取消或被拒）。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("CodexHarness"),
        QStringLiteral("已在管理员窗口中启动安装，请在该窗口查看进度。\n\n"
                       "窗口出现「[OK] CodexHarness 安装完成」才算装好（出现 [错误] "
                       "按提示处理后重跑，可续装）；完成后回到本页点「重新检测」，"
                       "检测通过即可选用该后端（根目录自动填入，无需手填）。"));
}


// —— 2026.08.21 CodexHarness 本机源离线安装（robocopy；无需管理员/网络）——
void CSettingsDialog::OnInstallCodexLocal() {
    // 默认源：exe 上级 CodexHarness（与 PIAgent 本机源同级约定一致），可浏览改选。
    const QString sDefSrc = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../CodexHarness"));
    const QString sSrc = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择本机 CodexHarness 源目录（其下应有 node_modules/@openai/codex）"),
        QFile::exists(sDefSrc) ? sDefSrc : QString());
    if (sSrc.isEmpty()) {
        return;
    }
    if (!CCodexRunner::LooksLikeCodexRoot(sSrc)) {
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("所选目录不是有效的 CodexHarness 安装源：\n%1\n\n"
                           "其下未找到 node_modules/@openai/codex。").arg(sSrc));
        return;
    }
    // 目标：exe 同级 Execute\CodexHarness（固定锚点，与探测/路由一致）。
    const QString sDst = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Execute/CodexHarness"));
    if (!QDir().mkpath(sDst)) {
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("无法创建目标目录：\n%1").arg(sDst));
        return;
    }
    QProgressDialog stProg(QStringLiteral("正在从本机源复制 CodexHarness（robocopy）…"),
                           QStringLiteral("取消"), 0, 0, this);
    stProg.setWindowTitle(QStringLiteral("CodexHarness 安装"));
    stProg.setWindowModality(Qt::WindowModal);
    stProg.setMinimumDuration(0);
    stProg.show();
    QCoreApplication::processEvents();
    // robocopy <src> <dst> /E：整树复制；退出码 0-7 均为成功（8+ 才是错误）。
    QProcess stProc;
    stProc.start(QStringLiteral("robocopy"),
                 { QDir::toNativeSeparators(sSrc), QDir::toNativeSeparators(sDst),
                   QStringLiteral("/E"), QStringLiteral("/NFL"), QStringLiteral("/NDL"),
                   QStringLiteral("/NJH"), QStringLiteral("/NJS"), QStringLiteral("/NP"),
                   QStringLiteral("/R:2"), QStringLiteral("/W:2") });
    if (!stProc.waitForStarted(10000)) {
        stProg.cancel();
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("无法启动 robocopy（系统组件缺失？）"));
        return;
    }
    while (!stProc.waitForFinished(1000)) {
        if (stProc.state() != QProcess::Running) {
            break;
        }
        QCoreApplication::processEvents();
        if (stProg.wasCanceled()) {
            stProc.kill();
            stProc.waitForFinished(3000);
            stProg.cancel();
            return;
        }
    }
    stProg.cancel();
    const int iRc = stProc.exitCode();
    if (iRc >= 8) {
        QMessageBox::warning(this, QStringLiteral("CodexHarness"),
            QStringLiteral("robocopy 复制失败（退出码 %1）：\n%2")
                .arg(QString::number(iRc), QString::fromUtf8(stProc.readAllStandardError())));
        return;
    }
    DetectCodexInstalled(false);   // 自动重检 + 结果提示（命中自动回填根目录）
}


// ======================= Hermes 第五后端（任务 3213cdbf） =======================
// 口径同 Pi 页：参数组按「后端选中」启用（Hermes 项不做安装态禁选——探测失败时
// StartStage 带具体缺失路径报错，配置错误不应变成静默回退）；模型树与四方法同构。
void CSettingsDialog::UpdateHermesEnabled() {
    if (m_pComboBackend == nullptr) {
        return;
    }
    const bool bHermes = (m_pComboBackend->currentData() == 5);
    const bool bCustomActive = m_iHermesActiveModel >= 0
        && m_iHermesActiveModel < m_aHermesModels.size();
    if (m_pEditHermesNode != nullptr)    { m_pEditHermesNode->setEnabled(true); }
    if (m_pEditHermesRoot != nullptr)    { m_pEditHermesRoot->setEnabled(true); }
    if (m_pSpinHermesPriceIn != nullptr)        { m_pSpinHermesPriceIn->setEnabled(bHermes); }
    if (m_pSpinHermesPriceCacheRead != nullptr) { m_pSpinHermesPriceCacheRead->setEnabled(bHermes); }
    if (m_pSpinHermesPriceCacheWrite != nullptr){ m_pSpinHermesPriceCacheWrite->setEnabled(bHermes); }
    if (m_pSpinHermesPriceOut != nullptr)       { m_pSpinHermesPriceOut->setEnabled(bHermes); }
    // 2026.09.09 v2.1.138 自定义大模型列表不再要求选中本后端（同 DSH/Pi/Codex 口径）：
    //   [HermesModels] 为纯 config 数据，未选 Hermes 时也可增改删/设为当前。
}

void CSettingsDialog::DetectHermesInstalled(bool a_bSilent) {
    // 探测：显式根目录下 dist\cli.js（Harness）或 hermes-agent\venv\Scripts\
    //   hermes.exe（Python 版）存在才算装好；空则看 ResolveHermesCli()（含
    //   HERMES_HOME、exe 同级 HermesHarness 与 PATH 兜底）。2026.09 补本机源
    //   安装入口：按钮恒可用（未装=安装 / 已装=覆盖更新，无 npm/远端源）。
    const QString sRoot = m_pEditHermesRoot != nullptr ? m_pEditHermesRoot->text().trimmed()
                                                       : QString();
    // 探测前临时回填编辑框当前值（CR-v2 minor：ResolveHermesCli 读 Runner 静态
    //   s_sHermesRoot——未保存前点「重新检测」会拿到过期 config 值），探测后还原。
    const QString sSavedRoot = CHermesRunner::HermesRoot();
    if (!sRoot.isEmpty()) {
        CHermesRunner::SetHermesRoot(sRoot);
    }
    const QString sCli = CHermesRunner::ResolveHermesCli();
    if (!sRoot.isEmpty()) {
        CHermesRunner::SetHermesRoot(sSavedRoot);
    }
    m_bHermesInstalled = !sCli.isEmpty();
    if (m_pLblHermesState != nullptr) {
        if (!sCli.isEmpty()) {
            m_pLblHermesState->setText(QStringLiteral("已检测到 Hermes CLI ✓（%1）").arg(
                QDir::toNativeSeparators(sCli)));
            m_pLblHermesState->setStyleSheet(QStringLiteral("color: green;"));
        } else if (!sRoot.isEmpty()) {
            m_pLblHermesState->setText(QStringLiteral(
                "安装未完成：根目录存在但缺 dist\\cli.js 与 "
                "hermes-agent\\venv\\Scripts\\hermes.exe（%1）").arg(
                QDir::toNativeSeparators(sRoot)));
            m_pLblHermesState->setStyleSheet(QStringLiteral("color: #b00;"));
        } else {
            m_pLblHermesState->setText(QStringLiteral(
                "未安装（未配置根目录，且未探测到 HERMES_HOME、exe 同级 "
                "HermesHarness 与 PATH 的 hermes）。点右侧「本机源安装（离线）」"
                "可从本机源安装"));
            m_pLblHermesState->setStyleSheet(QStringLiteral("color: #b00;"));
        }
    }
    UpdateHermesEnabled();
    if (!a_bSilent) {
        if (m_bHermesInstalled) {
            QMessageBox::information(this, QStringLiteral("Hermes"),
                QStringLiteral("Hermes 安装检测通过（%1）。").arg(
                    QDir::toNativeSeparators(sCli)));
        } else {
            QMessageBox::warning(this, QStringLiteral("Hermes"),
                QStringLiteral("未检测到可用的 Hermes 安装。\n\n%1").arg(
                    CHermesRunner::HermesConfigError()));
        }
    }
}

// —— 2026.09 Hermes 本机源离线安装（robocopy；无需管理员/网络，口径同 Codex）——
void CSettingsDialog::OnInstallHermesLocal() {
    // 默认源：exe 上级 HermesHarness（与本机源部署约定一致），可浏览改选。
    const QString sDefSrc = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../HermesHarness"));
    const QString sSrc = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择本机 Hermes Harness 源目录（其下应有 dist\\cli.js）"),
        QFile::exists(sDefSrc) ? sDefSrc : QString());
    if (sSrc.isEmpty()) {
        return;
    }
    if (!CHermesRunner::LooksLikeHermesRoot(sSrc)) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("所选目录不是有效的 Hermes Harness 源：\n%1\n\n"
                           "其下未找到 dist\\cli.js。").arg(sSrc));
        return;
    }
    // 目标：exe 同级 HermesHarness（固定锚点，与 ResolveHermesCli 探测序一致）。
    //   源≠目标（浏览误选安装目标本身时直接视为重装完成，不 robocopy 自拷）。
    const QString sDst = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("HermesHarness"));
    if (QDir(sSrc).absolutePath() == QDir(sDst).absolutePath()) {
        QMessageBox::information(this, QStringLiteral("Hermes"),
            QStringLiteral("所选目录就是安装目标本身，无需复制。"));
        DetectHermesInstalled(false);
        return;
    }
    if (!QDir().mkpath(sDst)) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("无法创建目标目录：\n%1").arg(sDst));
        return;
    }
    QProgressDialog stProg(QStringLiteral("正在从本机源复制 Hermes（robocopy）…"),
                           QStringLiteral("取消"), 0, 0, this);
    stProg.setWindowTitle(QStringLiteral("Hermes 安装"));
    stProg.setWindowModality(Qt::WindowModal);
    stProg.setMinimumDuration(0);
    stProg.show();
    QCoreApplication::processEvents();
    // robocopy <src> <dst> /E：整树复制；退出码 0-7 均为成功（8+ 才是错误）。
    QProcess stProc;
    stProc.start(QStringLiteral("robocopy"),
                 { QDir::toNativeSeparators(sSrc), QDir::toNativeSeparators(sDst),
                   QStringLiteral("/E"), QStringLiteral("/NFL"), QStringLiteral("/NDL"),
                   QStringLiteral("/NJH"), QStringLiteral("/NJS"), QStringLiteral("/NP"),
                   QStringLiteral("/R:2"), QStringLiteral("/W:2") });
    if (!stProc.waitForStarted(10000)) {
        stProg.cancel();
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("无法启动 robocopy（系统组件缺失？）"));
        return;
    }
    while (!stProc.waitForFinished(1000)) {
        if (stProc.state() != QProcess::Running) {
            break;
        }
        QCoreApplication::processEvents();
        if (stProg.wasCanceled()) {
            stProc.kill();
            stProc.waitForFinished(3000);
            stProg.cancel();
            return;
        }
    }
    stProg.cancel();
    const int iRc = stProc.exitCode();
    if (iRc >= 8) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("robocopy 复制失败（退出码 %1）：\n%2")
                .arg(QString::number(iRc), QString::fromUtf8(stProc.readAllStandardError())));
        return;
    }
    DetectHermesInstalled(false);   // 自动重检 + 结果提示
}

// —— 2026.09 Hermes 联网安装（git 克隆 + pnpm 构建；提权口径同 OnInstallDsh）——
void CSettingsDialog::OnInstallHermesNet() {
    // Hermes 无固定远端发布源，仓库地址不设持久配置项（对齐 DSH/Pi 等后端设置页
    //   均无此项）——安装时经输入框现场给定：git URL（https:// 或 git@）或本机
    //   仓库路径（git clone 支持本地路径，离线机/内网源同样可用）。
    bool bOk = false;
    const QString sRepo = QInputDialog::getText(this, QStringLiteral("Hermes 联网安装"),
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
        // 本机路径：须是存在的目录且为 git 仓库（其下 .git 或 dist\cli.js）。
        const QString sGitDir = sRepo + QStringLiteral("/.git");
        const QString sCli = sRepo + QStringLiteral("/dist/cli.js");
        if (!QDir(sRepo).exists() || (!QFileInfo::exists(sGitDir) && !QFileInfo::exists(sCli))) {
            QMessageBox::warning(this, QStringLiteral("Hermes"),
                QStringLiteral("输入既不是 https:// 或 git@ 开头的 git URL，"
                               "也不是有效的本机 Hermes 仓库目录（其下应有 .git "
                               "或 dist\\cli.js）：\n%1").arg(sRepo));
            return;
        }
    }
    // 提权启动 exe 同级 install_env.bat 的 /hermes 单装模式（地址作第 2 参传入）。
    //   cmd /k 保持窗口：git/pnpm/失败输出可查。
    // 2026.09.07 v2.1.121 修引号剥离（用户实报：「'E:\...\AutoCode\Execute' 不是
    //   内部或外部命令」）：cmd 对 /k 后串的引号规则（cmd /? 规则 2）——引号数 >2 时
    //   剥掉首个引号并去掉末尾最后一个引号。旧式 `/k "bat" /hermes "url"`（4 引号）
    //   被剥后路径失去包裹，exe 目录带空格（Execute - 副本）即在首个空格处断开，把
    //   `E:\...\AutoCode\Execute` 当命令执行。修法＝整条命令外层再包一对引号
    //   （`/k ""bat" /hermes "url""`）——cmd 剥掉外层后剩下平衡的
    //   `"bat" /hermes "url"`；实测带空格路径正确执行、%~2 剥引号取 URL 不受影响。
    //   （DSH/Pi/Codex 的 `/k "bat" /mode` 恰 2 引号走 cmd 规则 1 保留引号，无需改。）
    const QString sScript = QCoreApplication::applicationDirPath()
        + QStringLiteral("/install_env.bat");
    if (!QFile::exists(sScript)) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整（安装脚本随"
                           "构建同步到 exe 同级）。").arg(QDir::toNativeSeparators(sScript)));
        return;
    }
    const QString sArgs = QStringLiteral("/k \"\"")
        + QDir::toNativeSeparators(sScript) + QStringLiteral("\" /hermes \"")
        + sRepo + QStringLiteral("\"\"");
    const HINSTANCE hInst = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                                           sArgs.toStdWString().c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {   // <=32 = 提权被拒/失败
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("未能以管理员权限启动安装脚本（UAC 被取消或被拒）。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("Hermes"),
        QStringLiteral("已在管理员窗口中启动联网安装，请在该窗口查看进度。\n\n"
                       "窗口出现「[OK] Hermes 安装完成」才算装好（出现 [错误] "
                       "按提示处理后重跑，可续装）；完成后回到本页点「重新检测」，"
                       "检测通过即可选用该后端（exe 同级 HermesHarness 自动命中）。"));
}

// Hermes 参数 getters（保存路径在 CMainWindow::OnSettings 统一落盘）。
QString CSettingsDialog::HermesNodePath() const {
    return m_pEditHermesNode != nullptr ? m_pEditHermesNode->text().trimmed() : QString();
}

QString CSettingsDialog::HermesRoot() const {
    return m_pEditHermesRoot != nullptr ? m_pEditHermesRoot->text().trimmed() : QString();
}




double CSettingsDialog::HermesPriceInPerM() const {
    return m_pSpinHermesPriceIn != nullptr ? m_pSpinHermesPriceIn->value() : 0.0;
}

double CSettingsDialog::HermesPriceCacheReadPerM() const {
    return m_pSpinHermesPriceCacheRead != nullptr ? m_pSpinHermesPriceCacheRead->value() : 0.0;
}

double CSettingsDialog::HermesPriceCacheWritePerM() const {
    return m_pSpinHermesPriceCacheWrite != nullptr ? m_pSpinHermesPriceCacheWrite->value() : 0.0;
}

double CSettingsDialog::HermesPriceOutPerM() const {
    return m_pSpinHermesPriceOut != nullptr ? m_pSpinHermesPriceOut->value() : 0.0;
}

QList<SDshModelEntry> CSettingsDialog::HermesModels() const {
    return m_aHermesModels;
}

int CSettingsDialog::HermesActiveModel() const {
    return m_iHermesActiveModel;
}
