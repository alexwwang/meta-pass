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
validateStorageSnapshotManifest(manifest, { targetPartitions: partitions, runningHostPartition: host });
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
assert.throws(() => validateStorageSnapshotManifest(manifest, {
  targetPartitions: partitions.map((p) => p.label === "store" ? { ...p, size: p.size - 0x1000 } : p),
  runningHostPartition: host,
}), /incompatible/);
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
  targetPartitions: partitions,
  runningHostPartition: host,
  protectedRanges: [{ offset: 0x1a0000, size: 0x1000 }],
}), /protected partition/);
assert.throws(() => validateStorageSnapshotManifest({
  ...manifest,
  partitions: manifest.partitions.filter((p) => p.label !== "pool_0"),
}, { targetPartitions: partitions, runningHostPartition: host }), /every eligible allocated partition/);
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
  assert.equal(captured.manifest.partitions[1].extents[0].offset, 0x20);
  assert.deepEqual([...captured.payloads.get(captured.manifest.partitions[1].extents[0].file)], [0x5a]);
}

console.log("PASS storage snapshot manifest, protected ranges, sparse extents, layout compatibility, and payload integrity");
