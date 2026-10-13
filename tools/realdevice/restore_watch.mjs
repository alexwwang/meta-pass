#!/usr/bin/env node
// tools/realdevice/restore_watch.mjs — instrumented single restore attempt.
// Clicks Restore then polls page state every 15s so we can SEE where time goes
// (staging write / APP write / readback / stall) instead of blind timeouts.
import { execFile, spawn } from "node:child_process";
import { readFileSync } from "node:fs";
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
    else { const n = argv[i + 1]; out[a.slice(2)] = n !== undefined && !n.startsWith("--") ? n : true; }
  }
  return out;
}
const argv = parseArgs(process.argv.slice(2));
const IP = argv.ip, TOKEN = String(argv.token || ""), PORT = Number(argv.port || 4191);
const SERIAL_PORT = String(argv["serial-port"] || "");
const RUNDIR = path.resolve(String(argv.rundir || ""));
const BAUD = String(argv.baud || "fast"); // fast=921600 | slow=115200
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
const state = JSON.parse(readFileSync(path.join(RUNDIR, "run-state.json"), "utf8"));

async function waitFor(fn, timeoutMs, label, intervalMs = 500) {
  const start = Date.now();
  for (;;) {
    try { const v = await fn(); if (v) return v; } catch {}
    if (Date.now() - start > timeoutMs) throw new Error(`timeout: ${label}`);
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}
async function reviveDevice() {
  return new Promise((resolve) => {
    execFile("python3", ["-m", "esptool", "--chip", "esp32c3", "--port", SERIAL_PORT, "--after", "hard_reset", "chip_id"], { timeout: 40000 }, (err) => resolve(!err));
  });
}

const server = spawn(process.execPath, [SERVER], { cwd: ROOT, env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" }, stdio: ["ignore", "pipe", "pipe"] });
await waitFor(async () => { try { const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) }); return r.ok; } catch { return false; } }, 15000, "server");
const browser = await chromium.launchPersistentContext(PROFILE, { headless: false, channel: "chrome", viewport: { width: 1280, height: 1000 } });
const page = await browser.newPage();
const url = BAUD === "slow" ? `${BASE}/?e2e=real&baud=115200` : `${BASE}/?e2e=real`;
await page.goto(url, { waitUntil: "domcontentloaded" });
await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });

// connect
await page.locator("#btn-connect").click();
await waitFor(async () => {
  const m = await page.evaluate(() => ({ c: document.getElementById("chip-status")?.className || "", mode: window.__installSlotDebug?.slotModel?.mode || "" }));
  return m.c === "ok" && m.mode.startsWith("DYN_") ? m : null;
}, 120000, "connect");

await page.locator("#restore-input").setInputFiles(state.zipPath);
await waitFor(async () => (await page.locator("#restore-info").getAttribute("class")) === "ok", 60000, "preflight");
console.log("preflight OK — clicking Restore at", new Date().toISOString());
await page.locator("#btn-restore").click();

const t0 = Date.now();
let lastLog = "";
for (;;) {
  await new Promise((r) => setTimeout(r, 15000));
  const s = await page.evaluate(() => ({
    status: document.getElementById("restore-status")?.className || "",
    text: (document.getElementById("restore-status")?.textContent || "").slice(0, 80),
    bar: document.getElementById("restore-bar")?.style.width || "",
    log: (document.getElementById("log")?.textContent || "").slice(-260),
  }));
  const el = ((Date.now() - t0) / 60000).toFixed(1);
  if (s.log !== lastLog) {
    console.log(`[${el}min] bar=${s.bar} status=${s.status}`);
    console.log("  log>>", s.log.replace(/\s+/g, " ").slice(-220));
    lastLog = s.log;
  } else {
    console.log(`[${el}min] bar=${s.bar} status=${s.status} (no new log)`);
  }
  if (s.status === "ok") { console.log("RESTORE OK", new Date().toISOString()); break; }
  if (s.status === "error") { console.log("RESTORE FAILED:", s.text); break; }
  if (Date.now() - t0 > 2700000) { console.log("WATCHDOG 45min"); break; }
}
try { await page.evaluate(() => { window.__installSlotDebug?.loader?.transport?.disconnect(); }); } catch {}
await browser.close();
server.kill("SIGTERM");
