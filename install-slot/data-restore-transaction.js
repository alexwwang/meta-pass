// install-slot/data-restore-transaction.js
// Device-adapter orchestration for MPTB v2 restore. The adapter must resolve all
// storage locations from device-side ownership/allocation records; callers never
// supply Flash addresses. No production bridge is wired until the device transaction
// endpoints provide these guarantees.
function fail(message, code = "RESTORE_TRANSACTION_FAILED") {
  const error = new Error(message);
  error.code = code;
  throw error;
}

function bytesFor(payloads, file) {
  const bytes = payloads instanceof Map ? payloads.get(file) : payloads?.[file];
  if (!(bytes instanceof Uint8Array)) fail("missing validated payload: " + file, "INVALID_RESTORE_PAYLOAD");
  return bytes;
}

/**
 * Execute restore using a trusted device adapter.
 *
 * Required adapter contract:
 *   beginRestore(plan) -> opaque transaction token
 *   allocateData(token, record) -> opaque device allocation handle
 *   writeData(token, handle, offset, bytes)
 *   readData(token, handle, offset, length) -> Uint8Array
 *   writeApp(token, slot, fileRecord, bytes)
 *   verifyApp(token, slot, fileRecord)
 *   commitRestore(token, {playIds, dataHandles}) -> {committed:true}
 *   readCommittedState(playIds) -> ownership/geometry snapshot
 *   cleanupStaging(token) -> best-effort cleanup
 * No method accepts or returns client-selected absolute Flash addresses.
 */
export async function executeDataRestoreTransaction({
  manifest, payloads, plan, device, sha256Hex, chunkSize = 4096,
}) {
  if (!manifest || manifest.manifest_version !== 2 || manifest.complete !== true ||
      !plan || plan.manifestVersion !== 2 || !Array.isArray(plan.playIds) ||
      !Array.isArray(plan.data) || !device || typeof sha256Hex !== "function") {
    fail("invalid restore transaction inputs", "INVALID_RESTORE_TRANSACTION");
  }
  if (!Number.isInteger(chunkSize) || chunkSize < 1 || chunkSize > 4096) {
    fail("restore chunk size must be between 1 and 4096 bytes", "INVALID_RESTORE_TRANSACTION");
  }
  const manifestPlayIds = manifest.slots.map((slot) => slot.play_id);
  if (new Set(manifestPlayIds).size !== manifestPlayIds.length ||
      manifestPlayIds.length !== plan.playIds.length ||
      manifestPlayIds.some((id) => !plan.playIds.includes(id))) {
    fail("restore plan APP identities do not match the manifest", "INVALID_RESTORE_PLAN");
  }
  if (manifest.data.length !== plan.data.length ||
      manifest.data.some((record) => !plan.data.some((planned) =>
        planned.play_id === record.play_id && planned.label === record.label &&
        planned.bytes === record.bytes && planned.sha256 === record.sha256 &&
        planned.file === record.file &&
        planned.allocatedBytes === Math.ceil(record.bytes / 4096) * 4096))) {
    fail("restore plan DATA identities do not match the manifest (including sizes)", "INVALID_RESTORE_PLAN");
  }
  const expectedDataBytes = manifest.data.reduce((sum, record) =>
    sum + Math.ceil(record.bytes / 4096) * 4096, 0);
  if (!Number.isSafeInteger(expectedDataBytes) || plan.dataBytes !== expectedDataBytes ||
      plan.appCount !== undefined && plan.appCount !== manifest.slots.length) {
    fail("restore plan capacity totals do not match the manifest", "INVALID_RESTORE_PLAN");
  }
  const requiredMethods = [
    "beginRestore", "allocateData", "writeData", "readData", "writeApp",
    "verifyApp", "commitRestore", "readCommittedState", "cleanupStaging",
  ];
  if (manifest.nvs) requiredMethods.push("writeNvs", "verifyNvs");
  for (const name of requiredMethods) {
    if (typeof device[name] !== "function") fail("device adapter missing " + name, "INVALID_RESTORE_ADAPTER");
  }

  // Recheck archive bytes at the transaction boundary, even if a UI already
  // performed preflight. This prevents mutated payload maps between preflight/write.
  const appRecords = manifest.slots.flatMap((slot) =>
    slot.files.map((file) => ({ slot, file, bytes: bytesFor(payloads, file.name) })));
  const dataRecords = manifest.data.map((record) => ({
    record, bytes: bytesFor(payloads, record.file),
  }));
  const nvsItem = manifest.nvs ? { file: manifest.nvs, bytes: bytesFor(payloads, manifest.nvs.file) } : null;
  for (const item of [...appRecords, ...dataRecords, ...(nvsItem ? [nvsItem] : [])]) {
    if (item.bytes.byteLength !== item.file?.bytes && item.file) {
      fail("APP payload length changed after preflight", "INVALID_RESTORE_PAYLOAD");
    }
    if (item.record && item.bytes.byteLength !== item.record.bytes) {
      fail("DATA payload length changed after preflight", "INVALID_RESTORE_PAYLOAD");
    }
    const expected = item.file?.sha256 ?? item.record?.sha256;
    const actual = await sha256Hex(item.bytes);
    if (actual !== expected) fail("payload digest changed after preflight", "INVALID_RESTORE_PAYLOAD");
  }

  let token;
  let commitStarted = false;
  const dataHandles = [];
  try {
    token = await device.beginRestore({
      playIds: [...plan.playIds],
      dataCount: plan.data.length,
      dataBytes: plan.dataBytes,
    });
    if (token == null) fail("device did not create a restore transaction");

    for (const { record, bytes } of dataRecords) {
      const handle = await device.allocateData(token, {
        play_id: record.play_id, label: record.label, type: record.type,
        subtype: record.subtype, state: record.state, bytes: record.bytes,
      });
      if (handle == null) fail("device DATA allocation failed");
      dataHandles.push({ play_id: record.play_id, label: record.label, handle });
      for (let offset = 0; offset < bytes.length; offset += chunkSize) {
        const part = bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length));
        await device.writeData(token, handle, offset, part);
        const readback = await device.readData(token, handle, offset, part.length);
        if (!(readback instanceof Uint8Array) || readback.length !== part.length) {
          fail("DATA readback length mismatch");
        }
        const expected = await sha256Hex(part);
        const actual = await sha256Hex(readback);
        if (actual !== expected) fail("DATA readback digest mismatch");
      }
    }

    for (const { slot, file, bytes } of appRecords) {
      await device.writeApp(token, {
        play_id: slot.play_id, source_slot: slot.source_slot, name: slot.name,
      }, file, bytes);
      await device.verifyApp(token, {
        play_id: slot.play_id, source_slot: slot.source_slot, name: slot.name,
      }, file);
    }
    if (nvsItem) {
      await device.writeNvs(token, nvsItem.file, nvsItem.bytes);
      await device.verifyNvs(token, nvsItem.file);
    }

    // From this point onward a timeout/error may mean the durable commit happened.
    // Never erase staging on an ambiguous commit result.
    commitStarted = true;
    const commit = await device.commitRestore(token, {
      playIds: [...plan.playIds],
      dataHandles: dataHandles.map(({ play_id, label, handle }) => ({ play_id, label, handle })),
    });
    if (!commit || commit.committed !== true) fail("device did not confirm restore commit", "RESTORE_COMMIT_UNCERTAIN");

    const state = await device.readCommittedState([...plan.playIds]);
    if (!state || state.committed !== true ||
        !Array.isArray(state.playIds) ||
        plan.playIds.some((id) => !state.playIds.includes(id)) ||
        !Array.isArray(state.data) ||
        dataRecords.some(({ record }) => !state.data.some((d) =>
          d.play_id === record.play_id && d.label === record.label &&
          d.bytes === record.bytes && d.sha256 === record.sha256))) {
      fail("post-commit ownership/geometry verification failed", "RESTORE_POST_COMMIT_VERIFY_FAILED");
    }
    return Object.freeze({ committed: true, playIds: Object.freeze([...plan.playIds]) });
  } catch (error) {
    if (token != null && !commitStarted) {
      try { await device.cleanupStaging(token); } catch { /* retain original failure */ }
    }
    if (commitStarted) {
      error.restoreStateUncertain = true;
      error.message = "restore commit may have started; preserve staging and run recovery inspection: " + error.message;
    }
    throw error;
  }
}
