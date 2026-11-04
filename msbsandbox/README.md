# MsbSandbox —— AutoCode 测试沙箱（microsandbox / Linux microVM）

Rust SDK → cdylib 动态库，按 AutoCode `sandbox/SandboxAbi.h`（C ABI v1）
实现第二个测试沙箱：不可信代码在 **Linux microVM**（WHP 硬件隔离）内执行。

## 构建（需 Rust 工具链；首次联网）

```bash
cd msbsandbox
cargo build --release          # crates 走 rsproxy 镜像（.cargo/config.toml）
# 产物：target/release/MsbSandbox.dll
```

或经 CMake（等于上面 + 部署）：

```bash
cmake --build build --target MsbSandbox --config Release
```

首次 `cargo build` 时 SDK 的 `download-binaries` feature 会把官方 runtime
安装到 `~/.microsandbox/`（bin/msb.exe + lib/libkrunfw.dll，≈70MB）。
**部署 AutoCode 时**把这俩拷到 `Execute/TestSandbox/MsbSandbox/runtime/`
（bin/lib 结构）——dll 启动时优先用私有 runtime（便携），缺失回落全局。

## 运行前提

- Windows「虚拟机监控程序平台」已启用并重启（未启用时沙箱 start=0，
  AutoCode 其余功能不受影响）：

  ```powershell
  # 管理员 PowerShell
  Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
  ```

- 首次 `vm_exec` 拉 OCI 镜像（默认 alpine）需联网；之后离线可用。

## 命令面（经 sandbox 通道：--sandbox CLI / sandbox_* MCP 工具）

| cmd | 参数 | 说明 |
|---|---|---|
| `status` | `{}` | WHP/runtime/命令面诊断 |
| `version` | `{}` | 版本信息 |
| `vm_exec` | `{"code":"sh -c …","image":"alpine","name":"…","timeout_ms":120000}` | 一次性 VM 执行，回 exit/stdout/stderr |
| `vm_stop` | `{"name":"…"}` | 停止并移除常驻沙箱 |
| `vm_write` | `{"name":"…","path":"/x","content_b64":"…"}` | 写 guest 文件（常驻沙箱 connect_or_create） |
| `vm_read` | `{"name":"…","path":"/x"}` | 读 guest 文件（回 base64） |
| GUI 命令 | — | 一律 not-supported（GUI 测试请切换 GhostSandbox） |

注意：`--sandbox` CLI（SandboxCli.cpp）目前只认 GhostSandbox 命令集；
`vm_*` 命令经 MCP 工具或宿主侧扩展后可用（见 AutoCode doc 接入指南）。

## 结构

```
msbsandbox/
├─ Cargo.toml           # cdylib；microsandbox 0.7.x（裁剪 cloud/keyring）
├─ .cargo/config.toml   # crates 国内镜像（rsproxy，项目级）
└─ src/lib.rs           # 22 个 ABI 导出 + WHP 探测 + vm_* 命令
```

内存约定：返回串 CoTaskMemAlloc 分配，宿主经 `sbx_free`（CoTaskMemFree）
归还——跨 CRT 边界安全（两侧分配器均为 COM 任务分配器）。
