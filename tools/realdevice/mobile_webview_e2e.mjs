#!/usr/bin/env node
// Mobile embedded-page E2E runner. Uses Appium's W3C WebDriver API directly,
// so it does not add a second client dependency beside the Appium server/driver.
//
// Modes:
//   browser: Android Chrome, useful for the mobile web entry point.
//   webview: launch a native host app and switch to its embedded WebView.
// All UI mutations are performed through WebDriver UI commands; read-only
// device API checks are allowed for independent postcondition verification.
// The runner never calls install prepare/session/chunk/finalize endpoints.

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const argv = (() => {
  const out = {};
  for (let i = 2; i < process.argv.length; i++) {
    const arg = process.argv[i];
    if (!arg.startsWith("--")) continue;
    const eq = arg.indexOf("=");
    if (eq > 2) out[arg.slice(2, eq)] = arg.slice(eq + 1);
    else if (process.argv[i + 1] && !process.argv[i + 1].startsWith("--")) out[arg.slice(2)] = process.argv[++i];
    else out[arg.slice(2)] = true;
  }
  return out;
})();

const MODE = String(argv.mode || process.env.MOBILE_E2E_MODE || "webview");
const APPIUM = String(argv.appium || process.env.APPIUM_URL || "http://127.0.0.1:4723").replace(/\/$/, "");
const DEVICE = String(argv.device || process.env.MOBILE_E2E_DEVICE || "Android");
const URL = String(argv.url || process.env.MOBILE_E2E_URL || "");
const IP = String(argv.ip || process.env.META_PASS_IP || "");
const PACKAGE = String(argv["app-package"] || process.env.MOBILE_APP_PACKAGE || "");
const ACTIVITY = String(argv["app-activity"] || process.env.MOBILE_APP_ACTIVITY || "");
const SCENARIO_FILE = argv.scenario ? path.resolve(String(argv.scenario)) : path.join(ROOT, "tools/realdevice/mobile-webview-scenario.json");
const LOGDIR = path.resolve(String(argv.logdir || path.join(ROOT, "tools/realdevice/logs/mobile-e2e-" + new Date().toISOString().replace(/[:.]/g, "-"))));
const TIMEOUT = Number(argv.timeout || process.env.MOBILE_E2E_TIMEOUT_MS || 30000);
const POLL = 500;

function usage() {
  console.error("Usage: node tools/realdevice/mobile_webview_e2e.mjs --mode webview --app-package <id> --app-activity <activity> --url <device-url> [--appium http://127.0.0.1:4723] [--scenario <json>]");
  console.error("   or: node tools/realdevice/mobile_webview_e2e.mjs --mode browser --url <device-url>");
}
if (!["webview", "browser"].includes(MODE) || !URL || (MODE === "webview" && (!PACKAGE || !ACTIVITY))) {
  usage();
  process.exit(2);
}
if (IP && !/^\d{1,3}(\.\d{1,3}){3}$/.test(IP)) {
  console.error("--ip must be an IPv4 address");
  process.exit(2);
}
fs.mkdirSync(LOGDIR, { recursive: true });

const report = {
  framework: "mobile-webview-e2e",
  mode: MODE,
  startedAt: new Date().toISOString(),
  verdict: "FAIL",
  results: [],
};
let sessionId = null;
let currentContext = null;
let commandSeq = 0;

function redact(value) {
  return String(value)
    .replace(/\b[a-f0-9]{32}\b/gi, "<redacted-token>")
    .replace(/https?:\/\/[^\s"'<>]+/g, (url) => url.replace(/#s=[^&\s]+/i, "#s=<redacted>"))
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, "<device-ip>");
}
function saveJson(name, value) {
  fs.writeFileSync(path.join(LOGDIR, name), JSON.stringify(value, null, 2) + "\n");
}
function record(name, ok, detail = "") {
  report.results.push({ name, ok, detail: redact(detail), at: new Date().toISOString() });
  console.log(`[${ok ? "PASS" : "FAIL"}] ${name}${detail ? " — " + redact(detail) : ""}`);
  if (!ok) process.exitCode = 1;
}
async function request(route, { method = "GET", body, timeout = TIMEOUT } = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  try {
    const response = await fetch(APPIUM + route, {
      method,
      headers: body === undefined ? {} : { "content-type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
    });
    const text = await response.text();
    let json;
    try { json = text ? JSON.parse(text) : {}; } catch { json = { value: text }; }
    if (!response.ok || json?.value?.error) {
      const error = json?.value?.message || json?.message || text || response.statusText;
      throw new Error(`Appium ${method} ${route}: HTTP ${response.status}: ${error}`);
    }
    return json.value ?? json;
  } finally {
    clearTimeout(timer);
  }
}
const base = () => `/session/${sessionId}`;
async function wd(method, route, body) {
  return request(base() + route, { method, body });
}
async function sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }
async function waitFor(fn, label, timeout = TIMEOUT) {
  const end = Date.now() + timeout;
  let lastError;
  while (Date.now() < end) {
    try {
      const value = await fn();
      if (value) return value;
    } catch (error) { lastError = error; }
    await sleep(POLL);
  }
  throw new Error(`timeout waiting for ${label}${lastError ? ": " + lastError.message : ""}`);
}
async function contexts() {
  const result = await wd("GET", "/contexts");
  return Array.isArray(result) ? result : [];
}
async function switchContext(name) {
  await wd("POST", "/context", { name });
  currentContext = name;
}
async function chooseWebContext() {
  return waitFor(async () => {
    const all = await contexts();
    const web = all.find((x) => /WEBVIEW|CHROMIUM/i.test(x));
    if (web) {
      await switchContext(web);
      return web;
    }
    return null;
  }, "mobile WebView context", Number(argv["context-timeout"] || 60000));
}
async function execute(script, args = []) {
  return wd("POST", "/execute/sync", { script, args });
}
async function executeAsync(script, args = []) {
  return wd("POST", "/execute/async", { script, args });
}
async function find(selector, strategy = "css selector") {
  const found = await wd("POST", "/element", { using: strategy, value: selector });
  return found.ELEMENT || found["element-6066-11e4-a52e-4f735466cecf"];
}
async function click(selector, strategy = "css selector") {
  const id = await find(selector, strategy);
  await wd("POST", `/element/${id}/click`, {});
}
async function fill(selector, value) {
  const id = await find(selector);
  await wd("POST", `/element/${id}/clear`, {});
  await wd("POST", `/element/${id}/value`, { text: String(value), value: [...String(value)] });
}
async function textOf(selector) {
  const id = await find(selector);
  return wd("GET", `/element/${id}/text`);
}
async function waitSelector(selector, label = selector, timeout = TIMEOUT) {
  return waitFor(async () => {
    try { return await find(selector); } catch { return null; }
  }, label, timeout);
}
async function snapshot() {
  try {
    const source = await wd("GET", "/source");
    fs.writeFileSync(path.join(LOGDIR, "page-source.xml"), redact(source));
  } catch (error) {
    fs.writeFileSync(path.join(LOGDIR, "page-source-error.txt"), redact(error.message) + "\n");
  }
  try {
    const shot = await wd("GET", "/screenshot");
    const b64 = typeof shot === "string" ? shot : shot.value;
    if (b64) fs.writeFileSync(path.join(LOGDIR, "failure.png"), Buffer.from(b64, "base64"));
  } catch {}
}
async function verifyDeviceApi() {
  if (!IP) throw new Error("device-state assertions require --ip");
  // Read back from inside the actual mobile page origin: preserve the phone
  // WebView's UA/origin/request defaults and send no invented auth headers.
  const result = await executeAsync(
    "const done = arguments[arguments.length - 1]; fetch('/api/install/slots').then(async r => ({status:r.status, body:await r.text()})).then(done).catch(e => done({error:String(e)}));"
  );
  if (result?.error) throw new Error(`device slots API fetch failed: ${result.error}`);
  if (result?.status !== 200) throw new Error(`device slots API HTTP ${result?.status}: ${String(result?.body || "").slice(0, 160)}`);
  const data = JSON.parse(result.body);
  if (!Array.isArray(data.slots) && !Array.isArray(data.slot)) {
    throw new Error("device slots API response has no slots array");
  }
  saveJson("device-slots.json", data);
  return data;
}
async function verifyDeviceStatus() {
  const result = await executeAsync(
    "const done = arguments[arguments.length - 1]; fetch('/api/install/status').then(async r => ({status:r.status, body:await r.text()})).then(done).catch(e => done({error:String(e)}));"
  );
  if (result?.error) throw new Error(`device status API fetch failed: ${result.error}`);
  if (result?.status !== 200) throw new Error(`device status API HTTP ${result?.status}: ${String(result?.body || "").slice(0, 160)}`);
  const status = JSON.parse(result.body);
  saveJson("device-status.json", status);
  return status;
}
function slotList(data) {
  const list = data?.slots || data?.slot;
  if (!Array.isArray(list)) throw new Error("device slots response has no slots array");
  return list;
}

async function runScenario(scenario) {
  const slotSnapshots = new Map();
  if (scenario.navigate !== false) {
    await wd("POST", "/url", { url: URL });
    record("open target URL in mobile context", true, URL);
  }
  await waitFor(async () => {
    const ready = await execute("return document.readyState === 'interactive' || document.readyState === 'complete'");
    return ready === true;
  }, "document ready");
  const title = await execute("return document.title || ''");
  const href = await execute("return location.origin + location.pathname");
  record("mobile document loaded", true, `title=${title}; origin/path=${href}`);
  if (scenario.assertions?.titleIncludes) {
    const pass = String(title).includes(scenario.assertions.titleIncludes);
    record("document title assertion", pass, `expected includes ${scenario.assertions.titleIncludes}`);
    if (!pass) throw new Error("document title assertion failed");
  }
  if (scenario.assertions?.bodyTextMinLength !== undefined) {
    const length = await execute("return (document.body?.innerText || '').trim().length");
    const pass = Number(length) >= Number(scenario.assertions.bodyTextMinLength);
    record("document body has visible text", pass, `length=${length}`);
    if (!pass) throw new Error("document body text is empty or shorter than expected");
  }
  if (scenario.assertions?.interactiveElementCountAtLeast !== undefined) {
    const count = await execute("return document.querySelectorAll('button, input, select, textarea, [role=button]').length");
    const pass = Number(count) >= Number(scenario.assertions.interactiveElementCountAtLeast);
    record("interactive controls present", pass, `count=${count}`);
    if (!pass) throw new Error("no expected interactive controls found");
  }
  if (scenario.assertions?.readySelector) {
    await waitSelector(scenario.assertions.readySelector, "page ready selector");
    record("page ready selector visible", true, scenario.assertions.readySelector);
  }
  for (const [index, step] of (scenario.steps || []).entries()) {
    const label = step.name || `step ${index + 1}`;
    if (step.action === "waitVisible") {
      await waitSelector(step.selector, label, step.timeout || TIMEOUT);
    } else if (step.action === "click") {
      await waitSelector(step.selector, label);
      await click(step.selector);
    } else if (step.action === "fill") {
      await waitSelector(step.selector, label);
      await fill(step.selector, step.valueFrom === "deviceIp" ? IP : step.valueFrom === "deviceUrl" ? URL : step.value);
    } else if (step.action === "waitText") {
      await waitFor(async () => (await textOf(step.selector)).includes(step.text), label, step.timeout || TIMEOUT);
    } else if (step.action === "assertText") {
      const actual = await textOf(step.selector);
      const pass = actual.includes(step.text);
      record(label, pass, `expected text includes ${step.text}`);
      if (!pass) throw new Error(`assertText failed: ${label}`);
    } else if (step.action === "snapshotSlots") {
      const data = await verifyDeviceApi();
      slotSnapshots.set(step.snapshot, data);
      saveJson(`slots-${String(step.snapshot).replace(/[^a-z0-9_-]/gi, "_")}.json`, data);
      record(label, true, `snapshot=${step.snapshot}; slots=${slotList(data).length}`);
    } else if (step.action === "assertSlotPresent" || step.action === "assertSlotAbsent") {
      const data = slotSnapshots.get(step.snapshot);
      if (!data) throw new Error(`unknown slot snapshot: ${step.snapshot}`);
      const present = slotList(data).some((slot) =>
        step.slotName !== undefined ? slot.name === step.slotName :
        step.playId !== undefined ? Number(slot.play_id ?? slot.playId) === Number(step.playId) : false
      );
      const pass = step.action === "assertSlotPresent" ? present : !present;
      record(label, pass, `snapshot=${step.snapshot}; present=${present}`);
      if (!pass) throw new Error(`${step.action} failed: ${label}`);
    } else if (step.action === "assertInstallerIdle") {
      const status = await verifyDeviceStatus();
      const pass = status.protocol === 1 && !status.active && !status.session;
      record(label, pass, `protocol=${status.protocol}; active=${status.active}; session=${Boolean(status.session)}`);
      if (!pass) throw new Error(`installer is not idle: ${label}`);
    } else if (step.action === "sleep") {
      await sleep(Number(step.ms || 500));
    } else if (step.action === "assertJs") {
      const actual = await execute(step.script);
      const pass = step.equals === undefined ? Boolean(actual) : JSON.stringify(actual) === JSON.stringify(step.equals);
      record(label, pass, `actual=${JSON.stringify(actual)}`);
      if (!pass) throw new Error(`assertJs failed: ${label}`);
    } else {
      throw new Error(`unsupported scenario action: ${step.action}`);
    }
    record(label, true, step.action);
  }
  if (scenario.deviceAssertions?.slotsCountAtLeast !== undefined) {
    const slots = await verifyDeviceApi();
    const list = slots.slots || slots.slot;
    const pass = list.length >= Number(scenario.deviceAssertions.slotsCountAtLeast);
    record("device slot count lower bound", pass, `actual=${list.length}`);
    if (!pass) throw new Error("device slot count assertion failed");
  }
}

let scenario;
try {
  scenario = JSON.parse(fs.readFileSync(SCENARIO_FILE, "utf8"));
  if (MODE === "browser") {
    // Android Chrome: the WebDriver session starts the browser and uses the
    // mobile browser's Chromium context, not a desktop viewport simulation.
    const caps = {
      platformName: "Android",
      browserName: "Chrome",
      "appium:automationName": "UiAutomator2",
      "appium:deviceName": DEVICE,
      "appium:noReset": true,
      "appium:newCommandTimeout": Math.ceil((TIMEOUT * 4) / 1000),
    };
    const session = await request("/session", { method: "POST", body: { capabilities: { alwaysMatch: caps, firstMatch: [{}] } } });
    sessionId = session.sessionId;
  } else {
    const caps = {
      platformName: "Android",
      "appium:automationName": "UiAutomator2",
      "appium:deviceName": DEVICE,
      "appium:appPackage": PACKAGE,
      "appium:appActivity": ACTIVITY,
      "appium:noReset": true,
      "appium:autoGrantPermissions": true,
      "appium:newCommandTimeout": Math.ceil((TIMEOUT * 4) / 1000),
    };
    const session = await request("/session", { method: "POST", body: { capabilities: { alwaysMatch: caps, firstMatch: [{}] } } });
    sessionId = session.sessionId;
  }
  record("Appium session created", true, `mode=${MODE}`);
  const webContext = await chooseWebContext();
  record("switched to mobile WebView", true, webContext);
  await runScenario(scenario);
  if (IP) {
    const slots = await verifyDeviceApi();
    record("device slots API independently readable", true, `slots=${(slots.slots || slots.slot).length}`);
  }
  report.verdict = report.results.every((x) => x.ok) ? "PASS" : "FAIL";
} catch (error) {
  record("fatal", false, error?.stack || error?.message || String(error));
  await snapshot();
} finally {
  report.finishedAt = new Date().toISOString();
  report.context = currentContext;
  saveJson("report.json", report);
  fs.writeFileSync(path.join(LOGDIR, "report.txt"),
    [`Mobile embedded-page E2E: ${report.verdict}`, `mode: ${MODE}`,
     ...report.results.map((r) => `${r.ok ? "PASS" : "FAIL"} | ${r.name} | ${r.detail}`)].join("\n") + "\n");
  if (sessionId) {
    try { await request(`/session/${sessionId}`, { method: "DELETE" }); } catch {}
  }
  console.log(`REPORT_DIR=${LOGDIR}`);
  console.log(`VERDICT=${report.verdict}`);
}
if (report.verdict !== "PASS") process.exitCode = 1;
