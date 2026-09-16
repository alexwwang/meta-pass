// install-slot/launcher-upgrade.js —— launcher 升级纯逻辑(ES module,浏览器/Node 双端)。
//
// 升级契约(2026-09 固化):meta-pass 分区结构已稳定,launcher 升级只允许写入:
//   0x0        bootloader.bin          (~21KB)
//   0x8000     partition-table.bin     (3KB,含 MD5 marker)
//   0x10000    FoloToy-AI-Passport.bin (factory app)
//   0x7FE000   ota_data_initial.bin    (8KB 擦除态,重置 OTA 选择,升级后回到 factory)
// 其余分区一律不写,设备上的数据原样保留:
//   nvs 0x9000(NVS 存储数据:Wi-Fi 配置、应用内部状态)、cardid 0x356000、ota_0/1/2(已装子固件)。
//
// 安全前提:升级前必须从设备读回分区表(0x8000, 0x1000 字节)与升级包内的
// partition-table.bin 逐字节比对——不一致说明布局变了,必须拒绝升级(否则
// "仅刷 factory"的假设失效,可能覆盖或错位用户数据)。

import { TAIL_SECTOR } from "./name-blob.js";

export const PARTITION_TABLE_OFFSET = 0x8000;
export const PARTITION_TABLE_READ_SIZE = 0x1000; // 读回整 4KB 扇区(表本体 0xC00)
export const OTADATA_OFFSET = 0x7FE000;

// 升级写入计划(顺序即写入顺序;地址为 flash 绝对偏移)。
// 返回 [{ name, offset, why }] —— 页面据此逐文件 writeFlash。
export function upgradeWritePlan() {
  return [
    { name: "bootloader.bin", offset: 0x0, why: "second-stage bootloader" },
    { name: "partition-table.bin", offset: PARTITION_TABLE_OFFSET, why: "layout contract" },
    { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
    { name: "ota_data_initial.bin", offset: OTADATA_OFFSET, why: "reset OTA selection (boot factory)" },
  ];
}

// 升级包完整性:四个文件必须齐全且非空;分区表必须可解析。
// files: Map<name, Uint8Array>。返回 { ok, reason? }。
export function checkUpgradeBundle(files) {
  for (const step of upgradeWritePlan()) {
    const data = files.get(step.name);
    if (!data || data.length === 0) {
      return { ok: false, reason: `missing or empty ${step.name}` };
    }
  }
  try {
    parsePartitionTable(files.get("partition-table.bin"));
  } catch (err) {
    return { ok: false, reason: `partition table invalid: ${err.message}` };
  }
  // factory app 必须是 ESP 镜像(0xE9 魔数),防止选错文件
  const app = files.get("FoloToy-AI-Passport.bin");
  if (app[0] !== 0xe9) {
    return { ok: false, reason: "FoloToy-AI-Passport.bin is not an ESP app image (bad magic)" };
  }
  if (app.length > 0x170000) {
    return { ok: false, reason: `factory app ${app.length} bytes exceeds the 0x170000 partition` };
  }
  return { ok: true };
}

// ---- 分区表解析(与 tools/verify_firmware.py 同语义,用于设备读回比对) ----

const ENTRY = 32; // esp_partition_info_t
const MAGIC_ENTRY = 0x50aa;
const MAGIC_MD5 = 0xebeb;

// 解析分区表字节(设备读回的 4KB 扇区或升级包内的 3KB 文件均可)。
// 返回 [{ label, type, subtype, offset, size }];md5 校验失败抛错。
export function parsePartitionTable(raw) {
  if (!(raw instanceof Uint8Array) || raw.length < ENTRY) {
    throw new Error("partition table too small");
  }
  const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  const partitions = [];
  const dec = new TextDecoder();
  for (let cursor = 0; cursor + ENTRY <= raw.length; cursor += ENTRY) {
    const magic = dv.getUint16(cursor, true);
    if (magic === 0xffff) break;
    if (magic === MAGIC_MD5) {
      // MD5 marker:与 verify_firmware.py 同语义——digest 位于 marker 条目的
      // [cursor+16, cursor+32),覆盖其前的全部表数据(marker 仍占一个 32B 条目)
      const digest = raw.subarray(cursor + 16, cursor + 32);
      const expected = md5(raw.subarray(0, cursor));
      for (let i = 0; i < 16; i++) {
        if (digest[i] !== expected[i]) throw new Error("partition table md5 mismatch");
      }
      return partitions;
    }
    if (magic !== MAGIC_ENTRY) throw new Error(`bad partition entry magic 0x${magic.toString(16)}`);
    const type = raw[cursor + 2];
    const subtype = raw[cursor + 3];
    const offset = dv.getUint32(cursor + 4, true);
    const size = dv.getUint32(cursor + 8, true);
    const labelRaw = raw.subarray(cursor + 12, cursor + 28);
    const label = dec.decode(labelRaw.subarray(0, labelRaw.indexOf(0)));
    partitions.push({ label, type, subtype, offset, size });
  }
  return partitions; // 无 md5 marker:升级包内的表由 idf.py 生成必有 marker,
  // 设备读回的表 marker 在 0xC00 之后本就读不到——比对按字节级进行,这里宽松返回
}

// 判断从设备读回的分区表扇区是否为全擦除态(空白机:全 0xFF)。
export function isErasedTable(table) {
  return table instanceof Uint8Array && table.length > 0 &&
    table.every((b) => b === 0xff);
}

// 判断设备是否运行原厂旧布局(FoloToy 单固件:无 OTA 槽,有 recovery@0x700000)。
// 识别依据:表内有 factory app 且没有任何 ota_* app 分区。不依赖具体地址,
// 未来原厂改版仍能识别。devicePartitions: parsePartitionTable(设备表) 的结果。
export function isLegacyFactoryLayout(devicePartitions) {
  if (!Array.isArray(devicePartitions) || devicePartitions.length === 0) return false;
  const hasFactory = devicePartitions.some(
    (p) => p.type === 0 && p.subtype === 0,
  );
  const hasOta = devicePartitions.some(
    (p) => p.type === 0 && p.subtype >= 0x10 && p.subtype <= 0x1f,
  );
  return hasFactory && !hasOta;
}

// 判断一块数据是否有"子固件头"的可能(非全 FF 即视为有数据)。
// 只看头 24B:任何 ESP 镜像(0xE9)或遗留数据首块都必然非 FF。
export function slotHasData(head24) {
  return head24 instanceof Uint8Array && head24.length === 24 &&
    head24.some((b) => b !== 0xff);
}

// ---- 单文件升级容器(MPUP: Meta-pass Upgrade Pack)----
//
// 市场分发要求升级产物是且仅是一个文件。容器把四段镜像(bootloader/分区表/
// factory app/otadata 擦除态)打包为一个 .bin,页面选这一个文件、解析后仍按
// 写入计划逐段写到各自的分区地址(多次写是地址问题,不是多个固件问题)。
//
// 容器布局(全部小端):
//   [0..7]    魔数 "MPUPV1\0"
//   [8..11]   header_size(u32,含魔数,即第一段数据的绝对偏移)
//   [12..15]  段数(u32)
//   随后每段 72B 段表项 × N:
//     [0..31]  name(定长 32B,NUL 结尾 ASCII,不足补 0)
//     [32..35] offset(u32,flash 绝对地址)
//     [36..39] size(u32)
//     [40..63] sha256(32B)
//   [header_size..] 各段数据依次紧随(无对齐要求,原样字节)。

export const MPUP_MAGIC = "MPUPV1";
const MPUP_ENTRY_SIZE = 72;   // name 32B + offset 4B + size 4B + sha256 32B
const NAME_MAX = 32;

// 打包:files 为 Map<name, Uint8Array>,名称必须与 upgradeWritePlan() 一致;
// 顺序按写入计划排列。返回 Uint8Array。解包后的数据与写入计划可逐字节往返。
export function packUpgradeContainer(files) {
  const plan = upgradeWritePlan();
  const headerSize = 16 + plan.length * MPUP_ENTRY_SIZE;
  const entries = [];
  let bodySize = 0;
  for (const step of plan) {
    const data = files.get(step.name);
    if (!(data instanceof Uint8Array) || data.length === 0) {
      throw new Error(`pack: missing or empty ${step.name}`);
    }
    const nameBytes = new TextEncoder().encode(step.name);
    if (nameBytes.length >= NAME_MAX) throw new Error(`pack: name too long ${step.name}`);
    entries.push({ step, nameBytes, data, bodyOffset: bodySize });
    bodySize += data.length;
  }
  const out = new Uint8Array(headerSize + bodySize);
  const dv = new DataView(out.buffer);
  // 魔数 + header_size + 段数
  out.set(new TextEncoder().encode(MPUP_MAGIC), 0);
  dv.setUint32(8, headerSize, true);
  dv.setUint32(12, plan.length, true);
  // 段表 + 数据
  let entryAt = 16;
  for (const e of entries) {
    out.set(e.nameBytes, entryAt);   // 名字段定长 32B,NUL 结尾(后面自动为 0)
    dv.setUint32(entryAt + 32, e.step.offset, true);
    dv.setUint32(entryAt + 36, e.data.length, true);
    out.set(sha256Sync(e.data), entryAt + 40);
    out.set(e.data, headerSize + e.bodyOffset);
    entryAt += MPUP_ENTRY_SIZE;
  }
  return out;
}

// 解包 + 完整校验:魔数/段数/段表与写入计划一致、每段 sha256 与段表声明一致、
// 段数据无越界。返回 Map<name, Uint8Array>(可直接交给 checkUpgradeBundle/页面写入)。
// 任何不一致抛 Error(message 以 "upgrade container: " 开头)。
export function unpackUpgradeContainer(raw) {
  const fail = (m) => { throw new Error(`upgrade container: ${m}`); };
  if (!(raw instanceof Uint8Array) || raw.length < 16) fail("too small");
  const magic = new TextDecoder().decode(raw.subarray(0, 6));
  if (magic !== MPUP_MAGIC) fail(`bad magic "${magic}"`);
  const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  const headerSize = dv.getUint32(8, true);
  const count = dv.getUint32(12, true);
  if (headerSize !== 16 + count * MPUP_ENTRY_SIZE) fail(`header_size ${headerSize} != 16 + ${count}*72`);
  if (headerSize > raw.length) fail("header exceeds file");
  const plan = upgradeWritePlan();
  if (count !== plan.length) fail(`segment count ${count} != write plan ${plan.length}`);
  const dec = new TextDecoder();
  const files = new Map();
  let bodyAt = headerSize;
  for (let i = 0; i < count; i++) {
    const at = 16 + i * MPUP_ENTRY_SIZE;
    const nameField = raw.subarray(at, at + NAME_MAX);
    const nameLen = nameField.indexOf(0);
    if (nameLen <= 0) fail(`segment ${i}: bad name field`);
    const name = dec.decode(nameField.subarray(0, nameLen));
    const expected = plan[i];
    if (name !== expected.name) fail(`segment ${i}: "${name}" != plan "${expected.name}"`);
    const offset = dv.getUint32(at + 32, true);
    const size = dv.getUint32(at + 36, true);
    if (offset !== expected.offset) fail(`segment ${i}: offset 0x${offset.toString(16)} != plan 0x${expected.offset.toString(16)}`);
    if (bodyAt + size > raw.length) fail(`segment ${i}: data out of bounds`);
    const data = raw.slice(bodyAt, bodyAt + size);
    const digest = sha256Sync(data);
    const declared = raw.subarray(at + 40, at + 72);
    for (let k = 0; k < 32; k++) {
      if (digest[k] !== declared[k]) fail(`segment ${name}: sha256 mismatch`);
    }
    files.set(name, data);
    bodyAt += size;
  }
  if (bodyAt !== raw.length) fail(`trailing bytes: file ${raw.length}, segments end at ${bodyAt}`);
  return files;
}

// ---- 同步 SHA-256(容器段校验用;标准算法,与 hashlib/openssl 互通)----
// 页面与 Node 均可运行;不依赖 crypto.subtle(避免非安全上下文差异),纯同步。
const SHA_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function sha256Sync(data) {
  const l = data.length;
  const bitLen = l * 8;
  const padded = new Uint8Array((((l + 8) >> 6) + 1) << 6);
  padded.set(data);
  padded[l] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 4, bitLen >>> 0, false);
  dv.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000), false);
  const H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  for (let block = 0; block < padded.length; block += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(block + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + SHA_K[i] + w[i]) >>> 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      h = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    const upd = [a, b, c, d, e, f, g, h];
    for (let i = 0; i < 8; i++) H[i] = (H[i] + upd[i]) >>> 0;
  }
  const out = new Uint8Array(32);
  const odv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) odv.setUint32(i * 4, H[i], false);
  return out;
}

// 原厂机首次迁移的擦除计划:清除旧固件/旧数据在新布局槽位区域内的全部残留。
// 目标:0x180000(ota_0 起点)→ 0x7FE000(otadata 起点),其中 cardid 保护区跳过。
// 返回 [{ offset, size, why }] —— 页面据此调用 esptool eraseRegion;空数组表示无需擦除。
export function migrationErasePlan() {
  const CARDID = { offset: 0x356000, size: 0x4000 };
  const START = 0x180000;
  const END = 0x7fe000;
  const plan = [];
  // cardid 之前的整段
  if (CARDID.offset > START) {
    plan.push({
      offset: START,
      size: CARDID.offset - START,
      why: "wipe legacy firmware residue inside ota_0 (before cardid)",
    });
  }
  // cardid 之后的整段(otadata 起点前结束;otadata 由升级写入项重置)
  const after = CARDID.offset + CARDID.size;
  if (END > after) {
    plan.push({
      offset: after,
      size: END - after,
      why: "wipe legacy residue inside ota_1/ota_2 (after cardid)",
    });
  }
  return plan;
}

// 升级前校验:设备上的分区表与升级包内的表必须逐字节一致(以升级包文件为基准)。
// deviceTable: 从设备 0x8000 读回的 Uint8Array;bundleTable: 升级包内 partition-table.bin。
// 一致性判定:升级包表长度 ≤ 设备读回长度,且升级包表的非 0xFF 前缀与设备对应区域完全一致。
// 返回 { ok, reason? }。
export function comparePartitionTables(deviceTable, bundleTable) {
  if (!(deviceTable instanceof Uint8Array) || deviceTable.length < PARTITION_TABLE_READ_SIZE) {
    return { ok: false, reason: `device partition table read too small (${deviceTable?.length ?? 0})` };
  }
  if (!(bundleTable instanceof Uint8Array) || bundleTable.length === 0) {
    return { ok: false, reason: "bundle partition table missing" };
  }
  if (bundleTable.length > deviceTable.length) {
    return { ok: false, reason: "bundle table larger than the device sector" };
  }
  // idf.py 生成的表文件尾部有 0xFF 填充;逐字节比对到包文件长度即可
  for (let i = 0; i < bundleTable.length; i++) {
    if (deviceTable[i] !== bundleTable[i]) {
      return {
        ok: false,
        reason: `device partition table differs at byte ${i} (device 0x${deviceTable[i].toString(16)}, bundle 0x${bundleTable[i].toString(16)}) — layout changed, refusing in-place upgrade`,
      };
    }
  }
  return { ok: true };
}

// ---- 最小 MD5(仅用于分区表 marker 校验;实现按 RFC 1321,输入 ≤ 4KB) ----
// 页面已有 sha256 工具,但分区表 marker 是 MD5,自实现避免引依赖。

function md5(bytes) {
  const ff = (x, y, z) => (x & y) | (~x & z);
  const gg = (x, y, z) => (x & z) | (y & ~z);
  const hh = (x, y, z) => x ^ y ^ z;
  const ii = (x, y, z) => y ^ (x | ~z);
  const rot = (x, c) => (x << c) | (x >>> (32 - c));
  const K = new Int32Array(64);
  const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);
  const origLen = bytes.length;
  const withPadding = (((origLen + 8) >> 6) + 1) * 64;
  const msg = new Uint8Array(withPadding);
  msg.set(bytes);
  msg[origLen] = 0x80;
  const dv = new DataView(msg.buffer);
  dv.setUint32(withPadding - 8, (origLen * 8) >>> 0, true);
  dv.setUint32(withPadding - 4, Math.floor((origLen * 8) / 4294967296), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (let off = 0; off < withPadding; off += 64) {
    const M = new Int32Array(16);
    for (let i = 0; i < 16; i++) M[i] = dv.getInt32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = ff(B, C, D); g = i; }
      else if (i < 32) { F = gg(B, C, D); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = hh(B, C, D); g = (3 * i + 5) % 16; }
      else { F = ii(B, C, D); g = (7 * i) % 16; }
      F = (F + A + K[i] + M[g]) | 0;
      A = D; D = C; C = B;
      B = (B + rot(F, S[(i >> 4) * 4 + (i & 3)])) | 0;
    }
    a0 = (a0 + A) | 0; b0 = (b0 + B) | 0; c0 = (c0 + C) | 0; d0 = (d0 + D) | 0;
  }
  const out = new Uint8Array(16);
  const odv = new DataView(out.buffer);
  odv.setInt32(0, a0, true); odv.setInt32(4, b0, true);
  odv.setInt32(8, c0, true); odv.setInt32(12, d0, true);
  return out;
}

// 防止 TAIL_SECTOR 未使用告警(保留导入:4KB 扇区是布局语义的一部分)
export const SECTOR_SIZE = TAIL_SECTOR;
