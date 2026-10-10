#!/usr/bin/env node
// Non-mutating mobile viewport smoke test for the phone-install page.
// Uses an existing Chromium CDP endpoint; never clicks controls or calls write APIs.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { CdpMobilePage } from "./cdp_mobile_page.mjs";

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith("--")) continue;
    const eq = arg.indexOf("=");
    if (eq > 2) out[arg.slice(2, eq)] = arg.slice(eq + 1);
    else out[arg.slice(2)] = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : true;
  }
  return out;
}
const args = parseArgs(process.argv.slice(2));
const urlArg = String(args.url || process.env.MOBILE_BROWSER_SMOKE_URL || "");
const cdpUrl = String(args["cdp-url"] || process.env.MOBILE_E2E_CDP_URL || "http://127.0.0.1:9222");
const viewportMatrix = String(args.viewports || "320x720,360x800,390x844,430x932").split(",").map((item) => {
  const m = item.trim().match(/^(\d+)x(\d+)$/);
  if (!m || Number(m[1]) < 240 || Number(m[2]) < 320) {
    console.error("Invalid viewport: " + item + " (expected widthxheight, width>=240, height>=320)");
    process.exit(2);
  }
  return { width: Number(m[1]), height: Number(m[2]) };
});
if (!urlArg) {
  console.error("Usage: node tools/realdevice/mobile_page_browser_smoke.mjs --url http://127.0.0.1:4191/?mock=1 [--cdp-url http://127.0.0.1:9222]");
  process.exit(2);
}
let target;
try { target = new URL(urlArg); } catch { console.error("--url must be an absolute URL"); process.exit(2); }
if (!["http:", "https:"].includes(target.protocol) || target.username || target.password) {
  console.error("--url must be HTTP(S) without embedded credentials");
  process.exit(2);
}
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const defaultLogDir = path.join(root, "tools/realdevice/logs/mobile-browser-smoke-" + new Date().toISOString().replace(/[:.]/g, "-"));
const logDir = path.resolve(String(args.logdir || defaultLogDir));
fs.mkdirSync(logDir, { recursive: true });
const report = {
  test: "Non-mutating phone-install mobile viewport smoke",
  startedAt: new Date().toISOString(),
  browser: "Chromium via direct CDP",
  cdpEndpoint: cdpUrl.replace(/:\/\/[^/]+/, "://<redacted-host>"),
  mobileEmulation: { viewports: viewportMatrix, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
  target: target.origin + target.pathname,
  verdict: "FAIL",
  results: [],
  errors: { page: [], console: [], requests: [] },
};
let page;
function redact(value) {
  return String(value)
    .replace(/\b[a-f0-9]{32}\b/gi, "<redacted-token>")
    .replace(/https?:\/\/[^\s"'<>]+/g, (u) => u.replace(/#s=[^&\s]+/i, "#s=<redacted>"))
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g, "<host-ip>");
}
function record(name, ok, detail = "") {
  report.results.push({ name, ok, detail: redact(detail), at: new Date().toISOString() });
  console.log("[" + (ok ? "PASS" : "FAIL") + "] " + name + (detail ? " — " + redact(detail) : ""));
}
try {
  page = await new CdpMobilePage({ endpoint: cdpUrl, viewport: viewportMatrix[0] }).connect();
  page.on("pageerror", (error) => report.errors.page.push(redact(error?.message || error)));
  page.on("console", (message) => report.errors.console.push(redact(message.text?.() || "console error")));
  page.on("requestfailed", (request) => report.errors.requests.push({
    method: request.method?.() || "REQUEST",
    url: redact(request.url?.() || ""),
    failure: redact(request.failure?.()?.errorText || "failed"),
  }));
  await page.goto(target.href, { timeout: Number(args.timeout || 60000) });
  await page.waitForTimeout(800);
  for (const viewport of viewportMatrix) {
    await page.setViewportSize(viewport);
    await page.waitForTimeout(150);
    const state = await page.evaluate(() => {
      const visible = (selector) => {
        const el = document.querySelector(selector);
        if (!el) return false;
        const rect = el.getBoundingClientRect();
        const style = getComputedStyle(el);
        return rect.width > 0 && rect.height > 0 && style.display !== "none" && style.visibility !== "hidden";
      };
      return {
        readyState: document.readyState,
        title: document.title,
        bodyTextLength: (document.body?.innerText || "").trim().length,
        root: Boolean(document.querySelector("#mp-install-root")),
        search: visible("#mp-q"),
        management: visible("#mp-mgmt"),
        viewportMeta: Boolean(document.querySelector('meta[name="viewport"]')),
        innerWidth: window.innerWidth,
        documentWidth: document.documentElement.scrollWidth,
        bodyWidth: document.body?.scrollWidth || 0,
      };
    });
    const keyControls = state.root && state.search && state.management;
    const noOverflow = state.documentWidth <= state.innerWidth + 1 && state.bodyWidth <= state.innerWidth + 1;
    record("viewport " + viewport.width + "x" + viewport.height + " key controls", keyControls,
      "root=" + state.root + "; search=" + state.search + "; management=" + state.management);
    record("viewport " + viewport.width + "x" + viewport.height + " no horizontal overflow", noOverflow,
      "inner=" + state.innerWidth + "; document=" + state.documentWidth + "; body=" + state.bodyWidth);
    record("viewport " + viewport.width + "x" + viewport.height + " viewport metadata/content",
      state.viewportMeta && state.bodyTextLength > 0 && ["interactive", "complete"].includes(state.readyState),
      "ready=" + state.readyState + "; title=" + state.title + "; bodyTextLength=" + state.bodyTextLength);
    await page.screenshot({ path: path.join(logDir, "viewport-" + viewport.width + "x" + viewport.height + ".png"), fullPage: true });
    report.results[report.results.length - 3].viewport = viewport;
    report.results[report.results.length - 2].viewport = viewport;
    report.results[report.results.length - 1].viewport = viewport;
  }
  record("no uncaught page errors", report.errors.page.length === 0, report.errors.page.join("; "));
  record("no console errors", report.errors.console.length === 0, report.errors.console.join("; "));
  record("no failed network requests", report.errors.requests.length === 0, JSON.stringify(report.errors.requests));
} catch (error) {
  record("browser smoke execution", false, error?.stack || error?.message || String(error));
} finally {
  try { await page?.close(); } catch {}
}
report.finishedAt = new Date().toISOString();
report.summary = {
  total: report.results.length,
  passed: report.results.filter((item) => item.ok).length,
  failed: report.results.filter((item) => !item.ok).length,
};
report.verdict = report.summary.failed === 0 ? "PASS" : "FAIL";
fs.writeFileSync(path.join(logDir, "report.json"), JSON.stringify(report, null, 2) + "\n");
console.log("REPORT_DIR=" + logDir);
console.log("VERDICT=" + report.verdict + " (" + report.summary.passed + "/" + report.summary.total + ")");
if (report.verdict !== "PASS") process.exitCode = 1;
