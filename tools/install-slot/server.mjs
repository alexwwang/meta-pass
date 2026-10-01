#!/usr/bin/env node
// tools/install-slot/server.mjs —— USB 串口安装通道本地代理服务器。
// 零外部依赖，仅使用 node 内置模块；默认端口 4191，可用 PORT 环境变量覆盖。
// 页面：GET / 返回 install-slot.html；社区玩法 API 无 CORS 头，由本服务器代理转发。
// SSRF 防护：只允许代理 ai-passport.folotoy.cn 的 /api/download/ 与 /api/plays 路径。
//
// 页面资源直接服务仓库规范目录 install-slot/(与 Cloudflare Pages 部署同源),
// 本目录只保留 server.mjs 与测试,杜绝两份页面副本再次漂移(见 docs/BUGS.md BUG-03)。

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { execSync } from "node:child_process";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { createStoreAnalyzer } from "../../install-slot/store-analyze.js";

const PORT = Number(process.env.PORT) || 4191;
const BACKEND = "https://ai-passport.folotoy.cn";
const DIR = path.dirname(fileURLToPath(import.meta.url));
const PAGE_DIR = path.resolve(DIR, "..", "..", "install-slot");

// 设备端 OTA 通道(store-analyze 纯逻辑 + Node sha256,后端与代理同一源站)。
// 分析/已验证镜像按 play id 进程内缓存(store-analyze 内部);设备流量 =
// 一次 analyze + 一次 extracted 流式下载。
const sha256 = async (buf) => createHash("sha256").update(buf).digest("hex");
const storeAnalyzer = createStoreAnalyzer({ fetchImpl: fetch, backend: BACKEND, sha256 });

// analyze/extracted 按 IP 固定窗口限速(方案 §1.2 服务端安全约束;生产为 CF Pages
// KV token bucket,本地 dev 用进程内计数近似对齐行为)。
const RATE_LIMIT = 60;            // 每窗口请求数
const RATE_WINDOW_MS = 60_000;    // 窗口时长
const rateBuckets = new Map();    // ip -> {windowStart, count}
function rateLimited(ip) {
  const now = Date.now();
  const b = rateBuckets.get(ip);
  if (!b || now - b.windowStart >= RATE_WINDOW_MS) {
    if (rateBuckets.size > 10_000) rateBuckets.clear(); // 防表无限增长
    rateBuckets.set(ip, { windowStart: now, count: 1 });
    return false;
  }
  b.count += 1;
  return b.count > RATE_LIMIT;
}

// 页面版本占位符替换(本地 dev):与 CI 部署管线同一占位符 __PAGE_VERSION__。
// 取 git describe(如 dev-v0.2.2-83-g71724a8),让日志页标可追溯到具体源码;
// git 不可用或不在仓库内时降级 dev-unknown(不阻断服务)。
function pageVersion() {
  try {
    const sha = execSync(
      "git describe --tags --always --dirty 2>/dev/null",
      { cwd: PAGE_DIR, encoding: "utf8", timeout: 3000 },
    ).trim();
    return sha ? `dev-${sha}` : "dev-unknown";
  } catch {
    return "dev-unknown";
  }
}
const PAGE_VERSION = pageVersion();
// 页面与静态模块均按请求现读,不在启动时缓存(页面迭代期免重启)
// 页面以 type=module 引入的同目录 ES 模块;白名单按需登记
const STATIC_FILES = new Map([
  ["/extract-app-image.js", { file: "extract-app-image.js", type: "text/javascript; charset=utf-8" }],
  ["/name-blob.js", { file: "name-blob.js", type: "text/javascript; charset=utf-8" }],
  ["/store-analyze.js", { file: "store-analyze.js", type: "text/javascript; charset=utf-8" }],
  ["/phone-install.js", { file: "phone-install.js", type: "text/javascript; charset=utf-8" }],
  ["/slot-backup.js", { file: "slot-backup.js", type: "text/javascript; charset=utf-8" }],
  ["/launcher-upgrade.js", { file: "launcher-upgrade.js", type: "text/javascript; charset=utf-8" }],
  // 本地化的 esptool-js 及其依赖(jsdelivr +esm 构建,国内 CDN 不可达时页面整体卡死)
  ["/vendor/esptool-js.js", { file: "vendor/esptool-js.js", type: "text/javascript; charset=utf-8" }],
  ["/vendor/md5.js", { file: "vendor/md5.js", type: "text/javascript; charset=utf-8" }],
  ["/vendor/pako.js", { file: "vendor/pako.js", type: "text/javascript; charset=utf-8" }],
  ["/vendor/atob-lite.js", { file: "vendor/atob-lite.js", type: "text/javascript; charset=utf-8" }],
  ["/vendor/esp32c3.js", { file: "vendor/esp32c3.js", type: "text/javascript; charset=utf-8" }],
  ["/vendor/stub_flasher_32c3.js", { file: "vendor/stub_flasher_32c3.js", type: "text/javascript; charset=utf-8" }],
]);

function sendJson(res, status, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
  });
  res.end(body);
}

function sendError(res, status, msg) {
  sendJson(res, status, { error: msg });
}

// 代理 GET 请求到 BACKEND + upstreamPath，转发 content-type，二进制流式转发。
// 失败返回 502 + 错误文本。
async function proxyFetch(upstreamPath, res) {
  try {
    const upstream = await fetch(BACKEND + upstreamPath, { redirect: "follow" });
    if (!upstream.ok) {
      sendError(res, 502, `upstream ${upstream.status}: ${upstream.statusText}`);
      return;
    }
    const headers = {
      "content-type": upstream.headers.get("content-type") || "application/octet-stream",
    };
    const len = upstream.headers.get("content-length");
    if (len != null) headers["content-length"] = len;
    res.writeHead(200, headers);
    // 流式转发 body，避免整包入内存
    const reader = upstream.body.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!res.write(Buffer.from(value))) {
        await new Promise((resolve) => res.once("drain", resolve));
      }
    }
    res.end();
  } catch (err) {
    if (!res.headersSent) {
      sendError(res, 502, `proxy failed: ${err.message}`);
    } else {
      res.destroy(err);
    }
  }
}

const server = http.createServer((req, res) => {
  let urlObj;
  try {
    urlObj = new URL(req.url, `http://localhost:${PORT}`);
  } catch {
    sendError(res, 400, "malformed URL");
    return;
  }
  const pathname = urlObj.pathname;

  if (req.method !== "GET") {
    sendError(res, 405, "method not allowed");
    return;
  }

  // GET / → 安装页(规范目录 install-slot/)
  if (pathname === "/" || pathname === "/install-slot.html") {
    res.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
      // 页面与模块迭代期禁用缓存:浏览器缓存可能比仓库代码旧(ES 模块同样受限),
      // 曾导致"改了但页面没体现"。生产环境(Cloudflare Pages)发自己的缓存策略,不受影响。
      "cache-control": "no-store",
    });
    // 每请求现读:页面迭代期免重启;版本占位符随响应替换为当前 git describe
    const html = fs.readFileSync(path.join(PAGE_DIR, "install-slot.html"), "utf8")
      .replaceAll("__PAGE_VERSION__", PAGE_VERSION);
    res.end(html);
    return;
  }

  // 白名单静态文件（页面引入的 ES 模块等）
  const staticEntry = STATIC_FILES.get(pathname);
  if (staticEntry) {
    res.writeHead(200, {
      "content-type": staticEntry.type,
      "cache-control": "no-store",   // 同上:迭代期模块也必须新鲜
    });
    fs.createReadStream(path.join(PAGE_DIR, staticEntry.file)).pipe(res);
    return;
  }

  // (phone-install.js 已入上方白名单;CORS 由白名单通道统一处理 —— 本地 dev
  // 页面同源加载,手机安装模块仅在 metapass/设备源加载,不需本地 CORS。)

  // GET /api/plays → 玩法列表（JSON 透传）
  if (pathname === "/api/plays") {
    proxyFetch("/api/plays", res);
    return;
  }

  // GET /api/play?id=N → 单个玩法详情
  if (pathname === "/api/play") {
    const id = urlObj.searchParams.get("id");
    if (id == null || !/^\d+$/.test(id)) {
      sendError(res, 400, "missing or invalid id parameter");
      return;
    }
    proxyFetch(`/api/plays/id/${id}`, res);
    return;
  }

  // GET /api/analyze?id=N → 设备 P2 详情页信息源(名称/可装性/最小槽位)。
  // 业务结果(含 supported=false 的原因码)一律 200 + 契约 JSON——设备端对非 200
  // 不读体,422 会把 too-large/wrong-chip 等真实原因吞成 unavailable。
  if (pathname === "/api/analyze") {
    if (rateLimited(res.socket.remoteAddress ?? "?")) {
      sendError(res, 429, "rate limited");
      return;
    }
    const id = urlObj.searchParams.get("id");
    if (id == null || !/^\d{1,7}$/.test(id)) {
      sendError(res, 400, "missing or invalid id parameter");
      return;
    }
    storeAnalyzer.analyze(Number(id)).then(
      (out) => sendJson(res, 200, out),
      // r10.4:内部异常也回完整契约 JSON(reason+detail),不回裸 502 ——
      // 设备端对非 200 不读体,会把真实异常吞成传输失败。
      (err) => sendJson(res, 200, {
        ok: false,
        id: Number(id),
        revisionId: null,
        name: null,
        store: null,
        extracted: null,
        slots: null,
        suggestedSlot: -1,
        supported: false,
        reason: "unavailable",
        detail: `server exception: ${String(err && err.message ? err.message : err).slice(0, 80)}`,
      }),
    );
    return;
  }

  // GET /api/extracted?id=N → 已验证的 factory 应用镜像(二进制流式下发,
  // 头携带 x-image-len / x-image-sha256,设备边下边算与此比对;字节直接来自
  // analyze 阶段已校验合并镜像的解包缓存,不重复回源/解包)。
  if (pathname === "/api/extracted") {
    if (rateLimited(res.socket.remoteAddress ?? "?")) {
      sendError(res, 429, "rate limited");
      return;
    }
    const id = urlObj.searchParams.get("id");
    if (id == null || !/^\d{1,7}$/.test(id)) {
      sendError(res, 400, "missing or invalid id parameter");
      return;
    }
    storeAnalyzer.extractedStream(Number(id)).then(
      (out) => {
        if (out.error) {
          // r10.4:失败也回契约 JSON + x-debug-detail 头(层位可见)。
          res.writeHead(out.error === "not-found" ? 404 : 502, {
            "content-type": "application/json; charset=utf-8",
            "access-control-allow-origin": "*",
            "cache-control": "no-store",
            ...(out.detail ? { "x-debug-detail": String(out.detail).slice(0, 120) } : {}),
          });
          res.end(JSON.stringify({
            ok: false,
            id: Number(id),
            revisionId: null,
            name: null,
            store: null,
            extracted: null,
            slots: null,
            suggestedSlot: -1,
            supported: false,
            reason: out.error,
            ...(out.detail ? { detail: out.detail } : {}),
          }));
          return;
        }
        res.writeHead(200, {
          "content-type": "application/octet-stream",
          "content-length": out.imageLen,
          "x-image-len": String(out.imageLen),
          "x-image-sha256": out.sha256,
        });
        res.end(Buffer.from(out.stream));
      },
      (err) => sendError(res, 502, `extracted failed: ${err.message}`),
    );
    return;
  }

  // GET /api/firmware?path=<encoded> → 固件下载；path 必须以 /api/download/ 开头
  if (pathname === "/api/firmware") {
    const p = urlObj.searchParams.get("path") ?? "";
    if (!p.startsWith("/api/download/")) {
      sendError(res, 403, "forbidden path");
      return;
    }
    proxyFetch(p, res);
    return;
  }

  sendError(res, 404, "not found");
});

server.on("error", (err) => {
  if (err.code === "EADDRINUSE") {
    console.error(`port ${PORT} is already in use; set PORT env to change.`);
  } else {
    console.error("server error:", err.message);
  }
  process.exit(1);
});

server.listen(PORT, () => {
  console.log(`install-slot server: http://localhost:${PORT}/`);
  console.log(`Open http://localhost:${PORT}/ in Chrome (Web Serial requires a secure context).`);
});
