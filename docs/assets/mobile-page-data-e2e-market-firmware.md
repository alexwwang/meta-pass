<p align="right">
  <a href="mobile-page-data-e2e-market-firmware.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Mobile-page DATA E2E: selecting a real marketplace firmware

Status: the target is selected and an explicit opt-in size profile is wired into analysis and the install offer; real marketplace-binary and hardware acceptance are still pending.

## Decision

**Use Recorder (Play 28) as the real business-DATA acceptance target. Its DATA partition is `recordings`, FAT subtype `0x81`.** Keep the custom `e2edata` child as a protocol, launcher-control, and allocator-regression fixture only. A passing fixture run must not be reported as a passing marketplace-play DATA lifecycle test.

## Why Play 28
Cross-check: see the [voice and e-book DATA storage comparison](mobile-page-data-e2e-market-comparison.md), including public FoloToy AI Passport / DeepSeek voice source and an open-source e-reader reference, with the binary-download verification gap explicitly stated.


Based on `docs/assets/dynslot-data-unification-research.md` (2026-10-02 byte-level analysis of marketplace binaries):

- The app image is about 1,536 KiB; `recordings` is about 6,592 KiB and is erased (`0xFF`) in the factory image.
- The firmware mounts FAT using `esp_vfs_fat_spiflash_mount_rw_wl` and the `recordings` label. This is the production T1 label-addressed access pattern that dynamic carve must support.
- Recordings are runtime-generated user data; the play UI can export WAV files and delete recordings. It therefore exercises a real write/export/read-after-reboot/delete lifecycle rather than a synthetic record structure.
- Most sampled marketplace plays are approximately 8 MiB full-flash images and are not directly usable as dynamic child apps. Play 563 is installable, but its `store` partition is NVS (a globally shared MVP resource), so it is not the preferred first target for per-play isolated user-data acceptance.

## Capacity gate — mandatory

Play 28 declares an approximately 1.5 MiB app plus a 6.4 MiB `recordings` partition, which exceeds the capacity available to one play in the current dynamic pool. The research proposed shrinking the erased `recordings` carve to about 4 MiB. Standard IDF FAT mounting derives the size from the actual partition entry, making this a plausible approach, The default installer still uses the declared DATA size. This branch now has an explicit opt-in test profile, but that is not yet evidence of real-device compatibility.

Implementation status: `?mp_test_data_profile=play28-recordings-4m` is wired for Play ID 28 only; the server analysis and phone install offer use the same profile. The default URL does not enable it. The actual marketplace image must still prove the partition is blank and the running firmware sees a 4 MiB partition.\n\nSafety boundaries:

1. Allow the override from the declared value to 4 MiB only for Play ID 28 plus the explicit test query parameter; leave production defaults unchanged.
2. Do not rewrite the original firmware binary or copy out-of-range DATA bytes from the source image into the smaller carve.
3. Analyze/prepare/finalize and the device store record must agree on the final size; after boot, the real firmware must resolve `recordings` by label and see a 4 MiB partition.
4. The profile accepts only one `recordings` FAT (`0x81`) partition, a declared size of at least 6 MiB, and `initial_image_size=0`; all mismatches fail closed.\n5. If consistency cannot be achieved with the controlled configuration, fail the capacity gate. Do not bypass it by dropping checks or fabricating DATA reservations.

## Acceptance scenarios

1. Install Play 28 from the marketplace. Confirm analyze reports `recordings`, FAT subtype `0x81`, and verify the reduced carve size/address.
2. Create a short recording through the play UI and export WAV. Save file size and SHA-256 as the first data evidence.
3. Use the real device lifecycle to return to the launcher and relaunch Play 28. Export the same recording again and require an identical SHA-256. A slot listing or HTTP 200 is not a substitute for file evidence.
4. Delete the recording in the UI and verify that it no longer appears or exports.
5. After removing the play, verify whether DATA is ARCHIVED or erased according to the current product policy. Slot absence alone does not prove physical erasure.
6. Record firmware source/version, play ID, declared DATA size, actual carve size/offset, both WAV hashes, reboot method, deletion policy, and uncovered cases.

## Automation boundary and next implementation

The generic mobile-page runner automates installer UI and read-only slot/DATA metadata checks. Its `e2edata` serial protocol only applies to the dedicated fixture and cannot drive Play 28's recording business logic. The real-play test must gather file-level evidence through the play's existing UI/export interface; do not inject fictitious `WRITE/READ` commands into a marketplace binary.

Unattended acceptance also requires pinning the exact Play 28 marketplace binary and confirming its recording UI DOM/API plus a usable hardware path to reboot/return to the launcher. These details are not pinned in this repository, so until they are supplied, Play 28's business E2E status must remain **BLOCKED / NOT RUN**. Passing the dedicated fixture does not change that status.

## Why keep the dedicated fixture

`tests/realdevice/data-child/` remains useful for isolated tests of dynamic partition lookup, serial control, DATA carve isolation, device-side checksums, and CI firmware builds. It is infrastructure coverage, not a substitute for a real marketplace play.