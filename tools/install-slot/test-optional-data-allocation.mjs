#!/usr/bin/env node
// TDD contract for optional per-APP DATA allocation, including blank DATA.
import assert from "node:assert/strict";
import { makeBlankDataImage, resolveDataAllocationChoice } from "../../install-slot/optional-data-allocation.js";

const existing = [{ label: "recordings", subtype: 0x81, required_size: 0x100000,
  initial_image_size: 0, data: new Uint8Array() }];
{
  const none = resolveDataAllocationChoice(existing, false, 28);
  assert.deepEqual(none, []);
  const declared = resolveDataAllocationChoice(existing, true, 28);
  assert.equal(declared.length, 1);
  assert.equal(declared[0].label, "recordings");
  assert.equal(declared[0].subtype, 0x81);
  console.log("PASS: opting out creates no DATA reservation; opting in preserves declared DATA");
}
{
  const blank = makeBlankDataImage(28, []);
  assert.equal(blank.subtype, 0x83, "blank DATA uses registered LittleFS subtype");
  assert.match(blank.label, /^[A-Za-z0-9_-]{1,16}$/);
  assert.equal(blank.initial_image_size, 0);
  assert.equal(blank.data.length, 0);
  assert.equal(blank.required_size, 64 * 1024);
  assert.deepEqual(resolveDataAllocationChoice([], false, 28), []);
  const chosen = resolveDataAllocationChoice([], true, 28);
  assert.equal(chosen.length, 1);
  assert.deepEqual(chosen[0], blank);
  console.log("PASS: apps without embedded DATA can request a blank, app-owned DATA image");
}
{
  const occupied = [{ label: "mpd1c", subtype: 0x83 }];
  const blank = makeBlankDataImage(28, occupied);
  assert.notEqual(blank.label, "mpd1c");
  assert.ok(blank.label.length <= 16);
  assert.throws(() => makeBlankDataImage(0, []), /valid positive app ID/);
  console.log("PASS: generated labels avoid existing LittleFS labels and reject invalid owners");
}
