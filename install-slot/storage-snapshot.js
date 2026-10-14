import { buildDataArchiveZip, readDataArchiveZip } from "./data-byte-archive.js";
// install-slot/storage-snapshot.js
// Pure shared policy/validation for the v2 device-storage snapshot format.
// Transport-specific Flash I/O belongs to USB and the guarded device bridge.
export const STORAGE_SNAPSHOT_VERSION = 2;
export const STORAGE_SNAPSHOT_KIND = "meta-pass-storage-snapshot";
const SHA256_RE = /^[0-9a-f]{64}$/;

function fail(message) {
  const error = new Error(message);
  error.code = "INVALID_STORAGE_SNAPSHOT";
  throw error;
}

function validInt(n) {
  return Number.isSafeInteger(n) && n >= 0;
}


function parseVersion(value) {
  if (typeof value !== "string" || !/^\d+\.\d+\.\d+$/.test(value)) return null;
  const parts = value.split(".").map(Number);
  return parts.every(Number.isSafeInteger) ? parts : null;
}

function compareVersions(a, b) {
  const left = parseVersion(a), right = parseVersion(b);
  if (!left || !right) return null;
  for (let i = 0; i < 3; i++) {
    if (left[i] !== right[i]) return left[i] < right[i] ? -1 : 1;
  }
  return 0;
}

function validateMetaPassMetadata(metaPass) {
  const source = parseVersion(metaPass?.system_version);
  const min = parseVersion(metaPass?.compatible_versions?.min_inclusive);
  const max = parseVersion(metaPass?.compatible_versions?.max_inclusive);
  if (!source || !min || !max ||
      compareVersions(metaPass.compatible_versions.min_inclusive, metaPass.compatible_versions.max_inclusive) > 0 ||
      compareVersions(metaPass.system_version, metaPass.compatible_versions.min_inclusive) < 0 ||
      compareVersions(metaPass.system_version, metaPass.compatible_versions.max_inclusive) > 0) {
    fail("snapshot must declare a source MetaPass version and an inclusive compatible-version range");
  }
  return metaPass;
}

export function assertSnapshotVersionCompatible(manifest, currentVersion) {
  validateMetaPassMetadata(manifest?.meta_pass);
  if (!parseVersion(currentVersion)) {
    throw new Error("Target device did not report a valid MetaPass version; restore refused before Flash writes");
  }
  const { min_inclusive: min, max_inclusive: max } = manifest.meta_pass.compatible_versions;
  if (compareVersions(currentVersion, min) < 0 || compareVersions(currentVersion, max) > 0) {
    const source = manifest.meta_pass.system_version;
    const error = new Error(
      `MetaPass ${currentVersion} is outside this snapshot's supported range ${min}–${max}. ` +
      `Reflash MetaPass ${source} (the version used for this snapshot), reconnect, and retry.`
    );
    error.code = "SNAPSHOT_VERSION_INCOMPATIBLE";
    throw error;
  }
  return true;
}

function rangeEnd(offset, size) {
  const end = offset + size;
  if (!validInt(offset) || !Number.isSafeInteger(size) || size <= 0 ||
      !Number.isSafeInteger(end) || end <= offset) fail("invalid or overflowing Flash range");
  return end;
}

export function overlaps(aOffset, aSize, bOffset, bSize) {
  return aOffset < rangeEnd(bOffset, bSize) && bOffset < rangeEnd(aOffset, aSize);
}

function validPartition(p) {
  return !!p && typeof p.label === "string" && p.label.length > 0 &&
    Number.isInteger(p.type) && p.type >= 0 && p.type <= 255 &&
    Number.isInteger(p.subtype) && p.subtype >= 0 && p.subtype <= 255 &&
    validInt(p.offset) && Number.isSafeInteger(p.size) && p.size > 0 &&
    Number.isSafeInteger(p.offset + p.size);
}

// The current running host app partition must be identified by the device,
// not inferred from a hard-coded address or from an archive supplied by the user.
export function isProtectedPartition(partition, runningHostPartition) {
  return partition.label === "cardid" ||
    (runningHostPartition && partition.label === runningHostPartition.label &&
      partition.type === runningHostPartition.type &&
      partition.subtype === runningHostPartition.subtype &&
      partition.offset === runningHostPartition.offset &&
      partition.size === runningHostPartition.size);
}

export function eligibleSnapshotPartitions(partitions, runningHostPartition, protectedRanges = []) {
  if (!Array.isArray(partitions) || !validPartition(runningHostPartition) || !Array.isArray(protectedRanges)) {
    fail("live partition table, running host partition, and protected ranges are required");
  }
  const labels = new Set();
  for (let i = 0; i < partitions.length; i++) {
    const p = partitions[i];
    if (!validPartition(p)) fail("invalid live partition table entry");
    if (labels.has(p.label)) fail("duplicate live partition label");
    labels.add(p.label);
    for (let j = 0; j < i; j++) {
      if (overlaps(p.offset, p.size, partitions[j].offset, partitions[j].size)) {
        fail("overlapping live partition table entries");
      }
    }
  }
  if (!partitions.some((p) => isProtectedPartition(p, runningHostPartition))) {
    fail("running host partition is absent from live partition table");
  }
  const protectedOverlap = (p) => protectedRanges.some((r) => {
    if (!r || !validInt(r.offset) || !Number.isSafeInteger(r.size) || r.size <= 0) {
      fail("invalid device-provided protected range");
    }
    return overlaps(p.offset, p.size, r.offset, r.size);
  });
  if (partitions.some((p) => !isProtectedPartition(p, runningHostPartition) &&
      p.label !== "cardid" && protectedOverlap(p))) {
    fail("eligible live partition overlaps a protected range");
  }
  return partitions.filter((p) => p.protected !== true && p.running_host !== true && !isProtectedPartition(p, runningHostPartition) &&
    p.label !== "cardid" && !protectedOverlap(p));
}

function validateExtent(extent, partitionSize, seenNames) {
  if (!extent || typeof extent.file !== "string" || !extent.file || extent.file === "manifest.json" ||
      extent.file.startsWith("/") || extent.file.includes("\\") ||
      extent.file.split("/").some((part) => !part || part === "." || part === "..") ||
      !Number.isSafeInteger(extent.offset) || extent.offset < 0 || extent.offset % 4 !== 0 ||
      !Number.isSafeInteger(extent.bytes) || extent.bytes <= 0 || extent.bytes % 4 !== 0 ||
      !Number.isSafeInteger(extent.offset + extent.bytes) ||
      extent.offset + extent.bytes > partitionSize ||
      typeof extent.sha256 !== "string" || !SHA256_RE.test(extent.sha256)) {
    fail("invalid snapshot extent");
  }
  if (seenNames.has(extent.file)) fail("duplicate snapshot payload path");
  seenNames.add(extent.file);
}

export function buildStorageSnapshotManifest({ createdUtc = new Date().toISOString(), partitions, metaPassVersion, compatibleMetaPassVersions }) {
  if (typeof createdUtc !== "string" || !Number.isFinite(Date.parse(createdUtc)) ||
      !Array.isArray(partitions) || partitions.length === 0) fail("invalid snapshot input");
  const metaPass = validateMetaPassMetadata({ system_version: metaPassVersion, compatible_versions: compatibleMetaPassVersions });
  const seen = new Set();
  const normalized = partitions.map((p) => {
    if (!validPartition(p) || !Array.isArray(p.extents)) {
      fail("invalid snapshot partition record");
    }
    if (seen.has(p.label)) fail("duplicate snapshot partition label");
    seen.add(p.label);
    return {
      label: p.label, type: p.type, subtype: p.subtype,
      offset: p.offset, size: p.size,
      extents: p.extents.map((e) => ({ file: e.file, offset: e.offset, bytes: e.bytes, sha256: e.sha256 })),
    };
  });
  const manifest = {
    manifest_version: STORAGE_SNAPSHOT_VERSION,
    kind: STORAGE_SNAPSHOT_KIND,
    created_utc: createdUtc,
    complete: true,
    meta_pass: {
      system_version: metaPass.system_version,
      compatible_versions: {
        min_inclusive: metaPass.compatible_versions.min_inclusive,
        max_inclusive: metaPass.compatible_versions.max_inclusive,
      },
    },
    partitions: normalized,
  };
  validateStorageSnapshotManifest(manifest);
  return manifest;
}

export function validateStorageSnapshotManifest(manifest, {
  targetPartitions,
  runningHostPartition,
  protectedRanges = [],
} = {}) {
  if (!Array.isArray(protectedRanges)) fail("device-provided protected ranges are required");
  if (!manifest || manifest.kind !== STORAGE_SNAPSHOT_KIND ||
      manifest.manifest_version !== STORAGE_SNAPSHOT_VERSION) {
    fail("unsupported archive: expected a v2 storage snapshot");
  }
  if (manifest.complete !== true || typeof manifest.created_utc !== "string" ||
      !Number.isFinite(Date.parse(manifest.created_utc)) ||
      !Array.isArray(manifest.partitions) || manifest.partitions.length === 0) {
    fail("snapshot is incomplete or malformed");
  }
  validateMetaPassMetadata(manifest.meta_pass);

  const seenLabels = new Set();
  const seenFiles = new Set();
  const allRanges = [];
  for (const p of manifest.partitions) {
    if (!validPartition(p) || !Array.isArray(p.extents)) fail("invalid partition record");
    if (seenLabels.has(p.label)) fail("duplicate partition label");
    seenLabels.add(p.label);
    const protectedOverlap = protectedRanges.some((r) => {
      if (!r || !validInt(r.offset) || !Number.isSafeInteger(r.size) || r.size <= 0) {
        fail("invalid device-provided protected range");
      }
      return overlaps(p.offset, p.size, r.offset, r.size);
    });
    if (p.protected === true || p.running_host === true || p.label === "cardid" || protectedOverlap ||
        (runningHostPartition && isProtectedPartition(p, runningHostPartition))) {
      fail("snapshot attempts to include a protected partition");
    }
    allRanges.push({ offset: p.offset, size: p.size, label: p.label });
    let previousEnd = -1;
    for (const e of p.extents) {
      validateExtent(e, p.size, seenFiles);
      if (e.offset < previousEnd) fail("snapshot extents overlap or are out of order");
      previousEnd = e.offset + e.bytes;
    }
  }
  for (let i = 0; i < allRanges.length; i++) {
    for (let j = i + 1; j < allRanges.length; j++) {
      if (overlaps(allRanges[i].offset, allRanges[i].size, allRanges[j].offset, allRanges[j].size)) {
        fail("snapshot partition ranges overlap");
      }
    }
  }

  if (targetPartitions !== undefined) {
    if (!Array.isArray(targetPartitions) || !validPartition(runningHostPartition)) {
      fail("target partition table and running host partition are required");
    }
    const eligible = eligibleSnapshotPartitions(targetPartitions, runningHostPartition, protectedRanges);
    if (manifest.partitions.length !== eligible.length) {
      fail("snapshot does not cover every eligible allocated partition");
    }
    for (const target of eligible) {
      const source = manifest.partitions.find((p) => p.label === target.label &&
        p.type === target.type && p.subtype === target.subtype);
      if (!source || target.offset !== source.offset || target.size !== source.size) {
        fail("target partition layout is incompatible or snapshot is incomplete: " + target.label);
      }
    }
  }
  return manifest;
}

// Build a deterministic, non-mutating restore plan. Erasing each eligible
// partition before writing its extents restores omitted sparse ranges to 0xFF;
// no operation addresses an unallocated Flash gap.
export function buildStorageSnapshotRestorePlan(manifest, {
  targetPartitions,
  runningHostPartition,
  protectedRanges = [],
} = {}) {
  validateStorageSnapshotManifest(manifest, {
    targetPartitions,
    runningHostPartition,
    protectedRanges,
  });
  const eligible = eligibleSnapshotPartitions(targetPartitions, runningHostPartition, protectedRanges);
  const partitions = eligible.map((target) => {
    const source = manifest.partitions.find((p) => p.label === target.label &&
      p.type === target.type && p.subtype === target.subtype);
    if (!source) fail("restore plan is missing partition: " + target.label);
    return {
      partition: {
        label: target.label, type: target.type, subtype: target.subtype,
        offset: target.offset, size: target.size,
      },
      erase: { relativeOffset: 0, bytes: target.size },
      extents: source.extents.map((e) => ({
        file: e.file, relativeOffset: e.offset, bytes: e.bytes, sha256: e.sha256,
      })),
    };
  });
  return {
    kind: STORAGE_SNAPSHOT_KIND,
    partitionCount: partitions.length,
    totalEraseBytes: partitions.reduce((sum, p) => sum + p.erase.bytes, 0),
    partitions,
  };
}

export async function buildStorageSnapshotZip({ manifest, payloads }, sha256Hex) {
  validateStorageSnapshotManifest(manifest);
  if (!(payloads instanceof Map)) fail("snapshot payloads must be a Map");
  await validateStorageSnapshotPayloads(manifest, payloads, sha256Hex);
  const expected = new Set(manifest.partitions.flatMap((p) => p.extents.map((e) => e.file)));
  if (payloads.size !== expected.size || [...payloads.keys()].some((name) => !expected.has(name))) {
    fail("snapshot payload set does not match manifest");
  }
  const entries = [...expected].map((name) => ({ name, data: payloads.get(name) }));
  entries.push({ name: "manifest.json", data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
  return buildDataArchiveZip(entries, { maxEntries: 4096 });
}

export async function readStorageSnapshotZip(input, sha256Hex) {
  const entries = readDataArchiveZip(input, {
    maxArchiveBytes: 16 * 1024 * 1024,
    maxEntries: 4096,
    maxEntryBytes: 8 * 1024 * 1024,
  });
  const manifestBytes = entries.get("manifest.json");
  if (!(manifestBytes instanceof Uint8Array)) fail("storage snapshot archive is missing manifest.json");
  let manifest;
  try { manifest = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(manifestBytes)); }
  catch { fail("storage snapshot manifest is not valid UTF-8 JSON"); }
  validateStorageSnapshotManifest(manifest);
  entries.delete("manifest.json");
  const expected = new Set(manifest.partitions.flatMap((p) => p.extents.map((e) => e.file)));
  if (entries.size !== expected.size || [...entries.keys()].some((name) => !expected.has(name))) {
    fail("storage snapshot archive contains missing or undeclared payload files");
  }
  await validateStorageSnapshotPayloads(manifest, entries, sha256Hex);
  return { manifest, payloads: entries };
}

export async function validateStorageSnapshotPayloads(manifest, payloads, sha256Hex) {
  validateStorageSnapshotManifest(manifest);
  if (typeof sha256Hex !== "function") fail("SHA-256 implementation is required");
  const get = (name) => payloads instanceof Map ? payloads.get(name) : payloads?.[name];
  let extentCount = 0;
  for (const p of manifest.partitions) {
    for (const e of p.extents) {
      const bytes = get(e.file);
      if (!(bytes instanceof Uint8Array) || bytes.byteLength !== e.bytes) {
        fail("missing snapshot payload or length mismatch: " + e.file);
      }
      const digest = await sha256Hex(bytes);
      if (digest !== e.sha256) fail("snapshot payload SHA-256 mismatch: " + e.file);
      extentCount++;
    }
  }
  return { partitionCount: manifest.partitions.length, extentCount };
}
