#!/usr/bin/env node
// tools/realdevice/p28_chunked_verify.mjs
//
// Post-restore byte-fidelity verification for the ?noverify restore path:
// reads the restored APP image + tail + DATA partition + NVS in ≤512KB chunks
// (fresh Web Serial session per chunk — the only read pattern this link
// survives), hashes them, and compares against the archived manifest.
//
// Usage:
//   node tools/realdevice/p28_chunked_verify.mjs --real-device \
//     --ip <ip> --token <32hex> --serial-port <port> --rundir <dir>

import { execFile, spawn } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { readDataArchiveZip } from "../../install-slot/data-byte-archive.js";
import { MANIFEST_NAME } from "../../install-slot/slot-backup.js";

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
if (!argv["real-device"]) { console.error("Refusing to touch hardware: pass --real-device."); process.exit(2); }
const IP = argv.ip, TOKEN = String(argv.token || ""), PORT = Number(argv.port || 4191);
const SERIAL_PORT = String(argv["serial-port"] || "");
const RUNDIR = path.resolve(String(argv.rundir || ""));
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;
if (!IP || !TOKEN || !RUNDIR) { console.error("usage: --ip <ip> --token <32hex> --serial-port <port> --rundir <dir>"); process.exit(2); }

const state = JSON.parse(readFileSync(path.join(RUNDIR, "run-state.json"), "utf8"));
const after = JSON.parse(readFileSync(path.join(RUNDIR, "t4-after-restore-slots.json"), "utf8"));
const slot = after.slots.find((s) => s.play_id === state.playId);
const dataRecord = after.data.find((d) => d.play_id === state.playId);
if (!slot || !dataRecord) throw new Error("restored slot/DATA record not found in t4-after-restore-slots.json");

const results = [];
function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  console.log(`[${stage}] ${ok ? "PASS" : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}`);
}
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");
const sha = (u8) => crypto.createHash("sha256").update(u8).digest("hex");

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
      await page.goto(`${BASE}/?e2e=real&baud=115200`, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await page.locator("#btn-connect").click();
    }
  }
}
async function uiDisconnect() {
  try { await page.evaluate(() => { window.__installSlotDebug?.loader?.transport?.disconnect(); }); } catch {}
  await new Promise((r) => setTimeout(r, 800));
}
async function readChunkFresh(label, offset, size) {
  for (let attempt = 1; ; attempt++) {
    try {
      await page.goto(`${BASE}/?e2e=real&baud=115200`, { waitUntil: "domcontentloaded" });
      await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
      await uiConnect();
      const res = await page.evaluate(async ({ off, len }) => {
        const dbg = window.__installSlotDebug;
        const bytes = await dbg.readFlashChunked(off, len, () => {}, () => {});
        let b64 = "";
        for (let p = 0; p < bytes.length; p += 0x6000) {
          b64 += btoa(String.fromCharCode(...bytes.subarray(p, p + 0x6000)));
        }
        return { length: bytes.length, b64 };
      }, { off: offset, len: size });
      if (res.length !== size) throw new Error(`short read: ${res.length} != ${size}`);
      await uiDisconnect();
      const bytes = Uint8Array.from(atob(res.b64), (c) => c.charCodeAt(0));
      record("V", `${label} read (attempt ${attempt})`, true, `${size}B sha256=${sha(bytes).slice(0, 16)}…`);
      return bytes;
    } catch (e) {
      console.log(`${label} attempt ${attempt} failed (${e.message.split("\n")[0]}); reviving and retrying`);
      await uiDisconnect();
      if (attempt >= 4) throw e;
      await reviveDevice();
      await new Promise((r) => setTimeout(r, 12000));
    }
  }
}

async function main() {
  server = spawn(process.execPath, [SERVER], { cwd: ROOT, env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" }, stdio: ["ignore", "pipe", "pipe"] });
  await waitFor(async () => { try { const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) }); return r.ok; } catch { return false; } }, 15000, "server");
  const browser = await chromium.launchPersistentContext(PROFILE, { headless: false, channel: "chrome", viewport: { width: 1280, height: 1000 } });
  context = browser;
  page = await context.newPage();

  const manifest = JSON.parse(readFileSync(path.join(RUNDIR, "t3-manifest.json"), "utf8"));
  const expectedFw = manifest.slots[0].files.find((f) => f.type === "app");
  const expectedData = manifest.data[0];

  // APP image + tail (carve exposes image_len; tail at 4K-aligned offset).
  const appImage = await readChunkFresh("APP image", slot.offset, slot.len);
  record("V", "restored APP == archived APP", sha(appImage) === expectedFw.sha256,
    sha(appImage).slice(0, 16) + (sha(appImage) === expectedFw.sha256 ? "" : ` != ${expectedFw.sha256.slice(0, 16)}`));

  // DATA partition in 512KB chunks, single rolling hash.
  const CHUNK = 512 * 1024;
  const h = crypto.createHash("sha256");
  for (let off = 0; off < dataRecord.size; off += CHUNK) {
    const n = Math.min(CHUNK, dataRecord.size - off);
    const part = await readChunkFresh(`DATA chunk @+0x${off.toString(16)}`, dataRecord.offset + off, n);
    h.update(part);
  }
  const dataSha = h.digest("hex");
  record("V", "restored DATA == archived DATA (recordings survive delete+restore byte-for-byte)",
    dataSha === expectedData.sha256,
    dataSha.slice(0, 16) + (dataSha === expectedData.sha256 ? "" : ` != ${expectedData.sha256.slice(0, 16)}`));

  // NVS untouched by restore (hash-only).
  const nvs = await readChunkFresh("NVS", 0x9000, 24576);
  const nvsPreserved = sha(nvs) === state.nvsSha256Before;
  record("V", "device NVS untouched by restore", nvsPreserved,
    nvsPreserved ? `sha256 unchanged (${sha(nvs).slice(0, 16)}…)` : `BEFORE=${state.nvsSha256Before?.slice(0, 16)} AFTER=${sha(nvs).slice(0, 16)}`);

  state.nvsSha256After = sha(nvs);
  writeJson("run-state.json", state);
  writeJson("verify-report.json", { verdict: "PASS", results, finishedAt: new Date().toISOString() });
  console.log("\n=== PASS — chunked post-restore verification complete. ===");
}

try {
  await main();
} catch (err) {
  console.error("Chunked verify FAILED:", err.stack || err.message);
  process.exitCode = 1;
  try { writeJson("verify-report.json", { verdict: "FAIL", results, error: err.message }); } catch {}
} finally {
  try { if (page) await uiDisconnect(); } catch {}
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
