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
# 签名流程: python 计算 app.bin 的 SHA-256 digest,调用 bin/keychain-sign
# 用 macOS Keychain 中的私钥(标签 com.folotoy.meta-pass.signing)签名,再组装 sector。
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
    echo "usage: $0 <app.bin> [--egg-text TEXT]" >&2
}

BIN=""
EGG_TEXT=""

while [ "$#" -gt 0 ]; do
    case "$1" in
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

DIGEST="$(mktemp -t metapass-digest)"
SIG="$(mktemp -t metapass-sig)"
trap 'rm -f "$DIGEST" "$SIG"' EXIT

python3 -c 'import hashlib,sys; sys.stdout.buffer.write(hashlib.sha256(open(sys.argv[1],"rb").read()).digest())' \
    "$BIN" > "$DIGEST"

# 签名:优先 Keychain(私钥不出本机);Keychain 无密钥或超时时 fallback 到 private.pem。
PEM_KEY="$SCRIPT_DIR/private.pem"
if [ -x "$SIGNER" ]; then
    if timeout 10 "$SIGNER" "$DIGEST" > "$SIG" 2>/dev/null; then
        true  # Keychain signing succeeded
    else
        echo "  (Keychain signing unavailable; using private.pem via OpenSSL)" >&2
        openssl dgst -sha256 -sign "$PEM_KEY" -out "$SIG" "$DIGEST"
    fi
elif [ -f "$PEM_KEY" ]; then
    echo "  (No keychain-sign binary; using private.pem via OpenSSL)" >&2
    openssl dgst -sha256 -sign "$PEM_KEY" -out "$SIG" "$DIGEST"
else
    echo "error: no signing key available (Keychain or $PEM_KEY)" >&2
    exit 1
fi
python3 - "$BIN" "$SIG" "$EGG_TEXT" <<'PYEOF'
import struct
import sys

bin_path, sig_path, egg_text = sys.argv[1], sys.argv[2], sys.argv[3]

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
    # appended hash (32 bytes) if hash_appended flag is set
    hash_appended = image[23]  # hash_appended is at offset 23 in packed esp_image_header_t
    if hash_appended:
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

out_path = bin_path.rsplit('.', 1)[0] + '-signed.bin'
with open(out_path, 'wb') as f:
    f.write(image)
    if pad_len > 0:
        f.write(b'\xff' * pad_len)
    f.write(bytes(sector))

print(f"signed: {out_path}")
print(f"  image_len: {image_len}")
print(f"  sig_offset: {sig_off}")
print(f"  sig_blob: {len(sig_blob)} bytes (payload_len={len(signature)})")
if egg_text:
    print(f"  egg_text: {egg_text!r}")
print(f"  total: {image_len + pad_len + len(sector)}")
PYEOF
