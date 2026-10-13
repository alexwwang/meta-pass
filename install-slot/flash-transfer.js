// install-slot/flash-transfer.js
// Bounded, sector-aligned Web Serial writes. A single writeFlash call per 512 KiB
// chunk avoids the repeated command/setup overhead of 64 KiB calls while keeping
// each transfer bounded for browser serial transports.
export const FLASH_WRITE_CHUNK_BYTES = 512 * 1024;
export const FLASH_SECTOR_BYTES = 4096;

function fail(message) {
  throw new Error(message);
}

function toBinaryString(bytes) {
  const parts = [];
  const conversionChunk = 0x8000;
  for (let i = 0; i < bytes.length; i += conversionChunk) {
    parts.push(String.fromCharCode.apply(null, bytes.subarray(i, Math.min(bytes.length, i + conversionChunk))));
  }
  return parts.join("");
}

export async function writeFlashChunked(loader, bytes, address, {
  chunkBytes = FLASH_WRITE_CHUNK_BYTES,
  onProgress,
} = {}) {
  if (!loader || typeof loader.writeFlash !== "function") fail("flash writer is required");
  if (!(bytes instanceof Uint8Array)) fail("flash payload must be Uint8Array");
  if (!Number.isSafeInteger(address) || address < 0 ||
      !Number.isSafeInteger(address + bytes.length)) fail("invalid flash address or payload length");
  if (!Number.isSafeInteger(chunkBytes) || chunkBytes < FLASH_SECTOR_BYTES ||
      chunkBytes % FLASH_SECTOR_BYTES !== 0) {
    fail("flash chunk size must be a positive multiple of the 4 KiB sector size");
  }
  if (bytes.length === 0) {
    onProgress?.(1);
    return;
  }

  let written = 0;
  while (written < bytes.length) {
    const chunk = bytes.subarray(written, Math.min(bytes.length, written + chunkBytes));
    const chunkStart = written;
    await loader.writeFlash({
      fileArray: [{ data: toBinaryString(chunk), address: address + chunkStart }],
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      compress: true,
      reportProgress: (_index, chunkWritten, chunkTotal) => {
        const fraction = Number.isFinite(chunkWritten) && Number.isFinite(chunkTotal) && chunkTotal > 0
          ? Math.max(0, Math.min(1, chunkWritten / chunkTotal))
          : 0;
        onProgress?.((chunkStart + fraction * chunk.length) / bytes.length);
      },
    });
    written += chunk.length;
    onProgress?.(written / bytes.length);
  }
}
