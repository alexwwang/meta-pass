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
        tests/test_meta_seq.c main/meta_seq.c \
        -o "${test_dir}/test_meta_seq"
    "${test_dir}/test_meta_seq"
    # 商店通道 analyze 响应的有界 JSON 提取器(纯逻辑)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_store_json.c main/meta_store_json.c \
        -o "${test_dir}/test_meta_store_json"
    "${test_dir}/test_meta_store_json"
    # LAN 安装通道纯逻辑(install offer 解析/本地几何复核/session/chunk/finalize
    # 判定;真机 meta_store_install.c 与本测试链接同一份 meta_install_model.c)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_install_model.c main/meta_install_model.c \
        main/meta_store_json.c \
        -o "${test_dir}/test_meta_install_model"
    "${test_dir}/test_meta_install_model"
    # 配网纯逻辑(WiFi 表单解析/断连原因文案/SSID 转义/DNS 门户报文;
    # 与真机链接同一份 meta_store_prov.c)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
        tests/test_meta_store_prov.c main/meta_store_prov.c \
        -o "${test_dir}/test_meta_store_prov"
    "${test_dir}/test_meta_store_prov"
    # 商店通道 ESP-IDF 模块 host 语法检查(桩头在 tests/esp_stubs,对齐 IDF 5.x
    # 签名;不链接,只验证类型/声明/警告级问题,真机构建仍由 --firmware 负责)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsyntax-only \
        -Itests/esp_stubs -Imain -DHOST_TEST \
        main/meta_store_net.c main/meta_store_install.c
    # 开机策略纯逻辑(单次会话模型规则引擎,bootloader hook 与宿主共享)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_boot_policy.c \
        -o "${test_dir}/test_meta_boot_policy"
    "${test_dir}/test_meta_boot_policy"
    # 深睡唤醒契约(面板唤醒恢复顺序 + bootloader hook 的 otadata 续期路径;
    # 上游 jiandanc/meta-pass be9ec2e0 的修复,设备实测:休眠唤醒回子固件亮屏)
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_display_wake_contract.py
    # 签名段格式解析测试(stub 化 RSA 验签,只测格式)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
        tests/test_meta_sign.c tests/esp_stubs/meta_sign_stub.c \
        -o "${test_dir}/test_meta_sign"
    "${test_dir}/test_meta_sign"
    # 槽位扫描状态机(BUG-21;真机同一份 meta_store.c,esp_image_verify 用
    # meta_image_verify_sim.c 复现 IDF 段表遍历与 4 字节对齐规则)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
        tests/test_meta_store_scan.c main/meta_store.c main/meta_slots.c \
        main/meta_name.c main/meta_image.c \
        tests/esp_stubs/meta_image_verify_sim.c \
        tests/esp_stubs/meta_store_host_fixture.c \
        tests/esp_stubs/meta_sign_stub.c \
        -o "${test_dir}/test_meta_store_scan"
    "${test_dir}/test_meta_store_scan"
    # 扫描静默校验门(r10.11/BUG-21;同上,IDF checkout 缺失时跳过源码事实检查)
    python3 tests/test_bug21_scan_silent.py
    python3 tests/test_verify_firmware.py
    # 浏览器侧(install-slot)模块与页面逻辑测试(Node ES module):
    local node_bin
    node_bin="$(command -v node || true)"
    if [[ -z "${node_bin}" && -x /usr/local/bin/node ]]; then
        node_bin=/usr/local/bin/node
    fi
    if [[ -n "${node_bin}" ]]; then
        "${node_bin}" tools/install-slot/test-extract.mjs
        "${node_bin}" tools/install-slot/test-store-analyze.mjs
        "${node_bin}" tests/worker_contract.mjs
        # 手机安装模块(设计文档 §4.2/§6):SHA-256 对拍、preflight 三道门、
        # 设备会话全流程与续传语义(mock 设备契约 = meta_store_install.c)。
        "${node_bin}" tests/test_phone_install.mjs
        "${node_bin}" tools/install-slot/test-slot-backup.mjs
        "${node_bin}" tools/install-slot/test-launcher-upgrade.mjs
        "${node_bin}" tools/install-slot/test-readflash-protocol.mjs
    else
        echo "WARN: node not found; skipping install-slot mjs tests" >&2
    fi
    rm -rf "${test_dir}"
    echo "Host tests: PASS"

    # 安装页版本占位符守护(网页服务纳入版本管理):源码必须且只能包含
    # __PAGE_VERSION__ 占位符,禁止误写死版本号(写死会让部署替换失效、
    # 线上/本地版本标识漂移)。CI 部署时替换为 git 短 SHA,本地 server.mjs
    # 替换为 dev-<git describe>。
    if ! grep -q '__PAGE_VERSION__' install-slot/install-slot.html; then
        echo "ERROR: install-slot.html 页面版本占位符 __PAGE_VERSION__ 丢失" >&2
        return 1
    fi
    if grep -Eq 'build [0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html; then
        echo "ERROR: install-slot.html 含写死的页面构建号(应使用 __PAGE_VERSION__ 占位符)" >&2
        return 1
    fi
    echo "Page version placeholder: PASS"
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
    # 单文件混合格式产物(与 tools/build-firmware.sh 同一契约:
    # bootable 本体 + 44B MPUPV2 指纹尾段;verify_firmware.py 强制校验)
    local version
    version="$(git -c safe.directory='*' -C "${repo_root}" describe --tags --match 'v[0-9]*' 2>/dev/null || echo v0.0.0-dev)"
    python3 - "${validation_build_dir}" "${version}" <<'PYEOF'
import hashlib
import struct
import sys
from pathlib import Path

d = Path(sys.argv[1])
app = (d / "FoloToy-AI-Passport.bin").read_bytes()
full = (d / "FoloToy-AI-Passport-full.bin").read_bytes()
body = full[: 0x10000 + len(app)]
assert body[0] == 0xE9 and body[0x8000:0x8000 + 2] == b"\xAA\x50", "head layout"
footer = b"MPUPV2\x00\x00" + struct.pack("<I", len(body)) + hashlib.sha256(body).digest()
assert len(footer) == 44
# 文件名必须匹配 verify_firmware.py 的 glob「meta-pass_v*.bin」(v 开头);
# 同时即发布工件名(CI 上传 / GitHub Release 附件同名)
(d / f"meta-pass_{sys.argv[2]}.bin").write_bytes(body + footer)
PYEOF
    python3 tools/verify_firmware.py "${validation_build_dir}"
    # 发布工件落盘:CI(firmware-checks.yml / build-firmware.yml)在 validate.sh
    # --firmware 之后上传 build/meta-pass_v*.bin(if-no-files-found: error),
    # release job 直接把它挂到 GitHub Release——必须在这里产出,否则 CI 断供。
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/meta-pass_${version}.bin" \
        "${repo_root}/build/meta-pass_${version}.bin"
    echo "Firmware build: PASS (hybrid single-file: build/meta-pass_${version}.bin)"
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
