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
    size,
    sha256: sha.toLowerCase(),
    downloadUrl: url,
    updatedAt: p.updatedAt ?? null,
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

export async function searchPlays(q, limit = 20) {
  const d = await mpJson(`/api/plays?multiDevice=false&q=${encodeURIComponent(q)}`);
  const plays = Array.isArray(d?.plays) ? d.plays : [];
  return plays.map(normalizePlay).filter((p) => p).slice(0, limit);
}

export async function getPlayDetail(id) {
  // Worker 路由表只有 /api/plays(列表)与 /api/play?id=(详情,代理到上游
  // /api/plays/id/<id>);手机直接打 /api/plays/id/<id> 会落到 ASSETS 404
  // (生产实测,审计 B2)。
  const d = await mpJson(`/api/play?id=${encodeURIComponent(id)}`);
  return normalizePlay(d?.play);
}

// ── 安装预检(§6.3,单入口九步) ───────────────────────────────────────
// 返回 {ok, offer, ...} 或 {ok:false, stage, reason, ...}。data 为合并镜像
// Uint8Array,调用方(boot/runInstall)持有并在确认后上传。
export async function preflight(id, hooks = {}) {
  const stage = (s) => hooks.stage?.(s);
  stage("analyze");
  const a = await mpJson(`/api/analyze?id=${encodeURIComponent(id)}`);
  if (!a || a.supported !== true) {
    return { ok: false, stage: "analyze", reason: a?.reason ?? "unsupported", detail: a?.detail ?? null };
  }

  stage("market");
  const play = await getPlayDetail(id);
  if (!play) return { ok: false, stage: "market", reason: "play metadata missing firmware fields" };

  stage("download");
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

  stage("verify");
  const digest = await sha256Hex(merged);
  if (digest !== play.sha256) {
    return { ok: false, stage: "verify", reason: "store sha256 mismatch", got: digest };
  }
  if (!isFullImage(merged)) {
    return { ok: false, stage: "extract", reason: "not an esp merged image" };
  }

  stage("extract");
  let ext;
  try {
    ext = extractAppImage(merged);
  } catch (e) {
    return { ok: false, stage: "extract", reason: String(e && e.message ? e.message : e) };
  }

  stage("hash");
  const appSha = await sha256Hex(ext.data);
  // §6.3.7:本地解包长度/哈希必须与 analyze 一致,不一致即拒绝(不信任半截数据)。
  if (!a.extracted || ext.length !== a.extracted.imageLen || appSha !== a.extracted.sha256) {
    return { ok: false, stage: "preflight", reason: "extracted mismatch vs analyze",
             got: { imageLen: ext.length, sha256: appSha },
             analyze: a.extracted };
  }

  // 槽位 fit 表:仓库单一事实源(store-analyze.js SLOT_GEOMETRY)− 4KB 尾 sector。
  const slots = SLOT_GEOMETRY.map(({ slot, partSize }) => {
    const limit = partSize - TAIL_SECTOR;
    return { slot, limit, fit: ext.length <= limit };
  });
  const fitSlots = slots.filter((s) => s.fit);
  if (fitSlots.length === 0) {
    return { ok: false, stage: "preflight", reason: "image larger than every slot" };
  }
  const suggestedSlot = fitSlots.some((s) => s.slot === a.suggestedSlot)
    ? a.suggestedSlot : fitSlots[0].slot;

  const offer = {
    protocol: PROTOCOL_V1,
    playId: play.id,
    revisionId: a.revisionId ?? play.revisionId ?? 0,
    name: pickDisplayName(a.name, play.name, play.id),
    storeSha256: play.sha256,
    imageLen: ext.length,
    sha256: appSha,
    suggestedSlot,
    slots,
    reason: typeof a.reason === "string" && a.reason ? a.reason : "ok",
  };
  return { ok: true, offer, merged, ext, analyze: a, play };
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

// ── 最小自动 UI(boot 页加载本模块后自动挂载;§4.1 shell 是本地桥) ────
// 产品级 UI 后续迭代;MVP 目标:扫码 → 搜 → 装,全程手机可见状态。
export function boot(opts = {}) {
  if (typeof document === "undefined") throw new Error("boot() requires a browser document");
  const deviceOrigin = opts.deviceOrigin ?? detectDeviceOrigin();
  let token = opts.token ?? detectToken();
  let bridge = createBridge(deviceOrigin, token);

  const root = document.createElement("div");
  root.id = "mp-install-root";
  root.style.cssText = "font-family:sans-serif;max-width:26em;margin:1em auto;padding:0 8px";
  root.innerHTML = `
    <h3 style="margin:.4em 0">meta-pass install</h3>
    <div id=mp-log style="color:#555;white-space:pre-wrap;min-height:1.2em"></div>
    <div id=mp-search-row style="display:${deviceOrigin ? "flex" : "none"};gap:6px;margin:.5em 0">
      <input id=mp-q style="flex:1" placeholder="search plays…">
      <button id=mp-go>Search</button>
    </div>
    <div id=mp-dev-row style="display:${deviceOrigin ? "none" : "block"};margin:.5em 0">
      device address not detected — enter e.g. http://192.168.1.23
      <input id=mp-dev placeholder="http://192.168.x.x" style="width:100%">
      <button id=mp-dev-go>Connect</button>
    </div>
    <ul id=mp-list style="list-style:none;padding:0;margin:.5em 0"></ul>
    <div id=mp-detail style="margin:.5em 0"></div>
    <div id=mp-bar-wrap style="display:none;margin:.5em 0">
      <progress id=mp-bar style="width:100%" value=0 max=1></progress>
    </div>`;
  (opts.mount ?? document.body).appendChild(root);
  const $ = (id) => root.querySelector(`#${id}`);
  const log = (msg) => { $("mp-log").textContent = msg; };

  if (!deviceOrigin) {
    $("mp-dev-go").onclick = () => {
      const o = $("mp-dev").value.trim().replace(/\/$/, "");
      if (!/^http:\/\/\d+\.\d+\.\d+\.\d+$/.test(o)) { log("enter device address like http://192.168.1.23"); return; }
      bridge = createBridge(o, token);
      $("mp-dev-row").style.display = "none";
      $("mp-search-row").style.display = "flex";
      log("connected (token " + (token ? "from link" : "MISSING — pair by code below the device page") + ")");
    };
  } else if (!token) {
    log("no session token in link — enter the 6-digit pairing code on the device page first, then reload");
  }

  let last = [];
  $("mp-go").onclick = async () => {
    const q = $("mp-q").value.trim();
    if (!q) return;
    log(`searching "${q}"…`);
    try {
      last = await searchPlays(q);
    } catch (e) {
      log(`search failed: ${e.message}`);
      return;
    }
    const ul = $("mp-list");
    ul.innerHTML = "";
    for (const p of last) {
      const li = document.createElement("li");
      li.style.cssText = "padding:6px 0;border-bottom:1px solid #eee;cursor:pointer";
      li.textContent = `${p.name} · ${(p.size / 1048576).toFixed(1)}MB`;
      li.onclick = () => showDetail(p);
      ul.appendChild(li);
    }
    log(last.length ? `${last.length} result(s)` : "no installable results");
  };

  async function showDetail(p) {
    // §5/§6.2 元数据契约:name/size/revisionId/updatedAt/sha256(审计 M8)。
    const updated = p.updatedAt ? new Date(p.updatedAt).toLocaleString() : "?";
    $("mp-detail").textContent = `${p.name}\nsize ${(p.size / 1048576).toFixed(1)}MB · rev ${p.revisionId ?? "?"} · updated ${updated}\nsha256 ${p.sha256.slice(0, 16)}…`;
    const btn = document.createElement("button");
    btn.textContent = "Install";
    btn.onclick = () => install(p);
    $("mp-detail").appendChild(document.createElement("br"));
    $("mp-detail").appendChild(btn);
  }

  async function install(p) {
    log("preflight: analyze → download → verify → extract…");
    $("mp-bar-wrap").style.display = "block";
    const pre = await preflight(p.id, { stage: (s) => log(`preflight: ${s}…`) });
    if (!pre.ok) {
      log(`preflight failed [${pre.stage}]: ${pre.reason}`);
      return;
    }
    log(`offer: ${pre.offer.name} → device. Confirm the slot on the device screen.`);
    // 上传体 = 剥离后的 app 镜像(merged 是完整合并镜像,长度必不等于
    // offer.imageLen;runInstall 只收 ext.data —— 真机回归:审计 B1)。
    const r = await runInstall(bridge, pre.offer, pre.ext, {
      status: (s) => { if (s.confirmed) log(`device confirmed slot ${s.slot} — uploading…`); },
      progress: (off, total) => { $("mp-bar").value = off / total; },
      resume: (off) => log(`resuming at ${off}`),
    });
    $("mp-bar-wrap").style.display = "none";
    log(r.ok
      ? `installed to slot ${r.slot}. Power off & on the device to boot it.`
      : `install failed [${r.stage}]: ${r.reason}`);
    if (!r.ok && r.stage === "token") log("hint: re-scan the device QR code (token is one-shot per visit).");
  }

  // 配对成功后 shell 会写 hash 并 reload;hashchange 监听是双保险 —— 即便
  // 某个 shell 变体不 reload,模块也能即时拿到 token(审计:paired 后页面
  // 不跳转的根因就是模块加载时以空 token 初始化且再无更新通道)。
  if (typeof window !== "undefined") {
    window.addEventListener("hashchange", () => {
      const t = detectToken();
      if (t && t !== token) setToken(t);
    });
  }

  return { root, setToken: (t) => { token = t; bridge = createBridge(deviceOrigin ?? $("mp-dev").value, t); } };
}

// 浏览器直开(设备 boot 页 module script)时自动挂载;import 测试环境无 document。
if (typeof document !== "undefined" && typeof window !== "undefined" &&
    !window.__MP_INSTALL_BOOTED) {
  window.__MP_INSTALL_BOOTED = true;
  try { boot(); } catch (e) { console.error("phone-install boot failed:", e); }
}
