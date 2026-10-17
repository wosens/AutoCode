#pragma once

#include <QWidget>
#include <QString>
#include <QList>
#include <QStringList>

class QLabel;
class QFrame;
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
    // 2026.10.16 视口惰性排版（"打开助手日志还是慢"二期）：批量建件时不再同步 setPlainText
    //   占位（200 块 × 全文纯文本排版仍是主线程大头），只记文本 + 估算高度锁块——正文
    //   排版推迟到块滚入视口带（MaterializeAssistant，由 CMessageView 驱动）；远离视口
    //   再弃正文（DematerializeAssistant，锁定当前高度防滚动跳动，文本保留待再排版）。
    //   思路同 Chatterino 的 RequiresLayout 脏标记惰性布局：窗口内只有视口附近的块
    //   付排版成本，窗口其余块近乎零成本落地。
    void SetAssistantTextDeferred(const QString& a_sMarkdown, qreal a_dEstWidth);
    bool MaterializeAssistant();      // 滚入视口带：占位+预排版上屏；false=非惰性/已排版
    void DematerializeAssistant();    // 远离视口带：弃正文保高度；false 路径自守卫
    bool IsLazyAssistant() const { return m_bLazy && m_eKind == Assistant; }
    bool AssistantMaterialized() const { return !m_bLazy || m_bMaterialized; }   // 测试观察口：正文已排版（非惰性块恒 true）
    // 2026.09.02 说话人标签改接块内记录的产出后端（旧版取渲染时刻的全局后端，
    //   切后端后重建视图会把历史回复整体改标，张冠李戴）；空串不生效（保持
    //   默认"Agent"＝旧数据/未知，不用当前后端冒充）。
    void SetSpeakerBackend(const QString& a_sName);
    void SetTool(const QString& a_sTool, const QString& a_sInputJson);
    // 2026.08.22 连续工具调用合并：追加一条进本卡（标题显示 ×N，展开看全部）
    void AddMergedTool(const QString& a_sTool, const QString& a_sInputJson);
    void SetResult(const QString& a_sResult, bool a_bAutoExpandOnError = false);   // 工具结果回填（ToolUse 专用；合并卡按到达顺序填首个空结果；自动展开仅限实时错误单条卡）
    int  ToolCount() const;                            // 合并卡内工具条目数（单卡=1）
    void SetEvent(const QString& a_sKind, const QString& a_sText);
    void SetSystemText(const QString& a_sText);         // System / Error 共用（System 首行/复位）
    // 2026.08.29 反向借鉴 HarnessAgent：连续 System 行合并为一张折叠组卡，
    //   面板层把后续行并入首卡时逐行追加（组头首行摘要 + 共 N 行，默认折叠）
    void AppendSystemLine(const QString& a_sText);

    void SetExpanded(bool a_bExpanded);
    bool IsExpanded() const;

    // —— 双引擎共享纯函数（2026-10-17 自绘三期：Legacy 卡片流与 ModelView 自绘链
    //    （CChatModel/CChatDelegate）同源共用，改口径须双改+双测）——
    // 判断工具结果是否为错误（Claude tool_result 的 is_error 未单独透传，按文本启发式）
    static bool IsErrorResult(const QString& a_sResult);
    // v2.0.113 旧轮次裸 JSON 事件行的人读摘要清洗（tooldone/think/tool→摘要、
    // progress→空串=丢弃；非裸 JSON 行原样返回）。载档渲染两侧共用。
    static QString PrettyLegacyJsonLine(const QString& a_sText);
    // 工具卡标题摘要（取 file_path/command/pattern/path/query/url/note_id；
    // a_iTargetMax=路径截断 80 / a_iFallbackMax=整串回退截断 60——delegate 复用）
    static QString ToolSummary(const QString& a_sTool, const QString& a_sInputJson,
                               int a_iTargetMax = 80, int a_iFallbackMax = 60);

protected:
    void resizeEvent(QResizeEvent* a_pEvent) override;   // #2 块宽度变化 → 重排换行 + 重算高度
    void showEvent(QShowEvent* a_pEvent) override;       // #5 隐藏页中创建的块首次可见 → 补换行重排

private:
    int  EstimateAssistantHeight(const QString& a_sMarkdown, qreal a_dWidthPx) const;   // 惰性占位估高
    void ConnectDocSignals();   // 当前 document 的 layout ↔ 自适应高度 接线（换 doc 后必重连）

private slots:
    void slotAdjustBrowserHeight();   // 助手 Markdown 块自适应高度

private:
    void BuildAssistant();
    void BuildToolCard();
    void BuildEvent();
    void BuildSystem(bool a_bError);
    void RefreshSysHeader();                            // System 折叠组头（摘要+行数）

    // 工具卡标题（含 ▶/▼ 箭头、首条摘要与合并 ×N 计数）
    QString HeaderText(bool a_bArrowDown) const;
    // 2026.09.22 工具卡标题单行化：按当前宽度省略号截断（ElideMiddle），全文进 tooltip
    void UpdateToolHeaderText();
    // 工具卡：构造 diff/入参内容
    void RebuildToolContent();

    EKind m_eKind;
    bool  m_bAdjusting = false;   // #2 防重入：setFixedHeight 触发的 resize 不再重入 slot

    // Assistant
    QTextBrowser*   m_pBrowser = nullptr;
    QString         m_sPendingMarkdown;   // 2026.09.04 异步预排版在途的原文（失败回退同步渲染用）
    void ApplyPreloadedDoc(class QTextDocument* a_pDoc, bool a_bFallbackSync);   // 预排版落地
    QLabel*         m_pSpeaker = nullptr;   // 2026.09.02 说话人标签（产出后端名；SetSpeakerBackend 回填）
    // 2026.10.16 视口惰性排版：批量建件只记文本+估高（m_bLazy=true），滚入视口带才真正
    //   排版（MaterializeAssistant），远离再弃正文（DematerializeAssistant）——流式单块
    //   不经此链（出生即 m_bMaterialized=true，行为与旧版一致）。
    bool            m_bLazy = false;          // 批量路径 Assistant 块（参与视口惰性排版）
    bool            m_bMaterialized = true;   // 正文已排版（false=仅估高占位/已弃正文）
    QString         m_sLazyMarkdown;          // 惰性正文源（估高/再排版；QString 隐式共享无拷贝成本）
    qreal           m_dLazyWidth = 0.0;       // 估宽（Materialize 提交预排版用）

    // ToolUse
    QToolButton*    m_pHeader   = nullptr;
    QWidget*        m_pContent  = nullptr;
    QVBoxLayout*    m_pContentLay = nullptr;   // 展开区布局（合并卡按条目重建）
    // 2026.08.22 合并卡：连续 ToolUse 条目列表（单卡也走此列表，长度 1）
    struct SToolEntry { QString sTool; QString sInput; QString sResult; };
    QList<SToolEntry> m_aTools;
    bool            m_bExpanded  = false;
    // 2026.10.16 工具卡展开区懒重建：折叠期 SetTool/AddMergedTool/SetResult 只标脏不建件
    //   （×N 合并卡逐条并入即重建 O(N²) 控件创建，是载档卡顿主因之一——折叠态展开区
    //   本就不可见，展开时一次建齐即可）；展开态保持即时重建（实时回填可见）。
    bool            m_bToolDirty = false;

    // Event / System / Error
    QLabel*         m_pLabel = nullptr;
    // 2026.09.18 用户提示词（eventKind=user）顶部贯穿左右的 3px 分隔横线
    //   （仅助手日志的用户块；构建于 SetEvent 首次遇到 user 时，幂等）。
    QFrame*         m_pUserSep = nullptr;
    // System 折叠组：已归组的系统行（Error 不折叠，无此列表）
    QStringList     m_aSysLines;
};
