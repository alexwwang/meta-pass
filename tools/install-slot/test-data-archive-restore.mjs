#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { buildDataArchiveManifest } from "../../install-slot/data-byte-archive.js";
import { prepareDataArchiveRestore } from "../../install-slot/data-archive-restore.js";

const sha256Hex = async (bytes) => createHash("sha256").update(bytes).digest("hex");
const app = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
const extractAppImage = (bytes, maxBytes) => {
  assert.equal(maxBytes, 0x7ffff000);
  return { data: bytes.slice(0, 4), length: 4, tailSector: null, tailSectorOffset: 4096 };
};

async function fixture(dataBytes) {
  const manifest = buildDataArchiveManifest({
    createdUtc: "2026-10-12T00:00:00.000Z",
    slots: [{ play_id: 42, source_slot: 0, name: "restore-test",
      files: [{ name: "app.bin", bytes: app.length, sha256: await sha256Hex(app), type: "app" }] }],
    data: [{ play_id: 42, label: "cfg_save", type: 1, subtype: 0x82, state: 1,
      bytes: dataBytes.length, sha256: await sha256Hex(dataBytes) }],
  });
  return { manifest, payloads: new Map([["app.bin", app], [manifest.data[0].file, dataBytes]]) };
}

{
  const data = new Uint8Array(4096).fill(0x5a);
  const { manifest, payloads } = await fixture(data);
  const prepared = await prepareDataArchiveRestore(manifest, payloads, {
    sha256Hex, extractAppImage, dataSizeGranule: 4096,
  });
  assert.equal(prepared.slot.play_id, 42);
  assert.deepEqual([...prepared.image.data], [1, 2, 3, 4]);
  assert.equal(prepared.dataImages.length, 1);
  assert.equal(prepared.dataImages[0].required_size, 4096);
  assert.equal(prepared.dataImages[0].initial_image_size, 4096);
  assert.equal(prepared.dataImages[0].sha256, await sha256Hex(data));
  console.log("PASS 1: shared APP+DATA archive preparation returns verified, aligned payloads");
}

{
  const data = new Uint8Array(4095).fill(0x5a);
  const { manifest, payloads } = await fixture(data);
  await assert.rejects(
    prepareDataArchiveRestore(manifest, payloads, {
      sha256Hex, extractAppImage, dataSizeGranule: 4096,
    }),
    /not aligned to the allocation granule/,
  );
  console.log("PASS 2: unaligned DATA archive payload is rejected consistently before allocation");
}

{
  const data = new Uint8Array(4096).fill(0x5a);
  const { manifest, payloads } = await fixture(data);
  await assert.rejects(
    prepareDataArchiveRestore(manifest, payloads, {
      sha256Hex, extractAppImage: null,
    }),
    /APP image extractor is required/,
  );
  console.log("PASS 3: missing transport-provided APP extractor is rejected");
}
