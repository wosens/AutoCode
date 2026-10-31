#pragma once

// ============================================================================
// CCodeSyntaxView —— CodeStyle 源码语法渲染宿主（2026-10-31 集成）
// ----------------------------------------------------------------------------
// 参照 E:\wosens\Programs\VisualC++\QT\CodeStyle\doc\SDK开发手册.md 动态加载
// exe 同目录 CodeParser.dll + CodeView.dll（两库零链接、GetProcAddress 绑定，
// 口径同 review/CReviewSdkClient）：
//   - CodeView.dll：内嵌 Scintilla+Lexilla 的只读渲染视图（词法着色兜底 +
//     F2 同名词联动 / F3 括号配对，均库内实现）；
//   - CodeParser.dll：tree-sitter 语义解析，输出带样式档的 CpsSpan 数组由
//     本类灌回渲染库（函数/类型/变量真语义分档）。
// 降级链：DLL 缺失 / 导出不全 / CvcCreate 失败（POC 全局仅一个存活视图）→
//   IsOk()=false，CFilePreviewWidget 回退纯文本视图，其余功能不受影响。
// 契约要点（手册 §7）：span 为 UTF-8 字节偏移；CvcSetText 后才能 CvcApplySpans；
//   全部调用在 UI 线程；GBK 等本地编码文本先转 UTF-8 再喂两端（调用方负责）。
// ============================================================================

#include <QWidget>
#include <QByteArray>

class CCodeSyntaxView : public QWidget {
    Q_OBJECT

public:
    explicit CCodeSyntaxView(QWidget* a_pParent = nullptr);
    ~CCodeSyntaxView() override;

    // SDK 在位性（DLL 均可加载 + 导出齐全；不含视图实例占用）。进程内只探测一次。
    static bool ModuleAvailable();
    // 本实例视图是否创建成功（失败即调用方回退纯文本）。
    bool IsOk() const { return m_hView != nullptr; }

    // 显示源码全文：a_aU8Text 为 UTF-8 字节（GB 等编码由调用方解码后回编码）；
    //   a_bCpp=true 走 C++ 语法库（.c 传 false 走 C）；a_bSemantic=true 时同步做
    //   tree-sitter 语义分档（大文件调用方置 false 只词法着色，防整篇解析卡 UI）。
    //   返回 false = 视图不可用（调用方回退）。
    bool ShowSource(const QByteArray& a_aU8Text, bool a_bCpp, bool a_bSemantic);

    // 文件内查找（Scintilla SCI_SEARCHINTARGET；命中即选中滚动；循环回卷）。
    bool FindText(const QString& a_sText, bool a_bBackward, bool a_bCase, bool a_bWrap);

    // 定位到 1-based 行并整行选中滚动可见（代码审查定位链）。
    void GotoLine(int a_iLine);

signals:
    // 原生窗口持有键盘焦点时 Qt 快捷键链收不到（Ctrl+F 开查找栏）——本类装了
    //   原生消息过滤器代听 Ctrl+F，命中发此信号由宿主（查找栏所在）接管。
    void sigFindRequested();

protected:
    void resizeEvent(QResizeEvent* a_e) override;

private:
    // 视图句柄（CodeView.dll 的 hCvcView；析构 CvcDestroy——POC 单实例全局仅一个
    //   存活，本类与 CFilePreviewWidget 均为单实例，先析构后可再建）。
    void* m_hView = nullptr;

#ifdef Q_OS_WIN
    //   原生键监听过滤器（随本视图装/卸；见 cpp）。
    class CCodeKeyFilter* m_pKeyFilter = nullptr;
#endif
};
