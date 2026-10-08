#pragma once

// ============================================================================
// CCodeReviewDialog —— 手动代码审查对话框（2026-09-30 ReviewSdk 接入 UI）
// ----------------------------------------------------------------------------
// 入口：资源管理器右键「代码审查」（文件或文件夹——文件夹递归收集代码文件）。
// 2026-10-08 Tab 化改造（用户需求）：
//   ① 两个 Tab：选择文件 / 审查；
//   ② 选择文件页：过滤下拉（C/C++|Python|C#|Delphi|VB|Rust|Go|Java + 全部，
//      档案存 config.ini [Review] Filters，可编辑/新增）+ 文件清单（复选框列，
//      状态列标注 修改/新增/无变化），右键菜单：选择/取消选择/开始审查；
//   ③ 审查页：发现表格与原版一致，行首加复选框（缺省全勾，按建议修复只带走
//      勾选行）；
//   ④ 顶部「全量审查」复选框（缺省不勾）：不勾=清单只列自上次审查后修改/新增
//      的文件，且逐文件走增量审查（仅审修改段，基线=上次审查成功时的内容快照，
//      存 exe 同级 review_cache/<目标哈希>.bl）；勾=全部文件全量审。
// 行为：非模态（审查分钟级，不锁主窗口）；双击发现行 → sigOpenFile 打开文件
// 预览页并定位行；引擎/后端与任务阶段预审同一套（ReviewSdk.dll + 当前全局后端）。
// ============================================================================

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QList>

#include "review/CReviewBaseline.h"   // 2026-10-08 基线仓（修改判定/增量旧文）

class QTreeWidget;
class QTreeWidgetItem;
class QProgressBar;
class QLabel;
class QPushButton;
class QTabWidget;
class QComboBox;
class QCheckBox;
class CReviewSpinner;   // 2026-10-02 审查中动态图标（cpp 内定义，同 CBusyIndicator 风格）

// —— 2026-10-08 过滤档案项（config.ini [Review] Filters，JSON [{n,e}]）——
// sExts：分号分隔小写扩展名（无点），如 "cpp;h;hpp"。
struct SReviewFilter {
    QString sName;
    QString sExts;
};

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
    // —— 2026-10-08 Tab 化 ——
    void OnFullToggled(bool a_bChecked);        // 全量审查开关 → 重建文件清单
    void OnFilterChanged(int a_iIndex);         // 过滤下拉切换 → 重建文件清单
    void OnEditFilters();                       // 编辑过滤档案（弹窗，可增/删/改）
    void OnFilesContextMenu(const QPoint& a_pt);   // 选择/取消选择/开始审查
    void OnFindingsContextMenu(const QPoint& a_pt);// 全选/取消全选（发现行复选框）

private:
    void BuildUi();
    void CollectFiles();       // 目标展开：文件直用；目录递归收集（扩展名过滤+上限）
    void PopulateFileList();   // 依 全量/当前过滤/基线 填「选择文件」清单
    void FillFilterCombo();     // 「全部」+ 各过滤档案项（编辑/开窗后重建，保名）
    void RefreshCountLabel();  // 文件页右上计数（共 N / 已勾 M / 修改+新增 K）
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

    // —— 2026-10-08 过滤档案（config.ini [Review] Filters，JSON 数组 [{n,e}]）——
    QList<SReviewFilter> LoadFilters() const;
    void SaveFilters(const QList<SReviewFilter>& a_aFilters);
    static QList<QByteArray> ExtsOfProfile(const SReviewFilter& a_stProfile);
    QList<QByteArray> AllFilterExts() const;   // 全部档案扩展名并集（=「全部」口径）

    // 基线仓目录（exe 同级 review_cache/<目标哈希>.bl）。
    QString BaselineDirPath() const;
    // 单文件修改判定：无基线=新增；mtime/size 不符=修改；否则无变化。
    enum EModState { eModNew = 0, eModChanged = 1, eModSame = 2 };
    EModState ModStateOf(const QString& a_sRel, const QFileInfo& a_stInfo) const;

    QString m_sTarget;                  // 送审目标绝对路径（文件或目录）
    QString m_sRootDir;                 // 相对路径基（目标文件=其父目录；目录=自身）
    QStringList m_aAbsFiles;            // 展开后的全部候选清单（过滤前）
    int     m_iTotalFiles   = 0;
    int     m_iDoneFiles    = 0;
    int     m_iFindings     = 0;
    QString m_sReportMd;                // 最近一轮完整报告（保存按钮用）

    // 缓存复用：本轮开审指纹 + 逐文件结果累积（OnFileDone 填、OnReviewDone 落盘）。
    QJsonArray m_aFp;
    QJsonArray m_aCacheFiles;
    QString m_sCachedTime;              // 命中缓存时展示的旧结果时间

    // —— 2026-10-08 Tab 化状态 ——
    QList<SReviewFilter> m_aFilters;    // 过滤档案（开窗载入，编辑后重载）
    CReviewBaseline::TMap m_mapBase;    // 基线快照（开窗/重建清单时重读）
    bool    m_bReviewRunning = false;   // 本窗一轮进行中（清单重建保护）
    bool    m_bPopulating    = false;   // 文件清单填充中（itemChanged 抑制）

    class CReviewService* m_pSvc = nullptr;

    QLabel*      m_pLblTarget  = nullptr;
    QCheckBox*   m_pChkFull    = nullptr;   // 全量审查（缺省不勾）
    QTabWidget*  m_pTabs       = nullptr;
    // 选择文件页
    QComboBox*   m_pCboFilter  = nullptr;
    QPushButton* m_pBtnEditFit = nullptr;
    QLabel*      m_pLblCount   = nullptr;
    QTreeWidget* m_pTreeFiles  = nullptr;
    // 审查页
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
