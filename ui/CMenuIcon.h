#pragma once

// CMenuIcon —— 右键菜单项/小按钮共享图标装配（2026.09.17 v2.3.170 全应用右键菜单图标化；
//   2026.10.16 v4.4.386 图标重设计 + 尺寸体系升级）。
//
// 源稿 resources/svg/ow-*.svg（icon_claude 技能绘制）：24 画布 / 线宽 2 / 圆角端点，
//   主色品牌蓝 #2563EB 统一，语义点缀（删除红 #E5484D / 确认绿 #30A46C / 提示橙
//   #F5A623 / 命名紫 #7C6CF0）+ 淡蓝块面（fill-opacity 0.16）双层构图。resvg 渲染
//   五档 PNG 内嵌 qrc：
//     16 = 兜底档（极小界面/低 DPI 遗留）；
//     20 = 菜单主尺寸（CAppStyle 把 QMenu 图标列设为 20px，与 11pt 菜单字体行高匹配，
//           2026.10.16 前旧档 16px 相对菜单项偏小——用户反馈"不能太小"）；
//     24 / 32 / 40 = 高 DPI 与工具按钮档（QIcon 按设备像素比自动取最佳档）。
// 注意：图标资源编译进 AutoCode 主目标；测试目标未挂 qrc，取到空 QIcon 无害。

#include <QIcon>
#include <QString>

inline QIcon MenuIcon(const QString& a_sName) {
    QIcon oIcon(QStringLiteral(":/icons/menu/16/%1.png").arg(a_sName));
    oIcon.addFile(QStringLiteral(":/icons/menu/20/%1.png").arg(a_sName));
    oIcon.addFile(QStringLiteral(":/icons/menu/24/%1.png").arg(a_sName));
    oIcon.addFile(QStringLiteral(":/icons/menu/32/%1.png").arg(a_sName));
    oIcon.addFile(QStringLiteral(":/icons/menu/40/%1.png").arg(a_sName));
    return oIcon;
}
