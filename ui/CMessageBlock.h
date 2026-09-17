#pragma once

#include <QWidget>
#include <QString>
#include <QList>
#include <QStringList>

class QLabel;
class QTextBrowser;
class QPlainTextEdit;
class QToolButton;
class QVBoxLayout;
class QResizeEvent;
class QShowEvent;

// 对话流中的一条消息块，按种类切换布局，模拟 Claude Code 在 VS Code 中的呈现：
//   - Assistant：Markdown 富文本（setMarkdown）+ 品牌色 speaker 行 + 左侧强调条
//   - ToolUse  ：可折叠卡片（标题摘要 + 展开看入参/结果），Edit/Write 显示红绿 diff
//   - Event    ：阶段生命周期徽章（彩色圆点 + 文本）
//   - System   ：原始/系统信息（小号灰字）
//   - Error    ：错误（小号红字）
//
// 由 CExecutionPanel 持有；任务切换时由面板按缓存数据重建。
class CMessageBlock : public QWidget {
    Q_OBJECT

public:
    enum EKind { Assistant, ToolUse, Event, System, Error };

    explicit CMessageBlock(EKind a_eKind, QWidget* a_pParent = nullptr);

    // —— 按 kind 填充（只调对应那个）——
    void SetAssistantText(const QString& a_sMarkdown);
    // 2026.09.04 批量路径（切档重建/到顶补建）异步版：Markdown parse+初排版在子线程
    //   预排版（CMarkdownPreloader），完成回主线程 setDocument 落地——主线程只剩廉价
    //   落地+宽度微调，批量建块不再持续占用主线程（"加载日志卡"根因治理）。
    //   a_dEstWidth = 预估排版宽度（视口宽-块边距）；落地前先 setPlainText 占位防空白闪。
    void SetAssistantTextAsync(const QString& a_sMarkdown, qreal a_dEstWidth);
    // 2026.09.02 说话人标签改接块内记录的产出后端（旧版取渲染时刻的全局后端，
    //   切后端后重建视图会把历史回复整体改标，张冠李戴）；空串不生效（保持
    //   默认"Agent"＝旧数据/未知，不用当前后端冒充）。
    void SetSpeakerBackend(const QString& a_sName);
    void SetTool(const QString& a_sTool, const QString& a_sInputJson);
    // 2026.08.22 连续工具调用合并：追加一条进本卡（标题显示 ×N，展开看全部）
    void AddMergedTool(const QString& a_sTool, const QString& a_sInputJson);
    void SetResult(const QString& a_sResult);          // 工具结果回填（ToolUse 专用；合并卡按到达顺序填首个空结果）
    int  ToolCount() const;                            // 合并卡内工具条目数（单卡=1）
    void SetEvent(const QString& a_sKind, const QString& a_sText);
    void SetSystemText(const QString& a_sText);         // System / Error 共用（System 首行/复位）
    // 2026.08.29 反向借鉴 HarnessAgent：连续 System 行合并为一张折叠组卡，
    //   面板层把后续行并入首卡时逐行追加（组头首行摘要 + 共 N 行，默认折叠）
    void AppendSystemLine(const QString& a_sText);

    void SetExpanded(bool a_bExpanded);
    bool IsExpanded() const;

protected:
    void resizeEvent(QResizeEvent* a_pEvent) override;   // #2 块宽度变化 → 重排换行 + 重算高度
    void showEvent(QShowEvent* a_pEvent) override;       // #5 隐藏页中创建的块首次可见 → 补换行重排

private slots:
    void slotAdjustBrowserHeight();   // 助手 Markdown 块自适应高度

private:
    void BuildAssistant();
    void BuildToolCard();
    void BuildEvent();
    void BuildSystem(bool a_bError);
    void RefreshSysHeader();                            // System 折叠组头（摘要+行数）

    // 工具卡：从入参 JSON 生成标题摘要（取 file_path/command/pattern/path 等）
    static QString ToolSummary(const QString& a_sTool, const QString& a_sInputJson);
    // 2026.09.04 v2.0.113 旧轮次裸 JSON 事件行的渲染清洗（tooldone/think/tool→人读摘要，progress→空串丢弃）
    static QString PrettyLegacyJsonLine(const QString& a_sText);
    // 工具卡标题（含 ▶/▼ 箭头、首条摘要与合并 ×N 计数）
    QString HeaderText(bool a_bArrowDown) const;
    // 工具卡：构造 diff/入参内容
    void RebuildToolContent();

    EKind m_eKind;
    bool  m_bAdjusting = false;   // #2 防重入：setFixedHeight 触发的 resize 不再重入 slot

    // Assistant
    QTextBrowser*   m_pBrowser = nullptr;
    QString         m_sPendingMarkdown;   // 2026.09.04 异步预排版在途的原文（失败回退同步渲染用）
    void ApplyPreloadedDoc(class QTextDocument* a_pDoc, bool a_bFallbackSync);   // 预排版落地
    QLabel*         m_pSpeaker = nullptr;   // 2026.09.02 说话人标签（产出后端名；SetSpeakerBackend 回填）

    // ToolUse
    QToolButton*    m_pHeader   = nullptr;
    QWidget*        m_pContent  = nullptr;
    QVBoxLayout*    m_pContentLay = nullptr;   // 展开区布局（合并卡按条目重建）
    // 2026.08.22 合并卡：连续 ToolUse 条目列表（单卡也走此列表，长度 1）
    struct SToolEntry { QString sTool; QString sInput; QString sResult; };
    QList<SToolEntry> m_aTools;
    bool            m_bExpanded  = false;

    // Event / System / Error
    QLabel*         m_pLabel = nullptr;
    // System 折叠组：已归组的系统行（Error 不折叠，无此列表）
    QStringList     m_aSysLines;
};
