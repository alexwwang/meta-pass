#!/usr/bin/env node
import assert from "node:assert/strict";
import {
  applyDataSizeProfile,
  firmwareDataRequiredSize,
  PLAY28_RECORDINGS_4M_PROFILE,
  PLAY28_RECORDINGS_4M_SIZE,
} from "../install-slot/data-size-profile.js";

const blank = [{ label: "recordings", subtype: 0x81, size: 6592 * 1024,
  required_size: 6592 * 1024, initial_image_size: 0, offset: 0x200000 }];
const resized = applyDataSizeProfile(blank, 28, PLAY28_RECORDINGS_4M_PROFILE);
assert.equal(resized[0].required_size, PLAY28_RECORDINGS_4M_SIZE);
assert.equal(resized[0].size, PLAY28_RECORDINGS_4M_SIZE);
assert.equal(blank[0].required_size, 6592 * 1024, "policy must not mutate source manifest");
assert.equal(resized[0].testSizeProfile, PLAY28_RECORDINGS_4M_PROFILE);

for (const [id, data, message] of [
  [563, blank, "wrong play ID"],
  [28, [{ ...blank[0], label: "other" }], "wrong partition label"],
  [28, [{ ...blank[0], subtype: 0x82 }], "wrong filesystem subtype"],
  [28, [{ ...blank[0], initial_image_size: 1 }], "non-empty initial DATA must never be truncated"],
  [28, [{ label: "recordings", subtype: 0x81, size: 6592 * 1024, required_size: 6592 * 1024 }], "missing initial DATA size must fail closed"],
  [28, [{ ...blank[0], size: 4 * 1024 * 1024, required_size: 4 * 1024 * 1024 }], "unexpected source size"],
]) {
  assert.throws(() => applyDataSizeProfile(data, id, PLAY28_RECORDINGS_4M_PROFILE),
    undefined, message);
}
assert.throws(() => applyDataSizeProfile(blank, 28, "arbitrary-profile"));
assert.equal(applyDataSizeProfile(blank, 28, null)[0].required_size, 6592 * 1024,
  "default production path must preserve original DATA size");
assert.equal(firmwareDataRequiredSize({
  app: { image_size: 1536 * 1024 },
  data: resized,
}), 1536 * 1024 + PLAY28_RECORDINGS_4M_SIZE);
console.log("DATA size profile: PASS (opt-in, blank FAT only, fail-closed, 4 MiB capacity accounting)");
