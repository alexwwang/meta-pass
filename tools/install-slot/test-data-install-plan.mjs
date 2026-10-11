#!/usr/bin/env node
// Pure DATA allocation tests used by USB local-firmware install.
import assert from "node:assert/strict";
import { planDataReservations, nextDataPlayId } from "../../install-slot/data-install-plan.js";
import { carveValid, SLOT_KIND, SLOT_STATE } from "../../install-slot/dynslot-record.js";

const empty = {
  slots: [{
    kind: SLOT_KIND.APP, state: SLOT_STATE.EMPTY,
    offset: 0x180000, size: 0x40000, imageLen: 0, playId: 0,
    sha: new Uint8Array(32), name: "",
  }],
  data: [],
};
const images = [
  { label: "e2edata", subtype: 0x40, required_size: 0x10000, initial_image_size: 0x100, data: new Uint8Array(0x100) },
  { label: "assets", subtype: 0x82, required_size: 0x10000, initial_image_size: 0, data: new Uint8Array() },
];

{
  const id = nextDataPlayId(empty);
  assert.equal(id, 0x80000000, "local install IDs use the high-bit namespace");
  const plan = planDataReservations(empty, images, [0x10000, 0x20000], id);
  assert.equal(plan.ok, true);
  assert.equal(plan.carve.data.length, 2);
  assert.ok(plan.carve.data.every((d) => d.playId === id));
  assert.equal(plan.carve.data[0].offset, 0x1c0000, "first DATA follows APP reservation");
  assert.equal(plan.carve.data[1].offset, 0x200000, "second DATA avoids APP and first DATA");
  assert.equal(carveValid(plan.carve), true);
  assert.equal(empty.data.length, 0, "planner does not mutate source carve");
  console.log("PASS 1: APP-owned DATA group planned without mutating source carve");
}
{
  const duplicate = planDataReservations(empty, [images[0], images[0]], [0x10000, 0x10000], 0x80000000);
  assert.deepEqual({ ok: duplicate.ok, reason: duplicate.reason }, { ok: false, reason: "duplicate-label-subtype" });
  const tooSmall = planDataReservations(empty, [images[0]], [0x1000], 0x80000000);
  assert.equal(tooSmall.ok, false);
  assert.equal(tooSmall.reason, "invalid-data-size");
  const badId = planDataReservations(empty, [images[0]], [0x10000], 0);
  assert.equal(badId.reason, "invalid-play-id");
  console.log("PASS 2: duplicate DATA keys, undersized extents, and invalid owner IDs rejected");
}
{
  const full = {
    slots: empty.slots,
    data: Array.from({ length: 8 }, (_, i) => ({
      playId: i + 1, offset: 0x1c0000 + i * 0x10000, size: 0x10000,
      state: 0, subtype: 0x40 + i, type: 1, label: `d${i}`,
    })),
  };
  const plan = planDataReservations(full, [images[0]], [0x10000], 0x80000000);
  assert.equal(plan.ok, false);
  assert.equal(plan.reason, "no-space");
  console.log("PASS 3: DATA planner fails closed when DATA record capacity is exhausted");
}
console.log("ALL USB DATA INSTALL PLAN TESTS PASSED");
