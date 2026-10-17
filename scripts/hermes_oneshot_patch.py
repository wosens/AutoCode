# -*- coding: utf-8 -*-
"""AutoCode Hermes oneshot 事件补丁敷设器 v6（软编码源，2026.09.04 v2.0.117 起，2026-10-16 v6）。

用法：python hermes_oneshot_patch.py <HermesHome>
      （缺省回落 HERMES_HOME 环境变量）

给 <home>\\hermes-agent\\hermes_cli\\oneshot.py 幂等敷设 HERMES_ONESHOT_EVENTS
开关：上游 hermes -z 一次性模式刻意全静音（stdout/stderr 重定向 devnull +
流式回调置 None，无 CLI 开关）。补丁接通 AIAgent 现成回调，往重定向前捕获的
真 stdout 打 NDJSON 事件，AutoCode Hermes 后端按 JSON 契约解析：

  v1（兼容保留）：tool（工具名）/ result（usage/session/apiCalls 终局计量）
  v2：progress（stream_delta 节流聚合——生成段字数+尾部预览+心跳）
  v3：tool.started 带脱敏参数 preview / tool.completed 带耗时/错误/结果 /
      think（思考流）
  v4：中间段文本以 {"type":"assistant"} 整段上抛（pi/claude 式阶段叙述块）
  v5：`-z @<file>` 提示词文件展开——Windows CreateProcess 命令行
      32767 字符上限（AutoCode 回灌缓存轻易超限，「文件名或扩展名太长」启动
      失败）；AutoCode 长提示词写 UTF-8 临时文件传 @path，本补丁在
      run_oneshot 入口展开；文件缺失保持原文（字面 @ 开头提示词不受影响）。
  v6（新增）：tool.started 附带 {"type":"ctx","fill":N} 占用快照（取
      agent._last_turn_usage.prompt_tokens——每次 provider 响应后刷新，
      tool.started 时恰为最新响应的在窗 token）。修实害：v5 前只有终局
      result 携 usage，且值是本轮各请求累计和（非在窗占用），AutoCode 上下文条
      整轮无实时数据、轮末又被超窗钳制成「—」（pdfplumber「修界面交互缺陷」
      Hermes 助手 2417 块 ctxFill 恒 -1 实锤）。

软编码约定：本文件是补丁的唯一正本（随 exe 部署在 install_env.bat 同级），
改补丁只需改本文件、无需重编 AutoCode；由 install_env.bat /hermes-patch 调用
（程序侧在选择了 Hermes 后端保存时与 StartStage 兜底时自动执行）。

幂等/升级：v5→v6 / v4→v6 / v3→v6 / v2→v6 / v1→v6 / 全新敷设 / 已 v6 跳过；锚点失配
（上游改版）不写入、exit 1（调用方安全回退静默形态）。
"""
import os
import re
import sys

MARKER_V1 = "AutoCode events patch v1"
MARKER_V2 = "AutoCode events patch v2"
MARKER_V3 = "AutoCode events patch v3"
MARKER_V4 = "AutoCode events patch v4"
MARKER_V5 = "AutoCode events patch v5"
MARKER_V6 = "AutoCode events patch v6"

V4_HOOK = '''        # --- AutoCode events patch v4: interim text + rich process events ---
        # v4: interim segment text emitted as {"type":"assistant"} when a tool
        # starts (final answer segment is never flushed - response prints at
        # exit, no duplication). tool_gen (v2) retired: "tool.started" is a
        # superset (redacted preview).
        import time as _ac_time

        _AC_PROG = {"chars": 0, "last": 0.0, "tail": "", "text": ""}

        def _ac_on_delta(_text):
            try:
                _AC_PROG["chars"] += len(_text)
                _AC_PROG["tail"] = (_AC_PROG["tail"] + _text)[-64:]
                if len(_AC_PROG["text"]) < 4000:
                    _AC_PROG["text"] += _text   # full interim segment (v4)
                _now = _ac_time.monotonic()
                if _now - _AC_PROG["last"] >= 4.0:
                    _AC_PROG["last"] = _now
                    _autocode_events_write({"type": "progress",
                                            "chars": _AC_PROG["chars"],
                                            "tail": _AC_PROG["tail"]})
            except Exception:
                pass

        def _ac_on_tool_progress(event, name, preview, args, **kw):
            try:
                if event == "tool.started":
                    # v4: flush the finished interim segment as a regular
                    # assistant message (pi/claude-style step narration).
                    if _AC_PROG["text"].strip():
                        _autocode_events_write({"type": "assistant",
                                                "text": _AC_PROG["text"][:4000]})
                    _AC_PROG["chars"] = 0   # new segment after a tool call
                    _AC_PROG["tail"] = ""
                    _AC_PROG["text"] = ""
                    _autocode_events_write({
                        "type": "tool", "tool": str(name),
                        "preview": str(preview or "")[:160]})
                    # --- AutoCode events patch v6: live context occupancy ---
                    # agent._last_turn_usage is refreshed after every provider
                    # response (turn_usage.record_response_usage); tool.started
                    # fires right after the response that decided to call this
                    # tool, so its prompt_tokens is the current in-window fill
                    # (input+cacheRead+cacheWrite as fallback).
                    try:
                        _ud = getattr(agent, "_last_turn_usage", None) or {}
                        _fill = int(_ud.get("prompt_tokens") or 0)
                        if _fill <= 0:
                            _fill = (int(_ud.get("input_tokens") or 0)
                                     + int(_ud.get("cache_read_tokens") or 0)
                                     + int(_ud.get("cache_write_tokens") or 0))
                        if _fill > 0:
                            _autocode_events_write({"type": "ctx", "fill": _fill})
                    except Exception:
                        pass
                    # --- end AutoCode events patch v6 ---
                elif event == "tool.completed":
                    _autocode_events_write({
                        "type": "tooldone", "tool": str(name),
                        "ms": int((kw.get("duration") or 0) * 1000),
                        "err": bool(kw.get("is_error")),
                        "result": str(kw.get("result") or "")[:200]})
                elif event == "_thinking":
                    _autocode_events_write({"type": "think",
                                            "text": str(name)[:160]})
                elif event == "reasoning.available":
                    _autocode_events_write({"type": "think",
                                            "text": str(preview or "")[:200]})
            except Exception:
                pass

        if _AUTOCODE_EVENTS_OUT is not None:
            agent.stream_delta_callback = _ac_on_delta
            agent.tool_progress_callback = _ac_on_tool_progress
            agent.tool_gen_callback = None
            agent.tool_start_callback = None
            agent.tool_complete_callback = None
        else:
            agent.stream_delta_callback = None
            agent.tool_gen_callback = None
        # --- end AutoCode events patch v4 ---
'''

# v5：run_oneshot 入口的 @<file> 提示词展开
V5_PROMPT_EXPAND = (
    "    # --- AutoCode events patch v5: '@<file>' prompt expansion ---\n"
    "    # Windows CreateProcess caps the command line at 32767 chars; AutoCode\n"
    "    # writes large prompts (session replay cache) to a UTF-8 temp file and\n"
    "    # passes '@<path>'. Expand here; a missing file keeps the text as-is\n"
    "    # (a literal '@'-leading prompt still works).\n"
    "    if isinstance(prompt, str) and prompt.startswith('@') and len(prompt) > 1:\n"
    "        try:\n"
    "            with open(prompt[1:], 'r', encoding='utf-8') as _ac_pf:\n"
    "                prompt = _ac_pf.read()\n"
    "        except OSError:\n"
    "            pass\n"
    "    # --- end AutoCode events patch v5 ---\n"
)

I1 = '''# --- AutoCode events patch v1 (idempotent, do not remove) ---
# HERMES_ONESHOT_EVENTS=1 (set by AutoCode Hermes backend): emit NDJSON
# events to the REAL stdout (captured before redirect_stdout(devnull)):
#   {"type":"tool","tool":<name>}   live tool cards / heartbeat
#   {"type":"result",...,\"usage\":{...}}   token stats / session id
#   {"type":"progress","chars":N,"tail":...}   v2: streaming generation
#   {"type":"tooldone"/"think",...}            v3: tool results / thinking
#   {"type":"assistant","text":...}            v4: interim step narration
# Upstream keeps -z silent by design; this opt-in side channel only writes
# via _autocode_events_write, never touches sys.stdout under the redirect.
_AUTOCODE_EVENTS = os.getenv("HERMES_ONESHOT_EVENTS", "").strip() not in ("", "0")
_AUTOCODE_EVENTS_OUT = None


def _autocode_events_bind(stream) -> None:
    global _AUTOCODE_EVENTS_OUT
    _AUTOCODE_EVENTS_OUT = stream if _AUTOCODE_EVENTS else None


def _autocode_events_write(payload: dict) -> None:
    if _AUTOCODE_EVENTS_OUT is None:
        return
    try:
        import json as _json

        _AUTOCODE_EVENTS_OUT.write(_json.dumps(payload, ensure_ascii=False) + "\\n")
        _AUTOCODE_EVENTS_OUT.flush()
    except Exception:
        pass
# --- end AutoCode events patch v1 ---
'''

I4 = '''    # AutoCode events patch v1: result event with usage/session/api_calls
    _autocode_events_write({
        "type": "result",
        "subtype": "success" if not (result.get("failed") or result.get("partial")) else "error",
        "sessionId": str(result.get("session_id") or ""),
        "model": str(result.get("model") or ""),
        "usage": {
            "input": result.get("input_tokens") or 0,
            "cacheRead": result.get("cache_read_tokens") or 0,
            "cacheWrite": result.get("cache_write_tokens") or 0,
            "output": result.get("output_tokens") or 0,
        },
        "apiCalls": result.get("api_calls") or 0,
    })
'''

I5 = '''        _autocode_events_write({"type": "result", "subtype": "error",
                                "sessionId": str(result.get("session_id") or ""),
                                "usage": {}})
'''

# v1 工具块（v1→v5 升级需整体移除，文本须与 v1 敷设产物逐字节一致）
V1_TOOL_BLOCK = '''        # AutoCode events patch v1: live tool events (heartbeat + tool cards)
        if _AUTOCODE_EVENTS_OUT is not None:
            agent.tool_gen_callback = (
                lambda name: _autocode_events_write({"type": "tool", "tool": str(name)})
            )
'''

V2_HOOK_START = "        # --- AutoCode events patch v2: streaming progress (throttled deltas) ---\n"
V2_HOOK_END = "        # --- end AutoCode events patch v2 ---\n"
V3_HOOK_START = "        # --- AutoCode events patch v3: rich process events ---\n"
V3_HOOK_END = "        # --- end AutoCode events patch v3 ---\n"

# run_oneshot 入口锚（docstring 结束行 + 首行注释）
# 2026.09.13 锚点自适应（上游 v0.21.2 改版实测）：docstring 结束行措辞在
#   "Returns the exit code.  The caller owns..."（旧）与 "by this turn. Returns
#   the exit code; the caller owns..."（v0.21.2）间漂移 - 改正则双版本兼容。
A6_TAIL = "    # Silence every stdlib logger for the duration.  AIAgent, tools, and\n"
A6_RE = re.compile(
    r'(?:by this turn\.\s+)?Returns the exit code[.;]?\s+'
    r'[Tt]he caller owns process termination\.\r?\n    """\r?\n')


def _a6_match(src: str):
    """A6 锚点自适应匹配：返回 (前段, 后段) 或 None。前段＝docstring 结束行。"""
    m = A6_RE.search(src)
    if m is None:
        return None
    return m.group(0), m.end()


def with_v5(s: str) -> str:
    """确保 @<file> 展开段存在（v4 钩子之外的独立插入点）。"""
    if MARKER_V5 in s:
        return s
    m = _a6_match(s)
    if m is None or len(A6_RE.findall(s)) != 1:
        print("A6 anchor mismatch for v5 insert (upstream oneshot.py revised?)")
        sys.exit(1)
    head, tail_pos = m
    return s[:tail_pos] + V5_PROMPT_EXPAND + s[tail_pos:]


# v6 升级锚：v4 钩子内 tool.started 的 tool 事件发射块（v4/v5 敷设产物中唯一）
V6_ANCHOR = ('                    _autocode_events_write({\n'
             '                        "type": "tool", "tool": str(name),\n'
             '                        "preview": str(preview or "")[:160]})\n')

# v6 块文本：直接从 V4_HOOK 内嵌副本提取（单一事实源——升级插入与全新敷设字节一致）
_V6_START = "# --- AutoCode events patch v6"
_V6_END = "# --- end AutoCode events patch v6 ---"
_i6 = V4_HOOK.index(_V6_START)
_j6 = V4_HOOK.index(_V6_END, _i6) + len(_V6_END)
V6_CTX_BLOCK = V4_HOOK[_i6:_j6] + "\n"


def with_v6(s: str) -> str:
    """v5→v6：tool.started 后补发 ctx 占用快照（幂等；v6 块文本与 V4_HOOK 内嵌一致）。"""
    if MARKER_V6 in s:
        return s
    if s.count(V6_ANCHOR) != 1:
        print(f"v6 upgrade anchor mismatch: {s.count(V6_ANCHOR)} "
              "(upstream oneshot.py revised?)")
        sys.exit(1)
    i = s.find(V6_ANCHOR) + len(V6_ANCHOR)
    return s[:i] + V6_CTX_BLOCK + s[i:]


def install_fresh(src: str) -> str:
    """全新敷设（无任何标记的原始上游文件）。"""
    A1 = "from hermes_cli.fallback_config import get_fallback_chain\n"
    A2 = "    real_stdout = sys.stdout\n    real_stderr = sys.stderr\n"
    A3 = ("        agent.suppress_status_output = True\n"
          "        agent.stream_delta_callback = None\n"
          "        agent.tool_gen_callback = None\n")
    # A4 改用「不变 write 块」锚（2026.09.13）：上游 v0.21.2 起 `if response:` 前
    #   插入了 UTF-16 代理对清洗块（两种措辞变体），前缀不再稳定；write 块本身
    #   在全部已知变体中逐字节不变且唯一，I4 插在 write 块后 = 原插入位。
    A4 = ("        real_stdout.write(response)\n"
          "        if not response.endswith(\"\\n\"):\n"
          "            real_stdout.write(\"\\n\")\n"
          "        real_stdout.flush()\n")
    A5 = ('        real_stderr.write(f"hermes -z: agent failed: {failure}\\n")\n'
          "        real_stderr.flush()\n")
    for s_name, s_anchor in (("A1", A1), ("A2", A2), ("A3", A3), ("A4", A4), ("A5", A5)):
        if src.count(s_anchor) != 1:
            print(f"anchor {s_name} mismatch: {src.count(s_anchor)} "
                  f"(upstream oneshot.py revised - update anchors per header)")
            sys.exit(1)
    m6 = _a6_match(src)
    if m6 is None or len(A6_RE.findall(src)) != 1:
        print("anchor A6 mismatch: 0 (upstream oneshot.py revised - update anchors per header)")
        sys.exit(1)
    src = src.replace(A1, A1 + I1)
    src = src.replace(A2, A2 + "    _autocode_events_bind(real_stdout)   # AutoCode events patch v1\n")
    src = src.replace(A3, "        agent.suppress_status_output = True\n" + V4_HOOK)
    src = src.replace(A4, A4 + I4)
    src = src.replace(A5, A5 + I5)
    # A6 用匹配到的原文做文本替换（不用偏移：上面 5 处插入已改变偏移）。
    head6 = m6[0]
    src = src.replace(head6, head6 + V5_PROMPT_EXPAND)
    return src


def upgrade_v1(src: str) -> str:
    """v1 → v4 钩子（@file 段由 with_v5 统一补）。"""
    U_NONE = "        agent.stream_delta_callback = None\n        agent.tool_gen_callback = None\n"
    if src.count(V1_TOOL_BLOCK) != 1 or src.count(U_NONE) != 1:
        print("v1 upgrade anchors mismatch")
        sys.exit(1)
    src = src.replace(V1_TOOL_BLOCK, "")
    src = src.replace(U_NONE, V4_HOOK)
    return src


def upgrade_v2(src: str) -> str:
    """v2 → v4 钩子。"""
    i = src.find(V2_HOOK_START)
    j = src.find(V2_HOOK_END, i)
    if i < 0 or j < 0:
        print("v2 hook block not found")
        sys.exit(1)
    return src[:i] + V4_HOOK + src[j + len(V2_HOOK_END):]


def upgrade_v3(src: str) -> str:
    """v3 → v4 钩子。"""
    i = src.find(V3_HOOK_START)
    j = src.find(V3_HOOK_END, i)
    if i < 0 or j < 0:
        print("v3 hook block not found")
        sys.exit(1)
    return src[:i] + V4_HOOK + src[j + len(V3_HOOK_END):]


def main() -> int:
    s_home = sys.argv[1] if len(sys.argv) > 1 else os.getenv("HERMES_HOME", "")
    if not s_home:
        print("usage: hermes_oneshot_patch.py <HermesHome> (or set HERMES_HOME)")
        return 1
    # 2026.09.13 双布局：官方 install.ps1 布局 <home>\\hermes-agent\\... 优先，
    #   回退旧平铺 <home>\\hermes_cli\\oneshot.py（install_env.bat 2026.09 旧装）。
    a_cand = [
        os.path.join(s_home, "hermes-agent", "hermes_cli", "oneshot.py"),
        os.path.join(s_home, "hermes_cli", "oneshot.py"),
    ]
    p = next((c for c in a_cand if os.path.isfile(c)), "")
    if not p:
        print(f"cannot find oneshot.py under {s_home} "
              f"(tried: {'; '.join(a_cand)})")
        return 1
    try:
        src = open(p, encoding="utf-8", newline="").read()
    except OSError as exc:
        print(f"cannot read {p}: {exc}")
        return 1
    if MARKER_V6 in src:
        print("already patched (v6)")
        return 0
    if MARKER_V5 in src:
        out, s_verb = with_v6(src), "upgraded v5 -> v6"
    elif MARKER_V4 in src:
        out, s_verb = with_v6(with_v5(src)), "upgraded v4 -> v6"
    elif MARKER_V3 in src:
        out, s_verb = with_v6(with_v5(upgrade_v3(src))), "upgraded v3 -> v6"
    elif MARKER_V2 in src:
        out, s_verb = with_v6(with_v5(upgrade_v2(src))), "upgraded v2 -> v6"
    elif MARKER_V1 in src:
        out, s_verb = with_v6(with_v5(upgrade_v1(src))), "upgraded v1 -> v6"
    else:
        out, s_verb = install_fresh(src), "patched OK (v6)"
    s_tmp = p + ".ac-tmp"
    with open(s_tmp, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    os.replace(s_tmp, p)
    print(s_verb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
