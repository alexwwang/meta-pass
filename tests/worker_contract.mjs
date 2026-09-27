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

// 5. analyze/extracted 不进 CDN 缓存(analyze JSON 与镜像经 revisionId 绑定,
//    缓存会击穿 TOCTOU 复核;镜像流也按请求即时生成)。
{
  const analyzeBlock = workerSrc.slice(workerSrc.indexOf('"/api/analyze"'), workerSrc.indexOf('"/api/extracted"'));
  const extractedBlock = workerSrc.slice(workerSrc.indexOf('"/api/extracted"'), workerSrc.indexOf('"/"'));
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

console.log("ALL WORKER CONTRACT TESTS PASSED");
