#!/usr/bin/env python3
"""tools/pages-deploy.py — Pages 直接部署(绕过 wrangler)。

背景:本机 wrangler 上传时 undici 经当前网络 POST /pages/assets/upload 会
静默挂起(bun/node 均复现),python requests 正常。本脚本按 wrangler
4.145 源码里的协议逐步复刻(见 wrangler-dist/cli.js upload/deployProject):

  hash = blake3(b64(content) + ext).hex()[:32]
  1. GET  /accounts/{a}/pages/projects/{p}/upload-token          -> {jwt}
  2. POST /pages/assets/check-missing  (Bearer jwt)              -> 缺失 hash
  3. POST /pages/assets/upload         (Bearer jwt)              -> 补传(base64)
  4. POST /pages/assets/upsert-hashes  (Bearer jwt)              -> 注册 hash
     ^^ 漏这步 = 资产在库但部署解析全 404(实测踩坑)
  5. POST /accounts/{a}/pages/projects/{p}/deployments (multipart):
     manifest={"/path":"<hash>"}(纯字符串值,键 "/" 前缀 —— {hash,size} 对象
     与双斜杠键都是踩过的坑)、branch、commit_*、_worker.bundle(内层
     multipart:metadata + 部件;部件 Content-Type 必须
     application/javascript+module,否则 worker 被当 classic;worker 先经
     bun build 内联相对 import)

用法:
  python3 tools/pages-deploy.py                # staging install-slot 并部署到生产
  python3 tools/pages-deploy.py --dry-run      # 只构建 manifest,不部署
  python3 tools/pages-deploy.py --dir <path>   # 部署指定目录(需已打好版本戳)

注意:生产 Worker 与 main 分支的偏差见 docs/assets/mota-implementation-audit.md
部署说明 —— push main 前先把 feat/mota 合并,否则 CI 会用旧代码覆盖本部署。
"""
import argparse
import base64
import io
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

import requests

ACCOUNT = "5dd115ded7763e7a82a4e5c7482aec79"
PROJECT = "meta-pass"
API = "https://api.cloudflare.com/client/v4"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 仓库根

WRANGLER_CONFIG = os.path.expanduser("~/Library/Preferences/.wrangler/config/default.toml")


def oauth_token():
    if not os.path.exists(WRANGLER_CONFIG):
        sys.exit(f"wrangler 配置不存在: {WRANGLER_CONFIG}(先跑一次 wrangler login)")
    m = re.search(rb'oauth_token = "([^"]+)"', open(WRANGLER_CONFIG, "rb").read())
    if not m:
        sys.exit("default.toml 里找不到 oauth_token")
    return m.group(1).decode()


def stage_install_slot():
    """install-slot/ → 临时副本,__PAGE_VERSION__ 戳为 git 短 SHA(同 CI)。"""
    dst = tempfile.mkdtemp(prefix="pages-deploy-")
    shutil.rmtree(dst)
    shutil.copytree(os.path.join(ROOT, "install-slot"), dst)
    sha = subprocess.run(["git", "-C", ROOT, "-c", "safe.directory=*",
                          "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True, check=True).stdout.strip()
    html = os.path.join(dst, "install-slot.html")
    s = open(html, encoding="utf-8").read()
    s = s.replace("__PAGE_VERSION__", sha)
    open(html, "w", encoding="utf-8").write(s)
    print(f"staged install-slot @ {sha} → {dst}")
    return dst


def build_manifest(directory):
    """{"/path": {"hash","size"}} + hash→abs path 反查表。"""
    import blake3  # 延迟导入:--dry-run 也需要,缺失时报错更友好
    manifest, filemap = {}, {}
    for root, _dirs, files in os.walk(directory):
        for f in sorted(files):
            ap = os.path.join(root, f)
            rel = os.path.relpath(ap, directory).replace(os.sep, "/")
            if rel.startswith(".") or "/." in rel:
                continue
            key = "/" + rel   # wrangler normalizeFilePath:必须 "/" 前缀
            content = open(ap, "rb").read()
            ext = os.path.splitext(f)[1][1:]
            h = blake3.blake3(base64.b64encode(content) + ext.encode()).hexdigest()[:32]
            manifest[key] = {"hash": h, "size": len(content)}
            filemap[h] = ap
    return manifest, filemap


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", help="部署指定目录(默认临时 staging install-slot/)")
    ap.add_argument("--dry-run", action="store_true", help="只构建 manifest,不部署")
    args = ap.parse_args()

    directory = args.dir or stage_install_slot()
    manifest, filemap = build_manifest(directory)
    print(f"manifest: {len(manifest)} files")
    if args.dry_run:
        for k in sorted(manifest):
            print(f"  {k}  {manifest[k]['hash']}  {manifest[k]['size']}B")
        return

    H = {"Authorization": f"Bearer {oauth_token()}"}
    r = requests.get(f"{API}/accounts/{ACCOUNT}/pages/projects/{PROJECT}/upload-token",
                     headers=H, timeout=60)
    r.raise_for_status()
    jwt = r.json()["result"]["jwt"]
    JH = {"Authorization": f"Bearer {jwt}", "Content-Type": "application/json"}

    hashes = [v["hash"] for v in manifest.values()]
    r = requests.post(f"{API}/pages/assets/check-missing", headers=JH,
                      data=json.dumps({"hashes": hashes}), timeout=120)
    r.raise_for_status()
    missing = r.json()["result"]
    print(f"missing assets: {len(missing)}")
    if missing:
        payload = []
        for h in missing:
            fp = filemap[h]
            ctype = mimetypes.guess_type(fp)[0] or "application/octet-stream"
            if ctype.startswith("text/") and "charset" not in ctype:
                ctype += "; charset=utf-8"   # wrangler getContentType 同款
            payload.append({
                "key": h,
                "value": base64.b64encode(open(fp, "rb").read()).decode(),
                "metadata": {"contentType": ctype},
                "base64": True,
            })
        r = requests.post(f"{API}/pages/assets/upload", headers=JH,
                          data=json.dumps(payload), timeout=300)
        r.raise_for_status()
        print("assets uploaded")

    # 注册 hash:漏这步,部署 stage=success 但 ASSETS 全 404(实测踩坑)。
    r = requests.post(f"{API}/pages/assets/upsert-hashes", headers=JH,
                      data=json.dumps({"hashes": hashes}), timeout=120)
    r.raise_for_status()
    print(f"upserted {len(hashes)} hashes")

    # worker:bun build 内联相对 import(bundle 后 0 残留 import),再包成
    # 内层 multipart(部件 Content-Type = application/javascript+module,否则
    # API 把 ESM 当 classic → "Cannot use import statement outside a module")。
    bundle_path = os.path.join(tempfile.gettempdir(), "pages-worker-bundled.mjs")
    subprocess.run(["bun", "build", os.path.join(directory, "_worker.js"),
                    "--format=esm", f"--outfile={bundle_path}"],
                   check=True, capture_output=True)
    worker_src = open(bundle_path, "rb").read()

    b_in = uuid.uuid4().hex
    buf = io.BytesIO()

    def part(name, data, filename=None, ctype=None):
        buf.write(f"--{b_in}\r\n".encode())
        disp = f'form-data; name="{name}"'
        if filename:
            disp += f'; filename="{filename}"'
        buf.write(f"Content-Disposition: {disp}\r\n".encode())
        if ctype:
            buf.write(f"Content-Type: {ctype}\r\n".encode())
        buf.write(b"\r\n")
        buf.write(data if isinstance(data, bytes) else data.encode())
        buf.write(b"\r\n")

    part("metadata", json.dumps({"main_module": "_worker.js"}))
    part("_worker.js", worker_src, filename="_worker.js",
         ctype="application/javascript+module")
    buf.write(f"--{b_in}--\r\n".encode())
    worker_bundle = buf.getvalue()

    commit = subprocess.run(["git", "-C", ROOT, "-c", "safe.directory=*",
                             "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True, check=True).stdout.strip()
    outer = {
        "manifest": (None, json.dumps({k: v["hash"] for k, v in manifest.items()}),
                     "application/json"),
        "branch": (None, "main"),
        "commit_message": (None, f"pages-deploy.py {commit}"),
        "commit_hash": (None, commit),
        "commit_dirty": (None, "true"),
        "_worker.bundle": ("_worker.bundle", worker_bundle, "multipart/form-data"),
    }
    r = requests.post(f"{API}/accounts/{ACCOUNT}/pages/projects/{PROJECT}/deployments",
                      headers=H, files=outer, timeout=300)
    if not r.ok:
        sys.exit(f"DEPLOY FAILED: {r.status_code} {r.text[:800]}")
    res = r.json()["result"]
    print("deployment:", res.get("id"), "| env:", res.get("environment"),
          "|", res.get("url"))
    print("验证: curl -sI https://metapass.chuanxilu.net/phone-install.js")


if __name__ == "__main__":
    main()
