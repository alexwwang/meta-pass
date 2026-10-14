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
    r"await loadSlotModel\(\).*?"
    r"APP/DATA ownership metadata changed while restarting the backup session",
    "session restart must resync and reject changed APP/DATA ownership metadata",
)
require(
    r"const payload = await readFlashChunked\(record\.offset, record\.size",
    "DATA backup must use the production segmented reader",
)

print("USB segmented backup contract: PASS")
