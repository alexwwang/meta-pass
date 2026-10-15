// Shared optional DATA allocation policy for USB and embedded-phone installers.
// Blank allocations use a registered ESP-IDF SPIFFS data subtype (0x82), never
// the test-only e2edata subtype. The app-scoped label is stable and <= 16 bytes.
export const BLANK_DATA_SUBTYPE_SPIFFS = 0x82;
export const BLANK_DATA_MIN_SIZE = 64 * 1024;

export function makeBlankAppDataImage(playId) {
  if (!Number.isInteger(playId) || playId <= 0 || playId >= 0xffffffff) {
    throw new RangeError("invalid APP/DATA owner id");
  }
  const label = `mp${(playId >>> 0).toString(16)}`;
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(label)) throw new RangeError("invalid blank DATA label");
  return {
    kind: "data", label, subtype: BLANK_DATA_SUBTYPE_SPIFFS,
    required_size: BLANK_DATA_MIN_SIZE, initial_image_size: 0,
    data: new Uint8Array(0),
  };
}

// Explicit choice semantics: declining allocation means no DATA descriptors at
// all; accepting keeps all declared DATA images, or creates one blank private
// SPIFFS extent when the APP image declares none.
export function resolveAppDataChoice({ allocate, declaredImages, playId }) {
  if (!Array.isArray(declaredImages)) throw new TypeError("declaredImages must be an array");
  if (!allocate) return [];
  if (declaredImages.length) return declaredImages.slice();
  return [makeBlankAppDataImage(playId)];
}
