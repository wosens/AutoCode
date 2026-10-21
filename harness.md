# harness.md —— AutoCode 五后端（Harness）集成说明

> 本文件随 AutoCode.exe 部署（exe 同级）。后端出现故障时，修复 Agent 请先读完本文，
> 再按「各后端通用排查」与对应后端小节的验证命令定位、修复、复检。
> 最后更新：2026-10-21（任务：验证所有后端 + 自动修复）。

## 0. 总体架构

AutoCode 通过 `agent/` 目录下的 5 个 Runner（C++/Qt）驱动 5 个 CLI 后端，统一由
`agent/CAgentRunnerFactory.h` 分派，后端号全局约定：

| 后端号 | 名称 | Runner | 进程模型 |
|---|---|---|---|
| 1 | Claude Code | `CClaudeRunner` | 一次性进程（每轮一个 `claude -p`） |
| 2 | DeepSeekHarness（DSH） | `CDeepSeekRunner` | **长驻** node 进程（stdio JSON-RPC 2.0） |
| 3 | PIAgent | `CPiAgentRunner` | 一次性进程（`pi --mode json`，NDJSON 流） |
| 4 | CodexHarness | `CCodexRunner` | 一次性进程（`codex exec --json`） |
| 5 | Hermes | `CHermesRunner` | 一次性进程（`hermes -z`） |

全局开关：`<exe 同级> config.ini` 的 `[Agent] backend`（1..5）。各后端路径/模型等
配置同在该节（`claudecmd / dshroot / piroot / codexroot` 等）。

**PATH 关键事实**：AutoCode 启动后新装的 Node/Claude 等不会出现在已运行进程的
PATH 中——Runner 统一经 `CAgentRunnerBase::MergedPathEnvironment()` 把注册表
Machine+User PATH 合并进子进程环境（30s TTL 缓存）。排查「命令找不到」时先用
同口径环境验证，勿只看当前 shell。

**版本闸门**：DSH 受支持上限 = `dsh-v0.1.5-alpha.1`（高于它的 tag 会破坏集成）；
CodexHarness 锁定 0.87.0。更新后端时不要越过这两道闸。

## 1. Claude Code（后端号 1）

- 承载：npm 全局包 `@anthropic-ai/claude-code`（引擎要求 Node ≥ 22）。
- 启动方式：`cmd /c <claudecmd> -p --output-format stream-json ...`（claudecmd 来自
  config.ini `[Agent] claudecmd`，缺省 `claude`）。
- 验证命令：
  ```bat
  claude --version
  claude -p "只回复两个字：可用"
  ```
- 常见故障与修复：
  - `claude 进程异常退出，退出码=1`：多为 Node 缺失/过旧（<22 时 postinstall 语法
    解析失败）或 PATH 陈旧。修复：跑 exe 同级 `install_env.bat /node` 装新版 LTS，
    装完重启 AutoCode；或 `npm install -g @anthropic-ai/claude-code@latest` 重装。
  - 余额/网关类错误：模型路由走网关时核对 config.ini 大模型表与网关服务状态。

## 2. DeepSeekHarness / DSH（后端号 2）

- 承载：deepseek-harness 仓库（config.ini `[Agent] dshroot`），node 长驻进程。
- 布局判型（`CDeepSeekRunner::DshLayoutOf`）：
  - **Profile**（官方 SDK，dsh-v0.1.2+，本机当前形态）：
    `node <dshroot>\apps\cli\lib\bin.js --profile sdk --patch <exe 同级>\dsh\sdk.autocode.patch.yml`
  - **Legacy**（≤0.1.1 示例包）：`node <dshroot>\packages\examples\jsonrpc-demo\lib\bin.js <cordis.yml>`
- 协议：stdio 换行分帧 JSON-RPC。握手帧（AutoCode 口径）：
  ```json
  {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"cwd":"<工作目录>","provider":"<provider>","model":"<model>"}}
  ```
  响应含 `result` = 握手成功；含 `error` = 失败（message 即原因）。stdin 全程不能关
  （bin.js 对 stdin end 敏感，收到即退出）。
- 模型路由：
  - 自定义大模型表（config.ini `[DshModels]`）激活条目 → provider=`autocode-active`，
    env 槽位 `DSH_LLM_NAME/BASE_URL/API/MODEL/CTX/MAXTOK` + `AUTOCODE_DSH_API_KEY`。
  - 表空 → 固定路由 `[Agent] dshprovider/dshmodel`（历史缺省 glm-custom/glm-5.2）。
- 验证命令（PowerShell/手动 node 脚本均可，核心是握手）：
  ```bat
  node <dshroot>\apps\cli\lib\bin.js --profile sdk --patch <exe同级>\dsh\sdk.autocode.patch.yml --dump-config
  node -v
  ```
  完整握手验证建议复用 AutoCode 的「验证所有后端」（发 initialize 等响应）。
- 常见故障与修复（按实测频率排序）：
  1. **initialize 报 `no adapter registered for provider "glm-custom"`（新版 DSH 高发，
     具有强误导性）**：根因不是 glm-custom 本身——新版 llm-pi-ai 插件启动时会为
     `$DSH_HOME` 导入设置里的**全部** provider 建 adapter，任一 provider 的
     `apiKeyEnv` 环境键缺失（如 `GLM_CUSTOM_API_KEY`）会让插件启动失败，导致所有
     adapter 都没注册。修复：给进程环境补 `GLM_CUSTOM_API_KEY`（空串即可，AutoCode
     2026-10-21 起已自动兜底注入）；若用户自用 glm-custom，则设真实键值。
  2. `~/.dsh/.credentials.yaml` 为 version+refs 包装格式（旧工具所写）→ DSH 解析不出
     密钥。修复：改写为平铺 `KEY: 值` 映射（AutoCode `HealCredentialsFile` 有现成
     实现，先备份 `.autosave`）。
  3. `%USERPROFILE%\.dsh\profiles\sdk\cordis.yml` 不可写（每次冷启动无条件重写）→
     进程即崩退出码 1。修复：`icacls "<目录>" /grant "*<本机用户SID>:(OI)(CI)(F)"`。
  4. 版本漂移（本地 > dsh-v0.1.5-alpha.1）：上游移除了 AutoCode 集成依赖的文件。
     修复：`git fetch + git reset --hard dsh-v0.1.5-alpha.1`，随后清 lib/`*.tsbuildinfo`
     残留 → `pnpm install` → `pnpm build:lib`（AutoCode 有自愈流水线同款步骤）。
  5. node 缺失：装 Node.js 或在 config.ini `[Agent] dshnodepath` 指定 node.exe。

## 3. PIAgent（后端号 3）

- 承载：pi monorepo（config.ini `[Agent] piroot`，源码形态
  `<root>\pi\packages\coding-agent\dist\cli.js`）或 npm 全局包
  `@earendil-works/pi-coding-agent`。
- 启动方式：`node <cli.js> --mode json ...`（模型经
  `--provider autocode-<n> --model <id>`，provider 槽位由
  `~/.pi/agent/models.json` 的 `autocode-*` 条目承载，密钥在其中）。
- 验证命令：
  ```bat
  node <root>\pi\packages\coding-agent\dist\cli.js --version
  node <root>\pi\packages\coding-agent\dist\cli.js --provider autocode-1 --model <id> -p "只回复两个字：可用"
  ```
- 常见故障与修复：
  - `Unknown provider "autocode-n"`：models.json 的 autocode-* 槽位被清（多实例/热
    重载竞态）。修复：按 config.ini `[PiModels]` 表重写 models.json（保留用户手写的
    非 autocode-* provider），AutoCode 启动路径有幂等自愈同款逻辑。
  - 429 限流：`~/.pi/agent/settings.json` 补 `retry.baseDelayMs=15000`（跨配额滚动窗）。
  - node 缺失/过旧：装 Node LTS 或 config.ini `[Agent] pinodepath` 指定。

## 4. CodexHarness（后端号 4）

- 承载：npm 本地工程（config.ini `[Agent] codexroot`，锁定
  `@openai/codex@0.87.0`：`<root>\node_modules\@openai\codex\bin\codex.js`）。
- 启动方式：`node <codex.js> exec --json --skip-git-repo-check -C <workdir> ...`，
  模型路由用 `-c model_provider=<槽>` `-c model=<id>` 覆盖级参数；落地配置在
  `~/.codex/config.toml`（`[model_providers.autocode-<n>]` 节）+ `~/.codex/auth.json`
  （`OPENAI_API_KEY`）。
- 验证命令：
  ```bat
  node <root>\node_modules\@openai\codex\bin\codex.js --version
  node <root>\node_modules\@openai\codex\bin\codex.js exec --skip-git-repo-check -s read-only "只回复两个字：可用"
  ```
- 常见故障与修复：
  - 包缺失/损坏（半安装态）：`cd <root> && npm install @openai/codex@0.87.0 --save-exact`
    （受保护路径需管理员）。
  - `model_providers` 槽位丢失（多实例竞态写空 config.toml）：按 config.ini
    `[CodexModels]` 激活条目重写 config.toml + auth.json（AutoCode
    `WriteCodexUserConfig` 为现成实现，行级手术保留用户内容）。
  - node < 20：装 Node LTS。

## 5. Hermes（后端号 5）

- 承载：git 部署的 hermes-agent 仓（`HERMES_HOME` 环境变量或 config.ini
  `[Agent] hermesroot` 或 exe 同级 `HermesHarness\`，形态 A=node `dist\cli.js`，
  形态 B=venv `bin\hermes.exe`）。
- 启动方式：`<hermes> -z "<prompt>"`（一次性）。
- 验证命令：
  ```bat
  hermes --version
  hermes -z "只回复两个字：可用"
  ```
- 常见故障与修复：
  - 进程起不来（Process failed to start）：HERMES_HOME 失效/venv 损坏 → 重跑 exe 同级
    `install_env.bat /hermes <仓库>`（git 克隆 + pnpm 构建，可续装）。
  - 已装但需更新：在 Hermes 根 `git fetch && git reset --hard origin/HEAD`，Node 形态
    重跑 `pnpm install && pnpm build`，Python 形态重敷 oneshot 补丁
    （`hermes_oneshot_patch.py`）。

## 6. 修复守则（对修复 Agent 的硬约束）

1. **只修故障后端与 AutoCode 的集成层**（runner 配置指向的目录、exe 同级 dsh\ 组合层、
   ~/.codex、~/.pi/agent、~/.dsh 等），不改动无关工程与用户数据。
2. 不修改 AutoCode.exe、install_env.bat 等部署正本；需要时用其自带的参数重跑。
3. 修配置前先备份原文件（同目录 `.bak-<日期>` 后缀）。
4. 提权操作（npm 全局装、icacls）优先改用非提权方案；确需提权时在汇报里明确说明。
5. 修复完成后必须用上文对应后端的「验证命令」复检，并汇报：根因、改动清单、验证结果。
