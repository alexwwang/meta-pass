#!/usr/bin/env node
// tools/install-slot/test-extract.mjs —— extract-app-image.js 的 Node 自检。
// 运行:/usr/local/bin/node test-extract.mjs(在本目录下)。
// 构造最小合法 ESP 镜像(2 segment + 校验和 + hash),断言:
//   1. extractAppImage 输出长度正确;
//   2. 坏 magic 抛错;
//   3. Full 合并镜像(分区表 + 前缀)能定位 factory 应用。

import assert from "node:assert/strict";
import { extractAppImage, espImageLength, isFullImage } from "./extract-app-image.js";
import {
  NAME_MAX, BLOB_OFFSET, MAX_APP_IMAGE_SIZE,
  packNameBlob, unpackNameBlob, sanitizeDisplayName,
} from "./name-blob.js";

// 构造合法 ESP 应用镜像:24B 头 + 16B 扩展头 + 2 个 segment + 填充 + 1B 校验和 + 32B hash
function buildAppImage() {
  const seg0 = 100;
  const seg1 = 64;
  const bodyLen = 24 + 16 + (8 + seg0) + (8 + seg1); // 220
  let total = bodyLen;
  while (total % 16 !== 15) total++; // 填充到 %16==15
  total += 1;  // 校验和
  total += 32; // SHA-256
  const buf = new Uint8Array(total).fill(0xab);
  buf[0] = 0xe9;            // magic
  buf[1] = 2;               // segment_count
  buf[12] = 5; buf[13] = 0; // chip_id = ESP32-C3(小端)
  buf[23] = 1;              // hash_appended
  // segment 0:load_addr=0x3fc80000,data_len=seg0
  let off = 24 + 16;
  buf.set([0x00, 0x00, 0xc8, 0x3f, seg0, 0x00, 0x00, 0x00], off);
  off += 8 + seg0;
  // segment 1:load_addr=0x42000020,data_len=seg1
  buf.set([0x20, 0x00, 0x00, 0x42, seg1, 0x00, 0x00, 0x00], off);
  return buf;
}

// 1. 最小合法镜像:长度精确等于构造长度,原样返回
{
  const app = buildAppImage();
  assert.equal(espImageLength(app, 0), app.length, "espImageLength must equal constructed length");
  const img = extractAppImage(app);
  assert.equal(img.length, app.length, "extracted length mismatch");
  assert.equal(img.data.length, app.length);
  assert.deepEqual([...img.data], [...app], "app image bytes must pass through unchanged");
  assert.equal(img.source, "app image");
  console.log("PASS 1: minimal valid image (2 segments + checksum + hash) length =", img.length);
}

// 2. 坏 magic 必须抛错
{
  const bad = buildAppImage();
  bad[0] = 0x00;
  assert.throws(() => extractAppImage(bad), /magic/i, "bad magic must throw");
  console.log("PASS 2: bad magic rejected");
}

// 3. Full 合并镜像:0x8000 分区表 + 0x10000 factory 应用,能定位并解包
{
  const app = buildAppImage();
  const fullLen = 0x10000 + app.length;
  const full = new Uint8Array(fullLen).fill(0xff); // 未用区域 0xFF
  // 分区表条目(32B):magic AA 50,type=0(app),subtype=0(factory),offset=0x10000,size=0x300000
  const pt = 0x8000;
  full.set([0xaa, 0x50, 0x00, 0x00], pt);
  full.set([0x00, 0x00, 0x01, 0x00], pt + 4);  // offset = 0x10000 U32LE
  full.set([0x00, 0x00, 0x30, 0x00], pt + 8);  // size = 0x300000 U32LE
  full.set(app, 0x10000);
  assert.ok(isFullImage(full), "partition table magic must be detected");
  const img = extractAppImage(full);
  assert.equal(img.length, app.length, "extracted app length mismatch in full image");
  assert.deepEqual([...img.data], [...app], "extracted app bytes must match");
  assert.match(img.source, /full image/, "source should identify full image");
  console.log("PASS 3: full flash image -> factory app located at 0x10000, length =", img.length);
}

// ===== 4. 显示名 blob(name-blob.js,与 tests/test_meta_name.c 双向锁定)=====

// 期望字节序列与 C 侧 host test 逐字节一致(手工按格式算出,双向锁定)
const NAME_VECTORS = [
  {
    name: "Pocket Walkie",
    hex: "4d4e414d0d506f636b65742057616c6b696536",
  },
  {
    name: "Radar",
    hex: "4d4e414d05526164617241",
  },
  {
    // 恰好 32 字节边界名("0123456789"×3 + "01"),checksum 0x20
    name: "01234567890123456789012345678901",
    hex: "4d4e414d203031323334353637383930313233343536373839" +
         "30313233343536373839303120",
  },
];

function hexToBytes(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

{
  assert.equal(NAME_MAX, 32);
  assert.equal(BLOB_OFFSET, 0x1ff000);
  assert.equal(MAX_APP_IMAGE_SIZE, 0x1ff000);
  for (const { name, hex } of NAME_VECTORS) {
    const expected = hexToBytes(hex);
    const packed = packNameBlob(name);
    assert.ok(packed, `pack "${name}" must succeed`);
    assert.deepEqual([...packed], [...expected], `blob bytes for "${name}" must match C-side vector`);
    assert.equal(unpackNameBlob(packed), name, `roundtrip "${name}"`);
    // 尾部填充 0xFF(擦除态)不影响解包(固件侧只读前 64B)
    const padded = new Uint8Array(64).fill(0xff);
    padded.set(packed, 0);
    assert.equal(unpackNameBlob(padded), name, `unpack from padded sector prefix "${name}"`);
  }
  console.log("PASS 4: blob vectors match C-side bytes (Pocket Walkie / Radar / 32-char boundary)");
}

// 5. 坏 blob 一律视为无 blob
{
  const good = packNameBlob("Radar");
  const badMagic = Uint8Array.from(good);
  badMagic[0] = 0x00;
  assert.equal(unpackNameBlob(badMagic), null, "bad magic must be rejected");
  const badXor = Uint8Array.from(good);
  badXor[badXor.length - 1] ^= 0x01;
  assert.equal(unpackNameBlob(badXor), null, "bad checksum must be rejected");
  const len0 = Uint8Array.from(good);
  len0[4] = 0;
  assert.equal(unpackNameBlob(len0), null, "len 0 must be rejected");
  const len33 = Uint8Array.from(good);
  len33[4] = 33;
  assert.equal(unpackNameBlob(len33), null, "len 33 must be rejected");
  const nonPrintable = Uint8Array.from(good);
  nonPrintable[5] = 0x01;   // 重算 checksum,隔离可打印性检查
  nonPrintable[nonPrintable.length - 1] = 0x05 ^ 0x01 ^ 0x61 ^ 0x64 ^ 0x61 ^ 0x72;
  assert.equal(unpackNameBlob(nonPrintable), null, "non-printable byte must be rejected");
  assert.equal(unpackNameBlob(good.subarray(0, 5)), null, "truncated buffer must be rejected");
  assert.equal(unpackNameBlob(new Uint8Array(64).fill(0xff)), null, "erased sector has no blob");
  assert.equal(packNameBlob(""), null, "empty name must be rejected");
  assert.equal(packNameBlob("x".repeat(33)), null, "33-char name must be rejected");
  assert.equal(packNameBlob("héllo"), null, "non-ASCII name must be rejected");
  assert.equal(packNameBlob("a\tb"), null, "control char must be rejected");
  console.log("PASS 5: invalid blobs rejected (magic/checksum/len/printable/truncated)");
}

// 6. sanitizeDisplayName:剔除非可打印 ASCII,截断到 32
{
  assert.equal(sanitizeDisplayName("héllo wörld"), "hllo wrld");
  assert.equal(sanitizeDisplayName("雷达"), "");
  assert.equal(sanitizeDisplayName("x".repeat(60)).length, NAME_MAX);
  assert.equal(sanitizeDisplayName(" Pocket Walkie "), " Pocket Walkie ");
  console.log("PASS 6: sanitizeDisplayName strips non-printable and truncates to 32");
}

// 7. 应用镜像上限收紧到 0x1FF000(尾部 4KB 保留给 blob)
{
  // ESP 镜像总长恒为 16 的倍数:0x1FF000 是边界合法值,0x1FF010 是下一个超限值
  const buildSized = (target) => {
    let s0 = target - 96;
    for (let it = 0; it < 32; it++) {
      let total = 24 + 16 + 8 + s0;
      while (total % 16 !== 15) total++;
      total += 33;
      if (total === target) break;
      s0 += target - total;
    }
    const buf = new Uint8Array(target).fill(0xab);
    buf[0] = 0xe9; buf[1] = 1; buf[12] = 5; buf[13] = 0; buf[23] = 1;
    buf.set([0x00, 0x00, 0xc8, 0x3f, s0 & 0xff, (s0 >> 8) & 0xff, (s0 >> 16) & 0xff, (s0 >> 24) & 0xff], 40);
    return buf;
  };
  const okImg = buildSized(MAX_APP_IMAGE_SIZE);
  assert.equal(extractAppImage(okImg).length, MAX_APP_IMAGE_SIZE, "exactly 0x1FF000 must pass");
  const bigImg = buildSized(MAX_APP_IMAGE_SIZE + 0x10);
  assert.throws(() => extractAppImage(bigImg), /max app image size/, "0x1FF010 must be rejected");
  console.log("PASS 7: app image limit tightened to 0x1FF000 (blob sector reserved)");
}

console.log("All tests passed.");
