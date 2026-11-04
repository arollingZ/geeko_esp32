# geeko_esp32 — ESP32-S3M-N16R8 功能演示

基于 **ESP32-S3M-N16R8**（16MB Flash / 8MB PSRAM）与 **ESP-IDF v6.1.0** 的功能演示工程。

功能清单与验收要求见 [`readme.txt`](readme.txt)，当前进度：

| 序号 | 功能 | 状态 |
| ---- | ---- | ---- |
| 1 | TF 卡数据读写 + 读写准确性验证 | ⏸ 暂时屏蔽（`APP_ENABLE_TF_CARD=0`，待 TF 卡到位后改回 1） |
| 2 | WiFi 配网（Web 配网，凭据存 TF 卡/NVS） | ✅ 已完成 |
| 3 | LED 指示灯（IO1，指示网络状态） | ✅ 已完成 |
| 4 | LCD 显示（ST7789 80x160） | ✅ 已完成（时间 + 热点名，20fps） |

功能开关集中在 `main/feature_config.h`，宏为 0 时对应功能被条件编译屏蔽，
对外接口保留桩实现，调用方无需修改。

## 目录结构

```
geeko_esp32/
├── CMakeLists.txt          # 工程入口
├── sdkconfig.defaults      # 默认配置：esp32s3 / 16MB Flash / FatFS 长文件名
├── tools/                  # PC 端开发脚本（不参与固件构建）
└── main/
    ├── CMakeLists.txt
    ├── feature_config.h    # 功能开关宏（TF 卡 / WiFi / LED / LCD / 刷屏测试）
    ├── main.c              # app_main：1s 主循环非阻塞调度各功能
    ├── tf_card.c/.h        # TF 卡驱动：SPI 挂载、读写校验（当前屏蔽，桩实现）
    ├── wifi_mgr.c/.h       # WiFi 管理：SoftAP Web 配网、断线重连、外网验证
    ├── led_status.c/.h     # LED 状态指示：按网络状态闪烁（IO1）
    ├── lcd_st7789.c/.h     # LCD 底层：ST7789 初始化、填色、flush
    ├── lcd_ui.c/.h         # LCD 界面：东八区时间 + 热点名，20fps
    └── font8x16.c/.h       # 8x16 ASCII 点阵字库（由 tools 生成）
```

`tools/` 下是开发期用的 PC 端脚本，不参与固件构建：

| 脚本 | 用途 |
| ---- | ---- |
| `ttf2font.py` | 把 TTF 栅格化成 `main/font8x16.c`（纯标准库，无需 PIL） |
| `preview_ui.py` | 解析生成的 C 字库，用 ASCII art 预览两行文字的实际排版 |

## 功能一：TF 卡读写与准确性验证

### 接线（SPI 模式）

| TF 卡 | ESP32-S3 | 说明 |
| ----- | -------- | ---- |
| CS | IO2 | 片选 |
| MOSI | IO11 | 主机输出 |
| SCK | IO12 | 时钟 |
| MISO | IO13 | 主机输入 |
| VCC | 3V3 | 建议就近加 100nF 去耦 |
| GND | GND | 共地 |

- 走线较长或使用杜邦线时，可在 `tf_card.c` 里恢复 `host.max_freq_khz = TFCARD_MAX_FREQ_KHZ` 赋值并从 `20000` 降到 `10000` 提高稳定性（当前参考 xiaozhi 实现使用默认频率）。
- 模块若无板载上拉，SPI 线建议各加 10k 上拉到 3V3。
- 初始化流程参考 xiaozhi-esp32 工程 `xingzhi-abs-2.0` 板的 `InitializeSDcardSpi()`（SPI 模式一步挂载）；引脚宏集中在 `tf_card.c` 顶部，必须与实际接线一致。

### 实现要点

- 使用 `SPI2_HOST` + `esp_vfs_fat_sdspi_mount()` 一步完成「SPI 设备挂载 + FAT 挂载」，挂载点为 `/sdcard`。
- 挂载成功后可直接用 `fopen/fread/fwrite/fseek/stat/opendir` 等 POSIX 接口访问卡内文件（后续 WiFi 凭据文件同理）。
- 默认**不会**自动格式化卡：卡需为 FAT16/FAT32。确认为空白卡时，把 `TFCARD_FORMAT_IF_MOUNT_FAILED` 改成 `1` 即可自动格式化。
- 未插卡时 `app_main` 每 5 秒重试一次，自检失败会自动卸载并重试，不会死机。

### 自检流程（`tf_card_selftest()`）

1. 写：向 `/sdcard/rw_test.bin` 写入 64KB 可复现图案数据（4KB 一个分块），统计耗时与速度，同时累计 CRC32。
2. 读：读回同样长度，先校验文件长度，再**逐字节比对**图案，并重新计算 CRC32 与写入值比对。
3. 追加：以追加模式再写 512 字节，用偏移读取（offset = 64KB）核对，验证 append 语义。
4. 随机读：从文件偏移 12345 处读取 512 字节核对，验证 FATFS 簇/扇区寻址。
5. 打印卡信息、剩余空间及根目录文件列表。

串口预期输出（节选）：

```
I (312) APP: ==================================================
I (312) APP:  ESP32-S3M-N16R8 demo project
I (312) APP:  ESP-IDF v6.1.0
I (312) APP:  feature 1: TF card read / write + verification
I (312) APP: ==================================================
I (322) TFCARD: Initializing SD card over SPI (CS=2 MOSI=11 SCK=12 MISO=13)
I (352) TFCARD: TF card mounted at /sdcard
I (352) TFCARD: ---- TF card info ----
I (352) TFCARD: name        : SD32G
I (352) TFCARD: capacity    : 30436 MB
I (352) TFCARD: filesystem  : 30420 KB total, 30400 KB free
I (352) TFCARD: ----------------------
I (352) TFCARD: ===== TF card read/write self test =====
I (402) TFCARD: write: 65536 bytes in 47 ms (1362 KB/s)
I (452) TFCARD: read : 65536 bytes in 41 ms (1561 KB/s)
I (452) TFCARD: byte compare OK
I (452) TFCARD: CRC32 OK (0x1A2B3C4D)
I (462) TFCARD: random access read OK (offset 65536, 512 bytes)
I (472) TFCARD: append OK (file size = 66048 bytes)
I (482) TFCARD: random access read OK (offset 12345, 512 bytes)
I (492) TFCARD: ===== TF card self test PASSED =====
I (492) APP: TF card is ready for use at /sdcard
```

### 对外接口（`tf_card.h`）

| 接口 | 说明 |
| ---- | ---- |
| `tf_card_init()` / `tf_card_deinit()` | 挂载 / 卸载（可重复调用） |
| `tf_card_is_mounted()` / `tf_card_handle()` | 查询状态、取 `sdmmc_card_t *` |
| `tf_card_print_info()` | 打印卡信息与剩余空间 |
| `tf_card_selftest()` | 读写准确性自检 |
| `tf_card_write_file()` / `tf_card_read_file()` | 整文件覆盖写 / 读取，后续存 WiFi 凭据直接复用 |
| `tf_card_list_dir()` | 列出目录内容 |

## 功能二：WiFi 配网（Web 配网 + 断线重连）

实现于 `wifi_mgr.c`，满足 `readme.txt` 功能点 2 的全部要求：

- **APSTA 模式**：SoftAP `ESP32_DEMO` 常开（开放网络）作为配网入口；STA 负责连接路由器。
- **配网流程**：手机连接 `ESP32_DEMO` → 系统自动弹出「登录网络」/浏览器打开任意网址
  → 页面显示设备实时扫描到的附近热点（下拉选择，支持手动输入隐藏热点）→ 填密码提交
  → 设备保存凭据并连接。
- **凭据存储**：TF 卡可用（`APP_ENABLE_TF_CARD=1` 且挂载成功）时写入
  `/sdcard/wifi_cfg.txt`（`ssid=`/`pass=` 两行）并双写 NVS 兜底；TF 卡屏蔽期间仅写 NVS。
  重启后优先从 TF 卡读取，自动恢复连接。
- **断线重连**：STA 断开由事件驱动立即重连；SoftAP 常开，随时可连回重新配网。
- **外网验证**：每 10s 请求 `generate_204`（URL 见 `NET_CHECK_URL` 可改），连通性反映在
  状态机上。
- **状态机**（`wifi_mgr_get_state()`，供后续 LED/LCD 功能复用）：
  `NO_CRED → CONNECTING → CONNECTED → INTERNET_OK`。

### 门户（captive portal）是怎么工作的

「连上热点就自动弹配网页」不是浏览器自己找上门，而是三件事配合：

1. **DHCP 下发 DNS = 192.168.4.1**（option 6）——否则客户端解析不了域名，
   浏览器敲网址直接报「无法解析」，请求根本到不了 ESP32。
   IDF 的 DHCP 服务端默认**不发** DNS，代码里用 `esp_netif_dhcps_option(DOMAIN_NAME_SERVER)`
   显式打开 `OFFER_DNS`。
2. **DNS 劫持（UDP 53）**——`dns_server_task` 对所有域名的 A 查询都回 `192.168.4.1`，
   于是访问 `www.baidu.com` 也会连到本机。
3. **HTTP 302 跳转**——系统探测路径（Android `generate_204`、iOS `hotspot-detect.html`、
   Windows `connecttest.txt` 等）以及 Host 不是本机 IP 的请求一律 302 到
   `http://192.168.4.1/`，系统据此判断「需要登录」并弹出提示；只有直接访问本机 IP 时才返回配网页。
   另通过 DHCP option 114 广播门户地址，部分系统会直接打开。

> 注意：HTTPS 请求（`https://...`）无法劫持（TLS 握手过不去），所以别用 https 网址测试，
> 用系统弹出的提示或手输 `http://192.168.4.1/`。

### 首次配网操作步骤

1. 烧录后串口日志出现 `DHCP serves DNS 192.168.4.1 ...` 与
   `Config portal ready: join "ESP32_DEMO", open any URL or http://192.168.4.1/`
2. 手机连接热点 `ESP32_DEMO`（无密码）。**改代码后首次连接要先「忘记网络」再重连**，
   否则手机会沿用旧的 DHCP 租约（里面没有 DNS）。
3. 手机会自动弹「登录网络」提示；没有的话打开浏览器输入 `http://192.168.4.1/` 或任意 http 网址
4. 选择热点、输入密码、点「保存并连接」，页面提示成功后 5s 自动返回首页可查看状态

### IDF v6.1 API 注意

- WiFi 接口枚举用 `WIFI_IF_STA` / `WIFI_IF_AP`（旧 `ESP_IF_WIFI_*` 已移除）
- HTTP 请求体长度直接读 `req->content_len` 字段（旧 `httpd_req_get_content_len()` 已移除）
- 通配 URI 需 `cfg.uri_match_fn = httpd_uri_match_wildcard`，`uri="*"` 可匹配任意路径

## 功能三：LED 状态指示（IO1）

实现于 `led_status.c`，独立低优先级任务按 `wifi_mgr_get_state()` 的状态查表播放闪烁序列，
状态切换时序列从头开始并在串口打印 `LED: state=...`，方便对照验收现象。

| WiFi 状态 | 含义 | LED 行为（readme.txt 要求） |
| ---- | ---- | ---- |
| `NO_CRED` | 未配网 | 熄灭 |
| `CONNECTING` | 已保存凭据但未连上热点 | 快闪 2 下（亮 100ms → 灭 100ms → 亮 100ms）后熄灭 1s，循环 |
| `CONNECTED` | 已连上热点，外网不通 | 慢闪：亮 500ms / 灭 500ms |
| `INTERNET_OK` | 外网连通 | 常亮 |

- 引脚 `LED_GPIO_NUM=1`，亮灭电平 `LED_ACTIVE_LEVEL`（本板为灌电流驱动，**低电平点亮**，
  已实测确认；换高电平驱动的板子改为 1）。
- `APP_ENABLE_LED=0` 时整体条件编译屏蔽（接口返回 `ESP_ERR_NOT_SUPPORTED`）；
  `APP_ENABLE_WIFI=0` 时 LED 仅初始化并保持熄灭（没有网络状态可指示）。

## 功能四：LCD 显示（ST7789 80x160）

实现于 `lcd_st7789.c`，驱动直接用 **IDF v6.1 内置的 `esp_lcd_panel_st7789`**
（`esp_lcd` 组件），不手写初始化序列。

### 接线

| 屏 | ESP32-S3 | 说明 |
| -- | -------- | ---- |
| CS | IO39 | 片选 |
| DC | IO40 | 数据/命令选择 |
| RST | IO38 | 复位 |
| BL | IO41 | 背光，高电平点亮 |
| SCLK | IO12 | 时钟 |
| MOSI | IO11 | 数据（单向，屏不回读） |

> **与 TF 卡共用总线**：IO11/12/13 是 TF 卡和 LCD 同一条 SPI2 总线（板载就是这样设计的），
> 两者靠各自的 CS 分时复用。`lcd_init()` 遇到 `ESP_ERR_INVALID_STATE` 会当作「总线已被
> 初始化过」直接复用，所以两个功能谁先初始化都行。

### 面板参数（`lcd_st7789.c` 顶部宏）

| 宏 | 默认 | 说明 |
| -- | ---- | ---- |
| `LCD_RGB_ORDER` | `BGR` | 红蓝颠倒时改成 `LCD_RGB_ELEMENT_ORDER_RGB` |
| `LCD_INVERT_COLOR` | `true` | 黑变白/白变黑时取反 |
| `LCD_SWAP_XY` | `true` | 横屏（160 宽 × 80 高） |
| `LCD_MIRROR_X/Y` | `false` / `true` | 上下或左右镜像 |
| `LCD_OFFSET_X/Y` | `1` / `26` | 可见区域在控制器 RAM 中的偏移 |
| `LCD_PIXEL_CLOCK_HZ` | 60 MHz | 缩短写帧窗口抗撕裂；不稳（花屏）降到 40/20 MHz |

颜色用 `LCD_RGB565(r, g, b)` 宏构造（RGB565，传 MCU 原生 `uint16_t`，
所以 `data_endian` 设为 `LCD_RGB_DATA_ENDIAN_LITTLE`）。

### 周期刷屏测试

`feature_config.h` 里的 **`APP_LCD_COLOR_TEST`** 决定 LCD 跑哪种模式：

```c
#define APP_LCD_COLOR_TEST   1   // 逐色刷屏测试（每 3s 换一色）
#define APP_LCD_COLOR_TEST   0   // 正常运行显示界面（时间 + 热点名）—— 当前默认
```

设为 1 时，`lcd_init()` 内部创建 `lcd_sweep` 任务，每 **3 秒**整屏换一种颜色循环，
用于验证屏幕与接线（颜色顺序、反色、偏移、撕裂）：

```
红 → 绿 → 蓝 → 白 → 黑 → 黄 → 青 → 品红 → 红 → ...
```

串口每次打印 `LCD: sweep #N: <color>`。设为 0 时这个任务完全不编译（省 flash），
屏幕由 `lcd_ui` 任务驱动。

### 正常显示界面

`APP_LCD_COLOR_TEST = 0` 时，`lcd_ui_init()` 启动界面任务，**每 50ms（20fps）**刷新一次：

```
                2026-11-04 16:13:26
                   MyHomeWiFi
```

| 行 | 内容 | 说明 |
| -- | ---- | ---- |
| 第一行 | `年-月-日 时:分:秒` | **东八区（CST-8）**，通过 SNTP 自动对时 |
| 第二行 | 当前连接的 WiFi 热点名 | 未配网时显示 `no wifi config`；超过 20 列截断加 `..` |

时间来源是 **SNTP**（`ntp.aliyun.com`，时区 `CST-8`）。上电时 WiFi 往往还没连上，
首次同步必然失败，所以未对时期间任务每 1.5s 重新发起一次同步（最多重试 10 分钟），
对时成功前第一行显示 `NTP syncing...`，成功后不再变化。

**字体是 8×16 ASCII 点阵**（95 个字符，1520 字节），由 `tools/ttf2font.py` 从
Arial 栅格化生成，每行 20 个字符（160 / 8）。注意 **不支持中文**：含中文的 SSID
会显示成 `?`（字库只覆盖 ASCII 32..126）。需要中文可以扩展字库，见文末说明。

### 抗撕裂（tearing）

刷屏时若看到「上半屏一种颜色、下半屏另一种颜色」并一闪而过，中间还夹一条黑线，
这是 **屏幕撕裂（tearing）**，不是 bug，而是 ST7789 没有双缓冲导致的：
屏幕以约 60Hz 持续从显存扫描输出，我们一边往里写，**写入追上扫描线**时就会出现这条分界。

| 手段 | 做法 | 效果 |
| ---- | ---- | ---- |
| 整帧一次传完 | `lcd_fill_rect()` 用整屏缓冲（160×80×2 = 25600 字节）单次 `draw_bitmap`，不再分 4 块 | 分块之间的命令/等待间隙最容易被看到，消除它即大幅改善；整帧 @60MHz 约 3.4ms |
| 提高 SPI 时钟 | `LCD_PIXEL_CLOCK_HZ` = 60MHz（不稳可降 40MHz） | 线性缩短写入窗口 |
| 关显示写帧 | `LCD_FILL_DISPOFF_AROUND = 1` | **彻底无撕裂**，但每帧闪一下，只适合纯色测试 |
| TE 垂直同步 | 硬件接 TE 引脚 | 最正统，但**本板 TE 未引出**，用不了 |

当前默认：整帧传输 + 60MHz（撕裂窗口 ≈ 3.4ms / 16.7ms 一帧，肉眼基本不可见）。
填充缓冲改为一次性分配、反复复用，不再每帧 `malloc/free`。

### 一次性纯色自检（联调用）

`lcd_color_test()` 与刷屏任务分开：需人工诊断时在 `app_main` 里临时调用一次，
它依次整屏刷 **白 → 红 → 绿 → 蓝 → 黑**（各 400ms），最后留 **黑底 2px 白色边框**。
正常启动流程不调用它。

| 现象 | 原因 | 处理 |
| ---- | ---- | ---- |
| 完全不亮 | 背光/接线/CS | 查 BL=IO41 电平、SPI 引脚、共地 |
| 「红」显示成蓝色 | R/B 通道颠倒 | `LCD_RGB_ORDER` 改成 RGB |
| 黑变白、白变黑 | 反色配置反了 | `LCD_INVERT_COLOR` 取反 |
| 边框缺一边 / 偏出屏幕 | 偏移或方向不对 | 调 `LCD_OFFSET_X/Y`、`LCD_MIRROR_*` |
| 刷屏时上下半屏颜色不同、夹黑线，一闪而过 | **屏幕撕裂**（写入追上扫描线） | 已默认整帧传输 + 60MHz；仍明显就开 `LCD_FILL_DISPOFF_AROUND=1` |
| 花屏、雪花 | 时钟太快 / 传输未同步 | 降 `LCD_PIXEL_CLOCK_HZ`（60→40→20）；代码已用传输完成回调等待 |

### 对外接口（`lcd_st7789.h`）

| 接口 | 说明 |
| ---- | ---- |
| `lcd_init()` | 初始化总线 + panel IO + ST7789 + 背光（按 `APP_LCD_COLOR_TEST` 决定是否起刷屏测试） |
| `lcd_backlight(bool)` | 背光开关 |
| `lcd_fill_color(color)` | 整屏填色 |
| `lcd_fill_rect(x,y,w,h,color)` | 填矩形（自动裁剪） |
| `lcd_flush(x,y,w,h,buf)` | 把内存里的一块 RGB565 缓冲送到屏幕（UI 层用） |
| `lcd_color_test()` | 一次性纯色 + 边框自检（人工诊断用，启动流程不调用） |
| `lcd_ui_init()` | 启动 SNTP 对时 + 界面刷新任务（20fps） |

### 重新生成字库 / 换成其他字体

`tools/ttf2font.py` 只用 Python 标准库解析 TTF，不依赖 PIL：

```bash
python tools/ttf2font.py C:/Windows/Fonts/arial.ttf main/font8x16.c 14 12
#                        <字体文件>                    <输出>      <字号> <基线>
```

改完字号/字体后可先用 `preview_ui.py` 在终端预览排版（ASCII art），确认无误再编译：

```bash
python tools/preview_ui.py
```

> 想支持中文需要另建大字号点阵（GB2312 常用字 6763 个 × 16B ≈ 108KB），
> 且中文字形无法塞进 8×16 网格，一般要单独做 16×16 字库并按 UTF-8 解码查表。

## 编译与烧录

```bash
# 0) 安装 ESP-IDF v6.1（任意路径），并初始化子模块（否则 bootloader/mbedtls 等无法编译）
cd <你的 esp-idf 目录>
git submodule update --init --recursive

# 1) 激活 IDF 环境
#    Windows PowerShell:  .\export.ps1
#    Linux/macOS:         . ./export.sh

# 2) 编译
cd geeko_esp32
idf.py set-target esp32s3     # 仅首次；sdkconfig.defaults 已指定 esp32s3
idf.py build

# 3) 烧录并查看日志
idf.py -p COMx flash monitor
```

## 常见问题

| 现象 | 排查方向 |
| ---- | -------- |
| `ESP_ERR_TIMEOUT` / 卡初始化失败 | 检查 3.3V 供电、MOSI/MISO 是否接反、CS 是否为 IO2、共地 |
| 板载 TF 卡座仍超时 | 正点原子 DNESP32S3M 的 **TF 卡与 SPI LCD 共用 IO11/12/13**，确认 LCD 未选中或先拔掉 LCD 屏/跳线；卡要插进板载自锁卡座并插到底 |
| 挂载失败 `ESP_FAIL` | 卡不是 FAT16/FAT32（exFAT 不支持），用 PC 格式化为 FAT32 |
| 读写校验偶发失败 | 降低 `TFCARD_MAX_FREQ_KHZ` 到 10000 或 4000，缩短杜邦线，加去耦电容 |
| 连上 `ESP32_DEMO` 但打不开网页、也不弹配网页 | 先「忘记网络」重连拿新 DHCP 租约（DNS=192.168.4.1）；手输 `http://192.168.4.1/`；别用 https 网址；安卓开了「私人 DNS」会绕过劫持，关掉或用 IP 直连 |
| 门户页能开但点保存没反应 | 看串口是否有 `New credentials:` 日志；页面依赖 POST `/save`，用手机自带浏览器而非内置 WebView |
| LCD 颜色不对/花屏/边框偏移 | 见「功能四」里的现象对照表：调 `LCD_RGB_ORDER`、`LCD_INVERT_COLOR`、`LCD_OFFSET_*`、`LCD_PIXEL_CLOCK_HZ` |
| LCD 第一行一直显示 `NTP syncing...` | 设备没联网（看 LED 是否常亮）；SNTP 需要能访问外网 UDP 123 |
| LCD 第二行显示 `?????` | SSID 含中文/非 ASCII 字符，当前字库只覆盖 ASCII |
| LCD 想回到刷屏测试 | `feature_config.h` 里 `APP_LCD_COLOR_TEST` 改回 1 |
| 反复重启 | 检查是否误开了 PSRAM 配置（`sdkconfig.defaults` 中相关项默认注释） |
| 移植到其他板后 LED 现象相反 | 该板 LED 为高电平驱动，把 `led_status.c` 的 `LED_ACTIVE_LEVEL` 改为 1（本板为低电平点亮） |

## 许可证

[MIT License](LICENSE)，可自由使用、修改与分发，保留版权声明即可。

