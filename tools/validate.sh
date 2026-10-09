#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware|--sim]" >&2
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
    # 判定 + dynslot carve 提案复核;真机 meta_store_install.c 与本测试链接同一份
    # meta_install_model.c,carve 复核链同一份 meta_carve.c 分配器)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_install_model.c main/meta_install_model.c \
        main/meta_store_json.c main/meta_carve.c main/meta_md5.c \
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
    # dynslot 纯逻辑(dynslot-design §8:分配器/表物化黄金对拍(gen_esp32part.py
    # 产物 tests/fixtures)/迁移种子/store A-B 记录/hook 表裁决+失败矩阵;
    # bootloader hook 链接同一份源)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_md5.c main/meta_md5.c \
        -o "${test_dir}/test_meta_md5"
    "${test_dir}/test_meta_md5"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_carve.c main/meta_carve.c main/meta_md5.c \
        -o "${test_dir}/test_meta_carve"
    "${test_dir}/test_meta_carve"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_carve_store.c main/meta_carve_store.c main/meta_carve.c \
        main/meta_md5.c \
        -o "${test_dir}/test_meta_carve_store"
    "${test_dir}/test_meta_carve_store"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_carve_boot.c main/meta_carve_boot.c \
        main/meta_carve_store.c main/meta_carve.c main/meta_md5.c \
        -o "${test_dir}/test_meta_carve_boot"
    "${test_dir}/test_meta_carve_boot"
    # dynslot 设备侧胶水(meta_carve_flash.c 的 host 行为测试:RAM NOR 模型
    # AND 写语义 + 撕裂写注入,覆盖 §4.7 失败矩阵:全新/legacy 迁移黄金对拍
    # + L6 凭据搬移/记录重建表/store 死回安全表/提交轮转/扫描回填)
    # esp_flash_host_fixture.c: f909fc8/207dc1c 新增的设备 API(默认芯片指针 /
    # 缓存失效 / 只读区检查)的 host 桩;meta_store_host_fixture.c 提供 host_parts
    # 注册表(只读区检查的数据源)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain \
        tests/test_meta_carve_flash.c main/meta_carve_flash.c \
        tests/esp_stubs/esp_flash_host_fixture.c \
        tests/esp_stubs/meta_store_host_fixture.c \
        main/meta_carve_store.c main/meta_carve.c main/meta_md5.c \
        -o "${test_dir}/test_meta_carve_flash"
    "${test_dir}/test_meta_carve_flash"
    # M5 备份格式单元测试(header 验证 + serialize/deserialize roundtrip)
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_meta_backup.c main/meta_backup.c \
        -o "${test_dir}/test_meta_backup"
    "${test_dir}/test_meta_backup"
    # hook 接线静态门:carve 裁决必须先于 otadata 单次会话策略(B5),恢复路径
    # 擦写表扇区并清 otadata(host 测不到的 flash 副作用,源码事实门兜底)
    python3 tests/test_dynslot_hook_gate.py
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
    python3 tests/test_usb_web_e2e_contract.py
    python3 tests/test_mobile_webview_e2e_contract.py
    # 真机门禁(self-hosted/USB)不在云端 static CI 自动烧板:smoke.py 是
    # 协议客户端,run_browser_smoke.py 是前端模块 × 真机回归,usb_web_e2e.mjs 是
    # USB Web UI × Web Serial × 真机回归。这里只做语法门,
    # 避免这两个脚本提交后本身不可运行。
    python3 -m py_compile tools/realdevice/smoke.py tools/realdevice/run_smoke.py \
        tools/realdevice/run_browser_smoke.py
    # --check 需要真 node:$PATH 上的 node 可能是 Bun 包装器(不支持 --check,
    # 会把脚本当程序执行 —— 表现为脚本跑起来、以 exit 2 结束)。逐个候选用
    # 探测哪个二进制是真实的 Node,而不是无条件相信 command -v。
    local _ck_bin="" _ck
    # 判别标准是 --version 输出 v<数字>:真 node 是 v26.3.0,Bun 包装器直接报
    # "Missing script to execute" 退出。不用 --check 当探针 —— Bun 对语法完整
    # 的文件返回 0(它把脚本执行了),对坏文件返回非 0,两种情况都不可靠。
    for _ck in "${NODE_BIN:-}" /usr/local/bin/node "$(command -v node 2>/dev/null || true)"; do
        [[ -n "$_ck" && -x "$_ck" ]] || continue
        [[ "$($_ck --version 2>/dev/null)" == v[0-9]* ]] || continue
        _ck_bin="$_ck"
        break
    done
    if [[ -n "$_ck_bin" ]]; then
        echo "node --check: ${_ck_bin}"
        "$_ck_bin" --check tools/realdevice/browser_smoke.mjs
        "$_ck_bin" --check tools/realdevice/usb_web_e2e.mjs
        "$_ck_bin" --check tools/realdevice/usb_web_e2e_multi.mjs
        "$_ck_bin" --check tools/realdevice/mobile_webview_e2e.mjs
    else
        echo "WARNING: no real node found (only a Bun wrapper?); skipping harness syntax gate" >&2
    fi

    # 路径卫生门禁(docs/development/engineering/coding-conventions.md 的路径规则)。
    # 扫 git 跟踪文件里的本机绝对路径、钉死的兄弟仓库产物、设备身份(MAC/SSID/IPv4/
    # 序列号)与主机用户名。git ls-files 已天然排除 build/ attic/ logs/。
    python3 - <<'PATHCHECK'
import os, re, subprocess, sys
# 从 git 输出反推仓库根,不依赖 shell 变量传入。
files = subprocess.run(["git", "ls-files"], capture_output=True, text=True).stdout.split()
root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                      capture_output=True, text=True).stdout.strip()
# 扫描器不能自我命中:规则声明在 coding-conventions.md,实现在 validate.sh。
SELF = ("tools/validate.sh",
        "docs/development/engineering/coding-conventions.md",
        "docs/development/engineering/coding-conventions.zh_CN.md")
# 历史 Bug 取证记录里钉死产物名是证据本身(「g8fcce59 那份验签通过」),不能改;
# 约定约束的是运行时代码,不是事后报告。
HISTORICAL_PREFIXES = (
    "docs/BUGS.md", "docs/BUGS.zh_CN.md",
    "docs/assets/play563-appstore-download-reverse",
    "docs/assets/handoff-unsigned-rootcause",
)
# 配置文件本身:wrangler.toml 的 kv_namespaces.id 是部署契约(wrangler 二进制读取),
# 不是运行时路径,也不能用 env var 覆盖。真正的凭证 CF_API_TOKEN 已在 secrets。
CONFIG_FILES = ("wrangler.toml",)
# 测试向量里的标准 MD5 常数(空串/a/b/c/d/e 与分区表/JSON 样例),不是凭证:
#   d41d8cd98f00b204e9800998ecf8427e = md5("")
#   0cc175b9c0f1b6a831c399e269772661 = md5("a")
#   900150983cd24fb0d6963f7d28e17f72 = md5("b")
#   4a7d1ed414474e4033ac29ccb8653d9b = md5("c") (未见,先不收)
#   f96b697d7cb7938d525a2f31aaf161d0 = md5("d")
#   c3fcd3d76192e4007dfb496cca67e13b = md5("e")
#   01234567890123456789012345678901 = 16 字节测试填充
#   d98a2bec... f3f5d92358... 57edf4a2... d174ab98... = 分区表/JSON 样例 MD5
TEST_CONSTANT_SHA256_HEX = {
    "d41d8cd98f00b204e9800998ecf8427e",  # md5("")
    "0cc175b9c0f1b6a831c399e269772661",  # md5("a")
    "900150983cd24fb0d6963f7d28e17f72",  # md5("b")
    "f96b697d7cb7938d525a2f31aaf161d0",  # md5("d")
    "c3fcd3d76192e4007dfb496cca67e13b",  # md5("e")
    "01234567890123456789012345678901",  # 16 字节测试填充
    "d98a2bec71be5d7d03b53d17d4b98798",  # partitions.csv 样例 MD5
    "d174ab98d277d9f5a5611c2c9f419d9f",  # readFlash stub MD5 帧
    "f3f5d92358b9cb356add8d36006fac78",  # JSON 契约测试样例 sha
    "57edf4a22be3c955ac49da2e2107b67a",  # readFlash stub 校验 MD5
}
PATTERNS = [
    ("本机绝对路径", r"/Users/[A-Za-z0-9_.-]+"),
    ("钉死的兄弟仓库产物", r"pass-radar[^\n]{0,28}?g[0-9a-f]{5,}"),
    ("设备 MAC 地址", r"\b[0-9a-f]{2}(?::[0-9a-f]{2}){5}\b"),
    ("路由器 SSID", r"\bHundhaus\b"),
    ("本地 IPv4", r"\b(?:192\.168|10\.)\d{1,3}\.\d{1,3}\b"),
    ("本机序列号", r"usbmodem\d{4,}"),
    ("32-hex 凭证(Cloudflare account/KV id)", r"\b[0-9a-f]{32}\b"),
]
hits = []
for f in files:
    if f in SELF or f.startswith(HISTORICAL_PREFIXES) or f in CONFIG_FILES:
        continue
    try:
        with open(os.path.join(root, f), encoding="utf-8") as h:
            lines = h.read().split("\n")
    except (UnicodeDecodeError, OSError):
        continue
    for n, line in enumerate(lines, 1):
        for label, pat in PATTERNS:
            m = re.search(pat, line)
            if not m:
                continue
            val = m.group(0)
            if label.startswith("32-hex") and val in TEST_CONSTANT_SHA256_HEX:
                continue
            hits.append((f, n, label, val))
for f, n, label, val in hits:
    print("路径违例 %s:%d [%s] %s" % (f, n, label, val))
if hits:
    print("共 %d 处;规则见 coding-conventions.md「禁止硬编码本地路径」" % len(hits))
    sys.exit(1)
print("路径卫生:OK")
PATHCHECK
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
        # dynslot 池几何/分配器(手机提案 = 设备 meta_carve_place 同算法,§4.5 L4)
        "${node_bin}" tests/test_dynslot_pool.mjs
        "${node_bin}" tools/install-slot/test-slot-backup.mjs
        "${node_bin}" tools/install-slot/test-backup-data.mjs
        "${node_bin}" tools/install-slot/test-launcher-upgrade.mjs
        "${node_bin}" tools/install-slot/test-readflash-protocol.mjs
        # dynslot carve 记录编解码 / 安装与删除规划 / 视图模型 / mock 设备交互
        # (USB 安装页动态槽位设计:docs/assets/dynslot-usb-slot-page-design.md §11 T1-T6)
        "${node_bin}" tools/install-slot/test-dynslot-record.mjs
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
    if grep -Eq 'build[[:space:]]*[:=]?[[:space:]]*["'\'']?[0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html; then
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
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
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
    --sim)
        tools/sim/run-sim-test.sh
        ;;
    *)
        usage
        exit 2
        ;;
esac
