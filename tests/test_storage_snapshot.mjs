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
const eligible = eligibleSnapshotPartitions(partitions, host);
assert.deepEqual(eligible.map((p) => p.label), ["nvs", "store", "pool_0"],
  "snapshot includes allocated NVS/storage, but excludes running host firmware and cardid");

const payload = new Uint8Array([1, 2, 3, 4]);
const manifest = buildStorageSnapshotManifest({
  createdUtc: "2026-10-14T00:00:00.000Z",
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
  partitions: [
    { label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000, extents: [] },
    { label: "pool_0", type: 1, subtype: 0x42, offset: 0x1a8000, size: 0x10000, extents: [] },
  ],
}), /overlap/);
assert.throws(() => buildStorageSnapshotManifest({
  partitions: [{ label: "store", type: 1, subtype: 0x41, offset: 0x1a0000, size: 0x10000,
    extents: [{ file: "../escape.bin", offset: 0, bytes: 4, sha256: hash(payload) }] }],
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

console.log("PASS storage snapshot manifest, protected ranges, sparse extents, layout compatibility, and payload integrity");
