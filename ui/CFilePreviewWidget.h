#pragma once

#include <QWidget>

class QStackedWidget;
class QTextBrowser;
class QLabel;
class QPlainTextEdit;

// 2026.08.25 文件预览控件（替代原 QPlainTextEdit 单视图）：
//   按文件类型三选一切换（QStackedWidget）：
//   [0] 纯文本（QTextBrowser 富文本关闭，等宽字体长行横滚）
//   [1] Markdown（setMarkdown 带格式渲染，本地图片 file:/// 重写）
//   [2] HTML（setHtml 富文本渲染，基本格式保留）
//   [3] 十六进制（CHexView：左 hex 每行 16 字节 + 右 ASCII，双区滚动同步）
// 编码探测：BOM（UTF-16LE/BE、UTF-8）→ UTF-8 严格 → GB18030（Windows API 校验）
//   → 空字节比例判二进制 → Latin1 兜底；状态栏显示「编码 · 大小 · 截断」。
class CFilePreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit CFilePreviewWidget(QWidget* a_pParent = nullptr);

    // 加载并按类型分发显示（路径空/读失败显示错误信息）
    void ShowFile(const QString& a_sAbsPath);

    // 编码探测：成功填充 a_sText 并返回编码名；字节流判为二进制返回空串。
    // 2026.08.26 public（单元测试直调；纯函数无副作用）：BOM→UTF-8严格→GB18030→二进制→Latin1。
    QString DetectAndDecode(const QByteArray& a_aBytes, QString& a_sText, bool& a_bBinary) const;

private:
    void BuildUi();
    void ShowHex(const QByteArray& a_aBytes);

    // md/html 内本地相对链接重写为 file:/// 绝对路径（图片可见）
    static QString RewriteLocalUrls(const QString& a_sText, const QString& a_sBaseDir);

private:
    QStackedWidget* m_pStack    = nullptr;
    QTextBrowser*   m_pText     = nullptr;   // 纯文本
    QTextBrowser*   m_pMd       = nullptr;   // Markdown
    QTextBrowser*   m_pHtml     = nullptr;   // HTML
    QWidget*        m_pHexWrap  = nullptr;   // 十六进制视图（CFileHexView，cpp 内定义）
    QLabel*         m_pStat     = nullptr;   // 底部状态栏：编码 · 大小 · 截断
    QString         m_sCurPath;              // 相对链接重写基目录
};
