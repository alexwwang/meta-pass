#!/usr/bin/env node
import assert from "node:assert/strict";
import { restoreStorageSnapshotArchive } from "../install-slot/storage-snapshot-restore.js";

// A device exposing only the existing read-only snapshot APIs must fail closed
// before any write/erase is attempted. This is the current firmware contract.
{
  const calls = [];
  const device = {
    async storageInfo() {
      calls.push("storageInfo");
      return { ok: true, text: JSON.stringify({
        running_host: { label: "ota_0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 },
        partitions: [
          { label: "ota_0", type: 0, subtype: 16, offset: 0x10000, size: 0x10000 },
          { label: "cardid", type: 1, subtype: 64, offset: 0x20000, size: 0x1000 },
          { label: "store", type: 1, subtype: 65, offset: 0x21000, size: 0x1000 },
        ],
      }) };
    },
  };
  await assert.rejects(
    restoreStorageSnapshotArchive(device, new Uint8Array(), { sha256Hex: async () => "" }),
    error => error.code === "STORAGE_SNAPSHOT_RESTORE_UNAVAILABLE" &&
      /beginSnapshotRestore/.test(error.message),
  );
  assert.deepEqual(calls, [], "missing durable transaction methods fail before parsing/writing");
}
console.log("PASS snapshot restore refuses devices without the durable transaction contract");
