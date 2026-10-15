#!/usr/bin/env node
import assert from "node:assert/strict";
import { resolveAppDataChoice } from "../../install-slot/data-allocation-choice.js";

const declared = [{ label: "recordings", subtype: 0x81, required_size: 0x10000, initial_image_size: 0, data: new Uint8Array() }];
assert.deepEqual(resolveAppDataChoice({ allocate: false, declaredImages: declared }), [], "declining allocation must produce no DATA records");
assert.equal(resolveAppDataChoice({ allocate: true, declaredImages: declared })[0], declared[0], "declared DATA descriptors are retained exactly");
assert.throws(() => resolveAppDataChoice({ allocate: true, declaredImages: [] }), /declares no DATA partition\/access contract/, "must not invent a label, subtype, or filesystem contract");
assert.throws(() => resolveAppDataChoice({ allocate: true, declaredImages: null }), /declaredImages must be an array/);
console.log("Optional APP DATA allocation policy: PASS (4 assertions)");
