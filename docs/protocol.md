# CANLink Wi-Fi 中继协议（v0，第一轮）

目标：网页端**不改解析逻辑**，只多一个“传输层”。盒子把 CANable 的 slcan 字节流原样搬到 WebSocket 上，网页把 WebSocket 当成一根串口用。

```
CANable Pro ──USB CDC(slcan 文本)──> ESP32-S3 ──Wi-Fi AP──> 手机浏览器
              USB Host                  WebSocket          canlink 网页（未来新增 WsTransport）
```

## 1. 端点

| 端点 | 说明 |
|---|---|
| `ws://192.168.4.1/ws` | 数据通道，端口 80，**明文 ws**（本轮不做 TLS） |
| `GET http://192.168.4.1/api/status` | 状态 JSON，带 `Access-Control-Allow-Origin: *`，可跨域 fetch |
| `GET http://192.168.4.1/` | 板子自带的调试页：状态 + 原始 slcan 控制台，用于台架自检 |
| `POST http://192.168.4.1/api/wifi` | 预留的 STA 配置口，默认编译关闭（见第 6 节） |

AP 默认 SSID `CANLink-Relay`、密码 `canlink1234`、网关 `192.168.4.1`，都可在 `idf.py menuconfig → CANLink Wi-Fi relay` 里改。

## 2. 消息格式

WebSocket **只传文本**（TEXT 帧），UTF-8/ASCII，一条消息对应一行 slcan，不做 JSON 包装。

### 2.1 下行（盒子 → 网页）

一条消息 = **一条完整的 slcan 行，含行尾**：

| 消息内容 | 含义 |
|---|---|
| `t1232AABB\r` | 标准帧，ID `0x123`，2 字节数据 |
| `T00000123DLC…\r` | 扩展帧（格式同 slcan 原样） |
| `d1238…\r` / `b1238…\r` | CAN FD / 带 BRS 的 CAN FD |
| `\a` | 上一条命令被适配器拒绝（slcan 的 BEL） |
| `#usb down\r`、`#usb up\r` | 盒子提示行，`#` 开头，不是 CAN 数据 |

因为行尾一起发，网页可以直接喂给现有的 `Slcan.LineReader`，`\r` / `\a` 的语义和串口完全一致。

盒子**不会**在行中间切分，也不会把两行拼进一条消息；缓冲区上限 256 字节（64 字节 CAN FD 数据约 150 字符，够用），超长行丢弃并在状态里计数。

### 2.2 上行（网页 → 盒子）

一条消息 = **一条 slcan 命令，不带 `\r`**；盒子补 `\r` 后写给 CANable：

```
O            →  O\r        打开通道
C            →  C\r        关闭通道
S6           →  S6\r       500 kbit/s
Z2           →  Z2\r       开微秒时间戳
xI           →  xI\r       固件信息
t1232AABB    →  t1232AABB\r  发一帧
```

- 带不带 `\r` 都行，盒子会先去掉行尾再补一个。
- 限速：TX 队列 16 条，每条之间 2 ms；队列满时**丢弃**并计入 `drops.tx`。
- 建议网页沿用串口那边的习惯：**发一条，等它的 `\r` 或 `\a` 回来，再发下一条**（尤其 `O` / `C` / `S6` / `xN` 这类改状态命令）。连续猛发会被丢。
- 二进制消息（BINARY 帧）保留：盒子原样转给 CANable，不补 `\r`。canlink 本轮不用。

## 3. 复用现有 slcan 解析路径

网页侧只需要新增一个与 `docs/transport.js` 同接口的 `WsTransport`（`open / write / close / onData / onClose`），其余不动：

```js
class WsTransport {
  constructor() { this.kind = 'wifi'; this.onData = null; this.onClose = null; }
  async open(url) {
    this.ws = new WebSocket(url || 'ws://192.168.4.1/ws');
    await new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; });
    this.ws.onmessage = ev => {
      if (typeof ev.data !== 'string') return;
      if (ev.data[0] === '#') return;   // 盒子提示行，忽略
      this.onData(ev.data);             // 含行尾，交给现有 LineReader
    };
    this.ws.onclose = () => this.onClose && this.onClose();
  }
  async write(text) { this.ws.send(text.replace(/[\r\n]+$/, '')); }
  async close() { this.ws.onclose = null; this.ws.close(); }
}
```

现有的这段链路一行都不用改：

```js
const reader = new Slcan.LineReader((line, failed) => {
  const msg = Slcan.parseLine(line, tsMode);   // tsMode 由网页发的 Z2/Z1/Z0 决定
  // ...原来的录帧 / 在线报文逻辑
});
// 串口: transport.onData(chunk) -> reader.push(chunk)
// Wi-Fi: 每条 WS 消息就是一行（含行尾），同样 reader.push(msg)
```

要点：

1. `LineReader` 已经按 `[\r\x07]` 切行，所以 WS 一条消息里带不带行尾都能工作；带上行尾是为了和串口字节流保持同构。
2. **`tsMode` 必须和网页发给盒子的 `Z0/Z1/Z2` 一致**。盒子不解析、不改写时间戳，只透传。
3. 上行直接用 `Slcan.frameCmd()` / `Slcan.timingCmd()` 生成字符串，`write()` 掉行尾即可；不需要新的组帧代码。
4. 盒子的响应顺序就是 CANable 的响应顺序，没有额外排队重排；`>` 回显（`xT1`）也原样转发。

## 4. 状态与错误

`GET /api/status` 返回：

```json
{"usb":true,"frames_rx":1024,"frames_tx":12,"lines_rx":1030,
 "drops":{"usb":0,"ws":0,"tx":0,"line":0},
 "ws_clients":1,"ap":{"up":true,"ssid":"CANLink-Relay","ip":"192.168.4.1"},
 "sta":{"up":false},"heap":180000,"uptime_s":3600}
```

| 字段 | 含义 |
|---|---|
| `usb` | CANable 是否已打开（枚举 + CDC 配置完成） |
| `frames_rx` | 收到的 slcan 帧行数（`t/T/r/R/d/D/b/B` 开头） |
| `frames_tx` / `lines_tx` | 写给 CANable 的命令/帧行数 |
| `drops.usb` | USB 回调 → 中继缓冲区溢出丢的字节数 |
| `drops.line` | 超过 256 字符被丢的行数 |
| `drops.tx` | 上行队列满 / 超长被丢的命令数 |
| `drops.ws` | WebSocket 发送失败或超长消息数 |

`#` 开头的提示行目前有三种：`#usb up`、`#usb down`，以及后续可能加的 `#wifi ...`。网页应当忽略未知的 `#` 行。USB 掉线时 WebSocket **不会**断，靠 `#usb down` 通知；重插后收到 `#usb up`，网页应重新发 `xI` / `Z2`，并按需重开通道。

## 5. 断线重连约定

| 断的位置 | 盒子的行为 | 网页该做的 |
|---|---|---|
| WebSocket | 关闭时从客户端表里移除；15 s 发一次 PING | 1 s 后重连，失败则 2/4/5 s 退避；重连后重发 `Z2`、`xI`（**不要**自动重发 `O`，由用户点“连接”再开） |
| USB / CANable | 每 2 s 重新枚举，最多试 3 组 VID:PID；掉线时清空待发队列，发 `#usb down` | 收到 `#usb up` 后重发 `xI`、`Z2`、`O`（如果之前在录） |
| Wi-Fi（手机侧） | AP 常开；AP 意外停止时看门狗 1 s 后重启 Wi-Fi | 手机自动回连，网页按 WebSocket 断开处理 |
| Wi-Fi（盒子 STA，预留） | 断线后 2 s 起指数退避，最长 60 s 重连 | 本轮不涉及 |

## 6. 预留：STA 模式配置口

`POST /api/wifi`，body `{"ssid":"...","pass":"..."}`，把凭据写进 NVS，**重启后生效**（本轮只预留，不做网页配网界面）：

- 默认 `CONFIG_RELAY_ENABLE_STA_PROVISIONING=n`，不注册该路由。
- 打开后，AP 和 STA 同时工作（APSTA）；盒子仍从 `192.168.4.1` 提供服务，方便现场排查。
- 该接口没有额外鉴权，连上 AP 的人都能改；只在实验台上短时打开。

## 7. 已知限制（第一轮）

1. **明文 `ws://` + HTTPS 网页会被浏览器拦**。canlink 网页目前是 HTTPS 部署，从 HTTPS 页面连 `ws://192.168.4.1` 属于混合内容，Chrome 会直接拒绝。本轮先用板子自带的调试页或本地 http 页面验证链路；正式接入前需要给盒子加 TLS（`wss://`，自签证书要先在手机上手动信任）或让网页以本地 http 方式打开。这是下一轮的首要问题。
2. **没有鉴权**。AP 用 WPA2 密码保护，应用层不再校验；同一热点下的人都能发 slcan 命令。只在车内/实验台短时使用。
3. **吞吐**：ESP32-S3 的 USB Host 是 Full Speed（12 Mbit/s），slcan 文本比二进制膨胀约 5～10 倍；1 Mbit/s CAN FD 满负载时可能丢帧。先按 500 kbit/s 仲裁段验证，`drops.*` 增长就说明链路吃不下。
4. 盒子不解析 CAN 帧、不缓存历史、不做过滤；断线期间的数据没有补发。
