// CMarkdownPreloader：子线程 Markdown 预排版器（见头文件说明）。
#include "CMarkdownPreloader.h"

#include <QTextDocument>
#include <QThreadPool>
#include <QRunnable>
#include <QMetaObject>
#include <QThread>

namespace {
// 预排版文本上限：超长正文排版耗时随长度线性涨，超限按失败处理——调用方回退
// 主线程 setMarkdown（行为与原同步路径一致，只是该块不加速）。
constexpr int k_iMaxChars = 200000;
} // namespace

// 单次预排版任务：捕获回调与素材；完成（含失败）必经 invokeMethod 队列回主线程，
//   以单例对象为 context（进程级常驻，理论不亡；即便亡，队列事件自动丢弃不悬垂）。
class CPreloadTask : public QRunnable {
public:
    CPreloadTask(CMarkdownPreloader* a_pOwner, QString a_sMarkdown, qreal a_dWidth,
                 std::function<void(QTextDocument*)> a_fnDone)
        : m_pOwner(a_pOwner), m_sMarkdown(std::move(a_sMarkdown)),
          m_dWidth(a_dWidth), m_fnDone(std::move(a_fnDone)) {
        setAutoDelete(true);
    }

    void run() override {
        // 工作线程：纯 CPU 排版，不触任何 GUI。QTextDocument 允许在非 GUI 线程使用。
        QTextDocument* pDoc = nullptr;
        if (!m_sMarkdown.isEmpty() && m_dWidth > 0 && m_sMarkdown.size() <= k_iMaxChars) {
            // new 出的 doc 无 parent（QObject 线程亲和=本工作线程）——排版完
            // moveToThread 移交主线程再回调（QObject 跨线程访问须先移亲和）。
            pDoc = new QTextDocument();
            pDoc->setMarkdown(m_sMarkdown);
            pDoc->setTextWidth(m_dWidth);   // 按预估宽度初排版（换行+高度就绪）
            pDoc->moveToThread(m_pOwner->thread());
        }
        auto fnDone = m_fnDone;   // 拷贝出（run 返回后本对象即自毁）
        CMarkdownPreloader* pOwner = m_pOwner;
        QMetaObject::invokeMethod(pOwner, [pOwner, fnDone, pDoc]() {
            fnDone(pDoc);   // 主线程：落地（setDocument）或 delete；nullptr=失败回退
        }, Qt::QueuedConnection);
    }

private:
    CMarkdownPreloader* m_pOwner = nullptr;   // 单例常驻（invoke context）
    QString m_sMarkdown;
    qreal   m_dWidth = 0.0;
    std::function<void(QTextDocument*)> m_fnDone;
};

CMarkdownPreloader* CMarkdownPreloader::Instance() {
    static CMarkdownPreloader* pInst = new CMarkdownPreloader();
    return pInst;
}

CMarkdownPreloader::CMarkdownPreloader(QObject* a_pParent)
    : QObject(a_pParent) {
    // 单例不挂父对象（进程级）；创建于主线程（首次 Submit 时惰性初始化）。
}

void CMarkdownPreloader::Submit(const QString& a_sMarkdown, qreal a_dWidth,
                                const std::function<void(QTextDocument*)>& a_fnDone) {
    if (a_fnDone == nullptr || thread() != QThread::currentThread()) {
        return;   // 只接受主线程提交（回调经 invokeMethod 必回主线程）
    }
    QThreadPool::globalInstance()->start(
        new CPreloadTask(this, a_sMarkdown, a_dWidth, a_fnDone));
}
