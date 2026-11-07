#pragma once

#include <QTextBrowser>

// 阶段产物查看器：读取 .md 产物文件，按 Markdown 渲染显示
// （标题 / 列表 / 表格 / 代码块 / 任务列表）。
//
// 编码处理遵循 CLAUDE.md：带 UTF-8 BOM 按 UTF-8 读取；
// 若 UTF-8 解出替换符 U+FFFD（说明源文件不是 UTF-8），
// 退回系统 ANSI（中文系统为 GBK）兜底，避免乱码。
class CArtifactView : public QTextBrowser {
    Q_OBJECT

public:
    explicit CArtifactView(QWidget* a_pParent = nullptr);

    // 读取并渲染指定文件；打开失败则显示错误提示。
    void ShowFile(const QString& a_sFilePath);

    // 2026-11-04 分析报告页：直接渲染内存中的 Markdown 文本（报告不在阶段产物
    //   目录，由助手分析报告对话框持有原文；口径与 ShowFile 一致——setMarkdown）。
    void ShowMarkdownText(const QString& a_sText);

    // 清空当前显示。
    void Clear();
};
