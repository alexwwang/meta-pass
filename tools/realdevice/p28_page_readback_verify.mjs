#!/usr/bin/env node
// tools/realdevice/p28_page_readback_verify.mjs
//
// Decoupled device read-back verification for handoff R2/R3 on hosts where
// Chrome WebSerial wedges the read path after write traffic in one serial
// session (observed 2026-10-13: in-restore read-back died at 192KB @921600 /
// 960KB @115200, while read-only sessions survive multi-MiB). Runs the exact
// production verifier — install-slot/flash-read-transfer.js
// readFlashAndVerifyChunked (≤32 KiB reads, disconnect/connect/sync reconnect
// every 512 KiB, fail-closed byte compare) — in a fresh read-only page session
// against bytes written by a completed ?noverify restore. No verification is
// skipped: the same module the restore path calls performs every read/compare.
//
// Usage:
//   node tools/realdevice/p28_page_readback_verify.mjs --real-device \
//     --ip <ip> --token <32hex> --serial-port <port> --rundir <dir>
// Prereqs: harness restore stage completed (t4-after-restore-slots.json) with
// the archive at <rundir>/backup.zip.

import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

import { readDataArchiveZip } from "../../install-slot/data-byte-archive.js";
import { MANIFEST_NAME } from "../../install-slot/slot-backup.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
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
const RUNDIR = path.resolve(String(argv.rundir || ""));
const PROFILE = path.resolve(String(argv.profile || path.join(LOGROOT, "usb-e2e-chrome-profile")));
const BASE = `http://localhost:${PORT}`;
if (!IP || !TOKEN || !RUNDIR) { console.error("usage: --ip <ip> --token <32hex> --rundir <dir>"); process.exit(2); }

const state = JSON.parse(readFileSync(path.join(RUNDIR, "run-state.json"), "utf8"));
const after = JSON.parse(readFileSync(path.join(RUNDIR, "t4-after-restore-slots.json"), "utf8"));
const slot = after.slots.find((s) => s.play_id === state.playId);
const dataRecord = after.data.find((d) => d.play_id === state.playId);
if (!slot || !dataRecord) throw new Error("restored slot/DATA record not found in t4-after-restore-slots.json");

const entries = readDataArchiveZip(new Uint8Array(readFileSync(path.join(RUNDIR, "backup.zip"))));
const manifest = JSON.parse(new TextDecoder().decode(entries.get(MANIFEST_NAME)));
const appName = manifest.slots[0].files.find((f) => f.type === "app").name;
const dataName = manifest.data[0].file;
const expectedApp = entries.get(appName);
const expectedData = entries.get(dataName);
if (expectedApp.length !== slot.len) throw new Error(`archive APP ${expectedApp.length} != slot len ${slot.len}`);
if (expectedData.length !== dataRecord.size) throw new Error(`archive DATA ${expectedData.length} != record size ${dataRecord.size}`);

const results = [];
function record(stage, name, ok, detail = "") {
  results.push({ stage, name, ok, detail });
  console.log(`[${stage}] ${ok ? "PASS" : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
  if (!ok) throw new Error(`${stage}: ${name}`);
}
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");

async function waitFor(fn, timeoutMs, label, intervalMs = 500) {
  const start = Date.now();
  for (;;) {
    try { const v = await fn(); if (v) return v; } catch {}
    if (Date.now() - start > timeoutMs) throw new Error(`timeout: ${label}`);
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}

let context = null, page = null;
async function uiConnect() {
  const chipClass = await page.locator("#chip-status").getAttribute("class") || "";
  if (chipClass !== "ok") await page.locator("#btn-connect").click();
  await waitFor(async () => {
    const m = await page.evaluate(() => ({
      chipClass: document.getElementById("chip-status")?.className || "",
      mode: window.__installSlotDebug?.slotModel?.mode || "",
    }));
    return m.chipClass === "ok" && m.mode.startsWith("DYN_") ? m : null;
  }, 120000, "DYN_SLOT connection");
}
async function uiDisconnect() {
  try { await page.evaluate(() => { window.__installSlotDebug?.loader?.transport?.disconnect(); }); } catch {}
  await new Promise((r) => setTimeout(r, 800));
}

// One production readFlashAndVerifyChunked run inside the live page. Probe
// events land in window.__flashReadEvents; they are recovered even on failure.
// Empirical host limit (2026-10-13, two independent 115200 sessions):
// exactly 30 readFlash calls per page/SerialPort session wedge the Chrome
// WebSerial RX pipe ("No serial data received"); transport reconnect does not
// reset the budget, a fresh page does. Segments therefore stay ≤512KB
// (16 reads), matching the page-reload-per-chunk pattern proven by
// p28_chunked_backup/p28_chunked_verify on this link.
const SEGMENT = 512 * 1024;
async function runVerifySegments(baseAddress, expected, label) {
  const segments = [];
  for (let off = 0; off < expected.length; off += SEGMENT) {
    segments.push({ address: baseAddress + off, expected: expected.subarray(off, Math.min(expected.length, off + SEGMENT)), index: segments.length });
  }
  const allEvents = [];
  for (const seg of segments) {
    await freshPageSession();
    const t0 = Date.now();
    try {
      await page.evaluate(async ({ address, expected }) => {
        const mod = await import("./flash-read-transfer.js");
        window.__flashReadEvents = [];
        window.__flashReadProbe = (e) => window.__flashReadEvents.push(e);
        const loader = window.__installSlotDebug.loader;
        await mod.readFlashAndVerifyChunked({
          address,
          expected,
          readChunk: (offset, length) => {
            window.__flashReadProbe?.({ kind: "read", offset, length, at: Date.now() });
            return loader.readFlash(offset, length);
          },
          reconnect: async () => {
            window.__flashReadProbe?.({ kind: "reconnect-start", at: Date.now() });
            await loader.transport.disconnect();
            await loader.transport.connect(115200);
            loader.transport.buffer = new Uint8Array(0);
            await loader.sync();
            await new Promise((r) => setTimeout(r, 50));
            window.__flashReadProbe?.({ kind: "reconnect-done", at: Date.now() });
          },
        });
      }, { address: seg.address, expected: seg.expected });
    } finally {
      const events = await page.evaluate(() => window.__flashReadEvents || []).catch(() => []);
      for (const e of events) allEvents.push({ ...e, segment: seg.index });
      const reads = events.filter((e) => e.kind === "read");
      console.log(`  ${label} segment ${seg.index} @0x${seg.address.toString(16)}: ${reads.length} reads ` +
        `${((Date.now() - t0) / 1000).toFixed(1)}s`);
      if (reads.length * 0x8000 < seg.expected.length - 0x8000) {
        writeJson(`p28-readback-probe-${label}.json`, allEvents);
        throw new Error(`${label} segment ${seg.index}: died after ${reads.length} reads ` +
          `(host WebSerial session budget; segment budget is ${SEGMENT / 1024}KB = 16 reads)`);
      }
    }
  }
  writeJson(`p28-readback-probe-${label}.json`, allEvents);
  validateProbe(label, baseAddress, expected.length, allEvents);
}

async function freshPageSession() {
  await page.goto("about:blank");
  await new Promise((r) => setTimeout(r, 500));
  await page.goto(`${BASE}/?e2e=real&baud=115200`, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await uiConnect();
}

function validateProbe(label, address, totalBytes, events) {
  const CHUNK = 0x8000, SESSION = 512 * 1024;
  const reads = events.filter((e) => e.kind === "read");
  const starts = events.filter((e) => e.kind === "reconnect-start");
  const dones = events.filter((e) => e.kind === "reconnect-done");
  const bad = reads.filter((r) => r.length > CHUNK);
  if (bad.length) throw new Error(`${label}: ${bad.length} reads exceed 32 KiB`);
  let expect = address, bytes = 0;
  for (const r of reads) {
    if (r.offset !== expect) throw new Error(`${label}: discontinuous offset 0x${r.offset.toString(16)} (expected 0x${expect.toString(16)})`);
    expect += r.length; bytes += r.length;
  }
  if (bytes !== totalBytes) throw new Error(`${label}: read ${bytes} of ${totalBytes} bytes before failure`);
  if (starts.length !== dones.length) throw new Error(`${label}: ${starts.length} reconnect-starts vs ${dones.length} dones`);
  // Per page-session segment budget: the module reconnects only when MORE
  // data remains after a 512 KiB boundary (offset < expected.length), so a
  // segment of exactly 512 KiB performs 0 reconnects; expected = Σ seg>512KiB
  // ? floor(seg/512KiB) : 0.
  const segBytes = new Map();
  for (const r of reads) segBytes.set(r.segment, (segBytes.get(r.segment) || 0) + r.length);
  const expectedReconnects = [...segBytes.values()].reduce((n, b) => n + (b > SESSION ? Math.floor(b / SESSION) : 0), 0);
  if (starts.length !== expectedReconnects) throw new Error(`${label}: ${starts.length} reconnects, expected ${expectedReconnects}`);
  const first = reads[0]?.at, last = reads.at(-1)?.at;
  const secs = first && last ? ((last - first) / 1000).toFixed(1) : "?";
  record("RV", `${label}: production chunked verifier passed on device`, true,
    `${reads.length} reads (max ${Math.max(...reads.map((r) => r.length))}B) contiguous 0x${address.toString(16)}+${totalBytes}, ` +
    `${starts.length} reconnects @512KiB, byte-for-byte equal, reads spanned ${secs}s`);
}

async function main() {
  await waitFor(async () => { try { const r = await fetch(BASE + "/", { signal: AbortSignal.timeout(1000) }); return r.ok; } catch { return false; } }, 15000, "server");
  const browser = await chromium.launchPersistentContext(PROFILE, { headless: false, channel: "chrome", viewport: { width: 1280, height: 1000 } });
  context = browser;
  page = await context.newPage();

  await runVerifySegments(dataRecord.offset, expectedData, "DATA");
  await runVerifySegments(slot.offset, expectedApp, "APP");

  await uiDisconnect();
  writeJson("p28-page-readback-report.json", { verdict: "PASS", results, finishedAt: new Date().toISOString() });
  console.log("\n=== PASS — production chunked read-back verified on device (decoupled read-only session). ===");
}

try {
  await main();
} catch (e) {
  console.error(`\n=== FAIL: ${e.message.split("\n")[0]} ===`);
  try { writeJson("p28-page-readback-report.json", { verdict: "FAIL", error: e.message, results, finishedAt: new Date().toISOString() }); } catch {}
  process.exitCode = 1;
} finally {
  try { if (context) await context.close(); } catch {}
}
