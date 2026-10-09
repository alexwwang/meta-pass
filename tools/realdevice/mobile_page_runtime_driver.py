#!/usr/bin/env python3
"""Runtime driver for mobile_page_e2e.mjs using the test-only launcher serial channel.

Requires pyserial. Build and flash a launcher with CONFIG_META_E2E_TEST_CONTROL=y,
then install the dedicated DATA child firmware as the selected marketplace play IDs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


def read_json(port, deadline, predicate=None):
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        try:
            obj = json.loads(raw.decode("utf-8", errors="replace").strip())
        except json.JSONDecodeError:
            continue
        if not isinstance(obj, dict):
            continue
        if predicate is None:
            if "ok" in obj:
                return obj
        elif predicate(obj):
            return obj
    raise TimeoutError("timed out waiting for expected device JSON response")


def send(port, command, expected_command, timeout):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    response = read_json(port, time.monotonic() + timeout,
                         lambda x: x.get("command") == expected_command or x.get("ok") is False)
    if response.get("ok") is not True or response.get("command") != expected_command:
        raise RuntimeError(f"{command.split()[0]} rejected: {response}")
    return response


def reopen_port(port):
    try:
        port.close()
    except OSError:
        pass
    time.sleep(0.25)
    try:
        port.open()
        return True
    except OSError:
        return False


def expect_rejection(port, command, expected_error, timeout):
    port.write((command + "\\n").encode("ascii"))
    port.flush()
    response = read_json(port, time.monotonic() + timeout,
                         lambda x: x.get("ok") is False)
    if response.get("error") != expected_error:
        raise RuntimeError(f"expected {expected_error} for {command}, got {response}")
    return response


def wait_event(port, event, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return read_json(port, min(deadline, time.monotonic() + 1.0),
                             lambda x: x.get("event") == event)
        except TimeoutError as exc:
            last = str(exc)
        except OSError as exc:
            last = str(exc)
            reopen_port(port)
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for device event {event}: {last or 'no event'}")


def launcher_hello(port, timeout):
    return send(port, "E2E HELLO", "HELLO", timeout)


def wait_launcher(port, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return launcher_hello(port, min(1.5, max(0.2, deadline - time.monotonic())))
        except (TimeoutError, RuntimeError, OSError) as exc:
            last = str(exc)
            if isinstance(exc, OSError):
                reopen_port(port)
            time.sleep(0.2)
    raise TimeoutError("launcher test control did not recover" + (": " + last if last else ""))


def wait_child(port, timeout):
    # Child emits READY once its own USB Serial/JTAG command protocol is installed.
    return wait_event(port, "READY", timeout)


def child_request(port, command, expected, timeout):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    response = read_json(port, time.monotonic() + timeout,
                         lambda x: x.get("command") == expected or x.get("ok") is False)
    if response.get("ok") is not True or response.get("command") != expected:
        raise RuntimeError(f"child {command.split()[0]} rejected: {response}")
    return response


def expected_digest(nonce, sequence):
    payload = bytes(ord(nonce[i % 32]) ^ ((i * 31) & 0xFF) for i in range(192))
    prefix = struct.pack("<II", 0x44415441, 1) + nonce.encode("ascii") + struct.pack("<I", sequence) + payload
    digest = hashlib.sha256(prefix).digest()
    crc = zlib_crc32(prefix + digest)
    return digest.hex(), f"{crc:08x}"


def zlib_crc32(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def read_slots(port, timeout):
    return send(port, "E2E SLOTS", "SLOTS", timeout)


def boot_child(port, slot, timeout, evidence_lines):
    response = send(port, f"E2E BOOT_SLOT {slot}", "BOOT_SLOT", timeout)
    evidence_lines.append(response)
    ready = wait_child(port, timeout)
    evidence_lines.append(ready)
    hello = child_request(port, "HELLO", "HELLO", timeout)
    if hello.get("chip") != "esp32c3":
        raise RuntimeError(f"unexpected child chip identity: {hello}")
    info = child_request(port, "INFO", "INFO", timeout)
    if info.get("label") != "e2edata" or int(info.get("size", 0)) < 4096:
        raise RuntimeError(f"child DATA partition invalid: {info}")
    evidence_lines.extend([hello, info])
    return info


def reboot_to_launcher(port, timeout, evidence_lines):
    ack = child_request(port, "REBOOT", "REBOOT", timeout)
    evidence_lines.append(ack)
    launcher = wait_launcher(port, timeout)
    evidence_lines.append(launcher)
    return launcher


def action_boot_test(port, args):
    evidence_lines = []
    evidence = {
        "childBooted": False, "dataWriteOk": False, "dataReadOk": False,
        "dataChecksumOk": False, "dataPersistedAfterReboot": False,
        "returnedToLauncher": False, "serialEvidence": "",
        "dataLabel": "e2edata", "slot": args.slot, "playId": args.play_id,
    }
    launcher_hello(port, args.timeout_s)
    before = read_slots(port, args.timeout_s)
    evidence_lines.append(before)
    slot_entry = next((s for s in before.get("slots", [])
                       if int(s.get("slot", -1)) == args.slot and
                       int(s.get("playId", -1)) == args.play_id and s.get("state") == "valid"), None)
    if not slot_entry:
        raise RuntimeError(f"launcher registry does not contain requested valid slot/play: {args.slot}/{args.play_id}")
    info = boot_child(port, args.slot, args.timeout_s, evidence_lines)
    data_entries = [d for d in before.get("data", [])
                    if int(d.get("playId", -1)) == args.play_id and d.get("label") == "e2edata"]
    if (len(data_entries) != 1 or
            int(data_entries[0].get("offset", -1)) != int(info["address"]) or
            int(data_entries[0].get("size", -1)) != int(info["size"])):
        raise RuntimeError("child DATA partition does not match launcher carve registry")
    evidence["dataAddress"] = int(info["address"])
    evidence["dataSize"] = int(info["size"])
    evidence["childBooted"] = True
    nonce = hashlib.sha256(f"{args.play_id}:{args.phase}:{time.time_ns()}".encode()).hexdigest()[:32]
    written = child_request(port, f"WRITE {nonce}", "WRITE", args.timeout_s)
    evidence_lines.append(written)
    digest, crc = expected_digest(nonce, int(written["sequence"]))
    evidence["dataWriteOk"] = bool(written.get("readback") is True and written.get("nonce") == nonce)
    evidence["dataChecksumOk"] = bool(written.get("sha256") == digest and written.get("crc32") == crc)
    first_read = child_request(port, "READ", "READ", args.timeout_s)
    evidence_lines.append(first_read)
    evidence["dataReadOk"] = bool(first_read.get("nonce") == nonce and
                                  first_read.get("sha256") == digest and first_read.get("crc32") == crc)
    reboot_to_launcher(port, args.timeout_s, evidence_lines)
    mid = read_slots(port, args.timeout_s)
    evidence_lines.append(mid)
    current = next((s for s in mid.get("slots", [])
                    if int(s.get("slot", -1)) == args.slot and
                    int(s.get("playId", -1)) == args.play_id and s.get("state") == "valid"), None)
    if not current:
        raise RuntimeError("child slot disappeared during persistence check")
    if int(current.get("offset", -1)) != int(slot_entry.get("offset", -2)):
        raise RuntimeError("child slot offset changed across reboot")
    new_info = boot_child(port, args.slot, args.timeout_s, evidence_lines)
    if (int(new_info.get("address", -1)) != int(info.get("address", -2)) or
            int(new_info.get("size", -1)) != int(info.get("size", -2))):
        raise RuntimeError("DATA partition geometry changed across reboot")
    persisted = child_request(port, "READ", "READ", args.timeout_s)
    evidence_lines.append(persisted)
    evidence["dataPersistedAfterReboot"] = bool(
        persisted.get("nonce") == nonce and persisted.get("sha256") == digest and
        persisted.get("crc32") == crc and persisted.get("readback") is True)
    reboot_to_launcher(port, args.timeout_s, evidence_lines)
    evidence["returnedToLauncher"] = True
    evidence["serialEvidence"] = json.dumps(evidence_lines, separators=(",", ":"))
    return evidence


def action_verify_deleted(port, args):
    launcher_hello(port, args.timeout_s)
    listing = read_slots(port, args.timeout_s)
    slots = listing.get("slots", [])
    data = listing.get("data", [])
    remaining_play = [s for s in slots if int(s.get("playId", -1)) == args.play_id]
    remaining_name = [s for s in slots if s.get("name") == args.slot_name]
    remaining_data = [d for d in data if int(d.get("playId", -1)) == args.play_id]
    # Ask the launcher to resolve this deleted play ID through its current
    # registry. It must reject the request, not fall back to a stale slot address.
    rejected = expect_rejection(port, f"E2E BOOT_PLAY {args.play_id}",
                                "SLOT_NOT_BOOTABLE", args.timeout_s)
    return {
        "deletedSlotNotBootable": not remaining_play and not remaining_name and rejected.get("error") == "SLOT_NOT_BOOTABLE",
        "dataPartitionReleased": not remaining_data,
        "serialEvidence": json.dumps({"listing": listing, "bootRejected": rejected}, separators=(",", ":")),
        "playId": args.play_id,
        "slotName": args.slot_name,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action", required=True, choices=("boot-test-and-return", "verify-deleted"))
    parser.add_argument("--slot", type=int, help="current launcher slot index")
    parser.add_argument("--play-id", type=int, required=True)
    parser.add_argument("--slot-name", required=True)
    parser.add_argument("--phase", default="runtime")
    parser.add_argument("--timeout-ms", type=int, default=180000)
    parser.add_argument("--evidence-file", required=True)
    parser.add_argument("--port", default=os.environ.get("META_PASS_E2E_SERIAL_PORT", ""))
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    if not args.port:
        parser.error("set --port or META_PASS_E2E_SERIAL_PORT to the native USB Serial/JTAG port")
    if args.action == "boot-test-and-return" and args.slot is None:
        parser.error("--slot is required for boot-test-and-return")
    args.timeout_s = max(5.0, args.timeout_ms / 1000.0)
    evidence = {"action": args.action, "playId": args.play_id, "slotName": args.slot_name}
    try:
        with serial.Serial(args.port, args.baud, timeout=0.25, write_timeout=10) as port:
            # USB Serial/JTAG is native USB; baud is ignored by the device.
            time.sleep(0.15)
            evidence = (action_boot_test(port, args) if args.action == "boot-test-and-return"
                        else action_verify_deleted(port, args))
        required = ["childBooted", "dataWriteOk", "dataReadOk", "dataChecksumOk",
                    "dataPersistedAfterReboot", "returnedToLauncher"]
        if args.action == "boot-test-and-return":
            evidence["ok"] = all(evidence.get(key) is True for key in required) and bool(evidence.get("serialEvidence"))
        else:
            evidence["ok"] = evidence.get("deletedSlotNotBootable") is True and evidence.get("dataPartitionReleased") is True
        with open(args.evidence_file, "w", encoding="utf-8") as output:
            json.dump(evidence, output, indent=2)
            output.write("\n")
        print(json.dumps(evidence, indent=2))
        return 0 if evidence["ok"] else 1
    except (OSError, RuntimeError, TimeoutError, KeyError, ValueError) as exc:
        evidence["ok"] = False
        evidence["error"] = str(exc)
        with open(args.evidence_file, "w", encoding="utf-8") as output:
            json.dump(evidence, output, indent=2)
            output.write("\n")
        print(json.dumps(evidence, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
