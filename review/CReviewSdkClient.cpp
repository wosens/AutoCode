// CReviewSdkClient.cpp —— ReviewSdk.dll 动态加载实现
// 加载点：exe 同目录 ReviewSdk.dll（便携原则：Execute 目录拷走即自包含，
// 缺 dll 时审查功能闸门关闭、其余功能不受影响——与 CodeReview.exe 同口径）。

#include "CReviewSdkClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

CReviewSdkClient* CReviewSdkClient::Get() {
    // 进程内单例：C++11 起静态局部初始化线程安全。
    static CReviewSdkClient s_inst;
    return &s_inst;
}

bool CReviewSdkClient::EnsureLoaded(QString* a_pErr) {
#ifdef Q_OS_WIN
    if (m_bTried) {
        if (a_pErr != nullptr) {
            *a_pErr = m_sLoadErr;
        }
        return m_bLoaded;
    }
    m_bTried = true;

    const QString sDll = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/ReviewSdk.dll");
    const std::wstring wsDll = QDir::toNativeSeparators(sDll).toStdWString();
    HMODULE hMod = ::LoadLibraryW(wsDll.c_str());
    if (hMod == nullptr) {
        const DWORD dwErr = ::GetLastError();
        wchar_t wszBuf[256] = {0};
        ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, dwErr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         wszBuf, 256, nullptr);
        m_sLoadErr = QStringLiteral("加载 %1 失败：%2")
                         .arg(sDll, QString::fromWCharArray(wszBuf).trimmed());
        if (a_pErr != nullptr) {
            *a_pErr = m_sLoadErr;
        }
        return false;
    }

    // 9 个导出函数逐个解析（任一失败即判不可用，防半加载状态）。
    m_pfnVersion = reinterpret_cast<const char* (*)(void)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_Version")));
    m_pfnSupportedLanguages = reinterpret_cast<const char* (*)(void)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_SupportedLanguages")));
    m_pfnRulesForLanguage = reinterpret_cast<char* (*)(const char*)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_RulesForLanguage")));
    m_pfnLanguageForFile = reinterpret_cast<char* (*)(const char*)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_LanguageForFile")));
    m_pfnReviewContent = reinterpret_cast<int (*)(const char*, const char*, const char*,
                                                  FnLcrLlmInvoke, void*, char**, char**)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_ReviewContent")));
    m_pfnReviewContentEx = reinterpret_cast<int (*)(const char*, const char*, const char*, int,
                                                    FnLcrLlmInvoke, void*, char**, char**)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_ReviewContentEx")));
    m_pfnBuildIncrementalScope = reinterpret_cast<char* (*)(const char*, const char*)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_BuildIncrementalScope")));
    m_pfnReviewFile = reinterpret_cast<int (*)(const char*, FnLcrLlmInvoke, void*, char**, char**)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_ReviewFile")));
    m_pfnFreeString = reinterpret_cast<void (*)(char*)>(
        reinterpret_cast<void*>(::GetProcAddress(hMod, "LCR_FreeString")));

    if (m_pfnVersion == nullptr || m_pfnSupportedLanguages == nullptr
        || m_pfnRulesForLanguage == nullptr || m_pfnLanguageForFile == nullptr
        || m_pfnReviewContent == nullptr || m_pfnReviewContentEx == nullptr
        || m_pfnBuildIncrementalScope == nullptr || m_pfnReviewFile == nullptr
        || m_pfnFreeString == nullptr) {
        m_sLoadErr = QStringLiteral("ReviewSdk.dll 导出表不完整（LCR_* 函数解析失败）");
        if (a_pErr != nullptr) {
            *a_pErr = m_sLoadErr;
        }
        return false;
    }

    m_bLoaded = true;
    return true;
#else
    if (a_pErr != nullptr) {
        *a_pErr = QStringLiteral("非 Windows 平台不支持 ReviewSdk.dll"));
    }
    return false;
#endif
}

QString CReviewSdkClient::Version() const {
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnVersion == nullptr) {
        return QString();
    }
    const char* psz = m_pfnVersion();
    return (psz != nullptr) ? QString::fromUtf8(psz) : QString();
#else
    return QString();
#endif
}

QStringList CReviewSdkClient::SupportedLanguages() const {
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnSupportedLanguages == nullptr) {
        return QStringList();
    }
    const char* psz = m_pfnSupportedLanguages();
    if (psz == nullptr) {
        return QStringList();
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(QByteArray(psz));
    QStringList aList;
    const QJsonArray aArr = oDoc.array();
    for (const auto& v : aArr) {
        aList.append(v.toString());
    }
    return aList;
#else
    return QStringList();
#endif
}

QString CReviewSdkClient::LanguageForFile(const QString& a_sAbsPath) {
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnLanguageForFile == nullptr || m_pfnFreeString == nullptr) {
        return QString();
    }
    const QByteArray byPath = a_sAbsPath.toUtf8();
    char* psz = m_pfnLanguageForFile(byPath.constData());
    if (psz == nullptr) {
        return QString();   // 未知扩展名
    }
    const QString sLang = QString::fromUtf8(psz);
    m_pfnFreeString(psz);
    return sLang;
#else
    Q_UNUSED(a_sAbsPath);
    return QString();
#endif
}

QStringList CReviewSdkClient::RulesForLanguage(const QString& a_sLanguage) {
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnRulesForLanguage == nullptr || m_pfnFreeString == nullptr) {
        return QStringList();
    }
    const QByteArray byLang = a_sLanguage.toUtf8();
    char* psz = m_pfnRulesForLanguage(byLang.constData());
    if (psz == nullptr) {
        return QStringList();
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(QByteArray(psz));
    m_pfnFreeString(psz);
    QStringList aList;
    const QJsonArray aArr = oDoc.array();
    for (const auto& v : aArr) {
        aList.append(v.toString());
    }
    return aList;
#else
    Q_UNUSED(a_sLanguage);
    return QStringList();
#endif
}

int CReviewSdkClient::ReviewFile(const QString& a_sAbsPath, const QString& a_sDisplayPath,
                                 FnLcrLlmInvoke a_pfnLlm, void* a_pUserCtx,
                                 QString& a_sFindingsJson, QString& a_sErr) {
    a_sFindingsJson.clear();
    a_sErr.clear();
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnReviewFile == nullptr || m_pfnFreeString == nullptr) {
        a_sErr = QStringLiteral("ReviewSdk.dll 未加载");
        return AUTOCODE_LCR_ERR_ARGS;
    }
    // ReviewFile 无独立标注路径参数：findings.file = 所传路径原样。传磁盘绝对
    // 路径保证库内读取链可用，报告中回显绝对路径；调用方（CReviewService）
    // 在合并 JSON 时统一改写为工程相对路径。
    const QByteArray byPath = QDir::toNativeSeparators(a_sAbsPath).toUtf8();
    char* pszFindings = nullptr;
    char* pszError    = nullptr;
    const int iRc = m_pfnReviewFile(byPath.constData(), a_pfnLlm, a_pUserCtx,
                                    &pszFindings, &pszError);
    if (pszFindings != nullptr) {
        a_sFindingsJson = QString::fromUtf8(pszFindings);
        m_pfnFreeString(pszFindings);
    }
    if (pszError != nullptr) {
        a_sErr = QString::fromUtf8(pszError);
        m_pfnFreeString(pszError);
    }
    return iRc;
#else
    Q_UNUSED(a_sAbsPath); Q_UNUSED(a_sDisplayPath); Q_UNUSED(a_pfnLlm);
    Q_UNUSED(a_pUserCtx);
    a_sErr = QStringLiteral("非 Windows 平台不支持 ReviewSdk.dll");
    return AUTOCODE_LCR_ERR_ARGS;
#endif
}

int CReviewSdkClient::ReviewContent(const QString& a_sLanguage, const QString& a_sDisplayPath,
                                    const QString& a_sContentUtf8,
                                    FnLcrLlmInvoke a_pfnLlm, void* a_pUserCtx,
                                    QString& a_sFindingsJson, QString& a_sErr) {
    a_sFindingsJson.clear();
    a_sErr.clear();
#ifdef Q_OS_WIN
    if (!m_bLoaded || m_pfnReviewContent == nullptr || m_pfnFreeString == nullptr) {
        a_sErr = QStringLiteral("ReviewSdk.dll 未加载");
        return AUTOCODE_LCR_ERR_ARGS;
    }
    const QByteArray byLang   = a_sLanguage.toUtf8();
    const QByteArray byPath   = a_sDisplayPath.toUtf8();
    const QByteArray byContent = a_sContentUtf8.toUtf8();
    char* pszFindings = nullptr;
    char* pszError    = nullptr;
    const int iRc = m_pfnReviewContent(
        (a_sLanguage.isEmpty() ? nullptr : byLang.constData()),
        byPath.constData(), byContent.constData(),
        a_pfnLlm, a_pUserCtx, &pszFindings, &pszError);
    if (pszFindings != nullptr) {
        a_sFindingsJson = QString::fromUtf8(pszFindings);
        m_pfnFreeString(pszFindings);
    }
    if (pszError != nullptr) {
        a_sErr = QString::fromUtf8(pszError);
        m_pfnFreeString(pszError);
    }
    return iRc;
#else
    Q_UNUSED(a_sLanguage); Q_UNUSED(a_sDisplayPath); Q_UNUSED(a_sContentUtf8);
    Q_UNUSED(a_pfnLlm); Q_UNUSED(a_pUserCtx);
    a_sErr = QStringLiteral("非 Windows 平台不支持 ReviewSdk.dll");
    return AUTOCODE_LCR_ERR_ARGS;
#endif
}

QString CReviewSdkClient::RelativeToRoot(const QString& a_sRootDir, const QString& a_sAbsPath) {
    const QString sRoot = QDir::cleanPath(a_sRootDir);
    const QString sAbs  = QDir::cleanPath(a_sAbsPath);
    if (!sRoot.isEmpty() && sAbs.size() > sRoot.size()
        && QString::compare(sAbs.left(sRoot.size()), sRoot, Qt::CaseInsensitive) == 0
        && (sAbs.at(sRoot.size()) == QLatin1Char('/')
            || sAbs.at(sRoot.size()) == QLatin1Char('\\'))) {
        return QDir::fromNativeSeparators(sAbs.mid(sRoot.size() + 1));
    }
    return QDir::fromNativeSeparators(sAbs);
}
