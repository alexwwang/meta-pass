// install-slot/capture-upload.js —— pass-radar 独立模式上行模块(方案 A 独立通道)。
//
// 部署形态:与 phone-install.js 同构 —— pass-radar 固件的设备 boot 页
// (radar_upload.c SHELL_HTML)以 <script type=module src=…/capture-upload.js>
// 加载,模块在设备源(192.168.4.1)上运行,相对 fetch 直达设备迷你服务:
//   GET /api/capture/info        批数据区概览(容量/批次/条数/设备哈希)
//   GET /api/capture/read?offset&len  原始字节块(4096B,越界 400)
// 拉全量后解析 RCB1 头部(与固件 radar_capture_batch.h v6 布局对拍:magic@0,
// batch@8,expected@12,committed@16,device_hash@28→hex32),再 POST
// https://metapass.chuanxilu.net/api/capture/ingest?batch=(device 由服务端
// 从头部解析,客户端不能冒充)。单文件、零依赖、no-store(见 _worker.js)。

const INGEST_BASE = "https://metapass.chuanxilu.net";
const CHUNK = 4096;

const esc = (s) => String(s).replace(/[&<>"']/g,
  (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

function parseHeader(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength < 128 ||
      bytes[0] !== 0x52 || bytes[1] !== 0x43 || bytes[2] !== 0x42 || bytes[3] !== 0x31) {
    return null;
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let device = "";
  for (let i = 28; i < 44; i++) device += bytes[i].toString(16).padStart(2, "0");
  return {
    version: view.getUint16(4, true),
    batch: view.getUint32(8, true),
    expected: view.getUint32(12, true),
    committed: view.getUint32(16, true),
    device,
  };
}

export async function fetchCaptureInfo(base) {
  const res = await fetch(base + "/api/capture/info");
  if (!res.ok) throw new Error("info HTTP " + res.status);
  return res.json();
}

export async function fetchCaptureBytes(base, size, onProgress = () => {}) {
  if (!Number.isSafeInteger(size) || size <= 0 || size > 32 * 1024 * 1024) {
    throw new Error("bad size " + size);
  }
  const bytes = new Uint8Array(size);
  for (let offset = 0; offset < size; offset += CHUNK) {
    const len = Math.min(CHUNK, size - offset);
    const res = await fetch(base + "/api/capture/read?offset=" + offset + "&len=" + len);
    if (!res.ok) throw new Error("read HTTP " + res.status + " @0x" + offset.toString(16));
    const buf = new Uint8Array(await res.arrayBuffer());
    if (buf.byteLength !== len) throw new Error("short read @0x" + offset.toString(16));
    bytes.set(buf, offset);
    onProgress({ completed: offset + len, total: size });
  }
  return bytes;
}

export async function uploadCaptureBytes(bytes, batch) {
  const res = await fetch(INGEST_BASE + "/api/capture/ingest?batch=" + batch, {
    method: "POST",
    headers: { "content-type": "application/octet-stream" },
    body: bytes,
  });
  const text = await res.text();
  let payload = null;
  try { payload = JSON.parse(text); } catch { /* 非 JSON 错误体 */ }
  if (!res.ok || !payload || !payload.ok) {
    throw new Error("ingest HTTP " + res.status + ":" +
      (payload && payload.error ? payload.error : text.slice(0, 120)));
  }
  return payload;
}

// ── 设备身份(契约 v1:MAC 派生 Ed25519)────────────────────────────
// 固件 GET /api/identity → {"device":hex64, "pubkey":hex64, "kind":"mac-ed25519"}
// (radar_identity_get,MAC 派生永缓存)。注册 = POST 云端 /api/capture/enroll
// (first-wins:重复注册 409 视为"已注册"成功语义)。
export async function fetchDeviceIdentity(base) {
  const res = await fetch(base + "/api/identity");
  if (!res.ok) throw new Error("identity HTTP " + res.status);
  const id = await res.json();
  if (!id || typeof id.device !== "string" || typeof id.pubkey !== "string") {
    throw new Error("identity 响应缺 device/pubkey");
  }
  return id;
}

export async function enrollDeviceIdentity(device, pubkey) {
  const res = await fetch(INGEST_BASE + "/api/capture/enroll", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ device, pubkey }),
  });
  const text = await res.text();
  let payload = null;
  try { payload = JSON.parse(text); } catch { /* 非 JSON 错误体 */ }
  if (res.status === 409) return { enrolled: true, duplicate: true, payload };
  if (!res.ok || !payload || !payload.ok) {
    throw new Error("enroll HTTP " + res.status + ":" +
      (payload && payload.error ? payload.error : text.slice(0, 120)));
  }
  return { enrolled: true, duplicate: false, payload };
}

// ── UI ─────────────────────────────────────────────────────────────
function mount() {
  const host = document.getElementById("cu-root");
  if (!host) return;
  // 设备源直开:location 即设备;CF 源打开(调试):手输设备地址。
  const onDevice = /^\d+\.\d+\.\d+\.\d+$/.test(location.hostname) ||
                   location.hostname === "localhost";
  const base = onDevice ? location.origin : "";
  host.innerHTML = `
    <style>
      #cu-root{font:15px/1.6 system-ui,sans-serif;max-width:560px;margin:16px auto;padding:0 14px;color:#1f1f1f}
      #cu-root .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:10px 0}
      #cu-root input{flex:1;min-width:180px;padding:8px;border:1px solid #bbb;border-radius:8px}
      #cu-root button{padding:9px 16px;border:0;border-radius:8px;background:#0b57d0;color:#fff;font-size:15px}
      #cu-root button:disabled{background:#9e9e9e}
      #cu-root button.secondary{background:#fff;color:#0b57d0;border:1px solid #0b57d0}
      #cu-root .info{background:#f1f3f4;border-radius:8px;padding:10px 12px;margin:10px 0;white-space:pre-wrap;word-break:break-all}
      #cu-root .err{color:#b3261e}#cu-root .ok{color:#137333}
      #cu-root .bar{height:8px;background:#e1e3e6;border-radius:4px;overflow:hidden}
      #cu-root .bar>i{display:block;height:100%;width:0;background:#0b57d0}
      #cu-root .card{border:1px solid #e1e3e6;border-radius:12px;padding:12px 14px;margin:12px 0}
      #cu-root .step{display:flex;gap:10px;align-items:flex-start;margin:14px 0}
      #cu-root .step>b{flex:0 0 24px;height:24px;border-radius:50%;background:#0b57d0;color:#fff;
        display:flex;align-items:center;justify-content:center;font-size:14px}
      #cu-root .step>div{flex:1}
      #cu-root h3{margin:2px 0 6px;font-size:16px}
      #cu-root .muted{color:#5f6368;font-size:13px}
      #cu-root .mono{font-family:ui-monospace,monospace;font-size:12px}
      #cu-root .verified{background:#e6f4ea;border:1px solid #137333;color:#137333}
    </style>
    ${onDevice ? "" : `<div class=row><input id=cu-dev placeholder="设备地址,如 http://192.168.4.1"><button id=cu-go>连接</button></div>`}
    <div id=cu-msg class=info>正在读取批次信息…</div>
    <div id=cu-detail hidden></div>

    <div class=step>
      <b>⤒</b>
      <div>
        <h3>拉取并上传</h3>
        <div class=muted>从设备读完整批次(带设备签名),传到云端。无需任何注册或配置。</div>
        <div class=row><button id=cu-up hidden>拉取并上传</button></div>
        <div class=bar id=cu-bar hidden><i></i></div>
      </div>
    </div>`;
  const $ = (id) => host.querySelector("#" + id);
  const msg = (t, cls = "") => {
    const n = $("cu-msg");
    n.className = "info " + cls;
    n.textContent = t;
  };
  let info = null;
  let bytes = null;

  async function loadInfo() {
    const b = onDevice ? base : ($("cu-dev").value || "").trim().replace(/\/$/, "");
    if (!b) { msg("请填写设备地址", "err"); return; }
    try {
      info = await fetchCaptureInfo(b);
      if (!info.magic_ok) {
        msg("数据区没有 RCB1 批次(尚未采集或固件版本旧)。", "err");
        return;
      }
      const records = Number.isFinite(info.records) ? info.records : null;
      $("cu-detail").hidden = false;
      $("cu-detail").className = "info";
      $("cu-detail").textContent =
        `批次 ${info.batch}(v${info.version})  ${info.committed} 次采集` +
        (records != null ? ` · ${records} 条记录` : "") + `\n` +
        `数据区 ${(info.size / 1024).toFixed(0)} KiB  设备 ${info.device.slice(0, 16)}…`;
      $("cu-up").hidden = false;
      $("cu-up").onclick = () => upload(b);
      msg("就绪:点「拉取并上传」。", "");
    } catch (e) {
      msg("连接失败:" + (e && e.message ? e.message : e), "err");
    }
  }

  async function upload(b) {
    $("cu-up").disabled = true;
    $("cu-bar").hidden = false;
    try {
      bytes = await fetchCaptureBytes(b, info.size, (p) => {
        $("cu-bar").firstElementChild.style.width =
          Math.floor(p.completed * 100 / p.total) + "%";
        msg("拉取中 " + Math.floor(p.completed * 100 / p.total) + "%");
      });
      const head = parseHeader(bytes);
      if (!head || String(head.batch) !== String(info.batch)) {
        throw new Error("头部解析与 info 不符,数据损坏");
      }
      msg("拉取完成,批次 " + head.batch + ",正在上传…");
      const payload = await uploadCaptureBytes(bytes, head.batch);
      msg("已上传:" + payload.bytes + " 字节,sha256 " + payload.sha256.slice(0, 16) +
          "…," + payload.ingestedAt +
          (payload.verified ? ",签名已验证 ✓" : ""), "ok");
    } catch (e) {
      msg("上传失败:" + (e && e.message ? e.message : e), "err");
    } finally {
      $("cu-up").disabled = false;
    }
  }

  if (onDevice) loadInfo();
  else $("cu-go").onclick = loadInfo;
}

if (typeof document !== "undefined") {
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", mount);
  } else {
    mount();
  }
}
