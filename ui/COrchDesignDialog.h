#pragma once

// ============================================================================
// COrchDesignDialog —— 任务编排设计窗口（2026-10-14 功能菜单「任务编排设计...」）
// ----------------------------------------------------------------------------
// 需求（任务 2026-10-14）：
//   ① 左侧三个列表：任务类型 / 流程档位 / 执行阶段（自上而下）；各配
//      [添加][编辑][删除] 三按钮 + 右键菜单同三项，列表均支持多选；
//   ② 执行阶段表末列为「缺省选择」（默认勾选）：右键可切换；删除按钮后
//      追加 [设置为缺省] 按钮（已缺省 → 点击变为不缺省，切换语义）；
//   ③ 右侧主区：点击阶段节点 → 显示/编辑该阶段要求（prompts/<文件>.md）；
//   ④ 编辑器上方工具栏：[保存] [查找] [替换]（查找/替换栏可展开，Esc 关闭）；
//   ⑤ 编排即时生效：结构变更（增删改/缺省切换）立即写 orchestration.json +
//      注册运行期动态阶段表，新建任务对话框即用最新编排。
// 数据：直接编辑 COrchConfig::Instance()（进程内单例），详见 core/COrchConfig.h。
// ============================================================================

#include <QDialog>
#include <QList>

#include "core/COrchConfig.h"

class QListWidget;
class QListWidgetItem;
class QTableWidget;
class QTableWidgetItem;
class QPlainTextEdit;
class QToolButton;
class QLabel;
class QLineEdit;
class QCheckBox;
class QShortcut;

class COrchDesignDialog : public QDialog {
    Q_OBJECT

public:
    explicit COrchDesignDialog(QWidget* a_pParent = nullptr);

protected:
    void closeEvent(QCloseEvent* a_pEvent) override;

private:
    // —— 界面搭建 ——
    void SetupUi();
    QWidget* BuildTypeGroup();
    QWidget* BuildTierGroup();
    QWidget* BuildStageGroup();
    QWidget* BuildEditorGroup();
    void BuildFindBar();
    void BuildReplaceBar();

    // —— 列表刷新 ——
    void RefreshTypes();
    void RefreshTiers();
    void RefreshStages();
    void RefreshAll();

    // —— 任务类型 / 档位 操作（a_bType=true 为类型，否则档位）——
    void OnAddEntity(bool a_bType);
    void OnEditEntity(bool a_bType);
    void OnDeleteEntity(bool a_bType);

    // —— 执行阶段操作 ——
    void OnAddStage();
    void OnEditStage();
    void OnDeleteStage();
    void OnToggleStageDefault();     // 「设置为缺省」/右键切换（已缺省 → 取消）
    void OnStageSelectionChanged();

    // —— 阶段要求编辑器 ——
    void LoadStagePrompt(int a_iRow);           // 载入该行阶段提示词（脏检查）
    bool SaveStagePrompt(bool a_bSilent);       // 保存编辑器内容 → prompts/<文件>
    bool ConfirmDiscard();                      // 脏内容询问（true=可继续切换/关闭）
    void OnSaveClicked();
    void OnFindOpen();
    void OnReplaceOpen();
    bool FindInEditor(bool a_bBackward);
    void ReplaceOne();
    void ReplaceAll();

    // —— 子对话框（返回 true=确认）——
    bool EditTypeDlg(SOrchType& a_stType, bool a_bNew);
    bool EditTierDlg(SOrchTier& a_stTier, bool a_bNew);
    bool EditStageDlg(SOrchStage& a_stStage, bool a_bNew);

    // 结构变更统一收口：落盘 + 注册动态阶段 + 刷新（a_iSelStage 保存后重选行）
    void CommitStructure();

    COrchConfig*       m_pOrch = nullptr;

    // 左侧三列表
    QListWidget*       m_pLstTypes = nullptr;
    QListWidget*       m_pLstTiers = nullptr;
    QTableWidget*      m_pTblStages = nullptr;

    // 右侧编辑器
    QPlainTextEdit*    m_pEdit = nullptr;
    QLabel*            m_pLblPrompt = nullptr;
    QToolButton*       m_pBtnSave = nullptr;
    QToolButton*       m_pBtnFind = nullptr;
    QToolButton*       m_pBtnReplace = nullptr;

    // 查找 / 替换栏（默认隐藏）
    QWidget*           m_pFindBar = nullptr;
    QLineEdit*         m_pEdFind = nullptr;
    QCheckBox*         m_pChkCase = nullptr;
    QCheckBox*         m_pChkWrap = nullptr;
    QWidget*           m_pRepBar = nullptr;
    QLineEdit*         m_pEdRepFind = nullptr;
    QLineEdit*         m_pEdRepWith = nullptr;

    // 编辑器状态
    int                m_iCurStageRow = -1;   // 当前载入的阶段行（-1=未载入）
    QString            m_sCurPromptPath;      // 当前编辑的提示词文件绝对路径
    bool               m_bDirty = false;      // 有未保存修改
    QShortcut*         m_pScFind = nullptr;
};
