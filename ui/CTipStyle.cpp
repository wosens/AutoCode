#include "CTipStyle.h"

#include <QApplication>
#include <QEvent>
#include <QObject>
#include <QTextDocument>
#include <QToolTip>
#include <QWidget>

// 应用级 ToolTipChange 过滤器：任意控件 setToolTip 纯文本时就地转富文本（自动换行）。
//   事件同步直发（QWidget::setToolTip 内 sendEvent），转完富文本会再次触发本过滤器，
//   mightBeRichText 命中即早退——天然再入安全，不递归。
class CTipFilter : public QObject {
public:
    explicit CTipFilter(QObject* a_pParent) : QObject(a_pParent) {}

    bool eventFilter(QObject* a_pObj, QEvent* a_pEvent) override {
        if (a_pEvent->type() == QEvent::ToolTipChange) {
            if (QWidget* pW = qobject_cast<QWidget*>(a_pObj)) {
                const QString s = pW->toolTip();
                if (!s.isEmpty() && !Qt::mightBeRichText(s)) {
                    pW->setToolTip(CTipStyle::WrapText(s));
                }
            }
        }
        return QObject::eventFilter(a_pObj, a_pEvent);
    }
};

QString CTipStyle::WrapText(const QString& a_sText) {
    // 富文本提示（QTipLabel 为 QLabel）自动按宽度换行；<p> 富文本段落默认上下各留
    //   一段边距（视觉比纯文本高），margin 四边归零与旧观感贴齐；\n 转 <br> 保留手工
    //   换行（富文本默认折叠空白/换行）；toHtmlEscaped 防文本内 <>& 被当标签吃掉。
    //   margin-* 逐条写（Qt 富文本 CSS 不认 margin 简写）。
    QString s = a_sText.toHtmlEscaped();
    s.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral(
        "<p style='margin-top:0;margin-bottom:0;margin-left:0;margin-right:0'>%1</p>").arg(s);
}

void CTipStyle::ApplyGlobal() {
    // ① 11 号字体：QToolTip::font() 取当前（Windows 缺省 9pt Segoe UI），改点数回设。
    //   QToolTip::setFont 即 QApplication::setFont(font, "QTipLabel") 类字体——提示标签
    //   首次显示时创建并按类字体 polish，本函数在构造早期调用（早于任何提示显示）。
    QFont fTip = QToolTip::font();
    fTip.setPointSize(11);
    QToolTip::setFont(fTip);
    // ② 换行过滤器：进程一次（静态局部，qApp 父对象托管生命周期）。
    static CTipFilter* s_pFilter = nullptr;
    if (s_pFilter == nullptr) {
        s_pFilter = new CTipFilter(qApp);
        qApp->installEventFilter(s_pFilter);
    }
}
