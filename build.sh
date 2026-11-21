#!/usr/bin/env bash
# 编译 passport-os（macOS、Linux、WSL 或 Git Bash）。
#
# 用法:
#   bash build.sh          编译本地官方 Jollybot 形象版
#   bash build.sh clean    先清掉 CMake 缓存再全量编译（换 IDF 版本或路径变了才需要）
#   bash build.sh release  用官网发布配置独立构建官方 Jollybot 版并校验
#   bash build.sh diag     内存诊断版(堆任务追踪 + 申请峰值),输出到 build-diag,不可发布
#
# ESP-IDF 路径按 IDF_DIR、IDF_PATH、常见安装位置依次查找；也可以在命令行显式指定:
#   IDF_DIR=/path/to/esp-idf bash build.sh release
# 已激活 ESP-IDF 时也建议显式传入 IDF_DIR，避免用到另一个版本的工具链。
#
# 烧录说明见 docs/safe-flashing.zh_CN.md：完整镜像写 0x0，应用镜像只写 0x10000。
set -u

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_IDF_DIR="$HOME/.espressif/v5.5.3/esp-idf"

cd "$PROJECT_DIR" || { echo "找不到项目目录 $PROJECT_DIR"; exit 1; }

find_idf_dir() {
    for candidate in "$@"; do
        if [ -f "$candidate/export.sh" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [ -n "${IDF_DIR:-}" ]; then
    :
elif [ -n "${IDF_PATH:-}" ] && [ -f "${IDF_PATH}/export.sh" ]; then
    IDF_DIR="$IDF_PATH"
elif IDF_DIR="$(find_idf_dir "$DEFAULT_IDF_DIR" /opt/esp-idf "$HOME/esp/esp-idf")"; then
    :
else
    IDF_DIR="$DEFAULT_IDF_DIR"
fi

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "找不到 ESP-IDF: $IDF_DIR/export.sh"
    echo "用 IDF_DIR=/path/to/esp-idf 或 IDF_PATH=/path/to/esp-idf 指定安装目录。"
    exit 1
fi

echo "==> 载入 ESP-IDF 环境 ($IDF_DIR)"
# 标准安装用 export.sh。macOS 的 EIM 安装没有 ~/.espressif/python_env，
# export.sh 会失败，改为读取 activate_idf_<版本>.sh -e 输出的环境。
EIM_ACTIVATE="$HOME/.espressif/tools/activate_idf_$(basename "$(dirname "$IDF_DIR")").sh"
if [ -f "$EIM_ACTIVATE" ]; then
    EIM_ENV="$("$EIM_ACTIVATE" -e 2>/dev/null)"
    if [ -z "$EIM_ENV" ]; then
        echo "EIM 环境读取失败: $EIM_ACTIVATE -e"
        exit 1
    fi
    while IFS= read -r line; do
        case "$line" in
            PATH=*) IDF_ENV_PATH="${line#PATH=}" ;;
            SYSTEM_PATH=*) SYSTEM_PATH="${line#SYSTEM_PATH=}" ;;
            ESP_IDF_VERSION=*|IDF_VERSION=*|IDF_TOOLS_PATH=*|IDF_PATH=*|IDF_PYTHON_ENV_PATH=*|ESP_ROM_ELF_DIR=*|OPENOCD_SCRIPTS=*)
                export "$line"
                ;;
        esac
    done <<EOF
$EIM_ENV
EOF
    if [ -n "${IDF_ENV_PATH:-}" ]; then
        PATH="$IDF_ENV_PATH${SYSTEM_PATH:+:$SYSTEM_PATH}"
        export PATH
    fi
    unset EIM_ENV IDF_ENV_PATH SYSTEM_PATH
else
    # shellcheck disable=SC1090
    . "$IDF_DIR/export.sh" >/dev/null 2>&1
fi

if [ -x "$IDF_DIR/tools/idf.py" ]; then
    IDF_PY="$IDF_DIR/tools/idf.py"
elif command -v idf.py >/dev/null 2>&1; then
    IDF_PY="$(command -v idf.py)"
else
    echo "ESP-IDF 环境载入失败，找不到 idf.py；请检查 $IDF_DIR"
    exit 1
fi

IDF_VERSION_TEXT="$("$IDF_PY" --version 2>/dev/null | head -1)"
case "$IDF_VERSION_TEXT" in
    *v5.5*) ;;
    *) echo "警告: 仓库以 ESP-IDF v5.5.3 验证，当前为 ${IDF_VERSION_TEXT:-未知版本}" ;;
esac

if [ "${1:-}" = "release" ]; then
    # Seed from tracked defaults every time. Local sdkconfig may silently retain
    # obsolete TLS/flash options even after sdkconfig.defaults has been updated.
    mkdir -p build-public
    cp -f sdkconfig.defaults build-public/sdkconfig
    "$IDF_PY" -B build-public -DSDKCONFIG="$PROJECT_DIR/build-public/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=ON \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build \
        merge-bin -o FoloToy-AI-Passport-full.bin
    python3 tools/verify_firmware.py build-public --public
    exit 0
fi

if [ "${1:-}" = "diag" ]; then
    mkdir -p build-diag
    cat sdkconfig.defaults sdkconfig.diag > build-diag/sdkconfig
    "$IDF_PY" -B build-diag -DSDKCONFIG="$PROJECT_DIR/build-diag/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
    exit 0
fi

if [ "${1:-}" = "clean" ]; then
    echo "==> 清理构建目录"
    rm -rf build
fi

echo "==> 开始编译（本地官方 Jollybot 形象版）"
# Never reuse the ignored root sdkconfig: it may preserve obsolete options even
# after defaults change. Keep the expanded generated config inside the build
# directory and seed it from the tracked memory budget on every invocation.
mkdir -p build
cp -f sdkconfig.defaults build/sdkconfig
"$IDF_PY" -B build -DSDKCONFIG="$PROJECT_DIR/build/sdkconfig" \
    -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
    -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
