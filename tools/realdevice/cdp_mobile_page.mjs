// Minimal direct-CDP mobile page adapter. No Playwright dependency.
import fs from "node:fs";

export class CdpMobilePage {
  constructor({ endpoint, viewport, timeout = 15000 }) {
    this.endpoint = endpoint.replace(/\/$/, "");
    this.viewport = viewport;
    this.timeout = timeout;
    this.ws = null;
    this.sessionId = null;
    this.targetId = null;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Map();
    this.closed = false;
  }
  on(event, callback) {
    const list = this.listeners.get(event) || [];
    list.push(callback);
    this.listeners.set(event, list);
  }
  emit(event, value) {
    for (const callback of this.listeners.get(event) || []) {
      try { callback(value); } catch {}
    }
  }
  async connect() {
    const response = await fetch(this.endpoint + "/json/version", { signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw new Error("CDP discovery HTTP " + response.status);
    const version = await response.json();
    if (!version.webSocketDebuggerUrl) throw new Error("CDP /json/version has no webSocketDebuggerUrl");
    this.ws = new WebSocket(version.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("CDP websocket connect timeout")), 5000);
      this.ws.addEventListener("open", () => { clearTimeout(timer); resolve(); }, { once: true });
      this.ws.addEventListener("error", () => { clearTimeout(timer); reject(new Error("CDP websocket connection failed")); }, { once: true });
    });
    this.ws.addEventListener("message", (event) => this.onMessage(String(event.data)));
    const target = await this.send("Target.createTarget", { url: "about:blank" });
    this.targetId = target.targetId;
    const attached = await this.send("Target.attachToTarget", { targetId: this.targetId, flatten: true });
    this.sessionId = attached.sessionId;
    await this.command("Page.enable");
    await this.command("Runtime.enable");
    await this.command("Network.enable");
    await this.command("Log.enable");
    await this.command("Emulation.setDeviceMetricsOverride", {
      width: this.viewport.width, height: this.viewport.height, deviceScaleFactor: 3, mobile: true,
    });
    await this.command("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
    await this.command("Emulation.setUserAgentOverride", {
      userAgent: "Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36",
      platform: "Android",
    });
    this.ws.addEventListener("message", (event) => this.onEvent(String(event.data)));
    return this;
  }
  onMessage(raw) {
    let message;
    try { message = JSON.parse(raw); } catch { return; }
    if (!message.id) return;
    const pending = this.pending.get(message.id);
    if (!pending) return;
    this.pending.delete(message.id);
    if (message.error) pending.reject(new Error("CDP " + pending.method + ": " + message.error.message));
    else pending.resolve(message.result || {});
  }
  onEvent(raw) {
    let message;
    try { message = JSON.parse(raw); } catch { return; }
    if (message.sessionId && message.sessionId !== this.sessionId) return;
    const p = message.params || {};
    if (message.method === "Runtime.exceptionThrown") this.emit("pageerror", new Error(p.exceptionDetails?.text || "Runtime exception"));
    if (message.method === "Log.entryAdded" && p.entry?.level === "error") {
      this.emit("console", { type: () => "error", text: () => p.entry.text || "console error" });
    }
    if (message.method === "Network.loadingFailed") {
      this.emit("requestfailed", { method: () => "REQUEST", url: () => p.requestId || "", failure: () => ({ errorText: p.errorText || "failed" }) });
    }
  }
  send(method, params = {}, sessionId = undefined) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error("CDP command timeout: " + method));
        }
      }, this.timeout);
    });
  }
  command(method, params = {}) { return this.send(method, params, this.sessionId); }
  async evaluate(fnOrString, ...args) {
    const fnSource = typeof fnOrString === "function" ? fnOrString.toString() : String(fnOrString);
    const expression = typeof fnOrString === "function"
      ? "(" + fnSource + ")(" + args.map((x) => JSON.stringify(x)).join(",") + ")"
      : "(function(){ " + fnSource.replace(/^\s*return\s+/, "return ") + " })()";
    const result = await this.command("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, userGesture: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text || "page evaluation failed");
    return result.result?.value;
  }
  async goto(url, { timeout = 60000 } = {}) {
    await this.command("Page.navigate", { url });
    const end = Date.now() + timeout;
    while (Date.now() < end) {
      if (await this.evaluate(() => document.readyState === "interactive" || document.readyState === "complete").catch(() => false)) return;
      await new Promise((r) => setTimeout(r, 100));
    }
    throw new Error("CDP navigation timed out");
  }
  locator(selector) { return new CdpLocator(this, selector); }
  async evaluateExpression(expression) {
    const result = await this.command("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, userGesture: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text || "page evaluation failed");
    return result.result?.value;
  }
  setDefaultTimeout(ms) { this.timeout = ms; }
  async setViewportSize(size) {
    this.viewport = size;
    await this.command("Emulation.setDeviceMetricsOverride", { width: size.width, height: size.height, deviceScaleFactor: 3, mobile: true });
  }
  async waitForTimeout(ms) { await new Promise((r) => setTimeout(r, ms)); }
  async content() { return this.evaluate(() => document.documentElement.outerHTML); }
  async screenshot({ path, fullPage = false } = {}) {
    const result = await this.command("Page.captureScreenshot", { format: "png", captureBeyondViewport: Boolean(fullPage), fromSurface: true });
    if (path) fs.writeFileSync(path, Buffer.from(result.data, "base64"));
    return result.data;
  }
  async close() {
    if (this.closed) return;
    this.closed = true;
    try { if (this.targetId) await this.send("Target.closeTarget", { targetId: this.targetId }); } catch {}
    try { this.ws?.close(); } catch {}
  }
}

class CdpLocator {
  constructor(page, selector) { this.page = page; this.selector = selector; }
  async count() { return this.page.evaluate((selector) => document.querySelectorAll(selector).length, this.selector); }
  async exists() { return (await this.count()) > 0; }
  async isVisible() {
    return this.page.evaluate((selector) => {
      const el = document.querySelector(selector);
      if (!el) return false;
      const s = getComputedStyle(el), r = el.getBoundingClientRect();
      return s.display !== "none" && s.visibility !== "hidden" && Number(s.opacity) !== 0 && r.width > 0 && r.height > 0;
    }, this.selector);
  }
  async isDisabled() {
    return this.page.evaluate((selector) => {
      const el = document.querySelector(selector);
      return !el || Boolean(el.disabled || el.getAttribute("aria-disabled") === "true");
    }, this.selector);
  }
  async textContent() { return this.page.evaluate((selector) => document.querySelector(selector)?.textContent ?? null, this.selector); }
  async innerText() { return this.page.evaluate((selector) => document.querySelector(selector)?.innerText ?? "", this.selector); }
  async click() {
    await this.page.evaluate((selector) => {
      const el = document.querySelector(selector);
      if (!el) throw new Error("element not found: " + selector);
      if (el.disabled || el.getAttribute("aria-disabled") === "true") throw new Error("element disabled: " + selector);
      el.click();
    }, this.selector);
  }
  async fill(value) {
    await this.page.evaluate(({ selector, value }) => {
      const el = document.querySelector(selector);
      if (!el) throw new Error("element not found: " + selector);
      el.focus();
      const proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
      const setter = Object.getOwnPropertyDescriptor(proto, "value")?.set;
      if (setter) setter.call(el, String(value)); else el.value = String(value);
      el.dispatchEvent(new InputEvent("input", { bubbles: true, inputType: "insertText", data: String(value) }));
      el.dispatchEvent(new Event("change", { bubbles: true }));
    }, { selector: this.selector, value: String(value) });
  }
  async check() {
    await this.page.evaluate((selector) => {
      const el = document.querySelector(selector);
      if (!el) throw new Error("element not found: " + selector);
      if ("checked" in el && !el.checked) {
        el.checked = true;
        el.dispatchEvent(new Event("input", { bubbles: true }));
        el.dispatchEvent(new Event("change", { bubbles: true }));
      }
    }, this.selector);
  }
  async evaluate(fn) {
    const source = typeof fn === "function" ? fn.toString() : String(fn);
    return this.page.evaluateExpression("(" + source + ")(document.querySelector(" + JSON.stringify(this.selector) + "))");
  }
  async waitFor({ state = "visible", timeout = this.page.timeout } = {}) {
    const end = Date.now() + timeout;
    while (Date.now() < end) {
      const count = await this.count().catch(() => 0);
      const visible = count > 0 && await this.isVisible().catch(() => false);
      if ((state === "attached" && count > 0) || (state === "detached" && count === 0) ||
          (state === "visible" && visible) || (state === "hidden" && (!count || !visible))) return;
      await new Promise((r) => setTimeout(r, 100));
    }
    throw new Error("timeout waiting for " + state + ": " + this.selector);
  }
}
