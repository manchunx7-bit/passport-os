# 维护方法

## 日常小改动

从 `AGENTS.md` 与当前状态定位模块，修改源码，跑对应测试；交付前执行完整主机回归。只更新当前状态中的变化和验证结果，不重复维护多份历史说明。

## 环境和命令

本机 Windows 使用 WSL `Ubuntu-24.04`，ESP-IDF 安装在 `/opt/esp-idf`（CI 为 5.5.3）。进入本目录对应 WSL 路径：

```bash
cd /mnt/f/WORK/AI硬件/passport-os-current
bash tools/test-host.sh
bash build.sh
```

主机测试需要 Python 3、C/C++ 编译器、Node.js 及脚本用到的常规工具。首次固件构建需按 `dependencies.lock` 准备 managed_components；本精简目录不携带约 210 MB 的下载缓存。原目录有相同依赖缓存，必要时可复用副本，不能改旧目录的依赖。

`build.sh` 每次从 `sdkconfig.defaults` 生成配置，避免旧 sdkconfig 破坏内存预算。不要删 CMake 的内存配置检查。`bash build.sh release` 是独立发布配置构建，不表示已经发布；`diag` 是诊断构建，不能当正式包。

固件交付检查（在载入 ESP-IDF 的 shell 中）：

```bash
idf.py -B build merge-bin -o FoloToy-AI-Passport-full.bin
python3 tools/verify_firmware.py build
```

`python3 tools/check_repo.py --firmware-fork` 做仓库检查；`bash tools/validate.sh --static` 还会运行 actionlint 和完整主机测试，首次缺工具时安装脚本需要联网。Git 关联与当前审阅分支以 `docs/CURRENT.md` 为准。

## 设备更新

先确认实际串口和用户当前操作范围。用户明确要求全新安装时，可以在备份后全片擦除并写入完整镜像；其他升级默认保留现场资料；新鲜整片备份留在本地诊断目录，核对分区后仅更新 `0x10000` 应用分区，读回验证并确认非应用区保持。未获得全清授权时，不要全片擦除或将合并镜像覆盖已有设备资料。

`firmware/passport-os-app.bin` 已更新为 rc.58 发布配置应用镜像，不是完整初始化镜像；源码改动并构建后，它不会自动更新。只有重新校验并更新状态/校验清单后才替换它。所有固件以 SHA256 为准。

## 原目录与交接校验

旧项目 `F:/WORK/AI硬件/passport-os` 是本次整理前的完整历史档案，保留旧文档、构建和发布包；旁边 `_diag` 保留实机证据和私密备份。日常只维护新目录。不要让 AI 同时加载两个项目。

`firmware/snapshot-manifest.json` 保存复制文件 SHA256 与源 Git HEAD；该 HEAD 仅供追溯，快照包含未提交及新增文件。源码及生成资源按原字节复制，历史测试输出被排除。2026-11-08 已复制固定版本依赖缓存，并从新目录完成 rc.57 发布配置全量构建与校验。源码快照仍不包含可再生成的 build-public 和 managed_components。

本次整理仅调整 `.gitignore` 和源码打包工具的文件清单：收录 AGENTS 与诊断默认配置；公开文档在后续远程基线整理中恢复，见 `docs/CURRENT.md`。运行源码、资源、测试及构建配置保持原字节。源码打包已做不生成发布包的 dry-run，确认精简文档会随下次打包保留。
