# AI语音 · Wi-Fi UDP 通道设计（2026-09-12）

> 本文是**设备端与 companion 端共同遵守的契约**。改一侧必须同步另一侧。

## 0. 一句话

把 AI语音 的上行通道从 BLE 换成 Wi-Fi UDP：设备与电脑在同一局域网，companion 每秒广播 beacon，
设备听到后学到电脑 IP，之后音频与事件走 UDP 单播；**BLE 整层移除**。

---

## 1. 为什么换（BLE 的现状代价）

| 代价 | 证据 |
| --- | --- |
| 进语音就杀 Wi-Fi，其他应用全断 | `app_voice.cc` `suspend_wifi_driver()` → `StopStation()` + `esp_wifi_deinit()` |
| NimBLE controller 常驻 ~44.7KB，剩余最大连续块仅 ~7KB | `ble_audio.c` 原注释 |
| 发送栈 4096→5120（+1KB）即 `ble_hs_start` 失败 → **重启循环** | 同上 |
| 射频干扰造成幽灵按键 | `app_state.c` 为此堆了 5 组 mV 门禁 |
| 代码体量 | `ble_audio.c` 747 行，占 voice 传输层近一半 |

**历史事实**：本站点原本存在过 Wi-Fi 通道，用的是 **WebSocket + mDNS**（`ws_client.c` / `mdns_resolver.c` /
`wifi_app.c`），已随重构整体移除，且 git 历史被 squash 过（只剩 1 个 commit），不可恢复。
本次用 **UDP** 重建，理由：

- 无需 HTTP Upgrade / mTLS 握手，设备端代码量约为 WS 的 1/3；
- UDP 报文天然有边界 —— 一个数据报就是一个音频块，**不需要分片重组**；
- ADPCM 块自带 predictor/index 头，**丢一块只损失 100ms 且不失步**，与 UDP 的丢包模型天然契合；
  换成 PCM 反而会因为缺字节导致后续全部错位；
- 端点已存在：首页头像上传早就跑在 Wi-Fi 上（真机 `http://192.168.5.4`），Wi-Fi 栈是已验证路径。

---

## 2. 角色与端口

| 端 | 角色 | 行为 |
| --- | --- | --- |
| companion（电脑） | 服务端 | `bind 0.0.0.0:33333`；每 **1000ms** 向 `255.255.255.255:33333` 广播 beacon |
| 设备（ESP32-C3） | 客户端 | `bind 0.0.0.0:33333`（收 beacon）；学到电脑 IP 后向 `<pc_ip>:33333` 单播 |

端口常量：`VOICE_UDP_PORT = 33333`（双向同端口）。

- 设备**不广播**：它只需要听。
- companion 由 `recvfrom` 的来源地址学出设备地址，因此不需要设备做任何宣告。
- companion 收到 `type==0x03`（beacon）一律忽略 —— 那是自己广播出去的回环，不是设备发来的。

---

## 3. 报文格式

```
每个 UDP 数据报 = [1B type][payload]
```

| type | 方向 | payload | 总长（含 1B type） |
| --- | --- | --- | --- |
| `0x01` AUDIO | 设备→电脑 | `[seq][0x80]` + 804B IMA ADPCM block | 807B |
| `0x02` EVENT | 双向 | UTF-8 JSON 行（**含结尾 `\n`**） | ≤ 513B |
| `0x03` BEACON | 电脑→设备 | UTF-8 JSON `{"v":1,"role":"pc","port":33333}` | ~40B |
| `0x04` PING | 设备→电脑 | 空 | 1B |

`0x04 PING` 是**纯判活**报文：设备在语音应用运行期间每 **2000ms** 发一发。
它由 `udp_transport.py` 在读线程里直接消费（刷新设备地址与时刻），**不会派发到
relay**，所以 relay 核心完全不知道它的存在。没有它，设备空闲（没在录音、没事件）
时 companion 无法把"设备还在"和"设备退出了语音应用"区分开。

### 3.1 为什么 AUDIO 仍保留 2B 分片帧头

BLE 时代帧头是 `[块序号][片序号|0x80 末片]`，用途是"片级丢失只损坏该块，后续块不失步"。
UDP 下每块恒为**单片 + 末片**，帧头退化为 `[seq][0x80]`，但**保留它让 companion 的
`reassemble_adpcm()` 一行都不用改** —— 它照旧按"新 seq + is_last"终结并解码。
`seq` 每块 `++ mod 256`，重复/迟到块由 companion 现有的 `last_seq` 逻辑丢弃。

### 3.2 为什么 EVENT 带 `\n`

与 BLE 一致，companion 的 `reassemble_event()` 按 `\n` 切行，零改动。

---

## 4. 发现与链路判定

```
companion ──beacon(1s)──► 设备
                             │ 记录 src_ip + 时刻
                             ▼
                     首次/换 IP/距上次 >3s → 上行 device.hello
                             │
设备 ──AUDIO/EVENT 单播──► companion（来源地址即设备地址）
```

| 状态 | 判定 |
| --- | --- |
| `link_up` | 已学到对端 IP **且** 距最近一次收到对端报文 < `PEER_TIMEOUT_MS`(3000) |
| `link_up == false` | UI 显示 OFFLINE 横幅、**PTT 禁止**（与 BLE「未订阅 EVENT」同语义） |
| `connected` | 同 `link_up`（BLE 时代 connected 与 subscribed 是两态，UDP 下无此区分） |

设备侧对外接口与原 `ble_audio_*` 一一对应，`mode.c` / `app_voice.cc` 只需换函数名。

### 4.1 怎么重连（设备全自动 / 电脑要手动重启）

**设备侧：全自动，没有任何按键或菜单。**

- 语音应用运行期间 socket 一直 `bind 33333` 听 beacon（`udp_audio.c` 的 `rx_task`）。
- 收到 beacon → 学到电脑 IP → 立即回一发 `device.hello`。日志：
  `companion beacon 来自 192.168.x.x → link up`。
- 电脑静默超过 `VOICE_UDP_PEER_TIMEOUT_MS`（3000 ms，= 3 个 beacon 周期）→ `link_up=false`
  → UI 弹「与电脑断开，正在重连」横幅、**PTT 禁止**。
- beacon 一恢复就自动续上：`peer_accept_beacon()` 发现"对端消失过又回来"
  （间隔 > `PEER_STALE_US`，实际等于 3 s）会**再发一次 hello**，让 companion 重新学到设备地址。
- 退出语音应用会清空对端记忆（`udp_audio_deinit()` 清 `s_peer_known`），下次进入重新发现。

**电脑侧：v1 不自动重连 —— 断开后必须重启 relay。**

- `udp_transport.py` 类注释原文即为 *「Wi-Fi UDP 传输层(单设备;v1 不自动重连, 对齐 BLE 语义)」*。
- 设备静默 `LINK_TIMEOUT`（6.0 s，= 3 个 PING 周期）→ 触发 `on_disconnect`
  → `relay.py` 的 `_handle_disconnect()` 置 `_stop` 并给两个队列塞 stop
  → `_drain()` 退出 → `run()` 的 `finally` 收束会话并 `t.disconnect()`
  → **`run()` 返回，进程结束**。
- 所以"重连"的动作 = **在电脑上重新跑一次** `python companion/relay.py`。

电脑侧通道由 `companion/config.local.json` 决定：

```json
{ "channel": "wifi", "udp_port": "" }
```

（`udp_port` 留空即用 33333。`relay.py` 的 `_build_transport()` 按此选 `UdpTransport`。）

**连不上的三个前提**（任一不满足就永远 OFFLINE）：

1. 设备与电脑**在同一子网**（接同一个路由器）；路由器不能开 **AP 隔离**。
2. 电脑上 companion 正在运行，且 `channel` 为 `wifi`。
3. Windows **防火墙放行入站 UDP 33333**（首次运行会弹窗，需点允许）。

**日志对照**：

| 现象 | 含义 |
| --- | --- |
| `UDP 通道就绪(port 33333),等待 companion beacon` | 设备已在听，但还没听到电脑 |
| `companion beacon 来自 192.168.x.x → link up` | 已重连成功 |
| 只有第一行、一直不上来 | 电脑没在广播 / 不同网段 / 防火墙拦了 / AP 隔离 |

> 实测踩过的坑：设备连 `ZTE-2302-5G` 拿到 `192.168.5.4`，而当时那台电脑唯一可路由地址是
> `192.168.1.3`（以太网，Wi-Fi 网卡全部断开）→ 不同网段，beacon 根本到不了设备。
> **电脑必须接到设备所在的同一个 Wi-Fi/路由器上**，不是"能上网"就行。

---

## 5. 会话时序

```
设备                                          电脑 companion
 │  按住 UP → 本地提示音 → 开始采集
 │ ── EVENT voice.start(audio=ima_adpcm) ──►   开始 ASR 流
 │ ── AUDIO ×N（每 100ms 一块） ──────────►   逐块解码 → 喂火山 ASR
 │                                             └─ 中间结果 → 悬浮窗 / 下行 transcript
 │  松开 UP → drain 环 → voice.end
 │ ── EVENT voice.end ────────────────────►   发送 ASR 收尾
 │ ── EVENT status(drop=n) ───────────────►   与本地实收帧数对账
 │ ◄─ EVENT agent.status(done) ────────────   设备回 DONE
```

下行（电脑→设备）只有 `type=0x02` EVENT：`transcript` / `agent.status` /
`agent.approval_request` / `time.set`，全部经 `app_protocol_parse()` 投事件，与 BLE 完全一致。
热键转发器另有 `pc.status`（每秒一次的就绪心跳）与 `pc.ptt`（见 5.1）。

### 5.1 电脑键盘按住热键开麦（`pc.ptt`，2026-11-21）

在电脑上按住输入法热键，设备即开麦，把设备当成电脑麦克风；设备上键原用法不变。

```
电脑 companion                                 设备
 按住热键满 0.2s（期间无其他键）
 ── {"type":"pc.ptt","active":true} ──►        READY 且 link_up 且 pc_ready → 开麦
                                               ◄── voice.start + AUDIO ×N
 按住期间每 0.5s 续期 active:true ─────►        刷新续期时刻
 松开 ── active:false ×2 ─────────────►         关麦（排空）→ voice.end
```

- `active` 必须是 JSON 布尔，缺失/其他类型丢弃。
- 设备 `APP_PC_PTT_TIMEOUT_MS`（1500 ms）收不到续期即自动关麦，防止关麦帧丢失或电脑程序卡死时麦克风常开。
- 设备上键会话期间忽略 `pc.ptt`；电脑会话期间上键松开不打断。`pc.status` 未就绪或链路断开照常取消会话。
- 达到 10 分钟上限后，须先收到 `active:false`（松开热键）才能再次开麦。
- companion 默认 `pc_trigger=hotkey`：用户直接按住输入法热键，输入法由真实按键调起、松开即结束，companion 不注入（仅 hold 模式）。
  可选 `pc_trigger=space`（全局钩子拦截长按空格，由 companion 注入热键），实测可靠性较差。设备侧协议两者相同。

---

## 6. 丢包与对账

- 每个音频数据报 = 一个 100ms 块；块自带 predictor/index 头 → **丢一块只损失该 100ms，后续不失步**。
- companion 的 `_finalize_adpcm_block()` 已实现 miss 计数与"补零保节拍"，本改造**不改它**。
- 设备端 `audio_streamer_take_drops()` 继续在 `voice.end` 后随 `status` 帧上报，双端对账口径不变。
- 与 BLE 相比新增：UDP 无重传，**丢包率会真实反映在转写里**（BLE 靠链路层重传掩盖）。局域网内实测应接近 0。

---

## 7. 移除清单 / 不动清单

**移除**

- `main/apps/voice/ble_audio.c` / `ble_audio.h`（747 行）+ `main/CMakeLists.txt` 中的条目与 `bt` 依赖
- `app_voice.cc` 的 `suspend_wifi_driver()` / `resume_wifi_driver()`（进语音不再停 Wi-Fi）
- `mode.c` 里的 USB 半成品分支（`mode_channel_up(USB)` 恒 false，从来不可用）

**不动**

- `app_protocol.c` / `audio_streamer.c` / `adpcm.c` / `app_state.c` / `app_ui.c` / `app_events.c`
  —— 这些全是通道无关的，一行不改。

---

## 8. 未决 / 风险（需知悉）

1. **局域网前提**：设备与电脑必须在同一子网。若路由器开了 **AP 隔离（client isolation）**，
   广播到不了设备 → 永远 OFFLINE。家用路由器默认关闭该功能。
2. **无加密、无鉴权**（⚠️ 本次唯一的**安全回退**）：BLE 时代靠配对加密防窃听与防注入；
   UDP 明文且无身份校验 —— 同网段的任何主机都能伪造 beacon 让设备连过去，或伪造下行 JSON。
   当前接受（个人设备 + 家用局域网）。若要收紧：beacon 带共享密钥，UDP 载荷加 HMAC。
3. **首次运行需放行防火墙**：Windows 会拦截入站 UDP 33333，需在弹窗中允许（或手动加规则）。
4. **单设备假设**：端口 33333 固定，同网关两套设备会冲突。本版按单设备设计。
5. **待真机确认**：ESP32-C3 在 Wi-Fi 常驻 + LVGL + 音频采集并存时的堆余量
   （原 BLE 方案靠 `esp_wifi_deinit()` 腾内存；现在 Wi-Fi 常驻，必须实测）。

---

## 9. 实测结果（2026-09-12）

| 项 | 改造前 | 改造后 |
| --- | --- | --- |
| 固件体积 | 3,108,096 B | **2,870,416 B（−232 KB）** |
| app 分区剩余 | 26% | **32%** |
| 传输层代码 | `ble_audio.c` 747 行 + `mode.c` 55 行 | `udp_audio.c` 348 行 + `mode.c` 56 行 |
| ELF 里的 NimBLE 符号 | 存在 | **零**（`nm` 查不到 `ble_audio*` / `nimble_port*` / `ble_gap_*` / `ble_gatts*`） |
| 进语音时的 Wi-Fi | `StopStation()` + `esp_wifi_deinit()` | **完全不动** |

编译：`BUILD_EXIT=0`（WSL + ESP-IDF 5.5.3）。

> 体积下降主要来自 `bt` 组件不再被链接 —— 光删源码不会掉 232KB，是整块 NimBLE 栈
> 退出链接的结果。这也顺带说明：只要没人引用，`CONFIG_BT_ENABLED=y` 不会把代码带进固件。
>
> 更正一个此前的乐观估算：曾写"voice 4.4k 行 → ~1.5k 行"。实测 voice 目录（含头文件、
> UI、状态机）仍有 4,865 行 —— 真正被砍掉的是传输层约 400 行与整个 NimBLE 依赖；
> UI / 状态机 / 协议层本来就该留着，不该算进"可删"。
>
> 后续清理（2026-09-12 复核）：§7 计划删除的 `ble_audio.c`(747) / `ble_audio.h`(128)
> 当时仍留在工作区（未进 CMakeLists、无人 include），已确认删除。voice 目录随之
> 4,865 → **3,991 行**（`_diag/elf_verify2.log`），固件体积与符号表均无变化
> （`_diag/build_udp2.log`：仍是 `0x2bcc90` / 32% free）。
