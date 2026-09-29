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
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'), workerSrc.indexOf('path === "/"'));
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
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('path === "/api/extracted"'), workerSrc.indexOf('path === "/"'));
  assert.ok(workerSrc.includes("async function downloadTicket"), "ticket checker must be downloadTicket");
  assert.ok(!workerSrc.includes("checkDownloadTicket"), "reject-style checkDownloadTicket must be gone");
  assert.ok(!/missing ticket|ticket expired|bad ticket|server ticket config/.test(workerSrc),
            "tickets must never cause a service-rejection response");
  assert.ok(extractedBlock.includes("if (ticket.ticketed) {\n        try {\n          const meta"),
            "R2 read path must be gated on ticket.ticketed");
  assert.ok(extractedBlock.includes("ticket.ticketed && env.meta_pass_extracted"),
            "R2 write path must be gated on ticket.ticketed");
  const gate = workerSrc.indexOf("rateLimit(env, req, id)");
  const tick = workerSrc.indexOf("downloadTicket(env, url, id)", extractedBlock);
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
  const iEdge = extractedBlock.indexOf("putExtractedToEdge(req, out)");
  const iR2w = extractedBlock.indexOf('r2Write = `ok:');
  assert.ok(i206 > 0 && i206 < iEdge && i206 < iR2w,
            "206 must return before any cache/R2 write");
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

console.log("ALL WORKER CONTRACT TESTS PASSED");
