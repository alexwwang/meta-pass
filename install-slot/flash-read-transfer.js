// install-slot/flash-read-transfer.js
// Read-and-verify a Flash range in bounded esptool-js calls, reopening the
// Web Serial transport at fixed byte intervals to avoid a long-lived read stream.
export const FLASH_READ_CHUNK_BYTES = 0x8000;       // 32 KiB; keeps cumulative ACK SLIP-safe
export const FLASH_READ_SESSION_BYTES = 512 * 1024; // empirical stable transfer budget per session

function fail(message) { throw new Error(message); }

export async function readFlashAndVerifyChunked({
  address,
  expected,
  readChunk,
  reconnect,
  onProgress,
  chunkBytes = FLASH_READ_CHUNK_BYTES,
  sessionBytes = FLASH_READ_SESSION_BYTES,
}) {
  if (!Number.isSafeInteger(address) || address < 0) fail("invalid flash address");
  if (!(expected instanceof Uint8Array)) fail("expected payload must be Uint8Array");
  if (typeof readChunk !== "function") fail("readChunk callback is required");
  if (typeof reconnect !== "function") fail("reconnect callback is required");
  if (!Number.isSafeInteger(chunkBytes) || chunkBytes <= 0 || chunkBytes % 4096 !== 0) {
    fail("read chunk size must be a positive multiple of the 4 KiB sector size");
  }
  if (!Number.isSafeInteger(sessionBytes) || sessionBytes < chunkBytes || sessionBytes % chunkBytes !== 0) {
    fail("session byte budget must be a positive multiple of read chunk size");
  }
  let offset = 0;
  while (offset < expected.length) {
    const length = Math.min(chunkBytes, expected.length - offset);
    const actual = await readChunk(address + offset, length);
    if (!(actual instanceof Uint8Array) || actual.length !== length) {
      fail(`short/invalid Flash read at +0x${offset.toString(16)}: expected ${length} bytes`);
    }
    for (let i = 0; i < length; i++) {
      if (actual[i] !== expected[offset + i]) {
        fail(`Flash read-back mismatch at +0x${(offset + i).toString(16)}`);
      }
    }
    offset += length;
    onProgress?.(expected.length === 0 ? 1 : offset / expected.length);
    if (offset < expected.length && offset % sessionBytes === 0) {
      await reconnect();
    }
  }
  if (expected.length === 0) onProgress?.(1);
}
