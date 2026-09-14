#!/usr/bin/env python3
"""tools/signing/gen-pubkey.py —— 从公钥生成 C 数组,注入固件头文件。

手写公钥字节极易出错(手抄 91 字节必然抄错,验签全部失败)。
此脚本从 tools/signing/public.pem 读取 SubjectPublicKeyInfo DER,
同步重写两处内嵌公钥:
  - main/meta_sign_pubkey.h   (启动器 meta_sign.c 验签用,整文件重写)
  - main/metapass_hook.h      (子固件 hook 验签用,MARKER 之间替换)
validate.sh 会逐字节比对二者,不一致即失败。

用法: python3 tools/signing/gen-pubkey.py [public.pem]

私钥托管在 macOS Keychain(见 keychain_keygen.c),本脚本只碰公钥。
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOOK = REPO / "main" / "metapass_hook.h"
PUBKEY_H = REPO / "main" / "meta_sign_pubkey.h"
DEFAULT_KEY = REPO / "tools" / "signing" / "public.pem"

MARK_BEGIN = "// MP_PUBKEY_BEGIN — 下方数组由 tools/signing/gen-pubkey.py 生成,勿手改"
MARK_END = "// MP_PUBKEY_END"


def der_from_pubkey(key: Path) -> bytes:
    # openssl → DER (SubjectPublicKeyInfo)
    pub = subprocess.run(
        ["openssl", "pkey", "-pubin", "-in", str(key), "-outform", "DER"],
        check=True, capture_output=True,
    )
    return pub.stdout


def c_array(der: bytes, name: str) -> str:
    lines = [f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(der), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in der[i : i + 12])
        lines.append(f"  {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    key = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_KEY
    if not key.exists():
        print(f"error: public key {key} not found", file=sys.stderr)
        return 1
    der = der_from_pubkey(key)

    # 1. meta_sign_pubkey.h 整文件重写
    PUBKEY_H.write_text(
        "// Auto-generated from tools/signing/public.pem — do not edit.\n"
        f"// ECDSA-P256 public key in SubjectPublicKeyInfo DER format ({len(der)} bytes).\n"
        f"#define METAPASS_SIGN_PUBKEY_LEN {len(der)}\n"
        + c_array(der, "metapass_sign_pubkey_der")
        + "\n"
    )
    print(f"updated {PUBKEY_H.name}: {len(der)} bytes")

    # 2. metapass_hook.h 标记块替换
    text = HOOK.read_text()
    pattern = rf"{re.escape(MARK_BEGIN)}.*?{re.escape(MARK_END)}"
    block = "\n".join([
        MARK_BEGIN,
        f"#define MP_PUBKEY_LEN {len(der)}",
        c_array(der, "mp_pubkey_der"),
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
