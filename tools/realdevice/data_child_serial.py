#!/usr/bin/env python3
"""Narrow USB Serial/JTAG client for the DATA E2E child firmware.

Requires pyserial: python -m pip install pyserial
This tool does not select/boot launcher slots and is not the mobile-page runtime driver.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


def read_json_line(port, deadline):
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        try:
            item = json.loads(raw.decode("utf-8", errors="strict").strip())
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue  # Ignore boot log noise; accept only protocol JSON.
        if isinstance(item, dict) and "ok" in item:
            return item
    raise TimeoutError("timed out waiting for child firmware JSON response")


def request(port, command, timeout):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    result = read_json_line(port, time.monotonic() + timeout)
    if result.get("ok") is not True:
        raise RuntimeError(f"device rejected {command.split()[0]}: {result}")
    return result


def expected_digest(nonce, sequence):
    nonce_bytes = nonce.encode("ascii")
    payload = bytes(ord(nonce[i % 32]) ^ ((i * 31) & 0xFF) for i in range(192))
    prefix = struct.pack("<II", 0x44415441, 1) + nonce_bytes + struct.pack("<I", sequence) + payload
    digest = hashlib.sha256(prefix).digest()
    crc = zlib.crc32(prefix + digest) & 0xFFFFFFFF
    return digest.hex(), f"{crc:08x}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB Serial/JTAG device path, e.g. /dev/cu.usbmodemXXXX")
    parser.add_argument("--baud", type=int, default=115200, help="ignored by native USB Serial/JTAG; retained for serial adapters")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--command", choices=("hello", "info", "write", "read", "erase", "reboot"), required=True)
    parser.add_argument("--nonce", help="32 hexadecimal chars; required for write and optional for read verification")
    parser.add_argument("--evidence-file", help="write command/result JSON to this path")
    args = parser.parse_args()

    if args.command == "write" and not re.fullmatch(r"[0-9a-fA-F]{32}", args.nonce or ""):
        parser.error("--nonce must be exactly 32 hexadecimal characters for write")
    if args.command == "read" and args.nonce and not re.fullmatch(r"[0-9a-fA-F]{32}", args.nonce):
        parser.error("--nonce must be exactly 32 hexadecimal characters")

    command = {"hello": "HELLO", "info": "INFO", "write": f"WRITE {args.nonce}",
               "read": "READ", "erase": "ERASE", "reboot": "REBOOT"}[args.command]
    evidence = {"tool": "data_child_serial.py", "command": args.command, "port": args.port,
                "startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    try:
        with serial.Serial(args.port, args.baud, timeout=0.25, write_timeout=args.timeout) as port:
            # Let any previous boot READY line drain; commands remain request/response serialized.
            time.sleep(0.15)
            if args.command != "hello":
                hello = request(port, "HELLO", args.timeout)
                evidence["hello"] = hello
            result = request(port, command, args.timeout)
            evidence["response"] = result
            if args.command == "write":
                digest, crc = expected_digest(args.nonce, int(result["sequence"]))
                evidence["hostChecksumOk"] = result.get("sha256") == digest and result.get("crc32") == crc
                evidence["nonceMatched"] = result.get("nonce", "") == args.nonce
                evidence["deviceReadbackOk"] = result.get("readback") is True
            elif args.command == "read":
                nonce_to_check = args.nonce or str(result.get("nonce", ""))
                digest, crc = expected_digest(nonce_to_check, int(result["sequence"]))
                evidence["hostChecksumOk"] = result.get("sha256") == digest and result.get("crc32") == crc
                evidence["nonceMatched"] = bool(re.fullmatch(r"[0-9a-fA-F]{32}", nonce_to_check)) and result.get("nonce", "") == nonce_to_check
                evidence["deviceReadbackOk"] = result.get("readback") is True
            evidence["ok"] = all(v is True for k, v in evidence.items()
                                 if k in ("hostChecksumOk", "nonceMatched", "deviceReadbackOk"))
            print(json.dumps(evidence, indent=2))
            if args.evidence_file:
                with open(args.evidence_file, "w", encoding="utf-8") as out:
                    json.dump(evidence, out, indent=2)
                    out.write("\n")
            if evidence.get("ok") is False:
                return 1
            return 0
    except (OSError, RuntimeError, TimeoutError, KeyError, ValueError) as exc:
        evidence["ok"] = False
        evidence["error"] = str(exc)
        print(json.dumps(evidence, indent=2), file=sys.stderr)
        if args.evidence_file:
            with open(args.evidence_file, "w", encoding="utf-8") as out:
                json.dump(evidence, out, indent=2)
                out.write("\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
