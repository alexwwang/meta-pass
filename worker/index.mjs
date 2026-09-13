// worker/index.mjs —— Cloudflare Worker:静态页面 + API 反向代理。
// 访问 / 返回安装页面;/api/* 转发到 ai-passport.folotoy.cn。
// 页面里的 fetch('/api/...') 走同源请求,天然绕过浏览器跨域限制。

const BACKEND = "https://ai-passport.folotoy.cn";

function err(status, msg) {
  return new Response(JSON.stringify({ error: msg }), {
    status,
    headers: { "content-type": "application/json", "access-control-allow-origin": "*" },
  });
}

async function proxy(upstreamPath) {
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

export default {
  async fetch(req, env) {
    if (req.method !== "GET" && req.method !== "HEAD") {
      return err(405, "method not allowed");
    }
    const url = new URL(req.url);
    const path = url.pathname;

    // ── API 反向代理 ─────────────────────────────────────────────────
    if (path === "/api/plays") return proxy("/api/plays");

    if (path === "/api/play") {
      const id = url.searchParams.get("id");
      if (!/^\d+$/.test(id)) return err(400, "missing or invalid id parameter");
      return proxy(`/api/plays/id/${id}`);
    }

    if (path === "/api/firmware") {
      const p = url.searchParams.get("path") ?? "";
      if (!p.startsWith("/api/download/")) return err(403, "forbidden path");
      return proxy(p);
    }
    // ── 静态文件服务 ─────────────────────────────────────────────────
    if (path === "/" || path === "") {
      // ASSETS 对 .html 文件会做规范化重定向(307),跟随获取最终内容
      let resp = await env.ASSETS.fetch(new Request(new URL("/install-slot.html", req.url), req));
      if (resp.status >= 300 && resp.status < 400) {
        resp = await env.ASSETS.fetch(new Request(new URL(resp.headers.get("location"), req.url), req));
      }
      return resp;
    }
    return env.ASSETS.fetch(req);
  },
};
