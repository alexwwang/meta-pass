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

// r10.15b:同 zone 边缘缓存。上一版把回源响应(folotoy.cn)写进 caches.default ——
// Cache API 只允许缓存本 zone 的 URL,put 被静默拒绝,生产实测缓存从未命中
// (fast/slow/slow/fast 锯齿依旧)。改为缓存*设备-facing*的响应:
//   - key = 本 zone 的 /api/extracted?id=N(同 zone,put/match 必然可用);
//   - 缓存体 = 已解包、已验证的 factory 镜像字节 + x-image-len/sha256 元数据头;
//   - analyze(P2 详情页)成功后后台预热同 key —— 浏览详情页即预填,点安装时
//     直接边缘直出,与“静态文件下载”同形态(参照项目的快路径);
//   - 信任链不变:缓存里的字节只能来自“商店公布 SHA256 校验 + 解包”的产物,
//     任何冷构建仍强制走完整校验,缓存字节永远绕不过验证。
const EDGE_TTL = 3600;

function extractedEdgeKey(req) {
  return new Request(req.url, { method: "GET" });
}

function edgeHeaders(out, byteLen) {
  return {
    "content-type": "application/octet-stream",
    "x-image-len": String(out.imageLen ?? byteLen),
    "x-image-sha256": out.sha256 ?? "",
    "cache-control": `public, max-age=${EDGE_TTL}`,
  };
}

// r10.18:R2 键约定 —— 键内嵌"玩法原始固件 sha256"(定稿要求:R2 上的固件
// 必须记录对应的原始玩法固件 sha256;市场侧固件更新 → sha 变 → 键自然 miss
// → 重新解包写新键,旧键自然过时;命中路径还会用 analyze 的 extracted.sha256
// 复核 customMetadata,双重防陈旧)。同键 .meta.json 存完整映射供审计。
function r2Key(id, storeSha256) {
  return `extracted/${id}/${storeSha256}.bin`;
}

async function putExtractedToEdge(req, out) {
  try {
    await caches.default.put(extractedEdgeKey(req), new Response(out.stream.slice(), {
      status: 200,
      headers: edgeHeaders(out, out.imageLen),
    }));
  } catch { /* quota/unavailable: serve uncached */ }
}

async function warmEdgeExtracted(req, id) {
  try {
    const u = new URL(req.url);
    u.pathname = "/api/extracted";
    const key = new Request(u.toString(), { method: "GET" });
    if (await caches.default.match(key)) return;
    const out = await storeAnalyzer.extractedStream(id);
    if (out.error || !out.stream) return;
    await caches.default.put(key, new Response(out.stream.slice(), {
      status: 200,
      headers: edgeHeaders(out, out.imageLen),
    }));
  } catch { /* warming is best-effort */ }
}

// store-analyze.js 契约:fetchImpl 返回 {ok,status,statusText,url,arrayBuffer},
// sha256 接收 Uint8Array 返回 64 位小写 hex。
const storeAnalyzer = createStoreAnalyzer({
  fetchImpl: (url) => fetch(url, { redirect: "follow" }),
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

// ── r10.17:安全防护 ─────────────────────────────────────────────────
// /api/extracted 的 R2 物化是唯一昂贵快路径,防滥用设计(r10.18 定稿):
//   P1. 票据 = R2 专属钥匙(不是准入门槛):analyze 成功时下发
//       sig = HMAC-SHA256(DL_SECRET, id:ts) 截 16 hex;extracted 原样带回
//       ?sig=&ts= 才能走 R2 物化路径。无票/坏票/过期票一律不拒绝 —— 降级
//       r10.15b 老链路(边缘缓存+现算),R2 读/写完全不发生(定稿规则:
//       "没有票据只能走老的链路,不能访问 R2")。旧固件(不带 sig)零成本兼容。
//       威胁模型:sig 抓包可重放至过期 —— 危害仅是快路径带宽/R2 读配额;
//       HMAC 单向,算法开源也伪造不了(安全性只依赖 secret);R2 写键由服务端
//       在市场 sha 校验后构造,外部不可注入。
//   P2. 限速:每 (IP,id) 每分钟 N 次,KV 计数,超限 429 —— 两条链路都生效。
//   P3. R2 读/写仅在有票且市场 sha 校验通过后发生(信任链内),键由 id+sha 构造。
//   兜底:DL_SECRET 未配置时所有请求都走老链路(R2 永不启用,功能不受损)。
const DL_TICKET_MAX_AGE_S = 600;
const RATE_LIMIT_PER_MIN = 6;

async function hmacHex16(secret, msg) {
  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw", enc.encode(secret),
    { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, enc.encode(msg));
  // 票据标签 = 16 hex 字符 = 64bit:必须先截字节前 8 个,再逐字节转 hex。
  // 历史教训(两次):hex 化后 .slice(0,16) 切的是"64 个单字符组成的数组"的前 16
  // 个元素,join 仍是 32 字符;字节侧截 16 字节也得 32 字符。两种错法都曾把
  // 签发 sig 弄成 32 字符,与校验方 {16} 正则永不匹配 → 所有票据 invalid。
  return [...new Uint8Array(mac)].slice(0, 8).map((b) => b.toString(16).padStart(2, "0")).join("");
}

function timingSafeEq(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// 票据校验:只回答"能不能吃 R2 快路径",绝不做准入拒绝(r10.18 定稿语义)。
async function downloadTicket(env, url, idStr) {
  if (!env.DL_SECRET) return { ticketed: false, reason: "no-secret" };
  const sig = url.searchParams.get("sig");
  if (!sig || !/^[0-9a-f]{16}$/.test(sig)) return { ticketed: false, reason: "missing" };
  const ts = Number(url.searchParams.get("ts"));
  if (!Number.isFinite(ts) || Math.abs(Date.now() / 1000 - ts) > DL_TICKET_MAX_AGE_S) {
    return { ticketed: false, reason: "expired" };
  }
  const expect = await hmacHex16(env.DL_SECRET, `${idStr}:${ts}`);
  if (!timingSafeEq(expect, sig)) return { ticketed: false, reason: "bad" };
  return { ticketed: true, reason: "ok" };
}

async function rateLimit(env, req, idStr) {
  if (!env.RATE_KV) return true; // 未绑 KV(本地 dev):不设限;生产环境绑定后生效
  const ip = req.headers.get("cf-connecting-ip") ?? "unknown";
  const window = Math.floor(Date.now() / 60000);
  const key = `rl:${ip}:${idStr}:${window}`;
  const cur = Number((await env.RATE_KV.get(key)) ?? "0") + 1;
  await env.RATE_KV.put(key, String(cur), { expirationTtl: 120 });
  return cur <= RATE_LIMIT_PER_MIN;
}

export default {
  async fetch(req, env, ctx) {
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
        // r10.18:成功时下发下载票据(HMAC,与玩法 id + 签发时间绑定,10 分钟有效);
        // 设备原样带回 sig+ts 才能吃 R2 快路径(无票/过期 = 自动降级老链路)。
        if (out && out.ok && env.DL_SECRET) {
          const ts = Math.floor(Date.now() / 1000);
          out.dl = {
            sig: await hmacHex16(env.DL_SECRET, `${id}:${ts}`),
            ts,
            max_age: DL_TICKET_MAX_AGE_S,
          };
        }
        // r10.15b:P2 详情页即预热 —— 浏览过详情的玩法,点安装时 extracted
        // 边缘缓存已就绪,设备直接吃缓存字节,不再撞回源停顿/502。
        if (out && out.supported) {
          ctx.waitUntil(warmEdgeExtracted(req, Number(id)));
        }
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
      // r10.18:限速对两条链路都生效(同一出口带宽);票据只决定 R2 快路径。
      if (!(await rateLimit(env, req, id))) {
        const h = { "content-type": "application/json", "cache-control": "no-store",
                    "retry-after": "60" };
        return new Response(JSON.stringify({ error: "rate limited" }),
                            { status: 429, headers: h });
      }
      const ticket = await downloadTicket(env, url, id);
      // r10.15b:边缘缓存命中直接直出(老链路快路径;字节只能来自已验证产物)。
      try {
        const hit = await caches.default.match(extractedEdgeKey(req));
        if (hit) return hit;
      } catch { /* cache unavailable: normal path */ }
      // r10.18:R2 按需物化命中路径 —— 仅有效票据(ticket = R2 专属钥匙)。
      // analyze(内存缓存命中时纯内存)取 store.sha256 构造键;命中且
      // sha256/imageLen 与 analyze 声明一致 → 直出,跳过回源+解包全流程。
      // 不一致或 miss → 落回完整信任链路径(现算后写 R2)。
      if (ticket.ticketed) {
        try {
          const meta = await storeAnalyzer.analyze(Number(id));
          if (meta && meta.ok && meta.store?.sha256 && env.meta_pass_extracted) {
            const obj = await env.meta_pass_extracted.get(r2Key(Number(id), meta.store.sha256));
            if (obj && obj.body
                && obj.customMetadata?.["x-image-sha256"] === meta.extracted.sha256
                && Number(obj.customMetadata?.["x-image-len"]) === meta.extracted.imageLen) {
              return new Response(obj.body, {
                status: 200,
                headers: {
                  "content-type": "application/octet-stream",
                  "content-length": String(obj.size),
                  "x-image-len": String(obj.size),
                  "x-image-sha256": meta.extracted.sha256,
                  "x-source": "r2",
                  "cache-control": "no-store",
                },
              });
            }
          }
        } catch { /* R2 miss/不可用/analyze 失败:走完整信任链路径 */ }
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
        // 现算成功:写边缘缓存(下一个请求/下一台设备直出) + R2 物化(跨实例/跨
        // POP 持久,消灭后续 Worker 冷启动回源慢路径),并回给本请求。
        // 体是 ReadableStream,需要复制才能同时 put + serve。
        await putExtractedToEdge(req, out);
        // r10.18:R2 写入同样仅有效票据 —— 未授权流量不得消耗写配额/键空间。
        // 失败原因透传到 x-r2-write 响应头(诊断;生产稳定后此头仍有用且无敏感信息)。
        let r2Write = "skipped";
        if (ticket.ticketed && env.meta_pass_extracted) {
          try {
            const m = await storeAnalyzer.analyze(Number(id));
            const storeSha = m && m.store ? m.store.sha256 : null;
            if (storeSha) {
              await env.meta_pass_extracted.put(
                r2Key(Number(id), storeSha), out.stream.slice(), {
                  httpMetadata: { contentType: "application/octet-stream" },
                  customMetadata: {
                    "x-image-len": String(out.imageLen),
                    "x-image-sha256": out.sha256,
                    "x-revision-id": m.revisionId == null ? "" : String(m.revisionId),
                  },
                });
              // 映射文件:R2 固件 ↔ 原始玩法固件 sha256 + revisionId(审计用)。
              await env.meta_pass_extracted.put(
                `${r2Key(Number(id), storeSha)}.meta.json`, JSON.stringify({
                  id: Number(id),
                  storeFwSha256: storeSha,
                  extractedSha256: out.sha256,
                  imageLen: out.imageLen,
                  revisionId: m.revisionId == null ? null : m.revisionId,
                  writtenAt: new Date().toISOString(),
                }), { httpMetadata: { contentType: "application/json" } });
              r2Write = `ok:${r2Key(Number(id), storeSha)}`;
            } else {
              r2Write = "no-store-sha";
            }
          } catch (e) {
            r2Write = `err:${String(e && e.message ? e.message : e).slice(0, 80)}`;
          }
        }
        return new Response(out.stream, {
          status: 200,
          headers: {
            "content-type": "application/octet-stream",
            "content-length": String(out.imageLen),
            "x-image-len": String(out.imageLen),
            "x-image-sha256": out.sha256,
            "x-source": "computed",
            "x-r2-write": r2Write,
            "cache-control": "no-store",
          },
        });
      } catch (e) {
        return err(502, `extracted failed: ${e && e.message ? e.message : e}`);
      }
    }

    // ── 诊断:R2 绑定 + 票据链路(只回布尔/原因码,不泄 secret/对象) ──
    if (path === "/api/r2check") {
      const t = await downloadTicket(env, url, url.searchParams.get("id") ?? "0");
      return new Response(JSON.stringify({
        r2Bound: !!env.meta_pass_extracted,
        secretSet: !!env.DL_SECRET,
        ticketReason: t.reason,
      }), { headers: { "content-type": "application/json", "cache-control": "no-store" } });
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
