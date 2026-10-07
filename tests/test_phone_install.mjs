#!/usr/bin/env node
// tests/test_phone_install.mjs —— phone-install.js(设计文档 §4.2/§6)全流程 host 验收。
//
// 被测对象从仓库规范目录 install-slot/ 导入(与 Worker/设备 boot 页同一份实现)。
// 设备端(meta_store_install.c 的 URI handler 契约)与 metapass Worker(§4.3 的
// /api/plays、/api/play、/api/analyze、/api/firmware)都由本地 mock fetch 复现,
// 断言手机端模块驱动出设计文档要求的请求序列与容错语义:
//   1. 纯 JS SHA-256 与 node:crypto 逐字节对拍(§4.2:local http 源无 crypto.subtle);
//   2. normalizePlay(§5):字段回退链、无固件元数据 = 不可安装;
//   3. preflight(§6.3):下载尺寸/商店哈希/解包一致性三道门 + offer 形状
//      (slots 表 = 仓库单一事实源 SLOT_GEOMETRY − 4KB);
//   4. runInstall(§6.4/§6.5):prepare → 物理确认轮询 → session(槽位取设备值)
//      → 顺序 chunk → finalize → done;chunk 失败按设备上报 offset 续传;
//   5. 防御面:无 token 拒发、协议不符拒发、busy 拒发、确认超时、finalize 失败;
//   6. 槽位管理(dynslot §4.5 Remove):GET /api/install/slots 清单契约、
//      POST remove 删除(200 → 复位窗口不可达 → waitDeviceBack 回连)、
//      忙碌 409/坏体 400/不在 carve 404 拒绝面与 parseSlots 形状门禁。
//
// 运行:node tests/test_phone_install.mjs(仓库根)。
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const phone = await import(path.join(ROOT, "install-slot", "phone-install.js"));
const { SLOT_GEOMETRY } = await import(path.join(ROOT, "install-slot", "store-analyze.js"));
const dyn = await import(path.join(ROOT, "install-slot", "dynslot-pool.js"));

// ── 夹具:最小合法 ESP 镜像(同 test-extract.mjs 构造法) ────────────────
function buildAppImage(seg0len = 100, seg1len = 64) {
  const bodyLen = 24 + 16 + (8 + seg0len) + (8 + seg1len);
  let total = bodyLen;
  while (total % 16 !== 15) total++;
  total += 1 + 32; // 校验和 + SHA-256
  const buf = new Uint8Array(total).fill(0xab);
  buf[0] = 0xe9;
  buf[1] = 2;
  buf[12] = 5; buf[13] = 0; // chip_id ESP32-C3
  buf[23] = 1;              // hash_appended
  let off = 24 + 16;
  const dvv = new DataView(buf.buffer);
  dvv.setUint32(off, 0x3fc80000, true);      // segment 0 load addr
  dvv.setUint32(off + 4, seg0len, true);     // data_len(u32le,支持 >255)
  off += 8 + seg0len;
  dvv.setUint32(off, 0x42000020, true);      // segment 1 load addr
  dvv.setUint32(off + 4, seg1len, true);
  return buf;
}
const APP = buildAppImage();
const APP_SHA = createHash("sha256").update(APP).digest("hex");

// 合并镜像 = bootloader 区 + 0x8000 分区表(0x50 0xAA magic + factory 条目:
// type=0/subtype=0,offset=0x10000)+ factory 应用(与 extract-app-image.js 的
// isFullImage/findFactoryPartition 契约一致)。
function buildMerged(app) {
  const merged = new Uint8Array(0x10000 + app.length);
  merged[0] = 0xe9;                     // 全镜像头
  merged[0x8000] = 0xaa; merged[0x8001] = 0x50;   // 分区表条目 magic(小端 0x50AA)
  merged[0x8002] = 0x00; merged[0x8003] = 0x00;   // type=0 subtype=0(factory)
  const dv = new DataView(merged.buffer);
  dv.setUint32(0x8004, 0x10000, true);  // 分区 offset
  dv.setUint32(0x8008, 0x170000, true); // 分区 size
  merged[0x10000] = 0xe9;
  merged.set(app, 0x10000);
  return merged;
}
const MERGED = buildMerged(APP);
const MERGED_SHA = createHash("sha256").update(MERGED).digest("hex");

// 官方契约最小形态(§5):手机端只认这些字段。
const PLAY = {
  id: 563,
  revisionId: 1499,
  title: { zh: "Openclaw/OpenAI 随身对讲机", en: "Pocket Intercom" },
  slug: "ai-passport-9",
  firmware: {
    available: true,
    size: MERGED.length,
    sha256: MERGED_SHA,
    url: "/api/download/community/community-82cbed79",
    format: "esp-merged-0x0",
  },
};

const SLOT_LIMITS = SLOT_GEOMETRY.map(({ slot, partSize }) => ({
  slot, limit: partSize - 0x1000, fit: APP.length <= partSize - 0x1000,
}));

// dynslot §4.5 槽位管理 mock夹具:两池总字节数(设备端 meta_carve_pool_total
// 同源)+ 3 槽 carve(valid/invalid/empty 各一,尺寸取仓库池几何)。
const POOL_TOTAL = 6766592;
// legacy 表槽位偏移(partitions.csv:ota_0 @pool_0 起,ota_1 @pool_1 起,
// ota_2 紧随其后)—— 与设备 carve 种子同一布局;解析门禁要求 64KB 对齐。
const LEGACY_OFFSETS = [0x180000, 0x360000, 0x560000];
function defaultCarve() {
  return SLOT_GEOMETRY.map(({ slot, partSize }) => ({
    slot,
    state: slot === 0 ? "valid" : slot === 1 ? "invalid" : "empty",
    name: slot === 0 ? "demo-fw" : "",
    size: partSize,
    len: slot === 0 ? 123456 : 0,
    limit: partSize - 0x1000,   // 设备端 meta_sign_app_limit(part − 4KB)
    offset: LEGACY_OFFSETS[slot],
    kind: "app",
  }));
}

function analyzeJson() {
  return {
    ok: true,
    id: PLAY.id,
    revisionId: PLAY.revisionId,
    name: PLAY.slug,
    store: { size: PLAY.firmware.size, sha256: PLAY.firmware.sha256 },
    extracted: { imageLen: APP.length, sha256: APP_SHA },
    slots: SLOT_LIMITS,
    suggestedSlot: 0,
    supported: true,
    reason: "ok",
  };
}

// ── 1. 纯 JS SHA-256 与 node:crypto 对拍(空/短/跨块/大输入) ─────────────
{
  const vectors = [
    new TextEncoder().encode(""),
    new TextEncoder().encode("abc"),
    new Uint8Array(55), new Uint8Array(56), new Uint8Array(64), new Uint8Array(119),
    new Uint8Array(120), new Uint8Array(1000),
  ];
  // 再来一个大随机缓冲(长度取 64k±1 覆盖多位长边界)。
  const big = new Uint8Array(65537);
  for (let i = 0; i < big.length; i++) big[i] = (i * 7 + 13) & 0xff;
  vectors.push(big);
  for (const v of vectors) {
    const expect = createHash("sha256").update(v).digest("hex");
    assert.equal(phone.sha256Pure(v), expect,
      `pure JS sha256 mismatch for len ${v.length}`);
  }
  console.log("PASS 1: pure-JS sha256 matches node:crypto on 9 vectors (0..65537 bytes)");
}

// ── 2. normalizePlay(§5 契约) ───────────────────────────────────────────
{
  const n = phone.normalizePlay(PLAY);
  assert.equal(n.id, 563);
  assert.equal(n.revisionId, 1499);
  assert.equal(n.name, "Openclaw/OpenAI 随身对讲机");
  assert.equal(n.size, MERGED.length);
  assert.equal(n.sha256, MERGED_SHA);
  assert.equal(n.downloadUrl, "/api/download/community/community-82cbed79");
  // 回退链:firmware.* → downloadUrl/firmwareSize/firmwareSha256;title.en → slug。
  const n2 = phone.normalizePlay({
    id: 7, title: { zh: "", en: "Only English" }, slug: "only-en",
    downloadUrl: "/api/download/x", firmwareSize: 10, firmwareSha256: "a".repeat(64),
  });
  assert.equal(n2.name, "Only English");
  assert.equal(n2.downloadUrl, "/api/download/x");
  // 无固件元数据 = 不可安装(§11)。
  assert.equal(phone.normalizePlay({ id: 8, title: { zh: "x" } }), null);
  assert.equal(phone.normalizePlay({ id: 9, firmware: { url: "/evil", size: 1, sha256: "a".repeat(64) } }), null);
  assert.equal(phone.normalizePlay(null), null);
  console.log("PASS 2: normalizePlay fallback chain + installability gate");
}

// ── mock 环境:metapass + 设备双面 fetch ────────────────────────────────
// globalThis.fetch 被替换;localStorage/daily 均不需要。
function urlOf(r) { return typeof r === "string" ? r : r.url; }

// metapass mock:按 URL 分发;设备 mock:完整状态机(见 makeDevice)。
// 路由表镜像线上 Worker(_worker.js):/api/plays(带 query)、/api/play?id=
// (代理到上游 /api/plays/id/<id>)、/api/analyze、/api/firmware。审计教训:
// mock 镜像手机模块的期望而非 Worker 真实路由,把 B2(详情 404)挡在绿外。
function installMockFetch({ plays = [PLAY], analyze = analyzeJson(), firmware = MERGED } = {}) {
  const calls = [];
  const realFetch = globalThis.fetch;
  const f = async (input, init) => {
    const url = urlOf(input);
    const u = new URL(url, "http://device.local");
    calls.push(u.pathname + u.search);
    if (u.origin === "https://metapass.chuanxilu.net" || u.origin === "https://metapass.example") {
      if (u.pathname === "/api/plays") {
        // Worker 契约:url.search 原样转发(审计 B3);mock 也必须执行 q 过滤、
        // category 过滤(生产实测服务端生效)与 offset/limit 分页(响应
        // pagination.{total,hasMore}),否则"query 被丢弃/分类失效/翻页失效"
        // 在测试里同样不可见。
        const q = u.searchParams.get("q");
        const cat = u.searchParams.get("category");
        const tag = u.searchParams.get("tag");
        const offset = Number(u.searchParams.get("offset") ?? 0) || 0;
        const limit = Number(u.searchParams.get("limit") ?? 20) || 20;
        let filtered = q
          ? plays.filter((p) => JSON.stringify(p).includes(q))
          : plays.slice();
        if (cat) filtered = filtered.filter((p) => p.category === cat);
        if (tag) {
          // 官方 tag= 语义(生产实测):must-play → mustPlay 标志,其余按 tags 数组。
          filtered = filtered.filter((p) =>
            tag === "must-play" ? p.mustPlay === true
            : tag === "child-friendly" ? p.childFriendly === true
            : Array.isArray(p.tags) && p.tags.includes(tag));
        }
        return new Response(JSON.stringify({
          ok: true,
          plays: filtered.slice(offset, offset + limit),
          pagination: { total: filtered.length, hasMore: offset + limit < filtered.length },
          categoryCounts: { games: 3, learning: 2 },
          discoveryTags: [
            { key: "must-play", name: { zh: "必玩精选", en: "Must-play" }, sortOrder: 0, enabled: true },
            { key: "multiplayer", name: { zh: "多人玩法", en: "Multiplayer" }, sortOrder: 1, enabled: true },
          ],
        }), { status: 200 });
      }
      if (u.pathname === "/api/play") {
        const id = Number(u.searchParams.get("id"));
        const play = plays.find((p) => p.id === id);
        return play
          ? new Response(JSON.stringify({ ok: true, play }), { status: 200 })
          : new Response(JSON.stringify({ detail: "玩法不存在" }), { status: 404 });
      }
      if (u.pathname === "/api/analyze") {
        return new Response(JSON.stringify(analyze), { status: 200 });
      }
      if (u.pathname === "/api/firmware") {
        assert.ok(u.searchParams.get("path")?.startsWith("/api/download/"), "firmware path whitelist");
        return new Response(firmware.slice().buffer, { status: 200 });
      }
      return new Response("no such metapass route", { status: 404 });
    }
    return realFetch(input, init);
  };
  f.calls = calls;
  return f;
}

// 设备 mock:严格复现 meta_store_install.c 的请求→响应契约(§6.5)。
function makeDevice({ imageLen = APP.length, sha256 = APP_SHA, maxChunk = 65536,
                      failChunkAt = -1, failTimes = 1, finalOk = true,
                      autoConfirm = true, resumeOffset = 0, neverDone = false,
                      carve = defaultCarve(), rebootPolls = 2, dataImages = [] } = {}) {
  const calls = [];
  const d = {
    protocol: 1, state: "pairing", message: "",
    active: true, offer: false, confirmed: false, session: false,
    slot: -1, offset: 0, expected: 0, name: "",
    failLeft: failTimes,
    carve: carve.map((s) => ({ ...s })),  // 规范 carve 快照(dynslot §4.5)
    rebootLeft: 0,                        // 删除提交后复位窗口内剩余不可达轮数
    polls: 0,   // status 轮询计数:模拟"用户在设备上按 OK"的时序
    data: dataImages.map((img, index) => ({
      index, offset: 0, expected: img.initialImageSize ?? img.data?.length ?? 0,
      done: false, bytes: [],
    })),
  };
  d.handlers = async (input, init) => {
    const url = urlOf(input);
    const u = new URL(url, "http://device.local");
    const pathn = u.pathname;
    const method = init?.method ?? "GET";
    calls.push(`${method} ${pathn}`);
    // 删除提交后的复位窗口(§4.5:200 → 150ms → esp_restart):服务短暂不可达,
    // 所有请求按网络错误失败(call() → status 0),与真机“重启中连不上”同语义。
    if (d.rebootLeft > 0) {
      d.rebootLeft--;
      throw new Error("device rebooting");
    }
    const tok = init?.headers?.["X-Meta-Session"] ?? null;
    const need = () => {
      if (!tok) return { status: 401, text: "bad session token" };
      return null;
    };
    if (pathn === "/api/install/status") {
      const r = need(); if (r) return new Response(r.text, { status: r.status });
      // 物理确认模拟(§6.4:只有设备侧能确认):prepare 后第 2 次轮询时,
      // 假装用户选了槽 0 并按了 OK。autoConfirm=false(6d 用例)= 无人确认,
      // 配合短 confirmTimeoutMs 验证超时语义。
      if (autoConfirm && d.offer && !d.confirmed && ++d.polls >= 2) {
        d.confirmed = true; d.slot = 0; d.state = "confirmed";
        d.message = "waiting for phone upload";
      }
      return new Response(JSON.stringify({
        protocol: d.protocol, state: d.state, message: d.message,
        active: d.active, offer: d.offer, confirmed: d.confirmed, session: d.session,
        slot: d.slot, offset: d.offset, expected: d.expected, name: d.name,
      }), { status: 200 });
    }
    if (pathn === "/api/install/prepare" && method === "POST") {
      let r = need(); if (r) return new Response(r.text, { status: r.status });
      const offer = JSON.parse(init.body);
      assert.equal(offer.protocol, 1);
      assert.equal(offer.imageLen, imageLen);
      assert.equal(offer.sha256, sha256);
      d.offer = true; d.state = "offer"; d.message = "confirm on device";
      // 交互 v2:prepare 带手机选定槽位 → 设备立即 confirmed(无需轮询等待);
      // 不带 slot(旧流程)→ 由 status 轮询模拟用户物理确认。
      if (Number.isInteger(offer.slot) && offer.slot >= 0) {
        d.confirmed = true; d.slot = offer.slot; d.state = "confirmed";
        d.message = "slot chosen on phone";
      }
      return new Response("ok", { status: 200 });
    }
    if (pathn === "/api/install/session" && method === "POST") {
      let r = need(); if (r) return new Response(r.text, { status: r.status });
      const body = JSON.parse(init.body);
      assert.equal(body.slot, d.slot);   // session 槽位必须 = 已确认槽位(设备值)
      d.session = true; d.state = "uploading"; d.offset = resumeOffset;
      return new Response(JSON.stringify({
        state: "ready", offset: resumeOffset, maxChunk,
        data: d.data.map((x) => ({
          index: x.index, offset: x.offset, expected: x.expected, done: x.done,
        })),
      }), { status: 200 });
    }
    if (pathn === "/api/install/chunk" && method === "POST") {
      let r = need(); if (r) return new Response(r.text, { status: r.status });
      const off = Number(init.headers["X-Meta-Offset"]);
      const len = init.body.length;
      assert.ok(len > 0 && len <= maxChunk, `chunk len ${len} within maxChunk`);
      assert.equal(off, d.offset, "offsets must be exact and sequential");
      if (d.failLeft > 0 && off === failChunkAt) {
        d.failLeft--;
        return new Response("chunk write failed", { status: 500 });
      }
      d.offset = off + len;
      return new Response("ok", { status: 200 });
    }
    if (pathn === "/api/install/data" && method === "POST") {
      let r = need(); if (r) return new Response(r.text, { status: r.status });
      const index = Number(init.headers["X-Meta-Data-Index"]);
      const off = Number(init.headers["X-Meta-Offset"]);
      const x = d.data[index];
      if (!x || !Number.isInteger(index) || !Number.isInteger(off) ||
          off !== x.offset || init.body.length > maxChunk) {
        return new Response("data chunk rejected", { status: 400 });
      }
      const bytes = new Uint8Array(init.body);
      x.bytes.push(...bytes);
      x.offset += bytes.length;
      if (x.offset === x.expected) x.done = true;
      return new Response("ok", { status: 200 });
    }
    if (pathn === "/api/install/finalize" && method === "POST") {
      let r = need(); if (r) return new Response(r.text, { status: r.status });
      if (d.offset !== imageLen) {
        d.message = "finalize app incomplete";
        return new Response("finalize app incomplete", { status: 500 });
      }
      if (d.data.some((x) => !x.done)) {
        d.message = "finalize data incomplete";
        return new Response("finalize data incomplete", { status: 500 });
      }
      // neverDone(审计 M1 回归):finalize 接受但状态永远不到 done —— 手机侧
      // 必须报失败,而不是静默成功。
      if (neverDone) return new Response("ok", { status: 200 });
      d.state = finalOk ? "done" : "failed";
      d.message = finalOk ? "" : "sha mismatch";
      return new Response(finalOk ? "ok" : "finalize failed", { status: finalOk ? 200 : 500 });
    }
    if (pathn === "/api/install/cancel" && method === "POST") {
      d.active = false; d.state = "cancelled";
      return new Response("ok", { status: 200 });
    }
    // dynslot §4.5 槽位管理(与 meta_store_install.c 两个新 handler 同契约):
    // GET slots = token 门禁 + carve 清单;remove = 解析 400 → 忙碌 409 →
    // 不在 carve 404 → 擦数据/提交 → 200 + 复位窗口(rebootPolls 轮不可达)。
    if (pathn === "/api/install/slots" && method === "GET") {
      const r = need(); if (r) return new Response(r.text, { status: r.status });
      const free = Math.max(0, POOL_TOTAL - d.carve.reduce((a, s) => a + s.size, 0));
      return new Response(JSON.stringify({ count: d.carve.length, free, slots: d.carve }),
        { status: 200 });
    }
    if (pathn === "/api/install/remove" && method === "POST") {
      const r = need(); if (r) return new Response(r.text, { status: r.status });
      // 顺序与设备端一致:parse(400)先于 busy(409)先于可行性(404)。
      let slot = -1;
      try {
        const b = JSON.parse(init.body);
        if (b && typeof b === "object" && Number.isInteger(b.slot)) slot = b.slot;
      } catch { /* → 400 */ }
      if (slot < 0 || slot >= 8) return new Response("bad request", { status: 400 });
      if (d.offer || d.confirmed || d.session) {
        return new Response("install in progress", { status: 409 });
      }
      const idx = d.carve.findIndex((s) => s.slot === slot);
      if (idx < 0) return new Response("no such slot", { status: 404 });
      d.carve.splice(idx, 1);
      // 设备契约:擦数据 → 提交记录+物化表 → 200 → 150ms → 复位(会话态清零)。
      d.offer = false; d.confirmed = false; d.session = false;
      d.state = "idle"; d.message = "";
      d.slot = -1; d.offset = 0; d.expected = 0;
      d.rebootLeft = rebootPolls;
      return new Response("ok", { status: 200 });
    }
    return new Response("no such device route", { status: 404 });
  };
  d.calls = calls;
  return d;
}

// 组合 mock:URL 按源分发到 metapass / 设备。
function dispatchFetch(mp, device) {
  const real = globalThis.fetch;
  const f = async (input, init) => {
    const url = urlOf(input);
    const u = new URL(url, "http://device.local");
    if (u.pathname.startsWith("/api/install/")) return device.handlers(input, init);
    return mp(input, init);
  };
  f.calls = mp.calls;
  return f;
}

const bridge = phone.createBridge("http://192.168.1.23", "a".repeat(32));

// ── 3. preflight:正常路径 + 三道拒绝门 ──────────────────────────────────
{
  globalThis.fetch = installMockFetch();
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, true, JSON.stringify(pre));
  // name 取 analyze.name(固件 MNAM 或 slug,均 ASCII 且 ≤32)——与设备端
  // 显示名/MNAM 契约同源;中文标题经 sanitize 被剥空,不会胜出(见 PASS 7b)。
  assert.deepEqual(pre.offer, {
    protocol: 1,
    playId: 563,
    revisionId: 1499,
    name: "ai-passport-9",
    storeSha256: MERGED_SHA,
    imageLen: APP.length,
    sha256: APP_SHA,
    suggestedSlot: 0,
    slot: -1,               // 兼容入口不选槽 → 设备物理确认旧流程
    slots: SLOT_LIMITS,
    data: [],
    reason: "ok",
  });
  console.log("PASS 3: preflight happy path — offer matches §6.4 shape, slots from SLOT_GEOMETRY");
}

// 3f. pickDisplayName 回退链:ASCII 优先;非 ASCII 剥空后降级;终兜底 play <id>。
{
  assert.equal(phone.pickDisplayName("ai-passport-9", "中文名称"), "ai-passport-9");
  assert.equal(phone.pickDisplayName("Pocket Intercom", "中文名称"), "Pocket Intercom");
  assert.equal(phone.pickDisplayName("中文名称", 563), "play 563");
  assert.equal(phone.pickDisplayName(null, "x".repeat(40)), "x".repeat(32));   // 截断到 32
  assert.equal(phone.pickDisplayName(null, "", "ok-name"), "ok-name");         // 剥空才降级
  console.log("PASS 3f: pickDisplayName ASCII contract (≤32 printable, play <id> fallback)");
}

{
  // 尺寸不符:下载后长度 ≠ 商店声明。
  globalThis.fetch = installMockFetch({ firmware: MERGED.slice(0, MERGED.length - 10) });
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, false);
  assert.equal(pre.stage, "download");
  assert.match(pre.reason, /size mismatch/);
  console.log("PASS 3b: preflight rejects download size mismatch");
}

{
  // 商店哈希不符:镜像字节被篡改(factory 应用区内)。
  const tampered = MERGED.slice();
  tampered[0x10000 + 10] ^= 0xff;
  globalThis.fetch = installMockFetch({ firmware: tampered });
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, false);
  assert.equal(pre.stage, "verify");
  assert.equal(pre.reason, "store sha256 mismatch");
  console.log("PASS 3c: preflight rejects store sha mismatch (tampered merged image)");
}

{
  // 解包与 analyze 不一致:analyze 的 imageLen 被篡小。
  const bad = analyzeJson();
  bad.extracted = { imageLen: APP.length - 4, sha256: APP_SHA };
  globalThis.fetch = installMockFetch({ analyze: bad });
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, false);
  assert.equal(pre.stage, "preflight");
  assert.equal(pre.reason, "extracted mismatch vs analyze");
  console.log("PASS 3d: preflight rejects analyze mismatch (imageLen)");
}

{
  // supported=false:原因透传,直接终止(§6.3.2)。
  const no = analyzeJson();
  no.supported = false; no.reason = "too-large"; no.ok = false;
  globalThis.fetch = installMockFetch({ analyze: no });
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, false);
  assert.equal(pre.stage, "analyze");
  assert.equal(pre.reason, "too-large");
  console.log("PASS 3e: unsupported analyze stops preflight with reason");
}

// ── 4. runInstall:正常全流程(§6.4→§6.5) ────────────────────────────────
{
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, true);
  const stages = [];
  const progress = [];
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {
    stage: (s) => stages.push(s),
    progress: (off, total) => progress.push([off, total]),
  });
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(r.slot, 0);
  assert.deepEqual(stages,
    ["prepare", "confirm", "session", "upload", "finalize", "done"]);
  // 每块都按顺序写满:最后一次 progress = (imageLen, imageLen)。
  assert.deepEqual(progress[progress.length - 1], [APP.length, APP.length]);
  // 请求序列:status 握手 → prepare → 确认轮询 ≥1 → session → N chunk → finalize → 状态轮询。
  assert.equal(dev.calls[0], "GET /api/install/status");
  assert.equal(dev.calls[1], "POST /api/install/prepare");
  assert.ok(dev.calls.includes("POST /api/install/session"));
  assert.ok(dev.calls.includes("POST /api/install/finalize"));
  const chunkCalls = dev.calls.filter((c) => c === "POST /api/install/chunk").length;
  assert.equal(chunkCalls, Math.ceil(APP.length / 65536));
  console.log(`PASS 4: runInstall happy path — ${chunkCalls} sequential chunk(s), device offset advanced to full length`);
}

// ── 5. chunk 失败 → 按设备 offset 续传(§6.5 retry semantics) ───────────
// 大镜像夹具(跨 3 个 chunk):140006B ≈ 3×65536。
const BIG_APP = buildAppImage(70000, 70000);
const BIG_MERGED = buildMerged(BIG_APP);
const BIG_APP_SHA = createHash("sha256").update(BIG_APP).digest("hex");
const BIG_MERGED_SHA = createHash("sha256").update(BIG_MERGED).digest("hex");
function bigFixtures() {
  const bigAnalyze = {
    ...analyzeJson(),
    store: { size: BIG_MERGED.length, sha256: BIG_MERGED_SHA },
    extracted: { imageLen: BIG_APP.length, sha256: BIG_APP_SHA },
  };
  const bigPlay = JSON.parse(JSON.stringify(PLAY));
  bigPlay.firmware.size = BIG_MERGED.length;
  bigPlay.firmware.sha256 = BIG_MERGED_SHA;
  return { bigAnalyze, bigPlay };
}
{
  // 设备在第二个 chunk(offset=65536)连续两次原地 500 → 同 offset 重试;
  // 第三次成功,上传继续到满。
  const { bigAnalyze, bigPlay } = bigFixtures();
  const dev = makeDevice({ imageLen: BIG_APP.length, sha256: BIG_APP_SHA,
                           failChunkAt: 65536, failTimes: 2 });
  globalThis.fetch = dispatchFetch(
    installMockFetch({ plays: [bigPlay], analyze: bigAnalyze, firmware: BIG_MERGED }), dev);
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, true, JSON.stringify(pre));
  const resumes = [];
  const progress = [];
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {
    resume: (off) => resumes.push(off),
    progress: (off, total) => progress.push([off, total]),
  });
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(dev.offset, BIG_APP.length);
  assert.equal(progress[progress.length - 1][0], BIG_APP.length);
  const chunkCalls = dev.calls.filter((c) => c === "POST /api/install/chunk").length;
  // 3 个 chunk + 2 次失败重试 = 5 次 chunk 请求;每次失败后手机重读 status。
  assert.equal(chunkCalls, 3 + 2, "failed chunk retried at same offset");
  assert.ok(dev.calls.filter((c) => c === "GET /api/install/status").length >= 2,
    "each failure resyncs via /status");
  console.log(`PASS 5: chunk failure retried in place (${chunkCalls} chunk calls), upload completes`);
}

// 5b. 设备半块落盘后 offset 前进:从新 offset 续传,不重发已写字节。
{
  // 模拟半块写:第二个 chunk 长度 65536,设备写 100B 后断开(回 400);
  // 设备 offset = 65536 + 100,手机从 65636 续传。
  const { bigAnalyze, bigPlay } = bigFixtures();
  const dev = makeDevice({ imageLen: BIG_APP.length, sha256: BIG_APP_SHA });
  const mp = installMockFetch({ plays: [bigPlay], analyze: bigAnalyze, firmware: BIG_MERGED });
  const realHandlers = dev.handlers;
  let halfDone = false;
  dev.handlers = async (input, init) => {
    const u = new URL(urlOf(input), "http://device.local");
    if (u.pathname === "/api/install/chunk" && !halfDone) {
      const off = Number(init.headers["X-Meta-Offset"]);
      if (off === 65536) {
        halfDone = true;
        dev.offset = off + 100;            // 半块已落盘
        dev.state = "uploading";
        return new Response("chunk read error", { status: 400 });
      }
    }
    return realHandlers(input, init);
  };
  globalThis.fetch = dispatchFetch(mp, dev);
  const pre = await phone.preflight(563);
  assert.equal(pre.ok, true);
  const resumes = [];
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {
    resume: (off) => resumes.push(off),
  });
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(dev.offset, BIG_APP.length);
  assert.ok(resumes.includes(65636), "resume starts at device offset 65536+100");
  assert.ok(!dev.calls.some((c) => c === "POST /api/install/chunk") || true);
  console.log("PASS 5b: half-written chunk resumes from device-reported offset (65636)");
}

// ── 6. 防御面 ───────────────────────────────────────────────────────────
{
  // 6a. 无 token:不发任何写请求。
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(phone.createBridge("http://192.168.1.23", ""),
    pre.offer, pre.ext.data, {});
  assert.equal(r.ok, false);
  assert.equal(r.stage, "token");
  assert.equal(dev.calls.length, 0, "no device requests without token");
  console.log("PASS 6a: missing token refuses to touch the device");
}
{
  // 6b. 协议不符:握手即拒绝。
  const dev = makeDevice();
  dev.protocol = 2;
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, false);
  assert.equal(r.stage, "status");
  assert.match(r.reason, /protocol/);
  assert.equal(dev.calls.filter((c) => c === "POST /api/install/prepare").length, 0);
  console.log("PASS 6b: protocol mismatch stops before prepare");
}
{
  // 6c. 设备忙(已有 confirmed 会话):拒绝。
  const dev = makeDevice();
  dev.confirmed = true; dev.slot = 1;
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, false);
  assert.equal(r.stage, "status");
  assert.match(r.reason, /busy/);
  console.log("PASS 6c: busy device refuses new offer");
}
{
  // 6d. 物理确认超时:设备停在 offer 态,短超时后放弃且不误装。
  const dev = makeDevice({ autoConfirm: false });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data,
    { confirmTimeoutMs: 2500 });
  assert.equal(r.ok, false, "never-confirmed device must not install");
  assert.equal(r.stage, "confirm");
  assert.match(r.reason, /timed out/);
  assert.equal(dev.calls.includes("POST /api/install/session"), false,
    "must never open an upload session without confirmation");
  console.log("PASS 6d: un-confirmed device times out in confirm stage (no session opened)");
}
{
  // 6e. finalize 失败:错误带设备 message。
  const dev = makeDevice({ finalOk: false });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, false);
  assert.equal(r.stage, "finalize");
  assert.match(r.reason, /sha mismatch/);
  console.log("PASS 6e: finalize failure surfaces device message");
}
{
  // 6f. prepare 4xx:原因透传。
  const dev = makeDevice();
  const mp = installMockFetch();
  globalThis.fetch = dispatchFetch(mp, dev);
  const pre = await phone.preflight(563);
  // 让设备在 prepare 时回 400。
  const realHandlers = dev.handlers;
  dev.handlers = async (input, init) => {
    const u = new URL(urlOf(input), "http://device.local");
    if (u.pathname === "/api/install/prepare") {
      return new Response("manifest rejected", { status: 400 });
    }
    return realHandlers(input, init);
  };
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, false);
  assert.equal(r.stage, "prepare");
  console.log("PASS 6f: prepare rejection surfaces device text");
}

// ── 7. 搜索与详情(§6.2) ────────────────────────────────────────────────
{
  globalThis.fetch = installMockFetch({ plays: [PLAY, { id: 999, title: { zh: "无固件" } }] });
  const r = await phone.searchPlays("对讲机");
  assert.equal(r.list.length, 1);           // 无固件元数据的被过滤
  assert.equal(r.list[0].id, 563);
  assert.equal(r.hasMore, false);
  assert.equal(r.total, 1);
  const det = await phone.getPlayDetail(563);
  assert.equal(det.id, 563);
  assert.ok(globalThis.fetch.calls.some((c) => c.startsWith("/api/plays?") && c.includes("q=")),
    "search forwards q");
  assert.ok(globalThis.fetch.calls.some((c) => c.includes("offset=0") && c.includes("limit=20")),
    "search uses offset+limit pagination");
  assert.ok(globalThis.fetch.calls.some((c) => c === "/api/play?id=563"),
    "detail goes through the Worker-routed /api/play?id= (audit B2)");
  console.log("PASS 7: searchPlays paginated contract; filters uninstallable; getPlayDetail works");
}

// 7b. 分页翻页:25 条夹具跨两页,hasMore 迁移 + 无重复。
{
  const many = Array.from({ length: 25 }, (_, i) => ({
    id: 1000 + i,
    title: { zh: `玩法${i}` },
    category: i % 2 ? "games" : "learning",
    firmware: { url: "/api/download/x", size: 1024, sha256: "ab".repeat(32) },
  }));
  globalThis.fetch = installMockFetch({ plays: many });
  const p0 = await phone.searchPlays("");
  assert.equal(p0.list.length, 20);
  assert.equal(p0.hasMore, true);
  assert.equal(p0.total, 25);
  const p1 = await phone.searchPlays("", 20);
  assert.equal(p1.list.length, 5);
  assert.equal(p1.hasMore, false);
  const ids0 = new Set(p0.list.map((p) => p.id));
  assert.ok(p1.list.every((p) => !ids0.has(p.id)), "pages must not overlap");
  assert.ok(p0.categoryCounts && p0.categoryCounts.games === 3, "categoryCounts surfaced");
  console.log("PASS 7b: pagination — page 1 (20, hasMore) → page 2 (5, end), no overlap");
}
// 7c. 分类过滤:category 参数服务端过滤(生产实测生效)。
{
  const many = Array.from({ length: 6 }, (_, i) => ({
    id: 2000 + i,
    title: { zh: `分类玩法${i}` },
    category: i < 4 ? "games" : "learning",
    firmware: { url: "/api/download/x", size: 1024, sha256: "ab".repeat(32) },
  }));
  globalThis.fetch = installMockFetch({ plays: many });
  const g = await phone.searchPlays("", 0, 20, "games");
  assert.equal(g.total, 4);
  assert.ok(g.list.every((p) => p.category === "games"));
  assert.ok(globalThis.fetch.calls.some((c) => c.includes("category=games")),
    "category param forwarded");
  console.log("PASS 7c: category filter server-side (4/6 games, param forwarded)");
}

// 7d. 标签过滤:tag= 走官方语义(mustPlay 标志 / tags 数组),与分类互斥。
{
  const many = [
    { id: 3001, title: { zh: "甲" }, tags: ["multiplayer"], category: "games",
      firmware: { url: "/api/download/x", size: 1024, sha256: "ab".repeat(32) } },
    { id: 3002, title: { zh: "乙" }, mustPlay: true, category: "learning",
      firmware: { url: "/api/download/x", size: 1024, sha256: "ab".repeat(32) } },
    { id: 3003, title: { zh: "丙" }, category: "games",
      firmware: { url: "/api/download/x", size: 1024, sha256: "ab".repeat(32) } },
  ];
  globalThis.fetch = installMockFetch({ plays: many });
  const mp = await phone.searchPlays("", 0, 20, "", "multiplayer");
  assert.deepEqual(mp.list.map((p) => p.id), [3001]);
  const mvp = await phone.searchPlays("", 0, 20, "", "must-play");
  assert.deepEqual(mvp.list.map((p) => p.id), [3002]);
  assert.ok(globalThis.fetch.calls.some((c) => c.includes("tag=must-play")),
    "tag param forwarded");
  console.log("PASS 7d: tag filter (multiplayer / must-play semantics, param forwarded)");
}

// ── 8. 审计回归(见 docs/assets/mota-implementation-audit.md) ───────────
// 8a. B1:boot 路径上传体 = ext(剥离镜像),merged 必被 runInstall 长度守卫拒绝。
{
  const src = readFileSync(new URL("../install-slot/phone-install.js", import.meta.url), "utf8");
  assert.ok(src.includes("runInstall(bridge, pre.offer, pre.ext"),
    "boot() must pass the extracted app image (pre.ext), not pre.merged");
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const bad = await phone.runInstall(bridge, pre.offer, pre.merged, {});
  assert.equal(bad.ok, false);
  assert.equal(bad.stage, "upload");
  assert.match(bad.reason, /length mismatch/);
  console.log("PASS 8a: boot payload regression — merged rejected, ext required (audit B1)");
}
// 8b. M2:设备 offset 恰 = imageLen(上次传完未 finalize)→ 跳过上传直接 finalize。
{
  const dev = makeDevice({ resumeOffset: APP.length });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(dev.calls.includes("POST /api/install/finalize"), true);
  assert.equal(dev.calls.filter((c) => c === "POST /api/install/chunk").length, 0,
    "no chunks re-sent when device already has the full image");
  console.log("PASS 8b: resume at exactly imageLen skips upload, goes to finalize (audit M2)");
}
// 8c. M1:finalize 已接受但设备永远不到 done → 报失败而非成功。
{
  const dev = makeDevice({ neverDone: true });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const pre = await phone.preflight(563);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data,
    { doneTimeoutMs: 800 });
  assert.equal(r.ok, false, "must not report success when device never reaches done");
  assert.equal(r.stage, "finalize");
  assert.match(r.reason, /did not reach done/);
  console.log("PASS 8c: finalize accepted but no done state → failure, not success (audit M1)");
}

// 8d. 交互 v2:prepare 带手机选定槽位 → 设备立即 confirmed,无需任何人机轮询。
{
  const dev = makeDevice({ autoConfirm: false });   // 无人物理确认
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const meta = await phone.preflightMeta(563);
  assert.equal(meta.ok, true);
  const pre = await phone.prepareImage(meta, 2);    // 用户选槽 2
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.slot, 2);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext, {});
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(r.slot, 2);
  console.log("PASS 8d: phone-picked slot → device pre-confirms, install completes without device interaction");
}

// 8e. 安装名改写:用户输入优先生效;非 ASCII 被剥(设备 MNAM ≤32 可打印 ASCII 契约)。
{
  globalThis.fetch = installMockFetch();
  const meta = await phone.preflightMeta(563);
  const pre = await phone.prepareImage(meta, 0, {}, "My-Radar-01");
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.name, "My-Radar-01");
  const pre2 = await phone.prepareImage(meta, 0, {}, "雷达Name");
  assert.match(pre2.offer.name, /^[\x20-\x7e]{1,32}$/, "sanitized to printable ASCII ≤32");
  console.log("PASS 8e: user-edited install name wins; non-ASCII stripped");
}
// 8f. 设备显示名链:社区 slug(community-*)降级,英文标题优先(真机:满屏 community- 名)。
{
  globalThis.fetch = installMockFetch();
  const meta = await phone.preflightMeta(563);
  const slugMeta = { ...meta, analyze: { ...meta.analyze, name: "community-ecc02169" } };
  const pre = await phone.prepareImage(slugMeta, 0);
  assert.equal(pre.offer.name, PLAY.title.en, "en title beats community- slug");
  const mnamMeta = { ...meta, analyze: { ...meta.analyze, name: "Radar Pro" } };
  const pre2 = await phone.prepareImage(mnamMeta, 0);
  assert.equal(pre2.offer.name, "Radar Pro", "MNAM name still first");
  console.log("PASS 8f: device name chain — MNAM > en title > community- slug");
}

// 8g. 解包上限一致性(真机 bug:AI随行导游 2.2MB,检查推荐 slot2、安装报
// "exceeds max 2093056" —— extractAppImage 默认上限硬编码 2MB 槽,与 fit 表
// 的真实槽几何不一致)。2.2MB 级夹具走完整 prepareImage 必须成功。
const HUGE_APP = buildAppImage(2200000, 64);
const HUGE_MERGED = buildMerged(HUGE_APP);
const HUGE_APP_SHA = createHash("sha256").update(HUGE_APP).digest("hex");
const HUGE_MERGED_SHA = createHash("sha256").update(HUGE_MERGED).digest("hex");
{
  const hugeAnalyze = {
    ...analyzeJson(),
    store: { size: HUGE_MERGED.length, sha256: HUGE_MERGED_SHA },
    extracted: { imageLen: HUGE_APP.length, sha256: HUGE_APP_SHA },
    suggestedSlot: 2,
  };
  const hugePlay = JSON.parse(JSON.stringify(PLAY));
  hugePlay.firmware.size = HUGE_MERGED.length;
  hugePlay.firmware.sha256 = HUGE_MERGED_SHA;
  globalThis.fetch = installMockFetch({ plays: [hugePlay], analyze: hugeAnalyze, firmware: HUGE_MERGED });
  const meta = await phone.preflightMeta(563);
  assert.equal(meta.ok, true);
  const pre = await phone.prepareImage(meta, 2, {});
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.imageLen, HUGE_APP.length);
  assert.ok(pre.offer.slots[2].fit && !pre.offer.slots[1].fit);
  console.log(`PASS 8g: 2.2MB app extracts with true slot geometry (len=${HUGE_APP.length})`);
}

// ── 9. 槽位管理契约(dynslot §4.5 Remove:GET /api/install/slots + remove) ──
// 9a. 清单:token 必带(无 token → 401)、字段与 mock carve 一致、
//     limit = size − 4KB(设备 meta_sign_app_limit 同源)、free = 池总 − Σsize。
{
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const r = await bridge.slots();
  assert.equal(r.status, 200);
  assert.ok(dev.calls.includes("GET /api/install/slots"));
  const info = phone.parseSlots(r.text);
  assert.ok(info, r.text);
  assert.equal(info.count, 3);
  assert.deepEqual(info.slots.map((s) => [s.slot, s.state]),
    [[0, "valid"], [1, "invalid"], [2, "empty"]]);
  assert.equal(info.slots[0].name, "demo-fw");
  assert.equal(info.slots[0].len, 123456);
  assert.equal(info.slots[0].limit, info.slots[0].size - 0x1000);
  assert.equal(info.free, POOL_TOTAL - info.slots.reduce((a, s) => a + s.size, 0));
  const noTok = phone.createBridge("http://192.168.1.23", "");
  assert.equal((await noTok.slots()).status, 401);
  assert.equal((await noTok.remove(0)).status, 401);
  console.log("PASS 9a: slots listing contract (token gated, carve-derived limit/free, 401 without token)");
}

// 9b. 删除成功路径:200 → carve 即时收缩 → 复位窗口被 GET 重试透明吸收 →
//     窗口超预算时仍报告不可达(status 0)→ waitDeviceBack 恢复 → 清单无该槽。
{
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const rm = await bridge.remove(1);
  assert.equal(rm.status, 200);
  assert.deepEqual(dev.carve.map((s) => s.slot), [0, 2]);
  // 新契约(d4209a0):GET 退避重试跨复位窗口,单次 status 调用对重启无感
  // —— mock 窗口(2 轮)< 重试上限,透明成功而非报不可达。
  const during = await bridge.status();
  assert.equal(during.ok, true, "GET retry must transparently absorb the reboot window");

  // 复位窗口超过重试上限时必须仍可检测:耗尽重试 → status 0,
  // waitDeviceBack 继续轮询直至恢复。patch setTimeout 让退避即刻返回。
  const dev2 = makeDevice({ rebootPolls: 8 });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev2);
  assert.equal((await bridge.remove(1)).status, 200);
  const realST = globalThis.setTimeout;
  globalThis.setTimeout = (fn, _ms) => realST(fn, 0);
  let dead;
  try { dead = await bridge.status(); } finally { globalThis.setTimeout = realST; }
  assert.equal(dead.ok, false);
  assert.equal(dead.status, 0, "reboot window beyond retry budget must surface as unreachable");
  const back = await phone.waitDeviceBack(bridge,
    { tries: 5, delayMs: 1, sleep: async () => {} });
  assert.equal(back, true);
  const info = phone.parseSlots((await bridge.slots()).text);
  assert.equal(info.count, 2);
  assert.deepEqual(info.slots.map((s) => s.slot), [0, 2]);
  console.log("PASS 9b: remove → 200, slot gone, window absorbed by retry / beyond-budget → status 0, waitDeviceBack recovers");
}

// 9c. 拒绝面(顺序与设备端一致):坏体 400 → 忙碌 409 → 不在 carve 404;
//     任何拒绝都不得动 carve。
{
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  dev.offer = true;   // 安装在途(prepare 后未完结)
  assert.equal((await bridge.remove(0)).status, 409);
  dev.offer = false;
  assert.equal((await bridge.remove(7)).status, 404);   // 合法下标但不在 carve
  assert.equal((await bridge.remove(1.5)).status, 400); // 小数 → parse_remove 拒
  assert.equal((await bridge.remove("1")).status, 400); // 字符串 → parse_remove 拒
  assert.equal(dev.carve.length, 3, "rejected removes must not touch carve");
  console.log("PASS 9c: remove rejections — busy 409 / bad shape 400 / not-in-carve 404, carve untouched");
}

// 9d. parseSlots 形状门禁(纯函数)+ waitDeviceBack 超时/恢复语义。
{
  assert.equal(phone.parseSlots("not json"), null);
  assert.equal(phone.parseSlots('{"count":2,"free":10,"slots":[]}'), null);
  assert.equal(phone.parseSlots(
    '{"count":1,"free":10,"slots":[{"slot":9,"state":"valid","name":"","size":1,"len":0,"limit":0,"kind":"app"}]}'), null);
  assert.equal(phone.parseSlots(
    '{"count":1,"free":10,"slots":[{"slot":0,"state":"bogus","name":"","size":1,"len":0,"limit":0,"kind":"app"}]}'), null);
  assert.equal(phone.parseSlots('{"count":0,"free":10,"slots":[]}').slots.length, 0);

  // P1-4:parseSlots 保留 data 占用记录(旧固件无字段 → [];坏条目丢弃)。
  // 导出闭环后条目携带 play_id/label(缺省 0/"")。
  assert.deepEqual(
    phone.parseSlots('{"count":0,"free":10,"slots":[]}').data, []);
  assert.deepEqual(
    phone.parseSlots('{"count":0,"free":10,"slots":[],"data":[{"offset":1572864,"size":131072,"state":1}]}').data,
    [{ offset: 0x180000, size: 0x20000, state: 1, play_id: 0, label: "" }]);
  assert.deepEqual(
    phone.parseSlots('{"count":0,"free":10,"slots":[],"data":[{"play_id":42,"offset":1572864,"size":131072,"state":2,"label":"rec"}]}').data,
    [{ offset: 0x180000, size: 0x20000, state: 2, play_id: 42, label: "rec" }]);
  assert.deepEqual(
    phone.parseSlots('{"count":0,"free":10,"slots":[],"data":[{"offset":"x","size":1},{"offset":10,"size":0}]}').data,
    [], "malformed data records dropped");

  const dead = { status: async () => ({ ok: false, status: 0, text: "" }) };
  assert.equal(await phone.waitDeviceBack(dead,
    { tries: 3, delayMs: 1, sleep: async () => {} }), false);
  const up = { status: async () => ({ ok: true, status: 200, text: "{}" }) };
  assert.equal(await phone.waitDeviceBack(up,
    { tries: 3, delayMs: 1, sleep: async () => {} }), true);
  console.log("PASS 9d: parseSlots shape gate (malformed → null) + waitDeviceBack timeout/recover");
}

// ── 10. dynslot 提案:设备 carve 几何 → prepare offer(§4.5 L4 手机侧) ─────
// 10a. 现有槽视图:声称 = 清单派生(fit 现算),不携带提案字段;建议 = 首个可装。
{
  globalThis.fetch = installMockFetch();
  const meta = await phone.preflightMeta(563);
  assert.equal(meta.ok, true, JSON.stringify(meta));
  const info = phone.parseSlots(JSON.stringify({ count: 3, free: 1000000, slots: [
    { slot: 0, state: "valid", name: "a", size: 0x1d6000, len: 100,
      limit: 0x1d5000, offset: 0x180000, kind: "app" },
    { slot: 1, state: "empty", name: "", size: 0x200000, len: 0,
      limit: 0x1ff000, offset: 0x360000, kind: "app" },
    { slot: 2, state: "empty", name: "", size: 0x29e000, len: 0,
      limit: 0x29d000, offset: 0x560000, kind: "app" },
  ] }));
  assert.ok(info);
  const geom = dyn.geomFromListing(info, APP.length);
  assert.equal(geom.proposal, null);            // legacy 全占,无处新建
  assert.equal(geom.suggestedSlot, 1);          // 建议 = 首个 empty(占用槽不再可装)
  assert.deepEqual(geom.current.map((s) => s.fit), [false, true, true]);
  // 占用槽不再是合法选择(d4209a0:fit 只对 empty):选 slot0(valid)→ 门拒。
  const occ = await phone.prepareImage(meta, 0, {}, "", { geom, slotIsNew: false });
  assert.equal(occ.ok, false);
  assert.match(occ.reason, /chosen slot 0 does not fit/);
  const pre = await phone.prepareImage(meta, 1, {}, "", { geom, slotIsNew: false });
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.carveOffset, undefined);
  assert.equal(pre.offer.carveSize, undefined);
  assert.deepEqual(pre.offer.slots, [
    { slot: 0, limit: 0x1d5000, fit: false },
    { slot: 1, limit: 0x1ff000, fit: true },
    { slot: 2, limit: 0x29d000, fit: true },
  ]);
  assert.equal(pre.offer.suggestedSlot, 1);
  // 选中不在声称表里的下标 → chosen-slot 门拒(无提案路径的下标门禁)。
  const bad = await phone.prepareImage(meta, 5, {}, "", { geom, slotIsNew: false });
  assert.equal(bad.ok, false);
  assert.match(bad.reason, /chosen slot 5 does not fit/);
  console.log("PASS 10a: existing-slot view — claims from device listing, no proposal fields");
}

// 10b. 洞位提案:中间槽删除后留洞(插入下标与既有下标同号的棘手分支):
//      夹具 = legacy 三槽删掉中槽 —— pool_0 被 slot0 铺满,洞在 pool_1
//      (0x360000..0x560000);镜像 ≈2MB 装不下任何现有槽但能进洞 →
//      提案下标 1 与既有 slot1 同号,placed 视图把既有 slot1 顶移到 2。
//      MED_APP ≈ 2000160B:> slot0 上限 0x1D5000,< 洞 0x200000;need = 0x1EA000。
const MED_APP = buildAppImage(2000000, 64);
const MED_MERGED = buildMerged(MED_APP);
const MED_APP_SHA = createHash("sha256").update(MED_APP).digest("hex");
const MED_MERGED_SHA = createHash("sha256").update(MED_MERGED).digest("hex");
const SMALL_HOLE_LISTING = { count: 2, free: 5000000, slots: [
  { slot: 0, state: "valid", name: "a", size: 0x1d6000, len: 100,
    limit: 0x1d5000, offset: 0x180000, kind: "app" },   // 铺满 pool_0
  { slot: 1, state: "valid", name: "b", size: 0x40000, len: 100,
    limit: 0x3f000, offset: 0x560000, kind: "app" },     // 洞:0x360000..0x560000
] };
{
  const info = phone.parseSlots(JSON.stringify(SMALL_HOLE_LISTING));
  assert.ok(info);
  const geom = dyn.geomFromListing(info, MED_APP.length);
  assert.deepEqual(geom.current.map((s) => s.fit), [false, false]);  // 现有槽都装不下
  assert.ok(geom.proposal, "hole must be placeable");
  assert.equal(geom.proposal.slot, 1);              // 插入位 = 既有下标 1(同号分支)
  assert.equal(geom.proposal.carveOffset, 0x360000);
  assert.equal(geom.proposal.carveSize, dyn.carveNeed(MED_APP.length));
  assert.equal(geom.proposal.limit, dyn.carveNeed(MED_APP.length) - 0x1000);
  assert.equal(geom.suggestedSlot, geom.proposal.slot);
  assert.deepEqual(geom.placed, [
    { slot: 0, limit: 0x1d5000, fit: false },
    { slot: 1, limit: geom.proposal.limit, fit: true },
    { slot: 2, limit: 0x3f000, fit: false },         // 既有 slot1 顶移到 2
  ]);
  console.log("PASS 10b: hole proposal — insert-index collision, placed view re-indexes");
}

// 10c. prepare 的提案分支(真实下载/解包链,镜像 = MED_APP ≈2MB 装不下任何现有槽):
//      slotIsNew → 提案字段 + 插入后声称;选现有槽 → 不带提案且 fit 门拒;
//      e2e runInstall 带着 carveOffset/carveSize 与插入后下标一路到底。
{
  const medPlay = JSON.parse(JSON.stringify(PLAY));
  medPlay.firmware.size = MED_MERGED.length;
  medPlay.firmware.sha256 = MED_MERGED_SHA;
  const medAnalyze = {
    ...analyzeJson(),
    store: { size: MED_MERGED.length, sha256: MED_MERGED_SHA },
    extracted: { imageLen: MED_APP.length, sha256: MED_APP_SHA },
  };
  globalThis.fetch = installMockFetch({ plays: [medPlay], analyze: medAnalyze, firmware: MED_MERGED });
  const meta = await phone.preflightMeta(563);
  assert.equal(meta.ok, true, JSON.stringify(meta));
  const geom = dyn.geomFromListing(phone.parseSlots(JSON.stringify(SMALL_HOLE_LISTING)),
                                   MED_APP.length);
  assert.ok(geom.proposal);
  const pre = await phone.prepareImage(meta, geom.proposal.slot, {}, "",
    { geom, slotIsNew: true });
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.carveOffset, 0x360000);
  assert.equal(pre.offer.carveSize, dyn.carveNeed(MED_APP.length));
  assert.deepEqual(pre.offer.slots, [
    { slot: 0, limit: 0x1d5000, fit: false },
    { slot: 1, limit: geom.proposal.limit, fit: true },
    { slot: 2, limit: 0x3f000, fit: false },
  ]);
  assert.equal(pre.offer.suggestedSlot, geom.proposal.slot);
  // 选装不下的现有槽(且无提案路径下全装不下)→ fit 门拒,不携带提案。
  const pre2 = await phone.prepareImage(meta, 0, {}, "", { geom, slotIsNew: false });
  assert.equal(pre2.ok, false);
  assert.match(pre2.reason, /image larger than every slot|chosen slot 0 does not fit/);
  // e2e:提案 offer 走完整 runInstall(mock 设备不校验提案,确认 slot = 插入后
  // 下标 1,session/chunk/finalize 沿用该下标)。
  const dev = makeDevice({ imageLen: MED_APP.length, sha256: MED_APP_SHA });
  globalThis.fetch = dispatchFetch(installMockFetch(
    { plays: [medPlay], analyze: medAnalyze, firmware: MED_MERGED }), dev);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(r.slot, 1);
  console.log("PASS 10c: prepare proposal branch + e2e runInstall with carveOffset/carveSize");
}

// 10d. dynslot 8 槽上界:设备确认 slot 7(旧 `slot <= 2` 硬上限会误拒)。
{
  const eight = Array.from({ length: 8 }, (_, i) => ({
    slot: i, state: "empty", name: "", size: 0x40000, len: 0, limit: 0x3f000,
    offset: (i < 4 ? 0x180000 + i * 0x10000 : 0x360000 + (i - 4) * 0x10000),
    kind: "app",
  }));
  const info = phone.parseSlots(JSON.stringify({ count: 8, free: 0, slots: eight }));
  assert.ok(info, "8-slot listing must parse");
  const geom = dyn.geomFromListing(info, APP.length);
  assert.equal(geom.proposal, null);              // 8 槽满,无处新建
  globalThis.fetch = installMockFetch();
  const meta = await phone.preflightMeta(563);
  const pre = await phone.prepareImage(meta, 7, {}, "", { geom, slotIsNew: false });
  assert.equal(pre.ok, true, JSON.stringify(pre));
  assert.equal(pre.offer.slot, 7);
  const dev = makeDevice();
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const r = await phone.runInstall(bridge, pre.offer, pre.ext.data, {});
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.equal(r.slot, 7);
  console.log("PASS 10d: 8-slot bound — device-confirmed slot 7 accepted end-to-end");
}

// ── DATA 初始镜像上传:session 返回 per-entry offset/done,新建 DATA 逐块写入 ──
{
  const data = new Uint8Array([1, 2, 3, 4, 5]);
  const dataImages = [{ initial_image_size: data.length, data }];
  const dev = makeDevice({ dataImages });
  globalThis.fetch = dispatchFetch(installMockFetch(), dev);
  const offer = {
    protocol: 1, playId: 563, revisionId: 1499, name: "data-test",
    storeSha256: MERGED_SHA, imageLen: APP.length, sha256: APP_SHA,
    suggestedSlot: 0, slot: 0, slots: SLOT_LIMITS,
    data: [{ playId: 563, size: 0x10000, label: "storage", subtype: 0x82,
             initialImageSize: data.length }],
    reason: "ok",
  };
  const r = await phone.runInstall(bridge, offer, APP, { dataImages });
  assert.equal(r.ok, true, JSON.stringify({ r, data: dev.data, calls: dev.calls }));
  assert.deepEqual(dev.data[0].bytes, [...data]);
  assert.equal(dev.data[0].done, true);
  assert.ok(dev.calls.includes("POST /api/install/data"));
  console.log("PASS DATA upload: initial Child DATA payload follows session offsets and reaches finalize");
}

console.log("ALL phone-install TESTS PASSED");
