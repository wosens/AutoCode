#include "CNewTaskDialog.h"

#include <QLineEdit>
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
#include "core/StageDef.h"
#include "core/TaskTypes.h"

CNewTaskDialog::CNewTaskDialog(EMode a_eMode, QWidget* a_pParent)
    : QDialog(a_pParent), m_eMode(a_eMode) {
    SetupUi();
    ApplyMode();
}

void CNewTaskDialog::SetupUi() {
    // 严格复刻原 OnNewTask 内联 QDialog 的字段/布局/尺寸。
    setMinimumSize(640, 520);
    resize(720, 600);

    m_pName = new QLineEdit(this);
    m_pName->setPlaceholderText(QStringLiteral("如：为 X 模块增加导出 CSV 功能"));

    m_pType = new QComboBox(this);
    m_pType->addItem(QStringLiteral("feature（新功能）"),
                     static_cast<int>(ETaskType::Feature));
    m_pType->addItem(QStringLiteral("bugfix（修缺陷）"),
                     static_cast<int>(ETaskType::Bugfix));

    // 2026.08.15 (流程档位) 档位下拉：只决定阶段清单默认勾选 + 评审聚合方式/轮次/视角数；
    // 阶段清单才是权威，用户可在下方清单自由增删勾选。
    m_pTier = new QComboBox(this);
    m_pTier->addItem(QStringLiteral("简单（精简阶段，代码审查单视角，评审多数决）"),
                     static_cast<int>(ETier::Simple));
    m_pTier->addItem(QStringLiteral("标准（完整阶段，评审多数决）"),
                     static_cast<int>(ETier::Standard));
    m_pTier->addItem(QStringLiteral("严格（完整阶段，评审一票否决）"),
                     static_cast<int>(ETier::Strict));
    m_pTier->setCurrentIndex(static_cast<int>(ETier::Standard));   // 默认标准（向后兼容）
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
    connect(m_pBtns, &QDialogButtonBox::accepted, this, &QDialog::accept);
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

ETaskType CNewTaskDialog::Type() const {
    return static_cast<ETaskType>(m_pType->currentData().toInt());
}

ETier CNewTaskDialog::Tier() const {
    return static_cast<ETier>(m_pTier->currentData().toInt());
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
    // 档位默认勾选集：标准/严格 = 完整规范流；简单 = 精简流
    //   （Feature 跳 需求评审/设计/设计评审/计划/计划评审；Bugfix 跳 根因核实）。
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
    const QList<EStage> aFlow = CStageMachine::GetFlowStages(Type());
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
    const QList<EStage> aPreset = TierPresetStages(Type(), Tier());
    for (int i = 0; i < m_pStages->count(); ++i) {
        QListWidgetItem* pItem = m_pStages->item(i);
        if (pItem == nullptr) {
            continue;
        }
        const EStage e = static_cast<EStage>(pItem->data(Qt::UserRole).toInt());
        pItem->setCheckState(aPreset.contains(e) ? Qt::Checked : Qt::Unchecked);
    }
}

void CNewTaskDialog::SetTask(const CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    // 名称/类型/需求描述
    m_pName->setText(a_pTask->Name());
    const int iType = static_cast<int>(a_pTask->Type());
    for (int i = 0; i < m_pType->count(); ++i) {
        if (m_pType->itemData(i).toInt() == iType) {
            m_pType->setCurrentIndex(i);
            break;
        }
    }
    m_pReq->setPlainText(a_pTask->UserRequest());

    // 2026.08.15 (流程档位) 档位 + 阶段清单回显：档位切换会触发 ApplyTierPreset，
    //   随后按任务持久化的 flowStages（空=完整流）重设勾选，覆盖预设。
    const int iTier = static_cast<int>(a_pTask->Tier());
    for (int i = 0; i < m_pTier->count(); ++i) {
        if (m_pTier->itemData(i).toInt() == iTier) {
            m_pTier->setCurrentIndex(i);
            break;
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
    m_pLblCost->setText(QStringLiteral("$%1 | 输入:%2 输出:%3")
                            .arg(QString::number(a_pTask->TotalCost(), 'f', 2))
                            .arg(a_pTask->TotalInTokens())
                            .arg(a_pTask->TotalOutTokens()));
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
