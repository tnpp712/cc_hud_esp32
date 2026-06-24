# CC-HUD 演进路线图设计(对标 ping-island)

- 状态:草案,待 review
- 日期:2026-06-24
- 范围:全局路线图(4 层演进)+ v7 协议总设计
- 不在本文范围:单层的实现细节(每层后续各自出 spec→plan)

## 一、背景与目标

CC-HUD 是一个基于 ESP32-S3 + 1.54" 240×240 ST7789 屏 + 24 颗 WS2812B 灯环的实体 HUD,通过 BLE 接收主机推送,实时展示 Claude Code 的额度、会话成本、工作状态。当前固件已相当成熟(4 屏轮播、燃烧率预测、WiFi OTA、夜间调暗、灯环状态),但主机侧深度绑定 Claude Code,其他客户端(Codex 仅有"完成绿脉冲")基本缺席。

参考项目 ping-island 是一个 macOS 灵动岛应用,支持 13+ AI 编码客户端,核心能力包括:多客户端统一接入、权限批准/追问提醒、多会话聚合、一键跳回、音效通知。它工程上最值得借鉴的是 **单一真实源 + 接入边界规范化** 的架构:所有客户端差异被压缩在 hook 规范化处,核心逻辑完全客户端无关。

本路线图的目标:把 CC-HUD 从"Claude 专用状态灯"演进为"客户端无关的实体 AI 工作台提醒器",并在演进中放大硬件相对屏幕应用的**独有优势**。

## 二、设计原则:硬件的差异化是「离屏感知」

ping-island 是"屏幕里的应用",强在**交互**(在岛里直接批准、回答、跳回窗口);CC-HUD 是"桌面上的实体设备",强在**离屏感知**(不盯屏幕也能用余光/灯环/未来的声震,知道"AI 在等我")。

因此本路线图**不照搬** ping-island 的全部功能(如一键跳回、在设备上回答问题——这些屏幕应用做得更好),而是聚焦能放大"实体提醒"优势的能力:

1. 把"需要你介入"这件事做成**强物理信号**(屏幕大字 + 红环强闪,未来加声/震)。
2. 让设备**不挑客户端**——无论用 Claude / Codex / Gemini / Qwen,都能看到一致的状态。
3. 在小屏上对多会话做**信息优先级取舍**,让"在等你的那个"最显眼。

## 三、对标结论

| 维度 | CC-HUD 现状 | ping-island | 路线图取舍 |
|------|------------|-------------|-----------|
| 多客户端 | 深度只做 Claude;Codex 仅完成脉冲 | 13+ 客户端统一接入 | 借鉴:适配器层 + 统一事件模型 |
| "需介入"提醒 | 有 waiting 红环,不区分类型/无详情 | 显示工具名+参数、问题文本 | 借鉴语义,用**物理强提醒**表达 |
| 多会话 | 主机聚合成单态,屏幕只显示聚合 | 完整会话列表 + 优先级排序 | 借鉴,小屏做信息取舍 |
| 反馈通道 | 仅视觉(屏+灯) | 系统音 + 音效包 | 协议/固件留声震接口,硬件未来焊接 |
| 主机可扩展性 | bash+python 针对 Claude 硬编码 | 统一事件模型 + 配置自动安装 | 借鉴:常驻 daemon + adapter + 自动装 hook |
| 交互(批准/跳回) | 无 | 在岛内直接操作 | **不搬**,非硬件优势 |

## 四、目标态架构(4 层全部落地后)

```
┌──────────────── 主机侧 (macOS) ────────────────┐        ┌─────── 设备 (ESP32-S3) ───────┐

 各客户端 hooks
  ├ Claude Code ─┐
  ├ Codex CLI ───┤   [适配器层]  每家一个 adapter,
  ├ Gemini CLI ──┼─► 把原生 hook/JSONL 规范化
  ├ Qwen / … ────┘   为统一 CcHudEvent
                          │
                          ▼
              ┌─ cchud-daemon (常驻) ─┐
              │  SessionStore 单一真相 │
              │  Aggregator  优先级聚合│
              │  QuotaTracker 额度/成本│
              │  BleLink 常驻BLE+v7编码 │──BLE(v7 TLV)──►  协议解析 v1-v6(旧) + v7(新)
              └────────────────────────┘                   设备侧轻量 SessionStore
                          ▲                                 ├ HUD/STATS/TOOL/CLOCK(现有)
              cchud install <client>                        ├ 介入页(第3层新增)
              自动写各客户端 hook 配置                       ├ 会话列表页(第4层新增)
                                                            └ 灯环 +(预留 声/震 GPIO)
```

关键结构变化(相对现状):

- **主机侧从无状态脚本 → 常驻 daemon**。现在每次 hook 都 fork 进程、读 `/tmp` 文件聚合、抢 flock 再重连 BLE(6s 超时)。daemon 持有 SessionStore 与常驻 BLE 连接,hook 退化为"发一条轻量事件给 daemon"的瘦客户端,提醒实时性大幅提升,多会话/多客户端天然支持。
- **接入边界规范化**。每个客户端一个 adapter,把原生 hook/JSONL 事件翻译成统一 `CcHudEvent`;daemon 核心逻辑客户端无关。
- **设备侧引入轻量 SessionStore**。从"单一聚合态"升级到能持有"会话列表 + 当前介入",为第 3、4 层铺路(ESP32-S3 有 8MB PSRAM,容量充足)。

## 五、四层演进路线图

每层独立交付价值,各自出 spec→plan→实现。依赖关系:第 1 层是地基,2/3/4 层都长在其上。

### 第 1 层:主机侧统一接入层(地基)

- **目标**:把现有 Claude 硬编码脚本重构为"daemon + 适配器层"架构,并定稿 v7 协议。
- **主机交付物**:
  - `cchud-daemon` 常驻进程:SessionStore(单一真相)、Aggregator(优先级聚合 `waiting>tool>thinking>idle`)、QuotaTracker、BleLink(常驻 BLE + v7 TLV 编码)。
  - 适配器接口 + **Claude adapter**(把现有 `cchud-hook.sh` / `cchud-quota-push.sh` 的逻辑迁为第一个 adapter)。
  - 瘦 hook 脚本:仅向 daemon 发事件(Unix domain socket,借鉴 ping-island `HookSocketServer`)。
  - `cchud install <client>` / `cchud status`:自动安装/校验各客户端 hook 配置与 statusLine。
  - daemon 生命周期:`launchd` plist(macOS),崩溃自启,日志落 `~/.cchud/`。
- **设备交付物**:固件接入 v7 TLV 解析骨架;设备侧轻量 SessionStore;旧 v1–v6 解析路径原样保留。
- **协议**:**v7 总设计定稿**(见第七节)。
- **验收**:Claude 全部现有能力在新架构下功能等价;BLE 不再每次重连;`cchud install claude` 一键可用。

### 第 2 层:多客户端一等公民

- **目标**:Codex / Gemini / Qwen 等做成与 Claude 同级的完整状态接入(thinking/tool/waiting/idle + 成本 + 标题 + 会话元数据),而非仅"完成脉冲"。
- **主机交付物**:Codex adapter、Gemini adapter、Qwen adapter(纯增量,各自把原生 hook/JSONL 映射为 `CcHudEvent`);`cchud install codex|gemini|qwen`。
- **设备交付物**:品牌/图标随当前活跃客户端切换(tag `CLIENT_ID`);"心情猫/Claude logo"不再写死。
- **协议**:复用 v7,启用 `CLIENT_ID` / `CLIENT_NAME` tag。
- **验收**:任一客户端单独使用时,设备显示与 Claude 体验一致。

### 第 3 层:强化物理提醒(硬件核心价值)

- **目标**:把"需要你介入"做成强物理信号,区分 `等批准` / `等回答问题` / `出错`,屏幕显示工具名/问题摘要。
- **主机交付物**:Aggregator 在 waiting 态细分 `INTERVENTION_KIND`,携带工具名、问题摘要、来源会话。
- **设备交付物**:新增**介入页**——大字显示 `⚠ 等你批准: Bash` / `❓ 问题: 选哪个方案`,红环强闪(1Hz→高强度);预留声/震驱动接口(`ALERT_CHANNELS` bitmask 已定义,焊接后无需改协议)。
- **协议**:启用介入域 tag(0x40–0x5F)。
- **验收**:Claude/Codex 触发工具批准或 AskUserQuestion 时,设备 3 秒内进入介入页并强提醒;清除后自动恢复。

### 第 4 层:多会话概览

- **目标**:小屏呈现多个会话,"在等你的"排最前。
- **主机交付物**:daemon 推送会话列表摘要(每条:客户端、状态、短标题),按优先级排序。
- **设备交付物**:新增**会话列表页**,小屏分行显示,介入态置顶高亮;与现有轮播/锁定逻辑协调。
- **协议**:启用会话列表域 tag(0x60–0x7F),处理分片(见第七节 MTU)。
- **验收**:并发 3+ 会话时,设备列表页正确排序与高亮,刷新无闪烁。

## 六、主机侧 daemon 形态

- **进程**:单一常驻进程 `cchud-daemon`,由 `launchd` 守护(KeepAlive),崩溃自启。
- **接入**:监听 Unix domain socket(如 `~/.cchud/daemon.sock`),hook 脚本以瘦客户端身份发送 JSON 事件;socket 不可用时 hook 静默降级(不阻塞客户端)。
- **适配器层**:`Adapter` 接口约定 `normalize(rawEvent) -> CcHudEvent`;`ClientProfile` 注册表登记每个客户端的 hook 路径、JSONL 路径、品牌、`CLIENT_ID`(借鉴 ping-island `ClientProfile.swift`)。
- **SessionStore**:以 `sessionId` 为键维护会话状态(阶段、工具、标题、最后活动时间、介入上下文);TTL 清理死会话(沿用现 5 分钟语义)。
- **BleLink**:常驻持有设备连接,断线重连退避;统一负责 v7 TLV 编码与(必要时)分片;替代现有"每次推送 flock + 重连"。
- **实现语言**:建议 Python(沿用现有 `bleak` BLE 栈,迁移成本最低);daemon 化与 socket 用标准库即可。语言最终在第 1 层 spec 定。

## 七、v7 协议总设计(TLV 可扩展帧)

### 7.1 为什么转 TLV

v1–v6 是"固定布局 + 版本号",固件已背 6 个解析分支。后三层还要加大量字段。TLV 让固件用一个循环解析全部字段,以后加能力只是**分配新 tag**,不再升版、不再加分支;daemon 也只发设备关心的字段。旧 v1–v6 路径原样保留,平滑过渡。

### 7.2 帧结构

复用现有 GATT(Service `12345678-aaaa-bbbb-cccc-1234567890ab`,Quota/State Characteristic 不变)。新增统一 msg_type:

```
偏移  字段        类型   说明
0     msg_type    u8     0x0B = CCHUD_MSG_V7_TLV
1     flags       u8     bit0=MORE_FRAGMENTS  bit1..7 预留
2     seq         u8     分片序号(未分片时为 0)
3     total       u8     分片总数(未分片时为 1)
4..N  fields      TLV*   连续若干 [tag:u8][len:u8][value:len]
```

- 未分片帧:`flags.bit0=0, seq=0, total=1`,字段直接跟在头后。
- 单字段格式:`[tag:u8][len:u8][value: len 字节]`;`len=0` 表示存在性标志位字段。
- 未知 tag:固件**跳过**(读 len 跳过 value),保证前向兼容。

### 7.3 MTU 与分片

- BLE 默认 ATT MTU 23 字节(有效载荷 20)。NimBLE 支持 MTU 协商至 ~512;**daemon 连接后先协商更大 MTU**,绝大多数帧单包可达。
- 仅当单帧(如会话列表、长问题文本)超出协商后 MTU 时启用分片:同一 msg_type,`flags.bit0=1`,按 `seq`/`total` 重组;固件侧设重组缓冲与超时丢弃。
- 高频状态帧(state/quota)应控制在单包内,避免分片开销。

### 7.4 完整 tag 分配表

tag 按域分区,预留充足空间;`✓`=本路线图已规划,`预留`=未来扩展。

#### 额度 / 成本域(0x01–0x1F)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0x01 | FIVE_HOUR_USED_PCT | u8 | 1 | 5h 用量百分比 |
| 0x02 | FIVE_HOUR_RESET_IN_S | u32 | 1 | 5h 重置倒计时(秒) |
| 0x03 | SEVEN_DAY_USED_PCT | u8 | 1 | 7d 用量百分比 |
| 0x04 | SEVEN_DAY_RESET_IN_S | u32 | 1 | 7d 重置倒计时(秒) |
| 0x05 | CONTEXT_USED_PCT | u8 | 1 | 上下文窗口用量 |
| 0x06 | COST_MICRO_USD | u32 | 1 | 会话成本(微美元) |
| 0x07 | DURATION_S | u32 | 1 | 会话时长(秒) |
| 0x08 | LINES_ADDED | u32 | 1 | 代码增加行 |
| 0x09 | LINES_REMOVED | u32 | 1 | 代码删除行 |
| 0x0A | PLAN_MODE | u8 | 1 | 0=sub,1=api |
| 0x0B | TITLE | utf8 | 1 | 屏幕标题(计划名) |
| 0x0C–0x1F | — | — | 预留 | 额度/成本扩展 |

#### 会话 / 状态域(0x20–0x3F)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0x20 | AGG_STATE | u8 | 1 | 聚合态 0=idle 1=thinking 2=tool 3=waiting 4=done |
| 0x21 | ACTIVE_TOOL_NAME | utf8 | 1 | 当前工具名 |
| 0x22 | TOTAL_SESSIONS | u8 | 1 | 总会话数 |
| 0x23 | BUSY_SESSIONS | u8 | 1 | 忙碌会话数 |
| 0x24 | ACTIVE_SESSION_TITLE | utf8 | 2 | 当前活跃会话短标题 |
| 0x25–0x3F | — | — | 预留 | 会话/状态扩展 |

#### 介入 / 提醒域(0x40–0x5F,第 3 层)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0x40 | INTERVENTION_KIND | u8 | 3 | 0=none 1=approval 2=question 3=error |
| 0x41 | INTERVENTION_TOOL | utf8 | 3 | 等批准的工具名 |
| 0x42 | INTERVENTION_TEXT | utf8 | 3 | 问题摘要/提示文本 |
| 0x43 | INTERVENTION_SOURCE | utf8 | 3 | 来源会话短标识 |
| 0x44 | ALERT_LEVEL | u8 | 3 | 提醒强度(驱动灯环/未来声震) |
| 0x45 | ALERT_CHANNELS | u8(bitmask) | 3 | bit0=屏 bit1=灯 bit2=蜂鸣 bit3=震(声/震预留) |
| 0x46–0x5F | — | — | 预留 | 介入扩展 |

#### 会话列表域(0x60–0x7F,第 4 层)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0x60 | SESSION_LIST_COUNT | u8 | 4 | 列表条数 |
| 0x61 | SESSION_ENTRY | bytes(子记录) | 4 | 一条会话摘要,见下 |
| 0x62–0x7F | — | — | 预留 | 列表扩展 |

`SESSION_ENTRY`(0x61)value 子结构(定长头 + 变长标题):

```
偏移  字段          类型   说明
0     entry_idx     u8     排序序号(0 最优先)
1     client_id     u8     客户端(见 CLIENT_ID 取值)
2     state         u8     同 AGG_STATE 取值
3     intervention  u8     同 INTERVENTION_KIND(0=无)
4     title_len     u8     短标题字节数
5..   title         utf8   短标题
```

#### 设备控制 / 时间域(0x80–0x9F)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0x80 | UNIX_TS | u32 | 1 | 当前 unix 时间 |
| 0x81 | UTC_OFFSET_MIN | i16 | 1 | 时区偏移(分钟) |
| 0x82 | IDLE_FORCE | u8 | 1 | 强制空闲(对应旧 0x05) |
| 0x83 | MOOD_PREVIEW | u8 | 1 | 心情预览 0–4(对应旧 0x06) |
| 0x84 | STATUS_STRING | utf8 | 1 | 空闲页天气/状态串 |
| 0x85–0x9F | — | — | 预留 | 设备控制扩展 |

#### 客户端 / 品牌域(0xA0–0xBF,第 2 层)

| tag | 名称 | 类型 | 引入层 | 说明 |
|-----|------|------|--------|------|
| 0xA0 | CLIENT_ID | u8 | 2 | 0=claude 1=codex 2=gemini 3=qwen 4=cursor …(注册表统一) |
| 0xA1 | CLIENT_NAME | utf8 | 2 | 客户端显示名 |
| 0xA2–0xBF | — | — | 预留 | 客户端扩展 |

#### 厂商 / 实验预留(0xC0–0xFF)

整段预留给实验性/厂商私有字段,固件遇到一律跳过。

### 7.5 旧协议与迁移

- msg_type `0x01–0x0A` 与 WiFi 凭证 `0x09` 路径**原样保留**;v7 不替换它们,只承载新能力。
- WiFi 凭证(敏感、一次性)继续走独立 msg_type,不进 v7 通用帧。
- 迁移顺序:第 1 层 daemon 先用 v7 重发"现有等价字段",验证 TLV 通路与 MTU 协商无误后,旧字段逐步只保留兼容。
- 固件协商失败回退:若 MTU 协商失败,daemon 退回单包小帧 + 必要分片,保证旧设备/弱链路可用。

## 八、关键风险与取舍

| 风险 | 影响 | 缓解 |
|------|------|------|
| daemon 生命周期复杂度 | 后台进程崩溃/僵死导致设备失更新 | launchd KeepAlive + 心跳日志 + hook 静默降级 |
| BLE MTU/分片 | 会话列表/长文本超单包 | 连接先协商大 MTU;仅超长才分片;高频帧控制单包 |
| 固件内存/渲染 | 介入页/列表页 + 多会话状态增加占用 | 复用 PSRAM;短标题截断;LVGL 对象复用 |
| 多客户端 hook 差异 | 各家事件语义不一 | adapter 边界吸收差异;核心只认 `CcHudEvent` |
| 中文/UTF8 显示 | 工具名/标题/问题文本含中文 | 复用现有 20pt CJK 字体;按宽度截断 |
| 范围蔓延 | 4 层一起做易失控 | 严格分层交付,每层独立 spec→plan→验收 |

## 九、后续步骤

1. 本路线图 review 通过后,**第 1 层(地基)单独出 spec**:细化 daemon 模块边界、adapter 接口、socket 事件格式、launchd 配置、v7 在固件侧的解析实现与回归验证清单。
2. 第 1 层 plan→实现→验收(Claude 功能等价 + BLE 常驻)。
3. 依次推进第 2/3/4 层,各自 spec→plan→实现。

## 附:关键文件参照(现状)

- 固件主循环/状态机:`firmware/src/main.cpp`
- BLE GATT 解析:`firmware/src/ble_server.cpp`
- 全局配置/UUID/协议常量:`firmware/src/config.h`
- 4 屏 UI:`firmware/src/lvgl_ui.cpp`
- 灯环:`firmware/src/led_ring.cpp`
- 现有主机 hook:`host/cchud-hook.sh`、`host/cchud-quota-push.sh`、`host/cchud-update.sh`
- 现有 BLE 推送:`host/push_quota.py`、`host/push_state.py`
- 接入指南:`host/HOOKS_SETUP.md`
