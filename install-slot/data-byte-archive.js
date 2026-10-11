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

export function buildDataArchiveManifest({ createdUtc = new Date().toISOString(), slots, data }) {
  if (!Array.isArray(slots) || !Array.isArray(data)) fail("slots and data must be arrays");
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
      files: slot.files.map((f) => ({ name: f.name, bytes: f.bytes, sha256: f.sha256, type: f.type || "app" })),
    };
  });
  const seenOwnership = new Set();
  const normalizedData = data.map((record, index) => {
    if (!record || !Number.isSafeInteger(record.play_id) || record.play_id <= 0 ||
        !slotIds.has(record.play_id)) fail("DATA record has no matching APP play_id");
    if (!safeLabel(record.label)) fail("invalid DATA label");
    const ownership = record.play_id + ":" + record.label;
    if (seenOwnership.has(ownership)) fail("duplicate DATA ownership key");
    seenOwnership.add(ownership);
    if (!Number.isInteger(record.type) || record.type < 0 || record.type > 255 ||
        !Number.isInteger(record.subtype) || record.subtype < 0 || record.subtype > 255 ||
        !Number.isInteger(record.state) || record.state < 1 || record.state > 2) {
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
        !Number.isInteger(record.state) || record.state < 1 || record.state > 2 ||
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
