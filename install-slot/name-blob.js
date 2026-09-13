// tools/install-slot/name-blob.js —— 槽位显示名 blob 打包/解包(ES module,浏览器/Node 双端)。
// 字节格式与 main/meta_name.c 完全一致;测试向量在 test-extract.mjs 与
// tests/test_meta_name.c 中双向锁定(逐字节相同)。
//
// Blob 布局(槽位分区尾部最后 4KB sector,即 slot_offset + BLOB_OFFSET):
//   [0..3]  magic "MNAM"
//   [4]     name_len(1..NAME_MAX)
//   [5..5+len)  name 字节(仅可打印 ASCII 0x20..0x7E)
//   [5+len] checksum:对 [4..5+len) 全部字节做 XOR 折叠

export const NAME_MAX = 32;
export const BLOB_SECTOR = 0x1000;         // blob sector 大小(4KB)

// 按槽位分区大小计算 blob sector 距槽位起始的偏移(分区末尾 4KB)。
export function blobOffset(partSize) {
  return partSize - BLOB_SECTOR;
}

// 按槽位分区大小计算应用镜像字节上限(分区大小减去尾部 4KB blob sector)。
export function maxAppImageSize(partSize) {
  return partSize - BLOB_SECTOR;
}

const MAGIC = [0x4d, 0x4e, 0x41, 0x4d]; // "MNAM"

function isPrintableAscii(b) {
  return b >= 0x20 && b <= 0x7e;
}

// 打包显示名为 blob 字节;name 非法(非字符串/空/超 32/含非可打印 ASCII)返回 null。
export function packNameBlob(name) {
  if (typeof name !== "string" || name.length === 0 || name.length > NAME_MAX) return null;
  const out = new Uint8Array(6 + name.length);
  out.set(MAGIC, 0);
  out[4] = name.length;
  for (let i = 0; i < name.length; i++) {
    const b = name.charCodeAt(i);
    if (!isPrintableAscii(b)) return null;
    out[5 + i] = b;
  }
  let x = 0;
  for (let i = 4; i < 5 + name.length; i++) x ^= out[i];
  out[5 + name.length] = x;
  return out;
}

// 解包 blob;任何校验失败(魔术/长度/可打印性/checksum)返回 null(视为无 blob)。
// buf 可只含 blob 前若干字节(固件侧只读 64B)。
export function unpackNameBlob(buf) {
  if (!buf || buf.length < 6) return null;
  for (let i = 0; i < 4; i++) {
    if (buf[i] !== MAGIC[i]) return null;
  }
  const len = buf[4];
  if (len < 1 || len > NAME_MAX) return null;
  if (buf.length < 6 + len) return null;
  let x = buf[4];
  for (let i = 5; i < 5 + len; i++) {
    if (!isPrintableAscii(buf[i])) return null;
    x ^= buf[i];
  }
  if (buf[5 + len] !== x) return null;
  let s = "";
  for (let i = 5; i < 5 + len; i++) s += String.fromCharCode(buf[i]);
  return s;
}

// UI 预填清洗:剔除非可打印 ASCII,截断到 NAME_MAX;剔完为空返回 ""(调用方不写 blob)。
export function sanitizeDisplayName(s) {
  let out = "";
  for (const ch of String(s ?? "")) {
    const b = ch.codePointAt(0);
    if (b >= 0x20 && b <= 0x7e) out += ch;
    if (out.length >= NAME_MAX) break;
  }
  return out;
}
