#!/usr/bin/env node
// usb_snapshot_restore_once.mjs — v2 storage snapshot restore via the production
// USB page (restore-input preflight + #btn-restore, DYN mode). Expects the
// snapshot ZIP path as --zip. Saves page log/console/final UI state into --rundir.
//
// Usage:
//   node tools/realdevice/usb_snapshot_restore_once.mjs --rundir <dir> --zip <snapshot.zip>
import { appendFileSync, writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import { chromium } from "playwright";

const argv = process.argv.slice(2);
const arg = (k) => { const i = argv.indexOf("--" + k); return i >= 0 ? argv[i + 1] : ""; };
const RUNDIR = path.resolve(arg("rundir") || ".");
const ZIP = path.resolve(arg("zip") || "");
mkdirSync(RUNDIR, { recursive: true });
if (!ZIP) { console.error("--zip required"); process.exit(2); }
const PROFILE = process.env.USB_E2E_PROFILE
  || path.resolve(path.dirname(new URL(import.meta.url).pathname), "logs", "usb-e2e-chrome-profile");
const BAUD = process.env.BACKUP_READ_BAUD || "115200";
const BASE = process.env.META_PASS_BASE || "http://localhost:4191";
const PAGE_URL = `${BASE}/?e2e=real&baud=${BAUD}`;
const log = (m) => { console.log(m); appendFileSync(path.join(RUNDIR, "console-live.log"), m + "\n"); };
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");

const result = { startedAt: new Date().toISOString(), url: PAGE_URL, zip: path.basename(ZIP), status: "RUNNING", failure_category: null };
const t0 = Date.now();

const browser = await chromium.launchPersistentContext(PROFILE, {
  headless: false, channel: "chrome",
  viewport: { width: 1280, height: 1000 }, acceptDownloads: true,
});
const page = await browser.newPage();
page.on("console", (m) => log(`[console.${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => log(`[pageerror] ${e.stack || e.message}`));
page.on("dialog", (d) => { log("[dialog] " + d.message().split("\n")[0] + " → accept"); d.accept().catch(() => {}); });

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

  await page.locator("#restore-input").setInputFiles(ZIP);
  log("zip attached, waiting for preflight…");
  await page.waitForFunction(
    () => {
      const el = document.getElementById("restore-info");
      return el && (el.className === "ok" || el.className === "error");
    },
    null, { timeout: 120000 });
  result.preflight_text = await page.locator("#restore-info").textContent();
  result.preflight_class = await page.locator("#restore-info").getAttribute("class");
  log("preflight: " + result.preflight_class + " — " + result.preflight_text);
  if (result.preflight_class !== "ok") throw new Error("preflight failed: " + result.preflight_text);

  await page.locator("#btn-restore").click();
  log("restore clicked, waiting for completion…");
  await page.waitForFunction(
    () => {
      const el = document.getElementById("restore-status");
      return el && (el.className === "ok" || el.className === "error" || el.className === "warn");
    },
    null, { timeout: 900000 });
  result.restore_status_text = await page.locator("#restore-status").textContent();
  result.restore_status_class = await page.locator("#restore-status").getAttribute("class");
  result.restore_progress = await page.locator("#progress-bar").getAttribute("style").catch(() => null);
  log("restore final: " + result.restore_status_class + " — " + result.restore_status_text);
  result.status = result.restore_status_class === "error" ? "FAIL" : "PASS";
  if (result.status === "FAIL") result.failure_category = "USB_SNAPSHOT_RESTORE";
  writeFileSync(path.join(RUNDIR, "page-log.txt"), await page.locator("#log").textContent().catch(() => ""));
  await page.screenshot({ path: path.join(RUNDIR, "final-screenshot.png") });
} catch (err) {
  result.status = "FAIL"; result.failure_category = result.failure_category || "DRIVER";
  result.error = String(err && err.stack || err);
  try {
    writeFileSync(path.join(RUNDIR, "page-log.txt"), await page.locator("#log").textContent().catch(() => ""));
    await page.screenshot({ path: path.join(RUNDIR, "failure-screenshot.png") });
  } catch {}
} finally {
  result.duration_s = (Date.now() - t0) / 1000;
  writeJson("snapshot-restore-result.json", result);
  log(`status=${result.status} (${result.duration_s?.toFixed(1)}s)`);
  await browser.close();
}
process.exit(result.status === "PASS" ? 0 : 1);
