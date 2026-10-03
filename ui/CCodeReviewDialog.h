#pragma once

// ============================================================================
// CCodeReviewDialog —— 手动代码审查对话框（2026-09-30 ReviewSdk 接入 UI）
// ----------------------------------------------------------------------------
// 入口：资源管理器右键「代码审查」（文件或文件夹——文件夹递归收集代码文件）。
// 行为：非模态（审查分钟级，不锁主窗口）；表格列发现（级别/文件/行/规则/问题/
// 建议），双击行 → sigOpenFile 打开文件预览页并定位行；顶部进度 + 状态摘要；
// 按钮区：重新审查 / 停止 / 关闭。引擎/后端与任务阶段预审同一套
// （ReviewSdk.dll + 当前全局 Agent 后端）。
// ============================================================================

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QJsonArray>

class QTreeWidget;
class QTreeWidgetItem;
class QProgressBar;
class QLabel;
class QPushButton;
class CReviewSpinner;   // 2026-10-02 审查中动态图标（cpp 内定义，同 CBusyIndicator 风格）

class CCodeReviewDialog : public QDialog {
    Q_OBJECT

public:
    // a_sTarget：送审目标（文件或目录的绝对路径）；对话框内部负责展开/过滤。
    explicit CCodeReviewDialog(const QString& a_sTarget, QWidget* a_pParent = nullptr);

    ~CCodeReviewDialog() override;

signals:
    // 双击发现行：打开该文件并定位行（主窗口接 → 文件预览页）。
    void sigOpenFile(const QString& a_sAbsPath, int a_iLine);
    // 2026-10-02「按建议修复」：把「请根据审查意见修复代码（存在就修复，不存在忽略）
    //   + 问题清单」注入当前助手输入框（主窗口接 → AppendToAdhocInput，同「添加到聊天框」链路）。
    void sigFixToChat(const QString& a_sPrompt);

private slots:
    void OnStart();
    void OnStop();
    void OnFileStart(const QString& sRelPath, int iCur, int iTotal);
    void OnFileDone(const QString& sRelPath, int iRc, const QString& sFindingsJson,
                    const QString& sErr);
    void OnReviewDone(bool bOk, bool bStopped, const QString& sFindingsJson,
                      const QString& sReportMd, int iFilesReviewed, int iFindings);
    void OnItemDoubleClicked(QTreeWidgetItem* pItem, int iCol);
    void OnSaveReport();
    void OnFixToChat();          // 2026-10-02 按建议修复 → 注入助手输入框

private:
    void BuildUi();
    void CollectFiles();       // 目标展开：文件直用；目录递归收集（语言过滤+上限）
    void AddFindings(const QString& sRelPath, const QString& sFindingsJson);
    void UpdateCounts();
    QString TargetAbs() const { return m_sTarget; }

    // —— 2026-10-02 结果缓存复用（代码未修改 → 直接展示上次结果）——
    // 当前送审清单指纹：[{rel,mt(msecs),sz}]（mtime+size 足够识别 agent 重写/重存）。
    QJsonArray ComputeFingerprint() const;
    // 读缓存并比对指纹：命中则回填表格/报告/进度并返回 true（不再自动开审）。
    bool TryRestoreCache();
    // 完整跑完一轮后落盘（被停止/全部失败不落盘；键=目标路径哈希，exe 同级 review_cache/）。
    void SaveCache(const QString& a_sTime);

    QString m_sTarget;                  // 送审目标绝对路径（文件或目录）
    QString m_sRootDir;                 // 相对路径基（目标文件=其父目录；目录=自身）
    QStringList m_aAbsFiles;            // 展开后的送审清单
    int     m_iTotalFiles   = 0;
    int     m_iDoneFiles    = 0;
    int     m_iFindings     = 0;
    QString m_sReportMd;                // 最近一轮完整报告（保存按钮用）

    // 缓存复用：本轮开审指纹 + 逐文件结果累积（OnFileDone 填、OnReviewDone 落盘）。
    QJsonArray m_aFp;
    QJsonArray m_aCacheFiles;
    QString m_sCachedTime;              // 命中缓存时展示的旧结果时间

    class CReviewService* m_pSvc = nullptr;

    QLabel*      m_pLblTarget  = nullptr;
    CReviewSpinner* m_pSpin    = nullptr;   // 2026-10-02 审查中转圈指示器（状态行首）
    QLabel*      m_pLblStatus  = nullptr;
    QProgressBar* m_pProgress  = nullptr;
    QTreeWidget* m_pTree       = nullptr;
    QPushButton* m_pBtnStart   = nullptr;
    QPushButton* m_pBtnStop    = nullptr;
    QPushButton* m_pBtnFix     = nullptr;   // 2026-10-02 按建议修复（有发现才可用）
    QPushButton* m_pBtnSave    = nullptr;
    QPushButton* m_pBtnClose   = nullptr;
};
