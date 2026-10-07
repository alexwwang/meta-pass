#!/usr/bin/env python3
"""Local real-device smoke orchestrator and evidence collector."""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGROOT = os.path.join(REPO, "tools", "realdevice", "logs")

def redact(s):
    return re.sub(r"(token=)[0-9a-f]{32}", r"\1<redacted>", s or "", flags=re.I)

def http_get(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return None, repr(e).encode()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--app", default=os.path.join(REPO, "build", "FoloToy-AI-Passport.bin"))
    ap.add_argument("--fresh", action="store_true")
    args = ap.parse_args()

    run_id = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(LOGROOT, run_id)
    os.makedirs(run_dir, exist_ok=True)

    cmd = [sys.executable, os.path.join(REPO, "tools", "realdevice", "smoke.py"),
           "--ip", args.ip, "--port", args.port, "--app", args.app]
    if args.fresh:
        cmd.append("--fresh")

    report = {
        "status": "RUNNING", "failure_category": None,
        "git_commit": "unknown", "git_branch": "unknown",
        "firmware": args.app, "firmware_sha256": None,
        "host": platform.platform(), "python": platform.python_version(),
        "serial_port": args.port, "device_ip": args.ip, "fresh": args.fresh,
        "soft_reset_tested": False, "physical_power_loss_tested": False,
        "stages": [], "artifacts": []
    }

    def git_value(argv):
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else "unknown"

    report["git_commit"] = git_value(["git", "rev-parse", "HEAD"])
    report["git_branch"] = git_value(["git", "branch", "--show-current"])
    if os.path.isfile(args.app):
        with open(args.app, "rb") as f:
            report["firmware_sha256"] = hashlib.sha256(f.read()).hexdigest()

    with open(os.path.join(run_dir, "command.txt"), "w", encoding="utf-8") as f:
        f.write(" ".join(cmd) + "\n")

    started = time.time()
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    elapsed = time.time() - started
    stdout, stderr = redact(proc.stdout), redact(proc.stderr)

    open(os.path.join(run_dir, "stdout.log"), "w", encoding="utf-8").write(stdout)
    open(os.path.join(run_dir, "stderr.log"), "w", encoding="utf-8").write(stderr)

    candidates = []
    if os.path.isdir(LOGROOT):
        for root, _, files in os.walk(LOGROOT):
            for name in files:
                if name == "uart.log" or (name.startswith("smoke-") and name.endswith(".log")):
                    p = os.path.join(root, name)
                    if p != os.path.join(run_dir, name):
                        candidates.append(p)
    if candidates:
        newest = max(candidates, key=os.path.getmtime)
        shutil.copy2(newest, os.path.join(run_dir, "uart.log"))
        report["uart_source"] = newest

    for label, path in (("slots", "/api/install/slots"), ("status", "/api/install/status")):
        status, body = http_get("http://" + args.ip + path)
        report.setdefault("http_observations", {})[label] = {"status": status}
        open(os.path.join(run_dir, label + ".response"), "wb").write(body)

    for line in stdout.splitlines():
        m = re.match(r"=== (S\d+ .+?) ===", line)
        if m:
            report["stages"].append({"name": m.group(1), "status": "OBSERVED"})
        if "软复位" in line:
            report["soft_reset_tested"] = True

    if proc.returncode == 0:
        report["status"] = "PASS"
    else:
        report["status"] = "FAIL"
        text = (stdout + "\n" + stderr).lower()
        if "serial" in text or "port" in text:
            report["failure_category"] = "USB_PORT"
        elif "esptool" in text:
            report["failure_category"] = "FIRMWARE_FLASH"
        elif "data" in text:
            report["failure_category"] = "DATA"
        elif "carve" in text:
            report["failure_category"] = "CARVE"
        elif "reboot" in text or "复位" in text:
            report["failure_category"] = "REBOOT_RECOVERY"
        else:
            report["failure_category"] = "UNKNOWN"

    report["elapsed_seconds"] = round(elapsed, 2)
    report["artifacts"] = sorted(os.path.relpath(os.path.join(run_dir, p), REPO)
                                 for p in os.listdir(run_dir))

    with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    with open(os.path.join(run_dir, "report.md"), "w", encoding="utf-8") as f:
        f.write("# meta-pass 真机 Smoke Report\n\n")
        f.write("- Status: " + report["status"] + "\n")
        f.write("- Failure category: " + str(report["failure_category"]) + "\n")
        f.write("- Commit: " + report["git_commit"] + "\n")
        f.write("- Firmware SHA256: " + str(report["firmware_sha256"]) + "\n")
        f.write("- Soft reset tested: " + str(report["soft_reset_tested"]) + "\n")
        f.write("- Physical power loss tested: NO\n\n")
        f.write("## Stages observed\n\n")
        for stage in report["stages"]:
            f.write("- " + stage["name"] + ": " + stage["status"] + "\n")
        f.write("\n## Artifacts\n\n")
        for artifact in report["artifacts"]:
            f.write("- " + artifact + "\n")
        f.write("\n## Interpretation\n\n")
        if proc.returncode == 0:
            f.write("Smoke returned 0. Physical power-loss recovery was not tested and is not PASS.\n")
        else:
            f.write("Smoke failed. Use stdout/stderr/UART/HTTP evidence and the handoff attribution matrix before changing code.\n")

    print("Report: " + os.path.join(run_dir, "report.md"))
    print("Artifacts: " + run_dir)
    return proc.returncode

if __name__ == "__main__":
    raise SystemExit(main())
