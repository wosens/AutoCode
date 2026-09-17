// filereadwrite.h —— 宿主接入唯一头文件（纯 C 接口；动态加载：LoadLibrary("fileread.dll") +
// GetProcAddress，不需要 .lib 导入库。x64 下调用约定被编译器忽略，见 design §5.4
// https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170 ）
//
// 组件形态：fileread.dll + code.exe（读文件服务进程，DLL 首次调用自动拉起）。
// 语义细则（与 doc/usage.md 同源）：
// | 规则           | 定义 |
// | 动态加载       | GetProcAddress 名称：FRW_OpenW / FRW_Read / FRW_Close（extern "C"、
// |                | x64 无名字修饰）；宿主须保持 DLL 加载至全部调用返回（FreeLibrary 时机约束） |
// | 单次上限       | FRW_Read 的 a_ullLength > 100*1024*1024 → FRW_ERR_LENGTH_EXCEEDED，
// |                | DLL 本地拒绝、不发起 IPC，不静默截断；恰好等于 100MB 合法（一次调用成功） |
// | 读取边界       | a_ullOffset ≥ 文件大小 → FRW_ERR_EOF（"读到头"的正常信号）；
// |                | 区间跨越文件尾 → 成功且短读（*a_pullRead < 请求长度）；分块循环以短读为终止条件 |
// | EOF 判定基准   | exe 侧每次 READ 实时 GetFileSizeEx 现取文件大小（读时刻磁盘内容语义）；
// |                | OpenW 返回的 size 为 OPEN 时刻快照，仅供调用方参考，不用于 EOF 判定 |
// | 参数非法       | 长度 0 / 空指针 / a_ullBufCap < a_ullLength → FRW_ERR_INVALID_PARAM |
// | 路径限制       | 空路径 → FRW_ERR_INVALID_PARAM；路径 ≥ 1024 宽字符 → FRW_ERR_PATH_TOO_LONG
// |                | （合法上限 1023：协议槽位路径字段容量 1024 含结尾 NUL） |
// | 悬空句柄       | exe 已空闲退出/死亡 → DLL 透明恢复（≤1 次）：重拉 exe、按句柄内路径重开、
// |                | 重试本次读一次；仍失败才返回实际错误码 |
// | 数据一致性     | 返回内容 = 读时刻磁盘文件对应区间的逐字节内容 |
// | FRW_Close      | 不拉起 exe（exe 不在则仅本地清理）；exe 存活且繁忙时最坏走
// |                | "槽位等待 5s + 完成等待 5s" ≈ 10s，尽力而为语义 |
// | 线程安全       | 3 个函数可多线程并发调用；同一句柄的并发调用在句柄锁上排队，不同句柄完全并行 |
#ifndef FRW_H_INCLUDED
#define FRW_H_INCLUDED

typedef void* FRWHANDLE;   // 不透明句柄，仅由本 API 创建/使用

// ---- 函数指针原型（与 DLL 导出符号一一对应；GetProcAddress 名称见各注释） ----

// 打开文件（只读）。a_pullSize 可为 NULL；返回 FRW 错误码，0 = 成功
typedef int (__stdcall* PFN_FRW_OpenW)(const wchar_t* a_wsPath, FRWHANDLE* a_phFile,
                                       unsigned long long* a_pullSize);   // "FRW_OpenW"

// 按偏移读一块。a_ullLength ≤ 100MB；a_ullBufCap ≥ a_ullLength；
// *a_pullRead 返回实际读到字节数（尾部短读 < 请求长度属正常成功）
typedef int (__stdcall* PFN_FRW_Read)(FRWHANDLE a_hFile, unsigned long long a_ullOffset,
                                      unsigned long long a_ullLength, void* a_pvBuf,
                                      unsigned long long a_ullBufCap,
                                      unsigned long long* a_pullRead);   // "FRW_Read"

// 关闭句柄（本地即刻释放；服务端表项尽力通知，恒返回 0 除非句柄非法）
typedef int (__stdcall* PFN_FRW_Close)(FRWHANDLE a_hFile);   // "FRW_Close"

#endif // FRW_H_INCLUDED
