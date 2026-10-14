#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { restoreStorageSnapshotArchive } from "../install-slot/storage-snapshot-restore.js";
import { buildStorageSnapshotManifest, buildStorageSnapshotZip } from "../install-slot/storage-snapshot.js";

const hash = bytes => createHash("sha256").update(bytes).digest("hex");
const payload = new Uint8Array([0x11, 0x22, 0x33, 0x44]);
const host = { label: "ota_0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 };
const partitions = [
  host,
  { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  { label: "cardid", type: 1, subtype: 64, offset: 0x20000, size: 0x1000 },
  { label: "store", type: 1, subtype: 65, offset: 0x21000, size: 0x1000 },
];
const manifest = buildStorageSnapshotManifest({
  createdUtc: "2026-10-14T00:00:00.000Z",
  metaPassVersion: "2.1.0",
  compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
  partitions: [
    { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000, extents: [] },
    { label: "store", type: 1, subtype: 65, offset: 0x21000, size: 0x1000,
      extents: [{ file: "extents/store-00000010.bin", offset: 0x10, bytes: payload.length, sha256: hash(payload) }] },
  ],
});
const archive = await buildStorageSnapshotZip({
  manifest, payloads: new Map([["extents/store-00000010.bin", payload]]),
}, async bytes => hash(bytes));

function mockDevice({ version = "2.2.0", failWriteAt = null,
  failBoot = false, layoutMismatch = false } = {}) {
  const calls = [];
  const flash = new Map();
  let writes = 0;
  const key = (partition, offset) => partition.offset + offset;
  const livePartitions = layoutMismatch
    ? partitions.map(p => p.label === "store" ? { ...p, size: p.size + 0x1000 } : p)
    : partitions;
  return {
    calls,
    async storageInfo() {
      calls.push(["info"]);
      return { ok: true, meta_pass_version: version, running_host: host, partitions: livePartitions };
    },
    async storageErase(partition) {
      calls.push(["erase", partition.label, partition.size]);
      for (let i = 0; i < partition.size; i++) flash.set(partition.offset + i, 0xff);
      return { ok: true };
    },
    async storageWrite(partition, offset, bytes) {
      calls.push(["write", partition.label, offset, bytes.length]);
      writes++;
      if (failWriteAt !== null && writes === failWriteAt) throw new Error("simulated write failure");
      for (let i = 0; i < bytes.length; i++) flash.set(key(partition, offset) + i, bytes[i]);
      return { ok: true };
    },
    async rebootAndReconnect() {
      calls.push(["reboot"]);
      if (failBoot) throw new Error("device did not reconnect");
      return { ok: true };
    },
  };
}

// Incompatible and missing device versions must fail before the first Flash mutation.
for (const version of ["3.0.0", null]) {
  const device = mockDevice({ version });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    /outside this snapshot's supported range|did not report a valid MetaPass version/,
  );
  assert.equal(device.calls.some(c => c[0] === "write" || c[0] === "erase"), false, "version preflight runs before writes");
}

// Live layout mismatch is rejected before the first erase.
{
  const device = mockDevice({ layoutMismatch: true });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    /incompatible/,
  );
  assert.equal(device.calls.some(c => c[0] === "write" || c[0] === "erase"), false,
    "layout preflight must finish before Flash mutation");
}

// Missing read/write capabilities must also fail before erase.
{
  const calls = [];
  const device = {
    async storageInfo() { calls.push("info"); return { ok: true, meta_pass_version: "2.2.0", running_host: host, partitions }; },
    async storageErase() { calls.push("erase"); return { ok: true }; },
  };
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    /does not provide live storage inventory/,
  );
  assert.equal(calls.includes("erase"), false, "transport capability preflight must be non-mutating");
}

// Success covers erase, sparse 0xFF gaps, payload writes, and reboot; restore does not reread every Flash byte.
{
  const device = mockDevice();
  const progress = [];
  const result = await restoreStorageSnapshotArchive(device, archive, {
    sha256Hex: async bytes => hash(bytes),
    onProgress: event => progress.push(event.phase),
  });
  assert.equal(result.dataWritten, true);
  assert.equal(result.bootValidated, true);
  assert.equal(device.calls.some(c => c[0] === "read"), false, "restore must not perform full-partition readback");
  assert.ok(device.calls.some(c => c[0] === "reboot"));
  assert.ok(progress.includes("write"), "restore progress should expose the extent-write phase");
  assert.ok(progress.includes("data-written") && progress.includes("complete"));
  assert.equal(device.calls.some(c => c[0] === "write" && c[1] === "ota_0"), false);
  assert.equal(device.calls.some(c => c[0] === "write" && c[1] === "cardid"), false);
}

// A write error is reported as incomplete and no rollback is attempted.
{
  const device = mockDevice({ failWriteAt: 1 });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    error => error.code === "SNAPSHOT_RESTORE_WRITE_FAILED" &&
      error.partition === "store" && error.relativeOffset === 0x10 &&
      /Restore incomplete/.test(error.message),
  );
  assert.equal(device.calls.some(c => c[0] === "reboot"), false);
}

// Completed writes and reboot failure are separate outcomes.
{
  const device = mockDevice({ failBoot: true });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    error => error.code === "SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED" &&
      error.dataWritten === true && /Snapshot bytes were written/.test(error.message),
  );
}

console.log("PASS sequential snapshot restore preflight, writes, sparse ranges, and reboot validation");
