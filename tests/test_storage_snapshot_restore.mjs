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

function mockDevice({ version = "2.2.0", corruptReadback = false, failWriteAt = null,
  failBoot = false } = {}) {
  const calls = [];
  const flash = new Map();
  let writes = 0;
  return {
    calls,
    async storageInfo() {
      calls.push(["info"]);
      return { ok: true, meta_pass_version: version, running_host: host, partitions };
    },
    async writeFlash(address, bytes) {
      calls.push(["write", address, bytes.length]);
      writes++;
      if (failWriteAt !== null && writes === failWriteAt) throw new Error("simulated write failure");
      for (let i = 0; i < bytes.length; i++) flash.set(address + i, bytes[i]);
    },
    async readFlash(address, length) {
      calls.push(["read", address, length]);
      const out = new Uint8Array(length);
      for (let i = 0; i < length; i++) out[i] = flash.has(address + i) ? flash.get(address + i) : 0xff;
      if (corruptReadback && address === 0x21000) out[0x10] ^= 0x01;
      return out;
    },
    async rebootAndReconnect() {
      calls.push(["reboot"]);
      if (failBoot) throw new Error("device did not reconnect");
      return { ok: true };
    },
  };
}

// Incompatible and missing device versions must fail before the first Flash mutation.
for (const version of ["3.0.0", undefined]) {
  const device = mockDevice({ version });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    /outside this snapshot's supported range|did not report a valid MetaPass version/,
  );
  assert.equal(device.calls.some(c => c[0] === "write"), false, "version preflight runs before writes");
}

// Success covers erase, sparse 0xFF gaps, payload writes, full partition readback, and reboot.
{
  const device = mockDevice();
  const progress = [];
  const result = await restoreStorageSnapshotArchive(device, archive, {
    sha256Hex: async bytes => hash(bytes),
    onProgress: event => progress.push(event.phase),
  });
  assert.equal(result.dataVerified, true);
  assert.equal(result.bootValidated, true);
  assert.ok(device.calls.some(c => c[0] === "read" && c[1] === 0x21000));
  assert.ok(device.calls.some(c => c[0] === "reboot"));
  assert.ok(progress.includes("data-verified") && progress.includes("complete"));
  assert.equal(device.calls.some(c => c[0] === "write" && c[1] === host.offset), false);
  assert.equal(device.calls.some(c => c[0] === "write" && c[1] === 0x20000), false);
}

// A byte mismatch reports the exact partition and relative offset, then stops.
{
  const device = mockDevice({ corruptReadback: true });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    error => error.code === "SNAPSHOT_RESTORE_READBACK_MISMATCH" &&
      error.partition === "store" && error.relativeOffset === 0x10 &&
      /Reflash MetaPass 2.1.0/.test(error.message),
  );
  assert.equal(device.calls.some(c => c[0] === "reboot"), false);
}

// A write error is reported as incomplete and no rollback is attempted.
{
  const device = mockDevice({ failWriteAt: 1 });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    error => error.code === "SNAPSHOT_RESTORE_WRITE_FAILED" &&
      error.partition === "nvs" && error.relativeOffset === 0 &&
      /Restore incomplete/.test(error.message),
  );
  assert.equal(device.calls.some(c => c[0] === "reboot"), false);
}

// Data readback success and reboot failure are separate outcomes.
{
  const device = mockDevice({ failBoot: true });
  await assert.rejects(
    restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) }),
    error => error.code === "SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED" &&
      error.dataVerified === true && /readback verification passed/.test(error.message),
  );
}

console.log("PASS sequential snapshot restore preflight, writes, independent readback, sparse ranges, and reboot validation");
