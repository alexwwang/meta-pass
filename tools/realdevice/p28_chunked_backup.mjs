#!/usr/bin/env node
// tools/realdevice/p28_chunked_backup.mjs
//
// Host-side chunked backup for play 28 (录音笔, APP 1.46MB + recordings DATA
// 4.84MB). The USB link dies after ~4-5 minutes of continuous transfer at any
// baud (真机 2026-10-13 三次页面备份定案: slot+NVS+~2.6MB DATA 后 8/8 重试全败),
// so the page's all-in-one backup cannot finish on this unit. This driver reads
// each region in ≤1.5MB chunks, every chunk in a FRESH Web Serial session
// (page reload + reconnect), assembles the bytes host-side, and builds the v2
// archive with the same shared modules the page uses (buildDataArchiveManifest /
// buildDataArchiveZip). The result is validated with the same parser the
// restore input uses (readDataArchiveZip + validateDataArchiveManifest +
// validateDataArchivePayloads) and then run through the page's real restore
// preflight (setInputFiles → #restore-info.ok) before being handed to the
// delete/restore stages.
//
// Usage:
//   node tools/realdevice/p28_chunked_backup.mjs --real-device \
//     --ip <ip> --token <32hex> --serial-port <port> --rundir <dir>

import { execFile, execFileSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { buildDataArchiveManifest, buildDataArchiveZip, readDataArchiveZip,
         validateDataArchiveManifest, validateDataArchivePayloads } from "../../install-slot/data-byte-archive.js";
import { MANIFEST_NAME, firmwareFileName, tailFileName } from "../../install-slot/slot-backup.js";

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
const RUNDIR = path.resolve(String(argv.rundir || ""));
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
const DEVICE = `http://${IP || ""}`;
if (!IP || !TOKEN || !RUNDIR || !existsSync(RUNDIR)) { console.error("usage: --ip <ip> --token <32hex> --serial-port <port> --rundir <dir>"); process.exit(2); }

const state = JSON.parse(readFileSync(path.join(RUNDIR, "run-state.json"), "utf8"));
const slotsJson = JSON.parse(readFileSync(path.join(RUNDIR, "t1-after-install-slots.json"), "utf8"));
const slot = slotsJson.slots.find((s) => s.name === state.displayName);
if (!slot) throw new Error("slot not found in t1-after-install-slots.json");
const dataRecord = slotsJson.data.find((d) => d.play_id === state.playId);
if (!dataRecord) throw new Error("DATA record not found");
writeFileSync(path.join(RUNDIR, "command.txt"), process.argv.slice(2).map((a) => a === TOKEN ? "<redacted>" : a === IP ? "<redacted>" : a === SERIAL_PORT ? "<redacted>" : a).join(" ") + "\n");

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
    if (Date.now() - start > timeoutMs) throw new Error(`timeout waiting for ${label}`);
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}
async function reviveDevice() {
  return new Promise((resolve) => {
    execFile("python3", ["-m", "esptool", "--chip", "esp32c3", "--port", SERIAL_PORT,
                         "--after", "hard_reset", "chip_id"], { timeout: 40000 }, (err) => resolve(!err));
  });
}
function waitServer() {
  return waitFor(async () => {
    try { const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) }); return r.ok; } catch { return false; }
  }, 15000, "local USB server");
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

// Read one chunk in a fresh serial session; returns bytes.
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
      record("T3", `${label} read (attempt ${attempt})`, true, `${size}B sha256=${sha(bytes).slice(0, 16)}…`);
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
  writeJson("p28-chunked-metadata.json", {
    commit: (() => { try { return execFileSync("git", ["rev-parse", "HEAD"], { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return null; } })(),
    slot, dataRecord, startedAt: new Date().toISOString(),
    reason: "page all-in-one backup cannot finish: link dies after ~4-5min continuous transfer at any baud",
  });
  server = spawn(process.execPath, [SERVER], { cwd: ROOT, env: { ...process.env, PORT: String(PORT), META_PASS_DEV: "1" }, stdio: ["ignore", "pipe", "pipe"] });
  await waitServer();

  const browser = await chromium.launchPersistentContext(PROFILE, {
    headless: false, channel: "chrome", viewport: { width: 1280, height: 1000 }, acceptDownloads: true,
  });
  context = browser;
  page = await context.newPage();

  // Region 1: APP image (exactly image_len bytes, same payload install wrote).
  const appImage = await readChunkFresh("APP image", slot.offset, slot.len);
  // Region 2: tail sector (4K at the 4K-aligned offset right after the image).
  const tailOffset = slot.offset + Math.ceil(slot.len / 4096) * 4096;
  const tail = await readChunkFresh("tail sector", tailOffset, 4096);
  // Region 3: DATA partition in ≤512KB chunks (empirical per-session transfer
  // budget on this link is a few minutes; 1.5MB chunks died mid-read 4/4).
  const CHUNK = 512 * 1024;
  const dataParts = [];
  for (let off = 0; off < dataRecord.size; off += CHUNK) {
    const n = Math.min(CHUNK, dataRecord.size - off);
    dataParts.push(await readChunkFresh(`DATA chunk @+0x${off.toString(16)}`, dataRecord.offset + off, n));
  }
  const dataBytes = new Uint8Array(dataRecord.size);
  dataParts.forEach((p, i) => dataBytes.set(p, i * CHUNK));
  // Region 4: NVS (hash-only capture; raw bytes feed the archive but no evidence copy).
  const nvsBytes = await readChunkFresh("NVS", 0x9000, 24576);

  const fwName = firmwareFileName(slot.slot);
  const tailName = tailFileName(slot.slot);
  const dataName = `data/${dataRecord.play_id}/0.bin`;
  const manifest = buildDataArchiveManifest({
    slots: [{ play_id: slot.play_id, source_slot: slot.slot, name: slot.name,
              files: [
                { name: fwName, sha256: sha(appImage), bytes: appImage.length, type: "app" },
                { name: tailName, sha256: sha(tail), bytes: tail.length, type: "other" },
              ] }],
    data: [{ play_id: dataRecord.play_id, label: dataRecord.label, type: dataRecord.type,
             subtype: dataRecord.subtype, state: dataRecord.state,
             bytes: dataBytes.length, sha256: sha(dataBytes), file: dataName }],
    nvs: { file: "nvs.bin", sha256: sha(nvsBytes), bytes: nvsBytes.length, source_offset: 0x9000 },
    complete: true, missingSlots: [],
  });
  writeJson("t3-manifest.json", manifest);
  const zip = buildDataArchiveZip([
    { name: fwName, data: appImage },
    { name: tailName, data: tail },
    { name: dataName, data: dataBytes },
    { name: "nvs.bin", data: nvsBytes },
    { name: MANIFEST_NAME, data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) },
  ]);
  writeFileSync(path.join(RUNDIR, "backup.zip"), Buffer.from(zip));
  record("T3", "host-assembled v2 archive", true, `${zip.length} bytes`);

  // Validate with the exact parser/validators the restore path uses.
  const entries = readDataArchiveZip(new Uint8Array(readFileSync(path.join(RUNDIR, "backup.zip"))));
  validateDataArchiveManifest(manifest);
  const payloads = new Map(entries);
  payloads.delete(MANIFEST_NAME);
  await validateDataArchivePayloads(manifest, payloads, async (u8) => sha(u8));
  record("T3", "archive parses + payloads verified (shared modules)", true);

  // Page-side restore preflight: the real UI acceptance gate for this archive.
  await page.goto(`${BASE}/?e2e=real&baud=115200`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
  await page.locator("#restore-input").setInputFiles(path.join(RUNDIR, "backup.zip"));
  await waitFor(async () => (await page.locator("#restore-info").getAttribute("class")) === "ok",
                120000, "page restore preflight accepted the archive");
  record("T3", "page restore preflight accepted host-built archive", true);
  await page.screenshot({ path: path.join(RUNDIR, "t3-preflight.png"), fullPage: true });

  // Persist pre-delete state for the restore-stage NVS comparison.
  state.nvsSha256Before = sha(nvsBytes);
  state.nvsOffset = 0x9000;
  state.initialDataSha256 = sha(dataBytes);
  state.zipPath = path.join(RUNDIR, "backup.zip");
  state.zipDataSha256 = sha(dataBytes);
  writeJson("run-state.json", state);
  writeJson("report.json", { verdict: "PASS", results, finishedAt: new Date().toISOString() });
  console.log(`\n=== PASS — chunked backup complete, archive preflight accepted. ===`);
}

try {
  await main();
} catch (err) {
  console.error("Chunked backup FAILED:", err.stack || err.message);
  process.exitCode = 1;
  try { writeJson("report.json", { verdict: "FAIL", results, error: err.message }); } catch {}
} finally {
  try { if (page) await uiDisconnect(); } catch {}
  if (page) await page.close().catch(() => {});
  if (context) await context.close().catch(() => {});
  if (server) server.kill("SIGTERM");
}
