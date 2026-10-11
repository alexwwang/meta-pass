// install-slot/data-restore-plan.js
// Pure preflight for MPTB v2 restore. No Flash addresses or writes are accepted here.
import {
  validateDataArchiveManifest,
  validateDataArchivePayloads,
} from "./data-byte-archive.js";

function reject(message) {
  const error = new Error(message);
  error.code = "INVALID_RESTORE_PLAN";
  throw error;
}

/**
 * Validate every archive byte and build a deterministic, address-free restore plan.
 *
 * target is a snapshot of device state:
 *   { freeBytes, largestGap, dataRecordSlots, existingPlayIds, appSlotCount }
 * This function never mutates target and never performs device I/O.
 */
export async function buildDataRestorePlan(manifest, payloads, sha256Hex, target) {
  validateDataArchiveManifest(manifest);
  if (manifest.complete !== true || (manifest.missing_slots?.length ?? 0) !== 0) {
    reject("incomplete archive cannot be restored");
  }
  if (!target || !Number.isSafeInteger(target.freeBytes) || target.freeBytes < 0 ||
      !Number.isSafeInteger(target.largestGap) || target.largestGap < 0 ||
      !Number.isInteger(target.dataRecordSlots) || target.dataRecordSlots < 0 ||
      !Number.isInteger(target.appSlotCount) || target.appSlotCount < 0 ||
      !Array.isArray(target.existingPlayIds)) {
    reject("invalid target device snapshot");
  }

  await validateDataArchivePayloads(manifest, payloads, sha256Hex);

  const incomingPlayIds = manifest.slots.map((slot) => slot.play_id);
  const incoming = new Set(incomingPlayIds);
  if (incoming.size !== incomingPlayIds.length) reject("duplicate incoming APP identity");
  const existing = new Set(target.existingPlayIds);
  for (const playId of incoming) {
    if (existing.has(playId)) reject("target already contains play_id " + playId);
  }

  if (incomingPlayIds.length > target.appSlotCount) {
    reject("insufficient APP slot capacity");
  }
  if (manifest.data.length > target.dataRecordSlots) {
    reject("insufficient DATA record slots");
  }

  // DATA sizes are already validated as positive safe integers by the manifest
  // validator. Round to the allocator's 4 KiB carve granule; never trust source offsets.
  const granule = 4096;
  const dataBytes = manifest.data.reduce((sum, record) => {
    const aligned = Math.ceil(record.bytes / granule) * granule;
    if (!Number.isSafeInteger(aligned) || !Number.isSafeInteger(sum + aligned)) {
      reject("DATA capacity calculation overflow");
    }
    return sum + aligned;
  }, 0);

  if (dataBytes > target.freeBytes) reject("insufficient total DATA pool capacity");
  if (dataBytes > target.largestGap) reject("insufficient contiguous DATA pool capacity");

  return Object.freeze({
    manifestVersion: manifest.manifest_version,
    playIds: Object.freeze([...incomingPlayIds]),
    data: Object.freeze(manifest.data.map((record) => Object.freeze({
      play_id: record.play_id,
      label: record.label,
      type: record.type,
      subtype: record.subtype,
      state: record.state,
      bytes: record.bytes,
      sha256: record.sha256,
      file: record.file,
      allocatedBytes: Math.ceil(record.bytes / granule) * granule,
    }))),
    dataBytes,
    appCount: incomingPlayIds.length,
    payloadFileCount: [
      ...manifest.slots.flatMap((slot) => slot.files),
      ...manifest.data,
      ...(manifest.nvs ? [manifest.nvs] : []),
    ].length,
    // Deliberately contains no offset/address: placement must be re-run by the device allocator.
  });
}
