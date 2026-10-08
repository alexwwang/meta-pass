#!/usr/bin/env node
// tools/realdevice/usb_web_e2e.mjs
//
// Real-device USB Web UI E2E:
// Chromium -> install-slot.html -> Web Serial -> esptool-js -> ESP32-C3 Flash.
//
// This is intentionally a thin browser harness. It never calls the direct phone-side
// installation functions or any /api/install write endpoint. The page performs the installation; the runner only
// drives UI, observes state, verifies the device independently, and cleans up its own slot.

import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { parseSlots } from "../../install-slot/phone-install.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const SERVER = path.join(ROOT, "tools", "install-slot", "server.mjs");
const LOGROOT = path.join(ROOT, "tools", "realdevice", "logs");

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
if (!argv["real-device"] && !argv.authorize) {
  console.error("Refusing to touch hardware: pass --real-device or --authorize explicitly.");
  process.exit(2);
}

const IP = argv.ip;
const TOKEN = argv.token || process.env.META_PASS_SESSION;
const PORT = Number(argv.port || process.env.META_PASS_PORT || 4191);
const PLAY = String(argv.play || process.env.USB_E2E_PLAY || "1");
const BROWSER_CHANNEL = String(argv["browser-channel"] || "chrome");
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const LOGDIR = path.join(LOGROOT, "usb-web-e2e-" + new Date().toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z"));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;

if (!argv.authorize && (!IP || !TOKEN || !/^[0-9a-f]{32}$/i.test(TOKEN))) {
  console.error("usage: node tools/realdevice/usb_web_e2e.mjs --real-device --ip <device-ip> --token <32hex> [--port 4191] [--play 1]");
  process.exit(2);
}

mkdirSync(LOGDIR, { recursive: true });
mkdirSync(PROFILE, { recursive: true });
writeFileSync(path.join(LOGDIR, "command.txt"), process.argv.slice(2).map((arg) => arg === TOKEN ? "<redacted-token>" : arg).join(" ") + "\n");

const results = [];
const consoleLines = [];
const pageErrors = [];
const report = {
  test: "USB Web UI -> Web Serial -> ESP32-C3 -> Flash",
  startedAt: new Date().toISOString(),
  device: IP,
  playId: Number(PLAY),
  server: BASE,
  profile: PROFILE,
  stages: results,
  verdict: "FAIL",
};

function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  console.log(`[${stage}] ${ok ? "PASS" : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}${detail ? " — " + detail : ""}`);
}

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

function writeJson(name, value) {
  writeFileSync(path.join(LOGDIR, name), JSON.stringify(value, null, 2) + "\n");
}

function writeText(name, value) {
  writeFileSync(path.join(LOGDIR, name), String(value));
}

async function waitFor(fn, timeoutMs, label, intervalMs = 250) {
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

async function waitServer() {
  await waitFor(async () => {
    try {
      const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) });
      return r.ok;
    } catch {
      return false;
    }
  }, 15000, "local USB server");
}

function findNewSlot(before, after) {
  const oldKeys = new Set(before.slots.map((s) => `${s.offset}:${s.size}`));
  return after.slots.filter((s) => !oldKeys.has(`${s.offset}:${s.size}`));
}

function sameSlotSet(a, b) {
  return JSON.stringify(stableSlots(a)) === JSON.stringify(stableSlots(b));
}

let server = null;
let context = null;
let page = null;
let baseline = null;
let created = null;
let testDisplayName = null;
let cleaned = false;

async function recoveryCleanup() {
  if (cleaned) return;
  try {
    const now = await readSlots();
    if (!created && baseline && testDisplayName) {
      const oldKeys = new Set(baseline.slots.slots.map((s) => `${s.offset}:${s.size}`));
      const owned = now.slots.filter((s) =>
        !oldKeys.has(`${s.offset}:${s.size}`) && s.name === testDisplayName);
      if (owned.length === 1) {
        const s = owned[0];
        created = { slot: s.slot, offset: s.offset, size: s.size, name: s.name };
      } else if (owned.length > 1) {
        throw new Error(`multiple possible test-owned slots found; refusing deletion: ${owned.length}`);
      }
    }
    if (!created) return;
    cleaned = true;
    console.log(`[E10] recovery cleanup candidate slot=${created.slot} offset=0x${created.offset.toString(16)} size=0x${created.size.toString(16)}`);
    const candidate = now.slots.find((s) =>
      s.offset === created.offset && s.size === created.size &&
      (s.name === created.name || s.name.toLowerCase().includes("usb-e2e")));
    if (!candidate) return;
    const r = await deviceCall("/api/install/remove", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ slot: candidate.slot }),
    });
    if (!r.ok) throw new Error(`recovery remove HTTP ${r.status}: ${r.text}`);
    console.log("[E10] recovery remove submitted");
  } catch (e) {
    console.error("[E10] recovery cleanup failed:", e.message);
    results.push({ stage: "E10", name: "recovery cleanup", ok: false, detail: e.message });
  }
}

async function authorizeProfile() {
  const server = spawn(process.execPath, [SERVER], {
    cwd: ROOT,
    env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" },
    stdio: ["ignore", "pipe", "pipe"],
  });
  try {
    await waitServer();
    const browser = await chromium.launchPersistentContext(PROFILE, {
      headless: false,
      channel: BROWSER_CHANNEL,
      viewport: { width: 1280, height: 1000 },
    });
    const p = await browser.newPage();
    await p.goto(`${BASE}/`, { waitUntil: "domcontentloaded" });
    console.log("Authorization setup: click Connect and select the target ESP32-C3 in the Web Serial chooser.");
    await waitFor(async () => /ESP32-C3/i.test((await p.locator("#chip-status").textContent()) || ""), 120000, "manual Web Serial authorization");
    await p.screenshot({ path: path.join(LOGROOT, "usb-e2e-authorized.png"), fullPage: true });
    console.log(`Web Serial authorization ready. Re-run with --real-device using profile ${PROFILE}`);
    await p.close();
    await browser.close();
  } finally {
    server.kill("SIGTERM");
  }
}

async function main() {
  const metadata = {
    branch: process.env.GIT_BRANCH || null,
    commit: process.env.GIT_COMMIT || (() => { try { return execFileSync("git", ["rev-parse", "HEAD"], { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return null; } })(),
    firmwareSha256: process.env.FIRMWARE_SHA256 || null,
    node: process.version,
    browserChannel: BROWSER_CHANNEL,
    device: IP,
    playId: Number(PLAY),
    server: BASE,
    startedAt: report.startedAt,
  };
  writeJson("metadata.json", metadata);

  // E0: environment + server.
  record("E0", "real-device flag", true);
  if (!existsSync(SERVER)) throw new Error(`missing server: ${SERVER}`);
  record("E0", "USB server source exists", true, SERVER);

  server = spawn(process.execPath, [SERVER], {
    cwd: ROOT,
    env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const serverLog = [];
  server.stdout.on("data", (b) => serverLog.push(b.toString()));
  server.stderr.on("data", (b) => serverLog.push(b.toString()));
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

  // E1: independent device baseline.
  baseline = {
    slots: await readSlots(),
    status: await readStatus(),
  };
  writeJson("baseline-slots.json", stableSlots(baseline.slots));
  writeJson("baseline-status.json", baseline.status);
  record("E1", "device baseline readable", true, `${baseline.slots.slots.length} slots`);

  // E2: actual USB page.
  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.locator("#lang-en").click();
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  if (await page.locator("#mock-banner").isVisible().catch(() => false)) {
    throw new Error("real E2E page unexpectedly entered mock mode");
  }
  if (pageErrors.length) throw new Error(`page error during load: ${pageErrors.join("\n")}`);
  await page.screenshot({ path: path.join(LOGDIR, "screenshot-page.png"), fullPage: true });
  record("E2", "real USB page loaded", true);

  // E3: real Web Serial connect.
  const serialCount = await page.evaluate(async () => {
    if (!("serial" in navigator)) return -1;
    return (await navigator.serial.getPorts()).length;
  });
  if (serialCount !== 1) {
    throw new Error(`expected exactly one authorized Web Serial port for ?e2e=real, found ${serialCount}; authorize the ESP32-C3 once with the normal Connect flow using the same profile: ${PROFILE}`);
  }
  await page.locator("#btn-connect").click();
  await waitFor(async () => {
    const text = await page.locator("#chip-status").textContent();
    return /ESP32-C3/i.test(text || "");
  }, 30000, "ESP32-C3 connection");
  await page.screenshot({ path: path.join(LOGDIR, "screenshot-connect.png"), fullPage: true });
  const uiModel = await page.evaluate(() => ({
    debug: window.__installSlotDebug?.slotModel ? {
      mode: window.__installSlotDebug.slotModel.mode,
      loaded: window.__installSlotDebug.slotModel.loaded,
    } : null,
    view: window.__installSlotDebug?.slotView || null,
  }));
  if (!uiModel.debug?.loaded) throw new Error("USB page slot model is not loaded");
  if (!String(uiModel.debug.mode).startsWith("DYN_")) {
    throw new Error(`storage E2E requires dynslot device mode, got ${uiModel.debug.mode}`);
  }
  record("E3", "real Web Serial + esptool connection", true, `mode=${uiModel.debug.mode}`);
  record("E3", "analyze connected device", Boolean(uiModel.debug.loaded && String(uiModel.debug.mode).startsWith("DYN_")), `dynslot mode=${uiModel.debug.mode}; slot model loaded`);

  // E4: UI slot model versus independent device baseline.
  const uiSlots = await page.evaluate(() => {
    const v = window.__installSlotDebug?.slotView;
    return v ? v.rows.filter((r) => r.kind === "app").map((r) => ({
      slot: r.slot, offset: r.offset, size: r.size, state: r.state, name: r.name,
    })) : null;
  });
  if (!uiSlots) throw new Error("USB page did not expose slot view");
  const baselineApp = baseline.slots.slots.filter((s) => s.kind === "app");
  for (const s of baselineApp) {
    const ui = uiSlots.find((x) => x.offset === s.offset && x.size === s.size);
    if (!ui) throw new Error(`UI missing baseline APP slot @0x${s.offset.toString(16)}`);
  }
  record("E4", "UI slot geometry matches device", true, `${uiSlots.length} APP rows`);
  record("E4", "analyze slot baseline", true, `all ${baselineApp.length} baseline APP slots present in UI`);

  // E5: actual Community Play UI.
  await page.locator('[data-tab="community"]').click();
  await page.locator("#play-url").fill(PLAY);
  await page.locator("#btn-fetch-play").click();
  await waitFor(async () => {
    const text = await page.locator("#play-info").textContent();
    return text && /SHA-256/i.test(text) && /Size/i.test(text);
  }, 30000, `Community Play #${PLAY} metadata`);
  const playInfo = await page.locator("#play-info").textContent();
  const displayName = `USB-E2E-${Date.now()}`;
  testDisplayName = displayName;
  await page.locator("#disp-name").fill(displayName);
  record("E5", "Community Play selected through UI", true, `play=${PLAY}; ${playInfo.replace(/\s+/g, " ").trim()}`);
  if (!(await page.locator("#btn-install").isEnabled())) throw new Error("Install is not enabled after Community Play metadata fetch");
  record("E5", "analyze install candidate", true, "metadata, size, SHA-256 present; Install enabled");

  // E6: only use dynslot Auto. Reusing a baseline empty slot would make cleanup
  // destructive to the pre-test state, so fail safely if Auto cannot fit this image.
  const auto = page.locator('.slot-row.auto input[name="slot"]:not([disabled])');
  if (!(await auto.count())) {
    throw new Error("dynslot Auto is unavailable for this image; refusing to consume or overwrite a baseline slot");
  }
  await auto.first().click();
  const checked = await page.locator('input[name="slot"]:checked').count();
  if (checked !== 1) throw new Error("UI did not select exactly one install target");
  const selectedTarget = await page.locator('input[name="slot"]:checked').evaluate((el) => ({
    value: el.value,
    disabled: el.disabled,
    rowText: el.closest(".slot-row")?.textContent || "",
    isAuto: el.closest(".slot-row")?.classList.contains("auto") || false,
  }));
  if (selectedTarget.disabled || !selectedTarget.isAuto) {
    throw new Error(`unsafe install target selected: ${JSON.stringify(selectedTarget)}`);
  }
  record("E6", "analyze install target", true, selectedTarget.rowText.replace(/\s+/g, " ").trim());
  await page.screenshot({ path: path.join(LOGDIR, "screenshot-install.png"), fullPage: true });
  await page.locator("#btn-install").click();

  await waitFor(async () => {
    const text = await page.locator("#install-status").textContent();
    return /done|success|完成|成功/i.test(text || "");
  }, 180000, "USB UI install completion", 500);

  const installLog = await page.locator("#log").textContent();
  writeText("page-install-log.txt", installLog || "");
  if (!/sha-?256 verified/i.test(installLog || "") || !/extracted/i.test(installLog || "")) {
    throw new Error("UI install log does not prove SHA-256 verification and extraction");
  }
  if (!/writing slot/i.test(installLog || "") || !/install done/i.test(installLog || "")) {
    throw new Error("UI install log does not prove Flash write and final completion");
  }
  record("E6", "UI install completed", true);

  // E7: final UI state.
  const finalStatusText = (await page.locator("#install-status").textContent() || "").trim();
  if (!/done|success|完成|成功/i.test(finalStatusText)) throw new Error(`unexpected install status: ${finalStatusText}`);
  await page.screenshot({ path: path.join(LOGDIR, "screenshot-final.png"), fullPage: true });
  record("E7", "UI reports installation done", true, finalStatusText);

  // E8/E9: independent device verification.
  const afterInstall = await readSlots();
  const newSlots = findNewSlot(baseline.slots, afterInstall);
  if (newSlots.length !== 1) {
    throw new Error(`expected exactly one new slot, found ${newSlots.length}`);
  }
  const testSlot = newSlots[0];
  created = { slot: testSlot.slot, offset: testSlot.offset, size: testSlot.size, name: testSlot.name };
  if (testSlot.state !== "valid" || testSlot.len <= 0 || testSlot.size <= 0) {
    throw new Error(`new slot is not valid: ${JSON.stringify(testSlot)}`);
  }
  if (testSlot.name !== displayName) {
    throw new Error(`device name mismatch: expected ${displayName}, got ${testSlot.name}`);
  }
  writeJson("after-install-slots.json", stableSlots(afterInstall));
  const afterInstallStatus = await readStatus();
  writeJson("after-install-status.json", afterInstallStatus);
  record("E8", "device API confirms new VALID APP slot", true,
    `slot=${testSlot.slot} offset=0x${testSlot.offset.toString(16)} size=0x${testSlot.size.toString(16)} imageLen=${testSlot.len}`);
  record("E9", "Flash-backed slot state persists in device model", true);

  // E10: analyze ownership, then cleanup through the actual USB page Remove path.
  const cleanupIdentityMatches = testSlot.offset === created.offset && testSlot.size === created.size && testSlot.name === created.name;
  if (!cleanupIdentityMatches) throw new Error("cleanup identity does not match the registered test slot");
  record("E10", "analyze deletion target ownership", true, `offset=0x${created.offset.toString(16)} size=0x${created.size.toString(16)} name=${created.name}`);
  page.once("dialog", async (dialog) => {
    await dialog.accept();
  });
  const row = page.locator(".slot-row").filter({ hasText: displayName }).first();
  if (!(await row.count())) throw new Error("created test slot is not visible in the USB page");
  await row.locator(".slot-remove").click();
  await waitFor(async () => {
    const rows = await page.locator(".slot-row").filter({ hasText: displayName }).count();
    return rows === 0;
  }, 30000, "USB UI removal");
  cleaned = true;
  record("E10", "test slot removed through USB UI", true);
  const afterUiRemove = await readSlots();
  if (afterUiRemove.slots.some((s) => s.offset === created.offset && s.size === created.size)) {
    throw new Error("device API still reports the test slot after UI Remove");
  }
  record("E10", "analyze deletion result", true, "device API no longer reports the test slot");

  // E11: restoration.
  const afterCleanup = await readSlots();
  writeJson("after-cleanup-slots.json", stableSlots(afterCleanup));
  if (!sameSlotSet(baseline.slots, afterCleanup)) {
    throw new Error("device slot state was not restored to baseline");
  }
  record("E11", "device restored to baseline", true);

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
    "# USB Web UI Real-Device E2E",
    "",
    `- verdict: **${report.verdict}**`,
    `- device: ${IP}`,
    `- play: ${PLAY}`,
    `- duration: ${report.durationSec.toFixed(1)}s`,
    "",
    "| stage | check | result | detail |",
    "|---|---|---|---|",
    ...results.map((r) => `| ${r.stage} | ${r.name} | ${r.ok ? "PASS" : "FAIL"} | ${String(r.detail || "").replaceAll("|", "\\|")} |`),
    "",
    "## Coverage boundary",
    "",
    "- USB Web UI -> Web Serial -> ESP32-C3 -> Flash: " + report.verdict,
    "- direct phone-install.js path: NOT USED",
    "- mocked Web Serial: NOT USED",
    "- mocked /api/install/*: NOT USED",
    "- DATA: covered by existing browser_smoke.mjs, not this test",
    "- physical power loss: NOT TESTED",
  ];
  writeText("report.md", lines.join("\n") + "\n");
}

process.on("SIGINT", async () => {
  await recoveryCleanup();
  process.exitCode = 130;
});

if (argv.authorize) {
  await authorizeProfile().catch((err) => {
    console.error("USB E2E authorization setup failed:", err.stack || err.message);
    process.exitCode = 1;
  });
} else try {
  await main();
} catch (err) {
  console.error("USB Web UI E2E FAILED:", err.stack || err.message);
  try {
    await recoveryCleanup();
    if (baseline) {
      const final = await readSlots();
      writeJson("after-failure-slots.json", stableSlots(final));
      const restored = sameSlotSet(baseline.slots, final);
      results.push({ stage: "E11", name: "failure-path baseline restoration", ok: restored, detail: restored ? "baseline restored after failure" : "device state differs from baseline after recovery cleanup" });
      if (!restored) throw new Error("failure-path cleanup did not restore device baseline");
    }
  } catch (cleanupErr) {
    console.error("cleanup/verification failed:", cleanupErr.stack || cleanupErr.message);
  }
  process.exitCode = 1;
} finally {
  await finish();
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
