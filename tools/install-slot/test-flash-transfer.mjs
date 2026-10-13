#!/usr/bin/env node
import assert from "node:assert/strict";
import { writeFlashChunked, FLASH_WRITE_CHUNK_BYTES } from "../../install-slot/flash-transfer.js";

function binaryToBytes(value) {
  return Uint8Array.from(value, (char) => char.charCodeAt(0));
}

{
  const calls = [];
  const progress = [];
  const loader = {
    async writeFlash(options) {
      const item = options.fileArray[0];
      calls.push({ address: item.address, bytes: binaryToBytes(item.data), compress: options.compress });
      options.reportProgress(0, item.data.length / 2, item.data.length);
      options.reportProgress(0, item.data.length, item.data.length);
    },
  };
  const bytes = Uint8Array.from({ length: FLASH_WRITE_CHUNK_BYTES * 2 + 8192 }, (_, i) => i & 0xff);
  await writeFlashChunked(loader, bytes, 0x360000, { onProgress: (p) => progress.push(p) });
  assert.equal(calls.length, 3, "large payload uses bounded 512 KiB calls, not 64 KiB calls");
  assert.deepEqual(calls.map((c) => [c.address, c.bytes.length]), [
    [0x360000, FLASH_WRITE_CHUNK_BYTES],
    [0x360000 + FLASH_WRITE_CHUNK_BYTES, FLASH_WRITE_CHUNK_BYTES],
    [0x360000 + FLASH_WRITE_CHUNK_BYTES * 2, 8192],
  ]);
  assert.ok(calls.every((c) => c.compress === true));
  const reconstructed = new Uint8Array(bytes.length);
  let offset = 0;
  for (const call of calls) { reconstructed.set(call.bytes, offset); offset += call.bytes.length; }
  assert.deepEqual(reconstructed, bytes, "chunk boundaries preserve every payload byte");
  assert.equal(progress.at(-1), 1);
  assert.ok(progress.every((p, i) => i === 0 || p >= progress[i - 1]), "progress is monotonic");
  console.log("PASS 1: bounded chunked writes preserve bytes, addresses, compression, and monotonic progress");
}

{
  const calls = [];
  await assert.rejects(() => writeFlashChunked({ writeFlash: async (o) => calls.push(o) },
    new Uint8Array(4096), 0, { chunkBytes: 4097 }), /multiple of the 4 KiB sector size/);
  await assert.rejects(() => writeFlashChunked({}, new Uint8Array(1), 0), /flash writer is required/);
  await assert.rejects(() => writeFlashChunked({ writeFlash: async () => {} }, new Uint8Array(1), -1), /invalid flash address/);
  assert.equal(calls.length, 0, "invalid inputs fail before device writes");
  console.log("PASS 2: invalid chunk geometry and addresses are rejected before I/O");
}

{
  let calls = 0;
  let progress = -1;
  await writeFlashChunked({ async writeFlash() { calls++; } }, new Uint8Array(0), 0,
    { onProgress: (p) => { progress = p; } });
  assert.equal(calls, 0);
  assert.equal(progress, 1);
  console.log("PASS 3: empty payload is a no-op and completes progress without device I/O");
}
