#!/usr/bin/env node
// Real-device phone-install page E2E using Playwright mobile viewport emulation.
// UI mutations go through the page; direct device API access is GET-only.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
import { parseSlots } from "../../install-slot/phone-install.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const LOGROOT = path.join(ROOT, "tools/realdevice/logs");
function argsOf(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const eq = a.indexOf("=");
    if (eq > 2) out[a.slice(2, eq)] = a.slice(eq + 1);
    else out[a.slice(2)] = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : true;
  }
  return out;
}
const args = argsOf(process.argv.slice(2));
if (!args["real-device"]) {
  console.error("Refusing to mutate hardware: pass --real-device explicitly.");
  process.exit(2);
}
const playA = String(args["play-a"] || process.env.MOBILE_E2E_PLAY_A || "1");
const playB = String(args["play-b"] || process.env.MOBILE_E2E_PLAY_B || "2");
const tokenArg = String(args.token || process.env.META_PASS_SESSION || "");
const urlArg = String(args.url || process.env.MOBILE_E2E_URL || "");
const viewport = { width: Number(args.width || 390), height: Number(args.height || 844) };
const timeout = Number(args.timeout || process.env.MOBILE_E2E_TIMEOUT_MS || 600000);
const logDir = path.resolve(String(args.logdir || path.join(LOGROOT, "mobile-page-e2e-" + new Date().toISOString().replace(/[:.]/g, "-"))));
if (!urlArg || !/^\d+$/.test(playA) || !/^\d+$/.test(playB) || playA === playB) {
  console.error("Usage: node tools/realdevice/mobile_page_e2e.mjs --real-device --url http://<device-ip>/ [--token <32hex>] [--play-a 1] [--play-b 2]");
  process.exit(2);
}
let target;
try { target = new URL(urlArg); } catch { console.error("--url must be an absolute device URL"); process.exit(2); }
if (!["http:", "https:"].includes(target.protocol) || target.username || target.password) {
  console.error("--url must be HTTP(S) and contain no embedded credentials"); process.exit(2);
}
const token = tokenArg || new URLSearchParams(target.hash.slice(1)).get("s") || "";
if (!/^[a-f0-9]{32}$/i.test(token)) {
  console.error("Provide a 32-hex session using --token/META_PASS_SESSION or URL fragment #s=...");
  process.exit(2);
}
// Always ensure the UI and read-only verifier use the same session. Never log target.href.
target.hash = "s=" + token;
const device = target.origin;
const suffix = Date.now().toString(36).slice(-6);
const nameA = ("MOB-A-" + suffix).slice(0, 32);
const nameB = ("MOB-B-" + suffix).slice(0, 32);
const ownedNames = new Set([nameA, nameB]);
const installAttempted = new Set();
fs.mkdirSync(logDir, { recursive: true });

const report = {
  test: "Real-device phone-install page E2E (Playwright mobile emulation)",
  startedAt: new Date().toISOString(),
  browser: "Chromium",
  mobileEmulation: { ...viewport, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
  device: "redacted",
  playIds: [Number(playA), Number(playB)],
  testNames: [nameA, nameB],
  verdict: "FAIL",
  results: [],
};
let browser, context, page, baseline;
const pageErrors = [], consoleErrors = [], failedRequests = [];
function redact(value) {
  return String(value)
    .replace(/\b[a-f0-9]{32}\b/gi, "<redacted-token>")
    .replace(/https?:\/\/[^\s"'<>]+/g, (u) => u.replace(/#s=[^&\s]+/i, "#s=<redacted>"))
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, "<device-ip>");
}
function saveJson(file, value) { fs.writeFileSync(path.join(logDir, file), JSON.stringify(value, null, 2) + "\n"); }
function record(name, ok, detail = "") {
  report.results.push({ name, ok, detail: redact(detail), at: new Date().toISOString() });
  console.log(`[${ok ? "PASS" : "FAIL"}] ${name}${detail ? " — " + redact(detail) : ""}`);
  if (!ok) throw new Error(`${name}${detail ? ": " + redact(detail) : ""}`);
}
async function getDevice(route) {
  const r = await fetch(device + route, { headers: { "X-Meta-Session": token }, signal: AbortSignal.timeout(45000) });
  const body = await r.text();
  if (!r.ok) throw new Error(`GET ${route} HTTP ${r.status}: ${body.slice(0, 160)}`);
  return body;
}
async function readSlots() {
  const parsed = parseSlots(await getDevice("/api/install/slots"));
  if (!parsed || !Array.isArray(parsed.slots)) throw new Error("invalid device slots response");
  return parsed;
}
async function readStatus() {
  try { return JSON.parse(await getDevice("/api/install/status")); }
  catch (e) { throw new Error("invalid installer status: " + e.message); }
}
function stableSlots(v) {
  return {
    count: v.count, free: v.free, protocolVersion: v.protocolVersion,
    slots: v.slots.map((s) => ({ slot: s.slot, state: s.state, name: s.name, size: s.size,
      len: s.len, limit: s.limit, offset: s.offset, kind: s.kind, arc: s.arc })),
    data: (v.data || []).map((d) => ({ offset: d.offset, size: d.size, state: d.state, play_id: d.play_id, label: d.label })),
  };
}
function assertIdle(status, label) {
  const ok = status.protocol === 1 && !status.active && !status.session && !status.offer && !status.confirmed;
  record(label + ": installer idle", ok, `protocol=${status.protocol}; active=${Boolean(status.active)}; session=${Boolean(status.session)}`);
}
async function waitFor(fn, label, ms = timeout) {
  const end = Date.now() + ms;
  let last;
  while (Date.now() < end) {
    try { const value = await fn(); if (value) return value; } catch (e) { last = e; }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`timeout waiting for ${label}${last ? ": " + last.message : ""}`);
}
async function dismissPanel() {
  const overlay = page.locator("#mp-overlay");
  if (await overlay.count()) {
    // The page itself closes the sheet when its backdrop receives a click.
    await overlay.evaluate((el) => el.dispatchEvent(new MouseEvent("click", { bubbles: true })));
    await page.locator("#mp-overlay").waitFor({ state: "detached", timeout: 5000 }).catch(() => {});
  }
}
async function openManagement() {
  if (await page.locator("#mp-mgmt-re").isVisible().catch(() => false)) return;
  await dismissPanel();
  await page.locator("#mp-mgmt").click();
  await page.locator("#mp-mgmt-re").waitFor({ state: "visible", timeout: 15000 });
}
async function removeByName(name) {
  const now = await readSlots();
  const current = now.slots.find((s) => s.name === name);
  if (!current) return;
  await openManagement();
  let button = page.locator(`[data-rm="${current.slot}"]`);
  await button.waitFor({ state: "visible", timeout: 10000 });
  await button.click();
  button = page.locator(`[data-rm="${current.slot}"]`);
  await button.click(); // second click is the page's deliberate two-step guard
  await page.locator("#mp-mgmt-confirm").waitFor({ state: "visible", timeout: 10000 });
  const erase = page.locator("#mp-rm-erase");
  if (await erase.count()) await erase.check();
  await page.locator("#mp-mgmt-confirm").click();
  await waitFor(async () => !(await readSlots()).slots.some((s) => s.name === name),
    "device API confirms UI removal of " + name, 90000);
  record("UI removed test slot", true, `name=${name}; originalSlot=${current.slot}`);
}
async function installPlay(playId, name, label) {
  await page.locator("#mp-q").fill(String(playId));
  await page.locator("#mp-go").click();
  await page.locator("#mp-install").waitFor({ state: "visible", timeout: 60000 });
  record(label + ": M03 search by ID and load detail", true, `playId=${playId}`);
  await page.locator("#mp-install").click();
  await page.locator("#mp-name").waitFor({ state: "visible", timeout: 60000 });
  await page.locator("#mp-name").fill(name);
  await page.locator("#mp-confirm").waitFor({ state: "visible", timeout: 10000 });
  const fitCount = await page.locator(".mp-slot:not([disabled])").count();
  if (fitCount < 1 || await page.locator("#mp-confirm").isDisabled()) throw new Error("no installable slot or disabled confirmation");
  record(label + ": slot and name selected", true, `fitChoices=${fitCount}; name=${name}`);
  installAttempted.add(name);
  await page.locator("#mp-confirm").click();
  const outcome = await waitFor(async () => {
    const stage = (await page.locator("#mp-stage").textContent().catch(() => "")) || "";
    const status = (await page.locator("#mp-status").textContent().catch(() => "")) || "";
    const panel = (await page.locator("#mp-panel").innerText().catch(() => "")) || "";
    if (/安装失败|无法安装|预检失败/.test(panel) || /安装失败|无法安装/.test(status)) return "FAIL:" + (panel || status).slice(0, 240);
    if (stage.includes("完成") || /已安装完成/.test(status)) return "PASS";
    return null;
  }, label + " UI install completion");
  if (outcome.startsWith("FAIL:")) throw new Error(outcome);
  record(label + ": UI reports install complete", true, `playId=${playId}; name=${name}`);
  const slot = await waitFor(async () => (await readSlots()).slots.find((s) => s.name === name && s.state === "valid"), label + " device slot commit", 120000);
  assertIdle(await readStatus(), label);
  await dismissPanel();
  return slot;
}
async function cleanupOwned() {
  if (!baseline || !page) return;
  for (const name of installAttempted) {
    try { await removeByName(name); }
    catch (e) { report.results.push({ name: "cleanup incomplete: " + name, ok: false, detail: redact(e.message) }); }
  }
}

try {
  browser = await chromium.launch({
    headless: args.headless === "false" ? false : true,
    ...(args["browser-channel"] ? { channel: String(args["browser-channel"]) } : {}),
  });
  context = await browser.newContext({
    viewport, deviceScaleFactor: 3, isMobile: true, hasTouch: true,
    userAgent: "Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36",
  });
  page = await context.newPage();
  page.setDefaultTimeout(15000);
  page.on("pageerror", (e) => pageErrors.push(redact(e.message)));
  page.on("console", (m) => { if (m.type() === "error") consoleErrors.push(redact(m.text())); });
  page.on("requestfailed", (r) => failedRequests.push(redact(`${r.method()} ${r.url()} :: ${r.failure()?.errorText || "failed"}`)));

  await page.goto(target.toString(), { waitUntil: "domcontentloaded", timeout: 60000 });
  await page.locator("#mp-install-root").waitFor({ state: "attached", timeout: 60000 });
  await page.locator("#mp-q").waitFor({ state: "visible", timeout: 15000 });
  await waitFor(async () => {
    const s = (await page.locator("#mp-status").textContent().catch(() => "")) || "";
    return s.length > 0 && !s.includes("正在加载玩法目录");
  }, "phone page initialization", 90000);
  const layout = await page.evaluate(() => ({
    width: innerWidth, height: innerHeight, documentWidth: document.documentElement.scrollWidth,
    rootWidth: document.querySelector("#mp-install-root")?.getBoundingClientRect().width || 0,
    title: document.title, touch: navigator.maxTouchPoints, mobileUA: /Android|Mobile/i.test(navigator.userAgent),
  }));
  record("M01 mobile viewport page loaded", layout.mobileUA && layout.touch > 0,
    `viewport=${layout.width}x${layout.height}; rootWidth=${layout.rootWidth}; title=${layout.title}; touch=${layout.touch}`);
  record("M01 no horizontal page overflow", layout.documentWidth <= layout.width + 1,
    `documentWidth=${layout.documentWidth}; viewportWidth=${layout.width}`);
  if (pageErrors.length) throw new Error("page JavaScript error: " + pageErrors[0]);

  baseline = await readSlots();
  const baselineStatus = await readStatus();
  saveJson("baseline-slots.json", stableSlots(baseline));
  saveJson("baseline-status.json", baselineStatus);
  assertIdle(baselineStatus, "M02 baseline");
  record("M02 baseline slots read", true, `slots=${baseline.slots.length}; free=${baseline.free}`);
  if (baseline.slots.some((s) => ownedNames.has(s.name))) throw new Error("test-name collision with existing slot");

  const a = await installPlay(playA, nameA, "M04 install A");
  const afterA = await readSlots();
  saveJson("after-install-a.json", stableSlots(afterA));
  record("M04 install A independently verified", afterA.slots.some((s) => s.name === nameA && s.state === "valid"));
  const b = await installPlay(playB, nameB, "M05 install B");
  const afterB = await readSlots();
  saveJson("after-install-b.json", stableSlots(afterB));
  record("M05 both plays coexist", afterB.slots.some((s) => s.name === nameA && s.state === "valid") &&
    afterB.slots.some((s) => s.name === nameB && s.state === "valid"), `slotA=${a.slot}; slotB=${b.slot}`);

  await removeByName(nameA);
  record("M06 B remains valid after removing A", (await readSlots()).slots.some((s) => s.name === nameB && s.state === "valid"));
  await removeByName(nameB);
  const finalSlots = await readSlots();
  const finalStatus = await readStatus();
  saveJson("final-slots.json", stableSlots(finalSlots));
  saveJson("final-status.json", finalStatus);
  assertIdle(finalStatus, "M08 final");
  const restored = JSON.stringify(stableSlots(finalSlots)) === JSON.stringify(stableSlots(baseline));
  record("M08 device state restored to baseline", restored,
    `baselineSlots=${baseline.slots.length}; finalSlots=${finalSlots.slots.length}; baselineFree=${baseline.free}; finalFree=${finalSlots.free}`);
  record("M09 no unhandled page errors", pageErrors.length === 0, `count=${pageErrors.length}`);
  report.verdict = report.results.every((r) => r.ok) ? "PASS" : "FAIL";
} catch (e) {
  report.results.push({ name: "fatal", ok: false, detail: redact(e?.stack || e?.message || String(e)), at: new Date().toISOString() });
  console.error("[FAIL] fatal — " + redact(e?.message || String(e)));
} finally {
  if (report.verdict !== "PASS") await cleanupOwned();
  report.finishedAt = new Date().toISOString();
  report.browserErrors = pageErrors;
  report.consoleErrors = consoleErrors.slice(0, 100);
  report.failedRequests = failedRequests.slice(0, 100);
  try {
    if (page) {
      await page.screenshot({ path: path.join(logDir, "final-page.png"), fullPage: true, timeout: 15000 });
      fs.writeFileSync(path.join(logDir, "page-source.html"), redact(await page.content()));
    }
  } catch {}
  saveJson("report.json", report);
  fs.writeFileSync(path.join(logDir, "report.txt"),
    [`Mobile page E2E: ${report.verdict}`, `viewport: ${viewport.width}x${viewport.height}`,
      ...report.results.map((r) => `${r.ok ? "PASS" : "FAIL"} | ${r.name} | ${r.detail || ""}`),
      `page errors: ${pageErrors.length}`, `console errors: ${consoleErrors.length}`,
      `failed requests: ${failedRequests.length}`].join("\n") + "\n");
  try { await context?.close(); } catch {}
  try { await browser?.close(); } catch {}
  console.log(`REPORT_DIR=${logDir}`);
  console.log(`VERDICT=${report.verdict}`);
}
if (report.verdict !== "PASS") process.exitCode = 1;
