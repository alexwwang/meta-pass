#!/usr/bin/env node
import assert from "node:assert/strict";
import { BLANK_DATA_MIN_SIZE, BLANK_DATA_SUBTYPE_SPIFFS, makeBlankAppDataImage, resolveAppDataChoice } from "../../install-slot/data-allocation-choice.js";

const declared = [{ label: "assets", subtype: 0x83, required_size: 0x10000, initial_image_size: 0, data: new Uint8Array() }];
assert.deepEqual(resolveAppDataChoice({ allocate: false, declaredImages: declared, playId: 42 }), [], "declining allocation must produce no DATA records");
assert.equal(resolveAppDataChoice({ allocate: true, declaredImages: declared, playId: 42 })[0], declared[0], "declared DATA descriptors are retained");
const blank = resolveAppDataChoice({ allocate: true, declaredImages: [], playId: 0x80000001 });
assert.equal(blank.length, 1);
assert.equal(blank[0].label, "mp80000001");
assert.equal(blank[0].subtype, BLANK_DATA_SUBTYPE_SPIFFS);
assert.equal(blank[0].required_size, BLANK_DATA_MIN_SIZE);
assert.equal(blank[0].initial_image_size, 0);
assert.equal(blank[0].data.length, 0);
assert.match(blank[0].label, /^[A-Za-z0-9_-]{1,16}$/);
assert.throws(() => makeBlankAppDataImage(0), /invalid APP\/DATA owner id/);
assert.throws(() => makeBlankAppDataImage(0xffffffff), /invalid APP\/DATA owner id/);
console.log("Optional APP DATA allocation policy: PASS (8 assertions)");
