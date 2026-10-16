#!/usr/bin/env python3
"""Guard the USB backup reader's bounded-session and fail-closed contract."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PAGE = (ROOT / "install-slot" / "install-slot.html").read_text(encoding="utf-8")

def require(pattern: str, message: str) -> None:
    if not re.search(pattern, PAGE, re.S):
        raise SystemExit(f"FAIL: {message}")

require(r"const BACKUP_SESSION_SEGMENT\s*=\s*0x80000\s*;", "session segment must remain 512 KiB")
require(
    r"async function readFlashChunked\(start, size, onProgress, onRetry\).*?"
    r"Math\.min\(BACKUP_SESSION_SEGMENT, size - offset\).*?"
    r"readFlashChunkedSingleSession\(.*?start \+ offset.*?length.*?"
    r"out\.set\(part, offset\).*?offset \+= length.*?"
    r"if \(offset < size\) await restartBackupReadSession\(\)",
    "reader must process bounded segments and renew the session only between segments",
)
require(
    r"if \(!\(part instanceof Uint8Array\) \|\| part\.length !== length\).*?"
    r"throw new Error\(`Backup segment",
    "short or malformed segment must fail closed",
)
require(
    r"async function restartBackupReadSession\(\).*?"
    r"await withTimeout\(nextLoader\.main\(\).*?"
    r"readCarveOwnershipSnapshot\(\).*?"
    r"APP/DATA ownership metadata changed while restarting the backup session",
    "session restart must resync and reject changed APP/DATA ownership metadata",
)
_restart_start = PAGE.index("async function restartBackupReadSession(")
_restart_end = PAGE.index("async function readFlashChunked(", _restart_start)
if "loadSlotModel()" in PAGE[_restart_start:_restart_end]:
    raise SystemExit(
        "FAIL: session restart must not call loadSlotModel() — a transient record-read "
        "failure degrades slotModel and is then misreported as ownership change"
    )
require(
    r"const BACKUP_RETRIES = 3;.*?const BACKUP_READ_TIMEOUT_MS = 15000;",
    "failed reads must use bounded retries and a finite per-chunk timeout",
)
require(
    r"async function restartBackupReadSession\(\).*?"
    r"backupDegradeBaud && linkBaud !== ROM_BAUD.*?baudrate: nextBaud",
    "session restart must degrade to the ROM baud automatically after unstable reads (adaptive, no URL parameter)",
)
reader_start = PAGE.index("async function readFlashChunkedSingleSession(")
reader_end = PAGE.index('$("btn-backup").addEventListener', reader_start)
reader = PAGE[reader_start:reader_end]
if "await restartBackupReadSession()" not in reader:
    raise SystemExit("FAIL: failed reads must restart the session")
if "loader.readFlash(start + got, n)" not in reader or "BACKUP_READ_TIMEOUT_MS" not in reader:
    raise SystemExit("FAIL: retry must read the same offset with a finite timeout")
require(
    r"function withTimeout\(promise, ms, label\).*?clearTimeout\(timer\)",
    "successful reads must clear their timeout timer",
)
require(
    r"async function restartBackupReadSession\(\).*?"
    r"APP/DATA ownership metadata changed while restarting the backup session",
    "session restart must reject changed APP/DATA ownership metadata",
)
if "resyncAfterReadError" in PAGE:
    raise SystemExit("FAIL: obsolete layered recovery remains in the backup page")

require(
    r"async function readUsbStorageInventory\(\).*?project !== \"FoloToy-AI-Passport\".*?async function createUsbStorageSnapshot",
    "dynamic v2 snapshot must identify the host from the ESP app descriptor and use the raw snapshot builder",
)
require(
    r"async function readUsbStorageInventory\(\).*?slotModel\.carve\.data.*?partitions\.push\(",
    "snapshot inventory must merge carve DATA records (the launcher materialized table omits DATA partitions)",
)
require(
    r"if \(isDynMode\(slotModel\.mode\)\).*?createUsbStorageSnapshot",
    "dynamic mode must route backup through the v2 snapshot path",
)
require(
    r"const nvsData = await readFlashChunked\(",
    "historical fixed-slot v1 backup must retain its bounded NVS read",
)

print("USB segmented backup contract: PASS")
