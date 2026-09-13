// install-slot/proxy.mjs —— Cloudflare Workers 反向代理。
// 将 /api/* 请求透明转发到 ai-passport.folotoy.cn，并补上 CORS 头。
// 原 tools/install-slot/server.mjs 在本地开发时仍可使用；本页由用户自选。

const BACKEND = "https://ai-passport.folotoy.cn";
// SSRF 白名单:只允许代理这两条路径,避免 Workers 被滥用
const ALLOWED_PATHS = ["/api/plays", "/api/download/"];
const ALLOWED_PREFIXES = ["/api/plays/id/"];

function ok(body, contentType = "application/json", extra = {}) {
  const h = {
    "content-type": contentType,
    "access-control-allow-origin": "*",
    ...extra,
  };
  return new Response(body, { status: 200, headers: h });
}

function err(status, msg) {
  return new Response(JSON.stringify({ error: msg }), {
    status,
    headers: { "content-type": "application/json", "access-control-allow-origin": "*" },
  });
}

async function proxy(cfwCtx, upstreamPath) {
  try {
    const upstream = await fetch(BACKEND + upstreamPath, { redirect: "follow" });
    if (!upstream.ok) {
      return err(502, `upstream ${upstream.status}: ${upstream.statusText}`);
    }
    const ct = upstream.headers.get("content-type") || "application/octet-stream";
    const body = await upstream.arrayBuffer();
    return new Response(body, {
      status: 200,
      headers: {
        "content-type": ct,
        "content-length": String(body.byteLength),
        "access-control-allow-origin": "*",
      },
    });
  } catch (e) {
    return err(502, `proxy failed: ${e.message}`);
  }
}

addEventListener("fetch", (event) => {
  event.respondWith(handle(event.request));
});

async function handle(req) {
  const url = new URL(req.url);
  if (req.method !== "GET") return err(405, "method not allowed");

  if (url.pathname === "/api/plays") {
    return proxy(req, "/api/plays");
  }

  if (url.pathname === "/api/play") {
    const id = url.searchParams.get("id");
    if (!/^\d+$/.test(id)) return err(400, "missing or invalid id parameter");
    return proxy(req, `/api/plays/id/${id}`);
  }

  if (url.pathname === "/api/firmware") {
    const p = url.searchParams.get("path") ?? "";
    if (!p.startsWith("/api/download/")) return err(403, "forbidden path");
    return proxy(req, p);
  }

  return err(404, "not found");
}
