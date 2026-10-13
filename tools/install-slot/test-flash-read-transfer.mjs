#!/usr/bin/env node
import assert from "node:assert/strict";
import { readFlashAndVerifyChunked, FLASH_READ_CHUNK_BYTES, FLASH_READ_SESSION_BYTES } from "../../install-slot/flash-read-transfer.js";

{
  const expected = Uint8Array.from({ length: FLASH_READ_SESSION_BYTES + 8192 }, (_, i) => (i * 17) & 0xff);
  const calls = [], progress = [];
  let reconnects = 0;
  await readFlashAndVerifyChunked({
    address: 0x360000, expected,
    readChunk: async (address, length) => {
      calls.push({ address, length });
      const offset = address - 0x360000;
      return expected.slice(offset, offset + length);
    },
    reconnect: async () => { reconnects++; },
    onProgress: (p) => progress.push(p),
  });
  assert.deepEqual(calls.map((c) => c.length), [
    ...Array(16).fill(FLASH_READ_CHUNK_BYTES), 8192,
  ]);
  assert.equal(calls[0].address, 0x360000);
  assert.equal(calls.at(-1).address, 0x360000 + FLASH_READ_SESSION_BYTES + FLASH_READ_CHUNK_BYTES);
  assert.equal(reconnects, 1, "reopen the transport after each 512 KiB session budget");
  assert.equal(progress.at(-1), 1);
  assert.ok(progress.every((p, i) => i === 0 || p >= progress[i - 1]));
  console.log("PASS 1: bounded Flash reads verify byte-for-byte and reconnect at session boundaries");
}
{
  await assert.rejects(() => readFlashAndVerifyChunked({
    address: 0, expected: new Uint8Array([1, 2, 3, 4]),
    readChunk: async () => new Uint8Array([1, 9, 3, 4]), reconnect: async () => {},
  }), /read-back mismatch at \+0x1/);
  await assert.rejects(() => readFlashAndVerifyChunked({
    address: 0, expected: new Uint8Array(4096),
    readChunk: async () => new Uint8Array(3), reconnect: async () => {},
  }), /short\/invalid Flash read/);
  console.log("PASS 2: mismatched and short reads fail closed");
}
{
  let reads = 0, reconnects = 0, p = 0;
  await readFlashAndVerifyChunked({
    address: 0, expected: new Uint8Array(0),
    readChunk: async () => { reads++; return new Uint8Array(); },
    reconnect: async () => { reconnects++; }, onProgress: (v) => { p = v; },
  });
  assert.equal(reads, 0);
  assert.equal(reconnects, 0);
  assert.equal(p, 1);
  console.log("PASS 3: empty payload is a no-op");
}
