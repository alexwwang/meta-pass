#!/usr/bin/env node
// phone_snapshot_restore_once.mjs — v2 storage snapshot restore via the phone
// install page served by the device (LAN, paired session via #s=<token>).
// Expects --zip and --token (or META_PASS_SESSION). Saves UI state + console.
//
// Usage:
//   node tools/realdevice/phone_snapshot_restore_once.mjs --rundir <dir> --zip <snapshot.zip> --token <32hex>
import { appendFileSync, writeFileSync, mkdirSync, readFileSync } from "node:fs";
import path from "node:path";
import { chromium } from "playwright";

const argv = process.argv.slice(2);
const arg = (k) => { const i = argv.indexOf("--" + k); return i >= 0 ? argv[i + 1] : ""; };
const RUNDIR = path.resolve(arg("rundir") || ".");
const ZIP = path.resolve(arg("zip") || "");
const TOKEN = arg("token") || process.env.META_PASS_SESSION || "";
const IP = arg("ip") || "192.168.0.14";
mkdirSync(RUNDIR, { recursive: true });
if (!ZIP || !/^[a-f0-9]{32}$/i.test(TOKEN)) { console.error("--zip and --token required"); process.exit(2); }
const PAGE_URL = `http://${IP}/#s=${TOKEN}`;
const log = (m) => { console.log(m); appendFileSync(path.join(RUNDIR, "console-live.log"), m + "\n"); };
const writeJson = (n, v) => writeFileSync(path.join(RUNDIR, n), JSON.stringify(v, null, 2) + "\n");

const result = { startedAt: new Date().toISOString(), url: PAGE_URL.replace(TOKEN, "<redacted>"), zip: path.basename(ZIP), status: "RUNNING", failure_category: null };
const t0 = Date.now();

const browser = await chromium.launch({ headless: false, channel: "chrome", viewport: { width: 420, height: 900 } });
const page = await browser.newPage();
// The device shell loads phone-install.js from a CDN deploy, which lags repo
// HEAD. Fulfill the module graph from the local worktree so the restore under
// test is the current code (same approach as the dev server whitelist).
const LOCAL_MODULES = new Map([
  ["phone-install.js", "phone-install.js"],
  ["storage-snapshot.js", "storage-snapshot.js"],
  ["storage-snapshot-restore.js", "storage-snapshot-restore.js"],
  ["data-byte-archive.js", "data-byte-archive.js"],
  ["dynslot-pool.js", "dynslot-pool.js"],
  ["extract-app-image.js", "extract-app-image.js"],
  ["name-blob.js", "name-blob.js"],
  ["data-size-options.js", "data-size-options.js"],
  ["data-size-profile.js", "data-size-profile.js"],
  ["store-analyze.js", "store-analyze.js"],
]);
const MODULE_DIR = path.resolve(path.dirname(new URL(import.meta.url).pathname), "../../install-slot");
await page.route("https://metapass.chuanxilu.net/**", (route) => {
  const name = new URL(route.request().url()).pathname.replace(/^\//, "");
  const local = LOCAL_MODULES.get(name);
  if (!local) return route.abort();
  route.fulfill({
    status: 200,
    headers: {
      "content-type": "text/javascript; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
    },
    body: readFileSync(path.join(MODULE_DIR, local)),
  });
});
page.on("console", (m) => log(`[console.${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => log(`[pageerror] ${e.stack || e.message}`));
page.on("dialog", (d) => { log("[dialog] " + d.message().split("\n")[0] + " → accept"); d.accept().catch(() => {}); });

try {
  log(`goto ${result.url}`);
  await page.goto(PAGE_URL, { waitUntil: "domcontentloaded" });
  await page.waitForSelector("#mp-mgmt", { timeout: 20000 });
  await page.locator("#mp-mgmt").click();
  log("mgmt panel opened");
  await page.waitForSelector("#mp-restore-input", { state: "attached", timeout: 15000 });
  await page.locator("#mp-restore-input").setInputFiles(ZIP);
  log("zip attached, restore running…");
  await page.waitForFunction(
    () => {
      const el = document.getElementById("mp-backup-status");
      const t = el ? el.textContent : "";
      return /快照数据已写入|恢复尚未完成|预检失败|恢复未完成/.test(t);
    },
    null, { timeout: 900000 });
  result.restore_status_text = await page.locator("#mp-backup-status").textContent();
  result.restore_status_class = await page.locator("#mp-backup-status").getAttribute("class");
  log("restore final: " + result.restore_status_class + " — " + result.restore_status_text);
  result.status = /快照数据已写入/.test(result.restore_status_text) ? "PASS" : "FAIL";
  if (result.status === "FAIL") result.failure_category = "PHONE_SNAPSHOT_RESTORE";
  await page.screenshot({ path: path.join(RUNDIR, "final-screenshot.png") });
} catch (err) {
  result.status = "FAIL"; result.failure_category = result.failure_category || "DRIVER";
  result.error = String(err && err.stack || err);
  try { await page.screenshot({ path: path.join(RUNDIR, "failure-screenshot.png") }); } catch {}
} finally {
  result.duration_s = (Date.now() - t0) / 1000;
  writeJson("phone-restore-result.json", result);
  log(`status=${result.status} (${result.duration_s?.toFixed(1)}s)`);
  await browser.close();
}
process.exit(result.status === "PASS" ? 0 : 1);
