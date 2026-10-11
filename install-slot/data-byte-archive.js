// install-slot/data-byte-archive.js
// Shared, browser/Node-safe validation for MPTB v2 archives. ZIP I/O stays in
// the callers; this module validates logical ownership and exact payload bytes.
export const DATA_ARCHIVE_MANIFEST_VERSION = 2;
export const DATA_ARCHIVE_KIND = "meta-pass-slot-backup";
export const DATA_ARCHIVE_MAX_FILES = 64;
export const DATA_ARCHIVE_MAX_LABEL_BYTES = 16;
const SHA256_RE = /^[0-9a-f]{64}$/;

function fail(message) {
  const error = new Error(message);
  error.code = "INVALID_DATA_ARCHIVE";
  throw error;
}

function safeLabel(label) {
  return typeof label === "string" && label.length > 0 &&
    label.length <= DATA_ARCHIVE_MAX_LABEL_BYTES &&
    /^[A-Za-z0-9_-]+$/.test(label);
}

function safeFileName(name) {
  return typeof name === "string" && name.length > 0 &&
    !name.startsWith("/") && !name.includes("\\") &&
    !name.split("/").some((part) => !part || part === "." || part === "..") &&
    !/^[A-Za-z]:/.test(name);
}

function validateFileRecord(file, seenNames) {
  if (!file || !safeFileName(file.name) || seenNames.has(file.name)) {
    fail("invalid or duplicate archive file path");
  }
  if (!Number.isSafeInteger(file.bytes) || file.bytes <= 0) {
    fail("invalid archive file length");
  }
  if (typeof file.sha256 !== "string" || !SHA256_RE.test(file.sha256)) {
    fail("invalid archive file SHA-256");
  }
  seenNames.add(file.name);
}

export function buildDataArchiveManifest({ createdUtc = new Date().toISOString(), slots, data, complete = true, missingSlots = [] }) {
  if (!Array.isArray(slots) || !Array.isArray(data)) fail("slots and data must be arrays");
  if (typeof complete !== "boolean" || !Array.isArray(missingSlots) ||
      missingSlots.some((slot) => !Number.isInteger(slot) || slot < 0) ||
      new Set(missingSlots).size !== missingSlots.length ||
      (complete && missingSlots.length !== 0) || (!complete && missingSlots.length === 0)) {
    fail("invalid archive completeness metadata");
  }
  if (data.length > DATA_ARCHIVE_MAX_FILES) fail("too many DATA records");
  const slotIds = new Set();
  const sourceSlots = new Set();
  const normalizedSlots = slots.map((slot) => {
    if (!slot || !Number.isSafeInteger(slot.play_id) || slot.play_id <= 0 ||
        !Number.isInteger(slot.source_slot) || slot.source_slot < 0 ||
        typeof slot.name !== "string" || !Array.isArray(slot.files)) {
      fail("invalid APP slot entry");
    }
    if (slotIds.has(slot.play_id)) fail("duplicate APP play_id");
    if (sourceSlots.has(slot.source_slot)) fail("duplicate source slot");
    if (slot.files.length === 0) fail("APP slot has no payload files");
    slotIds.add(slot.play_id);
    sourceSlots.add(slot.source_slot);
    return {
      play_id: slot.play_id,
      source_slot: slot.source_slot,
      name: slot.name,
      files: slot.files.map((f) => ({ name: f.name, bytes: f.bytes, sha256: f.sha256, type: f.type || (f.name.endsWith("_firmware.bin") ? "app" : f.name.endsWith("_raw.bin") ? "raw" : "other") })),
    };
  });
  const seenOwnership = new Set();
  const normalizedData = data.map((record, index) => {
    if (!record || !Number.isSafeInteger(record.play_id) || record.play_id <= 0 ||
        !slotIds.has(record.play_id)) fail("DATA record has no matching APP payload");
    if (!safeLabel(record.label)) fail("invalid DATA label");
    const ownership = record.play_id + ":" + record.label;
    if (seenOwnership.has(ownership)) fail("duplicate DATA ownership key");
    seenOwnership.add(ownership);
    if (!Number.isInteger(record.type) || record.type < 0 || record.type > 255 ||
        !Number.isInteger(record.subtype) || record.subtype < 0 || record.subtype > 255 ||
        !Number.isInteger(record.state) || record.state < 0 || record.state > 2) {
      fail("invalid DATA type, subtype, or state");
    }
    if (!Number.isSafeInteger(record.bytes) || record.bytes <= 0) fail("invalid DATA byte length");
    if (typeof record.sha256 !== "string" || !SHA256_RE.test(record.sha256)) fail("invalid DATA SHA-256");
    const name = `data/${record.play_id}/${index}.bin`;
    return {
      play_id: record.play_id, label: record.label, type: record.type,
      subtype: record.subtype, state: record.state, bytes: record.bytes,
      sha256: record.sha256, file: name,
    };
  });
  const manifest = {
    manifest_version: DATA_ARCHIVE_MANIFEST_VERSION,
    kind: DATA_ARCHIVE_KIND,
    created_utc: createdUtc,
    complete,
    ...(missingSlots.length ? { missing_slots: [...missingSlots].sort((a, b) => a - b) } : {}),
    slots: normalizedSlots,
    data: normalizedData,
  };
  validateDataArchiveManifest(manifest);
  return manifest;
}

export function validateDataArchiveManifest(manifest) {
  if (!manifest || manifest.kind !== DATA_ARCHIVE_KIND) fail("archive kind mismatch");
  if (manifest.manifest_version !== DATA_ARCHIVE_MANIFEST_VERSION) {
    fail("DATA bytes require manifest_version 2; v1 archives do not contain DATA bytes");
  }
  if (typeof manifest.created_utc !== "string" || !Number.isFinite(Date.parse(manifest.created_utc))) {
    fail("invalid archive creation timestamp");
  }
  if (manifest.complete !== undefined || manifest.missing_slots !== undefined) {
    const missing = manifest.missing_slots ?? [];
    if (typeof manifest.complete !== "boolean" || !Array.isArray(missing) ||
        missing.some((slot) => !Number.isInteger(slot) || slot < 0) ||
        new Set(missing).size !== missing.length ||
        (manifest.complete && missing.length !== 0) || (!manifest.complete && missing.length === 0)) {
      fail("invalid archive completeness metadata");
    }
  }
  if (!Array.isArray(manifest.slots) || manifest.slots.length === 0 || !Array.isArray(manifest.data)) {
    fail("manifest must contain slots and data arrays");
  }
  if (manifest.data.length > DATA_ARCHIVE_MAX_FILES) fail("too many DATA records");
  const playIds = new Set();
  const sourceSlots = new Set();
  const appPayloadPlayIds = new Set();
  const seenNames = new Set();
  for (const slot of manifest.slots) {
    if (!slot || !Number.isSafeInteger(slot.play_id) || slot.play_id <= 0 ||
        !Number.isInteger(slot.source_slot) || slot.source_slot < 0 ||
        typeof slot.name !== "string" || !Array.isArray(slot.files)) fail("invalid APP slot entry");
    if (playIds.has(slot.play_id)) fail("duplicate APP play_id");
    if (sourceSlots.has(slot.source_slot)) fail("duplicate source slot");
    if (slot.files.length === 0) fail("APP slot has no payload files");
    playIds.add(slot.play_id);
    sourceSlots.add(slot.source_slot);
    for (const file of slot.files) {
      validateFileRecord(file, seenNames);
      if (file.type === "app" || file.type === "raw" ||
          file.name.endsWith("_firmware.bin") || file.name.endsWith("_raw.bin")) {
        appPayloadPlayIds.add(slot.play_id);
      }
    }
  }
  const ownership = new Set();
  manifest.data.forEach((record, index) => {
    if (!record || !Number.isSafeInteger(record.play_id) || !playIds.has(record.play_id) ||
        !appPayloadPlayIds.has(record.play_id)) {
      fail("DATA record has no matching APP payload");
    }
    if (!safeLabel(record.label)) fail("invalid DATA label");
    const key = record.play_id + ":" + record.label;
    if (ownership.has(key)) fail("duplicate DATA ownership key");
    ownership.add(key);
    if (!Number.isInteger(record.type) || record.type < 0 || record.type > 255 ||
        !Number.isInteger(record.subtype) || record.subtype < 0 || record.subtype > 255 ||
        !Number.isInteger(record.state) || record.state < 0 || record.state > 2 ||
        !Number.isSafeInteger(record.bytes) || record.bytes <= 0 ||
        typeof record.sha256 !== "string" || !SHA256_RE.test(record.sha256)) {
      fail("invalid DATA record fields");
    }
    const expectedName = `data/${record.play_id}/${index}.bin`;
    if (record.file !== expectedName || !safeFileName(record.file) || seenNames.has(record.file)) {
      fail("invalid or duplicate DATA payload path");
    }
    seenNames.add(record.file);
  });
  if (manifest.nvs != null) {
    validateFileRecord({ name: manifest.nvs.file, bytes: manifest.nvs.bytes, sha256: manifest.nvs.sha256 }, seenNames);
  }
  return manifest;
}

// fileBytes is a Map or plain object keyed by archive path; sha256Hex accepts
// Uint8Array and returns a Promise<string>. No writes should occur before this passes.
export async function validateDataArchivePayloads(manifest, fileBytes, sha256Hex) {
  validateDataArchiveManifest(manifest);
  if (typeof sha256Hex !== "function") fail("SHA-256 implementation is required");
  const get = (name) => fileBytes instanceof Map ? fileBytes.get(name) : fileBytes?.[name];
  const records = [
    ...manifest.slots.flatMap((slot) => slot.files),
    ...manifest.data.map((d) => ({ name: d.file, bytes: d.bytes, sha256: d.sha256 })),
    ...(manifest.nvs ? [{ name: manifest.nvs.file, bytes: manifest.nvs.bytes, sha256: manifest.nvs.sha256 }] : []),
  ];
  for (const record of records) {
    const bytes = get(record.name);
    if (!(bytes instanceof Uint8Array)) fail(`missing archive payload: ${record.name}`);
    if (bytes.byteLength !== record.bytes) fail(`payload length mismatch: ${record.name}`);
    const digest = await sha256Hex(bytes);
    if (digest !== record.sha256) fail(`payload SHA-256 mismatch: ${record.name}`);
  }
  return { ok: true, fileCount: records.length, dataCount: manifest.data.length };
}


// Read one registered DATA reservation through the mobile bridge. Offsets are
// relative to that reservation; the device resolves (play_id, label) and never
// accepts a caller-provided absolute Flash address. The caller must not mark an
// archive complete until all reservations and their APP payloads are verified.
export async function readDataReservation(bridge, record, {
  chunkSize = 4096,
  sha256Hex,
  onProgress = () => {},
  maxBytes = 32 * 1024 * 1024,
} = {}) {
  if (!bridge || typeof bridge.dataRead !== "function") fail("mobile DATA read bridge is required");
  const size = record && Number.isSafeInteger(record.bytes) ? record.bytes : record?.size;
  if (!record || !Number.isSafeInteger(record.play_id) || record.play_id <= 0 ||
      !safeLabel(record.label) || !Number.isSafeInteger(size) ||
      size <= 0 || size > maxBytes) {
    fail("invalid DATA reservation identity or size");
  }
  if (!Number.isInteger(chunkSize) || chunkSize < 1 || chunkSize > 4096) {
    fail("DATA read chunk size must be between 1 and 4096 bytes");
  }
  if (typeof sha256Hex !== "function") fail("SHA-256 implementation is required");

  const bytes = new Uint8Array(size);
  for (let offset = 0; offset < size; offset += chunkSize) {
    const length = Math.min(chunkSize, size - offset);
    const result = await bridge.dataRead(record.play_id, record.label, offset, length);
    if (!result || !result.ok || !(result.bytes instanceof Uint8Array) ||
        result.bytes.byteLength !== length) {
      const status = result && result.status ? " (HTTP " + result.status + ")" : "";
      fail("DATA read failed at offset " + offset + status + ": " +
        (result && result.text ? result.text : "invalid binary response"));
    }
    bytes.set(result.bytes, offset);
    onProgress({ play_id: record.play_id, label: record.label, completed: offset + length, total: size });
  }
  const sha256 = await sha256Hex(bytes);
  if (typeof sha256 !== "string" || !SHA256_RE.test(sha256)) fail("SHA-256 implementation returned an invalid digest");
  return { bytes, sha256 };
}

// Read an APP slot through the authenticated bridge using relative offsets only.
// Export the full allocated slot as a raw mirror so firmware/tail/extra bytes survive.
export async function readAppSlotReservation(bridge, record, {
  chunkSize = 4096, sha256Hex, onProgress = () => {}, maxBytes = 32 * 1024 * 1024,
} = {}) {
  if (!bridge || typeof bridge.slotRead !== "function") fail("mobile APP read bridge is required");
  const size = record && Number.isSafeInteger(record.bytes) ? record.bytes : record?.size;
  if (!record || !Number.isSafeInteger(record.play_id) || record.play_id <= 0 ||
      !Number.isSafeInteger(size) || size <= 0 || size > maxBytes) {
    fail("invalid APP reservation identity or size");
  }
  if (!Number.isInteger(chunkSize) || chunkSize < 1 || chunkSize > 4096) {
    fail("APP read chunk size must be between 1 and 4096 bytes");
  }
  if (typeof sha256Hex !== "function") fail("SHA-256 implementation is required");
  const bytes = new Uint8Array(size);
  for (let offset = 0; offset < size; offset += chunkSize) {
    const length = Math.min(chunkSize, size - offset);
    const result = await bridge.slotRead(record.play_id, offset, length);
    if (!result || !result.ok || !(result.bytes instanceof Uint8Array) || result.bytes.byteLength !== length) {
      const status = result && result.status ? " (HTTP " + result.status + ")" : "";
      fail("APP read failed at offset " + offset + status + ": " +
        (result && result.text ? result.text : "invalid binary response"));
    }
    bytes.set(result.bytes, offset);
    onProgress({ play_id: record.play_id, completed: offset + length, total: size });
  }
  const sha256 = await sha256Hex(bytes);
  if (typeof sha256 !== "string" || !SHA256_RE.test(sha256)) fail("SHA-256 implementation returned an invalid digest");
  return { bytes, sha256 };
}

function zipCrc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Minimal ZIP writer using STORE (no compression), matching the USB page format.
export function buildDataArchiveZip(entries) {
  if (!Array.isArray(entries) || entries.length < 1 || entries.length > 128) fail("invalid ZIP entry count");
  const enc = new TextEncoder();
  const chunks = [];
  const central = [];
  let offset = 0;
  const seen = new Set();
  for (const entry of entries) {
    if (!entry || !safeFileName(entry.name) || seen.has(entry.name) || !(entry.data instanceof Uint8Array)) fail("invalid or duplicate ZIP entry");
    seen.add(entry.name);
    const nameBytes = enc.encode(entry.name);
    if (nameBytes.length > 65535 || entry.data.length > 0xffffffff) fail("ZIP entry exceeds format limits");
    const crc = zipCrc32(entry.data);
    const local = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true);
    lv.setUint16(6, 0x0800, true); lv.setUint16(8, 0, true);
    lv.setUint16(10, 0, true); lv.setUint16(12, 0x21, true);
    lv.setUint32(14, crc, true); lv.setUint32(18, entry.data.length, true);
    lv.setUint32(22, entry.data.length, true); lv.setUint16(26, nameBytes.length, true);
    lv.setUint16(28, 0, true); local.set(nameBytes, 30);
    chunks.push(local, entry.data);
    central.push({ nameBytes, crc, size: entry.data.length, offset });
    offset += local.length + entry.data.length;
  }
  const centralStart = offset;
  for (const entry of central) {
    const rec = new Uint8Array(46 + entry.nameBytes.length);
    const v = new DataView(rec.buffer);
    v.setUint32(0, 0x02014b50, true); v.setUint16(4, 20, true); v.setUint16(6, 20, true);
    v.setUint16(8, 0x0800, true); v.setUint16(10, 0, true); v.setUint16(12, 0, true);
    v.setUint16(14, 0x21, true); v.setUint32(16, entry.crc, true);
    v.setUint32(20, entry.size, true); v.setUint32(24, entry.size, true);
    v.setUint16(28, entry.nameBytes.length, true); v.setUint32(42, entry.offset, true);
    rec.set(entry.nameBytes, 46); chunks.push(rec); offset += rec.length;
  }
  const end = new Uint8Array(22); const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, central.length, true);
  ev.setUint16(10, central.length, true); ev.setUint32(12, offset - centralStart, true);
  ev.setUint32(16, centralStart, true); chunks.push(end);
  const total = chunks.reduce((n, part) => n + part.length, 0);
  const zip = new Uint8Array(total); let cursor = 0;
  for (const part of chunks) { zip.set(part, cursor); cursor += part.length; }
  return zip;
}