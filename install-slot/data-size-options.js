// Shared DATA size dropdown options used by the mobile and USB installers.
// Geometry and feasibility remain owned by dynslot-pool.js; this module only
// guarantees identical option/default semantics for the same computed bounds.
export function buildDataSizeOptions(min, max, declaredSize, step) {
  if (!Number.isSafeInteger(min) || min <= 0 ||
      !Number.isSafeInteger(max) || max < min ||
      !Number.isSafeInteger(step) || step <= 0) {
    throw new RangeError("invalid DATA size option bounds");
  }
  const values = [min];
  let next = Math.ceil((min + 1) / step) * step;
  while (next <= max) {
    values.push(next);
    next += step;
  }
  if (Number.isSafeInteger(declaredSize) && declaredSize >= min && declaredSize <= max) {
    values.push(declaredSize);
  }
  if (max > min) values.push(max);
  const uniqueValues = [...new Set(values)].filter((value) => value >= min && value <= max)
    .sort((a, b) => a - b);
  const defaultSize = Number.isSafeInteger(declaredSize) && declaredSize >= min && declaredSize <= max
    ? declaredSize : uniqueValues[uniqueValues.length - 1];
  return Object.freeze({
    min,
    max,
    values: Object.freeze(uniqueValues),
    defaultSize,
  });
}
