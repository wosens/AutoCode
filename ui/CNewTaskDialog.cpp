#include "CNewTaskDialog.h"

#include <QLineEdit>
#include <QMessageBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSet>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QSizePolicy>

#include "core/CTaskInfo.h"
#include "core/CStageMachine.h"
#include "core/COrchConfig.h"
#include "core/StageDef.h"
#include "core/TaskTypes.h"

CNewTaskDialog::CNewTaskDialog(EMode a_eMode, QWidget* a_pParent)
    : QDialog(a_pParent), m_eMode(a_eMode),
      m_pOrch(COrchConfig::Instance()) {   // 单例加载编排配置+注册自定义阶段
    SetupUi();
    ApplyMode();
}

void CNewTaskDialog::SetupUi() {
    // 严格复刻原 OnNewTask 内联 QDialog 的字段/布局/尺寸。
    setMinimumSize(640, 520);
    resize(720, 600);

    m_pName = new QLineEdit(this);
    // 2026.08.20 留空即自动命名已是默认行为，占位提示不再说明（仅示例）。
    m_pName->setPlaceholderText(QStringLiteral("如：为 X 模块增加导出 CSV 功能（可以为空）"));

    m_pType = new QComboBox(this);
    // 2026-10-14 任务编排设计：类型/档位清单改由编排配置驱动（功能菜单
    //   「任务编排设计」可增删改；每次打开对话框读同进程单例 = 最新版本）。
    if (m_pOrch->aTypes.isEmpty()) {
        m_pType->addItem(QStringLiteral("feature（新功能）"),
                         static_cast<int>(ETaskType::Feature));
        m_pType->addItem(QStringLiteral("bugfix（修缺陷）"),
                         static_cast<int>(ETaskType::Bugfix));
    } else {
        for (int i = 0; i < m_pOrch->aTypes.size(); ++i) {
            const SOrchType& t = m_pOrch->aTypes.at(i);
            m_pType->addItem(t.sName, i);   // UserData = 配置下标（Type() 取 base 映射）
        }
    }

    // 2026.08.15 (流程档位) 档位下拉：只决定阶段清单默认勾选 + 评审聚合方式/轮次/视角数；
    // 阶段清单才是权威，用户可在下方清单自由增删勾选。
    m_pTier = new QComboBox(this);
    if (m_pOrch->aTiers.isEmpty()) {
        m_pTier->addItem(QStringLiteral("简单（精简阶段，代码审查单视角，评审多数决）"),
                         static_cast<int>(ETier::Simple));
        m_pTier->addItem(QStringLiteral("标准（完整阶段，评审多数决）"),
                         static_cast<int>(ETier::Standard));
        m_pTier->addItem(QStringLiteral("严格（完整阶段，评审一票否决）"),
                         static_cast<int>(ETier::Strict));
    } else {
        for (int i = 0; i < m_pOrch->aTiers.size(); ++i) {
            const SOrchTier& t = m_pOrch->aTiers.at(i);
            m_pTier->addItem(t.sName, i);
        }
    }
    // 默认选中标准档（base=standard 的首个配置项；无则第 0 项）
    for (int i = 0; i < m_pTier->count(); ++i) {
        const QString sBase = (m_pOrch->aTiers.isEmpty()
                        || m_pTier->itemData(i).toInt() >= m_pOrch->aTiers.size())
                ? QString() : m_pOrch->aTiers.at(m_pTier->itemData(i).toInt()).sBase;
        if (m_pOrch->aTiers.isEmpty()) {
            if (m_pTier->itemData(i).toInt() == static_cast<int>(ETier::Standard)) {
                m_pTier->setCurrentIndex(i);
            }
        } else if (sBase == QLatin1String("standard")) {
            m_pTier->setCurrentIndex(i);
        }
    }
    m_pTier->setToolTip(QStringLiteral(
        "简单：需求收集→确认需求→开发→代码审查(单视角)→验证→测试（bugfix 同理精简），轻评审轮次 12；\n"
        "标准：完整阶段清单，评审多数决（2/3 驳回才驳回，少数意见随流程携带），轻评审轮次 18；\n"
        "严格：完整阶段清单，任一视角驳回即驳回（关键任务逃生门），轻评审轮次 30。\n"
        "选择档位后可在下方「执行阶段」清单自行增删勾选。"));

    // 2026.08.15 (流程档位) 执行阶段可勾选清单：按任务类型规范流顺序列出全部阶段。
    m_pStages = new QListWidget(this);
    m_pStages->setToolTip(QStringLiteral(
        "勾选本任务要执行的阶段（顺序固定按流程）；「需求收集」为入口阶段不可取消。\n"
        "被取消阶段产出的文档（如 plan.md）下游不再要求，缺文档的阶段会按实际可用资料推进。"));
    RebuildStageList(false);
    // 切任务类型 → 重建清单（阶段集不同，勾选按档位预设重设）；切档位 → 按预设重设勾选。
    connect(m_pType, &QComboBox::currentIndexChanged, this, [this]() { RebuildStageList(false); });
    connect(m_pTier, &QComboBox::currentIndexChanged, this, [this]() { ApplyTierPreset(); });

    m_pReq = new QPlainTextEdit(this);
    m_pReq->setPlaceholderText(
        QStringLiteral("描述你的需求：要实现什么功能 / 修复什么 bug？"
                       "涉及哪些模块、接口？有什么约束、边界（不做什么）？"));
    m_pReq->setMinimumHeight(240);
    m_pReq->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // FR-4「立刻启动任务」勾选（默认不勾，design §0 合理默认）。
    m_pChkStart = new QCheckBox(QStringLiteral("立刻启动任务"), this);
    m_pChkStart->setToolTip(QStringLiteral(
        "勾选后创建任务即立即开始执行；不勾选则仅创建并选中，状态保持待运行"));

    // View 模式只读字段（Q2 默认：状态/阶段/cost/创建时间 + 2026.08.15 运行耗时）。
    m_pLblStatus  = new QLabel(this);
    m_pLblStage   = new QLabel(this);
    m_pLblCost    = new QLabel(this);
    m_pLblCreated = new QLabel(this);
    m_pLblElapsed = new QLabel(this);

    m_pBtns = new QDialogButtonBox(this);
    // 2026.08.26 输入规则在对话框内校验（不再等窗口关闭后判断）：
    //   OK 不再直接 accept，先 Validate()，不通过则提示并留在对话框（输入不丢）。
    connect(m_pBtns, &QDialogButtonBox::accepted, this, [this]() {
        if (Validate()) {
            accept();
        }
    });
    connect(m_pBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* pForm = new QFormLayout;
    pForm->addRow(QStringLiteral("任务名称"), m_pName);
    pForm->addRow(QStringLiteral("任务类型"), m_pType);
    pForm->addRow(QStringLiteral("流程档位"), m_pTier);
    pForm->addRow(QStringLiteral("执行阶段"), m_pStages);
    pForm->addRow(QStringLiteral("需求描述"), m_pReq);
    // View 模式才显示的只读信息行（New 模式由 ApplyMode 隐藏）。
    pForm->addRow(QStringLiteral("状态"), m_pLblStatus);
    pForm->addRow(QStringLiteral("当前阶段"), m_pLblStage);
    pForm->addRow(QStringLiteral("费用 / Token"), m_pLblCost);
    pForm->addRow(QStringLiteral("运行耗时"), m_pLblElapsed);
    pForm->addRow(QStringLiteral("创建时间"), m_pLblCreated);

    auto* pV = new QVBoxLayout(this);
    pV->addLayout(pForm, 1);   // 表单区拿 stretch=1，需求框随窗口拉伸获得额外高度
    pV->addWidget(m_pChkStart);
    pV->addWidget(m_pBtns);
}

void CNewTaskDialog::ApplyMode() {
    if (m_eMode == New) {
        setWindowTitle(QStringLiteral("新建任务"));
        // 字段可编辑、启动勾选可见、按钮 Ok|Cancel。
        m_pName->setReadOnly(false);
        m_pType->setEnabled(true);
        m_pTier->setEnabled(true);
        m_pStages->setEnabled(true);
        m_pReq->setReadOnly(false);
        m_pChkStart->setVisible(true);
        m_pChkStart->setChecked(false);
        // New 模式不显示只读信息行：隐藏字段控件并清空其文本（QFormLayout 行因无内容而塌缩为最小）。
        m_pLblStatus->setVisible(false);
        m_pLblStatus->clear();
        m_pLblStage->setVisible(false);
        m_pLblStage->clear();
        m_pLblCost->setVisible(false);
        m_pLblCost->clear();
        m_pLblCreated->setVisible(false);
        m_pLblCreated->clear();
        m_pLblElapsed->setVisible(false);
        m_pLblElapsed->clear();
        m_pBtns->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    } else {
        setWindowTitle(QStringLiteral("查看任务"));
        // 全字段只读、隐藏启动勾选、按钮仅 Close（隐藏 OK）。
        m_pName->setReadOnly(true);
        m_pType->setEnabled(false);
        m_pTier->setEnabled(false);
        m_pStages->setEnabled(false);   // 阶段清单只读回显（项不可改勾）
        m_pReq->setReadOnly(true);
        m_pChkStart->setVisible(false);
        m_pBtns->setStandardButtons(QDialogButtonBox::Close);
        // Close 按钮 → reject；查看态按 Close 应直接关闭（不区分 accept/reject 语义）。
        // QDialogButtonBox::Close 默认触发 rejected，故复用 reject 连接即可关闭。
    }
}

QString CNewTaskDialog::Name() const {
    return m_pName->text().trimmed();
}

bool CNewTaskDialog::Validate() {
    // 2026.08.26 对话框内校验（New 模式）：不满足规则弹提示返回 false，OK 不生效、
    //   对话框不关，用户输入保留。View 模式无必填项，直接通过（按钮也仅 Close）。
    if (m_eMode == New && UserRequest().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("需求描述不能为空"));
        m_pReq->setFocus();
        return false;
    }
    return true;
}

ETaskType CNewTaskDialog::Type() const {
    // 配置驱动：当前选中配置类型的 base 映射为引擎枚举（决定驳回路由等引擎行为）
    const int iIdx = m_pType->currentData().toInt();
    if (!m_pOrch->aTypes.isEmpty() && iIdx >= 0 && iIdx < m_pOrch->aTypes.size()) {
        return COrchConfig::BaseToType(m_pOrch->aTypes.at(iIdx).sBase);
    }
    return static_cast<ETaskType>(iIdx);
}

ETier CNewTaskDialog::Tier() const {
    const int iIdx = m_pTier->currentData().toInt();
    if (!m_pOrch->aTiers.isEmpty() && iIdx >= 0 && iIdx < m_pOrch->aTiers.size()) {
        return COrchConfig::BaseToTier(m_pOrch->aTiers.at(iIdx).sBase);
    }
    return static_cast<ETier>(iIdx);
}

// 2026-10-14 勾选流与该基础类型的内置规范流完全一致 → true（主窗口据此存空表，
//   语义同旧任务；否则（自定义阶段/重排序）必须显式存清单，执行才按编排序走）。
bool CNewTaskDialog::IsCanonicalFlow() const {
    return CheckedStages() == CStageMachine::GetFlowStages(Type());
}

QList<EStage> CNewTaskDialog::CheckedStages() const {
    // 按清单顺序取勾选项（清单本身按规范流构建）。全勾 = 返回完整流（调用方可存空表）。
    QList<EStage> aStages;
    for (int i = 0; i < m_pStages->count(); ++i) {
        QListWidgetItem* pItem = m_pStages->item(i);
        if (pItem != nullptr && pItem->checkState() == Qt::Checked) {
            aStages.append(static_cast<EStage>(pItem->data(Qt::UserRole).toInt()));
        }
    }
    return aStages;
}

QString CNewTaskDialog::UserRequest() const {
    return m_pReq->toPlainText().trimmed();
}

bool CNewTaskDialog::StartImmediately() const {
    return m_pChkStart->isChecked();
}

QList<EStage> CNewTaskDialog::TierPresetStages(ETaskType a_eType, ETier a_eTier) {
    // 【已由编排配置取代——保留兼容旧路径】档位默认勾选集：标准/严格 = 完整规范流；
    //   简单 = 精简流（Feature 跳 需求评审/设计/设计评审/计划/计划评审；Bugfix 跳 根因核实）。
    const QList<EStage> aFull = CStageMachine::GetFlowStages(a_eType);
    if (a_eTier != ETier::Simple) {
        return aFull;
    }
    QList<EStage> aPreset;
    for (EStage e : aFull) {
        switch (e) {
            case EStage::ReviewReq:
            case EStage::Design:
            case EStage::ReviewDesign:
            case EStage::Plan:
            case EStage::ReviewPlan:
            case EStage::RootCauseVerify:
                break;   // 简单档默认跳过
            default:
                aPreset.append(e);
        }
    }
    return aPreset;
}

void CNewTaskDialog::RebuildStageList(bool a_bKeepChecked) {
    // 记住当前勾选（切类型重建时尽量保留仍在新流内的阶段勾选态）
    QSet<int> aChecked;
    if (a_bKeepChecked) {
        for (int i = 0; i < m_pStages->count(); ++i) {
            QListWidgetItem* pItem = m_pStages->item(i);
            if (pItem != nullptr && pItem->checkState() == Qt::Checked) {
                aChecked.insert(pItem->data(Qt::UserRole).toInt());
            }
        }
    }
    m_pStages->clear();
    // 2026-10-14 编排配置驱动：当前配置类型的阶段流（配置序即执行序，含自定义阶段）；
    //   配置缺失（空类型表）回退内置规范流。
    QList<EStage> aFlow;
    const int iTypeIdx = m_pType->currentData().toInt();
    if (!m_pOrch->aTypes.isEmpty() && iTypeIdx >= 0 && iTypeIdx < m_pOrch->aTypes.size()) {
        for (const QString& sId : m_pOrch->aTypes.at(iTypeIdx).aStages) {
            const int iEv = m_pOrch->EnumForStageId(sId);
            if (iEv > 0) {
                aFlow.append(static_cast<EStage>(iEv));
            }
        }
    }
    if (aFlow.isEmpty()) {
        aFlow = CStageMachine::GetFlowStages(Type());
    }
    for (EStage e : aFlow) {
        const SStageDef* pDef = FindStage(e);
        auto* pItem = new QListWidgetItem(pDef ? pDef->sNameZh : QString::number(int(e)), m_pStages);
        pItem->setData(Qt::UserRole, static_cast<int>(e));
        pItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        if (e == EStage::Intake) {
            // 入口阶段：强制勾选且不可取消（去掉 ItemIsEnabled → 显示禁用态勾选，不可点）
            pItem->setFlags(Qt::ItemIsUserCheckable);
            pItem->setCheckState(Qt::Checked);
        } else {
            pItem->setCheckState((a_bKeepChecked && aChecked.contains(static_cast<int>(e)))
                                     ? Qt::Checked : Qt::Unchecked);
        }
    }
    if (!a_bKeepChecked) {
        ApplyTierPreset();   // 新建/切类型：按档位预设勾选
    }
}

void CNewTaskDialog::ApplyTierPreset() {
    // 2026-10-14 编排配置驱动：档位预设 = 配置档位的阶段 id 集（与当前类型流取交集）；
    //   预设为空 → 回退各阶段「缺省选择」标志；配置缺失回退旧内置预设。
    QSet<int> aPreset;
    const int iTierIdx = m_pTier->currentData().toInt();
    if (!m_pOrch->aTiers.isEmpty() && iTierIdx >= 0 && iTierIdx < m_pOrch->aTiers.size()) {
        for (const QString& sId : m_pOrch->aTiers.at(iTierIdx).aStages) {
            const int iEv = m_pOrch->EnumForStageId(sId);
            if (iEv > 0) {
                aPreset.insert(iEv);
            }
        }
        if (aPreset.isEmpty()) {
            // 档位未配预设 → 按阶段缺省选择标志
            for (const SOrchStage& s : m_pOrch->aStages) {
                if (s.bDefault) {
                    const int iEv = m_pOrch->EnumForStageId(s.sId);
                    if (iEv > 0) {
                        aPreset.insert(iEv);
                    }
                }
            }
        }
    } else {
        const QList<EStage> aOld = TierPresetStages(Type(), Tier());
        for (EStage e : aOld) {
            aPreset.insert(static_cast<int>(e));
        }
    }
    // 2026-10-14 新增自定义阶段的兑底：未出现在任何档位预设里的阶段（新编排项天然
    //   不在旧预设内），只要「缺省选择」勾着就默认勾选——否则新阶段永远默认不勾；
    //   而被某档位显式排除的阶段（如简单档排除 design，design 在标准/严格预设里出现）
    //   不受此影响，仍按预设交集。
    QSet<QString> aInAnyTier;
    for (const SOrchTier& t : m_pOrch->aTiers) {
        for (const QString& sId : t.aStages) {
            aInAnyTier.insert(sId);
        }
    }
    for (int i = 0; i < m_pStages->count(); ++i) {
        QListWidgetItem* pItem = m_pStages->item(i);
        if (pItem == nullptr) {
            continue;
        }
        const EStage e = static_cast<EStage>(pItem->data(Qt::UserRole).toInt());
        if (e == EStage::Intake) {
            continue;   // 入口恒勾
        }
        bool bCheck = aPreset.contains(static_cast<int>(e));
        if (!bCheck) {
            const SOrchStage* pS = m_pOrch->FindStageCfg(m_pOrch->StageIdForEnum(int(e)));
            if (pS != nullptr && pS->bDefault && !aInAnyTier.contains(pS->sId)) {
                bCheck = true;
            }
        }
        pItem->setCheckState(bCheck ? Qt::Checked : Qt::Unchecked);
    }
}

void CNewTaskDialog::SetTask(const CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    // 名称/需求描述
    m_pName->setText(a_pTask->Name());
    m_pReq->setPlainText(a_pTask->UserRequest());

    // 2026-10-14 类型回显：配置驱动时按 base 匹配首个同基础类型的配置项
    //  （任务仅持久化 base 枚举；具体配置类型名属创建时编排，查看回退 base 对应项）
    {
        int iPick = -1;
        for (int i = 0; i < m_pType->count(); ++i) {
            const int iIdx = m_pType->itemData(i).toInt();
            const ETaskType eItem = (!m_pOrch->aTypes.isEmpty()
                                     && iIdx >= 0 && iIdx < m_pOrch->aTypes.size())
                ? COrchConfig::BaseToType(m_pOrch->aTypes.at(iIdx).sBase)
                : static_cast<ETaskType>(iIdx);
            if (eItem == a_pTask->Type()) {
                iPick = i;
                break;
            }
        }
        if (iPick >= 0) {
            m_pType->setCurrentIndex(iPick);
        }
    }

    // 2026.08.15 (流程档位) 档位 + 阶段清单回显：档位切换会触发 ApplyTierPreset，
    //   随后按任务持久化的 flowStages（空=完整流）重设勾选，覆盖预设。
    //   2026-10-14 同样按 base 匹配回显配置档位项。
    {
        int iPick = -1;
        for (int i = 0; i < m_pTier->count(); ++i) {
            const int iIdx = m_pTier->itemData(i).toInt();
            const ETier eItem = (!m_pOrch->aTiers.isEmpty()
                                 && iIdx >= 0 && iIdx < m_pOrch->aTiers.size())
                ? COrchConfig::BaseToTier(m_pOrch->aTiers.at(iIdx).sBase)
                : static_cast<ETier>(iIdx);
            if (eItem == a_pTask->Tier()) {
                iPick = i;
                break;
            }
        }
        if (iPick >= 0) {
            m_pTier->setCurrentIndex(iPick);
        }
    }
    const QList<EStage> aFlow = a_pTask->FlowStages().isEmpty()
                                    ? CStageMachine::GetFlowStages(a_pTask->Type())
                                    : a_pTask->FlowStages();
    for (int i = 0; i < m_pStages->count(); ++i) {
        QListWidgetItem* pItem = m_pStages->item(i);
        if (pItem == nullptr) {
            continue;
        }
        const EStage e = static_cast<EStage>(pItem->data(Qt::UserRole).toInt());
        pItem->setCheckState(aFlow.contains(e) ? Qt::Checked : Qt::Unchecked);
    }

    // 只读信息行（Q2 默认）
    m_pLblStatus->setText(ToString(a_pTask->Status()));
    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    m_pLblStage->setText(pDef ? pDef->sNameZh : QStringLiteral("?"));
    // 2026.08.18 费用 ≤0 显示 "-"（DSH 后端无计费恒 0）。
    const QString sCost = a_pTask->TotalCost() > 0.0
        ? QString::number(a_pTask->TotalCost(), 'f', 2) : QStringLiteral("-");
    m_pLblCost->setText(QStringLiteral("%1 | 输入:%2 输出:%3 | 请求:%4")
                            .arg(sCost)
                            .arg(a_pTask->TotalInTokens())
                            .arg(a_pTask->TotalOutTokens())
                            .arg(a_pTask->RequestCount()));
    m_pLblCreated->setText(a_pTask->CreatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    // 2026.08.15 运行耗时：<1h 为 mm:ss，≥1h 为 h:mm:ss；未运行过显示 -。
    {
        const qint64 iMs = a_pTask->RunElapsedMs();
        if (iMs <= 0) {
            m_pLblElapsed->setText(QStringLiteral("-"));
        } else {
            const qint64 iTotalSec = iMs / 1000;
            const qint64 iH = iTotalSec / 3600;
            const qint64 iM = (iTotalSec % 3600) / 60;
            const qint64 iS = iTotalSec % 60;
            m_pLblElapsed->setText(iH > 0
                ? QStringLiteral("%1:%2:%3").arg(iH)
                      .arg(iM, 2, 10, QLatin1Char('0'))
                      .arg(iS, 2, 10, QLatin1Char('0'))
                : QStringLiteral("%1:%2").arg(iM, 2, 10, QLatin1Char('0'))
                                          .arg(iS, 2, 10, QLatin1Char('0')));
        }
    }
}
