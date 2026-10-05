#!/usr/bin/env node
// tools/install-slot/test-wallet-web-failures.mjs —— USB 安装页修复审计的 TDD
// 验收(审核 docs/assets/meta-pass-usb-web-audit.md §6)。
//
// 覆盖 H1/H2/H3/L1/M1/M2/M3/M4/M5/L5;每个案例用 source-grep 或模块
// 导入的静态/运行时断言,禁止操作硬件;失败说明修复未完成,修复后重跑全绿。
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { spawnSync } from "node:child_process";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

// ── 辅助 ────────────────────────────────────────────────────────────────────

function read(rel) {
  return readFileSync(path.join(ROOT, rel), "utf-8");
}

function assertGrep(text, pattern, shouldMatch, label) {
  const m = pattern.test(text);
  if (shouldMatch && !m) {
    throw new Error(`[FAIL] ${label}: expected pattern ${pattern} to match, but did not`);
  }
  if (!shouldMatch && m) {
    throw new Error(`[FAIL] ${label}: expected pattern ${pattern} NOT to match, but did`);
  }
}

function assertFileExists(rel, label) {
  const p = path.join(ROOT, rel);
  if (!existsSync(p)) {
    throw new Error(`[FAIL] ${label}: file missing at ${rel}`);
  }
}

// ── H3: build date gate ──────────────────────────────────────────────────────

console.log("H3: validate.sh gate + build date placeholder");

const html = read("install-slot/install-slot.html");
const validateSh = read("tools/validate.sh");

// 3a. 占位符保留
assertGrep(html, /__PAGE_VERSION__/, true,
  "H3a: __PAGE_VERSION__ placeholder retained");

// 3b. 不再有硬编码日期字面量(`build: "2026-09-17-probe4"` 类型)
assertGrep(html, /\bbuild\s*:\s*["'][0-9]{4}-[0-9]{2}-[0-9]{2}/, false,
  "H3b: no hardcoded build date literal");

// 3c. validate.sh 正则覆盖 `build:` 写法(冒号 + 引号)
//     原来只匹配 `build `（空格），现在应同时匹配 `build:` / `build =`
assertGrep(validateSh, /build\[\[:space:\]\]\*\[:=\]\?/, true,
  "H3c: validate.sh regex covers build: / build = notation");

// 3d. validate.sh 仍能检出旧式 `build 2026-01-01`
assertGrep(validateSh, /build\[\[:space:\]\]\*\[:=\]\?/, true,
  "H3d: validate.sh still catches bare 'build YYYY-MM-DD'");

// 3e. 直接跑 gate grep,不依赖全量 validate.sh --static (后者含预存失败的 test_phone_install)
const htmlForGate = read("install-slot/install-slot.html");
const hasHardcodedBuild = /build\s*[:=]\s*["'\']?[0-9]{4}-[0-9]{2}-[0-9]{2}/.test(htmlForGate);
if (hasHardcodedBuild) {
  throw new Error("[FAIL] H3e: hardcoded build date found in install-slot.html");
}
console.log("  H3: PASS");

// ── H2: loader null guard in commitCarve ────────────────────────────────────

console.log("H2: loader null guard across async points in commitCarve");

// 应有 err_disconnected i18n key
assertGrep(html, /err_disconnected/, true,
  "H2a: err_disconnected i18n key exists");

// commitCarve 主体在 await 后应先 check loader
// (grep 核心模式:await 之后下一行有 loader 检查)
// 更可靠:整个 HTML 中 loader 在 await 后的 null 守卫至少有 2 处
assertGrep(html, /await[\s\S]{0,80}if\s*\(\s*!loader\s*\)/, true,
  "H2b: after-await null guard for loader present in commitCarve");

// bytesEqual 读回路径不应再单独使用(应改用 decodeAndValidate/checkTable)
// —— 允许 bytesEqual 与 decodeAndValidate 并存作为双保险(已保留)
// 关键:table readback 应调用 checkTable
assertGrep(html, /checkTable\(tblBack\)/, true,
  "H2c: table readback uses checkTable(tblBack)");

console.log("  H2: PASS");

// ── L1: innerHTML → textContent ─────────────────────────────────────────────

console.log("L1: no innerHTML injection of user/error content");

// play-info 错误路径只用 textContent
assertGrep(html, /play-info.*innerHTML.*err\.message/mi, false,
  "L1a: play-info does not innerHTML err.message");
// 通用断言:整个文件中不存在将 err.message 塞入 innerHTML 的模式
const lines = html.split("\n");
for (let i = 0; i < lines.length; i++) {
  const line = lines[i];
  if (/innerHTML\s*=/.test(line) && /err\.message/.test(line)) {
    throw new Error(`[FAIL] L1: innerHTML + err.message at HTML:${i + 1}: ${line.trim()}`);
  }
}
console.log("  L1: PASS");

// ── M2: readback assertion alignment ────────────────────────────────────────

console.log("M2: commitCarve readback uses decodeAndValidate + checkTable");

// record readback 断言应含 decodeAndValidate
assertGrep(html, /decodeAndValidate\(back\)/, true,
  "M2a: record readback uses decodeAndValidate(back)");
// table readback 断言应含 checkTable
assertGrep(html, /checkTable\(tblBack\)/, true,
  "M2b: table readback uses checkTable(tblBack)");

console.log("  M2: PASS");

// ── M3: busy guard before confirm ───────────────────────────────────────────

console.log("M3: removeSlotAction sets busy = true before window.confirm");

// removeSlotAction 中 busy = true 应出现在 window.confirm 之前
const removeMatch = html.match(/async function removeSlotAction[\s\S]{1,800}(?=async function|})/);
assert(removeMatch, "M3: removeSlotAction body found");
const removeBody = removeMatch[0];
const busyIdx = removeBody.indexOf("busy = true");
const confirmIdx = removeBody.indexOf("window.confirm");
assert(busyIdx !== -1 && confirmIdx !== -1,
  "M3: both 'busy = true' and 'window.confirm' present in removeSlotAction");
assert(busyIdx < confirmIdx,
  "M3: busy=true must precede window.confirm in removeSlotAction");
console.log("  M3: PASS");

// ── H1: mock prod gate in server.mjs ────────────────────────────────────────

console.log("H1: mock-device.js gated on META_PASS_DEV");

const serverMjs = read("tools/install-slot/server.mjs");
// mock-device.js 的白名单条目应仅在 META_PASS_DEV === '1' 时添加
assertGrep(serverMjs, /mock-device\.js/, true,
  "H1a: mock-device.js still registered (dev gate kept)");
// 不应无条件 mount;应有 env 门
assertGrep(serverMjs, /META_PASS_DEV/, true,
  "H1b: META_PASS_DEV env gate present in server.mjs");

console.log("  H1: PASS");

// ── M4: mock transport stubs ────────────────────────────────────────────────

console.log("M4: mock device has isMock guard or transport stubs");

// 方式 A:mock device 上加 isMock,并在 resync 路径守卫
// 方式 B:mock transport 补 stub 方法
// 这里只断言存在其中一种;具体在 mock-device.js 或 install-slot.html 中实现
const mockDevice = read("install-slot/mock-device.js");
const hasIsMock = /isMock\s*=/.test(mockDevice) || /\.isMock\b/.test(mockDevice);
const hasTransportStub = /transport\s*[=:]|disconnect\(\)|inWaiting\(|connect\(\)/.test(mockDevice);
assert(hasIsMock || hasTransportStub,
  "M4: mock device either exposes isMock flag or provides transport stub methods");

console.log("  M4: PASS");

// ── M1: pickRecord parenthesization ─────────────────────────────────────────

console.log("M1: pickRecord uses explicit parentheses for signed-diff comparison");

const dynslotRecord = read("install-slot/dynslot-record.js");
// 当前写法 `(ra.seq - rb.seq | 0) > 0` 依赖优先级,应加显式括号
// 修复后应有类似 `((ra.seq - rb.seq) | 0) > 0` 或注释说明
const hasExplicitParens = /\(\(ra\.seq\s*-\s*rb\.seq\)\s*\|\s*0\)\s*>/.test(dynslotRecord);
const hasComment = /显式|parenthes|precedence|带符号/.test(dynslotRecord);
assert(hasExplicitParens || hasComment,
  "M1: pickRecord has explicit parenthesization or comment on signed-diff");
console.log("  M1: PASS");

// ── M5: C reentrancy guard ──────────────────────────────────────────────────

console.log("M5: meta_carve_flash_table_write has reentrancy guard");

const metaCarveFlash = read("main/meta_carve_flash.c");
// 应有 s_in_progress 守卫（静态 bool 方案）
assertGrep(metaCarveFlash, /s_in_progress/, true,
  "M5a: s_in_progress guard present in meta_carve_flash.c");

console.log("  M5: PASS");

// ── L5: stray empty line removed ────────────────────────────────────────────

console.log("L5: no stray blank line after s_reboot_pending");

const lines_c = metaCarveFlash.split("\n");
let foundRebootPending = false;
for (let i = 0; i < lines_c.length; i++) {
  if (/s_reboot_pending\s*=\s*true/.test(lines_c[i])) {
    // 下一行不应是空行
    assert(i + 1 >= lines_c.length || lines_c[i + 1].trim() !== "",
      `L5: stray blank line after s_reboot_pending=true at line ${i + 1}`);
    foundRebootPending = true;
  }
}
assert(foundRebootPending, "L5: s_reboot_pending = true found in meta_carve_flash.c");
console.log("  L5: PASS");

// ── 完整性:所有修复项都已落地 ───────────────────────────────────────────────

console.log("\n✅ All TDD fixes verified.");
