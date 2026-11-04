MsbSandbox 私有目录
====================

microsandbox（Linux microVM）沙箱的私有依赖与状态目录：

  MsbSandbox.dll            沙箱本体（Rust cdylib，C ABI 见 AutoCode
                            sandbox/SandboxAbi.h；源码 msbsandbox/）
  MsbSandbox.pdb            调试符号
  MsbSandbox\
    runtime\bin\msb.exe     microVM 宿主进程（SDK spawn 的子进程）
    runtime\bin\libkrunfw.dll
                            内核副本（msb.exe 静态导入 libkrunfw.dll，
                            bin 旁必须有，否则 spawn 即 0xC0000135）
    runtime\lib\libkrunfw.dll
                            guest Linux 内核（SDK 配置引用位）
    runtime\home\           MSB_HOME（运行期生成：沙箱登记库/镜像缓存/
                            卷/日志——可整目录删除重置）
    README.txt              本说明

构建产物归置（与 GhostSandbox 同口径，全部只落 TestSandbox\）：
  cmake --build build --target MsbSandbox
  = cargo 构建（中间产物固定走 E:\msb-target 短路径——长路径下 cc-rs 调
    MSVC lib.exe 报 1104）→ dll/pdb 部署到本目录 → ~/.microsandbox 的
    runtime 自动同步到 runtime\（首次 cargo 构建时 SDK 自动安装到
    ~/.microsandbox；bin\libkrunfw.dll 副本随同步生成）。

运行前提：
  - Windows「虚拟机监控程序平台」（HypervisorPlatform）已启用并重启。
    **msb.exe 静态导入 winhvplatform.dll**——未启用的机器上 msb.exe
    启动即 0xC0000135（属预期；启用后系统补出该 DLL 自愈）。
    启用（管理员 PowerShell，需重启）：
    Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
  - 首次 vm_exec 拉 OCI 镜像（默认 alpine，约 5MB）需联网；之后离线可用
    （缓存在 runtime\home\）。

命令面（sandbox 通道）：status / version / vm_exec / vm_stop /
  vm_write / vm_read；GUI 命令一律 not-supported（GUI 测试请用
  GhostSandbox）。
