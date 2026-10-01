// install-slot/phone-install.js —— metapass 远程安装模块 v1(设计文档 §4.2/§5/§6)。
//
// 部署形态:Worker 以 /phone-install.js 提供(CORS *,no-store;见 _worker.js 路由),
// 由设备 boot 页(GT /,meta_store_install.c SHELL_HTML)以
// <script type=module src=https://metapass.chuanxilu.net/phone-install.js> 加载。
// 模块自身不编译进启动器、不依赖构建步骤;测试(tests/test_phone_install.mjs)
// 直接 import 本文件,浏览器 <script type=module> 也直接 import 本文件。
//
// 职责(设计文档 §4.2):
//   - 市场搜索/详情/元数据归一(§5:官方 API 无语义 version 字段,展示
//     title.zh|en、firmware.size/sha256、revisionId、updatedAt);
//   - 安装预检(§6.3):analyze → 下载合并镜像 → 商店 SHA-256 校验 →
//     本地解包 factory 镜像 → 本地哈希 → 与 analyze 双比对 → 槽位 fit 表;
//   - 设备会话驱动(§6.4/§6.5):prepare → 轮询物理确认 → session
//     (槽位取设备确认值,手机不得改)→ 顺序 chunk(断点按设备上报 offset
//     续传)→ finalize → 完成态轮询;
//   - 同源 DeviceBridge 是唯一设备写入口(§4.1);token 只来自 URL fragment
//     (#s=...)或配对码兑换,模块不持久化任何设备凭证(§8)。
//
// 信任边界(§3):设备不复查 analyze;本模块被攻破 = 显示元数据与上传镜像
// 可被同时替换(v1 接受;签名 install manifest 是后续加固项)。
//
// crypto.subtle 在 http:// 非 localhost 源(设备 IP 直开)不可用 —— §4.2 要求
// 纯 JS SHA-256:内置实现(与 FIPS 180-4 / 固件 mbedtls 同算法),注入实现
// (setSha256,测试用 node:crypto)优先。

import { extractAppImage, isFullImage } from "./extract-app-image.js";
import { SLOT_GEOMETRY } from "./store-analyze.js";
import { sanitizeDisplayName } from "./name-blob.js";

// ── 常量(与设备端 meta_install_model.h 同契约) ─────────────────────
export const PROTOCOL_V1 = 1;
export const MAX_CHUNK = 65536;          // 设备 META_INSTALL_MAX_CHUNK
const TAIL_SECTOR = 0x1000;              // 槽位尾部 4KB 名字 blob sector
const METAPASS = "https://metapass.chuanxilu.net";
const NAME_MAX = 32;                     // 设备端可显示名上限(META_NAME_LEN)
const CONFIRM_POLL_MS = 1000;            // 物理确认轮询间隔
const CONFIRM_TIMEOUT_MS = 300000;       // 5 分钟无人确认即放弃(设备侧有会话超时)

// ── SHA-256:注入优先,内置纯 JS 兜底(§4.2 sha256.js 职责,单文件合并) ──
let sha256Injected = null;
export function setSha256(fn) { sha256Injected = fn; }

const K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

// 纯 JS SHA-256(输入 Uint8Array,输出 64 位小写 hex)。整数运算全程 |0 保持
// 32 位语义(与 WebCrypto/mbedtls 结果逐字节一致,测试对照 node:crypto 钉死)。
export function sha256Pure(bytes) {
  const l = bytes.length;
  const blocks = Math.ceil((l + 9) / 64);
  const total = blocks * 64;
  const m = new Uint8Array(total);
  m.set(bytes);
  m[l] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(total - 8, Math.floor(l / 0x20000000));        // bit length 高 32 位
  dv.setUint32(total - 4, (l << 3) >>> 0);                    // 低 32 位
  const H = new Int32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  const w = new Int32Array(64);
  const rotr = (x, n) => ((x >>> n) | (x << (32 - n)));
  for (let b = 0; b < blocks; b++) {
    const o = b * 64;
    for (let i = 0; i < 16; i++) w[i] = dv.getInt32(o + i * 4);
    for (let i = 16; i < 64; i++) {
      const x = w[i - 15], y = w[i - 2];
      const s0 = rotr(x, 7) ^ rotr(x, 18) ^ (x >>> 3);
      const s1 = rotr(y, 17) ^ rotr(y, 19) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = H[0], b2 = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + K[i] + w[i]) | 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b2) ^ (a & c) ^ (b2 & c);
      const t2 = (S0 + maj) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0;
      d = c; c = b2; b2 = a; a = (t1 + t2) | 0;
    }
    H[0] = (H[0] + a) | 0; H[1] = (H[1] + b2) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
    H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
  }
  let out = "";
  for (let i = 0; i < 8; i++) out += (H[i] >>> 0).toString(16).padStart(8, "0");
  return out;
}

export async function sha256Hex(u8) {
  if (sha256Injected) return sha256Injected(u8);
  return sha256Pure(u8);
}

// ── 设备环境解析(§2:token 在 fragment,不随首个 GET 上行) ────────────
// 设备页 origin = http://<device-ip>(非 metapass 域的 http 源)。
// 在 metapass 页面/https 源打开时,需显式给出设备地址(MVP:手动输入)。
export function detectToken() {
  if (typeof location === "undefined") return "";
  return new URLSearchParams(location.hash.slice(1)).get("s") || "";
}

function looksLikeDeviceOrigin(origin) {
  return typeof origin === "string" &&
    origin.startsWith("http://") &&
    !origin.includes("metapass.chuanxilu.net") &&
    !origin.includes("localhost") && !origin.includes("127.0.0.1");
}

export function detectDeviceOrigin() {
  if (typeof location === "undefined") return null;
  return looksLikeDeviceOrigin(location.origin) ? location.origin : null;
}

// bridge 工厂:token + device origin → DeviceBridge 形状(§4.1 同源六个方法)。
// 设备 API 无 CORS 头(§8),跨源调用仅限同源场景;跨源(手动 IP)时仍尝试,
// 由浏览器策略决定成败(真机路径永远是同源,这条只服务高级用户)。
export function createBridge(deviceOrigin, token) {
  const base = (deviceOrigin || "").replace(/\/$/, "");
  async function call(path, opt = {}) {
    const headers = { ...(opt.headers || {}) };
    if (token) headers["X-Meta-Session"] = token;
    if (opt.json !== undefined) {
      headers["Content-Type"] = "application/json";
      opt = { ...opt, body: JSON.stringify(opt.json) };
    }
    let resp;
    try {
      resp = await fetch(base + path, { ...opt, headers });
    } catch (e) {
      return { ok: false, status: 0, text: `network: ${e && e.message ? e.message : e}` };
    }
    let text = "";
    try { text = await resp.text(); } catch { /* empty body */ }
    return { ok: resp.ok, status: resp.status, text };
  }
  return {
    token: () => token || "",
    status: () => call("/api/install/status"),
    prepare: (o) => call("/api/install/prepare", { method: "POST", json: o }),
    session: (o) => call("/api/install/session", { method: "POST", json: o }),
    chunk: (off, buf) => call("/api/install/chunk", {
      method: "POST",
      headers: { "X-Meta-Offset": String(off), "Content-Type": "application/octet-stream" },
      body: buf,
    }),
    finalize: () => call("/api/install/finalize", { method: "POST" }),
    cancel: () => call("/api/install/cancel", { method: "POST" }),
  };
}

// ── metapass 客户端(§5 元数据契约) ──────────────────────────────────
export function normalizePlay(p) {
  if (!p || typeof p !== "object" || !Number.isFinite(p.id)) return null;
  const t = p.title ?? {};
  const fw = p.firmware ?? {};
  const url = fw.url ?? p.downloadUrl;
  const size = fw.size ?? p.firmwareSize;
  const sha = fw.sha256 ?? p.firmwareSha256;
  // 无固件元数据 = 不可安装(§11:必须拒装而非隐藏)。
  if (typeof url !== "string" || !url.startsWith("/api/download/")) return null;
  if (!Number.isFinite(size) || size <= 0) return null;
  if (typeof sha !== "string" || !/^[0-9a-fA-F]{64}$/.test(sha)) return null;
  const name = (typeof t === "object" ? (t.zh || t.en || "") : String(t ?? "")) ||
    p.slug || `play ${p.id}`;
  return {
    id: p.id,
    revisionId: Number.isFinite(p.revisionId) ? p.revisionId : null,
    name: name.trim(),
    enName: (typeof t === "object" && typeof t.en === "string") ? t.en.trim() : "",
    size,
    sha256: sha.toLowerCase(),
    downloadUrl: url,
    updatedAt: p.updatedAt ?? null,
    category: typeof p.category === "string" ? p.category : null,
  };
}

async function mpJson(path) {
  let r;
  try {
    r = await fetch(METAPASS + path, { redirect: "follow" });
  } catch (e) {
    throw new Error(`metapass unreachable: ${e && e.message ? e.message : e}`);
  }
  if (!r.ok) throw new Error(`metapass ${r.status} on ${path}`);
  return r.json();
}

export async function searchPlays(q, offset = 0, limit = 20, category = "", tag = "") {
  // 官方上游分页契约(生产实测):只认 offset+limit(page/pageSize 被静默忽略),
  // 响应 pagination.{total,hasMore};limit=20 ≈ 100KB/页,limit>=100 上游 502。
  // category= 官方分类过滤(实测生效);tag= 官方标签过滤(实测生效:
  // discoveryTags 的键,如 multiplayer)。category 与 tag 互斥使用。
  const cat = category ? `&category=${encodeURIComponent(category)}` : "";
  const tg = tag ? `&tag=${encodeURIComponent(tag)}` : "";
  const d = await mpJson(`/api/plays?multiDevice=false&q=${encodeURIComponent(q)}&offset=${offset}&limit=${limit}${cat}${tg}`);
  const plays = Array.isArray(d?.plays) ? d.plays : [];
  const pagination = d?.pagination ?? {};
  return {
    list: plays.map(normalizePlay).filter((p) => p),
    hasMore: pagination.hasMore === true,
    total: Number.isFinite(pagination.total) ? pagination.total : null,
    categoryCounts: (d?.categoryCounts && typeof d.categoryCounts === "object")
      ? d.categoryCounts : null,
    discoveryTags: Array.isArray(d?.discoveryTags) ? d.discoveryTags : null,
  };
}

export async function getPlayDetail(id) {
  // Worker 路由表只有 /api/plays(列表)与 /api/play?id=(详情,代理到上游
  // /api/plays/id/<id>);手机直接打 /api/plays/id/<id> 会落到 ASSETS 404
  // (生产实测,审计 B2)。
  const d = await mpJson(`/api/play?id=${encodeURIComponent(id)}`);
  return normalizePlay(d?.play);
}

// ── 安装预检(§6.3,两阶段) ───────────────────────────────────────────
// 交互 v2:点 Install 先进 preflightMeta(轻:analyze+详情,亚秒级),手机
// 弹出槽位选择器;用户选槽确认后才跑 prepareImage(重:数 MB 下载/校验/剥离)。
// preflight(id) 保留为两阶段连跑的兼容入口(测试与旧调用)。
// 轻量预检:analyze 与详情互相独立,并行发起 —— 点安装到出槽位抽屉的
// 延迟 ≈ 两者较慢者;串行则叠加(真机反馈:点安装响应慢的一半来源)。
export async function preflightMeta(id, hooks = {}) {
  const stage = (s) => hooks.stage?.(s);
  stage("analyze");
  const [aRes, play] = await Promise.all([
    mpJson(`/api/analyze?id=${encodeURIComponent(id)}`),
    getPlayDetail(id),
  ]);
  const a = aRes;
  if (!a || a.supported !== true) {
    return { ok: false, stage: "analyze", reason: a?.reason ?? "unsupported", detail: a?.detail ?? null };
  }
  if (!play) return { ok: false, stage: "market", reason: "play metadata missing firmware fields" };
  return { ok: true, analyze: a, play };
}

// 设备显示名链(真机问题:社区固件无 MNAM,analyze 名退回 slug
// "community-xxxx";而手机侧受 MNAM ≤32 可打印 ASCII 契约限制,中文标题被
// 剥空,于是满屏 community- 名)。规则:MNAM 真名 > 英文标题 > slug >
// 中文标题(sanitize 剥空自动跳过)> play <id>。slug-ish(community- 前缀,
// 即 analyze 的 slug 兜底)降级到英文标题之后。
const slugish = (s) => !s || /^community-/i.test(s);
export function displayNameFor(analyzeName, play, userName = "") {
  // 兜底 slug 剥掉 "community-" 前缀(纯装饰,无信息);真无更好候选时短一点
  // 是一点。设备契约仍是 ≤32 可打印 ASCII —— 中文标题在设备端存不了,
  // 这是 MNAM 契约决定,不是取不到(网页版只给自己看,无此约束)。
  const slugClean = (analyzeName || "").replace(/^community-/i, "");
  return pickDisplayName(
    userName || null,
    slugish(analyzeName) ? "" : analyzeName,
    play?.enName || "",
    slugClean || null,
    play?.name,
    play?.id,
  );
}

// meta = preflightMeta 结果;slot = 用户选定槽位(>=0;交互 v2 设备直确认,
// -1 = 旧流程设备物理确认);userName = 用户在安装页改写的显示名(可空,
// 空则回退 analyze/商店名链)。
export async function prepareImage(meta, slot, hooks = {}, userName = "") {
  const stage = (s) => hooks.stage?.(s);
  const { analyze: a, play } = meta;
  stage("下载固件");
  let merged;
  try {
    const r = await fetch(`${METAPASS}/api/firmware?path=${encodeURIComponent(play.downloadUrl)}`,
      { redirect: "follow" });
    if (!r.ok) return { ok: false, stage: "download", reason: `firmware download ${r.status}` };
    merged = new Uint8Array(await r.arrayBuffer());
  } catch (e) {
    return { ok: false, stage: "download", reason: `firmware fetch: ${e && e.message ? e.message : e}` };
  }
  if (merged.length !== play.size) {
    return { ok: false, stage: "download",
             reason: `size mismatch: store=${play.size} got=${merged.length}` };
  }

  stage("校验完整性");
  const digest = await sha256Hex(merged);
  if (digest !== play.sha256) {
    return { ok: false, stage: "verify", reason: "store sha256 mismatch", got: digest };
  }
  if (!isFullImage(merged)) {
    return { ok: false, stage: "extract", reason: "not an esp merged image" };
  }

  stage("解包");
  let ext;
  try {
    // 解包上限 = 最大槽位上限(0x29E000-4KB=2740224)。extractAppImage
    // 的默认上限是硬编码 2MB 槽(2093056)——用它解 2.2MB 应用必炸,
    // 而 fit 表按真实槽几何说 slot2 可装:两条判断逻辑不一致(真机 bug:
    // 检查推荐 slot2,安装报"最大 2093056";根因在此,不在任何 fit 判定)。
    const maxSlotLimit = Math.max(...SLOT_GEOMETRY.map((s) => s.partSize)) - TAIL_SECTOR;
    ext = extractAppImage(merged, maxSlotLimit);
  } catch (e) {
    return { ok: false, stage: "extract", reason: String(e && e.message ? e.message : e) };
  }

  stage("本地复核");
  const appSha = await sha256Hex(ext.data);
  // §6.3.7:本地解包长度/哈希必须与 analyze 一致,不一致即拒绝(不信任半截数据)。
  if (!a.extracted || ext.length !== a.extracted.imageLen || appSha !== a.extracted.sha256) {
    return { ok: false, stage: "preflight", reason: "extracted mismatch vs analyze",
             got: { imageLen: ext.length, sha256: appSha },
             analyze: a.extracted };
  }

  // 槽位 fit 表:仓库单一事实源(store-analyze.js SLOT_GEOMETRY)− 4KB 尾 sector。
  const slots = SLOT_GEOMETRY.map(({ slot: s, partSize }) => {
    const limit = partSize - TAIL_SECTOR;
    return { slot: s, limit, fit: ext.length <= limit };
  });
  const fitSlots = slots.filter((s) => s.fit);
  if (fitSlots.length === 0) {
    return { ok: false, stage: "preflight", reason: "image larger than every slot" };
  }
  if (slot >= 0 && !fitSlots.some((s) => s.slot === slot)) {
    return { ok: false, stage: "preflight", reason: `chosen slot ${slot} does not fit` };
  }
  const suggestedSlot = fitSlots.some((s) => s.slot === a.suggestedSlot)
    ? a.suggestedSlot : fitSlots[0].slot;

  const offer = {
    protocol: PROTOCOL_V1,
    playId: play.id,
    revisionId: a.revisionId ?? play.revisionId ?? 0,
    name: displayNameFor(a.name, play, userName),
    storeSha256: play.sha256,
    imageLen: ext.length,
    sha256: appSha,
    suggestedSlot,
    slot,               // 交互 v2:手机选定槽位;-1 = 设备物理确认
    slots,
    reason: typeof a.reason === "string" && a.reason ? a.reason : "ok",
  };
  return { ok: true, offer, merged, ext, analyze: a, play };
}

export async function preflight(id, hooks = {}) {
  const meta = await preflightMeta(id, hooks);
  if (!meta.ok) return meta;
  return prepareImage(meta, -1, hooks);
}

// 设备端 name 契约:≤32 可打印 ASCII。逐级回退,sanitize 后为空再降级,
// 最终兜底 `play <id>`(纯 ASCII 恒可用)。
export function pickDisplayName(...candidates) {
  for (const c of candidates) {
    if (typeof c !== "string" || !c) continue;
    const s = sanitizeDisplayName(c);
    if (s && s.length <= NAME_MAX) return s;
  }
  const fallback = candidates.find((c) => Number.isFinite(c));
  return `play ${fallback ?? 0}`;
}

// ── 设备会话驱动(§6.4 确认 + §6.5 上传) ─────────────────────────────
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseJsonReply(r) {
  try { return JSON.parse(r.text); } catch { return null; }
}

async function pollUntil(bridge, pred, hooks, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const s = parseJsonReply(await bridge.status());
    if (!s) throw new Error("device status unreadable");
    hooks.status?.(s);
    if (pred(s)) return s;
    if (Date.now() > deadline) return { ...s, state: "timeout" };
    await sleep(CONFIRM_POLL_MS);
  }
}

// 全流程:prepare → 等物理确认 → session(槽位 = 设备确认值)→ 顺序 chunk
// (失败按设备上报 offset 续传)→ finalize → 等终态。appImage 是解包后的
// factory 应用镜像(preflight 返回的 ext.data;§9:只有 extracted 镜像过 LAN,
// 长度必须等于 offer.imageLen)。hooks:
//   stage(name) / status(snapshot) / progress(offset, total) / resume(offset)
//   confirmTimeoutMs(可选,默认 300000;host 测试用短值)
// 返回 {ok:true, slot} 或 {ok:false, stage, reason, ...}。
export async function runInstall(bridge, offer, appImage, hooks = {}) {
  const fail = (stage, reason, extra = {}) => ({ ok: false, stage, reason, ...extra });

  if (!bridge.token()) {
    return fail("token", "missing session token — open the device QR URL (or pair by code)");
  }
  // 兼容握手(§4.1:loader 必须先做协议兼容检查才允许 prepare)。
  let st;
  try {
    st = parseJsonReply(await bridge.status());
  } catch (e) {
    return fail("status", String(e && e.message ? e.message : e));
  }
  if (!st) return fail("status", "device status unreadable");
  if (st.protocol !== PROTOCOL_V1) {
    return fail("status", `device protocol ${st.protocol}, module expects ${PROTOCOL_V1}`);
  }
  if (st.confirmed || st.session) {
    return fail("status", "device busy with another offer — cancel it there first");
  }

  hooks.stage?.("prepare");
  const pr = await bridge.prepare(offer);
  if (!pr.ok) return fail("prepare", pr.text || `HTTP ${pr.status}`, { status: pr.status });

  hooks.stage?.("confirm");
  hooks.status?.(st);
  let c;
  try {
    c = await pollUntil(bridge,
      (s) => s.confirmed || s.state === "failed" || s.state === "cancelled" || s.state === "timeout",
      hooks, Number(hooks.confirmTimeoutMs) || CONFIRM_TIMEOUT_MS);
  } catch (e) {
    return fail("confirm", String(e && e.message ? e.message : e));
  }
  if (!c.confirmed) {
    return fail("confirm", c.state === "timeout" ? "no confirmation on device (timed out)"
      : `device state: ${c.state}${c.message ? ` (${c.message})` : ""}`);
  }
  const slot = c.slot;
  // 槽位只能由设备物理确认产生(§6.4:手机不得改最终 slot)。
  if (!(slot >= 0 && slot <= 2)) return fail("confirm", `device confirmed bad slot ${slot}`);

  hooks.stage?.("session");
  const se = await bridge.session({ imageLen: offer.imageLen, sha256: offer.sha256, slot });
  if (!se.ok) return fail("session", se.text || `HTTP ${se.status}`, { status: se.status });
  const sess = parseJsonReply(se);
  let offset = Number.isFinite(sess?.offset) ? sess.offset : 0;
  const maxChunk = Math.min(Number(sess?.maxChunk) || MAX_CHUNK, MAX_CHUNK);
  // offset == imageLen:上次上传已完成但 finalize 未发出(§6.5 设备 offset 为
  // 事实的续传场景),跳过上传直接 finalize;只有 > 才算坏状态(审计 M2)。
  if (offset > offer.imageLen) {
    return fail("session", `device offset ${offset} already beyond image length`);
  }

  // 上传体是解包后的 app 镜像:Uint8Array 或 {data}(extractAppImage 形态)。
  const upload = appImage instanceof Uint8Array ? appImage : appImage?.data;
  if (!(upload instanceof Uint8Array) || upload.length !== offer.imageLen) {
    return fail("upload", "upload buffer missing/length mismatch");
  }

  hooks.stage?.("upload");
  let retries = 0;                       // 同 offset 连续失败计数(设备原地拒绝 = 瞬时
                                         // 失败,按 §6.5 retry semantics 重试)
  while (offset < offer.imageLen) {
    const len = Math.min(maxChunk, offer.imageLen - offset);
    const r = await bridge.chunk(offset, upload.subarray(offset, offset + len));
    if (r.ok) {
      offset += len;
      retries = 0;
      hooks.progress?.(offset, offer.imageLen);
      continue;
    }
    // 失败重同步:设备 offset 是唯一事实(§6.5 retry starts at device-reported
    // offset)。会话已坏(state failed/cancelled/done 或 session 关闭)→ 终止;
    // 设备 offset 前进了(半块落盘)→ 从新 offset 续传;原地未动 → 瞬时失败,
    // 重试同一 offset,连续 3 次无进展才放弃(退避交给调用方/网络层)。
    let s2 = null;
    try { s2 = parseJsonReply(await bridge.status()); } catch { /* fatal below */ }
    const devOff = Number.isFinite(s2?.offset) ? s2.offset : null;
    const dead = s2 && (s2.state === "failed" || s2.state === "cancelled" ||
                        s2.state === "done" || !s2.session);
    if (dead || devOff === null) {
      return fail("upload", r.text || `HTTP ${r.status}`,
        { status: r.status, offset, deviceOffset: devOff });
    }
    if (devOff === offset) {
      if (++retries >= 3) {
        return fail("upload", r.text || `HTTP ${r.status}`,
          { status: r.status, offset, deviceOffset: devOff, retries });
      }
      continue;
    }
    offset = devOff;
    retries = 0;
    hooks.resume?.(offset);
    hooks.progress?.(offset, offer.imageLen);
  }

  hooks.stage?.("finalize");
  const fin = await bridge.finalize();
  if (!fin.ok) {
    let msg = fin.text || `HTTP ${fin.status}`;
    try {
      const s = parseJsonReply(await bridge.status());
      if (s?.message) msg = s.message;
    } catch { /* keep HTTP text */ }
    return fail("finalize", msg, { status: fin.status });
  }

  hooks.stage?.("done");
  // 完成轮询超时/不可读 ≠ 成功(审计 M1):finalize 已被接受但设备未到 done
  // (或 status 失联)时如实报错,让用户去设备上核对,而不是报成功。
  const doneState = await pollUntil(bridge, (s) => s.state === "done" || s.state === "failed",
    hooks, Number(hooks.doneTimeoutMs) || 30000).catch(() => null);
  if (!doneState || doneState.state !== "done") {
    return fail("finalize",
      doneState?.state === "failed"
        ? (doneState.message || "device reported failure")
        : "device did not reach done (finalize accepted but no confirmation)");
  }
  return { ok: true, slot };
}

// ── 手机安装 UI(设备 launcher 同款 pixel 世界:ink/paper/sky/grass) ─────
// 设计依据 impeccable(Operate 模式):工具消失进任务;accent 只给主动作;
// 每个交互件六态俱全(default/hover/focus/active/disabled/loading/error);
// 空态教学;错误命名问题+恢复路径;动效 150ms 只传达状态;无 emoji 图标,
// chevron 用作者 SVG 单一笔画;对比度正文 ≥4.5:1。
const MP_STYLE = `
#mp-install-root{--paper:#F4F4EA;--card:#FFFDF6;--ink:#17202A;--ink2:#45566B;
--line:rgba(23,32,42,.16);--sky:#1689E8;--sky-dark:#0872C9;--grass:#82BE2D;
--grass-dark:#4E8A19;--red:#C02B20;--r:6px;
font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC","Noto Sans SC",sans-serif;
color:var(--ink);background:var(--paper);max-width:min(30em,100%);margin:0 auto;
padding:20px 16px 56px;-webkit-text-size-adjust:100%}
/* iOS:内容撑宽文档时 layout viewport 变宽,position:fixed 浮层随之超屏(真机
   bug:浮层宽于手机屏幕,捏合缩放才恢复)。根上防撑宽 + 双保险裁溢出。 */
html,body{max-width:100%;overflow-x:hidden}
#mp-install-root *,#mp-install-root *::before,#mp-install-root *::after{box-sizing:border-box}
#mp-install-root ::selection{background:var(--ink);color:var(--paper)}
#mp-install-root :focus-visible{outline:2px solid var(--sky-dark);outline-offset:2px}
#mp-install-root button{font:inherit}   /* 不设 color:ID 优先级会压过 .mp-btn 的 paper 文字色 */
#mp-install-root button:disabled{opacity:.42;cursor:not-allowed}
.mp-wordmark{display:flex;align-items:center;gap:9px;margin:2px 0 2px}
.mp-wordmark b{font-size:18px;font-weight:700;letter-spacing:-.01em}
.mp-dot{flex:none;width:9px;height:9px;border-radius:50%;background:var(--ink2)}
.mp-dot.on{background:var(--grass-dark)}
/* 顶部引导区(随滚动离开,不占固定层):左操作步骤,右咖啡码。 */
.mp-hero{display:flex;gap:14px;align-items:flex-start;margin:12px 0 2px}
.mp-hero .steps{flex:1;margin:2px 0 0;font-size:12.5px;line-height:1.75;color:var(--ink2)}
.mp-hero .steps b{color:var(--ink);font-weight:600}
.mp-coffee{flex:none;margin:0;padding:0;background:none;border:0;cursor:pointer;text-align:center}
.mp-coffee img{display:block;width:84px;height:84px;object-fit:cover;border:2px solid var(--ink);border-radius:var(--r)}
.mp-coffee span{display:block;margin-top:4px;font-size:11px;color:var(--ink2)}
.mp-coffee-lg{text-align:center}
.mp-coffee-lg img{width:min(280px,78vw);height:auto;border:2px solid var(--ink);border-radius:var(--r)}
.mp-coffee-lg p{margin:10px 0 14px;font-size:14px}
/* 回到顶部浮层:右下角,滚动后现身。 */
.mp-top{position:fixed;right:16px;bottom:calc(20px + env(safe-area-inset-bottom));z-index:40;
width:44px;height:44px;display:none;align-items:center;justify-content:center;
border:2px solid var(--ink);border-radius:var(--r);background:var(--card);color:var(--ink);cursor:pointer}
.mp-top.show{display:flex}
.mp-sub{margin:0 0 14px;font-size:12.5px;color:var(--ink2)}   /* 抽屉/详情内小注(页头重复信息已移除) */
.mp-status{margin:12px 2px 0;font-size:13px;color:var(--ink2);min-height:1.5em}
.mp-status.err{color:var(--red)}
.mp-status.ok{color:var(--grass-dark)}
.mp-search{display:flex;gap:8px}
.mp-search input{flex:1;min-width:0;font:inherit;padding:10px 12px;border:2px solid var(--ink);
border-radius:var(--r);background:var(--card);color:var(--ink);caret-color:var(--sky-dark)}
.mp-search input::placeholder{color:var(--ink2)}
.mp-btn{padding:10px 16px;border:2px solid var(--ink);border-radius:var(--r);
background:var(--ink);color:var(--paper);font-weight:600;line-height:1.2}
.mp-btn:not(:disabled):active{transform:translateY(1px)}
.mp-btn.ghost{background:var(--card);color:var(--ink)}
.mp-list{list-style:none;margin:14px 0 0;padding:0;border-top:1px solid var(--line)}
.mp-row{display:flex;align-items:center;gap:10px;width:100%;padding:12px 4px;
background:none;border:0;border-bottom:1px solid var(--line);text-align:left;cursor:pointer}
.mp-row .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:500}
.mp-row .sz{font-size:12.5px;color:var(--ink2);font-variant-numeric:tabular-nums}
.mp-row:hover .nm{color:var(--sky-dark)}
.mp-row:active{background:rgba(23,32,42,.06)}
.mp-empty{margin:22px 4px;padding:18px 16px;border:2px dashed var(--line);border-radius:var(--r);
font-size:13.5px;color:var(--ink2)}
.mp-count{margin:10px 2px 0;font-size:12.5px;color:var(--ink2);font-variant-numeric:tabular-nums}
.mp-more{display:block;width:100%;margin-top:10px}
.mp-panel{border:2px solid var(--ink);border-radius:var(--r);background:var(--card);
padding:16px;margin-top:14px;animation:mp-in .18s ease-out}
.mp-panel-slot{position:relative}   /* 保留 DOM 位置占位,内容由浮层呈现 */
@keyframes mp-in{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.mp-overlay{position:fixed;inset:0;z-index:50;background:rgba(23,32,42,.48);
display:flex;align-items:flex-end;justify-content:center;animation:mp-fade .15s ease-out}
@keyframes mp-fade{from{opacity:0}}
.mp-sheet{background:var(--card);border:2px solid var(--ink);border-bottom:0;
border-radius:12px 12px 0 0;width:100%;max-width:min(30em,100vw);
box-sizing:border-box;max-height:84vh;
overflow-y:auto;padding:18px 16px calc(24px + env(safe-area-inset-bottom));
animation:mp-sheet .18s ease-out}
@media(min-width:520px){
.mp-overlay{align-items:center;padding:16px}
.mp-sheet{border-radius:var(--r);border-bottom:2px solid var(--ink);max-height:80vh}}
@keyframes mp-sheet{from{transform:translateY(24px)}to{transform:none}}
.mp-sheet .mp-panel{border:0;padding:0;margin-top:0;animation:none}   /* 面板进浮层后去重边框 */
.mp-panel h4{margin:0 0 4px;font-size:16px;letter-spacing:-.01em}
.mp-meta{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;margin:10px 0 14px;
font-size:13px}
.mp-meta dt{color:var(--ink2)}
.mp-meta dd{margin:0;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.mp-slots{display:flex;flex-direction:column;gap:8px;margin:12px 0}
.mp-slot{display:flex;align-items:center;gap:10px;width:100%;padding:10px 12px;
border:2px solid var(--line);border-radius:var(--r);background:var(--paper);
color:var(--ink);text-align:left;cursor:pointer}
.mp-slot .rd{flex:none;width:18px;height:18px;border:2px solid var(--ink);border-radius:50%;position:relative}
.mp-slot[aria-checked="true"]{border-color:var(--ink);background:var(--card)}
.mp-slot[aria-checked="true"] .rd::after{content:"";position:absolute;inset:3px;
border-radius:50%;background:var(--ink)}
.mp-slot .lb{font-weight:600}
.mp-slot .rec{flex:none;font-size:11px;font-weight:700;background:var(--grass);
color:var(--ink);padding:2px 7px;border-radius:999px}
.mp-slot .cap{margin-left:auto;font-size:12.5px;color:var(--ink2);font-variant-numeric:tabular-nums}
.mp-slot[disabled]{opacity:.45;cursor:not-allowed}
.mp-actions{display:flex;gap:8px;margin-top:4px}
.mp-actions .mp-btn{flex:1}
.mp-prog{margin:12px 0 4px}
.mp-prog progress{width:100%;height:14px;border:2px solid var(--ink);border-radius:var(--r);
background:var(--paper);overflow:hidden}
.mp-prog progress::-webkit-progress-bar{background:var(--paper)}
.mp-prog progress::-webkit-progress-value{background:var(--ink)}
.mp-prog progress::-moz-progress-bar{background:var(--ink)}
.mp-prog .lb{display:flex;justify-content:space-between;margin-top:6px;font-size:12.5px;
color:var(--ink2);font-variant-numeric:tabular-nums}
`;

const MP_CHEVRON = '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M6 3.5 10.5 8 6 12.5" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>';
const esc = (s) => String(s).replace(/[&<>"']/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const fmtMB = (n) => `${(n / 1048576).toFixed(1)} MB`;

export function boot(opts = {}) {
  if (typeof document === "undefined") throw new Error("boot() requires a browser document");
  const deviceOrigin = opts.deviceOrigin ?? detectDeviceOrigin();
  let token = opts.token ?? detectToken();
  let bridge = createBridge(deviceOrigin, token);

  if (!document.getElementById("mp-style")) {
    const st = document.createElement("style");
    st.id = "mp-style";
    st.textContent = MP_STYLE;
    document.head.appendChild(st);
  }

  const root = document.createElement("div");
  root.id = "mp-install-root";
  root.innerHTML = `
    <header class=mp-wordmark><span id=mp-dot class="mp-dot${token ? " on" : ""}"></span><b>meta-pass</b></header>
    <div class=mp-hero>
      <p class=steps><b>①</b> 搜索/浏览玩法,点条目查看详情和安装<br><b>②</b> 选槽、可改名,点「确认安装」<br><b>③</b> 安装期间请保持本页与设备商店页(SCAN ME)常驻,勿退出</p>
      <button class=mp-coffee id=mp-coffee aria-label="请作者喝咖啡">
        <img src="${METAPASS}/author-coffee.jpg" alt="请作者喝咖啡" width=84 height=84 loading=lazy>
        <span>请作者喝咖啡</span>
      </button>
    </div>
    <div id=mp-dev-row class=mp-search style="display:${deviceOrigin ? "none" : "flex"}">
      <input id=mp-dev placeholder="http://192.168.x.x" inputmode=url>
      <button id=mp-dev-go class=mp-btn>Connect</button>
    </div>
    <div class=mp-search>
      <input id=mp-q placeholder="搜索玩法,输入 ID 直达;留空浏览全部" enterkeyhint=search>
      <button id=mp-go class=mp-btn>搜索</button>
    </div>
    <p id=mp-status class=mp-status role=status></p>
    <ul id=mp-list class=mp-list></ul>
    <div id=mp-empty class=mp-empty style="display:none">
      没有可安装的玩法。换个关键词,或清空输入框浏览全部。</div>
    <div id=mp-count class=mp-count></div>
    <button id=mp-more class="mp-btn ghost mp-more" style="display:none">加载更多</button>
    <div id=mp-panel></div>
    <button id=mp-top class=mp-top aria-label="回到顶部" title="回到顶部">
      <svg width="20" height="20" viewBox="0 0 20 20" fill="none" aria-hidden="true">
        <path d="M4 15.5h12M10 12V4m0 0L5.5 8.5M10 4l4.5 4.5" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
    </button>`;
  (opts.mount ?? document.body).appendChild(root);
  const $ = (id) => root.querySelector(`#${id}`);
  // 壳页(SHELL_HTML)的 "meta-pass install" 标题与 "device IP · session" 行
  // 与模块 UI 重复(真机反馈两次):挂载后隐藏壳页头,保留配对区与 noscript。
  if (typeof document !== "undefined") {
    for (const sel of ["body > h3", "body > #st"]) {
      const el = document.querySelector(sel);
      if (el) el.style.display = "none";
    }
  }
  // 咖啡码:点击放大成浮层(手机扫码需要近距离)。
  $("mp-coffee").onclick = () => {
    setPanel(`<section class="mp-panel mp-coffee-lg">
      <img src="${METAPASS}/author-coffee.jpg" alt="请作者喝咖啡">
      <p>请作者喝咖啡</p>
      <button id=mp-coffee-x class=mp-btn style="width:100%">关闭</button>
    </section>`);
    $("mp-coffee-x").onclick = clearPanel;
  };
  // 回到顶部浮层:滚动超过一屏后现身。
  const topBtn = $("mp-top");
  const onScroll = () => topBtn.classList.toggle("show",
    (window.scrollY || document.documentElement.scrollTop || 0) > 480);
  if (typeof window !== "undefined") {
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }
  topBtn.onclick = () => {
    (window.scrollTo || (() => {})).call(window, { top: 0, behavior: "smooth" });
  };
  const statusEl = $("mp-status");
  const log = (msg, cls = "") => {
    statusEl.textContent = msg;
    statusEl.className = `mp-status ${cls}`.trim();
  };

  if (!deviceOrigin) {
    log("输入设备地址(如 http://192.168.1.23)后 Connect");
    $("mp-dev-go").onclick = () => {
      const o = $("mp-dev").value.trim().replace(/\/$/, "");
      if (!/^http:\/\/\d+\.\d+\.\d+\.\d+$/.test(o)) { log("地址格式: http://192.168.1.23", "err"); return; }
      bridge = createBridge(o, token);
      $("mp-dev-row").style.display = "none";
      $("mp-dot").classList.add("on");
      log(token ? "已连接(token 来自链接)" : "已连接,但无 token —— 先在设备页输入配对码", token ? "ok" : "err");
    };
  } else if (!token) {
    log("链接里没有会话 token —— 先在设备页输入 6 位配对码,页面会自动刷新", "err");
  } else {
    log("已连接,正在加载玩法目录…");
  }

  // 分页浏览状态机(官方契约:offset+limit,pagination.{total,hasMore})。
  // panelOpen:详情/槽位面板展开期间挂起自动加载 —— 面板在列表下方,
  // 不挂起的话"加载更多"按钮因面板出现而进入视区,IO 无限加载把面板
  // 越顶越远,永远够不到确认键(真机反馈)。
  // 分类一级菜单(官方 category= 过滤)+ 标签 chips(官方 tag= 过滤,
  // discoveryTags 键如 multiplayer —— 生产实测生效)+ 搜索,三者视图独立。
  //   首屏 = 分类菜单(全部 + 各分类,带数量);点分类进二级玩法列表(顶部
  //   「返回分类」);chips 标签行保留原布局,点击以 tag 过滤进二级;
  //   搜索独立,返回后回菜单。
  let lastQ = "";
  let lastCat = "";
  let lastTag = "";            // discoveryTags 键;与 category 互斥
  let catTags = null;          // categoryCounts(分类目录:键 → 数量)
  let discTags = null;         // discoveryTags(标签目录:键 + 中英文名)
  let menuMode = true;
  let items = [];
  let hasMore = false;
  let total = null;
  let loading = false;
  let panelOpen = false;
  const seen = new Set();

  // 分类 chips 数据源:官方 categoryCounts 键的中文名(未知键回退原名)。
  const CATEGORY_NAMES = {
    "games": "游戏", "learning": "学习", "information": "资讯",
    "productivity": "效率工具", "media": "媒体",
    "social": "社交", "developer": "开发者",
  };
  // 标签 chips 数据源:discoveryTags(官方标签目录)。兜底仅用于首帧未拿到
  // 响应时(与生产实测的 enabled 标签对齐;未知键回退原名)。
  const FALLBACK_DISC = [
    ["must-play", "必玩精选"], ["multiplayer", "多人玩法"],
    ["child-friendly", "亲子益智"], ["family", "亲子同乐"],
  ];
  function tagList() {           // 标签 chips
    if (discTags && discTags.length) {
      return discTags.filter((t) => t && typeof t.key === "string" && t.enabled !== false)
        .sort((a, b) => (a.sortOrder ?? 99) - (b.sortOrder ?? 99))
        .map((t) => [t.key, t.name?.zh || t.name?.en || t.key]);
    }
    return FALLBACK_DISC;
  }
  function catList() {           // 分类菜单
    const keys = (catTags && Object.keys(catTags).length)
      ? Object.keys(catTags)
      : Object.keys(CATEGORY_NAMES);
    // 上游黑名单(生产逐键实测 502):这三个伪分类键会把上游打崩,不出。
    const blocked = new Set(["child-friendly", "multi-device", "must-play"]);
    return keys.filter((k) => !blocked.has(k))
      .map((k) => [k, CATEGORY_NAMES[k] || k]);
  }

  function renderCatChips() {
    let host = $("mp-cats");
    if (!host) {
      host = document.createElement("div");
      host.id = "mp-cats";
      host.style.cssText = "display:flex;gap:8px;overflow-x:auto;margin:12px -16px 0;padding:0 16px;scrollbar-width:none";
      $("mp-list").before(host);
    }
    host.innerHTML = "";
    const mk = (key, label) => {
      const b = document.createElement("button");
      b.className = "mp-btn" + (key === lastCat ? "" : " ghost");
      b.style.cssText = "flex:none;padding:6px 12px;font-size:13px;border-radius:999px";
      b.textContent = label;
      b.onclick = () => enterTag(key, label);
      host.appendChild(b);
    };
    mk("", "全部");
    for (const [k, label] of tagList()) mk(k, label);
  }

  function renderRow(p) {
    const li = document.createElement("li");
    const btn = document.createElement("button");
    btn.className = "mp-row";
    btn.innerHTML = `<span class=nm>${esc(p.name)}</span><span class=sz>${fmtMB(p.size)}</span>${MP_CHEVRON}`;
    btn.onclick = () => showDetail(p);
    li.appendChild(btn);
    $("mp-list").appendChild(li);
  }

  function refreshCount() {
    if (menuMode) {
      $("mp-empty").style.display = "none";
      $("mp-count").textContent = "选择分类,进入玩法列表";
      $("mp-more").style.display = "none";
      return;
    }
    $("mp-empty").style.display = items.length === 0 && !loading ? "block" : "none";
    $("mp-count").textContent = items.length === 0 ? ""
      : `已加载 ${items.length}${total != null ? " / " + total : ""} 个` + (hasMore ? " · 下滑加载更多" : "");
    $("mp-more").style.display = hasMore ? "block" : "none";
  }

  // 一级菜单:全部 + 各分类行(数量来自官方 categoryCounts)。
  function renderMenu() {
    menuMode = true;
    $("mp-list").innerHTML = "";
    const mk = (key, label, count) => {
      const li = document.createElement("li");
      const btn = document.createElement("button");
      btn.className = "mp-row";
      const cnt = count != null ? ` · ${count} 个` : "";
      btn.innerHTML = `<span class=nm>${esc(label)}</span><span class=sz>${esc(cnt)}</span>${MP_CHEVRON}`;
      btn.onclick = () => enterCat(key, label);
      li.appendChild(btn);
      $("mp-list").appendChild(li);
    };
    const sum = catTags ? Object.values(catTags).reduce((a, b) => a + b, 0) : null;
    mk("", "全部玩法", sum);
    for (const [k, label] of catList()) mk(k, label, catTags?.[k]);
    refreshCount();
    log("选择分类,或点「全部玩法」浏览完整目录");
  }

  // 二级列表:顶部「返回分类」行 + 该分类/标签的玩法(可分页)。
  function renderBackRow() {
    const li = document.createElement("li");
    const btn = document.createElement("button");
    btn.className = "mp-row";
    btn.innerHTML = `<span class=nm style="color:var(--sky-dark)">‹ 返回分类</span>`;
    btn.onclick = renderMenu;
    li.appendChild(btn);
    $("mp-list").prepend(li);
  }

  function enterCat(key, label) {          // 分类(一级菜单进入)
    if (loading) return;
    menuMode = false;
    lastCat = key;
    lastTag = "";
    lastQ = "";
    $("mp-q").value = "";
    renderCatChips();
    items = []; hasMore = false; total = null; seen.clear();
    $("mp-list").innerHTML = "";
    renderBackRow();
    log(key ? `分类「${label}」加载中…` : "浏览全部玩法…");
    fetchPage(0);
  }

  function enterTag(key, label) {          // 标签(chips 点击)
    if (loading || (key === lastTag && !menuMode)) return;
    menuMode = false;
    lastTag = key;
    lastCat = "";
    lastQ = "";
    $("mp-q").value = "";
    renderCatChips();
    items = []; hasMore = false; total = null; seen.clear();
    $("mp-list").innerHTML = "";
    renderBackRow();
    log(key ? `标签「${label}」加载中…` : "浏览全部玩法…");
    fetchPage(0);
  }

  async function fetchPage(offset) {
    if (loading) return;
    loading = true;
    $("mp-more").textContent = "加载中…";
    try {
      let cur = offset;
      for (let guard = 0; guard < 500; guard++) {
        const r = await searchPlays(lastQ, cur, 20, lastCat, lastTag);
        let fresh = false;
        if (r.categoryCounts && !catTags) { catTags = r.categoryCounts; fresh = true; }
        if (r.discoveryTags && !discTags) { discTags = r.discoveryTags; fresh = true; }
        if (fresh) {           // 官方目录到手:chips 换真名,菜单带数量
          renderCatChips();
          if (menuMode) renderMenu();
        }
        let added = 0;
        for (const p of r.list) {
          if (seen.has(p.id)) continue;
          seen.add(p.id);
          items.push(p);
          renderRow(p);
          added++;
        }
        hasMore = r.hasMore;
        total = r.total;
        if (added > 0 || !hasMore) break;
        cur += 20;
        if (total != null && cur >= total) { hasMore = false; break; }
      }
      refreshCount();
    } catch (e) {
      log(lastCat
        ? `分类加载失败: ${e.message}(可换其它分类或「全部」)`
        : `目录加载失败: ${e.message}`, "err");
    } finally {
      loading = false;
      $("mp-more").textContent = "加载更多";
      refreshCount();
    }
  }

  $("mp-go").onclick = async () => {
    const q = $("mp-q").value.trim();
    if (loading) return;
    $("mp-panel").innerHTML = "";
    if (/^\d{1,7}$/.test(q)) {          // 纯数字 = play ID 直达
      log(`玩法 ID ${q} — 获取详情…`);
      try {
        const det = await getPlayDetail(Number(q));
        if (!det) { log(`玩法 ${q} 不存在或无可安装固件元数据`, "err"); return; }
        showDetail(det);
        log(`玩法 ${q}: ${det.name}`, "ok");
      } catch (e) {
        log(`玩法 ${q} 获取失败: ${e.message}`, "err");
      }
      return;
    }
    lastQ = q;
    lastCat = "";            // 搜索是独立视图:不带分类/标签过滤,chips 归「全部」
    lastTag = "";
    menuMode = false;
    renderCatChips();
    items = []; hasMore = false; total = null; seen.clear();
    $("mp-list").innerHTML = "";
    renderBackRow();
    await fetchPage(0);
    if (items.length === 0) log("没有结果", "");
    else if (q) log("");
  };
  renderCatChips();
  renderMenu();                        // 首屏 = 分类一级菜单
  $("mp-more").onclick = () => { if (!panelOpen && !menuMode) fetchPage(items.length); };
  if (typeof IntersectionObserver !== "undefined") {
    const io = new IntersectionObserver((entries) => {
      if (!panelOpen && !menuMode && entries.some((e) => e.isIntersecting)) fetchPage(items.length);
    }, { rootMargin: "120px" });
    io.observe($("mp-more"));
  }

  // 面板以浮层(底部抽屉,桌面端居中弹层)呈现,盖在列表上方 —— 列表可以
  // 很长,内联在列表下方会被无限加载越顶越远(真机反馈)。点遮罩关闭。
  function setPanel(html) {
    panelOpen = html !== "";
    $("mp-panel").innerHTML = html
      ? `<div class=mp-overlay id=mp-overlay><div class=mp-sheet>${html}</div></div>`
      : "";
    const ov = $("mp-overlay");
    if (ov) ov.onclick = (e) => { if (e.target === ov) clearPanel(); };
  }
  function clearPanel() { panelOpen = false; $("mp-panel").innerHTML = ""; }

  // 失败/不支持信息进浮层(真机反馈:列表滚了几屏后,搜索栏下方的状态行
  // 不在视口内,用户看不到"无法安装"的提示)。浮层 fixed 定位,任何滚动
  // 位置都可见;同时状态行也留一份(层级内用户同样受益)。
  function failSheet(title, msg) {
    setPanel(`<section class=mp-panel>
      <h4>${esc(title)}</h4>
      <p class=mp-sub style="margin-bottom:14px">${esc(msg)}</p>
      <button id=mp-fail-x class=mp-btn style="width:100%">知道了</button>
    </section>`);
    $("mp-fail-x").onclick = clearPanel;
  }

  function showDetail(p) {
    const updated = p.updatedAt ? new Date(p.updatedAt).toLocaleString() : "—";
    setPanel(`<section class=mp-panel>
      <h4>${esc(p.name)}</h4>
      <dl class=mp-meta>
        <dt>大小</dt><dd>${fmtMB(p.size)}</dd>
        <dt>修订</dt><dd>${p.revisionId ?? "—"}</dd>
        <dt>更新</dt><dd>${esc(updated)}</dd>
      </dl>
      <button id=mp-install class=mp-btn style="width:100%">安装</button>
      <p class=mp-status style="margin-top:8px">安装前会校验设备兼容性并选择槽位</p>
    </section>`);
    $("mp-install").onclick = () => install(p);
  }

  // 交互 v2:analyze(轻)→ 手机选槽 → 确认后才下载/校验/剥离(重)。
  async function install(p) {
    const t0 = Date.now();
    const stage = (s) => log(`${s}… (+${((Date.now() - t0) / 1000).toFixed(1)}s)`);
    stage("检查兼容性");
    const meta = await preflightMeta(p.id, { stage });
    if (!meta.ok) {
      const msg = meta.stage === "analyze"
        ? `该玩法不支持本设备: ${meta.reason}`
        : `预检失败: ${meta.reason}`;
      log(`✗ ${msg}`, "err");
      failSheet("无法安装", `${p.name}\n${msg}`);
      return;
    }
    showSlotPicker(meta, p);
  }

  function showSlotPicker(meta, p) {
    const a = meta.analyze;
    const imageLen = a?.extracted?.imageLen;
    const opts = SLOT_GEOMETRY.map(({ slot, partSize }) => {
      const limit = partSize - TAIL_SECTOR;
      return { slot, limit, fit: Number.isFinite(imageLen) && imageLen <= limit };
    });
    const fit = opts.filter((o) => o.fit);
    if (fit.length === 0) {
      const msg = Number.isFinite(imageLen)
        ? `固件 ${fmtMB(imageLen)} 超出所有槽位上限,无法安装`
        : "槽位容量信息缺失,无法安装";
      log(`✗ ${msg}`, "err");
      failSheet("无法安装", `${p.name}\n${msg}`);
      return;
    }
    const suggested = fit.some((o) => o.slot === a?.suggestedSlot)
      ? a.suggestedSlot : fit[0].slot;
    let chosen = suggested;
    let nameVal = displayNameFor(a?.name, p);   // 重选槽位重渲染时保留用户已改名

    const render = () => {
      setPanel(`<section class=mp-panel>
        <h4>安装到设备</h4>
        <p class=mp-sub style="margin-bottom:0">${esc(p.name)} · 剥离后 ${Number.isFinite(imageLen) ? fmtMB(imageLen) : "?"}</p>
        <label class=mp-note for=mp-name style="display:block;margin:12px 0 4px">安装名称(仅英文、数字与连接符号 - _ . ,≤32 字符)</label>
        <input id=mp-name maxlength=64 value="${esc(nameVal)}" autocomplete=off style="width:100%;font:inherit;padding:10px 12px;border:2px solid var(--ink);border-radius:var(--r);background:var(--paper);color:var(--ink)">
        <div class=mp-note style="text-align:right;margin-top:2px"><span id=mp-nch>0</span>/32</div>
        <div class=mp-slots role=radiogroup>
          ${opts.map((o) => `
          <button class=mp-slot role=radio aria-checked="${o.slot === chosen}"
            data-slot="${o.slot}" ${o.fit ? "" : "disabled"}>
            <span class=rd></span><span class=lb>槽位 ${o.slot}</span>
            ${o.slot === suggested ? '<span class=rec>建议</span>' : ""}
            <span class=cap>${o.fit ? `上限 ${fmtMB(o.limit)}`
              : (Number.isFinite(imageLen) ? `需 ${fmtMB(imageLen)} > 上限 ${fmtMB(o.limit)}` : "空间不足")}</span>
          </button>`).join("")}
        </div>
        <div class=mp-actions>
          <button id=mp-confirm class=mp-btn disabled>确认安装</button>
          <button id=mp-cancel class="mp-btn ghost">取消</button>
        </div>
      </section>`);
      const nameEl = $("mp-name"), nch = $("mp-nch");
      // 中文输入法组词期间绝不可回写 value(会打断 composition,把已上屏
      // 字符再吞一遍 —— 真机 bug:输入 shic 回显 shicshic)。组词中只更新
      // 计数;过滤发生在输入停止(compositionend/blur)与确认时。
      const cleanName = (s) => (s || "").replace(/[^\x20-\x7e]/g, "")
        .replace(/[^A-Za-z0-9._\- ]/g, "").slice(0, 32);
      let composing = false;
      nameEl.addEventListener("compositionstart", () => { composing = true; });
      nameEl.addEventListener("compositionend", () => {
        composing = false;
        nameEl.value = cleanName(nameEl.value);
        nameVal = nameEl.value;
        nch.textContent = String(nameEl.value.length);
      });
      nameEl.addEventListener("blur", () => {
        nameEl.value = cleanName(nameEl.value);
        nameVal = nameEl.value;
        nch.textContent = String(nameEl.value.length);
      });
      const updCount = () => {
        if (composing) { nch.textContent = String(cleanName(nameEl.value).length); return; }
        nameVal = nameEl.value;
        nch.textContent = String(nameEl.value.length);
      };
      nameEl.oninput = updCount;
      updCount();
      root.querySelectorAll(".mp-slot:not([disabled])").forEach((el) => {
        el.onclick = () => { chosen = Number(el.dataset.slot); render();
          requestAnimationFrame(() => { const n=$("mp-name"); if(n){n.focus(); n.setSelectionRange(n.value.length,n.value.length);} }); };
      });
      const confirm = $("mp-confirm");
      confirm.disabled = false;
      confirm.onclick = () => {
        nameVal = cleanName(nameEl.value);           // 确认时再滤一次(防漏网)
        continueInstall(meta, chosen, nameVal.trim());
      };
      $("mp-cancel").onclick = clearPanel;
    };
    render();
    log("选择槽位与名称后点「确认安装」");
  }

  async function continueInstall(meta, slot, userName) {
    const t0 = Date.now();
    const stage = (s) => log(`${s}… (+${((Date.now() - t0) / 1000).toFixed(1)}s)`);
    setPanel(`<section class=mp-panel>
      <div class=mp-prog>
        <progress id=mp-bar value=0 max=1></progress>
        <div class=lb><span id=mp-stage>准备中</span><span id=mp-pct>预检…</span></div>
      </div>
    </section>`);
    const bar = $("mp-bar"), stageEl = $("mp-stage"), pctEl = $("mp-pct");
    stage("下载固件");
    const pre = await prepareImage(meta, slot, { stage: (s) => { stageEl.textContent = s; } }, userName);
    if (!pre.ok) {
      log(`✗ 安装失败 [${pre.stage}]: ${pre.reason}`, "err");
      failSheet("安装失败", `${meta.play.name}\n[${pre.stage}] ${pre.reason}`);
      return;
    }
    stageEl.textContent = "上传";
    log("✓ " + pre.offer.name + " → 槽位 " + slot + ",开始上传");
    const r = await runInstall(bridge, pre.offer, pre.ext, {
      status: (s) => { if (s.confirmed) stageEl.textContent = "设备已确认,上传中"; },
      progress: (off, totalB) => {
        bar.value = off / totalB;
        pctEl.textContent = `${(off / totalB * 100).toFixed(0)}% · ${(off / 1024) | 0}/${(totalB / 1024) | 0} KB`;
      },
      resume: (off) => { stageEl.textContent = "断点续传"; pctEl.textContent = `从 ${(off / 1024) | 0} KB 恢复`; },
    });
    if (r.ok) {
      bar.value = 1;
      pctEl.textContent = "100%";
      log(`✓ 已安装到槽位 ${r.slot}。设备断电重开后在列表中选择启动。`, "ok");
      stageEl.textContent = "完成";
    } else {
      log(`✗ 安装失败 [${r.stage}]: ${r.reason}`, "err");
      let msg = `[${r.stage}] ${r.reason}`;
      if (r.stage === "confirm") msg += "\n设备在等待确认 —— 旧固件需在设备屏选槽按 OK;刷 v62+ 后手机上即可完成全部操作";
      if (r.stage === "token") msg += "\n重新扫码或重新配对(token 每店一次性)";
      failSheet("安装失败", `${pre.offer.name}\n${msg}`);
    }
  }

  // 配对成功后 shell 会写 hash 并 reload;hashchange 监听是双保险。
  if (typeof window !== "undefined") {
    window.addEventListener("hashchange", () => {
      const t = detectToken();
      if (t && t !== token) setToken(t);
    });
  }

  return { root, setToken: (t) => { token = t; bridge = createBridge(deviceOrigin ?? $("mp-dev").value, t); $("mp-dot")?.classList.add("on"); } };
}

// 浏览器直开(设备 boot 页 module script)时自动挂载;import 测试环境无 document。
if (typeof document !== "undefined" && typeof window !== "undefined" &&
    !window.__MP_INSTALL_BOOTED) {
  window.__MP_INSTALL_BOOTED = true;
  try { boot(); } catch (e) { console.error("phone-install boot failed:", e); }
}
