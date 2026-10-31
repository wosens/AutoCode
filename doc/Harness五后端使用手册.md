# AutoCode 五大 Harness 后端使用手册（对比 · 排序 · 选型指南）

> 版本：v1.0
> 日期：2026-10-26
> 适用：AutoCode v5.0.x（五后端：Claude Code / DeepSeekHarness / PIAgent / CodexHarness / Hermes）
> 依据代码：`agent/` 五个 Runner + `orch/CTaskOrchestrator.cpp` + `agent/dsh/*.yml` + `scripts/hermes_oneshot_patch.py`（文中均给出锚点，事实均可回查）
> 配套：部署层排障见 exe 同级 `harness.md`；功能操作见 `help/使用说明.html`

---

## 0. 总览

### 0.1 五后端身份表

| | Claude Code | DeepSeekHarness (DSH) | PIAgent | CodexHarness | Hermes |
|---|---|---|---|---|---|
| 后端号（config.ini `[Agent] backend`） | 1 | 2 | 3 | 4 | 5 |
| Runner | `CClaudeRunner` | `CDeepSeekRunner` | `CPiAgentRunner` | `CCodexRunner` | `CHermesRunner` |
| CLI 承载 | npm 包 `@anthropic-ai/claude-code`（Node ≥ 22） | deepseek-harness 仓（`dsh --profile sdk`） | pi monorepo 源码或 npm 包 `@earendil-works/pi-coding-agent` | npm 包 `@openai/codex` **版本锁 0.87.0** | hermes-agent 仓（Node `dist\cli.js` / Python venv `hermes.exe` 双形态） |
| 进程模型 | 一次性进程（每轮 `claude -p`） | **长驻 node 进程**（进程复用） | 一次性进程；RPC 形态为常驻管道 | 一次性进程（`codex exec --json`） | 一次性进程（`hermes -z`） |
| 输出协议 | stream-json（NDJSON） | 换行分帧 JSON-RPC 2.0 | NDJSON / RPC JSONL 双向 | JSONL 事件流 + 本地 max_tokens 代理 | NDJSON（AutoCode oneshot 补丁接通事件） |
| 会话延续 | `--resume` + `--fork-session`（原生跨轮） | 同 sid 进程内续轮（跨进程换 sid） | `--session-id` 磁盘会话（原生跨进程） | 无（每轮新会话，靠回灌） | 无（同 Codex） |
| 上下文自动压缩 | `--autocompact`（v2.1.221+，自动探测降级） | 组合内置 compaction-basic | pi 原生自动 compaction | 无 | 无（钳位 ctx 200K / maxTok 128K） |

### 0.2 总评分速查表（★=一般 ★★=可用 ★★★=好 ★★★★=优秀）

| 维度 | Claude Code | DSH | PiAgent | Codex | Hermes |
|---|---|---|---|---|---|
| 1. 轮中插入提示词 | ★★★★ | ★★☆ | ★★★★ | ★★ | ★★ |
| 2. 擅长任务广度 | ★★★★ | ★★★ | ★★★★ | ★★★ | ★★☆ |
| 3. 速度（框架开销） | ★★★ | ★★★★ | ★★★ | ★★★ | ★★☆ |
| 4. Token 经济性 | ★★★★ | ★★★★ | ★★★★ | ★★☆ | ★★☆ |
| 5. 工具调用能力 | ★★★★ | ★★★☆ | ★★★★ | ★★★ | ★★☆ |
| 6. 任务拆解支撑 | ★★★★ | ★★★☆ | ★★★☆ | ★★☆ | ★★ |
| 7. 多轮智力保持 | ★★★★ | ★★★☆ | ★★★★ | ★★ | ★★ |
| 8. 一次成功率（集成稳定性） | ★★★★ | ★★★ | ★★★☆ | ★★★ | ★★☆ |
| 9. 跨进程会话恢复 | ★★★☆ | ★★ | ★★★★ | ★ | ★ |
| 10. 实时计量精度 | ★★★★ | ★★★★ | ★★★★ | ★★★★ | ★★★ |
| 11. 过程反馈丰富度 | ★★★★ | ★★★ | ★★★★ | ★★★ | ★★★ |
| 12. 停滞检测与自愈 | ★★★★ | ★★★☆ | ★★★☆ | ★★★ | ★★★ |
| 13. 安全与审批 | ★★★☆ | ★★★★ | ★★★☆ | ★★☆ | ★★☆ |
| 14. 部署维护成本（低=好） | ★★★ | ★★☆ | ★★★ | ★★★★ | ★★☆ |
| 15. 模型接入灵活性 | ★★★ | ★★★★ | ★★★★ | ★★★ | ★★★★ |

> 结论先行：**任务流水线与复杂工程首选 Claude Code，PiAgent 为最接近的备援**；国产/内网网关模型走 DSH / Hermes；OpenAI 系模型走 Codex；需要强沙箱隔离选 DSH。逐维度论证见 §1–§15。

---

## 1. 是否支持插入提示词（运行中介入）

**机制**：AutoCode 统一经 `CAgentRunnerBase::InjectUserTurn()` 投递；返回 true = 已送达当前轮，false = 上层自动排队（任务侧排到下一阶段 prompt 前置；助手侧本轮结束后自动续发，多条排队指令**合并为一次投送**）。2026-10-16 起另有「退净窗口」判定（`IsTurnClosed`）：result 已收而进程未退的窗口内不注入，排队延迟排空，杜绝指令搁浅。

| 后端 | 支持 | 机制与代码锚点 |
|---|---|---|
| Claude Code | ✅ **轮中真注入** | `--input-format stream-json` 下 stdin 全程打开，`InjectUserTurn` 把文本编码为 user 帧写入（`CClaudeRunner::WriteUserFrame`），模型在当前轮内即收到转向/补充指令 |
| PiAgent | ✅ **轮中真注入**（RPC 形态） | `InjectUserTurn` 发 `{"type":"steer"}` 命令帧（`CPiAgentRunner.cpp` L889）；实测工具执行中注入、下一模型迭代即收到。一次性回退形态不支持（no-op），RPC 能力探测见 `RpcCapable()` |
| DSH | ⏳ 半支持 | 协议无轮中插话（`CDeepSeekRunner::InjectUserTurn` L2286）：进程**空闲即发**（同 sid 续轮，无冷启动开销）；运行中返回 false 排队，本轮结束自动发送 |
| Codex | ❌ 排队 | 一次性进程，prompt 投递后 stdin 即关；注入排队到本轮结束后作为新轮投送 |
| Hermes | ❌ 排队 | 同 Codex（`-z` 一次性进程） |

**排序**：Claude Code ≈ PiAgent ＞ DSH ＞ Codex ≈ Hermes
**实操**：想「干着活随时纠偏」选 Claude Code 或 PiAgent；DSH 错峰半分钟内送达也可接受；Codex/Hermes 的排队语义等于「下一条指令」，不适合纠偏正在跑的轮。

---

## 2. 擅长的任务类型

| 后端 | 擅长 | 相对短板 |
|---|---|---|
| Claude Code | 全能型旗舰：多阶段大型重构、跨文件架构级修改、需要精细工具编排/子代理/MCP 技能（ui-sandbox、ocr 等）的任务；AutoCode 任务流水线默认后端，实测问题最少 | 依赖 Anthropic 兼容端点（走网关需条目兼容）；费用口径走 claude 自身或共享表跟随 |
| PiAgent | 与 Claude Code 同级的成熟 coding-agent（事件流同构、能力对齐），适合作 Claude 的**平替/备援**；网关自定义模型接入方便（`~/.pi/agent/models.json` 的 `autocode-N` 槽位） | 无 ask_user 原生工具（手工审批走 `--ask` 弹窗命令通道）；maxTurns/autocompact 降级忽略 |
| DSH | DeepSeek 官方框架：国产模型（GLM 等 anthropic-messages 网关）+ **唯一默认带强沙箱**（pwsh workspace-write，NTFS DACL + Low 完整性标签）；长驻进程连续多轮问答/连续改码效率高；内置 todo/子代理/压缩 | 跨进程会话必换 sid（上下文靠回灌）；上游版本演进快有集成闸门（≤ dsh-v0.1.5-alpha.1）；写权限受沙箱 DACL 约束（关联项目目录需 `ExtraWriteDirs` 放行） |
| Codex | OpenAI 系模型的代码生成/补全类任务；事件粒度清晰（command/file_change/MCP/web_search）；部署最简单（npm 本地工程，可离线 robocopy 拷贝安装） | **版本锁 0.87.0**（v0.88+ 移除 wire_api="chat"）；只能连 chat completions 兼容端点；无会话续接；无轮次上限/自动压缩 |
| Hermes | NousResearch hermes-agent：轻量一次性任务、问答与中小型改动、本地/小众模型快速接入；Python/Node 双形态适应面广 | 上游 `-z` 一次性模式本身全静音（AutoCode 补丁接通事件流）；一次性进程无注入/续跑支撑；历史集成问题最多（多数已修，见 §8） |

**排序（任务广度）**：Claude Code ≈ PiAgent ＞ DSH ＞ Codex ＞ Hermes
**选型**：大活/重活 → Claude Code（备选 PiAgent）；内网国产模型 → DSH；要沙箱 → DSH；OpenAI 模型 → Codex；轻量快问快答 → Hermes / PiAgent。

---

## 3. 速度

**先说结论：速度主要取决于所配模型**——同一模型在五个后端上的框架开销差异，远小于不同模型之间的吞吐差异（AutoCode 实测过 glm-5.3 / glm-4.7-flash / Space Bunny Alpha 等，模型间差距是数量级的）。框架层面可比较的是**固定开销与反馈节奏**：

| 后端 | 首轮冷启动 | 轮间衔接 | 反馈节奏 |
|---|---|---|---|
| Claude Code | node 启动 ~1s 量级 | 每轮新进程（续跑同新起） | token 级流式，本轮↑↓实时跳动（`sigLivePartial`） |
| DSH | 冷启动 + initialize 握手（首轮最慢） | **进程复用零冷启动**（同 sid 续轮最快） | 按消息级事件；工具前思维链文本较厚（UI 已折叠合并） |
| PiAgent | node 启动 ~1s 量级 | 每轮新进程；RPC 形态管道常驻省重启 | token 级流式；注意 thinking=high 时单条消息可流式十几分钟（AutoCode 60s 心跳提示防“像卡死”） |
| Codex | node 启动 + 版本预检 | 每轮新进程 + 回灌历史（轮次多则每轮变慢） | JSONL 事件块级（agent_message 整块收尾才上屏） |
| Hermes | exe（venv）形态启动偏慢；Node 形态同 node | 每轮新进程 + 回灌 | 补丁后 4s 节流 progress + 工具卡（无 token 级流） |

**排序（框架开销，小=好）**：DSH（多轮场景）≈ Claude Code ≈ PiAgent ＞ Codex ＞ Hermes（exe 形态）
**实操**：长会话连续干活 DSH 进程复用最省；单轮任务五者差异可忽略；嫌 Hermes 慢优先确认走的是 Node 形态、模型本身吞吐是否够。

---

## 4. Token 消耗（经济性）

**计量口径统一**：五后端都拆 input / cacheRead / cacheWrite / output 四分量累计（`ApplyUsage` 各 Runner 同构），四后端共享一张单价表（`[Agent] DshPrice*/PiPrice*/HermesPrice*`）折算费用；Codex 经本地代理解析网关真实 usage（`CCodexProxy` 强制 `stream_options.include_usage`）。

结构性差异决定账单形态：

| 后端 | 每轮输入形态 | 放大效应 |
|---|---|---|
| Claude Code | `--resume+--fork-session` 原生续接，网关 prompt cache（cacheRead 单价 ~0.1×）摊薄大头 | 最小 |
| PiAgent | `--session-id` 磁盘会话续接，同 claude 量级 | 小 |
| DSH | 长驻进程同 sid 续轮，服务端会话在进程内延续 | 小（跨进程回灌时同 codex） |
| Codex | **每轮全新会话 + AutoCode 回灌最近对话**（`CTaskOrchestrator` 回灌缓存，2026.09.02 起） | 轮次越多输入越大（近似随轮数增长） |
| Hermes | 同 Codex 回灌 | 同上 |

成本控制配套（与后端正交）：回灌结构带防带偏补句且**长文（规划/验证）不进回灌缓存**；上下文进度条超 70% 变红、右键「立即压缩上下文」手动压缩；提示词长度闸门（超上下文 5% 自动提炼后投递）。

**排序（省 token）**：Claude Code ≈ PiAgent ≈ DSH ＞ Codex ≈ Hermes
**实操**：预期几十轮以上的长助手会话，尽量用前三者；用 Codex/Hermes 跑长链路时勤开新助手分段，避免回灌滚雪球。

---

## 5. 工具调用能力

| 后端 | 工具面 | deny/只读 | 技能挂载 |
|---|---|---|---|
| Claude Code | 原生全套：Read/Write/Edit/Bash/Glob/Grep/WebSearch/WebFetch/Task 子代理等 + **MCP**（ask_user 手工审批、sandbox_* 沙箱 MCP 常驻） | `--disallowedTools` **硬禁**（只读轮禁 Bash/Edit/Write） | 原生技能系统（SKILL.md） |
| PiAgent | 成熟 coding-agent 全套（同 Claude 量级） | `--exclude-tools` **硬禁**（等价映射） | `--skill` 依次挂载共享技能目录 |
| DSH | bash(pwsh) + fs + fs-search(glob/grep) + **todo** + **subagent** + sessions + token-meter（`agent/dsh/cordis.autocode.yml` 可查全表） | 工具默认放行（无人值守预设）；只读为提示词软约束 | `DSH_SKILLS_DIRS` env → cordis customSkillDirs |
| Codex | command 执行 / file_change（带 diff）/ MCP server 工具 / web_search 四类事件映射 | `-s` 沙箱档（AutoCode 用 danger-full-access）；只读软约束 | 无原生机制——AutoCode 把技能清单前置注入 prompt（`SharedSkillsCatalog`），模型命中后自行读 SKILL.md |
| Hermes | hermes-agent 自带 bash/文件工具 | 提示词软约束 | 同 Codex 清单注入 |

三态权限（自动审批/手工审批/只读）**全后端支持**：手工审批弹窗 Claude Code 走 ask_user MCP；其余四后端由助手运行 `%AUTOCODE_ASK_EXE% --ask` 命令经管道回传、主程序弹窗（`InjectAskEnv`）；只读轮 Claude/Pi 工具硬禁，DSH/Codex/Hermes 提示词约束。

**排序**：Claude Code ≈ PiAgent ＞ DSH ＞ Codex ＞ Hermes
**实操**：依赖技能（ui-sandbox GUI 测试等）的任务优先 Claude/Pi/DSH；Codex/Hermes 技能靠清单自取，弱模型命中率低，复杂 GUI 验证任务不建议。

---

## 6. 任务拆解能力

任务拆解的主体是 **AutoCode 十二阶段流水线**（需求→确认→评审→设计→计划→开发→审查→验证→测试…，确定性 C++ 状态机，不靠 LLM 路由），harness 层的差异在**轮次控制与续跑支撑**——拆得动还要跑得完：

| 后端 | 轮次上限 | 上限/中断续跑 | 压缩兜底 | 自主拆解工具 |
|---|---|---|---|---|
| Claude Code | `--max-turns` 硬上限 | 命中报 `error_max_turns` → 上层 `--resume` 自动续跑（「已完成部分已落盘，自动续跑」） | `--autocompact`（自动探测降级） | Task 子代理 / TodoWrite |
| PiAgent | 忽略 maxTurns | `stopReason=="length"` 映射 `error_max_turns` 自动续跑 | pi 原生 compaction | pi 内建 |
| DSH | 忽略 maxTurns | `turn/end reason=max-tokens` 映射续跑（补丁 `maxTokensAsSuccess: false` 保证） | 内置 compaction-basic（阈值 0.8/保留 0.16） | todo 工具 + subagent 工具 |
| Codex | 无轮次上限概念 | 无 AutoCode 层续跑；上下文逼近窗口时报 "ran out of room" 类错误（长输出截断误报已由 max_tokens 代理修复一类） | 无 | codex 自主迭代 |
| Hermes | 忽略 maxTurns | 无续跑映射 | 无（ctx 钳位 200K） | hermes 内部迭代循环 |

**排序（拆解+跑完的支撑度）**：Claude Code ＞ PiAgent ≈ DSH ＞ Codex ＞ Hermes
**实操**：大任务（多文件、长链路）交给有轮次续跑+自动压缩的后端；用 Codex/Hermes 时把任务拆小、一次一条指令。

---

## 7. 多轮对话的智力保持

| 后端 | 机制 | 实效 |
|---|---|---|
| Claude Code | `KeepsContextAcrossTurns()==true`：`--resume` + `--fork-session` 产生携带**完整上下文**的新 sid 分支（fork 不动原会话，撤销链安全） | 最好；另有悬空会话自愈（工程拷机后 sid 不在本机 → 自动重建重发，`m_bResumeDropped`） |
| PiAgent | 同 true：`--session-id` 磁盘会话**跨进程**续接（AutoCode 重启后仍可续） | 与 claude 同级 |
| DSH | 进程内同 sid 续轮记忆完整；**跨进程协议限制必须换新 sid**（持久化层要求磁盘日志是 live 会话前缀），上下文由编排器回灌弥补（防带偏结构） | 进程活得好=完整；重启后降级为回灌水平 |
| Codex | 每轮全新会话（忽略 resumeSid），全靠回灌 | 回灌只带最近对话（预算控制），「翻旧账」能力弱 |
| Hermes | 同 Codex | 同上 |

通用兜底（与后端无关）：`guide.md`（工程指南）/ `record.md`（修改日志）/ `memory.md`（任务经验）三份工程记忆文件每轮注入，跨会话的关键事实不依赖对话上下文。

**排序**：Claude Code ≈ PiAgent ＞ DSH ＞ Codex ≈ Hermes
**实操**：需要引用十几轮之前细节的连续任务（“还是按之前那个方案改”）务必用 Claude/Pi/DSH；每轮相对独立的任务用谁都不吃亏。

---

## 8. 一次成功率

> 口径说明：此处评的是 **AutoCode 集成层面的稳定性**（进程、协议、自愈、错误恢复），不是模型智力；各后端历史问题多数已修复，评的是当前版本的剩余风险面。

| 后端 | 稳定性设计 | 历史坑（均已修/已锁） |
|---|---|---|
| Claude Code | 最厚：快速失败自动重试（2 次）+ **孤儿宽限期**（cmd 被杀 node 还在跑，轮询 transcript 判定真收敛）+ Windows Job Object 进程树整杀 + 鉴权 401 自诊断（网关直探 + 大包级联探测）+ 悬空会话自愈 + `--autocompact` 兼容探测降级 | Node < 22 装包失败（install_env 已挡）；网关余额/401 类（诊断信息直出） |
| PiAgent | RPC 早夭自动回退一次性形态（升级/半安装自愈，用户无感）；429 限流自愈（`settings.retry` 15s 指数退避跨配额滚动窗）；settings 锁失败自愈重拉 | models.json 槽位被清（自愈重写）；pi 静默吞 API 错误（已带出） |
| DSH | **版本不兼容自动修复流水线**（锁→回滚基线→清残留→pnpm 重建→重入，约 5-15 分钟，每进程限一次）；credentials 坏格式自动迁移（备份 .autosave）；握手上限一次重试 | 新版 provider adapter 全灭问题（GLM_CUSTOM_API_KEY 兜底已注入）；cordis.yml 不可写即崩（自愈/提权修复） |
| Codex | 版本锁 0.87.0 杜绝升级破坏；本地代理修掉 chat wire 不带 max_tokens 导致的长输出截断误报；槽位丢失（多实例竞态写空 config.toml）自愈重写 | 半安装态（npm install 即修）；只认 chat 端点（选错条目类型即失败，UI 已约束） |
| Hermes | exe 形态 wal mtime 心跳旁路（`state.db-wal` 前移视为存活，防纯长生成误判 stall）；api_mode 按条目自动写入 config.yaml；长 prompt `@file` 投递绕命令行 32767 上限 | 上游 `-z` 全静音（oneshot 补丁 v1→v6 已接通工具卡/进度/ctx 快照）；HERMES_INFERENCE_MODEL provider 劫持（已禁注）；**上游改版可能令补丁锚点失配 → 安全回退静默形态**（仍可用但无过程反馈） |

**排序**：Claude Code ＞ PiAgent ＞ DSH ≈ Codex ＞ Hermes
**实操**：无人值守跑大任务用 Claude Code；异机部署首次使用前点「验证所有后端」（真实进程级探测：Claude/Pi/Codex/Hermes 拉 `--version`，DSH 做 initialize 握手）。

---

## 9. 跨进程会话恢复（附加维度）

AutoCode 重启 / 进程被杀后，能否凭 sessionId 续上原对话：

1. **PiAgent**（最佳）：磁盘会话跨进程续接，重启无损。
2. **Claude Code**：会话存 `~/.claude/projects/*.jsonl`，同机可续；**跨机器拷贝工程必丢**（会话不随工程走）→ 自愈重建重发本轮。
3. **DSH**：跨进程必须换新 sid + 回灌（协议限制）。
4. **Codex / Hermes**：无会话持久化，重启即全新（回灌兜底）。

**排序**：PiAgent ＞ Claude Code ＞ DSH ＞ Codex ≈ Hermes

---

## 10. 实时计量与费用统计（附加维度）

| 后端 | 输入/输出/请求 | 流式「本轮 ↑↓」 | 上下文占用条 |
|---|---|---|---|
| Claude Code | 逐条 assistant usage 实时（`sigUsageDelta`） | ✅ token 级（`sigLivePartial`） | ✅ 实时（三输入项之和） |
| PiAgent | 同 claude（usage 四分量累计） | ✅ | ✅ |
| DSH | 逐条 assistant usage 实时 | ❌（消息级） | ✅（脏值 >2×窗口丢弃） |
| Codex | **本地代理解析网关真实 usage**（含请求数实时） | ❌ | ✅（cached_input 计入） |
| Hermes | 事件 usage（终局 + 过程） | ❌ | ✅（v6 补丁 tool.started ctx 快照；修复前整轮恒「—」） |

费用折算：四后端共享单价表（$/M，四分量），全 0 则费用列显示 "-"；claude 可跟随共享表（仅 Anthropic 兼容端点）或走自身计费。

**排序**：Claude Code ≈ PiAgent ≈ DSH ≈ Codex ＞ Hermes

---

## 11. 过程反馈与可视化（附加维度）

日志区能看到多少“正在发生什么”：

- **Claude Code / PiAgent**：思维流文本 + 工具卡（Edit 红绿 diff）+ token 级流式，反馈最完整。
- **Codex**：agent_message 整块收尾上屏 + 工具事件块（command/file_change/MCP/web_search），块级节奏。
- **DSH**：消息级事件；工具调用前的思维链会以文本铺开（UI 已做折叠合并：连续工具调用收敛为一张组卡，不打断合并）。
- **Hermes**：依赖 oneshot 补丁（v4 中间段文本整段上抛、v2 progress 4 秒节流聚合、v3 工具卡带脱敏参数预览与耗时、v6 ctx 快照）；补丁未敷设/锚点失配时回退**全静默**，只有终局结果。

**排序**：Claude Code ≈ PiAgent ＞ Codex ≈ DSH ＞ Hermes（补丁后）；Hermes 静默形态垫底

---

## 12. 停滞检测与自愈（附加维度）

五后端统一 30s 一拍 stall 巡检（阈值 `[Agent] StallMinutes` 默认 10 分钟；挂 ask_user 等待用户作答的轮不启用）：

- **Claude Code**：零输出超阈值 → kill + `subtype="stall"` → 上层凭 sessionId 自动续跑；孤儿宽限期额外兜住「进程表面死了实际还在干活」。
- **DSH**：同款 stall → kill → 快速失败重试（重启进程重发，上限 2 次）。
- **PiAgent**：stall 巡检 + 流式心跳提示（message_update 增量不算完整事件，60s 节流提示「仍在生成」防误判卡死）。
- **Codex**：stall 巡检（一次性进程，无续跑）。
- **Hermes**：exe 形态 stdout 全静默期靠 **wal 心跳旁路**判活（真挂=wal 停走）；纯长生成阶段靠阈值 3 倍窗口兜底。

**排序**：Claude Code ＞ DSH ≈ PiAgent ＞ Hermes ＞ Codex

---

## 13. 安全与审批（附加维度）

- **沙箱**：DSH 独有强沙箱——pwsh 执行 workspace-write 模式下 NTFS DACL + Low 完整性标签双条件强制（拒绝由 stderr 特征分类，无路径预检）；可写根=工程根，关联项目目录经 `[Dsh] ExtraWriteDirs` 幂等授 ACE（`ApplyExtraWriteDirs`）；切 danger-full-access（缺省）则沙箱整段跳过。Codex 用 `-s` 档（AutoCode 缺省 danger-full-access）。Claude Code 无进程沙箱，靠 sandbox_* MCP 技能（Ghost 窗口 UI 测试沙箱）补位。
- **审批三态**（自动/手工/只读，随助手记忆，缺省自动）：全后端支持，见 §5。
- **密钥安全**：五后端密钥一律不进命令行/日志（env 注入或写 `~/.codex/auth.json`、`~/.pi/agent/models.json`、`~/.dsh` 凭据阶梯、`~/.hermes`/config.yaml）。

**排序（安全隔离）**：DSH ＞ Claude Code ≈ PiAgent ＞ Codex ≈ Hermes

---

## 14. 安装部署与版本维护成本（附加维度）

| 后端 | 安装方式 | 版本策略 | 维护风险 |
|---|---|---|---|
| Codex | npm 本地工程 / 离线 robocopy 拷贝安装源（无需网络/管理员） | **锁 0.87.0** 永不升级 | 最低（锁死即稳定） |
| Claude Code | `install_env.bat` 自动装 Git/Node/npm 包 | 跟随 npm latest | Node ≥ 22 硬要求 |
| PiAgent | 源码树或 npm 全局 | 跟随 | RPC 形态依赖 pi 版本带 steer（旧版自动回退一次性形态） |
| Hermes | git 克隆 + pnpm/构建（联网）或 robocopy（离线）；Python venv 双形态 | 日历发布 tag（v2026.M.D）比对 | **上游改版可致 oneshot 补丁锚点失配**（安全回退静默形态）；更新后需重敷补丁（AutoCode 已自动兜底） |
| DSH | git + pnpm 构建（可选组件） | **闸门 ≤ dsh-v0.1.5-alpha.1**，高于即「不受支持」并触发自动回滚重建 | 上游演进快；更新后 pnpm 全量重建 5-15 分钟 |

**排序（维护省心）**：Codex ＞ Claude Code ＞ PiAgent ＞ DSH ≈ Hermes

---

## 15. 模型接入灵活性（附加维度）

- **四后端共享一张模型表**（设置 → 模型页；`[DshModels]/[PiModels]/[CodexModels]/[HermesModels]` 合并归一）：切换后端无需重配模型。API 类型三选：anthropic-messages（DSH/Pi/Hermes 支持）、chat（DSH/Pi/Hermes/Codex）、responses（Codex）。
- **Codex 只能连 chat completions 兼容端点**（anthropic-messages 条目不可设为 Codex 当前模型）；「最大输出」≤ 131072（2^17，网关上限，超限全后端请求失败——Hermes/Codex 侧已钳位）。
- **Claude Code**：默认走自身登录/网关配置；可开「后端跟随共享表」（仅 Anthropic 兼容端点条目生效，每轮 env 注入 BASE_URL/KEY/MODEL + `--settings` 兜底）。
- **对话级模型覆盖**：各助手上下文条右键「选择模型（仅本助手）」——五后端（含 Claude）都支持，下一轮立刻生效、不改全局缺省。
- 思考强度各页可设：DSH off/low/high/max、Pi thinking off~high、Codex minimal~xhigh、Hermes false~ultra（不兼容组合自动去 effort 重试自愈，见 `m_bEffortDropped`）。

**排序**：PiAgent ≈ DSH ≈ Hermes ＞ Codex ＞ Claude Code（受 Anthropic 协议约束）

---

## 16. 选型速查

| 场景 | 推荐 | 理由锚点 |
|---|---|---|
| 任务流水线（十二阶段大工程） | **Claude Code**（备选 PiAgent） | §5/§6/§8 全维最优；轮次续跑+自愈最厚 |
| 边跑边纠偏（频繁介入） | **Claude Code / PiAgent** | §1 唯二轮中真注入 |
| 国产/内网网关模型（GLM 等） | **DSH / Hermes / Pi** | §15 共享模型表 anthropic-messages 直连 |
| 需要沙箱隔离（怕改坏） | **DSH**（workspace-write） | §13 唯一进程级强沙箱 |
| OpenAI 系模型 | **Codex** | §15 chat/responses；锁版最稳 |
| 超长多轮会话（几十轮+） | Claude / Pi / DSH | §4/§7 回灌放大小、原生续接 |
| 无人值守过夜任务 | Claude Code | §8 自愈最全；§12 stall 续跑 |
| 快问快答 / 小改动 | Hermes / PiAgent | 启动快、按轮计、无历史包袱 |
| 异机部署、多电脑轮换 | PiAgent | §9 磁盘会话随工程可续（配合会话自愈） |

## 17. 常见误区

1. **「Codex/Hermes 发了提示词没反应」**——不是丢了，是排队了：日志有「已排队追加指令（本轮结束后自动发送）」事件行，本轮结束自动续发（多条合并一次投送）。要立即送达换 Claude/Pi。
2. **「Codex 报上下文用完」**——先确认「最大输出」条目 ≤131072；长输出截断误报已由本地代理修，若仍复现多半是模型窗口真的满了（回灌+长输出），换 DshPrice 同款思路：拆小任务。
3. **「Hermes 日志全静默」**——oneshot 补丁未敷设或上游更新冲掉：选 Hermes 后端保存/StartStage 时 AutoCode 自动重敷（`EnsureHermesOneshotPatch`）；若上游改版锚点失配会安全回退静默形态（功能仍可用，只是没过程反馈）。
4. **「DSH 写不了关联项目目录」**——pwsh 沙箱 DACL 所致：设置 `[Dsh] ExtraWriteDirs` 放行该目录（AutoCode 自动授 ACE + Low 标签），或切 danger-full-access。
5. **「DSH 升级后起不来」**——集成闸门在 dsh-v0.1.5-alpha.1：新版移除了 AutoCode 依赖的组合文件；启动时会自动回滚重建（5-15 分钟），失败按日志手动 `git reset --hard` 即可。
6. **「Claude 换了电脑报 No conversation found」**——会话不随工程走：已自动降级新建会话重发，无需处理；要跨机带会话用 PiAgent。
7. **速度慢先查模型再查框架**——先在两台机器/两个模型间对比同一条指令，排除模型吞吐后再看 §3 的框架开销项。

---

*本手册基于 2026-10-26 代码库（agent/ 五 Runner、orch/CTaskOrchestrator、agent/dsh 组合层、scripts/hermes_oneshot_patch.py）与部署实测记录编写；后端升级后请对照 harness.md 的版本闸门复核 §14。*
