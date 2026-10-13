#!/usr/bin/env node
// tools/realdevice/install_market_play.mjs
//
// Install one marketplace play via the USB page community tab, then stop:
// the device is left with the play installed and running for the handoff T2
// human DATA verification (real UI firmware, e.g. play 28 录音笔).
//
// Usage:
//   node tools/realdevice/install_market_play.mjs --real-device \
//     --ip <device-ip> --token <32hex> --serial-port <port> --play-id 28 \
//     [--name <display-name>] [--port 4191]

import { execFile, execFileSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

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
if (!argv["real-device"]) { console.error("Refusing to touch hardware: pass --real-device."); process.exit(2); }
const IP = argv.ip;
const TOKEN = String(argv.token || process.env.META_PASS_SESSION || "");
const PORT = Number(argv.port || process.env.META_PASS_PORT || 4191);
const SERIAL_PORT = String(argv["serial-port"] || process.env.USB_SERIAL_PORT || "");
const PLAY_ID = String(argv["play-id"] || "28");
const NAME = String(argv.name || `PLAY-${PLAY_ID}-${Date.now()}`);
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;
const LOGDIR = path.join(LOGROOT, "install-market-play-" + new Date().toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z"));

if (!IP || !TOKEN || !SERIAL_PORT) { console.error("usage: --ip <ip> --token <32hex> --serial-port <port> --play-id 28"); process.exit(2); }
mkdirSync(LOGDIR, { recursive: true });
writeFileSync(path.join(LOGDIR, "command.txt"),
  process.argv.slice(2).map((a) => a === TOKEN ? "<redacted>" : a === IP ? "<redacted>" : a === SERIAL_PORT ? "<redacted>" : a).join(" ") + "\n");

const results = [];
function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  console.log(`[${stage}] ${ok ? "PASS" : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}`);
}
const writeJson = (n, v) => writeFileSync(path.join(LOGDIR, n), JSON.stringify(v, null, 2) + "\n");
const writeText = (n, v) => writeFileSync(path.join(LOGDIR, n), v);

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
const readSlots = async () => (await deviceCall("/api/install/slots")).json;

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
    try { const r = await fetch(DEVICE + "/", { signal: AbortSignal.timeout(3000) }); if (r.ok) return true; } catch {}
    await new Promise((r) => setTimeout(r, 2000));
  }
  return false;
}
function waitServer() {
  return waitFor(async () => {
    try { const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) }); return r.ok; } catch { return false; }
  }, 15000, "local USB server");
}
async function reviveDevice() {
  return new Promise((resolve) => {
    execFile("python3", ["-m", "esptool", "--chip", "esp32c3", "--port", SERIAL_PORT,
                         "--after", "hard_reset", "chip_id"], { timeout: 40000 }, (err) => resolve(!err));
  });
}

let server = null, context = null, page = null;
async function uiConnect() {
  const chipClass = await page.locator("#chip-status").getAttribute("class") || "";
  if (chipClass !== "ok") await page.locator("#btn-connect").click();
  for (let attempt = 1; ; attempt++) {
    try {
      return await waitFor(async () => {
        const m = await page.evaluate(() => ({
          chipClass: document.getElementById("chip-status")?.className || "",
          mode: window.__installSlotDebug?.slotModel?.mode || "",
        }));
        return m.chipClass === "ok" && m.mode.startsWith("DYN_") ? m : null;
      }, 120000, "DYN_SLOT connection");
    } catch (e) {
      if (attempt >= 3) throw e;
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
    await page.evaluate(() => { window.__installSlotDebug?.loader?.transport?.disconnect(); });
  } catch {}
  await new Promise((r) => setTimeout(r, 1000));
}

async function main() {
  writeJson("metadata.json", {
    commit: (() => { try { return execFileSync("git", ["rev-parse", "HEAD"], { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return null; } })(),
    playId: Number(PLAY_ID), startedAt: new Date().toISOString(),
  });
  server = spawn(process.execPath, [SERVER], { cwd: ROOT, env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" }, stdio: ["ignore", "pipe", "pipe"] });
  await waitServer();
  record("E0", "server ready", true, BASE);

  const browser = await chromium.launchPersistentContext(PROFILE, {
    headless: false, channel: "chrome", viewport: { width: 1280, height: 1000 }, acceptDownloads: true,
  });
  context = browser;
  page = await context.newPage();

  const before = await readSlots();
  writeJson("baseline-slots.json", before);
  record("T0", "baseline", true, `count=${before.count} free=${before.free}`);

  await page.goto(`${BASE}/?e2e=real`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  record("T1", "connected", true);

  await page.locator('[data-tab="community"]').click();
  await new Promise((r) => setTimeout(r, 500));
  await page.locator("#play-url").fill(`http://localhost:${PORT}/api/play?id=${PLAY_ID}`);
  await page.locator("#btn-fetch-play").click();
  await waitFor(async () => {
    const text = await page.locator("#play-info").textContent();
    return text && /SHA-256/i.test(text) && /Size/i.test(text);
  }, 60000, `Play #${PLAY_ID} metadata`);
  record("T1", `play #${PLAY_ID} fetched`, true);

  await page.locator("#disp-name").fill(NAME);
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
    }, 300000, "UI install completion");
  } catch (e) { await reviveDevice(); throw e; }
  const installLog = (await page.locator("#log").textContent()) || "";
  writeText("install-log.txt", installLog);
  record("T1", `play #${PLAY_ID} installed`, true, `displayName=${NAME}`);

  await uiDisconnect();
  const bootLog = await usbHardReset();
  writeText("boot-log.txt", bootLog);
  if (!(await waitLan())) throw new Error("LAN not up after reset");
  const after = await readSlots();
  writeJson("after-install-slots.json", after);
  const mine = after.slots.find((s) => s.name === NAME);
  if (!mine) throw new Error(`installed slot "${NAME}" not found after reset`);
  record("T1", "slot persisted across reset", true,
    `slot=${mine.slot} @${mine.offset}+${mine.size} play_id=${mine.play_id}`);
  record("T1", "DATA records after install", true, `data[]=${JSON.stringify(after.data)}`);
  writeJson("report.json", { verdict: "PASS", playId: Number(PLAY_ID), name: NAME, results, finishedAt: new Date().toISOString() });
  console.log(`\n=== PASS — play #${PLAY_ID} installed as "${NAME}". Device ready for human DATA verification. ===`);
  console.log(`LOGDIR=${LOGDIR}`);
}

try {
  await main();
} catch (err) {
  console.error("Install failed:", err.stack || err.message);
  process.exitCode = 1;
  try { writeJson("report.json", { verdict: "FAIL", results, error: err.message }); } catch {}
} finally {
  try { if (page) await uiDisconnect(); } catch {}
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
