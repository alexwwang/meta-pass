// tools/install-slot/extract-app-image.js —— ESP 镜像解包纯函数(ES module,浏览器/Node 双端)。
// install-slot.html 以 <script type="module"> import;test-extract.mjs 直接用 node 跑测试。
// 常量与 main/meta_image.c 及分区表布局同一约定。
import { maxAppImageSize } from "./name-blob.js";

export const ESP_IMAGE_MAGIC = 0xE9;   // esp_image_header_t 魔数
export const ESP_CHIP_ID_ESP32C3 = 5;  // chip_id 的 ESP32-C3 取值
const ESP_HEADER_LEN = 24;             // 镜像头固定 24 字节
const ESP_EXT_HEADER_LEN = 16;         // 扩展头 16 字节(部分工具链插入)
const ESP_MAX_SEGMENTS = 16;           // segment 上限
export const SLOT_CAPACITY = 0x200000; // 单槽 2MB
const PARTITION_TABLE_OFFSET = 0x8000; // Full 镜像中分区表位置
const PARTITION_ENTRY_LEN = 32;
const PARTITION_MAGIC_LO = 0xAA;       // [0x8000] 小端低字节
const PARTITION_MAGIC_HI = 0x50;       // [0x8001]

export function hex(n) { return "0x" + n.toString(16); }

function u32le(buf, off) {
  return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
}

// ESP 镜像格式:头 24B(magic@0、segment_count@1、chip_id 小端@12、hash_appended@23),
// 随后每 segment 8B 头(load_addr 4B + data_len 4B)+ 数据;全部 segment 后填充到
// (相对镜像起点)%16==15,再 1B 校验和;hash_appended=1 时再 +32B。
// 按给定扩展头长度走 segment 表,返回镜像精确总长度;结构不合法时抛错。
function walkSegments(buf, start, extHdrLen) {
  const segCount = buf[start + 1];
  const hashAppended = buf[start + 23] === 1;
  let off = start + ESP_HEADER_LEN + extHdrLen;
  for (let i = 0; i < segCount; i++) {
    if (off + 8 > buf.length) {
      throw new Error(`Truncated image: segment ${i} header extends beyond end of data.`);
    }
    const dataLen = u32le(buf, off + 4);
    off += 8 + dataLen;
    if (off > buf.length) {
      throw new Error(`Truncated image: segment ${i} data (${dataLen} bytes) extends beyond end of data.`);
    }
  }
  while ((off - start) % 16 !== 15) {
    off++;
    if (off > buf.length) throw new Error("Truncated image: padding extends beyond end of data.");
  }
  off += 1; // 校验和字节
  if (hashAppended) off += 32;
  if (off > buf.length) {
    throw new Error("Truncated image: checksum/hash extends beyond end of data.");
  }
  return off - start;
}

// 校验镜像头并计算精确总长度;任何校验失败抛出具体英文错误。
// 部分工具链在 24B 头后插入 16B 扩展头,这里对两种布局做自动探测。
export function espImageLength(buf, start) {
  if (start + ESP_HEADER_LEN > buf.length) {
    throw new Error(`Truncated image: header at ${hex(start)} needs 24 bytes, only ${buf.length - start} available.`);
  }
  const magic = buf[start];
  if (magic !== ESP_IMAGE_MAGIC) {
    throw new Error(`Bad magic byte ${hex(magic)} at offset ${hex(start)} — expected 0xE9 (not an ESP app image).`);
  }
  const segCount = buf[start + 1];
  if (segCount === 0 || segCount > ESP_MAX_SEGMENTS) {
    throw new Error(`Invalid segment count ${segCount} (expected 1..${ESP_MAX_SEGMENTS}).`);
  }
  const chipId = buf[start + 12] | (buf[start + 13] << 8);
  if (chipId !== ESP_CHIP_ID_ESP32C3) {
    throw new Error(`Wrong chip id ${hex(chipId)} — expected ${hex(ESP_CHIP_ID_ESP32C3)} (ESP32-C3).`);
  }
  // 先按带 16B 扩展头解析(契约默认),失败则回退无扩展头布局
  let lastErr = null;
  for (const extHdrLen of [ESP_EXT_HEADER_LEN, 0]) {
    try {
      return walkSegments(buf, start, extHdrLen);
    } catch (err) {
      lastErr = err;
    }
  }
  throw lastErr;
}

// Full Flash 合并镜像识别:0x8000 处分区表 magic(0x50 0xAA 小端)
export function isFullImage(buf) {
  return buf.length > PARTITION_TABLE_OFFSET + 1
    && buf[PARTITION_TABLE_OFFSET] === PARTITION_MAGIC_LO
    && buf[PARTITION_TABLE_OFFSET + 1] === PARTITION_MAGIC_HI;
}

// 分区表条目 32B/条:type@+2、subtype@+3、offset U32LE@+4、size U32LE@+8;
// type=0 且 subtype=0 即 factory 应用。
function findFactoryPartition(buf) {
  for (let off = PARTITION_TABLE_OFFSET; off + PARTITION_ENTRY_LEN <= buf.length; off += PARTITION_ENTRY_LEN) {
    if (buf[off] !== PARTITION_MAGIC_LO || buf[off + 1] !== PARTITION_MAGIC_HI) break;
    if (buf[off + 2] === 0x00 && buf[off + 3] === 0x00) {
      return { offset: u32le(buf, off + 4), size: u32le(buf, off + 8) };
    }
  }
  return null;
}

// 统一入口:Full 镜像自动解包出应用镜像;应用单镜像直接校验。
// 两条路都校验 magic/chip_id/尺寸,返回可写入槽位的字节。
export function extractAppImage(buf, maxSize) {
  let appStart = 0;
  let source = "app image";
  if (isFullImage(buf)) {
    const part = findFactoryPartition(buf);
    if (!part) {
      throw new Error("Full flash image detected, but no factory app partition (type=0, subtype=0) found in the partition table.");
    }
    appStart = part.offset;
    source = `full image (factory app at ${hex(part.offset)})`;
  }
  const imgLen = espImageLength(buf, appStart);
  // 上限按槽位分区大小动态计算:slot 尾部最后 8KB 保留给签名 sector 和显示名 blob
  const limit = maxSize ?? maxAppImageSize(SLOT_CAPACITY);
  if (imgLen > limit) {
    throw new Error(`App image is ${imgLen} bytes — exceeds max app image size ${limit} bytes (the last 8 KB of the slot is reserved for the signature badge and display name).`);
  }
  // 签名徽章:检测 image_len 之后的 4K 对齐 sector 是否含 MSIG magic。
  // 签名扇区在 image_len 之外,需单独写入(安装器第三次 writeFlash)。
  let signature = null, signatureOffset = 0;
  const sigOff = Math.ceil(imgLen / 4096) * 4096;
  if (appStart + sigOff + 4 <= buf.length) {
    const magic = buf.subarray(appStart + sigOff, appStart + sigOff + 4);
    if (magic[0] === 0x4D && magic[1] === 0x53 && magic[2] === 0x49 && magic[3] === 0x47) {
      // "MSIG" — 签名扇区存在,提取 265 字节(magic + payload_len + 256B sig + 1B xor)
      signature = buf.slice(appStart + sigOff, appStart + sigOff + 265);
      signatureOffset = sigOff;
    }
  }
  return { data: buf.slice(appStart, appStart + imgLen), length: imgLen, source, signature, signatureOffset };
}
