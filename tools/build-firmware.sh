#!/usr/bin/env bash
# tools/build-firmware.sh —— 一条命令完成 meta-pass 固件本地构建(面向不熟悉 ESP-IDF 的用户)。
#
# 做什么:
#   1. 自动寻找 ESP-IDF v5.5.3(常用路径 / --idf-path 参数 / $IDF_PATH 环境变量),没有则给出安装指引;
#   2. idf.py build 编译固件(含全部最新源码修复);
#   3. idf.py merge-bin 合并出可直接烧写的 8MB 完整镜像;
#   4. verify_firmware.py 校验受保护布局(应用大小 / 分区边界 / cardid 不被占用);
#   5. 产物复制到 build/ 并打印烧写指引。
#
# 用法:
#   tools/build-firmware.sh                  # 标准构建
#   tools/build-firmware.sh --idf-path ~/esp/esp-idf-v5.5.3
#   PORT=/dev/cu.usbserial-xxxx tools/build-firmware.sh   # 末尾直接给出该端口的烧写命令
#
# 前置(仅首次):
#   安装 ESP-IDF v5.5.3 与其依赖:https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32c3/get-started/
#   例:mkdir -p ~/esp && git clone -b v5.5.3 --recursive \
#        https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3 && ~/esp/esp-idf-v5.5.3/install.sh esp32c3
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

IDF_PATH_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --idf-path)
            [ $# -lt 2 ] && { echo "error: --idf-path needs a value" >&2; exit 2; }
            IDF_PATH_ARG="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. 寻找并激活 ESP-IDF v5.5.3 ---------------------------------------
IDF_CANDIDATES=()
if [ -n "$IDF_PATH_ARG" ]; then
    IDF_CANDIDATES=("$IDF_PATH_ARG")
elif [ -n "${IDF_PATH:-}" ]; then
    IDF_CANDIDATES=("$IDF_PATH")
else
    IDF_CANDIDATES=("$HOME/esp/esp-idf-v5.5.3" "$HOME/.espressif/frameworks/esp-idf-v5.5.3" "/opt/esp-idf-v5.5.3")
fi

IDF_DIR=""
for d in "${IDF_CANDIDATES[@]}"; do
    [ -f "$d/export.sh" ] && IDF_DIR="$d" && break
done
if [ -z "$IDF_DIR" ]; then
    if command -v idf.py >/dev/null 2>&1; then
        info "检测到 PATH 中已有 idf.py,直接使用"
    else
        cat >&2 <<'EOF'
error: 未找到 ESP-IDF v5.5.3。

方式一:已安装但不在默认位置?指定路径重跑:
    tools/build-firmware.sh --idf-path <你的 esp-idf-v5.5.3 目录>

方式二:尚未安装?约 10 分钟:
    mkdir -p ~/esp
    git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3
    ~/esp/esp-idf-v5.5.3/install.sh esp32c3
    tools/build-firmware.sh          # 重新运行本脚本即可
EOF
        exit 1
    fi
else
    info "激活 ESP-IDF: $IDF_DIR"
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
fi

# ---- 2. 编译 -------------------------------------------------------------
info "idf.py build(首次构建约 3~10 分钟,之后增量构建更快)"
idf.py build || fail "编译失败——把上方完整报错贴给 AI 助手或搜索 esp-idf issue"

# ---- 3. 合并完整镜像 -----------------------------------------------------
info "合并 8MB 完整镜像(merge-bin)"
idf.py merge-bin -o FoloToy-AI-Passport-full.bin >/dev/null \
    || fail "merge-bin 失败(注意:输出路径必须是文件名,见 docs/BUGS.md 记录)"

# ---- 5. 生成 launcher 升级包(仅刷 factory/分区表/bootloader/otadata,NVS 与子固件槽零接触)----
info "生成 launcher 升级包(build/upgrade/)"
mkdir -p build/upgrade
cp -f build/bootloader/bootloader.bin          build/upgrade/bootloader.bin
cp -f build/partition_table/partition-table.bin build/upgrade/partition-table.bin
cp -f build/FoloToy-AI-Passport.bin             build/upgrade/FoloToy-AI-Passport.bin
cp -f build/ota_data_initial.bin                build/upgrade/ota_data_initial.bin
# 单文件升级容器(MPUP):市场分发要求升级产物是且仅是一个文件;
# 四段镜像打包进一个 .bin,线刷页选这一个文件、按段表逐段写入各分区地址。
python3 - <<'PYEOF'
import sys
sys.path.insert(0, "tools")
from hashlib import sha256
from pathlib import Path
import struct

U = Path("build/upgrade")
PLAN = [
    ("bootloader.bin", 0x0),
    ("partition-table.bin", 0x8000),
    ("FoloToy-AI-Passport.bin", 0x10000),
    ("ota_data_initial.bin", 0x7FE000),
]
NAME_MAX, ENTRY = 32, 72
header_size = 16 + len(PLAN) * ENTRY
segs, body = [], bytearray()
for name, offset in PLAN:
    data = (U / name).read_bytes()
    segs.append((name, offset, len(data), sha256(data).digest(), len(body)))
    body += data
out = bytearray(header_size + len(body))
out[0:6] = b"MPUPV1"
struct.pack_into("<II", out, 8, header_size, len(PLAN))
for i, (name, offset, size, digest, body_off) in enumerate(segs):
    at = 16 + i * ENTRY
    nb = name.encode()
    out[at:at+len(nb)] = nb
    struct.pack_into("<II", out, at + 32, offset, size)
    out[at+40:at+72] = digest
    out[header_size+body_off:header_size+body_off+size] = (U / name).read_bytes()
version = __import__("subprocess").run(
    ["git", "-C", ".", "describe", "--tags", "--match", "v[0-9]*"],
    capture_output=True, text=True).stdout.strip() or "v0.0.0-dev"
(U / f"meta-pass-upgrade_{version}.bin").write_bytes(bytes(out))
print(f"MPUP container: {len(out)} bytes ({len(PLAN)} segments)")
PYEOF
cat > build/upgrade/flash-args.txt <<'EOF'
# launcher 升级最小写入集:升级 meta-pass 不动用户数据。
#   NVS(0x9000, 存储数据:Wi-Fi 配置、应用内部状态)、cardid、ota_0/1/2(已装子固件)全部保留。
# esptool 命令:
#   python -m esptool --port PORT write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \
#     0x10000 FoloToy-AI-Passport.bin 0x7fe000 ota_data_initial.bin
0x0 bootloader.bin
0x8000 partition-table.bin
0x10000 FoloToy-AI-Passport.bin
0x7fe000 ota_data_initial.bin
EOF

# ---- 6. 受保护布局与升级安全校验 ----
info "校验受保护固件布局与升级安全(verify_firmware.py)"
python3 tools/verify_firmware.py build/ || fail "布局校验未通过"

# ---- 5. 汇总产物与烧写指引 -----------------------------------------------
VERSION="$(git -c safe.directory='*' -C "$REPO_ROOT" describe --tags --match 'v[0-9]*' 2>/dev/null || echo v0.0.0-dev)"
cp -f build/FoloToy-AI-Passport-full.bin "build/meta-pass_${VERSION}.bin"

info "构建完成 ✓"
cat <<EOF

产物(build/ 下):
  meta-pass_${VERSION}.bin          完整 8MB 镜像(出厂/全量烧写用;数据区为擦除态)
  FoloToy-AI-Passport.bin           app 分区镜像(OTA / 安装页导入用)
  upgrade/meta-pass-upgrade_${VERSION}.bin  单文件升级容器(市场分发用,推荐)
  upgrade/{4 个分段 bin + flash-args.txt}   命令行烧写用原始分段

升级已装的 meta-pass(保留 NVS 存储数据与全部子固件,推荐):
  1. 设备按住 UP 键插 USB → 屏幕出现"安装模式"
  2. Chrome 打开 https://meta-pass.pages.dev/ (或 node tools/install-slot/server.mjs)
  3. Connect → "Upgrade launcher" → 选择 upgrade/meta-pass-upgrade_${VERSION}.bin(单文件) → Upgrade
  (页面解析容器逐段校验 SHA-256,并先读回设备分区表逐字节比对,不一致则拒绝升级)

安装到设备(首次烧录,USB 安装页自动处理签名/显示名):
  1. 设备按住 UP 键插 USB → 屏幕出现"安装模式"
  2. Chrome 打开 https://meta-pass.pages.dev/ (或 node tools/install-slot/server.mjs)
  3. Connect → 选槽位 → 选择 build/meta-pass_${VERSION}.bin → Install

命令行烧写(可选;PORT 换成你的串口,macOS 形如 /dev/cu.usbserial-xxxx):
  idf.py -p PORT flash
EOF

if [ -n "${PORT:-}" ]; then
    echo
    info "检测到 \$PORT=$PORT,直接烧写:"
    echo "  idf.py -p $PORT flash"
fi
