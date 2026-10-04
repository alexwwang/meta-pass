#!/usr/bin/env python3
"""tools/realdevice/smoke.py —— 真机自动化冒烟(USB 串口 + LAN HTTP)。

用法:
  python3 tools/realdevice/smoke.py --ip 192.168.x.x \
      [--port /dev/cu.usbmodem142401] [--app build/FoloToy-AI-Passport.bin] \
      [--fresh] [--keep-monitor-log]

前置(一次性,人工):
  1. 设备已配网(NVS 存了 WiFi 凭证;--fresh 全擦后需重新配网)。
  2. 启动后人工进一次商店页(列表 DOWN 到 STORE → OK)——服务起来、
     "pair ready: code=… token=…" 打进串口(固件 2026-10-04 起)。
     之后的每次复位都由 inst_active 续连标志自动拉起服务,不再按键。

覆盖(对应设计验收清单):
  S1 配对:串口取配对码 → POST /api/install/pair 换 token
  S2 fresh 首装:节点侧跑真 dynslot-pool.js 算 carve 提案 → prepare
     (phone_picked 直确认)→ session → 4KB 分块上传 → finalize
     → 断言 done-reboot + 串口 "carve loaded: seq=1 slots=1"(方案B + 记录态启动)
  S3 二次安装(carve 第二个槽)→ slots count=2
  S4 删除归档 + 导出清单 + 删除擦除 + 导入恢复(备份闭环,设备侧全链)
  S5 中断续连:第三次安装传 30% 后掐断 → esptool 软复位(忠实等价断电:
     RAM 丢、flash 现状保留、NVS 续连标志在)→ 断言 "install resume"
     → 免重配对(新 token 从串口取)→ 幂等重发 prepare → 传完 → finalize
  S6 取证:read_flash 回读表区 + store 区存 logs/,字节级现场

串口所有权分段交替:monitor(idf.py)持口时 esptool 不可跑;esptool 前后
自动停/起 monitor。日志全程落 tools/realdevice/logs/smoke-<ts>.log。
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import urllib.error

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IDF = os.environ.get("IDF_PATH", os.path.expanduser("~/esp/esp-idf-v5.5.3"))
NODE = "/usr/local/bin/node"
LOGDIR = os.path.join(REPO, "tools", "realdevice", "logs")
CHUNK = 4096

PASS = "\033[32m✓\033[0m"
FAIL = "\033[31m✗\033[0m"


def sh(cmd, timeout=None, check=True):
    r = subprocess.run(["bash", "-lc", cmd], capture_output=True, text=True,
                       timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout[-800:]}\n{r.stderr[-800:]}")
    return r


class Monitor:
    """idf.py monitor 子进程:后台读流,wait_for 正则,落盘。"""

    def __init__(self, port, logfile):
        self.port = port
        self.logfile = logfile
        self.proc = None
        self.buf = ""
        self.lock = threading.Lock()
        self.reader = None
        self.fw = None

    def start(self):
        self.fw = open(self.logfile, "a", buffering=1)
        self.proc = subprocess.Popen(
            ["bash", "-lc", f"source {IDF}/export.sh >/dev/null 2>&1; "
                            f"idf.py -p {self.port} monitor --timestamps"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", preexec_fn=os.setsid)
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()
        time.sleep(1.0)

    def _pump(self):
        for line in self.proc.stdout:
            with self.lock:
                self.buf += line
            self.fw.write(line)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        if self.fw:
            self.fw.close()
        time.sleep(0.8)   # 让串口句柄释放

    def wait_for(self, pattern, timeout=60, last_n=20000):
        """返回第一个 match 对象;超时返回 None。只在新增缓冲里找。"""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline:
            with self.lock:
                chunk = self.buf[seen:]
                seen = len(self.buf)
            m = rx.search(chunk)
            if m:
                return m
            time.sleep(0.3)
        return None

    def tail(self, n=3000):
        with self.lock:
            return self.buf[-n:]


class Api:
    def __init__(self, ip, token=None):
        self.base = f"http://{ip}"
        self.token = token

    def _hdr(self, extra=None):
        h = {"Content-Type": "application/json"}
        if self.token:
            h["X-Meta-Session"] = self.token
        return h | (extra or {})

    def get(self, path):
        req = urllib.request.Request(self.base + path, headers=self._hdr())
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def post(self, path, body=b"", ctype="application/json"):
        req = urllib.request.Request(self.base + path, data=body,
                                     headers=self._hdr({"Content-Type": ctype}))
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def chunk(self, offset, data):
        req = urllib.request.Request(self.base + "/api/install/chunk", data=data,
                                     headers=self._hdr({
                                         "Content-Type": "application/octet-stream",
                                         "X-Meta-Offset": str(offset)}))
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()


def esptool(port, args, timeout=300):
    return sh(f"source {IDF}/export.sh >/dev/null 2>&1; "
              f"esptool.py -p {port} {args}", timeout=timeout)


def node_proposal(slots_json_text, image_len):
    """真 node + dynslot-pool.js 算提案(L4 单一事实源,不在 Python 里重写分配器)。"""
    script = f"""
import {{ geomFromListing }} from "{REPO}/install-slot/dynslot-pool.js";
const listing = JSON.parse(process.argv[1]);
const geom = geomFromListing(listing, {image_len});
console.log(JSON.stringify(geom?.proposal ?? null));
"""
    r = subprocess.run([NODE, "--input-type=module", "-e", script,
                        slots_json_text], capture_output=True, text=True,
                       cwd=REPO, timeout=30)
    if r.returncode != 0:
        raise RuntimeError(f"node proposal failed: {r.stderr[-400:]}")
    return json.loads(r.stdout.strip())


def stage(name):
    print(f"\n=== {name} ===")


def ok(msg):
    print(f"  {PASS} {msg}")


def die(msg, mon=None):
    print(f"  {FAIL} {msg}")
    if mon:
        print("  --- UART tail ---")
        print("\n".join("  " + l for l in mon.tail().splitlines()[-15:]))
    sys.exit(1)


def wait_boot_ok(mon, slots_want, timeout=90):
    """等一次正常启动并断言 carve 行;变砖特征(panic/反复 rst)直接判死。"""
    m = mon.wait_for(r"carve loaded: seq=(\d+) slots=(\d+)|fresh device.*no carve", timeout)
    if not m:
        if mon.wait_for(r"rst:0x|Guru Meditation|abort\(\)", 5):
            die("启动 panic/复位循环(记录态启动变砖?)", mon)
        die("等启动 carve 行超时", mon)
    if "fresh" in m.group(0):
        ok("启动:fresh device(无记录)")
    else:
        ok(f"启动:carve loaded seq={m.group(1)} slots={m.group(2)}")
    return int(m.group(2)) if m.group(2) else 0


def upload(api, app_bytes, total, desc):
    st, body = api.post("/api/install/session", json.dumps({
        "imageLen": total, "sha256": __import__("hashlib").sha256(app_bytes).hexdigest(),
        "slot": 0}).encode())
    if st != 200:
        die(f"{desc}: session {st} {body[:200]!r}")
    for off in range(0, total, CHUNK):
        st, body = api.chunk(off, app_bytes[off:off + CHUNK])
        if st != 200:
            die(f"{desc}: chunk@{off} {st} {body[:200]!r}")
    st, body = api.post("/api/install/finalize", b"{}")
    if st != 200:
        die(f"{desc}: finalize {st} {body[:300]!r}")
    ok(f"{desc}: finalize 200(设备 300ms 后复位)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", default="/dev/cu.usbmodem142401")
    ap.add_argument("--app", default=os.path.join(REPO, "build", "FoloToy-AI-Passport.bin"))
    ap.add_argument("--fresh", action="store_true",
                    help="全片擦除(会抹 WiFi 凭证,需重新配网)")
    args = ap.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    logfile = os.path.join(LOGDIR, f"smoke-{time.strftime('%Y%m%d-%H%M%S')}.log")
    app_bytes = open(args.app, "rb").read()
    app_len = len(app_bytes)
    print(f"app image {app_len}B | log {logfile}")

    mon = Monitor(args.port, logfile)

    # ── S0 烧录/复位 ────────────────────────────────────────────────────
    stage("S0 烧录与启动")
    mon.stop()
    if args.fresh:
        esptool(args.port, "erase_flash", timeout=180)
        full = os.path.join(REPO, "build", "FoloToy-AI-Passport-full.bin")
        esptool(args.port, f"write_flash 0x0 {full}", timeout=600)
        print("  已全片擦除+烧录。请配网,然后进一次商店页(DOWN→…→STORE→OK)。")
    else:
        # 清池 + store(NVS/otadata 保留): carved 表无记录 → hook 自动回安全表
        esptool(args.port, "erase_region 0x180000 0x680000", timeout=180)
        esptool(args.port, "erase_region 0x35A000 0x2000", timeout=60)
        print("  已清池+store(WiFi 凭证保留)。如设备未在商店页,请进一次。")
    mon.start()
    wait_boot_ok(mon, 0)
    m = mon.wait_for(r"pair ready: code=(\d{6}) token=([0-9a-f]{32})", 120)
    if not m:
        die("120s 内未见 pair ready —— 需要人工进一次商店页(见文件头说明)", mon)
    pair_code, token0 = m.group(1), m.group(2)
    ok(f"配对码 {pair_code}(串口捕获)")

    api = Api(args.ip)
    st, body = api.post("/api/install/pair", json.dumps({"code": pair_code}).encode())
    if st != 200:
        die(f"pair {st} {body[:200]!r}", mon)
    api.token = json.loads(body)["token"]
    ok("pair → token(一次性)")

    def slots():
        st, body = api.get("/api/install/slots")
        if st != 200:
            die(f"slots {st} {body[:200]!r}", mon)
        return json.loads(body)

    # ── S2 fresh 首装(carve 提案 → 上传 → finalize → 记录态重启)─────────
    stage("S2 fresh 首装(方案B + carve 提案)")
    s = slots()
    assert s["count"] == 0 and s.get("protocol_version") == 2, f"factory 态异常 {s}"
    ok(f"factory:count=0 protocol_version=2 free={s['free']}")
    proposal = node_proposal(json.dumps(s), app_len)
    if not proposal:
        die("node 提案为空(池放不下首个槽?)")
    manifest = {
        "protocol": 1, "playId": 1, "revisionId": 1, "name": "smoke-one",
        "storeSha256": "00" * 32, "imageLen": app_len,
        "sha256": __import__("hashlib").sha256(app_bytes).hexdigest(),
        "suggestedSlot": 0, "slot": proposal["slot"],
        "slots": [{"slot": 0, "limit": app_len, "fit": True}],
        "reason": "ok",
        "carveOffset": proposal["carveOffset"], "carveSize": proposal["carveSize"],
    }
    st, body = api.post("/api/install/prepare", json.dumps(manifest).encode())
    if st != 200:
        die(f"prepare#1 {st} {body[:300]!r}", mon)
    ok(f"prepare#1 200(carve idx={proposal['slot']} off=0x{proposal['carveOffset']:x})")
    upload(api, app_bytes, app_len, "install#1")
    n = wait_boot_ok(mon, 1)
    if n != 1:
        die(f"首装后 slots={n} != 1(变砖或 carve 未持久化)", mon)
    ok("首装后带记录重启:活")

    # ── S3 二次安装 ─────────────────────────────────────────────────────
    stage("S3 二次安装(第二个 carve 槽)")
    s = slots()
    proposal = node_proposal(json.dumps(s), app_len)
    manifest["slot"] = proposal["slot"]
    manifest["carveOffset"] = proposal["carveOffset"]
    manifest["carveSize"] = proposal["carveSize"]
    manifest["playId"] = manifest["revisionId"] = 2
    manifest["name"] = "smoke-two"
    st, body = api.post("/api/install/prepare", json.dumps(manifest).encode())
    if st != 200:
        die(f"prepare#2 {st} {body[:300]!r}", mon)
    ok("prepare#2 200")
    upload(api, app_bytes, app_len, "install#2")
    n = wait_boot_ok(mon, 2)
    if n != 2:
        die(f"二装后 slots={n} != 2", mon)
    ok("slots=2 持久")

    # ── S4 备份闭环:归档删除 → 导出清单 → 擦除删除 → 导入恢复 ──────────
    stage("S4 备份闭环(归档/擦除/导入)")
    st, _ = api.post("/api/install/remove", b'{"slot":0}')
    if st != 200:
        die(f"remove(归档) {st}", mon)
    wait_boot_ok(mon, 1)
    s = slots()
    archived = [d for d in s.get("data", []) if d.get("state") == 2 and d.get("play_id") == 1]
    if not archived:
        die(f"归档后无 ARCHIVED 记录 {s.get('data')}", mon)
    ok(f"归档:玩法1 数据 {len(archived)} 条进入 ARCHIVED(导出清单素材就位)")
    st, body = api.post("/api/install/remove", b'{"slot":1,"eraseData":true}')
    if st != 200:
        die(f"remove(擦除) {st}", mon)
    wait_boot_ok(mon, 0)
    s = slots()
    if any(d.get("play_id") == 2 for d in s.get("data", [])):
        die(f"擦除后玩法2 数据仍在 {s.get('data')}", mon)
    ok("擦除:玩法2 数据记录已清除")
    st, st_body = api.get("/api/install/status")
    fw = json.loads(st_body).get("firmware_version", "")
    st, body = api.post("/api/backup/import", json.dumps({
        "play_id": 1, "firmware_version": fw,
        "data": [{"offset": d["offset"], "size": d["size"],
                  "state": d["state"], "label": d["label"]} for d in archived],
    }).encode())
    if st != 200:
        die(f"import {st} {body[:300]!r}", mon)
    s = slots()
    if not any(d.get("play_id") == 1 for d in s.get("data", [])):
        die(f"导入后玩法1 记录未出现 {s.get('data')}", mon)
    ok("导入:归档记录已重挂(备份闭环,设备侧全链)")

    # ── S5 中断续连(软复位 = 断电的忠实近似)─────────────────────────────
    stage("S5 中断续连")
    s = slots()
    proposal = node_proposal(json.dumps(s), app_len)
    manifest["slot"] = proposal["slot"]
    manifest["carveOffset"] = proposal["carveOffset"]
    manifest["carveSize"] = proposal["carveSize"]
    manifest["playId"] = manifest["revisionId"] = 3
    manifest["name"] = "smoke-three"
    st, body = api.post("/api/install/prepare", json.dumps(manifest).encode())
    if st != 200:
        die(f"prepare#3 {st} {body[:300]!r}", mon)
    st, _ = api.post("/api/install/session", json.dumps({
        "imageLen": app_len, "sha256": __import__("hashlib").sha256(app_bytes).hexdigest(),
        "slot": proposal["slot"]}).encode())
    if st != 200:
        die("session#3 失败", mon)
    cut = app_len // 3
    for off in range(0, cut, CHUNK):
        api.chunk(off, app_bytes[off:off + CHUNK])
    ok(f"上传 {cut}B 后掐断 —— 软复位(等价断电:RAM 丢、flash 现状、NVS 保留)")
    mon.stop()
    esptool(args.port, "run", timeout=30)
    mon.start()
    m = mon.wait_for(r"install resume: restoring", 60)
    if not m:
        die("复位后未见 install resume(续连标志未生效?)", mon)
    ok("续连:自动恢复 WiFi + install 服务")
    m = mon.wait_for(r"pair ready: code=(\d{6}) token=([0-9a-f]{32})", 60)
    if not m:
        die("续连后未见新 token", mon)
    api.token = None
    st, body = api.post("/api/install/pair", json.dumps({"code": m.group(1)}).encode())
    api.token = json.loads(body)["token"]
    ok("新 token 获取(免重扫 QR,串口直取)")
    st, body = api.post("/api/install/prepare", json.dumps(manifest).encode())
    if st != 200:
        die(f"幂等重发 prepare {st} {body[:300]!r}", mon)
    ok("幂等重发 prepare 200(已 carve 槽复用)")
    upload(api, app_bytes, app_len, "install#3(续)")
    n = wait_boot_ok(mon, 1)
    s = slots()
    ok(f"续连完成:slots={s['count']}")

    # ── S6 取证 ─────────────────────────────────────────────────────────
    stage("S6 取证")
    mon.stop()
    evid = os.path.join(LOGDIR, time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(evid, exist_ok=True)
    esptool(args.port, f"read_flash 0x8000 0xC00 {evid}/table.bin", timeout=60)
    esptool(args.port, f"read_flash 0x35A000 0x2000 {evid}/store.bin", timeout=60)
    ok(f"表区 + store 区已回读 → {evid}/")

    print(f"\n{PASS} 全部阶段通过。日志:{logfile}")
    print("人工抽验(2 分钟):列表导航/OK 启动玩法/彩蛋 —— 自动化不覆盖物理按键。")


if __name__ == "__main__":
    main()
