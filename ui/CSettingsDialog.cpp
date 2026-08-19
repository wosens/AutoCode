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
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

#include "CDeepSeekRunner.h"
#include "CProjectManager.h"

// 2026.08.18 自定义大模型条目编辑对话框（无 Q_OBJECT：纯表单 + exec/lambda 连接，
//   不进 moc/构建系统）。校验：模型 ID / 接入点必填（空则拦截 accept 并提示）；
//   其余空值在 Entry() 回默认（API 类型→anthropic-messages，显示名→模型 ID）。
class CDshModelEditDialog : public QDialog {
public:
    // a_pEntry = nullptr → 新增模式（全默认值）；非空 → 编辑模式回填。
    explicit CDshModelEditDialog(QWidget* a_pParent, const SDshModelEntry* a_pEntry)
        : QDialog(a_pParent) {
        setWindowTitle(a_pEntry != nullptr ? QStringLiteral("编辑自定义模型")
                                           : QStringLiteral("新增自定义模型"));
        setMinimumWidth(460);
        auto* pForm = new QFormLayout(this);
        m_pEditDisp = new QLineEdit(this);
        m_pEditDisp->setPlaceholderText(QStringLiteral("如：公司中转 GLM-5.2"));
        m_pEditDisp->setToolTip(QStringLiteral("列表中显示的名字（仅展示用）"));
        pForm->addRow(QStringLiteral("显示名："), m_pEditDisp);
        m_pEditURL = new QLineEdit(this);
        m_pEditURL->setPlaceholderText(QStringLiteral("http://192.168.x.x:9000/claude"));
        m_pEditURL->setToolTip(QStringLiteral(
            "接入点（anthropic-messages 兼容网关的 Base URL；模型 ID 与接入点为必填项）"));
        pForm->addRow(QStringLiteral("接入点："), m_pEditURL);
        m_pEditModel = new QLineEdit(this);
        m_pEditModel->setPlaceholderText(QStringLiteral("glm-5.2"));
        m_pEditModel->setToolTip(QStringLiteral("模型 ID（发给网关的 model 参数；必填）"));
        pForm->addRow(QStringLiteral("模型 ID："), m_pEditModel);
        m_pEditKey = new QLineEdit(this);
        m_pEditKey->setPlaceholderText(QStringLiteral("sk-…（网关不校验可留空）"));
        m_pEditKey->setToolTip(QStringLiteral(
            "API 密钥。注意：以明文保存在 exe 同级 config.ini 的 [DshModels] 节，"
            "请勿把该文件明文外传到共享目录"));
        pForm->addRow(QStringLiteral("API Key："), m_pEditKey);
        // 2026.08.19 API 类型改下拉（不可自由输入）：候选=pi-ai 库支持的接入点形态，
        //   默认 anthropic-messages（内网中继标准形态）；旧档自由文本值不在候选中时
        //   动态补入并选中（不丢用户已存配置）。
        m_pEditApi = new QComboBox(this);
        m_pEditApi->addItems(QStringList()
            << QStringLiteral("anthropic-messages")
            << QStringLiteral("openai-completions")
            << QStringLiteral("openai-responses")
            << QStringLiteral("google-generative-ai")
            << QStringLiteral("google-vertex"));
        m_pEditApi->setToolTip(QStringLiteral(
            "API 类型（组合 llm-pi-ai 的 api 形态）。内网 anthropic-messages 兼容中继选默认项；"
            "OpenAI 兼容网关选 openai-completions"));
        pForm->addRow(QStringLiteral("API 类型："), m_pEditApi);
        m_pSpinCtx = new QSpinBox(this);
        m_pSpinCtx->setRange(1000, 10000000);
        m_pSpinCtx->setSingleStep(10000);
        m_pSpinCtx->setValue(200000);
        m_pSpinCtx->setToolTip(QStringLiteral(
            "该模型的上下文窗口（token），用于上下文占用估算与压缩阈值"));
        pForm->addRow(QStringLiteral("上下文窗口(token)："), m_pSpinCtx);
        m_pSpinMax = new QSpinBox(this);
        m_pSpinMax->setRange(1024, 1000000);
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
            stE.sApi = QStringLiteral("anthropic-messages");
        }
        if (stE.sDisp.isEmpty()) {
            stE.sDisp = stE.sModelId;
        }
        stE.iCtx    = m_pSpinCtx->value();
        stE.iMaxTok = m_pSpinMax->value();
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
};

CSettingsDialog::CSettingsDialog(CProjectManager* a_pPm, QWidget* a_pParent)
    : QDialog(a_pParent) {
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
    const QString sDefScan = (a_pPm != nullptr) ? a_pPm->LoadScanExcludeDirs() : QString();
    const QString sDefBackup = (a_pPm != nullptr) ? a_pPm->LoadBackupExcludeDirs() : QString();
    // 2026.08.18 Agent 后端 + DSH 参数回填（默认 1=claude；路径空=未配置）。
    const int iDefBackend = (a_pPm != nullptr) ? a_pPm->LoadAgentBackend() : 1;
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
    const QString sDefDshModel = (a_pPm != nullptr) ? a_pPm->LoadDshModel()
                                                    : QStringLiteral("glm-5.2");
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
    m_pEditDshModel->setText(sDefDshModel);
    m_pSpinDshPriceIn->setValue(dDefPriceIn);
    m_pSpinDshPriceCacheRead->setValue(dDefPriceCr);
    m_pSpinDshPriceCacheWrite->setValue(dDefPriceCw);
    m_pSpinDshPriceOut->setValue(dDefPriceOut);
    DetectDshInstalled(true);
    m_pComboBackend->setCurrentIndex(
        m_pComboBackend->findData(iDefBackend) < 0 ? 0 : m_pComboBackend->findData(iDefBackend));
    m_pEditClaudeCmd->setText(sDefCmd);
    m_pSpinAskTimeout->setValue(iDefAskTimeout);
    m_pChkWebSearch->setChecked(bDefWeb);
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
    m_pComboBackend->setToolTip(QStringLiteral(
        "编排器使用的 Agent 后端（全局单选，[Agent] Backend）。\n"
        "DeepSeekHarness：JSON-RPC 完整接入（流式过程/token 统计/多轮会话），"
        "须在下方配置三项路径；切换在空闲时生效（运行中禁止切换）"));
    pFormAgent->addRow(QStringLiteral("Agent 后端："), m_pComboBackend);
    connect(m_pComboBackend, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { UpdateDshEnabled(); });
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
    m_pEditDshModel = new QLineEdit(pGroupDsh);
    m_pEditDshModel->setPlaceholderText(QStringLiteral("glm-5.2"));
    m_pEditDshModel->setToolTip(QStringLiteral(
        "模型名（provider 下的 models id；[Agent] DshModel，默认 glm-5.2）"));
    pFormDsh->addRow(QStringLiteral("模型："), m_pEditDshModel);
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

    // —— 目录排除 ——
    auto* pGroupDirs = new QGroupBox(QStringLiteral("目录排除"), this);
    auto* pFormDirs = new QFormLayout(pGroupDirs);
    m_pEditScanDirs = new QLineEdit(pGroupDirs);
    m_pEditScanDirs->setPlaceholderText(QStringLiteral(
        ".autodev,build,Execute,.git,.vs,node_modules,doc"));
    m_pEditScanDirs->setToolTip(QStringLiteral(
        "guide 指纹扫描排除的目录名，逗号分隔；留空 = 内置默认（[Scan] ExcludeDirs）"));
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
    m_pTreeDshModels->setMinimumHeight(150);
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

    auto* pTabs = new QTabWidget(this);
    pTabs->addTab(pTabCommon, QStringLiteral("常用"));
    pTabs->addTab(pTabDsh, QStringLiteral("DeepSeekHarness参数设置"));

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
    if (m_pEditDshNode != nullptr)     { m_pEditDshNode->setEnabled(bDsh); }
    if (m_pEditDshRoot != nullptr)     { m_pEditDshRoot->setEnabled(bDsh); }
    if (m_pEditDshProvider != nullptr) { m_pEditDshProvider->setEnabled(bDsh && !bCustomActive); }
    if (m_pEditDshModel != nullptr)    { m_pEditDshModel->setEnabled(bDsh && !bCustomActive); }
    if (m_pSpinDshPriceIn != nullptr)        { m_pSpinDshPriceIn->setEnabled(bDsh); }
    if (m_pSpinDshPriceCacheRead != nullptr) { m_pSpinDshPriceCacheRead->setEnabled(bDsh); }
    if (m_pSpinDshPriceCacheWrite != nullptr) { m_pSpinDshPriceCacheWrite->setEnabled(bDsh); }
    if (m_pSpinDshPriceOut != nullptr)       { m_pSpinDshPriceOut->setEnabled(bDsh); }
    if (m_pTreeDshModels != nullptr)   { m_pTreeDshModels->setEnabled(bDsh); }
    // 2026.08.18 后端专属项反向联动：DSH 后端下这四项对运行无效果——claude 命令/提问等待
    //   超时只进 CClaudeRunner（DSH 用 node 启动、无 ask_user MCP 工具），联网检索开关与
    //   自动压缩比只折算进 --disallowedTools/--autocompact 等 claude 专属启动参数（DSH 侧
    //   忽略并降级提示）。选中 DSH 即禁用防误导，切回 claude 自动恢复。「上下文窗口上限」
    //   是后端中立项（上下文占用条的窗口钳制，两后端同口径），保持可用。
    const bool bClaudeOnly = !(m_pComboBackend != nullptr && m_pComboBackend->currentData() == 2);
    if (m_pEditClaudeCmd != nullptr)  { m_pEditClaudeCmd->setEnabled(bClaudeOnly); }
    if (m_pSpinAskTimeout != nullptr) { m_pSpinAskTimeout->setEnabled(bClaudeOnly); }
    if (m_pChkWebSearch != nullptr)   { m_pChkWebSearch->setEnabled(bClaudeOnly); }
    if (m_pSpinCompactPct != nullptr) { m_pSpinCompactPct->setEnabled(bClaudeOnly); }
    if (m_pComboBackend != nullptr && m_pComboBackend->count() > 1) {
        // 2026.08.18 修「DSH 项恒不可选」：QComboBox 的 UserRole-1 内部角色收的是
        //   Qt::ItemFlags 掩码——旧代码传 bool，true=1 只含 Selectable 缺 Enabled(0x20)，
        //   false=0 两位全无，检测结果再好 DSH 也永远禁用。改走 QStandardItem::setFlags
        //   正道（默认模型即 QStandardItemModel），非标准模型才用掩码值兜底。
        const Qt::ItemFlags eFlagsDsh = m_bDshInstalled
            ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled) : Qt::NoItemFlags;
        auto* pStdModel = qobject_cast<QStandardItemModel*>(m_pComboBackend->model());
        if (pStdModel != nullptr) {
            if (QStandardItem* pItem = pStdModel->item(1); pItem != nullptr) {
                pItem->setFlags(eFlagsDsh);
            }
        } else {
            m_pComboBackend->model()->setData(m_pComboBackend->model()->index(1, 0),
                                              static_cast<int>(eFlagsDsh), Qt::UserRole - 1);
        }
        if (!m_bDshInstalled && m_pComboBackend->currentData() == 2) {
            m_pComboBackend->setCurrentIndex(0);   // DSH 不可用 → 弹回 claude（触发本函数重入收敛）
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
            QStringLiteral("（空：使用上方 Provider/模型 固定路由）")});
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
    RefreshDshModelList();   // 先重建（新行落位），再选中末行
    if (m_pTreeDshModels->topLevelItemCount() > 0) {
        m_pTreeDshModels->setCurrentItem(
            m_pTreeDshModels->topLevelItem(m_pTreeDshModels->topLevelItemCount() - 1));
    }
    UpdateDshEnabled();   // 按钮态 +（当前条目变化时的）Provider/模型置灰联动
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
            m_pLblDshState->setText(QStringLiteral("已安装：")
                + QDir::toNativeSeparators(sRoot));
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

QString CSettingsDialog::DshModel() const {
    return m_pEditDshModel->text().trimmed();
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

bool CSettingsDialog::WebSearch() const {
    return m_pChkWebSearch->isChecked();
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
