// install-slot/mock-device.js —— 无硬件测试用的内存模拟设备(ES module,浏览器/Node 双端)。
//
// 设计文档: docs/assets/dynslot-usb-slot-page-design.md §10(mock 模式)。
// 为什么值得做:USB 页的交互(动态列表 → 删除 → 动态分配 → 刷新)必须能被人在
// 浏览器里点出来、也能被 host 测试跑出来,否则只能等真机才发现问题。mock 用与
// 生产流程**同一套**编解码(dynslot-record.js)存取字节,所以一次走查是真正的
// 字节级往返,而不是 UI 桩。
//
// 写语义对齐 esptool/ROM:writeFlash 先按数据覆盖范围擦除 4KB 扇区、再写字节
// (flash 只能 1→0,先擦后写才是真实顺序);readFlash 按 4KB 分块回调进度。
// 绝不提供"整片擦除"之外的捷径 —— 页面走的就是这两条原语。

import {
  encodeRecord,
  materializeTable,
  decodeRecord,
  carveValid,
  STORE_OFFSET,
  STORE_SECTOR_SIZE,
  PT_SIZE,
  SLOT_STATE,
  SLOT_KIND,
  DATA_STATE,
} from "./dynslot-record.js";

export const FLASH_SIZE = 0x800000;      // 8 MB(与真机一致)
export const TABLE_OFFSET = 0x8000;
export const TABLE_READ_SIZE = 0x1000;

// ---- 分区表(与 main/meta_carve.c 的 FIXED / POOL_PLACEHOLDER / legacy 同源) --

const FIXED = [
  { type: 1, subtype: 2, offset: 0x9000,   size: 0x6000,  label: "nvs" },
  { type: 1, subtype: 1, offset: 0xf000,   size: 0x1000,  label: "phy_init" },
  { type: 0, subtype: 0, offset: 0x10000,  size: 0x170000, label: "factory" },
  { type: 1, subtype: 2, offset: 0x356000, size: 0x4000,  label: "cardid" },
  { type: 1, subtype: 2, offset: 0x35a000, size: 0x6000,  label: "store" },
  { type: 1, subtype: 0, offset: 0x7fe000, size: 0x2000,  label: "otadata" },
];

const POOL_PLACEHOLDER = [
  { type: 1, subtype: 0x40, offset: 0x180000, size: 0x1d6000, label: "pool_0" },
  { type: 1, subtype: 0x40, offset: 0x360000, size: 0x49e000, label: "pool_1" },
];

// legacy v1.x 固定 3 槽表(= meta_carve.c LEGACY_OTA + 卡片头,无 store 条目)。
const LEGACY = [
  ...FIXED.filter((e) => e.label !== "store"),
  { type: 0, subtype: 0x10, offset: 0x180000, size: 0x1d6000, label: "ota_0" },
  { type: 0, subtype: 0x11, offset: 0x360000, size: 0x200000, label: "ota_1" },
  { type: 0, subtype: 0x12, offset: 0x560000, size: 0x29e000, label: "ota_2" },
].sort((a, b) => a.offset - b.offset);

function packTable(entries) {
  const out = new Uint8Array(PT_SIZE);
  out.fill(0xff);
  entries.forEach((e, i) => {
    const p = i * 32;
    out[p] = 0xaa; out[p + 1] = 0x50;
    out[p + 2] = e.type; out[p + 3] = e.subtype;
    for (let k = 0; k < 4; k++) {
      out[p + 4 + k] = (e.offset >>> (8 * k)) & 0xff;
      out[p + 8 + k] = (e.size >>> (8 * k)) & 0xff;
    }
    out.fill(0, p + 12, p + 28);                    // label 区整段清 0(= meta_pt_encode)
    for (let n = 0; n < 16 && n < e.label.length; n++) out[p + 12 + n] = e.label.charCodeAt(n);
    for (let k = 0; k < 4; k++) out[p + 28 + k] = 0;
  });
  const m = entries.length * 32;
  out[m] = 0xeb; out[m + 1] = 0xeb;
  out.fill(0xff, m + 2, m + 16);
  out.set(globalThis.__READFLASH_MD5__(out.subarray(0, m)), m + 16);
  return out;
}

/** dynslot 安全表(= meta_pt_safe;fresh 设备 0x8000 的出厂内容)。 */
export function safeTableBytes() {
  return packTable([...FIXED, ...POOL_PLACEHOLDER].sort((a, b) => a.offset - b.offset));
}

/** legacy 固定 3 槽表(= meta_pt_legacy)。 */
export function legacyTableBytes() {
  return packTable(LEGACY);
}

// ---- 种子数据 ---------------------------------------------------------------

/**
 * 合成的最小 ESP32-C3 应用镜像:24B 头(magic 0xE9 / 1 段 / chip_id 5 /
 * hash_appended=0)+ 8B 段头 + 0xA5 填充段 + 填充 + 校验字节。
 * 关键点:段数据用 0xA5(非 0x00/0xFF),使 extract-app-image 的 16B 扩展头
 * 猜测路径必然读到 0xA5A5A5A5 而越界回退,拿到正确的 0 偏移布局。
 */
export function makeFakeImage(dataLen = 0x1000) {
  const headerLen = 24;
  const segHdr = 8;
  let off = headerLen + segHdr + dataLen;
  while (off % 16 !== 15) off++;           // 段后填充到 (len%16)==15
  off += 1;                                // 校验和
  const img = new Uint8Array(off).fill(0xa5);
  img[0] = 0xe9;                           // magic
  img[1] = 1;                              // segment count
  img[12] = 5; img[13] = 0;                // chip_id = ESP32-C3
  img[23] = 0;                             // hash_appended = 0
  // 段头:load_addr=0x40380000,data_len
  const base = headerLen;
  for (let k = 0; k < 4; k++) img[base + k] = (0x40380000 >>> (8 * k)) & 0xff;
  for (let k = 0; k < 4; k++) img[base + 4 + k] = (dataLen >>> (8 * k)) & 0xff;
  img[img.length - 1] = 0x1f;              // 校验和字节(内容不参与长度解析)
  return img;
}

/** 种子 carve:1 个已装玩法 + 1 个空槽 + 1 条数据记录(覆盖全部 UI 分支)。 */
export function defaultSeedCarve(imageLen) {
  return {
    slots: [
      {
        kind: SLOT_KIND.APP, state: SLOT_STATE.VALID,
        offset: 0x180000, size: 0x1d6000,
        imageLen, playId: 0,
        sha: Uint8Array.from({ length: 32 }, (_, i) => (i * 3 + 1) & 0xff),
        name: "Demo Play",
      },
      {
        kind: SLOT_KIND.APP, state: SLOT_STATE.EMPTY,
        offset: 0x360000, size: 0x200000,
        imageLen: 0, playId: 0, sha: new Uint8Array(32), name: "",
      },
    ],
    data: [
      {
        playId: 42, offset: 0x700000, size: 0x10000,
        state: DATA_STATE.ARCHIVED, subtype: 0x41, type: 1, label: "assets",
      },
    ],
  };
}

/**
 * 把种子写进 flash:
 *   dyn    —— carved 表 @0x8000 + 有效记录 @0x35A000(2 槽)+ slot0 里的合成镜像
 *   fresh  —— 安全表 @0x8000,无记录(dynslot 出厂态)
 *   legacy —— legacy 3 槽表 @0x8000,无记录(固定槽位设备)
 */
export function seedMockFlash(flash, profile = "dyn") {
  const write = (offset, bytes) => flash.set(bytes, offset);
  if (profile === "fresh") {
    write(TABLE_OFFSET, safeTableBytes());
    return flash;
  }
  if (profile === "legacy") {
    write(TABLE_OFFSET, legacyTableBytes());
    const img = makeFakeImage();
    write(0x180000, img);                   // legacy 槽 0 放个可解析镜像
    return flash;
  }
  const img = makeFakeImage();
  const carve = defaultSeedCarve(img.length);
  if (!carveValid(carve)) throw new Error("seed carve invalid");
  const rec = encodeRecord({ seq: 1, slots: carve.slots, data: carve.data });
  if (!rec) throw new Error("seed record encode failed");
  const table = materializeTable(carve);
  write(TABLE_OFFSET, table);
  write(STORE_OFFSET, rec);                 // 记录 A;B 保持擦除态
  write(carve.slots[0].offset, img);
  return flash;
}

// ---- 设备对象 ----------------------------------------------------------------

const binStringToBytes = (s) => {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
  return out;
};

/**
 * 创建模拟设备。API 形状 = 页面用到的 esptool loader 子集:
 *   readFlash(offset, size, onChunk?) / writeFlash({fileArray, reportProgress})
 *   sync() / connect() / runStub() / transport
 * @param {{profile?: "dyn"|"fresh"|"legacy", flash?: Uint8Array}} opts
 */
export function createMockDevice(opts = {}) {
  const profile = opts.profile ?? "dyn";
  const flash = opts.flash ?? new Uint8Array(FLASH_SIZE).fill(0xff);
  if (!opts.flash) seedMockFlash(flash, profile);

  const mock = {
    isMock: true,
    profile,
    flash,
    // M4: mock transport stubs for resync path
    transport: {
      disconnect: async () => {},  // no-op for mock
    },
    chipName: `MOCK ESP32-C3 (${profile})`,

    async readFlash(offset, size, onChunk) {
      if (!Number.isInteger(offset) || !Number.isInteger(size) || offset < 0 ||
          size < 0 || offset + size > FLASH_SIZE) {
        throw new Error(`mock read out of range: ${offset}+${size}`);
      }
      const CHUNK = 4096;
      const out = new Uint8Array(size);
      let got = 0;
      while (got < size) {
        const n = Math.min(CHUNK, size - got);
        out.set(flash.subarray(offset + got, offset + got + n), got);
        got += n;
        if (onChunk) onChunk(out.subarray(got - n, got), got, size);
        await Promise.resolve();            // 交出事件循环,进度条可绘制
      }
      return out;
    },

    // N22: mock 忽略 compress/flashSize/flashMode/flashFreq 参数 —— esptool-js 在
    // 真机上用压缩减少传输字节,mock 直接写字节(内存 flash 无传输层)。压缩只
    // 影响传输效率不影响写入语义(地址/擦除/数据),mock 的验证目标(字节级往返)
    // 不受影响。跨扇区写入已正确:erase 范围 = floor(addr/4K)*4K .. ceil((addr+len)/4K)*4K。
    async writeFlash({ fileArray = [], reportProgress } = {}) {
      for (let idx = 0; idx < fileArray.length; idx++) {
        const f = fileArray[idx];
        const bytes = typeof f.data === "string" ? binStringToBytes(f.data) : new Uint8Array(f.data);
        const addr = f.address;
        if (addr < 0 || addr + bytes.length > FLASH_SIZE) {
          throw new Error(`mock write out of range: ${addr}+${bytes.length}`);
        }
        // esptool 语义:flash_begin 按数据覆盖范围擦除 4KB 扇区,再写数据。
        const first = Math.floor(addr / 4096) * 4096;
        const last = Math.ceil((addr + bytes.length) / 4096) * 4096;
        flash.fill(0xff, first, last);
        const STEP = 0x10000;
        let done = 0;
        while (done < bytes.length) {
          const n = Math.min(STEP, bytes.length - done);
          flash.set(bytes.subarray(done, done + n), addr + done);
          done += n;
          if (reportProgress) reportProgress(idx, done, bytes.length);
          await Promise.resolve();
        }
      }
      return { ok: true };
    },

    // 页面恢复路径用到的同步/重连原语:mock 无总线,直接成功。
    async sync() { return true; },
    async connect() { return true; },
    async runStub() { return true; },
    async eraseFlash() { throw new Error("mock: full-chip erase is not provided on purpose"); },

    /** 便捷读:当前 carve 记录(页面也可自己 readFlash + pickRecord)。 */
    readRecord() {
      const a = flash.subarray(STORE_OFFSET, STORE_OFFSET + 4096);
      const b = flash.subarray(STORE_OFFSET + STORE_SECTOR_SIZE, STORE_OFFSET + 2 * STORE_SECTOR_SIZE);
      return decodeRecord(a) ?? decodeRecord(b);
    },
    readTable() {
      return flash.subarray(TABLE_OFFSET, TABLE_OFFSET + PT_SIZE);
    },
  };
  return mock;
}

/**
 * profile 解析：`?mock` / `?mock=1` / `?mock=fresh` / `?mock=legacy` / `#mock`。
 * 未出现 mock 参数 → null（页面走真串口路径）。
 */
export function mockProfileFromLocation(loc) {
  const search = (loc && loc.search) || "";
  const hash = (loc && loc.hash) || "";
  const raw = new URLSearchParams(search).get("mock");
  const m = /(?:^|#)mock(?:=([^&]*))?/.exec(hash);
  if (raw === null && !m) return null;
  const v = String(raw !== null ? raw : (m && m[1] !== undefined ? m[1] : "1")).toLowerCase();
  if (v === "fresh" || v === "legacy") return v;
  return "dyn";   // 空 / 1 / true / dyn / 未知值 → 默认 dyn 档
}
