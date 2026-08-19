# agent/dsh — DeepSeekHarness 后端部署物

## 文件

- `cordis.autocode.yml` — AutoCode 专用插件组合的**版本管理正本**。

## 正本 → 运行副本

DeepSeekHarness 的启动器以**配置文件所在目录**为模块解析基址（裸插件名
`@deepseek-ai/dsh-*` 从该目录的 node_modules 解析）。DSH 仓库根的
node_modules 不含全量包，只有 `examples/`（union manifest，全插件 symlink）
能解析完整插件树。因此**运行副本必须落在 DSH 仓库的 `examples/` 树内**：

```
运行副本: <DSH仓库>/examples/autocode-agent/cordis.yml
```

修改组合时：改这里的正本，然后手工同步（复制）到上述运行副本路径。
不做构建期自动拷贝——方向是跨仓库写入，且拷到 AutoCode 的 Execute/ 下
无法解析插件，反而制造假象。

## 实测结论（2026-08-18，dsh v0.1.0-rc.7，Windows）

- `tool-fs-search` 必须带 `config.sampleOverCapGlobResults`（必填项，缺失
  会让整棵插件树加载失败，表现为 initialize 报 no adapter registered）。
- `dsh-permission-presets` 不可组入：它等待 `approval` 服务（交互问询），
  无人值守组合会卡在 pending。工具默认放行（与 claude 后端全开语义一致）。
- `dsh-settings-file` 的 settings 分节在 jsonrpc 组合下没有把
  `llm-pi-ai:` 分节送达休眠的 pi-ai 适配器（原因未深究）；**provider
  profile 直接内联在 llm-pi-ai 条目里**，组合自包含。
- 凭据链有效：`credentials-local` 从 `~/.dsh/.credentials.yaml` 解析
  `GLM_CUSTOM_API_KEY`。
- 事件信封：`session.event` 的 payload 直接内联（无 data 包裹）；
  `assistant/message = {turn, step, message:{content:[...]}, usage?}`；
  `tool/call = {turn, step, callId, name, arguments(原始JSON串)}`；
  `turn/end = {turn, reason:{kind: completed|aborted|blocked|error|max-tokens|interrupted}}`。
  `assistant/chunk` 为 token 级流（量大，AutoCode 侧应过滤）。
- **跨进程同 sessionId 会撞持久化层 id collision**（SDK server 只
  `agents.create`，不暴露 `agents.resume`）。会话多轮仅在**同一进程内**
  有效；AutoCode 侧进程重启后 resumeSid 应换新 sid（上下文靠 prompt 回灌）。
- 同进程多轮记忆、tool-fs 读写、usage（inputTokens/outputTokens/
  cacheReadTokens 三项可算上下文占用）均验证通过。

## 探针

DSH 仓库 `examples/autocode-agent/probe.py` 为端到端探针（真实调用模型），
覆盖握手/事件流/工具/多轮/跨进程冲突预期。
