#!/usr/bin/env node
import assert from "node:assert/strict";
import { buildDataArchiveZip, readDataArchiveZip } from "../../install-slot/data-byte-archive.js";

const enc = new TextEncoder();
const findAll = (bytes, needle) => {
  const out = [];
  outer: for (let i = 0; i <= bytes.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) if (bytes[i + j] !== needle[j]) continue outer;
    out.push(i);
  }
  return out;
};
const replaceAll = (bytes, from, to) => {
  assert.equal(from.length, to.length);
  const out = bytes.slice();
  const positions = findAll(out, enc.encode(from));
  assert.ok(positions.length >= 1, `missing ZIP filename ${from}`);
  for (const pos of positions) out.set(enc.encode(to), pos);
  return out;
};

const originalPayload = enc.encode("verified bytes");
const good = buildDataArchiveZip([
  { name: "payload", data: originalPayload },
  { name: "manifest.json", data: enc.encode('{"manifest_version":2}') },
]);
{
  const entries = readDataArchiveZip(good);
  assert.equal(entries.size, 2);
  assert.deepEqual(entries.get("payload"), originalPayload);
  console.log("PASS 1: STORE ZIP round-trips payload bytes and manifest");
}
{
  const bad = good.slice();
  const nameLength = new DataView(bad.buffer, bad.byteOffset, bad.byteLength).getUint16(26, true);
  bad[30 + nameLength] ^= 0x01;
  assert.throws(() => readDataArchiveZip(bad), /ZIP CRC mismatch/);
  console.log("PASS 2: payload corruption fails CRC before archive entries are accepted");
}
{
  const traversal = replaceAll(good, "payload", "../evil");
  assert.throws(() => readDataArchiveZip(traversal), /unsafe or duplicate ZIP path/);
  console.log("PASS 3: path traversal is rejected in ZIP entry names");
}
{
  const two = buildDataArchiveZip([
    { name: "a.txt", data: enc.encode("one") },
    { name: "b.txt", data: enc.encode("two") },
  ]);
  const duplicate = replaceAll(two, "b.txt", "a.txt");
  assert.throws(() => readDataArchiveZip(duplicate), /unsafe or duplicate ZIP path/);
  console.log("PASS 4: duplicate names across ZIP records are rejected");
}
{
  const two = buildDataArchiveZip([
    { name: "a.txt", data: enc.encode("one") },
    { name: "b.txt", data: enc.encode("two") },
  ]);
  const overlapping = two.slice();
  const dv = new DataView(overlapping.buffer, overlapping.byteOffset, overlapping.byteLength);
  const central = findAll(overlapping, new Uint8Array([0x50, 0x4b, 0x01, 0x02]));
  assert.equal(central.length, 2);
  const firstLocalOffset = dv.getUint32(central[0] + 42, true);
  dv.setUint32(central[1] + 42, firstLocalOffset, true);
  assert.throws(() => readDataArchiveZip(overlapping), /overlapping ZIP local records/);
  console.log("PASS 5: central entries cannot alias/overlap local ZIP records");
}
{
  const compressed = good.slice();
  const dv = new DataView(compressed.buffer, compressed.byteOffset, compressed.byteLength);
  dv.setUint16(8, 8, true); // local header compression method
  const central = findAll(compressed, new Uint8Array([0x50, 0x4b, 0x01, 0x02]));
  assert.ok(central.length >= 1);
  dv.setUint16(central[0] + 10, 8, true);
  assert.throws(() => readDataArchiveZip(compressed), /unsupported ZIP entry flags\/compression\/size/);
  console.log("PASS 6: unsupported compression methods are rejected");
}
{
  assert.throws(() => readDataArchiveZip(good.subarray(0, good.length - 1)), /ZIP end record missing|unsupported or malformed ZIP directory/);
  assert.throws(() => readDataArchiveZip(good, { maxArchiveBytes: 16 }), /ZIP archive size is outside the allowed range/);
  assert.throws(() => readDataArchiveZip(good, { maxEntries: 1 }), /unsupported or malformed ZIP directory/);
  console.log("PASS 7: truncated, oversized, and over-entry-limit archives are rejected");
}
console.log("ALL DATA ARCHIVE ZIP TESTS PASSED");
