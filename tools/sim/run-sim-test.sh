#!/usr/bin/env bash
# tools/sim/run-sim-test.sh —— 模拟器端到端测试(meta-pass 引导 + 按键)
#
# 用法:
#   tools/sim/run-sim-test.sh                     # 用默认固件镜像
#   tools/sim/run-sim-test.sh <meta-pass_*.bin>    # 指定镜像
#
# 前置:
#   - node >= 20(用 /usr/local/bin/node,仓库里的 node 是 bun shim)
#   - 本地 passport-sim 检出(提供 public/wasm/pkg/esp_emu_bg.wasm)
#     路径可用 PASSPORT_SIM_DIR 指定;默认是仓库的兄弟目录 ../passport-sim
#
# 设计:esp_emu_bg.wasm 是 3.3MB 第三方 QEMU 预编译产物,不进本仓库。
# 这里把它复制到 tools/sim/(已 gitignore),做法与
# tools/signing/run-verify-tests.sh 探测 Homebrew mbedtls 一致。
set -euo pipefail

fail() { echo "  ✗ $1" >&2; exit 1; }
ok()   { echo "  ✓ $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SIM_DIR="${PASSPORT_SIM_DIR:-$REPO_ROOT/../passport-sim}"

# 仓库里 node 在 PATH 上是 bun shim(见 passport-sim 同款问题),直接用 /usr/local/bin/node。
NODE_BIN="${NODE_BIN:-/usr/local/bin/node}"
[ -x "$NODE_BIN" ] || fail "未找到 node: $NODE_BIN (用 NODE_BIN 指定)"
NODE_MAJOR="$($NODE_BIN -p 'process.versions.node.split(".")[0]')"
[ "$NODE_MAJOR" -ge 20 ] || fail "需要 node >= 20(当前 $($NODE_BIN --version))"

IMAGE="${1:-$REPO_ROOT/build/meta-pass_v0.2.2-35-gaa25c52.bin}"

echo "=========================================="
echo " meta-pass 模拟器端到端测试"
echo "=========================================="

# [1/3] 依赖与固件镜像
echo ""
ok "node $($NODE_BIN --version) ($NODE_BIN)"
[ -d "$SIM_DIR/public/wasm/pkg" ] || \
  fail "未找到 passport-sim: $SIM_DIR/public/wasm/pkg (用 PASSPORT_SIM_DIR 指定)"
ok "passport-sim: $SIM_DIR"
[ -f "$IMAGE" ] || \
  fail "固件镜像不存在: $IMAGE (先跑 tools/validate.sh --firmware)"
ok "固件镜像: $(basename "$IMAGE") ($(wc -c <"$IMAGE" | tr -d ' ') bytes)"

# [2/3] 准备 QEMU wasm 运行时(外部产物,复制到 gitignore 的 tools/sim/)
echo ""
echo "[2/3] 准备 QEMU wasm 运行时"
WASM_SRC="$SIM_DIR/public/wasm/pkg/esp_emu_bg.wasm"
WASM_DST="$SCRIPT_DIR/esp_emu_bg.wasm"
[ -f "$WASM_SRC" ] || fail "缺 wasm: $WASM_SRC"
if [ "$WASM_DST" -nt "$WASM_SRC" ]; then
  ok "已是最新,跳过复制"
else
  cp "$WASM_SRC" "$WASM_DST"
  ok "esp_emu_bg.wasm ($(wc -c <"$WASM_DST" | tr -d ' ') bytes)"
fi

# [3/3] 运行测试
echo ""
echo "[3/3] 运行 (node --test)"
echo "------------------------------------------"
META_PASS_IMAGE="$IMAGE" "$NODE_BIN" --test --test-reporter=spec "$SCRIPT_DIR/metapass-boot.test.mjs"
RC=$?

echo "------------------------------------------"
echo ""
if [ "$RC" -eq 0 ]; then
  echo "  PASS — meta-pass 模拟器端到端测试通过"
else
  echo "  FAIL — exit=$RC"
fi
echo "=========================================="
exit "$RC"
