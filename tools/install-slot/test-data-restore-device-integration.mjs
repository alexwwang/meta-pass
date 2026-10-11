#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { restoreDataArchive } from "../../install-slot/phone-install.js";
import { buildDataArchiveManifest } from "../../install-slot/data-byte-archive.js";

const sha = (b) => createHash("sha256").update(b).digest("hex");
function buildAppImage() {
  const segLen = 64;
  let total = 24 + 16 + 8 + segLen;
  while (total % 16 !== 15) total++;
  total += 1 + 32;
  const app = new Uint8Array(total).fill(0xab);
  app[0] = 0xe9; app[1] = 1; app[12] = 5; app[13] = 0; app[23] = 1;
  const dv = new DataView(app.buffer);
  dv.setUint32(24 + 16, 0x3fc80000, true);
  dv.setUint32(24 + 16 + 4, segLen, true);
  return app;
}
const app = buildAppImage();
const rawSlot = new Uint8Array(0x20000).fill(0xff);
rawSlot.set(app);
const data = new Uint8Array(4096);
for (let i = 0; i < data.length; i++) data[i] = (i * 13 + 7) & 255;
const manifest = buildDataArchiveManifest({
  slots: [{ play_id: 563, source_slot: 0, name: "restored-demo",
    files: [{ name: "slot0_raw.bin", bytes: rawSlot.length, sha256: sha(rawSlot), type: "raw" }] }],
  data: [{ play_id: 563, label: "cfg_save", type: 1, subtype: 0x82,
    state: 1, bytes: data.length, sha256: sha(data) }],
});
const payloads = new Map([
  ["slot0_raw.bin", rawSlot],
  [manifest.data[0].file, data],
]);
const listing = {
  slots: [{ slot: 0, state: "valid", kind: "app", play_id: 900,
    offset: 0x180000, size: 0x20000, limit: 0x1f000 }],
  data: [],
};

function fakeBridge() {
  let state = "idle";
  let offer;
  const events = [];
  const bridge = {
    token: () => "test-token",
    async status() {
      const snapshot = state === "confirmed"
        ? { protocol: 1, state, confirmed: true, slot: 1, session: false }
        : state === "session"
          ? { protocol: 1, state: "uploading", confirmed: true, slot: 1, session: true, offset: app.length }
          : { protocol: 1, state, confirmed: false, slot: -1, session: false };
      return { ok: true, status: 200, text: JSON.stringify(snapshot) };
    },
    async prepare(o) { offer = o; events.push(["prepare", o]); state = "confirmed"; return { ok: true, status: 200, text: "ok" }; },
    async session(s) {
      events.push(["session", s]); state = "session";
      return { ok: true, status: 200, text: JSON.stringify({ offset: 0, maxChunk: 4096,
        data: [{ index: 0, offset: 0, expected: data.length, done: false }] }) };
    },
    async chunk(offset, bytes) { events.push(["app", offset, bytes.length]); return { ok: true, status: 200, text: "ok" }; },
    async dataChunk(index, offset, bytes) { events.push(["data", index, offset, bytes.length]); return { ok: true, status: 200, text: "ok" }; },
    async finalize() { events.push(["finalize"]); state = "done"; return { ok: true, status: 200, text: "ok" }; },
    async cancel() { events.push(["cancel"]); state = "cancelled"; return { ok: true, status: 200, text: "ok" }; },
    get events() { return events; },
    get offer() { return offer; },
  };
  return bridge;
}

{
  const bridge = fakeBridge();
  const result = await restoreDataArchive(bridge, listing, manifest, payloads);
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.equal(result.playId, 563);
  assert.equal(result.restoredData, 1);
  assert.equal(bridge.offer.restoreData, true);
  assert.equal(bridge.offer.playId, 563);
  assert.equal(bridge.offer.data[0].initialImageSize, 4096);
  assert.ok(bridge.events.some((e) => e[0] === "app"));
  assert.ok(bridge.events.some((e) => e[0] === "data"));
  assert.ok(bridge.events.some((e) => e[0] === "finalize"));
  console.log("PASS 1: single APP+DATA archive uses device allocator, APP/DATA upload, and finalize");
}
{
  const tail = new Uint8Array(4096).fill(0xff);
  const splitManifest = buildDataArchiveManifest({
    slots: [{ play_id: 564, source_slot: 2, name: "usb-backed-demo",
      files: [
        { name: "slot2_firmware.bin", bytes: app.length, sha256: sha(app), type: "app" },
        { name: "slot2_tail.bin", bytes: tail.length, sha256: sha(tail), type: "other" },
      ] }],
    data: [{ play_id: 564, label: "cfg_save", type: 1, subtype: 0x82,
      state: 1, bytes: data.length, sha256: sha(data) }],
  });
  const splitPayloads = new Map([
    ["slot2_firmware.bin", app], ["slot2_tail.bin", tail],
    [splitManifest.data[0].file, data],
  ]);
  const bridge = fakeBridge();
  const result = await restoreDataArchive(bridge, listing, splitManifest, splitPayloads);
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.equal(result.playId, 564);
  assert.equal(bridge.offer.playId, 564);
  console.log("PASS 2: USB-style firmware + tail archive is restored through the same mobile transaction");
}
{
  const bridge = fakeBridge();
  const collision = { ...listing, slots: [...listing.slots, { ...listing.slots[0], slot: 1, play_id: 563 }] };
  const result = await restoreDataArchive(bridge, collision, manifest, payloads);
  assert.equal(result.ok, false);
  assert.match(result.reason, /play_id already exists/);
  assert.equal(bridge.events.length, 0);
  console.log("PASS 3: conflicting play_id is rejected before device I/O");
}
{
  const bridge = fakeBridge();
  const incomplete = { ...manifest, complete: false, missing_slots: [1] };
  const result = await restoreDataArchive(bridge, listing, incomplete, payloads);
  assert.equal(result.ok, false);
  assert.match(result.reason, /incomplete archives/);
  assert.equal(bridge.events.length, 0);
  console.log("PASS 4: incomplete archive is rejected before device I/O");
}
{
  const bridge = fakeBridge();
  const withNvs = { ...manifest, nvs: { file: "nvs.bin", bytes: 4096, sha256: sha(data) } };
  const result = await restoreDataArchive(bridge, listing, withNvs, payloads);
  assert.equal(result.ok, false);
  assert.match(result.reason, /global NVS/);
  assert.equal(bridge.events.length, 0);
  console.log("PASS 5: global NVS is never silently restored outside the APP/DATA transaction");
}
