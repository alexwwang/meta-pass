// install-slot/slot-backup.js —— 子固件备份/恢复纯逻辑(ES module,浏览器/Node 双端)。
// 数据格式与设备分区布局解耦:只要新固件的"app image + 4KB tail metadata sector"槽位结构不变,
// 无论分区大小/偏移如何调整,备份都能按 manifest 内的长度信息自适应刷回。
//
// 备份包(zip)内容,每个 slot 最多三份文件 + 一份校验清单:
//   slot{N}_firmware.bin        app image(image_len 字节;空槽无此文件)
//   slot{N}_extra.bin           slot 内 tail sector 之后的额外存储数据(可选,全 0xFF 时省略)
//   slot{N}_tail.bin            4KB metadata sector(MSIG/MAEG/MNAM;全 0xFF 时省略)
//   manifest.json               全部文件的 sha256 + 长度 + 来源信息(唯一事实来源)
//
// 恢复顺序(必须):firmware → extra → tail。tail sector 必须最后写,
// 避免 esptool"每写必擦整扇区"破坏先写入的数据(见 docs/BUGS.md BUG-03)。

import { espImageLength } from "./extract-app-image.js";
import { TAIL_SECTOR } from "./name-blob.js";

export const MANIFEST_NAME = "manifest.json";
export const MANIFEST_VERSION = 1;
export const EMPTY_BYTE = 0xff;

export function firmwareFileName(slot) { return `slot${slot}_firmware.bin`; }
export function extraFileName(slot)   { return `slot${slot}_extra.bin`; }
export function tailFileName(slot)    { return `slot${slot}_tail.bin`; }

// ===== 备份侧 =====

// 判断一块数据是否"全 0xFF"(设备擦除态;尾部少量非 0xFF 视为有数据)。
export function isAllFF(u8) {
  for (let i = 0; i < u8.length; i++) {
    if (u8[i] !== EMPTY_BYTE) return false;
  }
  return true;
}

// 从整个 slot 的原始读取数据(imageLen + tail + extra 已按 app 语义解析)中切出三份文件。
// slotData: 从 slot 起始地址读出的完整分区字节(长度 = 分区大小)。
// 返回 { firmware, extra, tail, imageLen, tailOffset } 或 null(空槽:头 24B 全 0xFF)。
export function sliceSlotBackup(slotData) {
  if (slotData.length < 24 || isAllFF(slotData.subarray(0, 24))) return null;

  // app image 长度按 ESP 镜像格式解析(与设备 esp_image_verify 同语义);
  // 头部损坏(非 0xE9/坏 segment 表)时按"无法识别"处理,调用方决定是否强制全量备份。
  let imageLen;
  try {
    imageLen = espImageLength(slotData, 0);
  } catch {
    const err = new Error("slot data is not a valid ESP app image and not erased");
    err.code = "BAD_IMAGE";
    throw err;
  }
  const tailOffset = Math.ceil(imageLen / TAIL_SECTOR) * TAIL_SECTOR;
  const tail = slotData.subarray(tailOffset, tailOffset + TAIL_SECTOR);
  const extra = slotData.subarray(tailOffset + TAIL_SECTOR);

  return {
    imageLen,
    tailOffset,
    firmware: slotData.subarray(0, imageLen),
    tail: isAllFF(tail) ? null : tail.slice(),
    extra: extra.length > 0 && !isAllFF(extra) ? extra.slice() : null,
  };
}

// 生成 manifest 对象。entries: [{ name, sha256, bytes }...](不含 manifest 自身)。
export function buildManifest({ slot, entries, notes }) {
  return {
    manifest_version: MANIFEST_VERSION,
    kind: "meta-pass-slot-backup",
    created_utc: new Date().toISOString(),
    slots: [
      {
        slot,
        notes: notes || undefined,
        files: entries,
      },
    ],
  };
}

// ===== 恢复侧 =====

// 解析 manifest:结构/版本/kind 校验;返回 { manifestVersion, slots: [{slot, files:[{name, sha256, bytes}]}] }。
export function parseManifest(json) {
  const m = typeof json === "string" ? JSON.parse(json) : json;
  if (!m || m.kind !== "meta-pass-slot-backup") {
    throw new Error("not a meta-pass slot backup manifest (kind mismatch)");
  }
  if (m.manifest_version !== MANIFEST_VERSION) {
    throw new Error(`unsupported manifest_version ${m.manifest_version} (expected ${MANIFEST_VERSION})`);
  }
  if (!Array.isArray(m.slots) || m.slots.length === 0) {
    throw new Error("manifest has no slot entries");
  }
  for (const s of m.slots) {
    if (!Number.isInteger(s.slot) || s.slot < 0) throw new Error("manifest slot index invalid");
    if (!Array.isArray(s.files)) throw new Error(`manifest slot ${s.slot} has no files array`);
    for (const f of s.files) {
      if (typeof f.name !== "string" || typeof f.sha256 !== "string" || !Number.isInteger(f.bytes)) {
        throw new Error(`manifest file entry malformed in slot ${s.slot}: ${JSON.stringify(f)}`);
      }
      if (!/^[0-9a-f]{64}$/.test(f.sha256)) {
        throw new Error(`manifest sha256 malformed for ${f.name}`);
      }
    }
  }
  return m;
}

// 取指定 slot 的文件清单;没有该 slot 时返回 []。
export function manifestFilesForSlot(manifest, slot) {
  const s = manifest.slots.find((x) => x.slot === slot);
  return s ? s.files : [];
}

// 恢复前置检查:备份内容是否能放进目标槽位(自适应性核心)。
// partSize: 目标设备该 slot 的分区大小。返回 { ok, reason? }。
// 规则:所有文件长度之和 ≤ partSize,且 firmware ≤ partSize - 4KB(tail sector 必须放得下)。
export function checkRestoreFit(files, partSize) {
  const total = files.reduce((n, f) => n + f.bytes, 0);
  if (total > partSize) {
    return { ok: false, reason: `backup needs ${total} bytes, slot holds ${partSize} bytes` };
  }
  const fw = files.find((f) => f.name.endsWith("_firmware.bin"));
  if (fw && fw.bytes > partSize - TAIL_SECTOR) {
    return {
      ok: false,
      reason: `firmware ${fw.bytes} bytes leaves no room for the ${TAIL_SECTOR}-byte metadata sector (slot holds ${partSize})`,
    };
  }
  return { ok: true };
}

// 恢复写入顺序:firmware → extra → tail(绝对顺序,见文件头注释)。
// files: 该 slot 的 manifest 文件清单;返回 [{ name, order, why }]。
export function restoreOrder(files) {
  const rank = (name) => (name.endsWith("_firmware.bin") ? 0 : name.endsWith("_extra.bin") ? 1 : 2);
  return [...files].sort((a, b) => rank(a.name) - rank(b.name));
}

// ===== 残留清理(槽位级)=====

// 均匀取探针偏移(每处读 24B 即可判断该处是否有非 FF 数据)。
// 返回去重后的绝对偏移数组;size < 24 时返回 null(区域太小无法探针,
// 调用方应直接整体清理——写 FF 的开销可忽略);size ≤ 0 返回 []。
export function probeOffsets(start, size, count = 8) {
  if (size <= 0) return [];
  if (size < 24) return null;
  const n = Math.max(2, Math.min(count, Math.floor(size / 4096) + 1));
  const offs = [];
  for (let i = 0; i < n; i++) {
    offs.push(start + Math.floor((i * (size - 24)) / (n - 1)));
  }
  return [...new Set(offs)];
}
