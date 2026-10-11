#!/usr/bin/env node
// Preflight tests: no target state mutation and no device writes are possible here.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { buildDataArchiveManifest } from "../../install-slot/data-byte-archive.js";
import { buildDataRestorePlan } from "../../install-slot/data-restore-plan.js";

const sha = (b) => createHash("sha256").update(b).digest("hex");
const app = new Uint8Array([0xe9, 1, 2, 3]);
const data = new Uint8Array([4, 5, 6, 7, 8]);
const manifest = buildDataArchiveManifest({
  createdUtc: "2026-10-11T00:00:00.000Z",
  slots: [{ play_id: 123, source_slot: 0, name: "demo",
    files: [{ name: "slot-0_firmware.bin", bytes: app.length, sha256: sha(app), type: "app" }] }],
  data: [{ play_id: 123, label: "cfg_save", type: 1, subtype: 64, state: 1,
    bytes: data.length, sha256: sha(data) }],
});
const payloads = new Map([
  ["slot-0_firmware.bin", app],
  ["data/123/0.bin", data],
]);
const target = {
  freeBytes: 65536, largestGap: 65536, dataRecordSlots: 8,
  appSlotCount: 2, existingPlayIds: [456],
};
const before = structuredClone(target);
const plan = await buildDataRestorePlan(manifest, payloads, async (b) => sha(b), target);
assert.deepEqual(target, before);
assert.deepEqual(plan.playIds, [123]);
assert.equal(plan.dataBytes, 4096);
assert.equal(plan.data[0].allocatedBytes, 4096);
assert.equal("offset" in plan.data[0], false);
assert.equal("address" in plan.data[0], false);
console.log("PASS 1: valid archive produces address-free plan without mutating device snapshot");

await assert.rejects(
  buildDataRestorePlan(manifest, new Map([["slot-0_firmware.bin", app]]), async (b) => sha(b), target),
  /missing archive payload/,
);
console.log("PASS 2: missing bytes reject before any restore operation");

await assert.rejects(
  buildDataRestorePlan(manifest, payloads, async (b) => sha(b), { ...target, existingPlayIds: [123] }),
  /already contains play_id/,
);
await assert.rejects(
  buildDataRestorePlan(manifest, payloads, async (b) => sha(b), { ...target, freeBytes: 4095 }),
  /insufficient total DATA pool capacity/,
);
await assert.rejects(
  buildDataRestorePlan(manifest, payloads, async (b) => sha(b), { ...target, largestGap: 4095 }),
  /insufficient contiguous DATA pool capacity/,
);
await assert.rejects(
  buildDataRestorePlan(manifest, payloads, async (b) => sha(b), { ...target, dataRecordSlots: 0 }),
  /insufficient DATA record slots/,
);
await assert.rejects(
  buildDataRestorePlan({ ...manifest, complete: false, missing_slots: [1] }, payloads, async (b) => sha(b), target),
  /incomplete archive/,
);
console.log("PASS 3: duplicate ownership, total/contiguous capacity, record limit and partial archive rejected");
