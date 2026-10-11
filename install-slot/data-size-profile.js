// Explicit, opt-in compatibility profiles for controlled storage E2E only.
// Never enable a profile implicitly in production: the default profile is null.
//
// 2026-10-11 真机实测:动态池被 cardid(0x356000)/store(0x35A000) 固定条目
// 分为 1.88MB + 4.61MB 两段,单段连续 carve 上限 ~4.6MB。Play 28 声明的
// 6.4MB recordings 永远装不进单段;4MB 变体仅当 APP 落在前段时成立,而与
// 第二个 APP 共存时 APP 会被挤到后段,只有 3MB 变体同时满足共存。
export const PLAY28_RECORDINGS_4M_PROFILE = "play28-recordings-4m";
export const PLAY28_RECORDINGS_4M_SIZE = 4 * 1024 * 1024;
export const PLAY28_RECORDINGS_3M_PROFILE = "play28-recordings-3m";
export const PLAY28_RECORDINGS_3M_SIZE = 3 * 1024 * 1024;
const PLAY28_RECORDINGS_DECLARED_MIN = 6 * 1024 * 1024;

const PROFILES = {
  [PLAY28_RECORDINGS_4M_PROFILE]: PLAY28_RECORDINGS_4M_SIZE,
  [PLAY28_RECORDINGS_3M_PROFILE]: PLAY28_RECORDINGS_3M_SIZE,
};

/**
 * Apply a narrowly-scoped test-only DATA carve override to parsed firmware DATA.
 * Input objects are copied. Throws instead of silently falling back when the
 * requested profile does not match the expected blank FAT recordings partition.
 */
export function applyDataSizeProfile(dataPartitions, playId, profile) {
  const input = Array.isArray(dataPartitions) ? dataPartitions : [];
  if (profile == null || profile === "") return input.map((p) => ({ ...p }));
  const overrideSize = PROFILES[profile];
  if (overrideSize == null || Number(playId) !== 28) {
    throw new Error("unsupported-data-size-profile");
  }
  const matches = input.filter((p) => p?.label === "recordings");
  if (matches.length !== 1) throw new Error("profile-recordings-partition-mismatch");
  const part = matches[0];
  const size = Number(part.required_size ?? part.requiredSize ?? part.size);
  const subtype = Number(part.subtype);
  const initialSizeRaw = part.initial_image_size ?? part.initialImageSize;
  const initialSize = initialSizeRaw == null ? Number.NaN : Number(initialSizeRaw);
  if (subtype !== 0x81 || !Number.isInteger(size) || size < PLAY28_RECORDINGS_DECLARED_MIN ||
      initialSize !== 0) {
    throw new Error("profile-requires-blank-fat-recordings-partition");
  }
  return input.map((p) => p === part ? {
    ...p,
    size: overrideSize,
    required_size: overrideSize,
    requiredSize: overrideSize,
    testSizeProfile: profile,
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
