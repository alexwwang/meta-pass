// install-slot/data-install-plan.js — pure USB APP-owned DATA allocation planning.
// This module performs no I/O. The page must write and verify payloads before
// committing the returned carve, so failed writes leave no live DATA records.
import { carveValid, DATA_STATE, placeNewData } from "./dynslot-record.js";
import { DATA_SIZE_GRANULE, dataPartitionMinimum } from "./dynslot-pool.js";

export function nextDataPlayId(carve) {
  if (!carve || !Array.isArray(carve.slots) || !Array.isArray(carve.data)) return null;
  const used = new Set([
    ...carve.slots.map((s) => Number(s.playId) >>> 0).filter((n) => n > 0),
    ...carve.data.map((d) => Number(d.playId) >>> 0).filter((n) => n > 0),
  ]);
  // Locally installed firmware uses the high-bit namespace to reduce the
  // chance of colliding with marketplace play IDs.
  for (let id = 0x80000000; id < 0xffffffff; id++) if (!used.has(id)) return id;
  return null;
}

export function planDataReservations(carve, dataImages, sizes, playId) {
  if (!carveValid(carve)) return { ok: false, reason: "invalid-carve" };
  if (!Array.isArray(dataImages) || !Array.isArray(sizes) ||
      dataImages.length === 0 || dataImages.length !== sizes.length) {
    return { ok: false, reason: "invalid-input" };
  }
  if (!Number.isInteger(playId) || playId <= 0 || playId >= 0xffffffff) {
    return { ok: false, reason: "invalid-play-id" };
  }

  let next = { slots: carve.slots.slice(), data: carve.data.slice() };
  const records = [];
  for (let i = 0; i < dataImages.length; i++) {
    const image = dataImages[i];
    const min = dataPartitionMinimum(image);
    const size = sizes[i];
    if (!image || typeof image.label !== "string" || !Number.isInteger(image.subtype) ||
        !Number.isInteger(min) || !Number.isInteger(size) || size < min ||
        size < DATA_SIZE_GRANULE || size % DATA_SIZE_GRANULE !== 0) {
      return { ok: false, reason: "invalid-data-size", index: i };
    }
    if (next.data.some((d) => d.label === image.label && d.subtype === image.subtype) ||
        records.some((d) => d.label === image.label && d.subtype === image.subtype)) {
      return { ok: false, reason: "duplicate-label-subtype", index: i };
    }
    const place = placeNewData(next, size);
    if (!place) return { ok: false, reason: "no-space", index: i, size };
    const record = {
      playId, offset: place.offset, size, state: DATA_STATE.PRISTINE,
      subtype: image.subtype, type: 1, label: image.label,
    };
    next = { slots: next.slots.slice(), data: [...next.data, record] };
    if (!carveValid(next)) return { ok: false, reason: "invalid-allocation", index: i };
    records.push({ ...image, size, offset: place.offset });
  }
  return { ok: true, carve: next, records };
}
