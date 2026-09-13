#!/usr/bin/env python3
"""tools/signing/gen-pubkey.py —— 从私钥生成 C 数组,注入 metapass_hook.h。

手写公钥字节极易出错(手抄 294 字节必然抄错,验签全部失败)。
此脚本从 tools/signing/private.pem 读取真实 SubjectPublicKeyInfo DER,
重写 metapass_hook.h 中 MARKER 之间的字节数组。

用法: python3 tools/signing/gen-pubkey.py [private.pem]

meta_sign_pubkey.h 与 metapass_hook.h 必须由同一私钥生成,
validate.sh 会逐字节比对二者,不一致即失败。
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOOK = REPO / "main" / "metapass_hook.h"
DEFAULT_KEY = REPO / "tools" / "signing" / "private.pem"

MARK_BEGIN = "// MP_PUBKEY_BEGIN — 下方数组由 tools/signing/gen-pubkey.py 生成,勿手改"
MARK_END = "// MP_PUBKEY_END"


def der_from_key(key: Path) -> bytes:
    # openssl → DER (SubjectPublicKeyInfo)
    pub = subprocess.run(
        ["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"],
        check=True, capture_output=True,
    )
    return pub.stdout


def c_array(der: bytes, name: str = "mp_pubkey_der") -> str:
    lines = [f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(der), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in der[i:i + 12])
        lines.append(f"  {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    key = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_KEY
    if not key.exists():
        print(f"error: private key {key} not found", file=sys.stderr)
        return 1

    der = der_from_key(key)
    text = HOOK.read_text()

    pattern = rf"{re.escape(MARK_BEGIN)}.*?{re.escape(MARK_END)}"
    block = "\n".join([
        MARK_BEGIN,
        f"#define MP_PUBKEY_LEN {len(der)}",
        c_array(der),
        MARK_END,
    ])
    if not re.search(pattern, text, flags=re.DOTALL):
        print(f"error: markers not found in {HOOK}", file=sys.stderr)
        return 1
    HOOK.write_text(re.sub(pattern, block, text, flags=re.DOTALL))
    print(f"updated {HOOK.name}: {len(der)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
