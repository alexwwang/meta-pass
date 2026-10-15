#!/usr/bin/env node
// usb_data_backup_read_once.mjs — Stage C natural-reproduction driver
// (HANDOFF-usb-data-backup-read §C). Drives the page's own all-in-one backup
// with Debug mode on, in ONE fresh page session at the historical baud — no
// reconnect/reload/segmentation workarounds. If the transfer stalls/dead,
// waits, preserves full evidence (per-read page log, esptool-js TRACE console,
// final UI state, screenshot) and classifies the failure. On success, saves
// the ZIP and runs the offline archive hash validation.
//
// Usage:
//   node tools/realdevice/usb_data_backup_read_once.mjs --rundir <evidence-dir>
import { createHash } from "node:crypto";
import { appendFileSync, writeFileSync, existsSync, mkdirSync } from "node:fs";
import path from "node:path";
import { chromium } from "playwright";
import { readDataArchiveZip } from "../../install-slot/data-byte-archive.js";
import { MANIFEST_NAME } from "../../install-slot/slot-backup.js";

const argv = process.argv.slice(2);
const rdi = argv.indexOf("--rundir");
const RUNDIR = path.resolve(rdi >= 0 ? argv[rdi + 1] : ".");
mkdirSync(RUNDIR, { recursive: true });
// Persistent Chrome profile holding the one-time Web Serial grant (shared with
// the other realdevice harnesses). Override via USB_E2E_PROFILE if relocated.
const PROFILE = process.env.USB_E2E_PROFILE
  || path.resolve(path.dirname(new URL(import.meta.url).pathname), "logs", "usb-e2e-chrome-profile");
const BAUD = process.env.BACKUP_READ_BAUD || "115200";
const BASE = process.env.META_PASS_BASE || "http://localhost:4191";
const PAGE_URL = `${BASE}/?e2e=real&baud=${BAUD}`;
const log = (m) => { console.log(m); appendFileSync(path.join(RUNDIR, "console-live.log"), m + "\n"); };
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");
const sha = (u8) => createHash("sha256").update(u8).digest("hex");

const result = {
  startedAt: new Date().toISOString(), url: PAGE_URL, baud: Number(BAUD),
  status: "RUNNING", failure_category: null, reads: [],
  bytes_completed: 0, read_calls: 0, read_retries: 0,
  backup_duration_seconds: null, archive_sha256: null,
  payload_hashes_match: null, artifacts: [],
};
const t0 = Date.now();

const browser = await chromium.launchPersistentContext(PROFILE, {
  headless: false, channel: "chrome",
  viewport: { width: 1280, height: 1000 }, acceptDownloads: true,
});
const page = await browser.newPage();
page.on("console", (m) => log(`[console.${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => log(`[pageerror] ${e.stack || e.message}`));

let lastLogText = "", lastReadLineAt = Date.now(), backupRunning = false;

async function pollLog() {
  const txt = (await page.locator("#log").textContent().catch(() => "")) || "";
  if (txt.length > lastLogText.length) {
    const fresh = txt.slice(lastLogText.length);
    appendFileSync(path.join(RUNDIR, "page-log-live.txt"), fresh);
    const readOk = fresh.match(/\[debug\] 0x[0-9a-f]+ \+\d+B · \d+ms/g);
    if (readOk) lastReadLineAt = Date.now();
    lastLogText = txt;
  }
}

try {
  log(`[C] goto ${PAGE_URL}`);
  await page.goto(PAGE_URL, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  await page.locator("#debug-mode").check();
  if (((await page.locator("#chip-status").getAttribute("class")) || "") !== "ok") {
    await page.locator("#btn-connect").click();
  }
  await page.waitForFunction(
    () => document.getElementById("chip-status")?.className === "ok"
      && (window.__installSlotDebug?.slotModel?.mode || "").startsWith("DYN_"),
    null, { timeout: 120000 });
  log("[C] connected, DYN mode, debug on");

  await page.evaluate(() => { window.showSaveFilePicker = undefined; });
  if (!(await page.locator("#bk-slot0").isChecked())) await page.locator("#bk-slot0").check();
  await page.locator("#btn-backup").click();
  backupRunning = true;
  log("[C] backup clicked, waiting…");

  const poll = setInterval(() => pollLog().catch(() => {}), 2000);
  const outcome = await Promise.race([
    page.waitForEvent("download", { timeout: 2400000 }).then((d) => ({ kind: "download", d })),
    page.waitForFunction(
      () => /失败|fail/i.test(document.getElementById("backup-status")?.textContent || ""),
      null, { timeout: 2400000, polling: 1000 }).then(() => ({ kind: "status-error" })),
    (async () => {  // stall: backup in progress but no successful read for ≥60s
      for (;;) {
        await new Promise((r) => setTimeout(r, 5000));
        const busy = await page.evaluate(() => {
          const st = document.getElementById("backup-status")?.textContent || "";
          return st !== "" && !/完成|done|ok/i.test(st);
        }).catch(() => false);
        if (Date.now() - lastReadLineAt >= 60000) return { kind: "stall" };
      }
    })(),
  ]);
  clearInterval(poll);
  await pollLog();

  if (outcome.kind === "download") {
    const zipPath = path.join(RUNDIR, "backup.zip");
    await outcome.d.saveAs(zipPath);
    const zipBytes = new Uint8Array((await import("node:fs")).readFileSync(zipPath));
    result.backup_duration_seconds = (Date.now() - t0) / 1000;
    result.archive_sha256 = sha(zipBytes);
    // 离线归档验证(handoff §C.4)
    const entries = readDataArchiveZip(zipBytes);
    const manifest = JSON.parse(new TextDecoder().decode(entries.get(MANIFEST_NAME)));
    writeJson("backup-manifest.json", manifest);
    const problems = [];
    if (manifest.manifest_version !== 2) problems.push("manifest_version != 2");
    if (manifest.complete !== true) problems.push("complete != true");
    if (manifest.missing_slots || manifest.missingSlots) problems.push("missing_slots present");
    if (!Array.isArray(manifest.slots) || manifest.slots.length !== 1) problems.push("slots != 1");
    if (!Array.isArray(manifest.data) || manifest.data.length !== 1) problems.push("data != 1");
    let payloads = 0;
    for (const s of manifest.slots || []) for (const f of s.files || []) {
      const b = entries.get(f.name);
      if (!b) { problems.push(`missing entry ${f.name}`); continue; }
      if (b.length !== f.bytes || sha(b) !== f.sha256) problems.push(`payload hash mismatch ${f.name}`);
      else payloads++;
    }
    for (const drec of manifest.data || []) {
      const b = entries.get(drec.file);
      if (!b) { problems.push(`missing DATA entry ${drec.file}`); continue; }
      if (b.length !== drec.bytes || sha(b) !== drec.sha256) problems.push(`DATA hash mismatch ${drec.file}`);
      else { payloads++; result.data_bytes = drec.bytes; }
    }
    result.payload_hashes_match = problems.length === 0;
    result.status = result.payload_hashes_match ? "PASS" : "FAIL_ARCHIVE_INTEGRITY";
    if (problems.length) writeJson("backup-validation-problems.json", problems);
    result.artifacts.push("backup.zip", "backup-manifest.json", "page-log-live.txt", "console-live.log");
    log(`[C] download + validation: payloads_ok=${payloads} problems=${problems.length}`);
  } else {
    result.status = "FAIL_STALL";
    result.failure_category = outcome.kind === "status-error" ? "USB_READ_PROTOCOL" : "USB_READ_TIMEOUT";
    // 终态证据:status 文本、完整 #log、截图
    const st = await page.evaluate(() => ({
      status: document.getElementById("backup-status")?.textContent || "",
      cls: document.getElementById("backup-status")?.className || "",
      bar: document.getElementById("backup-bar")?.style.width || "",
      logTail: (document.getElementById("log")?.textContent || "").slice(-2000),
    })).catch((e) => ({ error: e.message }));
    writeJson("failure-final-state.json", st);
    await page.screenshot({ path: path.join(RUNDIR, "failure-screenshot.png"), fullPage: true }).catch(() => {});
    log(`[C] ${outcome.kind}; final status: ${JSON.stringify(st).slice(0, 300)}`);
  }
} catch (e) {
  result.status = "FAIL_DRIVER"; result.failure_category = "UNKNOWN";
  log(`[C] driver error: ${e.stack || e.message}`);
  await page.screenshot({ path: path.join(RUNDIR, "failure-screenshot.png"), fullPage: true }).catch(() => {});
} finally {
  // 解析逐读证据
  const logText = existsSync(path.join(RUNDIR, "page-log-live.txt"))
    ? (await import("node:fs")).readFileSync(path.join(RUNDIR, "page-log-live.txt"), "utf8") : "";
  const re = /\[debug\] → read (0x[0-9a-f]+) \+(\d+)B #(\d+)|\[debug\] (0x[0-9a-f]+) \+(\d+)B · (\d+)ms|\[read-error\] (0x[0-9a-f]+) attempt (\d+)\/(\d+): (.*)/g;
  let m, pending = {};
  while ((m = re.exec(logText))) {
    if (m[1]) { pending[m[1] + ":" + m[2]] = { offset: m[1], len: +m[2], attempt: +m[3] }; result.read_calls++; if (+m[3] > 1) result.read_retries++; }
    else if (m[4]) { const k = m[4] + ":" + m[5]; if (pending[k]) { pending[k].duration_ms = +m[6]; result.reads.push(pending[k]); result.bytes_completed += +m[5]; delete pending[k]; } }
    else result.reads.push({ offset: m[7], error_attempt: +m[8], max: +m[9], error: m[10] });
  }
  result.read_calls = result.reads.filter((r) => r.duration_ms).length + result.reads.filter((r) => r.error).length;
  result.endedAt = new Date().toISOString();
  writeJson("backup-read-result.json", result);
  log(`[C] done status=${result.status} reads=${result.reads.length} bytes=${result.bytes_completed}`);
  await browser.close().catch(() => {});
  process.exit(result.status === "PASS" ? 0 : 1);
}
