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

import { isFullImage, extractAppImage } from "./extract-app-image.js";
import { unpackNameBlobTail } from "./name-blob.js";

// 目标分区布局(main/partitions.csv, 8MB flash):
//   factory 0x170000 | ota_0 0x1D6000 | cardid 0x4000 | ota_1 0x200000 | ota_2 0x29E000(兼 littlefs)
// 槽位应用上限 = 分区大小 - 单一 4KB 尾 sector(与 meta_sign_app_limit 同一约定)。
export const SLOT_GEOMETRY = [
  { slot: 0, partSize: 0x1d6000 },
  { slot: 1, partSize: 0x200000 },
  { slot: 2, partSize: 0x29e000 },
];

const TAIL_SECTOR = 0x1000;

// 合并镜像分区表检查(r9 政策:白名单外数据分区一律警告放行):
//   白名单(标准存储:nvs/phy_init/otadata/cardid/store/coredump)→ 静默通过。
//   其余 type=1 数据分区(0x40 自定义区如 play 675 的 rec;标准文件系统如
//   play 563 的 easter 0x82 SPIFFS、play 200 的 voicefs 0x81 FAT、play 2 的
//   legacy_cardid 0x02 NVS)→ supported=true 照常可装,reason='custom-partitions'
//   + detail=<label> 警告透传:解包只取 factory 应用,该分区内容不会随镜像
//   进入设备;若子固件运行时真读写它,会缺存储而部分功能降级(设备 NOTE 行
//   显示分区名,用户自决)。硬拒会让市场上带资源分区的玩法全部不可装 ——
//   2026-09-27 实测 563(easter)因市场方新增彩蛋分区被拒,政策据此修正。
// 应用类型分区(type=0,含 factory/ota_*/recovery 等)一律不检查:解包只取
// factory 应用镜像写入槽位,其余应用分区内容在目标布局中完全惰性(563 的
// recovery 即此类)。
export const ALLOWED_PARTITION_LABELS = new Set([
  "nvs", "phy_init", "otadata", "cardid", "store", "coredump",
]);
// subtype 0x40:ESP-IDF 预留给“任意自定义数据用途”的数据分区 subtype。
const CUSTOM_DATA_SUBTYPE = 0x40;

const REASON_NOT_FOUND = "not-found";
const REASON_UNAVAILABLE = "unavailable";
const REASON_FORMAT = "format";
const REASON_NO_FACTORY = "no-factory";
const REASON_WRONG_CHIP = "wrong-chip";
const REASON_CUSTOM_PARTITIONS = "custom-partitions";
const REASON_TOO_LARGE = "too-large";

// 解包器内部上限按 2MB 槽位算;真正的槽位适配在下面按 SLOT_GEOMETRY 做,
// 这里传 slot2 上限即可避免解包器在 2MB 上限处误杀大镜像。
const UNPACK_MAX = SLOT_GEOMETRY[2].partSize - TAIL_SECTOR;

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
  async function analyzed(id) {
    if (!Number.isInteger(id) || id <= 0) return { error: REASON_NOT_FOUND };

    const meta = await fetchJson(`/api/plays/id/${id}`);
    if (meta.error) return { error: meta.error, ...(meta.detail ? { detail: meta.detail } : {}) };
    const play = meta.json && meta.json.play;
    if (!play || typeof play !== "object") return { error: REASON_NOT_FOUND };

    const cacheKey = `analyzed:${id}`;
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
    let partitionWarning = null;
    if (parts) {
      for (const p of parts) {
        if (p.type !== 0x01) continue; // 应用分区惰性,见 ALLOWED_PARTITION_LABELS 注释
        if (ALLOWED_PARTITION_LABELS.has(p.label)) continue;
        // r9:白名单外数据分区一律警告放行(0x40 自定义区与标准文件系统 subtype
        // 同性质 —— 内容不进设备,装了最坏功能降级)。首个分区名作 detail 透传。
        if (!partitionWarning) partitionWarning = p.label;
      }
    }

    let ext;
    try {
      ext = extractAppImage(got.buf, UNPACK_MAX);
    } catch (err) {
      return { error: mapExtractError(err) };
    }

    // 显示名优先取固件自带 MNAM(可打印 ASCII),否则退回商店 slug(服务端保证 ASCII)。
    let name = null;
    if (ext.tailSector) name = unpackNameBlobTail(ext.tailSector);
    if (!name) name = play.slug;

    const slots = SLOT_GEOMETRY.map(({ slot, partSize }) => {
      const limit = partSize - TAIL_SECTOR;
      return { slot, limit, fit: ext.length <= limit };
    });
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
      suggestedSlot: supported ? fitSlots[0].slot : -1,
      supported,
      // 警告可继续:subtype 0x40 自定义数据分区不阻断安装,reason 透传分区名,
      // 设备端详情页显示警告后由用户决定;reason='ok' 表示无任何警告。
      reason: supported ? (partitionWarning ? REASON_CUSTOM_PARTITIONS : "ok") : REASON_TOO_LARGE,
      ...(partitionWarning ? { detail: partitionWarning } : {}),
      extractedSha256: null, // 惰性:首次需要时对已验证的 ext.data 计算
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
      slots: entry.slots,
      suggestedSlot: entry.suggestedSlot,
      supported: entry.supported,
      reason: entry.reason,
      ...(entry.detail ? { detail: entry.detail } : {}),
    };
  }

  // 对外:分析接口。返回完整 JSON 对象(含 extracted.sha256)。
  async function analyze(id) {
    const got = await analyzed(id);
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
