#!/usr/bin/env node
// usb_snapshot_backup_once.mjs — v2 storage snapshot backup via the production
// USB page (#btn-backup, DYN mode). Saves the downloaded snapshot ZIP plus
// page log/console evidence into --rundir.
//
// Usage:
//   node tools/realdevice/usb_snapshot_backup_once.mjs --rundir <evidence-dir>
import { createHash } from "node:crypto";
import { appendFileSync, writeFileSync, mkdirSync, readFileSync } from "node:fs";
import path from "node:path";
import { chromium } from "playwright";

const argv = process.argv.slice(2);
const rdi = argv.indexOf("--rundir");
const RUNDIR = path.resolve(rdi >= 0 ? argv[rdi + 1] : ".");
mkdirSync(RUNDIR, { recursive: true });
const PROFILE = process.env.USB_E2E_PROFILE
  || path.resolve(path.dirname(new URL(import.meta.url).pathname), "logs", "usb-e2e-chrome-profile");
const BAUD = process.env.BACKUP_READ_BAUD || "115200";
const BASE = process.env.META_PASS_BASE || "http://localhost:4191";
const PAGE_URL = `${BASE}/?e2e=real&baud=${BAUD}`;
const log = (m) => { console.log(m); appendFileSync(path.join(RUNDIR, "console-live.log"), m + "\n"); };
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");

const result = { startedAt: new Date().toISOString(), url: PAGE_URL, status: "RUNNING", failure_category: null };
const t0 = Date.now();

const browser = await chromium.launchPersistentContext(PROFILE, {
  headless: false, channel: "chrome",
  viewport: { width: 1280, height: 1000 }, acceptDownloads: true,
});
const page = await browser.newPage();
page.on("console", (m) => log(`[console.${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => log(`[pageerror] ${e.stack || e.message}`));

let downloaded = null;
page.on("download", (d) => {
  const target = path.join(RUNDIR, d.suggestedFilename());
  d.saveAs(target).then(() => { downloaded = target; log("[dl] saved " + target); });
});

try {
  log(`goto ${PAGE_URL}`);
  await page.goto(PAGE_URL, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => window.__installSlotReady === true, null, { timeout: 15000 });
  if (((await page.locator("#chip-status").getAttribute("class")) || "") !== "ok") {
    await page.locator("#btn-connect").click();
  }
  await page.waitForFunction(
    () => document.getElementById("chip-status")?.className === "ok"
      && (window.__installSlotDebug?.slotModel?.mode || "").startsWith("DYN_"),
    null, { timeout: 120000 });
  log("connected, DYN mode");

  await page.locator("#btn-backup").click();
  log("backup clicked, waiting for completion…");
  await page.waitForFunction(
    () => {
      const el = document.getElementById("backup-status");
      return el && (el.className === "ok" || el.className === "error");
    },
    null, { timeout: 900000 });
  const st = await page.locator("#backup-status").textContent();
  const cls = await page.locator("#backup-status").getAttribute("class");
  result.backup_status_text = st; result.backup_status_class = cls;
  await page.waitForTimeout(1500); // let download land
  const logText = await page.locator("#log").textContent().catch(() => "");
  writeFileSync(path.join(RUNDIR, "page-log.txt"), logText);
  if (cls === "ok" && downloaded) {
    const bytes = readFileSync(downloaded);
    result.status = "PASS";
    result.archive = path.basename(downloaded);
    result.archive_bytes = bytes.length;
    result.archive_sha256 = createHash("sha256").update(bytes).digest("hex");
  } else {
    result.status = "FAIL";
    result.failure_category = "USB_SNAPSHOT_BACKUP";
  }
} catch (err) {
  result.status = "FAIL"; result.failure_category = "DRIVER";
  result.error = String(err && err.stack || err);
  try {
    writeFileSync(path.join(RUNDIR, "page-log.txt"), await page.locator("#log").textContent().catch(() => ""));
    await page.screenshot({ path: path.join(RUNDIR, "failure-screenshot.png") });
  } catch {}
} finally {
  result.duration_s = (Date.now() - t0) / 1000;
  writeJson("snapshot-backup-result.json", result);
  log(`status=${result.status} (${result.duration_s?.toFixed(1)}s)`);
  await browser.close();
}
process.exit(result.status === "PASS" ? 0 : 1);
