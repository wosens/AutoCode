# MicroVM 调查报告

调查时间：2026-10-24
调查范围：microVM 概念与原理、主流 microVM 实现技术、AI Agent 沙箱场景（含本工程
Hermes Harness 内置的终端后端）、Windows/macOS 平台对应形态。

---

## 一、什么是 microVM

microVM（micro virtual machine，微型虚拟机）是一种「把几乎所有东西都裁剪掉的虚拟机」：
极小的设备模型、专用的 VMM（Virtual Machine Monitor，虚拟机监视器）、启动时间以几十至
几百毫秒计。它与容器、传统 VM 的本质区别一句话说清：

- **容器**：共享宿主机内核，隔离靠 namespace/cgroup（seccomp/Landlock 等辅助）。
  逃逸面 = 内核攻击面。
- **microVM**：工作负载获得**自己的 Linux 内核**，恶意代码要先穿透自己的内核、再穿透
  VMM、再穿透宿主 hypervisor 暴露面——这是与「逃出一个 namespace」完全不同量级的问题。
  安全文献普遍把 microVM 视为对抗敌意代码的可信答案。
- **传统 VM（QEMU 全量机型）**：同样有独立内核，但设备模型庞大（QEMU 模拟数百种设备，
  代码以百万行 C 计），每个模拟设备都是潜在漏洞；启动动辄数十秒。

microVM 的「微」来自减法：以 Firecracker 为例，只模拟约 6 种设备、VMM 本体约 5 万行
Rust（QEMU 是百万行量级 C），内存开销 < 5 MiB/实例，冷启动 ~125 ms，单机可密度化部署
数千实例。设备模型更小 = 攻击面更小 + 启动路径更短。

---

## 二、主流 microVM 引擎（VMM）盘点

### 1. Firecracker（AWS）
- **定位**：为 serverless 而生的极简 VMM，一进程一 microVM。
- **技术底座**：Linux KVM（依赖 /dev/kvm）；Rust 编写；只暴露单一 API（Unix socket）。
- **关键数字**：启动 ~125 ms、内存开销 < 5 MiB；自带 jailer（加固沙箱）、快照。
- **生产背书**：AWS Lambda 与 Fargate 的底层，驱动数百万生产工作负载。
- **短板**：极简主义即代价——无 virtio-fs（目录挂载不便）、无热插拔、无 live migration、
  设备模型几乎不可扩展；强绑定 KVM（多数 VPS 无 KVM，不可用）。
- **谁在用**：AWS Lambda/Fargate、Vercel Sandbox、Daytona、E2B、Fly.io（Machines）、
  OpenAI 的代码执行基础设施等。

### 2. Cloud Hypervisor（Intel 发起，Linux Foundation 托管）
- **定位**：「功能齐全的现代 VMM」——microVM 隔离 + 开发者便利。
- **技术底座**：KVM / MSHV（Hyper-V 主机上的 Linux 子系统）；Rust 编写；基于 virtio。
- **亮点**：virtio-fs 目录共享、CPU/内存热插拔、vhost-user 卸载、快照恢复、live
  migration、类型化 Rust API、worker 线程 per-thread seccomp。
- **生态位**：Kata Containers 支持的 VMM 后端之一；想要「microVM + 好用」的默认选择。

### 3. crosvm（Google）
- **定位**：ChromeOS 的官方 VMM，Rust 编写，安全、轻量、高性能。
- **生产场景**：ChromeOS 里跑 Linux 容器（Crostini）、安卓（Arc++）、以及 ChromeOS
  上的杀毒/开发环境；也是 2026 年 Google 天梯中的默认 VMM。
- **特点**：设备模型比 Firecracker 丰富（GPU virtio-gpu、音频等），Jail 机制（seccomp +
  namespace 双层加固每个设备进程）。

### 4. QEMU microvm 机型
- **定位**：QEMU 家族里仿照 Firecracker 思路裁剪出的专用机型（`-M microvm`）。
- **特点**：去 PCI、去 ACPI（可选）、virtio-mmio 直挂，启动可压到几百毫秒——但 VMM
  本体仍是百万行 C 的 QEMU，攻击面与内存开销远大于专用 microVM VMM。
- **生态位**：已有 QEMU 资产、需要兼容性优先时的选择；Kata Containers 的默认后端
  仍是 QEMU。

### 5. libkrun（Red Hat / containers 生态）
- **定位**：**动态库形态**的虚拟化进程隔离——把 VM 藏进一个 dlopen 就能用的库里。
- **技术底座**：Linux KVM / macOS HVF（Hypervisor.framework）；Rust 编写。
- **特点**：无需特权守护进程、无需容器运行时，普通进程直接加载即获得 VM 隔离；
  virtio-console/block/fs(virtio-fs)/net(vhost-user) 设备齐全；配 krunkit 可在
  Apple Silicon 上跑 GPU（venus）轻量 VM；~ms 级启动。子项目 krunvm、Microsandbox
  均基于它。
- **短板**：仍需 KVM/HVF 硬件虚拟化，VPS 上不可用。

### 6. Kata Containers（运行时层，非 VMM）
- **定位**：OCI 兼容容器运行时，把每个 Pod/容器放进一台 microVM——「容器体验 + VM 边界」。
- **可换后端 VMM**：QEMU（默认）、Cloud Hypervisor、Firecracker、Dragonball（蚂蚁）。
- **短板**：是基础设施不是桌面工具——特权 DaemonSet 安装、host kernel/VMM/shim/guest
  kernel/agent 五方版本锁步，有运维研究测得 on-call 页面比普通容器多 3.4 倍。

### 7. 平台内建虚拟化框架（Windows/macOS 对应形态）
- **Apple Virtualization framework（VirtualizationKit）**：macOS 专属，Swift API 创建
  macOS/Linux VM；Apple Silicon 上配 rosquee 可跑 x86 Linux；E2B 等本地方案在其上实现。
- **Hyper-V / Windows Hypervisor Platform（WHP）**：Windows 的 hypervisor 平台；WSL2、
  Docker Desktop、Sandbox 即基于它；VBS（基于虚拟化的安全）用它划出安全内核。
  注：Firecracker/Cloud Hypervisor/libkrun 等 Linux 系 microVM 引擎均**不支持 Windows
  宿主**（依赖 KVM），Windows 上做 microVM 只能走 Hyper-V/WHP 路线。

---

## 三、AI Agent 沙箱场景：谁用的是什么

Agent 代码执行沙箱是 microVM 增长最快的消费场景。主要玩家实测口径：

| 平台 | 隔离引擎 | 说明 |
|------|----------|------|
| **E2B** | Firecracker microVM | 每个会话一台 Firecracker VM；用预启动快照恢复代替冷启动，秒级拉起；开源（e2b-dev/infra）。 |
| **Vercel Sandbox** | Firecracker microVM | 官方页明示「每个 sandbox 都是一台隔离的 Firecracker microVM，自带文件系统」；计算与网络双向隔离。 |
| **Daytona** | Firecracker microVM | 官方文档明示「hardware-isolated sandboxes——Firecracker VM，own kernel/filesystem/network」，可由任意 OCI 镜像创建。 |
| **Modal** | gVisor 或 VM sandbox（双选项） | 用户可选 gVisor（用户态应用内核拦截 syscall）或 VM 级隔离；sidecar 提供低延迟信任边界；号称 10,000+/秒 创建、累计 30 亿 sandbox。 |
| **Fly.io Machines** | Firecracker microVM | 早期把 Firecracker 产品化的代表。 |
| **OpenAI / Anthropic 代码执行** | microVM 类隔离 | 公开口径为 VM 级隔离（OpenAI 用 gVisor+Firecracker 混合的报道均有；以官方安全文档为准）。 |
| **Microsandbox** | libkrun fork | 本地 microVM、支持分支/快照/OCI 镜像/四语言 SDK、无守护进程；仍为 beta（AMD 平台启动失败、快照完整性、virtio-fs 泄漏、2026 CVE 等已知问题）。 |
| **hearth 等本地方案** | Firecracker | 「E2B but on your machine」类项目，依赖本机 KVM。 |

**gVisor 是「中间路线」**：Google 的用户态内核（runsc），拦截全部 syscall 以应用内核
处理，不透传给宿主内核——隔离强度介于容器与 microVM 之间，兼容性损耗出现在特定二进制
（调试器、native 模块、数据库客户端、图像库）。对 agent 场景，官方口径都是「拿真实任务
全序列先跑一遍再定」。

---

## 四、本工程（Hermes Harness）的 microVM 相关现状

本仓库 `Execute - 副本\HermesHarness\`（hermes-agent 部署副本）的 terminal 工具支持
7 种执行后端（`tools/environments/`、`website/docs/user-guide/configuration.md`）：

| 后端 | 位置 | 隔离级别 | 官方定位 |
|------|------|----------|----------|
| local | 本机直跑 | 无 | 开发、信任任务 |
| ssh | 远程机 | 网络边界 | 远程开发 |
| docker | 容器 | 容器级（cap-drop ALL/no-new-privileges/pids-limit 256/tmpfs noexec） | 生产网关 |
| singularity | 容器 | 容器级（--containall --no-home） | HPC 集群 |
| **modal** | Modal 云 sandbox | **完整（云 VM）** | 弹性云隔离、评测 |
| **daytona** | Daytona 云工作区 | **完整（云容器→实为 Firecracker microVM）** | 持久云开发环境 |
| **vercel_sandbox** | Vercel Sandbox | **完整（云 microVM）** | 快照回灌的云执行 |

三处文档（configuration.md / tools.md / security.md）口径一致地把 vercel_sandbox 标注
为「cloud microVM」；modal/daytona 标注为「cloud VM/cloud container」——按三家公司
官方文档核实，这三者的底层都是 **Firecracker 系 microVM**（Modal 另提供 gVisor 选项）。

关键实现细节（源码定位）：
- `tools/environments/vercel_sandbox.py`（662 行）：`VercelSandboxEnvironment` 继承
  `BaseEnvironment`；runtime 支持 node24/node22/python3.13（缺省 node24）；磁盘不支持
  自定义（只认缺省 51200 MB，非默认值直接报错）；持久化 = 清理时打快照，下次同 task
  从快照恢复（快照库 `~/.hermes/vercel_sandbox_snapshots.json`）；快照恢复失败自动回退
  全新 sandbox；鉴权需 VERCEL_TOKEN + VERCEL_PROJECT_ID + VERCEL_TEAM_ID 三件套（也
  支持一次性 VERCEL_OIDC_TOKEN）；SDK 懒加载并默认禁用其遥测
  （VERCEL_TELEMETRY_DISABLED=1）。
- `tools/environments/daytona.py`：`DaytonaEnvironment`；停/恢复式持久化（不是删了重建）；
  sandbox 命名 `hermes-{task_id}`；磁盘上限 10 GiB 硬顶；鉴权 DAYTONA_API_KEY。
- `tools/environments/modal.py` + `managed_modal.py` + `modal_utils.py`：直连 SDK
  （Sandbox.create + exec，快照存 `~/.hermes/modal_snapshots.json`）与 managed（经
  Nous Tool Gateway 托管）双模式，`terminal.modal_mode` 选 direct/managed/auto。
- `tools/environments/docker.py`：安全加固基线 `_BASE_SECURITY_ARGS`（cap-drop ALL、
  no-new-privileges、pids-limit、tmpfs noexec），`docker_forward_env` 显式白名单透传
  环境变量。
- 工厂分发在 `tools/terminal_tool.py` `_create_environment()`（约 1980-2120 行），
  `_CONTAINER_BACKENDS = {docker, singularity, modal, daytona, vercel_sandbox}`。

**与本机现实的关联**：本机是 Windows 宿主，AutoCode 的 Hermes 后端跑 exe 形态（HVF/
KVM 引擎不可用），terminal.backend 实际为 local。上述 cloud microVM 后端（modal/
daytona/vercel_sandbox）都是云侧隔离——Hermes 只把 terminal/file 工具面投给模型，无
Vercel 专属模型工具；要启用需配三件套令牌 + `pip install 'hermes-agent[vercel]'`。
另：Hermes deliverable-mode 文档特意对比 Perplexity Computer，强调自家「生成发生在
用户自己的 venv/sandbox，无多租户 microVM、无托管 token 存储」——即 Hermes 本体不
内嵌任何 microVM 运行时，microVM 全部来自外接后端。

---

## 五、选型速查（什么时候用谁）

| 需求 | 推荐 | 理由 |
|------|------|------|
| 最高隔离强度、跑敌意代码 | Firecracker / Cloud Hypervisor | 独立内核 + 最小设备模型；业界 agent 沙箱事实标准 |
| microVM + 开发便利（共享目录/热插拔/迁移） | Cloud Hypervisor | virtio-fs + 热插拔 + live migration，Rust 类型化 API |
| 桌面/单机、不想跑守护进程 | libkrun（krunvm/Microsandbox） | 动态库形态，dlopen 即得 VM 隔离 |
| ChromeOS/Google 生态 | crosvm | ChromeOS 官方 VMM，设备模型较全 |
| 已有 QEMU 资产/兼容优先 | QEMU microvm 机型 | 复用 QEMU 生态，但攻击面大 |
| K8s 集群给每个 Pod 上 VM 边界 | Kata Containers（+CH/Fc 后端） | OCI 兼容；运维复杂度换隔离 |
| 不想碰 KVM、要托管服务 | E2B / Vercel / Daytona / Modal | Firecracker 系托管沙箱，API 即用 |
| Windows 宿主 | Hyper-V / WHP 路线 | Linux 系 microVM 引擎均不支持 Windows 宿主 |

**共性约束（踩坑提示）**：
1. 全部 Linux 系 microVM 引擎依赖 KVM（云 VPS 通常不透传 /dev/kvm，libkrun/
   Microsandbox/firecracker 一律起不来）——Apple Silicon 上 libkrun 走 HVF 可用。
2. microVM 不解决「镜像里带了什么」的问题： guest 内 root 仍能看 guest 全部文件，
   凭据要留在宿主侧（Hermes vercel 后端把凭据 sync 进 sandbox 时就有此告警口径）。
3. 快照恢复只保留文件系统态，不保留进程/PID/后台任务/网络连接——所有云后端文档统一
   强调，勿当「暂停虚拟机」用。

---

## 六、主要参考来源

- AWS Firecracker 官方与原理博客（aws.amazon.com/cn/blogs/china、firecracker-microvm.github.io）
- A Field Guide to AI Agent Sandboxes — MicroVMs 章节（wuu73.org/minis/sandbox-field-guide/microvms.html）
- Vercel Sandbox 官方页与文档（vercel.com/sandbox、vercel.com/docs/sandbox/concepts）
- Daytona 官方文档（docs.rl.trydaytona.com — "each sandbox is a Firecracker microVM"）
- Modal 博客：untrusted code 隔离双选项、sandbox sidecars、10k+/秒 创建
  （modal.com/resources、modal.com/blog）
- E2B 官方与 infra 架构文档（e2b.dev、github.com/e2b-dev/infra ARCHITECTURE.md）
- libkrun 仓库（github.com/containers/libkrun；KVM/HVF、krunkit/venus GPU）
- Kata Containers 官方虚拟化设计文档（kata-containers.github.io）
- QEMU microvm 机型文档（qemu.org/docs/master/system/i386/microvm.html）
- crosvm 仓库（github.com/google/crosvm）
- Microsoft VBS 文档（learn.microsoft.com — Windows 侧虚拟化安全）
- 本仓库内：`Execute - 副本\HermesHarness\website\docs\user-guide\{configuration,tools,security}.md`、
  `Execute - 副本\HermesHarness\tools\environments\*.py`
