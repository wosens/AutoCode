#pragma once

#include <QDialog>

// 「关于」对话框（2026.10.16 v4.4.386 整版重设计——饱满/高级审美）：
//   顶部深蓝渐变品牌横幅（图标/名称/版本徽章/slogan），中部 Agent 后端五品牌胶囊行
//   + 核心能力 2×3 特性卡片，底部作者/时间/版权 + 确定按钮。
// 无自定义信号槽（accept 直接连基类），不需要 Q_OBJECT/moc。
// 沿用标准对话框框体（可拖动、系统阴影），不做无边框自绘。
class CAboutDialog : public QDialog {
public:
    explicit CAboutDialog(QWidget* a_pParent = nullptr);
};
