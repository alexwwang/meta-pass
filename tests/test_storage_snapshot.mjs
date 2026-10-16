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
assert.throws(() => assertSnapshotVersionCompatible(manifest, "3.0.0"), /declared maximum/);
assert.throws(() => assertSnapshotVersionCompatible(manifest, undefined), /valid MetaPass version/);
const taggedVersion = "v0.2.2-35-gaa25c52";
const taggedManifest = buildStorageSnapshotManifest({
  metaPassVersion: taggedVersion,
  compatibleMetaPassVersions: { min_inclusive: taggedVersion, max_inclusive: taggedVersion },
  partitions: [{ label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x1000, extents: [] }],
});
assert.equal(assertSnapshotVersionCompatible(taggedManifest, taggedVersion), true,
  "git-describe version strings from ESP-IDF app descriptors are valid source versions");
assert.throws(() => assertSnapshotVersionCompatible(taggedManifest, "v1.0.0"), /v0 storage line/,
  "a different major version is a different storage generation");
assert.equal(assertSnapshotVersionCompatible(taggedManifest, "v0.2.2-36-gabcdef0"), true,
  "forward-compatible: a newer build in the same major line restores the snapshot");
assert.throws(() => assertSnapshotVersionCompatible(taggedManifest, "v0.2.2-34-gabcdef0"), /older than the snapshot's minimum/,
  "an older build than the snapshot source is still refused");
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
// live 与档案不等(同偏移不同大小 / relabel / re-carve 重用)一律放行:
// 还原 = 档案覆盖整机,label/subtype 是记录槽位序号的位置名,重雕后同
// 一片字节改名不构成拒绝理由;档案范围外的 live 字节原样保留,下次启动
// 按重建的记录重新物化表后即成为未分配空间。唯一红线是受保护分区。
{
  const shrunk = buildStorageSnapshotRestorePlan(manifest, {
    targetPartitions: partitions.map((p) => p.label === "store" ? { ...p, size: p.size - 0x1000 } : p),
    runningHostPartition: host,
  });
  assert.equal(shrunk.partitionCount, 3, "relabelled/resized live allocations do not block restore");
}
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
// 还原安全红线只有两条:受保护分区重叠、与身份不同的现有分配重叠。
// live 表是 carve 记录的启动态物化视图,合法地小于档案(2026-10-16 真机:
// 下载模式下 0x8000 表只剩 FIXED 项);记录可读但已不拥有档案分配、或记录
// 不可解码,都不是拒绝理由 —— 无冲突的未分配/已释放空间直接按档案身份写,
// store 分区写回后 carve 记录随档案自愈。
{
  const minimalLive = partitions.filter((p) => p === host || p.label === "cardid" || p.label === "nvs");
  const healingPlan = buildStorageSnapshotRestorePlan(manifest, {
    targetPartitions: minimalLive, runningHostPartition: host,
  });
  assert.equal(healingPlan.partitionCount, 3, "archive layout is authoritative; missing live allocations do not block restore");
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
{
  // re-carve 重用(relabel + 换大小)也不拒绝:档案权威,按档案范围写。
  const reused = buildStorageSnapshotRestorePlan(manifest, {
    targetPartitions: partitions.map((p) => p.label === "store"
      ? { label: "reused", type: 0, subtype: 0x11, offset: 0x1a0000, size: 0x8000 }
      : p),
    runningHostPartition: host,
  });
  assert.equal(reused.partitionCount, 3);
  assert.deepEqual(reused.unownedRestored, ["store"], "relabelled live slot is still owned; only the resized store is unowned");
}
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

// 版本兼容策略:向前兼容 —— 同 major 版本线内,新固件可还原旧档案;
// 低于源版本或跨 major(不同存储代际)才拒绝。旧归档把范围钉死为源版本
// 精确值(min===max)时同样按同 major 放行(2026-10-16 真机:v2.1.0 档案
// 还原到 v2.2.0 设备)。
{
  const legacyPinned = {
    meta_pass: {
      system_version: "v2.1.0-1475-gb74e9da-dirty",
      compatible_versions: { min_inclusive: "v2.1.0-1475-gb74e9da-dirty", max_inclusive: "v2.1.0-1475-gb74e9da-dirty" },
    },
  };
  assert.equal(assertSnapshotVersionCompatible(legacyPinned, "v2.2.0"), true,
    "newer same-major firmware restores an older exact-pinned snapshot");
  assert.equal(assertSnapshotVersionCompatible(legacyPinned, "v2.1.0-2000-gdeadbeef"), true,
    "same-line prerelease firmware restores an older exact-pinned snapshot");
  assert.throws(() => assertSnapshotVersionCompatible(legacyPinned, "v3.0.0"), /v2 storage line/);
  assert.throws(() => assertSnapshotVersionCompatible(legacyPinned, "v2.0.0"), /older than the snapshot's minimum/);
  assert.throws(() => assertSnapshotVersionCompatible(legacyPinned, null), /did not report a valid MetaPass version/);
  // 显式范围归档:max 之上拒绝,范围之内(含同 major 更新)放行。
  const ranged = {
    meta_pass: {
      system_version: "v2.1.0",
      compatible_versions: { min_inclusive: "v2.1.0", max_inclusive: "v2.99.99" },
    },
  };
  assert.equal(assertSnapshotVersionCompatible(ranged, "v2.2.0"), true);
  assert.throws(() => assertSnapshotVersionCompatible(ranged, "v3.0.0"), /declared maximum/);
}

console.log("PASS storage snapshot manifest, protected ranges, sparse extents, layout compatibility, and payload integrity");
