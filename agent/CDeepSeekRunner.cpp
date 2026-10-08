#include "CDeepSeekRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include "CClaudeRunner.h"   // ContextWindowForModel：DSH 侧窗口兜底探测复用同一实现

// 2026.09.27 额外可写目录（pwsh 沙箱 DACL 放行）：SID 派生 + ACE 授/撤。
#include <QCryptographicHash>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>   // GetNamedSecurityInfo / SetEntriesInAcl / SetNamedSecurityInfo
#include <sddl.h>     // ConvertStringSidToSid

// —— 进程级全局配置（static：所有 DSH runner 实例共享，主窗口启动/设置变更时写）——
static QString s_sDshRootPath;    // DSH 仓库根（deepseek-harness 目录；空=未配置/未探测到）
static QString s_sDshNodePath;    // node.exe 绝对路径（空=走 PATH 的 "node"）
static QString s_sDshEffort;      // 2026-10-08 思考强度（initialize reasoningEffort；空=默认）
static QString s_sDshBinJsPath;   // bin.js 绝对路径（空=按根目录推导；显式键优先，旧档回退）
static QString s_sDshCordisPath;  // cordis.yml 绝对路径（空=按根目录推导；显式键优先，旧档回退）
static QString s_sDshProvider;    // provider 名（空回退 glm-custom）
static QString s_sDshModel;       // 模型名（固定路由项已删，仅作 [DshModels] 缺省占位）
// 计费单价（$/百万 token；DSH 协议只报 token 不报金额，客户端折算）。
//   默认对齐 claude 后端经同一网关显示的 Sonnet 档价目；全 0 = 不折算（费用列回 "-"）。
static double s_dDshPriceInPerM        = 3.0;
static double s_dDshPriceCacheReadPerM = 0.3;
static double s_dDshPriceCacheWritePerM = 3.75;
static double s_dDshPriceOutPerM       = 15.0;
// 自定义大模型表 + 激活下标（-1 = 未启用；条目字段注入见 EnsureProcess env 段）
static QVector<CDeepSeekRunner::SCustomModel> s_aCustomModels;
static int s_iCustomActive = -1;
// 2026.08.19 热生效支持：活实例登记（配置变更时寻址各进程）+ 冷启动签名（激活条目
//   全字段；变化才重启，设置页反复确定/无关改动不打扰既有会话）。
static QSet<CDeepSeekRunner*> s_aLiveRunners;
static QString s_sCustomBootSig;

// 根目录内固定相对布局（bin.js 与 cordis.yml 的推导位置）。
static constexpr const char* kSzDshBinJsRel  = "/packages/examples/jsonrpc-demo/lib/bin.js";
static constexpr const char* kSzDshCordisRel = "/examples/autocode-agent/cordis.yml";
// 2026.09.10 Profile 布局：上游 dsh-v0.1.2 起官方 SDK 入口（dsh CLI 构建产物；
//   git 里只有 src，pnpm build 后生成——存在即「构建完成的新版根」）。
static constexpr const char* kSzDshCliBinRel = "/apps/cli/lib/bin.js";

void CDeepSeekRunner::SetDshRootPath(const QString& a_sRoot) {
    s_sDshRootPath = QDir::fromNativeSeparators(a_sRoot.trimmed());
    while (s_sDshRootPath.endsWith(QLatin1Char('/'))) {
        s_sDshRootPath.chop(1);
    }
}

QString CDeepSeekRunner::DshRootPath() {
    return s_sDshRootPath;
}

// —— 2026.09.27 DSH 额外可写目录（pwsh 沙箱 DACL 放行）——————————————
// DSH windows-acl 沙箱：workspace-write 的强制是纯 NTFS DACL——受限令牌的 restricting
//   组里带工作区写 SID（S-1-4-x-y，sha256(规范化工作区根) 派生，见 harness
//   sandbox-windows-acl/src/workspace-sid.ts）；目录可写 ⟺ 其 DACL 授了该 SID
//   （Allow，OI|CI，FILE_GENERIC_WRITE|DELETE|FILE_DELETE_CHILD & ~STANDARD_RIGHTS_WRITE，
//   见 win32-abi.ts GRANT_MASK / acl.ts buildExplicitAccess）。拒绝标记是 stderr 特征
//   分类（无路径预检）→ 对工程根之外的关联目录授同一 SID 的同形 ACE 即放行，
//   零 harness 改动、对已部署副本全部生效。ACE 是 standing（留在 DACL 里跨会话复用，
//   与 harness 自身的 workspace 复用缓存同语义）。
static QStringList s_aDshExtraWriteDirs;   // [Dsh] ExtraWriteDirs（';' 分隔）

void CDeepSeekRunner::SetDshExtraWriteDirs(const QStringList& a_aDirs) {
    QStringList aNorm;
    for (const QString& s : a_aDirs) {
        const QString sT = QDir::fromNativeSeparators(s.trimmed());
        if (!sT.isEmpty() && !aNorm.contains(sT)) {
            aNorm.append(sT);
        }
    }
    s_aDshExtraWriteDirs = aNorm;
}

QStringList CDeepSeekRunner::DshExtraWriteDirs() {
    return s_aDshExtraWriteDirs;
}

// node realpathSync.native 等价（GetFinalPathNameByHandleW，剥 \?\ 前缀，真实大小写）：
//   SID 输入必须与 harness 侧 canonicalPath 完全一致。
static QString DshCanonicalWinPath(const QString& a_sDir) {
    const QString sNative = QDir::toNativeSeparators(a_sDir);
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(sNative.utf16()),
                           FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE
                               | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // 目录句柄
    if (h == INVALID_HANDLE_VALUE) {
        return QString();
    }
    wchar_t wsBuf[1024] = {};
    const DWORD dwN = GetFinalPathNameByHandleW(h, wsBuf, 1024, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (dwN == 0 || dwN >= 1024) {
        return QString();
    }
    QString s = QString::fromWCharArray(wsBuf, static_cast<int>(dwN));
    if (s.startsWith(QLatin1String("\\\\?\\"))) {   // \\?\ 前缀（源码级双反斜杠×4+?+2）
        s = s.mid(4);
    }
    return s;
}

// 工作区写 SID：sha256(utf8(规范化根)) 前两个 LE u32 mod (2^30-1) + 1（对齐
//   sandbox-windows-acl/src/workspace-sid.ts workspaceWriteSid）。
static QString DshWorkspaceWriteSidOf(const QString& a_sCanonicalRoot) {
    const QByteArray aDig = QCryptographicHash::hash(
        a_sCanonicalRoot.toUtf8(), QCryptographicHash::Sha256);
    if (aDig.size() < 8) {
        return QString();
    }
    const quint32 uOne = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(aDig.constData()));
    const quint32 uTwo = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(aDig.constData()) + 4);
    const quint64 uMax = (Q_UINT64_C(1) << 30) - 1;
    return QStringLiteral("S-1-4-%1-%2")
        .arg(QString::number(uOne % uMax + 1), QString::number(uTwo % uMax + 1));
}

// 授/撤一个 (目录, SID) 的 ACE。a_bGrant=false 撤销（REVOKE_ACCESS 删该 trustee 全部
//   ACE）。幂等：已存在精确 ACE（Allow+OI|CI+GRANT_MASK+同 SID）跳过。跨进程命名
//   互斥串行化（多 AutoCode 实例/多助手并发授同目录防 DACL 读-合-写竞态，对齐 harness
//   的 dsh-acl-locks 思路）。返回是否成功（目录不存在不算失败，返回 true 跳过）。
static bool DshApplyDirAce(const QString& a_sDir, const QString& a_sSid, bool a_bGrant) {
    static const DWORD kDshGrantMask =
        (FILE_GENERIC_WRITE | DELETE | FILE_DELETE_CHILD) & ~STANDARD_RIGHTS_WRITE;
    const QString sNative = QDir::toNativeSeparators(a_sDir);
    if (!QFileInfo::exists(sNative) || !QFileInfo(sNative).isDir()) {
        return true;   // 目录不在场：无可授（后续会话也不需要；不算失败）
    }
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"AutoCode_DshAclGrant_Mutex");
    if (hMutex != nullptr) {
        WaitForSingleObject(hMutex, 8000);   // 已被创建并持有→等；二次持有也无妨
    }
    struct CMutexGuard {
        HANDLE h;
        ~CMutexGuard() { if (h != nullptr) { ReleaseMutex(h); CloseHandle(h); } }
    } stGuard{ hMutex };

    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(
            reinterpret_cast<const wchar_t*>(a_sSid.utf16()), &pSid) || pSid == nullptr) {
        return false;
    }
    struct CSidGuard {
        PSID p;
        ~CSidGuard() { if (p != nullptr) { LocalFree(p); } }
    } stSidGuard{ pSid };

    // 已有精确 ACE？读 DACL 逐条比对（幂等防叠加）。
    PACL pOld = nullptr;
    PSECURITY_DESCRIPTOR pSd = nullptr;
    if (GetNamedSecurityInfoW(reinterpret_cast<const wchar_t*>(sNative.utf16()),
                              SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &pOld, nullptr, &pSd) != ERROR_SUCCESS
        || pOld == nullptr) {
        if (pSd != nullptr) { LocalFree(pSd); }
        return false;
    }
    struct CSdGuard {
        PSECURITY_DESCRIPTOR p;
        ~CSdGuard() { if (p != nullptr) { LocalFree(p); } }
    } stSdGuard{ pSd };
    if (a_bGrant) {
        BOOL bFound = FALSE;
        for (DWORD i = 0; i < pOld->AceCount && !bFound; ++i) {
            void* pAce = nullptr;
            if (!GetAce(pOld, i, &pAce) || pAce == nullptr) { continue; }
            const auto* pHdr = static_cast<const ACE_HEADER*>(pAce);
            if (pHdr->AceType != ACCESS_ALLOWED_ACE_TYPE) { continue; }
            auto* pAllow = const_cast<ACCESS_ALLOWED_ACE*>(
                static_cast<const ACCESS_ALLOWED_ACE*>(pAce));
            if (pAllow->Mask != kDshGrantMask) { continue; }
            if ((pHdr->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
                    != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) {
                continue;
            }
            if (EqualSid(reinterpret_cast<PSID>(&pAllow->SidStart), pSid)) {
                bFound = TRUE;
            }
        }
        if (bFound) {
            return true;
        }
    }

    EXPLICIT_ACCESS_W stEa = {};
    stEa.grfAccessPermissions = kDshGrantMask;
    stEa.grfAccessMode = a_bGrant ? GRANT_ACCESS : REVOKE_ACCESS;
    stEa.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    stEa.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    stEa.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSid);
    PACL pNew = nullptr;
    if (SetEntriesInAclW(1, &stEa, pOld, &pNew) != ERROR_SUCCESS || pNew == nullptr) {
        return false;
    }
    const DWORD dwR = SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(const_cast<ushort*>(sNative.utf16())),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, pNew, nullptr);
    LocalFree(pNew);
    return dwR == ERROR_SUCCESS;
}

int CDeepSeekRunner::ApplyExtraWriteDirs(const QString& a_sWorkDir,
                                         const QStringList& a_aGrant,
                                         const QStringList& a_aRevoke) {
    // 工程根规范化 → 工作区写 SID（与 harness 派生一致）→ 逐目录授/撤。
    if ((a_aGrant.isEmpty() && a_aRevoke.isEmpty()) || a_sWorkDir.trimmed().isEmpty()) {
        return 0;
    }
    const QString sCanonicalRoot = DshCanonicalWinPath(a_sWorkDir);
    if (sCanonicalRoot.isEmpty()) {
        return a_aGrant.size();   // 工程根拿不到规范名：全部按失败计（不阻断，日志可见）
    }
    const QString sSid = DshWorkspaceWriteSidOf(sCanonicalRoot);
    if (sSid.isEmpty()) {
        return a_aGrant.size();
    }
    int iFailed = 0;
    for (const QString& sDir : a_aRevoke) {
        if (!a_aGrant.contains(sDir) && !DshApplyDirAce(sDir, sSid, false)) {
            ++iFailed;
        }
    }
    for (const QString& sDir : a_aGrant) {
        if (!DshApplyDirAce(sDir, sSid, true)) {
            ++iFailed;
        }
    }
    return iFailed;
}

QString CDeepSeekRunner::ResolveNodePath() {
    // 留空走 PATH：QProcess 直起可执行名由系统搜路径（本机装了 node 即零配置）。
    return s_sDshNodePath.isEmpty() ? QStringLiteral("node") : s_sDshNodePath;
}

CDeepSeekRunner::EDshLayout CDeepSeekRunner::DshLayoutOf(const QString& a_sRoot) {
    // 2026.09.10 布局判型（优先 Legacy：新旧同存的过渡根按旧口径跑，行为已验证）：
    //   Legacy = examples/jsonrpc-demo/bin.js 存在（≤0.1.1 示例包布局）；
    //   Profile = apps/cli/lib/bin.js 存在（≥0.1.2 官方 SDK profile，需已构建）；
    //   None = 两者皆无（未安装/未构建/未知布局）。
    const QString sRoot = QDir::fromNativeSeparators(
        (a_sRoot.isEmpty() ? s_sDshRootPath : a_sRoot).trimmed());
    if (sRoot.isEmpty()) {
        return EDshLayout::None;
    }
    if (QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshBinJsRel))) {
        return EDshLayout::Legacy;
    }
    if (QFileInfo::exists(sRoot + QString::fromLatin1(kSzDshCliBinRel))) {
        return EDshLayout::Profile;
    }
    return EDshLayout::None;
}

QString CDeepSeekRunner::ResolveBinJsPath() {
    if (!s_sDshBinJsPath.isEmpty()) {
        return s_sDshBinJsPath;
    }
    if (s_sDshRootPath.isEmpty()) {
        return QString();
    }
    // 2026.09.10 双布局：Profile 根（apps/cli 构建产物）返回 dsh CLI 入口。
    if (DshLayoutOf() == EDshLayout::Profile) {
        return s_sDshRootPath + QString::fromLatin1(kSzDshCliBinRel);
    }
    return s_sDshRootPath + QString::fromLatin1(kSzDshBinJsRel);
}

QString CDeepSeekRunner::ResolvePatchPath() {
    // 2026.09.10 Profile 布局组合层：exe 同级 dsh\sdk.autocode.patch.yml（CMake
    //   POST_BUILD 从仓库 agent/dsh 同步）；绝对路径引用，无需部署到 DSH root。
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/dsh/sdk.autocode.patch.yml");
}

QStringList CDeepSeekRunner::ResolveStartArgs() {
    // 2026.09.10 双布局启动参数：Profile = dsh CLI 官方 SDK profile（--patch 叠加
    //   AutoCode 组合层：autocode-active 槽位/maxTokensAsSuccess=false/技能目录/
    //   persona/session root/压缩阈值）；Legacy = 旧两件套 {bin, cordis} 不变。
    if (DshLayoutOf() == EDshLayout::Profile) {
        return { ResolveBinJsPath(), QStringLiteral("--profile"), QStringLiteral("sdk"),
                 QStringLiteral("--patch"), ResolvePatchPath() };
    }
    return { ResolveBinJsPath(), ResolveCordisPath() };
}

QString CDeepSeekRunner::ResolveCordisPath() {
    if (!s_sDshCordisPath.isEmpty()) {
        return s_sDshCordisPath;
    }
    return s_sDshRootPath.isEmpty() ? QString()
                                    : s_sDshRootPath + QString::fromLatin1(kSzDshCordisRel);
}

bool CDeepSeekRunner::LooksLikeDshRoot(const QString& a_sRoot) {
    // 2026.09.10 双布局：Legacy 两件套或 Profile 构建产物，任一即认。
    const QString sRoot = QDir::fromNativeSeparators(a_sRoot.trimmed());
    return !sRoot.isEmpty()
        && DshLayoutOf(sRoot) != EDshLayout::None;
}

QString CDeepSeekRunner::DshConfigError() {
    // 2026.08.18 DSH 可用性统一校验：启动警告 / 设置保存兜底 / StartStage 前置三处
    //   共用同一口径——用户在任何入口看到的都是同一条完整处理指引，不会只报
    //   「路径无效」却不说怎么办。返回空串 = 可用。
    const QString sNode = ResolveNodePath();
    if (sNode == QStringLiteral("node")) {
        if (QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty()) {
            return QStringLiteral(
                "node 未配置且 PATH 中找不到 node.exe。\n"
                "处理：安装 Node.js 后重启 AutoCode（node 路径留空即走 PATH），或在"
                "「设置 → DeepSeekHarness 参数 → node 路径」填 node.exe 绝对路径。");
        }
    } else if (!QFile::exists(sNode)) {
        return QStringLiteral(
            "node 路径无效：%1。\n"
            "处理：「设置 → DeepSeekHarness 参数 → node 路径」改填有效的 node.exe"
            "（留空即走 PATH）。").arg(sNode);
    }
    const QString sBin = ResolveBinJsPath();
    if (sBin.isEmpty() || !QFile::exists(sBin)) {
        return QStringLiteral(
            "DSH 运行时 bin.js 无效：%1。\n"
            "处理：「设置 → DeepSeekHarness 参数」点「安装 DeepSeekHarness…」一键安装"
            "（完成后点「重新检测」，根目录自动填入）；或把「DSH 根目录」填为 "
            "deepseek-harness 仓库根（bin.js 按固定布局自动推导）。").arg(
                sBin.isEmpty() ? QStringLiteral("(空)") : sBin);
    }
    // 2026.09.10 Profile 布局分支（≥0.1.2 官方 SDK）：不查 cordis 两件套/spine 包
    //   （examples 树已删），改查 --patch 组合层存在性 + 同构模板自检（!!js 语法/
    //   autocode-active 槽位——与 Legacy 的 cordis 守卫同一套检查目标）。
    if (DshLayoutOf() == EDshLayout::Profile) {
        const QString sPatch = ResolvePatchPath();
        if (!QFile::exists(sPatch)) {
            return QStringLiteral(
                "DSH 为官方 SDK profile 布局，但程序目录缺组合层文件：%1。\n"
                "处理：从完整程序目录拷贝 dsh\\ 子目录（含 sdk.autocode.patch.yml），"
                "或重跑「安装 DeepSeekHarness…」后重试。").arg(
                    QDir::toNativeSeparators(sPatch));
        }
        CFrwDevice oPatch(sPatch);
        if (oPatch.open(QIODevice::ReadOnly)) {
            const QString sPatchText = QString::fromUtf8(oPatch.readAll());
            oPatch.close();
            for (const QString& sLine : sPatchText.split(QChar::LineFeed)) {
                const QString sTrimmed = sLine.trimmed();
                if (sTrimmed.startsWith(QLatin1Char('#'))) {
                    continue;
                }
                if (sTrimmed.contains("!!js [") || sTrimmed.contains("!!js {")) {
                    return QStringLiteral(
                        "组合层 sdk.autocode.patch.yml 含不支持的 !!js 序列/映射表达式"
                        "（!!js 仅支持标量）。\n文件：%1\n处理：更新程序目录 dsh\\"
                        "sdk.autocode.patch.yml 后重开 AutoCode。").arg(
                            QDir::toNativeSeparators(sPatch));
                }
            }
            if (CustomActiveValid() && !sPatchText.contains("autocode-active")) {
                return QStringLiteral(
                    "程序目录的 dsh\\sdk.autocode.patch.yml 是旧版（缺 autocode-active "
                    "槽位），自定义大模型无法路由。\n处理：从完整程序目录拷贝新的 "
                    "dsh\\sdk.autocode.patch.yml，或重跑「01.更新程序.bat」同步后重启。");
            }
        }
        return QString();   // Profile 布局校验完备
    }
    const QString sCordis = ResolveCordisPath();
    if (sCordis.isEmpty() || !QFile::exists(sCordis)) {
        return QStringLiteral(
            "DSH 插件组合 cordis.yml 无效：%1（组合文件须位于 DSH 仓库 examples/ 树内，"
            "裸插件名才能解析 node_modules）。\n"
            "处理：重跑「安装 DeepSeekHarness…」（会把组合文件部署到固定布局）；或把正本 "
            "AutoCode 仓库 agent/dsh/cordis.autocode.yml 复制为 "
            "<DSH根>/examples/autocode-agent/cordis.yml。").arg(
                sCordis.isEmpty() ? QStringLiteral("(空)") : sCordis);
    }
    // 2026.09.08 v2.1.127 模板语法自检：DSH loader 的 !!js schema 只注册 scalar kind，
    //   `!!js [数组].filter(...)`（flow 序列节点）必炸 unknown tag（08-24 persona 行潜伏雷，
    //   旧部署位未同步该版才未暴露；v2.1.125 修同步后引爆为「握手反复退出」）。主模板与部署位
    //   双侧扫描——只查部署位会漏「首次同步投毒」（SyncCordisFromMaster 在本函数之后的
    //   EnsureProcess 才跑）。
    for (const QString& sYml : QStringList{ sCordis, QCoreApplication::applicationDirPath()
                                                + QStringLiteral("/dsh/cordis.autocode.yml") }) {
        CFrwDevice oYml(sYml);
        if (!oYml.exists() || !oYml.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString sYmlText = QString::fromUtf8(oYml.readAll());
        oYml.close();
        // 逐行扫描且跳过 # 注释行（v2.1.128 自摆乌龙教训：修复版模板的注释里写了
        //   示例文本「!!js [数组]」被自家守卫命中——守卫只该看有效配置行）。
        bool bBadJs = false;
        for (const QString& sLine : sYmlText.split(QChar::LineFeed)) {
            const QString sTrimmed = sLine.trimmed();
            if (sTrimmed.startsWith(QLatin1Char('#'))) {
                continue;
            }
            if (sTrimmed.contains("!!js [") || sTrimmed.contains("!!js {")) {
                bBadJs = true;
                break;
            }
        }
        if (bBadJs) {
            return QStringLiteral(
                "组合 cordis.yml 含 DSH 不支持的 !!js 序列/映射表达式（!!js 仅支持标量，"
                "解析必炸 unknown tag）。\n"
                "文件：%1\n处理：更新程序目录 dsh\\cordis.autocode.yml（v2.1.127 已修复"
                "persona 行）后重开 AutoCode。").arg(QDir::toNativeSeparators(sYml));
        }
    }
    // 2026.09.08 v2.1.125 模板陈旧自检（实害：副本 08-18 旧模板缺 autocode-active 槽位 →
    //   initialize 报 -32603 no adapter registered for provider "autocode-active"）：激活自定义
    //   模型而 exe 侧 dsh\cordis.autocode.yml 不带槽位 = 部署目录未随主程序更新，前置拦截并给
    //   更新指引（不再等任务握手才炸）。
    if (CustomActiveValid()) {
        CFrwDevice oMasterYml(QCoreApplication::applicationDirPath()
                          + QStringLiteral("/dsh/cordis.autocode.yml"));
        if (oMasterYml.exists() && oMasterYml.open(QIODevice::ReadOnly)) {
            const bool bHasSlot = oMasterYml.readAll().contains("autocode-active");
            oMasterYml.close();
            if (!bHasSlot) {
                return QStringLiteral(
                    "程序目录的 dsh\\cordis.autocode.yml 是旧版（缺 autocode-active 槽位），"
                    "自定义大模型无法路由。\n"
                    "处理：从完整程序目录拷贝新的 dsh\\cordis.autocode.yml，或重跑"
                    "「01.更新程序.bat」同步后重启。");
            }
        }
    }
    // 2026.09.01 组合依赖包可达性（版本兼容探测，任务握手前拦截）：cordis.yml 引用的
    //   dsh-agent-spine-demo 是上游 dsh-v0.1.2 起移除的示例包（官方删包无别名）——
    //   DSH 被「后端版本管理」更新到该版本后，git reset 删 tracked 源、旧 bin.js 幸存，
    //   探测两件套（bin.js/cordis.yml）都还在，直到任务启动握手才 ERR_MODULE_NOT_FOUND
    //   反复退出（实测任务 3213cdbf 两次失败）。这里直接探组合树内的包入口文件，
    //   不可达即报不兼容并给回退命令。
    const QString sRootForProbe = DshRootPath();
    if (!sRootForProbe.isEmpty()) {
        const QString sSpinePkg = QDir::fromNativeSeparators(sRootForProbe) + QStringLiteral(
            "/examples/node_modules/@deepseek-ai/dsh-agent-spine-demo/package.json");
        if (!QFile::exists(sSpinePkg)) {
            return QStringLiteral(
                "DSH 版本与 AutoCode 集成不兼容：组合依赖的 "
                "@deepseek-ai/dsh-agent-spine-demo 不可达（上游 dsh-v0.1.2 起已移除"
                "全部示例包，多由「后端版本管理」更新到该版本导致）。\n"
                "处理：命令行运行 install_env.bat /update-dsh %1 \"%2\""
                "回退到受支持版本，完成后重试。")
                .arg(SupportedDshTag(), QDir::toNativeSeparators(sRootForProbe));
        }
    }
    return QString();
}

QString CDeepSeekRunner::ProbeSiblingRoot() {
    // install_env.bat 把 DSH 装在 exe 同级目录内的 DeepSeekHarness\deepseek-harness
    //   （绿色便携：随 Execute 程序目录整体拷走）；这里同布局探测（exe 同级/上级 ×
    //   两种目录名，exe 同级最优先；上两级为旧布局兼容回退），命中返回 canonical 根。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    const QStringList aNames = {
        QStringLiteral("/DeepSeekHarness/deepseek-harness"), QStringLiteral("/deepseek-harness")
    };
    for (const QString& sBase : aBases) {
        for (const QString& sName : aNames) {
            const QString sRoot = QDir(sBase + sName).canonicalPath();
            if (LooksLikeDshRoot(sRoot)) {
                return sRoot;
            }
        }
    }
    return QString();
}

QString CDeepSeekRunner::DshInstallDiagnosis() {
    // 半安装态诊断：探测布局同 ProbeSiblingRoot，但目录存在即停下来报缺项——
    //   bin.js 缺 = 克隆/构建没完成；cordis.yml 缺 = 程序目录没带 dsh\ 正本、
    //   组合文件没部署（旧版安装脚本此处只警告仍报「安装完成」，2026.08.18 已改硬错误）。
    const QString sExeDir = QCoreApplication::applicationDirPath();
    const QStringList aBases = {
        sExeDir, sExeDir + QStringLiteral("/.."), sExeDir + QStringLiteral("/../..")
    };
    const QStringList aNames = {
        QStringLiteral("/DeepSeekHarness/deepseek-harness"), QStringLiteral("/deepseek-harness")
    };
    for (const QString& sBase : aBases) {
        for (const QString& sName : aNames) {
            if (!QDir(sBase + sName).exists()) {
                continue;
            }
            const QString sRoot = QDir(sBase + sName).canonicalPath();
            // 2026.09.10 双布局缺项诊断：Legacy 看 bin.js/cordis.yml 两件套；Profile
            //   看 apps/cli 构建产物（git 只有 src，pnpm build 后才有 lib）。
            const bool bCliBin = QFileInfo::exists(
                sRoot + QString::fromLatin1(kSzDshCliBinRel));
            const bool bBin = bCliBin || QFileInfo::exists(
                sRoot + QString::fromLatin1(kSzDshBinJsRel));
            const bool bCordis = QFileInfo::exists(
                sRoot + QString::fromLatin1(kSzDshCordisRel));
            if (bBin && (bCordis || bCliBin)) {
                continue;   // 完整根（探测应已命中），继续看其他候选
            }
            QString sMissing;
            if (!bBin) {
                sMissing = QStringLiteral(
                    "bin.js（packages\\examples\\jsonrpc-demo\\lib\\ 或新版本 "
                    "apps\\cli\\lib\\ 下）——克隆或 pnpm 构建未完成，重跑「安装 "
                    "DeepSeekHarness…」续装");
            }
            if (!bCordis && !bCliBin) {
                if (!sMissing.isEmpty()) {
                    sMissing += QStringLiteral("；");
                }
                sMissing += QStringLiteral("cordis.yml（examples\\autocode-agent\\ 下）——程序目录缺 "
                                           "dsh\\cordis.autocode.yml，从完整程序目录拷来 dsh 子目录后"
                                           "重跑安装，或手动把该文件复制到上述路径");
            }
            return QStringLiteral("发现安装目录但未装全：%1\n缺：%2").arg(
                QDir::toNativeSeparators(sRoot), sMissing);
        }
    }
    return QStringLiteral("各探测位置（程序目录同级/上级）均无 DeepSeekHarness 安装目录——"
                          "克隆可能失败（网络/Git），请查看安装窗口的输出按提示处理后重装。");
}

// —— 2026.09 版本管理：DSH 本地版本与远端仓屿 URL（头文件注释为准） ——

QString CDeepSeekRunner::ResolveDshVersion(const QString& a_sRoot) {
    // 读 <root>/package.json 的 version；open 失败/解析错/无 version 字段→空串
    //   （＝「未知」，JSON 解析样板同 CCodexRunner::ResolveCodexVersion）。
    const QString sRoot = QDir::fromNativeSeparators(
        (a_sRoot.isEmpty() ? DshRootPath() : a_sRoot).trimmed());
    if (sRoot.isEmpty()) {
        return QString();
    }
    CFrwDevice stFile(sRoot + QStringLiteral("/package.json"));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QJsonParseError stErr;
    const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll(), &stErr);
    stFile.close();
    if (stErr.error != QJsonParseError::NoError || !oDoc.isObject()) {
        return QString();
    }
    return oDoc.object().value(QStringLiteral("version")).toString();
}

QString CDeepSeekRunner::DshRepoUrl() {
    // 与 install_env.bat 的 git clone 命令同一 URL（单一口径锚点；bat 侧更新段 fetch
    //   走 origin 远端不需要它，此处供 C++ 侧 ls-remote 最新版查询/提示共用）。
    return QStringLiteral("https://github.com/deepseek-ai/deepseek-harness.git");
}

QString CDeepSeekRunner::SupportedDshTag() {
    // 2026.09.01 见头文件注释：AutoCode 集成绑定的基线 tag（回退/安装命令用）。
    // 2026.09.09 v2.1.134 回退越权改动：工作区曾被某 Agent 任务未记档抬至 0.1.5-alpha.1
    //   （"双布局支持"半成品，与本机 rc.7 部署矛盾且破坏上限锁测试）。
    return QStringLiteral("dsh-v0.1.0-rc.7");
}

QVersionNumber CDeepSeekRunner::SupportedDshVersion() {
    // 数值比较基线（0.1.2-alpha.3 的 fromString=0.1.2 > 0.1.0 判"超基线"；
    //   rc.7 == 0.1.0 不高于自身）。适配新版后同步抬升两常量。
    return QVersionNumber::fromString(QStringLiteral("0.1.0"));
}

// ===== 2026.09.01 DSH 环境自愈：纯函数与文件修复 =====

namespace {
// 剥 YAML 标量首尾成对引号（单/双）。
QString StripQuotes(const QString& a_s) {
    QString s = a_s.trimmed();
    while (s.size() >= 2
           && ((s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
            || (s.startsWith(QLatin1Char('\'')) && s.endsWith(QLatin1Char('\''))))) {
        s = s.mid(1, s.size() - 2).trimmed();
    }
    return s;
}
}   // namespace

bool CDeepSeekRunner::ParseWrappedCredentials(const QString& a_sText,
                                               QStringList* a_pOutPairs) {
    // 仅识别已知坏格式（version/refs 包装，DSH credentials-local 期望纯平铺）：
    //   形态 A（flow map，单行）：refs: { K: v, K2: v2 }
    //   形态 B（块式缩进）：refs:\n  K: v\n  K2: v2
    // 平铺/其它格式一律返回 false（不动文件，避免误伤合法凭据）。
    if (a_pOutPairs != nullptr) {
        a_pOutPairs->clear();
    }
    const QStringList aLines = a_sText.split(QLatin1Char('\n'));
    bool bWrapped = false;
    QString sFlow;
    int iBlockStart = -1;
    int iRefsLine = -1;
    for (int i = 0; i < aLines.size(); ++i) {
        const QString sT = aLines[i].trimmed();
        if (sT.startsWith(QStringLiteral("version:"))) {
            bWrapped = true;
        } else if (sT.startsWith(QStringLiteral("refs:"))) {
            bWrapped = true;
            iRefsLine = i;
            const QString sRest = sT.mid(QStringLiteral("refs:").size()).trimmed();
            if (sRest.startsWith(QLatin1Char('{'))) {
                sFlow = sRest;
            } else if (!sRest.isEmpty()) {
                return false;   // refs: 后跟其它标量：不认识的形态，不动
            } else {
                iBlockStart = i + 1;
            }
        }
    }
    if (!bWrapped) {
        return false;
    }
    // 形态 A 变体（实测备份文件形态）：refs: 下一行起的缩进 flow map，可跨行到 '}'。
    //   收集 refs: 行之后到 EOF 的文本统一找 {…}，块式分支不再吃掉它。
    if (sFlow.isEmpty() && iRefsLine >= 0) {
        QString sCollected;
        for (int i = iRefsLine + 1; i < aLines.size(); ++i) {
            sCollected += aLines[i].trimmed() + QLatin1Char(' ');
        }
        const int iOpen = sCollected.indexOf(QLatin1Char('{'));
        if (iOpen >= 0) {
            sFlow = sCollected;
        }
    }
    QStringList aPairs;
    const auto fnAddPair = [&aPairs](const QString& a_sK, const QString& a_sV) {
        const QString sK = StripQuotes(a_sK);
        const QString sV = StripQuotes(a_sV);
        if (!sK.isEmpty() && !sV.isEmpty()) {
            aPairs.append(sK + QLatin1Char('\t') + sV);
        }
    };
    if (!sFlow.isEmpty()) {
        // 形态 A：{ K: v, K2: v2, ... }（右大括号可能同行收尾）
        const int iOpen = sFlow.indexOf(QLatin1Char('{'));
        const int iClose = sFlow.lastIndexOf(QLatin1Char('}'));
        if (iOpen < 0 || iClose <= iOpen) {
            return false;
        }
        const QStringList aItems = sFlow.mid(iOpen + 1, iClose - iOpen - 1)
                                       .split(QLatin1Char(','));
        for (const QString& sItem : aItems) {
            const int iColon = sItem.indexOf(QLatin1Char(':'));
            if (iColon <= 0) {
                continue;
            }
            fnAddPair(sItem.left(iColon), sItem.mid(iColon + 1));
        }
    } else if (iBlockStart >= 0) {
        // 形态 B：refs: 之后的缩进行 K: v（退到顶格/EOF 停）
        for (int i = iBlockStart; i < aLines.size(); ++i) {
            const QString& sRaw = aLines[i];
            if (sRaw.trimmed().isEmpty()) {
                break;   // 空行收尾
            }
            const bool bIndent = sRaw.startsWith(QLatin1Char(' '))
                              || sRaw.startsWith(QLatin1Char('\t'));
            const QString sT2 = sRaw.trimmed();
            const int iColon = sT2.indexOf(QLatin1Char(':'));
            if (!bIndent || iColon <= 0) {
                break;   // 顶格或无冒号：refs 块结束
            }
            fnAddPair(sT2.left(iColon), sT2.mid(iColon + 1));
        }
    } else {
        return false;   // 只有 version 无 refs：没有可迁移内容
    }
    if (aPairs.isEmpty()) {
        return false;
    }
    if (a_pOutPairs != nullptr) {
        *a_pOutPairs = aPairs;
    }
    return true;
}

QString CDeepSeekRunner::RenderFlatCredentials(const QStringList& a_aPairs) {
    QString sOut;
    for (const QString& sP : a_aPairs) {
        const int iTab = sP.indexOf(QLatin1Char('\t'));
        if (iTab <= 0) {
            continue;
        }
        sOut += sP.left(iTab) + QStringLiteral(": ") + sP.mid(iTab + 1)
              + QLatin1Char('\n');
    }
    return sOut;
}

int CDeepSeekRunner::HealCredentialsFile(QString* a_pOutWhy) {
    // 预检预修：StartStage 前置（任务/助手统一入口）调用。文件不存在→0（DSH 自己
    //   会生成/或走 env 阶梯）；平铺或解析不出的格式→0（不动）；已知坏格式→备份
    //   .autosave（覆盖旧备份）+ 改写平铺 →1。凭据值逐对原样迁移，不重排不丢失。
    const QString sPath = QDir::home().filePath(QStringLiteral(".dsh/.credentials.yaml"));
    CFrwDevice stFile(sPath);
    if (!stFile.exists()) {
        if (a_pOutWhy != nullptr) {
            *a_pOutWhy = QStringLiteral("文件不存在");
        }
        return 0;
    }
    if (!stFile.open(QIODevice::ReadOnly)) {
        if (a_pOutWhy != nullptr) {
            *a_pOutWhy = QStringLiteral("无法读取");
        }
        return 0;
    }
    const QString sText = QString::fromUtf8(stFile.readAll());
    stFile.close();
    QStringList aPairs;
    if (!ParseWrappedCredentials(sText, &aPairs)) {
        if (a_pOutWhy != nullptr) {
            *a_pOutWhy = QStringLiteral("非已知坏格式，未改动");
        }
        return 0;
    }
    const QString sBak = sPath + QStringLiteral(".autosave");
    if (QFile::exists(sBak)) {
        QFile::remove(sBak);
    }
    if (!QFile::copy(sPath, sBak)) {
        if (a_pOutWhy != nullptr) {
            *a_pOutWhy = QStringLiteral("备份失败");
        }
        return 0;
    }
    QFile stWrite(sPath);
    if (!stWrite.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (a_pOutWhy != nullptr) {
            *a_pOutWhy = QStringLiteral("写入失败");
        }
        return 0;
    }
    stWrite.write(RenderFlatCredentials(aPairs).toUtf8());
    stWrite.close();
    if (a_pOutWhy != nullptr) {
        *a_pOutWhy = QStringLiteral("已从 version/refs 包装迁移为平铺（原文件备份于 %1）")
                         .arg(sBak);
    }
    return 1;
}

QStringList CDeepSeekRunner::ComputeHollowDirs(const QString& a_sRoot,
                                                const QString& a_sLsFilesOut) {
    // 空壳目录：packages/*/*、vendor/*、apps/cli 候选中不含任何 tracked 文件的目录。
    //   背景：DSH 被更新到高版本后回退（git reset --hard 基线），高版本新增包的
    //   tracked 文件被删、只乘 node_modules 残壳——tsdown workspace glob 匹配到
    //   空壳、无 package.json 回溯到根包名报 Cannot find entry 卡死 build:lib
    //   （实测 0.1.2-alpha.3 → rc.7 回退后 40 个空壳目录）。返回绝对路径（可直接删）。
    QSet<QString> setHaveTracked;   // 含 tracked 文件（含自身被直接跟踪）的相对目录
    const QStringList aLines = a_sLsFilesOut.split(QLatin1Char('\n'));
    for (const QString& sLine : aLines) {
        const QString sRel = sLine.trimmed();
        if (sRel.isEmpty()) {
            continue;
        }
        int ix = sRel.lastIndexOf(QLatin1Char('/'));
        while (ix > 0) {
            setHaveTracked.insert(sRel.left(ix));
            ix = sRel.lastIndexOf(QLatin1Char('/'), ix - 1);
        }
    }
    QStringList aHollow;
    const QDir stRoot(a_sRoot);
    const auto fnCheck = [&setHaveTracked, &aHollow, &stRoot](const QString& a_sRelDir) {
        if (!setHaveTracked.contains(a_sRelDir)) {
            aHollow.append(stRoot.filePath(a_sRelDir));
        }
    };
    const QString sPkgs = QStringLiteral("packages");
    const QDir stPkgs(stRoot.filePath(sPkgs));
    const QStringList aTiers = stPkgs.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sTier : aTiers) {
        if (sTier.startsWith(QLatin1Char('.'))) {
            continue;
        }
        const QDir stTier(stPkgs.filePath(sTier));   // 2026.09.01 绝对路径（相对 cwd 枚举恒空）
        const QStringList aSubs = stTier.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sSub : aSubs) {
            if (sSub.startsWith(QLatin1Char('.'))) {
                continue;
            }
            fnCheck(sPkgs + QLatin1Char('/') + sTier + QLatin1Char('/') + sSub);
        }
    }
    const QDir stVendor(stRoot.filePath(QStringLiteral("vendor")));
    const QStringList aVendors = stVendor.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sV : aVendors) {
        if (!sV.startsWith(QLatin1Char('.'))) {
            fnCheck(QStringLiteral("vendor/") + sV);
        }
    }
    fnCheck(QStringLiteral("apps/cli"));
    return aHollow;
}

int CDeepSeekRunner::CleanBuildResidue(const QString& a_sRoot) {
    // 同步清理：packages 树内（跳过 node_modules）名为 lib 的目录 + *.tsbuildinfo。
    //   高版本构建产物与低版本源码混态会让 tsc -b 增量缓存（tsbuildinfo 残留而产物
    //   已删）跳过 emit、tsdown 找不到 entry——实测回退后必须清残留重全量构建。
    int iRemoved = 0;
    std::function<void(const QString&)> fnVisit = [&](const QString& a_sDir) {
        QDir stDir(a_sDir);
        const QFileInfoList aEntries = stDir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo& stE : aEntries) {
            if (stE.isDir()) {
                if (stE.fileName() == QStringLiteral("node_modules")) {
                    continue;
                }
                if (stE.fileName() == QStringLiteral("lib")) {
                    if (QDir(stE.absoluteFilePath()).removeRecursively()) {
                        ++iRemoved;
                    }
                    continue;
                }
                fnVisit(stE.absoluteFilePath());
            } else if (stE.fileName().endsWith(QStringLiteral(".tsbuildinfo"))) {
                if (QFile::remove(stE.absoluteFilePath())) {
                    ++iRemoved;
                }
            }
        }
    };
    fnVisit(QDir(a_sRoot).filePath(QStringLiteral("packages")));
    // 2026.09.10 补根目录 solution 级 tsbuildinfo（实测坑：根 tsconfig.host/client
    //   .tsbuildinfo 残留时 tsc -b 认为 host/client 面已构建、跳过包内 emit，
    //   tsdown 报 Cannot find entry 卡死 build；手工删包内 lib 不够，根残留必清）。
    QDir stRootDir(a_sRoot);
    const QFileInfoList aRootInfos = stRootDir.entryInfoList(
        {QStringLiteral("tsconfig*.tsbuildinfo")}, QDir::Files);
    for (const QFileInfo& stI : aRootInfos) {
        if (QFile::remove(stI.absoluteFilePath())) {
            ++iRemoved;
        }
    }
    return iRemoved;
}

void CDeepSeekRunner::SetDshNodePath(const QString& a_sPath) {
    s_sDshNodePath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshNodePath() {
    return s_sDshNodePath;
}

void CDeepSeekRunner::SetDshEffort(const QString& a_sLevel) {
    s_sDshEffort = a_sLevel.trimmed().toLower();
}

QString CDeepSeekRunner::DshEffort() {
    return s_sDshEffort;
}

void CDeepSeekRunner::SetDshBinJsPath(const QString& a_sPath) {
    s_sDshBinJsPath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshBinJsPath() {
    return s_sDshBinJsPath;
}

void CDeepSeekRunner::SetDshCordisPath(const QString& a_sPath) {
    s_sDshCordisPath = a_sPath.trimmed();
}

QString CDeepSeekRunner::DshCordisPath() {
    return s_sDshCordisPath;
}

void CDeepSeekRunner::SetDshProvider(const QString& a_sProvider) {
    s_sDshProvider = a_sProvider.trimmed().isEmpty() ? QStringLiteral("glm-custom")
                                                     : a_sProvider.trimmed();
}

QString CDeepSeekRunner::DshProvider() {
    return s_sDshProvider.isEmpty() ? QStringLiteral("glm-custom") : s_sDshProvider;
}

void CDeepSeekRunner::SetDshModel(const QString& a_sModel) {
    s_sDshModel = a_sModel.trimmed().isEmpty() ? QStringLiteral("glm-5.2")
                                               : a_sModel.trimmed();
}

QString CDeepSeekRunner::DshModel() {
    return s_sDshModel.isEmpty() ? QStringLiteral("glm-5.2") : s_sDshModel;
}

void CDeepSeekRunner::SetDshPriceInPerM(double a_d) {
    s_dDshPriceInPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceCacheReadPerM(double a_d) {
    s_dDshPriceCacheReadPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceCacheWritePerM(double a_d) {
    s_dDshPriceCacheWritePerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetDshPriceOutPerM(double a_d) {
    s_dDshPriceOutPerM = qMax(0.0, a_d);
}

void CDeepSeekRunner::SetCustomModels(const QVector<SCustomModel>& a_aModels, int a_iActive) {
    // 2026.08.19 热生效：先比「影响冷启动 env 的签名」（激活条目全字段，无激活=空串）。
    //   变化才重启空闲进程——设置页反复「确定」或只改非激活条目时不动既有会话。
    const int iNewActive = (a_iActive >= 0 && a_iActive < a_aModels.size()) ? a_iActive : -1;
    QString sNewSig;
    if (iNewActive >= 0) {
        const SCustomModel& stM = a_aModels.at(iNewActive);
        sNewSig = stM.sDisp + QChar(1) + stM.sBaseURL + QChar(1) + stM.sApiKey + QChar(1)
            + stM.sApi + QChar(1) + stM.sModelId + QChar(1) + QString::number(stM.iCtx)
            + QChar(1) + QString::number(stM.iMaxTok);
    }
    const bool bCfgChanged = (sNewSig != s_sCustomBootSig);
    s_aCustomModels = a_aModels;
    s_iCustomActive = iNewActive;
    s_sCustomBootSig = sNewSig;
    if (!bCfgChanged) {
        return;
    }
    for (CDeepSeekRunner* pR : s_aLiveRunners) {
        if (pR != nullptr) {
            pR->RestartForCfgChange();
        }
    }
}

void CDeepSeekRunner::RestartForCfgChange() {
    // 空闲（无轮次/非握手/非重试等待）→ 立即静默杀进程（kill 引发的 finished 由
    //   m_bTurnReported 吞掉，同 Stop 口径；sid 集随 MarkProcDead 作废，下轮换新 sid）。
    //   忙碌 → 只挂脏标，FinishTurn 落幕再杀（本轮结果完整送达，下一轮新配置冷启动）。
    if (m_eRunState == RunPrompting || m_eProcState == ProcBooting || m_bRetryPending) {
        m_bCfgDirty = true;
        m_iCfgDirtyGen = m_iProcGen;   // 记当代进程，收尾时只杀它
        return;
    }
    m_bCfgDirty = false;
    m_bTurnReported = true;   // 吞 kill 的 finished（下次 StartStage/ResetTurnState 复位）
    if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
    MarkProcDead();
}

bool CDeepSeekRunner::CustomActiveValid() {
    return s_iCustomActive >= 0 && s_iCustomActive < s_aCustomModels.size();
}

CDeepSeekRunner::CDeepSeekRunner(QObject* a_pParent)
    : CAgentRunnerBase(a_pParent) {
    s_aLiveRunners.insert(this);   // 2026.08.19 热生效：登记活实例（dtor 摘除）
    m_pProc = new QProcess(this);
    connect(m_pProc, &QProcess::readyReadStandardOutput, this, &CDeepSeekRunner::slotOnStdOut);
    // stderr 逐行上抛落 run.log（[stderr] 前缀）+ 尾部保留（失败消息附带）——同 claude 口径。
    connect(m_pProc, &QProcess::readyReadStandardError, this, &CDeepSeekRunner::slotOnStdErr);
    connect(m_pProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CDeepSeekRunner::slotOnFinished);
    connect(m_pProc, &QProcess::errorOccurred, this, &CDeepSeekRunner::slotOnErrorOccurred);
    // 停滞巡检定时器：常驻创建、Prompting 时武装（30s 一拍，超 StallMinutes 判挂起）。
    m_pStallTimer = new QTimer(this);
    m_pStallTimer->setInterval(30000);
    connect(m_pStallTimer, &QTimer::timeout, this, [this]() {
        CheckStall();
    });
}

CDeepSeekRunner::~CDeepSeekRunner() {
    s_aLiveRunners.remove(this);
    Stop();
}

QString CDeepSeekRunner::NewSid() {
    // DSH 对 sid 无格式要求（探针用 autocode-probe-<hex>）；加 dsh- 前缀便于在
    // 任务元数据里一眼区分后端来源。
    return QStringLiteral("dsh-") + QUuid::createUuid().toString(QUuid::Id128);
}

void CDeepSeekRunner::ResetTurnState() {
    // 轮次统计清零（StartStage/InjectUserTurn 开头调用）。进程/会话/模型/窗口缓存
    //   是跨轮稳定量，不在此清（长驻设计）。
    m_bTurnReported = false;
    m_bTurnEnded = false;
    m_bRetryPending = false;
    m_iBootRetry = 0;
    m_iFastRetry = 0;
    m_sTurnText.clear();
    m_sTurnReasonKind.clear();
    m_sTurnErrInfo.clear();   // 2026.09.10 上轮错误详情不复用
    m_iSumInTok = 0;
    m_iSumOutTok = 0;
    m_iLiveEmitIn = 0;    // 2026.08.27 实时计量已发累计复位
    m_iLiveEmitOut = 0;
    m_iSumPlainInTok = 0;
    m_iSumCacheReadTok = 0;
    m_iSumCacheWriteTok = 0;
    m_sLastSubtype.clear();
    m_iLastReqCount = 0;
    m_iLastContextFill = 0;
    m_aStderrTail.clear();
    m_buf.clear();
}

double CDeepSeekRunner::ComputeTurnCost() const {
    // 轮内 token 分量 × 单价折算（$/M）：DSH 协议只报 token 不报金额（与 claude CLI
    //   自带 total_cost_usd 不同），同一网关下按对齐价目估算；单价全 0 → 0（费用列回 "-"）。
    return (static_cast<double>(m_iSumPlainInTok) * s_dDshPriceInPerM
          + static_cast<double>(m_iSumCacheReadTok) * s_dDshPriceCacheReadPerM
          + static_cast<double>(m_iSumCacheWriteTok) * s_dDshPriceCacheWritePerM
          + static_cast<double>(m_iSumOutTok) * s_dDshPriceOutPerM) / 1000000.0;
}

void CDeepSeekRunner::StartStage(const QString& a_sWorkDir,
                                 const QString& a_sPrompt,
                                 const QString& a_sDisallowedTools,
                                 int a_iMaxTurns,
                                 const QString& a_sResumeSid,
                                 bool a_bFork,
                                 int a_iAutocompactK,
                                 bool a_bEnableAskUser) {
    ResetTurnState();

    // claude 专属参数降级提示（仅非默认值出现时提示一次，默认路径零噪音）。
    if (!a_sDisallowedTools.isEmpty() || a_iMaxTurns != 30 || a_iAutocompactK > 0
        || a_bEnableAskUser) {
        emit sigInfo(QStringLiteral(
            "[system/dsh] claude 专属参数（disallowedTools/maxTurns/autocompact/ask_user）"
            "在 DSH 后端不适用，已忽略（组合内置 compaction 自动压缩、工具默认放行）"));
    }

    // 启动前校验：缺失直接报失败（带完整处理指引，不拉进程）。统一走 DshConfigError，
    //   与启动警告/设置保存兜底同一口径——用户看到的第一条消息就说清怎么处理。
    const QString sCfgErr = DshConfigError();
    if (!sCfgErr.isEmpty()) {
        // 2026.09.01 版本不兼容自愈：组合包缺失（多为「后端版本管理」更新到
        //   dsh-v0.1.2+ 所致）→ 自动回滚基线+重建（每进程限一次，失败回落人工指引），
        //   修复期间占用运行态防重复提交，完成后自动重入本函数继续原阶段。
        if (sCfgErr.contains(QStringLiteral("版本与 AutoCode 集成不兼容"))
            && m_iHealAttempts == 0) {
            ++m_iHealAttempts;
            SHealArgs stArgs;
            stArgs.sWorkDir = a_sWorkDir;
            stArgs.sPrompt = a_sPrompt;
            stArgs.sDisallowedTools = a_sDisallowedTools;
            stArgs.sResumeSid = a_sResumeSid;
            stArgs.iMaxTurns = a_iMaxTurns;
            stArgs.bFork = a_bFork;
            stArgs.iAutocompactK = a_iAutocompactK;
            stArgs.bEnableAskUser = a_bEnableAskUser;
            BeginSelfHeal(stArgs);
            return;
        }
        EmitFailed(sCfgErr);
        return;
    }
    // 2026.09.01 credentials 坏格式预检（秒级）：version/refs 包装 → 自动迁移平铺
    //   （备份 .autosave）。不动其它格式，失败留握手报错带指引。
    QString sWhy;
    if (HealCredentialsFile(&sWhy) == 1) {
        emit sigInfo(QStringLiteral("[system/dsh] 已自动修复 ~/.dsh/.credentials.yaml"
                                    " 格式（%1），继续启动。")
                         .arg(sWhy));
    }

    // 会话判定：resumeSid 在本进程创建过且非 fork → 复用（同进程多轮=协议内续跑）；
    //   否则换新 sid（跨进程恢复是协议限制，上下文由上层 prompt 回灌弥补）。
    if (!a_sResumeSid.isEmpty() && !a_bFork
        && m_aLiveSids.contains(a_sResumeSid)
        && m_eProcState == ProcReady) {
        m_sSessionId = a_sResumeSid;
    } else {
        if (!a_sResumeSid.isEmpty() && !m_bCtxNoticeShown) {
            m_bCtxNoticeShown = true;
            // 2026.09.02 改 [ctx/dsh] 前缀穿透上层 [system 过滤（此前被静默吞掉，
            //   用户无法感知每轮失忆）；每实例只发一次。上下文回灌由编排器
            //   StartAdhocStage 注入最近对话记录（KeepsContextAcrossTurns()==false）。
            emit sigInfo(QStringLiteral(
                "[ctx/dsh] DSH 协议不支持跨轮会话续接（fork/跨进程均换新会话），"
                "上下文由 AutoCode 按轮回灌最近对话记录（resumeSid=%1 已忽略）")
                .arg(a_sResumeSid));
        }
        m_sSessionId = NewSid();
        m_aLiveSids.insert(m_sSessionId);
    }
    m_sLastSessionId = m_sSessionId;
    m_sPromptWorkDir = a_sWorkDir;
    m_sPendingPrompt = a_sPrompt;
    m_eRunState = RunPrompting;   // 冷启动窗口也算占用（IsRunnerBusy 立即可见）

    if (m_eProcState == ProcReady) {
        BeginPrompt(m_sPendingPrompt);
        return;
    }
    EnsureProcess(a_sWorkDir);   // 失败走 EmitFailed（m_bTurnReported 守卫放行）
}

// ===== 2026.09.01 版本不兼容自动修复流水线 =====

void CDeepSeekRunner::BeginSelfHeal(const SHealArgs& a_stArgs) {
    // 入口前置：StartStage 已判「版本不兼容」且首次修复。占用运行态（IsRunnerBusy
    //   可见，防修复中重复提交）；原阶段参数缓存，修复成功后自动重入。
    m_stHealArgs = a_stArgs;
    m_bSelfHealing = true;
    m_eRunState = RunPrompting;
    m_sPromptWorkDir = a_stArgs.sWorkDir;
    m_bTurnReported = false;
    m_bTurnEnded = false;
    emit sigInfo(QStringLiteral(
        "🔧 检测到 DSH 版本与 AutoCode 集成不兼容（组合依赖的示例包不可达，多为"
        "更新到 dsh-v0.1.2+ 所致）。启动自动修复：回滚基线 %1 + 清理残留 + 依赖重建，"
        "预计 5~15 分钟，完成后自动继续本阶段…")
            .arg(SupportedDshTag()));

    // 多实例互斥锁：<root>/.autocode-heal.lock 独占创建。已存在且 60 分钟内→
    //   另一实例正在修复（放弃，人工稍后重试）；陈旧锁→接管（删除重建）。
    const QString sRoot = DshRootPath();
    m_sHealLockPath = QDir(sRoot).filePath(QStringLiteral(".autocode-heal.lock"));
    if (m_pHealLock != nullptr) {
        m_pHealLock->close();
        delete m_pHealLock;
        m_pHealLock = nullptr;
    }
    QFileInfo stLockInfo(m_sHealLockPath);
    if (stLockInfo.exists()
        && stLockInfo.lastModified().msecsTo(QDateTime::currentDateTime())
               < 60LL * 60 * 1000) {
        FinishHeal(false, QStringLiteral(
            "另一 AutoCode 实例正在修复 DSH（锁文件 %1 未过期）。请等待其完成后"
            "重试本阶段。").arg(m_sHealLockPath));
        return;
    }
    if (stLockInfo.exists()) {
        QFile::remove(m_sHealLockPath);   // 陈旧锁接管
    }
    auto* pLock = new QFile(m_sHealLockPath, this);
    if (!pLock->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        delete pLock;
        FinishHeal(false, QStringLiteral("无法创建修复锁文件 %1（权限？），"
                                         "请手动修复。").arg(m_sHealLockPath));
        return;
    }
    pLock->write(QStringLiteral("pid=%1 time=%2")
                     .arg(QCoreApplication::applicationPid())
                     .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                     .toUtf8());
    pLock->flush();
    m_pHealLock = pLock;
    HealRunStep(1);
}

void CDeepSeekRunner::HealRunStep(int a_iStep) {
    // 串行状态机：1=git reset 基线 2=git ls-files（退出后同步删空壳+清残留）
    //   3=pnpm install 4=build:lib 5=复检。每步独立超时；stdout/stderr 分开收。
    m_iHealStep = a_iStep;
    m_sHealStdout.clear();
    m_sHealErrTail.clear();
    if (m_pHealProc == nullptr) {
        m_pHealProc = new QProcess(this);
        connect(m_pHealProc,
                static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(
                    &QProcess::finished),
                this, &CDeepSeekRunner::OnHealFinished);
        connect(m_pHealProc, &QProcess::readyReadStandardOutput, this, [this]() {
            m_sHealStdout.append(QString::fromUtf8(
                m_pHealProc->readAllStandardOutput()));
            if (m_sHealStdout.size() > 8 * 1024 * 1024) {
                m_sHealStdout = m_sHealStdout.right(4 * 1024 * 1024);   // ls-files 大仓八挂上限
            }
        });
        connect(m_pHealProc, &QProcess::readyReadStandardError, this, [this]() {
            const QStringList aNew = QString::fromUtf8(
                m_pHealProc->readAllStandardError()).split(QLatin1Char('\n'));
            for (const QString& sL : aNew) {
                if (!sL.trimmed().isEmpty()) {
                    m_sHealErrTail = sL.trimmed();   // 只留最后一行非空（失败消息用）
                }
            }
        });
    }
    if (m_pHealTimer == nullptr) {
        m_pHealTimer = new QTimer(this);
        m_pHealTimer->setSingleShot(true);
        connect(m_pHealTimer, &QTimer::timeout, this, [this]() {
            if (m_bSelfHealing && m_pHealProc != nullptr
                && m_pHealProc->state() != QProcess::NotRunning) {
                emit sigInfo(QStringLiteral("⚠ 自动修复步骤 %1 超时，终止并转手动指引…")
                                 .arg(m_iHealStep));
                m_pHealProc->kill();   // → OnHealFinished(-1)
            }
        });
    }
    const QString sRoot = DshRootPath();
    const int kiStepTimeoutMs[] = {0, 60 * 1000, 60 * 1000,
                                   15 * 60 * 1000, 30 * 60 * 1000, 30 * 1000};
    QProcessEnvironment stEnv = QProcessEnvironment::systemEnvironment();
    // 2026-09-12 手工审批弹窗通道（自愈重建后重入 StartStage，环境同口径）
    InjectAskEnv(stEnv);
    stEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    stEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    stEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    stEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    m_pHealProc->setProcessEnvironment(stEnv);
    m_pHealProc->setWorkingDirectory(sRoot);
    m_pHealProc->setProcessChannelMode(QProcess::SeparateChannels);
    switch (a_iStep) {
        case 1:
            emit sigInfo(QStringLiteral("🔧 [1/5] git 回滚基线 %1（丢弃 DSH 仓库内"
                                        "未提交改动）…").arg(SupportedDshTag()));
            m_pHealProc->start(QStringLiteral("git"),
                { QStringLiteral("reset"), QStringLiteral("--hard"),
                  SupportedDshTag() });
            break;
        case 2:
            emit sigInfo(QStringLiteral("🔧 [2/5] 检测残留空壳目录与构建产物…"));
            m_pHealProc->start(QStringLiteral("git"),
                { QStringLiteral("ls-files") });
            break;
        case 3:
            emit sigInfo(QStringLiteral("🔧 [3/5] pnpm install（依赖对齐基线，约 1~5 分钟）…"));
            m_pHealProc->start(QStringLiteral("cmd"),
                { QStringLiteral("/c"), QStringLiteral("pnpm install") });
            break;
        case 4:
            // 2026.09.10 build:lib → build：0.1.5 起 lib 依赖 native-system 先行，
            //   单跑 build:lib 必挂（Cannot find entry）；官方完整流程含 native+lib+web。
            emit sigInfo(QStringLiteral("🔧 [4/5] pnpm run build（完整重建，约 5~15 分钟）…"));
            m_pHealProc->start(QStringLiteral("cmd"),
                { QStringLiteral("/c"), QStringLiteral("pnpm run build") });
            break;
        default:
            FinishHeal(false, QStringLiteral("内部错误：未知修复步骤 %1").arg(a_iStep));
            return;
    }
    m_pHealTimer->start(kiStepTimeoutMs[a_iStep]);
}

void CDeepSeekRunner::OnHealFinished(int a_iExitCode) {
    if (!m_bSelfHealing) {
        return;   // 迟到信号（Stop 已收尾）：忽略
    }
    m_pHealTimer->stop();
    const int iStep = m_iHealStep;
    if (a_iExitCode != 0) {
        FinishHeal(false, QStringLiteral("步骤 %1 失败（退出码=%2）：%3")
                             .arg(iStep)
                             .arg(a_iExitCode)
                             .arg(m_sHealErrTail.isEmpty()
                                      ? QStringLiteral("（无 stderr 输出）")
                                      : m_sHealErrTail));
        return;
    }
    switch (iStep) {
        case 1:
            HealRunStep(2);
            return;
        case 2: {
            // 同步清理：空壳目录（高版本残壳，卡死 tsdown 的根因）+ 构建残留
            //   （lib/tsbuildinfo 混态，卡死 tsc -b 增量）。
            const QStringList aHollow = ComputeHollowDirs(DshRootPath(), m_sHealStdout);
            int iRm = 0;
            for (const QString& sDir : aHollow) {
                if (QDir(sDir).removeRecursively()) {
                    ++iRm;
                }
            }
            const int iResidue = CleanBuildResidue(DshRootPath());
            emit sigInfo(QStringLiteral("🔧 已清理 %1 个空壳目录、%2 项构建残留")
                             .arg(iRm).arg(iResidue));
            HealRunStep(3);
            return;
        }
        case 3:
            HealRunStep(4);
            return;
        case 4: {
            // 复检：配置错误清零才算修好（组合包可达 + 其余探测项）
            const QString sErr = DshConfigError();
            if (!sErr.isEmpty()) {
                FinishHeal(false, QStringLiteral("重建完成但校验仍报错：%1").arg(sErr));
                return;
            }
            FinishHeal(true, QString());
            return;
        }
        default:
            FinishHeal(false, QStringLiteral("内部错误：未知完成步骤 %1").arg(iStep));
    }
}

void CDeepSeekRunner::FinishHeal(bool a_bOk, const QString& a_sMsg) {
    // 终态统一收口：解锁 → 复位修复态 → 成功重入 StartStage / 失败 EmitFailed（带
    //   手动回退命令）。锁文件始终删除（失败也允许下次/其它实例重试）。
    if (m_pHealTimer != nullptr) {
        m_pHealTimer->stop();
    }
    if (m_pHealProc != nullptr && m_pHealProc->state() != QProcess::NotRunning) {
        m_pHealProc->kill();
        m_pHealProc->waitForFinished(3000);
    }
    if (m_pHealLock != nullptr) {
        m_pHealLock->close();
        m_pHealLock->deleteLater();
        m_pHealLock = nullptr;
    }
    if (!m_sHealLockPath.isEmpty()) {
        QFile::remove(m_sHealLockPath);
    }
    m_bSelfHealing = false;
    m_iHealStep = 0;
    if (a_bOk) {
        emit sigInfo(QStringLiteral("✅ DSH 自动修复完成，继续执行原阶段…"));
        const SHealArgs stArgs = m_stHealArgs;
        m_eRunState = RunIdle;   // 重入 StartStage 自己重占
        StartStage(stArgs.sWorkDir, stArgs.sPrompt, stArgs.sDisallowedTools,
                   stArgs.iMaxTurns, stArgs.sResumeSid, stArgs.bFork,
                   stArgs.iAutocompactK, stArgs.bEnableAskUser);
        return;
    }
    emit sigInfo(QStringLiteral("⚠ DSH 自动修复中止：%1").arg(a_sMsg));
    EmitFailed(QStringLiteral(
        "DSH 自动修复失败：%1\n"
        "手动修复：命令行运行 install_env.bat /update-dsh %2 \"%3\""
        "（失败重跑即修复），完成后重试本阶段。")
                   .arg(a_sMsg, SupportedDshTag(),
                        QDir::toNativeSeparators(DshRootPath())));
}

bool CDeepSeekRunner::EnsureProcess(const QString& a_sWorkDir) {
    // 2026-10-07 对话级模型覆盖：起步条目 = 会话覆盖下标（per-runner 实例槽，右键
    //   「选择模型」仅本助手生效）优先，缺省全局激活。env 槽位按起步条目烘焙；
    //   保活进程的 env 是启动时快照 → 起步条目身份变化须杀旧冷启（同 cwd 守卫先例）。
    const int iOvM = ModelOvIdxIn(s_aCustomModels.size());
    const int iUseM = (iOvM >= 0) ? iOvM : s_iCustomActive;
    const bool bUseCustom = (iUseM >= 0 && iUseM < s_aCustomModels.size());
    QString sWantModelKey = QStringLiteral("fixed");
    if (bUseCustom) {
        const SCustomModel& stW = s_aCustomModels.at(iUseM);
        SDshModelEntry stId;
        stId.sBaseURL = stW.sBaseURL;
        stId.sApi     = stW.sApi;
        stId.sModelId = stW.sModelId;
        sWantModelKey = ModelEntryIdentity(stId);
    }
    if (m_eProcState == ProcBooting || m_eProcState == ProcReady) {
        // 2026.08.20 cwd 热切换守卫：进程是旧工程 cwd 时不可复用——agent 的相对路径写
        //   （.autodev/tasks/<id>/intake.md 等）会落进旧工程，新工程校验产物永远缺失，
        //   造成「多次回退仍未生成 intake.md」级联暂停（实例：扑克任务产物写进扫雷）。
        // 2026-10-07 模型热切换守卫：env 槽位随起步条目变（覆盖设置/清除、全局激活
        //   变化）时同样不可复用——旧进程仍连旧模型，与本轮目标不一致。
        if ((m_sProcWorkDir.isEmpty() ||
             QDir::cleanPath(m_sProcWorkDir) == QDir::cleanPath(a_sWorkDir))
            && m_sProcModelKey == sWantModelKey) {
            return true;
        }
        if (m_pProc->state() != QProcess::NotRunning) {
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
        }
        m_eProcState = ProcDead;
        // 落到下方冷启动路径：以新工程 cwd 重新拉进程 + 握手。
    }
    // 部署位组合文件对账（2026.09.10 仅 Legacy 布局：Profile 布局的组合层是
    //   --patch 绝对路径直接引用 exe 目录正本，无部署副本概念）：模板（exe 同级
    //   dsh\cordis.autocode.yml）有更新而部署副本还是旧的，冷启动前覆盖。
    if (DshLayoutOf() == EDshLayout::Legacy) {
        SyncCordisFromMaster();
    }
    // 收尾旧进程（Dead 残留状态清理；正常路径进程已退出）。
    if (m_pProc->state() != QProcess::NotRunning) {
        if (!m_pProc->waitForFinished(3000)) {
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
        }
    }
    // 环境注入：DSH_CWD（bash/fs-local 工作目录）+ DSH_SESSION_ROOT（会话 jsonl 落盘，
    //   放工程 .autodev 下与任务产物同域，不污染工程根）。
    QProcessEnvironment oEnv = QProcessEnvironment::systemEnvironment();
    // 2026-09-12 手工审批弹窗通道（AUTOCODE_ASK_EXE/ASK_PIPE，全后端统一）
    InjectAskEnv(oEnv);
    // 2026.08.22 中文编码根治（同 CCodexRunner）：子进程全链路 UTF-8，防 GBK 乱码/挂起。
    oEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    oEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    oEnv.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    oEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    oEnv.insert(QStringLiteral("DSH_CWD"), a_sWorkDir);
    oEnv.insert(QStringLiteral("DSH_SESSION_ROOT"),
                a_sWorkDir + QStringLiteral("/.autodev/dsh-sessions"));
    // 2026.08.18 自定义大模型：激活条目注入 llm-pi-ai 的 env 槽位（组合 cordis.yml 的
    //   autocode-active provider 逐字段读这些变量；密钥值走 credentials-local 的 env
    //   阶梯最优先级）。未启用（表空/Active 无效）时不设——组合回落 ?? 兜底默认值。
    if (bUseCustom) {   // 2026-10-07 起按起步条目（覆盖优先）；冷启后记入 m_sProcModelKey（保活守卫用）
        const SCustomModel& stM = s_aCustomModels.at(iUseM);
        oEnv.insert(QStringLiteral("DSH_LLM_NAME"),
                    stM.sDisp.isEmpty() ? QStringLiteral("AutoCode Active Model") : stM.sDisp);
        oEnv.insert(QStringLiteral("DSH_LLM_BASE_URL"), stM.sBaseURL);
        oEnv.insert(QStringLiteral("DSH_LLM_API"),
                    stM.sApi.isEmpty() ? QStringLiteral("anthropic-messages") : stM.sApi);
        oEnv.insert(QStringLiteral("DSH_LLM_MODEL"), stM.sModelId);
        oEnv.insert(QStringLiteral("DSH_LLM_CTX"), QString::number(stM.iCtx));
        oEnv.insert(QStringLiteral("DSH_LLM_MAXTOK"), QString::number(stM.iMaxTok));
        oEnv.insert(QStringLiteral("AUTOCODE_DSH_API_KEY"), stM.sApiKey);
        // 2026.09.10 连接类失败定位（用户实测：本地 ollama 停服 → "Connection error" 裸文本
        //   无目标地址，远端/本地不分排查一圈）：记录本进程实际 LLM 目标，失败提示时呈现。
        m_sLlmBaseUrl = stM.sBaseURL;
        m_sLlmLabel   = stM.sDisp.isEmpty() ? stM.sModelId : stM.sDisp;
        if (stM.iCtx > 0) {
            m_iCtxWindow = stM.iCtx;   // 窗口种子（request/context 上报后覆盖）
        }
    } else {
        // 固定路由（settings.yaml 的 provider 定义）：URL 在 dsh 侧，AutoCode 只知名字。
        m_sLlmBaseUrl.clear();
        m_sLlmLabel = DshModel();
    }
    // 2026.08.24 便携资产注入（三后端统一口径，见 CAgentRunnerBase 注释）：
    //   技能目录 = <exe>\skills（便携）+ ~/.claude/skills（存在者），';' 连接经
    //   DSH_SKILLS_DIRS 传入组合 yml（customSkillDirs 按分隔符拆分）；
    //   全局规则 = <exe>\CLAUDE.md 或 ~/.claude/CLAUDE.md 内容，经 DSH_GLOBAL_CONTEXT
    //   并入 persona。全程不向用户主目录写任何文件（便携部署目标）。
    const QStringList aSkillDirs = CAgentRunnerBase::SharedSkillDirs();
    if (!aSkillDirs.isEmpty()) {
        oEnv.insert(QStringLiteral("DSH_SKILLS_DIRS"), aSkillDirs.join(QLatin1Char(';')));
    }
    const QString sGlobalCtx = CAgentRunnerBase::SharedGlobalContextContent();
    if (!sGlobalCtx.isEmpty()) {
        oEnv.insert(QStringLiteral("DSH_GLOBAL_CONTEXT"), sGlobalCtx);
        emit sigInfo(QStringLiteral("[system/dsh] 已注入全局规则（%1 字符）")
                         .arg(QString::number(sGlobalCtx.size())));
    }
    m_pProc->setProcessEnvironment(oEnv);
    m_pProc->setWorkingDirectory(a_sWorkDir);
    m_sProcWorkDir = a_sWorkDir;   // 2026.08.20 记录进程 cwd（EnsureProcess 工程切换检测用）
    m_sProcModelKey = sWantModelKey;   // 2026-10-07 记录 env 烘焙的模型身份（覆盖/全局切换检测用）
    ++m_iProcGen;   // 2026.08.19 进程代次 +1（热生效守卫：脏标只杀配置变更前的旧进程）
    m_eProcState = ProcBooting;
    m_uptime.restart();
    m_actClock.restart();
    // node 直起（不经 cmd.exe：参数化传参，无 shim 引号重解析问题；Windows 路径含
    //   空格安全）。stdin 全程不关（铁律：bin.js 对 stdin end 敏感）。
    // 2026.09.10 双布局参数：Profile = dsh CLI --profile sdk --patch <exe组合层>；
    //   Legacy = {bin, cordis} 两件套（见 ResolveStartArgs）。
    m_pProc->start(ResolveNodePath(), ResolveStartArgs());
    if (!m_pProc->waitForStarted(10000)) {
        m_eProcState = ProcDead;
        const QStringList aArgs = ResolveStartArgs();
        EmitFailed(QStringLiteral("无法启动 DSH 进程（%1 布局）：node %2")
                       .arg(DshLayoutOf() == EDshLayout::Profile
                                ? QStringLiteral("官方 SDK profile")
                                : QStringLiteral("示例包"),
                            aArgs.join(QLatin1Char(' '))));
        return false;
    }
    // initialize 握手：cwd/provider/model（maxTokens 不传——组合 maxTokensAsSuccess=false，
    //   token 封顶以 turn/end reason=max-tokens 上报，续跑语义靠它）。
    //   2026.09.27 额外可写目录自愈授 ACE（幂等；见 ApplyExtraWriteDirs）——pwsh 沙箱
    //   的写强制是纯 NTFS DACL，会话 cwd（=工程根）派生的写 SID 被授到额外目录即放行。
    ApplyExtraWriteDirs(a_sWorkDir, DshExtraWriteDirs());
    m_iInitId = m_iNextRpcId++;
    QJsonObject oParams;
    oParams.insert(QStringLiteral("cwd"), a_sWorkDir);
    oParams.insert(QStringLiteral("provider"), EffectiveProvider());
    oParams.insert(QStringLiteral("model"), EffectiveModel());
    // 2026-10-08 思考强度（SDK initialize reasoningEffort；off/low/high/max；
    //   服务端校验非空字符串，空=不传走 adapter 默认）。
    if (!s_sDshEffort.isEmpty()) {
        oParams.insert(QStringLiteral("reasoningEffort"), s_sDshEffort);
    }
    SendRequest(QStringLiteral("initialize"), oParams, m_iInitId);
    // 握手超时 30s：重试一次（重启进程），再失败判死。重启由本 lambda 独占——
    //   m_bRebooting 拦住 kill 触发的 slotOnFinished Booting 重试分支（防双重重启）。
    QTimer::singleShot(30000, this, [this]() {
        if (m_eProcState != ProcBooting || m_bTurnReported) {
            return;
        }
        if (m_iBootRetry >= 1) {
            EmitFailed(QStringLiteral("DSH initialize 握手超时（30 秒×2），进程已终止。"
                                      "请检查组合文件与网络（中继 %1）").arg(DshProvider()));
            return;
        }
        ++m_iBootRetry;
        emit sigInfo(QStringLiteral("↻ DSH initialize 握手超时（30 秒），重启进程重试…"));
        m_bRebooting = true;
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
        m_bRebooting = false;
        MarkProcDead();
        EnsureProcess(m_sPromptWorkDir);
    });
    return true;
}

QString CDeepSeekRunner::EffectiveProvider() const {
    // 自定义模型启用 → 固定发组合 yml 的 env 槽位名；否则走 [Agent] DshProvider。
    return CustomActiveValid() ? QStringLiteral("autocode-active") : DshProvider();
}

QString CDeepSeekRunner::EffectiveModel() const {
    return CustomActiveValid() ? s_aCustomModels.at(s_iCustomActive).sModelId : DshModel();
}

void CDeepSeekRunner::SyncCordisFromMaster() {
    // 模板（exe 同级 dsh\cordis.autocode.yml，构建从仓库 agent/dsh 同步）与部署副本
    //   （ResolveCordisPath）逐字节比对，不同则覆盖部署副本。部署副本是生成物（头注释
    //   标明正本位置），覆盖安全；只读部署目录等失败仅提示不阻断（旧副本仍可跑默认路由）。
    const QString sMaster = QCoreApplication::applicationDirPath()
        + QStringLiteral("/dsh/cordis.autocode.yml");
    const QString sDeployed = ResolveCordisPath();
    if (sMaster.isEmpty() || sDeployed.isEmpty() || !QFile::exists(sMaster)) {
        return;
    }
    CFrwDevice oMaster(sMaster);
    if (!oMaster.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray aMaster = oMaster.readAll();
    oMaster.close();
    // 2026.09.13 文件读取统一走 CFrwDevice（fileread.dll）；该对象随后还要写开，
    //   读/写拆两个对象（CFrwDevice 只读组件，拒绝写标志）。
    CFrwDevice oDeployedRd(sDeployed);
    if (oDeployedRd.open(QIODevice::ReadOnly)) {
        const QByteArray aOld = oDeployedRd.readAll();
        oDeployedRd.close();
        if (aOld == aMaster) {
            return;   // 已同步
        }
        // 2026.09.08 v2.1.125 防降级守卫（实害：副本 dsh\cordis.autocode.yml 停在
        //   08-18 无槽位版——01.更新程序.bat 只同步 exe/bat/help 不同步 dsh\，冷启动
        //   把部署位覆盖回旧版 → DSH initialize 报 -32603 no adapter registered for
        //   provider "autocode-active"，任务阶段直接失败）：exe 侧模板缺 autocode-active
        //   槽位而部署副本已带 → 跳过覆盖（宁用部署位新模板，不拿旧模板降级）。
        if (!aMaster.contains("autocode-active") && aOld.contains("autocode-active")) {
            emit sigInfo(QStringLiteral("[system/dsh] 程序目录模板缺 autocode-active 槽位"
                                        "（疑似旧部署未同步），保留部署位现有模板：%1")
                             .arg(QDir::toNativeSeparators(sMaster)));
            return;
        }
    }
    QFile oDeployedWr(sDeployed);
    if (oDeployedWr.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        oDeployedWr.write(aMaster);
        oDeployedWr.close();
        emit sigInfo(QStringLiteral("[system/dsh] 组合文件已从程序目录模板刷新：%1")
                         .arg(QDir::toNativeSeparators(sDeployed)));
    } else {
        emit sigInfo(QStringLiteral("[system/dsh] 组合文件模板有更新但部署副本写入失败"
                                    "（可能只读）：%1").arg(QDir::toNativeSeparators(sDeployed)));
    }
}

void CDeepSeekRunner::SendRequest(const QString& a_sMethod, const QJsonObject& oParams,
                                  int a_iId) {
    if (m_pProc->state() == QProcess::NotRunning) {
        return;   // 进程已死：写入无意义（退出路径自会收尾）
    }
    QJsonObject oFrame;
    oFrame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    oFrame.insert(QStringLiteral("id"), a_iId > 0 ? a_iId : m_iNextRpcId++);
    oFrame.insert(QStringLiteral("method"), a_sMethod);
    if (!oParams.isEmpty()) {
        oFrame.insert(QStringLiteral("params"), oParams);
    }
    const QByteArray bufFrame = QJsonDocument(oFrame).toJson(QJsonDocument::Compact) + '\n';
    m_pProc->write(bufFrame);
    m_pProc->waitForBytesWritten(500);   // 及时冲出（铁律：绝不 closeWriteChannel）
}

void CDeepSeekRunner::SendPrompt(const QString& a_sText) {
    QJsonObject oBlock;
    oBlock.insert(QStringLiteral("type"), QStringLiteral("text"));
    oBlock.insert(QStringLiteral("text"), a_sText);
    QJsonArray aBlocks;
    aBlocks.append(oBlock);
    QJsonObject oParams;
    oParams.insert(QStringLiteral("sessionId"), m_sSessionId);
    oParams.insert(QStringLiteral("contentBlocks"), aBlocks);
    m_iPromptId = m_iNextRpcId++;   // 记下 id 供响应配对（受理即回，错误即失败）
    SendRequest(QStringLiteral("session/prompt"), oParams, m_iPromptId);
}

void CDeepSeekRunner::MarkProcDead() {
    // 进程死亡的统一落点：置 Dead + 清本进程 sid 集合。集合标记的是「当前活进程
    //   创建过的 sid」——新进程复用旧 sid 会被持久化层判 id collision（磁盘 jsonl
    //   不是新 live 会话的前缀），必须随进程死亡一并作废。
    m_eProcState = ProcDead;
    m_aLiveSids.clear();
}

void CDeepSeekRunner::BeginPrompt(const QString& a_sPrompt) {
    // 武装计时 + 投递（Ready 直发 / 握手后补发 / 注入轮三路径共用；文本显式传入）。
    m_actClock.restart();
    m_uptime.restart();
    if (m_pStallTimer != nullptr && !m_pStallTimer->isActive()) {
        m_pStallTimer->start();
    }
    SendPrompt(a_sPrompt);
}

void CDeepSeekRunner::Stop() {
    // 静默收尾（同 claude 语义）：先置终态再 kill——kill 触发的 finished/事件一律吞掉，
    //   不发 sigFinished；上层 StopTask 直接落 Stopped。DSH 无逐轮取消，只能杀进程；
    //   进程死后下轮 StartStage 冷启动换新 sid（协议限制，StartStage 内有提示）。
    //   2026.09.01 修复中停止：终止修复子进程并静默收尾（不发失败，锁文件释放）。
    m_bTurnReported = true;
    m_bRetryPending = false;
    m_eRunState = RunIdle;
    if (m_bSelfHealing) {
        if (m_pHealTimer != nullptr) {
            m_pHealTimer->stop();
        }
        if (m_pHealProc != nullptr && m_pHealProc->state() != QProcess::NotRunning) {
            m_pHealProc->kill();
            m_pHealProc->waitForFinished(3000);
        }
        if (m_pHealLock != nullptr) {
            m_pHealLock->close();
            m_pHealLock->deleteLater();
            m_pHealLock = nullptr;
        }
        if (!m_sHealLockPath.isEmpty()) {
            QFile::remove(m_sHealLockPath);
        }
        m_bSelfHealing = false;
        m_iHealStep = 0;
        emit sigInfo(QStringLiteral("🔧 DSH 自动修复已被用户停止（锁已释放，"
                                    "下次启动阶段会重新检测）"));
        return;
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    if (m_pProc->state() != QProcess::NotRunning) {
        m_pProc->kill();
        m_pProc->waitForFinished(3000);
    }
    MarkProcDead();
}

bool CDeepSeekRunner::InjectUserTurn(const QString& a_sText) {
    // DSH 协议无「轮次中插话」：仅在进程 Ready 且无轮次进行中时，作为新一轮
    //   session/prompt（同 sid 续轮）发出；否则安全 no-op 返回 false。
    if (a_sText.isEmpty() || m_eProcState != ProcReady || m_eRunState != RunIdle
        || m_sSessionId.isEmpty()) {
        return false;
    }
    ResetTurnState();
    m_eRunState = RunPrompting;
    BeginPrompt(a_sText);   // 2026.08.18 实测修复：必须显式传注入文本（曾误发上一轮 m_sPendingPrompt 旧值）
    return true;
}

void CDeepSeekRunner::slotOnStdOut() {
    m_actClock.restart();   // 收到输出 = 进程活着，停滞计时清零
    m_buf += m_pProc->readAllStandardOutput();
    int iIdx = 0;
    while ((iIdx = m_buf.indexOf('\n')) >= 0) {
        QByteArray sLine = m_buf.left(iIdx);
        m_buf = m_buf.mid(iIdx + 1);
        sLine = sLine.trimmed();
        if (!sLine.isEmpty()) {
            HandleLine(sLine);
        }
    }
}

void CDeepSeekRunner::slotOnStdErr() {
    m_actClock.restart();   // stderr 字节同样算活跃
    const QByteArray bufData = m_pProc->readAllStandardError();
    const QList<QByteArray> aLines = bufData.split('\n');
    for (const QByteArray& szRaw : aLines) {
        const QString sLine = QString::fromUtf8(szRaw).trimmed();
        if (sLine.isEmpty()) {
            continue;
        }
        m_aStderrTail.append(sLine);
        if (m_aStderrTail.size() > 15) {
            m_aStderrTail.removeFirst();
        }
        emit sigInfo(QStringLiteral("[stderr] ") + sLine);
    }
}

void CDeepSeekRunner::HandleLine(const QByteArray& a_sLine) {
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(a_sLine, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        emit sigInfo(QString::fromUtf8(a_sLine));   // 非 JSON 行原样上抛（排查用）
        return;
    }
    const QJsonObject oRoot = stDoc.object();
    // 响应（有 id 且含 result/error）与通知（有 method）分流。
    if (oRoot.contains(QStringLiteral("id"))
        && (oRoot.contains(QStringLiteral("result")) || oRoot.contains(QStringLiteral("error")))) {
        HandleResponse(oRoot.value(QStringLiteral("id")).toInt(), oRoot);
        return;
    }
    if (oRoot.contains(QStringLiteral("method"))) {
        HandleNotify(oRoot);
        return;
    }
    emit sigInfo(QString::fromUtf8(a_sLine));
}

void CDeepSeekRunner::HandleResponse(int a_iId, const QJsonObject& oRoot) {
    if (a_iId == m_iInitId) {
        // initialize 响应：error 说明组合/配置问题（重试无益，直接失败）。
        if (oRoot.contains(QStringLiteral("error"))) {
            const QJsonObject oErr = oRoot.value(QStringLiteral("error")).toObject();
            m_bRebooting = true;   // kill 触发的 slotOnFinished 走 Booting 重试分支，拦下
            m_pProc->kill();
            m_pProc->waitForFinished(3000);
            m_bRebooting = false;
            MarkProcDead();
            EmitFailed(QStringLiteral("DSH initialize 失败：%1 %2").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString(),
                oErr.value(QStringLiteral("message")).toString()));
            return;
        }
        m_eProcState = ProcReady;
        const QJsonObject oInfo = oRoot.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("serverInfo")).toObject();
        emit sigInfo(QStringLiteral("[system/dsh-ready] server=%1/%2 provider=%3 model=%4").arg(
            oInfo.value(QStringLiteral("name")).toString(),
            oInfo.value(QStringLiteral("version")).toString(),
            DshProvider(), DshModel()));
        // 冷启动路径：握手成功后补发暂存的 prompt（Ready 直发路径不走这里）。
        if (m_eRunState == RunPrompting && !m_sPendingPrompt.isEmpty()) {
            BeginPrompt(m_sPendingPrompt);
        }
        return;
    }
    if (a_iId == m_iPromptId) {
        // session/prompt 响应：仅代表「受理」；轮次完成靠事件流（turn/end + idle）。
        if (oRoot.contains(QStringLiteral("error"))) {
            const QJsonObject oErr = oRoot.value(QStringLiteral("error")).toObject();
            EmitFailed(QStringLiteral("DSH session/prompt 失败：%1 %2").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString(),
                oErr.value(QStringLiteral("message")).toString()));
        }
        return;
    }
    // 未知响应（不应出现）：上抛便于排查。
    emit sigInfo(QString::fromUtf8(QJsonDocument(oRoot).toJson(QJsonDocument::Compact)));
}

void CDeepSeekRunner::HandleNotify(const QJsonObject& oRoot) {
    const QString sMethod = oRoot.value(QStringLiteral("method")).toString();
    const QJsonObject oParams = oRoot.value(QStringLiteral("params")).toObject();
    if (sMethod == QStringLiteral("session.event")) {
        HandleSessionEvent(oParams);
        return;
    }
    if (sMethod == QStringLiteral("session.status")) {
        // idle 到达且 turn 已结束 → 收尾（idle 实测在 turn/end 后到达，trailing 事件已吃完）。
        const QString sStatus = oParams.value(QStringLiteral("status")).toString();
        if (sStatus == QStringLiteral("idle") && m_bTurnEnded) {
            FinishTurn();
        }
        return;
    }
    if (sMethod == QStringLiteral("subagent.started")
        || sMethod == QStringLiteral("subagent.finished")) {
        emit sigInfo(QStringLiteral("[system/%1] session=%2").arg(
            sMethod.section(QLatin1Char('.'), -1),
            oParams.value(QStringLiteral("sessionId")).toString()));
        return;
    }
    // 未知通知：原样上抛（协议容错——rc.7 无兼容承诺，未知 method 不崩）。
    emit sigInfo(QString::fromUtf8(QJsonDocument(oRoot).toJson(QJsonDocument::Compact)));
}

void CDeepSeekRunner::HandleSessionEvent(const QJsonObject& oParams) {
    // 只处理当前会话的事件（多 runner 共存时 DSH 各自独立进程，正常只会有自己 sid；
    //   防御性过滤仍保留——sid 不匹配直接忽略）。
    const QString sSid = oParams.value(QStringLiteral("sessionId")).toString();
    if (sSid != m_sSessionId) {
        return;
    }
    const QJsonObject oEvent = oParams.value(QStringLiteral("event")).toObject();
    const QString sType = oEvent.value(QStringLiteral("type")).toString();
    // payload 直接内联（阶段 0 实测无 data 包裹；带 data 键的形态亦兼容）。
    QJsonObject oData = oEvent.value(QStringLiteral("data")).toObject();
    if (oData.isEmpty()) {
        oData = oEvent;
    }

    if (sType == QStringLiteral("assistant/chunk")) {
        return;   // token 级流：量大且无复盘价值，源头丢弃（同 claude thinking_tokens 口径）
    }
    if (sType == QStringLiteral("assistant/message")) {
        ++m_iLastReqCount;   // 每条 assistant 消息 = 一次 API 调用（同 claude 口径）
        // usage：轮内 Σ 输入/输出（账单口径含 cache 读/写）；上下文占用取本条的
        //   input 三项之和（脏值 >2×窗口丢弃——中继偶发异常巨大 usage，claude 侧实测）。
        const QJsonObject oUsage = oData.value(QStringLiteral("usage")).toObject();
        if (!oUsage.isEmpty()) {
            const qint64 iPlainIn = oUsage.value(QStringLiteral("inputTokens")).toInteger();
            const qint64 iCacheRead = oUsage.value(QStringLiteral("cacheReadTokens")).toInteger();
            const qint64 iCacheWrite = oUsage.value(QStringLiteral("cacheWriteTokens")).toInteger();
            const qint64 iIn = iPlainIn + iCacheRead + iCacheWrite;
            const qint64 iOut = oUsage.value(QStringLiteral("outputTokens")).toInteger()
                + oUsage.value(QStringLiteral("reasoningTokens")).toInteger();
            m_iSumInTok += iIn;
            m_iSumOutTok += iOut;
            // 计费分量分开累计（三项单价不同，见 ComputeTurnCost）
            m_iSumPlainInTok += iPlainIn;
            m_iSumCacheReadTok += iCacheRead;
            m_iSumCacheWriteTok += iCacheWrite;
            // 2026.08.27 逐消息实时计量上抛（本轮 ↑↓ 显示）；收尾 sigFinished 据已发累计报 0 防双计。
            if (iIn > 0 || iOut > 0) {
                m_iLiveEmitIn += iIn;
                m_iLiveEmitOut += iOut;
                emit sigUsageDelta(iIn, iOut);
            }
            const qint64 iWinCap = 2 * (m_iCtxWindow > 0
                ? m_iCtxWindow : CClaudeRunner::ContextWindowForModel(m_sLastModel));
            if (iIn > 0 && iIn != m_iLastContextFill && iIn <= iWinCap) {
                m_iLastContextFill = iIn;
                emit sigContextFill(iIn);
            }
        }
        // 文本块上抛 + 记本轮最后文本（thinking 等其他块跳过）。
        const QJsonArray aContent = oData.value(QStringLiteral("message")).toObject()
                                        .value(QStringLiteral("content")).toArray();
        for (const QJsonValue& v : aContent) {
            const QJsonObject oItem = v.toObject();
            if (oItem.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                const QString sText = oItem.value(QStringLiteral("text")).toString();
                if (!sText.isEmpty()) {
                    m_sTurnText = sText;
                    emit sigAssistantText(sText);
                }
            }
        }
        return;
    }
    if (sType == QStringLiteral("tool/call")) {
        const QString sArgs = oData.value(QStringLiteral("arguments")).toString();
        emit sigToolUse(oData.value(QStringLiteral("name")).toString(),
                        sArgs.isEmpty() ? QStringLiteral("{}") : sArgs);
        return;
    }
    if (sType == QStringLiteral("tool/result")) {
        // message.content 兼容字符串 / [{type:text,text:...}] 两种形态；error 附加。
        const QJsonValue vContent = oData.value(QStringLiteral("message")).toObject()
                                        .value(QStringLiteral("content"));
        QString sResult;
        if (vContent.isString()) {
            sResult = vContent.toString();
        } else if (vContent.isArray()) {
            for (const QJsonValue& vc : vContent.toArray()) {
                sResult += vc.toObject().value(QStringLiteral("text")).toString();
            }
        }
        const QJsonObject oErr = oData.value(QStringLiteral("error")).toObject();
        if (!oErr.isEmpty()) {
            sResult += QStringLiteral(" [错误:%1]").arg(
                oErr.value(QStringLiteral("code")).toVariant().toString());
        }
        if (sResult.size() > 400) {
            sResult = sResult.left(400) + QStringLiteral("...");
        }
        emit sigToolResult(sResult);
        return;
    }
    if (sType == QStringLiteral("user/message")) {
        return;   // 用户轮回显（AutoCode 侧自知内容），忽略
    }
    if (sType == QStringLiteral("turn/end")) {
        m_bTurnEnded = true;
        const QJsonObject oReason = oData.value(QStringLiteral("reason")).toObject();
        m_sTurnReasonKind = oReason.value(QStringLiteral("kind")).toString();
        // 2026.09.10 失败可诊断性：error 类 reason 携带 reason.error{message,code}（如
        //   "Connection error."/TRANSPORT）——此前丢弃致 FinishTurn 只能报「无错误文本」。
        if (m_sTurnReasonKind != QStringLiteral("completed")
                && m_sTurnReasonKind != QStringLiteral("max-tokens")) {
            const QJsonObject oErr = oReason.value(QStringLiteral("error")).toObject();
            const QString sErrCode = oErr.value(QStringLiteral("code")).toString();
            const QString sErrMsg  = oErr.value(QStringLiteral("message")).toString();
            if (!sErrMsg.isEmpty()) {
                m_sTurnErrInfo = sErrCode.isEmpty()
                    ? sErrMsg : QStringLiteral("%1（code=%2）").arg(sErrMsg, sErrCode);
            }
        }
        // 完成判定严格门控 idle：turn/end 只说明轮次逻辑结束，server 内部收尾可能
        //   未完，此刻投递下一轮有竞态风险。idle 正常在 turn/end 后数秒内到达；
        //   5s 未到仅提示（idle 永不到的异常形态交 stall 巡检兜底 kill，不在此收尾）。
        QTimer::singleShot(5000, this, [this]() {
            if (m_bTurnEnded && m_eRunState == RunPrompting && !m_bTurnReported) {
                emit sigInfo(QStringLiteral(
                    "[system/dsh-idle-wait] turn/end 已到达 5 秒仍未见 idle，继续等待（stall 兜底）"));
            }
        });
        return;
    }
    if (sType == QStringLiteral("request/context")) {
        // provider/model/上下文窗口（比模型名猜测准，优先缓存）。
        const QString sProvider = oData.value(QStringLiteral("provider")).toString();
        const QString sModel = oData.value(QStringLiteral("model")).toString();
        if (!sModel.isEmpty()) {
            m_sLastModel = sProvider.isEmpty() ? sModel
                                               : sProvider + QLatin1Char('/') + sModel;
        }
        const qint64 iWin = oData.value(QStringLiteral("contextWindow")).toInteger();
        if (iWin > 0) {
            m_iCtxWindow = iWin;
        }
        return;
    }
    // 未知事件：[system/<type>] 上抛（不上屏只落日志的口径与 claude system 行一致）。
    emit sigInfo(QStringLiteral("[system/dsh-%1] session=%2").arg(sType, sSid));
}

void CDeepSeekRunner::FinishTurn() {
    if (m_eRunState != RunPrompting || m_bTurnReported || !m_bTurnEnded) {
        return;   // 幂等守卫：idle 与 5s 兜底双入口只收一次；turn/end 未到不收尾
    }
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    m_eRunState = RunIdle;
    m_bTurnReported = true;
    const QString sKind = m_sTurnReasonKind;
    bool bOk = false;
    QString sSub;
    QString sMsg = m_sTurnText;
    if (sKind == QStringLiteral("completed")) {
        bOk = true;
        sSub = QStringLiteral("success");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("(本轮无文本输出)");
        }
    } else if (sKind == QStringLiteral("max-tokens")) {
        // 复用 claude 词汇：上层 LastWasMaxTurns() → 自动以同 sid 再 StartStage =
        // 同进程续轮（DSH 强项：上下文完整保留）。
        sSub = QStringLiteral("error_max_turns");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("达到输出 token 上限（max-tokens），转自动续跑");
        }
    } else if (sKind == QStringLiteral("aborted")) {
        sSub = QStringLiteral("aborted");
        if (sMsg.trimmed().isEmpty()) {
            sMsg = QStringLiteral("轮次被中止（aborted）");
        }
    } else {
        sSub = sKind.isEmpty() ? QStringLiteral("error") : sKind;
        if (sMsg.trimmed().isEmpty()) {
            // 2026.09.10 turn/end 的 error 详情（Connection error/TRANSPORT 等）优先于占位文案。
            sMsg = m_sTurnErrInfo.trimmed().isEmpty()
                 ? QStringLiteral("无错误文本（turn/end reason=%1）").arg(sSub)
                 : m_sTurnErrInfo;
        }
        // 2026.09.10 连接类失败定位：追加实际 LLM 目标——本地端口未监听（重试秒败特征）
        //   时给出显式指引，不再让「本地服务没开」伪装成「网络故障」。
        if (sMsg.contains(QStringLiteral("Connection error"), Qt::CaseInsensitive)
                || sMsg.contains(QStringLiteral("TRANSPORT"))
                || sMsg.contains(QStringLiteral("ECONNREFUSED"))) {
            const QString sTarget = m_sLlmBaseUrl.isEmpty()
                ? QStringLiteral("固定路由 %1（定义见 ~/.dsh/settings.yaml）").arg(DshProvider())
                : m_sLlmBaseUrl;
            const bool bLocal = m_sLlmBaseUrl.startsWith(QStringLiteral("http://127.0.0.1"))
                             || m_sLlmBaseUrl.startsWith(QStringLiteral("http://localhost"));
            sMsg = QStringLiteral("%1；LLM 目标：%2（模型：%3）%4")
                       .arg(sMsg, sTarget,
                            m_sLlmLabel.isEmpty() ? DshModel() : m_sLlmLabel,
                            bLocal ? QStringLiteral("——本地地址连不上，请确认模型服务已启动"
                                     "（如 ollama/spark），或在设置中切换 DSH 模型条目")
                                   : QString());
        }
    }
    m_sLastSubtype = sSub;
    emit sigFinished(bOk, sMsg,
                     (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                     (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                     ComputeTurnCost(), m_sSessionId);
    // 2026.08.19 配置热生效：轮次中收到自定义模型变更（RestartForCfgChange 挂的脏标），
    //   本轮已完整送达，落幕后杀旧进程——下一轮以新配置冷启动（免重启 AutoCode）。
    //   两类竞态守卫：①emit 期间编排器已冷启动新代进程（=新配置）→ 不动；②emit 期间
    //   编排器在旧进程上同步续跑了下一轮 → 保留脏标，那一轮落幕后（下次 FinishTurn）再杀。
    if (m_bCfgDirty) {
        if (m_iProcGen != m_iCfgDirtyGen) {
            m_bCfgDirty = false;   // 已是新代进程（新配置冷启动过），无事可做
        } else if (m_eRunState == RunPrompting || m_eProcState == ProcBooting) {
            ;   // 旧进程已续跑新一轮：不杀（打断本轮），脏标留到下次 FinishTurn
        } else {
            m_bCfgDirty = false;
            if (m_pProc != nullptr && m_pProc->state() != QProcess::NotRunning) {
                m_pProc->kill();
                m_pProc->waitForFinished(3000);
                MarkProcDead();
            }
        }
    }
}

void CDeepSeekRunner::CheckStall() {
    // 停滞检测（同 claude 语义）：进程活着但距上次收到任何输出超 [Agent]
    //   StallMinutes（默认 10 分钟）→ 判挂起，kill 进程并以 subtype="stall" 报失败，
    //   上层凭 lastSessionId 自动续跑（DSH 侧进程已死 → 换新 sid，StartStage 有提示）。
    if (m_eRunState != RunPrompting || m_bTurnReported || m_bRetryPending) {
        return;   // 无轮次 / 已收尾 / 重试等待中
    }
    if (m_pProc->state() == QProcess::NotRunning) {
        return;   // 进程已退出，交给 finished 路径
    }
    const qint64 iLimitMs = static_cast<qint64>(CClaudeRunner::StallMinutes()) * 60000;
    if (m_actClock.elapsed() < iLimitMs) {
        return;
    }
    if (m_pStallTimer != nullptr) {
        m_pStallTimer->stop();
    }
    m_sLastSubtype = QStringLiteral("stall");
    emit sigInfo(QStringLiteral("⚠ 停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），"
                                "终止 DSH 进程并转自动续跑…").arg(CClaudeRunner::StallMinutes()));
    // 先置终态再 kill（静默吞掉后续事件，同 Stop）。
    m_bTurnReported = true;
    m_eRunState = RunIdle;
    m_pProc->kill();
    m_pProc->waitForFinished(3000);
    MarkProcDead();
    emit sigFinished(false,
                     QStringLiteral("停滞检测：%1 分钟无任何输出（疑似网络/中继挂起），进程已终止")
                         .arg(CClaudeRunner::StallMinutes()),
                     m_iSumInTok, m_iSumOutTok, ComputeTurnCost(), m_sSessionId);
}

void CDeepSeekRunner::slotOnFinished(int a_iExitCode, QProcess::ExitStatus a_eStatus) {
    Q_UNUSED(a_eStatus);
    const EProcState ePrev = m_eProcState;
    MarkProcDead();
    if (m_bTurnReported) {
        return;   // Stop/stall/已收尾：静默吞掉
    }
    if (m_bRebooting) {
        return;   // 握手超时/init 错误路径的主动重启：重启由发起方独占，此处静默
    }
    if (ePrev == ProcBooting) {
        // 握手阶段自行退出：进程参数/组合加载问题——重启重试一次，再失败判死。
        if (m_iBootRetry < 1) {
            ++m_iBootRetry;
            emit sigInfo(QStringLiteral("↻ DSH 进程在握手阶段退出（退出码=%1），重启重试…").arg(a_iExitCode));
            QTimer::singleShot(5000, this, [this]() {
                if (!m_bTurnReported) {
                    EnsureProcess(m_sPromptWorkDir);
                }
            });
            return;
        }
        QString sBootFailMsg = QStringLiteral(
            "DSH 进程在握手阶段反复退出（退出码=%1），"
            "请检查 node/bin.js/cordis.yml 路径与组合文件").arg(a_iExitCode);
        // 2026.09.01 组合包缺失自含真因（EmitFailed 虽附 stderr 尾部，但 node 堆栈
        //   对用户不可读）：Cannot find package = 组合引用的包不可达，多为 DSH 被更新
        //   到 dsh-v0.1.2+（上游移除示例包）——直接给回退命令，不再让用户对着堆栈猜。
        const QString sTailJoined = m_aStderrTail.join(QLatin1Char('|'));
        if (sTailJoined.contains(QStringLiteral("Cannot find package"))) {
            sBootFailMsg += QStringLiteral(
                "\n根因：组合依赖的包缺失（ERR_MODULE_NOT_FOUND）——DSH 版本与 AutoCode "
                "集成不兼容（dsh-v0.1.2+ 移除了示例包）。\n"
                "处理：命令行运行 install_env.bat /update-dsh %1 \"<DSH 根目录>\" "
                "回退到受支持版本。")
                .arg(SupportedDshTag());
        }
        EmitFailed(sBootFailMsg);
        return;
    }
    if (m_eRunState != RunPrompting) {
        return;   // Idle 时的意外退出：不属本轮执行，下轮 StartStage 冷启动自愈
    }
    // Prompting 中退出且未收 turn/end：快速失败自动重试（同 claude——短命退出多为
    //   瞬态故障，5s 后重启进程 + 重发 prompt；上限 2 次）。
    if (m_uptime.isValid() && m_uptime.elapsed() < 15000 && m_iFastRetry < 2) {
        ++m_iFastRetry;
        m_bRetryPending = true;
        emit sigInfo(QStringLiteral("DSH 进程 %1 秒内即退出（退出码=%2），5 秒后自动重试（第 %3/2 次）")
                         .arg(m_uptime.elapsed() / 1000.0, 0, 'f', 1)
                         .arg(a_iExitCode)
                         .arg(m_iFastRetry));
        QTimer::singleShot(5000, this, [this]() {
            RelaunchStage();
        });
        return;
    }
    EmitFailed(QStringLiteral("DSH 进程异常退出，退出码=%1").arg(a_iExitCode));
}

void CDeepSeekRunner::RelaunchStage() {
    // 快速失败重试：重启进程 + 握手 + 重发 prompt（期间被 Stop/新一轮 StartStage 则作废）。
    m_bRetryPending = false;
    if (m_bTurnReported || m_sPendingPrompt.isEmpty()) {
        return;
    }
    emit sigInfo(QStringLiteral("↻ 自动重试 DSH 调用（第 %1/2 次）…").arg(m_iFastRetry));
    m_sLastSubtype.clear();
    EnsureProcess(m_sPromptWorkDir);   // 握手响应到达后自动 BeginPrompt 补发
}

void CDeepSeekRunner::slotOnErrorOccurred(QProcess::ProcessError a_eError) {
    if (m_bTurnReported) {
        return;
    }
    QString sMsg;
    switch (a_eError) {
        case QProcess::FailedToStart: sMsg = QStringLiteral("DSH 启动失败（node 路径无效或无执行权限）"); break;
        case QProcess::Crashed:       sMsg = QStringLiteral("DSH 进程崩溃"); break;
        case QProcess::Timedout:      sMsg = QStringLiteral("DSH 进程超时"); break;
        case QProcess::ReadError:     sMsg = QStringLiteral("读取 DSH 输出错误"); break;
        case QProcess::WriteError:    sMsg = QStringLiteral("写入 DSH 输入错误"); break;
        default:                      sMsg = QStringLiteral("DSH 进程未知错误"); break;
    }
    // FailedToStart 不会触发 finished 信号，须在此直接收尾；其余错误等 finished 路径
    //   （CrashExit 会在 slotOnFinished 里走快速重试/EmitFailed）。
    if (a_eError == QProcess::FailedToStart) {
        m_eProcState = ProcDead;
        EmitFailed(sMsg);
    }
}

void CDeepSeekRunner::EmitFailed(const QString& a_sMsg) {
    if (m_bTurnReported) {
        return;
    }
    // 失败消息附带 stderr 尾部 + sid（上层失败后仍可续跑）——同 claude 口径。
    QString sMsg = a_sMsg;
    if (!m_aStderrTail.isEmpty()) {
        QString sTail = m_aStderrTail.join(QStringLiteral(" | "));
        if (sTail.size() > 800) {
            sTail = sTail.left(800) + QStringLiteral("…");
        }
        sMsg += QStringLiteral("；stderr 尾部：%1").arg(sTail);
    }
    m_bTurnReported = true;
    m_eRunState = RunIdle;
    if (m_pStallTimer != nullptr && m_pStallTimer->isActive()) {
        m_pStallTimer->stop();
    }
    emit sigInfo(QStringLiteral("[错误] ") + sMsg);
    emit sigFinished(false, sMsg,
                     (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumInTok,
                     (m_iLiveEmitIn > 0 || m_iLiveEmitOut > 0) ? 0 : m_iSumOutTok,
                     ComputeTurnCost(), m_sSessionId);
}
