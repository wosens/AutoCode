#pragma once

#include <QObject>
#include <QString>
#include <QPointer>
#include <functional>

class QTextDocument;

// =============================================================================
// CMarkdownPreloader：子线程 Markdown 预排版器（2026.09.04 日志加载不卡主线程）。
//
// 背景：对话流块创建（切档重建/到顶补建）的主线程成本大头是 Assistant 块的
// QTextBrowser::setMarkdown——Markdown parse + 初排版纯 CPU，单块可达几 ms~十几 ms，
// 200 块批量重建即使分帧也持续占用主线程（用户实测"加载日志还是卡"）。
//
// 做法：QTextDocument 不是 widget，允许在非 GUI 线程创建与排版。把 parse+初排版
//   （setMarkdown + setTextWidth 按预估宽度）投给 QThreadPool 工作线程；完成后
//   document moveToThread 回主线程，回调里交给 QTextBrowser::setDocument 落地。
//   主线程只剩 setDocument（廉价）+ 真实宽度微调重排（CMessageBlock 既有链路）。
//
// 语义：
//   - Submit 立即返回（永不阻塞主线程）；完成回调必在主线程调用（invokeMethod）。
//   - a_fnDone(nullptr) = 预估宽度无效等失败（调用方回退同步 setMarkdown）。
//   - 回调时请求方（块/视图）可能已销毁——调用方用 QPointer 自行判活（见
//     CMessageBlock::SetAssistantTextAsync）。
//   - 全局单例，进程生命周期；关窗时未完任务随线程池析构自然丢弃（回调不再执行）。
// =============================================================================
class CMarkdownPreloader : public QObject {
    Q_OBJECT

public:
    static CMarkdownPreloader* Instance();   // 主线程惰性建（单例）

    // 提交一次预排版：a_dWidth = 预估排版宽度（视口宽-块边距，<=0 视为失败）；
    //   完成后主线程回调 a_fnDone(doc)——doc 所有权移交回调方（落地或 delete）。
    void Submit(const QString& a_sMarkdown, qreal a_dWidth,
                const std::function<void(QTextDocument*)>& a_fnDone);

private:
    explicit CMarkdownPreloader(QObject* a_pParent = nullptr);
};
