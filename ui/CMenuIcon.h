#pragma once

// CMenuIcon —— 右键菜单项共享图标装配（2026.09.17 v2.3.170 全应用右键菜单图标化）。
// 源稿 resources/svg/ow-*.svg（icon_claude 技能绘制，32 画布线稿主色 #2563eb 同
// user.svg，删除红/差异绿等语义点缀），resvg 渲染 16/32 双尺寸 png 内嵌 qrc：
//   16 = 菜单主尺寸；32 = 高 DPI 档（QIcon 按设备 DPI 自动选最佳档）。
// 注意：图标资源编译进 AutoCode 主目标；测试目标未挂 qrc，取到空 QIcon 无害。

#include <QIcon>
#include <QString>

inline QIcon MenuIcon(const QString& a_sName) {
    QIcon oIcon(QStringLiteral(":/icons/menu/16/%1.png").arg(a_sName));
    oIcon.addFile(QStringLiteral(":/icons/menu/32/%1.png").arg(a_sName));
    return oIcon;
}
