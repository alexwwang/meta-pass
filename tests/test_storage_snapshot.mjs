#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { buildDataArchiveZip } from "../install-slot/data-byte-archive.js";
import {
  STORAGE_SNAPSHOT_KIND,
  eligibleSnapshotPartitions,
  buildStorageSnapshotManifest,
  validateStorageSnapshotManifest,
  validateStorageSnapshotPayloads,
  buildStorageSnapshotZip,
  readStorageSnapshotZip,
  buildStorageSnapshotRestorePlan,
  assertSnapshotVersionCompatible,
  captureStorageSnapshot,
} from "../install-slot/storage-snapshot.js";

const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const host = { label: "ota_0", type: 0, subtype: 0x10, offset: 0x10000, size: 0x180000 };
const partitions = [
  host,
  { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  { label: "cardid", type: 1, subtype: 0x40, offset: 0xf000, size: 0x1000 },
  { label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000 },
  { label: "pool_0", type: 1, subtype: 0x42, offset: 0x1b0000, size: 0x20000 },
];
const eligible = eligibleSnapshotPartitions(partitions, host, []);
assert.deepEqual(eligible.map((p) => p.label), ["nvs", "store", "pool_0"],
  "snapshot includes allocated NVS/storage, but excludes running host firmware and cardid");
assert.deepEqual(eligibleSnapshotPartitions(partitions, host, [
  { offset: host.offset, size: host.size }, { offset: 0xf000, size: 0x1000 },
]).map((p) => p.label), ["nvs", "store", "pool_0"],
  "protected ranges may describe already-protected host/cardid partitions without invalidating the table");
const withProtected = [...partitions,
  { label: "secure", type: 1, subtype: 0x43, offset: 0x1d0000, size: 0x1000, protected: true },
  { label: "reported-host", type: 0, subtype: 0x10, offset: 0x1e0000, size: 0x1000, running_host: true },
];
assert.deepEqual(eligibleSnapshotPartitions(withProtected, host, []).map((p) => p.label),
  ["nvs", "store", "pool_0"], "device-reported protected flags exclude otherwise valid partitions");

const payload = new Uint8Array([1, 2, 3, 4]);
const manifest = buildStorageSnapshotManifest({
  createdUtc: "2026-10-14T00:00:00.000Z",
  metaPassVersion: "2.0.0",
  compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
  partitions: [
    { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000, extents: [] },
    {
      label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000,
      // The gap before this extent represents erased 0xFF bytes; it remains positional.
      extents: [{ file: "extents/store-000010.bin", offset: 0x10, bytes: payload.length, sha256: hash(payload) }],
    },
    { label: "pool_0", type: 1, subtype: 0x42, offset: 0x1b0000, size: 0x20000, extents: [] },
  ],
});
assert.equal(manifest.kind, STORAGE_SNAPSHOT_KIND);
assert.equal(manifest.manifest_version, 2);
assert.deepEqual(manifest.meta_pass, {
  system_version: "2.0.0",
  compatible_versions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
});
assert.equal(assertSnapshotVersionCompatible(manifest, "2.3.1"), true);
assert.throws(() => assertSnapshotVersionCompatible(manifest, "3.0.0"), /Reflash MetaPass 2.0.0/);
assert.throws(() => assertSnapshotVersionCompatible(manifest, undefined), /valid MetaPass version/);
const taggedVersion = "v0.2.2-35-gaa25c52";
const taggedManifest = buildStorageSnapshotManifest({
  metaPassVersion: taggedVersion,
  compatibleMetaPassVersions: { min_inclusive: taggedVersion, max_inclusive: taggedVersion },
  partitions: [{ label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x1000, extents: [] }],
});
assert.equal(assertSnapshotVersionCompatible(taggedManifest, taggedVersion), true,
  "git-describe version strings from ESP-IDF app descriptors are valid source versions");
assert.throws(() => assertSnapshotVersionCompatible(taggedManifest, "v0.2.2-34-gabcdef0"), /Reflash MetaPass/,
  "a different build is outside the conservative exact-source compatibility range");
const zip = await buildStorageSnapshotZip({
  manifest,
  payloads: new Map([["extents/store-000010.bin", payload]]),
}, async (bytes) => hash(bytes));
const restoredArchive = await readStorageSnapshotZip(zip, async (bytes) => hash(bytes));
assert.equal(restoredArchive.manifest.kind, STORAGE_SNAPSHOT_KIND);
assert.equal(restoredArchive.payloads.get("extents/store-000010.bin").length, payload.length);
const legacyLogicalZip = buildDataArchiveZip([{
  name: "manifest.json",
  data: new TextEncoder().encode(JSON.stringify({
    manifest_version: 2, kind: "meta-pass-slot-backup", complete: true,
    created_utc: "2026-10-14T00:00:00.000Z", slots: [], data: [],
  })),
}]);
await assert.rejects(
  () => readStorageSnapshotZip(legacyLogicalZip, async (bytes) => hash(bytes)),
  /expected a v2 storage snapshot/,
);
validateStorageSnapshotManifest(manifest, { runningHostPartition: host });
const restorePlan = buildStorageSnapshotRestorePlan(manifest, {
  targetPartitions: partitions, runningHostPartition: host,
});
assert.equal(restorePlan.partitionCount, 3);
assert.equal(restorePlan.totalEraseBytes, 0x6000 + 0x10000 + 0x20000);
assert.equal(restorePlan.partitions.find((p) => p.partition.label === "store").erase.bytes, 0x10000);
assert.equal(restorePlan.partitions.find((p) => p.partition.label === "store").extents[0].relativeOffset, 0x10);
assert.deepEqual(await validateStorageSnapshotPayloads(manifest, { "extents/store-000010.bin": payload }, async (b) => hash(b)),
  { partitionCount: 3, extentCount: 1 });

assert.throws(() => validateStorageSnapshotManifest({
  ...manifest,
  partitions: [{ ...manifest.partitions[0], label: "cardid" }],
}), /protected partition/);
assert.throws(() => validateStorageSnapshotManifest({
  ...manifest,
  partitions: [{ ...manifest.partitions[0], offset: host.offset, size: host.size, label: "ota_0", type: 0, subtype: 0x10 }],
}, { runningHostPartition: host }), /protected partition/);
// 当前分配与档案不等(同偏移不同大小 = re-carve 后的空间)→ 还原必须拒绝
assert.throws(() => buildStorageSnapshotRestorePlan(manifest, {
  targetPartitions: partitions.map((p) => p.label === "store" ? { ...p, size: p.size - 0x1000 } : p),
  runningHostPartition: host,
}), /conflicts with current allocation/);
assert.throws(() => buildStorageSnapshotManifest({
  metaPassVersion: "2.0.0",
  compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
  partitions: [
    { label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000, extents: [] },
    { label: "pool_0", type: 1, subtype: 0x42, offset: 0x1a8000, size: 0x10000, extents: [] },
  ],
}), /overlap/);
assert.throws(() => buildStorageSnapshotManifest({
  metaPassVersion: "2.0.0",
  compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
  partitions: [{ label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000,
    extents: [{ file: "../escape.bin", offset: 0, bytes: 4, sha256: hash(payload) }] }],
}), /invalid snapshot extent/);
assert.throws(() => buildStorageSnapshotManifest({
  metaPassVersion: "2.0.0",
  compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
  partitions: [{ label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000,
    extents: [{ file: "extents/unaligned.bin", offset: 1, bytes: 4, sha256: hash(payload) }] }],
}), /invalid snapshot extent/);
await assert.rejects(() => validateStorageSnapshotPayloads(manifest, {
  "extents/store-000010.bin": new Uint8Array([4, 3, 2, 1]),
}, async (b) => hash(b)), /SHA-256 mismatch/);
await assert.rejects(() => validateStorageSnapshotPayloads(manifest, {}, async (b) => hash(b)),
  /missing snapshot payload/);


assert.throws(() => validateStorageSnapshotManifest(manifest, {
  runningHostPartition: host,
  protectedRanges: [{ offset: 0x1a0000, size: 0x1000 }],
}), /protected partition/);
// 还原身份规则:live 表是 carve 记录的启动态物化视图,合法地小于档案
// (2026-10-16 真机:下载模式下 0x8000 表只剩 FIXED 项,记录仍拥有全部槽位),
// 缺项本身不是拒绝理由 —— 合并记录后的权威 inventory 必须能身份匹配每个
// 档案分区。记录不可解码(recordUnusable)时档案自成权威,允许直写自愈。
{
  const minimalLive = partitions.filter((p) => p === host || p.label === "cardid" || p.label === "nvs");
  assert.throws(() => buildStorageSnapshotRestorePlan(manifest, {
    targetPartitions: minimalLive, runningHostPartition: host,
  }), /is not a current device allocation/);
  const healingPlan = buildStorageSnapshotRestorePlan(manifest, {
    targetPartitions: minimalLive, runningHostPartition: host, recordUnusable: true,
  });
  assert.equal(healingPlan.partitionCount, 3, "recordUnusable: archive is the allocation authority");
  assert.deepEqual(healingPlan.unownedRestored, ["store", "pool_0"]);
  assert.equal(healingPlan.partitions.find((p) => p.partition.label === "store").partition.offset, 0x1a0000,
    "restore targets are the archive's recorded offsets, never the live view");
}
// carve 记录合并后的权威 inventory(表未物化槽位)必须身份匹配 → 正常通过
{
  const { mergeCarveAllocations } = await import("../install-slot/dynslot-record.js");
  const record = {
    slots: [
      { state: 1, kind: 0, offset: 0x1a0000, size: 0x10000, imageLen: 1, playId: 1, sha: new Uint8Array(32), name: "s" },
    ],
    data: [
      { playId: 2, offset: 0x1b0000, size: 0x20000, state: 0, subtype: 0x42, type: 1, label: "pool_0" },
    ],
  };
  // 与 manifest 同构的合成档案:store→ota 槽位、pool_0→DATA,验证合并后身份匹配
  const dynManifest = buildStorageSnapshotManifest({
    metaPassVersion: "2.0.0",
    compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
    partitions: [
      { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000, extents: [] },
      { label: "ota_0", type: 0, subtype: 0x10, offset: 0x1a0000, size: 0x10000, extents: [] },
      { label: "pool_0", type: 1, subtype: 0x42, offset: 0x1b0000, size: 0x20000, extents: [] },
    ],
  });
  const dynHost = { label: "factory", type: 0, subtype: 0, offset: 0x10000, size: 0x180000 };
  const baseLive = [
    dynHost,
    { label: "cardid", type: 1, subtype: 0x40, offset: 0xf000, size: 0x1000 },
    { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  ];
  const merged = mergeCarveAllocations(baseLive, record);
  const dynPlan = buildStorageSnapshotRestorePlan(dynManifest, {
    targetPartitions: merged, runningHostPartition: dynHost,
  });
  assert.equal(dynPlan.partitionCount, 3, "record-merged inventory satisfies identity rule");
  assert.deepEqual(dynPlan.unownedRestored, []);
}
assert.throws(() => buildStorageSnapshotRestorePlan(manifest, {
  targetPartitions: partitions.map((p) => p.label === "store"
    ? { label: "reused", type: 0, subtype: 0x11, offset: 0x1a0000, size: 0x8000 }
    : p),
  runningHostPartition: host,
}), /conflicts with current allocation/);
assert.throws(() => buildStorageSnapshotRestorePlan({
  ...manifest,
  partitions: [{ ...manifest.partitions[0], offset: host.offset, size: host.size, label: "ota_0", type: 0, subtype: 0x10 }],
}, { targetPartitions: partitions, runningHostPartition: host }), /overlaps protected live partition/);
// 档案缺分区不再是校验错误:档案即权威,还原按档案写(缺的分区只是不回滚)。
// 相应 plan 按档案现有的分区构建。
{
  const trimmed = { ...manifest, partitions: manifest.partitions.filter((p) => p.label !== "pool_0") };
  validateStorageSnapshotManifest(trimmed, { runningHostPartition: host });
  assert.equal(buildStorageSnapshotRestorePlan(trimmed, {
    targetPartitions: partitions, runningHostPartition: host,
  }).partitionCount, 2);
}
assert.throws(() => eligibleSnapshotPartitions([
  ...partitions,
  { label: "overlap", type: 1, subtype: 0x43, offset: 0x1a8000, size: 0x1000 },
], host), /overlapping live partition table/);

{
  const captureHost = { label: "factory", type: 0, subtype: 0, offset: 0x10000, size: 0x1000 };
  const capturePartitions = [
    captureHost,
    { label: "nvs", type: 1, subtype: 2, offset: 0x20000, size: 0x1000 },
    { label: "cardid", type: 1, subtype: 64, offset: 0x21000, size: 0x1000 },
    { label: "store", type: 1, subtype: 65, offset: 0x22000, size: 0x1000 },
  ];
  const captureDevice = {
    async storageInfo() {
      return { ok: true, meta_pass_version: "v0.2.2-35-gaa25c52", running_host: captureHost, partitions: capturePartitions };
    },
    async storageRead() { throw new Error("whole-partition reader should be selected"); },
    async storageReadPartition(partition) {
      const bytes = new Uint8Array(partition.size).fill(0xff);
      if (partition.label === "store") bytes[0x20] = 0x5a;
      return bytes;
    },
  };
  const captured = await captureStorageSnapshot(captureDevice, { sha256Hex: async bytes => hash(bytes) });
  assert.equal(captured.manifest.meta_pass.system_version, "v0.2.2-35-gaa25c52");
  assert.equal(captured.bytesRead, captured.totalBytes);
  assert.deepEqual(captured.manifest.partitions.map(p => p.label), ["nvs", "store"]);
  assert.equal(captured.manifest.partitions[1].extents[0].offset, 0);
  assert.equal(captured.payloads.get(captured.manifest.partitions[1].extents[0].file).length, 4096);
  assert.equal(captured.payloads.get(captured.manifest.partitions[1].extents[0].file)[0x20], 0x5a);
}

console.log("PASS storage snapshot manifest, protected ranges, sparse extents, layout compatibility, and payload integrity");
