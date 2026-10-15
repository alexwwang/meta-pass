// Shared optional DATA policy. Only explicit firmware declarations provide a safe access contract.
export function resolveAppDataChoice({ allocate, declaredImages }) {
  if (!Array.isArray(declaredImages)) throw new TypeError("declaredImages must be an array");
  if (!allocate) return [];
  if (declaredImages.length === 0) throw new Error("Cannot allocate DATA: firmware declares no DATA partition/access contract");
  return declaredImages.slice();
}
