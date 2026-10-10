// tools/install-slot/store-analyze.js —— 设备端 OTA 通道的服务端分析/分离逻辑(纯 ESM,零 Node API)。
// server.mjs 在 Node 下注入 node:crypto 的 sha256;Cloudflare Pages Functions 注入 WebCrypto 版本;
// 测试注入 mock fetch。核心依赖 install-slot/extract-app-image.js 的生产解包器,保证与安装页同一算法。
//
// 对外两个入口(挂在 createStoreAnalyzer 返回的对象上):
//   analyze(id)         → 元数据 + 解包分析 JSON(设备 P2 详情页唯一信息源)
//   extractedStream(id) → ReadableStream<Uint8Array>(已验证的 factory 应用镜像,流式下发)
//   extracted(id)       → 便捷一次性形态 {data, imageLen, sha256,name}(测试/工具用)
//
// 信任链:商店公布 firmwareSha256 → 本模块下载合并镜像后校验 → 解包出 factory 镜像并计算
// 其 sha256 → analyze JSON 把 extracted.sha256 下发设备 → 设备边下边算,同一 TLS 会话内比对。
// extracted 字节必须来自"sha256 已校验的合并镜像"的解包结果(方案 §1.2 服务端安全约束),
// 且 analyze/extracted 复用同一份缓存:按 play id 缓存 {merged, ext, storeFw, play},
// revisionId 变化时缓存失效回源重取(方案 §1.2 缓存策略)。

import { isFullImage, extractAppImage, parseDataPartitions, parseFirmwareManifest } from "./extract-app-image.js";
import { unpackNameBlobTail } from "./name-blob.js";
import { applyDataSizeProfile, firmwareDataRequiredSize } from "./data-size-profile.js";

// 目标分区布局(main/partitions.csv, 8MB flash):
//   pool_0 0x1D6000 | cardid 0x4000 | pool_1 0x49E000(含 store @0x35A000)
// 槽位应用上限 = 分区大小 - 单一 4KB 尾 sector(与 meta_sign_app_limit 同一约定)。
// dynslot §4.1:两池总字节数 = 6,766,592 B。

// 保留 SLOT_GEOMETRY 供 phone-install.js 的 legacy 回退视图使用(旧固件
// / 未建连设备);正式 analyze 走下方 POOL_TOTAL 尺寸检查。
 export const SLOT_GEOMETRY = [
   { slot: 0, partSize: 0x1d6000 },
   { slot: 1, partSize: 0x200000 },
   { slot: 2, partSize: 0x29e000 },
 ];

 const TAIL_SECTOR = 0x1000;

// 动态分区(§4.1):解包器上限按两池总量算,避免解包器误杀超大镜像(目前
// 最大的合法应用 1.84MB < 6.45MiB,足够兜底)。实际装不装得下由
// /api/install/slots 设备 carve 状态决定,这里只做预检。
import { POOL_TOTAL } from "./dynslot-pool.js";
const UNPACK_MAX = POOL_TOTAL - TAIL_SECTOR;

// DATA admission is owned by parseFirmwareManifest(): only global NVS/PHY and
// supported child filesystem DATA are accepted; unsupported DATA is a hard reject.
const REASON_NOT_FOUND = "not-found";
const REASON_UNAVAILABLE = "unavailable";
const REASON_FORMAT = "format";
const REASON_NO_FACTORY = "no-factory";
const REASON_WRONG_CHIP = "wrong-chip";
const REASON_TOO_LARGE = "too-large";


// 从 extractAppImage 抛出的英文错误映射为设备可展示的 reason 码。
export function mapExtractError(err) {
  const msg = err && err.message ? err.message : String(err);
  if (msg.includes("no factory app partition")) return REASON_NO_FACTORY;
  if (msg.includes("Wrong chip id")) return REASON_WRONG_CHIP;
  if (msg.includes("exceeds max app image size")) return REASON_TOO_LARGE;
  return REASON_FORMAT;
}

// 解析玩法元数据出下载所需的稳定字段;字段缺失视为 unavailable。
export function pickFirmwareFields(play) {
  const fw = play && typeof play === "object" ? play.firmware : null;
  const available = fw ? fw.available === true : false;
  const url = (fw && fw.url) || play?.downloadUrl || null;
  const size = Number((fw && fw.size) ?? play?.firmwareSize);
  const sha = (fw && fw.sha256) || play?.firmwareSha256 || null;
  if (!url || !Number.isFinite(size) || !sha) return null;
  return { available, url, size, sha256: sha };
}

export function listPartitions(buf) {
  if (!isFullImage(buf)) return null;
  const parts = [];
  const PARTITION_TABLE_OFFSET = 0x8000;
  const PARTITION_ENTRY_LEN = 32;
  for (
    let off = PARTITION_TABLE_OFFSET;
    off + PARTITION_ENTRY_LEN <= buf.length;
    off += PARTITION_ENTRY_LEN
  ) {
    if (buf[off] !== 0xaa || buf[off + 1] !== 0x50) break;
    let label = "";
    for (let i = 0; i < 16; i++) {
      const b = buf[off + 12 + i];
      if (b === 0) break;
      label += String.fromCharCode(b);
    }
    const u32le = (o) =>
      (buf[o] | (buf[o + 1] << 8) | (buf[o + 2] << 16) | (buf[o + 3] << 24)) >>> 0;
    parts.push({ label, type: buf[off + 2], subtype: buf[off + 3], offset: u32le(off + 4), size: u32le(off + 8) });
  }
  return parts;
}

// 构造分析器。deps 全部可注入:
//   fetchImpl  默认全局 fetch(测试注入 mock)
//   backend    官方市场源站,默认 https://ai-passport.folotoy.cn
//   sha256     async (Uint8Array) => 64 字符小写 hex(Node: node:crypto; CF Pages: WebCrypto)
//   cache      可选 Map 兼容对象(get/set),用于跨请求缓存分析结果(见 analyzeMerged 注释)
export function createStoreAnalyzer({ fetchImpl, backend, sha256, cache } = {}) {
  const doFetch = fetchImpl ?? globalThis.fetch;
  const store = backend ?? "https://ai-passport.folotoy.cn";
  const doSha256 = sha256;
  const doCache = cache ?? new Map();

  if (typeof doFetch !== "function") throw new Error("createStoreAnalyzer: fetchImpl required");
  if (typeof doSha256 !== "function") throw new Error("createStoreAnalyzer: sha256 required");

  // 错误对象统一携带 detail(r10.4):设备与调试者需要知道是哪一层失败
  // (metadata/image download 阶段 × 上游状态码),而不是一个孤零零的
  // unavailable。detail 走既有契约字段(设备端 parse 已支持)。
  async function fetchJson(path) {
    let res;
    try {
      res = await doFetch(store + path, { redirect: "follow" });
    } catch {
      return { error: REASON_UNAVAILABLE, detail: "upstream unreachable (metadata)" };
    }
    if (res.status === 404) return { error: REASON_NOT_FOUND };
    if (!res.ok) {
      return { error: REASON_UNAVAILABLE, detail: `upstream ${res.status} (metadata)` };
    }
    try {
      return { json: await res.json() };
    } catch {
      return { error: REASON_UNAVAILABLE, detail: "upstream returned non-JSON metadata" };
    }
  }

  async function fetchBytes(path) {
    let res;
    try {
      res = await doFetch(store + path, { redirect: "follow" });
    } catch {
      return { error: REASON_UNAVAILABLE, detail: "upstream unreachable (image download)" };
    }
    if (!res.ok) {
      return res.status === 404
        ? { error: REASON_NOT_FOUND }
        : { error: REASON_UNAVAILABLE, detail: `upstream ${res.status} (image download)` };
    }
    const buf = new Uint8Array(await res.arrayBuffer());
    return { buf };
  }

  // 缓存条目 {merged, ext, storeFw, play, revisionId}:analyze 首次构建后,
  // extracted 直接复用 ext.data(不再重拉 3MB、不再重新解包);merged/extData 均不
  // 长期驻留的淘汰交给注入的 cache(生产按方案为 LRU/R2,容量策略在部署侧)。
  async function analyzed(id, dataProfile = null) {
    if (!Number.isInteger(id) || id <= 0) return { error: REASON_NOT_FOUND };

    const meta = await fetchJson(`/api/plays/id/${id}`);
    if (meta.error) return { error: meta.error, ...(meta.detail ? { detail: meta.detail } : {}) };
    const play = meta.json && meta.json.play;
    if (!play || typeof play !== "object") return { error: REASON_NOT_FOUND };

    const cacheKey = `analyzed:${id}:${dataProfile || "default"}`;
    const cached = doCache.get(cacheKey);
    // 命中仍需回源确认 revisionId 未变(方案 §1.2:命中也需回源确认 revisionId)。
    // revisionId 缺失视为"未知版本":只在本地缓存过时才复用(首请求),避免每次回源。
    if (cached && play.revisionId != null && cached.revisionId != null) {
      if (cached.revisionId === play.revisionId) return { entry: cached };
      doCache.delete(cacheKey);
    }

    const fw = pickFirmwareFields(play);
    if (!fw) return { error: REASON_UNAVAILABLE, detail: "play metadata missing firmware fields (url/size/sha256)" };
    if (!fw.available) return { error: REASON_UNAVAILABLE, detail: "play marked unavailable by marketplace" };

    const got = await fetchBytes(fw.url);
    if (got.error) return { error: got.error, ...(got.detail ? { detail: got.detail } : {}) };
    if (got.buf.length !== fw.size) {
      return { error: REASON_FORMAT, detail: `size mismatch: store=${fw.size} got=${got.buf.length}` };
    }
    // 信任链第一步:商店公布哈希校验一致后才解包。
    const digest = await doSha256(got.buf);
    if (digest !== fw.sha256.toLowerCase()) return { error: REASON_FORMAT };

    if (!isFullImage(got.buf)) return { error: REASON_FORMAT };
    const parts = listPartitions(got.buf);
    let ext;
    let dataPartitions = []; // M5: 数据分区声明
    let firmwareManifest = null;
    try {
      ext = extractAppImage(got.buf, UNPACK_MAX);
      // DATA is part of the allocation contract, not an optional annotation.
      // A manifest parse failure or an unsupported child DATA subtype must stop
      // admission before the device is asked to carve or erase anything.
      firmwareManifest = parseFirmwareManifest(got.buf);
      dataPartitions = firmwareManifest.data;
      if (dataProfile) {
        dataPartitions = applyDataSizeProfile(dataPartitions, id, dataProfile);
        firmwareManifest.data = dataPartitions;
        firmwareManifest.required_size = firmwareDataRequiredSize(firmwareManifest);
      }
    } catch (err) {
      return { error: err?.message === "unsupported-partition"
        ? "unsupported-partition" : mapExtractError(err) };
    }

    // 显示名优先取固件自带 MNAM(可打印 ASCII),否则退回商店 slug(服务端保证 ASCII)。
    let name = null;
    if (ext.tailSector) name = unpackNameBlobTail(ext.tailSector);
    if (!name) name = play.slug;

    // dynslot §4.1:analyze 阶段没有设备 carve 状态,不能提案。只回答
    // "镜像能不能放进池?"——真正落点由 /api/install/slots 的设备 carve
    // 状态 + phone-install.js 的 geomFromListing 决定。
    const poolLimit = POOL_TOTAL - TAIL_SECTOR;
    const storageSupported = firmwareManifest.supported === true;
    const requiredSize = firmwareManifest.required_size;
    const poolFit = storageSupported && Number.isInteger(requiredSize) && requiredSize <= poolLimit;
    const slots = poolFit
      ? [{ slot: 0, limit: poolLimit, fit: true }]
      : [];
    const fitSlots = slots.filter((s) => s.fit);
    const supported = fitSlots.length > 0;

    const entry = {
      merged: got.buf,
      ext,
      storeFw: fw,
      play,
      revisionId: play.revisionId ?? null,
      name,
      slots,
      suggestedSlot: supported ? 0 : -1,
      supported,
      // Unsupported child DATA is an admission failure, not a warning: the device
      // must never be asked to carve or mutate storage for an image it cannot map.
      reason: !storageSupported
        ? (firmwareManifest.reason || "unsupported-partition")
        : (supported ? "ok" : REASON_TOO_LARGE),
      extractedSha256: null, // 惰性:首次需要时对已验证的 ext.data 计算
      dataPartitions, // M5: 数据分区声明
      manifest: firmwareManifest, // storage MVP: APP+DATA capacity model
    };
    doCache.set(cacheKey, entry);
    return { entry };
  }

  function analyzeJson(entry) {
    return {
      ok: entry.supported,
      id: entry.play.id,
      revisionId: entry.revisionId,
      name: entry.name,
      store: { size: entry.storeFw.size, sha256: entry.storeFw.sha256.toLowerCase() },
      extracted: { imageLen: entry.ext.length, sha256: entry.extractedSha256 },
      // M5: 数据分区声明(升级迁移用)
      data: entry.dataPartitions?.map(d => ({
        size: d.size,
        requiredSize: d.required_size,
        initialImageSize: d.initial_image_size,
        label: d.label,
        subtype: d.subtype,
      })) ?? [],
      storage: entry.manifest ? {
        requiredSize: entry.manifest.required_size,
        supported: entry.manifest.supported,
        reason: entry.manifest.reason,
      } : null,
      slots: entry.slots,
      suggestedSlot: entry.suggestedSlot,
      supported: entry.supported,
      reason: entry.reason,
      ...(entry.detail ? { detail: entry.detail } : {}),
    };
  }

  // 对外:分析接口。返回完整 JSON 对象(含 extracted.sha256)。
  async function analyze(id, dataProfile = null) {
    const got = await analyzed(id, dataProfile);
    if (got.error) {
      // 规范为完整契约(设备端 parse_analysis 依赖固定字段形态)。
      return {
        ok: false,
        id: Number.isInteger(id) ? id : null,
        revisionId: null,
        name: null,
        store: null,
        extracted: null,
        slots: null,
        suggestedSlot: -1,
        supported: false,
        reason: got.error,
        ...(got.detail ? { detail: got.detail } : {}),
      };
    }
    const entry = got.entry;
    if (entry.supported && entry.extractedSha256 === null) {
      entry.extractedSha256 = await doSha256(entry.ext.data);
    }
    return analyzeJson(entry);
  }

  // 对外:已验证 factory 应用镜像的流式形态(设备 /api/extracted 下发)。
  // 字节直接来自 analyze 阶段缓存(同一 TLS 会话内哈希绑定),不重复解包。
  async function extractedStream(id) {
    const got = await analyzed(id);
    if (got.error) return got;
    const entry = got.entry;
    if (!entry.supported) return { error: entry.reason };
    // 摘要惰性计算一次;此处必须先于流式响应算好,设备端会拿它与响应头/流比对。
    if (entry.extractedSha256 === null) {
      entry.extractedSha256 = await doSha256(entry.ext.data);
    }
    return { stream: entry.ext.data, imageLen: entry.ext.length, sha256: entry.extractedSha256, name: entry.name };
  }

  // 对外:便捷一次性形态(测试/工具)。从流式结果聚合,保证与流式路径字节一致。
  async function extracted(id) {
    const got = await extractedStream(id);
    if (got.error) return got;
    return {
      data: got.stream,
      imageLen: got.imageLen,
      sha256: got.sha256,
      name: got.name,
    };
  }

  return { analyze, extracted, extractedStream, listPartitions, mapExtractError };
}
