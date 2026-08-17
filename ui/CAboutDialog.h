#pragma once

#include <QDialog>

// 「关于」对话框：正式版式——上部左侧应用大图标、右侧应用名/版本号/一句简介，
// 中部分隔线，下部作者/开发时间/版权信息行，右下确定按钮。
// 无自定义信号槽（accept 直接连基类），不需要 Q_OBJECT/moc。
// 沿用标准对话框框体（可拖动、系统阴影），不做无边框自绘。
class CAboutDialog : public QDialog {
public:
    explicit CAboutDialog(QWidget* a_pParent = nullptr);
};
