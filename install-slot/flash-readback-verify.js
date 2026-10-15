// install-slot/flash-readback-verify.js
// Verify a written Flash range using the ESP stub's device-side MD5 command.
// This avoids transferring the entire range back through Web Serial (which can
// wedge after a bounded number of readFlash calls on some Chrome/macOS hosts).
// MD5 is used as an accidental-corruption check, not as an authenticity proof.

function fail(message) { throw new Error(message); }

function digestHex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

export async function verifyFlashRangeByMd5({
  address,
  expected,
  flashMd5sum,
  md5Bytes,
  onProgress,
}) {
  if (!Number.isSafeInteger(address) || address < 0) fail("invalid flash address");
  if (!(expected instanceof Uint8Array)) fail("expected payload must be Uint8Array");
  if (expected.length === 0) fail("empty payload must use the explicit erase-verification path");
  if (typeof flashMd5sum !== "function") fail("flashMd5sum callback is required");
  if (typeof md5Bytes !== "function") fail("md5Bytes callback is required");

  const expectedDigest = md5Bytes(expected);
  if (!(expectedDigest instanceof Uint8Array) || expectedDigest.length !== 16) {
    fail("local MD5 implementation must return exactly 16 bytes");
  }
  const expectedMd5 = digestHex(expectedDigest);
  onProgress?.(0);

  // esptool-js' flashMd5sum uses the ESP stub command and returns a 32-hex digest.
  // Its command timeout scales with the requested range; do not add a shorter
  // outer timeout that would reject valid large DATA partitions.
  const actual = await flashMd5sum(address, expected.length);
  if (typeof actual !== "string" || !/^[0-9a-f]{32}$/i.test(actual)) {
    fail("device Flash MD5 returned an invalid digest");
  }
  const actualMd5 = actual.toLowerCase();
  if (actualMd5 !== expectedMd5) {
    fail(`Flash read-back MD5 mismatch at 0x${address.toString(16)}+0x${expected.length.toString(16)}: expected ${expectedMd5}, got ${actualMd5}`);
  }

  onProgress?.(1);
  return { address, length: expected.length, expectedMd5, actualMd5 };
}
