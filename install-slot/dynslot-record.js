// install-slot/dynslot-record.js —— dynslot carve 记录编解码 + 表物化 + 分配/删除
// 规划 + 槽位视图模型(纯 ES 模块,页面与 Node 测试共用,零依赖)。
//
// 与设备侧逐字节/逐规则同源(改这里必须同步改设备侧并跑 tools/validate.sh):
//   记录布局/编解码/A-B 选取  -> main/meta_carve_store.h/.c
//   carve 合法性/物化/分配器   -> main/meta_carve.c (meta_carve_valid /
//                                meta_pt_from_carve / meta_carve_place)
//   槽位状态语义              -> main/meta_slots.h (EMPTY=0 VALID=1 INVALID=2)
// 池几何与 first-fit 的数值复用 install-slot/dynslot-pool.js(= meta_carve.h)。
//
// 设计文档: docs/assets/dynslot-usb-slot-page-design.md §4/§5/§6/§7。
//
// 为什么页面要直接碰记录: USB 安装页跑在 ROM 下载模式,设备固件不在线;
// 新建/删除槽位必须由页面自己改写 store 扇区里的 carve 记录并重物化 0x8000 表,
// 否则启动器永远看不到改动(孤儿字节 / 删除复活)。

import { POOL, POOL_TOTAL, carveNeed, appLimit, freeSpans } from "./dynslot-pool.js";
import { isDynslotLayout, discoverSlots } from "./launcher-upgrade.js";
import "./vendor/md5.js";   // 副作用: globalThis.__READFLASH_MD5__ (RFC 1321)

// ---- 常量(= meta_carve_store.h / meta_carve.h / meta_slots.h) ----------------

export const REC_SIZE = 4096;
export const REC_HEADER = 16;
export const REC_SLOT_SIZE = 92;             // v2
export const REC_DATA_SIZE = 32;             // v2
export const REC_TABLE_OFF = 752;            // 16 + 8*92
export const REC_DATA_OFF = 3824;            // 752 + 0xC00(= meta_carve_store.h 宏真值;
                                             //  头文件行尾注释写 3776 是陈旧错误)
export const REC_CRC_OFF = 4080;             // 3824 + 8*32(同上,注释 4032 为陈旧错误)
export const REC_V1_SLOT_SIZE = 88;          // 只读兼容
export const REC_V1_TABLE_OFF = 720;
export const REC_V1_CRC_OFF = 3792;
export const REC_VERSION = 2;
export const REC_V1_VERSION = 1;

export const REC_MAGIC = 0x4353504d;         // 'M','P','S','C' 小端
export const REC_MAGIC_BYTES = [0x4d, 0x50, 0x53, 0x43];
export const CRED_MAGIC_BYTES = [0x4d, 0x50, 0x43, 0x4b];   // 'M','P','C','K' (MPCK)
export const SEQ_INVALID = [0x00000000, 0xffffffff];

export const PT_SIZE = 0xc00;
export const PT_ENTRY_SIZE = 32;
export const PT_MAX_ENTRIES = 24;
export const PT_MAGIC = 0x50aa;
export const PT_MAGIC_MD5 = 0xebeb;

export const SLOT_STATE = { EMPTY: 0, VALID: 1, INVALID: 2 };
export const SLOT_KIND = { APP: 0, STORAGE: 1 };
export const DATA_STATE = { PRISTINE: 0, DIRTY: 1, ARCHIVED: 2 };

export const SLOT_COUNT_MAX = 8;             // META_CARVE_MAX_SLOTS
export const DATA_MAX = 8;                   // META_DATA_MAX
export const MIN_SLOT = POOL.minSlot;        // 0x20000
export const OFFSET_ALIGN = POOL.offsetAlign;// 0x10000
export const SIZE_GRANULE = POOL.sizeGranule; // 0x1000
export const TAIL = POOL.tail;               // 0x1000
export const MIN_DATA = 0x1000;
export const LABEL_MAX = 16;                 // META_DATA_LABEL_MAX
export const NAME_MAX = 40;                  // 记录内 name[40]

export const STORE_OFFSET = 0x35a000;
export const STORE_SECTOR_SIZE = 0x1000;
export const STORE_SECTOR_A = 0;
export const STORE_SECTOR_B = 1;

// 固定系统条目(offset 升序,= meta_carve.c FIXED)。
const FIXED = [
  { type: 1, subtype: 2, offset: 0x9000,   size: 0x6000,  label: "nvs" },
  { type: 1, subtype: 1, offset: 0xf000,   size: 0x1000,  label: "phy_init" },
  { type: 0, subtype: 0, offset: 0x10000,  size: 0x170000, label: "factory" },
  { type: 1, subtype: 2, offset: 0x356000, size: 0x4000,  label: "cardid" },
  { type: 1, subtype: 2, offset: 0x35a000, size: 0x6000,  label: "store" },
  { type: 1, subtype: 0, offset: 0x7fe000, size: 0x2000,  label: "otadata" },
];

// 保留数据标签(= meta_carve.c RESERVED_LABELS,与 FIXED 同源)。
const RESERVED_LABELS = new Set(["nvs", "phy_init", "factory", "cardid", "store", "otadata"]);

// ---- CRC32(标准反射型,zlib/Python zlib.crc32 同值) --------------------------

let CRC_TABLE = null;
function crcTable() {
  if (!CRC_TABLE) {
    CRC_TABLE = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      CRC_TABLE[n] = c >>> 0;
    }
  }
  return CRC_TABLE;
}

export function crc32(bytes) {
  const t = crcTable();
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = t[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const md5 = (bytes) => globalThis.__READFLASH_MD5__(bytes);

// ---- 小端读写 --------------------------------------------------------------

const rdU16 = (b, o) => b[o] | (b[o + 1] << 8);
const rdU32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
function wrU16(b, o, v) { b[o] = v & 0xff; b[o + 1] = (v >>> 8) & 0xff; }
function wrU32(b, o, v) {
  b[o] = v & 0xff; b[o + 1] = (v >>> 8) & 0xff;
  b[o + 2] = (v >>> 16) & 0xff; b[o + 3] = (v >>> 24) & 0xff;
}
const alignUp = (v, a) => Math.ceil(v / a) * a;

// ---- carve 结构与合法性(= meta_carve_valid) ---------------------------------

/** 空 carve(与 C 侧 empty_carve 等价)。 */
export function emptyCarve() {
  return { slots: [], data: [] };
}

/** 池归属(= pool_of):[offset, offset+size) 完整落在某一段池内。 */
function poolOf(offset, size) {
  for (let i = 0; i < POOL.seg.length; i++) {
    const s = POOL.seg[i];
    if (offset >= s.start && offset + size <= s.end) return i;
  }
  return -1;
}

function labelPrintable(label) {
  if (typeof label !== "string" || label.length === 0 || label.length > LABEL_MAX) return false;
  for (let i = 0; i < label.length; i++) {
    const c = label.charCodeAt(i);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

/** 结构不变量:数量/顺序/对齐/粒度/池内/不重叠/kind/state 合法(= meta_carve_valid)。 */
export function carveValid(carve) {
  if (!carve || !Array.isArray(carve.slots) || !Array.isArray(carve.data)) return false;
  const slots = carve.slots;
  const data = carve.data;
  if (slots.length > SLOT_COUNT_MAX || data.length > DATA_MAX) return false;

  for (let i = 0; i < slots.length; i++) {
    const s = slots[i];
    // N8: 加下界校验 — 负数 state/kind 会被截断成非法值
    if (!Number.isInteger(s.kind) || s.kind < 0 || s.kind > SLOT_KIND.STORAGE) return false;
    if (!Number.isInteger(s.state) || s.state < 0 || s.state > SLOT_STATE.INVALID) return false;
    if (!Number.isInteger(s.size) || s.size < MIN_SLOT || s.size % SIZE_GRANULE !== 0) return false;
    if (!Number.isInteger(s.offset) || s.offset % OFFSET_ALIGN !== 0) return false;
    if (poolOf(s.offset, s.size) < 0) return false;
    if (i > 0) {
      const p = slots[i - 1];
      if (p.offset >= s.offset) return false;                 // 严格升序
      if (p.offset + p.size > s.offset) return false;         // 重叠
    }
  }

  for (let i = 0; i < data.length; i++) {
    const d = data[i];
    // N8: 加下界校验
    if (!Number.isInteger(d.playId) || d.playId <= 0) return false;
    if (!Number.isInteger(d.type) || d.type !== 1) return false;
    if (!Number.isInteger(d.state) || d.state < 0 || d.state > DATA_STATE.ARCHIVED) return false;
    if (!Number.isInteger(d.subtype) || d.subtype <= 0) return false;  // DATA_OTA 会被 otadata 抢单
    if (!Number.isInteger(d.size) || d.size < MIN_DATA || d.size % SIZE_GRANULE !== 0) return false;
    if (!Number.isInteger(d.offset) || d.offset % OFFSET_ALIGN !== 0) return false;
    if (poolOf(d.offset, d.size) < 0) return false;
    if (!labelPrintable(d.label)) return false;
    if (RESERVED_LABELS.has(d.label)) return false;
    for (let j = 0; j < i; j++) {
      const o = data[j];
      if (o.subtype === d.subtype && o.label === d.label) return false;   // (label,subtype) 唯一
      if (d.offset < o.offset + o.size && o.offset < d.offset + d.size) return false;
    }
    for (let s = 0; s < slots.length; s++) {
      const sl = slots[s];
      if (d.offset < sl.offset + sl.size && sl.offset < d.offset + d.size) return false;
    }
  }
  return true;
}

// ---- 表物化(= meta_pt_from_carve / meta_pt_encode) --------------------------

function ptEntryBytes(entries) {
  const out = new Uint8Array(entries.length * PT_ENTRY_SIZE + 32);
  out.fill(0xff);
  entries.forEach((e, i) => {
    const p = i * PT_ENTRY_SIZE;
    out[p] = PT_MAGIC & 0xff;
    out[p + 1] = (PT_MAGIC >> 8) & 0xff;
    out[p + 2] = e.type & 0xff;
    out[p + 3] = e.subtype & 0xff;
    wrU32(out, p + 4, e.offset >>> 0);
    wrU32(out, p + 8, e.size >>> 0);
    // [12,28) label:先整段清 0(meta_pt_encode 的 memset(p+12,0,16)),再拷字符
    out.fill(0, p + 12, p + 28);
    const label = String(e.label ?? "");
    for (let n = 0; n < 16 && n < label.length && label.charCodeAt(n) !== 0; n++) {
      out[p + 12 + n] = label.charCodeAt(n) & 0xff;
    }
    wrU32(out, p + 28, 0);   // flags
  });
  const m = entries.length * PT_ENTRY_SIZE;
  out[m] = PT_MAGIC_MD5 & 0xff;
  out[m + 1] = (PT_MAGIC_MD5 >> 8) & 0xff;
  out.fill(0xff, m + 2, m + 16);                    // 规范:14×0xFF
  out.set(md5(out.subarray(0, m)), m + 16);
  return out;   // 长度 = entries*32 + 32,调用方拷进 0xC00 缓冲
}

/**
 * carved 表物化:固定条目 + 槽位条目(ota_<i>) + 数据条目,按 offset 升序合并,
 * 条目链 + 0xEBEB marker + md5(entries),余量 0xFF(= meta_pt_from_carve)。
 * carve 非法返回 null。
 */
export function materializeTable(carve) {
  if (!carveValid(carve)) return null;
  const entries = FIXED.map((e) => ({ ...e }));
  carve.slots.forEach((s, i) => {
    entries.push({ type: 0, subtype: 0x10 + i, offset: s.offset, size: s.size, label: `ota_${i}` });
  });
  carve.data.forEach((d) => {
    entries.push({ type: d.type, subtype: d.subtype, offset: d.offset, size: d.size, label: d.label });
  });
  entries.sort((a, b) => a.offset - b.offset);   // 稳定排序;valid 已排除等偏移
  const packed = ptEntryBytes(entries);
  const out = new Uint8Array(PT_SIZE);
  out.fill(0xff);
  out.set(packed.subarray(0, Math.min(packed.length, PT_SIZE)), 0);
  return out;
}

/** 内嵌表有效性(= meta_pt_check):条目链 + MD5 marker,O(1) 栈。 */
export function checkTable(raw) {
  if (!raw || raw.length < PT_SIZE) return false;
  let count = 0;
  for (let off = 0; off + PT_ENTRY_SIZE <= PT_SIZE; off += PT_ENTRY_SIZE) {
    const magic = rdU16(raw, off);
    if (magic === PT_MAGIC_MD5) {
      if (count === 0) return false;
      const digest = md5(raw.subarray(0, off));
      for (let i = 0; i < 16; i++) if (raw[off + 16 + i] !== digest[i]) return false;
      return true;
    }
    if (magic !== PT_MAGIC || count >= PT_MAX_ENTRIES) return false;
    count++;
  }
  return false;   // 无 marker
}

// ---- 记录解码(= meta_carve_rec_decode/raw_info) -----------------------------

function recLayout(version) {
  if (version === REC_VERSION) {
    return { stride: REC_SLOT_SIZE, tableOff: REC_TABLE_OFF, crcOff: REC_CRC_OFF, v2: true };
  }
  if (version === REC_V1_VERSION) {
    return { stride: REC_V1_SLOT_SIZE, tableOff: REC_V1_TABLE_OFF, crcOff: REC_V1_CRC_OFF, v2: false };
  }
  return null;
}

function readName(raw, off) {
  let s = "";
  for (let i = 0; i < NAME_MAX; i++) {
    const c = raw[off + i];
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
}

// N1: data label 字段只有 LABEL_MAX(16) 字节,必须限制读取范围,
// 否则 16 字符无 NUL 标签会吞入后续 24 字节导致 decode 失败
function readNameBounded(raw, off, maxLen) {
  let s = "";
  for (let i = 0; i < maxLen; i++) {
    const c = raw[off + i];
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
}

/**
 * 解码一条 4KB 记录:v1/v2 布局、magic/版本/count/seq/CRC/字段范围/内嵌表 MD5
 * 逐层校验。失败返回 null(与 C 侧 decode 语义一致:失败即"当作无记录")。
 * 返回 { version, seq, slots:[{state,kind,offset,size,imageLen,playId,sha,name}],
 *        data:[{playId,offset,size,state,subtype,type,label}], table }。
 */
export function decodeRecord(raw) {
  if (!raw || raw.length < REC_SIZE) return null;
  if (raw[0] !== REC_MAGIC_BYTES[0] || raw[1] !== REC_MAGIC_BYTES[1] ||
      raw[2] !== REC_MAGIC_BYTES[2] || raw[3] !== REC_MAGIC_BYTES[3]) return null;
  const version = rdU16(raw, 4);
  const lay = recLayout(version);
  if (!lay) return null;
  const count = rdU16(raw, 6);
  if (count > SLOT_COUNT_MAX) return null;
  const seq = rdU32(raw, 8);
  if (seq === SEQ_INVALID[0] || seq === SEQ_INVALID[1]) return null;
  if (crc32(raw.subarray(0, lay.crcOff)) !== rdU32(raw, lay.crcOff)) return null;

  const slots = [];
  for (let i = 0; i < count; i++) {
    const p = REC_HEADER + i * lay.stride;
    const state = raw[p];
    const kind = raw[p + 1];
    if (state > SLOT_STATE.INVALID || kind > SLOT_KIND.STORAGE) return null;
    const nameOff = lay.v2 ? 52 : 48;
    const shaOff = lay.v2 ? 20 : 16;
    slots.push({
      state, kind,
      offset: rdU32(raw, p + 4),
      size: rdU32(raw, p + 8),
      imageLen: rdU32(raw, p + 12),
      playId: lay.v2 ? rdU32(raw, p + 16) : 0,
      sha: raw.slice(p + shaOff, p + shaOff + 32),
      name: readName(raw, p + nameOff),
    });
  }

  const data = [];
  if (lay.v2) {
    const dataCount = rdU16(raw, 12);
    if (dataCount > DATA_MAX) return null;
    for (let i = 0; i < dataCount; i++) {
      const p = REC_DATA_OFF + i * REC_DATA_SIZE;
      const d = {
        playId: rdU32(raw, p),
        offset: rdU32(raw, p + 4),
        size: rdU32(raw, p + 8),
        state: raw[p + 12],
        subtype: raw[p + 13],
        type: raw[p + 14],
        label: readNameBounded(raw, p + 16, LABEL_MAX),
      };
      if (d.state > DATA_STATE.ARCHIVED || d.type !== 1 || d.playId === 0) return null;
      data.push(d);
    }
  }

  const table = raw.slice(lay.tableOff, lay.tableOff + PT_SIZE);
  if (!checkTable(table)) return null;   // 内嵌表 MD5 损坏
  return { version, seq, slots, data, table };
}

/** 轻校验 + seq(= meta_carve_rec_raw_info):不解码主体。 */
export function rawRecordInfo(raw) {
  if (!raw || raw.length < REC_SIZE) return null;
  if (raw[0] !== REC_MAGIC_BYTES[0] || raw[1] !== REC_MAGIC_BYTES[1] ||
      raw[2] !== REC_MAGIC_BYTES[2] || raw[3] !== REC_MAGIC_BYTES[3]) return null;
  const version = rdU16(raw, 4);
  const lay = recLayout(version);
  if (!lay) return null;
  const count = rdU16(raw, 6);
  if (count > SLOT_COUNT_MAX) return null;
  const seq = rdU32(raw, 8);
  if (seq === SEQ_INVALID[0] || seq === SEQ_INVALID[1]) return null;
  if (crc32(raw.subarray(0, lay.crcOff)) !== rdU32(raw, lay.crcOff)) return null;
  return { version, seq, count };
}

/** 记录是否可采纳(= decode + validate:内嵌表 MD5 + carve 结构不变量)。 */
export function decodeAndValidate(raw) {
  const rec = decodeRecord(raw);
  if (!rec) return null;
  if (!carveValid({ slots: rec.slots, data: rec.data })) return null;
  return rec;
}

/** 扇区开头是否是旧 Wi-Fi 凭据备份 MPCK(= meta_carve_flash 的 cred_relocate 判据)。 */
export function isCredentialBackup(raw) {
  return !!raw && raw.length >= 4 &&
    raw[0] === CRED_MAGIC_BYTES[0] && raw[1] === CRED_MAGIC_BYTES[1] &&
    raw[2] === CRED_MAGIC_BYTES[2] && raw[3] === CRED_MAGIC_BYTES[3];
}

/**
 * A/B 选取(= meta_carve_rec_pick):各自 decode+validate,seq 新者胜(带符号差
 * 回绕比较);都无效 → null。
 * targetSector = 下一次提交写入的扇区(非胜者;无记录 → A)。例外:扇区 A 还
 * 站着 MPCK 凭据备份时改写 B —— 绝不覆盖凭据(设计 §9 E1)。
 */
export function pickRecord(rawA, rawB) {
  const ra = decodeAndValidate(rawA);
  const rb = decodeAndValidate(rawB);
  if (!ra && !rb) {
    const aIsCred = isCredentialBackup(rawA);
    return { rec: null, fromA: false, targetSector: aIsCred ? STORE_SECTOR_B : STORE_SECTOR_A, credInA: aIsCred };
  }
  let rec, fromA;
  if (ra && rb) {
    fromA = ((ra.seq - rb.seq) | 0) > 0;   // 带符号差回绕比较(显式括号防歧义)
    rec = fromA ? ra : rb;
  } else {
    fromA = !!ra;
    rec = ra || rb;
  }
  return {
    rec,
    fromA,
    targetSector: fromA ? STORE_SECTOR_B : STORE_SECTOR_A,
    credInA: !ra && isCredentialBackup(rawA),
  };
}

// ---- 记录编码(= meta_carve_rec_encode) --------------------------------------

/**
 * 编码一条 v2 记录(4096B):erase-before-write 语义(未用字节 0xFF),内嵌表 =
 * materializeTable(carve),CRC 最后写。
 * carve 非法 / seq 非法 → null。
 */
export function encodeRecord({ seq, slots, data = [] }) {
  const carve = { slots, data };
  // N8: seq 范围校验 — 负数或非整数会被 >>>0 截断成非法值
  if (!Number.isInteger(seq) || seq < 1 || seq > 0xfffffffe) return null;
  if (!carveValid(carve)) return null;
  const table = materializeTable(carve);
  if (!table) return null;

  const out = new Uint8Array(REC_SIZE);
  out.fill(0xff);
  wrU32(out, 0, REC_MAGIC);
  wrU16(out, 4, REC_VERSION);
  wrU16(out, 6, slots.length);
  wrU32(out, 8, seq >>> 0);
  wrU16(out, 12, data.length);

  for (let i = 0; i < slots.length; i++) {
    const s = slots[i];
    const p = REC_HEADER + i * REC_SLOT_SIZE;
    out[p] = s.state & 0xff;
    out[p + 1] = s.kind & 0xff;
    wrU32(out, p + 4, s.offset >>> 0);
    wrU32(out, p + 8, s.size >>> 0);
    wrU32(out, p + 12, (s.imageLen || 0) >>> 0);
    wrU32(out, p + 16, (s.playId || 0) >>> 0);
    const sha = s.sha || new Uint8Array(32);
    for (let k = 0; k < 32; k++) out[p + 20 + k] = sha[k] & 0xff;
    out.fill(0, p + 52, p + 92);                     // name 零填充
    const name = String(s.name || "");
    for (let n = 0; n < NAME_MAX && n < name.length && name.charCodeAt(n) !== 0; n++) {
      out[p + 52 + n] = name.charCodeAt(n) & 0xff;
    }
  }
  out.set(table, REC_TABLE_OFF);

  for (let i = 0; i < data.length; i++) {
    const d = data[i];
    const p = REC_DATA_OFF + i * REC_DATA_SIZE;
    wrU32(out, p, d.playId >>> 0);
    wrU32(out, p + 4, d.offset >>> 0);
    wrU32(out, p + 8, d.size >>> 0);
    out[p + 12] = d.state & 0xff;
    out[p + 13] = d.subtype & 0xff;
    out[p + 14] = d.type & 0xff;
    out.fill(0, p + 16, p + 32);                     // label 零填充
    const label = String(d.label || "");
    for (let n = 0; n < LABEL_MAX && n < label.length && label.charCodeAt(n) !== 0; n++) {
      out[p + 16 + n] = label.charCodeAt(n) & 0xff;
    }
  }
  wrU32(out, REC_CRC_OFF, crc32(out.subarray(0, REC_CRC_OFF)));
  return out;
}

// ---- 分配与删除规划(= meta_carve_find_fit / meta_carve_place / meta_carve_remove)

/** 占用域 = 槽位 ∪ 数据记录,offset 升序(= build_ranges)。 */
export function occupancyOf(carve) {
  const ranges = (carve?.slots ?? []).map((s) => ({ offset: s.offset, size: s.size }))
    .concat((carve?.data ?? []).map((d) => ({ offset: d.offset, size: d.size })));
  ranges.sort((a, b) => a.offset - b.offset);
  return ranges;
}

/**
 * first-fit 新槽落点(= meta_carve_place:上限只看槽位数,扫描看占用域)。
 * 返回 { index, offset, size } 或 null(放不下/超上限/参数非法)。
 * 注:install-slot/dynslot-pool.js 的 carvePlace 入参是占用域且按其长度判上限,
 * 与设备"槽位数判上限"在 7 槽 + 数据记录场景下有保守分歧;本函数按设备语义。
 */
export function placeNewSlot(carve, need) {
  if (!carve || !carveValid(carve)) return null;
  if (!Number.isInteger(need) || need < MIN_SLOT || need % SIZE_GRANULE !== 0) return null;
  if (carve.slots.length >= SLOT_COUNT_MAX) return null;
  const occ = occupancyOf(carve);
  let found = -1;
  let si = 0;
  for (let seg = 0; seg < POOL.seg.length && found < 0; seg++) {
    const segStart = POOL.seg[seg].start;
    const segEnd = POOL.seg[seg].end;
    let cursor = segStart;
    while (si < occ.length && occ[si].offset + occ[si].size <= segStart) si++;
    for (;;) {
      let gapEnd = segEnd;
      if (si < occ.length && occ[si].offset < segEnd) gapEnd = occ[si].offset;
      const cand = alignUp(cursor, OFFSET_ALIGN);
      if (cand + need <= gapEnd) { found = cand; break; }
      if (si < occ.length && occ[si].offset < segEnd) {
        cursor = occ[si].offset + occ[si].size;
        si++;
      } else {
        break;
      }
    }
  }
  if (found < 0) return null;
  let index = carve.slots.length;
  for (let i = 0; i < carve.slots.length; i++) {
    if (carve.slots[i].offset > found) { index = i; break; }
  }
  return { index, offset: found, size: need };
}

/** 空 APP 槽复用判定(= meta_carve_find_fit 但仅 EMPTY:装着玩法的槽不作推荐)。 */
export function findEmptyFit(carve, imageLen) {
  if (!carve || !Number.isInteger(imageLen) || imageLen <= 0) return -1;
  for (let i = 0; i < carve.slots.length; i++) {
    const s = carve.slots[i];
    if (s.kind !== SLOT_KIND.APP || s.state !== SLOT_STATE.EMPTY) continue;
    if (imageLen + TAIL <= s.size) return i;
  }
  return -1;
}

/** 移除槽位(数组压缩;字节擦除由调用方负责,= meta_carve_remove)。 */
export function removeSlot(carve, index) {
  if (!carve || !Number.isInteger(index) || index < 0 || index >= carve.slots.length) return null;
  return {
    slots: carve.slots.slice(0, index).concat(carve.slots.slice(index + 1)),
    data: carve.data.slice(),
  };
}

/** 在插入下标处放入新槽(= meta_carve_place 的插入语义)。 */
export function insertSlot(carve, index, { offset, size }) {
  const slots = carve.slots.slice();
  slots.splice(index, 0, {
    kind: SLOT_KIND.APP,
    state: SLOT_STATE.EMPTY,
    offset,
    size,
    imageLen: 0,
    playId: 0,
    sha: new Uint8Array(32),
    name: "",
  });
  return { slots, data: carve.data.slice() };
}

// ---- 安装/删除规划(页面动作的唯一决策源) ------------------------------------

export const PLAN = {
  CREATE: "create",     // 新建槽位(记录先写)
  REUSE: "reuse",       // 复用现有空槽
  NONE: "none",         // 无处可装
};

/**
 * 安装目标解析(设计 §6):
 *   {target:'auto'}            → first-fit 新槽(不复用)
 *   {target:'slot', index}     → 该槽必须是 APP 且装得下
 * 返回 {action, index?, offset?, size?, limit?, need?, largestGap?, totalFree?}。
 */
export function planInstall(carve, imageLen, target) {
  carve = carve ?? emptyCarve();   // N13: null 兜底,防 TypeError
  const gaps = freeSpans(occupancyOf(carve));
  const need = Number.isInteger(imageLen) && imageLen > 0 ? carveNeed(imageLen) : 0;
  const base = { need, largestGap: gaps.max, totalFree: gaps.total };

  if (!Number.isInteger(imageLen) || imageLen <= 0) {
    return { ...base, action: PLAN.NONE, reason: "no_image" };
  }
  if (target && target.target === "auto") {
    const place = need ? placeNewSlot(carve, need) : null;
    if (!place) return { ...base, action: PLAN.NONE, reason: "no_space" };
    return {
      ...base,
      action: PLAN.CREATE,
      index: place.index,
      offset: place.offset,
      size: place.size,
      limit: appLimit(place.size),
    };
  }
  const index = target ? target.index : -1;
  const slot = carve.slots[index];
  if (!slot || slot.kind !== SLOT_KIND.APP) {
    return { ...base, action: PLAN.NONE, reason: "bad_slot" };
  }
  const limit = appLimit(slot.size);
  if (imageLen > limit) return { ...base, action: PLAN.NONE, reason: "too_large", index, limit };
  return { ...base, action: PLAN.REUSE, index, offset: slot.offset, size: slot.size, limit };
}

/** 删除规划(设计 §7):返回 {carve: 下一个 carve, removed: 被删槽} 或 null。 */
export function planRemove(carve, index) {
  const next = removeSlot(carve, index);
  if (!next || !carveValid(next)) return null;
  return { carve: next, removed: carve.slots[index] };
}

// ---- 视图模型(= 渲染与动作的单一决策源,BUG-20) ------------------------------

export const MODE = {
  DYN_SLOT: "DYN_SLOT",         // 记录有效(设计 §3.1)
  DYN_FRESH: "DYN_FRESH",       // dynslot 表、无记录(§3.2)
  LEGACY_FIXED: "LEGACY_FIXED", // 固定三槽表(§3.3)
  LEGACY_FALLBACK: "LEGACY_FALLBACK", // 表不可读/空白(§3.4)
};

export const isDynMode = (mode) => mode === MODE.DYN_SLOT || mode === MODE.DYN_FRESH;

const stateWord = (state) =>
  state === SLOT_STATE.VALID ? "valid" :
  state === SLOT_STATE.INVALID ? "invalid" : "empty";

/**
 * 构建槽位视图(设计 §5):rows / auto / summary 全部由此一处产生,
 * 渲染器与动作处理器只读它 —— 渲染与路由不得各自分类(BUG-20)。
 *
 * @param {object} p
 * @param {{slots:Array,data:Array}} p.carve   carve 记录(legacy 模式可为 null)
 * @param {number} p.imageLen                  已载入镜像长度(0 = 未选文件)
 * @param {string} p.mode                      MODE.*
 * @param {Array}  p.legacySlots               legacy: [{offset,size}] (discoverSlots 结果)
 * @param {boolean} p.deferred                 社区 tab 已选但玩法详情未取回(imageLen 暂未知)。
 *                                             为 true 且无 imageLen 时 auto.reason = "deferred"
 *                                             (渲染成「等待玩法详情」,而非 no_image)。
 */
export function buildSlotView({ carve, imageLen = 0, mode = MODE.DYN_SLOT, legacySlots = [], deferred = false } = {}) {
  const dyn = isDynMode(mode);
  const slots = dyn ? (carve?.slots ?? []) : legacySlots;
  const data = dyn ? (carve?.data ?? []) : [];
  const occupancy = dyn
    ? occupancyOf(carve)
    : legacySlots.map((s) => ({ offset: s.offset, size: s.size }));
  const gaps = freeSpans(occupancy);

  const rows = slots.map((s, i) => {
    const kind = dyn ? (s.kind === SLOT_KIND.STORAGE ? "storage" : "app") : "app";
    const state = dyn ? stateWord(s.state) : "unknown";
    const limit = kind === "app" && dyn ? appLimit(s.size) : (dyn ? 0 : appLimit(s.size));
    const fits = !Number.isInteger(imageLen) || imageLen <= 0
      ? true
      : kind === "app" && imageLen + TAIL <= s.size;
    return {
      id: `slot-${i}`,
      slot: i,
      offset: s.offset,
      size: s.size,
      limit,
      state,
      name: dyn ? (s.name || "") : "",
      kind,
      occupied: dyn ? s.state !== SLOT_STATE.EMPTY : undefined,
      targetable: dyn ? (kind === "app" && fits) : fits,
      recommended: false,
      removeEnabled: dyn,
    };
  });

  // 推荐:首个装得下的空 APP 槽(= 设备 suggestedSlot 语义:先复用、零副作用)。
  const recIdx = dyn && Number.isInteger(imageLen) && imageLen > 0
    ? rows.findIndex((r) => r.kind === "app" && r.state === "empty" && r.targetable)
    : -1;
  if (recIdx >= 0) rows[recIdx].recommended = true;

  // Auto(新槽)提案 —— 仅 dynslot(设计 G3)。
  const need = Number.isInteger(imageLen) && imageLen > 0 ? carveNeed(imageLen) : 0;
  let auto;
  if (!dyn) {
    auto = { enabled: false, recommended: false, reason: { code: "legacy_mode" }, need, offset: 0, size: 0, limit: 0 };
  } else if (!need) {
    auto = { enabled: false, recommended: false,
             reason: { code: deferred ? "deferred" : "no_image" },
             need: 0, offset: 0, size: 0, limit: 0 };
  } else {
    const place = placeNewSlot(carve, need);
    auto = place
      ? { enabled: true, recommended: recIdx < 0, reason: null, need,
          offset: place.offset, size: place.size, limit: appLimit(place.size) }
      : { enabled: false, recommended: false, reason: { code: "no_space" },
          need, largestGap: gaps.max, totalFree: gaps.total, offset: 0, size: 0, limit: 0 };
  }

  const usedSlots = slots.reduce((a, s) => a + s.size, 0);
  const usedData = data.reduce((a, d) => a + d.size, 0);
  const total = POOL_TOTAL;
  const free = Math.max(0, total - usedSlots - usedData);

  return {
    mode,
    dyn,
    rows,
    auto,
    summary: {
      count: slots.length,
      usedSlots,
      usedData,
      free,
      total,
      largestGap: gaps.max,
    },
  };
}

/**
 * 模式判定（设计 §3，首个命中生效）：
 *   1. 记录可解码            → DYN_SLOT   （模型 = 记录）
 *   2. 表有 store@0x35A000   → DYN_FRESH  （模型 = 空 carve）
 *   3. 表有 ota_0/1/2        → LEGACY_FIXED（模型 = 表派生，禁止写记录）
 *   4. 其余                  → LEGACY_FALLBACK
 * @param {{record?: object|null, partitions?: Array|null}} p
 */
export function detectSlotMode({ record = null, partitions = null } = {}) {
  if (record) return MODE.DYN_SLOT;
  const parts = Array.isArray(partitions) ? partitions : [];
  if (isDynslotLayout(parts)) return MODE.DYN_FRESH;
  if (discoverSlots(parts).length > 0) return MODE.LEGACY_FIXED;
  return MODE.LEGACY_FALLBACK;
}
