// Explicit, opt-in compatibility profiles for controlled storage E2E only.
// Never enable a profile implicitly in production: the default profile is null.
export const PLAY28_RECORDINGS_4M_PROFILE = "play28-recordings-4m";
export const PLAY28_RECORDINGS_4M_SIZE = 4 * 1024 * 1024;
const PLAY28_RECORDINGS_DECLARED_MIN = 6 * 1024 * 1024;

/**
 * Apply a narrowly-scoped test-only DATA carve override to parsed firmware DATA.
 * Input objects are copied. Throws instead of silently falling back when the
 * requested profile does not match the expected blank FAT recordings partition.
 */
export function applyDataSizeProfile(dataPartitions, playId, profile) {
  const input = Array.isArray(dataPartitions) ? dataPartitions : [];
  if (profile == null || profile === "") return input.map((p) => ({ ...p }));
  if (profile !== PLAY28_RECORDINGS_4M_PROFILE || Number(playId) !== 28) {
    throw new Error("unsupported-data-size-profile");
  }
  const matches = input.filter((p) => p?.label === "recordings");
  if (matches.length !== 1) throw new Error("profile-recordings-partition-mismatch");
  const part = matches[0];
  const size = Number(part.required_size ?? part.requiredSize ?? part.size);
  const subtype = Number(part.subtype);
  const initialSize = Number(part.initial_image_size ?? part.initialImageSize ?? 0);
  if (subtype !== 0x81 || !Number.isInteger(size) || size < PLAY28_RECORDINGS_DECLARED_MIN ||
      initialSize !== 0) {
    throw new Error("profile-requires-blank-fat-recordings-partition");
  }
  return input.map((p) => p === part ? {
    ...p,
    size: PLAY28_RECORDINGS_4M_SIZE,
    required_size: PLAY28_RECORDINGS_4M_SIZE,
    requiredSize: PLAY28_RECORDINGS_4M_SIZE,
    testSizeProfile: PLAY28_RECORDINGS_4M_PROFILE,
  } : { ...p });
}

export function firmwareDataRequiredSize(manifest) {
  const align4k = (n) => Math.ceil(n / 0x1000) * 0x1000;
  if (!manifest || !manifest.app || !Number.isInteger(manifest.app.image_size)) return null;
  const data = Array.isArray(manifest.data) ? manifest.data : [];
  return align4k(manifest.app.image_size) + data.reduce((sum, p) => {
    const size = Number(p.required_size ?? p.requiredSize ?? p.size);
    return sum + (Number.isInteger(size) && size > 0 ? align4k(size) : Number.NaN);
  }, 0);
}
