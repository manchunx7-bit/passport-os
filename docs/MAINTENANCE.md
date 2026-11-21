# 维护方法

## 日常小改动

从 `AGENTS.md` 与当前状态定位模块，修改源码，跑对应测试；交付前执行完整主机回归。只更新当前状态中的变化和验证结果，不重复维护多份历史说明。

## 环境和命令

使用 ESP-IDF 5.5.3（CI 同为 5.5.3）。

**Windows 用户**：在 WSL `Ubuntu-24.04` 中操作，ESP-IDF 安装在 `/opt/esp-idf`。进入本目录对应 WSL 路径：

```bash
cd /mnt/f/WORK/AI硬件/passport-os-current
```

**macOS / Linux / WSL**：进入仓库根目录：

```bash
bash tools/test-host.sh
bash build.sh            # 本地开发构建
bash build.sh release    # 发布配置构建并校验
```

主机测试需要 Python 3、C/C++ 编译器、Node.js 及脚本用到的常规工具。首次固件构建需按 `dependencies.lock` 准备 managed_components；本精简目录不携带约 210 MB 的下载缓存。原目录有相同依赖缓存，必要时可复用副本，不能改旧目录的依赖。

`build.sh` 依次从 `IDF_DIR`、`IDF_PATH` 和常见安装位置（`~/.espressif/v5.5.3/esp-idf`、`/opt/esp-idf`、`~/esp/esp-idf`）查找 ESP-IDF，兼容标准安装与 macOS EIM 安装。已激活其它版本时请显式传入 `IDF_DIR`，避免混用工具链。`build.sh` 每次从 `sdkconfig.defaults` 生成配置，避免旧 sdkconfig 破坏内存预算。不要删 CMake 的内存配置检查。`bash build.sh release` 是独立发布配置构建，不表示已经发布；`diag` 是诊断构建，不能当正式包。

固件交付检查（在载入 ESP-IDF 的 shell 中）：

```bash
idf.py -B build merge-bin -o FoloToy-AI-Passport-full.bin
python3 tools/verify_firmware.py build
```

`python3 tools/check_repo.py --firmware-fork` 做仓库检查；`bash tools/validate.sh --static` 还会运行 actionlint 和完整主机测试，首次缺工具时安装脚本需要联网。Git 关联与当前审阅分支以 `docs/CURRENT.md` 为准。

## 设备更新

先确认实际串口和用户当前操作范围。完整镜像写 `0x0`，只用于首次安装或彻底重置；日常升级默认保留现场资料，仅更新 `0x10000` 应用分区，写后读回验证并确认非应用区保持。未获得全清授权时，不要全片擦除或将合并镜像覆盖已有设备资料。

分区布局见 `partitions.csv`：bootloader `0x0`、分区表 `0x8000`、NVS `0x9000/0x6000`、phy_init `0xF000/0x1000`、factory app `0x10000/0x400000`、cardid `0x410000`、storage `0x420000`。

完整的分平台命令、备份步骤和误刷恢复流程见 [安全刷机指南](safe-flashing.zh_CN.md)。核心约束：`idf.py -p <端口> app-flash` 或 `esptool ... 0x10000 <app.bin>` 只写应用分区，保留 NVS（Wi-Fi/设置）、cardid 和 storage；应用镜像写到 `0x0` 会导致 `rst:0x7 (TG0WDT_SYS_RST)`、`Invalid image block, can't boot` 的反复重启。串口日志出现 `Loaded app from partition at offset 0x10000`、`Passport OS Boot Complete`，发送 `v` 后 `heap_ok=1`，才算启动正常。

`firmware/passport-os-app.bin` 已更新为 rc.58 发布配置应用镜像，不是完整初始化镜像；源码改动并构建后，它不会自动更新。只有重新校验并更新状态/校验清单后才替换它。所有固件以 SHA256 为准。

## 原目录与交接校验

旧项目 `F:/WORK/AI硬件/passport-os` 是本次整理前的完整历史档案，保留旧文档、构建和发布包；旁边 `_diag` 保留实机证据和私密备份。日常只维护新目录。不要让 AI 同时加载两个项目。

`firmware/snapshot-manifest.json` 保存复制文件 SHA256 与源 Git HEAD；该 HEAD 仅供追溯，快照包含未提交及新增文件。源码及生成资源按原字节复制，历史测试输出被排除。2026-11-08 已复制固定版本依赖缓存，并从新目录完成 rc.57 发布配置全量构建与校验。源码快照仍不包含可再生成的 build-public 和 managed_components。

本次整理仅调整 `.gitignore` 和源码打包工具的文件清单：收录 AGENTS 与诊断默认配置；公开文档在后续远程基线整理中恢复，见 `docs/CURRENT.md`。运行源码、资源、测试及构建配置保持原字节。源码打包已做不生成发布包的 dry-run，确认精简文档会随下次打包保留。
