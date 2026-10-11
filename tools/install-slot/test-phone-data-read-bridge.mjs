#!/usr/bin/env node
import assert from "node:assert/strict";
import { createBridge } from "../../install-slot/phone-install.js";

const originalFetch = globalThis.fetch;
try {
  let calls = [];
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return {
      ok: true,
      status: 200,
      arrayBuffer: async () => Uint8Array.from([0x00, 0xff, 0x80, 0x41]).buffer,
    };
  };
  const bridge = createBridge("http://device.local", "session-secret");
  const result = await bridge.dataRead(123, "cfg_save", 4096, 4);
  assert.equal(result.ok, true);
  assert.equal(result.status, 200);
  assert.deepEqual([...result.bytes], [0x00, 0xff, 0x80, 0x41]);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, "http://device.local/api/install/data/read");
  assert.equal(calls[0].options.headers["X-Meta-Session"], "session-secret");
  assert.equal(calls[0].options.headers["X-Meta-Play-Id"], "123");
  assert.equal(calls[0].options.headers["X-Meta-Data-Label"], "cfg_save");
  assert.equal(calls[0].options.headers["X-Meta-Offset"], "4096");
  assert.equal(calls[0].options.headers["X-Meta-Length"], "4");
  console.log("PASS 1: mobile bridge preserves arbitrary DATA bytes and sends identity/range/session headers");

  calls = [];
  const invalid = await bridge.dataRead(123, "../cfg", 0, 4);
  assert.equal(invalid.ok, false);
  assert.equal(invalid.status, 400);
  assert.equal(calls.length, 0);
  assert.equal((await bridge.dataRead(123, "cfg", 0, 4097)).ok, false);
  assert.equal((await bridge.dataRead(0, "cfg", 0, 4)).ok, false);
  assert.equal(calls.length, 0);
  console.log("PASS 2: invalid identity and out-of-range chunk lengths are rejected before network access");

  globalThis.fetch = async () => ({
    ok: false, status: 416, text: async () => "DATA read range out of bounds",
  });
  const rejected = await bridge.dataRead(123, "cfg_save", 65535, 4);
  assert.equal(rejected.ok, false);
  assert.equal(rejected.status, 416);
  assert.equal(rejected.text, "DATA read range out of bounds");
  assert.equal(rejected.bytes, null);
  console.log("PASS 3: device range errors propagate without pretending to return bytes");
} finally {
  globalThis.fetch = originalFetch;
}
