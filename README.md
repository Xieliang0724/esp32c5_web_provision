# ESP32-C5 Web 配网固件

ESP32-C5 通过 **SoftAP + Web 页面**完成 Wi-Fi 配网的示例固件。

## 功能特性

- 🌐 **Web 配网**：设备开机进入 SoftAP 热点，手机/电脑连接后访问 `http://192.168.4.1` 打开配网页面
- 🔎 **mDNS**：联网后局域网内可直接访问 `http://esp32c5.local`（无需记 IP）
- 📶 **SSID 下拉选择**：自动双频（2.4G + 5G）扫描，从列表选择 Wi-Fi，也支持手动输入（含隐藏网络）
- 🌐 **IP 方式可选**：
  - **DHCP 自动获取**（默认）
  - **静态 IP**：可自定义 IP / 子网掩码 / 网关 / DNS（留空则掩码默认 `255.255.255.0`、DNS 默认用网关）
- 📴 **SoftAP 可关**：配网时勾选"连接成功后关闭热点"，STA 连上后 AP 自动关闭（省电、更安全）；不勾选则 AP 保持开启，随时可重新配网
- 🔁 **自动回退**：STA 连接失败超过 `CONFIG_PROV_CONNECT_RETRY_MAX` 次（默认 5 次）后自动重新打开 SoftAP 进入配网模式
- 💾 **配置持久化**：配置保存在 NVS，断电重启自动连接
- 🔘 **复位按键**：长按 GPIO9（BOOT 键）3 秒清除配置并重启进入配网模式

### 🛡️ 失联兜底（AP 与 STA 不允许同时死掉）

C5 为**单射频**芯片，STA 连接/扫描时 AP 信号会变弱；且部分板子（如本项目的）没有复位按钮，因此固件保证**任何时刻 AP 和 STA 至少有一个可用**：

| 状态 | AP | STA | 可达方式 |
|---|---|---|---|
| 配网模式 | 🟢 开 | 空闲 | 走热点 `192.168.4.1` |
| 连接中/重试中 | 🟢 开 | 连接中 | 走热点 |
| 已联网 + 关闭热点 | 🔴 关 | 🟢 在线 | 走路由器 |
| 已联网 + 保持热点 | 🟢 开 | 🟢 在线 | 双通道 |

**兜底规则**：STA 断开后**不立即**开 AP（避免单射频频繁跳变），而是**连续断开超过 N 秒**（默认 15 秒）才自动开启热点。N 可在配网表单"断网后开启热点兜底延迟（秒）"设置（1~3600，随配置持久化）。重试耗尽后自动回退配网模式（AP 常开）。

> ⚠️ 已知限制：表单提示中的"0 = 立即开启"当前**未实现**——0 在前端/NVS 加载/运行时均被按默认 15 秒处理（详见下文"商用部署须知"）。

> ⚠️ 唯一残留窗口：路由器"静默死亡"时（信号还在但路由中断），驱动需等 beacon 超时（约 10~60 秒）才触发断开，之后 AP 兜底才启动。属 WiFi 协议固有延迟。

## 环境要求

- ESP32-C5 开发板（如 ESP32-C5-DevKitC-1，N4=4MB / N8R8=8MB 闪存）
- **ESP-IDF v5.4+**（本工程基于 v6.0.1 编译验证；v5.4 为 C5 技术预览支持）
- VS Code + [ESP-IDF 扩展](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)（推荐）

## 目录结构

```
esp32c5_web_provision/
├── CMakeLists.txt
├── CHANGELOG.md             # 版本记录（修复/功能明细）
├── sdkconfig.defaults       # 目标/闪存大小等默认配置
├── .vscode/settings.json    # 项目级 IDF 路径（不影响全局设置）
├── tools/                   # 串口读取/监控脚本、stunnel 配置
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild    # 配网相关 menuconfig 选项
    ├── idf_component.yml    # 托管组件依赖（cjson/led_strip）
    ├── app_main.c           # 入口：初始化 + 启动流程
    ├── config_store.[ch]    # NVS 配置持久化（配网 + 网关）
    ├── wifi_mgr.[ch]        # Wi-Fi 状态机（AP/STA/扫描/静态IP/回退/兜底）
    ├── web_server.[ch]      # HTTP 配网服务器（REST API）
    ├── modbus_gw.[ch]       # Modbus RTU↔TCP 网关（可选 TLS）
    ├── rgb_led.[ch]         # WS2812 状态指示灯
    ├── certs/               # TLS 服务器证书/私钥（私钥不入库）
    └── www/index.html       # 内嵌配网网页（EMBED_FILES）
```

## 编译与烧录

### 方式一：VS Code（推荐）

1. VS Code 打开本工程文件夹（`File > Open Folder`）
2. 确认左下角显示目标芯片 **ESP32-C5**
3. 点击底部状态栏 **Build**（或 `Ctrl+E B`）编译
4. 插上开发板，点击 **Flash**（或 `Ctrl+E F`）烧录
5. 点击 **Monitor**（或 `Ctrl+E M`）查看串口日志

> 若未识别目标芯片，执行 `Ctrl+Shift+P` → `ESP-IDF: Set Espressif Device Target` → 选择 `esp32c5`。

### 方式二：命令行

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh        # 按你的 IDF 路径调整
cd esp32c5_web_provision
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor      # macOS 串口设备名
```

## 使用流程

**首次使用（配网）：**

1. 给设备上电，等待日志出现 `SoftAP ssid=ESP32C5-XXXXXXXX`（MAC 后缀，每台唯一）
2. 手机/电脑连接 Wi-Fi 热点 **`ESP32C5-XXXXXXXX`**（MAC 后缀，每台设备唯一）（默认开放网络；如需密码见下文配置）
3. 浏览器访问 **`http://192.168.4.1`**，打开配网页面
4. 点击 **刷新** 扫描 Wi-Fi → 下拉选择你的路由器 SSID（或手动输入）
5. 输入密码；按需选择 **静态 IP** 并填写 IP/网关等；按需勾选 **连接成功后关闭热点**
6. 点击 **保存并连接**，等待状态变为"已连接"

**再次使用：** 上电后自动按已保存配置连接。若勾选了关闭热点，如需重新配网：

- 长按开发板 **BOOT 键（GPIO9）3 秒** 清除配置重启，或
- 烧录前 `idf.py erase-flash` 清空

## 配置项（menuconfig / sdkconfig.defaults）

| 配置 | 默认值 | 说明 |
|---|---|---|
| `CONFIG_PROV_AP_SSID_PREFIX` | `ESP32C5` | 热点名前缀，实际为 `<前缀>-<芯片MAC后N字节>`，见 `CONFIG_PROV_AP_SSID_MAC_BYTES` |
| `CONFIG_PROV_AP_PASSWORD` | 空 | 热点密码，留空=开放网络；设置需 ≥8 位 |
| `CONFIG_PROV_AP_CHANNEL` | 1 | 热点 2.4G 信道 |
| `CONFIG_PROV_CONNECT_RETRY_MAX` | 5 | STA 失败重试次数，超过后回到配网模式 |
| `CONFIG_PROV_STA_TIMEOUT_MS` | 20000 | 单次连接超时（毫秒） |
| `CONFIG_PROV_RESET_GPIO` | 9 | 复位配网按键 GPIO（-1 禁用） |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | y | 闪存 8MB（实测 N8R4；N4 板改 4MB） |

## REST API

| 接口 | 方法 | 说明 |
|---|---|---|
| `/` | GET | 配网页面 |
| `/api/status` | GET | `{state: config/connecting/connected, ssid, ip, gateway, netmask, dns, rssi, ap_on, ap_ssid, ap_clients, wifi_mode, version}` |
| `/api/scan` | GET | 触发/查询双频扫描，返回网络列表（仅配网模式可用） |
| `/api/config` | POST | 提交 `{ssid, password, ip_mode, ip, netmask, gateway, dns, ap_off, ap_fallback_delay}` |
| `/api/reset` | POST | 清除已保存配置并回到配网模式 |
| `/api/disconnect` | POST | 断开 STA 进入配网模式（保留已保存配置，重启后按原配置联网） |
| `/api/ap` | POST | `{ap_on: true/false}` 运行时开关热点（仅已联网时允许关闭） |
| `/api/cert` | GET | 下载设备 TLS 证书（PEM） |
| `/api/gw` | GET/POST | 读写 Modbus 网关配置 `{enabled, port, baud, tx, rx, client_ip, tls_enabled, tls_port}` |

## Modbus RTU ↔ TCP 网关（高级设置）

配网页面底部 **⚙️ 高级设置** 中可配置并启用，ESP32 作为 **Modbus TCP 服务端**，把 TCP 请求转换成 RTU 通过 UART1 与 GD32 从站通信（**协议转换，非透传**）：

- **TCP → RTU**：去掉 MBAP 头 → 组 RTU 帧（从站地址 + 重新计算 CRC16）→ UART1 发出
- **RTU → TCP**：校验 CRC16 与从站地址 → 去掉 CRC → 组 MBAP 帧（回填事务 ID）→ 发回客户端

| 配置项 | 默认 | 说明 |
|---|---|---|
| 启用网关 | 关 | 勾选后生效 |
| 本地 TCP 端口 | 502 | Modbus 明文标准端口 |
| 允许客户端 IP | 空 | 留空 = 允许所有客户端；填写后仅该 IP 可连 |
| RTU 波特率 | 9600 | 与 GD32 从站一致 |
| UART1 TX/RX GPIO | 5 / 6 | 按接线修改，勿用 GPIO11/12（控制台）与 27（RGB） |
| 启用 TLS | 关 | 单向 TLS（服务端证书），与明文 502 并存 |
| TLS 端口 | 802 | Modbus Security 标准端口 |

**接线**：ESP32 `TX GPIO5` → GD32 `RX`，ESP32 `RX GPIO6` ← GD32 `TX`，共地 GND。TCP 请求中的单元号（MBAP uid）即 RTU 从站地址。

### Modbus TLS（v1.1.0+）

- 启用后设备同时监听 **明文 502** 和 **TLS 802** 两个端口
- **单向 TLS**：客户端验证设备证书（`CN=esp32c5.local`，自签名，10 年），设备不验证客户端
- 测试握手：`openssl s_client -connect <设备IP>:802 -servername esp32c5.local`
- 自签名证书客户端会提示"不受信任"，属正常

**替换为自己的证书**：把 `main/certs/server_cert.pem` 和 `server_key.pem` 换成你们自己的（重新编译烧录）。生成自签名证书命令：

```bash
cd main/certs
openssl req -x509 -newkey rsa:2048 -keyout server_key.pem -out server_cert.pem \
  -days 3650 -nodes -subj "/CN=esp32c5.local/O=YourOrg" \
  -addext "subjectAltName=DNS:esp32c5.local,IP:192.168.4.1"
```

## v1.2.2 可靠性修复详情（2026-08-22，代码审查）

商用前全量代码审查发现并修复 4 个高优先级缺陷，均与**长期运行稳定性**相关（内存/任务泄漏、状态机计数错误）。以下为逐项明细，供评审与回归测试对照。

### 修复 1：STA 连接超时重试计数双计（`wifi_mgr.c`）

| 项 | 内容 |
|---|---|
| 现象 | 路由器不可达/连接卡住时，配置的 5 次重试（`CONFIG_PROV_CONNECT_RETRY_MAX`）实际 2~3 次即耗尽，设备过早放弃并进入配网模式 |
| 根因 | 单次连接超时（20 s）路径先 `s_retry_count++`，随后调用 `esp_wifi_disconnect()` 清理挂起连接；该调用**必然**产生 `WIFI_EVENT_STA_DISCONNECTED` 事件，而事件处理器内会再次 `s_retry_count++`——一次超时被计两次。另：该路径中紧随 disconnect 的 `esp_wifi_connect()` 与驱动内尚未完成的断开操作竞争，基本必然返回 `ESP_ERR_WIFI_STATE` 而被忽略，属无效调用 |
| 修复 | 新增 `s_self_disconnect` 标志：超时路径置标志后仅调用 `esp_wifi_disconnect()`；事件处理器识别到标志则跳过计数（超时路径已计过），后续流程（切回 CONNECTING → 1 s 后重试 → 重连成功才重新武装超时定时器）不变。删除无效的直接 connect 调用。同时超时路径重新武装超时定时器作为**看门狗**：正常路径下事件处理器会解除它，仅在"断开事件异常丢失"的极端场景下保证状态机仍能周期推进直至回退配网模式（保留原实现的活性保证） |
| 影响面 | 仅超时路径；密码错误等快速失败路径（一次失败计一次）行为不变 |

### 修复 2：Web 延迟操作内存泄漏（`web_server.c`）

| 项 | 内容 |
|---|---|
| 现象 | 每次 `POST /api/config` 泄漏约 200 B 堆 + 1 个 esp_timer 对象；`/api/reset`、`/api/disconnect` 各泄漏 1 个 esp_timer 对象。反复配网（密码输错重试是常态）持续消耗堆，长期运行最终 OOM |
| 根因 | 配置副本 malloc 后交由一次性 esp_timer 回调使用，回调只读不释放；三个接口每次请求 `esp_timer_create` 一次性定时器，触发后不 `esp_timer_delete` |
| 修复 | 改为**静态资源终身复用**：进入配网模式共用 1 个静态定时器（断开/重置两接口），连接用另 1 个静态定时器 + 静态 `s_pending_cfg` 缓冲（新请求覆盖旧值，语义与原来一致：总是应用最新配置）。懒创建、零分配零泄漏。顺带消除了原先以 `(void(*)(void*))` 强转无参函数做定时器回调的 C 标准未定义行为 |
| 影响面 | `/api/config`、`/api/reset`、`/api/disconnect` 内部实现，外部行为不变 |

### 修复 3：Modbus RTU 响应帧泄漏（`modbus_gw.c`）

| 项 | 内容 |
|---|---|
| 现象 | 网关长跑（尤其从站响应慢、频繁超时）堆缓慢下降，最终 OOM |
| 根因 | 两处：① UART 接收任务将完整 RTU 帧入队时用非阻塞 `xQueueSend`，队列（深度 4）满时 malloc 的帧缓冲被丢弃且不 `free`；② 每次事务开头 `xQueueReset` 清队列，滞留帧的指针被直接丢弃——从站恰好在 1 s 超时后才应答的帧会滞留队列，被下一次事务的 Reset 丢掉（每帧最大 262 B，无界累积） |
| 修复 | 入队失败立即 `free`；事务开头改为循环出队逐个 `free` |
| 影响面 | 仅内存管理，协议行为不变 |

### 修复 4：网关停止任务泄漏 + fd 复用误关新连接（`modbus_gw.c`）

| 项 | 内容 |
|---|---|
| 现象 | 每次网页修改网关配置（`POST /api/gw` 触发重配置）最多泄漏 2 个监听 + 4 个客户任务；更严重的是滞留客户任务退出时按 **fd 数值**匹配槽位清理，而 lwIP 会复用 fd 号——重启后的**新连接可能被旧任务的 `close()` 误杀** |
| 根因 | 所有 socket 阻塞式且无超时；lwIP 下从另一任务 `close()` 一个 fd **无法唤醒**阻塞在该 fd 上 `accept()/recv()` 的任务，`gw_stop` 固定等待 1.5 s 等不到它们退出 |
| 修复 | `gw_stop` 对监听/客户 fd 先 `shutdown(SHUT_RDWR)` 再 `close()`（shutdown 可靠唤醒阻塞调用），清客户槽位置于槽位互斥锁内；客户任务创建时携带自己的槽位下标，退出时持锁校验"槽位仍归属本 fd"才关闭，杜绝 fd 复用误伤 |
| 附带修复 | TLS 握手失败/证书未就绪两条早退路径原先**不归还客户端槽位**（连续失败 4 次后网关拒绝所有新连接），现统一走 `client_done` 清理出口 |
| 影响面 | 网关启停/重配置路径；数据面（MBAP↔RTU 转换）不变 |

## 商用部署须知（已知限制与风险）

> 本节如实记录当前版本**已确认但尚未修复**的问题。商用决策前请逐项评估；后续版本可按优先级安排。

### 🔒 安全（商用前重点评估）

- **所有 HTTP API 无任何鉴权**，且设备入网后 Web 服务常驻监听（路由器分配的 IP）：同网段任意设备可读取状态、**修改 Wi-Fi 配置、恢复出厂、修改/停用 Modbus 网关**。Modbus 明文 502 端口同理。
  - 建议：部署于可信网段/独立 VLAN 或在交换机层隔离；客户端 IP 白名单仅是过滤不是防护
  - 后续可加可配置访问令牌（token）
- 配网凭据经 HTTP 明文传输（SoftAP 场景可接受；经路由器 LAN 访问时注意嗅探风险，TLS 仅覆盖 Modbus 端口，配网页面始终是 HTTP）

### ⚠️ 已知中优先级问题（特定条件触发）

1. **AP 兜底开启时序**（`wifi_mgr_ap_enable`）：先切 APSTA 模式再写 AP 配置，AP 会以先前被清空的配置（空 SSID）先行启动；若 `esp_wifi_set_config` 对已启动 AP 不生效（IDF 版本相关），**兜底热点可能不可见**——恰好影响"断网自救"这一兜底场景本身。属待上板验证项（见下方清单第 5 条）
2. **兜底延迟 0 的语义矛盾**：前端提示"0 = 立即开启"，实际 0 在前端/NVS 加载/运行时三层均被按默认 15 处理，该功能暂未实现（本 README 兜底规则一节已同步更正）
3. **超长凭据静默截断**：32 字符 SSID、64 字符 WPA 密码通过校验后被截断为 31/63 字符使用，导致连接失败但无明确提示。运维侧应避免此类超长输入
4. **重试定时器路径无兜底**：`on_conn_retry` 中 `esp_wifi_connect()` 若返回错误（罕见，驱动内部状态繁忙），当前无定时器接续，状态机可能停留在 CONNECTING；可通过网页重新保存配置恢复
5. **跨任务共享状态无互斥保护**（重试计数/配置等在 esp_timer、事件循环、httpd 多个上下文读写），极端时序下存在竞态。商用长跑前建议做压力测试

### ℹ️ 已知低优先级问题

- 前端"断开 Wi-Fi"提示硬编码了单台设备的热点名（非本机的设备会显示错误热点名）；服务端校验失败的具体原因（如 "ssid required"）网页上显示不出来；`recv_body` 对慢速/恶意客户端无限等待可占住整个 HTTP 服务；网关端口未禁止设为 80（会与配网服务冲突且保存时无告警）

### ✅ 待上板验证清单（烧录 v1.2.2 后逐项确认）

1. **重试计数**：关闭路由器（保持设备可见其信号）或断电，观察串口日志 `connect timeout (n/5)` 与 `retry connect` 交替——一次超时只应计一次
2. **Web 内存**：`for i in $(seq 1 100); do curl -s -X POST http://<设备IP>/api/config -d '{"ssid":"x"}' >/dev/null; done` 后对比 `/api/status` 前后串口堆日志（`minimum free heap` 不应持续下降）
3. **网关内存**：断开 RS485 从站使事务全部超时，压测数千次 Modbus 请求后堆稳定
4. **网关重配置**：网页反复（≥10 次）修改并保存网关设置，串口无异常，新 Modbus 客户端可正常连接收发
5. **AP 兜底（重点）**：设备联网且勾选"连接成功后关闭热点"后，关闭路由器电源等待 15 s+，确认热点以正确 SSID（`ESP32C5-XXXXXXXX`）出现且可访问 `192.168.4.1`——此为已知问题 1 的实测项

## 版本管理（git tag）

当前已发布至 **v1.2.2**（含可靠性修复：重试计数双计/内存泄漏/任务泄漏），固件内置版本号（网页状态面板 / `/api/status` / 串口日志 `App version:` 均可查看）。

**发布新版本**（改完代码后）：

```bash
git add -A
git commit -m "v1.2.2: 可靠性修复（重试计数双计/内存泄漏/任务泄漏）"
git tag -a v1.2.2 -m "v1.2.2"
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
```

**回退到旧版本**（出问题时一键回到上个可用版本）：

```bash
git checkout v1.2.1
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
git checkout main   # 回退完切回最新代码继续开发
```

> 提示：版本号由 `git describe` 自动生成（即 `PROJECT_VER`）；`build/`、`sdkconfig` 等已加入 `.gitignore` 不入库。若需**运行时自动回退**（OTA 升级失败自动回滚旧固件），可后续基于 IDF 的 OTA + `esp_ota_mark_app_valid_cancel_rollback` 机制扩展。

## 常见问题

- **连不上热点**：确认热点名是 `ESP32C5-XXXXXXXX`（MAC 后缀，日志中会打印）；若设了密码，确认密码 ≥8 位
- **扫描不到 5G 网络**：确认路由器 5G 开启且设备处于 5G 覆盖范围（5G 穿墙弱）
- **配网后想换网络**：长按 BOOT 3 秒，或连接热点（若未关闭）重新配置
- **IDF 版本兼容性**：本工程按 IDF v6.0.1 API 编写（`ESP_ERR_WIFI_CONN`、`esp_system.h`、cjson 托管组件等）；如用 v5.4/v5.5 请留意 API 差异
