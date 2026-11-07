#pragma once

// ============================================================================
// CFindReplaceDialog —— 编辑菜单「查找/替换」共用对话框（2026-11-07；同日增强：
//                        查找串历史下拉 / 选项持久化由主窗口注入回读）
// ----------------------------------------------------------------------------
// 查找与替换复用同一套设计：顶部两个单选按钮「在文件中查找 / 在文件中替换」——
// 调用参数决定初始选中，用户可手工切换（切到查找=隐藏替换行）。查找内容为可编辑
// 下拉（QComboBox）：点击输入框自动列出最近查找内容（时间倒序；点击条目直接
// 填入），下拉列表右键菜单：删除（当前条目）/清空（全部）——历史经 FindHistory()
// 回传主窗口持久化。选项（范围/三复选）初值由 SetUiState 注入（config.ini [Find]）。
// 「查找所有」→ accept，由主窗口解析参数并在独立窗口弹「查找结果」页。
// 排除目录配置在 设置→目录排除「查找排除」（[Find] ExcludeDirs）。
// ============================================================================

#include <QDialog>
#include <QStringList>

class QRadioButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;

class CFindReplaceDialog : public QDialog {
    Q_OBJECT

public:
    // a_bReplaceMode=true 由「替换...」菜单进入：预设选中「在文件中替换」并显示
    //   替换行；false 走「查找...」。用户单选钮可随时互切。
    // a_aHistory：最近查找内容（时间倒序，新在前；主窗口自 config.ini 载入）。
    explicit CFindReplaceDialog(bool a_bReplaceMode,
                                const QStringList& a_aHistory = QStringList(),
                                QWidget* a_pParent = nullptr);

    // —— 参数出口（accept 后由主窗口读取）——
    bool ReplaceMode() const;      // 单选钮现值（用户可能手切过）
    QString FindText() const;
    QString ReplaceText() const;
    bool CaseSensitive() const;
    bool WholeWord() const;
    bool UseRegex() const;
    int Scope() const;             // 0=全部（含关联项目） 1=本项目 2=任务和助手日志
    // 历史（含下拉右键 删除/清空 的编辑结果；主窗口持久化用）。
    const QStringList& FindHistory() const { return m_aHistory; }

    // 选项初值注入（持久化回填：范围 + 三复选）。
    void SetUiState(int a_iScope, bool a_bCase, bool a_bWord, bool a_bRegex);

private:
    void OnFindAll();              // 查找所有：预检（空串/非法正则）→ accept
    void OnModeToggled();          // 单选钮互切：替换行显隐
    void RemoveHistoryItem(int a_iIdx);   // 下拉右键「删除」
    void ClearHistory();                  // 下拉右键「清空」

protected:
    // 查找输入框 FocusIn → 自动弹历史下拉（需求 3）。
    bool eventFilter(QObject* a_pObj, QEvent* a_pEvent) override;

    QRadioButton* m_pRadioFind    = nullptr;   // 在文件中查找
    QRadioButton* m_pRadioReplace = nullptr;   // 在文件中替换
    QComboBox*    m_pCmbFind      = nullptr;   // 查找内容（可编辑+历史下拉）
    QLineEdit*    m_pEditReplace  = nullptr;
    QLabel*       m_pLblReplace   = nullptr;   // 「替换为：」（随模式显隐）
    QCheckBox*    m_pChkCase      = nullptr;   // 区分大小写
    QCheckBox*    m_pChkWord      = nullptr;   // 全字匹配
    QCheckBox*    m_pChkRegex     = nullptr;   // 使用正则表达式
    QComboBox*    m_pCmbScope     = nullptr;   // 查找范围
    QStringList   m_aHistory;                  // 历史副本（右键编辑作用于它）
};
