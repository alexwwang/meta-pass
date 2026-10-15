#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import * as phone from "../../install-slot/phone-install.js";

const sha256Hex = (bytes) => createHash("sha256").update(bytes).digest("hex");
const host = { label: "factory", type: 0, subtype: 0, offset: 0x10000, size: 0x80000 };
const partitions = [
  { ...host, running_host: true },
  { label: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x6000 },
  { label: "cardid", type: 1, subtype: 0x40, offset: 0xf000, size: 0x1000 },
  { label: "store", type: 1, subtype: 0x41, offset: 0x90000, size: 0x10000 },
];
const reads = [];
const bridge = {
  async storageInfo() {
    return { ok: true, status: 200, text: JSON.stringify({ meta_pass_version: "2.1.0", running_host: host, partitions }) };
  },
  async storageRead(partition, offset, length) {
    reads.push({ label: partition.label, offset, length });
    const bytes = new Uint8Array(length).fill(0xff);
    if (partition.label === "store" && offset === 0) {
      bytes[0] = 0x4d;
      bytes[1] = 0x50;
      bytes[2] = 0x53;
      bytes[3] = 0x32;
    }
    return { ok: true, bytes };
  },
};

assert.equal("restoreDataArchive" in phone, false,
  "the phone UI module must not expose the retired APP+DATA v2 restore operation");
assert.equal(typeof phone.createStorageSnapshotArchive, "function");
assert.equal(typeof phone.validateStorageSnapshotArchiveForDevice, "function");
assert.throws(() => phone.assertPhoneArchiveVersion({
  manifest_version: 2, kind: "meta-pass-slot-backup",
}), /旧 APP\+DATA 逻辑归档/);

const archive = await phone.createStorageSnapshotArchive(bridge, { sha256Hex });
assert.equal(archive.manifest.kind, "meta-pass-storage-snapshot");
assert.equal(archive.manifest.complete, true);
assert.equal(archive.manifest.partitions.some((p) => p.label === "factory" || p.label === "cardid"), false);
assert.deepEqual(reads.some((r) => r.label === "factory" || r.label === "cardid"), false);

const beforePreflight = reads.length;
const checked = await phone.validateStorageSnapshotArchiveForDevice(bridge, archive.zip, { sha256Hex });
assert.equal(checked.manifest.kind, "meta-pass-storage-snapshot");
assert.ok(checked.restorePlan.partitionCount > 0);
assert.equal(reads.length, beforePreflight, "restore preflight must inspect inventory only and perform no Flash writes");
console.log("PASS snapshot backup and read-only preflight; sequential v2 restore is covered separately; retired APP+DATA v2 restore is not exposed");
