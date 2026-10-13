#!/usr/bin/env node
// tools/realdevice/usb_web_e2e_backup_restore.mjs
//
// Real-device APP+DATA backup/restore E2E per
// docs/HANDOFF-realdevice-backup-restore-manual-data-confirmation.*.md.
//
// Stage map (handoff T-numbers):
//   t1      connect → local-install data-child → USB hard reset → LAN verify
//           → record APP+DATA carve manifest → snapshot initial DATA bytes
//           → hash device NVS (bytes never leave the device; hash only)
//   runtime python runtime driver: genuine device-side DATA write/read/verify
//           (writes known pattern P into the DATA partition)
//   backup  page backup (slot+DATA+NVS) → capture downloaded zip → validate
//           structure, manifest completeness, per-entry length + SHA-256,
//           single-group scope, DATA zip-entry == live DATA bytes
//   ── HARD GATE: stop here; human DATA confirmation required before any
//      destructive stage (handoff §0/§T2) ──
//   delete  page remove APP → USB hard reset → verify APP + owned DATA gone
//   restore page v2 restore from the archived zip → USB hard reset → verify
//           APP+DATA present, restored DATA bytes == archived DATA entry,
//           device NVS hash unchanged (restore must never write NVS)
//   verify  python runtime driver again: restored firmware DATA IO works
//   cleanup page remove restored APP → USB hard reset → baseline restored
//
// Usage (first run — non-destructive stages only):
//   node tools/realdevice/usb_web_e2e_backup_restore.mjs --real-device \
//     --ip <ip> --token <32hex> --serial-port <port> \
//     --image tests/realdevice/data-child/build/data_child_full.bin \
//     --stages t1,runtime,backup
// Usage (second run — only after explicit human confirmation):
//   node tools/realdevice/usb_web_e2e_backup_restore.mjs --real-device \
//     --ip <ip> --token <32hex> --serial-port <port> \
//     --stages delete,restore,verify,cleanup --rundir <dir-from-first-run>
//
// The Chrome profile with the one-time Web Serial grant is shared with
// usb_web_e2e_multi.mjs (usb-e2e-chrome-profile).

import { execFile, execFileSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { readDataArchiveZip } from "../../install-slot/data-byte-archive.js";
import { MANIFEST_NAME } from "../../install-slot/slot-backup.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const SERVER = path.join(ROOT, "tools", "install-slot", "server.mjs");
const LOGROOT = path.join(ROOT, "tools", "realdevice", "logs");
const RUNTIME_DRIVER = path.join(ROOT, "tools", "realdevice", "mobile_page_runtime_driver.py");

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const eq = a.indexOf("=");
    if (eq > 2) out[a.slice(2, eq)] = a.slice(eq + 1);
    else {
      const n = argv[i + 1];
      out[a.slice(2)] = n !== undefined && !n.startsWith("--") ? n : true;
    }
  }
  return out;
}

const argv = parseArgs(process.argv.slice(2));
if (!argv["real-device"]) {
  console.error("Refusing to touch hardware: pass --real-device explicitly.");
  process.exit(2);
}

const IP = argv.ip;
const TOKEN = String(argv.token || process.env.META_PASS_SESSION || "");
const PORT = Number(argv.port || process.env.META_PASS_PORT || 4191);
const SERIAL_PORT = String(argv["serial-port"] || process.env.USB_SERIAL_PORT || "");
const IMAGE = String(argv.image || process.env.USB_E2E_IMAGE || "");
const STAGES = String(argv.stages || "t1,runtime,backup").split(",").map((s) => s.trim());
const PLAY_ID_ARG = Number(argv["play-id"] || 0); // 0 = read from carve after install
const BROWSER_CHANNEL = String(argv["browser-channel"] || "chrome");
// 单一持久浏览器窗口:整个测试周期(含人工确认门禁等待)只启动一次
// Chrome,结束才关闭。不得为每个 stage 反复开关窗口(用户明确要求)。
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;
const RUNDIR = argv.rundir
  ? path.resolve(String(argv.rundir))
  : path.join(LOGROOT, "usb-backup-restore-" + new Date().toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z"));

if (!IP || !TOKEN || !/^[0-9a-f]{32}$/i.test(TOKEN)) {
  console.error("usage: --ip <device-ip> --token <32hex> --serial-port <port> [--image <bin>] [--stages ...] [--rundir <dir>]");
  process.exit(2);
}
if (!SERIAL_PORT) { console.error("error: --serial-port is required"); process.exit(2); }
if (STAGES.includes("t1") && (!IMAGE || !existsSync(path.resolve(IMAGE)))) {
  console.error("error: --image <merged data_child_full.bin> is required for stage t1");
  process.exit(2);
}
if (!STAGES.includes("t1") && !existsSync(RUNDIR)) {
  console.error(`error: --rundir ${RUNDIR} not found (run stage t1 first)`);
  process.exit(2);
}

mkdirSync(RUNDIR, { recursive: true });
mkdirSync(PROFILE, { recursive: true });
writeFileSync(
  path.join(RUNDIR, "command.txt"),
  process.argv.slice(2).map((arg) =>
    arg === TOKEN ? "<redacted-token>" : arg === IP ? "<redacted-ip>" :
    arg === SERIAL_PORT ? "<redacted-serial-port>" : arg
  ).join(" ") + "\n"
);

// ── Test state ────────────────────────────────────────────────────

const results = [];
const consoleLines = [];
const pageErrors = [];
let state = {};               // run state persisted across invocations
let baselineSlots = null;
let displayName = null;
let server = null;
let context = null;
let page = null;

const report = {
  test: "USB page APP+DATA backup/restore real-device E2E (handoff T0–T5)",
  startedAt: new Date().toISOString(),
  stages: STAGES,
  rundir: "redacted",
  verdict: "FAIL",
};

function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  console.log(`[${stage}] ${ok ? "PASS" : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}${detail ? " — " + detail : ""}`);
}
function writeJson(name, value) { writeFileSync(path.join(RUNDIR, name), JSON.stringify(value, null, 2) + "\n"); }
function writeText(name, value) { writeFileSync(path.join(RUNDIR, name), value); }
function writeBin(name, value) { writeFileSync(path.join(RUNDIR, name), Buffer.from(value)); }
function saveState() { writeJson("run-state.json", { ...state, token: "<redacted>" }); }
function loadState() { state = JSON.parse(readFileSync(path.join(RUNDIR, "run-state.json"), "utf8")); }

async function waitFor(fn, timeoutMs, label, intervalMs = 500) {
  const start = Date.now();
  for (;;) {
    try { const v = await fn(); if (v) return v; } catch {}
    if (Date.now() - start > timeoutMs) throw new Error(`timeout waiting for ${label}`);
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}

async function deviceCall(endpoint, options = {}) {
  const r = await fetch(DEVICE + endpoint, {
    ...options,
    headers: { "X-Meta-Session": TOKEN, ...(options.headers || {}) },
    signal: AbortSignal.timeout(10000),
  });
  const text = await r.text();
  let json = null;
  try { json = JSON.parse(text); } catch {}
  return { status: r.status, ok: r.ok, json, text };
}

async function readSlots() {
  const r = await deviceCall("/api/install/slots");
  if (!r.ok || !r.json) throw new Error(`read slots failed: HTTP ${r.status}`);
  return r.json;
}

async function readStatus() {
  const r = await deviceCall("/api/install/status");
  if (!r.ok || !r.json) throw new Error(`read status failed: HTTP ${r.status}`);
  return r.json;
}

function assertInstallerIdle(status, where) {
  // "idle" = 无在途安装事务。state="pairing"+active=true 只表示 LAN 服务在线
  // (真机 2026-10-12 定案,与 usb_web_e2e_multi.mjs 同一口径);
  // 在途判定看 session/offer/confirmed。
  if (status.protocol !== 1 || status.session || status.offer || status.confirmed) {
    throw new Error(`${where}: HTTP installer is not idle: ${JSON.stringify({
      protocol: status.protocol, session: status.session,
      offer: status.offer, confirmed: status.confirmed, state: status.state,
    })}`);
  }
}

async function usbHardReset() {
  return new Promise((resolve, reject) => {
    const py = `
import serial, time, sys
try:
    s = serial.Serial('${SERIAL_PORT}', 115200, timeout=2)
    time.sleep(0.5)
    s.dtr = False; s.rts = True; time.sleep(0.1)
    s.rts = False; s.dtr = True;  time.sleep(0.1)
    s.dtr = False
    time.sleep(12)
    data = s.read(32768)
    text = data.decode('utf-8', errors='replace')
    for line in text.split('\\n'):
        if any(k in line.lower() for k in ['carve', 'online', 'ip:', 'rst:']):
            print(line)
    s.close()
    sys.exit(0)
except Exception as e:
    print('reset failed:', e, file=sys.stderr)
    sys.exit(1)
`;
    execFile("python3", ["-c", py], { timeout: 25000 }, (err, stdout) => {
      if (err) return reject(new Error("USB hard reset failed: " + err.message));
      resolve(stdout);
    });
  });
}

async function waitLan(timeoutMs = 300000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const r = await fetch(DEVICE + "/", { signal: AbortSignal.timeout(3000) });
      if (r.ok) return true;
    } catch {}
    await new Promise((r) => setTimeout(r, 2000));
  }
  return false;
}

function waitServer() {
  return waitFor(async () => {
    try {
      const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) });
      return r.ok;
    } catch { return false; }
  }, 15000, "local USB server");
}

function startServer() {
  server = spawn(process.execPath, [SERVER], {
    cwd: ROOT,
    env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const serverLog = [];
  server.stdout.on("data", (b) => serverLog.push(b.toString()));
  server.stderr.on("data", (b) => serverLog.push(b.toString()));
  return server;
}

async function reviveDevice() {
  return new Promise((resolve) => {
    execFile("python3", ["-m", "esptool", "--chip", "esp32c3", "--port", SERIAL_PORT,
                         "--after", "hard_reset", "chip_id"],
             { timeout: 40000 }, (err) => resolve(!err));
  });
}

async function uiConnect() {
  const chipClass = await page.locator("#chip-status").getAttribute("class") || "";
  if (chipClass !== "ok") await page.locator("#btn-connect").click();
  for (let attempt = 1; ; attempt++) {
    try {
      const model = await waitFor(async () => {
        const m = await page.evaluate(() => ({
          chipClass: document.getElementById("chip-status")?.className || "",
          mode: window.__installSlotDebug?.slotModel?.mode || "",
        }));
        if (m.chipClass === "ok" && m.mode.startsWith("DYN_")) return m;
        return null;
      }, 120000, "DYN_SLOT connection");
      if (attempt > 1) record("CONN", `connected after ${attempt} attempts`, true);
      return model;
    } catch (e) {
      if (attempt >= 3) throw e;
      console.log(`connect attempt ${attempt} failed (${e.message}); reviving device and retrying`);
      await reviveDevice();
      await new Promise((r) => setTimeout(r, 10000));
      await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await page.locator("#btn-connect").click();
    }
  }
}

async function uiDisconnect() {
  try {
    await page.evaluate(() => {
      if (window.__installSlotDebug?.loader?.transport) {
        window.__installSlotDebug.loader.transport.disconnect();
      }
    });
  } catch {}
  await new Promise((r) => setTimeout(r, 1000));
}

async function uiInstallLocal(imagePath, nameArg) {
  await page.locator('[data-tab="local"]').click();
  await new Promise((r) => setTimeout(r, 400));
  await page.locator("#file-input").setInputFiles(imagePath);
  await waitFor(async () => (await page.locator("#file-info").getAttribute("class")) === "ok",
                30000, "local image validated");
  const fileInfo = ((await page.locator("#file-info").textContent()) || "").replace(/\s+/g, " ").trim();
  record("T1", "local image validated", true, fileInfo.slice(0, 160));

  await page.locator("#disp-name").fill(nameArg);
  await waitFor(async () => page.locator("#btn-install").isEnabled(), 15000, "Install enabled");
  const auto = page.locator('.slot-row.auto input[name="slot"]:not([disabled])');
  if (!(await auto.count())) throw new Error("dynslot Auto unavailable");
  await auto.first().click();
  await page.locator("#btn-install").click();

  const dialogOpened = await waitFor(async () =>
    page.evaluate(() => document.getElementById("data-size-dialog")?.open === true),
    15000, "data-size dialog").then(() => true).catch(() => false);
  if (dialogOpened) {
    await page.locator("#data-size-confirm").click();
    record("T1", "DATA sizes confirmed (defaults)", true);
  }
  try {
    await waitFor(async () => {
      const text = await page.locator("#install-status").textContent();
      return text && /done|success|完成|成功/i.test(text);
    }, 240000, "UI install completion");
  } catch (e) {
    await reviveDevice();
    throw e;
  }
  const installLog = (await page.locator("#log").textContent()) || "";
  if (!/read-back verified|writing slot|carve record committed|partition table materialized/i.test(installLog)) {
    throw new Error("install log lacks flash write / carve commit / DATA read-back evidence");
  }
  writeText("t1-install-log.txt", installLog);
  record("T1", "local image installed", true, `displayName=${nameArg}`);
  await uiDisconnect();
}

async function uiRemoveSlot(nameArg) {
  await page.evaluate(() => {
    window.__origConfirm = window.confirm;
    window.confirm = () => true;
  });
  const row = page.locator(".slot-row").filter({ hasText: nameArg }).first();
  if (!(await row.count())) {
    await page.evaluate(() => { window.confirm = window.__origConfirm; });
    throw new Error(`slot row "${nameArg}" not found in UI`);
  }
  await row.locator(".slot-remove").click();
  await waitFor(async () =>
    (await page.locator(".slot-row").filter({ hasText: nameArg }).count()) === 0,
    60000, `UI removal of "${nameArg}"`);
  await page.evaluate(() => { window.confirm = window.__origConfirm; });
  const removeLog = (await page.locator("#log").textContent()) || "";
  writeText("remove-log.txt", removeLog);
  record("T5", `slot "${nameArg}" removed via UI`, true);
  await uiDisconnect();
}

// Web Serial 链路会在任意读操作中途死亡(esptool-js read timeout / No
// serial data received;真机 2026-10-13 多 stage 随机复现)。统一自愈:
// 断页面 → esptool 硬复位(芯片出 stub 回 app)→ 重载页面 → 重连 → 重试。
async function withSerialRetry(label, fn, attempts = 3) {
  let lastErr = null;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      return await fn();
    } catch (e) {
      lastErr = e;
      if (attempt >= attempts) break;
      console.log(`${label} attempt ${attempt} failed (${e.message.split("\n")[0]}); reviving and retrying`);
      await uiDisconnect();
      await reviveDevice();
      await new Promise((r) => setTimeout(r, 12000));
      await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await uiConnect();
    }
  }
  throw new Error(`${label} failed after ${attempts} attempts: ${lastErr.message}`);
}

// Read a flash range in-page and return SHA-256 hex (bytes never leave the device).
async function flashSha256(offset, size) {
  return page.evaluate(async (off, len) => {
    const dbg = window.__installSlotDebug;
    const bytes = await dbg.readFlashChunked(off, len, () => {}, () => {});
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    return { sha256: [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join(""), length: bytes.length };
  }, offset, size);
}

// Snapshot DATA partition bytes into the evidence dir (full bytes — this is the
// test fixture's own data, not NVS), return sha256.
async function snapshotData(stageLabel, dataRecord) {
  const res = await page.evaluate(async (rec) => {
    const dbg = window.__installSlotDebug;
    const bytes = await dbg.readFlashChunked(rec.offset, rec.size, () => {}, () => {});
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    // chunked base64: spread-arg fromCharCode blows the argument limit ~64K,
    // and each chunk must be a multiple of 3 bytes or btoa pads mid-stream.
    let b64 = "";
    for (let p = 0; p < bytes.length; p += 0x6000) {
      b64 += btoa(String.fromCharCode(...bytes.subarray(p, p + 0x6000)));
    }
    return {
      sha256: [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join(""),
      length: bytes.length,
      b64,
    };
  }, dataRecord);
  writeBin(`${stageLabel}-data.bin`, Uint8Array.from(atob(res.b64), (c) => c.charCodeAt(0)));
  delete res.b64;
  writeJson(`${stageLabel}-data-sha256.json`, { ...res, offset: dataRecord.offset, size: dataRecord.size, label: dataRecord.label });
  return res.sha256;
}

// Locate the device NVS partition via the partition table and hash its bytes.
async function deviceNvsSha256() {
  return page.evaluate(async () => {
    const dbg = window.__installSlotDebug;
    const table = await dbg.readFlashChunked(0x8000, 0x1000, () => {}, () => {});
    // 布局:2B magic(0x50AA) 后接 32B 条目:type(1) subtype(1) offset(4,LE)
    // size(4,LE) label(16) flags(4);条目从偏移 2 起步(真机分区表实证)。
    for (let i = 2; i + 30 <= table.length; i += 32) {
      const type = table[i];
      if (type === 0xff) continue;          // erased / empty
      if (type === 0x00) break;             // end marker
      const u32le = (p) => (table[p + 3] << 24 | table[p + 2] << 16 | table[p + 1] << 8 | table[p]) >>> 0;
      const offset = u32le(i + 2);
      const size = u32le(i + 6);
      const labelLen = table.indexOf(0x00, i + 10);
      const label = new TextDecoder().decode(table.subarray(i + 10, labelLen >= i + 10 && labelLen < i + 26 ? labelLen : i + 26));
      if (label === "nvs") {
        const bytes = await dbg.readFlashChunked(offset, size, () => {}, () => {});
        const digest = await crypto.subtle.digest("SHA-256", bytes);
        return {
          offset, size,
          sha256: [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join(""),
        };
      }
    }
    return null;
  });
}

async function runRuntimeDriver(phase) {
  const evidence = path.join(RUNDIR, `runtime-${phase}.json`);
  await new Promise((resolve, reject) => {
    execFile("python3", [RUNTIME_DRIVER,
      "--action", "boot-test-and-return",
      "--slot", String(state.slotIndex),
      "--play-id", String(state.playId),
      "--slot-name", displayName,
      "--phase", phase,
      "--evidence-file", evidence,
      "--port", SERIAL_PORT,
    ], { timeout: 180000 }, (err, stdout, stderr) => {
      writeText(`runtime-${phase}.log`, (stdout || "") + (stderr || ""));
      if (err) return reject(new Error(`runtime driver (${phase}) failed: ${err.message}`));
      resolve();
    });
  });
  const ev = JSON.parse(readFileSync(evidence, "utf8"));
  record(phase.toUpperCase(), "runtime driver DATA IO", true,
    `evidence=runtime-${phase}.json keys=${Object.keys(ev).join(",")}`);
  return ev;
}

// ── Stages ────────────────────────────────────────────────────────

async function stageT1() {
  console.log("\n=== T1: install data-child with DATA ===");
  baselineSlots = await readSlots();
  writeJson("t0-baseline-slots.json", baselineSlots);
  record("T0", "baseline clean", baselineSlots.count === 0,
    `count=${baselineSlots.count} free=${baselineSlots.free}`);
  assertInstallerIdle(await readStatus(), "T0");

  displayName = `BK-E2E-${Date.now()}`;
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  await uiInstallLocal(path.resolve(IMAGE), displayName);

  const bootLog = await usbHardReset();
  writeText("t1-boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("device did not come back on LAN after install reset");
  const after = await readSlots();
  writeJson("t1-after-install-slots.json", after);
  const mine = after.slots.find((s) => s.name === displayName);
  if (!mine) throw new Error(`installed slot "${displayName}" not found after reset`);
  if (after.data.length !== 1) throw new Error(`expected exactly 1 DATA record, got ${after.data.length}`);
  record("T1", "slot + DATA persisted across reset", true,
    `slot=${mine.slot} @${mine.offset}+${mine.size} play_id=${mine.play_id}; DATA=${after.data[0].label}@${after.data[0].offset}+${after.data[0].size}`);

  state = {
    displayName,
    slotIndex: mine.slot,
    playId: PLAY_ID_ARG || mine.play_id,
    dataRecord: after.data[0],
    baselineFree: baselineSlots.free,
    installedFree: after.free,
  };
  saveState();

  // Reconnect after reset to snapshot initial DATA bytes + NVS hash.
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  const snap = await withSerialRetry("t1 snapshot", async () => {
    const initialSha = await snapshotData("t1-initial", state.dataRecord);
    const nvs = await deviceNvsSha256();
    return { initialSha, nvs };
  });
  const initialSha = snap.initialSha;
  const nvs = snap.nvs;
  state.initialDataSha256 = initialSha;
  state.nvsSha256Before = nvs?.sha256 || null;
  state.nvsOffset = nvs?.offset || null;
  saveState();
  record("T1", "initial DATA snapshot", true, `sha256=${initialSha.slice(0, 16)}… len=${state.dataRecord.size}`);
  record("T1", "device NVS hashed (pre-destructive)", true,
    nvs ? `offset=${nvs.offset} size=${nvs.size} sha256=${nvs.sha256.slice(0, 16)}…` : "no NVS partition found");
  await uiDisconnect();
}

async function stageRuntime() {
  console.log("\n=== T1/runtime: genuine device-side DATA write/read/verify ===");
  if (!state.playId) throw new Error("run-state.json missing playId");
  displayName = state.displayName;
  // esptool-js 会话会把设备 USB CDC 输入路径楔死(SLIP 残留 + WebUSB
  // claim),裸 \n 冲洗也救不回(真机 2026-10-13 六连冲失败定案)。
  // 先把页面导航走(释放 WebUSB claim),再 DTR/RTS 硬复位重启 USB 外设,
  // 运行器在干净通道上运行(重启后通道健康已实证)。
  await page.goto("about:blank");
  await new Promise((r) => setTimeout(r, 1500));
  const bootLog = await usbHardReset();
  writeText(`runtime-boot-log.txt`, bootLog);
  if (!(await waitLan())) throw new Error("LAN not up after runtime-prep reset");
  await runRuntimeDriver("pre");
  record("T1", "pattern P written and verified on device", true);
}

async function stageBackup() {
  console.log("\n=== T3: page backup → zip validation ===");
  displayName = state.displayName;
  // 备份读 ~260KB 大区域。921600 下真机反复"slot 读完 → 后续区域读 8/8
  // 超时"(重连亦复现),115200 走 esptool.py 同款停等路径为已验证的可靠
  // 配置(慢 ~8×,总时长仍可接受)——页面支持 ?baud=115200 显式降速。
  const SLOW = `${BASE}/?e2e=real&baud=115200`;
  await page.goto(SLOW, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();

  // Backup read can die mid-read (esptool-js read timeout — 真机多次定案),
  // leaving the chip in download/stub mode. Retry up to 3 attempts:
  // revive (hard reset) → reload page → reconnect.
  let zipBytes = null;
  for (let attempt = 1; attempt <= 3 && !zipBytes; attempt++) {
    try {
      // Automation substitution (recorded honestly): the OS-native save picker is
      // not drivable; force the page's anchor-download fallback and capture the
      // download event. Zip bytes are identical either way.
      await page.evaluate(() => { window.showSaveFilePicker = undefined; });
      for (let i = 0; i < 3; i++) {
        const checked = await page.locator(`#bk-slot${i}`).isChecked().catch(() => null);
        if (checked === null) continue;
        if (i === state.slotIndex) { if (!checked) await page.locator(`#bk-slot${i}`).check(); }
        else if (checked) await page.locator(`#bk-slot${i}`).uncheck();
      }
      await page.locator("#btn-backup").click();
      const outcome = await Promise.race([
        page.waitForEvent("download", { timeout: 420000 }).then((d) => ({ download: d })),
        page.waitForFunction(
          () => document.getElementById("backup-status")?.className === "error",
          null, { timeout: 420000 },
        ).then(() => ({ failed: true })),
      ]);
      if (outcome.failed) {
        const status = ((await page.locator("#backup-status").textContent()) || "").trim();
        throw new Error(`page backup failed: ${status}`);
      }
      const zipPath = path.join(RUNDIR, "backup.zip");
      await outcome.download.saveAs(zipPath);
      zipBytes = new Uint8Array(readFileSync(zipPath));
      record("T3", `backup zip captured (attempt ${attempt})`, true, `${zipBytes.length} bytes`);
    } catch (e) {
      const backupLog = (await page.locator("#log").textContent().catch(() => "")) || "";
      writeText(`t3-backup-attempt${attempt}-log.txt`, backupLog + `\n[harness] ${e.message}\n`);
      if (attempt >= 3) throw new Error(`backup failed after 3 attempts: ${e.message}`);
      console.log(`backup attempt ${attempt} failed (${e.message.split("\n")[0]}); reviving device and retrying`);
      await uiDisconnect();
      await reviveDevice();
      await new Promise((r) => setTimeout(r, 12000));
      await page.goto(SLOW, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await uiConnect();
    }
  }
  if (!zipBytes) throw new Error("backup produced no zip");

  // Live DATA bytes after backup — must equal the archive's DATA entry.
  const liveDataSha = await withSerialRetry("t3 live snapshot",
    () => snapshotData("t3-live", state.dataRecord));

  // Structural validation with the same parser the restore input uses.
  const entries = readDataArchiveZip(zipBytes);
  const manifestBytes = entries.get(MANIFEST_NAME);
  if (!manifestBytes) throw new Error("manifest.json missing from archive");
  const manifest = JSON.parse(new TextDecoder().decode(manifestBytes));
  writeJson("t3-manifest.json", manifest);
  if (manifest.manifest_version !== 2) throw new Error(`expected manifest_version 2, got ${manifest.manifest_version}`);
  if (manifest.complete !== true) throw new Error("archive marked incomplete");
  if (!Array.isArray(manifest.slots) || manifest.slots.length !== 1) {
    throw new Error(`archive must contain exactly one APP group, got ${manifest.slots?.length}`);
  }
  if (manifest.slots[0].play_id !== state.playId) {
    throw new Error(`archived play_id ${manifest.slots[0].play_id} != installed ${state.playId}`);
  }
  if (!Array.isArray(manifest.data) || manifest.data.length !== 1) {
    throw new Error(`archive must contain exactly one DATA record, got ${manifest.data?.length}`);
  }
  const dataEntry = manifest.data[0];
  if (dataEntry.play_id !== state.playId || dataEntry.label !== state.dataRecord.label) {
    throw new Error("archived DATA ownership does not match installed APP");
  }
  if (!manifest.nvs?.file || !manifest.nvs?.sha256) {
    throw new Error("archive lacks the NVS integrity entry");
  }
  record("T3", "manifest scope: one APP+DATA group + NVS integrity entry", true,
    `play_id=${manifest.slots[0].play_id} data=${dataEntry.label} nvs=${manifest.nvs.file}`);

  // Per-entry length + SHA-256 validation (payload bytes only hashed in memory;
  // NVS raw bytes are never written to evidence).
  const crypto = await import("node:crypto");
  const sha = (u8) => crypto.createHash("sha256").update(u8).digest("hex");
  const checked = [];
  for (const slot of manifest.slots) {
    for (const f of slot.files) {
      const bytes = entries.get(f.name);
      if (!bytes) throw new Error(`archive entry missing: ${f.name}`);
      if (bytes.length !== f.bytes) throw new Error(`${f.name}: ${bytes.length}B != manifest ${f.bytes}B`);
      const got = sha(bytes);
      if (got !== f.sha256) throw new Error(`${f.name}: sha256 mismatch`);
      checked.push(`${f.name} ${f.bytes}B ok`);
    }
  }
  const dataBytes = entries.get(dataEntry.file);
  if (!dataBytes || dataBytes.length !== dataEntry.bytes) {
    throw new Error(`DATA entry ${dataEntry.file}: length mismatch`);
  }
  if (sha(dataBytes) !== dataEntry.sha256) throw new Error("DATA entry sha256 mismatch");
  if (sha(dataBytes) !== liveDataSha) {
    throw new Error("archive DATA bytes != live DATA bytes at backup time");
  }
  checked.push(`${dataEntry.file} ${dataEntry.bytes}B == live DATA ok`);
  const nvsBytes = entries.get(manifest.nvs.file);
  if (!nvsBytes || nvsBytes.length !== manifest.nvs.bytes) throw new Error("NVS entry length mismatch");
  if (sha(nvsBytes) !== manifest.nvs.sha256) throw new Error("NVS entry sha256 mismatch");
  if (sha(nvsBytes) !== state.nvsSha256Before) {
    throw new Error("archived NVS != device NVS at backup time");
  }
  checked.push(`${manifest.nvs.file} ${manifest.nvs.bytes}B == device NVS ok`);
  writeText("t3-zip-validation.txt", checked.join("\n") + "\n");
  record("T3", "every entry length + SHA-256 verified", true, `${checked.length} entries`);
  record("T3", "archive DATA == live pattern-P DATA", true, `sha256=${liveDataSha.slice(0, 16)}…`);

  state.zipPath = path.join(RUNDIR, "backup.zip");
  state.zipDataSha256 = sha(dataBytes);
  saveState();
  const backupLog = (await page.locator("#log").textContent()) || "";
  writeText("t3-backup-log.txt", backupLog);
  await page.screenshot({ path: path.join(RUNDIR, "t3-backup-done.png"), fullPage: true });
  await uiDisconnect();
}

async function stageDelete() {
  console.log("\n=== T5a: remove APP, verify owned DATA removed ===");
  displayName = state.displayName;
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  await uiRemoveSlot(displayName);
  const bootLog = await usbHardReset();
  writeText("t5a-boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("device did not come back on LAN after remove reset");
  const after = await readSlots();
  writeJson("t5a-after-remove-slots.json", after);
  if (after.slots.some((s) => s.name === displayName)) throw new Error("APP still present after remove+reset");
  if (after.data.length !== 0) throw new Error(`owned DATA not removed: ${after.data.length} record(s) left`);
  record("T5", "APP + owned DATA removed", true, `count=${after.count} data=${after.data.length} free=${after.free}`);
  state.freeAfterDelete = after.free;
  saveState();
  await uiDisconnect();
}

async function stageRestore() {
  console.log("\n=== T4: v2 restore from archive ===");
  displayName = state.displayName;
  // 115200 停等路径下恢复总时长实测 >40min;921600 下 esptool-js 持续流式
  // 读回会死(esptool.py 同链路 233KB/s 零失败——问题在 Chrome WebSerial
  // 侧)。故 e2e 模式用 ?noverify 跳过页内读回,字节保真由 harness 分块
  // 慢速读回代验(512KB/新会话,备份期已实证可靠)。
  const FAST = `${BASE}/?e2e=real${state.skipReadbackVerify ? "&noverify" : ""}`;
  await page.goto(FAST, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();

  let committed = false;
  for (let attempt = 1; attempt <= 2 && !committed; attempt++) {
    try {
      await page.locator("#restore-input").setInputFiles(state.zipPath);
      await waitFor(async () => (await page.locator("#restore-info").getAttribute("class")) === "ok",
                    60000, "restore preflight (archive validated)");
      const info = ((await page.locator("#restore-info").textContent()) || "").replace(/\s+/g, " ").trim();
      record("T4", `restore preflight passed (attempt ${attempt})`, true, info.slice(0, 140));
      await page.screenshot({ path: path.join(RUNDIR, "t4-preflight.png"), fullPage: true });

      await page.locator("#btn-restore").click();
      await waitFor(async () =>
        (await page.locator("#restore-status").getAttribute("class")) === "ok",
        1500000, "UI restore completion");
      committed = true;
    } catch (e) {
      if (attempt >= 2) { await reviveDevice(); throw e; }
      console.log(`restore attempt ${attempt} failed (${e.message.split("\n")[0]}); reviving and retrying`);
      await uiDisconnect();
      await reviveDevice();
      await new Promise((r) => setTimeout(r, 12000));
      await page.goto(FAST, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await uiConnect();
    }
  }
  if (!committed) throw new Error("restore did not commit");
  const restoreLog = (await page.locator("#log").textContent()) || "";
  writeText("t4-restore-log.txt", restoreLog);
  if (!/Restore committed|read-back verified/i.test(restoreLog)) {
    throw new Error("restore log lacks commit / read-back evidence");
  }
  record("T4", "restore committed", true);
  await uiDisconnect();

  const bootLog = await usbHardReset();
  writeText("t4-boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("device did not come back on LAN after restore reset");
  const after = await readSlots();
  writeJson("t4-after-restore-slots.json", after);
  const mine = after.slots.find((s) => s.play_id === state.playId);
  if (!mine) throw new Error(`restored APP playId=${state.playId} not found`);
  if (after.data.length !== 1) throw new Error(`expected 1 DATA record after restore, got ${after.data.length}`);
  record("T4", "restored APP + DATA visible after reset", true,
    `slot=${mine.slot} @${mine.offset}+${mine.size} name=${mine.name}`);

  // Reconnect and verify byte fidelity + NVS preservation. In skip-verify
  // mode the page-side readback was skipped; fidelity/NVS verification runs
  // afterwards via the harness chunked verifier (512KB/fresh session).
  if (state.skipReadbackVerify) {
    record("T4", "page readback skipped (?noverify) — harness chunked verify follows", true);
    await page.goto("about:blank");
    saveState();
    return;
  }
  await page.goto(FAST, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  state.dataRecord = after.data[0];
  const check = await withSerialRetry("t4 restore verification", async () => {
    const restoredSha = await snapshotData("t4-restored", state.dataRecord);
    const nvs = await deviceNvsSha256();
    return { restoredSha, nvs };
  });
  const restoredSha = check.restoredSha;
  const nvs = check.nvs;
  if (restoredSha !== state.zipDataSha256) {
    throw new Error(`restored DATA sha256 ${restoredSha.slice(0, 16)} != archived ${state.zipDataSha256.slice(0, 16)}`);
  }
  record("T4", "restored DATA bytes == archived bytes (pattern P survived delete+restore)", true,
    `sha256=${restoredSha.slice(0, 16)}…`);
  state.nvsSha256After = nvs?.sha256 || null;
  saveState();
  const nvsPreserved = state.nvsSha256After === state.nvsSha256Before;
  record("T4", "device NVS untouched by restore", nvsPreserved,
    nvsPreserved
      ? `sha256 unchanged (${state.nvsSha256Before.slice(0, 16)}…)`
      : `BEFORE=${state.nvsSha256Before?.slice(0, 16)} AFTER=${state.nvsSha256After?.slice(0, 16)}`);
  await uiDisconnect();
}

async function stageVerify() {
  console.log("\n=== T4/runtime: restored firmware DATA IO ===");
  displayName = state.displayName;
  // 同 stageRuntime:restore 的 esptool-js 写会话楔死 CDC 输入,导航释放
  // WebUSB claim 后硬复位,再让运行器走干净通道。
  await page.goto("about:blank");
  await new Promise((r) => setTimeout(r, 1500));
  const bootLog = await usbHardReset();
  writeText("runtime-post-boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("LAN not up after verify-prep reset");
  const ev = await runRuntimeDriver("post");
  writeJson("runtime-post-summary.json", ev);
}

async function stageCleanup() {
  console.log("\n=== T5b: cleanup → baseline ===");
  displayName = state.displayName;
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  await uiRemoveSlot(displayName);
  const bootLog = await usbHardReset();
  writeText("t5b-boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("device did not come back on LAN after cleanup reset");
  const after = await readSlots();
  writeJson("t5b-final-slots.json", after);
  const restored = after.count === 0 && after.free === state.baselineFree;
  record("T5", "baseline restored", restored,
    `count=${after.count} free=${after.free} (baseline free=${state.baselineFree})`);
  await uiDisconnect();
}

// ── Main ──────────────────────────────────────────────────────────

async function main() {
  const metadata = {
    branch: process.env.GIT_BRANCH || null,
    commit: (() => { try { return execFileSync("git", ["rev-parse", "HEAD"], { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return null; } })(),
    node: process.version,
    stages: STAGES,
    rundir: "redacted",
    startedAt: report.startedAt,
  };
  writeJson("metadata.json", metadata);
  if (!STAGES.includes("t1")) loadState();

  startServer();
  await waitServer();
  record("E0", "local USB server ready", true, BASE);

  const browser = await chromium.launchPersistentContext(PROFILE, {
    headless: false,
    channel: BROWSER_CHANNEL,
    viewport: { width: 1280, height: 1000 },
    acceptDownloads: true,
  });
  record("E0", "single persistent Chrome window opened (whole-run lifecycle)", true);
  context = browser;
  page = await context.newPage();
  page.on("console", (msg) => consoleLines.push(`[${msg.type()}] ${msg.text()}`));
  page.on("pageerror", (err) => pageErrors.push(err.stack || err.message));

  for (const stage of STAGES) {
    // 人工 DATA 确认硬门禁(handoff §0/§T2):backup(只读)完成后、任何
    // 破坏性 stage 之前,无期限等待 rundir/human-gate.txt 出现。
    // 绝不超时自动放行;浏览器窗口保持打开,人工确认后继续同一会话。
    if (stage === "delete" && STAGES.includes("backup") && !existsSync(path.join(RUNDIR, "human-gate.txt"))) {
      record("T2", "BLOCKED_WAITING_FOR_HUMAN_DATA_CONFIRMATION", true,
        "create human-gate.txt in the rundir after confirming DATA read/write on the device");
      console.log("\n>>> T2 HUMAN GATE: please confirm on the device that the child firmware");
      console.log(">>> boots, reads pre-existing DATA, and persists an observable write.");
      console.log(`>>> Then:  touch ${path.join(RUNDIR, "human-gate.txt")}`);
      await waitFor(() => existsSync(path.join(RUNDIR, "human-gate.txt")), Number.POSITIVE_INFINITY, "human gate confirmation", 5000);
      record("T2", "PASS_HUMAN_CONFIRMED", true, "gate file observed; proceeding to destructive stages");
    }
    if (stage === "t1") await stageT1();
    else if (stage === "runtime") await stageRuntime();
    else if (stage === "backup") await stageBackup();
    else if (stage === "delete") await stageDelete();
    else if (stage === "restore") await stageRestore();
    else if (stage === "verify") await stageVerify();
    else if (stage === "cleanup") await stageCleanup();
    else throw new Error(`unknown stage: ${stage}`);
  }
  report.verdict = "PASS";
}

async function finish() {
  writeText("browser-console.log", consoleLines.join("\n") + "\n");
  writeText("page-errors.log", pageErrors.join("\n") + "\n");
  report.finishedAt = new Date().toISOString();
  report.durationSec = ((Date.parse(report.finishedAt) - Date.parse(report.startedAt)) / 1000);
  report.verdict = report.verdict === "PASS" && results.every((r) => r.ok) ? "PASS" : "FAIL";
  writeJson("report.json", report);
  const lines = [
    "# USB APP+DATA Backup/Restore Real-Device E2E",
    "",
    `- verdict: **${report.verdict}**`,
    `- stages: ${STAGES.join(", ")}`,
    `- device: redacted`,
    `- duration: ${report.durationSec.toFixed(1)}s`,
    "",
    "| stage | check | result | detail |",
    "|---|---|---|---|",
    ...results.map((r) => `| ${r.stage} | ${r.name} | ${r.ok ? "PASS" : "FAIL"} | ${String(r.detail || "").replaceAll("|", "\\|")} |`),
    "",
  ];
  writeText("report.md", lines.join("\n") + "\n");
  console.log(`\n=== ${report.verdict} === (${report.durationSec.toFixed(1)}s) rundir=${RUNDIR}`);
}

try {
  await main();
} catch (err) {
  console.error("Backup/restore E2E FAILED:", err.stack || err.message);
  report.verdict = "FAIL";
  process.exitCode = 1;
} finally {
  await finish();
  try { if (page) await uiDisconnect(); } catch {}
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
