// install-slot/name-blob.js —— 槽位显示名 blob 打包/解包(ES module,浏览器/Node 双端)。
// 字节格式与 main/meta_name.c 完全一致;测试向量在 test-extract.mjs 与
// tests/test_meta_name.c 中双向锁定(逐字节相同)。
//
// metadata sector 紧跟 app image(image_len 后 4K 对齐);sector 内布局:
//   [0..127]     MSIG 签名徽章预留区
//   [128..4055]  MAEG 彩蛋窗口
//   [4056..4095] MNAM 显示名窗口(40B)
// MNAM 窗口内变长 blob 右对齐存放:窗口最后一字节必须是 checksum,前部未用字节保持 0xFF。
//
// Blob 字节格式不变:
//   [0..3]  magic "MNAM"
//   [4]     name_len(1..NAME_MAX)
//   [5..5+len)  name 字节(仅可打印 ASCII 0x20..0x7E)
//   [5+len] checksum:对 [4..5+len) 全部字节做 XOR 折叠

export const NAME_MAX = 32;
export const TAIL_SECTOR = 0x1000;         // metadata sector 大小(4KB)
export const NAME_OFFSET = 0x0fd8;         // MNAM 窗口起点:4056 = 4096 - 40
export const NAME_RESERVE = 40;            // MNAM 固定窗口:40B(4056..4095)

// metadata sector 距槽位起始的偏移:紧跟 app image,4K 对齐。
export function tailSectorOffset(imageLen) {
  return Math.ceil(imageLen / TAIL_SECTOR) * TAIL_SECTOR;
}

// MNAM 40B 窗口起点(metadata sector + 4056)。
export function blobOffset(imageLen) {
  return tailSectorOffset(imageLen) + NAME_OFFSET;
}

// 按槽位分区大小计算应用镜像字节上限(分区大小减去单一 metadata sector)。
export function maxAppImageSize(partSize) {
  return partSize - TAIL_SECTOR;
}

const MAGIC = [0x4d, 0x4e, 0x41, 0x4d]; // "MNAM"

function isPrintableAscii(b) {
  return b >= 0x20 && b <= 0x7e;
}

// 打包显示名为紧凑 blob 字节;name 非法(非字符串/空/超 32/含非可打印 ASCII)返回 null。
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

// 解包紧凑 blob;任何校验失败(魔术/长度/可打印性/checksum)返回 null(视为无 blob)。
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

// 打包到 40B MNAM 窗口并右对齐:窗口最后一字节是 checksum,前部保持 0xFF。
export function packNameBlobTail(name) {
  const packed = packNameBlob(name);
  if (!packed) return null;
  const out = new Uint8Array(NAME_RESERVE).fill(0xff);
  out.set(packed, NAME_RESERVE - packed.length);
  return out;
}

// 解包 MNAM 窗口:只接受右对齐 blob。可传 40B 窗口,也可直接传整个 4KB sector。
export function unpackNameBlobTail(buf) {
  if (!buf) return null;
  const win = buf.length >= TAIL_SECTOR ? buf.subarray(NAME_OFFSET, TAIL_SECTOR) : buf;
  if (win.length < NAME_RESERVE) return null;
  const minStart = NAME_RESERVE - (6 + NAME_MAX); // 2
  const maxStart = NAME_RESERVE - (6 + 1);        // 33
  for (let start = minStart; start <= maxStart; start++) {
    const len = win[start + 4];
    if (len < 1 || len > NAME_MAX) continue;
    if (start + 6 + len !== NAME_RESERVE) continue; // checksum 必须落在窗口最后一字节
    const name = unpackNameBlob(win.subarray(start, NAME_RESERVE));
    if (name !== null) return name;
  }
  return null;
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
