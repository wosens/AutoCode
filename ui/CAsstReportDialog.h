#pragma once

// ===========================================================================
// CAsstReportDialog —— 助手分析报告页（2026-11-04 用户需求）
//
// 入口：助手 Tab 右键「查看分析报告」（CExecutionPanel::TriggerAsstReportView）。
// 语义：
//   · 之前生成过报告 → 打开即直接显示（CMainWindow 读 .autodev/asst-reports/
//     <asstId>/report.md 回填，ShowReport）；
//   · 自上次报告后对话日志有新增（面板 AsstChatFingerprint 变化）→ 旧报告照常
//     显示 + 顶部横幅提示「正在后台重新总结」，编排器报告轮（AsstReport 模式，
//     只读 fork 一次性分支）完成后经 ApplyAsstReport 自动替换为新报告；
//   · 从未生成过 → 显示「正在后台生成…」占位提示，完成后同样自动回填；
//   · 无任何对话内容（没有用户提示词）→ 静态提示，不起报告轮。
// 报告正文的 Markdown 渲染复用 CArtifactView（阶段产物查看器）；「导出报告」
// 按钮把原始 Markdown 另存为 .md 文件（UTF-8 with BOM）。
//
// 非模态、WA_DeleteOnClose：宿主面板持 QPointer，关闭即销毁；同一时刻单实例，
// 切换查看另一助手的报告时复用本窗口换内容。
// ===========================================================================

#include <QDialog>

class QLabel;
class QPushButton;
class CArtifactView;

class CAsstReportDialog : public QDialog {
    Q_OBJECT

public:
    explicit CAsstReportDialog(QWidget* a_pParent = nullptr);

    // 展示报告正文。a_sMetaNote：头部说明行（如「生成时间：2026-11-04 12:00」）；
    //   a_bStale=true 表示该内容落后于当前对话（后台正在重新总结，横幅提示）。
    void ShowReport(const QString& a_sAsstId, const QString& a_sAsstName,
                    const QString& a_sReport, const QString& a_sMetaNote,
                    bool a_bStale = false);

    // 后台总结进行中/结束的横幅状态。仅当窗口当前展示的正是该助手（或尚无内容）
    //   时生效——a_bOn=true 显示转圈提示（保留旧正文不动），false 收起横幅。
    void SetGenerating(const QString& a_sAsstId, bool a_bOn, const QString& a_sHint);

    // 静态提示态（无对话内容/生成失败等）：清空正文改显提示，禁用导出。
    void SetHint(const QString& a_sAsstId, const QString& a_sAsstName,
                 const QString& a_sHint);

    QString AsstId() const { return m_sAsstId; }        // 当前展示的助手 id（空=未展示）
    bool HasReport() const { return !m_sReport.isEmpty(); }   // 是否持有可导出的正文

private slots:
    void OnExport();   // 「导出报告…」：另存原始 Markdown（UTF-8 with BOM）

private:
    void ApplyBanner();   // 按 m_sBanner/m_bGenerating 刷新横幅显隐与文案

    QLabel*         m_pHead = nullptr;      // 标题行：助手名 + 生成时间/说明
    QLabel*         m_pBanner = nullptr;    // 顶部状态横幅（生成中/提示/导出结果）
    CArtifactView*  m_pView = nullptr;      // Markdown 正文
    QPushButton*    m_pBtnExport = nullptr; // 导出报告

    QString m_sAsstId;      // 当前展示的助手 id
    QString m_sAsstName;    // 助手名（导出缺省文件名用）
    QString m_sReport;      // 报告原始 Markdown（导出正本）
    QString m_sBanner;      // 横幅文案（空=隐藏；生成中态由 m_bGenerating 叠加前缀）
    bool    m_bGenerating = false;   // 后台总结进行中（横幅保持显示直到完成/失败）
};
