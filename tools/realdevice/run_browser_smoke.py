#!/usr/bin/env python3
"""tools/realdevice/run_browser_smoke.py —— 前端模块 × 真机回归的编排与取证。

补的缺口:smoke.py 是纯 Python 协议客户端(只 import dynslot-pool.js),证明设备
契约;tests/test_phone_install.mjs 用 mock fetch,证明模块逻辑自洽。两者都不证明
**真实前端模块对真机**可用(2026-10-08 真机就是靠这条路绿到 /api/install/status
丢 2 字节 JSON 尾巴才炸)。tools/realdevice/browser_smoke.mjs 用真 fetch 打真机,
本文件负责无人值守的前置与证据:
  1. 从 NVS 取持久化 token(smoke.try_restore_token 同款,免物理按键配对),
     --token 可显式覆盖;
  2. 强制 IDF_PYTHON_ENV_PATH=idf5.5_py3.10_env —— 默认侦测的 py3.14 venv
     pydantic_core ABI 损坏,esptool 直接不可用;
  3. stream node 输出 + 抓 ##REPORT 行 + 全量落 tools/realdevice/logs/。

状态中立:本文件不碰 flash;browser_smoke.mjs 只装测试槽位并按 (offset,size)
精确自清,不清池、不 erase-flash、不动 cardid/otadata/既有归档记录。

运行(仓库根):
  python3 tools/realdevice/run_browser_smoke.py --ip <device-ip> \
      --port /dev/cu.XXXX [--fixture build/FoloToy-AI-Passport.bin]
"""
import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
LOGROOT = os.path.join(HERE, "logs")
SCRIPT = os.path.join(HERE, "browser_smoke.mjs")
# FoloToy-AI-Passport-full.bin 是 app 补零到 8MB、0x8000 无分区表,不能当
# fixture(合成完整镜像会越界);默认取 app-only 构建产物。
FIXTURE_DEFAULT = os.path.join(REPO, "build", "FoloToy-AI-Passport.bin")
# 默认侦测的 py3.14 venv 可能坏了(pydantic_core ABI 不匹配);本机存在健康的
# py3.10 venv 就显式指定,否则交给 IDF export.sh 自己探测 —— CI runner 上没有
# 这份本机 env,不能用硬编码路径。
HOMEDIR = os.path.expanduser("~")
_PY310 = os.path.join(HOMEDIR, ".espressif", "python_env", "idf5.5_py3.10_env")
IDF_PY310 = _PY310 if os.path.isdir(_PY310) else None

REPORT_LINE = "##REPORT "
TOKEN_RE = re.compile(r"(?i)\b[0-9a-f]{32}\b")
IPV4_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
SERIAL_RE = re.compile(r"(?i)\b(?:/dev/)?(?:cu\.usbmodem|tty\.usbmodem|ttyUSB|ttyACM)\d+\b")
HOME_RE = re.compile(re.escape(HOMEDIR) + r"(?!/[^/]*\.git)")


def redact(s):
    """证据脱敏:token / LAN IP / USB 串口号 / 本机 home 绝对路径。

    只作用于**写进证据文件**的内容(stdout.log / command.txt / report.*)。
    本机交互输出不脱敏 —— 调试时需要看真实 IP/端口。
    """
    s = TOKEN_RE.sub("<token>", s)
    s = IPV4_RE.sub("<ip>", s)
    s = SERIAL_RE.sub("<serial>", s)
    s = HOME_RE.sub("$HOME", s)
    return s


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True, help="ESP32 设备 LAN IPv4")
    ap.add_argument("--port", default=os.environ.get("META_PASS_PORT"),
                    help="USB 串口(仅用于从 NVS 取 token)")
    ap.add_argument("--token", default=None,
                    help="显式 session token(32 hex);缺省则从 NVS 恢复")
    ap.add_argument("--fixture", default=FIXTURE_DEFAULT,
                    help="P2 DATA 分支用的本地全镜像(默认 build 产物)")
    ap.add_argument("--play", default=None,
                    help="P1 live-store 玩法 id(缺省取脚本内默认)")
    ap.add_argument("--node", default=os.environ.get("NODE_BIN"),
                    help="真 node(~/.local/bin/node 是 bun 包装,argv 语义不同)")
    ap.add_argument("--logdir", default=None,
                    help="证据目录(默认 logs/browser-smoke-<ts>)")
    ap.add_argument("--timeout", type=int, default=1500,
                    help="node 子进程超时秒(含 2MB×2 真上传)")
    args = ap.parse_args()
    if not args.port:
        ap.error("--port 缺失:传 --port /dev/cu.XXXX 或 export META_PASS_PORT=...")
    if not args.node:
        ap.error("--node 缺失:传 --node 或 export NODE_BIN=...")


    if IDF_PY310:
        os.environ["IDF_PYTHON_ENV_PATH"] = IDF_PY310
    else:
        os.environ.pop("IDF_PYTHON_ENV_PATH", None)
    ts = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    logdir = args.logdir or os.path.join(LOGROOT, "browser-smoke-" + ts)
    os.makedirs(logdir, exist_ok=True)

    cmd = [args.node, SCRIPT, "--ip", args.ip, "--port", args.port,
              "--fixture", args.fixture]
    if args.token:
        cmd += ["--token", args.token]

    eprint(f"设备     : {args.ip}")
    eprint(f"串口     : {args.port}")
    eprint(f"fixture  : {os.path.relpath(args.fixture, REPO)}")
    eprint(f"证据目录 : {os.path.relpath(logdir, REPO)}")
    eprint(f"node     : {args.node} ({subprocess.run([args.node, '--version'], capture_output=True, text=True).stdout.strip()})")

    token = args.token
    if not token:
        eprint("取 token : 从 NVS 恢复(esptool read_flash 0x9000)...")
        sys.path.insert(0, HERE)
        import smoke
        api = smoke.Api(args.ip)
        token = smoke.try_restore_token(args.port, api)
        if not token:
            eprint("✗ NVS 里没有可用 token —— 在设备上 DOWN→OK 进商店页生成配对码,"
                   "或用 --token 显式传入")
            return 1
        eprint(f"           ✓ 复用持久化 token(免配对)")
    cmd += ["--token", token]
    if args.play:
        os.environ["BROWSER_SMOKE_PLAY"] = args.play

    open(os.path.join(logdir, "command.txt"), "w", encoding="utf-8").write(
        redact("#!/bin/sh\n" + " ".join(cmd) + "\n"))

    env = dict(os.environ)
    if IDF_PY310:
        env["IDF_PYTHON_ENV_PATH"] = IDF_PY310
    else:
        env.pop("IDF_PYTHON_ENV_PATH", None)
    eprint("启动     : " + " ".join("<token>" if c == token else c for c in cmd))
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=REPO, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=args.timeout)
    with open(os.path.join(logdir, "stdout.log"), "w", encoding="utf-8") as out:
        out.write(redact(proc.stdout))
    sys.stdout.write(proc.stdout)   # 终端保留真实值,便于就地排查
    dur = time.time() - t0

    report = None
    for line in proc.stdout.splitlines():
        if line.startswith(REPORT_LINE):
            try:
                report = json.loads(line[len(REPORT_LINE):])
            except json.JSONDecodeError:
                pass

    if report:
        # detail 文本与顶层 device 都含 IP —— 两处都要脱敏。
        for r in report.get("results", []):
            r["detail"] = redact(r.get("detail", ""))
        report["device"] = redact(report.get("device", ""))
        with open(os.path.join(logdir, "report.json"), "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        with open(os.path.join(logdir, "report.md"), "w", encoding="utf-8") as f:
            f.write(f"# Browser-module × Real-device Regression\n\n"
                    f"- device: `<device>` ({redact(report['device'])})\n"
                    f"- verdict: **{report['verdict']}** "
                    f"({sum(1 for r in report['results'] if r['ok'])}/{len(report['results'])} passed)\n"
                    f"- duration: {dur:.1f}s\n"
                    f"- device self-reboot observed: {report['rebooted']}\n\n"
                    f"| stage | check | result |\n|---|---|---|\n")
            for r in report["results"]:
                f.write(f"| {r['stage']} | {r['name']} | "
                        f"{'✅ PASS' if r['ok'] else '❌ FAIL'}"
                        f"{' — ' + r['detail'] if r['detail'] else ''} |\n")
        eprint(f"\nreport: {os.path.relpath(logdir, REPO)}/report.md ({dur:.1f}s)")

    eprint(f"退出码 {proc.returncode} — {'PASS' if proc.returncode == 0 else 'FAIL'}")
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
