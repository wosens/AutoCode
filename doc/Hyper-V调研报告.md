# Hyper-V 调查报告

调查时间：2026-10-25（本机实测复核于当日 01:49）
调查范围：Hyper-V 功能全景（核心 hypervisor、管理与服务、网络、存储、GPU、开放 API）、
衍生产品（WSL2 / Windows 沙盒 / Docker Desktop / VBS）、本机实测状态、对本工程
（AutoCode）的适用性评估。承接《microVM调查报告.md》——Windows 侧唯一的整机级隔离底座。

---

## 一、Hyper-V 是什么

Hyper-V 是 Windows 内置的 Type-1 hypervisor（裸金属型——直接运行在硬件之上）。与
VMware Workstation 这类 Type-2（跑在宿主 OS 之上的应用）不同：启用 Hyper-V 后，
hypervisor 先于操作系统接管 CPU，宿主 Windows 自己变成跑在「root partition」上的
第一台虚拟机，此后创建的每台 VM 都是平级的 child partition。由此带来两个推论：

- 隔离边界是**硬件虚拟化层**（VT-x/AMD-V + SLAT 二级地址转换），每台 VM 拥有独立
  内核——隔离强度与《microVM调查报告》里的 VM 边界同级，而非容器的 namespace 级。
- 代价是宿主自身也虚拟化运行：hypervisor 一旦启用便常驻（HypervisorPresent=True），
  部分依赖 VT-x 直通的老软件受影响（见第三节代价提示）。

与 microVM 报告的关键衔接：Firecracker / Cloud Hypervisor / libkrun 等全部 Linux 系
microVM 引擎依赖 KVM（Linux 内核设施），**均不支持 Windows 宿主**；Windows 上做
整机隔离只有 Hyper-V / WHP 这一条路。

---

## 二、功能全景

按可选功能组件分六块（前五块随 Microsoft-Hyper-V-All，第六块是独立可选功能）：

### 1. 核心 hypervisor（Microsoft-Hyper-V-Hypervisor）
- 分区模型：root + child partition，每 VM 独立地址空间与内核。
- 硬件虚拟化：VT-x/AMD-V 多重执行 + SLAT（EPT/NPT）二级地址转换。
- vCPU 调度、内存管理（ballooning 动态气球回收）。

### 2. VM 管理与服务（Microsoft-Hyper-V-Services = VMMS）
- 两代 VM：Gen1（BIOS + IDE/legacy 设备）与 Gen2（UEFI + 安全启动 + 纯 synthetic
  设备，攻击面更小）；vTPM 仅 Gen2 支持，配合 BitLocker 可做 VM 磁盘加密。
- 检查点：标准检查点（内存+磁盘全量态）与**生产检查点**（VSS 应用一致性，新建 VM
  缺省），可回滚；配合导出可做模板分发。
- 动态内存（按负载增减分配）、运行中热插拔 vCPU/内存。
- 嵌套虚拟化：VM 内再跑 Hyper-V / WSL2（向 child partition 暴露虚拟化扩展）。
- 集成服务：心跳、时间同步、Key-Value 数据交换、来宾关机。

### 3. 网络虚拟化
- 虚拟交换机三型：External（桥接物理网卡）/ Internal（宿主+VM 互通）/ Private
  （仅 VM 间）；缺省 Default Switch 提供 NAT 出网。
- Hyper-V 防火墙：Win11 新增的 per-VM 防火墙策略（本机 Win10 无此组件）。
- SR-IOV 直通：网卡硬件虚拟化直接交给 VM（Server/服务器网卡场景）。

### 4. 存储
- VHDX 虚拟磁盘：单盘上限 64TB、在线扩容、TRIM 回收。
- 差分磁盘（基于父盘的差异链）、精简置备（动态扩展、按需占盘）。

### 5. GPU
- GPU-P（GPU Paravirtualization）：把物理 GPU 划分（partition）给多台 VM 共享；
  客户端 Win10/11 可用，但只能 PowerShell 配置（Add-VMGpuPartitionAdapter），
  无官方图形界面；Linux guest 亦可用。
- WDDM 直通变体：WSL2 / Windows 沙盒的 GPU 加速走此机制（WSLg 图形、CUDA in WSL）。

### 6. 开放 API 层（WHP / HCS）——对生态最重要的一块
- **Windows Hypervisor Platform（WHP，可选功能名 HypervisorPlatform）**：把
  hypervisor 以公开 API 开放给第三方 VMM。典型消费者：QEMU 的 WHPx 加速后端
  （Windows 上通用 QEMU / Android 模拟器的加速路径）。
- **Host Compute Service（HCS）**：同一 hypervisor 之上的容器 / utility-VM 编排层，
  WSL2、Windows 沙盒、Docker Desktop 的 Windows 后端经它创建轻量实例。注意 HCS
  是未正式公开文档的 COM API（仅部分头文件 + 社区逆向）——自行集成工程量大，
  这是第四节「不把 VM 能力写进代码」结论的依据之一。

### 7. 同一 hypervisor 上的衍生产品（各为独立可选功能）

| 衍生 | 可选功能名 | 说明 |
|------|-----------|------|
| WSL2 | VirtualMachinePlatform | 轻量 utility VM 跑真 Linux 内核；Hermes 官方支持环境 |
| Windows 沙盒 | Containers-DisposableClientVM | 一次性 Windows 桌面，关闭即全部销毁 |
| Docker Desktop | —（WSL2/Hyper-V 后端） | Windows 上容器的事实标准入口 |
| VBS / HVCI / 凭据保护 | —（独立开关） | 基于虚拟化的安全：hypervisor 划出安全内核，有性能损耗 |

---

## 三、本机实测状态（2026-10-25 01:49 复核）

| 项 | 状态 |
|----|------|
| 操作系统 | Windows 10 专业版 22H2（build 19044）——专业版支持 Hyper-V 全家（家庭版不行） |
| CPU / 内存 | i7-12700H（14 核 20 线程）/ 63.7GB——跑多台 VM 绰绰有余 |
| 固件 VT-x / SLAT | True / True——硬件条件齐 |
| HypervisorPresent | **False——hypervisor 当前未在运行** |
| Microsoft-Hyper-V-All 及全部子组件 | Disabled |
| VirtualMachinePlatform / HypervisorPlatform / Windows 沙盒 / WSL | 全 Disabled |
| WSL 发行版 | 无（只有 wsl.exe 框架） |
| Docker Desktop | 未安装 |
| VBS（Win32_DeviceGuard） | Status=0 未启用——宿主处于零虚拟化损耗状态 |

即：**硬件与许可证全部满足，当前处于「全关」状态**。启用方式（管理员 PowerShell，
需重启生效）：
- 完整启用：`Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All`
- 最小启用（仅为 WSL2/Docker）：`Enable-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform -All`

代价提示（hypervisor 启用后常驻）：
1. VMware Workstation 旧版（<15.5）无法 VT-x 直通，VM 性能大降（15.5+ 可与 Hyper-V
   共存仍有折损）；VirtualBox 6.x+ 走兼容模式同理。
2. 部分带内核级反作弊的游戏受影响。
3. 单独开 Hyper-V **不等于**开 VBS/HVCI——后者是独立开关（本机未开）；若连带开启，
   会有可测损耗（视负载个位数至 15%）。

---

## 四、能用于 AutoCode 吗？——分层结论

### 1. 直接替换现有 Agent 链路：不能，也没必要

五个后端（claude/DSH/pi/codex/hermes）都是 Windows 原生 CLI；工程在 NTFS 盘上，
路径口径、config.ini（GBK Profile API）、ACL 沙箱（workspace-SID DACL + SACL Low
标签双条件）、CSandboxCore 窗口沙箱（Ghost 收纳）全部按 Windows 语义构建。把 Agent
挪进 Linux VM 后：工程文件要跨 /mnt 边界、GBK 配置失效、窗口沙箱无从谈起。且现有
隔离手段（DSH windows-acl 受限令牌沙箱、窗口 Ghost 沙箱）在 OS 层已覆盖日常工作区
需求——与 VM 隔离是**正交**关系而非竞争。

### 2. 作为基础设施解锁周边能力：能，三条现成路线

**路线 A（对本工程最有价值）：WSL2 + Docker Desktop → 解锁 Hermes docker terminal
后端。** Hermes 本机终端后端只有 local（无隔离）；docker 后端自带安全加固基线
（`tools/environments/docker.py` 的 `_BASE_SECURITY_ARGS`：cap-drop ALL /
no-new-privileges / pids-limit / tmpfs noexec，环境变量显式白名单透传）。本机没装
Docker 才一直用不了。启用 VirtualMachinePlatform + 装 Docker Desktop 即解锁，跑
不可信任务时 `terminal.backend=docker` 切换，AutoCode 侧零改动。顺带获得 WSL2
POSIX 环境（Hermes 官方文档 windows-wsl-quickstart.md 有全篇部署指南；dashboard
内嵌终端的 POSIX PTY 只有 WSL2 路径可用）。注意文件跨界路径摩擦：工程在 E 盘，
Agent 侧路径全变 `/mnt/e/...`。

**路线 B：完整 Hyper-V + Linux VM → Hermes ssh 后端（专职隔离机）。** 起一台
Ubuntu VM（64GB 内存随便开），Hermes `terminal.backend=ssh` 连进去——独立内核的
真 VM 边界，隔离强度对标云侧 microVM。代价：VM 启动秒级~十秒级、常驻占资源，密度
远不如 Firecracker（每任务一台不现实），适合「一台常驻专职隔离机」形态。

**路线 C（运维向）：Windows 沙盒验证便携部署。** 开启 Containers-DisposableClientVM
后，把 `打包-最小自装.bat` 的产物拷进 Windows 沙盒装一遍、验证 install_env.bat——
关窗即全销毁，不污染本机。

### 3. 把 VM 能力写进 AutoCode 代码（像 E2B 那样每任务一台）：现阶段不值得

Windows 没有 Firecracker 等价物；可编程创建 VM 的 Host Compute Service（HCS）是
未正式公开文档的 COM API，集成工程量与维护成本高。而云侧 microVM 后端（modal /
daytona / vercel_sandbox，均为 Firecracker 系）已经以「外接后端」形式覆盖同一需求，
配令牌即可用，无需自建 VM 池。

**一句话结论**：Hyper-V 是 Windows 侧唯一的整机级隔离底座，本机硬件/许可证全齐、
当前全关。对 AutoCode「直接替换现有链路」无意义，「集成进代码」不划算；落地价值
集中在路线 A（最小启用 VirtualMachinePlatform 装 Docker，解锁 docker 沙箱后端）
与路线 B（完整 Hyper-V 开 Linux 隔离机走 ssh 后端）——二者均可随用随开、
AutoCode 侧零改动。

---

## 五、主要参考来源

- Microsoft Learn：Hyper-V 技术文档（Architecture / System requirements / Get started
  / What's new in Hyper-V on Windows）
- Microsoft Learn：Windows Hypervisor Platform（WHP）与 Host Compute Service（HCS）
- Microsoft Learn：基于虚拟化的安全（VBS/HVCI）与 Win32_DeviceGuard 文档
- Microsoft Learn：WSL2 文档；Docker Desktop Windows 后端文档（WSL2 / Hyper-V）
- 本仓库内：`Execute - 副本\HermesHarness\website\docs\user-guide\configuration.md`
  （terminal.backend 七后端口径）、`user-guide\windows-wsl-quickstart.md`（WSL2 部署
  指南）、`tools\environments\docker.py`（docker 后端安全基线）
- 本仓库内：《doc\microVM调查报告.md》（第一、三、四节与本篇互为补充）
