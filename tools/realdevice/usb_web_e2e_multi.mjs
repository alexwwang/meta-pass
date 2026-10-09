#!/usr/bin/env node
// tools/realdevice/usb_web_e2e_multi.mjs
//
// Multi-round USB Web UI E2E:
//   R1 install Play A → USB hard reset → verify persistence
//   R2 install Play B → USB hard reset → verify coexistence + free-space delta
//   R3 remove Play A  → USB hard reset → verify Play B intact
//   R4 remove Play B  → USB hard reset → verify baseline fully restored
//
// This is a thin browser harness — same design principles as usb_web_e2e.mjs:
// - Never calls phone-install.js or /api/install write endpoints
// - The page performs install/remove via Web Serial; the runner only drives UI
// - Independent device API verification after each USB hard reset
// - Single Chrome session throughout (Web Serial authorized once)
// - USB hard reset via serial DTR/RTS toggle (not Chrome restart)
//
// Prerequisites:
//   1. Authorize Web Serial once:  node usb_web_e2e.mjs --authorize --port 4191
//   2. Device on LAN, installer idle
//   3. Local install-slot server running on PORT
//
// Usage:
//   node tools/realdevice/usb_web_e2e_multi.mjs --real-device \
//     --ip <device-ip> --token <32hex> --serial-port <serial-port> \
//     --play-a 1 --play-b 2 --port 4191
//
// Environment variables (fallbacks):
//   META_PASS_SESSION  — 32-hex session token
//   META_PASS_PORT     — local server port (default 4191)
//   USB_E2E_PLAY_A     — first play id (default "1")
//   USB_E2E_PLAY_B     — second play id (default "2")
//   USB_SERIAL_PORT    — serial port path (e.g. /dev/cu.usbmodemXXXX)

import { execFile, execFileSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { parseSlots } from "../../install-slot/phone-install.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const SERVER = path.join(ROOT, "tools", "install-slot", "server.mjs");
const LOGROOT = path.join(ROOT, "tools", "realdevice", "logs");

// ── Argument parsing ──────────────────────────────────────────────

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
  console.error("Usage: node usb_web_e2e_multi.mjs --real-device --ip <ip> --token <32hex> --serial-port <serial-port> [--play-a 1] [--play-b 2] [--port 4191]");
  process.exit(2);
}

const IP = argv.ip;
const TOKEN = String(argv.token || process.env.META_PASS_SESSION || "");
const PORT = Number(argv.port || process.env.META_PASS_PORT || 4191);
const SERIAL_PORT = String(argv["serial-port"] || process.env.USB_SERIAL_PORT || "");
const PLAY_A = String(argv["play-a"] || process.env.USB_E2E_PLAY_A || "1");
const PLAY_B = String(argv["play-b"] || process.env.USB_E2E_PLAY_B || "2");
const BROWSER_CHANNEL = String(argv["browser-channel"] || "chrome");
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const LOGDIR = path.join(LOGROOT, "usb-web-e2e-multi-" + new Date().toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z"));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;

if (!IP || !TOKEN || !/^[0-9a-f]{32}$/i.test(TOKEN)) {
  console.error("usage: node usb_web_e2e_multi.mjs --real-device --ip <device-ip> --token <32hex> --serial-port <serial-port> [--play-a 1] [--play-b 2]");
  process.exit(2);
}
if (!SERIAL_PORT) {
  console.error("error: --serial-port is required (e.g. <serial-port>)");
  process.exit(2);
}

mkdirSync(LOGDIR, { recursive: true });
mkdirSync(PROFILE, { recursive: true });
writeFileSync(
  path.join(LOGDIR, "command.txt"),
  process.argv.slice(2).map((arg) =>
    arg === TOKEN ? "<redacted-token>" :
    arg.startsWith("--token=") ? "--token=<redacted-token>" :
    arg === IP ? "<redacted-ip>" :
    arg.startsWith("--ip=") ? "--ip=<redacted-ip>" :
    arg === SERIAL_PORT ? "<redacted-serial-port>" :
    arg.startsWith("--serial-port=") ? "--serial-port=<redacted-serial-port>" :
    arg === PROFILE ? "<redacted-profile-path>" :
    arg.startsWith("--profile=") ? "--profile=<redacted-profile-path>" :
    arg
  ).join(" ") + "\n"
);

// ── Test state ────────────────────────────────────────────────────

const results = [];
let baselineSlots = null;
let displayNameA = null;
let displayNameB = null;
const consoleLines = [];
const pageErrors = [];
const report = {
  test: "Multi-round USB Web UI E2E: install→reset→install→reset→remove→reset→remove→reset",
  startedAt: new Date().toISOString(),
  device: "redacted",
  playA: Number(PLAY_A),
  playB: Number(PLAY_B),
  server: BASE,
  profile: "persistent Chrome profile (path redacted)",
  serialPort: "redacted",
  stages: results,
  verdict: "FAIL",
};

function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  const status = ok ? "PASS" : "FAIL";
  console.log(`[${stage}] ${status} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}${detail ? " — " + detail : ""}`);
}

function writeJson(name, value) {
  writeFileSync(path.join(LOGDIR, name), JSON.stringify(value, null, 2) + "\n");
}

function writeText(name, value) {
  writeFileSync(path.join(LOGDIR, name), String(value));
}

async function waitFor(fn, timeoutMs, label, intervalMs = 500) {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const v = await fn();
      if (v) return v;
    } catch (e) {
      lastError = e;
    }
    await new Promise((r) => setTimeout(r, intervalMs));
  }
  throw new Error(`timeout waiting for ${label}${lastError ? ": " + lastError.message : ""}`);
}

// ── Device API helpers ───────────────────────────────────────────

async function deviceCall(endpoint, options = {}) {
  const headers = { ...(options.headers || {}), "X-Meta-Session": TOKEN };
  const response = await fetch(DEVICE + endpoint, {
    ...options,
    headers,
    signal: AbortSignal.timeout(options.timeoutMs || 45000),
  });
  const text = await response.text();
  return { ok: response.ok, status: response.status, text };
}

async function readSlots() {
  const r = await deviceCall("/api/install/slots");
  if (!r.ok) throw new Error(`GET /api/install/slots HTTP ${r.status}: ${r.text}`);
  const parsed = parseSlots(r.text);
  if (!parsed) throw new Error(`invalid /api/install/slots response: ${r.text}`);
  return parsed;
}

async function readStatus() {
  const r = await deviceCall("/api/install/status");
  if (!r.ok) throw new Error(`GET /api/install/status HTTP ${r.status}: ${r.text}`);
  try { return JSON.parse(r.text); }
  catch { throw new Error(`invalid /api/install/status JSON: ${r.text}`); }
}

function assertInstallerIdle(status, where) {
  if (status.protocol !== 1 || status.active || status.session) {
    throw new Error(`HTTP installer is not idle at ${where}: ${JSON.stringify({
      protocol: status.protocol, active: status.active, session: status.session, state: status.state,
    })}`);
  }
  record(where, "HTTP installer idle / USB path isolated", true,
    `protocol=${status.protocol}; state=${status.state}; active/session=false`);
}

function stableSlots(v) {
  return {
    count: v.count,
    free: v.free,
    protocolVersion: v.protocolVersion,
    slots: v.slots.map((s) => ({
      slot: s.slot, state: s.state, name: s.name, size: s.size, len: s.len,
      limit: s.limit, offset: s.offset, kind: s.kind, arc: s.arc,
    })),
    data: v.data.map((d) => ({
      offset: d.offset, size: d.size, state: d.state, play_id: d.play_id, label: d.label,
    })),
  };
}

function sameSlotSet(a, b) {
  return JSON.stringify(stableSlots(a)) === JSON.stringify(stableSlots(b));
}

// ── USB hard reset via serial DTR/RTS ────────────────────────────
//
// Toggles DTR/RTS to trigger USB_UART_CHIP_RESET, then waits for the
// device to boot back into the app and rejoin LAN. This is equivalent
// to a user pressing the reset button or replugging USB.
//
// Must be called AFTER the Web Serial transport is disconnected
// (Chrome releases the port so Python can open it).

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

// ── Server lifecycle ─────────────────────────────────────────────

function waitServer() {
  return waitFor(async () => {
    try {
      const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) });
      return r.ok;
    } catch {
      return false;
    }
  }, 15000, "local USB server");
}

let server = null;
let context = null;
let page = null;

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

// ── UI helpers (all via Playwright page) ─────────────────────────

async function uiConnect() {
  // Click Connect if not already connected
  const chipClass = await page.locator("#chip-status").getAttribute("class") || "";
  if (chipClass !== "ok") {
    await page.locator("#btn-connect").click();
  }
  // Wait for DYN_SLOT mode (not LEGACY_FALLBACK)
  const model = await waitFor(async () => {
    const m = await page.evaluate(() => ({
      chip: document.getElementById("chip-status")?.textContent || "",
      chipClass: document.getElementById("chip-status")?.className || "",
      mode: window.__installSlotDebug?.slotModel?.mode || "",
      seq: window.__installSlotDebug?.slotModel?.seq,
      carve: window.__installSlotDebug?.slotModel?.carve?.slots?.length,
    }));
    if (m.chipClass === "ok" && m.mode.startsWith("DYN_")) return m;
    return null;
  }, 120000, "DYN_SLOT connection");
  return model;
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

async function uiInstallPlay(playId, displayName) {
  // Community play tab
  await page.locator('[data-tab="community"]').click();
  await new Promise((r) => setTimeout(r, 500));

  // Enter play URL and fetch
  await page.locator("#play-url").fill(`http://localhost:${PORT}/api/play?id=${playId}`);
  await page.locator("#btn-fetch-play").click();
  await waitFor(async () => {
    const text = await page.locator("#play-info").textContent();
    return text && /SHA-256/i.test(text) && /Size/i.test(text);
  }, 60000, `Play #${playId} metadata`);

  const playInfo = (await page.locator("#play-info").textContent() || "").replace(/\s+/g, " ").trim();
  record("R", `Play #${playId} fetched`, true, playInfo.slice(0, 180));

  // Set display name
  await page.locator("#disp-name").fill(displayName);

  // Wait for Install button enabled
  await waitFor(async () => page.locator("#btn-install").isEnabled(), 15000, "Install enabled");

  // Select auto target
  const auto = page.locator('.slot-row.auto input[name="slot"]:not([disabled])');
  if (!(await auto.count())) {
    throw new Error("dynslot Auto unavailable — no space for this image");
  }
  await auto.first().click();
  const checked = await page.locator('input[name="slot"]:checked').count();
  if (checked !== 1) throw new Error("UI did not select exactly one install target");

  const targetText = await page.locator('input[name="slot"]:checked').evaluate(
    (el) => el.closest(".slot-row")?.textContent?.replace(/\s+/g, " ").trim() || ""
  );
  record("R", `install target selected`, true, targetText.slice(0, 100));

  // Click install
  await page.locator("#btn-install").click();

  // Wait for completion
  await waitFor(async () => {
    const text = await page.locator("#install-status").textContent();
    return text && /done|success|完成|成功/i.test(text);
  }, 240000, "UI install completion");

  const installLog = (await page.locator("#log").textContent()) || "";
  if (!/sha-?256/i.test(installLog)) throw new Error("install log lacks SHA-256 evidence");
  if (!/writing slot|carve record committed|partition table materialized/i.test(installLog)) {
    throw new Error("install log lacks flash write / carve commit evidence");
  }
  record("R", `Play #${playId} installed`, true, `displayName=${displayName}`);

  return installLog;
}

async function uiRemoveSlot(displayName) {
  // Override window.confirm to auto-accept
  await page.evaluate(() => {
    window.__origConfirm = window.confirm;
    window.confirm = () => true;
  });

  const row = page.locator(".slot-row").filter({ hasText: displayName }).first();
  if (!(await row.count())) {
    await page.evaluate(() => { window.confirm = window.__origConfirm; });
    throw new Error(`slot row "${displayName}" not found in UI`);
  }

  // Click the .slot-remove button in that row
  await row.locator(".slot-remove").click();

  // Wait for the row to disappear (remove completed)
  await waitFor(async () => {
    const count = await page.locator(".slot-row").filter({ hasText: displayName }).count();
    return count === 0;
  }, 60000, `UI removal of "${displayName}"`);

  // Restore confirm
  await page.evaluate(() => { window.confirm = window.__origConfirm; });

  const removeLog = (await page.locator("#log").textContent()) || "";
  record("R", `slot "${displayName}" removed`, true);
  return removeLog;
}

// ── Round execution ───────────────────────────────────────────────

async function runInstallRound(roundLabel, playId, displayName, baselineSlots) {
  console.log(`\n=== ${roundLabel}: install Play #${playId} ===`);

  // Reload page for fresh state
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });

  // Connect
  const model = await uiConnect();
  await page.screenshot({ path: path.join(LOGDIR, `${roundLabel}-connect.png`), fullPage: true });
  record(roundLabel, "connected", true, `mode=${model.mode} seq=${model.seq} carve=${model.carve}`);

  // Install
  const installLog = await uiInstallPlay(playId, displayName);
  writeText(`${roundLabel}-install-log.txt`, installLog);
  await page.screenshot({ path: path.join(LOGDIR, `${roundLabel}-installed.png`), fullPage: true });

  // Disconnect + USB hard reset
  await uiDisconnect();
  const bootLog = await usbHardReset();
  writeText(`${roundLabel}-boot-log.txt`, bootLog);
  record(roundLabel, "USB hard reset done", true, bootLog.trim().slice(0, 240));

  // Wait for LAN recovery
  const lanUp = await waitLan(300000);
  if (!lanUp) throw new Error("LAN not up after reset");
  record(roundLabel, "LAN recovered", true);

  // Verify via device API
  const afterSlots = await readSlots();
  writeJson(`${roundLabel}-after-reset-slots.json`, stableSlots(afterSlots));
  const afterStatus = await readStatus();
  writeJson(`${roundLabel}-after-reset-status.json`, afterStatus);
  assertInstallerIdle(afterStatus, roundLabel);

  const newSlots = afterSlots.slots.filter((s) =>
    !baselineSlots.slots.some((b) => b.offset === s.offset && b.size === s.size)
  );
  const thisSlot = newSlots.find((s) => s.name === displayName);
  if (!thisSlot) throw new Error(`device API: slot "${displayName}" not found after reset`);
  if (thisSlot.state !== "valid") throw new Error(`slot state=${thisSlot.state}, expected valid`);

  record(roundLabel, "slot persisted across reset", true,
    `slot=${thisSlot.slot} @0x${thisSlot.offset.toString(16)} size=${thisSlot.size} name=${thisSlot.name}`);

  return { slots: afterSlots, installed: thisSlot };
}

async function runRemoveRound(roundLabel, displayName, baselineSlots) {
  console.log(`\n=== ${roundLabel}: remove "${displayName}" ===`);

  // Reload page for fresh state
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });

  // Connect
  const model = await uiConnect();
  await page.screenshot({ path: path.join(LOGDIR, `${roundLabel}-connect.png`), fullPage: true });
  record(roundLabel, "connected", true, `mode=${model.mode} seq=${model.seq} carve=${model.carve}`);

  // Remove
  const removeLog = await uiRemoveSlot(displayName);
  writeText(`${roundLabel}-remove-log.txt`, removeLog);
  await page.screenshot({ path: path.join(LOGDIR, `${roundLabel}-removed.png`), fullPage: true });

  // Disconnect + USB hard reset
  await uiDisconnect();
  const bootLog = await usbHardReset();
  writeText(`${roundLabel}-boot-log.txt`, bootLog);
  record(roundLabel, "USB hard reset done", true, bootLog.trim().slice(0, 240));

  // Wait for LAN recovery
  const lanUp = await waitLan(300000);
  if (!lanUp) throw new Error("LAN not up after reset");
  record(roundLabel, "LAN recovered", true);

  // Verify via device API
  const afterSlots = await readSlots();
  writeJson(`${roundLabel}-after-reset-slots.json`, stableSlots(afterSlots));
  const afterStatus = await readStatus();
  writeJson(`${roundLabel}-after-reset-status.json`, afterStatus);
  assertInstallerIdle(afterStatus, roundLabel);

  const stillThere = afterSlots.slots.some((s) => s.name === displayName);
  if (stillThere) throw new Error(`slot "${displayName}" still present after remove + reset`);

  record(roundLabel, "slot removed and persisted", true,
    `count=${afterSlots.count} free=${afterSlots.free}`);

  return { slots: afterSlots };
}

// ── Main ─────────────────────────────────────────────────────────

async function main() {
  const metadata = {
    branch: process.env.GIT_BRANCH || null,
    commit: (() => { try { return execFileSync("git", ["rev-parse", "HEAD"], { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return null; } })(),
    node: process.version,
    browserChannel: BROWSER_CHANNEL,
    device: "redacted",
    playA: Number(PLAY_A),
    playB: Number(PLAY_B),
    server: BASE,
    serialPort: "redacted",
    startedAt: report.startedAt,
  };
  writeJson("metadata.json", metadata);

  // E0: environment
  record("E0", "real-device flag", true);
  if (!existsSync(SERVER)) throw new Error(`missing server: ${SERVER}`);
  record("E0", "USB server source exists", true, SERVER);

  startServer();
  await waitServer();
  record("E0", "local USB server ready", true, BASE);

  const browser = await chromium.launchPersistentContext(PROFILE, {
    headless: false,
    channel: BROWSER_CHANNEL,
    viewport: { width: 1280, height: 1000 },
    acceptDownloads: true,
  });
  context = browser;
  page = await context.newPage();
  page.on("console", (msg) => consoleLines.push(`[${msg.type()}] ${msg.text()}`));
  page.on("pageerror", (err) => pageErrors.push(err.stack || err.message));

  // E1: baseline (before any USB connection — LAN only)
  baselineSlots = await readSlots();
  writeJson("baseline-slots.json", stableSlots(baselineSlots));
  const baselineStatus = await readStatus();
  writeJson("baseline-status.json", baselineStatus);
  assertInstallerIdle(baselineStatus, "E1");
  record("E1", "device baseline readable", true,
    `${baselineSlots.count} slots; free=${baselineSlots.free}`);

  displayNameA = `USB-E2E-MULTI-A-${Date.now()}`;
  displayNameB = `USB-E2E-MULTI-B-${Date.now()}`;

  // R1: install Play A
  const r1 = await runInstallRound("R1", PLAY_A, displayNameA, baselineSlots);
  if (r1.slots.free >= baselineSlots.free) {
    throw new Error(`R1 install did not reduce free space: ${baselineSlots.free} → ${r1.slots.free}`);
  }
  record("R1", "free space decreased", true,
    `free: ${baselineSlots.free} → ${r1.slots.free} (delta ${baselineSlots.free - r1.slots.free})`);

  // R2: install Play B (coexistence)
  const r2 = await runInstallRound("R2", PLAY_B, displayNameB, r1.slots);
  const slotAStillThere = r2.slots.slots.some((s) => s.name === displayNameA);
  if (!slotAStillThere) throw new Error("Play A slot disappeared after R2 install");
  record("R2", "Play A + Play B coexist", true,
    `slots=${r2.slots.count} (${displayNameA} + ${displayNameB})`);
  if (r2.slots.free >= r1.slots.free) {
    throw new Error(`R2 install did not further reduce free space: ${r1.slots.free} → ${r2.slots.free}`);
  }
  record("R2", "free space decreased further", true,
    `free: ${r1.slots.free} → ${r2.slots.free} (delta ${r1.slots.free - r2.slots.free})`);

  // R3: remove Play A (verify Play B intact)
  const r3 = await runRemoveRound("R3", displayNameA, r2.slots);
  const slotBStillThere = r3.slots.slots.some((s) => s.name === displayNameB);
  if (!slotBStillThere) throw new Error("Play B slot disappeared after R3 remove");
  record("R3", "Play B intact after Play A removal", true);

  // R4: remove Play B (verify baseline restored)
  const r4 = await runRemoveRound("R4", displayNameB, r3.slots);
  const baselineRestored = sameSlotSet(baselineSlots, r4.slots);
  if (!baselineRestored) throw new Error("device state not restored to baseline");
  record("R4", "baseline fully restored", true,
    `count=${r4.slots.count} free=${r4.slots.free}`);

  report.verdict = "PASS";
}

// ── Finish + cleanup ─────────────────────────────────────────────

async function finish() {
  writeText("browser-console.log", consoleLines.join("\n") + "\n");
  writeText("page-errors.log", pageErrors.join("\n") + "\n");
  report.finishedAt = new Date().toISOString();
  report.durationSec = ((Date.parse(report.finishedAt) - Date.parse(report.startedAt)) / 1000);
  report.verdict = report.verdict === "PASS" && results.every((r) => r.ok) ? "PASS" : "FAIL";
  writeJson("report.json", report);

  const lines = [
    "# Multi-round USB Web UI Real-Device E2E",
    "",
    `- verdict: **${report.verdict}**`,
    "- device: redacted",
    `- play A: ${PLAY_A}`,
    `- play B: ${PLAY_B}`,
    "- serial port: redacted",
    `- duration: ${report.durationSec.toFixed(1)}s`,
    "",
    "| stage | check | result | detail |",
    "|---|---|---|---|",
    ...results.map((r) => `| ${r.stage} | ${r.name} | ${r.ok ? "PASS" : "FAIL"} | ${String(r.detail || "").replaceAll("|", "\\|")} |`),
    "",
    "## Coverage",
    "",
    "- Multi-round install→reset→install→reset→remove→reset→remove→reset: " + report.verdict,
    "- USB Web Serial (single Chrome session, no restart): " + report.verdict,
    "- USB hard reset via DTR/RTS toggle: " + report.verdict,
    "- Carve persistence across reboots: " + report.verdict,
    "- Multi-slot coexistence: " + report.verdict,
    "- Selective remove (remove A, B intact): " + report.verdict,
    "- Baseline restore: " + report.verdict,
    "",
    "## Not tested",
    "",
    "- Physical power loss (USB reset ≠ unplugging)",
    "- DATA slots (covered by browser_smoke.mjs)",
    "- Direct phone-install.js path (not used)",
  ];
  writeText("report.md", lines.join("\n") + "\n");

  console.log(`\n=== ${report.verdict} === (${report.durationSec.toFixed(1)}s)`);
}

process.on("SIGINT", async () => {
  process.exitCode = 130;
});

try {
  await main();
} catch (err) {
  console.error("Multi-round USB E2E FAILED:", err.stack || err.message);
  report.verdict = "FAIL";
  process.exitCode = 1;
  // Best-effort rollback only for uniquely identifiable slots created by this run.
  // Never delete a baseline slot or an ambiguously owned slot.
  try {
    if (baselineSlots) {
      let current = await readSlots();
      const baselineKeys = new Set(baselineSlots.slots.map((s) => `${s.offset}:${s.size}`));
      const ownedNames = new Set([displayNameA, displayNameB].filter(Boolean));
      let candidates = current.slots.filter((s) =>
        !baselineKeys.has(`${s.offset}:${s.size}`) && ownedNames.has(s.name));
      if (candidates.length > 2) throw new Error(`cleanup ownership ambiguous: ${candidates.length} candidates`);
      for (const candidate of [...candidates].sort((a, b) => b.offset - a.offset)) {
        current = await readSlots();
        const live = current.slots.find((s) =>
          s.offset === candidate.offset && s.size === candidate.size && s.name === candidate.name);
        if (!live) continue;
        const removed = await deviceCall("/api/install/remove", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ slot: live.slot }),
        });
        if (!removed.ok) throw new Error(`recovery remove HTTP ${removed.status}: ${removed.text}`);
      }
      const finalSlots = await readSlots();
      writeJson("after-failure-slots.json", stableSlots(finalSlots));
      const restored = sameSlotSet(baselineSlots, finalSlots);
      record("E11", "failure-path baseline restoration", restored,
        restored ? "baseline restored after recovery cleanup" : "device state differs from baseline");
    }
  } catch (cleanupErr) {
    console.error("failure-path cleanup/verification failed:", cleanupErr.stack || cleanupErr.message);
    results.push({ stage: "E11", name: "failure-path cleanup", ok: false, detail: cleanupErr.message });
  }
} finally {
  await finish();
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
