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

## 环境要求

- ESP32-C5 开发板（如 ESP32-C5-DevKitC-1，N4=4MB / N8R8=8MB 闪存）
- **ESP-IDF v5.4+**（本工程基于 v6.0.1 编译验证；v5.4 为 C5 技术预览支持）
- VS Code + [ESP-IDF 扩展](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)（推荐）

## 目录结构

```
esp32c5_web_provision/
├── CMakeLists.txt
├── sdkconfig.defaults        # 目标/闪存大小等默认配置
├── .vscode/settings.json     # 项目级 IDF 路径（不影响全局设置）
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild     # 配网相关 menuconfig 选项
    ├── idf_component.yml     # 托管组件依赖（espressif/cjson）
    ├── app_main.c            # 入口：初始化 + 启动流程
    ├── config_store.[ch]     # NVS 配置持久化
    ├── wifi_mgr.[ch]         # Wi-Fi 状态机（AP/STA/扫描/静态IP/回退）
    ├── web_server.[ch]       # HTTP 配网服务器（REST API）
    └── www/index.html        # 内嵌配网网页（EMBED_FILES）
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

1. 给设备上电，等待日志出现 `SoftAP ssid=ESP32C5-XXXX`
2. 手机/电脑连接 Wi-Fi 热点 **`ESP32C5-XXXX`**（默认开放网络；如需密码见下文配置）
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
| `CONFIG_PROV_AP_SSID_PREFIX` | `ESP32C5` | 热点名前缀，实际为 `<前缀>-<MAC后4位>` |
| `CONFIG_PROV_AP_PASSWORD` | 空 | 热点密码，留空=开放网络；设置需 ≥8 位 |
| `CONFIG_PROV_AP_CHANNEL` | 1 | 热点 2.4G 信道 |
| `CONFIG_PROV_CONNECT_RETRY_MAX` | 5 | STA 失败重试次数，超过后回到配网模式 |
| `CONFIG_PROV_STA_TIMEOUT_MS` | 15000 | 单次连接超时（毫秒） |
| `CONFIG_PROV_RESET_GPIO` | 9 | 复位配网按键 GPIO（-1 禁用） |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | y | 闪存 8MB（实测 N8R4；N4 板改 4MB） |

## REST API

| 接口 | 方法 | 说明 |
|---|---|---|
| `/` | GET | 配网页面 |
| `/api/status` | GET | `{state: config/connecting/connected, ssid, ip, ap_on}` |
| `/api/scan` | GET | 触发/查询双频扫描，返回网络列表 |
| `/api/config` | POST | 提交 `{ssid, password, ip_mode, ip, netmask, gateway, dns, ap_off}` |
| `/api/reset` | POST | 清除配置并回到配网模式 |
| `/api/gw` | GET/POST | 读写 Modbus 网关配置 `{enabled, port, baud, tx, rx, client_ip}` |

## Modbus RTU ↔ TCP 网关（高级设置）

配网页面底部 **⚙️ 高级设置** 中可配置并启用，ESP32 作为 **Modbus TCP 服务端**，把 TCP 请求转换成 RTU 通过 UART1 与 GD32 从站通信（**协议转换，非透传**）：

- **TCP → RTU**：去掉 MBAP 头 → 组 RTU 帧（从站地址 + 重新计算 CRC16）→ UART1 发出
- **RTU → TCP**：校验 CRC16 与从站地址 → 去掉 CRC → 组 MBAP 帧（回填事务 ID）→ 发回客户端

| 配置项 | 默认 | 说明 |
|---|---|---|
| 启用网关 | 关 | 勾选后生效 |
| 本地 TCP 端口 | 502 | Modbus 标准端口 |
| 允许客户端 IP | 空 | 留空 = 允许所有客户端；填写后仅该 IP 可连 |
| RTU 波特率 | 9600 | 与 GD32 从站一致 |
| UART1 TX/RX GPIO | 5 / 6 | 按接线修改，勿用 GPIO11/12（控制台）与 27（RGB） |

**接线**：ESP32 `TX GPIO5` → GD32 `RX`，ESP32 `RX GPIO6` ← GD32 `TX`，共地 GND。TCP 请求中的单元号（MBAP uid）即 RTU 从站地址。

## 版本管理（git tag）

当前版本 **v1.0.0** 已打标签，固件内置版本号（网页状态面板 / `/api/status` / 串口日志 `App version:` 均可查看）。

**发布新版本**（改完代码后）：

```bash
git add -A
git commit -m "v1.1.0: 新功能描述"
git tag -a v1.1.0 -m "v1.1.0"
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
```

**回退到旧版本**（出问题时一键回到上个可用版本）：

```bash
git checkout v1.0.0
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
git checkout master   # 回退完切回最新代码继续开发
```

> 提示：版本号由 `git describe` 自动生成（即 `PROJECT_VER`）；`build/`、`sdkconfig` 等已加入 `.gitignore` 不入库。若需**运行时自动回退**（OTA 升级失败自动回滚旧固件），可后续基于 IDF 的 OTA + `esp_ota_mark_app_valid_cancel_rollback` 机制扩展。

## 常见问题

- **连不上热点**：确认热点名是 `ESP32C5-XXXX`（日志中会打印）；若设了密码，确认密码 ≥8 位
- **扫描不到 5G 网络**：确认路由器 5G 开启且设备处于 5G 覆盖范围（5G 穿墙弱）
- **配网后想换网络**：长按 BOOT 3 秒，或连接热点（若未关闭）重新配置
- **IDF 版本兼容性**：本工程按 IDF v6.0.1 API 编写（`ESP_ERR_WIFI_CONN`、`esp_system.h`、cjson 托管组件等）；如用 v5.4/v5.5 请留意 API 差异
