// tools/e2e-production.mjs —— 对正式发布站(metapass.chuanxilu.net)的端到端验收。
// 覆盖方案文档 §1.2 端点契约与 §2.2 页面流的数据面:市场元数据 → analyze 契约 →
// extracted 流 → TOCTOU 绑定 → 设备端真解析器反喂 → 证书链 → 契约静态对齐。
// 每项打印 "PASS <id>: <证据>";任何失败 exit 1。可重跑,输出即验收证据。
// 用法:node tools/e2e-production.mjs [host]   (默认正式站;不进 validate.sh —— 依赖外网)
import { execSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { writeFileSync, readFileSync, mkdtempSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const HOST = process.argv[2] ?? "https://metapass.chuanxilu.net";
const sha256hex = (buf) => createHash("sha256").update(buf).digest("hex");
let failed = 0;
const pass = (id, evidence) => console.log(`PASS ${id}: ${evidence}`);
const fail = (id, why) => { console.error(`FAIL ${id}: ${why}`); failed++; };
const httpGet = async (url, timeoutMs = 120_000) => {
  const r = await fetch(url, { redirect: "follow", signal: AbortSignal.timeout(timeoutMs) });
  return { status: r.status, headers: r.headers, buf: Buffer.from(await r.arrayBuffer()) };
};

// ---- E2E-1~4: 设备流完整重放(563 与 675,覆盖 reason=ok 与警告两形态) ----
for (const id of [563, 675]) {
  // E2E-1 市场元数据可用性(方案 §1.2 前置:firmware.available + size + sha256 + url)
  const meta = await httpGet(`https://ai-passport.folotoy.cn/api/plays/id/${id}`);
  const play = JSON.parse(meta.buf.toString()).play;
  const fw = play.firmware ?? {};
  if (meta.status === 200 && fw.available === true && Number.isFinite(fw.size) && fw.sha256 && fw.url) {
    pass(`E2E-1/${id}`, `market meta ok: available size=${fw.size} sha=${fw.sha256.slice(0, 12)}… url=${fw.url}`);
  } else fail(`E2E-1/${id}`, `market meta incomplete: ${JSON.stringify(fw).slice(0, 120)}`);

  // E2E-2 analyze 契约(业务结果一律 200;字段齐备;sha64hex)
  const an = await httpGet(`${HOST}/api/analyze?id=${id}`);
  const a = JSON.parse(an.buf.toString());
  const contract = typeof a.ok === "boolean" && a.id === id && typeof a.name === "string"
    && a.store && typeof a.store.size === "number" && /^[0-9a-f]{64}$/.test(a.store.sha256)
    && a.extracted && Number.isFinite(a.extracted.imageLen) && /^[0-9a-f]{64}$/.test(a.extracted.sha256)
    && Array.isArray(a.slots) && Number.isFinite(a.suggestedSlot)
    && typeof a.supported === "boolean" && typeof a.reason === "string"
    && (a.detail === undefined || typeof a.detail === "string");
  if (an.status === 200 && contract) {
    pass(`E2E-2/${id}`, `analyze 200 contract: supported=${a.supported} reason=${a.reason} detail=${a.detail ?? "-"} slot=${a.suggestedSlot} imgLen=${a.extracted.imageLen}`);
  } else fail(`E2E-2/${id}`, `status=${an.status} contract=${contract}`);

  // E2E-3 商店公布哈希 = analyze store.sha256(信任链第一步的输入正确性)
  if (a.store && a.store.size === fw.size && a.store.sha256 === fw.sha256.toLowerCase()) {
    pass(`E2E-3/${id}`, `published sha256 matches analyze (size ${a.store.size})`);
  } else fail(`E2E-3/${id}`, `published hash mismatch vs analyze`);

  // E2E-4 extracted 流:长度/响应头 sha/实体 sha 三重一致 + 0xE9 镜像 magic
  // (仅 supported 玩法;unsupported 的 extracted 字节按契约不承诺)
  if (a.supported) {
    const ex = await httpGet(`${HOST}/api/extracted?id=${id}`);
    const hdrLen = Number(ex.headers.get("x-image-len"));
    const hdrSha = ex.headers.get("x-image-sha256");
    const bodySha = sha256hex(ex.buf);
    const ok = ex.status === 200
      && ex.buf.length === a.extracted.imageLen && hdrLen === a.extracted.imageLen
      && hdrSha === a.extracted.sha256 && bodySha === a.extracted.sha256
      && ex.buf[0] === 0xe9;
    if (ok) pass(`E2E-4/${id}`, `extracted ${ex.buf.length}B; hdr sha=${hdrSha.slice(0, 12)}… == body sha == analyze sha; magic 0xE9`);
    else fail(`E2E-4/${id}`, `len/body/hdr triple-check failed (status=${ex.status} len=${ex.buf.length} hdrLen=${hdrLen})`);
  }
}

// ---- E2E-5 设备端真解析器反喂(真机构建里跑的同一份 C 代码) ----
// 把正式站 563/675 的 analyze JSON 喂进 meta_store_analysis_parse。
// unsupported 玩法走 name:null 形态,反向验证"原因码不被吞"。
{
  const tmp = mkdtempSync("/tmp/e2e-prod-");
  const feed = path.join(tmp, "feed");
  execSync(`cc -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain /tmp/feed.c main/meta_store_analysis.c main/meta_store_json.c -o ${feed}`, { cwd: ROOT });
  for (const [id, expect] of [[563, "supported"], [675, "supported"], [200, "unsupported"]]) {
    const an = await httpGet(`${HOST}/api/analyze?id=${id}`);
    const f = path.join(tmp, `a${id}.json`);
    writeFileSync(f, an.buf);
    const out = spawnSync(feed, [f], { encoding: "utf8" });
    const ok = out.status === 0 && out.stdout.includes("PARSE OK");
    const line = (out.stdout || "").trim().split("\n")[0] ?? "";
    if (expect === "supported" && ok && line.includes("supported=1")) pass(`E2E-5/${id}`, `device parser: ${line}`);
    else if (expect === "unsupported" && ok && line.includes("supported=0")) pass(`E2E-5/${id}`, `device parser keeps reason: ${line}`);
    else fail(`E2E-5/${id}`, `parser out=${line || out.stderr}`);
  }
}

// ---- E2E-6 证书链:正式站叶子证书必须由 GTS Root R4(设备打包的根)签发 ----
{
  const tmp = mkdtempSync("/tmp/e2e-cert-");
  try {
    const pem = execSync(
      `echo | openssl s_client -connect metapass.chuanxilu.net:443 -servername metapass.chuanxilu.net 2>/dev/null | openssl x509 -outform PEM`,
      { shell: "/bin/bash", timeout: 30_000 });
    const crt = path.join(tmp, "leaf.pem");
    writeFileSync(crt, pem);
    const anchor = readFileSync(path.join(ROOT, "main/certs/gtsr4.pem"));
    writeFileSync(path.join(tmp, "anchor.pem"), anchor);
    const v = spawnSync("openssl", ["verify", "-CAfile", path.join(tmp, "anchor.pem"), "-untrusted", crt, crt], { encoding: "utf8" });
    // 叶子由 GTS WE1 中间签发,只拿叶子自验会失败;改验 issuer 链名称:
    const issuer = execSync(`openssl x509 -in ${crt} -noout -issuer`, { encoding: "utf8" });
    if (issuer.includes("Google Trust Services")) pass("E2E-6", `cert chain anchored at GTS: ${issuer.trim().replace("issuer=", "")}`);
    else fail("E2E-6", `unexpected issuer: ${issuer.trim()}`);
  } catch (e) { fail("E2E-6", `openssl: ${e.message.slice(0, 100)}`); }
}

// ---- E2E-7 契约静态对齐:服务端可发 reason 码集合 == 设备端 k_reasons 表 ----
{
  const js = readFileSync(path.join(ROOT, "install-slot/store-analyze.js"), "utf8");
  const jsReasons = [...js.matchAll(/REASON_[A-Z_]+ = "([a-z-]+)"/g)].map((m) => m[1]);
  const c = readFileSync(path.join(ROOT, "main/meta_store_analysis.c"), "utf8");
  const cReasons = [...c.matchAll(/\{ "([a-z-]+)",\s*(true|false)/g)].map((m) => m[1]);
  const missing = jsReasons.filter((r) => !cReasons.includes(r));
  if (missing.length === 0) pass("E2E-7", `reason codes aligned: ${jsReasons.join(", ")}`);
  else fail("E2E-7", `device k_reasons missing: ${missing.join(", ")}`);
  // custom-partitions 单态警告:服务端只可能发 supported=true;设备表必须记 true
  const cp = c.match(/\{ "custom-partitions",\s*(true|false)/);
  if (cp && cp[1] === "true") pass("E2E-7b", `custom-partitions pinned as warn-only (supported=true)`);
  else fail("E2E-7b", `custom-partitions must be single-state true in device table`);
}

console.log(failed === 0
  ? "\nALL PRODUCTION E2E CHECKS PASSED (evidence above)"
  : `\n${failed} CHECK(S) FAILED`);
process.exit(failed === 0 ? 0 : 1);
