#!/usr/bin/env node
// tools/install-slot/test-store-analyze.mjs —— store-analyze.js 的 Node 自检。
// 运行:node test-store-analyze.mjs(在本目录下)。
// 用 mock fetch + 合成合并镜像覆盖:golden 路径、too-large、wrong-chip、
// custom-partitions、not-found、sha256 不匹配、缓存去重、extracted 字节一致性。
// 黄金锚点:play 563 的 extracted imageLen=1,844,496 / sha256=0d68…c70a 由真实固件在
// test-extract.mjs 之外的 E2E 覆盖;本文件只锁合成向量的相对关系。

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createStoreAnalyzer, SLOT_GEOMETRY, listPartitions, mapExtractError } from "../../install-slot/store-analyze.js";
import { POOL_TOTAL } from "../../install-slot/dynslot-pool.js";
import { packNameBlobTail } from "../../install-slot/name-blob.js";

const sha256 = async (buf) => createHash("sha256").update(buf).digest("hex");

const BACKEND = "https://example.test";

// ---- 合成镜像构造 ----------------------------------------------------------

// 构造合法 ESP 应用镜像(与 test-extract.mjs 同一布局),segLens 控制镜像大小。
function buildAppImage(segLens, { chipId = 5, hashAppended = 1 } = {}) {
  let bodyLen = 24 + 16 + segLens.reduce((a, l) => a + 8 + l, 0);
  let total = bodyLen;
  while (total % 16 !== 15) total++;
  total += 1 + (hashAppended ? 32 : 0);
  const buf = new Uint8Array(total).fill(0xab);
  buf[0] = 0xe9;
  buf[1] = segLens.length;
  buf[12] = chipId & 0xff;
  buf[13] = (chipId >> 8) & 0xff;
  buf[23] = hashAppended;
  let off = 24 + 16;
  for (const len of segLens) {
    buf.set([0x00, 0x00, 0xc8, 0x3f,
      len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, (len >> 24) & 0xff], off);
    off += 8 + len;
  }
  return buf;
}

// 分区表条目 32B:magic AA 50 + type + subtype + offset U32LE + size U32LE + label(16B)
function makeEntry(label, type, subtype, offset, size) {
  const e = new Uint8Array(32);
  e[0] = 0xaa; e[1] = 0x50;
  e[2] = type; e[3] = subtype;
  e.set([offset & 0xff, (offset >> 8) & 0xff, (offset >> 16) & 0xff, (offset >> 24) & 0xff], 4);
  e.set([size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff, (size >> 24) & 0xff], 8);
  for (let i = 0; i < label.length && i < 16; i++) e[12 + i] = label.charCodeAt(i);
  return e;
}

// meta-pass 8MB 布局分区表(factory@0x10000 + ota_0/1/2 + nvs/phy_init/otadata/cardid)
const META_PARTS = [
  ["nvs", 1, 2, 0x9000, 0x6000],
  ["phy_init", 1, 1, 0xf000, 0x1000],
  ["factory", 0, 0, 0x10000, 0x170000],
  ["ota_0", 0, 16, 0x180000, 0x1d6000],
  ["cardid", 1, 6, 0x356000, 0x4000],
  ["ota_1", 0, 17, 0x360000, 0x200000],
  ["ota_2", 0, 18, 0x560000, 0x29e000],
  ["otadata", 1, 0, 0x7fe000, 0x2000],
];

// 合并镜像:0x8000 分区表 + factory 分区处的应用(可选 MSIG 尾 sector + MNAM 名)。
function buildFullImage(app, { parts = META_PARTS, name = null } = {}) {
  const factoryOff = 0x10000;
  const tailOff = factoryOff + Math.ceil(app.length / 4096) * 4096;
  const total = name != null ? tailOff + 4096 : factoryOff + app.length;
  const full = new Uint8Array(total).fill(0xff);
  parts.forEach((p, i) => full.set(makeEntry(...p), 0x8000 + i * 32));
  full.set(app, factoryOff);
  if (name != null) {
    full.set([0x4d, 0x53, 0x49, 0x47], tailOff); // "MSIG"
    full.set(packNameBlobTail(name), tailOff + 4056);
  }
  return full;
}

// mock fetch:routes = Map<path, {status?, json?, bytes?}>;记录请求次数。
function mockFetch(routes) {
  const calls = [];
  const fn = async (url) => {
    const u = new URL(url);
    calls.push(u.pathname);
    const r = routes.get(u.pathname);
    if (!r) return new Response("not found", { status: 404 });
    if (r.bytes) return new Response(r.bytes, { status: 200 });
    return new Response(JSON.stringify(r.json), { status: r.status ?? 200 });
  };
  fn.calls = calls;
  return fn;
}

// 标准 mock 环境:play 563 元数据 + 下载路由。merged 参数化以便构造异常镜像。
function makeEnv({ app, name = null, parts, metaOver = {}, sha = null, size = null } = {}) {
  const merged = buildFullImage(app, { parts, name });
  const digest = sha ?? sha256Sync(merged);
  const mergedSize = size ?? merged.length;
  const play = {
    play: {
      id: 563, revisionId: 1279, slug: "ai-passport-9",
      firmwareSize: mergedSize, firmwareSha256: digest,
      downloadUrl: "/api/download/community/ai-passport-9",
      firmware: { available: true, size: mergedSize, sha256: digest, url: "/api/download/community/ai-passport-9" },
      ...metaOver,
    },
  };
  const fetchImpl = mockFetch(new Map([
    ["/api/plays/id/563", { json: play }],
    ["/api/download/community/ai-passport-9", { bytes: merged }],
  ]));
  return { fetchImpl, merged, play };
}

function sha256Sync(buf) { return createHash("sha256").update(buf).digest("hex"); }

function makeAnalyzer(fetchImpl, cache = new Map()) {
  return createStoreAnalyzer({ fetchImpl, backend: BACKEND, sha256, cache });
}

// ---- 1. golden 路径:小镜像 + MNAM 名 → supported,suggestedSlot=0,MNAM 名 ----
{
  const app = buildAppImage([100, 64]);
  const { fetchImpl } = makeEnv({ app, name: "demo-app" });
  const a = makeAnalyzer(fetchImpl);
  const out = await a.analyze(563);

  assert.equal(out.ok, true);
  assert.equal(out.supported, true);
  assert.equal(out.name, "demo-app", "MNAM 名优先于 slug");
  assert.equal(out.id, 563);
  assert.equal(out.revisionId, 1279);
  assert.equal(out.suggestedSlot, 0, "最小可装槽位");
  assert.deepEqual(out.slots.map((s) => s.limit), [0x673000], "槽位上限=dynslot 共享池");
  assert.deepEqual(out.slots.map((s) => s.fit), [true], "fit 标志");
  assert.equal(out.extracted.imageLen, app.length);
  assert.equal(out.extracted.sha256, sha256Sync(app), "extracted sha256 对解包后镜像计算");
  console.log("PASS 1: golden analyze (MNAM name, suggestedSlot=0, slot limits)");
}

// ---- 1b. storage manifest: DATA capacity is surfaced separately from initial payload ----
{
  const app = buildAppImage([100, 64]);
  const parts = [
    ...META_PARTS,
    ["storage", 1, 0x82, 0x700000, 0x80000],
  ];
  const { fetchImpl } = makeEnv({ app, parts });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.ok, true);
  assert.equal(out.data.length, 1);
  assert.equal(out.data[0].label, "storage");
  assert.equal(out.data[0].requiredSize, 0x80000);
  assert.equal(out.data[0].initialImageSize, 0);
  assert.equal(out.data[0].subtype, 0x82);
  assert.ok(out.storage);
  assert.equal(out.storage.requiredSize,
    Math.ceil(app.length / 0x1000) * 0x1000 + 0x80000);
  assert.equal(out.storage.supported, true);
  console.log("PASS 1b: analyze exposes APP+DATA storage manifest without changing admission behavior");
}

// ---- 1c. DATA capacity participates in admission, not just APP size ----
{
  const app = buildAppImage([64]);
  const poolLimit = POOL_TOTAL - 0x1000;
  const appNeed = Math.ceil(app.length / 0x1000) * 0x1000;
  const dataSize = poolLimit - appNeed + 0x1000;
  const dataOffset = 0x200000;
  const parts = [
    ...META_PARTS,
    ["storage", 1, 0x82, dataOffset, dataSize],
  ];
  const { fetchImpl } = makeEnv({ app, parts });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.storage.supported, true);
  assert.equal(out.storage.requiredSize, appNeed + dataSize);
  assert.ok(out.storage.requiredSize > poolLimit);
  assert.equal(out.supported, false);
  assert.equal(out.reason, "too-large");
  console.log("PASS 1c: APP+DATA required capacity gates pool admission");
}

// ---- 2. 无 MNAM → 名退回 slug;extracted 字节与镜像一致 ----
{
  const app = buildAppImage([256]);
  const { fetchImpl, merged } = makeEnv({ app });
  const a = makeAnalyzer(fetchImpl);
  const out = await a.analyze(563);
  assert.equal(out.name, "ai-passport-9", "无 MNAM 时退回 slug");
  const ex = await a.extracted(563);
  assert.equal(ex.error, undefined);
  assert.equal(ex.imageLen, app.length);
  assert.equal(ex.sha256, sha256Sync(app));
  assert.deepEqual([...ex.data], [...app], "extracted 返回 factory 应用原字节");
  assert.equal(merged[0x8000], 0xaa, "merged 仍是 full image");
  console.log("PASS 2: name fallback to slug; extracted bytes identical");
}

// ---- 3. too-large:镜像超池上限 → supported=false, reason=too-large ----
{
  const poolLimit = POOL_TOTAL - 0x1000; // 池上限 = POOL_TOTAL - tail sector
  const bigLen = poolLimit + 1; // 恰好超上限
  const app = buildAppImage([bigLen]);
  const { fetchImpl } = makeEnv({ app });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, false);
  assert.equal(out.reason, "too-large");
  console.log("PASS 3: oversized app -> reason=too-large, pool unfit");
}

// ---- 4. wrong-chip:chip_id 非 C3 → reason=wrong-chip ----
{
  const app = buildAppImage([64], { chipId: 2 });
  const { fetchImpl } = makeEnv({ app });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, false);
  assert.equal(out.reason, "wrong-chip");
  console.log("PASS 4: wrong chip id -> reason=wrong-chip");
}

// ---- 5. unsupported child DATA → hard reject before device admission ----
{
  const app = buildAppImage([64]);
  const parts = [...META_PARTS, ["spiffs", 1, 0x84, 0x7f0000, 0x10000]];
  const { fetchImpl } = makeEnv({ app, parts });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, false, "unsupported child DATA must block admission");
  assert.equal(out.reason, "unsupported-partition");
  console.log("PASS 5: unsupported child DATA -> hard reject");
}

// ---- 5b. subtype 0x40 custom DATA → hard reject ----
{
  const app = buildAppImage([64]);
  const parts = [...META_PARTS, ["rec", 1, 0x40, 0x7f0000, 0x80000]];
  const { fetchImpl } = makeEnv({ app, parts });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, false, "custom DATA must block admission");
  assert.equal(out.reason, "unsupported-partition");
  const ex = await makeAnalyzer(fetchImpl).extracted(563);
  assert.equal(ex.error, "unsupported-partition");
  console.log("PASS 5b: custom data partition subtype 0x40 -> hard reject");

// ---- 5c. supported standard DATA remains supported; unsupported subtypes reject ----
// Standard filesystem DATA is part of the allocation/runtime-table contract and
// therefore remains supported. NVS/PHY are global MVP resources and are excluded.
{
  const app = buildAppImage([64]);
  // [label, type, subtype, offset, size]
  const cases = [
    ["easter", 0x82],        // SPIFFS:play 563 真实形态
    ["legacy_cardid", 0x02], // NVS subtype:play 2 真实形态
    ["voicefs", 0x81],       // FAT:play 200 真实形态
  ];
  for (const [label, sub] of cases) {
    const parts = [...META_PARTS, [label, 1, sub, 0x7f0000, 0x80000]];
    const { fetchImpl } = makeEnv({ app, parts });
    const out = await makeAnalyzer(fetchImpl).analyze(563);
    if (sub === 0x82 || sub === 0x81) {
      assert.equal(out.supported, true, `${label}: supported filesystem DATA is admissible`);
      assert.equal(out.reason, "ok");
      assert.equal(out.suggestedSlot, 0);
      const ex = await makeAnalyzer(fetchImpl).extracted(563);
      assert.equal(ex.error, undefined, `${label}: extracted 可用`);
    } else if (sub === 0x02) {
      // Global NVS is meta-pass-owned in MVP: accepted, but excluded from child DATA.
      assert.equal(out.supported, true, `${label}: global NVS must not block admission`);
      assert.equal(out.data.length, 0);
      assert.equal(out.reason, "ok");
    } else {
      assert.equal(out.supported, false, `${label}: unsupported DATA must be rejected`);
      assert.equal(out.reason, "unsupported-partition");
    }
  }
  console.log("PASS 5c: filesystem DATA admitted, global NVS ignored, unsupported DATA rejected");
}
}

// ---- 6. not-found:元数据 404 → reason=not-found(不触碰固件下载) ----
{
  const fetchImpl = mockFetch(new Map());
  const a = makeAnalyzer(fetchImpl);
  const out = await a.analyze(563);
  assert.equal(out.supported, false);
  assert.equal(out.reason, "not-found");
  assert.ok(!fetchImpl.calls.includes("/api/download/community/ai-passport-9"), "404 时不下载固件");
  console.log("PASS 6: metadata 404 -> reason=not-found, no firmware download");
}

// ---- 7. sha256 不匹配 → reason=format ----
{
  const app = buildAppImage([64]);
  const { fetchImpl } = makeEnv({ app, sha: "0".repeat(64) });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, false);
  assert.equal(out.reason, "format");
  console.log("PASS 7: sha256 mismatch -> reason=format");
}

// ---- 8. 缓存:两次 analyze + 一次 extracted 只下载一次固件 ----
{
  const app = buildAppImage([100]);
  const { fetchImpl } = makeEnv({ app });
  const cache = new Map();
  const a = makeAnalyzer(fetchImpl, cache);
  await a.analyze(563);
  await a.analyze(563);
  await a.extracted(563);
  const dl = fetchImpl.calls.filter((p) => p.startsWith("/api/download/"));
  assert.equal(dl.length, 1, "合并镜像只下载一次");
  console.log("PASS 8: merged image downloaded exactly once across analyze+extracted");
}

// ---- 9. listPartitions / mapExtractError 单元锚点 ----
{
  const app = buildAppImage([64]);
  const full = buildFullImage(app);
  const parts = listPartitions(full);
  assert.equal(parts.length, META_PARTS.length);
  assert.equal(parts[2].label, "factory");
  assert.equal(parts[2].offset, 0x10000);
  assert.equal(mapExtractError(new Error("Wrong chip id 0x2 — expected 0x5")), "wrong-chip");
  assert.equal(mapExtractError(new Error("no factory app partition")), "no-factory");
  assert.equal(mapExtractError(new Error("exceeds max app image size")), "too-large");
  assert.equal(mapExtractError(new Error("Bad magic byte")), "format");
  console.log("PASS 9: listPartitions and mapExtractError anchors");
}

// ---- 10. 仿 play 563 真实布局:含 recovery 应用分区 → 通过 ----
{
  const app = buildAppImage([100, 64]);
  const parts563 = [
    ["nvs", 1, 2, 0x9000, 0x6000],
    ["phy_init", 1, 1, 0xf000, 0x1000],
    ["factory", 0, 0, 0x10000, 0x300000],
    ["otadata", 1, 0, 0x310000, 0x2000],
    ["cardid", 1, 6, 0x356000, 0x4000],
    ["ota_0", 0, 16, 0x360000, 0x300000],
    ["store", 1, 2, 0x660000, 0x4000],
    ["recovery", 0, 32, 0x700000, 0x100000],
  ];
  const { fetchImpl } = makeEnv({ app, parts: parts563 });
  const out = await makeAnalyzer(fetchImpl).analyze(563);
  assert.equal(out.supported, true, "recovery 应用分区不应触发 custom-partitions");
  assert.equal(out.reason, "ok");
  console.log("PASS 10: app-type recovery partition accepted (play 563 layout)");
}

// ---- 11. analyze+extracted 复用同一份已验证字节;revision 变更后缓存失效回源 ----
{
  const app = buildAppImage([100]);
  const merged = buildFullImage(app);
  const digest = sha256Sync(merged);
  let revision = 1279;
  let fwDownloads = 0;
  const fetchImpl = async (url) => {
    const path = new URL(url).pathname;
    if (path === "/api/plays/id/563") {
      return new Response(
        JSON.stringify({
          play: {
            id: 563, revisionId: revision, slug: "ai-passport-9",
            firmwareSize: merged.length, firmwareSha256: digest,
            firmware: { available: true, size: merged.length, sha256: digest, url: "/api/download/community/ai-passport-9" },
          },
        }),
        { status: 200 },
      );
    }
    if (path === "/api/download/community/ai-passport-9") {
      fwDownloads++;
      return new Response(merged, { status: 200 });
    }
    return new Response("not found", { status: 404 });
  };
  const a = makeAnalyzer(fetchImpl);
  const out1 = await a.analyze(563);
  assert.equal(out1.supported, true);
  const ex = await a.extracted(563);
  assert.equal(ex.imageLen, app.length);
  assert.equal(fwDownloads, 1, "analyze+extracted 复用同一份已验证合并镜像,不重复回源");
  revision = 1280;
  const out2 = await a.analyze(563);
  assert.equal(out2.supported, true);
  assert.equal(out2.revisionId, 1280, "revision 变更必须体现在新结果里");
  assert.equal(fwDownloads, 2, "revision 变更后缓存失效,回源重取合并镜像");
  console.log("PASS 11: verified bytes reused across analyze+extracted; revision change re-fetches");
}

// ---- 12. unsupported 玩法的 extracted():拒绝并携带与 analyze 一致的 reason ----
{
  // 使用超过池上限的尺寸测试 too-large
  const poolLimit = POOL_TOTAL - 0x1000;
  const bigLen = poolLimit + 1; // 恰好超池上限
  const app = buildAppImage([bigLen]);
  const { fetchImpl } = makeEnv({ app });
  const a = makeAnalyzer(fetchImpl);
  const out = await a.analyze(563);
  assert.equal(out.reason, "too-large");
  const ex = await a.extracted(563);
  assert.equal(ex.error, "too-large");
  assert.equal(ex.data, undefined, "不支持时不携带镜像字节");
  console.log("PASS 12: extracted() refuses unsupported play with same reason");
}

// ---- 13. 错误路径的可观测性(r10.4):所有 reason=unavailable 都必须携带 ----
// ---- detail,说明是哪一层失败(上游状态码/缺失字段),设备屏上可直接显示 ----
{
  // 13a: 元数据回源 503 → unavailable + detail="upstream 503 (metadata)"。
  {
    const fetchImpl = mockFetch(new Map([
      ["/api/plays/id/563", { json: { oops: true }, status: 503 }],
    ]));
    const a = makeAnalyzer(fetchImpl);
    const out = await a.analyze(563);
    assert.equal(out.ok, false);
    assert.equal(out.reason, "unavailable");
    assert.match(out.detail, /upstream 503/);
    assert.match(out.detail, /metadata/);
  }
  // 13b: 元数据网络异常(fetch reject)→ unavailable + detail="upstream unreachable (metadata)"。
  {
    const fetchImpl = async () => { throw new Error("socket hangup"); };
    const a = makeAnalyzer(fetchImpl);
    const out = await a.analyze(563);
    assert.equal(out.reason, "unavailable");
    assert.match(out.detail, /unreachable/);
    assert.match(out.detail, /metadata/);
  }
  // 13c: 固件字段缺失(fw=null)→ unavailable + detail 指明缺什么。
  {
    const { fetchImpl } = makeEnv({
      app: buildAppImage([1024]),
      metaOver: { firmware: undefined, downloadUrl: undefined, firmwareSize: undefined, firmwareSha256: undefined },
    });
    const a = makeAnalyzer(fetchImpl);
    const out = await a.analyze(563);
    assert.equal(out.reason, "unavailable");
    assert.match(out.detail, /firmware fields/);
  }
  // 13d: firmware.available=false → unavailable + detail="play marked unavailable"。
  {
    const { fetchImpl } = makeEnv({
      app: buildAppImage([1024]),
      metaOver: { firmware: { available: false, size: 1, sha256: "x", url: "/u" } },
    });
    const a = makeAnalyzer(fetchImpl);
    const out = await a.analyze(563);
    assert.equal(out.reason, "unavailable");
    assert.match(out.detail, /marked unavailable/);
  }
  // 13e: 合并镜像回源 503 → unavailable + detail="upstream 503 (image download)"。
  {
    // 正常元数据 + 503 的下载路由。
    const env = makeEnv({ app: buildAppImage([1024]) });
    const fetch3 = async (url) => {
      if (url.includes("/api/plays/id/")) return env.fetchImpl(url);
      return new Response("boom", { status: 503 });
    };
    const b = makeAnalyzer(fetch3);
    const out = await b.analyze(563);
    assert.equal(out.reason, "unavailable");
    assert.match(out.detail, /upstream 503/);
    assert.match(out.detail, /image download/);
  }
  // 13f: size 不匹配 → 仍是 format,但 detail 指明期望/实际。
  {
    const env = makeEnv({ app: buildAppImage([1024]) });
    const wrongSize = env.merged.length + 100;
    const play = JSON.parse(JSON.stringify(env.play));
    play.play.firmware.size = wrongSize;
    play.play.firmwareSize = wrongSize;
    const fetch4 = mockFetch(new Map([
      ["/api/plays/id/563", { json: play }],
      ["/api/download/community/ai-passport-9", { bytes: env.merged }],
    ]));
    const c = makeAnalyzer(fetch4);
    const out = await c.analyze(563);
    assert.equal(out.reason, "format");
    assert.match(out.detail, /size mismatch/);
    assert.ok(out.detail.includes(String(wrongSize)), "detail 必须含期望字节数");
  }
  // 13g: 成功路径不得带 error-detail(避免误导)。
  {
    const { fetchImpl } = makeEnv({ app: buildAppImage([1024]) });
    const a = makeAnalyzer(fetchImpl);
    const out = await a.analyze(563);
    assert.equal(out.ok, true);
    assert.equal(out.detail, undefined);
  }
  console.log("PASS 13: every unavailable/format carries a debug detail (upstream status/phase)");
}

console.log("\nALL store-analyze TESTS PASSED");
