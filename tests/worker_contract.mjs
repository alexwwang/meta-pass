// tests/worker_contract.mjs —— _worker.js 与 server.mjs 的 API 面契约测试。
// 锁定部署到 Cloudflare Pages 的 worker(install-slot/_worker.js)必须提供设备端
// 商店通道的全部端点(线上 worker 曾早于 r6,metapass.chuanxilu.net 对
// /api/analyze 全量 404 —— 设备端只见 "unavailable"),以及:
//   - analyze/extracted 的 id 校验与 server.mjs 同为 \d{1,7};
//   - analyze 出口经 store-analyze.js createStoreAnalyzer(同一份纯 ESM 模块,
//     本地 dev 与线上同源);
//   - 未知路径 404、非 GET/HEAD 405。
// 运行:node tests/worker_contract.mjs(仓库根)。
// 注:端到端语义验配(真 fetch 一个 id)属 wrangler pages dev / 部署后冒烟,
// 本文件只做不依赖 workerd 运行时的静态契约检查。
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import assert from "node:assert/strict";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const workerSrc = readFileSync(path.join(ROOT, "install-slot", "_worker.js"), "utf8");

// 1. 端点存在:设备商店通道两条 + 既有浏览器代理三条。
for (const ep of ["/api/analyze", "/api/extracted", "/api/plays", "/api/play", "/api/firmware"]) {
  assert.ok(workerSrc.includes(`"${ep}"`), `worker missing endpoint ${ep}`);
}
console.log("PASS 1: worker exposes analyze/extracted + legacy proxy endpoints");

// 2. 分析器同源:worker 必须经 createStoreAnalyzer(store-analyze.js 纯 ESM),
//    而不是内联第二份解包实现。
assert.ok(workerSrc.includes('from "./store-analyze.js"'), "worker must import ./store-analyze.js");
assert.ok(workerSrc.includes("createStoreAnalyzer"), "worker must use createStoreAnalyzer");
assert.ok(workerSrc.includes("dataProfile") && workerSrc.includes("PLAY28_RECORDINGS_4M_PROFILE"),
  "worker must expose only the explicitly named Play 28 test DATA profile");
console.log("PASS 2: worker analyze path uses the shared store-analyze module");

// 3. id 校验口径与设备/本地 dev 一致(1~7 位数字;8 位拒绝)。
assert.ok(workerSrc.includes("\\d{1,7}"), "analyze/extracted id must be \\d{1,7}");
console.log("PASS 3: play id validation matches \\d{1,7} contract");

// 4. extracted 必须携带设备双重比对要用的响应头。
assert.ok(workerSrc.includes('"x-image-sha256"'), "extracted must send x-image-sha256");
assert.ok(workerSrc.includes('"x-image-len"'), "extracted must send x-image-len");
console.log("PASS 4: extracted response carries x-image-len / x-image-sha256");

// 5. analyze/extracted 的设备-facing 响应不进 CDN 缓存(analyze JSON 与镜像经
//    revisionId 绑定;extracted 的边缘缓存是 worker 内 Cache API,非 CDN 头)。
{
  const analyzeBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/analyze"'), workerSrc.indexOf('path === "/api/extracted"'));
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'), workerSrc.indexOf('path === "/api/r2check"'));
  assert.ok(analyzeBlock.includes("no-store"), "analyze must be no-store");
  assert.ok(extractedBlock.includes("no-store"), "extracted must be no-store");
  console.log("PASS 5: analyze/extracted responses are no-store");
}

// 6. 非 GET/HEAD 405、未知路径落到 ASSETS(404 由 Pages 静态面给)。
assert.ok(workerSrc.includes("405"), "must reject non GET/HEAD with 405");
assert.ok(workerSrc.includes("env.ASSETS.fetch"), "unknown paths must fall through to ASSETS");
console.log("PASS 6: method guard + ASSETS fallthrough present");

// 7. 本地 dev 与线上同契约:server.mjs 的 analyze/extracted id 校验同为 \d{1,7}。
{
  const serverSrc = readFileSync(path.join(ROOT, "tools", "install-slot", "server.mjs"), "utf8");
  assert.ok(serverSrc.includes("\\d{1,7}"), "server.mjs id validation must match");
  assert.ok(serverSrc.includes('["/data-size-profile.js"'), "local dev must serve the shared DATA profile module");
  assert.ok(serverSrc.includes('process.env.ENABLE_TEST_DATA_PROFILES !== "1"'), "local dev must keep test profiles disabled by default");
  console.log("PASS 7: server.mjs (local dev) matches the same id contract");
}

// 8. r10.15b:同 zone 边缘缓存 —— 真机日志(5% 段 28s、~4kB/s 龟速、502 回源失败)
// 定位到 Worker 冷启动每次现场回源 3MB 镜像→校验→解包。上一版缓存回源 URL
// (folotoy.cn,他人 zone)被 Cache API 静默拒绝,生产从未命中。现缓存设备-facing
// 的 extracted 响应(本 zone 必然可 put/match),analyze 成功后 ctx.waitUntil 预热。
{
  assert.ok(workerSrc.includes("caches.default"), "must use the Workers Cache API (caches.default)");
  assert.ok(workerSrc.includes("putExtractedToEdge"), "extracted must populate the edge cache");
  assert.ok(workerSrc.includes("warmEdgeExtracted"), "analyze must warm the edge cache (detail-page prewarm)");
  assert.ok(workerSrc.includes("ctx.waitUntil"), "prewarm must use ctx.waitUntil (Pages Advanced Mode)");
  assert.ok(/EDGE_TTL\s*=\s*3600/.test(workerSrc), "edge TTL must be 3600s");
  assert.ok(!/sha256[^\n]*skip|bypass.*sha256/i.test(workerSrc), "trust chain must remain unconditional");
  console.log("PASS 8: extracted served from same-zone edge cache + analyze prewarm (trust chain intact)");
}

// 9. r10.18:票据 = R2 专属钥匙,不是准入门槛。定稿规则:"没有票据只能走老
//    的链路,不能访问 R2"。钉死:
//    a. 票据校验器是 downloadTicket(ticketed 布尔语义),旧的拒绝式
//       checkDownloadTicket 不再存在;
//    b. 无票/坏票/过期票不得产生任何 403/4xx 拒绝响应(错误字符串清零);
//    c. R2 读和 R2 写都必须在 ticket.ticketed 条件内(未授权流量不碰 R2);
//    d. 限速必须先于票据校验且对两条链路无条件生效。
{
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'), workerSrc.indexOf('path === "/api/r2check"'));
  assert.ok(workerSrc.includes("async function downloadTicket"), "ticket checker must be downloadTicket");
  assert.ok(!workerSrc.includes("checkDownloadTicket"), "reject-style checkDownloadTicket must be gone");
  assert.ok(!/missing ticket|ticket expired|bad ticket|server ticket config/.test(workerSrc),
            "tickets must never cause a service-rejection response");
  assert.ok(extractedBlock.includes("if (ticket.ticketed) {\n        try {\n          const meta"),
            "R2 read path must be gated on ticket.ticketed");
  assert.ok(extractedBlock.includes("ticket.ticketed && env.meta_pass_extracted"),
            "R2 write path must be gated on ticket.ticketed");
  const gate = workerSrc.indexOf("rateLimit(env, req, id)");
  const tick = workerSrc.indexOf("downloadTicket(env, url, id,", extractedBlock); // r10.20: 调用带 maxAge 参数
  assert.ok(gate > 0 && tick > gate, "rate limit must run before ticket check (applies to both paths)");
  console.log("PASS 9: ticket = R2-only key; no-ticket requests fall back to legacy path, never rejected");
}

// 10. r10.18:票据标签必须是 16 hex 字符(64bit)。历史教训连犯两次:
//    hex 化后 .slice(0,16) 切的是"64 个单字符数组"的前 16 个元素(join 仍 32 字符);
//    字节侧截 16 字节也是 32 字符。签发 32 字符 sig 与校验方 {16} 正则永不匹配,
//    生产上所有票据 invalid、R2 永远 skipped(x-r2-write: skipped 实测)。本门直接
//    执行 hmacHex16 源码断言输出长度。
{
  const m = workerSrc.match(/async function hmacHex16[\s\S]*?\n}/);
  assert.ok(m, "hmacHex16 must exist");
  // eslint-disable-next-line no-eval
  const fn = eval("(" + m[0].replace("async function hmacHex16", "async function") + ")");
  const sig = await fn("a".repeat(64), "675:1790701486");
  assert.ok(/^[0-9a-f]{16}$/.test(sig), `ticket sig must be exactly 16 hex chars, got ${JSON.stringify(sig)}`);
  console.log("PASS 10: hmacHex16 issues exactly-16-hex-char ticket labels");
}

// 11. r10.19:断点续传(206)。固件 r10.17 对中断安装发 `Range: bytes=<start>-`,
//     设备端 meta_store_range_parse 只认 "bytes <first>-<last>/<total>"(带空格,
//     span 必须等于该响应 content-length,total 必须等于 analyze image_len)。
//     钉死:
//     a. 只接受 suffix-range 形态 `bytes=<n>-`,其余 Range 形态按 RFC 回 200;
//     b. Range 请求绕过边缘缓存读(缓存键不含 Range,缓存体恒为 200 全量);
//     c. R2 路径用原生区间读 get(key,{range:{offset}}),现算路径切片供给;
//     d. 两条 206 路径的 content-range 模板逐字符匹配固件解析器;
//     e. 206 的 x-image-sha256 仍是全镜像摘要(设备端续传后强校验完整镜像);
//     f. 206 提前返回,绝不写边缘缓存/不触发 R2 物化(污染防护);
//     g. start>=total 回 416(固件作废 OTA 降级整单重试,安全)。
{
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'), workerSrc.indexOf('path === "/api/r2check"'));
  assert.ok(workerSrc.includes("function parseSuffixRange"), "suffix-range parser must exist");
  assert.ok(workerSrc.includes("/^bytes=(\\d+)-$/"), "only `bytes=<n>-` form accepted (firmware emits exactly this)");
  assert.ok(extractedBlock.includes("if (rangeStart === null) {"),
            "range requests must bypass the edge-cache read");
  assert.ok(extractedBlock.includes("get(key, { range: { offset: rangeStart } })"),
            "R2 resume must use native range read");
  assert.ok(extractedBlock.includes("out.stream.slice(rangeStart)"),
            "computed resume must slice the analyzed Uint8Array");
  // content-range 模板:与固件 meta_store_range_parse 的 "bytes <first>-<last>/<total>"
  // 逐字符同源(R2 与现算两条路径共用同一模板字面量)。
  const tmpl = "`bytes ${rangeStart}-${total - 1}/${total}`";
  assert.ok(workerSrc.split(tmpl).length - 1 === 2,
            "both 206 paths must share the exact content-range template");
  assert.ok(extractedBlock.split("status: 206").length - 1 === 2,
            "exactly two 206 supply paths (r2-range + computed-range)");
  // 206 早于缓存/物化写路径返回(顺序断言,防未来重构把写路径挪到前面)。
  const i206 = extractedBlock.indexOf("status: 206");
  const iBg = extractedBlock.indexOf("ctx.waitUntil((async () => {");
  assert.ok(i206 > 0 && i206 < iBg,
            "206 must return before any cache/R2 write (persistence is backgrounded since r10.20-H4)");
  assert.ok((extractedBlock.match(/status: 416/g) || []).length === 2,
            "start>=total must 416 on both supply paths");
  // 动态执行解析器:固件唯一形态接受,其余形态(多字节/后缀/无头)必须 null → 200。
  const pm = workerSrc.match(/function parseSuffixRange[\s\S]*?\n}/);
  assert.ok(pm, "parseSuffixRange must be extractable");
  // eslint-disable-next-line no-eval
  const parseFn = eval("(" + pm[0].replace("function parseSuffixRange", "function") + ")");
  const withRange = (v) => new Request("https://x/api/extracted?id=1",
    v === null ? {} : { headers: { range: v } });
  assert.equal(parseFn(withRange("bytes=123-")), 123, "suffix-range accepted");
  assert.equal(parseFn(withRange("bytes=0-")), 0, "zero offset accepted");
  assert.equal(parseFn(withRange("bytes=0-499")), null, "explicit-end form rejected (200 full)");
  assert.equal(parseFn(withRange("bytes=-500")), null, "suffix-length form rejected");
  assert.equal(parseFn(withRange(null)), null, "no header -> full response");
  console.log("PASS 11: range resume serves 206 with firmware-exact Content-Range; other Range forms stay 200");
}

// 12. r10.20-H2/H6: 边缘缓存键归一化 + 命中新鲜度复核。ts/sig 是每请求凭证,
//     绝不入键(否则 analyze 预热与带票固件各占一条目,预热失效+一次性 2.6MB
//     条目堆积);1h TTL 内市场镜像可能更新,命中必须与当前 analyze 摘要复核,
//     不符即驱逐陈旧条目 —— 绝不拿旧字节冒新声明(固件对摘要头是确定性失败)。
{
  assert.ok(workerSrc.includes("function extractedEdgeKey(url)"), "edge key derives from URL");
  const kf = workerSrc.slice(workerSrc.indexOf("function extractedEdgeKey"),
                             workerSrc.indexOf("function putExtractedToEdge"));
  assert.ok(kf.includes('searchParams.get("id")'), "edge key carries the play id");
  assert.ok(!kf.includes('"ts"') && !kf.includes('"sig"'),
            "ts/sig must never enter the edge cache key");
  const ex = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'),
                             workerSrc.indexOf('path === "/api/r2check"'));
  assert.ok(ex.includes('hit.headers.get("x-image-sha256") === cur.extracted.sha256'),
            "edge hits are re-checked against the current analyze sha before serving");
  assert.ok(ex.includes("caches.default.delete(extractedEdgeKey(url))"),
            "stale edge entries are evicted, never served");
}
console.log("PASS 12: edge key normalized to id; hits re-checked against current analyze sha");

// 13. r10.20-H4: 冷路径持久化后台化。响应体与 R2 写体是两份独立复制
//     (后台闭包独占,不共享可变 buffer);持久化进 ctx.waitUntil,响应先行。
{
  const ex = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'),
                             workerSrc.indexOf('path === "/api/r2check"'));
  assert.ok(workerSrc.includes("const respBody = out.stream.slice();"),
            "response body is an independent copy");
  assert.ok(workerSrc.includes("const edgeBody = out.stream.slice();"),
            "R2 write gets its own buffer copy");
  const iResp = ex.indexOf("return new Response(respBody");
  const iBg = ex.indexOf("ctx.waitUntil((async () => {");
  assert.ok(iResp > 0 && iBg > 0 && iBg < iResp,
            "waitUntil is scheduled before the response returns");
  const iEdgeCall = ex.indexOf("await putExtractedToEdge");
  assert.ok(iEdgeCall < 0 || iEdgeCall > iBg,
            "any inline edge put must live inside the waitUntil closure");
}
console.log("PASS 13: cold-path persistence (edge/R2/meta) runs in ctx.waitUntil, response returns first");

// 14. r10.20-H1/H5: 限速真实接线(KV namespace 绑定进 wrangler.toml,审计发现
//     此前仅声明未绑定 → 生产 429 从未生效);Range 续传用长票据窗口。
{
  const rl = workerSrc.slice(workerSrc.indexOf("async function rateLimit"),
                             workerSrc.indexOf("export default"));
  assert.ok(rl.includes("env.RATE_KV.get") && rl.includes("env.RATE_KV.put"),
            "rateLimit must count via RATE_KV");
  const toml = readFileSync(path.join(ROOT, "wrangler.toml"), "utf8");
  assert.ok(/\[\[kv_namespaces\]\][^\[]*binding = "RATE_KV"/.test(toml),
            "wrangler.toml must bind the RATE_KV namespace");
  assert.ok(/DL_RANGE_TICKET_MAX_AGE_S = 3600/.test(workerSrc),
            "resume ticket window is 3600s");
  assert.ok(workerSrc.includes("rangeStart !== null ? DL_RANGE_TICKET_MAX_AGE_S : undefined"),
            "only Range resume requests use the long ticket window");
  assert.ok(workerSrc.includes('url.searchParams.get("range") === "1" ? DL_RANGE_TICKET_MAX_AGE_S : undefined'),
            "r2check exposes the long-window ticket diagnosis");
}
console.log("PASS 14: rate limit wired via RATE_KV binding; Range resume uses the 3600s ticket window");

// 15. 审计修复契约(docs/assets/mota-implementation-audit.md):
//     B3 /api/plays 必须透传 query;M3 proxy 流式(无 arrayBuffer 全量缓冲);
//     M4 手机模块全部静态 import 与入口同走 no-store;B2 手机 getPlayDetail
//     路径必须命中 Worker 真实路由(而不是只存在于 mock 里的路径)。
{
  assert.ok(workerSrc.includes('proxy("/api/plays" + url.search)'),
            "/api/plays must forward the query string (audit B3)");
  const proxyBlock = workerSrc.slice(workerSrc.indexOf("async function proxy"),
                                     workerSrc.indexOf("// analyze/extracted 的 id 校验"));
  assert.ok(!proxyBlock.includes("arrayBuffer"),
            "proxy must stream upstream.body, never buffer the whole image (audit M3)");
  assert.ok(proxyBlock.includes("new Response(upstream.body"),
            "proxy returns the upstream stream (audit M3)");
  for (const m of ["/phone-install.js", "/extract-app-image.js",
                   "/store-analyze.js", "/name-blob.js", "/dynslot-pool.js"]) {
    assert.ok(workerSrc.includes(`path === "${m}"`),
              `module ${m} must be routed explicitly (audit M4)`);
  }
  const modBlock = workerSrc.slice(workerSrc.indexOf('path === "/phone-install.js"'),
                                   workerSrc.indexOf("// ── 静态文件服务"));
  assert.ok(!modBlock.includes("max-age=14400") && modBlock.includes('headers.set("cache-control", "no-store")'),
            "all phone modules are no-store, matching the entry (audit M4)");
  const serverSrc = readFileSync(path.join(ROOT, "tools", "install-slot", "server.mjs"), "utf8");
  assert.ok(serverSrc.includes('proxyFetch("/api/plays" + urlObj.search'),
            "server.mjs must forward the query string too (audit B3)");
  // B2:手机模块发往 metapass 的每条 API 路径都必须命中一条 Worker 路由。
  const phoneSrc = readFileSync(path.join(ROOT, "install-slot", "phone-install.js"), "utf8");
  const phonePaths = [...phoneSrc.matchAll(/mpJson\(`([^`]+)`\)/g)].map((m) => m[1]);
  assert.ok(phonePaths.length >= 3, "phone module API paths must be extractable");
  for (const p of phonePaths) {
    const route = p.split("?")[0];
    assert.ok(workerSrc.includes(`path === "${route}"`),
              `phone path ${route} has no Worker route (audit B2 class)`);
  }
  assert.ok(phoneSrc.includes('mpJson(`/api/play?id='),
            "getPlayDetail must use the Worker-routed /api/play?id= (audit B2)");
}
console.log("PASS 15: /api/plays query forwarded, proxy streams, phone modules no-store, phone paths routed");

// 16. 采集数据上行(方案 A):ingest 端点在 GET/HEAD 守卫之前分发(POST/OPTIONS
// 不被 405),RCB1 magic + 批次一致性校验,设备身份从头部 device_hash@28 解析
// (客户端不能冒充),复用 RATE_KV 限速,CORS 预检,wrangler 绑定 capture 桶;
// capture-upload.js(独立模式设备模块)以 no-store 提供。
{
  const ingestAt = workerSrc.indexOf('"/api/capture/ingest"');
  assert.ok(ingestAt > 0, "worker missing capture ingest endpoint");
  const guardAt = workerSrc.indexOf('req.method !== "GET"');
  assert.ok(ingestAt < guardAt, "ingest dispatch must precede the GET/HEAD 405 guard");
  assert.ok(workerSrc.includes("0x52") && workerSrc.includes("0x43") &&
            workerSrc.includes("0x42") && workerSrc.includes("0x31"),
            "ingest must validate the RCB1 magic");
  assert.ok(workerSrc.includes("batch mismatch"), "ingest must cross-check batch id");
  assert.ok(workerSrc.includes("i < 44"), "ingest must derive device from header device_hash");
  assert.ok(!workerSrc.includes('searchParams.get("cardid")'),
            "ingest must never accept a client-supplied cardid");
  assert.ok(workerSrc.includes("rateLimit(env, req, `cap:"), "ingest must reuse the RATE_KV limiter");
  assert.ok(workerSrc.includes("CAPTURE_MAX_PER_DAY") && workerSrc.includes("capd:"),
            "ingest must enforce a per-device daily quota via RATE_KV");
  assert.ok(workerSrc.includes("bad device identity"),
            "ingest must reject degenerate all-0/all-F device hashes");
  assert.ok(workerSrc.includes("getUint16(4, true)"),
            "ingest must parse the RCB1 header version field");
  assert.ok(workerSrc.includes('"mac-sha256-trunc16"'),
            "meta.json device_kind must be mac-sha256-trunc16 (cardid retired)");
  assert.ok(workerSrc.includes("verified"), "meta.json must carry the verified flag");
  const enrollAt = workerSrc.indexOf('"/api/capture/enroll"');
  assert.ok(enrollAt > 0, "worker missing capture enroll endpoint");
  assert.ok(enrollAt < guardAt, "enroll dispatch must precede the GET/HEAD 405 guard");
  assert.ok(workerSrc.includes("access-control-allow-methods"), "ingest must answer CORS preflight");
  assert.ok(workerSrc.includes("meta_pass_capture"), "ingest must write the capture R2 bucket");
  assert.ok(workerSrc.includes('path === "/capture-upload.js"'),
            "capture-upload.js must be served no-store for the standalone device page");
  const wrangler = readFileSync(path.join(ROOT, "wrangler.toml"), "utf8");
  assert.ok(wrangler.includes('binding = "meta_pass_capture"') &&
            wrangler.includes('bucket_name = "meta-pass-capture"'),
            "wrangler.toml must bind the meta-pass-capture bucket");
}
console.log("PASS 16: capture ingest endpoint contract (dispatch/magic/version/device-from-header/device-kind/rate-limit/daily-quota/identity/CORS/R2/enroll)");

// 17. capture 身份签名(契约 v1)运行时断言:真驱动 _worker.js 的 enroll/ingest
//    handler(mock env),Ed25519 密钥对与 device_hash 全部运行时生成(仓库
//    密钥扫描禁 32-hex 字面量)。覆盖:enroll 成功/重复 409/退化 400/限频 429;
//    已注册+合法 trailer → 200 verified;已注册+坏 sig → 403;已注册+payload
//    sha 不符 → 403;未注册 v7 → 200 verified:false;v6 无 trailer → 现状路径。
{
  const worker = await import(path.join(ROOT, "install-slot", "_worker.js"));
  const handle = worker.default.fetch;
  const hex = (u8) => [...u8].map((b) => b.toString(16).padStart(2, "0")).join("");
  // 身份派生同固件契约:device_hash = SHA-256("mpr-id-v1"||wifi_mac||ble_mac)[:16]。
  const macBytes = (n) => Uint8Array.from({ length: 6 }, (_, i) => (n * 37 + i * 11 + 2) & 0xff);
  async function makeIdentity(n) {
    const wifi = macBytes(n), ble = macBytes(n + 100);
    const enc = new TextEncoder();
    const idFull = new Uint8Array(await crypto.subtle.digest("SHA-256",
      new Uint8Array([...enc.encode("mpr-id-v1"), ...wifi, ...ble])));
    const seed = new Uint8Array(await crypto.subtle.digest("SHA-256",
      new Uint8Array([...enc.encode("mpr-ed25519-seed-v1"), ...wifi, ...ble])));
    const kp = await crypto.subtle.generateKey("Ed25519", true, ["sign", "verify"]);
    void seed; // 固件侧用 seed 派生密钥;worker 侧只验签,测试直接用生成的密钥对
    return { deviceBytes: idFull.slice(0, 16), device: hex(idFull.slice(0, 16)),
      pubkey: hex(new Uint8Array(await crypto.subtle.exportKey("raw", kp.publicKey))),
      sign: (m) => crypto.subtle.sign("Ed25519", kp.privateKey, m) };
  }
  // 组一帧 v7 批次区:128B 头 + payload + 4096B trailer 扇区(128B SGN1 @扇区头)。
  async function makeBatch(id, { version = 7, batch = 1, payloadLen = 256,
      corruptSig = false, corruptPayload = false } = {}) {
    const body = new Uint8Array(128 + payloadLen + 4096);
    body.set([0x52, 0x43, 0x42, 0x31], 0);            // "RCB1"
    const v = new DataView(body.buffer);
    v.setUint16(4, version, true);
    v.setUint32(8, batch, true);
    v.setUint32(12, 4, true);                          // expected
    v.setUint32(16, 2, true);                          // committed
    body.set(id.deviceBytes, 28);
    for (let i = 0; i < payloadLen; i++) body[128 + i] = (i * 7 + 3) & 0xff;
    const payloadSha = new Uint8Array(await crypto.subtle.digest("SHA-256",
      body.subarray(128, 128 + payloadLen)));
    // corrupt 必须在签名之后:trailer 记的是原始 payload 的 sha,体被篡改即不符。
    if (corruptPayload) body[128] ^= 0xff;
    const tOff = body.byteLength - 4096;
    v.setUint32(tOff, 0x314E4753, true);               // "SGN1"
    v.setUint16(tOff + 4, 1, true);                    // trailer.version
    v.setUint16(tOff + 6, 128, true);                  // trailer.size
    v.setUint32(tOff + 8, batch, true);
    body.set(id.deviceBytes, tOff + 12);
    body.set(payloadSha, tOff + 28);
    const pre = new Uint8Array(67);
    new TextEncoder().encodeInto("MPRCAP-SIG-v1", pre);
    const pv = new DataView(pre.buffer);
    pv.setUint16(13, 1, true);
    pv.setUint32(15, batch, true);
    pre.set(id.deviceBytes, 19);
    pre.set(payloadSha, 35);
    let sig = new Uint8Array(await id.sign(pre));
    if (corruptSig) sig = sig.map((b, i) => (i === 0 ? b ^ 0xff : b));
    body.set(sig, tOff + 60);
    return body;
  }
  function mockEnv() {
    const kv = new Map();
    const r2 = new Map();
    return {
      RATE_KV: { get: async (k) => (kv.has(k) ? kv.get(k) : null),
                 put: async (k, val) => { kv.set(k, String(val)); } },
      CAPKEY_KV: { get: async (k) => (kv.has(k) ? kv.get(k) : null),
                   put: async (k, val) => { kv.set(k, String(val)); } },
      meta_pass_capture: { put: async (k, val) => { r2.set(k, val); } },
      _kv: kv, _r2: r2,
    };
  }
  const post = (path, bodyObj, ip, rawBody) => new Request("https://metapass.chuanxilu.net" + path, {
    method: "POST",
    headers: { "content-type": bodyObj ? "application/json" : "application/octet-stream",
               "cf-connecting-ip": ip },
    body: bodyObj ? JSON.stringify(bodyObj) : rawBody,
  });
  const base = "/api/capture/enroll";

  // enroll 成功(200 + KV 落键)
  {
    const env = mockEnv();
    const id = await makeIdentity(1);
    const r = await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    assert.equal(r.status, 200);
    assert.equal(env._kv.get(`capkey:${id.device}`), id.pubkey);
  }
  // enroll 重复 → 409 first-wins
  {
    const env = mockEnv();
    const id = await makeIdentity(2);
    await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    const r = await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    assert.equal(r.status, 409, "duplicate enroll must be 409");
  }
  // enroll 退化 device(全0 / 全F)→ 400
  for (const dev of ["0".repeat(32), "f".repeat(32), "0".repeat(64)]) {
    const env = mockEnv();
    const id = await makeIdentity(3);
    const r = await handle(post(base, { device: dev, pubkey: id.pubkey }), env, {});
    assert.equal(r.status, 400, `degenerate device ${dev.slice(0, 4)}… must be 400`);
  }
  // enroll 每 IP 10/min:第 11 个不同 device → 429
  {
    const env = mockEnv();
    let lastStatus = 0;
    for (let i = 0; i < 11; i++) {
      const id = await makeIdentity(10 + i);
      const r = await handle(post(base, { device: id.device, pubkey: id.pubkey },
        "203.0.113.99"), env, {});
      lastStatus = r.status;
      if (i < 10) assert.equal(r.status, 200, `enroll #${i + 1} must succeed`);
    }
    assert.equal(lastStatus, 429, "11th enroll from same IP must be rate limited");
  }
  // ingest:已注册 + 合法 trailer → 200 verified:true + meta.pubkey
  {
    const env = mockEnv();
    const id = await makeIdentity(30);
    await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    const r = await handle(post("/api/capture/ingest?batch=1", null, "203.0.113.1",
      await makeBatch(id)), env, {});
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.verified, true, "registered valid trailer must verify");
    const meta = JSON.parse(env._r2.get(`capture/${id.device}/1.meta.json`));
    assert.equal(meta.verified, true);
    assert.equal(meta.pubkey, id.pubkey);
    assert.equal(meta.device_kind, "mac-sha256-trunc16");
  }
  // ingest:已注册 + 坏 sig → 403 且不落库
  {
    const env = mockEnv();
    const id = await makeIdentity(31);
    await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    const r = await handle(post("/api/capture/ingest?batch=1", null, "203.0.113.2",
      await makeBatch(id, { corruptSig: true })), env, {});
    assert.equal(r.status, 403, "registered bad sig must be 403");
    assert.ok(![...env._r2.keys()].some((k) => k.startsWith(`capture/${id.device}/`)),
              "403 ingest must not write R2");
  }
  // ingest:已注册 + payload sha 不符 → 403 且不落库
  {
    const env = mockEnv();
    const id = await makeIdentity(32);
    await handle(post(base, { device: id.device, pubkey: id.pubkey }), env, {});
    const r = await handle(post("/api/capture/ingest?batch=1", null, "203.0.113.3",
      await makeBatch(id, { corruptPayload: true })), env, {});
    assert.equal(r.status, 403, "payload sha mismatch must be 403");
    assert.equal(env._r2.size, 0, "sha-mismatch ingest must not write R2");
  }
  // ingest:未注册 v7(合法 trailer)→ 200 verified:false
  {
    const env = mockEnv();
    const id = await makeIdentity(33);
    const r = await handle(post("/api/capture/ingest?batch=1", null, "203.0.113.4",
      await makeBatch(id)), env, {});
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.verified, false, "unregistered v7 must be accepted unverified");
    const meta = JSON.parse(env._r2.get(`capture/${id.device}/1.meta.json`));
    assert.equal(meta.verified, false);
    assert.ok(!("pubkey" in meta), "unverified meta must not carry pubkey");
  }
  // ingest:v6 无 trailer → 200 走现状(unidentified,verified:false)
  {
    const env = mockEnv();
    const id = await makeIdentity(34);
    const r = await handle(post("/api/capture/ingest?batch=7", null, "203.0.113.5",
      await makeBatch(id, { version: 6, batch: 7 }).then((b) => b.slice(0, 128 + 256))), env, {});
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.verified, false, "v6 no-trailer must stay on the legacy path");
  }
  // enroll CORS 预检(OPTIONS → 204)
  {
    const env = mockEnv();
    const r = await handle(new Request("https://metapass.chuanxilu.net" + base,
      { method: "OPTIONS" }), env, {});
    assert.equal(r.status, 204);
    assert.equal(r.headers.get("access-control-allow-origin"), "*");
  }
}
console.log("PASS 17: capture identity runtime (enroll 200/409/400/429 + verify chain 200-true/403-sig/403-sha/unregistered-false/v6-legacy/CORS)");

console.log("ALL WORKER CONTRACT TESTS PASSED");
