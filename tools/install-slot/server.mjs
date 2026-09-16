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
import { fileURLToPath } from "node:url";

const PORT = Number(process.env.PORT) || 4191;
const BACKEND = "https://ai-passport.folotoy.cn";
const DIR = path.dirname(fileURLToPath(import.meta.url));
const PAGE_DIR = path.resolve(DIR, "..", "..", "install-slot");
// 页面与静态模块均按请求现读,不在启动时缓存(页面迭代期免重启)
// 页面以 type=module 引入的同目录 ES 模块;白名单按需登记
const STATIC_FILES = new Map([
  ["/extract-app-image.js", { file: "extract-app-image.js", type: "text/javascript; charset=utf-8" }],
  ["/name-blob.js", { file: "name-blob.js", type: "text/javascript; charset=utf-8" }],
  ["/slot-backup.js", { file: "slot-backup.js", type: "text/javascript; charset=utf-8" }],
  ["/launcher-upgrade.js", { file: "launcher-upgrade.js", type: "text/javascript; charset=utf-8" }],
  // 本地化的 esptool-js 及其依赖(jsdelivr +esm 构建,国内 CDN 不可达时页面整体卡死)
  ["/vendor/esptool-js.js", { file: "vendor/esptool-js.js", type: "text/javascript; charset=utf-8" }],
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
    res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    // 每请求现读:页面迭代期免重启
    fs.createReadStream(path.join(PAGE_DIR, "install-slot.html")).pipe(res);
    return;
  }

  // 白名单静态文件（页面引入的 ES 模块等）
  const staticEntry = STATIC_FILES.get(pathname);
  if (staticEntry) {
    res.writeHead(200, { "content-type": staticEntry.type });
    fs.createReadStream(path.join(PAGE_DIR, staticEntry.file)).pipe(res);
    return;
  }

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
