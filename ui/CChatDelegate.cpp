#include "CChatDelegate.h"

#include "CChatDocPool.h"

#include <QColor>
#include <QDesktopServices>
#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolTip>
#include <QUrl>
#include <QVariant>

namespace {

// —— 视觉常量（对齐 Legacy CMessageBlock QSS 实测值；灰度期双引擎双份同源，
//    改口径须双改+双测；Q-1 若判直接替换则随 Legacy 一并消失）——
const QColor kColAssistantBar(QStringLiteral("#d97757"));   // Assistant 左竖条/speaker
const QColor kColToolBg(QStringLiteral("#f3f4f6"));
const QColor kColToolBorder(QStringLiteral("#d1d5db"));
const QColor kColToolText(QStringLiteral("#374151"));
const QColor kColToolErrBg(QStringLiteral("#fee2e2"));
const QColor kColToolErrBorder(QStringLiteral("#ef4444"));
const QColor kColToolErrText(QStringLiteral("#b91c1c"));
const QColor kColLabel(QStringLiteral("#6b7280"));
const QColor kColMonoBg(QStringLiteral("#fafafa"));
const QColor kColMonoBorder(QStringLiteral("#e5e7eb"));
const QColor kColDiffOld(QStringLiteral("#b91c1c"));
const QColor kColDiffNew(QStringLiteral("#15803d"));
const QColor kColUserBadge(QStringLiteral("#2563eb"));
const QColor kColBody(QStringLiteral("#374151"));
const QColor kColErr(QStringLiteral("#dc2626"));
const QColor kColSys(QStringLiteral("#9ca3af"));
const QColor kColHighlight(QStringLiteral("#fffbeb"));
const QColor kColMore(QStringLiteral("#9ca3af"));
const QColor kColUserSep(QStringLiteral("#9ca3af"));   // 2026.09.19 用户提示词顶部分隔线
const QColor kColMonoText(0, 0, 0);            // QPlainTextEdit 缺省黑字口径

// 工具名 → 图标字符（CMessageBlock.cpp 匿名命名空间同源复制；灰度期双份）
QString ToolIcon(const QString& a_sTool)
{
    const QString s = a_sTool.toLower();
    if (s.startsWith(QStringLiteral("mcp__autocode__ask_user"))) return QStringLiteral("❓");
    if (s == QLatin1String("read"))    return QStringLiteral("📖");
    if (s == QLatin1String("edit") || s == QLatin1String("write")
        || s == QLatin1String("multiedit")) return QStringLiteral("✏️");
    if (s == QLatin1String("bash"))    return QStringLiteral("💻");
    if (s == QLatin1String("grep"))    return QStringLiteral("🔍");
    if (s == QLatin1String("glob"))    return QStringLiteral("🗂");
    return QStringLiteral("🔧");
}

// Event kind → 徽章样式（CMessageBlock.cpp EventStyle 同源复制；灰度期双份）
struct SEventStyle {
    QString sBullet;
    QColor col;
    bool bBold;
};
SEventStyle EventStyle(const QString& a_sKind)
{
    if (a_sKind == QStringLiteral("stage_start"))   return {QStringLiteral("●"),  QColor(QStringLiteral("#2563eb")), false};
    if (a_sKind == QStringLiteral("stage_done"))    return {QStringLiteral("✔"),  QColor(QStringLiteral("#16a34a")), false};
    if (a_sKind == QStringLiteral("stage_failed"))  return {QStringLiteral("✗"),  QColor(QStringLiteral("#dc2626")), false};
    if (a_sKind == QStringLiteral("reject"))        return {QStringLiteral("↩"),  QColor(QStringLiteral("#dc2626")), false};
    if (a_sKind == QStringLiteral("escalation"))    return {QStringLiteral("⚠"),  QColor(QStringLiteral("#ea580c")), true};
    if (a_sKind == QStringLiteral("done"))          return {QStringLiteral("🎉"), QColor(QStringLiteral("#16a34a")), true};
    if (a_sKind == QStringLiteral("paused") ||
        a_sKind == QStringLiteral("pause_requested")) return {QStringLiteral("⏸"), QColor(QStringLiteral("#6b7280")), false};
    if (a_sKind == QStringLiteral("stopped"))       return {QStringLiteral("⏹"), QColor(QStringLiteral("#6b7280")), false};
    if (a_sKind == QStringLiteral("need_guide"))    return {QStringLiteral("⚠"),  QColor(QStringLiteral("#ea580c")), false};
    if (a_sKind == QStringLiteral("user"))          return {QStringLiteral("你"), QColor(QStringLiteral("#2563eb")), true};
    return                                           {QStringLiteral("•"),  QColor(QStringLiteral("#6b7280")), false};
}

// Edit/MultiEdit 特化红绿 diff（自 CMessageBlock::RebuildToolContent 序列抽出）
QList<SDiffLine> BuildDiffLines(const QString& a_sInput)
{
    QList<SDiffLine> aOut;
    const QJsonObject o = QJsonDocument::fromJson(a_sInput.toUtf8()).object();
    const QString oldS = o.value(QStringLiteral("old_string")).toString();
    const QString newS = o.value(QStringLiteral("new_string")).toString();
    aOut.append({QStringLiteral("--- 旧"), true});
    for (const QString& l : oldS.split(QLatin1Char('\n')))
        aOut.append({QStringLiteral("- ") + l, true});
    aOut.append({QString(), false});           // 旧段与「--- 新」间空行（insertText("\n--- 新\n") 口径）
    aOut.append({QStringLiteral("--- 新"), false});
    for (const QString& l : newS.split(QLatin1Char('\n')))
        aOut.append({QStringLiteral("+ ") + l, false});
    return aOut;
}

QList<SMonoLine> monoInputLines(const SToolEntry& e)
{
    QList<SMonoLine> aOut;
    const QString low = e.sTool.toLower();
    if (low == QLatin1String("edit") || low == QLatin1String("multiedit")) {
        for (const SDiffLine& d : BuildDiffLines(e.sInput))
            aOut.append({d.sText, d.bOld ? kColDiffOld : kColDiffNew});
    } else {
        // 其他工具：格式化 JSON 展示（口径同 CMessageBlock::RebuildToolContent）
        const QJsonDocument d = QJsonDocument::fromJson(e.sInput.toUtf8());
        const QString s = d.isObject()
            ? QString::fromUtf8(d.toJson(QJsonDocument::Indented)) : e.sInput;
        for (const QString& l : s.split(QLatin1Char('\n')))
            aOut.append({l, QColor()});
    }
    return aOut;
}

QList<SMonoLine> monoResultLines(const SToolEntry& e)
{
    // 结果无着色（错误态由卡头换肤表达，口径同 Legacy SetResult）
    QList<SMonoLine> aOut;
    for (const QString& l : e.sResult.split(QLatin1Char('\n')))
        aOut.append({l, QColor()});
    return aOut;
}

// System 组头摘要（口径同 CMessageBlock::RefreshSysHeader：首行 simplified 截 60）
QString sysBrief(const QStringList& aLines)
{
    if (aLines.isEmpty())
        return QString();
    return aLines.first().simplified().left(60);
}

QString sysHeaderText(const SDisplayItem& a_it)
{
    return QStringLiteral("%1 %2（共 %3 行，点击展开）")
        .arg(a_it.bSysCollapsed ? QStringLiteral("▸") : QStringLiteral("▾"),
             sysBrief(a_it.aSysLines)).arg(a_it.aSysLines.size());
}

// QTextLayout 换行布局（Event/System/Error 文本；返回精确高度——这三类无 doc 校正
// 链，估高必须与绘制同源，否则内容被裁）
int layoutWrapped(QTextLayout& a_lay, int a_iWidth)
{
    a_lay.beginLayout();
    int y = 0;
    for (;;) {
        QTextLine ln = a_lay.createLine();
        if (!ln.isValid())
            break;
        ln.setLineWidth(qMax(1, a_iWidth));
        ln.setPosition(QPointF(0, y));
        y += static_cast<int>(ln.height());
    }
    a_lay.endLayout();
    return y;
}

int wrappedHeight(const QString& a_sText, const QFont& a_oFont, int a_iWidth)
{
    if (a_sText.isEmpty())
        return 0;
    QTextLayout lay(a_sText, a_oFont);
    return layoutWrapped(lay, a_iWidth);
}

// 条目内容指纹：流式追加只增长（长度严格递增）、折叠/错误展开只翻布尔、高亮不改
// 布局——全走 O(1) 字段组合成 64 位 FNV，任一变化 → 高度/等宽行缓存失效重估；
// 不含高亮态（跳转高亮不得抖动已校正的 doc 实高）
quint64 rowFingerprint(const SDisplayItem& a_it)
{
    const SToolEntry* pE = a_it.aTools.isEmpty() ? nullptr : &a_it.aTools.last();
    quint64 u = 1469598103934665603ULL;       // FNV-1a 64
    auto mix = [&u](quint64 a_uV) { u ^= a_uV; u *= 1099511628211ULL; };
    mix(static_cast<quint64>(a_it.eKind));
    mix(a_it.bCollapsed ? 1 : 0);
    mix(a_it.bSysCollapsed ? 1 : 0);
    mix(static_cast<quint64>(a_it.sText.size()));
    mix(static_cast<quint64>(a_it.sEventKind.size())
        + (a_it.sEventKind.isEmpty() ? 0 : (a_it.sEventKind.at(0).unicode() << 16)));
    mix(static_cast<quint64>(a_it.aSysLines.size()));
    mix(static_cast<quint64>(a_it.aTools.size()));
    mix(pE ? static_cast<quint64>(pE->sInput.size()) : 0);
    mix(pE ? static_cast<quint64>(pE->sResult.size()) : 0);
    return u ? u : ~0ULL;                     // 0 保留为「未缓存」哨兵
}

// 等宽块绘制：底 #fafafa 边 #e5e7eb 圆角 3，内容行右截断，>12 行画「……（共 N 行）」
//（方案 §7.5 口径，替代旧固定高 160/120 内部滚动；高度形态差计入视觉差容差）。
// 返回块底 y。
int paintMonoBlock(QPainter& p, int x, int y, int w, const QList<SMonoLine>& aLines,
                   const QFont& a_oMono)
{
    const QFontMetrics fmM(a_oMono);
    const int iLS = fmM.lineSpacing();
    const int iVis = int(qMin<qsizetype>(12, aLines.size()));
    const int iBlockH = iVis * iLS + 10 + (aLines.size() > 12 ? iLS : 0);
    p.setPen(QPen(kColMonoBorder, 1));
    p.setBrush(kColMonoBg);
    p.drawRoundedRect(QRect(x, y, w, iBlockH).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.setFont(a_oMono);
    const int iTextW = qMax(1, w - 10);
    for (int i = 0; i < iVis; ++i) {
        p.setPen(aLines.at(i).col.isValid() ? aLines.at(i).col : kColMonoText);
        p.drawText(QPoint(x + 5, y + 5 + i * iLS + fmM.ascent()),
                   fmM.elidedText(aLines.at(i).sText, Qt::ElideRight, iTextW));
    }
    if (aLines.size() > 12) {
        p.setPen(kColMore);
        p.drawText(QPoint(x + 5, y + 5 + iVis * iLS + fmM.ascent()),
                   QStringLiteral("……（共 %1 行）").arg(aLines.size()));
    }
    return y + iBlockH;
}

} // namespace

CChatDelegate::CChatDelegate(CChatDocPool* a_pPool, const QFont& a_oFont, QObject* a_pParent)
    : QAbstractItemDelegate(a_pParent)
    , m_pPool(a_pPool)
    , m_oFont(a_oFont)
{
    m_oFontMono.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Courier New"),
                             QStringLiteral("monospace")});
    m_oFontMono.setPointSize(11);              // Legacy QSS 等宽块 font-size: 11pt 口径
}

void CChatDelegate::SetFont(const QFont& a_oFont)
{
    m_oFont = a_oFont;
    m_aHeights.clear();                        // 字体变 → 全部估高失效
    m_aMono.clear();
}

const SDisplayItem* CChatDelegate::itemOf(const QModelIndex& a_oIndex) const
{
    // 本 delegate 只装在持 CChatModel 的视图上（HarnessAgent 先例 static_cast；
    //   CChatModel 无 Q_OBJECT，qobject_cast 走基类元对象判型不精确）
    const CChatModel* pModel = static_cast<const CChatModel*>(a_oIndex.model());
    return pModel ? pModel->PeekItem(a_oIndex.row()) : nullptr;
}

// ---------------------------------------------------------------------------
// 估高（方案 §7.4）：Assistant 纯算术估（doc 材化后 OnDocReady 校正）；
// ToolUse 按绘制布局逐项精确累计；Event/System/Error 经 QTextLayout 与绘制同源
// 精确计（这三类无 doc 校正链兜底，估高必须精确）。
// ---------------------------------------------------------------------------
int CChatDelegate::EstimateHeight(const SDisplayItem& a_it, int a_iWidth) const
{
    const QFontMetrics fm(m_oFont);
    const int iFmH = fm.height();
    const int iAvail = qMax(16, a_iWidth);
    switch (a_it.eKind) {
    case CMessageBlock::Assistant: {
        // speaker(fmH) + 4 + 估行（自 CMessageBlock::EstimateAssistantHeight 的逐行
        // CJK×2 折算公式迁出：行数按「ASCII×1 + CJK×2 个平均字符宽」逐行折行，
        // 标题/代码栏/空行按经验补块间距）+ 2（doc 落地后由 OnDocReady 校正实高）
        const int iDocW = qMax(16, iAvail - 13);
        const int iLineH = qMax(15, fm.lineSpacing());
        const qreal dAvg = qMax(qreal(3.0), qreal(fm.averageCharWidth()));
        const qreal dUsable = qMax(qreal(80.0), qreal(iDocW) - 6.0);
        int iLines = 0;
        int iExtra = 0;
        const QStringList aLines = a_it.sText.split(QLatin1Char('\n'));
        for (const QString& sLine : aLines) {
            qreal dUnits = 0.0;
            for (const QChar& ch : sLine) {
                dUnits += (ch.unicode() > 0x2E7F) ? 2.0 : 1.0;   // CJK/全宽记 2 倍宽
            }
            iLines += qMax(1, int(dUnits * dAvg / dUsable + 0.5));
            if (sLine.isEmpty())                             iExtra += 6;    // 段间距
            else if (sLine.startsWith(QLatin1Char('#')))     iExtra += 10;   // 标题上下留白
            else if (sLine.startsWith(QStringLiteral("```"))) iExtra += 8;    // 代码块 padding
        }
        return iFmH + 4 + iLines * iLineH + iExtra + 8;
    }
    case CMessageBlock::ToolUse: {
        // 折叠头：fmH + 14（padding 5×2 + 边框 1×2，Legacy QToolButton 固定高口径）
        const int iHeaderH = iFmH + 14;
        if (a_it.bCollapsed)
            return iHeaderH;
        const QFontMetrics fmM(m_oFontMono);
        const int iLS = fmM.lineSpacing();
        int iH = iHeaderH + 4;
        for (const SToolEntry& e : a_it.aTools) {
            const int iIn = int(monoInputLines(e).size());
            const int iOut = int(monoResultLines(e).size());
            iH += iFmH + 8 + qMin(12, iIn) * iLS + 10 + (iIn > 12 ? iLS : 0);   // 入参
            iH += 8 + iFmH + 8;
            iH += qMin(12, iOut) * iLS + 10 + (iOut > 12 ? iLS : 0);            // 结果
            iH += 8;                                                                        // 条目间距
        }
        return iH;
    }
    case CMessageBlock::Event: {
        // 徽章 + 正文整体 QTextLayout（与 paintEventBadge 同源）；user 块顶部 3px
        // 分隔线 + 头像缩进（宽 -18），非 user 全宽（绘制同口径）
        const SEventStyle stEv = EventStyle(a_it.sEventKind);
        const bool bUser = (a_it.sEventKind == QStringLiteral("user"));
        const int iTop = bUser ? 3 : 0;
        QFont fLay = m_oFont;
        if (stEv.bBold || bUser) {
            fLay.setWeight(QFont::DemiBold);   // 粗体徽章字宽更宽，估高同字体
        }
        const int iTextW = qMax(16, iAvail - (bUser ? 18 : 0));
        const int iWrapped = wrappedHeight(stEv.sBullet + QLatin1Char(' ') + a_it.sText,
                                           fLay, iTextW);
        return iTop + (iWrapped > 0 ? qMax(iFmH, 16) + iWrapped - fm.lineSpacing()
                                    : qMax(iFmH, 16));
    }
    case CMessageBlock::System: {
        // 组头 ▸/▾ brief（共 N 行，点击展开）斜体 #9ca3af；展开=左缘 10 缩进逐行
        QFont fIt = m_oFont;
        fIt.setItalic(true);
        const QFontMetrics fmIt(fIt);
        const int iX = 10;                                     // 块左缘缩进
        const int iHeadH = qMax<int>(fmIt.height() + 2,
                                     wrappedHeight(sysHeaderText(a_it), fIt, qMax(16, iAvail - iX)));
        if (a_it.bSysCollapsed)
            return iHeadH;
        int iH = iHeadH + 2;
        for (const QString& s : a_it.aSysLines)
            iH += qMax(fmIt.height(), wrappedHeight(s, fIt, qMax(16, iAvail - iX - 8)));
        return iH;
    }
    case CMessageBlock::Error:
    default: {
        return qMax<int>(fm.height(), wrappedHeight(a_it.sText, m_oFont, iAvail));
    }
    }
}

int CChatDelegate::CachedHeight(int a_iRow) const
{
    return m_aHeights.value(a_iRow, SHeightCache()).iH;
}

QSize CChatDelegate::sizeHint(const QStyleOptionViewItem& a_oOption,
                              const QModelIndex& a_oIndex) const
{
    const SDisplayItem* pIt = itemOf(a_oIndex);
    if (pIt == nullptr)
        return QSize(0, 0);
    const int iW = m_iWidth > 0 ? m_iWidth
        : (a_oOption.rect.width() > 0 ? a_oOption.rect.width() : 400);
    // 高度缓存按内容指纹自校验：折叠翻转/流式增长/错误自动展开都改变指纹 → 此处
    // 重估，不再命中翻转前旧值；指纹一致（含校正后 TouchSizeHint 触发的重查）直接
    // 命中——doc 实高不被打回算术估高
    const SHeightCache ent = m_aHeights.value(a_oIndex.row(), SHeightCache());
    const quint64 uFp = rowFingerprint(*pIt);
    if (ent.iH > 0 && ent.uFp == uFp)
        return QSize(iW, ent.iH);
    const int iH = EstimateHeight(*pIt, iW);
    m_aHeights[a_oIndex.row()] = SHeightCache{iH, uFp};   // 载档一次成型滚动条
    return QSize(iW, iH);
}

void CChatDelegate::OnDocReady(int a_iItem, QTextDocument* a_pDoc, const SDisplayItem* a_pIt)
{
    if (a_pDoc == nullptr)
        return;
    QFont fSp = m_oFont;                       // speaker 行与 paintAssistant 同款粗体度量
    fSp.setWeight(QFont::DemiBold);
    const int iSpeakerH = QFontMetrics(fSp).height();
    const int iH = qRound(a_pDoc->size().height()) + iSpeakerH + 4 + 2;
    // 指纹取条目权威值（视图经 PeekItem 注入）：实高与该内容版本绑定，后续
    // sizeHint 指纹一致即命中实高；条目指针取不到（Reset 后行号错位）沿用缓存指纹
    const quint64 uFp = a_pIt != nullptr ? rowFingerprint(*a_pIt)
                                         : m_aHeights.value(a_iItem, SHeightCache()).uFp;
    const SHeightCache ent = m_aHeights.value(a_iItem, SHeightCache());
    if (ent.iH == iH) {
        if (ent.uFp != uFp)
            m_aHeights[a_iItem] = SHeightCache{iH, uFp};
        return;
    }
    m_aHeights[a_iItem] = SHeightCache{iH, uFp};
    if (fnHeightCorrected)
        fnHeightCorrected(a_iItem, iH - ent.iH);
}

void CChatDelegate::InvalidateWidth(int a_iNewWidth)
{
    m_iWidth = a_iNewWidth;
    m_aHeights.clear();                        // 高度缓存全失效（惰性重估）
    m_aMono.clear();                           // 等宽块行缓存同失效（宽度变则截断变）
    if (m_pPool)
        m_pPool->OnWidthChanged(a_iNewWidth);  // 池内 doc 重排 → fnDocReady 逐条回调
}

// 等宽块行缓存（paint 热路径免逐帧 fromJson+toJson(Indented)）：
// 键=行号+内容指纹，流式追加/折叠翻转自动失效重解析
CChatDelegate::SMonoCache CChatDelegate::monoCacheFor(const SDisplayItem& a_it, int a_iRow) const
{
    const quint64 uFp = rowFingerprint(a_it);
    const SMonoCache ent = m_aMono.value(a_iRow);
    if (ent.uFp == uFp && ent.iCount == a_it.aTools.size() && !ent.aIn.isEmpty())
        return ent;
    SMonoCache neu;
    neu.uFp = uFp;
    neu.iCount = a_it.aTools.size();
    for (const SToolEntry& e : a_it.aTools) {
        neu.aIn.append(monoInputLines(e));
        neu.aOut.append(monoResultLines(e));
    }
    m_aMono.insert(a_iRow, neu);
    return neu;
}

// ---------------------------------------------------------------------------
// paint：按 Kind 分派（视觉规格见文件头注释）
// ---------------------------------------------------------------------------
void CChatDelegate::paint(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                          const QModelIndex& a_oIndex) const
{
    const SDisplayItem* pIt = itemOf(a_oIndex);
    if (pIt == nullptr)
        return;
    a_pPainter->save();
    if (pIt->bHighlight)                       // 跳转高亮 #fffbeb
        a_pPainter->fillRect(a_oOption.rect, kColHighlight);
    switch (pIt->eKind) {
    case CMessageBlock::Assistant: paintAssistant(a_pPainter, a_oOption, *pIt, a_oIndex.row()); break;
    case CMessageBlock::ToolUse:   paintToolUse(a_pPainter, a_oOption, *pIt, a_oIndex.row());   break;
    case CMessageBlock::Event:     paintEventBadge(a_pPainter, a_oOption, *pIt);                  break;
    case CMessageBlock::System:    paintSystem(a_pPainter, a_oOption, *pIt, a_oIndex.row());    break;
    case CMessageBlock::Error:
    default:                       paintError(a_pPainter, a_oOption, *pIt);                     break;
    }
    a_pPainter->restore();
}

void CChatDelegate::paintAssistant(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                                   const SDisplayItem& a_it, int a_iRow) const
{
    // 左 3px #d97757 竖条 + 左内边距 10（Legacy border-left+padding-left 即视觉）；
    // speaker 行 ⏺ 产出后端（空→Agent，绝不用当前后端冒充——v1.4.98 口径）
    const QRect rc = a_oOption.rect;
    QPainter& p = *a_pPainter;
    p.fillRect(rc.left(), rc.top(), 3, rc.height(), kColAssistantBar);
    QFont fSp = m_oFont;
    fSp.setWeight(QFont::DemiBold);
    const int iSpeakerH = QFontMetrics(fSp).height();
    p.setFont(fSp);
    p.setPen(kColAssistantBar);
    p.drawText(QRect(rc.left() + 13, rc.top(), qMax(1, rc.width() - 13), iSpeakerH),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("⏺ %1").arg(a_it.sBackend.isEmpty()
                                            ? QStringLiteral("Agent") : a_it.sBackend));
    // 正文：池化 QTextDocument 同源 Markdown 直绘（透明底、无边框）
    const int iDocW = qMax(16, rc.width() - 13);
    if (m_pPool == nullptr)
        return;
    QTextDocument* pDoc = m_pPool->Acquire(a_iRow, a_it.sText, iDocW);
    if (pDoc == nullptr)
        return;
    p.setPen(a_oOption.palette.text().color());
    p.translate(rc.left() + 13, rc.top() + iSpeakerH + 4);
    pDoc->drawContents(&p);
}

void CChatDelegate::paintToolUse(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                                 const SDisplayItem& a_it, int a_iRow) const
{
    const QRect rc = a_oOption.rect;
    QPainter& p = *a_pPainter;
    const QFontMetrics fm(m_oFont);
    const int iFmH = fm.height();
    const int iHeaderH = iFmH + 14;
    const bool bErr = a_it.bHadError;
    // 折叠头：圆角 4 底/边/字（错误态换肤，Legacy QSS 口径）
    QRect rcHead(rc.left(), rc.top(), rc.width(), iHeaderH);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(bErr ? kColToolErrBorder : kColToolBorder, 1));
    p.setBrush(bErr ? kColToolErrBg : kColToolBg);
    p.drawRoundedRect(rcHead.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    // 头文本：▶/▼ 图标 摘要 … ×N——整体 ElideMiddle（Legacy UpdateToolHeaderText
    // 口径：中截同时保住前缀图标与尾部 ×N 计数），全文与逐条摘要在 tooltip
    SToolEntry stEmpty;
    const SToolEntry& e0 = a_it.aTools.isEmpty() ? stEmpty : a_it.aTools.first();
    QString sBody = QStringLiteral("%1 %2").arg(ToolIcon(e0.sTool),
                                                CMessageBlock::ToolSummary(e0.sTool, e0.sInput));
    if (a_it.aTools.size() > 1) {
        sBody += QStringLiteral("  … ×%1").arg(a_it.aTools.size());
    }
    const QString sFull = QStringLiteral("%1 %2").arg(
        a_it.bCollapsed ? QStringLiteral("▶") : QStringLiteral("▼"), sBody);
    const int iAvail = rc.width() - 22;        // 左右 padding 8×2 + 边框 1×2 + 余量
    QString sShow = sFull;
    if (iAvail > 80)
        sShow = fm.elidedText(sFull, Qt::ElideMiddle, iAvail);
    p.setFont(m_oFont);
    p.setPen(bErr ? kColToolErrText : kColToolText);
    p.drawText(QRect(rcHead.left() + 8, rcHead.top(), qMax(1, rcHead.width() - 16), iHeaderH),
               Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, sShow);
    m_aHeaderRect[a_iRow] = rcHead;            // editorEvent 折叠命中区
    // 展开区：逐条「入参 #i / 结果 #i」小标 + 等宽块（Legacy RebuildToolContent 布局口径）
    if (a_it.bCollapsed || a_it.aTools.isEmpty())
        return;
    const SMonoCache mono = monoCacheFor(a_it, a_iRow);   // 指纹缓存：paint 免逐帧重解析 JSON
    int y = rc.top() + iHeaderH + 4;
    const bool bMulti = a_it.aTools.size() > 1;
    for (int iE = 0; iE < a_it.aTools.size(); ++iE) {
        p.setPen(kColLabel);
        p.setFont(m_oFont);
        p.drawText(QRect(rc.left(), y, rc.width(), iFmH), Qt::AlignLeft | Qt::AlignVCenter,
                   bMulti ? QStringLiteral("入参 #%1").arg(iE + 1)
                          : QStringLiteral("入参"));
        y += iFmH + 8;
        y = paintMonoBlock(p, rc.left(), y, rc.width(), mono.aIn.at(iE), m_oFontMono);
        y += 8;
        p.setPen(kColLabel);
        p.drawText(QRect(rc.left(), y, rc.width(), iFmH), Qt::AlignLeft | Qt::AlignVCenter,
                   bMulti ? QStringLiteral("结果 #%1").arg(iE + 1)
                          : QStringLiteral("结果"));
        y += iFmH + 8;
        y = paintMonoBlock(p, rc.left(), y, rc.width(), mono.aOut.at(iE), m_oFontMono);
        y += 8;
    }
}

void CChatDelegate::paintEventBadge(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                                    const SDisplayItem& a_it) const
{
    // 彩色徽章 + 正文（#374151）；user 块：顶部 3px #9ca3af 分隔线 + 16px 头像 +
    // 蓝「你」粗体徽章（CMessageBlock::SetEvent 口径）
    const QRect rc = a_oOption.rect;
    QPainter& p = *a_pPainter;
    const SEventStyle st = EventStyle(a_it.sEventKind);
    const bool bUser = (a_it.sEventKind == QStringLiteral("user"));
    int iTop = 0;
    if (bUser) {
        p.fillRect(rc.left(), rc.top(), rc.width(), 3, kColUserSep);
        iTop = 3;
    }
    const QFontMetrics fm(m_oFont);
    int iTextX = rc.left();
    if (bUser) {
        static const QPixmap pmUser = [] {
            QPixmap pm(QStringLiteral(":/icons/user.png"));
            return pm.isNull() ? QPixmap()   // qrc 缺失（测试目标）安全跳过
                               : pm.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }();
        if (!pmUser.isNull()) {
            p.drawPixmap(rc.left(), rc.top() + iTop + qMax(0, (fm.height() - 16) / 2),
                         16, 16, pmUser);
        }
        iTextX = rc.left() + 18;
    }
    QFont fLay = m_oFont;
    if (st.bBold || bUser) {
        fLay.setWeight(QFont::DemiBold);
    }
    const QString sAll = st.sBullet + QLatin1Char(' ') + a_it.sText;
    QTextLayout lay(sAll, fLay);
    QTextLayout::FormatRange rgBadge, rgText;   // 徽章彩色、空格中性、正文深灰
    rgBadge.start = 0;
    rgBadge.length = st.sBullet.size();
    QFont fBadge = fLay;
    rgBadge.format.setFont(fBadge);
    rgBadge.format.setForeground(st.col);
    rgText.start = rgBadge.length + 1;
    rgText.length = a_it.sText.size();
    rgText.format.setForeground(kColBody);
    lay.setFormats({rgBadge, rgText});
    layoutWrapped(lay, qMax(16, rc.width() - (iTextX - rc.left())));
    lay.draw(&p, QPoint(iTextX, rc.top() + iTop));
}

void CChatDelegate::paintSystem(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                                const SDisplayItem& a_it, int a_iRow) const
{
    // 头行 ▸/▾ brief（共 N 行，点击展开）斜体 #9ca3af；展开=左缘 10 缩进逐行灰斜体
    const QRect rc = a_oOption.rect;
    QPainter& p = *a_pPainter;
    const int iX = rc.left() + 10;             // 块左缘 10（Legacy margins(10,0,0,0) 口径）
    QFont fIt = m_oFont;
    fIt.setItalic(true);
    const int iW = qMax(16, rc.width() - 10);
    const int iHeadH = qMax<int>(QFontMetrics(fIt).height() + 2,
                                 wrappedHeight(sysHeaderText(a_it), fIt, iW));
    p.setFont(fIt);
    p.setPen(kColSys);
    p.drawText(QRect(iX, rc.top(), iW, iHeadH), Qt::AlignLeft | Qt::AlignVCenter,
               sysHeaderText(a_it));
    m_aHeaderRect[a_iRow] = QRect(iX, rc.top(), iW, iHeadH);   // editorEvent 折叠命中区
    if (a_it.bSysCollapsed)
        return;
    int y = rc.top() + iHeadH + 2;
    for (const QString& s : a_it.aSysLines) {
        QTextLayout lay(s, fIt);
        const int iH = qMax<int>(QFontMetrics(fIt).height(), layoutWrapped(lay, iW - 8));
        lay.draw(&p, QPoint(iX + 8, y));
        y += iH;
    }
}

void CChatDelegate::paintError(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                               const SDisplayItem& a_it) const
{
    // 错误单条红字换行（不折叠，Legacy BuildSystem(true) 口径）
    const QRect rc = a_oOption.rect;
    QPainter& p = *a_pPainter;
    p.setFont(m_oFont);
    p.setPen(kColErr);
    QTextLayout lay(a_it.sText, m_oFont);
    layoutWrapped(lay, qMax(16, rc.width()));
    lay.draw(&p, QPoint(rc.left(), rc.top()));
}

// ---------------------------------------------------------------------------
// editorEvent：命中=纯几何。折叠头点击 → setData(CollapsedRole) 翻转；
// Assistant 文档区单击 → Peek doc anchorAt 命中链接外部打开（QTextBrowser 同款）
// ---------------------------------------------------------------------------
bool CChatDelegate::editorEvent(QEvent* a_pEvent, QAbstractItemModel* a_pModel,
                                const QStyleOptionViewItem& a_oOption,
                                const QModelIndex& a_oIndex)
{
    if (a_pEvent->type() != QEvent::MouseButtonRelease)
        return false;
    auto* pM = static_cast<QMouseEvent*>(a_pEvent);
    if (pM->button() != Qt::LeftButton)        // 仅左键响应（右/中键不翻转折叠不开链接）
        return false;
    const QPoint pos = pM->position().toPoint();
    const SDisplayItem* pIt = itemOf(a_oIndex);
    if (pIt == nullptr)
        return false;
    if (pIt->eKind == CMessageBlock::ToolUse || pIt->eKind == CMessageBlock::System) {
        const QRect rcHead = m_aHeaderRect.value(a_oIndex.row());
        if (rcHead.contains(pos)) {
            const bool bCur = a_oIndex.data(CChatModel::CollapsedRole).toBool();
            a_pModel->setData(a_oIndex, !bCur, CChatModel::CollapsedRole);
            return true;
        }
        return false;
    }
    if (pIt->eKind == CMessageBlock::Assistant && m_pPool) {
        if (QTextDocument* pDoc = m_pPool->Peek(a_oIndex.row())) {
            QFont fSp = m_oFont;               // doc 绘制原点 = 条目左上 + (13, speaker+4)
            fSp.setWeight(QFont::DemiBold);
            const QPoint ptDoc = pos - a_oOption.rect.topLeft()
                - QPoint(13, QFontMetrics(fSp).height() + 4);
            const QString sUrl = pDoc->documentLayout()->anchorAt(ptDoc);
            if (!sUrl.isEmpty()) {
                QDesktopServices::openUrl(QUrl(sUrl));
                return true;
            }
        }
    }
    return false;
}

// tooltip：工具卡折叠头区 = 逐条工具摘要（宽松截断 200/120，Legacy tooltip 口径）
QString CChatDelegate::ToolTipAt(const QModelIndex& a_oIndex, const QPoint& a_oPos) const
{
    const SDisplayItem* pIt = itemOf(a_oIndex);
    if (pIt == nullptr || pIt->eKind != CMessageBlock::ToolUse)
        return QString();
    const QRect rcHead = m_aHeaderRect.value(a_oIndex.row());
    if (!rcHead.contains(a_oPos))
        return QString();
    QStringList aTips;
    for (const SToolEntry& e : pIt->aTools)
        aTips << ToolIcon(e.sTool) + QStringLiteral(" ")
              + CMessageBlock::ToolSummary(e.sTool, e.sInput, 200, 120);
    return aTips.join(QLatin1Char('\n'));
}
