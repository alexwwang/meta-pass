#!/usr/bin/env node
// TDD contract for optional app-owned DATA allocation.
// The same policy helper is used by USB and embedded-phone installers.
import assert from "node:assert/strict";
import { resolveDataInstallChoice, makeBlankAppData } from "../../install-slot/data-install-choice.js";

const appOnly = [];
assert.deepEqual(resolveDataInstallChoice({ allocate: false, dataImages: appOnly, slotIndex: 2 }), []);
const blank = resolveDataInstallChoice({ allocate: true, dataImages: appOnly, slotIndex: 2 });
assert.equal(blank.length, 1);
assert.equal(blank[0].label, "data_ota_2");
assert.equal(blank[0].subtype, 0x83, "blank app data uses the supported LittleFS subtype");
assert.equal(blank[0].required_size, 64 * 1024);
assert.equal(blank[0].initial_image_size, 0);
assert.equal(blank[0].data.length, 0);

const withInitial = [{ label: "assets", subtype: 0x82, required_size: 128 * 1024,
  initial_image_size: 32, data: new Uint8Array(32) }];
assert.deepEqual(resolveDataInstallChoice({ allocate: false, dataImages: withInitial, slotIndex: 1 }), []);
assert.deepEqual(resolveDataInstallChoice({ allocate: true, dataImages: withInitial, slotIndex: 1 }), withInitial);
assert.throws(() => makeBlankAppData(-1), /slot index/);
assert.throws(() => makeBlankAppData(100), /slot index/);
console.log("DATA install choice: PASS (optional allocation, blank DATA contract, declared DATA preservation)");
