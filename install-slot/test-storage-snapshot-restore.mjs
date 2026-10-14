import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { buildStorageSnapshotManifest, buildStorageSnapshotZip } from "./storage-snapshot.js";
import { restoreStorageSnapshotArchive } from "./storage-snapshot-restore.js";

const sha256Hex = async (bytes) =>
  createHash("sha256").update(Buffer.from(bytes)).digest("hex");

function fixture({ rebootAndReconnect = async () => ({ ok: true }) } = {}) {
  const host = { label: "app0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 };
  const nvs = { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x1000 };
  const cardid = { label: "cardid", type: 1, subtype: 130, offset: 0x8000, size: 0x1000 };
  const original = Uint8Array.from({ length: 16 }, (_, i) => i + 1);
  const manifest = buildStorageSnapshotManifest({
    createdUtc: "2026-10-14T00:00:00.000Z",
    metaPassVersion: "2.0.0",
    compatibleMetaPassVersions: { min_inclusive: "2.0.0", max_inclusive: "2.9.9" },
    partitions: [{
      label: "nvs", type: 1, subtype: 2, offset: nvs.offset, size: nvs.size,
      extents: [{ file: "partitions/nvs.bin", offset: 0, bytes: original.length, sha256: createHash("sha256").update(original).digest("hex") }],
    }],
  });
  const flash = new Uint8Array(nvs.size).fill(0);
  const calls = [];
  const device = {
    storageInfo: async () => ({ ok: true, text: JSON.stringify({
      meta_pass_version: "2.0.0",
      running_host: host,
      partitions: [nvs, cardid, host],
      protected_ranges: [],
    }) }),
    storageErase: async (p) => { calls.push(["erase", p.label]); flash.fill(0xff); return { ok: true }; },
    storageWrite: async (p, offset, bytes) => {
      calls.push(["write", p.label, offset, bytes.length]);
      flash.set(bytes, offset);
      return { ok: true };
    },
    storageRead: async (_p, offset, length) => ({ ok: true, bytes: flash.slice(offset, offset + length) }),
    rebootAndReconnect,
  };
  return { device, manifest, original, flash, calls };
}

async function archiveFor(manifest, original) {
  return buildStorageSnapshotZip({
    manifest,
    payloads: new Map([["partitions/nvs.bin", original]]),
  }, sha256Hex);
}

test("restores snapshot bytes, then validates reboot", async () => {
  const f = fixture();
  const archive = await archiveFor(f.manifest, f.original);
  const result = await restoreStorageSnapshotArchive(f.device, archive, { sha256Hex });
  assert.equal(result.dataWritten, true);
  assert.equal(result.bootValidated, true);
  assert.deepEqual(f.flash.slice(0, f.original.length), f.original);
  assert.ok(f.calls.some(([op]) => op === "erase"));
  assert.ok(f.calls.some(([op]) => op === "write"));
});

test("reports boot validation failure separately from completed writes", async () => {
  const f = fixture({ rebootAndReconnect: async () => ({ ok: false, message: "device did not reconnect" }) });
  const archive = await archiveFor(f.manifest, f.original);
  await assert.rejects(
    restoreStorageSnapshotArchive(f.device, archive, { sha256Hex }),
    (error) => error.code === "SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED" && error.dataVerified === true,
  );
});

test("does not claim boot validation when the adapter cannot reboot/reconnect", async () => {
  const f = fixture();
  delete f.device.rebootAndReconnect;
  const archive = await archiveFor(f.manifest, f.original);
  const result = await restoreStorageSnapshotArchive(f.device, archive, { sha256Hex });
  assert.equal(result.dataVerified, true);
  assert.equal(result.bootValidated, false);
  assert.match(result.warning, /reboot\/reconnect validation remains pending/);
});
