#pragma once

// CAppStyle —— 应用级代理样式（2026.10.16 菜单图标尺寸升级）。
//
// 背景：QMenu 图标列宽度 = style 的 PM_SmallIconSize（Windows 缺省 16px）。AutoCode
//   菜单字体为 11pt（行高约 20px、菜单项高约 28px），16px 图标明显偏小（用户反馈
//   "图标要匹配菜单项的高度，不能太小"）。QMenu::setIconSize 无法全局生效（菜单在
//   十余处局部构造），故用代理样式把「菜单上下文」的 SmallIconSize 提到 20px。
//
// 影响面控制（只动菜单，不碰其他小图标消费者）：
//   - opt 为 QStyleOptionMenuItem（QMenu 布局菜单项时传入）→ 20；
//   - widget 本身是 QMenu（QMenu::iconSize() 以 nullptr option 查询）→ 20；
//   - 其余（QComboBox/QLineEdit 清除钮/滚动条箭头等）原样透传基础样式。
// 其余所有 metric/绘制全部透传，不改任何别的观感。

#include <QProxyStyle>
#include <QStyleOption>
#include <QMenu>

class CAppStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric a_eMetric, const QStyleOption* a_pOption,
                    const QWidget* a_pWidget) const override {
        if (a_eMetric == PM_SmallIconSize
            && ((a_pOption != nullptr && a_pOption->type == QStyleOption::SO_MenuItem)
                || qobject_cast<const QMenu*>(a_pWidget) != nullptr)) {
            return 20;   // 菜单图标主档（resources/menu/20/ 专属渲染）
        }
        return QProxyStyle::pixelMetric(a_eMetric, a_pOption, a_pWidget);
    }
};
