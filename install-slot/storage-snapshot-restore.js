// install-slot/storage-snapshot-restore.js
// Sequential v2 snapshot restore: preflight, erase/write, then boot/reconnect check.
// Archive hashes are verified before mutation; no full-flash readback or rollback.
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

async function writeChunk(device, partition, relativeOffset, bytes) {
  if (typeof device.storageWrite !== "function") {
    throw new Error("Device adapter does not provide partition-relative storageWrite");
  }
  const result = await device.storageWrite(partition, relativeOffset, bytes);
  if (!result || result.ok !== true) throw new Error(result?.text || "device rejected partition write");
}

export async function restoreStorageSnapshotArchive(device, input, {
  sha256Hex,
  onProgress = () => {},
} = {}) {
  if (typeof sha256Hex !== "function") throw new TypeError("SHA-256 implementation is required");
  if (!device || typeof device.storageInfo !== "function" || typeof device.storageErase !== "function" ||
      typeof device.storageWrite !== "function") {
    throw new Error("Device does not provide required storage inventory, erase, and write operations");
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
  const totalWriteBytes = restorePlan.partitions.reduce(
    (sum, item) => sum + item.extents.reduce((partSum, extent) => partSum + extent.bytes, 0), 0,
  );
  const totalBytes = restorePlan.totalEraseBytes + totalWriteBytes;
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
        report("write", p, partitionRelativeOffset);
        try {
          await writeChunk(device, p, partitionRelativeOffset, chunk);
        } catch (cause) {
          throw restoreError(
            `Restore incomplete while writing partition ${p.label} at relative offset 0x${partitionRelativeOffset.toString(16)} (${extent.file}): ${cause.message}. Reflash MetaPass ${archive.manifest.meta_pass.system_version}, reconnect, and retry.`,
            "SNAPSHOT_RESTORE_WRITE_FAILED",
            { partition: p.label, relativeOffset: partitionRelativeOffset, phase: "write", cause },
          );
        }
        completedBytes += chunk.length;
        report("write", p, partitionRelativeOffset + chunk.length);
      }
    }
  }

  report("data-written", null, null);
  // Reboot is deliberately a separate validation step, not part of data integrity.
  if (typeof device.rebootAndReconnect !== "function") {
    return {
      dataWritten: true,
      bootValidated: false,
      restorePlan,
      warning: "Snapshot bytes were written; reboot/reconnect validation remains pending.",
    };
  }
  try {
    const boot = await device.rebootAndReconnect();
    if (!boot || boot.ok !== true) {
      throw new Error(boot?.message || "device did not reconnect after reboot");
    }
  } catch (cause) {
    throw restoreError(
      `Snapshot bytes were written, but reboot/reconnect validation failed (${cause.message}). Reflash MetaPass ${archive.manifest.meta_pass.system_version} and reconnect the device.`,
      "SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED",
      { phase: "boot-validation", dataWritten: true, cause },
    );
  }
  report("complete", null, null);
  return { dataWritten: true, bootValidated: true, restorePlan };
}
