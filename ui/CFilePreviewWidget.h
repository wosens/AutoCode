#pragma once

#include <QWidget>

class QStackedWidget;
class QTextBrowser;
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class CCodeSyntaxView;   // 2026-10-31 源码语法渲染页（CodeStyle 模块动态加载）

// 2026.08.25 文件预览控件（替代原 QPlainTextEdit 单视图）：
//   按文件类型三选一切换（QStackedWidget）：
//   [0] 纯文本（QTextBrowser 富文本关闭，等宽字体长行横滚）
//   [1] Markdown（setMarkdown 带格式渲染；2026-10-26 增强：文档 baseUrl 解析相对
//       图片 ![](rel)（旧 RewriteLocalUrls 只重写 src|href 碰不到 markdown 语法，
//       相对图片全部裂图）＋表格网格线/表头样式（Qt 导入器零样式）＋远程 http(s)
//       图片同步拉取（徽章/CDN 图标，预算 48 张/15s）＋emoji 字体回退链）
//   [2] HTML（setHtml 富文本渲染，基本格式保留）
//   [3] 十六进制（CHexView：左 hex 每行 16 字节 + 右 ASCII，双区滚动同步）
//   [4] 源码语法渲染（2026-10-31 CCodeSyntaxView：CodeStyle 模块 CodeParser.dll +
//       CodeView.dll 动态加载，Scintilla 渲染 + tree-sitter 语义分档；.c/.cpp/.h 等
//       源码后缀启用，设置 [View] CodeSyntax 可关（默认开），DLL 缺失/视图名额被
//       占用时回退 [0] 纯文本——功能闸门，其余页面不受影响）
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
    // 2026-10-08 改 static：CDiffViewDialog（查看变更）复用同源探测解码 GBK 等本地编码
    //   （实例名调用语法不变，旧调用处零改动）。
    static QString DetectAndDecode(const QByteArray& a_aBytes, QString& a_sText, bool& a_bBinary);

    // 2026.09.22 文件内查找（顶部工具栏「查找」按钮/Ctrl+F 打开查找栏；此为按钮/
    //   回车的执行核心，public 供单测直调）：在当前视图（纯文本/MD/HTML 的
    //   QTextBrowser 或十六进制的 ASCII 列）中按方向/大小写/循环查找，命中后
    //   选中并滚动到命中串；返回是否找到。文本/MD/HTML 走 QTextBrowser::find
    //   原生选中，十六进制在 ASCII 列选中。
    bool FindText(const QString& a_sText, bool a_bBackward, bool a_bCase, bool a_bWrap);

    // 2026-09-30 代码审查定位（双击发现行 → 打开文件并定位）：纯文本/MD/HTML
    //   视图滚动到指定 1-based 行并整行选中；十六进制视图忽略（无行号语义）。
    void GotoLine(int a_iLine);

    // 2026-10-26 Markdown 视图本体（单测直调：检查 baseUrl/表格样式/图片资源）。
    QTextBrowser* MdView() const { return m_pMd; }

    // 2026-10-31 源码语法渲染页是否在用（当前找的页=语法页；单测与外部观察用）。
    bool IsCodeViewActive() const;

private:
    void BuildUi();
    void ShowHex(const QByteArray& a_aBytes);

    // md/html 内本地相对链接重写为 file:/// 绝对路径（图片可见）
    static QString RewriteLocalUrls(const QString& a_sText, const QString& a_sBaseDir);

    // 2026.09.22 查找工具栏（顶部常驻「查找」按钮）+ 可展开查找栏（Ctrl+F/按钮/
    //   Esc 关闭；回车下一个、Shift+回车上一个；选项：区分大小写/循环，均缺省关）。
    void BuildFindUi(QVBoxLayout* a_pLay);
    void OnFindOpen();
    void OnFindTriggered(bool a_bBackward);

private:
    QStackedWidget* m_pStack    = nullptr;
    QTextBrowser*   m_pText     = nullptr;   // 纯文本
    QTextBrowser*   m_pMd       = nullptr;   // Markdown
    QTextBrowser*   m_pHtml     = nullptr;   // HTML
    QWidget*        m_pHexWrap  = nullptr;   // 十六进制视图（CFileHexView，cpp 内定义）
    CCodeSyntaxView* m_pCode    = nullptr;   // 源码语法渲染（CodeStyle 模块；可回退）
    QLabel*         m_pStat     = nullptr;   // 底部状态栏：编码 · 大小 · 截断
    QString         m_sCurPath;              // 相对链接重写基目录

    // —— 查找（2026.09.22） ——
    QWidget*        m_pToolBar  = nullptr;   // 顶部工具栏（常驻，含「查找」按钮）
    QToolButton*    m_pBtnFind  = nullptr;   // 「查找」按钮（展开查找栏）
    QWidget*        m_pFindBar  = nullptr;   // 查找栏（隐藏，Ctrl+F/按钮展开）
    QLineEdit*      m_pEditFind = nullptr;
    QCheckBox*      m_pChkCase  = nullptr;   // 区分大小写（缺省关）
    QCheckBox*      m_pChkWrap  = nullptr;   // 循环查找（缺省关）
    QPushButton*    m_pBtnPrev  = nullptr;   // ↑ 上一个
    QPushButton*    m_pBtnNext  = nullptr;   // ↓ 下一个（缺省方向）
    QLabel*         m_pLblFindSt = nullptr;  // 查找状态（未找到提示）
};
