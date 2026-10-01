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

// r10.20-H2:缓存键只含路径+id。ts/sig 是每请求凭证,绝不入键——否则 analyze
// 预热(无票)与固件(带票)各占一个条目,预热失效,且每个 (ts,sig) 组合都留下
// 一次性 2.6MB 条目。
function extractedEdgeKey(url) {
  const u = new URL(url);
  const keyUrl = `${u.origin}/api/extracted?id=${encodeURIComponent(u.searchParams.get("id") ?? "")}`;
  return new Request(keyUrl, { method: "GET" });
}

function edgeHeaders(out, byteLen) {
  return {
    "content-type": "application/octet-stream",
    "x-image-len": String(out.imageLen ?? byteLen),
    "x-image-sha256": out.sha256 ?? "",
    // r10.20-H2: 边缘命中可识别(与 r2/computed/r2-range/computed-range 并列;
    // 审计关闭记录要求生产可实测"预热条目服务带票请求")。
    "x-source": "edge",
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

async function putExtractedToEdge(url, out) {
  try {
    await caches.default.put(extractedEdgeKey(url), new Response(out.stream.slice(), {
      status: 200,
      headers: edgeHeaders(out, out.imageLen),
    }));
  } catch { /* quota/unavailable: serve uncached */ }
}

async function warmEdgeExtracted(req, id) {
  try {
    const u = new URL(req.url);
    u.pathname = "/api/extracted";
    u.search = `id=${encodeURIComponent(String(id))}`; // r10.20-H2:键只含 id
    const key = extractedEdgeKey(u);
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
    const headers = {
      "content-type": upstream.headers.get("content-type") || "application/octet-stream",
      "access-control-allow-origin": "*",
    };
    // 流式转发(设计 §4.3 item 4):不把数 MB 合并镜像整包缓冲进 isolate 内存。
    const len = upstream.headers.get("content-length");
    if (len != null) headers["content-length"] = len;
    return new Response(upstream.body, { status: 200, headers });
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
const DL_TICKET_MAX_AGE_S = 600;        // analyze 签发票据有效期(dl.max_age 同值)
// r10.20-H5:Range 续传专用窗口。固件每条续传连接复用 analyze 下发的票;10 分钟
// 盖不住"慢链路+多次续传"的总时长,过期掉回 computed 慢路径会复活本次改造要消灭
// 的问题。格式有效的票在续传请求上按此窗口仍受信;字节信任链不变。
const DL_RANGE_TICKET_MAX_AGE_S = 3600;
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
async function downloadTicket(env, url, idStr, maxAgeS) {
  if (!env.DL_SECRET) return { ticketed: false, reason: "no-secret" };
  const sig = url.searchParams.get("sig");
  if (!sig || !/^[0-9a-f]{16}$/.test(sig)) return { ticketed: false, reason: "missing" };
  const ts = Number(url.searchParams.get("ts"));
  const maxAge = maxAgeS ?? DL_TICKET_MAX_AGE_S;
  if (!Number.isFinite(ts) || Math.abs(Date.now() / 1000 - ts) > maxAge) {
    return { ticketed: false, reason: "expired" };
  }
  const expect = await hmacHex16(env.DL_SECRET, `${idStr}:${ts}`);
  if (!timingSafeEq(expect, sig)) return { ticketed: false, reason: "bad" };
  return { ticketed: true, reason: "ok" };
}

// r10.19:断点续传 —— 固件(r10.17)对中断安装发 `Range: bytes=<start>-`,期待
// 206 + `Content-Range: bytes <first>-<last>/<total>`(设备端 meta_store_range_parse
// 只认这一种形态:带空格的 "bytes " 前缀、span 必须等于该响应 content-length、
// total 必须等于 analyze 的 image_len)。只实现固件会发的 suffix-range 子集;
// 其余 Range 形态按 RFC 9110 忽略(回 200 全量,固件会作废 OTA 降级整单重试)。
// 返回 null = 无 Range 或非此形态;数字 = 起始偏移(>=total 由调用方判 416)。
function parseSuffixRange(req) {
  const h = req.headers.get("range");
  if (!h) return null;
  const m = /^bytes=(\d+)-$/.exec(h.trim());
  return m ? Number(m[1]) : null;
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
    // /api/plays 透传 query(设计 §4.3 item 1:q/multiDevice 必须到达上游,
    // 否则搜索退化为全量列表客户端切片 —— 审计 B3,生产实测已复现)。
    if (path === "/api/plays") return proxy("/api/plays" + url.search);

    if (path === "/api/play") {
      const id = url.searchParams.get("id");
      if (!/^\d+$/.test(id)) return err(400, "missing or invalid id parameter");
      return proxy(`/api/plays/id/${id}`);
    }

    // ── 手机安装固件下载(纯 CORS 代理,无服务端缓存) ────────────────────
    // 用户定稿:不物化市场固件。手机经此代理直连官方市场下载(官方无 CORS
    // 头,浏览器读响应必须代一趟),剥离与写入全在手机/设备侧完成;字节信任
    // = 手机侧商店 sha256 三道门。历史 R2 中的 firmware/* 孤儿对象已清理。
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
      const rangeStart = parseSuffixRange(req);
      // r10.20-H5:续传请求用长票据窗口(见 DL_RANGE_TICKET_MAX_AGE_S)。
      const ticket = await downloadTicket(env, url, id,
        rangeStart !== null ? DL_RANGE_TICKET_MAX_AGE_S : undefined);
      // r10.15b:边缘缓存命中直接直出(老链路快路径;字节只能来自已验证产物)。
      // r10.19:Range(续传)请求绕过缓存读 —— 缓存键不含 Range,缓存体恒为
      // 200 全量;206 绝不能命中也绝不能写入该键(见下方两条供给路径)。
      // r10.20-H6:边缘缓存 1h TTL 内的条目可能旧于市场当前镜像,而固件对摘要头
      // 缺失/不一致是确定性失败 —— 命中必须先与当前 analyze 摘要复核,不符即
      // 丢弃陈旧条目落现算路径(绝不拿旧字节冒新声明)。
      if (rangeStart === null) {
        try {
          const hit = await caches.default.match(extractedEdgeKey(url));
          if (hit) {
            try {
              const cur = await storeAnalyzer.analyze(Number(id));
              if (cur && cur.ok && cur.extracted?.sha256
                  && hit.headers.get("x-image-sha256") === cur.extracted.sha256
                  && Number(hit.headers.get("x-image-len")) === cur.extracted.imageLen) {
                return hit;
              }
              try { await caches.default.delete(extractedEdgeKey(url)); } catch { /* best-effort */ }
            } catch { /* analyze 失败:不信任缓存条目,落现算 */ }
          }
        } catch { /* cache unavailable: normal path */ }
      }
      // r10.18:R2 按需物化命中路径 —— 仅有效票据(ticket = R2 专属钥匙)。
      // analyze(内存缓存命中时纯内存)取 store.sha256 构造键;命中且
      // sha256/imageLen 与 analyze 声明一致 → 直出,跳过回源+解包全流程。
      // 不一致或 miss → 落回完整信任链路径(现算后写 R2)。
      if (ticket.ticketed) {
        try {
          const meta = await storeAnalyzer.analyze(Number(id));
          if (meta && meta.ok && meta.store?.sha256 && env.meta_pass_extracted) {
            const key = r2Key(Number(id), meta.store.sha256);
            const total = meta.extracted.imageLen;
            // r10.19:续传走 R2 原生区间读(get(key,{range})),不把整对象
            // 物化进 isolate 内存;R2 miss/元数据不符 → 落回现算切片路径。
            if (rangeStart !== null) {
              if (rangeStart >= total) {
                return new Response(JSON.stringify({ error: "range not satisfiable" }), {
                  status: 416,
                  headers: { "content-range": `bytes */${total}`,
                             "cache-control": "no-store" },
                });
              }
              const obj = await env.meta_pass_extracted.get(key, { range: { offset: rangeStart } });
              if (obj && obj.body
                  && obj.customMetadata?.["x-image-sha256"] === meta.extracted.sha256
                  && Number(obj.customMetadata?.["x-image-len"]) === total) {
                return new Response(obj.body, {
                  status: 206,
                  headers: {
                    "content-type": "application/octet-stream",
                    // r10.20-H3:区间长度显式计算(obj.size 是全对象元数据,
                    // 依赖运行时对 ranged 响应做规范化是隐式行为)。
                    "content-length": String(total - rangeStart),
                    "content-range": `bytes ${rangeStart}-${total - 1}/${total}`,
                    "x-image-len": String(total),
                    "x-image-sha256": meta.extracted.sha256,
                    "x-source": "r2-range",
                    "cache-control": "no-store",
                  },
                });
              }
            } else {
              const obj = await env.meta_pass_extracted.get(key);
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
        // r10.19:现算路径的续传供给 —— analyze 缓存里的全镜像 Uint8Array
        // 直接切片(只读副本);206 在此提前返回,绝不下探到缓存/物化写路径。
        if (rangeStart !== null) {
          const total = out.imageLen;
          if (rangeStart >= total) {
            return new Response(JSON.stringify({ error: "range not satisfiable" }), {
              status: 416,
              headers: { "content-range": `bytes */${total}`,
                         "cache-control": "no-store" },
            });
          }
          const body = out.stream.slice(rangeStart);
          return new Response(body, {
            status: 206,
            headers: {
              "content-type": "application/octet-stream",
              "content-length": String(body.byteLength),
              "content-range": `bytes ${rangeStart}-${total - 1}/${total}`,
              "x-image-len": String(total),
              "x-image-sha256": out.sha256,
              "x-source": "computed-range",
              "cache-control": "no-store",
            },
          });
        }
        // 现算成功。r10.20-H4:持久化(边缘缓存+R2 对象+映射)全部移入
        // ctx.waitUntil 后台执行,响应先行返回 —— 设备首字节时间不再含两笔
        // 2.6MB 写。out.stream 实为 Uint8Array(store-analyze.js 契约);响应体
        // 与 R2 写体各自独立复制,后台闭包独占自己的副本,不共享可变 buffer。
        const edgeBody = out.stream.slice();
        const respBody = out.stream.slice();
        // r10.18:R2 写入同样仅有效票据 —— 未授权流量不得消耗写配额/键空间。
        if (ticket.ticketed && env.meta_pass_extracted) {
          ctx.waitUntil((async () => {
            try {
              await putExtractedToEdge(url, out);
            } catch { /* 边缘缓存尽力而为 */ }
            try {
              const m = await storeAnalyzer.analyze(Number(id));
              const storeSha = m && m.store ? m.store.sha256 : null;
              if (storeSha) {
                await env.meta_pass_extracted.put(
                  r2Key(Number(id), storeSha), edgeBody, {
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
              }
            } catch (e) {
              console.error("r2 materialize failed:",
                String(e && e.message ? e.message : e).slice(0, 80));
            }
          })());
        }
        return new Response(respBody, {
          status: 200,
          headers: {
            "content-type": "application/octet-stream",
            "content-length": String(out.imageLen),
            "x-image-len": String(out.imageLen),
            "x-image-sha256": out.sha256,
            "x-source": "computed",
            "cache-control": "no-store",
          },
        });
      } catch (e) {
        return err(502, `extracted failed: ${e && e.message ? e.message : e}`);
      }
    }

    // ── 诊断:R2 绑定 + 票据链路(只回布尔/原因码,不泄 secret/对象) ──
    if (path === "/api/r2check") {
      // 诊断也可验长续传窗口:?range=1 按 DL_RANGE_TICKET_MAX_AGE_S 检查。
      const t = await downloadTicket(env, url, url.searchParams.get("id") ?? "0",
        url.searchParams.get("range") === "1" ? DL_RANGE_TICKET_MAX_AGE_S : undefined);
      return new Response(JSON.stringify({
        r2Bound: !!env.meta_pass_extracted,
        secretSet: !!env.DL_SECRET,
        ticketReason: t.reason,
      }), { headers: { "content-type": "application/json", "cache-control": "no-store" } });
    }

    // ── 手机安装模块(设计文档 §4.2:metapass 供版本化 JS,CORS *) ──
    // 设备 boot 页(meta_store_install.c SHELL_HTML)以
    // <script type=module src=.../phone-install.js> 加载。单文件入口(内含
    // sha256 纯 JS 实现),同源 import extract-app-image/name-blob/
    // store-analyze(ASSETS 静态服务)。版本策略 = no-store + 设备协议握手
    // (status.protocol)双保险;大改版换文件名(boot 页 URL 随固件走)。
    // 入口与其静态 import 必须同走 no-store(审计 M4):入口 no-store 而 import
    // max-age=14400 时,部署后 4h 内手机会拿新入口配旧依赖,设备握手覆盖不到
    // 跨模块错位。
    if (path === "/phone-install.js" || path === "/extract-app-image.js" ||
        path === "/store-analyze.js" || path === "/name-blob.js") {
      const asset = await env.ASSETS.fetch(new Request(new URL(path, req.url), req));
      if (!asset.ok) return err(404, "phone-install module missing from bundle");
      const headers = new Headers(asset.headers);
      headers.set("access-control-allow-origin", "*");
      headers.set("cache-control", "no-store");
      return new Response(asset.body, { status: asset.status, headers });
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
