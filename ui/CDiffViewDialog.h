#pragma once

#include <QDialog>
#include <QList>

class QPlainTextEdit;
class QLabel;

// 2026.08.24 回退窗口文件列表右键「查看变更」：双栏并排对比两个记录点中同一文件
// 的内容（Beyond Compare 风格：左=上一记录点，右=当前记录点，变化区域底色标红/绿，
// 公共前后缀对齐、中间空行补位，两侧滚动条联动）。纯只读展示，不做任何文件操作。
class CDiffViewDialog : public QDialog {
    Q_OBJECT

public:
    // a_sPath：工程根相对路径（标题/表头显示）。a_bufOld/a_bufNew：上一记录点/当前
    // 记录点内容（新增=左空 + bOldMissing；删除=右空 + bNewMissing——显示占位说明）。
    // a_sLabelOld/a_sLabelNew：如 "#12 · 2026-08-24 10:00"（侧栏标题）。
    CDiffViewDialog(const QString& a_sPath,
                    const QByteArray& a_bufOld, const QByteArray& a_bufNew,
                    bool a_bOldMissing, bool a_bNewMissing,
                    const QString& a_sLabelOld, const QString& a_sLabelNew,
                    QWidget* a_pParent = nullptr,
                    const QString& a_sCfgPath = QString());

protected:
    // 2026.08.29 窗口几何持久化（[Layout] DiffWin，profile API；a_sCfgPath 空=不持久化）。
    void showEvent(QShowEvent* a_pEvent) override;   // 2026.09.04 标题栏防超屏兜底校正
    void hideEvent(QHideEvent* a_pEvent) override;

private:
    void BuildUi();
    void Fill();
    // 公共前后缀对齐 + 中段补位空行（返回三段长度，用于底色高亮区间）。
    static void AlignSides(const QList<QString>& a_aOld, const QList<QString>& a_aNew,
                           QList<QString>& a_aOldOut, QList<QString>& a_aNewOut,
                           int& a_iOldFrom, int& a_iOldTo,
                           int& a_iNewFrom, int& a_iNewTo);

    QString       m_sPath;
    QByteArray    m_bufOld;
    QByteArray    m_bufNew;
    bool          m_bOldMissing = false;
    bool          m_bNewMissing = false;
    QString       m_sLabelOld;
    QString       m_sLabelNew;
    QString       m_sCfgPath;   // 2026.08.29 config.ini 全路径（布局持久化；空=禁用）

    QLabel*        m_pLblOld = nullptr;
    QLabel*        m_pLblNew = nullptr;
    QPlainTextEdit* m_pTxtOld = nullptr;
    QPlainTextEdit* m_pTxtNew = nullptr;
};
