// DATA 分区 subtype 准入契约(2026-11-19 修复:留声/天天带被误拒)
//
// 背景:parseFirmwareManifest 的白名单原本只认 FAT(0x81)/SPIFFS(0x82)/
// LittleFS(0x83),其余 subtype 一律 fail-closed。实测商店里大量合法玩法
// 声明 ESP-IDF "undefined/any"(0x06)或厂商自定义段(0x40–0xFE)作为裸数据区
// (录音类玩法存原始字节,不需要文件系统),被误杀为 unsupported-partition。
//
// 本测试钉住修复后的语义:
//   - 0x06 与 0x40–0xFE 按 kind=data 准入(裸区,生命周期与 FS 分区相同)
//   - 0x03 coredump / 0x04 nvs_keys / 0x05 efuse 视为系统分区跳过
//   - FAT/SPIFFS/LittleFS 维持准入(回归)
//   - 其余未知 subtype(如 0x10)仍 fail-closed 硬拒
//   - 共享资源(nvs label、cardid、PHY)不进子固件 DATA 清单(回归)

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const { parseFirmwareManifest, parseDataPartitions } = await import(
  path.join(ROOT, "install-slot", "extract-app-image.js")
);

// 最小合法 ESP 镜像(同 test_phone_install.mjs 构造法)
function buildAppImage(seg0len = 100, seg1len = 64) {
  const bodyLen = 24 + 16 + (8 + seg0len) + (8 + seg1len);
  let total = bodyLen;
  while (total % 16 !== 15) total++;
  total += 1 + 32;
  const buf = new Uint8Array(total).fill(0xab);
  buf[0] = 0xe9; buf[1] = 2;
  buf[12] = 5; buf[13] = 0;                 // chip_id ESP32-C3
  buf[23] = 1;                              // hash_appended
  let off = 24 + 16;
  const dv = new DataView(buf.buffer);
  dv.setUint32(off, 0x3fc80000, true);
  dv.setUint32(off + 4, seg0len, true);
  off += 8 + seg0len;
  dv.setUint32(off, 0x42000020, true);
  dv.setUint32(off + 4, seg1len, true);
  const tail = createHash("sha256").update(buf.subarray(0, total - 32)).digest();
  buf.set(tail, total - 32);
  return buf;
}

// 构造带任意分区表的合并镜像。entries: [{type, subtype, label, offset, size}]
// 布局与真实固件一致:nvs@0x9000, phy_init@0xf000, factory@0x10000。
function buildMerged(entries) {
  const app = buildAppImage();
  const merged = new Uint8Array(0x10000 + app.length);
  merged[0] = 0xe9;
  merged[0x8000] = 0xaa; merged[0x8001] = 0x50;
  const dv = new DataView(merged.buffer);
  entries.forEach((e, i) => {
    const o = 0x8000 + i * 32;
    merged[o] = 0xaa; merged[o + 1] = 0x50;   // 每条目自带 magic,漏了会导致解析在第一条后 break
    merged[o + 2] = e.type;
    merged[o + 3] = e.subtype;
    dv.setUint32(o + 4, e.offset, true);
    dv.setUint32(o + 8, e.size, true);
    for (let j = 0; j < e.label.length && j < 16; j++) merged[o + 12 + j] = e.label.charCodeAt(j);
  });
  merged[0x10000] = 0xe9;
  merged.set(app, 0x10000);
  return merged;
}

const BASE = [
  { type: 1, subtype: 0x02, label: "nvs", offset: 0x9000, size: 0x6000 },
  { type: 1, subtype: 0x01, label: "phy_init", offset: 0xf000, size: 0x1000 },
  { type: 0, subtype: 0x00, label: "factory", offset: 0x10000, size: 0x300000 },
];
const DATA_AT = 0x310000;   // factory 之后,不与其它条目重叠

function withData(subtype, label, size = 0x100000) {
  return buildMerged(BASE.concat([{ type: 1, subtype, label, offset: DATA_AT, size }]));
}

// ── 0x06 undefined/any:合法裸数据区 ──────────────────────────────────
{
  const m = parseFirmwareManifest(withData(0x06, "echo_voice"));
  assert.equal(m.supported, true, "subtype 0x06 must be admitted as child DATA");
  assert.equal(m.reason, "ok");
  assert.equal(m.data.length, 1);
  assert.equal(m.data[0].label, "echo_voice");
  assert.equal(m.data[0].subtype, 0x06);
  assert.equal(m.data[0].required_size, 0x100000);
  assert.deepEqual(m.unsupported, []);
  console.log("PASS subtype 0x06 (undefined/any) admitted as raw child DATA");
}

// ── 0x40 与 0xFE 边界:厂商自定义段 ──────────────────────────────────
for (const [sub, label] of [[0x40, "regret"], [0x99, "custom99"], [0xfe, "topcustom"]]) {
  const m = parseFirmwareManifest(withData(sub, label));
  assert.equal(m.supported, true, `subtype 0x${sub.toString(16)} must be admitted`);
  assert.equal(m.data[0]?.label, label);
  console.log(`PASS subtype 0x${sub.toString(16)} (custom range) admitted as raw child DATA`);
}

// ── 0x03/0x04/0x05:系统分区声明,跳过而非拒绝 ───────────────────────
{
  const m = parseFirmwareManifest(buildMerged(BASE.concat([
    { type: 1, subtype: 0x03, label: "coredump", offset: DATA_AT, size: 0x10000 },
    { type: 1, subtype: 0x04, label: "nvs_keys", offset: DATA_AT + 0x10000, size: 0x1000 },
    { type: 1, subtype: 0x05, label: "efuse", offset: DATA_AT + 0x11000, size: 0x1000 },
  ])));
  assert.equal(m.supported, true, "system subtype declarations must be skipped, not rejected");
  assert.equal(m.data.length, 0);
  assert.deepEqual(m.unsupported, []);
  console.log("PASS subtype 0x03/0x04/0x05 (coredump/nvs_keys/efuse) skipped as system-owned");
}

// ── FAT/SPIFFS/LittleFS 维持准入(回归) ──────────────────────────────
for (const [sub, label] of [[0x81, "fat"], [0x82, "spiffs"], [0x83, "littlefs"]]) {
  const m = parseFirmwareManifest(withData(sub, label));
  assert.equal(m.supported, true, `subtype 0x${sub.toString(16)} regression`);
  console.log(`PASS subtype 0x${sub.toString(16)} (${label}) still admitted`);
}

// ── 未知 subtype 仍 fail-closed ─────────────────────────────────────
{
  const m = parseFirmwareManifest(withData(0x10, "mystery"));
  assert.equal(m.supported, false);
  assert.equal(m.reason, "unsupported-partition");
  assert.equal(m.unsupported[0].label, "mystery");
  console.log("PASS unknown subtype 0x10 still rejected fail-closed");
}

// ── 共享资源不进子固件 DATA(回归) ───────────────────────────────────
{
  const m = parseFirmwareManifest(buildMerged(BASE.concat([
    { type: 1, subtype: 0x02, label: "nvs", offset: 0x9000, size: 0x6000 },
    { type: 1, subtype: 0x81, label: "fat", offset: DATA_AT, size: 0x100000 },
  ])));
  assert.equal(m.supported, true);
  assert.deepEqual(m.data.map((d) => d.label), ["fat"], "shared nvs must not enter child DATA");
  console.log("PASS shared NVS/cardid/PHY stay out of child DATA");
}

// ── parseDataPartitions(旧清单解析器)与新闸门一致 ────────────────────
{
  const raw = parseDataPartitions(withData(0x40, "regret"));
  assert.deepEqual(raw.map((d) => [d.label, d.type]), [["regret", "raw"]],
    "parseDataPartitions must surface custom-range partitions as raw");
  const sys = parseDataPartitions(buildMerged(BASE.concat([
    { type: 1, subtype: 0x03, label: "coredump", offset: DATA_AT, size: 0x10000 },
  ])));
  assert.deepEqual(sys, [], "parseDataPartitions must skip system subtypes");
  console.log("PASS parseDataPartitions consistent with new gate (raw surfaced, system skipped)");
}

// ── required_size 计算对裸区与 FS 一致(app 4K 对齐 + DATA 4K 对齐) ──
{
  const m = parseFirmwareManifest(withData(0x06, "echo_voice", 0x100000));
  const appImg = 24 + 16 + (8 + 100) + (8 + 64);
  const align = (n) => Math.ceil(n / 0x1000) * 0x1000;
  assert.equal(m.required_size, align(appImg) + 0x100000);
  console.log("PASS required_size accounting unchanged for raw partitions");
}

// ── 0x02 非 "nvs" label:固件私有 NVS 区按子固件 DATA 准入(2026-11-19) ──
{
  const m = parseFirmwareManifest(withData(0x02, "keep"));
  assert.equal(m.supported, true, "private NVS (0x02 non-nvs label) must be admitted as child DATA");
  assert.equal(m.data[0].label, "keep");
  assert.equal(m.data[0].subtype, 0x02);
  console.log("PASS subtype 0x02 non-nvs label admitted as child-private DATA");
}

// ── 0x02 但 label 恰为 "nvs":仍是主机共享区,不进子固件清单(回归) ──
{
  const m = parseFirmwareManifest(withData(0x02, "nvs"));
  assert.equal(m.supported, true);
  assert.equal(m.data.length, 0, "label 'nvs' stays host-shared, not child DATA");
  console.log("PASS label 'nvs' remains host-shared (regression)");
}

// ── 保留 label(store/phy_init/factory/otadata):fail-closed reserved-label ──
{
  const m = parseFirmwareManifest(withData(0x40, "store"));
  assert.equal(m.supported, false);
  assert.equal(m.reason, "reserved-label", "reserved label must fail closed with its own reason");
  assert.equal(m.unsupported[0].reserved, true);
  for (const lbl of ["phy_init", "factory", "otadata"]) {
    const r = parseFirmwareManifest(withData(0x06, lbl));
    assert.equal(r.supported, false, `${lbl} must be rejected`);
    assert.equal(r.reason, "reserved-label");
  }
  console.log("PASS reserved labels (store/phy_init/factory/otadata) fail closed as reserved-label");
}

console.log("\nAll partition subtype gate tests passed.");
