#!/usr/bin/env node
// Real-device phone-install page E2E using direct CDP; supports a hardware runtime driver or explicit manual-assisted checkpoints.
// UI mutations go through the page; direct device API access is GET-only.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import readline from "node:readline/promises";
import { stdin as input, stdout as output } from "node:process";
import { CdpMobilePage } from "./cdp_mobile_page.mjs";
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
const requireDataReservation = Boolean(args["require-data-reservation"]);
const runtimeDriver = String(args["runtime-driver"] || process.env.MOBILE_E2E_RUNTIME_DRIVER || "");
const manualAssist = Boolean(args["manual-assist"]);
if (!runtimeDriver && !manualAssist) {
  console.error("Provide --runtime-driver for device-side automated verification, or --manual-assist for explicitly limited human-assisted coverage.");
  process.exit(2);
}
if (runtimeDriver && manualAssist) {
  console.error("Choose one runtime mode: --runtime-driver or --manual-assist, not both.");
  process.exit(2);
}
if (runtimeDriver) {
  const driverPath = path.resolve(runtimeDriver);
  if (runtimeDriver.toLowerCase().endsWith(".py")) {
    if (!fs.existsSync(driverPath)) {
      console.error("Runtime driver does not exist: " + driverPath);
      process.exit(2);
    }
    const serialPort = String(process.env.META_PASS_E2E_SERIAL_PORT || "").trim();
    if (!serialPort) {
      console.error("Set META_PASS_E2E_SERIAL_PORT to the launcher's native USB Serial/JTAG port before any device mutation.");
      process.exit(2);
    }
    if ((path.isAbsolute(serialPort) || serialPort.includes(path.sep)) && !fs.existsSync(serialPort)) {
      console.error("META_PASS_E2E_SERIAL_PORT does not exist; refusing device mutation.");
      process.exit(2);
    }
    const python = process.env.PYTHON || "python3";
    const serialCheck = spawnSync(python, ["-c", "import serial"], { encoding: "utf8", timeout: 10000 });
    if (serialCheck.error || serialCheck.status !== 0) {
      console.error("Runtime driver requires pyserial in " + python + "; install it before running E2E.");
      process.exit(2);
    }
  } else if (runtimeDriver.includes(path.sep) && !fs.existsSync(driverPath)) {
    console.error("Runtime driver does not exist: " + driverPath);
    process.exit(2);
  }
}
if (manualAssist && (!process.stdin.isTTY || !process.stdout.isTTY)) {
  console.error("--manual-assist requires an interactive terminal.");
  process.exit(2);
}
const runtimeTimeout = Number(args["runtime-timeout"] || process.env.MOBILE_E2E_RUNTIME_TIMEOUT_MS || 180000);
const tokenArg = String(args.token || process.env.META_PASS_SESSION || "");
const urlArg = String(args.url || process.env.MOBILE_E2E_URL || "");
const viewport = { width: Number(args.width || 390), height: Number(args.height || 844) };
const viewportMatrix = String(args.viewports || "320x720,360x800,390x844,430x932")
  .split(",").map((item) => {
    const match = item.trim().match(/^(\d+)x(\d+)$/);
    if (!match || Number(match[1]) < 240 || Number(match[2]) < 320) {
      console.error(`Invalid --viewports item: ${item}; expected widthxheight with width>=240 and height>=320`);
      process.exit(2);
    }
    return { width: Number(match[1]), height: Number(match[2]) };
  });
const timeout = Number(args.timeout || process.env.MOBILE_E2E_TIMEOUT_MS || 600000);
const cdpUrl = String(args["cdp-url"] || process.env.MOBILE_E2E_CDP_URL || "http://127.0.0.1:9222");
const logDir = path.resolve(String(args.logdir || path.join(LOGROOT, "mobile-page-e2e-" + new Date().toISOString().replace(/[:.]/g, "-"))));
if (!urlArg || !/^\d+$/.test(playA) || !/^\d+$/.test(playB) || playA === playB) {
  console.error("Usage: node tools/realdevice/mobile_page_e2e.mjs --real-device --url http://<device-ip>/ (--runtime-driver <path> | --manual-assist) [--token <32hex>] [--play-a 1] [--play-b 2]");
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
const nameC = ("MOB-C-" + suffix).slice(0, 32);
const ownedNames = new Set([nameA, nameB, nameC]);
const installAttempted = new Set();
fs.mkdirSync(logDir, { recursive: true });

const report = {
  test: "Real-device phone-install + child runtime + DATA lifecycle E2E",
  runtimeMode: manualAssist ? "manual-assisted" : "hardware-driver",
  coverageGaps: manualAssist ? [
    { scenario: "child DATA write/read/checksum/reboot persistence", status: "NOT_COVERED", reason: "No dedicated test firmware protocol/runtime driver." },
    { scenario: "deleted firmware non-bootability", status: "NOT_COVERED", reason: "Slot absence does not prove the flash image cannot boot." }
  ] : [],
  notCovered: [],
  runtimeDriver: runtimeDriver ? path.basename(runtimeDriver) : null,
  startedAt: new Date().toISOString(),
  browser: "Chromium via direct CDP",
  cdpEndpoint: cdpUrl.replace(/:\/\/[^/]+/, "://<redacted-host>"),
  mobileEmulation: { ...viewport, viewportMatrix, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
  device: "redacted",
  playIds: [Number(playA), Number(playB), Number(playA)],
  testNames: [nameA, nameB, nameC],
  verdict: "FAIL",
  results: [],
};
let page, baseline;
let freshReservationsForB = [];
const pageErrors = [], consoleErrors = [], failedRequests = [];
function redact(value) {
  return String(value)
    .replace(/\b[a-f0-9]{32}\b/gi, "<redacted-token>")
    .replace(/https?:\/\/[^\s"'<>]+/g, (u) => u.replace(/#s=[^&\s]+/i, "#s=<redacted>"))
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, "<device-ip>");
}
function saveJson(file, value) { fs.writeFileSync(path.join(logDir, file), JSON.stringify(value, null, 2) + "\n"); }
function record(name, ok, detail = "", evidence = "automated") {
  report.results.push({ name, ok, evidence, detail: redact(detail), at: new Date().toISOString() });
  console.log(`[${ok ? "PASS" : "FAIL"}] ${name}${detail ? " — " + redact(detail) : ""}`);
}
async function runCase(name, fn) {
  try { return { ok: true, value: await fn() }; }
  catch (error) {
    const detail = redact(error?.stack || error?.message || String(error));
    report.results.push({ name: name + " (uncaught case error)", ok: false, evidence: "automated", detail, at: new Date().toISOString() });
    console.error("[FAIL] " + name + " — " + redact(error?.message || String(error)));
    return { ok: false, value: null };
  }
}
async function getDevice(route) {
  // Execute through the actual page origin and Chromium request stack, matching
  // phone-install.js rather than using a host-side HTTP client. GET is retried
  // only on transport failure because slot deletion deliberately reboots device.
  const backoff = [0, 250, 500, 1000, 2000, 4000, 8000];
  let lastError;
  for (const delay of backoff) {
    if (delay) await new Promise((resolve) => setTimeout(resolve, delay));
    try {
      const result = await page.evaluate(async ({ route, token }) => {
        const response = await fetch(route, {
          headers: token ? { "X-Meta-Session": token } : {},
          signal: AbortSignal.timeout(45000),
        });
        return { status: response.status, body: await response.text() };
      }, { route, token });
      if (result.status < 200 || result.status >= 300) {
        throw new Error("GET " + route + " HTTP " + result.status + ": " + result.body.slice(0, 160));
      }
      return result.body;
    } catch (error) {
      lastError = error;
      if (/HTTP \d+/.test(error.message || "")) throw error;
    }
  }
  throw new Error("GET " + route + " failed after device-reboot retries: " + (lastError?.message || lastError));
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

// Independent storage oracle: slots and child DATA reservations must fit inside
// allocator pools and must never overlap one another.
const POOL_SEGMENTS = [{ start: 0x180000, end: 0x356000 }, { start: 0x360000, end: 0x7fe000 }];
const POOL_BYTES = POOL_SEGMENTS.reduce((n, p) => n + p.end - p.start, 0);
function assertDynamicSlotGeometry(listing, label) {
  if (listing.protocolVersion !== 2) {
    record(label + ": dynamic-slot protocol", false, "expected protocol_version=2");
    return false;
  }
  const occupied = [
    ...listing.slots.map((s) => ({ type: "slot", id: s.slot, offset: s.offset, size: s.size })),
    ...(listing.data || []).map((d, i) => ({ type: "data", id: d.play_id || i, offset: d.offset, size: d.size })),
  ].sort((a, b) => a.offset - b.offset);
  let ok = true;
  const errors = [];
  for (const item of occupied) {
    if (!POOL_SEGMENTS.some((p) => item.offset >= p.start && item.offset + item.size <= p.end)) {
      ok = false; errors.push(item.type + "[" + item.id + "] outside pool");
    }
    if (!Number.isInteger(item.offset) || item.offset % 0x10000 !== 0) {
      ok = false; errors.push(item.type + "[" + item.id + "] offset alignment");
    }
    if (!Number.isInteger(item.size) || item.size <= 0 || item.size % 0x1000 !== 0) {
      ok = false; errors.push(item.type + "[" + item.id + "] size granularity");
    }
  }
  for (let i = 1; i < occupied.length; i++) {
    if (occupied[i - 1].offset + occupied[i - 1].size > occupied[i].offset) {
      ok = false; errors.push(occupied[i - 1].type + "[" + occupied[i - 1].id + "] overlaps " +
        occupied[i].type + "[" + occupied[i].id + "]");
    }
  }
  const expectedFree = POOL_BYTES - occupied.reduce((n, x) => n + x.size, 0);
  if (listing.free !== expectedFree) {
    ok = false; errors.push("free mismatch: device=" + listing.free + "; calculated=" + expectedFree);
  }
  record(label + ": dynamic slot geometry", ok,
    "slots=" + listing.slots.length + "; dataReservations=" + (listing.data || []).length +
    "; free=" + listing.free + "; " + errors.join("; "));
  return ok;
}
function dataReservations(listing) {
  return (listing.data || []).map((d) => ({
    offset: d.offset, size: d.size, state: d.state, play_id: d.play_id, label: d.label,
  })).sort((a, b) => a.offset - b.offset || a.size - b.size);
}
function assertBaselineDataPreserved(listing, label) {
  const expected = dataReservations(baseline);
  const actual = dataReservations(listing);
  const missing = expected.filter((d) => !actual.some((x) =>
    x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
  const ok = missing.length === 0;
  record(label + ": baseline data reservations preserved", ok,
    missing.length ? JSON.stringify(missing) : "baseline=" + expected.length + "; current=" + actual.length);
  return ok;
}
function assertBaselineAppSlotsPreserved(listing, label) {
  const expected = baseline.slots.filter((slot) => slot.state === "valid");
  const missing = expected.filter((slot) => !listing.slots.some((current) =>
    current.slot === slot.slot && current.state === slot.state && current.name === slot.name &&
    current.offset === slot.offset && current.size === slot.size));
  const ok = missing.length === 0;
  record(label + ": baseline APP slots preserved", ok,
    missing.length ? JSON.stringify(missing.map((slot) => ({
      slot: slot.slot, name: slot.name, offset: slot.offset, size: slot.size,
    }))) : "baselineValidSlots=" + expected.length);
  return ok;
}
function requireSafeStorageState(listing, label) {
  const geometryOk = assertDynamicSlotGeometry(listing, label);
  const baselineSlotsOk = assertBaselineAppSlotsPreserved(listing, label);
  const baselineDataOk = assertBaselineDataPreserved(listing, label);
  if (!geometryOk || !baselineSlotsOk || !baselineDataOk) {
    throw new Error(label + ": storage safety invariant failed; refusing subsequent mutations");
  }
}

function assertIdle(status, label) {
  const ok = status.protocol === 1 && !status.active && !status.session && !status.offer && !status.confirmed;
  record(label + ": installer idle", ok, `protocol=${status.protocol}; active=${Boolean(status.active)}; session=${Boolean(status.session)}`);
  return ok;
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
async function removeByName(name, stage = "UI") {
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
  record(`${stage}: UI removed test slot`, true, `name=${name}; originalSlot=${current.slot}`);
}
async function cancelRemoveByName(name, stage = "UI") {
  // A cancelled uninstall must be a strict no-op across both slot metadata and
  // DATA reservations. This guards the UI's destructive-confirmation boundary.
  const beforeSlots = stableSlots(await readSlots());
  const beforeStatus = await readStatus();
  const current = beforeSlots.slots.find((s) => s.name === name);
  if (!current) throw new Error("cannot test delete cancellation: slot not found: " + name);
  await openManagement();
  const button = page.locator(`[data-rm="${current.slot}"]`);
  await button.waitFor({ state: "visible", timeout: 10000 });
  await button.click();
  await button.click(); // enter the page's deliberate two-step delete confirmation
  await page.locator("#mp-mgmt-confirm").waitFor({ state: "visible", timeout: 10000 });
  await page.locator("#mp-mgmt-x").click();
  await page.locator("#mp-mgmt-confirm").waitFor({ state: "detached", timeout: 10000 });
  const afterSlots = stableSlots(await readSlots());
  const afterStatus = await readStatus();
  const unchanged = JSON.stringify(afterSlots) === JSON.stringify(beforeSlots);
  const idle = beforeStatus.protocol === afterStatus.protocol &&
    !afterStatus.active && !afterStatus.session && !afterStatus.offer && !afterStatus.confirmed;
  record(stage + ": cancel uninstall is a no-op", unchanged && idle,
    "slot=" + current.slot + "; slotsUnchanged=" + unchanged + "; installerIdle=" + idle);
  if (!unchanged || !idle) throw new Error("cancelled uninstall mutated device state");
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
async function manualRuntimeCheckpoint(slot, playId, name, phase) {
  const rl = readline.createInterface({ input, output });
  try {
    console.log("\n[MANUAL ACTION REQUIRED] " + phase);
    console.log("设备：FoloToy AI Passport；测试槽位名称：" + name + "；玩法 ID：" + playId + "；槽位：" + slot.slot);
    console.log("1. 查看设备屏幕，使用实体 UP/DOWN 键选择本轮刚安装的玩法；按实体 OK 键启动。");
    console.log("2. 确认屏幕确实进入该玩法，而不是仍停留在启动器。若测试固件有自检页，记录其显示结果；不要把仅显示玩法名称当成 DATA 验证通过。");
    console.log("3. 使用设备当前支持的返回/退出方式回到启动器。不要通过网页 API 或安装写接口代替设备操作。");
    console.log("4. 等待设备重新提供 USB 服务页面，然后在下方输入 PASS；若无法启动、无法返回或结果不明确，输入 FAIL。");
    const answer = (await rl.question("人工检查结果 [PASS/FAIL]: ")).trim().toUpperCase();
    if (answer !== "PASS") throw new Error("manual checkpoint failed or was not confirmed: " + phase);
  } finally {
    rl.close();
  }
  record(phase + ": human confirmed physical-button launch and return", true,
    "slot=" + slot.slot + "; playId=" + playId + "; human confirmation; DATA behavior not claimed",
    "manual-confirmed");
  await page.goto(target.toString(), { timeout: 90000 });
  await page.locator("#mp-install-root").waitFor({ state: "attached", timeout: 90000 });
  await page.locator("#mp-q").waitFor({ state: "visible", timeout: 30000 });
  await waitFor(async () => {
    const status = (await page.locator("#mp-status").textContent().catch(() => "")) || "";
    return status.length > 0 && !status.includes("正在加载玩法目录");
  }, phase + " launcher page recovered after manual checkpoint", 90000);
}
function spawnRuntimeDriver(driverArgs) {
  if (runtimeDriver.toLowerCase().endsWith(".py")) {
    return spawnSync(process.env.PYTHON || "python3", [path.resolve(runtimeDriver), ...driverArgs], {
      encoding: "utf8", timeout: runtimeTimeout + 15000, maxBuffer: 2 * 1024 * 1024,
    });
  }
  return spawnSync(runtimeDriver, driverArgs, {
    encoding: "utf8", timeout: runtimeTimeout + 15000, maxBuffer: 2 * 1024 * 1024,
  });
}
async function runChildRuntime(slot, playId, name, phase) {
  if (manualAssist) return manualRuntimeCheckpoint(slot, playId, name, phase);
  return runRuntimeDriver(slot, playId, name, phase);
}
async function runRuntimeDriver(slot, playId, name, phase) {
  const safePhase = phase.toLowerCase().replace(/[^a-z0-9_-]/g, "_");
  const evidenceFile = path.join(logDir, "runtime-" + safePhase + ".json");
  const driverArgs = ["--action", "boot-test-and-return", "--slot", String(slot.slot),
    "--play-id", String(playId), "--slot-name", name, "--phase", phase,
    "--timeout-ms", String(runtimeTimeout), "--evidence-file", evidenceFile];
  const result = spawnRuntimeDriver(driverArgs);
  if (result.error) throw new Error("runtime driver " + phase + " failed to start: " + result.error.message);
  if (result.status !== 0) throw new Error("runtime driver " + phase + " exit=" + result.status + "; stderr=" + redact(result.stderr || "").slice(0, 500));
  let evidence;
  try { evidence = JSON.parse(fs.readFileSync(evidenceFile, "utf8")); }
  catch (e) { throw new Error("runtime driver " + phase + " did not provide valid JSON evidence: " + e.message); }
  const required = ["childBooted", "dataEraseOk", "dataWriteOk", "dataReadOk", "dataChecksumOk", "dataPersistedAfterReboot", "returnedToLauncher"];
  const missing = required.filter((key) => evidence[key] !== true);
  if (typeof evidence.serialEvidence !== "string" || !evidence.serialEvidence.trim()) missing.push("serialEvidence");
  saveJson("runtime-" + safePhase + "-evidence.json", evidence);
  record(phase + ": child firmware executed and DATA verified", missing.length === 0,
    missing.length ? "missing/false evidence: " + missing.join(",") :
      "slot=" + slot.slot + "; playId=" + playId + "; DATA label=" + (evidence.dataLabel || "reported by driver") + "; serial evidence=" + Boolean(evidence.serialEvidence));
  if (missing.length) throw new Error("runtime evidence failed for " + phase + ": " + missing.join(", "));
  // Child execution/reboot takes the device HTTP service down temporarily.
  // Re-navigate the same CDP tab only after the driver proves launcher recovery.
  await page.goto(target.toString(), { timeout: 90000 });
  await page.locator("#mp-install-root").waitFor({ state: "attached", timeout: 90000 });
  await page.locator("#mp-q").waitFor({ state: "visible", timeout: 30000 });
  await waitFor(async () => {
    const status = (await page.locator("#mp-status").textContent().catch(() => "")) || "";
    return status.length > 0 && !status.includes("正在加载玩法目录");
  }, phase + " launcher page recovered", 90000);
  return evidence;
}
function verifyDeletedRuntime(name, playId, phase) {
  const evidenceFile = path.join(logDir, "runtime-" + phase.toLowerCase() + "-delete-evidence.json");
  const result = spawnRuntimeDriver(["--action", "verify-deleted", "--slot-name", name,
    "--play-id", String(playId), "--timeout-ms", String(runtimeTimeout), "--evidence-file", evidenceFile]);
  if (result.error || result.status !== 0) {
    throw new Error("runtime driver delete verification " + phase + " failed: " +
      (result.error?.message || result.stderr || result.status));
  }
  let evidence;
  try { evidence = JSON.parse(fs.readFileSync(evidenceFile, "utf8")); }
  catch (e) { throw new Error("invalid delete evidence " + phase + ": " + e.message); }
  saveJson("runtime-" + phase.toLowerCase() + "-delete-evidence.json", evidence);
  const ok = evidence.deletedSlotNotBootable === true && evidence.dataPartitionReleased === true;
  record(phase + ": deleted firmware cannot boot and DATA is released", ok,
    "slotNotBootable=" + evidence.deletedSlotNotBootable + "; dataReleased=" + evidence.dataPartitionReleased);
  if (!ok) throw new Error("delete postcondition failed for " + phase);
}
async function cleanupOwned() {
  if (!baseline || !page) return;
  for (const name of installAttempted) {
    try { await removeByName(name, "recovery cleanup"); }
    catch (e) { report.results.push({ name: "cleanup incomplete: " + name, ok: false, detail: redact(e.message) }); }
  }
}

try {
  page = new CdpMobilePage({ endpoint: cdpUrl, viewport, timeout: 15000 });
  await page.connect();
  page.setDefaultTimeout(15000);
  page.on("pageerror", (e) => pageErrors.push(redact(e.message)));
  page.on("console", (m) => { if (m.type() === "error") consoleErrors.push(redact(m.text())); });
  page.on("requestfailed", (r) => failedRequests.push(redact(r.method() + " " + r.url() + " :: " + (r.failure()?.errorText || "failed"))));

  await page.goto(target.toString(), { waitUntil: "domcontentloaded", timeout: 60000 });
  await page.locator("#mp-install-root").waitFor({ state: "attached", timeout: 60000 });
  await page.locator("#mp-q").waitFor({ state: "visible", timeout: 15000 });
  await waitFor(async () => {
    const status = (await page.locator("#mp-status").textContent().catch(() => "")) || "";
    return status.length > 0 && !status.includes("正在加载玩法目录");
  }, "phone page initialization", 90000);

  // Generic embedded-WebView checks retained from the previous runner.
  const layout = await page.evaluate(() => ({
    width: innerWidth, height: innerHeight, documentWidth: document.documentElement.scrollWidth,
    rootWidth: document.querySelector("#mp-install-root")?.getBoundingClientRect().width || 0,
    title: document.title, bodyTextLength: (document.body?.innerText || "").trim().length,
    interactiveCount: document.querySelectorAll("button,input,select,textarea,[role=button]").length,
    touch: navigator.maxTouchPoints, mobileUA: /Android|Mobile/i.test(navigator.userAgent),
    readyState: document.readyState,
    controls: ["#mp-q", "#mp-go", "#mp-mgmt"].map((selector) => {
      const el = document.querySelector(selector);
      return { selector, exists: Boolean(el), visible: Boolean(el && el.getBoundingClientRect().width && el.getBoundingClientRect().height) };
    }),
  }));
  record("M01 document ready and meaningful content", ["interactive", "complete"].includes(layout.readyState) && layout.bodyTextLength > 0 && layout.interactiveCount > 0,
    "readyState=" + layout.readyState + "; bodyTextLength=" + layout.bodyTextLength + "; interactiveCount=" + layout.interactiveCount);
  record("M01 required search and management controls visible", layout.controls.every((c) => c.exists && c.visible), JSON.stringify(layout.controls));
  record("M01 mobile viewport page loaded", layout.mobileUA && layout.touch > 0,
    "viewport=" + layout.width + "x" + layout.height + "; rootWidth=" + layout.rootWidth + "; title=" + layout.title + "; touch=" + layout.touch);
  record("M01 no horizontal page overflow", layout.documentWidth <= layout.width + 1,
    "documentWidth=" + layout.documentWidth + "; viewportWidth=" + layout.width);
  for (const size of viewportMatrix) {
    await page.setViewportSize(size);
    await page.waitForTimeout(100);
    const responsive = await page.evaluate(() => ({
      width: innerWidth, height: innerHeight, documentWidth: document.documentElement.scrollWidth,
      rootWidth: document.querySelector("#mp-install-root")?.getBoundingClientRect().width || 0,
    }));
    record("M01 responsive layout " + size.width + "x" + size.height,
      responsive.width === size.width && responsive.documentWidth <= responsive.width + 1 &&
      responsive.rootWidth > 0 && responsive.rootWidth <= responsive.width + 1,
      "inner=" + responsive.width + "x" + responsive.height + "; documentWidth=" + responsive.documentWidth + "; rootWidth=" + responsive.rootWidth);
  }
  await page.setViewportSize(viewport);
  record("M01 no unhandled page errors after initial load", pageErrors.length === 0, "count=" + pageErrors.length);
  record("M01 no console errors after initial load", consoleErrors.length === 0, "count=" + consoleErrors.length);
  record("M01 no failed network requests after initial load", failedRequests.length === 0, "count=" + failedRequests.length);

  // Baseline is a safety gate: never mutate unless the device is readable and idle.
  baseline = await readSlots();
  const baselineStatus = await readStatus();
  saveJson("baseline-slots.json", stableSlots(baseline));
  saveJson("baseline-status.json", baselineStatus);
  const baselineIdle = assertIdle(baselineStatus, "M02 baseline");
  const baselineGeometry = assertDynamicSlotGeometry(baseline, "M02 baseline");
  if (!baselineIdle || !baselineGeometry) throw new Error("baseline safety checks failed; refusing to mutate device");
  saveJson("baseline-data-reservations.json", dataReservations(baseline));
  record("M02 baseline slots read", true, "slots=" + baseline.slots.length + "; free=" + baseline.free);
  if (baseline.slots.some((slot) => ownedNames.has(slot.name))) throw new Error("test-name collision with existing slot; refusing to mutate device");

  // Each phase is isolated. A failure is recorded and later independent cases still run.
  let a = null, b = null, afterA = null, afterB = null, afterRemoveA = null;
  const installA = await runCase("M04 install A", async () => {
    const slot = await installPlay(playA, nameA, "M04 install A");
    await cancelRemoveByName(nameA, "M04A");
    const listing = await readSlots();
    saveJson("after-install-a.json", stableSlots(listing));
    const aValid = listing.slots.some((x) => x.name === nameA && x.state === "valid");
    record("M04 install A independently verified", aValid);
    if (!aValid) throw new Error("device API did not confirm install A as VALID");
    requireSafeStorageState(listing, "M04 after install A");
    if (requireDataReservation) {
      const oldData = dataReservations(baseline);
      const fresh = dataReservations(listing).filter((d) => d.play_id === Number(playA) &&
        !oldData.some((x) => x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
      record("M04 child-firmware A DATA reservation created", fresh.length > 0, "newReservations=" + fresh.length);
      if (fresh.length === 0) throw new Error("required DATA reservation for child A was not created");
    }
    return { slot, listing };
  });
  if (installA.ok) { a = installA.value.slot; afterA = installA.value.listing; }
  let runtimeAResult = null;
  if (a) runtimeAResult = await runCase("M04A runtime A", () => runChildRuntime(a, playA, nameA, "M04A"));
  else record("M04A runtime A", false, "blocked: install A did not complete");

  const installB = a && runtimeAResult?.ok
    ? await runCase("M05 install B", async () => {
    const slot = await installPlay(playB, nameB, "M05 install B");
    const listing = await readSlots();
    saveJson("after-install-b.json", stableSlots(listing));
    const coexist = listing.slots.some((x) => x.name === nameA && x.state === "valid") &&
      listing.slots.some((x) => x.name === nameB && x.state === "valid");
    record("M05 both plays coexist", coexist, "slotA=" + (a ? a.slot : "missing") + "; slotB=" + slot.slot);
    if (!coexist) throw new Error("device API did not confirm A/B coexistence");
    requireSafeStorageState(listing, "M05 after install B");
    const oldData = dataReservations(baseline);
    freshReservationsForB = dataReservations(listing).filter((d) => d.play_id === Number(playB) &&
      !oldData.some((x) => x.offset === d.offset && x.size === d.size &&
        x.play_id === d.play_id && x.label === d.label));
    if (requireDataReservation) {
      record("M05 child-firmware DATA reservation created", freshReservationsForB.length > 0,
        "playId=" + Number(playB) + "; newReservations=" + freshReservationsForB.length);
      if (freshReservationsForB.length === 0) throw new Error("required DATA reservation for child firmware was not created");
    }
    saveJson("after-install-data-reservations.json", dataReservations(listing));
    return { slot, listing };
  })
    : { ok: false, value: null };
  if (installB.ok) { b = installB.value.slot; afterB = installB.value.listing; }
  else record("M05 install B", false, "blocked: install A or runtime A did not pass safety gates");
  let runtimeBResult = null;
  if (b) runtimeBResult = await runCase("M05B runtime B", () => runChildRuntime(b, playB, nameB, "M05B"));
  else record("M05B runtime B", false, "blocked: install B did not complete");
  if (b && !manualAssist && requireDataReservation) {
    try {
      const runtimeA = JSON.parse(fs.readFileSync(path.join(logDir, "runtime-m04a-evidence.json"), "utf8"));
      const runtimeB = JSON.parse(fs.readFileSync(path.join(logDir, "runtime-m05b-evidence.json"), "utf8"));
      const isolated = runtimeA.ok === true && runtimeB.ok === true &&
        Number.isInteger(runtimeA.dataAddress) && Number.isInteger(runtimeB.dataAddress) &&
        runtimeA.dataAddress !== runtimeB.dataAddress;
      record("M05C A/B DATA physical extents isolated", isolated,
        "A=" + runtimeA.dataAddress + "; B=" + runtimeB.dataAddress);
    } catch (error) {
      record("M05C A/B DATA physical extents isolated", false, error.message);
    }
  }

  if (a && b && runtimeBResult?.ok) {
    await runCase("M06 remove A and verify B isolation", async () => {
      await removeByName(nameA, "M06");
      if (!manualAssist) verifyDeletedRuntime(nameA, playA, "M07A");
      else {
        report.notCovered.push({ scenario: "M07A deleted firmware non-bootability", status: "NOT_COVERED", reason: "UI and read-only slot state cannot prove flash image non-bootability." });
        console.log("[NOT COVERED] M07A deleted firmware non-bootability");
      }
      afterRemoveA = await readSlots();
      const bPreservedAfterA = Boolean(b) && afterRemoveA.slots.some((x) => x.name === nameB && x.state === "valid");
      record("M06 B remains valid after removing A", bPreservedAfterA);
      if (!bPreservedAfterA) throw new Error("removing A affected B APP slot; refusing further device mutations");
      if (b) await runChildRuntime(afterRemoveA.slots.find((x) => x.name === nameB), playB, nameB, "M06B");
      if (requireDataReservation) {
        const prior = dataReservations(afterB || afterRemoveA), remaining = dataReservations(afterRemoveA), oldData = dataReservations(baseline);
        const created = prior.filter((d) => d.play_id === Number(playA) && !oldData.some((x) =>
          x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
        const released = created.filter((d) => !remaining.some((x) =>
          x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
        record("M06 UI delete releases test-created DATA reservations", released.length > 0, "released=" + released.length + "; created=" + created.length);
        if (requireDataReservation && released.length === 0) {
          throw new Error("removing A did not release its test-created DATA reservation; refusing further device mutations");
        }
      }
      requireSafeStorageState(afterRemoveA, "M06 after removing A");
    });
  } else record("M06 remove A and verify B isolation", false, "blocked: A/B install and runtime safety gates did not pass");

  // Reinstall A's play ID under a new name after deleting A. This proves
  // the allocator creates a fresh APP carve instead of reviving stale metadata.
  if (afterRemoveA && b && afterRemoveA.slots.some((x) => x.name === nameB && x.state === "valid") &&
      !afterRemoveA.slots.some((x) => x.name === nameA)) {
    let freshReservationsForC = [];
    const installC = await runCase("M06C fresh install after deletion", async () => {
      const slot = await installPlay(playA, nameC, "M06C fresh install C");
      const listing = await readSlots();
      saveJson("after-install-c.json", stableSlots(listing));
      const fresh = listing.slots.some((x) => x.name === nameC && x.state === "valid") &&
        listing.slots.some((x) => x.name === nameB && x.state === "valid") &&
        !listing.slots.some((x) => x.name === nameA);
      const separate = slot.slot !== b.slot;
      record("M06C new APP carve, no stale A slot revival", fresh && separate,
        "newSlot=" + slot.slot + "; BSlot=" + b.slot + "; oldANameAbsent=" +
        !listing.slots.some((x) => x.name === nameA));
      if (!fresh || !separate) throw new Error("fresh install C did not allocate a separate slot while preserving B");
      requireSafeStorageState(listing, "M06C after install C");
      const oldData = dataReservations(baseline);
      freshReservationsForC = dataReservations(listing).filter((d) => d.play_id === Number(playA) &&
        !oldData.some((x) => x.offset === d.offset && x.size === d.size &&
          x.play_id === d.play_id && x.label === d.label));
      if (requireDataReservation) {
        record("M06C fresh DATA reservation for reinstalled play", freshReservationsForC.length > 0,
          "newReservations=" + freshReservationsForC.length);
        if (freshReservationsForC.length === 0) throw new Error("fresh install C did not create a new DATA reservation");
      }
      return slot;
    });
    if (installC.ok) {
      const runtimeC = await runCase("M06C runtime C", () => runChildRuntime(installC.value, playA, nameC, "M06C"));
      if (runtimeC.ok) {
        await runCase("M06C remove C and verify B isolation", async () => {
          await removeByName(nameC, "M06C");
          if (!manualAssist) verifyDeletedRuntime(nameC, playA, "M06C");
          const listing = await readSlots();
          const cAbsent = !listing.slots.some((x) => x.name === nameC);
          record("M06C C APP slot removed", cAbsent);
          if (!cAbsent) throw new Error("C APP slot remains after uninstall");
          const remainingData = dataReservations(listing);
          const cDataReleased = freshReservationsForC.every((d) => !remainingData.some((x) =>
            x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
          record("M06C fresh DATA reservations released", cDataReleased,
            "created=" + freshReservationsForC.length + "; remaining=" +
            freshReservationsForC.filter((d) => remainingData.some((x) =>
              x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label)).length);
          if (!cDataReleased) throw new Error("C DATA reservation remained after uninstall");
          const remainingBData = dataReservations(listing);
          const bDataPreserved = freshReservationsForB.every((d) => remainingBData.some((x) =>
            x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label));
          record("M06C B DATA reservations preserved", bDataPreserved,
            "expected=" + freshReservationsForB.length + "; remaining=" +
            freshReservationsForB.filter((d) => remainingBData.some((x) =>
              x.offset === d.offset && x.size === d.size && x.play_id === d.play_id && x.label === d.label)).length);
          if (!bDataPreserved) throw new Error("removing C affected B DATA reservations");
          const bStillValid = listing.slots.some((x) => x.name === nameB && x.state === "valid");
          record("M06C B remains valid after removing C", bStillValid);
          if (!bStillValid) throw new Error("removing C affected B");
          requireSafeStorageState(listing, "M06C after removing C");
        });
      }
    }
  } else {
    record("M06C fresh install after deletion", false, "blocked: A deletion or B-preservation safety gate failed");
  }

  if (b) {
    await runCase("M07 remove B", async () => {
      await removeByName(nameB, "M07");
      if (!manualAssist) verifyDeletedRuntime(nameB, playB, "M08B");
      else {
        report.notCovered.push({ scenario: "M08B deleted firmware non-bootability", status: "NOT_COVERED", reason: "UI and read-only slot state cannot prove flash image non-bootability." });
        console.log("[NOT COVERED] M08B deleted firmware non-bootability");
      }
    });
  } else record("M07 remove B", false, "blocked: install B did not complete");

  await runCase("M08 final baseline restoration", async () => {
    const finalSlots = await readSlots(), finalStatus = await readStatus();
    saveJson("final-slots.json", stableSlots(finalSlots));
    saveJson("final-status.json", finalStatus);
    assertIdle(finalStatus, "M08 final");
    requireSafeStorageState(finalSlots, "M08 final");
    record("M08 data reservations restored to baseline",
      JSON.stringify(dataReservations(finalSlots)) === JSON.stringify(dataReservations(baseline)),
      "baseline=" + dataReservations(baseline).length + "; final=" + dataReservations(finalSlots).length);
    saveJson("final-data-reservations.json", dataReservations(finalSlots));
    const restored = JSON.stringify(stableSlots(finalSlots)) === JSON.stringify(stableSlots(baseline));
    record("M08 device state restored to baseline", restored,
      "baselineSlots=" + baseline.slots.length + "; finalSlots=" + finalSlots.slots.length +
      "; baselineFree=" + baseline.free + "; finalFree=" + finalSlots.free);
  });
  record("M09 no unhandled page errors", pageErrors.length === 0, "count=" + pageErrors.length);
  record("M09 no console errors", consoleErrors.length === 0, "count=" + consoleErrors.length);
  record("M09 no failed network requests", failedRequests.length === 0, "count=" + failedRequests.length);
  report.verdict = report.results.every((x) => x.ok) ? (manualAssist ? "PASS_WITH_MANUAL_STEPS" : "PASS") : "FAIL";
} catch (e) {
  report.results.push({ name: "fatal", ok: false, detail: redact(e?.stack || e?.message || String(e)), at: new Date().toISOString() });
  console.error("[FAIL] fatal — " + redact(e?.message || String(e)));
} finally {
  if (!["PASS", "PASS_WITH_MANUAL_STEPS"].includes(report.verdict)) await cleanupOwned();
  report.finishedAt = new Date().toISOString();
  report.browserErrors = pageErrors;
  report.consoleErrors = consoleErrors.slice(0, 100);
  report.failedRequests = failedRequests.slice(0, 100);
  report.summary = {
    total: report.results.length,
    passed: report.results.filter((r) => r.ok).length,
    failed: report.results.filter((r) => !r.ok).length,
    notCovered: report.notCovered.length,
    pageErrors: pageErrors.length,
    consoleErrors: consoleErrors.length,
    failedRequests: failedRequests.length,
  };
  try {
    if (page) {
      await page.screenshot({ path: path.join(logDir, "final-page.png"), fullPage: true, timeout: 15000 });
      fs.writeFileSync(path.join(logDir, "page-source.html"), redact(await page.content()));
    }
  } catch {}
  saveJson("report.json", report);
  fs.writeFileSync(path.join(logDir, "report.txt"),
    [`Mobile page E2E: ${report.verdict}`, `viewport: ${viewport.width}x${viewport.height}`,
      `summary: total=${report.summary.total}; passed=${report.summary.passed}; failed=${report.summary.failed}; notCovered=${report.summary.notCovered}`,
      ...report.results.map((r) => `${r.ok ? "PASS" : "FAIL"} | evidence=${r.evidence || "automated"} | ${r.name} | ${r.detail || ""}`),
      `coverage gaps: ${JSON.stringify(report.coverageGaps || [])}`,
      `not covered: ${JSON.stringify(report.notCovered || [])}`,
      `page errors: ${pageErrors.length}`, `console errors: ${consoleErrors.length}`,
      `failed requests: ${failedRequests.length}`].join("\n") + "\n");
  try { await page?.close(); } catch {}
  console.log(`REPORT_DIR=${logDir}`);
  console.log(`VERDICT=${report.verdict}`);
}
if (!["PASS", "PASS_WITH_MANUAL_STEPS"].includes(report.verdict)) process.exitCode = 1;
