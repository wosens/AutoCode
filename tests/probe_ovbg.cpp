// probe_ovbg —— 双击日志条目进入「选择状态」（复制浮层）后，浮层内被选择文字的
// 背景必须不透明（2026-11-03 用户要求：背景一旦透明，delegate 已绘制的原条目文字
// 从浮层底下透出，与浮层文字同位叠加成重影，看不清当前要选的文本）。
// 判定不走 QSS 字符串（test_chatmodel::overlayOpenAndHeaderGuard 已锁字符串），
// 走像素：viewport()->grab() 离屏合成渲染（delegate 底稿先画、浮层子控件后画，
// 与真实屏幕 z 序一致），对浮层覆盖区取色统计众数——
//   浮层不透明 → 众数 = #fffbeb（浮层米黄底）；
//   浮层透明   → 众数 = #ffffff（视口白底透出，浮层开着焦点即走、行选中已清）。
// 产出 ovbg_event.png / ovbg_assistant.png 供人眼复核。退出码：0=通过，3=失败。
#include <QApplication>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QRect>
#include <QtTest>

#include <cstdio>

#include "ui/CChatListView.h"
#include "ui/CChatModel.h"
#include "ui/SBlockData.h"

static SBlockData mkAssistant(const QString& a_sText)
{
    SBlockData b;
    b.eKind = CMessageBlock::Assistant;
    b.sText = a_sText;
    b.sBackend = QStringLiteral("DSH");
    return b;
}

static SBlockData mkEvent(const QString& a_sText)
{
    SBlockData b;
    b.eKind = CMessageBlock::Event;
    b.sEventKind = QStringLiteral("done");
    b.sText = a_sText;
    return b;
}

namespace {

// 区间众数色（2px 步进网格）：背景像素必远多于字形像素，众数即底色
QRgb ModeColor(const QImage& a_img, const QRect& a_rc)
{
    QHash<QRgb, int> hist;
    int iTotal = 0;
    for (int y = a_rc.top(); y <= a_rc.bottom(); y += 2) {
        for (int x = a_rc.left(); x <= a_rc.right(); x += 2) {
            ++hist[a_img.pixel(x, y)];
            ++iTotal;
        }
    }
    QRgb uBest = 0;
    int iBest = -1;
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it) {
        if (it.value() > iBest) {
            iBest = it.value();
            uBest = it.key();
        }
    }
    std::printf("  mode color #%06X x%d (of %d samples)\n", uBest & 0xFFFFFF,
                iBest, iTotal);
    return uBest;
}

int Check(const QImage& a_img, const QRect& a_rc, const char* a_szTag)
{
    const QRgb uMode = ModeColor(a_img, a_rc);
    const QColor c(uMode);
    const QColor cWant(0xff, 0xfb, 0xeb);          // #fffbeb 浮层米黄底（不透明）
    const bool bPass = qAbs(c.red() - cWant.red()) <= 2
        && qAbs(c.green() - cWant.green()) <= 2
        && qAbs(c.blue() - cWant.blue()) <= 2;
    std::printf("  %s: %s (mode #%02X%02X%02X, want #FFFBEB)\n", a_szTag,
                bPass ? "PASS 不透明" : "FAIL 透明/异色",
                c.red(), c.green(), c.blue());
    return bPass ? 0 : 3;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication a(argc, argv);
    const QString sDir = QApplication::applicationDirPath();
    int iRc = 0;

    CChatModel m;
    m.AppendBlock(mkAssistant(QStringLiteral(
        "回复正文：垫在浮层底下的原条目文字 ABCDEFG 0123456789，若浮层透明这行字会透出成重影。\n\n第二段正文。\n- 列表项一\n- 列表项二\n")));
    m.AppendBlock(mkEvent(QStringLiteral("已完成（16:47:43，耗时 1分23秒）")));

    CChatListView v;
    v.resize(600, 500);
    v.SetModel(&m);
    v.show();
    for (int i = 0; i < 6; ++i)
        a.processEvents();

    // ① 事件行双击 → 复制浮层（纯文本 doc）；行矩形内众数色须为不透明米黄
    v.scrollTo(m.index(1, 0), QAbstractItemView::PositionAtTop);
    for (int i = 0; i < 6; ++i)
        a.processEvents();
    const QRect rcEv = v.visualRect(m.index(1, 0));
    if (!rcEv.isValid()) {
        std::printf("probe_ovbg: event row rect invalid\n");
        return 3;
    }
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier, rcEv.center());
    for (int i = 0; i < 8; ++i)
        a.processEvents();
    if (!v.OverlayVisible()) {
        std::printf("probe_ovbg: overlay not visible (event)\n");
        return 3;
    }
    const QImage imgEv = v.viewport()->grab().toImage();
    imgEv.save(sDir + QStringLiteral("/ovbg_event.png"));
    std::printf("[event row] overlay visible, row=%d,%d %dx%d\n",
                rcEv.left(), rcEv.top(), rcEv.width(), rcEv.height());
    iRc |= Check(imgEv, rcEv.adjusted(3, 3, -10, -3), "event ");

    // 收层再测助手行（池 Markdown doc 路径同浮层）
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier, rcEv.center());
    for (int i = 0; i < 6; ++i)
        a.processEvents();

    v.scrollTo(m.index(0, 0), QAbstractItemView::PositionAtTop);
    for (int i = 0; i < 6; ++i)
        a.processEvents();
    const QRect rcAs = v.visualRect(m.index(0, 0));
    if (!rcAs.isValid()) {
        std::printf("probe_ovbg: assistant row rect invalid\n");
        return 3;
    }
    QFont fSp = v.font();
    fSp.setWeight(QFont::DemiBold);
    const int iSpeakerH = QFontMetrics(fSp).height();
    QTest::mouseDClick(v.viewport(), Qt::LeftButton, Qt::NoModifier,
                       rcAs.topLeft() + QPoint(rcAs.width() / 2, iSpeakerH + 30));
    for (int i = 0; i < 8; ++i)
        a.processEvents();
    if (!v.OverlayVisible()) {
        std::printf("probe_ovbg: overlay not visible (assistant)\n");
        return 3;
    }
    const QImage imgAs = v.viewport()->grab().toImage();
    imgAs.save(sDir + QStringLiteral("/ovbg_assistant.png"));
    // 浮层只盖 doc 区（speaker 行之下），取样带压在浮层内、避开行头行尾
    const QRect rcBand(rcAs.left() + 20, rcAs.top() + iSpeakerH + 8,
                       qMax(40, rcAs.width() - 40),
                       qMax(20, qMin(60, rcAs.height() - iSpeakerH - 14)));
    std::printf("[assistant row] overlay visible, band=%d,%d %dx%d\n",
                rcBand.left(), rcBand.top(), rcBand.width(), rcBand.height());
    iRc |= Check(imgAs, rcBand, "assist");

    std::printf("probe_ovbg done rc=%d\n", iRc);
    return iRc;
}
