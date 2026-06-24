# CC-HUD 第 1 层(地基)详细设计:主机侧统一接入层 + v7 协议落地

- 状态:草案,待 review
- 日期:2026-06-24
- 上游:`2026-06-24-cc-hud-roadmap-design.md`(全局路线图)
- 范围:第 1 层(地基)。常驻 daemon + 适配器层 + Claude adapter 迁移 + v7 TLV 固件解析骨架 + CLI 安装。
- 不在本范围:Codex/Gemini/Qwen adapter(第 2 层)、介入页/会话列表页(第 3/4 层)。本层只保证 **Claude 现有能力功能等价**,并铺好后三层的地基与协议。

## 一、目标与验收

### 目标

1. 把现有"无状态 hook 脚本 + `/tmp` 文件 IPC"重构为单一常驻进程 `cchud-daemon`,内部持有会话/额度状态与常驻 BLE 连接。
2. 抽出 **adapter 层**,把现有 Claude 逻辑迁为"第一个 adapter",核心逻辑客户端无关。
3. 固件接入 **v7 TLV 解析**(与旧 v1–v6 并存),daemon 用 v7 编码下发。
4. 提供 `cchud` CLI:一键安装/校验 hook、管理 daemon。

### 验收标准(逐条对照现状,功能等价)

| # | 验收项 | 现状来源 |
|---|--------|---------|
| A1 | 多会话状态聚合优先级 `waiting>tool>thinking>idle` 不变 | `cchud-hook.sh:90-115` |
| A2 | 死会话 TTL 清理(默认 300s)语义不变 | `cchud-hook.sh:96-98` |
| A3 | 额度跨会话按 `resets_at` 分窗取最大值合并 | `cchud-quota-push.sh:57-76` |
| A4 | Notification 降级:仅真权限请求算 `waiting`,否则降级 `idle` | `cchud-hook.sh:52-58` |
| A5 | 额度推送 30s 限频 | `cchud-update.sh:58-64` |
| A6 | 标题拼装 `模型名 (1M context)` | `cchud-quota-push.sh:82-100` |
| A7 | BLE 写入串行(不并发连同一设备) | `ble.lock` 机制 |
| A8 | 设备显示与现状一致(额度/成本/上下文/行数/状态/会话数) | v6 + state 字段 |
| A9 | BLE 不再每次推送重连(常驻连接) | 新增收益 |
| A10 | `cchud install claude` 一键写好 hook + statusLine | 替代手工编辑 settings.json |

## 二、现状盘点(迁移基线)

### 2.1 现有主机脚本职责

| 脚本 | 职责 | 关键隐含约定 |
|------|------|------------|
| `cchud-hook.sh` | 应用状态:每会话写 `sessions/<sid>.state`,抢 `ble.lock` 后聚合推送 | 优先级聚合、TTL、Notification 降级、收敛重推(≤3 轮) |
| `cchud-statusline.sh` | statusLine 包装,转交 quota-push | — |
| `cchud-quota-push.sh` | 从 statusline JSON 抽额度/成本/标题,跨会话合并额度 | 按 resets_at 分窗取最大、标题拼装 |
| `cchud-update.sh` | 30s 限频 + 后台 fork + 抢 `ble.lock` 调 push_quota | 限频时间戳、共享 BLE 锁 |
| `push_quota.py` | v6 payload 编码 + BLE 写(直连/扫描/discover) | response=True 必须、重试 1 次 |
| `push_state.py` | 0x07 state payload 编码 + BLE 写 | total/busy 可选尾字节 |

### 2.2 现有 `/tmp` IPC 机制 → daemon 内存映射

这是迁移的核心:文件系统 IPC 在 daemon 内坍缩为内存状态。**每一项都必须有对应承接,否则丢行为。**

| 现有 `/tmp` 机制 | 作用 | daemon 内承接 |
|-----------------|------|--------------|
| `cchud/sessions/<sid>.state` + mtime | 多进程间共享每会话状态 + 存活信号 | `SessionStore`:`dict[sid → SessionState]`,含 `last_seen` 时间戳 |
| `cchud/quota.state` + `quota-state.lock` | 跨会话额度合并 | `QuotaTracker`:内存中按窗口保留最大值 |
| `cchud/ble.lock`(mkdir 原子锁) | 串行化 BLE 访问(macOS 单连接) | `BleLink`:单一 asyncio 任务串行处理发送队列 |
| `cchud-last-push.ts` | 额度 30s 限频 | `QuotaTracker`:内存限频时间戳 |
| `cchud/last-pushed.state` | 状态去重(不变不推) | `Aggregator`:内存中上次推送指纹 |

### 2.3 固件 BLE 解析现状

- `ble_server.cpp` `QuotaCallbacks::onWrite` 按 `(length, msg_type)` 路由;已用 msg_type `0x01–0x0A`(见路线图 7.4 对照)。
- onConnect 已设 **DLE 251 + 请求 2M PHY**(`ble_server.cpp:284-298`),但**未主动设 ATT MTU**(依赖 central 协商,macOS 通常 ~185B)。
- `bleNotifyState` 回包限 20 字节(保守兼容 23B MTU)。

## 三、daemon 架构

### 3.1 进程与生命周期

- **单进程** `cchud-daemon`,Python(沿用 `bleak`,迁移成本最低;`asyncio` 事件循环统一驱动 socket 与 BLE)。
- **守护**:macOS `launchd` user agent(`~/Library/LaunchAgents/io.cchud.daemon.plist`),`KeepAlive=true` 崩溃自启,`RunAtLoad=true`。
- **日志**:`~/.cchud/daemon.log`(轮转);状态目录从 `/tmp/cchud` 迁到 `~/.cchud/`(跨重启保留),`/tmp` 仅留 socket。
- **socket**:Unix domain socket `~/.cchud/daemon.sock`。

### 3.2 模块边界

每个模块单一职责、可独立测试。

```
        hook 瘦客户端 ──JSON──► SocketServer
                                   │ 规范化(经 AdapterRegistry)
                                   ▼
                              CcHudEvent
                                   │
                ┌──────────────────┼───────────────────┐
                ▼                  ▼                     ▼
          SessionStore       QuotaTracker          (其它未来)
                └────────┬─────────┘
                         ▼
                     Aggregator  ── 计算聚合态/快照,去重 ──►  BleLink
                                                               │ v7 编码 + 串行发送队列
                                                               ▼
                                                          BLE 设备(常驻连接)
```

| 模块 | 职责 | 对应现状 |
|------|------|---------|
| `SocketServer` | 监听 UDS,收 hook JSON,交 AdapterRegistry 规范化 | 替代"每 hook fork 进程" |
| `AdapterRegistry` | 持有各 adapter,按 client 字段路由 `normalize()` | 新增(地基) |
| `ClaudeAdapter` | 把 Claude hook/statusline 数据 → `CcHudEvent` | 迁移 `cchud-hook.sh` + `cchud-quota-push.sh` 逻辑 |
| `SessionStore` | 维护会话状态 + TTL 清理 | 承接 `sessions/*.state` |
| `QuotaTracker` | 额度分窗合并 + 限频 | 承接 `quota.state` + `last-push.ts` |
| `Aggregator` | 聚合态计算 + 快照组装 + 去重 | 承接 `cchud-hook.sh` 聚合 + `last-pushed.state` |
| `BleLink` | 常驻连接 + 重连退避 + v7 编码 + 串行发送 | 承接 `ble.lock` + `push_*.py` |

### 3.3 BleLink:常驻连接

- 启动后扫描/直连设备地址(`CCHUD_ADDR`),建立后**保持连接**;断线指数退避重连(0.5s→…→上限 30s)。
- 内部 `asyncio.Queue` 串行处理发送项,天然满足"不并发连同一设备",取代 `ble.lock`。
- 连接建立后**主动协商 MTU**(bleak 在 macOS 由系统协商,记录协商结果到日志;Linux 可显式 `client.mtu_size`),并校验是否满足单包阈值。
- 写入沿用 `response=True`(macOS + NimBLE 硬性要求)与单次重试。

## 四、Adapter 层与统一事件模型

### 4.1 `CcHudEvent`(规范化事件)

adapter 的唯一产物,核心逻辑只认它:

```python
@dataclass
class CcHudEvent:
    client_id: int            # 0=claude(第1层只有它);第2层扩展
    session_id: str
    kind: str                 # "state" | "quota" | "idle"
    # --- state 类 ---
    state: str | None         # idle|thinking|tool|waiting|done
    detail: str | None        # 工具名
    # --- quota 类 ---
    five_h_used: int | None    # 官方百分比
    five_h_resets_at: int | None
    seven_d_used: int | None
    seven_d_resets_at: int | None
    ctx_pct: int | None
    cost_usd: float | None
    duration_s: int | None
    lines_added: int | None
    lines_removed: int | None
    title: str | None
    mode: str | None          # sub|api
    ts: float                 # 事件接收时刻
```

### 4.2 `Adapter` 接口

```python
class Adapter(Protocol):
    client_id: int
    name: str
    def normalize(self, raw: dict) -> list[CcHudEvent]: ...   # 一条 hook 可产 0..N 事件
    def hook_spec(self) -> HookSpec: ...                      # 供 `cchud install` 写配置
```

### 4.3 `ClaudeAdapter`(迁移要点)

把现有两段 shell 逻辑搬进来,**逐项对应** 2.2 的隐含约定:

- 入参:hook 事件(含 `session_id`、`tool_name`、`message`)与 statusline JSON。
- `state` 事件:复刻 `cchud-hook.sh` 的状态映射;**A4 Notification 降级**——`message` 不含 permission/approve/confirm/needs-your 时降级 `idle`(第 3 层会改为细分 `INTERVENTION_KIND`,本层先保留降级,行为等价)。
- `quota` 事件:复刻 `cchud-quota-push.sh` 的字段抽取与 **A6 标题拼装**;跨会话合并交给 `QuotaTracker`(A3)。

## 五、Socket 事件协议(hook → daemon)

hook 脚本退化为瘦客户端:读 stdin → 包成 JSON → 写 socket → 立即退出(socket 不可用则**静默退出 0**,绝不阻塞客户端)。

```json
{ "client": "claude", "event": "PreToolUse", "payload": { /* 原始 hook stdin + statusline JSON */ } }
```

- daemon 侧由 `AdapterRegistry` 按 `client` 选 adapter 调 `normalize()`。
- 瘦 hook 用一个通用脚本 `cchud-emit.sh <client> <event>`(或编译版),所有客户端共用,差异在 daemon 的 adapter 里。

## 六、v7 TLV 固件侧实现

### 6.1 落点

在 `QuotaCallbacks::onWrite` 路由链尾部、回退分支前,新增:

```cpp
if (len >= 4 && data[0] == kQuotaMsgTypeV7Tlv) {   // 0x0B
    handleV7Tlv(data, len);
    return;
}
```

### 6.2 帧与解析(对应路线图 7.2/7.3)

- 帧头:`[0]=0x0B [1]=flags [2]=seq [3]=total`,其后为连续 `[tag][len][value]`。
- 第 1 层**所有帧保证单包**(等价字段总长 <100B < 协商 MTU),`flags.bit0=0, seq=0, total=1`。
- 解析循环:游标从 4 开始,逐字段读 `tag/len`,`len` 越界即 `kStateErrLen` 返回;**未知 tag 跳过**(读 len 跳 value),保证前向兼容。
- 把已知 tag 填进一个统一结构(扩展现有 `QuotaSnapshot` + 复用 state 分发):
  - 额度/成本/标题/上下文/行数 tag → 填 `QuotaSnapshot` → 调 `g_on_write`。
  - `AGG_STATE/ACTIVE_TOOL_NAME/TOTAL_SESSIONS/BUSY_SESSIONS` → 调 `g_on_state`。
  - `UNIX_TS/UTC_OFFSET_MIN/STATUS_STRING/IDLE_FORCE/MOOD_PREVIEW` → 调对应 idle 路径。
- **分片骨架**:本层解析 `seq/total` 字段但只接受 `total=1`;`total>1` 暂回 `kStateErrLen` 并日志告警。完整重组缓冲留到第 4 层(届时需要会话列表才超单包)。

### 6.3 MTU 加固(建议)

固件 `bleServerInit` 增加 `NimBLEDevice::setMTU(247)`(preferred),配合已有 DLE/2M PHY,确保单包阈值稳定;daemon 连接后记录实际协商值,低于阈值时日志告警并退回小帧策略。

### 6.4 新增常量(`config.h`)

`kQuotaMsgTypeV7Tlv = 0x0B`;v7 tag 枚举(按路线图 7.4 tag 表全量定义,本层只填充已规划字段,预留 tag 留空)。

## 七、CLI:`cchud`

| 子命令 | 行为 |
|--------|------|
| `cchud install claude` | 写入 `~/.claude/settings.json` 的 5 个 hook + statusLine(指向 `cchud-emit.sh`);幂等、自动备份原配置 |
| `cchud status` | 探测 daemon 存活、BLE 连接、最近事件/推送、当前聚合态 |
| `cchud daemon` | 前台运行 daemon(调试用);正式由 launchd 拉起 |
| `cchud uninstall claude` | 还原 hook 配置 |

借鉴 ping-island `HookInstaller`/`ClientProfile` 思路:`hook_spec()` 由各 adapter 提供,`install` 通用。

## 八、迁移策略(解耦"架构重构"与"协议切换")

分两步落地,各自独立可回退,降低风险:

- **阶段 1a:架构重构,协议仍用 v6/0x07。** 建 daemon 全部模块 + ClaudeAdapter + socket + 瘦 hook;BleLink 先内联现有 v6 quota / 0x07 state 编码。此时固件**无需改动**,验证 A1–A10(除 A9 外行为等价 + 常驻连接)。
- **阶段 1b:协议切 v7。** 固件加 v7 解析(与旧并存);daemon BleLink 切 v7 编码。固件可单独 OTA 升级,daemon 通过开关回退 v6,保证新旧固件都能用。

旧脚本(`cchud-hook.sh` 等)在 1a 完成后保留一个版本周期作回退,确认稳定后删除。

## 九、回归验证清单

每条对应第一节验收项,给出可执行验证方式:

1. **A1/A2 聚合**:模拟 3 个 sid 并发写不同 state(含一个超 TTL),`cchud status` 应显示正确聚合态与会话计数。
2. **A3 额度合并**:用两份不同 `resets_at` 的 statusline JSON 喂入,确认新窗口替换、同窗口取最大。
3. **A4 降级**:发 `Notification` 含/不含 "permission" 两种 message,确认仅前者进 `waiting`。
4. **A5 限频**:连续高频 quota 事件,确认 BLE 写间隔 ≥30s。
5. **A6 标题**:喂含 `model.display_name` + `context_window_size` 的 JSON,确认标题为 `Opus 4.8 (1M context)`。
6. **A7 串行 / A9 常驻**:并发 quota+state 事件,确认 BLE 不并发、不重连(日志单一连接)。
7. **A8 显示**:真机比对额度/成本/上下文/行数/状态/会话数与重构前一致。
8. **v7**:固件 v7 帧解析单元测试(已知 tag、未知 tag 跳过、越界 len 报错、total>1 告警);真机 1b 后端到端比对。
9. **A10 安装**:干净环境 `cchud install claude` 后 settings.json 正确、备份生成、再次执行幂等。

## 十、风险与缓解

| 风险 | 缓解 |
|------|------|
| daemon 僵死导致设备停更 | launchd KeepAlive + `cchud status` 健康探测 + hook 静默降级不阻塞客户端 |
| 重构丢失隐含行为 | 第 2.2/2.3 节逐项映射 + 第九节逐条回归 |
| v7 与旧固件不兼容 | 1a/1b 解耦;daemon v6/v7 开关;旧路径原样保留 |
| MTU 协商不足 | 固件 setMTU(247) + daemon 记录协商值 + 单包阈值守护 |
| Python 常驻内存/稳定性 | 模块无状态化、定期 GC、日志监控;崩溃由 launchd 兜底 |

## 十一、后续

第 1 层 spec review 通过后进入 writing-plans,产出实现计划(建议拆分:① daemon 骨架+socket+SessionStore/QuotaTracker/Aggregator;② ClaudeAdapter 迁移+回归;③ BleLink 常驻连接;④ 固件 v7 解析;⑤ CLI install)。
