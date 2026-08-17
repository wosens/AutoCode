#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QDialogButtonBox;

// 助手提问对话框：展示问题 + 选项（或自由输入），用户作答后返回文本。
//   options 非空 → 单选列表（双击 = 确认），末尾固定追加“其他（自行输入）”项，
//                 选中该项时显示输入框，确认时以输入内容作答；为空 → 多行自由输入。
// exec()=Accepted 时 AnswerText() 给出选中项/输入文本；Rejected 或空选择 → 空串。
// 风格仿 CRecentProjectsDialog（Q_OBJECT + QDialogButtonBox + QVBoxLayout + 中文标题）。
class CAskUserDialog : public QDialog {
    Q_OBJECT

public:
    explicit CAskUserDialog(const QString& a_sQuestion,
                            const QStringList& a_aOptions,
                            QWidget* a_pParent = nullptr);

    QString AnswerText() const;

private slots:
    void OnItemDoubleClicked();   // 双击选项 = 确认

private:
    QLabel*           m_pLblQuestion = nullptr;
    QListWidget*      m_pList        = nullptr;
    QPlainTextEdit*   m_pEdit        = nullptr;
    QPlainTextEdit*   m_pEditOther   = nullptr;   // 选项模式下“其他”项的输入框
    QDialogButtonBox* m_pBtns        = nullptr;
    QString           m_sAnswer;
};
