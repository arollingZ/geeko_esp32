# demo_project — ESP32-S3M-N16R8 功能演示

基于 **ESP32-S3M-N16R8**（16MB Flash / 8MB PSRAM）与 **ESP-IDF v6.1.0** 的功能演示工程。

功能清单与验收要求见 `../demo_project/readme.txt`，当前进度：

| 序号 | 功能 | 状态 |
| ---- | ---- | ---- |
| 1 | TF 卡数据读写 + 读写准确性验证 | ⏸ 暂时屏蔽（`APP_ENABLE_TF_CARD=0`，待 TF 卡到位后改回 1） |
| 2 | WiFi 配网（Web 配网，凭据存 TF 卡/NVS） | ✅ 已完成 |
| 3 | LED 指示灯（IO1，指示网络状态） | ⬜ 待实现 |
| 4 | LCD 显示（ST7735S 80x160） | ⬜ 待实现 |

功能开关集中在 `main/feature_config.h`，宏为 0 时对应功能被条件编译屏蔽，
对外接口保留桩实现，调用方无需修改。

## 目录结构

```
demo_project/
├── CMakeLists.txt          # 工程入口
├── sdkconfig.defaults      # 默认配置：esp32s3 / 16MB Flash / FatFS 长文件名
└── main/
    ├── CMakeLists.txt
    ├── feature_config.h    # 功能开关宏（TF 卡 / WiFi / 后续 LED、LCD）
    ├── main.c              # app_main：1s 主循环非阻塞调度各功能
    ├── tf_card.c/.h        # TF 卡驱动：SPI 挂载、读写校验（当前屏蔽，桩实现）
    └── wifi_mgr.c/.h       # WiFi 管理：SoftAP Web 配网、断线重连、外网验证
```

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

## 编译与烧录

```bash
# 1) 首次使用本仓库前，必须初始化 IDF 子模块（否则 bootloader/mbedtls 等无法编译）
cd /d/ESP-IDF/esp-idf-v6.1
git submodule update --init --recursive

# 2) 激活 IDF 环境（Windows PowerShell）
D:\ESP-IDF\esp-idf-v6.1\export.ps1

# 3) 编译
cd D:\ESP-IDF\my_projects\demo_project
idf.py set-target esp32s3     # 仅首次；sdkconfig.defaults 已指定 esp32s3
idf.py build

# 4) 烧录并查看日志
idf.py -p COMx flash monitor
```

> 工程位于 IDF 仓库目录内，构建时会自动被 IDF 的组件扫描跳过（`my_projects` 下无 `components` 目录冲突），无需额外设置 `EXTRA_COMPONENT_DIRS`。

## 常见问题

| 现象 | 排查方向 |
| ---- | -------- |
| `ESP_ERR_TIMEOUT` / 卡初始化失败 | 检查 3.3V 供电、MOSI/MISO 是否接反、CS 是否为 IO2、共地 |
| 板载 TF 卡座仍超时 | 正点原子 DNESP32S3M 的 **TF 卡与 SPI LCD 共用 IO11/12/13**，确认 LCD 未选中或先拔掉 LCD 屏/跳线；卡要插进板载自锁卡座并插到底 |
| 挂载失败 `ESP_FAIL` | 卡不是 FAT16/FAT32（exFAT 不支持），用 PC 格式化为 FAT32 |
| 读写校验偶发失败 | 降低 `TFCARD_MAX_FREQ_KHZ` 到 10000 或 4000，缩短杜邦线，加去耦电容 |
| 连上 `ESP32_DEMO` 但打不开网页、也不弹配网页 | 先「忘记网络」重连拿新 DHCP 租约（DNS=192.168.4.1）；手输 `http://192.168.4.1/`；别用 https 网址；安卓开了「私人 DNS」会绕过劫持，关掉或用 IP 直连 |
| 门户页能开但点保存没反应 | 看串口是否有 `New credentials:` 日志；页面依赖 POST `/save`，用手机自带浏览器而非内置 WebView |
| 反复重启 | 检查是否误开了 PSRAM 配置（`sdkconfig.defaults` 中相关项默认注释） |
