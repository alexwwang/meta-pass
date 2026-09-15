#!/bin/bash
# tools/signing/sign-firmware.sh —— 给子固件 app.bin 追加 meta-pass 签名徽章。
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
}

BIN=""
EGG_TEXT=""
VERSION=""

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

# 计算 image_len（与设备侧 esp_image_verify 语义一致：segments + checksum pad + appended hash）
IMAGE_LEN=$(python3 -c '
import struct, sys
with open(sys.argv[1], "rb") as f:
    image = f.read()
if not image or image[0] != 0xE9:
    print(len(image)); sys.exit()
seg_count = image[1]
offset = 24
for i in range(seg_count):
    seg_len = struct.unpack("<I", image[offset+4:offset+8])[0]
    offset += 8 + seg_len
unpadded = offset
length = (unpadded + 1 + 15) & ~15
offset = unpadded + (length - unpadded)
if image[23] & 1:
    offset += 32
print(offset)
' "$BIN")

# 计算 SHA-256 digest（覆盖 image_len 字节，与设备侧 slot_sha256 一致）
DIGEST="$(mktemp -t metapass-digest)"
trap 'rm -f "$SIG" "$DIGEST"' EXIT
python3 -c '
import hashlib, sys
with open(sys.argv[1], "rb") as f:
    data = f.read(int(sys.argv[2]))
sys.stdout.buffer.write(hashlib.sha256(data).digest())
' "$BIN" "$IMAGE_LEN" > "$DIGEST"

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
python3 - "$BIN" "$SIG" "$EGG_TEXT" "$VERSION" <<'PYEOF'
import struct
import sys

bin_path, sig_path, egg_text, version = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

with open(bin_path, 'rb') as f:
    image = f.read()
with open(sig_path, 'rb') as f:
    signature = f.read()

def compute_esp_image_len(image):
    """Mirror esp_image_verify(): header + segments + checksum pad + appended hash."""
    if not image or image[0] != 0xE9:
        return len(image)
    seg_count = image[1]
    hdr_len = 24  # esp_image_header_t (IDF 5.x extended)
    offset = hdr_len
    for i in range(seg_count):
        if offset + 8 > len(image):
            raise SystemExit(f"error: truncated image header at segment {i}")
        seg_len = struct.unpack('<I', image[offset+4:offset+8])[0]
        offset += 8 + seg_len
    # checksum: 1 byte at current offset, then pad to 16-byte boundary
    unpadded = offset
    length = (unpadded + 1 + 15) & ~15
    offset = unpadded + (length - unpadded)
    # appended hash (32 bytes) if HASH_APPENDED flag is set at byte 23
    # (IDF esp_image_header_t: 24-byte packed struct, hash_appended is last byte)
    if image[23] & 1:
        offset += 32
    return offset

image_len = compute_esp_image_len(image)
sig_off = (image_len + 4095) // 4096 * 4096
pad_len = sig_off - len(image)

assert 64 <= len(signature) <= 72, f"unexpected DER sig length: {len(signature)}"

header = b'MSIG'
payload_len = struct.pack('<I', len(signature))
xor = 0
for b in header + payload_len + signature:
    xor ^= b
sig_blob = header + payload_len + signature + bytes([xor])

assert len(sig_blob) <= 81  # 8 + 72 + 1

sector = bytearray(b'\xff' * 4096)
sector[0:len(sig_blob)] = sig_blob

if egg_text:
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

import os
base_name = os.path.basename(bin_path).rsplit('.', 1)[0]
out_dir = os.path.dirname(os.path.abspath(bin_path))
if version:
    out_name = f"{base_name}_{version}-signed.bin"
else:
    out_name = f"{base_name}-signed.bin"
out_path = os.path.join(out_dir, out_name)
with open(out_path, 'wb') as f:
    f.write(image)
    if pad_len > 0:
        f.write(b'\xff' * pad_len)
    f.write(bytes(sector))

print(f"signed: {out_path}")
if version:
    print(f"  version:  {version}")
print(f"  image_len: {image_len}")
print(f"  sig_offset: {sig_off}")
print(f"  sig_blob: {len(sig_blob)} bytes (payload_len={len(signature)})")
if egg_text:
    print(f"  egg_text: {egg_text!r}")
print(f"  total: {image_len + pad_len + len(sector)}")
PYEOF
