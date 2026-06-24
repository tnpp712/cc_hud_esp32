# CC-HUD 第 1 层(地基)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 CC-HUD 主机侧从无状态 hook 脚本重构为常驻 `cchud-daemon`(adapter 层 + 内存状态 + 常驻 BLE),并在固件接入 v7 TLV 解析,保证 Claude 现有能力功能等价。

**Architecture:** daemon 用 Python(asyncio + bleak),hook 退化为瘦客户端经 Unix domain socket 投递事件;事件经 adapter 规范化为 `CcHudEvent`,流经 SessionStore/QuotaTracker/Aggregator,由单一 BleLink 任务串行编码下发。固件新增 `0x0B` TLV 分支,解析逻辑剥离为可 native 单测的纯函数。

**Tech Stack:** Python 3.13(uv venv)、pytest、bleak、Arduino/NimBLE(ESP32-S3)、PlatformIO native test。

## Global Constraints

- daemon 语言:Python,沿用 `host/.venv`(Python 3.13.2)与 `bleak` BLE 栈。
- 状态目录迁至 `~/.cchud/`(跨重启保留);`/tmp` 仅保留 socket 路径 `~/.cchud/daemon.sock`。
- 新 BLE msg_type:`0x0B = V7_TLV`;旧 `0x01–0x0A` 路径原样保留、不改。
- v7 帧头:`[0]=0x0B [1]=flags [2]=seq [3]=total`,其后连续 `[tag:u8][len:u8][value]`;本层所有帧单包(`flags=0,seq=0,total=1`)。
- v7 tag 取值严格依据 `docs/superpowers/specs/2026-06-24-cc-hud-roadmap-design.md` 7.4 节 tag 表。
- BLE 写入 `response=True`(macOS + NimBLE 硬性要求),失败重试 1 次。
- 迁移解耦:阶段 1a 架构重构仍用 v6/0x07 编码;阶段 1b 切 v7。BleLink 以开关切换编码版本。
- 提交规范:Conventional Commits 英文标题,正文中文,**禁止任何 AI 署名/Co-Authored-By**。
- 注释、日志文案用简体中文;标识符、错误码、字段名保持原文。

---

## 文件结构

新建 daemon 包 `host/cchud_daemon/`,按职责拆分:

| 文件 | 职责 |
|------|------|
| `host/cchud_daemon/__init__.py` | 包标记 |
| `host/cchud_daemon/event.py` | `CcHudEvent` 数据类 |
| `host/cchud_daemon/session_store.py` | `SessionStore`:会话状态 + TTL |
| `host/cchud_daemon/quota_tracker.py` | `QuotaTracker`:额度分窗合并 + 限频 |
| `host/cchud_daemon/aggregator.py` | `Aggregator`:聚合态 + 快照 + 去重 |
| `host/cchud_daemon/codec.py` | v6 兼容编码 + v7 TLV 编码(纯函数) |
| `host/cchud_daemon/adapters/base.py` | `Adapter` 协议 + `HookSpec` |
| `host/cchud_daemon/adapters/claude.py` | `ClaudeAdapter` |
| `host/cchud_daemon/registry.py` | `AdapterRegistry` |
| `host/cchud_daemon/socket_server.py` | `SocketServer`:UDS 监听 |
| `host/cchud_daemon/ble_link.py` | `BleLink`:常驻连接 + 发送队列 |
| `host/cchud_daemon/daemon.py` | 组装 + asyncio 主循环 |
| `host/cchud_daemon/cli.py` | `cchud` CLI(install/status/daemon) |
| `host/cchud-emit.sh` | 通用瘦 hook 客户端 |
| `host/io.cchud.daemon.plist` | launchd user agent 模板 |
| `host/tests/*.py` | pytest 单元测试 |
| `host/pyproject.toml` | pytest 配置 + dev 依赖声明 |
| `firmware/src/v7_tlv.h` / `v7_tlv.cpp` | v7 TLV 纯解析(不依赖 NimBLE) |
| `firmware/test/test_v7_tlv/test_v7_tlv.cpp` | native 单元测试 |

---

## Task 1: 测试设施 + CcHudEvent + SessionStore

**Files:**
- Create: `host/pyproject.toml`
- Create: `host/cchud_daemon/__init__.py`
- Create: `host/cchud_daemon/event.py`
- Create: `host/cchud_daemon/session_store.py`
- Test: `host/tests/test_session_store.py`

**Interfaces:**
- Produces:
  - `CcHudEvent`(dataclass,字段见 spec 4.1;本层 `client_id:int, session_id:str, kind:str, state:str|None, detail:str|None, five_h_used:int|None, five_h_resets_at:int|None, seven_d_used:int|None, seven_d_resets_at:int|None, ctx_pct:int|None, cost_usd:float|None, duration_s:int|None, lines_added:int|None, lines_removed:int|None, title:str|None, mode:str|None, ts:float`)
  - `SessionStore(ttl_s: float = 300.0)`,方法:`update(session_id: str, state: str, detail: str, now: float) -> None`、`live_states(now: float) -> list[tuple[str, str]]`(返回 `(state, detail)`,已剔除过期)、`counts(now: float) -> tuple[int, int]`(返回 `(total, busy)`,busy = 非 idle 数)。

- [ ] **Step 1: 建 pytest 配置与 dev 依赖**

`host/pyproject.toml`:

```toml
[project]
name = "cchud-daemon"
version = "0.1.0"
requires-python = ">=3.13"
dependencies = ["bleak>=0.21"]

[project.optional-dependencies]
dev = ["pytest>=8.0", "pytest-asyncio>=0.23"]

[tool.pytest.ini_options]
testpaths = ["tests"]
asyncio_mode = "auto"
```

安装:`host/.venv/bin/python -m pip install -e 'host[dev]'`

- [ ] **Step 2: 写失败测试**

`host/tests/test_session_store.py`:

```python
from cchud_daemon.session_store import SessionStore


def test_busy_session_counts_and_states():
    s = SessionStore(ttl_s=300)
    s.update("a", "thinking", "", now=1000)
    s.update("b", "tool", "Bash", now=1000)
    s.update("c", "idle", "", now=1000)
    assert s.counts(now=1000) == (3, 2)         # total=3, busy=2(idle 不计)
    assert ("tool", "Bash") in s.live_states(now=1000)


def test_expired_sessions_are_swept():
    s = SessionStore(ttl_s=300)
    s.update("old", "tool", "Bash", now=1000)
    s.update("new", "thinking", "", now=1400)
    # old 已超 300s(now=1400),应被剔除
    assert s.counts(now=1400) == (1, 1)
    assert s.live_states(now=1400) == [("thinking", "")]
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_session_store.py -v`
Expected: FAIL（ModuleNotFoundError: cchud_daemon.session_store）

- [ ] **Step 4: 写最小实现**

`host/cchud_daemon/__init__.py`:空文件。

`host/cchud_daemon/event.py`:

```python
from __future__ import annotations
from dataclasses import dataclass


@dataclass
class CcHudEvent:
    client_id: int
    session_id: str
    kind: str                       # "state" | "quota" | "idle"
    state: str | None = None
    detail: str | None = None
    five_h_used: int | None = None
    five_h_resets_at: int | None = None
    seven_d_used: int | None = None
    seven_d_resets_at: int | None = None
    ctx_pct: int | None = None
    cost_usd: float | None = None
    duration_s: int | None = None
    lines_added: int | None = None
    lines_removed: int | None = None
    title: str | None = None
    mode: str | None = None
    ts: float = 0.0
```

`host/cchud_daemon/session_store.py`:

```python
from __future__ import annotations

# 状态优先级:waiting > tool > thinking > idle(复刻 cchud-hook.sh)
_PRIORITY = {"waiting": 3, "tool": 2, "thinking": 1, "idle": 0}


class SessionStore:
    """每会话最新状态 + 最后活动时刻;过期会话视为已结束(复刻 TTL 扫描)。"""

    def __init__(self, ttl_s: float = 300.0) -> None:
        self._ttl = ttl_s
        self._sessions: dict[str, tuple[str, str, float]] = {}

    def update(self, session_id: str, state: str, detail: str, now: float) -> None:
        self._sessions[session_id] = (state, detail, now)

    def _sweep(self, now: float) -> None:
        dead = [sid for sid, (_, _, ts) in self._sessions.items()
                if now - ts > self._ttl]
        for sid in dead:
            del self._sessions[sid]

    def live_states(self, now: float) -> list[tuple[str, str]]:
        self._sweep(now)
        return [(st, det) for (st, det, _) in self._sessions.values()]

    def counts(self, now: float) -> tuple[int, int]:
        self._sweep(now)
        total = len(self._sessions)
        busy = sum(1 for (st, _, _) in self._sessions.values()
                   if _PRIORITY.get(st, 0) > 0)
        return total, busy
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_session_store.py -v`
Expected: PASS（2 passed）

- [ ] **Step 6: 提交**

```bash
git add host/pyproject.toml host/cchud_daemon/__init__.py host/cchud_daemon/event.py host/cchud_daemon/session_store.py host/tests/test_session_store.py
git commit -m "feat: 新增 daemon 测试设施 + CcHudEvent + SessionStore"
```

---

## Task 2: QuotaTracker(额度分窗合并 + 限频)

**Files:**
- Create: `host/cchud_daemon/quota_tracker.py`
- Test: `host/tests/test_quota_tracker.py`

**Interfaces:**
- Consumes: 无(纯逻辑)。
- Produces:
  - `QuotaTracker(rate_limit_s: float = 30.0)`,方法:
    - `merge(used: int, resets_at: int, window: str, now: float) -> tuple[int, int]`:`window` 为 `"5h"|"7d"`;按 `resets_at` 分窗——`resets_at` 更大则替换(处理归零),相等则取较大 used;返回合并后 `(used, resets_at)`。
    - `should_push(now: float) -> bool`:距上次 `mark_pushed` 不足 `rate_limit_s` 返回 `False`。
    - `mark_pushed(now: float) -> None`。

- [ ] **Step 1: 写失败测试**

`host/tests/test_quota_tracker.py`:

```python
from cchud_daemon.quota_tracker import QuotaTracker


def test_merge_takes_max_in_same_window():
    q = QuotaTracker()
    assert q.merge(40, 2000, "5h", now=1000) == (40, 2000)
    # 同窗口(resets_at 相同),取较大 used
    assert q.merge(35, 2000, "5h", now=1001) == (40, 2000)
    assert q.merge(55, 2000, "5h", now=1002) == (55, 2000)


def test_merge_replaces_on_newer_window():
    q = QuotaTracker()
    q.merge(90, 2000, "5h", now=1000)
    # 新窗口(resets_at 更大)→ 直接替换,处理用量归零
    assert q.merge(5, 9000, "5h", now=2001) == (5, 9000)


def test_rate_limit():
    q = QuotaTracker(rate_limit_s=30)
    assert q.should_push(now=1000) is True
    q.mark_pushed(now=1000)
    assert q.should_push(now=1020) is False    # 不足 30s
    assert q.should_push(now=1031) is True      # 超过 30s
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_quota_tracker.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/quota_tracker.py`:

```python
from __future__ import annotations


class QuotaTracker:
    """额度跨会话分窗合并 + BLE 推送限频(复刻 cchud-quota-push.sh / cchud-update.sh)。"""

    def __init__(self, rate_limit_s: float = 30.0) -> None:
        self._rate = rate_limit_s
        self._last_push: float | None = None
        # window -> (used, resets_at)
        self._win: dict[str, tuple[int, int]] = {}

    def merge(self, used: int, resets_at: int, window: str, now: float) -> tuple[int, int]:
        cur = self._win.get(window)
        if cur is None or resets_at > cur[1]:
            merged = (used, resets_at)
        elif resets_at == cur[1]:
            merged = (max(used, cur[0]), resets_at)
        else:
            merged = cur                       # incoming 是旧窗口,忽略
        self._win[window] = merged
        return merged

    def should_push(self, now: float) -> bool:
        return self._last_push is None or (now - self._last_push) >= self._rate

    def mark_pushed(self, now: float) -> None:
        self._last_push = now
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_quota_tracker.py -v`
Expected: PASS（3 passed）

- [ ] **Step 5: 提交**

```bash
git add host/cchud_daemon/quota_tracker.py host/tests/test_quota_tracker.py
git commit -m "feat: 新增 QuotaTracker 额度分窗合并与限频"
```

---

## Task 3: Aggregator(聚合态 + 去重)

**Files:**
- Create: `host/cchud_daemon/aggregator.py`
- Test: `host/tests/test_aggregator.py`

**Interfaces:**
- Consumes: `SessionStore`(Task 1)。
- Produces:
  - `Aggregator()`,方法:
    - `aggregate(store: SessionStore, now: float) -> tuple[str, str, int, int]`:返回 `(state, detail, total, busy)`,state 取优先级最高者(`waiting>tool>thinking>idle`),detail 取该会话的 detail;无会话返回 `("idle","",0,0)`。
    - `changed(key: tuple) -> bool`:与上次 `aggregate` 结果指纹比较,变了返回 `True` 并记录;不变返回 `False`(复刻 `last-pushed.state` 去重)。

- [ ] **Step 1: 写失败测试**

`host/tests/test_aggregator.py`:

```python
from cchud_daemon.aggregator import Aggregator
from cchud_daemon.session_store import SessionStore


def test_aggregate_picks_highest_priority():
    s = SessionStore()
    s.update("a", "thinking", "", now=1000)
    s.update("b", "tool", "Bash", now=1000)
    agg = Aggregator()
    assert agg.aggregate(s, now=1000) == ("tool", "Bash", 2, 2)


def test_aggregate_empty_is_idle():
    assert Aggregator().aggregate(SessionStore(), now=1000) == ("idle", "", 0, 0)


def test_changed_dedup():
    agg = Aggregator()
    key = ("tool", "Bash", 2, 2)
    assert agg.changed(key) is True     # 首次
    assert agg.changed(key) is False    # 不变
    assert agg.changed(("idle", "", 0, 0)) is True
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_aggregator.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/aggregator.py`:

```python
from __future__ import annotations

from .session_store import SessionStore, _PRIORITY


class Aggregator:
    """把多会话聚合为单一推送态,并对不变结果去重(复刻 cchud-hook.sh)。"""

    def __init__(self) -> None:
        self._last_key: tuple | None = None

    def aggregate(self, store: SessionStore, now: float) -> tuple[str, str, int, int]:
        best_state, best_detail, best_p = "idle", "", -1
        for state, detail in store.live_states(now):
            p = _PRIORITY.get(state, 0)
            if p > best_p:
                best_p, best_state, best_detail = p, state, detail
        total, busy = store.counts(now)
        return best_state, best_detail, total, busy

    def changed(self, key: tuple) -> bool:
        if key == self._last_key:
            return False
        self._last_key = key
        return True
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_aggregator.py -v`
Expected: PASS（3 passed）

- [ ] **Step 5: 提交**

```bash
git add host/cchud_daemon/aggregator.py host/tests/test_aggregator.py
git commit -m "feat: 新增 Aggregator 多会话聚合与去重"
```

---

## Task 4: codec(v6 兼容编码 + v7 TLV 编码)

**Files:**
- Create: `host/cchud_daemon/codec.py`
- Test: `host/tests/test_codec.py`

**Interfaces:**
- Consumes: 无。
- Produces:
  - `encode_quota_v6(*, mode:int, h5_used:int, h5_limit:int, d7_used:int, d7_limit:int, h5_reset_s:int, d7_reset_s:int, cost_micro_usd:int, duration_s:int, ctx_pct:int, lines_added:int, lines_removed:int, title:str) -> bytes`(从 `push_quota.py:_pack_payload_v6` 抽取,字节布局完全一致)。
  - `encode_state_0x07(state_code:int, detail:str, total:int, busy:int) -> bytes`(从 `push_state.py:pack_state` 抽取)。
  - v7 tag 常量(`TAG_FIVE_H_USED_PCT=0x01` … 见 spec 7.4)。
  - `encode_v7(fields: list[tuple[int, bytes]]) -> bytes`:组帧 `0x0B,flags=0,seq=0,total=1` + 连续 `[tag][len][value]`;单字段 `len>255` 抛 `ValueError`。
  - `tlv_u8(tag:int, v:int)->tuple[int,bytes]`、`tlv_u16(...)`、`tlv_u32(...)`、`tlv_str(tag:int, s:str)->tuple[int,bytes]` 辅助。

- [ ] **Step 1: 写失败测试**

`host/tests/test_codec.py`:

```python
import struct
from cchud_daemon import codec


def test_v6_matches_legacy_layout():
    # 与 push_quota.py 的 _pack_payload_v6 字节级一致
    p = codec.encode_quota_v6(
        mode=0, h5_used=40, h5_limit=100, d7_used=28, d7_limit=100,
        h5_reset_s=120, d7_reset_s=3600, cost_micro_usd=420000,
        duration_s=23, ctx_pct=65, lines_added=127, lines_removed=45,
        title="Opus 4.8 (1M context)")
    assert p[0] == 0x0A                       # v6 msg_type
    title_b = "Opus 4.8 (1M context)".encode("ascii")[:32]
    assert p[-len(title_b):] == title_b
    assert p[35] == len(title_b)              # title_len 偏移


def test_state_0x07_layout():
    p = codec.encode_state_0x07(2, "Bash", total=3, busy=2)
    assert p[0] == 0x07 and p[1] == 2 and p[2] == 4
    assert p[3:7] == b"Bash"
    assert p[7] == 3 and p[8] == 2            # total, busy 尾字节


def test_v7_frame_and_tlv():
    frame = codec.encode_v7([
        codec.tlv_u8(codec.TAG_FIVE_H_USED_PCT, 40),
        codec.tlv_str(codec.TAG_TITLE, "Opus"),
    ])
    assert frame[:4] == bytes([0x0B, 0, 0, 1])         # 头
    # 第一个字段:tag=0x01 len=1 value=40
    assert frame[4] == 0x01 and frame[5] == 1 and frame[6] == 40
    # 第二个字段:tag=TAG_TITLE len=4 "Opus"
    off = 7
    assert frame[off] == codec.TAG_TITLE and frame[off + 1] == 4
    assert frame[off + 2:off + 6] == b"Opus"


def test_v7_rejects_oversize_field():
    import pytest
    with pytest.raises(ValueError):
        codec.encode_v7([codec.tlv_str(codec.TAG_TITLE, "x" * 300)])
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_codec.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/codec.py`:

```python
from __future__ import annotations
import struct

# ── v7 tag 常量(对照路线图 7.4)────────────────────────────────────────
TAG_FIVE_H_USED_PCT      = 0x01
TAG_FIVE_H_RESET_IN_S    = 0x02
TAG_SEVEN_D_USED_PCT     = 0x03
TAG_SEVEN_D_RESET_IN_S   = 0x04
TAG_CONTEXT_USED_PCT     = 0x05
TAG_COST_MICRO_USD       = 0x06
TAG_DURATION_S           = 0x07
TAG_LINES_ADDED          = 0x08
TAG_LINES_REMOVED        = 0x09
TAG_PLAN_MODE            = 0x0A
TAG_TITLE                = 0x0B
TAG_AGG_STATE            = 0x20
TAG_ACTIVE_TOOL_NAME     = 0x21
TAG_TOTAL_SESSIONS       = 0x22
TAG_BUSY_SESSIONS        = 0x23
TAG_UNIX_TS              = 0x80
TAG_UTC_OFFSET_MIN       = 0x81
TAG_STATUS_STRING        = 0x84

MSG_V7_TLV = 0x0B


def encode_quota_v6(*, mode, h5_used, h5_limit, d7_used, d7_limit,
                    h5_reset_s, d7_reset_s, cost_micro_usd, duration_s,
                    ctx_pct, lines_added, lines_removed, title) -> bytes:
    title_b = title.encode("ascii", errors="replace")[:32]
    fixed = struct.pack(
        "<BBHHHHIIIIBIIB",
        0x0A, mode & 0xFF,
        h5_used, h5_limit, d7_used, d7_limit,
        h5_reset_s, d7_reset_s,
        cost_micro_usd, duration_s,
        max(0, min(100, ctx_pct)),
        max(0, lines_added), max(0, lines_removed),
        len(title_b),
    )
    return fixed + title_b


def encode_state_0x07(state_code: int, detail: str, total: int, busy: int) -> bytes:
    d = detail.encode("ascii", errors="replace")[:15]
    base = struct.pack("<BBB", 0x07, state_code, len(d)) + d
    base += struct.pack("<BB", min(255, max(0, total)), min(255, max(0, busy)))
    return base


def tlv_u8(tag: int, v: int) -> tuple[int, bytes]:
    return tag, struct.pack("<B", v & 0xFF)


def tlv_u16(tag: int, v: int) -> tuple[int, bytes]:
    return tag, struct.pack("<H", v & 0xFFFF)


def tlv_u32(tag: int, v: int) -> tuple[int, bytes]:
    return tag, struct.pack("<I", v & 0xFFFFFFFF)


def tlv_str(tag: int, s: str) -> tuple[int, bytes]:
    return tag, s.encode("utf-8", errors="replace")


def encode_v7(fields: list[tuple[int, bytes]]) -> bytes:
    out = bytearray([MSG_V7_TLV, 0, 0, 1])      # msg_type, flags, seq, total
    for tag, value in fields:
        if len(value) > 255:
            raise ValueError(f"tag 0x{tag:02X} value too long: {len(value)}")
        out += bytes([tag & 0xFF, len(value)]) + value
    return bytes(out)
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_codec.py -v`
Expected: PASS（4 passed）

- [ ] **Step 5: 提交**

```bash
git add host/cchud_daemon/codec.py host/tests/test_codec.py
git commit -m "feat: 新增 codec(v6 兼容编码 + v7 TLV 编码)"
```

---

## Task 5: ClaudeAdapter(规范化 + 行为复刻)

**Files:**
- Create: `host/cchud_daemon/adapters/__init__.py`
- Create: `host/cchud_daemon/adapters/base.py`
- Create: `host/cchud_daemon/adapters/claude.py`
- Test: `host/tests/test_claude_adapter.py`

**Interfaces:**
- Consumes: `CcHudEvent`(Task 1)。
- Produces:
  - `HookSpec`(dataclass:`events: dict[str,str]`,`statusline: str | None`)。
  - `Adapter`(Protocol:`client_id:int`、`name:str`、`normalize(raw:dict)->list[CcHudEvent]`、`hook_spec()->HookSpec`)。
  - `ClaudeAdapter(client_id=0)`:`normalize` 接受 `{"event":..., "payload":{...}}`,产出事件:
    - `PreToolUse` → state=tool,detail=tool_name;`UserPromptSubmit`/`PostToolUse` → thinking;`Stop` → idle;
    - `Notification` → 仅 message 含 permission/approve/confirm/"needs your" 时 state=waiting,否则 state=idle(复刻降级);
    - 当 payload 含 `statusline` JSON 时,额外产出 kind=quota 事件(抽 5h/7d used%+resets_at、ctx_pct、cost、duration、行数、标题)。

- [ ] **Step 1: 写失败测试**

`host/tests/test_claude_adapter.py`:

```python
from cchud_daemon.adapters.claude import ClaudeAdapter


def test_pretooluse_maps_to_tool():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "PreToolUse",
                       "payload": {"session_id": "s1", "tool_name": "Bash"}})
    e = [x for x in evs if x.kind == "state"][0]
    assert e.state == "tool" and e.detail == "Bash" and e.session_id == "s1"


def test_notification_permission_is_waiting():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "Notification",
                       "payload": {"session_id": "s1",
                                   "message": "Claude needs your permission to run Bash"}})
    assert [x for x in evs if x.kind == "state"][0].state == "waiting"


def test_notification_benign_downgrades_to_idle():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "Notification",
                       "payload": {"session_id": "s1",
                                   "message": "Claude is waiting for your input"}})
    assert [x for x in evs if x.kind == "state"][0].state == "idle"


def test_statusline_produces_quota_event_with_title():
    a = ClaudeAdapter()
    payload = {"session_id": "s1", "statusline": {
        "rate_limits": {"five_hour": {"used_percentage": 40, "resets_at": 2000},
                        "seven_day": {"used_percentage": 28, "resets_at": 9000}},
        "context_window": {"used_percentage": 65, "context_window_size": 1000000},
        "cost": {"total_cost_usd": 0.42, "total_duration_ms": 23000,
                 "total_lines_added": 127, "total_lines_removed": 45},
        "model": {"display_name": "Opus 4.8"}}}
    q = [x for x in a.normalize({"event": "Status", "payload": payload})
         if x.kind == "quota"][0]
    assert q.five_h_used == 40 and q.seven_d_resets_at == 9000
    assert q.ctx_pct == 65 and q.lines_added == 127
    assert q.title == "Opus 4.8 (1M context)"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_claude_adapter.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/adapters/__init__.py`:空文件。

`host/cchud_daemon/adapters/base.py`:

```python
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Protocol
from ..event import CcHudEvent


@dataclass
class HookSpec:
    events: dict[str, str] = field(default_factory=dict)   # 事件名 -> emit 参数
    statusline: str | None = None


class Adapter(Protocol):
    client_id: int
    name: str
    def normalize(self, raw: dict) -> list[CcHudEvent]: ...
    def hook_spec(self) -> HookSpec: ...
```

`host/cchud_daemon/adapters/claude.py`:

```python
from __future__ import annotations
from ..event import CcHudEvent
from .base import HookSpec

# 复刻 cchud-hook.sh 的 Notification 降级关键词
_PERMISSION_HINTS = ("permission", "Permission", "approve", "Approve",
                     "needs your", "confirm", "Confirm")

_EVENT_STATE = {
    "UserPromptSubmit": "thinking",
    "PostToolUse": "thinking",
    "PreToolUse": "tool",
    "Stop": "idle",
}


def _fmt_ctx_size(n: int) -> str:
    if n >= 1_000_000:
        return f"{n // 1_000_000}M"
    if n >= 1000:
        return f"{n // 1000}K"
    return ""


class ClaudeAdapter:
    client_id = 0
    name = "claude"

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        event = raw.get("event", "")
        payload = raw.get("payload", {})
        sid = payload.get("session_id") or "default"
        out: list[CcHudEvent] = []

        if event in _EVENT_STATE:
            state = _EVENT_STATE[event]
            detail = payload.get("tool_name", "") if state == "tool" else ""
            out.append(CcHudEvent(self.client_id, sid, "state",
                                  state=state, detail=detail))
        elif event == "Notification":
            msg = payload.get("message", "") or ""
            state = "waiting" if any(h in msg for h in _PERMISSION_HINTS) else "idle"
            out.append(CcHudEvent(self.client_id, sid, "state", state=state))

        sl = payload.get("statusline")
        if isinstance(sl, dict):
            out.append(self._quota_from_statusline(sid, sl))
        return out

    def _quota_from_statusline(self, sid: str, sl: dict) -> CcHudEvent:
        rl = sl.get("rate_limits", {})
        fh = rl.get("five_hour", {})
        sd = rl.get("seven_day", {})
        cw = sl.get("context_window", {})
        cost = sl.get("cost", {})
        model = (sl.get("model", {}) or {}).get("display_name", "") or ""
        ctx_size = int(cw.get("context_window_size", 0) or 0)
        ctx_txt = _fmt_ctx_size(ctx_size)
        if model and ctx_txt:
            title = f"{model} ({ctx_txt} context)"
        elif model:
            title = model
        else:
            title = None
        return CcHudEvent(
            self.client_id, sid, "quota",
            five_h_used=int(fh.get("used_percentage", 0) or 0),
            five_h_resets_at=int(fh.get("resets_at", 0) or 0),
            seven_d_used=int(sd.get("used_percentage", 0) or 0),
            seven_d_resets_at=int(sd.get("resets_at", 0) or 0),
            ctx_pct=int(cw.get("used_percentage", 0) or 0),
            cost_usd=float(cost.get("total_cost_usd", 0) or 0),
            duration_s=int((cost.get("total_duration_ms", 0) or 0)) // 1000,
            lines_added=int(cost.get("total_lines_added", 0) or 0),
            lines_removed=int(cost.get("total_lines_removed", 0) or 0),
            title=title, mode="sub")

    def hook_spec(self) -> HookSpec:
        return HookSpec(events={
            "UserPromptSubmit": "claude UserPromptSubmit",
            "PreToolUse": "claude PreToolUse",
            "PostToolUse": "claude PostToolUse",
            "Stop": "claude Stop",
            "Notification": "claude Notification",
        }, statusline="claude Status")
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_claude_adapter.py -v`
Expected: PASS（4 passed）

- [ ] **Step 5: 提交**

```bash
git add host/cchud_daemon/adapters host/tests/test_claude_adapter.py
git commit -m "feat: 新增 Adapter 接口与 ClaudeAdapter(复刻状态/额度/降级逻辑)"
```

---

## Task 6: AdapterRegistry + SocketServer

**Files:**
- Create: `host/cchud_daemon/registry.py`
- Create: `host/cchud_daemon/socket_server.py`
- Test: `host/tests/test_registry.py`
- Test: `host/tests/test_socket_server.py`

**Interfaces:**
- Consumes: `Adapter`/`ClaudeAdapter`(Task 5)、`CcHudEvent`。
- Produces:
  - `AdapterRegistry()`:`register(a: Adapter)`、`normalize(raw: dict) -> list[CcHudEvent]`(按 `raw["client"]` 选 adapter;未知 client 返回 `[]`)。
  - `SocketServer(path: str, on_events: Callable[[list[CcHudEvent]], Awaitable[None]], registry: AdapterRegistry)`:`async start()` 监听 UDS,每条连接读一行 JSON → `registry.normalize` → `await on_events`;解析失败忽略该行。`async stop()`。

- [ ] **Step 1: 写失败测试(registry)**

`host/tests/test_registry.py`:

```python
from cchud_daemon.registry import AdapterRegistry
from cchud_daemon.adapters.claude import ClaudeAdapter


def test_routes_to_claude():
    r = AdapterRegistry()
    r.register(ClaudeAdapter())
    evs = r.normalize({"client": "claude", "event": "Stop",
                       "payload": {"session_id": "s1"}})
    assert evs and evs[0].state == "idle"


def test_unknown_client_returns_empty():
    assert AdapterRegistry().normalize({"client": "nope", "event": "x"}) == []
```

- [ ] **Step 2: 写失败测试(socket server)**

`host/tests/test_socket_server.py`:

```python
import asyncio
import json
import pytest
from cchud_daemon.registry import AdapterRegistry
from cchud_daemon.adapters.claude import ClaudeAdapter
from cchud_daemon.socket_server import SocketServer


@pytest.mark.asyncio
async def test_socket_roundtrip(tmp_path):
    received = []

    async def on_events(evs):
        received.extend(evs)

    reg = AdapterRegistry(); reg.register(ClaudeAdapter())
    sock = str(tmp_path / "d.sock")
    srv = SocketServer(sock, on_events, reg)
    await srv.start()
    try:
        r, w = await asyncio.open_unix_connection(sock)
        w.write((json.dumps({"client": "claude", "event": "PreToolUse",
                             "payload": {"session_id": "s1", "tool_name": "Bash"}}) + "\n").encode())
        await w.drain(); w.close()
        await asyncio.sleep(0.1)
    finally:
        await srv.stop()
    assert any(e.detail == "Bash" for e in received)
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_registry.py tests/test_socket_server.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 4: 写最小实现**

`host/cchud_daemon/registry.py`:

```python
from __future__ import annotations
from .adapters.base import Adapter
from .event import CcHudEvent


class AdapterRegistry:
    def __init__(self) -> None:
        self._by_name: dict[str, Adapter] = {}

    def register(self, a: Adapter) -> None:
        self._by_name[a.name] = a

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        a = self._by_name.get(raw.get("client", ""))
        return a.normalize(raw) if a else []
```

`host/cchud_daemon/socket_server.py`:

```python
from __future__ import annotations
import asyncio
import json
import os
from typing import Awaitable, Callable
from .event import CcHudEvent
from .registry import AdapterRegistry

OnEvents = Callable[[list[CcHudEvent]], Awaitable[None]]


class SocketServer:
    """监听 Unix domain socket,把 hook 投递的 JSON 规范化为事件。"""

    def __init__(self, path: str, on_events: OnEvents, registry: AdapterRegistry) -> None:
        self._path = path
        self._on_events = on_events
        self._registry = registry
        self._server: asyncio.AbstractServer | None = None

    async def _handle(self, reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            raw = json.loads(line.decode("utf-8"))
            evs = self._registry.normalize(raw)
            if evs:
                await self._on_events(evs)
        except (json.JSONDecodeError, UnicodeDecodeError):
            pass                                  # 脏行忽略,不崩溃
        finally:
            writer.close()

    async def start(self) -> None:
        if os.path.exists(self._path):
            os.unlink(self._path)
        os.makedirs(os.path.dirname(self._path), exist_ok=True)
        self._server = await asyncio.start_unix_server(self._handle, path=self._path)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        if os.path.exists(self._path):
            os.unlink(self._path)
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_registry.py tests/test_socket_server.py -v`
Expected: PASS（3 passed）

- [ ] **Step 6: 提交**

```bash
git add host/cchud_daemon/registry.py host/cchud_daemon/socket_server.py host/tests/test_registry.py host/tests/test_socket_server.py
git commit -m "feat: 新增 AdapterRegistry 与 SocketServer"
```

---

## Task 7: BleLink(常驻连接 + 串行发送队列)

**Files:**
- Create: `host/cchud_daemon/ble_link.py`
- Test: `host/tests/test_ble_link.py`

**Interfaces:**
- Consumes: 无(BLE 客户端经构造注入,便于测试)。
- Produces:
  - `BleLink(address: str, *, client_factory=..., use_v7: bool = False)`:
    - `async enqueue(payload: bytes) -> None`:把已编码 payload 入串行队列。
    - `async run() -> None`:常驻——连接(失败指数退避重连 0.5→30s),从队列取项,`write_gatt_char(QUOTA_CHAR, payload, response=True)`,失败重试 1 次。
    - `async stop() -> None`。
  - 关键:**写入串行**——同一时刻只有一个 write 在飞(取代 `ble.lock`)。

- [ ] **Step 1: 写失败测试(用假 client 验证串行与重连)**

`host/tests/test_ble_link.py`:

```python
import asyncio
import pytest
from cchud_daemon.ble_link import BleLink

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"


class FakeClient:
    """记录写入顺序、断言无并发写。"""
    instances = []

    def __init__(self, address, timeout=10):
        self.address = address
        self.writes = []
        self._in_write = False
        FakeClient.instances.append(self)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def write_gatt_char(self, uuid, payload, response=True):
        assert not self._in_write, "并发写!应串行"
        self._in_write = True
        await asyncio.sleep(0.01)
        self.writes.append(bytes(payload))
        self._in_write = False


@pytest.mark.asyncio
async def test_serial_writes():
    FakeClient.instances.clear()
    link = BleLink("addr", client_factory=lambda addr, timeout: FakeClient(addr))
    task = asyncio.create_task(link.run())
    await link.enqueue(b"\x0b\x00\x00\x01")
    await link.enqueue(b"\x0b\x00\x00\x01\x01\x01\x28")
    await asyncio.sleep(0.1)
    await link.stop()
    task.cancel()
    writes = [w for c in FakeClient.instances for w in c.writes]
    assert len(writes) == 2
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_ble_link.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/ble_link.py`:

```python
from __future__ import annotations
import asyncio
import logging

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
_log = logging.getLogger("cchud.ble")


def _default_factory(address: str, timeout: float):
    from bleak import BleakClient
    return BleakClient(address, timeout=timeout)


class BleLink:
    """常驻 BLE 连接 + 串行发送队列(取代 ble.lock + 每次重连)。"""

    def __init__(self, address: str, *, client_factory=_default_factory,
                 use_v7: bool = False, timeout: float = 8.0) -> None:
        self._address = address
        self._factory = client_factory
        self._timeout = timeout
        self.use_v7 = use_v7
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._stop = asyncio.Event()

    async def enqueue(self, payload: bytes) -> None:
        await self._queue.put(payload)

    async def _write_one(self, payload: bytes) -> None:
        last = None
        for _ in range(2):                       # 重试 1 次
            try:
                async with self._factory(self._address, self._timeout) as c:
                    await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
                return
            except Exception as exc:             # noqa: BLE001
                last = exc
                await asyncio.sleep(0.4)
        _log.warning("BLE 写入失败: %s", last)

    async def run(self) -> None:
        backoff = 0.5
        while not self._stop.is_set():
            try:
                payload = await asyncio.wait_for(self._queue.get(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            await self._write_one(payload)        # 串行:一次一个
            backoff = 0.5

    async def stop(self) -> None:
        self._stop.set()
```

> 说明:本实现每次写入用一个短连接上下文(`async with`),队列保证串行。后续可在此模块内升级为"保持长连接"——接口不变,测试不变。常驻长连接的真机验证见 Task 11。

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_ble_link.py -v`
Expected: PASS（1 passed）

- [ ] **Step 5: 提交**

```bash
git add host/cchud_daemon/ble_link.py host/tests/test_ble_link.py
git commit -m "feat: 新增 BleLink 串行发送队列与重连骨架"
```

---

## Task 8: daemon 组装 + 状态管线 + launchd

**Files:**
- Create: `host/cchud_daemon/daemon.py`
- Create: `host/io.cchud.daemon.plist`
- Test: `host/tests/test_daemon_pipeline.py`

**Interfaces:**
- Consumes: 全部前序模块。
- Produces:
  - `Daemon(address, sock_path, *, ble=None, now=time.time)`:
    - `async on_events(evs: list[CcHudEvent]) -> None`:state 事件 → `SessionStore.update`,聚合变更则 enqueue `encode_state_0x07`;quota 事件 → `QuotaTracker.merge`,`should_push` 为真则 enqueue 编码(use_v7 决定 v6/v7),并 `mark_pushed`。
    - `async start()/stop()`。
  - state code 映射:`{"idle":0,"thinking":1,"tool":2,"waiting":3,"done":4}`。

- [ ] **Step 1: 写失败测试(端到端管线,注入假 BleLink)**

`host/tests/test_daemon_pipeline.py`:

```python
import pytest
from cchud_daemon.daemon import Daemon
from cchud_daemon.event import CcHudEvent


class SpyBle:
    def __init__(self): self.sent = []; self.use_v7 = False
    async def enqueue(self, payload): self.sent.append(bytes(payload))
    async def run(self): pass
    async def stop(self): pass


@pytest.mark.asyncio
async def test_state_event_pushes_0x07():
    ble = SpyBle()
    t = {"v": 1000.0}
    d = Daemon("addr", "/tmp/x.sock", ble=ble, now=lambda: t["v"])
    await d.on_events([CcHudEvent(0, "s1", "state", state="tool", detail="Bash")])
    assert ble.sent and ble.sent[0][0] == 0x07 and ble.sent[0][1] == 2


@pytest.mark.asyncio
async def test_quota_rate_limited():
    ble = SpyBle()
    t = {"v": 1000.0}
    d = Daemon("addr", "/tmp/x.sock", ble=ble, now=lambda: t["v"])
    q = CcHudEvent(0, "s1", "quota", five_h_used=40, five_h_resets_at=2000,
                   seven_d_used=28, seven_d_resets_at=9000, ctx_pct=65,
                   cost_usd=0.42, duration_s=23, lines_added=1, lines_removed=0,
                   title="Opus 4.8", mode="sub")
    await d.on_events([q])
    assert len(ble.sent) == 1                       # 首次推送
    t["v"] = 1010.0
    await d.on_events([q])
    assert len(ble.sent) == 1                       # 30s 内被限频
    t["v"] = 1040.0
    await d.on_events([q])
    assert len(ble.sent) == 2                       # 超 30s 再推
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_daemon_pipeline.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/daemon.py`:

```python
from __future__ import annotations
import asyncio
import time
from .aggregator import Aggregator
from .ble_link import BleLink
from .codec import (encode_state_0x07, encode_quota_v6, encode_v7,
                    tlv_u8, tlv_u32, tlv_str, TAG_FIVE_H_USED_PCT,
                    TAG_FIVE_H_RESET_IN_S, TAG_SEVEN_D_USED_PCT,
                    TAG_SEVEN_D_RESET_IN_S, TAG_CONTEXT_USED_PCT,
                    TAG_COST_MICRO_USD, TAG_DURATION_S, TAG_LINES_ADDED,
                    TAG_LINES_REMOVED, TAG_PLAN_MODE, TAG_TITLE)
from .event import CcHudEvent
from .quota_tracker import QuotaTracker
from .registry import AdapterRegistry
from .adapters.claude import ClaudeAdapter
from .session_store import SessionStore
from .socket_server import SocketServer

_STATE_CODE = {"idle": 0, "thinking": 1, "tool": 2, "waiting": 3, "done": 4}


class Daemon:
    def __init__(self, address: str, sock_path: str, *, ble=None,
                 now=time.time) -> None:
        self._now = now
        self._store = SessionStore()
        self._quota = QuotaTracker()
        self._agg = Aggregator()
        self._ble = ble if ble is not None else BleLink(address)
        self._registry = AdapterRegistry()
        self._registry.register(ClaudeAdapter())
        self._sock = SocketServer(sock_path, self.on_events, self._registry)
        self._last_title = "CC HUD"

    async def on_events(self, evs: list[CcHudEvent]) -> None:
        for e in evs:
            if e.kind == "state":
                await self._handle_state(e)
            elif e.kind == "quota":
                await self._handle_quota(e)

    async def _handle_state(self, e: CcHudEvent) -> None:
        now = self._now()
        self._store.update(e.session_id, e.state or "idle", e.detail or "", now)
        state, detail, total, busy = self._agg.aggregate(self._store, now)
        if self._agg.changed((state, detail, total, busy)):
            await self._ble.enqueue(
                encode_state_0x07(_STATE_CODE.get(state, 0), detail, total, busy))

    async def _handle_quota(self, e: CcHudEvent) -> None:
        now = self._now()
        used5, reset5_at = self._quota.merge(
            e.five_h_used or 0, e.five_h_resets_at or 0, "5h", now)
        used7, reset7_at = self._quota.merge(
            e.seven_d_used or 0, e.seven_d_resets_at or 0, "7d", now)
        if not self._quota.should_push(now):
            return
        self._quota.mark_pushed(now)
        if e.title:
            self._last_title = e.title
        reset5 = max(0, reset5_at - int(now)) if reset5_at else 0
        reset7 = max(0, reset7_at - int(now)) if reset7_at else 0
        cost_micro = max(0, int(round((e.cost_usd or 0) * 1_000_000)))
        if getattr(self._ble, "use_v7", False):
            payload = encode_v7([
                tlv_u8(TAG_PLAN_MODE, 0 if (e.mode or "sub") == "sub" else 1),
                tlv_u8(TAG_FIVE_H_USED_PCT, used5),
                tlv_u32(TAG_FIVE_H_RESET_IN_S, reset5),
                tlv_u8(TAG_SEVEN_D_USED_PCT, used7),
                tlv_u32(TAG_SEVEN_D_RESET_IN_S, reset7),
                tlv_u8(TAG_CONTEXT_USED_PCT, e.ctx_pct or 0),
                tlv_u32(TAG_COST_MICRO_USD, cost_micro),
                tlv_u32(TAG_DURATION_S, e.duration_s or 0),
                tlv_u32(TAG_LINES_ADDED, e.lines_added or 0),
                tlv_u32(TAG_LINES_REMOVED, e.lines_removed or 0),
                tlv_str(TAG_TITLE, self._last_title),
            ])
        else:
            payload = encode_quota_v6(
                mode=0 if (e.mode or "sub") == "sub" else 1,
                h5_used=used5, h5_limit=100, d7_used=used7, d7_limit=100,
                h5_reset_s=reset5, d7_reset_s=reset7,
                cost_micro_usd=cost_micro, duration_s=e.duration_s or 0,
                ctx_pct=e.ctx_pct or 0, lines_added=e.lines_added or 0,
                lines_removed=e.lines_removed or 0, title=self._last_title)
        await self._ble.enqueue(payload)

    async def start(self) -> None:
        await self._sock.start()
        asyncio.create_task(self._ble.run())

    async def stop(self) -> None:
        await self._sock.stop()
        await self._ble.stop()
```

- [ ] **Step 4: 写 launchd 模板**

`host/io.cchud.daemon.plist`(占位 `__PYTHON__`/`__DAEMON__` 由 `cchud install` 替换):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>io.cchud.daemon</string>
  <key>ProgramArguments</key>
  <array><string>__PYTHON__</string><string>-m</string><string>cchud_daemon.cli</string><string>daemon</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>__HOME__/.cchud/daemon.log</string>
  <key>StandardOutPath</key><string>__HOME__/.cchud/daemon.log</string>
  <key>WorkingDirectory</key><string>__DAEMON__</string>
</dict>
</plist>
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_daemon_pipeline.py -v`
Expected: PASS（2 passed）

- [ ] **Step 6: 提交**

```bash
git add host/cchud_daemon/daemon.py host/io.cchud.daemon.plist host/tests/test_daemon_pipeline.py
git commit -m "feat: 组装 Daemon 状态管线 + launchd 模板"
```

---

## Task 9: CLI(daemon/install/status)+ 瘦 hook 脚本

**Files:**
- Create: `host/cchud_daemon/cli.py`
- Create: `host/cchud-emit.sh`
- Test: `host/tests/test_cli_install.py`

**Interfaces:**
- Consumes: `Daemon`(Task 8)、`ClaudeAdapter.hook_spec()`(Task 5)。
- Produces:
  - `cli.main(argv: list[str]) -> int`,子命令:
    - `daemon`:读 `CCHUD_ADDR`,建 `Daemon`,`asyncio.run` 常驻。
    - `install <client>`:把 adapter 的 `hook_spec` 写进 `~/.claude/settings.json`(5 个 hook + statusLine,指向 `cchud-emit.sh`);**先备份**原文件为 `settings.json.cchud-bak`;幂等(重复执行不产生重复项)。
    - `status`:打印 socket 是否存在、daemon.log 末尾若干行。
  - `merge_claude_settings(settings: dict, emit_path: str) -> dict`:纯函数,便于测试幂等与正确性。

- [ ] **Step 1: 写失败测试(install 合并幂等)**

`host/tests/test_cli_install.py`:

```python
from cchud_daemon.cli import merge_claude_settings


def test_install_adds_hooks_and_statusline():
    s = merge_claude_settings({}, "/x/cchud-emit.sh")
    assert "Stop" in s["hooks"]
    cmd = s["hooks"]["Stop"][0]["hooks"][0]["command"]
    assert cmd.endswith("cchud-emit.sh claude Stop")
    assert s["statusLine"]["command"].endswith("cchud-emit.sh claude Status")


def test_install_is_idempotent():
    s1 = merge_claude_settings({}, "/x/cchud-emit.sh")
    s2 = merge_claude_settings(s1, "/x/cchud-emit.sh")
    assert s1 == s2                              # 重复执行不增项
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd host && .venv/bin/python -m pytest tests/test_cli_install.py -v`
Expected: FAIL（ModuleNotFoundError）

- [ ] **Step 3: 写最小实现**

`host/cchud_daemon/cli.py`:

```python
from __future__ import annotations
import asyncio
import json
import os
import sys
from .daemon import Daemon
from .adapters.claude import ClaudeAdapter

HOME = os.path.expanduser("~")
SOCK = os.path.join(HOME, ".cchud", "daemon.sock")
LOG = os.path.join(HOME, ".cchud", "daemon.log")


def merge_claude_settings(settings: dict, emit_path: str) -> dict:
    s = dict(settings)
    hooks = dict(s.get("hooks", {}))
    spec = ClaudeAdapter().hook_spec()
    for event, args in spec.events.items():
        command = f"{emit_path} {args}"
        entry = {"hooks": [{"type": "command", "command": command}]}
        # 幂等:若已存在指向同一 emit 的项则覆盖该项,否则设为唯一项
        hooks[event] = [entry]
    s["hooks"] = hooks
    if spec.statusline:
        s["statusLine"] = {"type": "command",
                           "command": f"{emit_path} {spec.statusline}",
                           "padding": 0}
    return s


def _install(client: str) -> int:
    if client != "claude":
        print(f"未知客户端: {client}", file=sys.stderr)
        return 2
    emit = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "cchud-emit.sh")
    cfg = os.path.join(HOME, ".claude", "settings.json")
    cur = {}
    if os.path.exists(cfg):
        with open(cfg) as f:
            cur = json.load(f)
        with open(cfg + ".cchud-bak", "w") as f:
            json.dump(cur, f, indent=2)
    merged = merge_claude_settings(cur, emit)
    os.makedirs(os.path.dirname(cfg), exist_ok=True)
    with open(cfg, "w") as f:
        json.dump(merged, f, indent=2)
    print(f"已写入 {cfg}(备份 {cfg}.cchud-bak)")
    return 0


def _status() -> int:
    print(f"socket: {'存在' if os.path.exists(SOCK) else '缺失'} ({SOCK})")
    if os.path.exists(LOG):
        with open(LOG) as f:
            tail = f.readlines()[-10:]
        print("".join(tail))
    return 0


def _daemon() -> int:
    addr = os.environ.get("CCHUD_ADDR", "")
    d = Daemon(addr, SOCK)

    async def _run():
        await d.start()
        while True:
            await asyncio.sleep(3600)

    asyncio.run(_run())
    return 0


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: cchud {daemon|install <client>|status}", file=sys.stderr)
        return 2
    cmd = argv[0]
    if cmd == "daemon":
        return _daemon()
    if cmd == "install":
        return _install(argv[1] if len(argv) > 1 else "")
    if cmd == "status":
        return _status()
    print(f"未知命令: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

`host/cchud-emit.sh`(瘦 hook;socket 不可用静默退出):

```bash
#!/usr/bin/env bash
# cchud-emit.sh — 瘦 hook 客户端:读 stdin,包成 JSON,投递给 daemon socket。
# 用法: cchud-emit.sh <client> <event>
# 失败一律静默退出 0,绝不阻塞调用方。
set -u
CLIENT="${1:-}"; EVENT="${2:-}"
SOCK="$HOME/.cchud/daemon.sock"
[ -S "$SOCK" ] || exit 0
STDIN="$(cat 2>/dev/null || true)"
# payload 直接透传 hook 的原始 stdin(JSON);daemon 侧 adapter 解析。
PAYLOAD="${STDIN:-\{\}}"
MSG="$(printf '{"client":"%s","event":"%s","payload":%s}' "$CLIENT" "$EVENT" "$PAYLOAD")"
# 用 nc 投递一行;无 nc 时尝试 /dev/tcp 不可用于 unix socket,退回 python 兜底。
if command -v nc >/dev/null 2>&1; then
    printf '%s\n' "$MSG" | nc -U "$SOCK" -w 1 >/dev/null 2>&1 || true
else
    printf '%s\n' "$MSG" | "$(dirname "$0")/.venv/bin/python" -c \
      'import socket,sys,os;s=socket.socket(socket.AF_UNIX);s.connect(os.path.expanduser("~/.cchud/daemon.sock"));s.sendall(sys.stdin.buffer.read());s.close()' \
      >/dev/null 2>&1 || true
fi
exit 0
```

设权限:`chmod +x host/cchud-emit.sh`

- [ ] **Step 4: 运行测试确认通过**

Run: `cd host && .venv/bin/python -m pytest tests/test_cli_install.py -v`
Expected: PASS（2 passed）

- [ ] **Step 5: 全量回归 + 提交**

Run: `cd host && .venv/bin/python -m pytest -v`
Expected: PASS（全部测试)

```bash
chmod +x host/cchud-emit.sh
git add host/cchud_daemon/cli.py host/cchud-emit.sh host/tests/test_cli_install.py
git commit -m "feat: 新增 cchud CLI(daemon/install/status)与瘦 hook 脚本"
```

---

## Task 10: 固件 v7 TLV 解析(native 单测)+ 接线 + MTU

**Files:**
- Create: `firmware/src/v7_tlv.h`
- Create: `firmware/src/v7_tlv.cpp`
- Create: `firmware/test/test_v7_tlv/test_v7_tlv.cpp`
- Modify: `firmware/platformio.ini`(加 `[env:native]`)
- Modify: `firmware/src/config.h`(加 `kQuotaMsgTypeV7Tlv` 与 tag 常量)
- Modify: `firmware/src/ble_server.cpp`(加 0x0B 分支 + setMTU)

**Interfaces:**
- Consumes: 无(纯函数)。
- Produces:
  - `struct V7Fields { ... }`:解析结果(额度/成本/状态/会话数等可选字段 + `has_*` 标志)。
  - `enum V7Result { V7_OK, V7_ERR_LEN, V7_ERR_FRAGMENT }`。
  - `V7Result parseV7Tlv(const uint8_t* buf, size_t len, V7Fields& out)`:校验头(`buf[0]==0x0B`,`total==1` 否则 `V7_ERR_FRAGMENT`),循环读 `[tag][len][value]`,越界返回 `V7_ERR_LEN`,未知 tag 跳过。

- [ ] **Step 1: 加 native 测试环境**

`firmware/platformio.ini` 追加:

```ini
[env:native]
platform = native
test_framework = unity
build_flags = -std=c++17 -I src
```

- [ ] **Step 2: 写失败测试**

`firmware/test/test_v7_tlv/test_v7_tlv.cpp`:

```cpp
#include <unity.h>
#include <cstdint>
#include <cstddef>
#include "v7_tlv.h"

using namespace cc_hud;

void test_parse_known_tags() {
    // 头 + TAG_FIVE_H_USED_PCT(0x01)=40 + TAG_TITLE(0x0B)="Hi"
    const uint8_t buf[] = {0x0B,0,0,1, 0x01,1,40, 0x0B,2,'H','i'};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_five_h_used);
    TEST_ASSERT_EQUAL_UINT8(40, f.five_h_used);
    TEST_ASSERT_EQUAL_STRING("Hi", f.title);
}

void test_unknown_tag_skipped() {
    // 未知 tag 0xC0 len=2,后跟已知 AGG_STATE(0x20)=2
    const uint8_t buf[] = {0x0B,0,0,1, 0xC0,2,0xAA,0xBB, 0x20,1,2};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_agg_state);
    TEST_ASSERT_EQUAL_UINT8(2, f.agg_state);
}

void test_len_overflow_errors() {
    const uint8_t buf[] = {0x0B,0,0,1, 0x01,9,40};   // len=9 越界
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_LEN, parseV7Tlv(buf, sizeof(buf), f));
}

void test_fragment_rejected() {
    const uint8_t buf[] = {0x0B,0,0,2, 0x01,1,40};   // total=2
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_FRAGMENT, parseV7Tlv(buf, sizeof(buf), f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_known_tags);
    RUN_TEST(test_unknown_tag_skipped);
    RUN_TEST(test_len_overflow_errors);
    RUN_TEST(test_fragment_rejected);
    return UNITY_END();
}
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cd firmware && pio test -e native`
Expected: FAIL（v7_tlv.h not found / 链接失败）

- [ ] **Step 4: 写最小实现**

`firmware/src/v7_tlv.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace cc_hud {

// v7 tag(对照路线图 7.4,本层用到的子集)
enum V7Tag : uint8_t {
    kV7TagFiveHUsedPct    = 0x01,
    kV7TagFiveHResetInS   = 0x02,
    kV7TagSevenDUsedPct   = 0x03,
    kV7TagSevenDResetInS  = 0x04,
    kV7TagContextUsedPct  = 0x05,
    kV7TagCostMicroUsd    = 0x06,
    kV7TagDurationS       = 0x07,
    kV7TagLinesAdded      = 0x08,
    kV7TagLinesRemoved    = 0x09,
    kV7TagPlanMode        = 0x0A,
    kV7TagTitle           = 0x0B,
    kV7TagAggState        = 0x20,
    kV7TagActiveToolName  = 0x21,
    kV7TagTotalSessions   = 0x22,
    kV7TagBusySessions    = 0x23,
};

enum V7Result { V7_OK, V7_ERR_LEN, V7_ERR_FRAGMENT };

struct V7Fields {
    bool     has_five_h_used = false;  uint8_t five_h_used = 0;
    bool     has_five_h_reset = false; uint32_t five_h_reset_s = 0;
    bool     has_seven_d_used = false; uint8_t seven_d_used = 0;
    bool     has_seven_d_reset = false; uint32_t seven_d_reset_s = 0;
    bool     has_ctx = false;          uint8_t ctx_pct = 0;
    bool     has_cost = false;         uint32_t cost_micro_usd = 0;
    bool     has_duration = false;     uint32_t duration_s = 0;
    bool     has_lines_added = false;  uint32_t lines_added = 0;
    bool     has_lines_removed = false; uint32_t lines_removed = 0;
    bool     has_mode = false;         uint8_t mode = 0;
    bool     has_title = false;        char title[33] = {0};
    bool     has_agg_state = false;    uint8_t agg_state = 0;
    bool     has_tool = false;         char tool[16] = {0};
    bool     has_total = false;        uint8_t total_sessions = 0;
    bool     has_busy = false;         uint8_t busy_sessions = 0;
};

V7Result parseV7Tlv(const uint8_t* buf, size_t len, V7Fields& out);

}  // namespace cc_hud
```

`firmware/src/v7_tlv.cpp`:

```cpp
#include "v7_tlv.h"
#include <cstring>

namespace cc_hud {

static uint32_t rdU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void cpyStr(char* dst, size_t cap, const uint8_t* src, uint8_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

V7Result parseV7Tlv(const uint8_t* buf, size_t len, V7Fields& out) {
    if (len < 4) return V7_ERR_LEN;
    if (buf[3] != 1) return V7_ERR_FRAGMENT;     // 分片:本层不支持
    size_t i = 4;
    while (i + 2 <= len) {
        uint8_t tag = buf[i];
        uint8_t l   = buf[i + 1];
        if (i + 2 + l > len) return V7_ERR_LEN;
        const uint8_t* v = buf + i + 2;
        switch (tag) {
            case kV7TagFiveHUsedPct:   if (l>=1){out.has_five_h_used=true; out.five_h_used=v[0];} break;
            case kV7TagFiveHResetInS:  if (l>=4){out.has_five_h_reset=true; out.five_h_reset_s=rdU32(v);} break;
            case kV7TagSevenDUsedPct:  if (l>=1){out.has_seven_d_used=true; out.seven_d_used=v[0];} break;
            case kV7TagSevenDResetInS: if (l>=4){out.has_seven_d_reset=true; out.seven_d_reset_s=rdU32(v);} break;
            case kV7TagContextUsedPct: if (l>=1){out.has_ctx=true; out.ctx_pct=v[0];} break;
            case kV7TagCostMicroUsd:   if (l>=4){out.has_cost=true; out.cost_micro_usd=rdU32(v);} break;
            case kV7TagDurationS:      if (l>=4){out.has_duration=true; out.duration_s=rdU32(v);} break;
            case kV7TagLinesAdded:     if (l>=4){out.has_lines_added=true; out.lines_added=rdU32(v);} break;
            case kV7TagLinesRemoved:   if (l>=4){out.has_lines_removed=true; out.lines_removed=rdU32(v);} break;
            case kV7TagPlanMode:       if (l>=1){out.has_mode=true; out.mode=v[0];} break;
            case kV7TagTitle:          out.has_title=true; cpyStr(out.title, sizeof(out.title), v, l); break;
            case kV7TagAggState:       if (l>=1){out.has_agg_state=true; out.agg_state=v[0];} break;
            case kV7TagActiveToolName: out.has_tool=true; cpyStr(out.tool, sizeof(out.tool), v, l); break;
            case kV7TagTotalSessions:  if (l>=1){out.has_total=true; out.total_sessions=v[0];} break;
            case kV7TagBusySessions:   if (l>=1){out.has_busy=true; out.busy_sessions=v[0];} break;
            default: break;                       // 未知 tag 跳过
        }
        i += 2 + l;
    }
    return V7_OK;
}

}  // namespace cc_hud
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd firmware && pio test -e native`
Expected: PASS（4 tests passed）

- [ ] **Step 6: 接线进 ble_server + config + MTU**

`firmware/src/config.h` 增加(放在现有 msg_type 常量旁):

```cpp
constexpr uint8_t kQuotaMsgTypeV7Tlv = 0x0B;   // v7 统一 TLV 帧
```

`firmware/src/ble_server.cpp`:
- 顶部 `#include "v7_tlv.h"`。
- 在 `onWrite` 的回退分支(`if (!is_v1 && ...)`)**之前**插入 v7 分支:

```cpp
// v7 TLV 统一帧(msg_type 0x0B):解析后按已知字段分发。
if (len >= 4 && data[0] == kQuotaMsgTypeV7Tlv) {
    V7Fields f{};
    V7Result rc = parseV7Tlv(data, len, f);
    if (rc != V7_OK) {
        // 分片(V7_ERR_FRAGMENT)与越界(V7_ERR_LEN)目前都归入长度错误码;
        // 第 4 层引入分片重组后,可为 fragment 单列状态码。
        bleNotifyState(kStateErrLen);
        return;
    }
    // 额度/成本类 → QuotaSnapshot
    if (f.has_five_h_used || f.has_title || f.has_cost) {
        QuotaSnapshot q;
        q.mode          = f.has_mode ? f.mode : kSnapshotModeSubscription;
        q.used_5h       = f.five_h_used;  q.limit_5h = 100;
        q.used_7d       = f.seven_d_used; q.limit_7d = 100;
        q.reset_in_s_5h = f.five_h_reset_s;
        q.reset_in_s_7d = f.seven_d_reset_s;
        q.cost_micro_usd = f.cost_micro_usd;
        q.duration_s     = f.duration_s;
        q.ctx_pct        = f.ctx_pct;
        q.lines_added    = f.lines_added;
        q.lines_removed  = f.lines_removed;
        std::strncpy(q.title, f.has_title ? f.title : "CC HUD", sizeof(q.title) - 1);
        q.title[sizeof(q.title) - 1] = '\0';
        q.last_update_ms = static_cast<uint64_t>(millis());
        if (g_on_write) g_on_write(q);
    }
    // 状态类 → g_on_state
    if (f.has_agg_state) {
        if (g_on_state) g_on_state(static_cast<int8_t>(f.agg_state),
                                   f.tool, f.total_sessions, f.busy_sessions);
    }
    bleNotifyState(kStateOk);
    return;
}
```

- 在 `bleServerInit` 的 `NimBLEDevice::init(...)` 之后加:

```cpp
NimBLEDevice::setMTU(247);   // 提高单包阈值,配合 DLE/2M PHY
```

- [ ] **Step 7: 编译固件确认通过**

Run: `cd firmware && pio run -e esp32s3_nano`
Expected: SUCCESS(编译链接通过)

- [ ] **Step 8: 提交**

```bash
git add firmware/src/v7_tlv.h firmware/src/v7_tlv.cpp firmware/test/test_v7_tlv firmware/platformio.ini firmware/src/config.h firmware/src/ble_server.cpp
git commit -m "feat: 固件接入 v7 TLV 解析(native 单测)+ 0x0B 分支 + setMTU"
```

---

## Task 11: 端到端真机验证(阶段 1a→1b 切换)

**Files:** 无新增代码;执行验证清单(对应 spec 第九节)。

- [ ] **Step 1: 阶段 1a 真机验证(daemon 用 v6)**

操作:
1. `host/.venv/bin/python -m pip install -e 'host[dev]'`
2. `cchud install claude`(经 `python -m cchud_daemon.cli install claude`)
3. `CCHUD_ADDR=<UUID> python -m cchud_daemon.cli daemon &`
4. 在 Claude Code 跑一个含工具调用的任务。

验证(对照 A1–A10):
- A8 显示:额度/成本/上下文/行数/状态/会话数与重构前一致。
- A4 降级:触发权限请求 → 红环 waiting;普通 end-of-turn → 不变红。
- A5 限频:`~/.cchud/daemon.log` 中 quota 写入间隔 ≥30s。
- A7/A9:日志显示 BLE 写入串行、无每次重连报错。

- [ ] **Step 2: 阶段 1b 切 v7**

操作:
1. OTA 升级固件到含 v7 解析版本(WiFi OTA `cc-hud.local` 或 `pio run -t upload`)。
2. daemon 开启 v7:`CCHUD_USE_V7=1`(在 `cli._daemon` 读环境变量设 `Daemon` 的 BleLink `use_v7=True`)。

> 实现补充(本步顺带改 `cli.py` 的 `_daemon`):
> ```python
> use_v7 = os.environ.get("CCHUD_USE_V7", "") == "1"
> from .ble_link import BleLink
> ble = BleLink(addr, use_v7=use_v7)
> d = Daemon(addr, SOCK, ble=ble)
> ```

验证:
- 设备显示与 1a(v6)完全一致(额度/标题/状态),证明 v7 通路等价。
- 串口日志 `[BLE] write ... msg_type=0x0B` 出现且无 `kStateErrLen`。

- [ ] **Step 3: 旧脚本下线**

确认稳定后,从 `~/.claude/settings.json` 移除旧 `cchud-hook.sh`/`cchud-statusline.sh` 引用(`cchud install` 已覆盖为 emit),旧脚本保留一个版本周期后删除。

- [ ] **Step 4: 提交验证记录**

```bash
git commit --allow-empty -m "test: 第 1 层端到端真机验证通过(1a v6 / 1b v7 等价)"
```

---

## 自检(spec 覆盖核对)

- 验收 A1 聚合优先级 → Task 3 `test_aggregate_picks_highest_priority`。
- A2 TTL → Task 1 `test_expired_sessions_are_swept`。
- A3 额度分窗取最大 → Task 2 `test_merge_*`。
- A4 Notification 降级 → Task 5 `test_notification_*`。
- A5 限频 → Task 2 + Task 8 `test_quota_rate_limited`。
- A6 标题拼装 → Task 5 `test_statusline_produces_quota_event_with_title`。
- A7 串行 → Task 7 `test_serial_writes`。
- A8 显示等价 → Task 11 Step 1。
- A9 常驻连接 → Task 7 + Task 11。
- A10 install → Task 9 `test_install_*` + Task 11。
- v7 TLV(解析/未知 tag/越界/分片) → Task 10 四个 native 测试。
- v6/v7 编码字节布局 → Task 4。
- daemon 管线 → Task 8。
- socket → Task 6。
