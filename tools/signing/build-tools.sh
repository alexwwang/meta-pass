#!/bin/bash
# tools/signing/build-tools.sh —— 编译 Keychain 签名工具到 bin/(gitignore)。
# sign-firmware.sh 会在缺失时自动编译 keychain-sign;本脚本用于首次准备 keygen。
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/bin"
cc -O2 -Wall -Wextra -framework Security -framework CoreFoundation \
    "$DIR/keychain_keygen.c" -o "$DIR/bin/keychain-keygen"
cc -O2 -Wall -Wextra -framework Security -framework CoreFoundation \
    "$DIR/keychain_sign.c" -o "$DIR/bin/keychain-sign"
echo "built: bin/keychain-keygen bin/keychain-sign"
