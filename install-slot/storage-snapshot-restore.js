// install-slot/storage-snapshot-restore.js
// Sequential v2 snapshot restore: preflight, erase/write, independent readback,
// then a separate boot/reconnect check. No transaction journal or rollback.
import {
  assertSnapshotVersionCompatible,
  buildStorageSnapshotRestorePlan,
  readStorageSnapshotZip,
} from "./storage-snapshot.js";

const CHUNK_BYTES = 4096;

function restoreError(message, code, details = {}) {
  const error = new Error(message);
  error.code = code;
  Object.assign(error, details);
  return error;
}

function parseInventory(result) {
  if (!result || result.ok !== true) throw new Error("Unable to read the target device's live partition inventory");
  let inventory;
  try {
    inventory = typeof result.text === "string" ? JSON.parse(result.text) : result;
  } catch {
    throw new Error("Target device returned invalid live partition inventory JSON");
  }
  if (!Array.isArray(inventory.partitions) || !inventory.running_host) {
    throw new Error("Target device did not report its live partition table and running host identity");
  }
  return inventory;
}

function expectedChunk(partition, payloads, relativeOffset, length) {
  const expected = new Uint8Array(length).fill(0xff);
  for (const extent of partition.extents) {
    const bytes = payloads.get(extent.file);
    const left = Math.max(relativeOffset, extent.relativeOffset);
    const right = Math.min(relativeOffset + length, extent.relativeOffset + extent.bytes);
    if (left >= right) continue;
    expected.set(
      bytes.subarray(left - extent.relativeOffset, right - extent.relativeOffset),
      left - relativeOffset,
    );
  }
  return expected;
}

function firstMismatch(expected, actual) {
  if (!(actual instanceof Uint8Array) || actual.length !== expected.length) return 0;
  for (let i = 0; i < expected.length; i++) if (expected[i] !== actual[i]) return i;
  return -1;
}

async function writeChunk(device, partition, relativeOffset, bytes) {
  if (typeof device.storageWrite !== "function") {
    throw new Error("Device adapter does not provide partition-relative storageWrite");
  }
  const result = await device.storageWrite(partition, relativeOffset, bytes);
  if (!result || result.ok !== true) throw new Error(result?.text || "device rejected partition write");
}

async function readChunk(device, partition, relativeOffset, length) {
  if (typeof device.storageRead !== "function") {
    throw new Error("Device adapter does not provide partition-relative storageRead");
  }
  const result = await device.storageRead(partition, relativeOffset, length);
  if (!result || result.ok !== true || !(result.bytes instanceof Uint8Array) || result.bytes.length !== length) {
    throw new Error(result?.text || "partition read returned an invalid byte length");
  }
  return result.bytes;
}

export async function restoreStorageSnapshotArchive(device, input, {
  sha256Hex,
  onProgress = () => {},
} = {}) {
  if (typeof sha256Hex !== "function") throw new TypeError("SHA-256 implementation is required");
  if (!device || typeof device.storageInfo !== "function" || typeof device.storageErase !== "function" ||
      typeof device.storageWrite !== "function" || typeof device.storageRead !== "function") {
    throw new Error("Device does not provide live storage inventory");
  }

  // Archive structure and every payload hash are checked before device writes.
  const archive = await readStorageSnapshotZip(input, sha256Hex);
  const inventory = parseInventory(await device.storageInfo());
  const currentVersion = inventory.meta_pass_version;
  assertSnapshotVersionCompatible(archive.manifest, currentVersion);
  const restorePlan = buildStorageSnapshotRestorePlan(archive.manifest, {
    targetPartitions: inventory.partitions,
    runningHostPartition: inventory.running_host,
    protectedRanges: inventory.protected_ranges || [],
  });

  let completedBytes = 0;
  const totalBytes = restorePlan.totalEraseBytes * 2;
  const report = (phase, partition, relativeOffset, extra = {}) => onProgress({
    phase, partition: partition?.label, relativeOffset, completedBytes, totalBytes, ...extra,
  });

  for (const item of restorePlan.partitions) {
    const p = item.partition;
    report("erase", p, 0);
    try {
      const erased = await device.storageErase(p);
      if (!erased || erased.ok !== true) throw new Error(erased?.text || "device rejected partition erase");
    } catch (cause) {
      throw restoreError(
        `Restore incomplete while erasing partition ${p.label} at relative offset 0x0: ${cause.message}. Reflash MetaPass ${archive.manifest.meta_pass.system_version}, reconnect, and retry.`,
        "SNAPSHOT_RESTORE_WRITE_FAILED",
        { partition: p.label, relativeOffset: 0, phase: "erase", cause },
      );
    }
    completedBytes += p.size;
    report("erase", p, p.size);

    for (const extent of item.extents) {
      const bytes = archive.payloads.get(extent.file);
      for (let rel = 0; rel < bytes.length; rel += CHUNK_BYTES) {
        const chunk = bytes.subarray(rel, Math.min(bytes.length, rel + CHUNK_BYTES));
        const partitionRelativeOffset = extent.relativeOffset + rel;
        try {
          await writeChunk(device, p, partitionRelativeOffset, chunk);
        } catch (cause) {
          throw restoreError(
            `Restore incomplete while writing partition ${p.label} at relative offset 0x${partitionRelativeOffset.toString(16)} (${extent.file}): ${cause.message}. Reflash MetaPass ${archive.manifest.meta_pass.system_version}, reconnect, and retry.`,
            "SNAPSHOT_RESTORE_WRITE_FAILED",
            { partition: p.label, relativeOffset: partitionRelativeOffset, phase: "write", cause },
          );
        }
      }
    }

    // Read back the entire partition, including sparse gaps expected to be 0xFF.
    report("readback", p, 0);
    for (let rel = 0; rel < p.size; rel += CHUNK_BYTES) {
      const n = Math.min(CHUNK_BYTES, p.size - rel);
      let actual;
      try {
        actual = await readChunk(device, p, rel, n);
      } catch (cause) {
        throw restoreError(
          `Restore verification incomplete for partition ${p.label} at relative offset 0x${rel.toString(16)}: readback failed (${cause.message}). Reflash MetaPass ${archive.manifest.meta_pass.system_version}, reconnect, and retry.`,
          "SNAPSHOT_RESTORE_READBACK_FAILED",
          { partition: p.label, relativeOffset: rel, phase: "readback", cause },
        );
      }
      const expected = expectedChunk(item, archive.payloads, rel, n);
      const mismatch = firstMismatch(expected, actual);
      if (mismatch >= 0) {
        const offset = rel + mismatch;
        throw restoreError(
          `Restore verification mismatch in partition ${p.label} at relative offset 0x${offset.toString(16)} (expected 0x${expected[mismatch].toString(16).padStart(2, "0")}, read 0x${actual[mismatch].toString(16).padStart(2, "0")}). Restore is incomplete/inconsistent. Reflash MetaPass ${archive.manifest.meta_pass.system_version}, reconnect, and retry.`,
          "SNAPSHOT_RESTORE_READBACK_MISMATCH",
          { partition: p.label, relativeOffset: offset, phase: "readback" },
        );
      }
      completedBytes += n;
      report("readback", p, rel + n);
    }
  }

  report("data-verified", null, null);
  // Reboot is deliberately a separate validation step, not part of data integrity.
  if (typeof device.rebootAndReconnect !== "function") {
    return {
      dataVerified: true,
      bootValidated: false,
      restorePlan,
      warning: "Data restore and independent readback passed; reboot/reconnect validation remains pending.",
    };
  }
  try {
    const boot = await device.rebootAndReconnect();
    if (!boot || boot.ok !== true) {
      throw new Error(boot?.message || "device did not reconnect after reboot");
    }
  } catch (cause) {
    throw restoreError(
      `Data restore and readback verification passed, but reboot/reconnect validation failed (${cause.message}). Reflash MetaPass ${archive.manifest.meta_pass.system_version} and reconnect the device.`,
      "SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED",
      { phase: "boot-validation", dataVerified: true, cause },
    );
  }
  report("complete", null, null);
  return { dataVerified: true, bootValidated: true, restorePlan };
}
