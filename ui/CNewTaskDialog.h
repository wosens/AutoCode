#pragma once

#include <QDialog>

#include "core/TaskTypes.h"   // ETaskType / ETier（Type()/Tier() 返回值）

class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QCheckBox;
class QLabel;
class QListWidget;
class QDialogButtonBox;
class CTaskInfo;

// 新建/查看任务统一对话框（FR-1 / FR-4 / FR-5）。
// 两态：
//   - New  ：可编辑 + 「立刻启动任务」勾选（默认不勾）；按钮 Ok|Cancel。
//   - View ：只读展示（名称/类型/档位/阶段清单/需求描述/状态/阶段/cost/创建时间）；按钮仅 Close。
// 抽取自原 CMainWindow::OnNewTask 内联 QDialog，New 态严格复刻其字段与布局。
// 2026.08.15 (流程档位)：New 态增加「流程档位」下拉（简单/标准/严格预设）+
//   「执行阶段」可勾选清单——档位只决定默认勾选与评审策略，用户可自由增删阶段。
class CNewTaskDialog : public QDialog {
    Q_OBJECT

public:
    enum EMode { New, View };

    explicit CNewTaskDialog(EMode a_eMode, QWidget* a_pParent = nullptr);

    // —— New 模式：取录入值（调用方据此构造 CTaskInfo）——
    QString Name() const;
    ETaskType Type() const;
    ETier    Tier() const;              // 2026.08.15 流程档位
    QString UserRequest() const;
    bool     StartImmediately() const;   // FR-4「立刻启动」勾选态
    // 勾选的阶段序列（按该类型规范流顺序；含强制项 Intake）。空=全勾（完整流）。
    QList<EStage> CheckedStages() const;

    // —— View 模式：填入只读展示 ——
    void SetTask(const CTaskInfo* a_pTask);

private:
    void SetupUi();
    void ApplyMode();   // 按 m_eMode 切字段只读/显隐/标题/按钮
    // 2026.08.15 (流程档位) 按当前任务类型重建阶段清单（切类型时调）；
    //   a_bKeepChecked=false 或勾选集与流不匹配时按 a_eTier 预设重设勾选。
    void RebuildStageList(bool a_bKeepChecked);
    // 2026.08.15 (流程档位) 按档位预设重设勾选（切档位时调）。
    void ApplyTierPreset();
    // 2026.08.15 (流程档位) 档位预设默认勾选集（简单=精简流；标准/严格=全勾）。
    static QList<EStage> TierPresetStages(ETaskType a_eType, ETier a_eTier);

    EMode             m_eMode;
    QLineEdit*        m_pName      = nullptr;
    QComboBox*        m_pType      = nullptr;
    QComboBox*        m_pTier      = nullptr;    // 2026.08.15 流程档位（简单/标准/严格）
    QListWidget*      m_pStages    = nullptr;    // 2026.08.15 执行阶段可勾选清单
    QPlainTextEdit*   m_pReq       = nullptr;
    QCheckBox*        m_pChkStart  = nullptr;   // FR-4，仅 New 可见
    // View 模式额外只读字段（Q2 默认：状态/阶段/cost/创建时间 + 运行耗时）
    QLabel*           m_pLblStage  = nullptr;
    QLabel*           m_pLblStatus = nullptr;
    QLabel*           m_pLblCost   = nullptr;
    QLabel*           m_pLblElapsed = nullptr;   // 2026.08.15 运行耗时
    QLabel*           m_pLblCreated = nullptr;
    QDialogButtonBox* m_pBtns      = nullptr;
};
