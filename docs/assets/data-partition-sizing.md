# Generic DATA Partition Sizing

Status: implementation baseline for `feat/storage`

## 1. Goal

Make DATA partition capacity a general installation capability rather than a test-task override. For compatible dynamic-partition firmware, the user chooses the capacity during installation; the installer presents the device-supported range, and firmware independently validates the request before committing the partition layout.

The feature must not depend on a Play ID, test task, test switch, or blank-FAT assumption. Legacy firmware without a DATA request keeps its existing behavior. Unsupported devices must report that dynamic DATA sizing is unavailable rather than silently using an incorrect size.

## 2. Principles

1. Separate production policy from test fixtures.
2. The device is authoritative: UI values are advisory and are revalidated on-device.
3. Never silently shrink, format, truncate, or overwrite existing user data.
4. Use bytes in protocols/storage and MiB in the UI; conversions must be explicit.
5. Commit the complete carve layout transactionally; failed operations preserve the last valid layout.
6. Missing DATA fields in legacy offers mean no additional DATA request, not a zero-sized partition.

## 3. User flow

1. The installer parses the firmware image and its DATA manifest.
2. The device reports its current carve listing and the app allocation proposal.
3. For new DATA extents, the page displays a selectable capacity range, the firmware-declared minimum, and the current contiguous-space constraints.
4. The selected byte capacity is included in the install offer.
5. The device independently validates size, alignment, available space, reserved regions, labels, and overlaps before commit.
6. The running child firmware sees only its own DATA partitions.

## 4. Capacity range and UI

- Minimum: the firmware-declared `required_size`, rounded up to the 4 KiB DATA size granule. The UI must not let the user go below this requirement.
- Maximum: the largest usable contiguous free extent after accounting for existing APP/DATA allocations and the proposed new APP extent, with the 64 KiB placement alignment and 4 KiB size granule applied.
- UI step: 1 MiB, while preserving the exact firmware minimum and a valid aligned maximum as explicit choices.
- The UI may conservatively advertise less than total aggregate free space; it must never claim that fragmented aggregate free space is one contiguous extent.
- If the dynamic listing is unavailable, do not invent a capacity range. Keep the existing installation path and firmware-declared size.

## 5. Existing DATA and compatibility

The first sizing UI applies to new DATA extents. If a matching `(play_id, label)` DATA record already exists and meets the firmware minimum, its current capacity is retained and the selector is disabled for that record. Existing data is not silently resized or erased. Any future explicit resize workflow must explain migration, provide a recoverable transaction, and clearly distinguish a capacity change from formatting.

Offers without DATA remain backward compatible. The device rejects invalid, unaligned, overflowing, overlapping, out-of-range, or space-exhausting requests. Fixed protected regions, including `cardid`, NVS and `otadata`, are never allocatable.

## 6. Device allocation and commit

Reuse `meta_carve` and `meta_carve_flash`; do not create a second allocator.

1. Prepare a candidate APP + DATA layout.
2. Recompute placement and capacity constraints on-device.
3. Validate labels, subtypes, address ranges, alignment, overlaps, and protected regions.
4. Write the new A/B carve record first, then materialize the runtime partition table.
5. On failure, preserve the last committed record and recover the table from it on next boot.
6. Keep DATA ownership associated with the child firmware identity and materialize only that child's DATA view before launch.

## 7. Test-driven implementation

### Host / pure-logic tests

- Capacity conversions, 4 KiB size granularity and 64 KiB start alignment.
- Min/max boundaries, just below/above bounds, malformed values and integer overflow.
- Existing APP/DATA occupancy and proposed APP extent.
- Fragmented space and overlapping/corrupt occupancy (fail closed).
- Legacy offers without DATA fields.

### Device and UI tests

- Device independently rejects illegal requests.
- Reinstall preserves an existing DATA extent and contents.
- UI renders the available range and disables sizing when geometry is unknown or a DATA extent already exists.
- English/Chinese UI copy, narrow mobile viewport, cancel/confirm behavior.
- Firmware build, browser regression, then real-device tests for write/read, reboot persistence, insufficient space and interruption recovery.

## 8. Acceptance criteria

- Capacity selection is a general install feature, not enabled by a test task or Play ID.
- Users can select a valid size for new DATA extents within the device-supported range.
- Device-side validation remains authoritative and committed layouts remain recoverable.
- Existing DATA contents are not silently erased or shrunk.
- Host/static tests, firmware build, browser tests and real-device verification pass before declaring the feature complete.

## 9. Implementation status (2026-10-10)

- Added shared pure-JavaScript capacity range and value-validation helpers in `install-slot/dynslot-pool.js`, accounting for existing APP/DATA occupancy, 64 KiB placement alignment and 4 KiB size granularity. Invalid overlapping layouts advertise no available capacity.
- Added a DATA sizing step to `install-slot/phone-install.js` before upload. It uses the device's dynamic listing and the proposed new APP extent; selected sizes are included in the install offer.
- Existing matching DATA extents that meet the firmware minimum are displayed at their current size and cannot be changed through this selector.
- Added capacity-boundary and occupancy tests to `tests/test_dynslot_pool.mjs`.
- Remaining: rerun and pass static/Node tests and firmware build; audit full device-side validation/commit/rollback behavior for selected capacities; run browser and real-device verification. Do not treat the current implementation as accepted until those gates pass.
