#!/usr/bin/env node
import assert from "node:assert/strict";
import { buildDataSizeOptions } from "../../install-slot/data-size-options.js";

const basic = buildDataSizeOptions(3 * 1024 * 1024, 5 * 1024 * 1024, 4 * 1024 * 1024, 1024 * 1024);
assert.deepEqual(basic.values, [3, 4, 5].map((n) => n * 1024 * 1024));
assert.equal(basic.defaultSize, 4 * 1024 * 1024);

const declaredOffStep = buildDataSizeOptions(3 * 1024 * 1024, 5 * 1024 * 1024,
  3 * 1024 * 1024 + 4096, 1024 * 1024);
assert.ok(declaredOffStep.values.includes(3 * 1024 * 1024 + 4096));
assert.equal(declaredOffStep.defaultSize, 3 * 1024 * 1024 + 4096);

const minimumOnly = buildDataSizeOptions(5 * 1024 * 1024, 5 * 1024 * 1024, 1, 1024 * 1024);
assert.deepEqual(minimumOnly.values, [5 * 1024 * 1024]);
assert.equal(minimumOnly.defaultSize, 5 * 1024 * 1024);

assert.throws(() => buildDataSizeOptions(0, 4096, 4096, 4096), /invalid DATA size option bounds/);
assert.throws(() => buildDataSizeOptions(4096, 8192, 4096, 0), /invalid DATA size option bounds/);
console.log("DATA size options: PASS (5 cases)");
