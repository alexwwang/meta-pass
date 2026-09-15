#!/usr/bin/env bash
# tools/signing/recover-device.sh —— 设备变砖恢复
# 用途:flash 里 bootloader 被覆盖或 app crash-loop 导致黑屏/USB 反复断开时恢复。
#
# 操作:
#   1. 按住设备 BOOT 键不放 → 点一下 RESET 键 → 松开 BOOT 键(进入下载模式)
#   2. 运行本脚本;它会等端口出现后刷入已验证的完整镜像
#
# 刷入镜像:build/meta-pass_*.bin 中修改时间最新的一份(8MB 合并镜像,偏移 0x0);
# 用第 3 个参数可指定具体文件。镜像包含 bootloader(0x0) / partition table(0x8000) /
# app(0x10000) / ota_data(0x7fe000),刷完即可重启进入 meta-pass 启动器。
#
# 用法: tools/signing/recover-device.sh [PORT] [WAIT_SECONDS] [IMAGE]
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="${1:-/dev/cu.usbmodem142401}"
WAIT_SECONDS="${2:-120}"
if [ "${3:-}" != "" ]; then
  IMAGE="$3"
else
  # 不硬编码版本号:取 build/ 下最新的 meta-pass_*.bin,避免刷入旧固件。
  IMAGE="$(ls -t "$REPO_ROOT"/build/meta-pass_*.bin 2>/dev/null | head -1)"
fi

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py 不可用,请先 source ESP-IDF 5.5.3 的 export.sh" >&2
    exit 1
fi

if [ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ]; then
    echo "ERROR: 未找到恢复镜像" >&2
    echo "请指定: tools/signing/recover-device.sh $PORT $WAIT_SECONDS <image.bin>" >&2
    echo "或先构建: tools/validate.sh --firmware  (产出 build/meta-pass_*.bin)" >&2
    exit 1
fi

echo "恢复镜像: $(basename "$IMAGE") ($(stat -f%z "$IMAGE") bytes)"
echo "目标端口: $PORT"
echo "等待窗口: ${WAIT_SECONDS}s"
echo ""
echo ">>> 请按: 按住 BOOT → 点一下 RESET → 松开 BOOT"
echo ">>> 然后等端口出现..."
echo ""

# 等设备端口出现(crash-loop 时端口会反复消失,循环探测)
start=$(date +%s)
found=0
while [ $(( $(date +%s) - start )) -lt "$WAIT_SECONDS" ]; do
    if [ -e "$PORT" ]; then
        echo "[ok] 端口 $PORT 已出现"
        found=1
        break
    fi
    sleep 1
done

if [ "$found" -ne 1 ]; then
    echo "ERROR: 等待 ${WAIT_SECONDS}s 后仍未出现 $PORT" >&2
    echo "" >&2
    echo "请确认:" >&2
    echo "  1. USB 线已连接设备" >&2
    echo "  2. 端口名是否正确(运行 ls /dev/cu.usbmodem* 查看)" >&2
    echo "  3. 下载模式操作:按住 BOOT 键 → 点一下 RESET → 松开 BOOT" >&2
    exit 1
fi

echo ""
echo ">>> 开始刷写完整镜像到 0x0 ..."
echo ""

python -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    write_flash --verify 0x0 "$IMAGE"

echo ""
echo ">>> 刷写完成并回读校验通过。设备已自动重启。"
echo ">>> 如果仍然黑屏,请再进一次下载模式并运行:"
echo "    bash tools/signing/recover-device.sh $PORT 60"
