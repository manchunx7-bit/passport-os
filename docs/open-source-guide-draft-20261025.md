# Passport OS 社区安装说明

这是给普通用户的安装说明。使用预编译固件不需要安装 AI 工具；下载固件、烧录到匹配硬件，再完成首次配网即可。返回 [项目首页](../README.md) 可查看应用清单和 DIY 开发入口。

## 适用设备

- FoloToy AI Passport
- ESP32-C3、8 MB Flash
- 与发布包中的 `partitions.csv`、屏幕、音频编解码器和按键电路匹配的硬件版本

外观相似但 Flash 容量、屏幕、引脚、音频芯片或电池电路不同的设备不能直接刷入。

## 下载文件

从同一个 GitHub Release 下载：

- `FoloToy-AI-Passport-vX.Y.Z-full.bin`：首次安装或彻底重置，写入地址 `0x0`。
- `FoloToy-AI-Passport-vX.Y.Z-app.bin`：已有相同 Passport OS 分区表的普通升级，写入地址 `0x10000`。
- `SHA256.txt`：验证下载文件没有损坏或被替换。

不要混用不同版本的完整镜像、应用镜像和分区表。完整镜像会覆盖设备配置；需要保留 Wi-Fi、头像、名片或游戏记录时，应使用应用镜像并先确认分区表一致。

## 烧录镜像

完整镜像写入 `0x0`，应用镜像只写 `0x10000`。下面给出 Windows 命令；macOS / Linux、备份与误刷恢复见 [安全刷机指南](safe-flashing.zh_CN.md)。

### Windows

1. 安装 Python 3.11 或更新版本。
2. 打开 PowerShell，安装 esptool：

```powershell
py -m pip install esptool
```

3. 用 USB 连接设备，在设备管理器确认串口号，例如 `COM3`。
4. 可选：验证下载的完整镜像：

```powershell
Get-FileHash .\FoloToy-AI-Passport-vX.Y.Z-full.bin -Algorithm SHA256
```

5. 首次安装或清空数据时执行：

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x0 .\FoloToy-AI-Passport-vX.Y.Z-full.bin
```

6. 普通升级只写应用区：

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  0x10000 .\FoloToy-AI-Passport-vX.Y.Z-app.bin
```

烧录期间不要拔 USB。写入成功后设备会自动复位；如果串口暂时没有输出，重新插拔 USB 再观察设备屏幕。

### macOS / Linux

```bash
python3 -m pip install --user esptool
ls /dev/cu.usbmodem*        # macOS；Linux 用 ls /dev/ttyUSB* /dev/ttyACM*
```

首次安装或清空数据时执行（完整镜像写 `0x0`）：

```bash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 erase-flash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 FoloToy-AI-Passport-vX.Y.Z-full.bin
```

普通升级只写应用区（保留 Wi-Fi 和资料）：

```bash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x10000 FoloToy-AI-Passport-vX.Y.Z-app.bin
```

Linux 提示权限不足时，把当前用户加入 `dialout` 组并重新登录。WSL 默认看不到串口，需要 `usbipd` 绑定，或直接在 Windows 上烧录。

## 首次启动

首次启动会进入手机设置页面，并启动一个没有密码的热点，例如 `Passport-WiFi-D7A9`：

1. 手机连接这个热点。
2. 浏览器打开 `http://192.168.4.1`。
3. 保存家庭 Wi-Fi、城市和个人资料。
4. 按页面提示上传头像或名片；不需要的项目可以跳过。
5. 保存后返回首页，设备会使用自己的 Wi-Fi 连接网络。

## 功能和外部服务

- 首页天气和空气质量需要网络与城市坐标。
- 收音机依赖公共网络电台地址，电台可因地区或服务变化而失效。
- 小智需要网络和对应服务端配置。
- 随身语音需要另行运行 Windows 电脑端桥接程序。
- Muse 需要手机配对；使用代理时填写代理主机的局域网 IP 和 HTTP 端口，端口取决于代理工具。
- 狗游戏、俄罗斯方块、声音跳一跳、秒表和番茄钟可离线使用。

## 从源码构建

源码开发者需要 ESP-IDF 5.5.3、Python 3、Git，以及 Linux、macOS、WSL 或其他能够运行 bash 的环境：

```bash
git clone https://github.com/manchunx7-bit/passport-os.git
cd passport-os
bash build.sh release
```

`build.sh` 依次从 `IDF_DIR`、`IDF_PATH` 和常见安装位置查找 ESP-IDF，并兼容 macOS 的 EIM 安装；装在其它目录时用 `IDF_DIR=/实际路径/esp-idf bash build.sh release` 指定。

此命令的完整镜像输出到 `build-public/FoloToy-AI-Passport-full.bin`，应用镜像输出到 `build-public/passport-os.bin`。构建前会根据 `dependencies.lock` 获取 ESP-IDF 组件；不要把本机 `managed_components/` 或生成的 `sdkconfig` 提交到仓库。

在已激活 ESP-IDF 5.5.3 的环境中执行验证：

```bash
bash tools/test-host.sh
./tools/validate.sh --static
./tools/validate.sh --firmware
```

## 常见问题

**连接不上热点**：确认设备已经启动到手机设置页；必要时重新插拔 USB。热点是开放网络，不需要密码。

**烧录后仍出现旧资料**：说明只写了应用区，或擦除范围不完整。首次体验整机流程时使用完整镜像并先执行 `erase-flash`。

**小智或 Muse 无法连接**：先确认家庭 Wi-Fi 正常，再检查服务账号、手机配对状态和代理主机/端口。

**想保留自己的改版**：从源码修改对应应用，运行主机测试和固件验证，再生成自己的应用镜像或完整镜像。

## 许可证和贡献

Passport OS 主体使用 MIT。Muse 的 Noise 协议代码和适配层包含 Apache-2.0 上游内容，具体以组件目录中的 `LICENSE`、`NOTICE.md` 和第三方说明为准。提交代码前请不要加入 Wi-Fi 密码、API Token、设备日志、个人头像或本地配置导出。

欢迎提交问题、硬件兼容性反馈和功能改进；请在 Issue 中注明固件版本、硬件版本、烧录方式和是否保留了设备数据。
