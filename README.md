# CANLink Wi-Fi 中转盒（ESP32-S3 + CANable）

把 CANable 插到 ESP32-S3 的 USB Host 口，盒子自己开 Wi-Fi 热点，手机浏览器不插线就能通过 WebSocket 收发 slcan 帧。

本仓库是该固件的独立开源发布仓（源自 CANLink 项目），克隆下来即可独立编译；已在 ESP-IDF v5.3.6 实测编译通过（2026-11-18）。

**第一轮范围**：只做“录制数据透传通道”+ 盒子自带的调试页。不依赖 CANLink 网页本体，不做刷写、不做 DBC/诊断、不做 OTA，也不碰车上任何控制器。协议见 [`docs/protocol.md`](docs/protocol.md)。

```
手机浏览器  ──Wi-Fi(AP)──>  ESP32-S3  ──USB Host(CDC-ACM)──>  CANable Pro(slcan 固件)
             ws://192.168.4.1/ws
```

## 1. 目录

```
（仓库根目录）
├── CMakeLists.txt
├── partitions.csv          16 MB flash 分区表（app 3 MB，留 OTA 余地）
├── sdkconfig.defaults      N16R8 / USB Host / WebSocket 默认配置
├── main/
│   ├── Kconfig.projbuild   SSID、密码、USB VID:PID、缓冲上限等
│   ├── idf_component.yml   依赖 espressif/usb_host_cdc_acm ^2.2.0
│   ├── main.c              启动顺序、状态日志、状态灯
│   ├── usb_cdc.c/.h        USB Host 枚举 CDC-ACM，断线重连
│   ├── slcan_relay.c/.h    slcan 行组装、USB↔WS 双向转发、计数
│   ├── ws_server.c/.h      HTTP + WebSocket + /api/status + 调试页
│   └── wifi_ap.c/.h        softAP、预留 STA、断线退避
└── docs/protocol.md        网页端对接协议（一页）
```

## 2. 为什么选 ESP-IDF（而不是 Arduino-ESP32）

| | ESP-IDF（选用） | Arduino-ESP32 |
|---|---|---|
| USB Host | 官方 `usb_host` + `usb_host_cdc_acm` 类驱动，直接枚举 CDC-ACM | 只有 USB **Device** 栈，没有 Host；要自己移植 TinyUSB host 并手写 CDC-ACM host 类 |
| WebSocket 服务 | `esp_http_server` 自带 WebSocket（`CONFIG_HTTPD_WS_SUPPORT`） | 需要第三方库（如 ESPAsyncWebServer），与 USB/FreeRTOS 版本耦合 |
| 并发 | FreeRTOS 任务/队列/流缓冲直接可用，USB 回调只做搬运 | 同一套 FreeRTOS，但库之间抢栈、抢核的问题更多 |
| 可维护性 | 配置项进 Kconfig，日志统一 | 依赖库版本漂移大 |

结论：**ESP-IDF v5.3.x / v5.4.x**。Arduino 的唯一优势是上手快，但 USB Host 这块等于从零写，风险更大。

## 3. 接线

目标板：**ESP32-S3-N16R8**（16 MB flash + 8 MB PSRAM，双 Type-C）。两个口分工不同：

| 板上的口 | 用途 |
|---|---|
| **原生 USB 口**（接 GPIO19/20，通常丝印 `USB`） | 接 CANable Pro，做 USB Host |
| **UART 口**（板载 USB 转串口，丝印 `UART` / `COM`） | 接电脑烧录、看 115200 日志、供电 |

```
ESP32-S3 原生 USB-C  ──短 USB 线──  CANable Pro 的 USB-C
ESP32-S3 UART USB-C  ──USB 线─────  电脑（烧录 / monitor / 5V 供电）
```

**供电（最容易踩的坑）**：多数 S3 开发板原生 USB 口的 VBUS 是“输入检测”或经二极管单向供电，**不向外输出 5 V**，CANable 插上可能完全不上电。三种可行做法，任选其一：

1. 中间加一个**带电源的 USB 2.0 Hub**（盒子→Hub→CANable），Hub 给 CANable 供电；最稳，台架推荐。
2. 从板子的 `5V` 引脚（或 UART 口的 5 V）引 5 V 到 CANable 的 5 V 排针，GND 共地；注意不要再从 CANable 的 USB 口反向灌 5 V。
3. 车里：12 V → 5 V 降压模块（**≥1 A，建议 1.5 A**，Wi-Fi 发射有峰值）给板子和 CANable 供电，共地、加保险丝。**不要**同时插着电脑 USB 和车上 5 V，除非用隔离电源/隔离 USB。

其余：

- USB 线尽量短（< 30 cm），远离天线；USB Full Speed 对信号质量敏感。
- CANable 的 CAN_H/CAN_L 接总线，终端电阻按现场情况（Pro 板上有 120 Ω 跳线/开关）。
- 车上取电务必保险丝；盒子放金属盒里时注意 Wi-Fi 天线别被完全包住。
- 板载 WS2812 不做状态灯；想要指示灯就外接一个普通 LED 到空闲 GPIO，并在 menuconfig 里设 `RELAY_STATUS_LED_GPIO`。

## 4. 编译与烧录

### 4.1 工具链版本

| 项 | 版本 |
|---|---|
| ESP-IDF | **v5.3.2**（首选）或 v5.4.x；不支持 v4.x |
| 交叉编译器 | IDF v5.3.2 自带 `xtensa-esp-elf-gcc 13.2.0`；v5.4.x 自带 14.2.0 |
| CMake / Ninja | ≥ 3.16 / ≥ 1.10（IDF 安装脚本自带） |
| Python | ≥ 3.8 |
| 组件 | `espressif/usb_host_cdc_acm` **^2.2.0**（首次编译自动从组件仓库下载，需联网） |

> **本次提交没有在真实硬件上编译或运行过**（写代码的服务器上没有 ESP-IDF 工具链）。上面是目标版本；第一次 `idf.py build` 若有 API 差异，大概率集中在 `usb_host_cdc_acm` 的主版本（2.x 的 `cdc_acm_host_open()` 带 `interface_idx` 参数，1.x 没有）。

### 4.2 编译

```bash
# 1) 装 ESP-IDF（只装一次）
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32s3 && . ./export.sh

# 2) 编译本目录
cd CANLink/firmware-wifi
idf.py set-target esp32s3        # 会按 sdkconfig.defaults 生成 sdkconfig
idf.py build
```

- 板子不是 N16R8（比如 N8R2）时，改 `sdkconfig.defaults` 里的 flash/PSRAM 两项，或 `idf.py menuconfig → Serial flasher config / ESP PSRAM` 后重新 build。
- 网络受限、组件仓库访问不了时：把 `esp-usb` 仓库的 `host/class/cdc_acm_host` 拷到 `components/usb_host_cdc_acm/`，删掉 `main/idf_component.yml` 里的那一行依赖即可离线编译。
- 产物：`build/canlink_wifi_relay.bin`（预计 0.9～1.1 MB，app 分区 3 MB，未实测）。

### 4.3 烧录

用 **UART 口**接电脑（原生口留给 CANable；USB-Serial-JTAG 和 USB Host 共用 PHY，不能同时干这两件事）：

```bash
idf.py -p /dev/ttyUSB0 flash monitor     # Linux，端口按实际改（dmesg | tail）
idf.py -p COM5 flash monitor             # Windows
```

- 烧录波特率默认 460800；板子进不去下载模式时按住 BOOT 再点 RST。
- 看日志：115200 8N1，UART 口。
- 只想烧、不看日志：`idf.py -p /dev/ttyUSB0 flash`。

## 5. 上电后怎么验证（台架）

1. 手机搜 Wi-Fi：`CANLink-Relay`，密码 `canlink1234`（menuconfig 可改）。
2. 浏览器打开 `http://192.168.4.1/`，应看到状态行和黑色日志框；`WebSocket` 自动连上，日志出现 `-- ws open --`。
3. 点 `xI`：日志出现 `xIvdlink,1.0,80000000`（不同固件版本号不同），说明 USB 枚举 + 双向转发通了。
4. 点 `O`：日志出现一个空的 `\r`（成功）或 `<BEL>`（失败，通常是没接总线/没设波特率）。先点 `S6`（500 kbit/s）再点 `O`。
5. 总线上有报文时，`frames` 计数持续增长，日志里能看到 `t123...` 行；`drops.*` 应保持 0。
6. 拔掉 CANable：日志出现 `#usb down`，状态行 `USB down`；插回后 2 s 内 `#usb up` 并自动重新枚举。

串口日志每 5 s 一行，形如：

```
I (12345) main: usb=up ap=up sta=down ip=192.168.4.1 ws=1 fps=42 frames=2100 tx=3 drops(usb=0 line=0 tx=0 ws=0) heap=182344
```

`fps` 是最近 5 s 的平均帧率；`drops.usb` 涨说明 USB→中继缓冲溢出（总线太满），`drops.tx` 涨说明网页发得太快，`drops.ws` 涨说明手机侧收不动/断线。

## 6. 配置项（`idf.py menuconfig → CANLink Wi-Fi relay`）

| 配置 | 默认 | 说明 |
|---|---|---|
| `RELAY_AP_SSID` | `CANLink-Relay` | 热点名 |
| `RELAY_AP_PASSWORD` | `canlink1234` | 8～63 字符；短于 8 会退化成开放网络并打警告 |
| `RELAY_AP_CHANNEL` | 1 | 1～13 |
| `RELAY_USB_VID/PID` | `0x16D0/0x117E` | CANable 2.0 / vdlink |
| `RELAY_USB_TRY_KNOWN_DEVICES` | 开 | 额外尝试 `0x1D50:0x606F`（CANable 1.0 / MKS V2.0）、`0x0483:0x5740`（STM32 VCP） |
| `RELAY_MAX_LINE_LEN` | 256 | 超长行丢弃并计数 |
| `RELAY_WS_MAX_CLIENTS` | 4 | 同时连的浏览器数 |
| `RELAY_WS_PING_INTERVAL_S` | 15 | 保活，0 = 关 |
| `RELAY_STATUS_LED_GPIO` | -1 | 普通 LED 的 GPIO，-1 关闭 |
| `RELAY_ENABLE_STA_PROVISIONING` | 关 | 打开后才有 `POST /api/wifi`，见协议第 6 节 |

## 7. 已知风险与未验证项

1. **未编译、未上板**。本文的引脚、分区、组件版本都按 ESP32-S3-N16R8 + IDF 5.3 写；第一次编译可能要跟 `usb_host_cdc_acm` 的实际小版本对一下 API。
2. **USB Host 供电**（第 3 节）：原生口不出 5 V 时 CANable 不亮，这是最常见的“代码没问题但枚举不到”。先量 VBUS。
3. **CDC-ACM 兼容性**：代码按 115200 8N1 设置 line coding，并显式拉 DTR/RTS（有的 slcan 固件不拉就不发数据）；`interface_idx = 0`，复合设备（带多个 CDC 接口）没验证。CANable 若刷的是 candleLight/gs_usb 而不是 slcan，它根本不是 CDC 设备，打不开——需要先刷回 slcan/vdlink。
4. **吞吐**：ESP32-S3 USB Host 只有 Full Speed 12 Mbit/s；slcan 文本对 8 字节帧约 5 倍膨胀，64 字节 CAN FD 帧约 2～3 倍。500 kbit/s 经典 CAN 满负载大约 2～3 万帧/秒的文本量，预计吃紧；**1 Mbit/s CAN FD 不建议第一轮就压满**。丢没丢看 `drops.usb` / `drops.tx`。
5. **HTTPS 混合内容**：canlink 网页是 HTTPS 部署，浏览器会拦 `ws://`。本轮协议先用板载调试页/本地 http 页验证，正式接入前要给盒子加 TLS（自签证书需手机先信任）或改网页打开方式。
6. **WebSocket 并发**：`httpd_ws_send_frame_async()` 从非 HTTP 任务发送，代码里加了互斥和 fd 状态检查，但没做压力测试（多手机 + 满总线）。
7. **AP+STA 并存**没实测；STA 凭据只在启动时读，改完要重启。
8. 没有鉴权、没有 OTA、没有 mDNS（`canlink.local` 之类）；AP 密码是编译期常量，换密码要重新烧录。

## 8. 备选方案（如果这条路走不通）

| 方案 | 代价 | 何时用 |
|---|---|---|
| 直接用 `usb_host` + 自写 CDC-ACM 类驱动 | 中（约 500 行，要处理控制传输/环形缓冲） | `usb_host_cdc_acm` 组件与 IDF 版本打架、或需要多接口复合设备时 |
| CANable 刷 candleLight/gs_usb，盒子实现 gs_usb host 类 | 大（USB 批量传输 + 位定时命令，但带宽比 slcan 文本好） | 要跑满 1 Mbit/s CAN FD 时，第二轮评估 |
| 换带 USB Host 的 Linux 小板（如树莓派 Zero 2 W） | 中，但体积/功耗/开机时间都上去 | 只想快出结果、不在乎盒子形态 |
| 保持现在的有线 WebUSB/WebSerial | 0 | 盒子方案失败时的回退，功能不受影响 |

## 9. 许可

本仓库代码采用 GPLv3（见根目录 `LICENSE`）。代码为本次新写，没有复制 vdlink、CANable 或 candleLight 的源码；`usb_host_cdc_acm` 作为组件依赖按 Apache-2.0 使用。
