# 安全刷机指南

本指南说明如何为 FoloToy AI Passport（ESP32-C3、8 MB Flash）选择镜像、备份数据并完成烧录。适用于 Windows、macOS、Linux 和 WSL。只想使用预编译固件时，不需要安装 ESP-IDF。

> **一句话原则：完整镜像写 `0x0`，应用镜像只写 `0x10000`。**
> 把应用镜像写到 `0x0` 会覆盖二级 bootloader 和分区表，设备会反复重启。

## 适用硬件

- FoloToy AI Passport
- ESP32-C3、8 MB Flash
- 屏幕、音频编解码器、按键电路与发布包的 `partitions.csv` 匹配

外观相似但 Flash 容量、屏幕、引脚或音频芯片不同的设备不能直接刷入。

## 先分清两类镜像

同一个版本会提供两个镜像，先确认自己要哪一种：

| 你的情况 | 选择镜像 | 写入地址 | 数据影响 |
| --- | --- | --- | --- |
| 首次安装、彻底重置或更换分区布局 | `FoloToy-AI-Passport-vX.Y.Z-full.bin` | `0x0` | 覆盖范围内全部数据，刷完需要重新配网 |
| 已装 Passport OS，只想升级固件 | `passport-os.bin`（发布包的 `*-app.bin`） | `0x10000` | 保留 Wi-Fi、头像、名片、提醒、清单和设置 |

分区布局（`partitions.csv`）：

| 区域 | 偏移 | 大小 | 内容 |
| --- | --- | --- | --- |
| bootloader | `0x0` | — | 二级 bootloader |
| 分区表 | `0x8000` | `0x1000` | 分区定义 |
| NVS | `0x9000` | `0x6000` | Wi-Fi、设置、应用数据 |
| phy_init | `0xF000` | `0x1000` | 射频校准 |
| factory app | `0x10000` | `0x400000` | 应用固件 |
| cardid | `0x410000` | — | 设备身份 |
| storage | `0x420000` | — | 头像、名片等文件 |

升级前请确认设备当前分区布局与本表一致。旧版本或自行改过分区表的设备，应使用完整镜像重新安装。

## 准备

1. 安装 Python 3.11 或更新版本。
2. 安装 esptool：

```bash
python3 -m pip install --user esptool
```

Windows 用 `py -m pip install esptool`，后面把 `python3` 换成 `py`。

3. 用 USB 数据线连接设备，确认串口：

| 系统 | 常见串口 | 查看方式 |
| --- | --- | --- |
| Windows | `COM3`、`COM5` | 设备管理器 → 端口 |
| macOS | `/dev/cu.usbmodem101` | `ls /dev/cu.usbmodem*` |
| Linux | `/dev/ttyUSB0`、`/dev/ttyACM0` | `ls /dev/ttyUSB* /dev/ttyACM*` |

Linux 提示权限不足时，把当前用户加入 `dialout` 组并重新登录：

```bash
sudo usermod -aG dialout "$USER"
```

4. 校验下载的镜像（可选）：

```bash
# macOS / Linux
shasum -a 256 FoloToy-AI-Passport-vX.Y.Z-full.bin
# Windows PowerShell
Get-FileHash .\FoloToy-AI-Passport-vX.Y.Z-full.bin -Algorithm SHA256
```

## 普通升级：只写应用分区（保留数据）

这是日常更新推荐的方式。以完整构建产物 `build-public/passport-os.bin` 为例。

### 用 ESP-IDF（推荐）

在已激活 ESP-IDF 的终端里执行，`app-flash` 会从 `flash_args` 读取正确偏移：

```bash
idf.py -B build-public -p /dev/cu.usbmodem101 app-flash
```

Windows 示例：`idf.py -B build-public -p COM3 app-flash`。

### 直接用 esptool

```bash
# macOS / Linux
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x10000 build-public/passport-os.bin

# Windows PowerShell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x10000 .\passport-os.bin
```

> 应用镜像只能写 `0x10000`。写到 `0x0` 时 ROM 会把应用当成二级 bootloader，串口反复输出
> `rst:0x7 (TG0WDT_SYS_RST)` 和 `Invalid image block, can't boot`，屏幕不断闪烁重启；
> 从 `0x0` 起的数 MB 写入还会覆盖分区表和 NVS。

## 首次安装或彻底重置：完整镜像

完整镜像已经包含 bootloader、分区表和应用，写入 `0x0`。它会覆盖范围内的配置，刷完进入首次配网。需要彻底清空时先擦除整片 Flash：

```powershell
# Windows PowerShell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x0 .\FoloToy-AI-Passport-vX.Y.Z-full.bin
```

```bash
# macOS / Linux
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 erase-flash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 FoloToy-AI-Passport-vX.Y.Z-full.bin
```

不擦除整片 Flash 也可以写完整镜像，但 NVS 中的旧配置会保留；要保证干净的首次体验，仍然需要 `erase-flash`。

**在没有备份和明确授权时，不要执行 `erase-flash`，也不要把完整镜像写进已有设备。**

## 烧录后确认

1. 烧录期间不要拔 USB。写入成功后设备自动复位。
2. 查看串口日志（`Ctrl+]` 退出）：

```bash
idf.py -B build-public -p /dev/cu.usbmodem101 monitor
```

正常启动应包含：

- `boot: Loaded app from partition at offset 0x10000`
- `App version: ...`
- `Passport OS Boot Complete`

3. 在串口输入 `v`，日志会出现 `heap_ok=1`。
4. 普通升级后确认 Wi-Fi 仍已连接、头像和名片仍在；完整镜像后确认进入 `Passport-WiFi-xxxx` 热点配网页。

## 写错偏移如何恢复

如果误把应用镜像写到 `0x0`，按完整布局恢复。NVS 配置会丢失，设备回到首次配网；`cardid` 和 `storage` 未被覆盖时仍保留。

```bash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 \
  --before default_reset --after no_reset erase_region 0x9000 0x7000
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 \
  --before no_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build-public/bootloader/bootloader.bin \
  0x8000 build-public/partition_table/partition-table.bin \
  0x10000 build-public/passport-os.bin
```

`0x9000/0x7000` 同时覆盖 NVS 与 phy_init，被应用数据污染后必须清空，由固件重新初始化。也可以直接刷入对应版本的完整镜像。

## 常见问题

**刷完还是旧界面**：确认写入的是新构建的 `passport-os.bin`，并且地址是 `0x10000`；应用镜像不会更新 bootloader 和分区表。

**应用镜像写到 `0x0` 后无限重启**：按上一节恢复完整布局，不要继续重复写应用镜像。

**始终看不到串口**：换一根支持数据传输的 USB 线；Windows 安装 CP210x/CH34x 驱动；Linux 检查 `dialout` 权限；WSL 需要 `usbipd` 把 USB 设备绑定进发行版，或直接在 Windows 上用 esptool。

**Wi-Fi 和资料丢失**：完整镜像或 `erase-flash` 会清空配置。日常升级请只用应用镜像，并在操作前备份。

**烧录中途断开**：重新连接设备，重复同一命令；写入失败通常不会破坏设备，必要时用完整镜像恢复。
