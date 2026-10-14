#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { restoreStorageSnapshotArchive } from "../install-slot/storage-snapshot-restore.js";
import { buildStorageSnapshotManifest, buildStorageSnapshotZip } from "../install-slot/storage-snapshot.js";

// A device exposing only the existing read-only snapshot APIs must fail closed
// before any write/erase is attempted. This is the current firmware contract.
{
  const calls = [];
  const device = {
    async storageInfo() {
      calls.push("storageInfo");
      return { ok: true, text: JSON.stringify({
        running_host: { label: "ota_0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 },
        partitions: [
          { label: "ota_0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 },
          { label: "cardid", type: 1, subtype: 64, offset: 0x20000, size: 0x1000 },
          { label: "store", type: 1, subtype: 65, offset: 0x21000, size: 0x1000 },
        ],
      }) };
    },
  };
  await assert.rejects(
    restoreStorageSnapshotArchive(device, new Uint8Array(), { sha256Hex: async () => "" }),
    error => error.code === "STORAGE_SNAPSHOT_RESTORE_UNAVAILABLE" &&
      /beginSnapshotRestore/.test(error.message),
  );
  assert.deepEqual(calls, [], "missing durable transaction methods fail before parsing/writing");
}
console.log("PASS snapshot restore refuses devices without the durable transaction contract");

// Full host-side protocol test. The mock proves orchestration and fail-closed
// behavior only; it is not evidence that current device firmware implements writes.
{
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
    partitions: [
      { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000, extents: [] },
      { label: "store", type: 1, subtype: 65, offset: 0x21000, size: 0x1000,
        extents: [{ file: "extents/store-00000000.bin", offset: 0, bytes: payload.length, sha256: hash(payload) }] },
    ],
  });
  const archive = await buildStorageSnapshotZip({
    manifest, payloads: new Map([["extents/store-00000000.bin", payload]]),
  }, async bytes => hash(bytes));
  const calls = [];
  const device = {
    async storageInfo() {
      calls.push(["info"]);
      return { ok: true, text: JSON.stringify({ running_host: host, partitions }) };
    },
    async beginSnapshotRestore(meta) {
      calls.push(["begin", meta]);
      return { transaction_id: "opaque-txn" };
    },
    async stageSnapshotRestoreChunk(chunk) {
      calls.push(["stage", chunk]);
      return { ok: true };
    },
    async verifyStagedSnapshotRestore(args) {
      calls.push(["verify-staged", args]);
      return { ok: true, verified: true };
    },
    async commitSnapshotRestore(args) {
      calls.push(["commit", args]);
      return { committed: true };
    },
    async getSnapshotRestoreStatus(args) {
      calls.push(["status", args]);
      return { state: "committed", verified: true };
    },
  };
  const result = await restoreStorageSnapshotArchive(device, archive, { sha256Hex: async bytes => hash(bytes) });
  assert.equal(result.committed, true);
  const begin = calls.find(call => call[0] === "begin")[1];
  assert.equal("plan" in begin, false, "absolute target geometry must not be sent as a client-supplied plan");
  const stage = calls.find(call => call[0] === "stage")[1];
  assert.deepEqual(stage.partition, { label: "store", type: 1, subtype: 65 });
  assert.equal("offset" in stage.partition, false, "device resolves absolute geometry from its live table");
  assert.equal("size" in stage.partition, false);
  assert.equal(stage.relative_offset, 0);
  assert.ok(calls.some(call => call[0] === "verify-staged"));
  assert.ok(calls.some(call => call[0] === "commit"));
  assert.ok(calls.some(call => call[0] === "status"));
  console.log("PASS v2 restore coordinator validates archive, sends identity-relative chunks, and requires durable commit/readback status");
}
