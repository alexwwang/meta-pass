#!/usr/bin/env bash
# tools/signing/run-verify-tests.sh —— 固件验签集成测试(固化)
#
# 直接编译并运行固件的 main/meta_sign.c,调用固件真实的 meta_sign_verify()。
# 不定义 HOST_TEST,因此编译的是真实 mbedtls ECDSA 验签路径,不是 stub。
#
# 用法:
#   tools/signing/run-verify-tests.sh                    # 用默认测试镜像
#   tools/signing/run-verify-tests.sh <signed.bin>       # 指定签名镜像
#
# 前置: brew install mbedtls  (本机路径 /usr/local/opt/mbedtls)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SIGNED="${1:-$REPO_ROOT/../pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin}"
SRC="$SCRIPT_DIR/test_integration.c"
FIRMWARE_SRC="$REPO_ROOT/main/meta_sign.c"
BUILD_DIR="$REPO_ROOT/build/host-verify"
BIN="$BUILD_DIR/test_integration"

fail() { echo "  ✗ $1" >&2; exit 1; }
ok()   { echo "  ✓ $1"; }

echo "=========================================="
echo " meta-pass 固件验签集成测试"
echo "=========================================="

# [1/4] 依赖
echo ""
echo "[1/4] 依赖检查"
command -v cc >/dev/null 2>&1 || fail "未找到 cc"
command -v brew >/dev/null 2>&1 || fail "未找到 brew"
MBEDTLS="$(brew --prefix mbedtls 2>/dev/null)" || fail "mbedtls 未安装(brew install mbedtls)"
[ -d "$MBEDTLS/include" ] || fail "mbedtls include 缺失: $MBEDTLS/include"
[ -d "$MBEDTLS/lib" ] || fail "mbedtls lib 缺失: $MBEDTLS/lib"
ok "cc, mbedtls ($MBEDTLS)"
[ -f "$SIGNED" ] || fail "测试镜像不存在: $SIGNED"
ok "测试镜像: $SIGNED ($(wc -c <"$SIGNED" | tr -d ' ') bytes)"

# [2/4] 编译(image_len/tail_offset 由 C 程序自动解析,无需手写参数)
echo ""
echo "[2/4] 编译(真实 mbedtls 路径,未定义 HOST_TEST)"
mkdir -p "$BUILD_DIR"
if [ "$BIN" -nt "$SRC" ] && [ "$BIN" -nt "$FIRMWARE_SRC" ] && \
   [ "$BIN" -nt "$REPO_ROOT/main/meta_sign.h" ]; then
  ok "已是最新,跳过编译: $BIN"
else
  cc -O2 -Wall -Werror \
      -I"$REPO_ROOT/main" -I"$MBEDTLS/include" \
      "$SRC" "$FIRMWARE_SRC" \
      -L"$MBEDTLS/lib" -lmbedcrypto -lmbedx509 \
      -o "$BIN"
  ok "编译成功: $BIN"
fi

# [3/4] 运行
echo ""
echo "[3/4] 运行(调用固件 meta_sign_verify)"
echo "------------------------------------------"
"$BIN" "$SIGNED"
RC=$?
echo "------------------------------------------"

# [4/4] 结论
echo ""
echo "[4/4] 结论"
echo "=========================================="
if [ "$RC" -eq 0 ]; then
  echo "  PASS — 固件验签通过"
else
  echo "  FAIL — exit=$RC"
fi
echo "=========================================="
exit "$RC"
