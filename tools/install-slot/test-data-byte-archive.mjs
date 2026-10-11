#!/usr/bin/env node
// Shared MPTB v2 DATA archive manifest/payload contract tests.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  DATA_ARCHIVE_MANIFEST_VERSION,
  buildDataArchiveManifest,
  validateDataArchiveManifest,
  validateDataArchivePayloads,
} from "../../install-slot/data-byte-archive.js";

const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const app = new Uint8Array([0xe9, 1, 2, 3]);
const data = new Uint8Array([0x11, 0x22, 0x33, 0x44]);
const appPath = "slot-1_firmware.bin";
const dataPath = "data/123/0.bin";
const manifest = buildDataArchiveManifest({
  createdUtc: "2026-10-11T00:00:00.000Z",
  slots: [{
    play_id: 123, source_slot: 1, name: "demo",
    files: [{ name: appPath, bytes: app.length, sha256: digest(app), type: "app" }],
  }],
  data: [{
    play_id: 123, label: "cfg_save", type: 1, subtype: 64, state: 1,
    bytes: data.length, sha256: digest(data),
  }],
});
assert.equal(manifest.manifest_version, DATA_ARCHIVE_MANIFEST_VERSION);
assert.equal(manifest.data[0].file, dataPath);
assert.equal(validateDataArchiveManifest(manifest), manifest);
assert.deepEqual(
  await validateDataArchivePayloads(manifest, new Map([[appPath, app], [dataPath, data]]), async (b) => digest(b)),
  { ok: true, fileCount: 2, dataCount: 1 },
);
console.log("PASS 1: v2 manifest + exact APP/DATA payload digest validation");

// A metadata-only or old v1 archive must never be treated as DATA backup.
assert.throws(() => validateDataArchiveManifest({ ...manifest, manifest_version: 1 }), /v1 archives do not contain DATA bytes/);
await assert.rejects(() => validateDataArchivePayloads(manifest, new Map([[appPath, app]]), async (b) => digest(b)), /missing archive payload/);
await assert.rejects(() => validateDataArchivePayloads(manifest, new Map([[appPath, app], [dataPath, new Uint8Array([1])]]), async (b) => digest(b)), /payload length mismatch/);
await assert.rejects(() => validateDataArchivePayloads(manifest, new Map([[appPath, app], [dataPath, new Uint8Array([1, 2, 3, 4])]]), async (b) => digest(b)), /payload SHA-256 mismatch/);
console.log("PASS 2: v1, missing, truncated and corrupted payloads rejected");

// Ownership and path safety are logical identifiers, not source Flash addresses.
assert.throws(() => buildDataArchiveManifest({
  slots: [{ play_id: 123, source_slot: 0, name: "demo", files: [] }],
  data: [{ play_id: 999, label: "cfg", type: 1, subtype: 64, state: 1, bytes: 4, sha256: digest(data) }],
}), /no matching APP play_id/);
assert.throws(() => buildDataArchiveManifest({
  slots: [{ play_id: 123, source_slot: 0, name: "demo", files: [] }],
  data: [
    { play_id: 123, label: "cfg", type: 1, subtype: 64, state: 1, bytes: 4, sha256: digest(data) },
    { play_id: 123, label: "cfg", type: 1, subtype: 64, state: 1, bytes: 4, sha256: digest(data) },
  ],
}), /duplicate DATA ownership key/);
assert.throws(() => buildDataArchiveManifest({
  slots: [{ play_id: 123, source_slot: 0, name: "demo", files: [] }],
  data: [{ play_id: 123, label: "../cfg", type: 1, subtype: 64, state: 1, bytes: 4, sha256: digest(data) }],
}), /invalid DATA label/);
assert.throws(() => validateDataArchiveManifest({
  ...manifest, data: [{ ...manifest.data[0], file: "data/123/../../secret.bin" }],
}), /invalid or duplicate DATA payload path/);
assert.throws(() => validateDataArchiveManifest({
  ...manifest, data: [...manifest.data, { ...manifest.data[0], label: "other", file: "data/123/0.bin" }],
}), /invalid or duplicate DATA payload path/);
console.log("PASS 3: ownership, label, path traversal and malformed records rejected");
console.log("ALL DATA BYTE ARCHIVE TESTS PASSED");
