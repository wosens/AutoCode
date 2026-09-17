#pragma once

#include <QString>

// CTipStyle：全局提示（tooltip）样式统一（2026.09.10 v2.2.156 用户需求：设置页 tooltip
// 字体太小，统一 11 号；文本太长自动换行）。
//   - 字体：QToolTip::setFont（QTipLabel 类字体，提示窗为 Qt 自绘 QLabel 非系统原生，
//     应用级全局生效；11pt 按点数随 DPI 缩放，与全界面 11 号字口径一致）；
//   - 换行：应用级 ToolTipChange 过滤器把【纯文本】tooltip 就地转富文本——富文本提示
//     自动按宽度换行，手工 \n 转 <br> 保留；已富文本的（调用方自带 HTML）不动。
//     覆盖设置页/任务列表/资源管理器/执行面板全部提示，无需逐调用点改造。
// 调用时机：CMainWindow 构造早期（CreateUi 之前）ApplyGlobal() 一次——先装过滤器，
//   随后各控件 setToolTip 触发的 ToolTipChange 才会被就地转换。
class CTipStyle {
public:
    // 全局生效入口：装 11 号 tooltip 字体 + ToolTipChange 富文本转换过滤器（进程一次）。
    static void ApplyGlobal();
    // 纯文本 → 自动换行富文本（HTML 转义；\n → <br>；段落边距归零与纯文本视觉贴齐）。
    static QString WrapText(const QString& a_sText);
};
