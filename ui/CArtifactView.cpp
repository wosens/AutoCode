#include "CArtifactView.h"

#include <QFile>
#include <QByteArray>

CArtifactView::CArtifactView(QWidget* a_pParent)
    : QTextBrowser(a_pParent) {
    setOpenExternalLinks(true);   // md 内链接点击在系统浏览器打开
    setPlaceholderText(QStringLiteral("选中左侧阶段产物以查看内容"));
}

void CArtifactView::ShowFile(const QString& a_sFilePath) {
    QFile stFile(a_sFilePath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        setPlainText(QStringLiteral("无法读取: %1").arg(a_sFilePath));
        return;
    }
    QByteArray aBody = stFile.readAll();
    stFile.close();

    // 剥离 UTF-8 BOM（后续要用 aBody 做 GBK 兜底，先剥离再判断更稳）
    if (aBody.startsWith("\xEF\xBB\xBF")) {
        aBody = aBody.mid(3);
    }
    // 优先 UTF-8；解出替换符 U+FFFD 说明源文件并非 UTF-8 → 按系统 ANSI(中文 GBK) 兜底
    QString sText = QString::fromUtf8(aBody);
    if (sText.contains(QChar(0xFFFD))) {
        sText = QString::fromLocal8Bit(aBody);
    }
    setMarkdown(sText);
}

void CArtifactView::Clear() {
    clear();
}
