#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { verifyFlashRangeByMd5 } from "../../install-slot/flash-readback-verify.js";

const md5Bytes = (bytes) => new Uint8Array(createHash("md5").update(bytes).digest());
const md5Hex = (bytes) => createHash("md5").update(bytes).digest("hex");

{
  const expected = Uint8Array.from({ length: 0x18000 }, (_, i) => (i * 31 + 7) & 0xff);
  const progress = [];
  const calls = [];
  const result = await verifyFlashRangeByMd5({
    address: 0x360000,
    expected,
    flashMd5sum: async (address, length) => {
      calls.push({ address, length });
      return md5Hex(expected);
    },
    md5Bytes,
    onProgress: (p) => progress.push(p),
  });
  assert.deepEqual(calls, [{ address: 0x360000, length: expected.length }]);
  assert.equal(result.expectedMd5, result.actualMd5);
  assert.equal(result.length, expected.length);
  assert.deepEqual(progress, [0, 1]);
  console.log("PASS 1: verifies a large range with one device-side digest request");
}

{
  const expected = new Uint8Array([1, 2, 3, 4]);
  await assert.rejects(() => verifyFlashRangeByMd5({
    address: 0x180000, expected,
    flashMd5sum: async () => "00000000000000000000000000000000",
    md5Bytes,
  }), /Flash read-back MD5 mismatch/);
  await assert.rejects(() => verifyFlashRangeByMd5({
    address: 0x180000, expected,
    flashMd5sum: async () => "not-a-digest",
    md5Bytes,
  }), /invalid digest/);
  console.log("PASS 2: digest mismatch and malformed device response fail closed");
}

{
  const expected = new Uint8Array([1]);
  await assert.rejects(() => verifyFlashRangeByMd5({
    address: 0, expected,
    flashMd5sum: async () => { throw new Error("serial link lost"); },
    md5Bytes,
  }), /serial link lost/);
  await assert.rejects(() => verifyFlashRangeByMd5({
    address: 0, expected: new Uint8Array(0),
    flashMd5sum: async () => md5Hex(new Uint8Array(0)),
    md5Bytes,
  }), /empty payload/);
  await assert.rejects(() => verifyFlashRangeByMd5({
    address: 0, expected,
    flashMd5sum: async () => md5Hex(expected),
    md5Bytes: () => new Uint8Array(15),
  }), /exactly 16 bytes/);
  console.log("PASS 3: transport errors, empty ranges, and invalid local digest fail closed");
}
