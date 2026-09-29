// install-slot/_worker.js —— Cloudflare Pages 高级模式 worker(本目录即部署根)。
// 静态页面 + API 反向代理 + 设备端 OTA 商店通道(analyze/extracted)。
//
// 设备端(与浏览器安装页)只认本域名:分析/解包在服务端完成,逻辑全部在
// ./store-analyze.js(纯 ESM、零 Node API,与 tools/install-slot/server.mjs 本地
// dev、13 项 node 自测同一份模块 —— 三端同源,行为不会漂移)。
//   GET /api/analyze?id=N    设备 P2 详情页唯一信息源(业务结果一律 200 + 契约 JSON)
//   GET /api/extracted?id=N  已验证 factory 镜像流(x-image-len / x-image-sha256)
//   GET /api/plays|play|firmware 既有浏览器代理路由,保持不变。
// sha256 用 WebCrypto subtle.digest(Workers 原生,毫秒级);合并镜像按 play id
// + revisionId 进程内缓存(隔离实例各自回源,正确性不受影响)。

import { createStoreAnalyzer } from "./store-analyze.js";

const BACKEND = "https://ai-passport.folotoy.cn";

// r10.15:回源边缘缓存 —— 真机日志证据(5% 段 28s、全程 ~4kB/s 龟速)定位到
// Worker 每次冷启动都在现场回源 folotoy.cn 拉 3MB 合并镜像→校验→解包,回源
// 链路本身在滴水(502/回源停顿均源于此)。参照形态:参照项目设备下载的就是
// 静态固件 URL,不存在每次现算。把两笔回源产物放进 CF Cache API:
//   - 合并镜像按 URL 缓存 1h:信任链(shopping 公布 sha256)在每次冷构建时
//     仍强制校验,坏字节/陈旧字节进不了下发流;
//   - 玩法元数据缓存 60s:revisionId 复核语义保留 ≤1 分钟陈旧窗口,换源站
//     一场回源 RTT;
// 设备-facing 响应仍 no-store(不改变既有契约),缓存只发生在回源层。
// analyze(P2 详情页)与 extracted(下载)共用本 fetch 层 —— 用户浏览详情页
// 即预热镜像,点安装时边缘已命中。
const IMG_TTL = 3600;
const META_TTL = 60;

async function cachedOriginFetch(url) {
  const cache = caches.default;
  const key = new Request(url, { method: "GET" });
  try {
    const hit = await cache.match(key);
    if (hit) return hit;
  } catch { /* cache unavailable: fall through to origin */ }
  const ttl = url.includes("/api/download/") ? IMG_TTL : META_TTL;
  const res = await fetch(url, {
    redirect: "follow",
    cf: { cacheEverything: true, cacheTtl: ttl },
  });
  if (res.ok) {
    try {
      const body = await res.arrayBuffer();
      const cached = new Response(body, {
        status: res.status,
        headers: {
          "content-type": res.headers.get("content-type") || "application/octet-stream",
          "cache-control": `public, max-age=${ttl}`,
        },
      });
      await cache.put(key, cached);
      // 重新包一层:body 已被读出,返回与原响应等价的新副本。
      return new Response(body, {
        status: res.status,
        headers: res.headers,
      });
    } catch { /* cache.put failed: the fresh response below is still valid */ }
  }
  return res;
}

// store-analyze.js 契约:fetchImpl 返回 {ok,status,statusText,url,arrayBuffer},
// sha256 接收 Uint8Array 返回 64 位小写 hex。
const storeAnalyzer = createStoreAnalyzer({
  fetchImpl: (url) => cachedOriginFetch(url),
  backend: BACKEND,
  sha256: async (buf) => {
    const d = await crypto.subtle.digest("SHA-256", buf);
    return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
  },
});

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

// analyze/extracted 的 id 校验与设备端/本地 dev 完全一致(变长数字,1~7 位)。
const PLAY_ID_RE = /^\d{1,7}$/;

export default {
  async fetch(req, env) {
    if (req.method !== "GET" && req.method !== "HEAD") {
      return err(405, "method not allowed");
    }
    const url = new URL(req.url);
    const path = url.pathname;

    // ── API 反向代理(浏览器安装页用) ─────────────────────────────────
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

    // ── 设备端 OTA 商店通道(与 tools/install-slot/server.mjs 同契约) ──
    // 业务结果(含 supported=false 的原因码)一律 200 + JSON:设备端对非 200
    // 不读体,4xx 会把 too-large/wrong-chip 等真实原因吞成 unavailable。
    if (path === "/api/analyze") {
      const id = url.searchParams.get("id");
      if (id == null || !PLAY_ID_RE.test(id)) {
        return err(400, "missing or invalid id parameter");
      }
      try {
        const out = await storeAnalyzer.analyze(Number(id));
        return new Response(JSON.stringify(out), {
          status: 200,
          headers: {
            "content-type": "application/json; charset=utf-8",
            "access-control-allow-origin": "*",
            // analyze JSON 与镜像绑定(revisionId 复核),不进 CDN 缓存。
            "cache-control": "no-store",
          },
        });
      } catch (e) {
        // r10.4:内部异常也必须回完整契约 JSON(reason=unavailable + detail=异常
        // 摘要),不回裸 502 —— 设备端对非 200 不读体,会把真实原因吞成传输失败。
        return new Response(JSON.stringify({
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
          detail: `worker exception: ${String(e && e.message ? e.message : e).slice(0, 80)}`,
        }), {
          status: 200,
          headers: {
            "content-type": "application/json; charset=utf-8",
            "access-control-allow-origin": "*",
            "cache-control": "no-store",
          },
        });
      }
    }

    if (path === "/api/extracted") {
      const id = url.searchParams.get("id");
      if (id == null || !PLAY_ID_RE.test(id)) {
        return err(400, "missing or invalid id parameter");
      }
      try {
        const out = await storeAnalyzer.extractedStream(Number(id));
        if (out.error) {
          // r10.4:失败带 detail 头(x-debug-detail),curl/设备日志可见层位。
          const hdrs = {
            "content-type": "application/json; charset=utf-8",
            "access-control-allow-origin": "*",
            "cache-control": "no-store",
          };
          if (out.detail) hdrs["x-debug-detail"] = String(out.detail).slice(0, 120);
          return new Response(JSON.stringify({
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
          }), { status: out.error === "not-found" ? 404 : 502, headers: hdrs });
        }
        return new Response(out.stream, {
          status: 200,
          headers: {
            "content-type": "application/octet-stream",
            "content-length": String(out.imageLen),
            "x-image-len": String(out.imageLen),
            "x-image-sha256": out.sha256,
            "cache-control": "no-store",
          },
        });
      } catch (e) {
        return err(502, `extracted failed: ${e && e.message ? e.message : e}`);
      }
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
