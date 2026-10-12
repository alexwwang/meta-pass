#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { executeDataRestoreTransaction } from "../../install-slot/data-restore-transaction.js";

const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const app = new Uint8Array([0xe9, 1, 2, 3, 4, 5]);
const data = new Uint8Array([9, 8, 7, 6, 5, 4, 3]);
const manifest = {
  manifest_version: 2, complete: true, missing_slots: [],
  slots: [{ play_id: 101, source_slot: 0, name: "demo",
    files: [{ name: "app.bin", bytes: app.length, sha256: digest(app), type: "app" }] }],
  data: [{ play_id: 101, label: "cfg_save", type: 1, subtype: 64, state: 1,
    bytes: data.length, sha256: digest(data), file: "data/101/0.bin" }],
};
const plan = { manifestVersion: 2, playIds: [101], dataBytes: 4096,
  data: [{ play_id: 101, label: "cfg_save", type: 1, subtype: 64, state: 1,
    bytes: data.length, sha256: digest(data), file: "data/101/0.bin", allocatedBytes: 4096 }] };
const payloads = new Map([["app.bin", app], ["data/101/0.bin", data]]);

function fakeDevice({ failReadback = false, failAppWrite = false, failCommit = false, commitAppliedThenTimeout = false } = {}) {
  const events = [];
  const store = new Map();
  let staged = true;
  let commitApplied = false;
  const device = {
    async beginRestore(meta) { events.push(["begin", meta]); return "opaque-txn"; },
    async allocateData(token, record) { events.push(["allocate", record]); store.set("data", new Uint8Array(record.bytes)); return "opaque-data-handle"; },
    async writeData(token, handle, offset, bytes) { events.push(["writeData", offset, bytes.length]); store.get("data").set(bytes, offset); },
    async readData(token, handle, offset, length) {
      events.push(["readData", offset, length]);
      if (failReadback) return new Uint8Array(length).fill(0);
      return store.get("data").slice(offset, offset + length);
    },
    async writeApp(token, slot, file, bytes) { events.push(["writeApp", slot.play_id]); if (failAppWrite) throw new Error("simulated APP write failure"); store.set("app", bytes.slice()); },
    async verifyApp(token, slot, file) { events.push(["verifyApp", slot.play_id]); },
    async commitRestore(token, value) { events.push(["commit", value]); if (failCommit) throw new Error("simulated timeout before durable commit"); staged = false; commitApplied = true; if (commitAppliedThenTimeout) throw new Error("simulated timeout after durable commit"); return { committed: true }; },
    async readCommittedState(ids) { events.push(["verifyState", ids]); return { committed: true, playIds: ids, data: [{ play_id: 101, label: "cfg_save", bytes: data.length, sha256: digest(data) }] }; },
    async cleanupStaging(token) { events.push(["cleanup", token]); staged = false; },
  };
  return { device, events, staged: () => staged, commitApplied: () => commitApplied };
}

{
  const { device, events } = fakeDevice();
  const result = await executeDataRestoreTransaction({ manifest, payloads, plan, device, sha256Hex: async b => digest(b), chunkSize: 4 });
  assert.equal(result.committed, true);
  assert.ok(events.some(e => e[0] === "commit"));
  assert.ok(events.some(e => e[0] === "verifyState"));
  assert.equal(events.some(e => e[0] === "cleanup"), false);
  console.log("PASS 1: DATA is staged/read back, APP verified, commit and ownership recheck run");
}
{
  const { device, events } = fakeDevice({ failReadback: true });
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan, device, sha256Hex: async b => digest(b), chunkSize: 4 }), /DATA readback digest mismatch/);
  assert.equal(events.some(e => e[0] === "commit"), false);
  assert.equal(events.some(e => e[0] === "cleanup"), true);
  console.log("PASS 2: readback mismatch blocks metadata commit and cleans staging");
}
{
  const { device, events, staged } = fakeDevice({ failCommit: true });
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan, device, sha256Hex: async b => digest(b), chunkSize: 4 }), /commit may have started/);
  assert.equal(events.some(e => e[0] === "cleanup"), false);
  assert.equal(staged(), true);
  console.log("PASS 3: ambiguous commit preserves staging for recovery inspection");
}
{
  const { device, events, staged } = fakeDevice({ failAppWrite: true });
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan, device, sha256Hex: async b => digest(b), chunkSize: 4 }), /simulated APP write failure/);
  assert.equal(events.some(e => e[0] === "commit"), false, "APP write failure must never reach commit");
  assert.equal(events.some(e => e[0] === "cleanup"), true, "pre-commit failure must clean temporary staging");
  assert.equal(staged(), false);
  console.log("PASS 4: APP write failure aborts before commit and cleans temporary staging");
}
{
  const { device, events, staged, commitApplied } = fakeDevice({ commitAppliedThenTimeout: true });
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan, device, sha256Hex: async b => digest(b), chunkSize: 4 }), /commit may have started/);
  assert.equal(commitApplied(), true, "fake device simulates durable ownership commit before transport timeout");
  assert.equal(events.some(e => e[0] === "cleanup"), false, "ambiguous post-commit timeout must never clean staging");
  assert.equal(staged(), false, "the device may already have atomically published the staged allocation");
  console.log("PASS 5: timeout after durable commit preserves state for recovery and never cleans staging");
}
{
  const { device, events } = fakeDevice();
  const mismatchedPlan = { ...plan, dataBytes: 8192 };
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan: mismatchedPlan, device, sha256Hex: async b => digest(b) }), /capacity totals do not match/);
  assert.equal(events.length, 0, "capacity mismatch must be rejected before device transaction");
  console.log("PASS 6: capacity totals are recomputed from the manifest before device I/O");
}
{
  const { device, events } = fakeDevice();
  const mismatchedPlan = { ...plan, data: [] };
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads, plan: mismatchedPlan, device, sha256Hex: async b => digest(b) }), /DATA identities do not match/);
  assert.equal(events.length, 0, "plan mismatch must be rejected before device transaction");
  console.log("PASS 7: plan identities must exactly match the archive manifest");
}
{
  const bad = new Map(payloads);
  bad.set("data/101/0.bin", new Uint8Array([1, 1, 1]));
  const { device, events } = fakeDevice();
  await assert.rejects(executeDataRestoreTransaction({ manifest, payloads: bad, plan, device, sha256Hex: async b => digest(b) }), /payload length changed/);
  assert.equal(events.length, 0, "invalid payload must be rejected before device writes");
  console.log("PASS 8: mutated payload is rejected before opening device transaction");
}
