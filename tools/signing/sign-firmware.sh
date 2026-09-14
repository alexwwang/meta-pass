#!/bin/bash
# tools/signing/sign-firmware.sh —— 给子固件 app.bin 追加 meta-pass 签名徽章。
#
#   --egg-text: 可选,写入尾部 metadata sector 中部 MAEG 彩蛋文本(最长 3919 字节 ASCII)
#
# 签名段格式见 main/meta_sign.h:
#   [4B "MSIG"] [4B payload_len LE] [ECDSA-P256 DER 签名(70..72B)] [1B xor]
#   写入 image_len 之后(pad 到单一 4K tail sector 对齐)
#   尾部 sector 布局: MSIG@0、MAEG@128..4055(定长 3928B,0xFF padding)、MNAM@4056;彩蛋不覆盖 MSIG/MNAM,也不进入签名 digest。
#
# 签名流程: 先对 app.bin 计算 SHA-256,再用 Python cryptography 对 digest 做
# RFC 6979 确定性 ECDSA-P256 签名(同一消息+密钥 → 同一签名)。
# 验签在设备侧 meta_sign.c 做。此脚本不修改 image_len 以内的任何字节。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    echo "usage: $0 <app.bin> [private.pem] [--egg-text TEXT]" >&2
}

BIN=""
KEY=""
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
            elif [ -z "$KEY" ]; then
                KEY="$1"
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
if [ -z "$KEY" ]; then KEY="$SCRIPT_DIR/private.pem"; fi

if [ ! -f "$BIN" ]; then echo "error: $BIN not found"; exit 1; fi
if [ ! -f "$KEY" ]; then echo "error: private key $KEY not found"; exit 1; fi

python3 - "$BIN" "$KEY" "$EGG_TEXT" <<'PYEOF'
import hashlib
import struct
import sys

bin_path, key_path, egg_text = sys.argv[1], sys.argv[2], sys.argv[3]

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed

with open(bin_path, 'rb') as f:
    image = f.read()

image_len = len(image)
sig_off = (image_len + 4095) // 4096 * 4096
pad_len = sig_off - image_len

with open(key_path, 'rb') as f:
    private_key = serialization.load_pem_private_key(f.read(), password=None)

assert isinstance(private_key, ec.EllipticCurvePrivateKey), \
    f"expected EC key, got {type(private_key).__name__}"

digest = hashlib.sha256(image).digest()
# 对预计算 SHA-256 digest 签名；cryptography 使用 RFC 6979 deterministic nonce。
signature = private_key.sign(
    digest,
    ec.ECDSA(Prehashed(hashes.SHA256()), deterministic_signing=True),
)

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
