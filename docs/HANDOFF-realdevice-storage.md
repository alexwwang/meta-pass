[Chinese](HANDOFF-realdevice-storage.zh_CN.md)

# Handoff | feat/storage Real-device validation

This file is the executable handoff for the next local coding agent.

## Goal

Validate the dynamic APP + DATA storage installation path on a real ESP32-C3 Passport, collect reproducible evidence, classify failures, and feed actionable findings back into code changes.

Work only on branch `feat/storage`; never develop directly on `main`.

## Test layers

1. Host TDD and firmware build.
2. passport-sim using the real ESP32-C3 QEMU path.
3. Real Passport hardware over USB Serial/JTAG + LAN HTTP.

A real power-loss test is separate from a soft reset. Never report soft reset as power-loss PASS.

## First test: passport-sim

This is the first mandatory local test. Do not proceed to real hardware until it passes.

Simulator:
https://github.com/VOID001/FoloToy-Passport-Simulator

Prerequisites:
- Node.js >= 20 and npm.
- Local checkout of passport-sim.
- The simulator checkout is next to meta-pass, or `PASSPORT_SIM_DIR` is set.
- `public/wasm/pkg/esp_emu_bg.wasm` is prepared.

Prepare:

```bash
git clone https://github.com/VOID001/FoloToy-Passport-Simulator.git ../passport-sim
cd ../passport-sim
npm install
npm run prepare:emulator
cd ../meta-pass
```

Verify the test harness:

```bash
test -f tools/sim/run-sim-test.sh
test -f tools/sim/metapass-boot.test.mjs
test -f tools/sim/esp_emu.js
test -f tools/sim/ai-passport-board.js
```

The harness must load the real `esp_emu_bg.wasm`, create `WasmEmulator("esp32c3")`, load a real ESP32-C3 Full Flash image at 0x0, and drive the real firmware through the board bridge. It must check CPU progress, a rendered 240x320 frame, and button behavior. These checks are already present in the current `feat/storage` harness.

Run:

```bash
cd ../meta-pass
./tools/validate.sh --firmware
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

Expected:
- all Node tests report `ok`;
- simulator process exits 0;
- output ends with `PASS — meta-pass simulator end-to-end test passed`.

On failure, first preserve:

```bash
git rev-parse HEAD
node --version
cd ../passport-sim && git rev-parse HEAD
cd ../meta-pass
sha256sum build/FoloToy-AI-Passport-full.bin
PASSPORT_SIM_DIR=../passport-sim tools/sim/run-sim-test.sh 2>&1 | tee tools/sim/sim-run.log
```

Classify before changing code:
- missing simulator/wasm: ENVIRONMENT
- invalid full image: FIRMWARE_IMAGE
- QEMU load/boot failure: BOOT
- no display frame: DISPLAY/APP_BOOT
- CPU progress/PC failure: CPU/CRASH
- button failure: BUTTON/FIRMWARE_INPUT
- incorrect test assumption: TEST_HARNESS

Only modify `feat/storage` when evidence points to meta-pass.

The current agent environment cannot actually execute the local Node/QEMU simulator runtime, so this task is not claimed as PASS here. It must be executed by the next local agent before real-device testing.

## Real-device prerequisites

- ESP32-C3 AI Passport with 8 MB flash.
- Data-capable USB-C cable.
- USB Serial/JTAG device visible to the host.
- ESP-IDF 5.5.3.
- Python 3, Node.js >= 20, esptool, pyserial.
- Passport and host on the same LAN.
- Device IP and USB port.

Check:

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py --version
python3 -m serial.tools.list_ports -v
```

ESP32-C3 provides a USB Serial/JTAG controller that supports serial console, flashing, and JTAG through the USB connection. Use `idf.py monitor` for firmware logs. See Espressif's USB Serial/JTAG and IDF Monitor documentation.

## Real-device command

Preferred entry:

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX
```

Clean-device run:

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX \
  --fresh
```

The wrapper executes `smoke.py` and creates one evidence directory under:

```
tools/realdevice/logs/<timestamp>/
```

Expected artifacts include command.txt, stdout.log, stderr.log, uart.log, HTTP observations, report.json and report.md.

## Functional stages

### S0
Build/flash/start. Verify no panic, watchdog loop, or boot loop and that the LAN service is reachable.

### S1
Capture pairing information from UART, pair through HTTP, verify token consistency, then prefer persistent session/token recovery.

### S2
Install APP + DATA. Verify:
- allocator proposal comes from the Node dynslot implementation;
- prepare succeeds;
- APP upload completes;
- DATA prefix upload establishes the expected checkpoint;
- status reports that DATA offset;
- only the DATA suffix is uploaded;
- DATA becomes done;
- finalize succeeds;
- reboot preserves one installed slot.

### S3
Install a second APP slot and verify two slots survive reboot without damaging the first installation.

### S4
Delete the DATA-bearing slot. Verify DATA becomes ARCHIVED. Then delete the remaining slot with eraseData and verify count=0.

### S5
Start a third install, upload only about one third, interrupt it, perform esptool soft reset, wait for service recovery, reissue prepare idempotently, resume from persisted offset, finalize and reboot.

This proves RAM-loss recovery. It does NOT prove physical power-loss durability.

### S6
Read partition table, carve store and DATA extent from flash. Byte-compare the DATA initial image against the flash bytes at the persisted DATA offset and calculate SHA-256.

## Failure evidence

On failure preserve:
- full stdout/stderr;
- UART log and tail;
- slots/status observations;
- executed commands and exit codes;
- partition/store/DATA dumps when safe;
- firmware SHA-256 and git commit;
- failure category.

Suggested categories:
ENV_MISSING, USB_PORT, USB_BUSY, FIRMWARE_FLASH, BOOT, AUTH, CAPACITY, CARVE, APP_UPLOAD, DATA_UPLOAD, DATA_RESUME, DATA_FINALIZE, ARCHIVE, REBOOT_RECOVERY, FLASH_VERIFY, POWER_LOSS_UNTESTED, UNKNOWN.

## Attribution

- Host failure -> logic/test issue.
- Firmware build failure -> compiler/linker/size.
- Simulator boot failure -> image/partition/bootloader.
- Simulator pass + real boot failure -> hardware, flash, image or real-device difference.
- HTTP pass + upload failure -> installer/storage protocol.
- Metadata correct + flash bytes wrong -> DATA write/offset/erase.
- Reboot failure -> durable carve record/boot recovery/runtime table.
- Soft-reset pass + physical power-loss failure -> durability/commit ordering.
- USB no logs -> cable/driver/port/monitor ownership.
- Panic/watchdog -> firmware runtime.
- Only fresh passes -> persistent state/migration/reclaim.

Do not change code before collecting evidence and classifying the failure.

## Report

report.json must contain at least:
- timestamp/run id;
- git branch/commit;
- firmware SHA-256;
- host and tool versions;
- serial port/IP;
- fresh flag;
- stage results;
- overall status;
- failure category;
- artifact paths;
- whether soft reset was tested;
- whether physical power loss was tested.

report.md must summarize the same information for a human and explicitly list untested items.

## Completion criteria

Do not claim complete hardware validation until:
- Host PASS;
- passport-sim PASS;
- real-device S0-S6 PASS;
- DATA byte-level flash verification PASS;
- reboot recovery PASS;
- evidence package complete.

If physical power loss was not actually performed, report POWER_LOSS_UNTESTED.
