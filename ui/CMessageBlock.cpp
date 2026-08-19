#include "CMessageBlock.h"
#include "../agent/CAgentRunnerFactory.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QTextBrowser>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QPalette>
#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// 事件 kind → 徽章样式
struct SEventStyle {
    const char* szBullet;
    const char* szColor;
    bool        bBold;
};
SEventStyle EventStyle(const QString& a_sKind) {
    if (a_sKind == QStringLiteral("stage_start"))   return {"●",  "#2563eb", false};
    if (a_sKind == QStringLiteral("stage_done"))    return {"✔",  "#16a34a", false};
    if (a_sKind == QStringLiteral("stage_failed"))  return {"✗",  "#dc2626", false};
    if (a_sKind == QStringLiteral("reject"))        return {"↩",  "#dc2626", false};
    if (a_sKind == QStringLiteral("escalation"))    return {"⚠",  "#ea580c", true};
    if (a_sKind == QStringLiteral("done"))          return {"🎉", "#16a34a", true};
    if (a_sKind == QStringLiteral("paused") ||
        a_sKind == QStringLiteral("pause_requested")) return {"⏸", "#6b7280", false};
    if (a_sKind == QStringLiteral("stopped"))       return {"⏹",  "#6b7280", false};
    if (a_sKind == QStringLiteral("need_guide"))    return {"⚠",  "#ea580c", false};
    if (a_sKind == QStringLiteral("user"))          return {"你",  "#2563eb", true};
    return                                          {"•",  "#6b7280", false};
}

// 工具名 → 图标字符
QString ToolIcon(const QString& a_sTool) {
    const QString s = a_sTool.toLower();
    if (s.startsWith(QStringLiteral("mcp__autocode__ask_user"))) return QStringLiteral("❓");
    if (s == "read")    return QStringLiteral("📖");
    if (s == "edit" || s == "write" || s == "multiedit") return QStringLiteral("✏️");
    if (s == "bash")    return QStringLiteral("💻");
    if (s == "grep")    return QStringLiteral("🔍");
    if (s == "glob")    return QStringLiteral("🗂");
    return QStringLiteral("🔧");
}

// 判断工具结果是否为错误（Claude tool_result 的 is_error 未单独透传，按文本启发式）
bool IsErrorResult(const QString& a_sResult) {
    return a_sResult.contains(QStringLiteral("<error>")) ||
           a_sResult.startsWith(QStringLiteral("Error:")) ||
           a_sResult.startsWith(QStringLiteral("[错误]"));
}

const char* k_szMonoFont = "Consolas, 'Courier New', monospace";

} // namespace

CMessageBlock::CMessageBlock(EKind a_eKind, QWidget* a_pParent)
    : QWidget(a_pParent), m_eKind(a_eKind) {
    // 让 QWidget 能响应 stylesheet 的背景/边框
    setAttribute(Qt::WA_StyledBackground, true);

    switch (m_eKind) {
        case Assistant: BuildAssistant(); break;
        case ToolUse:   BuildToolCard();  break;
        case Event:     BuildEvent();     break;
        case System:    BuildSystem(false); break;
        case Error:     BuildSystem(true);  break;
    }
}

// ---------------------------------------------------------------------------
// Assistant：Markdown 富文本 + speaker 行 + 左侧强调条
// ---------------------------------------------------------------------------
void CMessageBlock::BuildAssistant() {
    setStyleSheet(QStringLiteral(
        "CMessageBlock { border-left: 3px solid #d97757; padding-left: 10px; }"));

    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(0, 0, 0, 0);
    pV->setSpacing(4);

    // 2026.08.18 说话人标签随当前 Agent 后端显示（旧版硬编码 "⏺ Claude"，DSH 后端下误导）。
    auto* pSpeaker = new QLabel(
        QStringLiteral("⏺ ") + CAgentRunnerFactory::BackendDisplayName(), this);
    pSpeaker->setStyleSheet(QStringLiteral(
        "color: #d97757; font-weight: 600; padding: 0;"));
    pV->addWidget(pSpeaker);

    m_pBrowser = new QTextBrowser(this);
    m_pBrowser->setReadOnly(true);
    m_pBrowser->setFrameShape(QFrame::NoFrame);
    m_pBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // #1 自动换行（按视口宽度）：关横向滚动条后必须显式开换行，否则长行溢出视口不可见。
    m_pBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
    m_pBrowser->setOpenExternalLinks(true);
    QPalette pal = m_pBrowser->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Window, Qt::transparent);
    m_pBrowser->setPalette(pal);
    m_pBrowser->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    connect(m_pBrowser->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &CMessageBlock::slotAdjustBrowserHeight);

    pV->addWidget(m_pBrowser);
}

void CMessageBlock::slotAdjustBrowserHeight() {
    // #2 助手 Markdown 块自适应高度 + 换行：按浏览器视口宽度 setTextWidth 重排，
    //   使长行按视口宽度折行（setMarkdown 早于布局就绪时 WidgetWidth 不一定生效，
    //   显式 setTextWidth 兜底）；防重入避免 setFixedHeight→resizeEvent 回环。
    if (m_pBrowser == nullptr || m_bAdjusting) {
        return;
    }
    m_bAdjusting = true;
    QTextDocument* pDoc = m_pBrowser->document();
    const int iViewportW = m_pBrowser->viewport()->width();
    if (iViewportW > 0) {
        pDoc->setTextWidth(iViewportW);   // 按当前视口宽度重排：换行 + 触发正确高度
    }
    const int iContentH = static_cast<int>(pDoc->size().height());
    const int iFrame = m_pBrowser->frameWidth() * 2;
    m_pBrowser->setFixedHeight(iContentH + iFrame + 2);
    m_bAdjusting = false;
}

void CMessageBlock::resizeEvent(QResizeEvent* a_pEvent) {
    QWidget::resizeEvent(a_pEvent);
    // #2 块宽度变化（首次布局就绪 / 视口缩放）→ 按新宽度重排换行并重算高度：
    //   setMarkdown 发生在块尚未获得真实宽度时，此时 setTextWidth 用的是 0/默认宽度，
    //   导致助手长行不换行、被裁出右沿（关掉 CMessageView 横向滚动条后尤为明显）。
    //   注意布局滞后一拍：块 resize 时子 m_pBrowser 尚未布局到新宽度（viewport 仍旧值），
    //   立即调整会读到过期宽度——延迟到事件循环下一拍，待子控件布局稳定后再重排。
    if (m_eKind == Assistant && m_pBrowser != nullptr) {
        QTimer::singleShot(0, this, &CMessageBlock::slotAdjustBrowserHeight);
    }
}

void CMessageBlock::showEvent(QShowEvent* a_pEvent) {
    QWidget::showEvent(a_pEvent);
    // #5 隐藏页（如助手 Tab 非当前页）中创建的块从未获得真实宽度，viewport 恒 0、
    //   setTextWidth 被跳过 → 不换行；切回该页时块尺寸可能不变（仅可见性变，不触发
    //   resizeEvent）——show 时补一次延迟重排，换行按此时已稳定的视口宽度生效。
    if (m_eKind == Assistant && m_pBrowser != nullptr) {
        QTimer::singleShot(0, this, &CMessageBlock::slotAdjustBrowserHeight);
    }
}

void CMessageBlock::SetAssistantText(const QString& a_sMarkdown) {
    if (m_pBrowser == nullptr) {
        return;
    }
    m_pBrowser->setMarkdown(a_sMarkdown);
    slotAdjustBrowserHeight();
}

// ---------------------------------------------------------------------------
// ToolUse：可折叠卡片
// ---------------------------------------------------------------------------
void CMessageBlock::BuildToolCard() {
    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(0, 0, 0, 0);
    pV->setSpacing(4);

    m_pHeader = new QToolButton(this);
    m_pHeader->setCheckable(true);
    m_pHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_pHeader->setStyleSheet(QStringLiteral(
        "QToolButton { text-align: left; background: #f3f4f6; border: 1px solid #d1d5db;"
        "              border-radius: 4px; padding: 5px 8px; color: #374151; }"
        "QToolButton:hover { background: #e5e7eb; }"
        "QToolButton:checked { background: #e5e7eb; border-color: #9ca3af; }"));
    connect(m_pHeader, &QToolButton::toggled, this, [this](bool a_bChecked) {
        m_bExpanded = a_bChecked;
        m_pContent->setVisible(a_bChecked);
        // 重建标题以刷新 ▶/▼ 箭头
        m_pHeader->setText(QStringLiteral("%1 %2 %3")
            .arg(a_bChecked ? QStringLiteral("▼") : QStringLiteral("▶"),
                 ToolIcon(m_sTool),
                 ToolSummary(m_sTool, m_sInput)));
    });
    pV->addWidget(m_pHeader);

    m_pContent = new QWidget(this);
    auto* pCV = new QVBoxLayout(m_pContent);
    pCV->setContentsMargins(0, 0, 0, 0);
    pCV->setSpacing(4);

    auto* pLblInput = new QLabel(QStringLiteral("入参"), m_pContent);
    pLblInput->setStyleSheet(QStringLiteral("color: #6b7280;"));
    pCV->addWidget(pLblInput);

    m_pInputEdit = new QPlainTextEdit(m_pContent);
    m_pInputEdit->setReadOnly(true);
    m_pInputEdit->setFixedHeight(160);
    m_pInputEdit->setStyleSheet(
        QStringLiteral("QPlainTextEdit { background: #fafafa; border: 1px solid #e5e7eb;"
                       " border-radius: 3px; font-family: %1; font-size: 11pt; }").arg(k_szMonoFont));
    pCV->addWidget(m_pInputEdit);

    auto* pLblResult = new QLabel(QStringLiteral("结果"), m_pContent);
    pLblResult->setStyleSheet(QStringLiteral("color: #6b7280;"));
    pCV->addWidget(pLblResult);

    m_pResultEdit = new QPlainTextEdit(m_pContent);
    m_pResultEdit->setReadOnly(true);
    m_pResultEdit->setFixedHeight(120);
    m_pResultEdit->setStyleSheet(
        QStringLiteral("QPlainTextEdit { background: #fafafa; border: 1px solid #e5e7eb;"
                       " border-radius: 3px; font-family: %1; font-size: 11pt; }").arg(k_szMonoFont));
    pCV->addWidget(m_pResultEdit);

    pV->addWidget(m_pContent);
    m_pContent->setVisible(false);   // 默认折叠
}

QString CMessageBlock::ToolSummary(const QString& a_sTool, const QString& a_sInputJson) {
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sInputJson.toUtf8());
    const QJsonObject o = stDoc.object();

    QString sTarget;
    if (o.contains("file_path"))      sTarget = o.value("file_path").toString();
    else if (o.contains("command"))   sTarget = o.value("command").toString();
    else if (o.contains("pattern"))   sTarget = o.value("pattern").toString();
    else if (o.contains("path"))      sTarget = o.value("path").toString();
    else if (o.contains("query"))     sTarget = o.value("query").toString();
    else if (o.contains("url"))       sTarget = o.value("url").toString();
    else if (o.contains("note_id"))   sTarget = o.value("note_id").toString();

    if (sTarget.isEmpty()) {
        sTarget = a_sInputJson;
        if (sTarget.size() > 60) {
            sTarget = sTarget.left(60) + QStringLiteral("...");
        }
    } else if (sTarget.size() > 80) {
        sTarget = sTarget.left(80) + QStringLiteral("...");
    }
    return a_sTool + QStringLiteral("(") + sTarget + QStringLiteral(")");
}

void CMessageBlock::RebuildToolContent() {
    if (m_pInputEdit == nullptr) {
        return;
    }
    QTextCursor cur(m_pInputEdit->document());
    cur.select(QTextCursor::Document);
    cur.removeSelectedText();

    // Edit/MultiEdit 特化：红绿 diff（取 old_string/new_string）
    const QString sToolLow = m_sTool.toLower();
    if (sToolLow == "edit" || sToolLow == "multiedit") {
        const QJsonObject o = QJsonDocument::fromJson(m_sInput.toUtf8()).object();
        const QString sOld = o.value("old_string").toString();
        const QString sNew = o.value("new_string").toString();

        QTextCharFormat fmtOld;
        fmtOld.setForeground(QColor("#b91c1c"));
        QTextCharFormat fmtNew;
        fmtNew.setForeground(QColor("#15803d"));

        cur.insertText(QStringLiteral("--- 旧\n"), fmtOld);
        for (const QString& sLine : sOld.split('\n')) {
            cur.insertText(QStringLiteral("- ") + sLine + QStringLiteral("\n"), fmtOld);
        }
        cur.insertText(QStringLiteral("\n--- 新\n"), fmtNew);
        for (const QString& sLine : sNew.split('\n')) {
            cur.insertText(QStringLiteral("+ ") + sLine + QStringLiteral("\n"), fmtNew);
        }
        return;
    }

    // 其他工具：格式化 JSON 展示
    const QJsonDocument stDoc = QJsonDocument::fromJson(m_sInput.toUtf8());
    const QString sShown = stDoc.isObject()
        ? QString::fromUtf8(stDoc.toJson(QJsonDocument::Indented))
        : m_sInput;
    cur.insertText(sShown);
}

void CMessageBlock::SetTool(const QString& a_sTool, const QString& a_sInputJson) {
    m_sTool = a_sTool;
    m_sInput = a_sInputJson;
    if (m_pHeader != nullptr) {
        m_pHeader->setText(QStringLiteral("▶ %1 %2")
            .arg(ToolIcon(a_sTool), ToolSummary(a_sTool, a_sInputJson)));
    }
    RebuildToolContent();
}

void CMessageBlock::SetResult(const QString& a_sResult) {
    m_sResult = a_sResult;
    if (m_pResultEdit != nullptr) {
        m_pResultEdit->setPlainText(a_sResult);
    }
    // 错误结果：标题栏标红 + 自动展开
    if (IsErrorResult(a_sResult) && m_pHeader != nullptr) {
        m_pHeader->setStyleSheet(QStringLiteral(
            "QToolButton { text-align: left; background: #fee2e2; border: 1px solid #ef4444;"
            "              border-radius: 4px; padding: 5px 8px; color: #b91c1c; }"
            "QToolButton:hover { background: #fecaca; }"));
        if (!m_bExpanded) {
            m_pHeader->setChecked(true);   // 触发 toggled → 展开
        }
    }
}

void CMessageBlock::SetExpanded(bool a_bExpanded) {
    if (m_pHeader != nullptr) {
        m_pHeader->setChecked(a_bExpanded);
    }
}

bool CMessageBlock::IsExpanded() const {
    return m_bExpanded;
}

// ---------------------------------------------------------------------------
// Event：彩色徽章
// ---------------------------------------------------------------------------
void CMessageBlock::BuildEvent() {
    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(0, 0, 0, 0);
    pV->setSpacing(0);
    m_pLabel = new QLabel(this);
    m_pLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 支持鼠标选中复制
    // #3 自动换行：用户提示词（eventKind=user）可能含 \n 且较长，开 wordWrap 防溢出；
    //   \n→<br> 的转换在 SetEvent 中处理（RichText 模式下 \n 会被折叠成空格）。
    m_pLabel->setWordWrap(true);
    pV->addWidget(m_pLabel);
}

void CMessageBlock::SetEvent(const QString& a_sKind, const QString& a_sText) {
    if (m_pLabel == nullptr) {
        return;
    }
    const SEventStyle st = EventStyle(a_sKind);
    // szBullet/szColor 是 UTF-8 字节字面量（❯/🎉/● 等非 ASCII），必须 fromUtf8 解码；
    // 早先用 fromLatin1 会把多字节序列按单字节 ISO-8859-1 拆解 → 显示成 â¯ / ð 之类乱码。
    // #3 用户提示词的原始换行需保留：toHtmlEscaped 后再把 \n 转 <br>，
    //   否则 RichText QLabel 会把 \n 当空白折叠成空格（多行提示词挤成一行看不清）。
    QString sBody = a_sText.toHtmlEscaped();
    sBody.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    const QString sHtml = QStringLiteral(
        "<span style='color:%1; %3'>%2</span>"
        "<span style='color:#374151; %3'> %4</span>")
        .arg(QString::fromUtf8(st.szColor),
             QString::fromUtf8(st.szBullet),
             st.bBold ? QStringLiteral("font-weight:600;") : QString(),
             sBody);
    m_pLabel->setText(sHtml);
    m_pLabel->setTextFormat(Qt::RichText);
}

// ---------------------------------------------------------------------------
// System / Error：弱化小字
// ---------------------------------------------------------------------------
void CMessageBlock::BuildSystem(bool a_bError) {
    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(10, 0, 0, 0);
    pV->setSpacing(0);
    m_pLabel = new QLabel(this);
    m_pLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 支持鼠标选中复制
    m_pLabel->setWordWrap(true);
    m_pLabel->setStyleSheet(QStringLiteral("%1")
        .arg(a_bError ? QStringLiteral("color: #dc2626;")
                      : QStringLiteral("color: #9ca3af; font-style: italic;")));
    pV->addWidget(m_pLabel);
}

void CMessageBlock::SetSystemText(const QString& a_sText) {
    if (m_pLabel != nullptr) {
        m_pLabel->setText(a_sText);
    }
}
