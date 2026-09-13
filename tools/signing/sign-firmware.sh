#!/bin/bash
# tools/signing/sign-firmware.sh —— 给子固件 app.bin 追加 meta-pass 签名徽章。
#
# 用法: sign-firmware.sh <app.bin> [private.pem]
#   默认私钥: tools/signing/private.pem(不入仓库)
#
# 签名段格式见 main/meta_sign.h:
#   [4B "MSIG"] [4B payload_len LE] [ECDSA-P256 DER 签名(70..72B)] [1B xor]
#   写入 image_len 之后(pad 到 4K sector 对齐)
#
# 签名流程: 先对 app.bin 计算 SHA-256,再用 Python cryptography 对 digest 做
# RFC 6979 确定性 ECDSA-P256 签名(同一消息+密钥 → 同一签名)。
# 验签在设备侧 meta_sign.c 做。此脚本不修改 image_len 以内的任何字节。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

BIN="${1:?usage: sign-firmware.sh <app.bin> [private.pem]}"
KEY="${2:-$SCRIPT_DIR/private.pem}"

if [ ! -f "$BIN" ]; then echo "error: $BIN not found"; exit 1; fi
if [ ! -f "$KEY" ]; then echo "error: private key $KEY not found"; exit 1; fi

python3 - "$BIN" "$KEY" <<'PYEOF'
import hashlib
import struct
import sys

bin_path, key_path = sys.argv[1], sys.argv[2]

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

out_path = bin_path.rsplit('.', 1)[0] + '-signed.bin'
with open(out_path, 'wb') as f:
    f.write(image)
    if pad_len > 0:
        f.write(b'\xff' * pad_len)
    f.write(sig_blob)

print(f"signed: {out_path}")
print(f"  image_len: {image_len}")
print(f"  sig_offset: {sig_off}")
print(f"  sig_blob: {len(sig_blob)} bytes (payload_len={len(signature)})")
print(f"  total: {image_len + pad_len + len(sig_blob)}")
PYEOF
