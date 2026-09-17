#!/bin/bash
# tools/signing/sign-firmware.sh —— 给子固件 app 追加 meta-pass 签名徽章。
#
#   输入可为裸 app 镜像或 Full 合并镜像(bootloader+分区表+app;发布/市场镜像即此格式)。
#   定位逻辑见 tools/signing/locate_app_image.py(与 install-slot/extract-app-image.js
#   同一契约)。输出:
#     裸 app 输入   → 槽位镜像:app + 0xFF pad + 4KB tail metadata sector;
#     合并镜像输入  → 原文件完整保留(bootloader+分区表逐字节不动,市场刷机格式不变),
#                     仅在 app 区域之后追加 pad + 4KB tail(槽位相对布局与设备一致)。
#
#   --egg-text: 可选,写入尾部 metadata sector 中部 MAEG 彩蛋文本(最长 3919 字节 ASCII)
#
# 签名段格式见 main/meta_sign.h:
#   [4B "MSIG"] [4B payload_len LE] [ECDSA-P256 DER 签名(64..72B)] [1B xor]
#   写入 image_len 之后(pad 到单一 4K tail sector 对齐)
#   尾部 sector 布局: MSIG@0、MAEG@128..4055(定长 3928B,0xFF padding)、MNAM@4056;彩蛋不覆盖 MSIG/MNAM,也不进入签名 digest。
#
# 签名流程: python 计算 app.bin 的 SHA-256 digest(覆盖 image_len 字节),
# 调用 bin/keychain-sign 用 macOS Keychain 中的私钥(标签 com.folotoy.meta-pass.signing)签名。
# 私钥不出 Keychain;首次使用会弹一次 Keychain 授权框。
# 注意: Keychain 签名使用随机 nonce,同一镜像两次签名字节不同(验签不受影响)。
# 验签在设备侧 meta_sign.c 做。此脚本不修改 image_len 以内的任何字节。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIGNER="$SCRIPT_DIR/bin/keychain-sign"

# 签名工具缺失时自动编译(cc + Security 框架,macOS 自带)。
if [ ! -x "$SIGNER" ]; then
    mkdir -p "$SCRIPT_DIR/bin"
    cc -O2 -Wall -Wextra -framework Security -framework CoreFoundation \
        "$SCRIPT_DIR/keychain_sign.c" -o "$SIGNER"
fi

usage() {
    echo "usage: $0 <app.bin> [--version VERSION] [--egg-text TEXT]" >&2
    echo "       $0 --check-egg-text TEXT   # 只校验彩蛋文本(不弹 Keychain 授权,退出码 0=合法)" >&2
}

BIN=""
EGG_TEXT=""
VERSION=""
CHECK_EGG=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)
            if [ "$#" -lt 2 ]; then usage; exit 2; fi
            VERSION="$2"
            shift 2
            ;;
        --egg-text)
            if [ "$#" -lt 2 ]; then usage; exit 2; fi
            EGG_TEXT="$2"
            shift 2
            ;;
        --check-egg-text)
            if [ "$#" -lt 2 ]; then usage; exit 2; fi
            EGG_TEXT="$2"
            CHECK_EGG=true
            shift 2
            ;;
        --*)
            echo "error: unknown option: $1" >&2
            usage
            exit 2
            ;;
        *)
            if [ -z "$BIN" ]; then
                BIN="$1"
            else
                echo "error: unexpected argument: $1" >&2
                usage
                exit 2
            fi
            shift
            ;;
    esac
done

# --check-egg-text 预检模式:只校验彩蛋文本的合法性与长度,不访问 Keychain、不读镜像。
# 校验规则与 --egg-text 写入路径完全一致(同一函数,避免两处规则漂移)。
if [ "$CHECK_EGG" = true ]; then
    python3 -c '
import sys


def validate_egg_text(text):
    """校验 --egg-text 文本;返回字节数,非法时 SystemExit。"""
    if text == "":
        raise SystemExit("error: --egg-text must not be empty")
    try:
        egg_bytes = text.encode("ascii")
    except UnicodeEncodeError as exc:
        raise SystemExit(f"error: --egg-text must be ASCII: {exc}")
    if len(egg_bytes) == 0:
        raise SystemExit("error: --egg-text must not be empty")
    if len(egg_bytes) > 3919:
        raise SystemExit(f"error: --egg-text too long: {len(egg_bytes)} > 3919 bytes")
    if any(ord(ch) < 0x20 or ord(ch) > 0x7e for ch in text):
        raise SystemExit("error: --egg-text must be printable ASCII")
    return len(egg_bytes)


n = validate_egg_text(sys.argv[1])
print(f"OK: {n} bytes (max 3919)")
' "$EGG_TEXT"
    exit $?
fi

if [ -z "$BIN" ]; then usage; exit 2; fi
if [ ! -f "$BIN" ]; then echo "error: $BIN not found"; exit 1; fi

# 版本检测: 优先 --version 参数,其次从输入文件所在 git 仓库的 describe 自动检测。
if [ -z "$VERSION" ]; then
    BIN_DIR="$(cd "$(dirname "$BIN")" && git rev-parse --show-toplevel 2>/dev/null || echo "")"
    if [ -n "$BIN_DIR" ]; then
        VERSION="$(git -c safe.directory='*' -C "$BIN_DIR" describe --tags --match 'v[0-9]*' 2>/dev/null || echo "")"
    fi
fi

SIG="$(mktemp -t metapass-sig)"
trap 'rm -f "$SIG"' EXIT

# 定位应用镜像(单一事实源 tools/signing/locate_app_image.py):
# 输出 "MODE APP_OFF IMAGE_LEN"。裸镜像 MODE=app APP_OFF=0;合并镜像 MODE=full,
# APP_OFF=factory 分区偏移(典型 0x10000)。image_len 语义与设备 esp_image_verify 一致。
LOCATE_OUT="$(python3 "$SCRIPT_DIR/locate_app_image.py" locate "$BIN")"
read -r IMG_MODE APP_OFF IMAGE_LEN <<< "$LOCATE_OUT"

# 计算 SHA-256 digest(从 APP_OFF 起覆盖 IMAGE_LEN 字节 —— 恰是设备侧 slot_sha256
# 读到的范围;合并镜像时不能把 bootloader/分区表算进去)
DIGEST="$(mktemp -t metapass-digest)"
trap 'rm -f "$SIG" "$DIGEST"' EXIT
python3 -c '
import hashlib, sys
with open(sys.argv[1], "rb") as f:
    f.seek(int(sys.argv[2]))
    data = f.read(int(sys.argv[3]))
if len(data) != int(sys.argv[3]):
    raise SystemExit(f"error: file truncated: need {sys.argv[3]} bytes at offset {sys.argv[2]}")
sys.stdout.buffer.write(hashlib.sha256(data).digest())
' "$BIN" "$APP_OFF" "$IMAGE_LEN" > "$DIGEST"

# 签名:只用 Keychain(私钥不出本机)。private.pem 是旧密钥对,与固件公钥不匹配,禁止 fallback。
if [ ! -x "$SIGNER" ]; then
    echo "error: keychain-sign not found at $SIGNER — run tools/signing/build-tools.sh" >&2
    exit 1
fi
"$SIGNER" "$DIGEST" > "$SIG"
if [ ! -s "$SIG" ]; then
    echo "error: keychain-sign produced no output — check Keychain access" >&2
    exit 1
fi
python3 - "$BIN" "$SIG" "$EGG_TEXT" "$VERSION" "$IMG_MODE" "$APP_OFF" "$IMAGE_LEN" <<'PYEOF'
import struct
import sys

bin_path, sig_path, egg_text, version, img_mode, app_off, image_len = (
    sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], int(sys.argv[6]), int(sys.argv[7]),
)

with open(bin_path, 'rb') as f:
    f.seek(app_off)
    image = f.read(image_len)
if len(image) != image_len:
    raise SystemExit(f"error: file truncated: need {image_len} bytes at offset {app_off}")
with open(sig_path, 'rb') as f:
    signature = f.read()

# image_len 由调用方经 locate_app_image.py 解析后传入(与设备 esp_image_verify /
# install-slot/extract-app-image.js 同一契约),此处不再重复解析,防两处实现漂移。

assert 64 <= len(signature) <= 72, f"unexpected DER sig length: {len(signature)}"

header = b'MSIG'
payload_len = struct.pack('<I', len(signature))
xor = 0
for b in header + payload_len + signature:
    xor ^= b
sig_blob = header + payload_len + signature + bytes([xor])

assert len(sig_blob) <= 81  # 8 + 72 + 1

sig_off = (image_len + 4095) // 4096 * 4096   # 槽位内 tail sector 落点(相对 app 起点)
pad_len = sig_off - image_len
sector = bytearray(b'\xff' * 4096)
sector[0:len(sig_blob)] = sig_blob

if egg_text:
    # 校验规则与 --check-egg-text 预检完全一致(同一套规则,防两处漂移)
    try:
        egg_bytes = egg_text.encode('ascii')
    except UnicodeEncodeError as exc:
        raise SystemExit(f"error: --egg-text must be ASCII: {exc}")
    if len(egg_bytes) == 0:
        raise SystemExit("error: --egg-text must not be empty")
    if len(egg_bytes) > 3919:
        raise SystemExit(f"error: --egg-text too long: {len(egg_bytes)} > 3919 bytes")
    if any(ord(ch) < 0x20 or ord(ch) > 0x7e for ch in egg_text):
        raise SystemExit("error: --egg-text must be printable ASCII")
    # MAEG 定长 3928B: magic(4) + len(4) + text 区(3919B, 0xFF padding) + xor(末字节)
    egg_field = bytearray(b'\xff' * 3928)
    egg_field[0:4] = b'MAEG'
    egg_field[4:8] = struct.pack('<I', len(egg_bytes))
    egg_field[8:8 + len(egg_bytes)] = egg_bytes
    xor = 0
    for b in egg_field[:3927]:
        xor ^= b
    egg_field[3927] = xor
    sector[128:4056] = egg_field

# ---- 输出组装 ----
#   裸 app 输入:   [app(image_len)][0xFF pad][4KB tail]           ← 与设备槽位 1:1
#   合并镜像输入:  [原 bootloader+分区表 0..app_off][app][pad][4KB tail]
#                  头部逐字节保留(市场刷机需要);不变量:MSIG 写在
#                  app_off + sig_off,恰为设备/安装页查找的绝对位置。
import os
base_name = os.path.basename(bin_path).rsplit('.', 1)[0]
out_dir = os.path.dirname(os.path.abspath(bin_path))
if version:
    out_name = f"{base_name}_{version}-signed.bin"
else:
    out_name = f"{base_name}-signed.bin"
out_path = os.path.join(out_dir, out_name)

head = b""
if img_mode == "full":
    with open(bin_path, 'rb') as f:
        head = f.read(app_off)
    if len(head) != app_off:
        raise SystemExit(f"error: file truncated: need {app_off} bytes for bootloader+partition table")

tail_file = app_off + sig_off
with open(out_path, 'wb') as f:
    f.write(head)
    f.write(image)
    if pad_len > 0:
        f.write(b'\xff' * pad_len)
    f.write(bytes(sector))

# 自检:重读输出,确认 MSIG 落在安装页/设备查找的位置
with open(out_path, 'rb') as f:
    f.seek(tail_file)
    probe = f.read(4)
if probe != b'MSIG':
    raise SystemExit(f"error: self-check failed: no MSIG at output offset 0x{tail_file:x}")

total = len(head) + image_len + pad_len + len(sector)
input_size = os.path.getsize(bin_path)
print(f"signed: {out_path}")
print(f"  input:      {img_mode} image ({input_size} bytes, app at file offset 0x{app_off:x})")
print(f"  image_len:  {image_len}")
print(f"  sig_offset: {sig_off} slot-relative -> 0x{tail_file:x} in output file")
print(f"  sig_blob:   {len(sig_blob)} bytes (payload_len={len(signature)})")
if egg_text:
    print(f"  egg_text:   {egg_text!r}")
print(f"  total:      {total}  (signed output file size in bytes)")
if img_mode == "full":
    print("  (full image: bootloader + partition table preserved byte-for-byte)")
PYEOF
