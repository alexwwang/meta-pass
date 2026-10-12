// install-slot/data-archive-restore.js
// Shared, transport-neutral preparation for restoring one MPTB v2 APP+DATA group.
// The phone HTTP bridge and USB Web Serial path must agree on archive semantics;
// only their allocation/write/commit adapters differ.
import {
  validateSingleGroupRestoreManifest,
  validateDataArchivePayloads,
} from "./data-byte-archive.js";

export async function prepareDataArchiveRestore(manifest, payloads, {
  sha256Hex,
  extractAppImage,
  dataSizeGranule = 4096,
  maxImageBytes = 0x7ffff000,
} = {}) {
  if (typeof extractAppImage !== "function") throw new Error("APP image extractor is required");
  if (!Number.isSafeInteger(dataSizeGranule) || dataSizeGranule < 1 ||
      (dataSizeGranule & (dataSizeGranule - 1)) !== 0) {
    throw new Error("DATA size granule must be a positive power of two");
  }

  const slot = validateSingleGroupRestoreManifest(manifest);
  await validateDataArchivePayloads(manifest, payloads, sha256Hex);

  const get = (name) => payloads instanceof Map ? payloads.get(name) : payloads?.[name];
  const rawFiles = slot.files.filter((file) =>
    file.type === "raw" || file.name.endsWith("_raw.bin"));
  const appFiles = slot.files.filter((file) =>
    file.type === "app" || file.name.endsWith("_firmware.bin"));
  const tailFiles = slot.files.filter((file) =>
    file.name.endsWith("_tail.bin") || file.type === "tail");
  const extraFiles = slot.files.filter((file) =>
    !rawFiles.includes(file) && !appFiles.includes(file) && !tailFiles.includes(file));

  if (rawFiles.length + appFiles.length !== 1 || extraFiles.length ||
      tailFiles.length > 1 ||
      (rawFiles.length && (slot.files.length !== 1 || tailFiles.length))) {
    throw new Error("archive must contain one raw-slot or APP image and at most one tail metadata sector; extra slot payloads are unsupported");
  }

  const appFile = rawFiles[0] || appFiles[0];
  const rawApp = get(appFile.name);
  if (!(rawApp instanceof Uint8Array)) throw new Error("APP payload missing");

  let image = extractAppImage(rawApp, maxImageBytes);
  if (rawFiles.length) {
    const tailStart = image.tailSectorOffset;
    const tailEnd = tailStart + 4096;
    if (rawApp.length < tailEnd) {
      throw new Error("raw-slot archive is shorter than its APP tail metadata sector");
    }
    const tailBytes = rawApp.subarray(tailStart, tailEnd);
    if (!image.tailSector && tailBytes.some((byte) => byte !== 0xff)) {
      throw new Error("raw-slot archive contains an unrecognized non-erased tail sector");
    }
    if (rawApp.subarray(tailEnd).some((byte) => byte !== 0xff)) {
      throw new Error("raw-slot archive contains unsupported non-erased extra bytes after the tail sector");
    }
  }

  if (tailFiles.length) {
    const tail = get(tailFiles[0].name);
    if (!(tail instanceof Uint8Array) || tail.length !== 4096) {
      throw new Error("tail metadata payload must be exactly one 4 KiB sector");
    }
    image = { ...image, tailSector: tail, tailSectorOffset: Math.ceil(image.length / 4096) * 4096 };
  }

  const dataImages = manifest.data.map((record) => {
    const bytes = get(record.file);
    if (!(bytes instanceof Uint8Array) || bytes.length !== record.bytes) {
      throw new Error("DATA payload missing or length mismatch: " + record.file);
    }
    const requiredSize = Math.ceil(record.bytes / dataSizeGranule) * dataSizeGranule;
    if (!Number.isSafeInteger(requiredSize) || requiredSize < record.bytes) {
      throw new Error("DATA allocation size overflow: " + record.file);
    }
    return {
      playId: slot.play_id,
      label: record.label,
      subtype: record.subtype,
      required_size: requiredSize,
      initial_image_size: bytes.length,
      data: bytes,
      sha256: record.sha256,
    };
  });

  return Object.freeze({
    slot,
    image,
    appSha256: await sha256Hex(image.data),
    dataImages: Object.freeze(dataImages),
  });
}
