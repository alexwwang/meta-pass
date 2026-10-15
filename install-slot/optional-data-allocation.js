// Shared policy for optional per-APP DATA reservations in USB and phone installers.
// Blank allocations use the registered LittleFS partition subtype (0x83); they
// carry no payload and are formatted by the child firmware when it mounts them.
export const BLANK_DATA_SUBTYPE_LITTLEFS = 0x83;
export const BLANK_DATA_MIN_SIZE = 64 * 1024;

export function makeBlankDataImage(playId, existingPartitions = []) {
  if (!Number.isSafeInteger(playId) || playId <= 0 || playId >= 0xffffffff) {
    throw new RangeError("valid positive app ID is required for blank DATA");
  }
  const used = new Set((Array.isArray(existingPartitions) ? existingPartitions : [])
    .map((p) => String(p?.label ?? "").toLowerCase()).filter(Boolean));
  const base = `mpd${playId.toString(36)}`.slice(0, 16);
  let label = base;
  for (let suffix = 1; used.has(label.toLowerCase()); suffix++) {
    const tail = suffix.toString(36);
    label = `${base.slice(0, 16 - tail.length)}${tail}`;
    if (suffix > 0xffff) throw new Error("no unique DATA label remains");
  }
  return {
    label,
    subtype: BLANK_DATA_SUBTYPE_LITTLEFS,
    required_size: BLANK_DATA_MIN_SIZE,
    initial_image_size: 0,
    data: new Uint8Array(0),
  };
}

export function resolveDataAllocationChoice(dataImages, allocate, playId, existingPartitions = []) {
  if (allocate !== true) return [];
  const declared = Array.isArray(dataImages) ? dataImages : [];
  if (declared.length) return declared.slice();
  return [makeBlankDataImage(playId, existingPartitions)];
}
