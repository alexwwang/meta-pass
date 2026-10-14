// install-slot/storage-snapshot-restore.js
// Host-side coordinator for the v2 whole-device snapshot restore protocol.
// This module deliberately refuses the legacy per-slot write APIs: a snapshot
// restore must be backed by a device-side durable transaction implementation.
import {
  buildStorageSnapshotRestorePlan,
  readStorageSnapshotZip,
  validateStorageSnapshotPayloads,
} from "./storage-snapshot.js";

function unsupported(message) {
  const error = new Error(message);
  error.code = "STORAGE_SNAPSHOT_RESTORE_UNAVAILABLE";
  return error;
}

function requireMethod(device, name) {
  if (!device || typeof device[name] !== "function") {
    throw unsupported("Device does not implement durable snapshot restore protocol: " + name);
  }
}

// Do not substitute ordinary partition write/erase calls here. beginSnapshotRestore
// must durably journal the transaction and reserve rollback/recovery capacity on
// the device before the first mutation; the device enforces protected ranges itself.
export async function restoreStorageSnapshotArchive(device, input, {
  sha256Hex,
  onProgress = () => {},
} = {}) {
  if (typeof sha256Hex !== "function") {
    throw new TypeError("SHA-256 implementation is required");
  }
  for (const name of [
    "storageInfo",
    "beginSnapshotRestore",
    "stageSnapshotRestoreChunk",
    "verifyStagedSnapshotRestore",
    "commitSnapshotRestore",
    "getSnapshotRestoreStatus",
  ]) requireMethod(device, name);

  // Fully validate archive bytes and every payload before device mutation.
  const archive = await readStorageSnapshotZip(input, sha256Hex);
  const inventoryResult = await device.storageInfo();
  if (!inventoryResult || inventoryResult.ok !== true) {
    throw new Error("Unable to read the target device's live partition inventory");
  }
  const inventory = typeof inventoryResult.text === "string"
    ? JSON.parse(inventoryResult.text)
    : inventoryResult;
  if (!Array.isArray(inventory.partitions) || !inventory.running_host) {
    throw new Error("Target device did not report a live partition table and running host identity");
  }
  const restorePlan = buildStorageSnapshotRestorePlan(archive.manifest, {
    targetPartitions: inventory.partitions,
    runningHostPartition: inventory.running_host,
    protectedRanges: inventory.protected_ranges || [],
  });
  await validateStorageSnapshotPayloads(archive.manifest, archive.payloads, sha256Hex);

  // The device must reject if it cannot durably stage rollback state. A returned
  // transaction ID is opaque; the host never supplies absolute Flash addresses.
  const begun = await device.beginSnapshotRestore({
    kind: archive.manifest.kind,
    manifest_version: archive.manifest.manifest_version,
    manifest: archive.manifest,
    plan: restorePlan,
  });
  if (!begun || typeof begun.transaction_id !== "string" || !begun.transaction_id) {
    throw new Error("Device did not create a durable snapshot restore transaction");
  }
  const transactionId = begun.transaction_id;
  try {
    let sent = 0;
    const total = [...archive.payloads.values()].reduce((sum, bytes) => sum + bytes.byteLength, 0);
    for (const partition of restorePlan.partitions) {
      for (const extent of partition.extents) {
        const bytes = archive.payloads.get(extent.file);
        if (!(bytes instanceof Uint8Array) || bytes.byteLength !== extent.bytes) {
          throw new Error("Validated payload disappeared before staging: " + extent.file);
        }
        const staged = await device.stageSnapshotRestoreChunk({
          transaction_id: transactionId,
          partition: partition.partition,
          relative_offset: extent.relativeOffset,
          expected_sha256: extent.sha256,
          bytes,
        });
        if (!staged || staged.ok !== true) {
          throw new Error("Device rejected snapshot extent staging: " + extent.file);
        }
        sent += bytes.byteLength;
        onProgress({ phase: "staging", sent, total, file: extent.file });
      }
    }
    const verified = await device.verifyStagedSnapshotRestore({ transaction_id: transactionId });
    if (!verified || verified.ok !== true || verified.verified !== true) {
      throw new Error("Device did not verify the complete staged snapshot and rollback journal");
    }
    onProgress({ phase: "committing", sent, total });
    // A timeout here is ambiguous. Never abort/clean a transaction after commit
    // may have started; inspect durable device status on reconnect instead.
    const committed = await device.commitSnapshotRestore({ transaction_id: transactionId });
    if (!committed || committed.committed !== true) {
      const error = new Error("Snapshot restore commit status is ambiguous; inspect durable device transaction status");
      error.code = "SNAPSHOT_RESTORE_COMMIT_AMBIGUOUS";
      error.transactionId = transactionId;
      throw error;
    }
    const status = await device.getSnapshotRestoreStatus({ transaction_id: transactionId });
    if (!status || status.state !== "committed" || status.verified !== true) {
      const error = new Error("Device did not confirm committed state and post-write readback verification");
      error.code = "SNAPSHOT_RESTORE_COMMIT_AMBIGUOUS";
      error.transactionId = transactionId;
      throw error;
    }
    onProgress({ phase: "complete", sent, total });
    return { committed: true, transactionId, restorePlan };
  } catch (error) {
    // No best-effort abort: after a transport error, the commit boundary may have
    // been crossed. The durable journal must decide resume vs rollback on device.
    if (!error.transactionId) error.transactionId = transactionId;
    throw error;
  }
}
