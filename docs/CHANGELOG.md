<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased
- Revert pipeline window 32768 -> 64 (stop-and-wait, identical to esptool.py read_flash);
  keep 921600. On-device A/B: esptool.py stop-and-wait @921600 ran 5/5 clean (87 KB/s),
  while the self-built pipeline failed proportionally to throughput at the same baud.
  Mechanism: with pipelining, ACK uplink overlaps the bulk data downlink on the same USB
  CDC endpoint, triggering C3 USB-Serial-JTAG RX loss (log evidence: post-failure drains
  up to 16 s = the stub was sitting on a large backlog of in-flight data). 921600 already
  shrank the per-frame gap from ~300 ms to ~10 ms, so pipelining bought <=15% at high risk.
- Post-hoc retraction of the round-6 investigation doc: the claimed standalone 2-byte
  stub error/status frame does not exist (header + status share one SLIP frame per
  stub_flasher.c; the OK probe and successful full backups disprove a leftover frame).
  Kept: the chunkT0 scope bug (real) and the mock-fidelity methodology lesson. See
  backup-readflash-error-status-frame.md (zh_CN keeps the original as an archive).
- On-device CLI verdict (esptool.py 4.12, 128 KB from slot-0 base): 115200 = 11.4 s,
  921600 = 1.5 s (7.6x); SHA256 identical across both baud rates; 5 consecutive reads
  at 921600 all passed. The high-baud link itself is reliable — backup read failures
  were client-side recovery flaws (now covered by tiered recovery), not the link.
- Tiered read-recovery for backup desync: L1 soft recovery (re-ACK, drain, sync) ->
  L2 reopen the port at the session baud (fixes the hardcoded 115200 reopen that caused
  "5 consecutive failures at the same address": during a 921600 session a 115200 reopen
  yields baud-mismatch garbage) -> L3 full USB-JTAG reset + stub re-upload + baud
  restore (fresh loader instance; no reuse of half-dead state). Every tier logs and
  propagates its own failures — no silent hangs.
- Connect at 921600 baud (8x faster reads). The earlier conclusion that "baudrate is a
  no-op on C3 native USB" was disproved by measurement: debug logs show ~356 ms per 4 KB
  frame, matching 115200-baud wire time. The reason previous baud changes did nothing:
  the page passed baudrate===romBaudrate, so the vendor main() changeBaud branch never
  fired. Now connecting with baudrate=921600/romBaudrate=115200 (standard changeBaud flow),
  with an immediate data-path check and automatic fallback to 115200 on any failure
  (worst case = previous behavior). Read timeout 15s->8s.
- Pipelined backup reads for a large speedup (measured 10.1 KB/s baseline): pinned down the
  `max_in_flight` semantics of the stub's `handle_flash_read` (`stub_commands.c:111`,
  `num_sent - num_acked < max_in_flight` — all three are **bytes**). We passed 64, i.e. 64
  bytes — less than one 4 KB frame, so the stub stopped and waited for an ACK after every
  frame; on top of that, USB-CDC only flushes a trailing partial (<64 B) packet when pushed
  by the next data wave — and in stop-and-wait every frame ends in a 4-byte trailing packet,
  costing ~300 ms per frame. The in-flight window is now
  `globalThis.__READFLASH_PARAMS__ = [4096, 32768]` (byte window = one 32 KB chunk; the stub
  streams 8 frames before waiting); ACKs are still sent per frame (as esptool.py does), with
  cumulative values staying within 0x1000..0x8000 (no 0xC0/0xDB bytes). Covered by a new
  "window bytes semantics" case in `test-readflash-protocol.mjs` (32 KB read must stream 8
  frames with zero ACKs; stop-and-wait mode still supported).
- Fix two transport-layer deadlock defects (observed as: every read session dies at ~2 min,
  and the retry recovery then hangs silently for 30+ minutes with no log):
  ① vendor `readLoop` abandons the in-flight `reader.read()` on timeout; when that
  generator's timeout timer later fires, `finally{buffer=new Uint8Array(0)}` wipes the whole
  transport buffer — any read session outliving `FLASH_READ_TIMEOUT` self-destructs, and
  every `newRead` spawning a fresh generator left stale timers counting. Now: persistent
  `_pendingRead`, explicit generator close, buffer wipe removed; `FLASH_READ_TIMEOUT`
  100s→15s.
  ② vendor `flushInput()` starts with `await this.reader.closed`, which never settles on an
  active serial port — the recovery path hung there forever. Now a bounded cancel
  (cancel + 500ms race); the page-level recovery chain got hard timeouts on every step and
  auto re-opens the serial port when sync fails.
- Add a Debug-mode checkbox to the backup section: logs protocol-level diagnostics (per-chunk
  timing, recovery steps, timeout positions) for remote troubleshooting.
- Fix the root cause of backup read failures ("Packet content transfer stopped" / "No serial
  data received", retries never recovering): the esptool stub appends an unconditional 16-byte
  MD5 digest frame after flash-read data frames (`stub_commands.c`), which esptool-js never
  reads — the leftover frame poisons the next command's response and protocol desync
  accumulates per `readFlash` call. `install-slot/vendor/esptool-js.js` now ACKs per data
  frame and reads/verifies the digest frame (matching `esptool.py read_flash`); new
  `install-slot/vendor/md5.js` provides the digest check; read params pinned to the official
  values (4 KB block — the stub's hard limit — and 64-frame in-flight window). Covered by
  `tools/install-slot/test-readflash-protocol.mjs` (mock-stub protocol test, wired into
  `validate.sh`).
- Add `tools/test-bootable-qemu.mjs`: headless QEMU boot verification — boots build artifacts
  through the passport-sim QEMU WASM core and asserts three things: the UART0 test variant
  completes bootloader → partition table → factory app → app_main; the MPUP upgrade container
  flashed raw at 0x0 indeed fails to boot (negative case, matching real-device behavior); the
  market image (USB-JTAG config) renders non-black ST7789 frames (LVGL display init runs).
  The bootability of the market image `meta-pass-bootable_*.bin` now has automated evidence
  instead of relying on real-device trial flashes.
- Fixed the installer connect flow breaking after a failed connect: the serial port is now released on any connect error (previously a hung or failed connect left the port open and every retry hit "The port is already open"), connects are non-reentrant (`busy`/`connecting` guards), a half-open session no longer clobbers an existing connection (commit to globals happens only after every step succeeds), and a device that silently drops a command no longer hangs the flow forever (15 s probe timeout via `withTimeout`). Removed the pointless `changeBaud()` disconnect/reconnect dance — baud is a no-op on the C3's native USB — and replaced the broken 16 KB read-block probe (the stub's `handle_flash_read` uses a 4 KB stack buffer and **silently returns** for larger blocks) with the officially sanctioned speedup: block size stays at the stub's 4 KB maximum while the in-flight window is raised from 4 KB to 64 blocks × 4 KB = 256 KB, cutting ACK round-trips 64× (mirrors upstream `esptool.py` which already used 4 KB blocks / 64-deep window). Probe verifies bootloader magic + exact length and falls back to conservative 1 KB/4 KB on any mismatch.
- Data-preserving launcher upgrades (§7.2): upgrades write only bootloader + partition table + factory app + an erased-state OTA-data reset; NVS (Wi-Fi credentials), `cardid`, and all three child-firmware slots are never touched. The USB installer gained a "7. Upgrade launcher" section that reads back the device partition table and byte-compares it with the bundle before writing (layout mismatch refuses the upgrade). `tools/build-firmware.sh` now emits a `build/upgrade/` bundle (4 files + `flash-args.txt`), and `tools/verify_firmware.py` enforces that the merged image keeps `nvs`/`ota_0-2`/`otadata` erased so a full image can never carry user-data-destroying content. Core logic in `install-slot/launcher-upgrade.js` with Node tests wired into `tools/validate.sh --static`. The upgrade is distributed as a **single-file MPUP container** (`build/upgrade/meta-pass-upgrade_<version>.bin`: magic + segment table + per-segment SHA-256) — the installer picks this one file, unpacks and verifies it, then writes the four segments to their partition addresses; `verify_firmware.py` additionally enforces container parity with the merged image.
- Slot backup & restore on the USB installer page (`install-slot/`): per-slot backup reads the full partition, slices it into `slot{N}_firmware.bin` (parsed ESP app image) + `slot{N}_tail.bin` (4 KB MSIG/MAEG/MNAM metadata sector) + optional `slot{N}_extra.bin` (post-tail storage), computes SHA-256 per file, and packs everything with a `manifest.json` into a timestamped zip. Restore lets the user map each backed-up slot to any target slot, checks free space against the manifest lengths (adaptive to future slot-size changes), verifies every file's SHA-256 before writing, then flashes firmware → extra → tail (tail last so sector re-erase cannot destroy earlier writes). A slot containing data that is neither erased nor a recognizable app image (e.g. slot 2 doubling as a data-storage partition, littlefs volumes, non-ESP resource packs) is no longer skipped: it gets a dd-style raw mirror fallback — `slot{N}_raw.bin` with trailing erased bytes trimmed, a `type: "raw"` manifest entry, restore written back from the slot base address with only a size ≤ partition and SHA-256 check (no tail-sector reservation). Backup/restore live in their own sections (§5/§6) separate from the install flow; core logic ships in the pure ES module `slot-backup.js` with Node tests wired into `tools/validate.sh --static`. Fixed a `DataView(TypedArray)` compatibility bug in the zip reader (older engines require an ArrayBuffer).
- Signature-verification hardening (`feat/sign` branch): fixed BUG-01/02/04 (uninitialized battery label, inverted `HOST_TEST` egg-magic check with m1–m4 regression tests, `%zu` for `size_t` log), single-source install page (`server.mjs` serves the canonical `install-slot/`; dev-copy drift closed, BUG-03), one-command local build (`tools/build-firmware.sh`), bilingual bug report and root-cause knowledge base (`docs/BUGS.md`, `docs/assets/handoff-unsigned-rootcause.md`, `docs/assets/meta-pass-signing-design.md`, `docs/development/engineering/debugging-workflow.md`). Root cause of the on-device "unsigned" symptom: the stale deployed installer page, not the signing chain; re-flashing through the fixed page restores expected behavior.
- Added the meta-pass multi-firmware launcher (`feature/meta-pass` branch): the partition table keeps the `factory`/`cardid` contract in place while adding `otadata` and three OTA slots of differing sizes (`ota_0@0x180000` / `0x1D6000`, `ota_1@0x360000` / `0x200000`, `ota_2@0x560000` / `0x29E000`); app rollback is enabled (un-adapted child firmware automatically falls back to the launcher on any reboot); firmware import over Wi-Fi SoftAP + web page (random password + on-screen one-time pairing code, streamed in 1024-byte chunks); mandatory image integrity checks (magic/chip-id/size/SHA-256 display, with `esp_ota_end()` as the authoritative re-check) and a warning page with a BOOT / CANCEL menu before booting unsigned firmware; local management UI to list/boot/delete slot firmware; the button BSP exposes an explicit `BSP_BTN_LONG` (1.5 s) threshold; pure-logic modules (image validation, slot registry, import state machine) ship with host tests wired into the static gate. Design: `docs/assets/meta-pass-design.md`.
- Second import channel (USB serial, `tools/install-slot/`): Chrome + Web Serial +
  esptool-js write child firmware directly into a slot while the device is in ROM
  download mode (power on while holding UP); sources are a local `.bin` (Full Flash
  images are unpacked to their app image) or a community play link (verified against
  the published SHA-256). Design: `docs/assets/meta-pass-design.md` §6.1.
- Slot display-name blob (§6.2): the real firmware name is written at install time
  into the slot partition's last 4KB sector (`slot_offset + partition_size − 4KB`,
  derived per slot because the three slot sizes now differ: `0x1D6000`/`0x200000`/
  `0x29E000`; `magic "MNAM"` + length + printable ASCII + XOR checksum, ≤32 bytes);
  the launcher scan prefers it and falls back to the core name (`project_name` minus
  the `FoloToy-` prefix, new `meta_slot_core_name`). `ota_2` is a dual-use area
  (bootable child slot, or littlefs recording storage when empty). The factory
  app limit tightens to 1.44 MB (`0x170000`) with `ota_0` moved into the
  cardid-before gap (`0x180000`). The USB install page auto-fills the community
  play's English title / local file name; the Wi-Fi import page gained an optional
  name field.

- Added the supplied 80-byte CW2017 profile for the specified 520 mAh cell, including content/update-flag checks, verified writes, the required restart sequence, and bounded SOC-readiness polling.

- Expanded the environment bootstrap document: added Espressif's Git service mirror (`git.espressif.com.cn`) as the preferred mainland-China route for ESP-IDF v5.5.3 and its submodules, documented submodule long-wait/timeout handling, in-place repair, and the pinned-commit shallow fetch for large submodules such as `esp32-wifi-lib`, warned about stale per-repository Jihulab `insteadOf` residue, and added the official offline release archive as a last-resort fallback (learned from `esp-mosaico/esp-mosaico-vibe`).

- Reorganized the documentation by function area with a dual entry point: the root `AGENTS.md` is now a thin router (hard constraints + task routing only) and the detailed AI workflow lives in `docs/development/ai-guide.md`; `agent-guide.md` was folded in. `docs/development/` gained a second level (`engineering/`, `ci/`, `release/`), and the `plays/` application archive and `experiences/` moved into a `docs/reference/` area with a dedicated README. Removed `docs/software-design/` (empty scaffold); folded the three `assets/{fonts,images,music}/README` leaves into the `assets/` README; flattened the six `project-completion` sub-documents into a single file; and unified each directory to a single README, eliminating every `INDEX` file and a duplicated experience index. All cross-references and bibliographic links were updated; no content was dropped.

- Removed the obsolete app/test partition at `0x700000` and its related
  bootloader, validation, and documentation requirements. The fixed protected
  `cardid` partition and its CI checks remain unchanged.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
