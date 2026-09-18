// tools/install-slot/test-launcher-upgrade.mjs —— launcher 升级纯逻辑测试(Node)。
// 运行:node tools/install-slot/test-launcher-upgrade.mjs(已接入 tools/validate.sh --static)
import assert from "node:assert/strict";
import {
  parsePartitionTable,
  comparePartitionTables,
  upgradeWritePlan,
  checkUpgradeBundle,
  isErasedTable,
  isLegacyFactoryLayout,
  migrationErasePlan,
  slotHasData,
  PARTITION_TABLE_READ_SIZE,
  packUpgradeContainer,
  unpackUpgradeContainer,
  parseUpgradeArtifact,
  HYBRID_MAGIC,
} from "../../install-slot/launcher-upgrade.js";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..");
const BUILD = join(REPO, "build");

// ---- PASS 1: 升级写入计划 = 四项最小写入集,地址与分区表契约一致 ----
const plan = upgradeWritePlan();
assert.deepEqual(
  plan.map((s) => [s.name, s.offset]),
  [
    ["bootloader.bin", 0x0],
    ["partition-table.bin", 0x8000],
    ["FoloToy-AI-Passport.bin", 0x10000],
    ["ota_data_initial.bin", 0x7fe000],
  ],
);
// 升级契约:计划中不允许出现任何数据区地址
const FORBIDDEN = [0x9000, 0x356000, 0x180000, 0x360000, 0x560000];
for (const step of plan) {
  for (const off of FORBIDDEN) {
    assert.ok(
      step.offset !== off,
      `write plan must never touch data region 0x${off.toString(16)}`,
    );
  }
}
console.log("PASS 1: write plan = bootloader/table/app/otadata at contract offsets; no data regions");

// ---- PASS 2: 真实构建产物的分区表解析(含 MD5 marker 校验) ----
const bundleTable = new Uint8Array(readFileSync(join(BUILD, "partition_table/partition-table.bin")));
const table = parsePartitionTable(bundleTable);
const labels = table.map((p) => p.label);
assert.deepEqual(
  labels,
  ["nvs", "phy_init", "factory", "ota_0", "cardid", "ota_1", "ota_2", "otadata"],
);
const byLabel = Object.fromEntries(table.map((p) => [p.label, p]));
assert.equal(byLabel.factory.offset, 0x10000);
assert.equal(byLabel.nvs.offset, 0x9000);
assert.equal(byLabel.cardid.offset, 0x356000);
assert.equal(byLabel.otadata.offset, 0x7fe000);
console.log("PASS 2: real partition table parses with MD5 marker (8 partitions, contract offsets)");

// ---- PASS 3: 设备读回表与升级包表逐字节比对 ----
// 设备读回 = 整 4KB 扇区(表 + 0xFF 填充)
const deviceTable = new Uint8Array(PARTITION_TABLE_READ_SIZE).fill(0xff);
deviceTable.set(bundleTable);
assert.deepEqual(comparePartitionTables(deviceTable, bundleTable), { ok: true });
// 篡改表数据区一个字节 → 拒绝
const tampered = new Uint8Array(bundleTable);
tampered[0x30] ^= 0xff;
const bad = comparePartitionTables(deviceTable, tampered);
assert.equal(bad.ok, false);
assert.match(bad.reason, /differs at byte 48/);
// 设备读回太短 → 拒绝
assert.equal(comparePartitionTables(new Uint8Array(0x800), bundleTable).ok, false);
// 升级包表为空 → 拒绝
assert.equal(comparePartitionTables(deviceTable, new Uint8Array(0)).ok, false);
console.log("PASS 3: byte-compare identical OK; tampered/short/empty rejected with reason");

// ---- PASS 4: 升级包完整性(真实构建产物) ----
const files = new Map([
  ["bootloader.bin", new Uint8Array(readFileSync(join(BUILD, "bootloader/bootloader.bin")))],
  ["partition-table.bin", bundleTable],
  ["FoloToy-AI-Passport.bin", new Uint8Array(readFileSync(join(BUILD, "FoloToy-AI-Passport.bin")))],
  ["ota_data_initial.bin", new Uint8Array(readFileSync(join(BUILD, "ota_data_initial.bin")))],
]);
assert.deepEqual(checkUpgradeBundle(files), { ok: true });
// 缺文件
const missing = new Map(files);
missing.delete("bootloader.bin");
assert.equal(checkUpgradeBundle(missing).ok, false);
// app 魔数错误
const badMagic = new Map(files);
badMagic.set("FoloToy-AI-Passport.bin", new Uint8Array([0x00, 0x01, 0x02, 0x03]));
assert.equal(checkUpgradeBundle(badMagic).ok, false);
// app 超过 factory 分区
const oversized = new Map(files);
oversized.set("FoloToy-AI-Passport.bin", new Uint8Array(0x170001).fill(0xe9, 0, 1));
assert.equal(checkUpgradeBundle(oversized).ok, false);
console.log("PASS 4: bundle check passes on real artifacts; missing/bad-magic/oversized rejected");

// ---- PASS 5: 真实 app 大小在 factory 限额内(升级可行性的正向断言) ----
const appSize = files.get("FoloToy-AI-Passport.bin").length;
assert.ok(appSize > 0 && appSize <= 0x170000, `app ${appSize} must fit factory 0x170000`);
console.log(`PASS 5: real factory app ${appSize} bytes fits the 0x170000 partition`);

// ---- PASS 6: 设备分类与原厂机迁移(市场轻量包路径的安全前提) ----
// 原厂旧布局 fixture:FoloToy 单固件机(factory 3MB + recovery@0x700000,无 OTA)
function fakePartitionTable(entries) {
  const raw = new Uint8Array(0xc00).fill(0xff);
  const dv = new DataView(raw.buffer);
  let cursor = 0;
  for (const [magic, type, subtype, offset, size, label] of entries) {
    dv.setUint16(cursor, magic, true);
    raw[cursor + 2] = type;
    raw[cursor + 3] = subtype;
    dv.setUint32(cursor + 4, offset, true);
    dv.setUint32(cursor + 8, size, true);
    raw.set(new TextEncoder().encode(label), cursor + 12);
    cursor += 32;
  }
  return raw;
}
const legacyTable = fakePartitionTable([
  [0x50aa, 1, 1, 0x9000, 0x6000, "nvs"],
  [0x50aa, 1, 1, 0xf000, 0x1000, "phy_init"],
  [0x50aa, 0, 0, 0x10000, 0x300000, "factory"],
  [0x50aa, 1, 1, 0x356000, 0x4000, "cardid"],
  [0x50aa, 0, 0x20, 0x700000, 0x100000, "recovery"],
]);
const metaTable = fakePartitionTable([
  [0x50aa, 1, 1, 0x9000, 0x6000, "nvs"],
  [0x50aa, 0, 0, 0x10000, 0x170000, "factory"],
  [0x50aa, 0, 0x10, 0x180000, 0x1d6000, "ota_0"],
  [0x50aa, 1, 1, 0x356000, 0x4000, "cardid"],
  [0x50aa, 0, 0x11, 0x360000, 0x200000, "ota_1"],
]);
// 分类判定
assert.equal(isLegacyFactoryLayout(parsePartitionTable(legacyTable)), true, "legacy factory layout must be detected");
assert.equal(isLegacyFactoryLayout(parsePartitionTable(metaTable)), false, "meta-pass layout is not legacy");
assert.equal(isErasedTable(new Uint8Array(PARTITION_TABLE_READ_SIZE).fill(0xff)), true, "all-FF table = blank device");
assert.equal(isErasedTable(legacyTable), false);
// 空白机表与包表比对失败(0xFF ≠ 包内容),但分类为 blank → 页面放行
const freshDevice = new Uint8Array(PARTITION_TABLE_READ_SIZE).fill(0xff);
assert.equal(comparePartitionTables(freshDevice, bundleTable).ok, false);
// 擦除计划:0x180000 → 0x7FE000,跳过 cardid,永不触碰 NVS/factory/otadata
const plan2 = migrationErasePlan();
assert.equal(plan2.length, 2);
assert.deepEqual([plan2[0].offset, plan2[0].size], [0x180000, 0x356000 - 0x180000]);
assert.deepEqual([plan2[1].offset, plan2[1].size], [0x35a000, 0x7fe000 - 0x35a000]);
for (const r of plan2) {
  const end = r.offset + r.size;
  assert.ok(r.offset >= 0x180000 && end <= 0x7fe000, "erase plan inside slot region");
  assert.ok(!(r.offset < 0x35a000 && end > 0x356000), "erase plan must skip cardid");
  assert.ok(r.offset >= 0x10000 + 0x170000, "erase plan must not touch factory");
}
// 槽位数据检测:头 24B
assert.equal(slotHasData(new Uint8Array(24).fill(0xff)), false, "erased slot = no data");
const img = new Uint8Array(24);
img[0] = 0xe9;
assert.equal(slotHasData(img), true, "0xE9 image head = data present");
const junk = new Uint8Array(24).fill(0xff);
junk[23] = 0x01;   // 尾部一个非 FF 字节也算有数据(保守)
assert.equal(slotHasData(junk), true);
assert.equal(slotHasData(new Uint8Array(12)), false, "wrong size treated as no-data");
console.log("PASS 6: legacy-factory / blank classification; erase plan skips cardid & factory; slot-head detection");

// ---- PASS 7: 旧版 MPUP 容器(存量包兼容)—— 打包/解包往返 + 逐段 SHA-256 门禁 ----
let packedFromPass7;   // PASS 8 兼容分发用例复用
{
  // 构造最小但结构合法的四段文件(build/upgrade 四段 bin 已随单文件化退役,
  // 此处用确定性伪数据;checkUpgradeBundle 要求:分区表首条目 magic 合法、
  // app 镜像 0xE9 头,故伪数据按此构造)
  const files = new Map();
  files.set("bootloader.bin", new Uint8Array(0x1000).fill(0xab));
  const pt = new Uint8Array(0x1000).fill(0xff);
  pt[0] = 0xaa; pt[1] = 0x50;   // 首条目 magic(小端 0x50AA),其余条目 0xFF 终止
  files.set("partition-table.bin", pt);
  const app = new Uint8Array(0x2000).fill(0x12);
  app[0] = 0xe9;                 // ESP app image magic(checkUpgradeBundle 强制)
  files.set("FoloToy-AI-Passport.bin", app);
  files.set("ota_data_initial.bin", new Uint8Array(0x2000).fill(0xff));
  const packed = packUpgradeContainer(files);
  packedFromPass7 = packed;
  // 容器头:魔数 + header_size + 段数
  assert.equal(new TextDecoder().decode(packed.subarray(0, 6)), "MPUPV1");
  const dv = new DataView(packed.buffer);
  assert.equal(dv.getUint32(12, true), 4, "segment count = 4");
  assert.equal(dv.getUint32(8, true), 16 + 4 * 72, "header size = 16 + 4*72");
  // 往返:逐段与原始字节一致
  const unpacked = unpackUpgradeContainer(packed);
  for (const [name, data] of unpacked) {
    const orig = files.get(name);
    assert.ok(data.length === orig.length && data.every((b, i) => b === orig[i]), `${name} roundtrip`);
  }
  // 解包结果直接通过既有包完整性检查
  assert.equal(checkUpgradeBundle(unpacked).ok, true);
  // 篡改任一字节 → 对应段 sha256 门禁拒绝
  const tampered = packed.slice();
  tampered[tampered.length - 1] ^= 0xff;
  assert.throws(() => unpackUpgradeContainer(tampered), /sha256 mismatch/);
  // 坏魔数 / 过短文件 / 段数不符
  const badMagic = packed.slice(); badMagic[0] = 0x58;
  assert.throws(() => unpackUpgradeContainer(badMagic), /magic/);
  assert.throws(() => unpackUpgradeContainer(packed.subarray(0, 100)), /header exceeds|too small/);
  const badCount = packed.slice();
  new DataView(badCount.buffer).setUint32(12, 3, true);
  assert.throws(() => unpackUpgradeContainer(badCount), /segment count|header_size/);
  console.log("PASS 7: legacy MPUP container (compat) — roundtrip byte-exact, per-segment sha256 gate, malformed rejected");
}

// ---- PASS 8: 单文件混合格式(MPUPV2 指纹尾段)—— 解析/切片/篡改负例/兼容分发 ----
{
  // 构造一个结构合法的 hybrid 文件:本体 = bootloader(真产物)…分区表…app(真产物)…填充。
  // 用真实构建产物切片,保证 espImageLength/chip_id/魔数全部合法,不mock契约。
  const bl = new Uint8Array(readFileSync(join(BUILD, "bootloader/bootloader.bin")));
  const ptReal = new Uint8Array(readFileSync(join(BUILD, "partition_table/partition-table.bin")));
  const appReal = new Uint8Array(readFileSync(join(BUILD, "FoloToy-AI-Passport.bin")));
  const bodyLen = 0x10000 + appReal.length;
  const body = new Uint8Array(bodyLen).fill(0xff);   // 间隙与真实合并镜像同为擦除态
  body.set(bl, 0);
  body.set(ptReal, 0x8000);
  body.set(appReal, 0x10000);
  const f = body.length;
  const footer = new Uint8Array(44);
  footer.set(new TextEncoder().encode(HYBRID_MAGIC), 0);   // MPUPV2 + 2 NUL
  new DataView(footer.buffer).setUint32(8, f, true);
  // body sha256:复用 launcher-upgrade 内部的同步实现(通过 parseUpgradeArtifact 间接验证;
  // 这里用 node crypto 独立计算,双实现交叉验证)
  const { createHash } = await import("node:crypto");
  footer.set(createHash("sha256").update(body).digest(), 12);
  const hybrid = new Uint8Array(f + 44);
  hybrid.set(body); hybrid.set(footer, f);

  // 解析:kind=hybrid,四段齐全,切片与真实产物逐字节一致
  const parsed = parseUpgradeArtifact(hybrid);
  assert.equal(parsed.kind, "hybrid");
  assert.equal(parsed.body.length, f);
  assert.deepEqual([...parsed.files.keys()].sort(),
    ["FoloToy-AI-Passport.bin", "bootloader.bin", "ota_data_initial.bin", "partition-table.bin"]);
  assert.equal(parsed.files.get("bootloader.bin").length, bl.length, "bootloader slice = real artifact length");
  assert.equal(parsed.files.get("partition-table.bin").length, 0x1000);
  assert.equal(parsed.files.get("ota_data_initial.bin").length, 0x2000);
  assert.ok(parsed.files.get("ota_data_initial.bin").every((b) => b === 0xff), "otadata slice = erased state");
  // 切片内容与本体逐字节一致
  for (const [name, orig] of [["bootloader.bin", bl], ["FoloToy-AI-Passport.bin", appReal]]) {
    const data = parsed.files.get(name);
    assert.equal(data.length, orig.length, `${name} slice length`);
    assert.ok(data.every((b, i) => b === orig[i]), `${name} slice matches body`);
  }
  // 分区表切片 = 整 4KB:前 0xC00 与真实表一致,其余为 0xFF 填充
  const ptSlice = parsed.files.get("partition-table.bin");
  assert.ok(ptSlice.subarray(0, ptReal.length).every((b, i) => b === ptReal[i]), "pt slice head matches real table");
  assert.ok(ptSlice.subarray(ptReal.length).every((b) => b === 0xff), "pt slice tail erased");

  // 篡改 body 任意字节 → 指纹 sha256 门禁拒绝
  const tampered = hybrid.slice();
  tampered[100] ^= 0xff;
  assert.throws(() => parseUpgradeArtifact(tampered), /sha256 mismatch/);
  // 篡改 body_len 字段 → 长度门禁拒绝
  const badLen = hybrid.slice();
  new DataView(badLen.buffer).setUint32(f + 8, f + 1, true);
  assert.throws(() => parseUpgradeArtifact(badLen), /body_len mismatch/);
  // 旧 MPUP 容器仍走 mpup 分支(兼容分发)
  const mpupParsed = parseUpgradeArtifact(packedFromPass7);
  assert.equal(mpupParsed.kind, "mpup");

  console.log("PASS 8: hybrid single-file (44B MPUPV2 footer) — slices match body, otadata erased, tamper rejected, legacy MPUP compat");
}

console.log("All launcher-upgrade tests passed.");
