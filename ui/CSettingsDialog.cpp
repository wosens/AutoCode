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
                                 CVersionDialog::FBusyCheck a_fnBackendBusy)
    : QDialog(a_pParent), m_fnBackendBusy(std::move(a_fnBackendBusy)) {
    m_pPm = a_pPm;   // 2026.08.27 几何持久化用（done() 落盘）；非拥有。
    SetupUi();

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
    const qint64 iDefCtx = (a_pPm != nullptr) ? a_pPm->LoadAdhocContextLimit() : 0;
    const bool bDefExpert = (a_pPm != nullptr) ? a_pPm->LoadExpertAssistant() : true;
    const QString sDefScan = (a_pPm != nullptr) ? a_pPm->LoadScanExcludeDirs() : QString();
    const QString sDefBackup = (a_pPm != nullptr) ? a_pPm->LoadBackupExcludeDirs() : QString();
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
    m_pEditDshNode->setText(sDefDshNode);
    m_pEditDshRoot->setText(sDefDshRoot);
    m_pEditDshProvider->setText(sDefDshProvider);
    m_pSpinDshPriceIn->setValue(dDefPriceIn);
    m_pSpinDshPriceCacheRead->setValue(dDefPriceCr);
    m_pSpinDshPriceCacheWrite->setValue(dDefPriceCw);
    m_pSpinDshPriceOut->setValue(dDefPriceOut);
    DetectDshInstalled(true);
    // 2026.08.19 PIAgent 参数回填（[Agent] Pi*）：构造序理由同上——先填根目录并完成
    //   安装检测、再选配置里的后端，否则 setCurrentIndex(3) 先触发 UpdatePiEnabled，
    //   m_bPiInstalled 尚为 false 会把 pi 选中态立即弹回 claude。
    m_pEditPiNode->setText((a_pPm != nullptr) ? a_pPm->LoadPiNodePath() : QString());
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
    m_pEditCodexNode->setText((a_pPm != nullptr) ? a_pPm->LoadCodexNodePath() : QString());
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
    // 2026.08.21 Codex 自定义大模型表回填（[CodexModels]；空表/无激活 = 走固定路由）。
    m_aCodexModels = (a_pPm != nullptr) ? a_pPm->LoadCodexModels(&m_iCodexActiveModel)
                                        : QList<SDshModelEntry>();
    RefreshCodexModelList();
    // 任务 3213cdbf Hermes 第五后端回填（[Agent] Hermes* / [HermesModels]）。
    m_pEditHermesNode->setText((a_pPm != nullptr) ? a_pPm->LoadHermesNodePath() : QString());
    m_pEditHermesRoot->setText((a_pPm != nullptr) ? a_pPm->LoadHermesRoot() : QString());
    m_pSpinHermesPriceIn->setValue((a_pPm != nullptr) ? a_pPm->LoadHermesPriceInPerM() : 3.0);
    m_pSpinHermesPriceCacheRead->setValue(
        (a_pPm != nullptr) ? a_pPm->LoadHermesPriceCacheReadPerM() : 0.3);
    m_pSpinHermesPriceCacheWrite->setValue(
        (a_pPm != nullptr) ? a_pPm->LoadHermesPriceCacheWritePerM() : 3.75);
    m_pSpinHermesPriceOut->setValue((a_pPm != nullptr) ? a_pPm->LoadHermesPriceOutPerM() : 15.0);
    DetectHermesInstalled(true);
    m_aHermesModels = (a_pPm != nullptr) ? a_pPm->LoadHermesModels(&m_iHermesActiveModel)
                                         : QList<SDshModelEntry>();
    RefreshHermesModelList();
    // 2026.08.20 PiAgent 自定义大模型表回填（[PiModels]；空表/无激活 = 走固定路由）。
    m_aPiModels = (a_pPm != nullptr) ? a_pPm->LoadPiModels(&m_iPiActiveModel)
                                     : QList<SDshModelEntry>();
    RefreshPiModelList();
    m_pComboBackend->setCurrentIndex(
        m_pComboBackend->findData(iDefBackend) < 0 ? 0 : m_pComboBackend->findData(iDefBackend));
    m_pEditClaudeCmd->setText(sDefCmd);
    m_pSpinAskTimeout->setValue(iDefAskTimeout);
    m_pChkWebSearch->setChecked(bDefWeb);
    m_pChkExpertAssistant->setChecked(bDefExpert);
    m_pSpinCompactPct->setValue(iDefPct);
    // 上下文上限以「千 token」展示更友好：内部 0=自动；展示 0 也表示自动（suffix 提示）。
    m_pSpinCtxLimit->setValue(
        static_cast<int>(qBound(qint64(0), iDefCtx / 1000, qint64(4000))));
    m_pEditScanDirs->setText(sDefScan);
    m_pEditBackupDirs->setText(sDefBackup);
    // 2026.08.18 自定义大模型表回填（[DshModels]；空表 / 无激活 = 未启用，走固定路由）。
    m_aDshModels = (a_pPm != nullptr) ? a_pPm->LoadDshModels(&m_iDshActiveModel)
                                      : QList<SDshModelEntry>();
    RefreshDshModelList();

    // 2026.08.27 窗口几何：优先恢复上次保存的（[SettingsWindow]）；无记录/已出屏则用
    //   默认尺寸。默认取 sizeHint（含最高页签的完整内容）夹在父窗口所在屏工作区 95% 内——
    //   QDialog::exec 的 adjustSize 会把窗口夹到屏幕 2/3，PiAgent 页内容较高时窗口被
    //   压到布局最小尺寸以下，出现按钮行叠进大模型列表（本次修复目标）。
    QRect stGeo;
    if (m_pPm != nullptr && m_pPm->LoadSettingsDialogGeometry(stGeo)
        && IsGeoOnScreen(stGeo)) {
        setGeometry(stGeo);   // 尺寸小于布局最小值时 Qt 自动钳位到最小，安全
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
}

void CSettingsDialog::done(int a_r) {
    // 2026.08.27 几何落盘：accept / reject / 标题栏 X 关闭（触发 reject）都经 done()，
    //   在此统一保存；失败静默（下次用默认几何，不影响功能）。
    if (m_pPm != nullptr) {
        m_pPm->SaveSettingsDialogGeometry(geometry());
    }
    QDialog::done(a_r);
}

void CSettingsDialog::SetupUi() {
    setWindowTitle(QStringLiteral("设置"));
    setMinimumWidth(520);

    // —— 运行策略 ——
    auto* pGroupPolicy = new QGroupBox(QStringLiteral("运行策略"), this);
    auto* pFormPolicy = new QFormLayout(pGroupPolicy);
    m_pSpinMaxIter = new QSpinBox(pGroupPolicy);
    m_pSpinMaxIter->setRange(1, 9);
    m_pSpinMaxIter->setToolTip(QStringLiteral(
        "单个阶段被评审驳回后的最大重跑次数，超过则熔断暂停并升级到用户处理（config.ini [Policy] MaxIter）"));
    pFormPolicy->addRow(QStringLiteral("单阶段最大迭代次数："), m_pSpinMaxIter);
    m_pSpinMaxResume = new QSpinBox(pGroupPolicy);
    m_pSpinMaxResume->setRange(1, 9);
    m_pSpinMaxResume->setToolTip(QStringLiteral(
        "阶段命中轮次上限后自动 --resume 续跑的封顶次数（[Policy] MaxResume）"));
    pFormPolicy->addRow(QStringLiteral("阶段续跑上限（次）："), m_pSpinMaxResume);
    m_pSpinReviewResume = new QSpinBox(pGroupPolicy);
    m_pSpinReviewResume->setRange(1, 9);
    m_pSpinReviewResume->setToolTip(QStringLiteral(
        "轻量评审命中轮次上限后催促落盘结论的续跑封顶次数（[Policy] ReviewResume）"));
    pFormPolicy->addRow(QStringLiteral("评审续跑上限（次）："), m_pSpinReviewResume);

    // —— Agent ——
    auto* pGroupAgent = new QGroupBox(QStringLiteral("Agent"), this);
    auto* pFormAgent = new QFormLayout(pGroupAgent);
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
        "须在下方配置三项路径；切换在空闲时生效（运行中禁止切换）。\n"
        "PIAgent：pi coding-agent CLI（--mode json 流式接入，磁盘会话跨进程续跑），"
        "参数在本页 PiAgent 页签配置"));
    pFormAgent->addRow(QStringLiteral("Agent 后端："), m_pComboBackend);
    connect(m_pComboBackend, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { UpdateDshEnabled(); UpdatePiEnabled(); UpdateCodexEnabled();
                             UpdateHermesEnabled(); });
    m_pEditClaudeCmd = new QLineEdit(pGroupAgent);
    m_pEditClaudeCmd->setPlaceholderText(QStringLiteral("claude"));
    m_pEditClaudeCmd->setToolTip(QStringLiteral(
        "claude 命令；默认 claude（PATH 查找），可填绝对路径指定安装（[Agent] ClaudeCmd）。\n"
        "仅在 Claude Code 后端使用"));
    pFormAgent->addRow(QStringLiteral("claude 命令："), m_pEditClaudeCmd);
    m_pSpinAskTimeout = new QSpinBox(pGroupAgent);
    m_pSpinAskTimeout->setRange(30, 86400);
    m_pSpinAskTimeout->setSuffix(QStringLiteral(" 秒"));
    m_pSpinAskTimeout->setToolTip(QStringLiteral(
        "ask_user 提问等待用户回答的超时（[Agent] AskTimeoutSec，默认 600）。\n"
        "仅 Claude Code 后端生效（DSH 无 ask_user 工具，该项禁用）"));
    pFormAgent->addRow(QStringLiteral("提问等待超时："), m_pSpinAskTimeout);
    m_pChkWebSearch = new QCheckBox(QStringLiteral("允许阶段联网检索（WebSearch/WebFetch）"), pGroupAgent);
    m_pChkWebSearch->setToolTip(QStringLiteral(
        "关闭后所有阶段的 --disallowedTools 追加 WebSearch、WebFetch（离线/内网环境用；[Agent] WebSearch）。\n"
        "仅 Claude Code 后端生效（DSH 侧忽略该参数，该项禁用）"));
    pFormAgent->addRow(QString(), m_pChkWebSearch);

    // 2026.09 后端版本管理入口（任务 23183ccb）：四后端当前/最新版本与更新一站式；
    //   与主窗功能菜单同源（D6 判据由 CMainWindow 注入透传）。
    auto* pBtnVersion = new QPushButton(
        QStringLiteral("后端版本管理…"), pGroupAgent);
    pBtnVersion->setToolTip(QStringLiteral(
        "查看四 Agent 后端（Claude Code/DSH/PIAgent/Codex）当前版本与远端最新版，\n"
        "并提供各自更新入口（结果仅本次会话有效，不落盘）"));
    connect(pBtnVersion, &QPushButton::clicked, this, &CSettingsDialog::OnVersionManage);
    pFormAgent->addRow(QString(), pBtnVersion);

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
    fnAddPathRow(QStringLiteral("node 路径："), &m_pEditDshNode,
                 QStringLiteral("node.exe 绝对路径；留空使用 PATH 中的 node（本机已装 node 即零配置；"
                                "[Agent] DshNodePath）"), false);
    m_pEditDshNode->setPlaceholderText(QStringLiteral("留空使用 PATH 中的 node"));
    fnAddPathRow(QStringLiteral("DSH 根目录："), &m_pEditDshRoot,
                 QStringLiteral("deepseek-harness 仓库根目录；bin.js 与 cordis.yml 按固定布局自动推导"
                                "（packages/examples/jsonrpc-demo/lib/bin.js、examples/autocode-agent/"
                                "cordis.yml）。留空时启动自动探测 exe 同级/上级的 DeepSeekHarness 目录"
                                "（[Agent] DshRoot）"), true);
    m_pEditDshProvider = new QLineEdit(pGroupDsh);
    m_pEditDshProvider->setPlaceholderText(QStringLiteral("glm-custom"));
    m_pEditDshProvider->setToolTip(QStringLiteral(
        "LLM provider 名（组合 llm-pi-ai 内联 providers 键；[Agent] DshProvider，默认 glm-custom）"));
    pFormDsh->addRow(QStringLiteral("Provider："), m_pEditDshProvider);
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

    // —— 评审 / 助手 ——
    auto* pGroupReview = new QGroupBox(QStringLiteral("评审与助手"), this);
    auto* pFormReview = new QFormLayout(pGroupReview);
    m_pSpinReviewPar = new QSpinBox(pGroupReview);
    m_pSpinReviewPar->setRange(1, 8);
    m_pSpinReviewPar->setToolTip(QStringLiteral(
        "每个评审阶段并行拉起的视角数，1=串行回退（[Review] Parallel，默认 3）"));
    pFormReview->addRow(QStringLiteral("评审并行视角数："), m_pSpinReviewPar);
    m_pSpinCompactPct = new QSpinBox(pGroupReview);
    m_pSpinCompactPct->setRange(10, 95);
    m_pSpinCompactPct->setSuffix(QStringLiteral(" %"));
    m_pSpinCompactPct->setToolTip(QStringLiteral(
        "上下文占用达到窗口的该比例时触发自动压缩；阈值 = 窗口 × 比例"
        "（[Adhoc] AutoCompactPct）。\n仅 Claude Code 后端生效（DSH 内置压缩策略，该项禁用）"));
    pFormReview->addRow(QStringLiteral("自动压缩比例："), m_pSpinCompactPct);
    m_pSpinCtxLimit = new QSpinBox(pGroupReview);
    m_pSpinCtxLimit->setRange(0, 4000);
    m_pSpinCtxLimit->setSuffix(QStringLiteral(" k"));
    m_pSpinCtxLimit->setSpecialValueText(QStringLiteral("自动"));
    m_pSpinCtxLimit->setToolTip(QStringLiteral(
        "助手上下文窗口上限（千 token）；0=按模型自动探测（[Adhoc] ContextLimit）"));
    pFormReview->addRow(QStringLiteral("上下文窗口上限："), m_pSpinCtxLimit);
    // 2026.08.26 专家助手开关：启用后助手提交改码提示词时自动验证（编译/测试）并回注重试。
    m_pChkExpertAssistant = new QCheckBox(QStringLiteral("启用专家助手"), pGroupReview);
    m_pChkExpertAssistant->setToolTip(QStringLiteral(
        "启用后，助手提交改码类提示词时自动执行编译/测试验证，失败自动回注重试（上限 3 次）；\n"
        "禁用则回到纯对话模式，验证由人工完成（[Adhoc] ExpertAssistant，默认启用）"));
    pFormReview->addRow(QString(), m_pChkExpertAssistant);

    // —— 目录排除 ——
    auto* pGroupDirs = new QGroupBox(QStringLiteral("目录排除"), this);
    auto* pFormDirs = new QFormLayout(pGroupDirs);
    m_pEditScanDirs = new QLineEdit(pGroupDirs);
    m_pEditScanDirs->setPlaceholderText(QStringLiteral(".autodev"));
    m_pEditScanDirs->setToolTip(QStringLiteral(
        "guide 指纹扫描额外排除的目录名，逗号分隔；留空 = 内置默认。"
        "内置规则：隐藏文件/文件夹（'.' 开头，含 .git/.vs）始终排除，不可关闭；"
        ".autodev 正常扫描，仅指纹表自身 guide.md 不参与比对（防自我变更死循环）"));
    pFormDirs->addRow(QStringLiteral("指纹扫描排除："), m_pEditScanDirs);
    m_pEditBackupDirs = new QLineEdit(pGroupDirs);
    m_pEditBackupDirs->setPlaceholderText(QStringLiteral(
        ".autodev,build,Execute,.git,.vs,x64"));
    m_pEditBackupDirs->setToolTip(QStringLiteral(
        "代码备份/恢复排除的目录名，逗号分隔；留空 = 内置默认（[Backup] ExcludeDirs）"));
    pFormDirs->addRow(QStringLiteral("备份恢复排除："), m_pEditBackupDirs);

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
    auto* pGroupModels = new QGroupBox(QStringLiteral("自定义大模型"), pTabDsh);
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
    pHdrModels->setSectionsMovable(false);                        // 禁拖动换列序
    pHdrModels->setSectionResizeMode(QHeaderView::Interactive);   // 全列可拖分隔线调宽
    pHdrModels->setSectionResizeMode(1, QHeaderView::Stretch);    // 接入点列伸缩填满
    pHdrModels->resizeSection(0, 130);
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
        "自定义模型条目（配置随程序走，存 exe 同级 config.ini 的 [DshModels] 节）。\n"
        "「当前」列打 ● 的条目生效，忽略上方 Provider/模型（组合改走 autocode-active\n"
        "槽位：接入点 / API Key / 窗口等全部取自该条目）。双击条目可直接编辑"));
    connect(m_pTreeDshModels, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { OnDshModelEdit(); });
    // 选中行变化只刷新按钮态（RefreshDshModelList 内部 blockSignals 重建，无重入）
    connect(m_pTreeDshModels, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { RefreshDshModelList(); });
    pLayModels->addWidget(m_pTreeDshModels);
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
        "「当前」列打 ● 的条目生效，上方 Provider/模型 不参与路由（组合走 autocode-active 槽位）。\n"
        "2026.08.19 保存后立即生效：空闲的 DSH 会话自动换新配置；进行中的轮次在该轮结束后切换\n"
        "（无需重启程序）。API Key 以明文存于 exe 同级 config.ini，注意文件外发安全。"), pGroupModels);
    pLblModelsHint->setWordWrap(true);
    pLayModels->addWidget(pLblModelsHint);
    pLayDsh->addWidget(pGroupModels);
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
    fnAddPiPathRow(QStringLiteral("node 路径："), &m_pEditPiNode,
                   QStringLiteral("node.exe 绝对路径；留空使用 PATH 中的 node（[Agent] PiNodePath）"),
                   false);
    m_pEditPiNode->setPlaceholderText(QStringLiteral("留空使用 PATH 中的 node"));
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

    // —— PiAgent 自定义大模型（2026.08.20，口径仿 DSH 页）——
    //   条目合成写 ~/.pi/agent/models.json 的 autocode-<n> provider 槽位（密钥随
    //   provider 配置走，不进命令行/日志）；激活条目覆盖上方 Provider/模型固定路由。
    auto* pGroupPiModels = new QGroupBox(QStringLiteral("自定义大模型"), pTabPi);
    auto* pLayPiModels = new QVBoxLayout(pGroupPiModels);
    m_pTreePiModels = new QTreeWidget(pGroupPiModels);
    m_pTreePiModels->setColumnCount(7);
    m_pTreePiModels->setHeaderLabels(QStringList()
        << QStringLiteral("显示名")
        << QStringLiteral("请求地址")
        << QStringLiteral("API 格式")
        << QStringLiteral("模型名称")
        << QStringLiteral("上下文")
        << QStringLiteral("最大输出")
        << QStringLiteral("当前"));
    m_pTreePiModels->setRootIsDecorated(false);
    m_pTreePiModels->setAlternatingRowColors(true);
    QHeaderView* pHdrPiModels = m_pTreePiModels->header();
    pHdrPiModels->setSectionsMovable(false);
    pHdrPiModels->setSectionResizeMode(QHeaderView::Interactive);
    pHdrPiModels->setSectionResizeMode(1, QHeaderView::Stretch);
    pHdrPiModels->resizeSection(0, 130);
    pHdrPiModels->resizeSection(2, 125);
    pHdrPiModels->resizeSection(3, 100);
    pHdrPiModels->resizeSection(4, 75);
    pHdrPiModels->resizeSection(5, 75);
    pHdrPiModels->resizeSection(6, 42);
    QFont fntPiModels = m_pTreePiModels->font();
    fntPiModels.setPointSize(11);
    m_pTreePiModels->setFont(fntPiModels);
    QFont fntPiModelsHdr = pHdrPiModels->font();
    fntPiModelsHdr.setPointSize(11);
    pHdrPiModels->setFont(fntPiModelsHdr);
    m_pTreePiModels->setMinimumHeight(120);
    m_pTreePiModels->setToolTip(QStringLiteral(
        "自定义模型条目（正本存 exe 同级 config.ini 的 [PiModels] 节，确定保存时合成写\n"
        "~/.pi/agent/models.json 的 autocode-<n> provider 槽位）。\n"
        "「当前」列打 ● 的条目生效，忽略上方 Provider/模型/API Key 固定路由。双击编辑"));
    connect(m_pTreePiModels, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { OnPiModelEdit(); });
    connect(m_pTreePiModels, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { RefreshPiModelList(); });
    pLayPiModels->addWidget(m_pTreePiModels);
    auto* pRowPiModelBtns = new QWidget(pGroupPiModels);
    auto* pLayPiModelBtns = new QHBoxLayout(pRowPiModelBtns);
    pLayPiModelBtns->setContentsMargins(0, 0, 0, 0);
    m_pBtnPiModelAdd = new QPushButton(QStringLiteral("新增…"), pRowPiModelBtns);
    m_pBtnPiModelAdd->setToolTip(QStringLiteral("新增一个自定义模型条目（对话框编辑各项后加入列表）"));
    connect(m_pBtnPiModelAdd, &QPushButton::clicked, this, [this]() { OnPiModelAdd(); });
    m_pBtnPiModelCopy = new QPushButton(QStringLiteral("复制…"), pRowPiModelBtns);
    m_pBtnPiModelCopy->setToolTip(QStringLiteral(
        "复制选中条目：预填编辑对话框（显示名加“-副本”，可改），确认后追加到列表末尾——"
        "适合在同一网关下快速建多个模型"));
    connect(m_pBtnPiModelCopy, &QPushButton::clicked, this, [this]() { OnPiModelCopy(); });
    m_pBtnPiModelEdit = new QPushButton(QStringLiteral("编辑…"), pRowPiModelBtns);
    m_pBtnPiModelEdit->setToolTip(QStringLiteral("编辑选中条目（双击条目同效）"));
    connect(m_pBtnPiModelEdit, &QPushButton::clicked, this, [this]() { OnPiModelEdit(); });
    m_pBtnPiModelDel = new QPushButton(QStringLiteral("删除"), pRowPiModelBtns);
    m_pBtnPiModelDel->setToolTip(QStringLiteral("删除选中条目；删的是「当前」条目时自动取消当前"));
    connect(m_pBtnPiModelDel, &QPushButton::clicked, this, [this]() { OnPiModelDel(); });
    m_pBtnPiModelActive = new QPushButton(QStringLiteral("设为当前"), pRowPiModelBtns);
    m_pBtnPiModelActive->setToolTip(QStringLiteral(
        "将选中条目设为当前生效模型（再点一次取消，回上方 Provider/模型固定路由）"));
    connect(m_pBtnPiModelActive, &QPushButton::clicked, this, [this]() { OnPiModelSetActive(); });
    pLayPiModelBtns->addWidget(m_pBtnPiModelAdd);
    pLayPiModelBtns->addWidget(m_pBtnPiModelCopy);
    pLayPiModelBtns->addWidget(m_pBtnPiModelEdit);
    pLayPiModelBtns->addWidget(m_pBtnPiModelDel);
    pLayPiModelBtns->addWidget(m_pBtnPiModelActive);
    pLayPiModelBtns->addStretch(1);
    pLayPiModels->addWidget(pRowPiModelBtns);
    auto* pLblPiModelsHint = new QLabel(QStringLiteral(
        "「当前」列打 ● 的条目生效：保存时合成写 ~/.pi/agent/models.json（autocode-<n> "
        "provider 槽位），\n启动参数 --provider/--model 取自该条目，上方 Provider/模型/API Key"
        " 不参与路由。\n条目增删改后需「确定」保存才写盘生效。API Key 以明文存 config.ini 与"
        " models.json，注意文件外发安全。"), pGroupPiModels);
    pLblPiModelsHint->setWordWrap(true);
    pLayPiModels->addWidget(pLblPiModelsHint);
    pLayPi->addWidget(pGroupPiModels);
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
    fnAddCodexPathRow(QStringLiteral("node 路径："), &m_pEditCodexNode,
                   QStringLiteral("node.exe 绝对路径（需 Node ≥ 20）；留空使用 PATH 中的 node"
                                  "（[Agent] CodexNodePath）"), false);
    m_pEditCodexNode->setPlaceholderText(QStringLiteral("留空使用 PATH 中的 node"));
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
    pFormCodex->addRow(QStringLiteral("Provider："), m_pEditCodexProvider);
    m_pComboCodexWireApi = new QComboBox(pGroupCodex);
    m_pComboCodexWireApi->addItem(QStringLiteral("chat"));
    m_pComboCodexWireApi->addItem(QStringLiteral("responses"));
    m_pComboCodexWireApi->setToolTip(QStringLiteral(
        "wire_api（写入 ~/.codex 的 provider 槽位；[Agent] CodexWireApi，默认 chat）。"
        "v0.88+ 已移除 chat，本后端锁定 0.87.0"));
    pFormCodex->addRow(QStringLiteral("WireApi："), m_pComboCodexWireApi);
    pLayCodex->addWidget(pGroupCodex);

    // —— CodexHarness 自定义大模型（2026.08.21，口径仿 DSH/Pi 页）——
    //   条目经 ~/.codex/config.toml 的 autocode-<n> provider 槽位 + auth.json 生效；
    //   激活条目覆盖上方 Provider/模型/BaseUrl/WireApi/APIKey 固定路由。
    auto* pGroupCodexModels = new QGroupBox(QStringLiteral("自定义大模型"), pTabCodex);
    auto* pLayCodexModels = new QVBoxLayout(pGroupCodexModels);
    m_pTreeCodexModels = new QTreeWidget(pGroupCodexModels);
    m_pTreeCodexModels->setColumnCount(7);
    m_pTreeCodexModels->setHeaderLabels(QStringList()
        << QStringLiteral("显示名")
        << QStringLiteral("请求地址")
        << QStringLiteral("wire_api")
        << QStringLiteral("模型名称")
        << QStringLiteral("上下文")
        << QStringLiteral("最大输出")
        << QStringLiteral("当前"));
    m_pTreeCodexModels->setRootIsDecorated(false);
    m_pTreeCodexModels->setAlternatingRowColors(true);
    QHeaderView* pHdrCodexModels = m_pTreeCodexModels->header();
    pHdrCodexModels->setSectionsMovable(false);
    pHdrCodexModels->setSectionResizeMode(QHeaderView::Interactive);
    pHdrCodexModels->setSectionResizeMode(1, QHeaderView::Stretch);
    pHdrCodexModels->resizeSection(0, 130);
    pHdrCodexModels->resizeSection(2, 90);
    pHdrCodexModels->resizeSection(3, 100);
    pHdrCodexModels->resizeSection(4, 75);
    pHdrCodexModels->resizeSection(5, 75);
    pHdrCodexModels->resizeSection(6, 42);
    QFont fntCodexModels = m_pTreeCodexModels->font();
    fntCodexModels.setPointSize(11);
    m_pTreeCodexModels->setFont(fntCodexModels);
    QFont fntCodexModelsHdr = pHdrCodexModels->font();
    fntCodexModelsHdr.setPointSize(11);
    pHdrCodexModels->setFont(fntCodexModelsHdr);
    m_pTreeCodexModels->setMinimumHeight(120);
    m_pTreeCodexModels->setToolTip(QStringLiteral(
        "自定义模型条目（正本存 exe 同级 config.ini 的 [CodexModels] 节，确定保存时合成写\n"
        "~/.codex 的 config.toml autocode-<n> provider 槽位与 auth.json）。\n"
        "「当前」列打 ● 的条目生效，忽略上方 Provider/模型/API Key/BaseUrl/WireApi 固定路由。"
        "双击编辑"));
    connect(m_pTreeCodexModels, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { OnCodexModelEdit(); });
    connect(m_pTreeCodexModels, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { RefreshCodexModelList(); });
    pLayCodexModels->addWidget(m_pTreeCodexModels);
    auto* pRowCodexModelBtns = new QWidget(pGroupCodexModels);
    auto* pLayCodexModelBtns = new QHBoxLayout(pRowCodexModelBtns);
    pLayCodexModelBtns->setContentsMargins(0, 0, 0, 0);
    m_pBtnCodexModelAdd = new QPushButton(QStringLiteral("新增…"), pRowCodexModelBtns);
    m_pBtnCodexModelAdd->setToolTip(QStringLiteral(
        "新增一个自定义模型条目（对话框编辑各项后加入列表）"));
    connect(m_pBtnCodexModelAdd, &QPushButton::clicked, this, [this]() { OnCodexModelAdd(); });
    m_pBtnCodexModelCopy = new QPushButton(QStringLiteral("复制…"), pRowCodexModelBtns);
    m_pBtnCodexModelCopy->setToolTip(QStringLiteral(
        "复制选中条目：预填编辑对话框（显示名加“-副本”，可改），确认后追加到列表末尾——"
        "适合在同一网关下快速建多个模型"));
    connect(m_pBtnCodexModelCopy, &QPushButton::clicked, this, [this]() { OnCodexModelCopy(); });
    m_pBtnCodexModelEdit = new QPushButton(QStringLiteral("编辑…"), pRowCodexModelBtns);
    m_pBtnCodexModelEdit->setToolTip(QStringLiteral("编辑选中条目（双击条目同效）"));
    connect(m_pBtnCodexModelEdit, &QPushButton::clicked, this, [this]() { OnCodexModelEdit(); });
    m_pBtnCodexModelDel = new QPushButton(QStringLiteral("删除"), pRowCodexModelBtns);
    m_pBtnCodexModelDel->setToolTip(QStringLiteral(
        "删除选中条目；删的是「当前」条目时自动取消当前"));
    connect(m_pBtnCodexModelDel, &QPushButton::clicked, this, [this]() { OnCodexModelDel(); });
    m_pBtnCodexModelActive = new QPushButton(QStringLiteral("设为当前"), pRowCodexModelBtns);
    m_pBtnCodexModelActive->setToolTip(QStringLiteral(
        "将选中条目设为当前生效模型（再点一次取消，回上方固定路由）"));
    connect(m_pBtnCodexModelActive, &QPushButton::clicked,
            this, [this]() { OnCodexModelSetActive(); });
    pLayCodexModelBtns->addWidget(m_pBtnCodexModelAdd);
    pLayCodexModelBtns->addWidget(m_pBtnCodexModelCopy);
    pLayCodexModelBtns->addWidget(m_pBtnCodexModelEdit);
    pLayCodexModelBtns->addWidget(m_pBtnCodexModelDel);
    pLayCodexModelBtns->addWidget(m_pBtnCodexModelActive);
    pLayCodexModelBtns->addStretch(1);
    pLayCodexModels->addWidget(pRowCodexModelBtns);
    auto* pLblCodexModelsHint = new QLabel(QStringLiteral(
        "「当前」列打 ● 的条目生效：保存时合成写 ~/.codex（config.toml 的 autocode-<n> "
        "provider 槽位 + auth.json），\n启动参数 model_provider/model 取自该条目，上方 "
        "Provider/模型/API Key/BaseUrl/WireApi 不参与路由。\n条目增删改后需「确定」保存"
        "才写盘生效。API Key 以明文存 config.ini 与 ~/.codex，注意文件外发安全。"),
        pGroupCodexModels);
    pLblCodexModelsHint->setWordWrap(true);
    pLayCodexModels->addWidget(pLblCodexModelsHint);
    pLayCodex->addWidget(pGroupCodexModels);
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
    fnAddHermesPathRow(QStringLiteral("node 路径："), &m_pEditHermesNode,
                       QStringLiteral("node.exe 绝对路径；留空使用 PATH 中的 node"
                                      "（[Agent] HermesNodePath）"), false);
    m_pEditHermesNode->setPlaceholderText(QStringLiteral("留空使用 PATH 中的 node"));
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
    pLayHermes->addWidget(pGroupHermes);
    // 自定义大模型（[HermesModels]，7 列口径同 Pi/Codex 树）。
    auto* pGroupHermesModels = new QGroupBox(QStringLiteral("自定义大模型"), pTabHermes);
    auto* pLayHermesModels = new QVBoxLayout(pGroupHermesModels);
    m_pTreeHermesModels = new QTreeWidget(pGroupHermesModels);
    m_pTreeHermesModels->setColumnCount(7);
    m_pTreeHermesModels->setHeaderLabels(QStringList()
        << QStringLiteral("显示名")
        << QStringLiteral("请求地址")
        << QStringLiteral("API 格式")
        << QStringLiteral("模型名称")
        << QStringLiteral("上下文")
        << QStringLiteral("最大输出")
        << QStringLiteral("当前"));
    m_pTreeHermesModels->setRootIsDecorated(false);
    m_pTreeHermesModels->setAlternatingRowColors(true);
    QHeaderView* pHdrHermesModels = m_pTreeHermesModels->header();
    pHdrHermesModels->setSectionsMovable(false);
    pHdrHermesModels->setSectionResizeMode(QHeaderView::Interactive);
    pHdrHermesModels->setSectionResizeMode(1, QHeaderView::Stretch);
    pHdrHermesModels->resizeSection(0, 130);
    pHdrHermesModels->resizeSection(2, 125);
    pHdrHermesModels->resizeSection(3, 100);
    pHdrHermesModels->resizeSection(4, 75);
    pHdrHermesModels->resizeSection(5, 75);
    pHdrHermesModels->resizeSection(6, 42);
    QFont fntHermesModels = m_pTreeHermesModels->font();
    fntHermesModels.setPointSize(11);
    m_pTreeHermesModels->setFont(fntHermesModels);
    QFont fntHermesHdr = pHdrHermesModels->font();
    fntHermesHdr.setPointSize(11);
    pHdrHermesModels->setFont(fntHermesHdr);
    m_pTreeHermesModels->setMinimumHeight(120);
    m_pTreeHermesModels->setToolTip(QStringLiteral(
        "自定义模型条目（正本存 config.ini 的 [HermesModels] 节）。\n"
        "「当前」列打 ● 的条目生效，忽略上方模型/API Key 固定路由。双击编辑。\n"
        "最大输出上限 131072（网关 2^17 上限，保存时钳位）"));
    connect(m_pTreeHermesModels, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) { OnHermesModelEdit(); });
    connect(m_pTreeHermesModels, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { RefreshHermesModelList(); });
    pLayHermesModels->addWidget(m_pTreeHermesModels);
    auto* pRowHermesModelBtns = new QWidget(pGroupHermesModels);
    auto* pLayHermesModelBtns = new QHBoxLayout(pRowHermesModelBtns);
    pLayHermesModelBtns->setContentsMargins(0, 0, 0, 0);
    m_pBtnHermesModelAdd = new QPushButton(QStringLiteral("新增…"), pRowHermesModelBtns);
    connect(m_pBtnHermesModelAdd, &QPushButton::clicked, this, [this]() { OnHermesModelAdd(); });
    m_pBtnHermesModelCopy = new QPushButton(QStringLiteral("复制…"), pRowHermesModelBtns);
    connect(m_pBtnHermesModelCopy, &QPushButton::clicked, this, [this]() { OnHermesModelCopy(); });
    m_pBtnHermesModelEdit = new QPushButton(QStringLiteral("编辑…"), pRowHermesModelBtns);
    connect(m_pBtnHermesModelEdit, &QPushButton::clicked, this, [this]() { OnHermesModelEdit(); });
    m_pBtnHermesModelDel = new QPushButton(QStringLiteral("删除"), pRowHermesModelBtns);
    connect(m_pBtnHermesModelDel, &QPushButton::clicked, this, [this]() { OnHermesModelDel(); });
    m_pBtnHermesModelActive = new QPushButton(QStringLiteral("设为当前"), pRowHermesModelBtns);
    connect(m_pBtnHermesModelActive, &QPushButton::clicked,
            this, [this]() { OnHermesModelSetActive(); });
    pLayHermesModelBtns->addWidget(m_pBtnHermesModelAdd);
    pLayHermesModelBtns->addWidget(m_pBtnHermesModelCopy);
    pLayHermesModelBtns->addWidget(m_pBtnHermesModelEdit);
    pLayHermesModelBtns->addWidget(m_pBtnHermesModelDel);
    pLayHermesModelBtns->addWidget(m_pBtnHermesModelActive);
    pLayHermesModelBtns->addStretch(1);
    pLayHermesModels->addWidget(pRowHermesModelBtns);
    auto* pLblHermesHint = new QLabel(QStringLiteral(
        "Hermes（第五后端）以独立 CLI 子进程 + 网关调用接入（一阶段一进程，NDJSON 流式）。\n"
        "启动时幂等写 ~/.hermes/agent/config.json（contextWindow/200000、maxTokens/131072 "
        "钳位）。\nAPI Key 明文存 config.ini 与 ~/.hermes，注意文件外发安全。"), pTabHermes);
    pLblHermesHint->setWordWrap(true);
    pLayHermes->addWidget(pGroupHermesModels);
    pLayHermes->addWidget(pLblHermesHint);
    pLayHermes->addStretch(1);

    pTabs->addTab(pTabCommon, QStringLiteral("常用"));
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
    if (m_pTreeDshModels != nullptr)   { m_pTreeDshModels->setEnabled(bDsh); }
    // 2026.08.18 后端专属项反向联动：DSH/PIgent 后端下这四项对运行无效果——claude 命令/
    //   提问等待超时只进 CClaudeRunner（DSH/Pi 用 node 启动、无 ask_user MCP 工具），
    //   联网检索开关与自动压缩比只折算进 claude 专属启动参数（两后端侧忽略并降级提示）。
    //   选中即禁用防误导，切回 claude 自动恢复。「上下文窗口上限」是后端中立项，保持可用。
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
    UpdateDshEnabled();
}

void CSettingsDialog::OnDshModelSetActive() {
    const int iRow = CurDshModelRow();
    if (iRow < 0 || iRow >= m_aDshModels.size()) {
        return;
    }
    m_iDshActiveModel = (m_iDshActiveModel == iRow) ? -1 : iRow;   // 再点一次取消
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
    if (m_pTreePiModels != nullptr)  { m_pTreePiModels->setEnabled(bPi); }
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
    RefreshPiModelList();
}

// 2026.08.20 PiAgent 自定义大模型列表刷新（口径同 RefreshDshModelList：重建多列
//   条目行、当前列 ● 标记、空表占位行，末尾按选中行与组级开关刷按钮态）。
void CSettingsDialog::RefreshPiModelList() {
    if (m_pTreePiModels == nullptr) {
        return;
    }
    const int iSel = CurPiModelRow();
    m_pTreePiModels->blockSignals(true);
    m_pTreePiModels->clear();
    for (int i = 0; i < m_aPiModels.size(); ++i) {
        const SDshModelEntry& stE = m_aPiModels.at(i);
        auto* pItem = new QTreeWidgetItem(QStringList()
            << stE.sDisp
            << stE.sBaseURL
            << stE.sApi
            << stE.sModelId
            << QString::number(stE.iCtx)
            << QString::number(stE.iMaxTok)
            << (i == m_iPiActiveModel ? QStringLiteral("●") : QString()));
        pItem->setToolTip(0, QStringLiteral("双击编辑；API Key：%1").arg(
            stE.sApiKey.isEmpty() ? QStringLiteral("未配置")
                                  : QStringLiteral("已配置（明文存 config.ini / models.json）")));
        pItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(6, Qt::AlignCenter);
        m_pTreePiModels->addTopLevelItem(pItem);
    }
    if (m_aPiModels.isEmpty()) {
        auto* pItem = new QTreeWidgetItem(QStringList{
            QStringLiteral("（空：使用上方 Provider 固定路由）")});
        pItem->setDisabled(true);
        m_pTreePiModels->addTopLevelItem(pItem);
    }
    if (iSel >= 0 && iSel < m_pTreePiModels->topLevelItemCount()) {
        m_pTreePiModels->setCurrentItem(m_pTreePiModels->topLevelItem(iSel));
    }
    m_pTreePiModels->blockSignals(false);
    const int iRow = CurPiModelRow();
    const bool bHasSel = (iRow >= 0 && iRow < m_aPiModels.size());
    const bool bEnabled = m_pTreePiModels->isEnabled();
    if (m_pBtnPiModelAdd != nullptr)  { m_pBtnPiModelAdd->setEnabled(bEnabled); }
    if (m_pBtnPiModelCopy != nullptr) { m_pBtnPiModelCopy->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnPiModelEdit != nullptr) { m_pBtnPiModelEdit->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnPiModelDel != nullptr)  { m_pBtnPiModelDel->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnPiModelActive != nullptr) {
        m_pBtnPiModelActive->setEnabled(bEnabled && bHasSel);
        m_pBtnPiModelActive->setText((bHasSel && iRow == m_iPiActiveModel)
            ? QStringLiteral("取消当前") : QStringLiteral("设为当前"));
    }
}

int CSettingsDialog::CurPiModelRow() const {
    if (m_pTreePiModels == nullptr) {
        return -1;
    }
    return m_pTreePiModels->indexOfTopLevelItem(m_pTreePiModels->currentItem());
}

void CSettingsDialog::OnPiModelAdd() {
    CDshModelEditDialog dlg(this, nullptr, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aPiModels.append(dlg.Entry());
    if (m_aPiModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iPiActiveModel = 0;
    }
    RefreshPiModelList();
    if (m_pTreePiModels->topLevelItemCount() > 0) {
        m_pTreePiModels->setCurrentItem(
            m_pTreePiModels->topLevelItem(m_pTreePiModels->topLevelItemCount() - 1));
    }
    UpdatePiEnabled();
}

// 2026.08.27 复制选中条目（口径同 DSH 页）：预填编辑对话框，确认后追加末尾。
void CSettingsDialog::OnPiModelCopy() {
    const int iRow = CurPiModelRow();
    if (iRow < 0 || iRow >= m_aPiModels.size()) {
        return;
    }
    SDshModelEntry stCopy = m_aPiModels.at(iRow);
    stCopy.sDisp += QStringLiteral("-副本");
    CDshModelEditDialog dlg(this, &stCopy, true, false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aPiModels.append(dlg.Entry());
    if (m_aPiModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iPiActiveModel = 0;
    }
    RefreshPiModelList();
    if (m_pTreePiModels->topLevelItemCount() > 0) {
        m_pTreePiModels->setCurrentItem(
            m_pTreePiModels->topLevelItem(m_pTreePiModels->topLevelItemCount() - 1));
    }
    UpdatePiEnabled();
}

void CSettingsDialog::OnPiModelEdit() {
    const int iRow = CurPiModelRow();
    if (iRow < 0 || iRow >= m_aPiModels.size()) {
        return;
    }
    CDshModelEditDialog dlg(this, &m_aPiModels[iRow], true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aPiModels[iRow] = dlg.Entry();
    UpdatePiEnabled();
}

void CSettingsDialog::OnPiModelDel() {
    const int iRow = CurPiModelRow();
    if (iRow < 0 || iRow >= m_aPiModels.size()) {
        return;
    }
    const SDshModelEntry& stE = m_aPiModels.at(iRow);
    if (QMessageBox::question(this, QStringLiteral("自定义模型"),
            QStringLiteral("删除条目「%1」？").arg(stE.sDisp)) != QMessageBox::Yes) {
        return;
    }
    m_aPiModels.removeAt(iRow);
    if (m_iPiActiveModel == iRow) {
        m_iPiActiveModel = -1;
    } else if (m_iPiActiveModel > iRow) {
        --m_iPiActiveModel;
    }
    if (m_aPiModels.size() == 1) {      // 仅剩一条 → 自动设为缺省
        m_iPiActiveModel = 0;
    }
    UpdatePiEnabled();
}

void CSettingsDialog::OnPiModelSetActive() {
    const int iRow = CurPiModelRow();
    if (iRow < 0 || iRow >= m_aPiModels.size()) {
        return;
    }
    m_iPiActiveModel = (m_iPiActiveModel == iRow) ? -1 : iRow;
    UpdatePiEnabled();
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
    // 探测安装：根目录框显式值优先（路径存在且 cli.js 在 → 装好；在但缺 cli.js → 半装态
    //   报诊断），否则 ProbeSiblingRoot 兄弟目录自动探测，命中且根目录为空自动填入（并持久化）。
    const bool bRootSet = m_pEditPiRoot != nullptr && !m_pEditPiRoot->text().trimmed().isEmpty();
    const QString sExplicit = bRootSet ? m_pEditPiRoot->text().trimmed() : QString();
    if (bRootSet) {
        m_bPiInstalled = CPiAgentRunner::LooksLikePiRoot(sExplicit);
    } else {
        const QString sProbed = CPiAgentRunner::ProbeSiblingRoot();
        m_bPiInstalled = !sProbed.isEmpty();
        if (m_bPiInstalled && m_pEditPiRoot != nullptr) {
            m_pEditPiRoot->setText(QDir::toNativeSeparators(sProbed));   // 自动回填
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
    return m_pEditDshNode->text().trimmed();
}

QString CSettingsDialog::DshRoot() const {
    return m_pEditDshRoot->text().trimmed();
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

double CSettingsDialog::PiPriceInPerM() const {
    return m_pSpinPiPriceIn != nullptr ? m_pSpinPiPriceIn->value() : 0.0;
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

int CSettingsDialog::AutoCompactPct() const {
    return m_pSpinCompactPct->value();
}

qint64 CSettingsDialog::ContextLimit() const {
    return static_cast<qint64>(m_pSpinCtxLimit->value()) * 1000;
}

QString CSettingsDialog::ScanExcludeDirs() const {
    return m_pEditScanDirs->text().trimmed();
}

QString CSettingsDialog::BackupExcludeDirs() const {
    return m_pEditBackupDirs->text().trimmed();
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
    if (m_pTreeCodexModels != nullptr)   { m_pTreeCodexModels->setEnabled(bCodexSel); }
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
    RefreshCodexModelList();
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

// —— 2026.08.21 Codex 自定义大模型列表维护（口径同 Pi 页同名方法）——
void CSettingsDialog::RefreshCodexModelList() {
    if (m_pTreeCodexModels == nullptr) {
        return;
    }
    const int iSel = CurCodexModelRow();
    m_pTreeCodexModels->blockSignals(true);
    m_pTreeCodexModels->clear();
    for (int i = 0; i < m_aCodexModels.size(); ++i) {
        const SDshModelEntry& stE = m_aCodexModels.at(i);
        auto* pItem = new QTreeWidgetItem(QStringList()
            << stE.sDisp
            << stE.sBaseURL
            << stE.sApi
            << stE.sModelId
            << QString::number(stE.iCtx)
            << QString::number(stE.iMaxTok)
            << (i == m_iCodexActiveModel ? QStringLiteral("●") : QString()));
        pItem->setToolTip(0, QStringLiteral("双击编辑；API Key：%1").arg(
            stE.sApiKey.isEmpty() ? QStringLiteral("未配置")
                                  : QStringLiteral("已配置（明文存 config.ini / ~/.codex）")));
        pItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(6, Qt::AlignCenter);
        m_pTreeCodexModels->addTopLevelItem(pItem);
    }
    if (m_aCodexModels.isEmpty()) {
        auto* pItem = new QTreeWidgetItem(QStringList{
            QStringLiteral("（空：使用上方 Provider 固定路由）")});
        pItem->setDisabled(true);
        m_pTreeCodexModels->addTopLevelItem(pItem);
    }
    if (iSel >= 0 && iSel < m_pTreeCodexModels->topLevelItemCount()) {
        m_pTreeCodexModels->setCurrentItem(m_pTreeCodexModels->topLevelItem(iSel));
    }
    m_pTreeCodexModels->blockSignals(false);
    const int iRow = CurCodexModelRow();
    const bool bHasSel = (iRow >= 0 && iRow < m_aCodexModels.size());
    const bool bEnabled = m_pTreeCodexModels->isEnabled();
    if (m_pBtnCodexModelAdd != nullptr)  { m_pBtnCodexModelAdd->setEnabled(bEnabled); }
    if (m_pBtnCodexModelCopy != nullptr) { m_pBtnCodexModelCopy->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnCodexModelEdit != nullptr) { m_pBtnCodexModelEdit->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnCodexModelDel != nullptr)  { m_pBtnCodexModelDel->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnCodexModelActive != nullptr) {
        m_pBtnCodexModelActive->setEnabled(bEnabled && bHasSel);
        m_pBtnCodexModelActive->setText((bHasSel && iRow == m_iCodexActiveModel)
            ? QStringLiteral("取消当前") : QStringLiteral("设为当前"));
    }
}

int CSettingsDialog::CurCodexModelRow() const {
    if (m_pTreeCodexModels == nullptr) {
        return -1;
    }
    return m_pTreeCodexModels->indexOfTopLevelItem(m_pTreeCodexModels->currentItem());
}

void CSettingsDialog::OnCodexModelAdd() {
    CDshModelEditDialog dlg(this, nullptr, false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aCodexModels.append(dlg.Entry());
    if (m_aCodexModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iCodexActiveModel = 0;
    }
    RefreshCodexModelList();
    if (m_pTreeCodexModels->topLevelItemCount() > 0) {
        m_pTreeCodexModels->setCurrentItem(
            m_pTreeCodexModels->topLevelItem(m_pTreeCodexModels->topLevelItemCount() - 1));
    }
    UpdateCodexEnabled();
}

// 2026.08.27 复制选中条目（口径同 DSH 页）：预填编辑对话框，确认后追加末尾。
void CSettingsDialog::OnCodexModelCopy() {
    const int iRow = CurCodexModelRow();
    if (iRow < 0 || iRow >= m_aCodexModels.size()) {
        return;
    }
    SDshModelEntry stCopy = m_aCodexModels.at(iRow);
    stCopy.sDisp += QStringLiteral("-副本");
    CDshModelEditDialog dlg(this, &stCopy, false, true, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aCodexModels.append(dlg.Entry());
    if (m_aCodexModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iCodexActiveModel = 0;
    }
    RefreshCodexModelList();
    if (m_pTreeCodexModels->topLevelItemCount() > 0) {
        m_pTreeCodexModels->setCurrentItem(
            m_pTreeCodexModels->topLevelItem(m_pTreeCodexModels->topLevelItemCount() - 1));
    }
    UpdateCodexEnabled();
}

void CSettingsDialog::OnCodexModelEdit() {
    const int iRow = CurCodexModelRow();
    if (iRow < 0 || iRow >= m_aCodexModels.size()) {
        return;
    }
    CDshModelEditDialog dlg(this, &m_aCodexModels[iRow], false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aCodexModels[iRow] = dlg.Entry();
    UpdateCodexEnabled();
}

void CSettingsDialog::OnCodexModelDel() {
    const int iRow = CurCodexModelRow();
    if (iRow < 0 || iRow >= m_aCodexModels.size()) {
        return;
    }
    const SDshModelEntry& stE = m_aCodexModels.at(iRow);
    if (QMessageBox::question(this, QStringLiteral("自定义模型"),
            QStringLiteral("删除条目「%1」？").arg(stE.sDisp)) != QMessageBox::Yes) {
        return;
    }
    m_aCodexModels.removeAt(iRow);
    if (m_iCodexActiveModel == iRow) {
        m_iCodexActiveModel = -1;
    } else if (m_iCodexActiveModel > iRow) {
        --m_iCodexActiveModel;
    }
    if (m_aCodexModels.size() == 1) {      // 仅剩一条 → 自动设为缺省
        m_iCodexActiveModel = 0;
    }
    RefreshCodexModelList();
    UpdateCodexEnabled();
}

void CSettingsDialog::OnCodexModelSetActive() {
    const int iRow = CurCodexModelRow();
    if (iRow < 0 || iRow >= m_aCodexModels.size()) {
        return;
    }
    m_iCodexActiveModel = (m_iCodexActiveModel == iRow) ? -1 : iRow;
    RefreshCodexModelList();
    UpdateCodexEnabled();
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
    if (m_pTreeHermesModels != nullptr) { m_pTreeHermesModels->setEnabled(bHermes); }
    RefreshHermesModelList();
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
    const QString sScript = QCoreApplication::applicationDirPath()
        + QStringLiteral("/install_env.bat");
    if (!QFile::exists(sScript)) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
            QStringLiteral("找不到安装脚本：\n%1\n\n请确认程序目录完整（安装脚本随"
                           "构建同步到 exe 同级）。").arg(QDir::toNativeSeparators(sScript)));
        return;
    }
    const QString sArgs = QStringLiteral("/k \"")
        + QDir::toNativeSeparators(sScript) + QStringLiteral("\" /hermes \"")
        + sRepo + QStringLiteral("\"");
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

void CSettingsDialog::RefreshHermesModelList() {
    if (m_pTreeHermesModels == nullptr) {
        return;
    }
    const int iSel = CurHermesModelRow();
    m_pTreeHermesModels->blockSignals(true);
    m_pTreeHermesModels->clear();
    for (int i = 0; i < m_aHermesModels.size(); ++i) {
        const SDshModelEntry& stE = m_aHermesModels.at(i);
        auto* pItem = new QTreeWidgetItem(QStringList()
            << stE.sDisp
            << stE.sBaseURL
            << stE.sApi
            << stE.sModelId
            << QString::number(stE.iCtx)
            << QString::number(stE.iMaxTok)
            << (i == m_iHermesActiveModel ? QStringLiteral("●") : QString()));
        pItem->setToolTip(0, QStringLiteral("双击编辑；API Key：%1").arg(
            stE.sApiKey.isEmpty() ? QStringLiteral("未配置")
                                  : QStringLiteral("已配置（明文存 config.ini）")));
        pItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(6, Qt::AlignCenter);
        m_pTreeHermesModels->addTopLevelItem(pItem);
    }
    if (m_aHermesModels.isEmpty()) {
        auto* pItem = new QTreeWidgetItem(QStringList{
            QStringLiteral("（空：使用上方模型列表当前条目）")});
        pItem->setDisabled(true);
        m_pTreeHermesModels->addTopLevelItem(pItem);
    }
    if (iSel >= 0 && iSel < m_pTreeHermesModels->topLevelItemCount()) {
        m_pTreeHermesModels->setCurrentItem(m_pTreeHermesModels->topLevelItem(iSel));
    }
    m_pTreeHermesModels->blockSignals(false);
    const int iRow = CurHermesModelRow();
    const bool bHasSel = (iRow >= 0 && iRow < m_aHermesModels.size());
    const bool bEnabled = m_pTreeHermesModels->isEnabled();
    if (m_pBtnHermesModelAdd != nullptr)  { m_pBtnHermesModelAdd->setEnabled(bEnabled); }
    if (m_pBtnHermesModelCopy != nullptr) { m_pBtnHermesModelCopy->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnHermesModelEdit != nullptr) { m_pBtnHermesModelEdit->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnHermesModelDel != nullptr)  { m_pBtnHermesModelDel->setEnabled(bEnabled && bHasSel); }
    if (m_pBtnHermesModelActive != nullptr) {
        m_pBtnHermesModelActive->setEnabled(bEnabled && bHasSel);
        m_pBtnHermesModelActive->setText((bHasSel && iRow == m_iHermesActiveModel)
            ? QStringLiteral("取消当前") : QStringLiteral("设为当前"));
    }
}

int CSettingsDialog::CurHermesModelRow() const {
    if (m_pTreeHermesModels == nullptr) {
        return -1;
    }
    return m_pTreeHermesModels->indexOfTopLevelItem(m_pTreeHermesModels->currentItem());
}

void CSettingsDialog::OnHermesModelAdd() {
    CDshModelEditDialog dlg(this, nullptr, false, false, false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aHermesModels.append(dlg.Entry());
    if (m_aHermesModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iHermesActiveModel = 0;
    }
    RefreshHermesModelList();
    if (m_pTreeHermesModels->topLevelItemCount() > 0) {
        m_pTreeHermesModels->setCurrentItem(
            m_pTreeHermesModels->topLevelItem(m_pTreeHermesModels->topLevelItemCount() - 1));
    }
    UpdateHermesEnabled();
}

void CSettingsDialog::OnHermesModelCopy() {
    const int iRow = CurHermesModelRow();
    if (iRow < 0 || iRow >= m_aHermesModels.size()) {
        return;
    }
    SDshModelEntry stCopy = m_aHermesModels.at(iRow);
    stCopy.sDisp += QStringLiteral("-副本");
    CDshModelEditDialog dlg(this, &stCopy, false, false, true, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aHermesModels.append(dlg.Entry());
    if (m_aHermesModels.size() == 1) {   // 仅此一条 → 自动设为缺省
        m_iHermesActiveModel = 0;
    }
    RefreshHermesModelList();
    if (m_pTreeHermesModels->topLevelItemCount() > 0) {
        m_pTreeHermesModels->setCurrentItem(
            m_pTreeHermesModels->topLevelItem(m_pTreeHermesModels->topLevelItemCount() - 1));
    }
    UpdateHermesEnabled();
}

void CSettingsDialog::OnHermesModelEdit() {
    const int iRow = CurHermesModelRow();
    if (iRow < 0 || iRow >= m_aHermesModels.size()) {
        return;
    }
    CDshModelEditDialog dlg(this, &m_aHermesModels[iRow], false, false, false, true);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    m_aHermesModels[iRow] = dlg.Entry();
    UpdateHermesEnabled();
}

void CSettingsDialog::OnHermesModelDel() {
    const int iRow = CurHermesModelRow();
    if (iRow < 0 || iRow >= m_aHermesModels.size()) {
        return;
    }
    const SDshModelEntry& stE = m_aHermesModels.at(iRow);
    if (QMessageBox::question(this, QStringLiteral("自定义模型"),
            QStringLiteral("删除条目「%1」？").arg(stE.sDisp)) != QMessageBox::Yes) {
        return;
    }
    m_aHermesModels.removeAt(iRow);
    if (m_iHermesActiveModel == iRow) {
        m_iHermesActiveModel = -1;
    } else if (m_iHermesActiveModel > iRow) {
        --m_iHermesActiveModel;
    }
    if (m_aHermesModels.size() == 1) {      // 仅剩一条 → 自动设为缺省
        m_iHermesActiveModel = 0;
    }
    UpdateHermesEnabled();
}

void CSettingsDialog::OnHermesModelSetActive() {
    const int iRow = CurHermesModelRow();
    if (iRow < 0 || iRow >= m_aHermesModels.size()) {
        return;
    }
    m_iHermesActiveModel = (m_iHermesActiveModel == iRow) ? -1 : iRow;
    UpdateHermesEnabled();
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
