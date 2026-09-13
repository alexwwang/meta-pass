#!/bin/bash
# tools/signing/sign-firmware.sh —— 给子固件 app.bin 追加 meta-pass 签名徽章。
#
# 用法: sign-firmware.sh <app.bin> [private.pem]
#   默认私钥: tools/signing/private.pem(不入仓库)
#
# 签名段格式见 main/meta_sign.h:
#   [4B "MSIG"] [4B payload_len=256 LE] [256B RSA-2048-PKCS1v15 签名] [1B xor]
#   = 265 字节,写入 image_len 之后(pad 到 4K sector 对齐)
#
# 验签在设备侧 meta_sign.c 做。此脚本不修改 image_len 以内的任何字节。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BIN="${1:?usage: sign-firmware.sh <app.bin> [private.pem]}"
KEY="${2:-$SCRIPT_DIR/private.pem}"

if [ ! -f "$BIN" ]; then echo "error: $BIN not found"; exit 1; fi
if [ ! -f "$KEY" ]; then echo "error: private key $KEY not found"; exit 1; fi

IMAGE_LEN=$(wc -c < "$BIN" | tr -d ' ')
SIG_OFF=$(( (IMAGE_LEN + 4095) / 4096 * 4096 ))
PAD_LEN=$(( SIG_OFF - IMAGE_LEN ))

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# 1. 计算 image 的 SHA-256,生成签名
openssl dgst -sha256 -sign "$KEY" -out "$TMPDIR/sig.bin" "$BIN"
SIG_LEN=$(wc -c < "$TMPDIR/sig.bin" | tr -d ' ')
if [ "$SIG_LEN" -ne 256 ]; then
  echo "error: signature is $SIG_LEN bytes, expected 256 (RSA-2048)"; exit 1
fi

# 2. 组装签名段: magic(4) + payload_len(4 LE) + signature(256) + xor(1)
python3 -c "
import struct, sys
sig = open('$TMPDIR/sig.bin','rb').read()
assert len(sig) == 256
header = b'MSIG'                             # magic 4 bytes
payload_len = struct.pack('<I', 256)        # payload_len = 256 (LE)
blob = header + payload_len + sig
xor = 0
for b in blob: xor ^= b
blob += bytes([xor])
assert len(blob) == 265
sys.stdout.buffer.write(blob)
" > "$TMPDIR/sig_blob.bin"

# 3. 追加: padding(0xFF 填充到 4K 对齐) + 签名段
# 输出文件 = 原 bin + pad + sig_blob
OUT="${BIN%.bin}-signed.bin"
cp "$BIN" "$OUT"
if [ "$PAD_LEN" -gt 0 ]; then
  python3 -c "import sys; sys.stdout.buffer.write(b'\\xff' * $PAD_LEN)" >> "$OUT"
fi
cat "$TMPDIR/sig_blob.bin" >> "$OUT"

OUT_LEN=$(wc -c < "$OUT" | tr -d ' ')
echo "signed: $OUT"
echo "  image_len: $IMAGE_LEN"
echo "  sig_offset: $SIG_OFF"
echo "  sig_blob: 265 bytes"
echo "  total: $OUT_LEN"
