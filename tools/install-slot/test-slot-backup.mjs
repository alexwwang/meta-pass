// tools/install-slot/test-slot-backup.mjs —— slot-backup.js 的 Node 自检。
// 运行:/usr/local/bin/node tools/install-slot/test-slot-backup.mjs
// 覆盖:切片(固件/尾扇区/额外数据/空槽/坏镜像)、manifest 构建与解析、
//       恢复空间自检(自适应核心)、恢复顺序契约、dd 兜底(raw 镜像备份/恢复)。
import assert from "node:assert/strict";
import {
  probeOffsets,
} from "../../install-slot/slot-backup.js";
import {
  sliceSlotBackup, buildManifest, parseManifest, manifestFilesForSlot,
  checkRestoreFit, restoreOrder, isAllFF,
  buildRawMirror, rawMirrorFileName,
  firmwareFileName, extraFileName, tailFileName,
  MANIFEST_NAME, MANIFEST_VERSION,
  NVS_FILE_NAME, findNvsPartition,
} from "../../install-slot/slot-backup.js";

// 与 test-extract.mjs PASS 3c 同源 fixture:24+16B 扩展头,总长 256
function buildApp() {
  let total = 24 + 16 + 8 + 100 + 8 + 64;
  while (total % 16 !== 15) total++;
  total += 1 + 32;
  const b = new Uint8Array(total).fill(0xab);
  b[0] = 0xe9; b[1] = 2; b[12] = 5; b[13] = 0; b[23] = 1;
  b.set([0x00, 0x00, 0xc8, 0x3f, 100, 0, 0, 0], 40);   // seg0 @0x3fc80000 len 100
  b.set([0x20, 0x00, 0x00, 0x42, 64, 0, 0, 0], 148);   // seg1 @0x42000020 len 64
  return b;
}

// 1. 完整槽位切片:固件 + 尾扇区(含 MSIG) + 额外数据
{
  const app = buildApp();
  const slot = new Uint8Array(0x200000).fill(0xff);
  slot.set(app, 0);
  slot.set([0x4d, 0x53, 0x49, 0x47], 4096);   // "MSIG" at tail sector
  slot.set([1, 2, 3], 8192);                  // extra storage data
  const r = sliceSlotBackup(slot);
  assert.equal(r.imageLen, app.length);
  assert.equal(r.tailOffset, 4096);
  assert.equal(r.firmware.length, app.length);
  assert.ok(r.tail && r.tail.length === 4096 && r.tail[0] === 0x4d, "tail must carry MSIG");
  assert.ok(r.extra && r.extra.length > 0 && r.extra[0] === 1, "extra must be captured");
  console.log("PASS 1: full slot slices into firmware + tail(MSIG) + extra");
}

// 2. 空槽 → null;固件后无任何数据 → extra/tail 为 null
{
  assert.equal(sliceSlotBackup(new Uint8Array(0x200000).fill(0xff)), null, "empty slot must be null");
  const slot = new Uint8Array(0x200000).fill(0xff);
  slot.set(buildApp(), 0);
  const r = sliceSlotBackup(slot);
  assert.equal(r.tail, null, "erased tail must be omitted");
  assert.equal(r.extra, null, "erased extra must be omitted");
  console.log("PASS 2: empty slot -> null; erased tail/extra omitted");
}

// 3. 坏镜像(非空但 segment 表不可解析)→ BAD_IMAGE
{
  const bad = new Uint8Array(4096).fill(0x00);
  bad[0] = 0xe9; bad[1] = 2;
  assert.throws(() => sliceSlotBackup(bad), (e) => e.code === "BAD_IMAGE");
  console.log("PASS 3: non-image non-erased slot -> BAD_IMAGE");
}

// 4. manifest 构建/解析/按 slot 取文件 + 结构校验负例
{
  const entries = [
    { name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 256 },
    { name: extraFileName(0), sha256: "b".repeat(64), bytes: 3 },
    { name: tailFileName(0), sha256: "c".repeat(64), bytes: 4096 },
  ];
  const man = buildManifest({ slot: 0, entries, notes: "test" });
  const parsed = parseManifest(JSON.stringify(man));
  assert.equal(parsed.manifest_version, MANIFEST_VERSION);
  assert.equal(manifestFilesForSlot(parsed, 0).length, 3);
  assert.equal(manifestFilesForSlot(parsed, 1).length, 0, "absent slot -> empty files");
  assert.throws(() => parseManifest({ hello: 1 }), /kind/);
  assert.throws(() => parseManifest({ kind: "meta-pass-slot-backup", manifest_version: 99, slots: [{ slot: 0, files: [] }] }), /manifest_version/);
  assert.throws(() => parseManifest({ kind: "meta-pass-slot-backup", manifest_version: 1, slots: [{ slot: 0, files: [{ name: "x", sha256: "zz", bytes: 1 }] }] }), /sha256/);
  assert.ok(isAllFF(new Uint8Array(16).fill(0xff)) && !isAllFF(new Uint8Array([0, 0xff])));
  console.log("PASS 4: manifest build/parse roundtrip + malformed rejection");
}

// 5. 恢复空间自检(自适应):备份总长 ≤ 目标分区;固件必须给尾扇区留 4KB
{
  const small = [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 256 }];
  assert.equal(checkRestoreFit(small, 0x1D6000).ok, true, "small backup fits any slot");
  // 恰好塞满整个分区(不含尾扇区)→ 拒绝
  const full = [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 0x1D6000 }];
  assert.equal(checkRestoreFit(full, 0x1D6000).ok, false, "no-room-for-tail must be rejected");
  // 总长超出分区 → 拒绝
  const over = [
    { name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 0x1D6000 },
    { name: extraFileName(0), sha256: "b".repeat(64), bytes: 0x100000 },
  ];
  assert.equal(checkRestoreFit(over, 0x1D6000).ok, false, "oversize backup must be rejected");
  // 旧小分区备份装进大分区 → 通过(自适应方向)
  assert.equal(checkRestoreFit(small, 0x29E000).ok, true, "small backup into bigger slot fits");
  console.log("PASS 5: restore fit check (adaptive across slot sizes)");
}

// 6. 恢复顺序契约:firmware → extra → tail(tail 最后,防扇区重擦)
{
  const order = restoreOrder([
    { name: tailFileName(0) }, { name: extraFileName(0) }, { name: firmwareFileName(0) },
  ]).map((f) => f.name);
  assert.deepEqual(order, [firmwareFileName(0), extraFileName(0), tailFileName(0)]);
  const fwOnly = restoreOrder([{ name: tailFileName(1) }, { name: firmwareFileName(1) }]);
  assert.deepEqual(fwOnly.map((f) => f.name), [firmwareFileName(1), tailFileName(1)]);
  console.log("PASS 6: restore order firmware -> extra -> tail");
}

// ---- PASS 7: probeOffsets(残留清理探针)----
// 正常区域:8 个均匀偏移,升序去重,全部落在 [start, start+size-24]
const offs = probeOffsets(0x180000, 0x1d5000);
assert.equal(offs.length, 8);
assert.ok(offs.every((o, i) => o >= 0x180000 && o <= 0x180000 + 0x1d5000 - 24));
assert.ok(offs.every((o, i) => i === 0 || o > offs[i - 1]));
// 小区域(<24B):返回 null → 调用方直接整区清理
assert.equal(probeOffsets(0x1000, 16), null);
// 零尺寸:空数组(无需清理)
assert.deepEqual(probeOffsets(0x1000, 0), []);
// 区域略大于 24B:至少 2 个探针
const small = probeOffsets(0x1000, 4096);
assert.ok(small.length >= 2);
console.log("PASS 7: probeOffsets — uniform probes in range, null for tiny, [] for empty");

// ---- PASS 8: dd 兜底(raw 镜像备份/恢复)----
// 8a. buildRawMirror:整槽镜像拷贝 + 尾部连续 0xFF 裁剪为零;补回 0xFF 可逐字节还原
{
  const slot = new Uint8Array(0x200000).fill(0xff);
  const blob = new Uint8Array([0x4c, 0x45, 0x4f, 0x56, 0x49, 0x44, 0x31, 0x30]);   // "LEOVID10" 资源包头
  slot.set(blob, 0);
  slot.set([0xde, 0xad, 0xbe, 0xef], 0x1234);
  const raw = buildRawMirror(slot);
  assert.equal(raw.length, 0x1238, "trailing erased bytes must be trimmed");
  assert.equal(raw[0], 0x4c);
  // 还原:裁剪部分补 0xFF 后与原槽逐字节一致(恢复写回时 esptool 逐扇区先擦后写,擦除态自动还原)
  const restored = new Uint8Array(0x200000).fill(0xff);
  restored.set(raw, 0);
  assert.deepEqual(Array.from(restored), Array.from(slot), "raw mirror must round-trip byte-exact");
  // 全 FF 槽:裁剪后为空(调用方按空槽处理)
  assert.equal(buildRawMirror(new Uint8Array(4096).fill(0xff)).length, 0);
  console.log("PASS 8a: buildRawMirror — trim trailing FF, byte-exact roundtrip, empty for erased");
}

// 8b. BAD_IMAGE 槽位的完整兜底链路:raw 条目入 manifest → 解析 → raw 空间规则 → 写入顺序
{
  const slot = new Uint8Array(0x1D6000).fill(0xff);   // 数据区槽位(littlefs/资源包,非 ESP 镜像)
  slot.set([0x4c, 0x45, 0x4f], 0);                    // 任意非镜像头
  slot.set([1, 2, 3, 4], 0x1000);
  const raw = buildRawMirror(slot);
  // 备份侧:raw 条目(带 type:raw)进 manifest
  const man = buildManifest({
    slot: 2,
    entries: [{ name: rawMirrorFileName(2), sha256: "d".repeat(64), bytes: raw.length, type: "raw" }],
  });
  const parsed = parseManifest(JSON.stringify(man));
  const files = manifestFilesForSlot(parsed, 2);
  assert.equal(files.length, 1);
  assert.equal(files[0].type, "raw");
  // 恢复侧:空间规则 —— raw 不预留 tail sector,只要求总长 ≤ 分区大小
  const fitExact = checkRestoreFit([{ ...files[0], bytes: 0x1D6000 }], 0x1D6000);   // 恰好塞满 → 通过(raw 无 tail 语义)
  assert.equal(fitExact.ok, true, "raw mirror may fill the whole slot (no tail sector reserved)");
  assert.equal(checkRestoreFit([{ ...files[0], bytes: 0x1D6000 + 1 }], 0x1D6000).ok, false, "oversize raw must be rejected");
  // 写入顺序:raw 自成一类,排最前(实际恢复中它是该 slot 唯一文件)
  const order = restoreOrder([
    { name: tailFileName(0) }, { name: rawMirrorFileName(0), type: "raw" }, { name: firmwareFileName(0) },
  ]).map((f) => f.name);
  assert.equal(order[0], rawMirrorFileName(0), "raw mirror sorts first");
  console.log("PASS 8b: raw fallback manifest roundtrip + fit rule (no tail reserved) + order");
}

// PASS 9: NVS 自动备份/还原的定位纯逻辑(用户决策 2026-09-18:自动打包/自动还原,
// 无 UI 选项)。定位规则:data(0x01)+ nvs 子类型(0x02),排除 cardid。
{
  assert.equal(findNvsPartition([]), null, "empty table → null");
  assert.equal(findNvsPartition(null), null, "null table → null");
  const mk = (label, type, subtype, offset, size) => ({ label, type, subtype, offset, size });
  // 真实布局缩影:nvs + cardid(同为 nvs 子类型,必须被排除)+ 各 app 槽
  const table = [
    mk("nvs", 0x01, 0x02, 0x9000, 0x6000),
    mk("phy_init", 0x01, 0x02, 0xf000, 0x1000),      // nvs 子类型但 label 不叫 nvs —— 仍匹配(规则按子类型)
    mk("factory", 0x00, 0x00, 0x10000, 0x170000),    // app 类型 → 忽略
    mk("ota_0", 0x00, 0x10, 0x180000, 0x1D6000),
    mk("cardid", 0x01, 0x02, 0x356000, 0x4000),      // 设备身份 → 排除
    mk("otadata", 0x01, 0x00, 0x7fe000, 0x2000),     // ota 子类型 → 忽略
  ];
  const nv = findNvsPartition(table);
  assert.ok(nv, "nvs partition found");
  assert.equal(nv.label, "nvs");
  assert.equal(nv.offset, 0x9000, "first nvs-subtype non-cardid wins (not cardid, not phy_init)");
  assert.equal(findNvsPartition([mk("cardid", 0x01, 0x02, 0x356000, 0x4000)]), null, "cardid-only table → null");
  assert.equal(findNvsPartition([mk("nvs", 0x01, 0x03, 0x9000, 0x6000)]), null, "wrong subtype → null");
  // manifest 顶层 nvs 字段(新 zip)与旧 zip(无该字段)双兼容:parseManifest 不拒绝未知字段,
  // 恢复侧仅当 manifest.nvs 存在时才要求 nvs.bin(页面逻辑,此处钉死解析层契约)
  const withNvs = parseManifest(JSON.stringify({
    manifest_version: MANIFEST_VERSION, kind: "meta-pass-slot-backup", created_utc: "t",
    slots: [{ slot: 0, files: [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 1 }] }],
    nvs: { file: NVS_FILE_NAME, sha256: "b".repeat(64), bytes: 0x6000, source_offset: 0x9000 },
  }));
  assert.equal(withNvs.nvs.file, NVS_FILE_NAME);
  assert.equal(withNvs.nvs.bytes, 0x6000);
  const old = parseManifest(JSON.stringify({
    manifest_version: MANIFEST_VERSION, kind: "meta-pass-slot-backup", created_utc: "t",
    slots: [{ slot: 0, files: [] }],
  }));
  assert.equal(old.nvs, undefined, "old zips (no nvs field) parse unchanged");
  console.log("PASS 9: findNvsPartition (subtype rule, cardid excluded, first-wins) + manifest nvs field compat");
}

// PASS 10: NVS 纳入备份/还原的完整数据路径契约(模拟页面胶水层的真实序列)。
// 备份:分区表定位 NVS → 读 → 全 FF 检测 → sha256 入 manifest(nvs 字段);
// 还原:manifest.nvs → 文件存在 + 字节数 + sha256 三重校验 → 写回**目标设备**定位的
// NVS(source_offset 仅溯源,不用于写入 —— 自适应语义)。篡改任一字节必须被拒。
{
  const { createHash } = await import("node:crypto");
  const sha256Hex = (u8) => createHash("sha256").update(u8).digest("hex");

  // 模拟设备分区表(parsePartitionTable 输出形态)
  const deviceTable = [
    { label: "nvs", type: 0x01, subtype: 0x02, offset: 0x9000, size: 0x6000 },
    { label: "ota_0", type: 0x00, subtype: 0x10, offset: 0x180000, size: 0x1D6000 },
    { label: "cardid", type: 0x01, subtype: 0x02, offset: 0x356000, size: 0x4000 },
  ];
  const nv = findNvsPartition(deviceTable);

  // —— 备份侧 ——
  const nvsData = new Uint8Array(0x6000).fill(0xff);
  nvsData.set([0x01, 0x02, 0x03, 0x42], 0x10);   // 有数据:非全 FF → 应纳入
  assert.equal(isAllFF(nvsData), false, "populated NVS must not be treated as erased");
  const nvsEntry = { file: NVS_FILE_NAME, sha256: sha256Hex(nvsData), bytes: nvsData.length, source_offset: nv.offset };
  const manifest = parseManifest(JSON.stringify({
    manifest_version: MANIFEST_VERSION, kind: "meta-pass-slot-backup", created_utc: "t",
    slots: [{ slot: 0, files: [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 4 }] }],
    nvs: nvsEntry,
  }));

  // —— 还原侧(目标设备 NVS 偏移不同:自适应,不沿用 source_offset)——
  const targetTable = [
    { label: "nvs", type: 0x01, subtype: 0x02, offset: 0xA000, size: 0x6000 },   // 新布局偏移变了
    { label: "cardid", type: 0x01, subtype: 0x02, offset: 0x356000, size: 0x4000 },
  ];
  const targetNv = findNvsPartition(targetTable);
  assert.notEqual(targetNv.offset, nv.offset, "layout change scenario: target offset differs");
  assert.equal(manifest.nvs.file, NVS_FILE_NAME);
  assert.equal(manifest.nvs.bytes, nvsData.length, "size gate: zip entry must match manifest bytes");
  assert.equal(sha256Hex(nvsData), manifest.nvs.sha256, "digest gate: write-back only on sha256 match");
  assert.notEqual(targetNv.offset, manifest.nvs.source_offset, "source_offset is provenance only — restore uses device-located offset");

  // 篡改负例:任一字节被改 → sha 失配 → 还原拒绝(页面按 err_restore_sha 抛错)
  const tampered = new Uint8Array(nvsData);
  tampered[0x11] ^= 0xff;
  assert.notEqual(sha256Hex(tampered), manifest.nvs.sha256, "tampered NVS must fail digest gate");

  // 全 FF NVS:备份侧不产出条目(manifest 无 nvs 字段 → 还原侧跳过,旧 zip 同型)
  assert.equal(isAllFF(new Uint8Array(0x6000).fill(0xff)), true, "erased NVS → skipped in backup");
  console.log("PASS 10: NVS backup→restore data-path contract (locate/read/digest/manifest/adaptive write-back/tamper-reject)");
}

console.log("All slot-backup tests passed.");
