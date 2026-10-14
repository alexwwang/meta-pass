// install-slot/storage-snapshot.js
// Pure shared policy/validation for the v2 device-storage snapshot format.
// Transport-specific Flash I/O belongs to USB and the guarded device bridge.
export const STORAGE_SNAPSHOT_VERSION = 2;
export const STORAGE_SNAPSHOT_KIND = "meta-pass-storage-snapshot";
export const PARTITION_TABLE_RANGE = Object.freeze({ offset: 0x8000, size: 0x1000 });
const SHA256_RE = /^[0-9a-f]{64}$/;

function fail(message) {
  const error = new Error(message);
  error.code = "INVALID_STORAGE_SNAPSHOT";
  throw error;
}

function validInt(n) {
  return Number.isSafeInteger(n) && n >= 0;
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

export function eligibleSnapshotPartitions(partitions, runningHostPartition) {
  if (!Array.isArray(partitions) || !validPartition(runningHostPartition)) {
    fail("live partition table and running host partition are required");
  }
  return partitions.filter((p) => validPartition(p) &&
    !isProtectedPartition(p, runningHostPartition) &&
    !overlaps(p.offset, p.size, PARTITION_TABLE_RANGE.offset, PARTITION_TABLE_RANGE.size));
}

function validateExtent(extent, partitionSize, seenNames) {
  if (!extent || typeof extent.file !== "string" || !extent.file ||
      extent.file.startsWith("/") || extent.file.includes("\\") ||
      extent.file.split("/").some((part) => !part || part === "." || part === "..") ||
      !Number.isSafeInteger(extent.offset) || extent.offset < 0 ||
      !Number.isSafeInteger(extent.bytes) || extent.bytes <= 0 ||
      !Number.isSafeInteger(extent.offset + extent.bytes) ||
      extent.offset + extent.bytes > partitionSize ||
      typeof extent.sha256 !== "string" || !SHA256_RE.test(extent.sha256)) {
    fail("invalid snapshot extent");
  }
  if (seenNames.has(extent.file)) fail("duplicate snapshot payload path");
  seenNames.add(extent.file);
}

export function buildStorageSnapshotManifest({ createdUtc = new Date().toISOString(), partitions }) {
  if (typeof createdUtc !== "string" || !Number.isFinite(Date.parse(createdUtc)) ||
      !Array.isArray(partitions) || partitions.length === 0) fail("invalid snapshot input");
  const seen = new Set();
  const normalized = partitions.map((p) => {
    if (!validPartition(p) || typeof p.name !== "string" || !Array.isArray(p.extents)) {
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
    partitions: normalized,
  };
  validateStorageSnapshotManifest(manifest);
  return manifest;
}

export function validateStorageSnapshotManifest(manifest, {
  targetPartitions,
  runningHostPartition,
} = {}) {
  if (!manifest || manifest.kind !== STORAGE_SNAPSHOT_KIND ||
      manifest.manifest_version !== STORAGE_SNAPSHOT_VERSION) {
    fail("unsupported archive: expected a v2 storage snapshot");
  }
  if (manifest.complete !== true || typeof manifest.created_utc !== "string" ||
      !Number.isFinite(Date.parse(manifest.created_utc)) ||
      !Array.isArray(manifest.partitions) || manifest.partitions.length === 0) {
    fail("snapshot is incomplete or malformed");
  }

  const seenLabels = new Set();
  const seenFiles = new Set();
  const allRanges = [];
  for (const p of manifest.partitions) {
    if (!validPartition(p) || !Array.isArray(p.extents)) fail("invalid partition record");
    if (seenLabels.has(p.label)) fail("duplicate partition label");
    seenLabels.add(p.label);
    if (p.label === "cardid" ||
        overlaps(p.offset, p.size, PARTITION_TABLE_RANGE.offset, PARTITION_TABLE_RANGE.size) ||
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
    const eligible = eligibleSnapshotPartitions(targetPartitions, runningHostPartition);
    for (const source of manifest.partitions) {
      const target = eligible.find((p) => p.label === source.label &&
        p.type === source.type && p.subtype === source.subtype);
      if (!target || target.offset !== source.offset || target.size !== source.size) {
        fail("target partition layout is incompatible: " + source.label);
      }
    }
  }
  return manifest;
}

export function validateStorageSnapshotPayloads(manifest, payloads) {
  validateStorageSnapshotManifest(manifest);
  const get = (name) => payloads instanceof Map ? payloads.get(name) : payloads?.[name];
  for (const p of manifest.partitions) {
    for (const e of p.extents) {
      const bytes = get(e.file);
      if (!(bytes instanceof Uint8Array) || bytes.byteLength !== e.bytes) {
        fail("missing snapshot payload or length mismatch: " + e.file);
      }
    }
  }
  return { partitionCount: manifest.partitions.length,
    extentCount: manifest.partitions.reduce((n, p) => n + p.extents.length, 0) };
}
