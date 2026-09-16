#!/usr/bin/env node
// tools/install-slot/test-extract.mjs —— extract-app-image.js 的 Node 自检。
// 运行:/usr/local/bin/node test-extract.mjs(在本目录下)。
// 构造最小合法 ESP 镜像(2 segment + 校验和 + hash),断言:
//   1. extractAppImage 输出长度正确;
//   2. 坏 magic 抛错;
//   3. Full 合并镜像(分区表 + 前缀)能定位 factory 应用。
//
// 被测模块直接从仓库规范目录 install-slot/ 导入(单一份实现,见 docs/BUGS.md BUG-03)。

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { extractAppImage, espImageLength, isFullImage } from "../../install-slot/extract-app-image.js";
import {
  NAME_MAX, NAME_OFFSET, NAME_RESERVE,
  tailSectorOffset, blobOffset, maxAppImageSize,
  packNameBlob, unpackNameBlob, packNameBlobTail, unpackNameBlobTail,
  sanitizeDisplayName,
} from "../../install-slot/name-blob.js";

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

// 3b. 签名镜像:必须提取完整 4KB tail sector;未签名镜像 tailSector=null 但仍返回 offset
{
  const app = buildAppImage();
  const tailOff = tailSectorOffset(app.length);
  const signed = new Uint8Array(tailOff + 4096).fill(0xff);
  signed.set(app, 0);
  signed.set([0x4d, 0x53, 0x49, 0x47], tailOff); // "MSIG"
  const img = extractAppImage(signed);
  assert.equal(img.length, app.length, "signed image app length mismatch");
  assert.equal(img.tailSectorOffset, tailOff, "tail sector offset mismatch");
  assert.ok(img.tailSector, "signed image must carry full tail sector");
  assert.equal(img.tailSector.length, 4096, "tail sector must be exactly 4KB");

  const plain = extractAppImage(app);
  assert.equal(plain.tailSector, null, "unsigned image must not invent a tail sector");
  assert.equal(plain.tailSectorOffset, tailOff, "unsigned image still reports tail offset");
  console.log("PASS 3b: signed image keeps full 4KB tail sector; unsigned image reports offset only");
}

// 3c. 布局探测契约:纯 24B 头(总长 240)与 24B+16B 扩展头(总长 256)两种布局都精确收敛,
//     与 sign-firmware.sh / test_integration.c 三方同源(决策见 debugging-workflow.md §4)
{
  function buildLayout(withExt) {
    const seg0Off = withExt ? 40 : 24;
    let total = 24 + (withExt ? 16 : 0) + (8 + 100) + (8 + 64);
    while (total % 16 !== 15) total++;
    total += 1 + 32;
    const buf = new Uint8Array(total).fill(0xab);
    buf[0] = 0xe9;
    buf[1] = 2;
    buf[12] = 5; buf[13] = 0; // chip_id ESP32-C3
    buf[23] = 1;              // hash_appended
    buf.set([0x00, 0x00, 0xc8, 0x3f, 100, 0, 0, 0], seg0Off);        // seg0 @0x3fc80000, len 100
    buf.set([0x20, 0x00, 0x00, 0x42, 64, 0, 0, 0], seg0Off + 108);  // seg1 @0x42000020, len 64
    return buf;
  }
  const plain = buildLayout(false);
  const ext = buildLayout(true);
  assert.equal(plain.length, 240, "plain fixture total length changed");
  assert.equal(ext.length, 256, "ext fixture total length changed");
  assert.equal(espImageLength(plain, 0), 240, "plain 24B layout must resolve");
  assert.equal(espImageLength(ext, 0), 256, "16B-extended-header layout must resolve");
  // 负例:两种布局都走不通的截断镜像必须抛错(而不是静默取错布局)
  const truncated = ext.slice(0, 200);
  assert.throws(() => espImageLength(truncated, 0), /Truncated|segment|extends/i,
    "unresolvable image must throw");
  console.log("PASS 3c: layout probe — plain-24B→240, 24B+16B-ext→256; unresolvable throws");
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
  assert.equal(NAME_OFFSET, 4056);
  assert.equal(NAME_RESERVE, 40);
  assert.equal(tailSectorOffset(256), 4096);
  assert.equal(blobOffset(256), 4096 + 4056);
  assert.equal(blobOffset(0x1ff000), 0x1ff000 + 4056);
  assert.equal(maxAppImageSize(0x200000), 0x1ff000);
  for (const { name, hex } of NAME_VECTORS) {
    const expected = hexToBytes(hex);
    const packed = packNameBlob(name);
    assert.ok(packed, `pack "${name}" must succeed`);
    assert.deepEqual([...packed], [...expected], `blob bytes for "${name}" must match C-side vector`);
    assert.equal(unpackNameBlob(packed), name, `roundtrip "${name}"`);

    const tail = packNameBlobTail(name);
    assert.ok(tail, `pack tail "${name}" must succeed`);
    assert.equal(tail.length, 40);
    assert.deepEqual([...tail.subarray(40 - packed.length)], [...packed], `tail blob for "${name}" must be right-aligned`);
    assert.ok(tail.subarray(0, 40 - packed.length).every((b) => b === 0xff), `tail prefix for "${name}" must stay erased`);
    assert.equal(unpackNameBlobTail(tail), name, `tail roundtrip "${name}"`);

    const sector = new Uint8Array(4096).fill(0xff);
    sector.set(tail, NAME_OFFSET);
    assert.equal(unpackNameBlobTail(sector), name, `sector tail roundtrip "${name}"`);

    const forward = new Uint8Array(40).fill(0xff);
    forward.set(packed, 0);
    assert.equal(unpackNameBlobTail(forward), null, `forward-placed blob for "${name}" must be rejected`);
  }
  console.log("PASS 4: blob vectors match C-side bytes; MNAM tail window is right-aligned");
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

// 7. 应用镜像上限按槽位分区大小动态计算(尾部 4KB 保留给 blob)
{
  // 以 ota_1 槽位(0x200000)为例:上限 = 0x1FF000
  const slotSize = 0x200000;
  const limit = maxAppImageSize(slotSize);
  // ESP 镜像总长恒为 16 的倍数:limit 是边界合法值,limit+0x10 是下一个超限值
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
  const okImg = buildSized(limit);
  assert.equal(extractAppImage(okImg, limit).length, limit, "exactly at limit must pass");
  const bigImg = buildSized(limit + 0x10);
  assert.throws(() => extractAppImage(bigImg, limit), /max app image size/, "limit+0x10 must be rejected");
  // 验证小槽位(ota_0=0x1D6000)上限更小
  const ota0Limit = maxAppImageSize(0x1D6000);
  assert.ok(ota0Limit < limit, "ota_0 limit must be smaller than ota_1");
  console.log("PASS 7: app image limit dynamically computed per slot (blob sector reserved)");
}

// ===== 8. install-slot.html i18n 字典:en/zh 键集合一致,页面引用的键全部存在 =====
{
  const html = readFileSync(new URL("../../install-slot/install-slot.html", import.meta.url), "utf8");
  const scriptMatch = html.match(/<script type="module">([\s\S]*?)<\/script>/);
  assert.ok(scriptMatch, "module script not found in install-slot.html");
  const script = scriptMatch[1];

  // 提取 const I18N = { ... };(平衡花括号扫描,字典为纯数据可安全 eval)
  const start = script.indexOf("const I18N = {");
  assert.ok(start >= 0, "I18N dictionary not found");
  let depth = 0, end = -1;
  for (let i = script.indexOf("{", start); i < script.length; i++) {
    const ch = script[i];
    if (ch === "{") depth++;
    else if (ch === "}") { depth--; if (depth === 0) { end = i; break; } }
    else if (ch === '"' || ch === "'") { // 跳过字符串字面量,避免括号误判
      const q = ch;
      for (i++; i < script.length && script[i] !== q; i++) if (script[i] === "\\") i++;
    }
  }
  assert.ok(end > start, "I18N dictionary braces unbalanced");
  const I18N = eval("(" + script.slice(script.indexOf("{", start), end + 1) + ")");

  const enKeys = Object.keys(I18N.en).sort();
  const zhKeys = Object.keys(I18N.zh).sort();
  assert.deepEqual(zhKeys, enKeys, "zh key set must equal en key set");
  for (const lang of ["en", "zh"]) {
    for (const [k, v] of Object.entries(I18N[lang])) {
      assert.ok(typeof v === "string" && v.length > 0, `${lang}.${k} must be a non-empty string`);
    }
  }

  // HTML 中 data-i18n / data-i18n-ph 引用的键必须在字典中
  const htmlPart = html.slice(0, html.indexOf("<script"));
  const htmlKeys = new Set();
  for (const m of htmlPart.matchAll(/data-i18n(?:-ph)?="([^"]+)"/g)) htmlKeys.add(m[1]);
  for (const k of htmlKeys) {
    assert.ok(k in I18N.en, `data-i18n key "${k}" missing from en dictionary`);
  }

  // JS 中 t("key") 引用的键必须在字典中
  const tKeys = new Set();
  for (const m of script.matchAll(/\bt\("([^"]+)"/g)) tKeys.add(m[1]);
  for (const k of tKeys) {
    assert.ok(k in I18N.en, `t() key "${k}" missing from en dictionary`);
  }

  console.log(`PASS 8: i18n dictionaries consistent (${enKeys.length} keys; ${htmlKeys.size} data-i18n + ${tKeys.size} t() refs all present)`);
}

console.log("All tests passed.");
