#include "CMessageBlock.h"

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
#include <QPointer>
#include "ui/CMarkdownPreloader.h"
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

    // 2026.09.02 说话人标签改用块内记录的产出后端（SetSpeakerBackend 回填；
    //   旧版取渲染时刻的全局后端，切后端后重建视图会把历史回复整体改标）。
    //   默认 "Agent"＝旧数据/未知，绝不用当前后端冒充。
    auto* pSpeaker = new QLabel(QStringLiteral("⏺ Agent"), this);
    pSpeaker->setStyleSheet(QStringLiteral(
        "color: #d97757; font-weight: 600; padding: 0;"));
    pV->addWidget(pSpeaker);
    m_pSpeaker = pSpeaker;

    m_pBrowser = new QTextBrowser(this);
    m_pBrowser->setReadOnly(true);
    m_pBrowser->setFrameShape(QFrame::NoFrame);
    m_pBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // #1 自动换行（按视口宽度）：关横向滚动条后必须显式开换行，否则长行溢出视口不可见。
    m_pBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
    m_pBrowser->setOpenExternalLinks(true);
    // 2026.08.26 修复右键菜单黑底：QPalette::Window 透明会随子控件创建传播到
    //   右键菜单（菜单背景用 Window 角色 → 透明/黑）。只保留 Base（浏览器自身
    //   文本区背景），不再碰 Window。
    QPalette pal = m_pBrowser->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    m_pBrowser->setPalette(pal);
    // 2026.08.26 修复右键菜单黑底：旧写法无选择器（"background: transparent; ..."）
    //   会级联到所有子控件——浏览器的标准右键菜单（createStandardContextMenu 的
    //   QMenu parent 即本浏览器）背景被改成透明，控件级样式表又优先于全局 QMenu
    //   样式 → 黑底黑字不可读。限定 QTextBrowser 选择器后不再波及菜单。
    m_pBrowser->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: transparent; border: none; }"));

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

void CMessageBlock::SetAssistantTextAsync(const QString& a_sMarkdown, qreal a_dEstWidth) {
    if (m_pBrowser == nullptr) {
        return;
    }
    // 先纯文本占位（廉价）：块立即有内容与大致高度，避免预排版回来前的空白闪烁；
    //   预排版完成（或失败）后覆盖为富文本。占位用纯文本不做 Markdown parse（正点）。
    m_pBrowser->setPlainText(a_sMarkdown);
    slotAdjustBrowserHeight();
    m_sPendingMarkdown = a_sMarkdown;   // 失败回退用（落地成功后清空）
    QPointer<CMessageBlock> pSelf = this;
    CMarkdownPreloader::Instance()->Submit(a_sMarkdown, a_dEstWidth,
        [pSelf](QTextDocument* a_pDoc) {
            if (pSelf == nullptr) {
                delete a_pDoc;   // 块已销毁（切档竞态）：doc 无主自清，防泄漏
                return;
            }
            pSelf->ApplyPreloadedDoc(a_pDoc, /*a_bFallbackSync=*/true);
        });
}

void CMessageBlock::ApplyPreloadedDoc(QTextDocument* a_pDoc, bool a_bFallbackSync) {
    // 预排版文档落地（主线程）：setDocument 接管显示。nullptr=预排版失败——
    //   a_bFallbackSync 时回退同步 setMarkdown（保内容正确，该块不加速）。
    if (m_pBrowser == nullptr) {
        delete a_pDoc;
        return;
    }
    if (a_pDoc == nullptr) {
        if (a_bFallbackSync && m_sPendingMarkdown.isEmpty() == false) {
            SetAssistantText(m_sPendingMarkdown);
        }
        return;
    }
    m_sPendingMarkdown.clear();
    a_pDoc->setParent(m_pBrowser);   // 所有权：随浏览器销毁（QTextBrowser 不强制收养）
    m_pBrowser->setDocument(a_pDoc);
    // 旧 document 被 setDocument 释放，其 layout 上的 documentSizeChanged 连接随之失效——
    //   对新 doc 的 layout 重连（自适应高度链路继续工作）。
    disconnect(m_pBrowser->document()->documentLayout(), nullptr, this, nullptr);
    connect(m_pBrowser->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &CMessageBlock::slotAdjustBrowserHeight);
    slotAdjustBrowserHeight();   // 真实宽度微调重排（预排版按预估宽度，此处校正）
}

void CMessageBlock::SetSpeakerBackend(const QString& a_sName) {
    // 2026.09.02 块内记录的产出后端回填说话人标签（CreateWidget 在 Assistant 块
    //   构建时调用）；空串不生效，保持默认"Agent"（旧数据/未知，不用当前后端冒充）。
    if (m_pSpeaker == nullptr || a_sName.trimmed().isEmpty()) {
        return;
    }
    m_pSpeaker->setText(QStringLiteral("⏺ ") + a_sName);
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
        if (a_bChecked) {
            RebuildToolContent();   // 展开时重建：折叠期间回填的结果/新并入条目此时才呈现
        }
        m_pContent->setVisible(a_bChecked);
        m_pHeader->setText(HeaderText(a_bChecked));   // 刷新 ▶/▼ 箭头与 ×N 计数
    });
    pV->addWidget(m_pHeader);

    m_pContent = new QWidget(this);
    m_pContentLay = new QVBoxLayout(m_pContent);
    m_pContentLay->setContentsMargins(0, 0, 0, 0);
    m_pContentLay->setSpacing(8);

    pV->addWidget(m_pContent);
    m_pContent->setVisible(false);   // 默认折叠
}

// 2026.08.22 标题：单卡=「▶ 图标 摘要」；合并卡=「▶ 图标 首条摘要 … ×N」
QString CMessageBlock::HeaderText(bool a_bArrowDown) const {
    if (m_aTools.isEmpty()) {
        return QString();
    }
    const SToolEntry& st0 = m_aTools.first();
    QString sBody = QStringLiteral("%1 %2")
        .arg(ToolIcon(st0.sTool), ToolSummary(st0.sTool, st0.sInput));
    if (m_aTools.size() > 1) {
        sBody += QStringLiteral("  … ×%1").arg(m_aTools.size());
    }
    return QStringLiteral("%1 %2").arg(a_bArrowDown ? QStringLiteral("▼") : QStringLiteral("▶"), sBody);
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

// 2026.08.22 展开区按条目重建（合并卡=多个「入参/结果」小节；单卡兼容=1 节）
void CMessageBlock::RebuildToolContent() {
    if (m_pContentLay == nullptr) {
        return;
    }
    // 清空旧条目控件
    while (m_pContentLay->count() > 0) {
        QLayoutItem* pItem = m_pContentLay->takeAt(0);
        if (pItem->widget() != nullptr) {
            pItem->widget()->deleteLater();
        }
        delete pItem;
    }
    for (int i = 0; i < m_aTools.size(); ++i) {
        const SToolEntry& st = m_aTools[i];
        auto* pLblInput = new QLabel(
            m_aTools.size() > 1 ? QStringLiteral("入参 #%1").arg(i + 1) : QStringLiteral("入参"),
            m_pContent);
        pLblInput->setStyleSheet(QStringLiteral("color: #6b7280;"));
        m_pContentLay->addWidget(pLblInput);

        auto* pIn = new QPlainTextEdit(m_pContent);
        pIn->setReadOnly(true);
        pIn->setFixedHeight(160);
        pIn->setStyleSheet(
            QStringLiteral("QPlainTextEdit { background: #fafafa; border: 1px solid #e5e7eb;"
                           " border-radius: 3px; font-family: %1; font-size: 11pt; }").arg(k_szMonoFont));

        // Edit/MultiEdit 特化：红绿 diff（取 old_string/new_string）
        const QString sToolLow = st.sTool.toLower();
        if (sToolLow == "edit" || sToolLow == "multiedit") {
            const QJsonObject o = QJsonDocument::fromJson(st.sInput.toUtf8()).object();
            const QString sOld = o.value("old_string").toString();
            const QString sNew = o.value("new_string").toString();

            QTextCharFormat fmtOld;
            fmtOld.setForeground(QColor("#b91c1c"));
            QTextCharFormat fmtNew;
            fmtNew.setForeground(QColor("#15803d"));

            QTextCursor cur(pIn->document());
            cur.insertText(QStringLiteral("--- 旧\n"), fmtOld);
            for (const QString& sLine : sOld.split('\n')) {
                cur.insertText(QStringLiteral("- ") + sLine + QStringLiteral("\n"), fmtOld);
            }
            cur.insertText(QStringLiteral("\n--- 新\n"), fmtNew);
            for (const QString& sLine : sNew.split('\n')) {
                cur.insertText(QStringLiteral("+ ") + sLine + QStringLiteral("\n"), fmtNew);
            }
        } else {
            // 其他工具：格式化 JSON 展示
            const QJsonDocument stDoc = QJsonDocument::fromJson(st.sInput.toUtf8());
            pIn->setPlainText(stDoc.isObject()
                ? QString::fromUtf8(stDoc.toJson(QJsonDocument::Indented))
                : st.sInput);
        }
        m_pContentLay->addWidget(pIn);

        auto* pLblResult = new QLabel(
            m_aTools.size() > 1 ? QStringLiteral("结果 #%1").arg(i + 1) : QStringLiteral("结果"),
            m_pContent);
        pLblResult->setStyleSheet(QStringLiteral("color: #6b7280;"));
        m_pContentLay->addWidget(pLblResult);

        auto* pOut = new QPlainTextEdit(m_pContent);
        pOut->setReadOnly(true);
        pOut->setFixedHeight(120);
        pOut->setStyleSheet(
            QStringLiteral("QPlainTextEdit { background: #fafafa; border: 1px solid #e5e7eb;"
                           " border-radius: 3px; font-family: %1; font-size: 11pt; }").arg(k_szMonoFont));
        pOut->setPlainText(st.sResult);
        m_pContentLay->addWidget(pOut);
    }
}

void CMessageBlock::SetTool(const QString& a_sTool, const QString& a_sInputJson) {
    SToolEntry st;
    st.sTool  = a_sTool;
    st.sInput = a_sInputJson;
    m_aTools.clear();
    m_aTools.append(st);
    if (m_pHeader != nullptr) {
        m_pHeader->setText(HeaderText(false));
    }
    RebuildToolContent();
}

void CMessageBlock::AddMergedTool(const QString& a_sTool, const QString& a_sInputJson) {
    SToolEntry st;
    st.sTool  = a_sTool;
    st.sInput = a_sInputJson;
    m_aTools.append(st);
    if (m_pHeader != nullptr) {
        m_pHeader->setText(HeaderText(m_bExpanded));
    }
    RebuildToolContent();   // 折叠态不可见，重建成本可忽略；展开中追加即时可见
}

int CMessageBlock::ToolCount() const {
    return m_aTools.size();
}

void CMessageBlock::SetResult(const QString& a_sResult) {
    // 2026.08.22 合并卡：结果按到达顺序回填到首个尚无结果的条目（与面板块序一致）
    int iTarget = -1;
    for (int i = 0; i < m_aTools.size(); ++i) {
        if (m_aTools[i].sResult.isEmpty()) { iTarget = i; break; }
    }
    if (iTarget < 0) {
        iTarget = m_aTools.size() - 1;   // 兜底：覆盖末条
    }
    m_aTools[iTarget].sResult = a_sResult;
    // 展开态：即时刷新该条结果框（折叠态等展开时由 RebuildToolContent 呈现）
    if (m_bExpanded && m_pContentLay != nullptr) {
        RebuildToolContent();
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
    // 2026.08.23 「你」徽标前缀用户头像图标（icon-generator 技能生成，qrc 内嵌）：
    //   仅 eventKind=user 加；QLabel 富文本 <img> 走 Qt 资源路径，行内基线对齐。
    const QString sIcon = (a_sKind == QStringLiteral("user"))
        ? QStringLiteral("<img src=':/icons/user.png' width='16' height='16'/> ")
        : QString();
    const QString sHtml = QStringLiteral(
        "%4<span style='color:%1; %3'>%2</span>"
        "<span style='color:#374151; %3'> %5</span>")
        .arg(QString::fromUtf8(st.szColor),
             QString::fromUtf8(st.szBullet),
             st.bBold ? QStringLiteral("font-weight:600;") : QString(),
             sIcon,
             sBody);
    m_pLabel->setText(sHtml);
    m_pLabel->setTextFormat(Qt::RichText);
}

// ---------------------------------------------------------------------------
// System：连续行折叠组（2026.08.29 反向借鉴 HarnessAgent，组头首行摘要+行数）
// Error：不折叠（错误重要且少见，保持独立红字）
// ---------------------------------------------------------------------------
void CMessageBlock::BuildSystem(bool a_bError) {
    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(10, 0, 0, 0);
    pV->setSpacing(2);

    if (a_bError) {
        pV->setSpacing(0);
        m_pLabel = new QLabel(this);
        m_pLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 支持鼠标选中复制
        m_pLabel->setWordWrap(true);
        m_pLabel->setStyleSheet(QStringLiteral("color: #dc2626;"));
        pV->addWidget(m_pLabel);
        return;
    }

    m_pHeader = new QToolButton(this);
    m_pHeader->setCheckable(true);
    m_pHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_pHeader->setStyleSheet(QStringLiteral(
        "QToolButton { text-align: left; background: transparent; border: none;"
        "              padding: 1px 0; color: #9ca3af; font-style: italic; }"
        "QToolButton:hover { color: #6b7280; }"));
    m_pHeader->setCursor(Qt::PointingHandCursor);
    connect(m_pHeader, &QToolButton::toggled, this, [this](bool a_bOn) {
        if (m_pLabel != nullptr) {
            m_pLabel->setVisible(a_bOn);
        }
        RefreshSysHeader();
    });
    pV->addWidget(m_pHeader);

    m_pLabel = new QLabel(this);
    m_pLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 支持鼠标选中复制
    m_pLabel->setWordWrap(true);
    m_pLabel->setStyleSheet(QStringLiteral("color: #9ca3af; font-style: italic;"));
    pV->addWidget(m_pLabel);
    m_pLabel->setVisible(false);   // 默认折叠
}

void CMessageBlock::SetSystemText(const QString& a_sText) {
    if (m_pLabel == nullptr) {
        return;
    }
    if (m_eKind == System) {
        // System 折叠组：首行入组（后续行由面板层 AppendSystemLine 并入）
        m_aSysLines.clear();
        m_aSysLines.append(a_sText);
        m_pLabel->setText(a_sText);
        RefreshSysHeader();
    } else {
        m_pLabel->setText(a_sText);   // Error：单条红字不折叠
    }
}

// 2026.08.29 连续 System 行并入本卡（面板层 TryMergeWidget 调用）
void CMessageBlock::AppendSystemLine(const QString& a_sText) {
    if (m_eKind != System) {
        return;
    }
    m_aSysLines.append(a_sText);
    if (m_pLabel != nullptr) {
        m_pLabel->setText(m_aSysLines.join(QLatin1Char('\n')));
    }
    RefreshSysHeader();
}

void CMessageBlock::RefreshSysHeader() {
    if (m_pHeader == nullptr || m_aSysLines.isEmpty()) {
        return;
    }
    const bool bOn = m_pHeader->isChecked();
    const QString sBrief = m_aSysLines.first().simplified().left(60);
    m_pHeader->setText(QStringLiteral("%1 %2（共 %3 行，点击展开）")
        .arg(bOn ? QStringLiteral("▾") : QStringLiteral("▸"),
             sBrief.toHtmlEscaped()).arg(m_aSysLines.size()));
}
