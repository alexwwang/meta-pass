// install-slot/launcher-upgrade.js —— launcher 升级纯逻辑(ES module,浏览器/Node 双端)。
//
// 升级契约(2026-09 固化):meta-pass 分区结构已稳定,launcher 升级只允许写入:
//   0x0        bootloader.bin          (~21KB)
//   0x8000     partition-table.bin     (3KB,含 MD5 marker)
//   0x10000    FoloToy-AI-Passport.bin (factory app)
//   0x7FE000   ota_data_initial.bin    (8KB 擦除态,重置 OTA 选择,升级后回到 factory)
// 其余分区一律不写,设备上的数据原样保留:
//   nvs 0x9000(Wi-Fi 配置)、cardid 0x356000、ota_0/1/2(已装子固件)。
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
