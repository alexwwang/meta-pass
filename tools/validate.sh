#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml
    # 公钥一致性:hook(子固件验签用)与 meta_sign_pubkey.h(启动器验签用)
    # 必须由同一私钥生成,字节级一致,否则子固件签名永远验不过。
    python3 - <<'PY'
import re, sys
def arr(p):
    t = open(p).read()
    m = re.search(r'unsigned char \w+\[\] = \{(.*?)\};', t, re.S)
    if not m: sys.exit(f"error: {p} 缺少公钥数组")
    return bytes(int(x,16) for x in re.findall(r'0x[0-9a-f]{2}', m.group(1)))
a = arr('main/metapass_hook.h')
b = arr('main/meta_sign_pubkey.h')
assert a == b, f"error: 公钥不一致(hook={len(a)}B, meta_sign={len(b)}B)——运行 tools/signing/gen-pubkey.py"
assert len(a) == 91, f"error: 公钥长度异常 {len(a)}B"
print(f"公钥一致性: PASS ({len(a)} bytes)")
PY

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    # 纯逻辑 host tests:新增测试源时在此登记编译/运行(meta-pass 的 meta_* 模块)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_image.c main/meta_image.c \
        -o "${test_dir}/test_meta_image"
    "${test_dir}/test_meta_image"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_slots.c main/meta_slots.c \
        -o "${test_dir}/test_meta_slots"
    "${test_dir}/test_meta_slots"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_name.c main/meta_name.c \
        -o "${test_dir}/test_meta_name"
    "${test_dir}/test_meta_name"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_import.c main/meta_import.c \
        -o "${test_dir}/test_meta_import"
    "${test_dir}/test_meta_import"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_seq.c main/meta_seq.c \
        -o "${test_dir}/test_meta_seq"
    "${test_dir}/test_meta_seq"
    # 上传链路集成测试:桩化 ESP-IDF(tests/esp_stubs),主机编译真实 meta_net.c
    # 死代码剥离 flag 平台相关:macOS ld 用 -dead_strip,GNU ld 用 --gc-sections
    local gc_flag="-Wl,--gc-sections"
    if [ "$(uname)" = "Darwin" ]; then
        gc_flag="-Wl,-dead_strip"
    fi
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain -DHOST_TEST \
        tests/test_meta_net_upload.c \
        main/meta_import.c main/meta_image.c main/meta_name.c main/meta_slots.c main/meta_sign.c \
        -ffunction-sections -fdata-sections ${gc_flag} \
        -o "${test_dir}/test_meta_net_upload"
    "${test_dir}/test_meta_net_upload"
    # 签名段格式解析测试(stub 化 RSA 验签,只测格式)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
        tests/test_meta_sign.c tests/esp_stubs/meta_sign_stub.c \
        -o "${test_dir}/test_meta_sign"
    "${test_dir}/test_meta_sign"
    python3 tests/test_verify_firmware.py
    python3 tests/test_meta_net_contract.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    local version
    version="$(git -c safe.directory='*' -C "${repo_root}" describe --tags --match 'v[0-9]*' 2>/dev/null || echo v0.0.0-dev)"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/meta-pass_${version}.bin"
    echo "Firmware build: PASS (build/meta-pass_${version}.bin)"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
