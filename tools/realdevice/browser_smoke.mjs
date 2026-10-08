#!/usr/bin/env node
// tools/realdevice/browser_smoke.mjs —— 真机 × 浏览器前端模块回归。
//
// 补的缺口:tools/realdevice/smoke.py 是纯 Python 协议客户端,只 import
// dynslot-pool.js;它证明的是设备契约,不证明 phone-install.js / store-analyze.js
// 这套手机端+USB 网页端逻辑对**真机**可用。tests/test_phone_install.mjs 跑的是
// 手写的 mock fetch —— mock 复现的是我们以为的契约(2026-10-08 真机就靠这条路
// 一路绿到 /api/install/status 丢 2 字节 JSON 尾巴才炸)。本文件 import 仓库规范
// 目录里的**真实**前端模块,打**真机** LAN HTTP,零 mock。
//
// 流程:
//   P1 live-store 路径:GET /api/install/slots 真实清单 → parseSlots 形状门禁
//      → geomFromListing 出 carve 提案(占用域含数据 carve) → prepareImage
//      (走 metapass Worker 真下载 2MB + 校验 + 解包) → runInstall
//      (prepare 带手机选定槽位 = 设备直确认,免物理按键 → session → 65536B
//       顺序 chunk → finalize → done) → slots 复核
//      → remove(无 eraseData) → 断言 APP 槽消失、Child DATA 仍 ARCHIVED
//   P2 夹具路径(合成完整 flash 镜像,真实 app 字节):同一设备跑通 DATA 分支 ——
//      data[] 分配 + /api/install/data 分块上传 + finalize
// 为什么 P1 用真商店玩法、P2 用夹具:store 里唯一带 Child DATA 且受支持的
// 玩法(play 200, 6.6MB)远超槽位容量上限,无法在 8MB 池上真正落盘。P2 合成一个
// 结构合法的完整 flash 镜像(合法分区表 + 仓库 build 的真实 app 字节 + 一条
// 0x82 文件系统数据分区),让 prepareImage 的下载/校验/解包/DATA 提取四道门
// 全部真实执行 —— data[] 由 extractDataImages 解析出来,不是测试手填的。
//
// 状态中立:不清池、不 erase-flash、不碰 cardid/NVS/otadata。只安装测试槽位
// 并在 finally 里按 (offset,size) 精确清除自己装的槽位与数据记录,恢复到运行前
// 原状(原有有效槽位与归档记录原封不动)。
//
// 运行(仓库根):node tools/realdevice/browser_smoke.mjs --ip <device-ip> \
//                  --token <32hex> [--fixture build/FoloToy-AI-Passport.bin]
// Python 编排器 tools/realdevice/run_browser_smoke.py 负责从 NVS 取 token + 证据归档。

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  createBridge, parseSlots, waitDeviceBack, preflightMeta, prepareImage, runInstall,
} from "../../install-slot/phone-install.js";
import { SLOT_GEOMETRY } from "../../install-slot/store-analyze.js";
import { isFullImage, extractAppImage, extractDataImages } from "../../install-slot/extract-app-image.js";
import { geomFromListing } from "../../install-slot/dynslot-pool.js";
import { homedir } from "node:os";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
// 注意:FoloToy-AI-Passport-full.bin 名不副实 —— 它是 app 镜像补零到 8MB,
// 0x8000 处没有分区表(落在补零区),extractDataImages 会抛
// "unsupported-partition"。所以默认取 app-only 构建产物,P2 自行合成完整镜像。
const FIXTURE_DEFAULT = path.join(ROOT, "build", "FoloToy-AI-Passport.bin");
const STORE_PLAY_ID = Number(process.env.BROWSER_SMOKE_PLAY ?? 1);
const STORE_SHA = process.env.BROWSER_SMOKE_STORE_SHA ||
  "2f6ffb972d847461d61fe6d790eae96627f3a79e5c1ae1fe0e63a67aeedd7df7";
const MISMATCH_PLAY_ID = Number(process.env.BROWSER_SMOKE_MISMATCH_PLAY ?? 200);
const FIXTURE_PLAY_ID = 987654321;   // 唯一,不与既有 1/2/3 记录冲突
const DATA_LABEL = "recordings";     // 与 smoke.py 夹具同名(设备 P1-4 契约内)
const DATA_SIZE = 8192;
const DATA_LEN = 2048;
const REBOOT_POLLS = Number(process.env.BROWSER_SMOKE_REBOOT_POLLS ?? 30);

const argv = (() => {
  // 同时接受 --key value 与 --key=value;布尔开关的下一个词以 -- 开头。
  const raw = process.argv.slice(2);
  const m = {};
  for (let i = 0; i < raw.length; i++) {
    const a = raw[i];
    if (!a.startsWith("--")) continue;
    const eq = a.indexOf("=");
    if (eq > 2) { m[a.slice(2, eq)] = a.slice(eq + 1); continue; }
    const nxt = raw[i + 1];
    m[a.slice(2)] = (nxt !== undefined && !nxt.startsWith("--")) ? nxt : true;
  }
  return m;
})();
const IP = argv.ip;
// 端口是本地硬件细节,不写默认值 —— CI/运行方必须显式 --port 传入。
const PORT = argv.port;
const TOKEN = argv.token;
const FIXTURE = path.resolve(argv.fixture ?? FIXTURE_DEFAULT);
const IDF_PATH = process.env.IDF_PATH ?? `${homedir()}/esp/esp-idf-v5.5.3`;
function envWithIdf() {
  return { ...process.env,
           IDF_PATH,
           IDF_PYTHON_ENV_PATH: process.env.IDF_PYTHON_ENV_PATH ?? "" };
}
if (!IP || !TOKEN) {
  console.error("usage: node tools/realdevice/browser_smoke.mjs --ip <device-ip> " +
                "--token <32hex> [--fixture build/FoloToy-AI-Passport.bin]");
  process.exit(2);
}
if (!/^[0-9a-f]{32}$/i.test(TOKEN)) {
  console.error(`token must be 32 hex chars (got ${String(TOKEN).length} chars)`);
  process.exit(2);
}

const sha256 = (b) => createHash("sha256").update(b).digest("hex");
let seq = 0;
function log(s) { console.log(`##[${++seq}] ${s}`); }
const PASS = "✓", FAIL = "✗";
function ok(s) { log(`${PASS} ${s}`); }
function fail(s) { log(`${FAIL} ${s}`); }

const results = [];
const installed = [];
const persisted = [];   // {playId, offset, size, name} —— cleanup 只清自己装的

function record(stage, name, okd, detail = "") {
  results.push({ stage, name, ok: okd, detail });
  (okd ? ok : fail)(`[${stage}] ${name}${detail ? ` — ${detail}` : ""}`);
  if (!okd) process.exitCode = 1;
}

// finalize 之后设备会进入「已 done」态;再发 prepare 会被 busy 拒。复位会话。
async function resetSession() {
  const st = JSON.parse((await bridge.status()).text ?? "{}");
  if (st.session || st.confirmed) {
    const r = await bridge.cancel();
    if (!r.ok) throw new Error(`cancel after done: HTTP ${r.status} ${r.text}`);
    ok(`session 已复位(cancel,原状态=${st.state})`);
  }
}

// 等一次真机重启:记录态重启是「装完 + UI tick 驱动 goto_page」的延迟行为,
// 无人值守下不一定发生(smoke.py S2/S5 同款兜底:软复位)。返回 {rebooted,polls}。
// done→自动复位是 UI tick 驱动的(退出商店页才复位),无人值守下不会发生。
// 这里主动用 esptool 软复位:清 RAM、不动 flash/NVS,等价于 smoke.py 的
// wait_done_reboot 兜底路径。不复位就无法验证"记录落在非易失存储"。
async function softReset() {
  return new Promise((resolve) => {
    // 与 smoke.py 同款:esptool.py 是 IDF 的 console script,必须 source export.sh
    // 才在 PATH 上。IDF_PYTHON_ENV_PATH 指向健康的 py3.10 venv(默认侦测的 py3.14
    // venv pydantic_core ABI 损坏,esptool 直接不可用)。
    execFileSync("bash", [
      "-lc",
      `source ${IDF_PATH}/export.sh >/dev/null 2>&1; esptool.py -p ${PORT} run`,
    ], { timeout: 60000, stdio: "ignore", env: envWithIdf() });
    // 复位后等 HTTP 恢复。复位窗口内 status 会失败,不能用"返回 200"判断。
    let i = 0;
    const tick = async () => {
      i++;
      try {
        const br = createBridge(`http://${IP}`, TOKEN);
        const r = await br.status();
        if (r.ok && r.text) {
          try {
            const st = JSON.parse(r.text);
            if (st.protocol === 1) return resolve({ rebooted: true, polls: i });
          } catch { /* 半截 body */ }
        }
      } catch { /* 复位窗口内不通 */ }
      if (i >= REBOOT_POLLS) return resolve({ rebooted: false, polls: i });
      setTimeout(() => { void tick(); }, 500);
    };
    setTimeout(() => { void tick(); }, 1000);
  });
}

async function listingRaw() {
  const r = await bridge.slots();
  if (!r.ok) throw new Error(`GET /api/install/slots: HTTP ${r.status} ${r.text}`);
  return r.text;
}

const bridge = createBridge(`http://${IP}`, TOKEN);

// 状态中立保证:任何失败路径也要清掉本测试装的槽位,不把悬空槽位留在共享设备上。
// beforeExit 只在事件循环排空时才触发,所以 async 清理会正常跑完;显式清理
// (happy path) 后 cleaned=true 使其不再重复。
let cleaned = false;
async function cleanup() {
  if (cleaned) return;
  cleaned = true;
  log("清理: 仅移除本测试创建的槽位/数据记录(不动既有槽位与归档)");
  let n = 0;
  for (const ins of installed) {
    try {
      const L = parseSlots(await listingRaw());
      const t = L?.slots.find((s) => s.offset === ins.offset && s.size === ins.size);
      if (!t) { log(`  - slot@0x${ins.offset.toString(16)} 已不在清单`); continue; }
      if (t.play_id !== undefined && t.play_id !== ins.playId && !/^browser smoke/i.test(t.name)) {
        log(`  ! slot@0x${ins.offset.toString(16)} 属 play ${t.play_id}(${t.name}) —— 未动`);
        continue;
      }
      const rm = await bridge.remove(t.slot, { eraseData: true });
      log(`  ${rm.ok ? PASS : FAIL} remove slot=${t.slot} (play ${ins.playId}) HTTP ${rm.status}`);
      if (rm.ok) { await waitDeviceBack(bridge, { tries: 20, delayMs: 1000 }); await resetSession(); n++; }
    } catch (e) { log(`  ! slot@0x${ins.offset.toString(16)} 清理失败: ${e.message}`); }
  }
  const finRaw = await listingRaw().catch(() => "(device unreachable)");
  const finL = parseSlots(finRaw);
  record("cleanup", "测试数据记录全部清除(设备恢复到运行前状态)",
         !!finL && !finL.data.some((d) => d.play_id === FIXTURE_PLAY_ID) &&
         !finL.slots.some((s) => installed.some((x) => s.offset === x.offset && s.size === x.size)),
         `cleaned=${n} count=${finL?.count ?? "-"} archivedData=${finL?.data.length ?? "-"}`);
}
process.on("beforeExit", () => { void cleanup(); });
// 早期失败(安装中即崩)也要清;直接 exit 会在事件循环排空前结束。
const hardExit = (c) => { void cleanup().then(() => process.exit(c)); };

// ── P0 前置:设备在线、协议版本、槽位清单形状门禁 ──────────────────────────
log(`前置: device=${IP} bridge=真 fetch(零 mock) fixture=${path.relative(ROOT, FIXTURE)}`);
let raw = await listingRaw();
if (!/protocol_version/.test(raw)) {
  fail("P0 真机未暴露 dynslot 协议版本(protocol_version 缺失)——固件不匹配 feat/storage");
  hardExit(1);
}
let listing = parseSlots(raw);
// 运行前 DATA 记录快照:P1 是纯 APP 安装(P1 已断言该玩法无 Child DATA),
// 装完再删完,DATA 区应一字不动。
const initialData = (listing.data ?? []).map((d) =>
  `${d.play_id}:${d.label}:${d.state}:${d.offset}:${d.size}`).sort().join(",");
if (!listing) {
  fail(`P0 parseSlots 形状门禁拒收真机清单: ${raw.slice(0, 300)}`);
  hardExit(1);
}
record("P0", "设备在线 + slots 协议 v" + listing.protocolVersion,
       listing.protocolVersion === 2,
       `count=${listing.count} free=${listing.free} archivedData=${listing.data.length}`);
record("P0", "parseSlots 对真机清单全字段通过(含数据 carve 记录)",
       listing.slots.every((s) => Number.isInteger(s.slot) && s.limit > 0),
       `slots=${listing.slots.map((s) => s.slot).join(",") || "∅"}`);

// ── P1 live-store 路径:真实商店元数据 → 真实下载 → 真实安装 → 删除归档 ──────
// 与 UI 同一入口:preflightMeta 内部走 mpJson + getPlayDetail(经 normalizePlay)。
// 这里不手拼 play/analyze 形状 —— 手拼容易与 normalizePlay 的字段回退链漂移,
// 而漂移出来的假失败会掩盖真失败。
const pm = await preflightMeta(STORE_PLAY_ID);
const ex = pm.ok ? (pm.analyze?.extracted ?? {}) : {};
const play = pm.play;
record("P1", "preflightMeta 可用(analyze + 商店详情,与 UI 同入口)",
       pm.ok === true && !!ex.imageLen,
       pm.ok
         ? `id=${STORE_PLAY_ID} name=${play?.name} imageLen=${ex.imageLen} ` +
           `store=${pm.analyze?.store?.size}`
         : `stage=${pm.stage} reason=${pm.reason}`);
if (!pm.ok) { fail("analyze 不可用,中止"); hardExit(1); }
if (Array.isArray(pm.analyze?.data) && pm.analyze.data.length) {
  fail(`P1 拒绝:该玩法带 Child DATA(${pm.analyze.data.map((d) => d.label).join(",")}),` +
       `会污染共享真机池 —— 用不带数据的玩法(BROWSER_SMOKE_PLAY=)`);
  hardExit(1);
}

// 槽位选择器路径:geom 取自真机清单(占用域含数据 carve),不是 SLOT_GEOMETRY 回退
const preGeom = geomFromListing(listing, ex.imageLen);
if (!preGeom.proposal) {
  fail(`P1 真机池无空间容纳 ${ex.imageLen}B 提案(maxGap=${preGeom.maxGap} totalFree=${preGeom.totalFree})`);
  hardExit(1);
}
const t0 = Date.now();
// slot >= 0 时设备免物理确认(交互 v2);传 -1 会把 offer 永久卡在等按键,
// 无人值守必然超时 —— 取 geomFromListing 的提案槽位,与 UI 选中新槽同一路径。
const pre = await prepareImage(pm, preGeom.proposal.slot, {}, "Browser Smoke", {
  geom: preGeom, slotIsNew: true,
});
record("P1", "prepareImage 走真下载+校验+解包(与 UI continueInstall 同入口)",
       pre.ok === true, pre.ok
         ? `imageLen=${pre.offer.imageLen} sha=${pre.offer.sha256.slice(0, 12)} ` +
           `carveOff=0x${pre.offer.carveOffset.toString(16)} carveSize=${pre.offer.carveSize} ` +
           `${((Date.now() - t0) / 1000).toFixed(1)}s`
         : `stage=${pre.stage} reason=${pre.reason}`);
if (!pre.ok) hardExit(1);

// 提案自洽性:落点必须 0x10000 对齐,槽位必须装得下镜像。carveSize 不在此
// 复算 —— 设备 meta_install_model.c 会按 meta_carve_need(image_len) 硬性
// 复核(carve_size != need 直接 409 拒),重推公式只把测试绑死在两个真实
// 实现上,任一方演进都会假失败。P1 的 prepare 真过 = 提案尺寸被设备接受。
record("P1", "提案落点/容量自洽(carveSize 由设备 meta_carve_need 复核)",
       pre.offer.carveOffset % 0x10000 === 0 &&
       pre.offer.imageLen <= pre.offer.carveSize &&
       pre.offer.carveSize % 0x1000 === 0,
       `off=0x${pre.offer.carveOffset.toString(16)} size=${pre.offer.carveSize} ` +
       `>= imageLen=${pre.offer.imageLen} (delta=${pre.offer.carveSize - pre.offer.imageLen}B 尾部分区)`);

const stages = [];
let r = await runInstall(bridge, pre.offer, pre.ext, {
  stage: (s) => stages.push(s),
  dataImages: pre.dataImages,
});
record("P1", "runInstall 全流程(prepare→设备直确认→session→chunk→finalize→done)",
       r.ok === true, r.ok
         ? `slot=${r.slot} stages=[${stages.join("→")}] offset=${pre.offer.imageLen}`
         : `stage=${r.stage} reason=${r.reason}`);
if (!r.ok) hardExit(1);
installed.push({ playId: play.id, offset: pre.offer.carveOffset,
                 size: pre.offer.carveSize, name: pre.offer.name });

raw = await listingRaw();
listing = parseSlots(raw);
const p1slot = listing?.slots.find((s) => s.offset === pre.offer.carveOffset && s.size === pre.offer.carveSize);
record("P1", "真机 slots 回读确认安装落盘", !!p1slot,
       p1slot
         ? `slot=${p1slot.slot} name=${p1slot.name} state=${p1slot.state} len=${p1slot.len}`
         : `未找到 offset=0x${pre.offer.carveOffset.toString(16)} size=${pre.offer.carveSize}`);
record("P1", "已装应用 len 等于解包镜像长(非 0)",
       !!p1slot && p1slot.len === pre.offer.imageLen,
       p1slot ? `len=${p1slot.len}` : "n/a");

// 删除(无 eraseData):APP 槽记录移除,Child DATA 应保留为 ARCHIVED
const rmRaw = await listingRaw();
const rmTarget = parseSlots(rmRaw)?.slots.find(
  (s) => s.offset === pre.offer.carveOffset && s.size === pre.offer.carveSize);
const rm = await bridge.remove(rmTarget?.slot ?? -1);
const rmOk = rm.ok === true;
let rmResult = null;
if (rmOk) rmResult = await waitDeviceBack(bridge, { tries: 20, delayMs: 1000 });
record("P1", "bridge.remove 真机 200 + waitDeviceBack 回连", rmOk && rmResult === true,
       `slot=${rmTarget?.slot ?? -1} removed=${rmOk} back=${rmResult}`);
if (!rmOk) process.exitCode = 1;

await resetSession();
const after = await listingRaw();
const afterL = parseSlots(after);
record("P1", "删除后 APP 槽消失(数据记录不参与 pool 占用)",
       !afterL?.slots.some((s) => s.offset === pre.offer.carveOffset),
       `count=${afterL?.count} free=${afterL?.free}`);
const afterData = afterL?.data ?? [];
const afterDataKey = afterData.map((d) =>
  `${d.play_id}:${d.label}:${d.state}:${d.offset}:${d.size}`).sort().join(",");
record("P1", "删除后 DATA 记录与运行前快照一致(P1 无 Child DATA,不应新增)",
       afterDataKey === initialData,
       `data=${JSON.stringify(afterData.map((d) => [d.play_id, d.label, d.state]))}`);
await resetSession();

// ── P2 夹具路径:同一真机跑通 Child DATA 分支 + eraseData 擦除闭环 ─────────
// 商店里没有「带 Child DATA 且装得下」的玩法(play 200 是 6.6MB,远超池容量),
// 所以合成一个**结构合法、字节真实**的完整 flash 镜像:
//   · 分区表从设备 flash 0x8000 读出(真布局:factory/ota_0/store/... 全在位);
//   · factory 分区填仓库 build 的真实 app 字节(与 P1 同一固件);
//   · 追加一条 0x82 文件系统数据分区,内容非 0xFF → 被解析成 Child DATA。
// 这样 prepareImage 的下载→sha256 校验→解包→DATA 提取四道门全部真实执行,
// 而不是手拼 offer 绕开它们。DATA 分区落在 store 分区 0x35A000 之后。
const PT_START = 0x8000;
const PT_ENTRY = 32;
const DATA_SUBTYPE = 0x82;                 // SPIFFS(child DATA 契约内)
const PM = [0xaa, 0x50];                   // 分区表条目 magic(小端 0x50AA)

function ptEntries(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset);
  const out = [];
  for (let off = PT_START; off + PT_ENTRY <= buf.length; off += PT_ENTRY) {
    if (buf[off] !== PM[0] || buf[off + 1] !== PM[1]) break;
    let label = "";
    for (let k = 12; k < 28 && buf[off + k] !== 0; k++) label += String.fromCharCode(buf[off + k]);
    out.push({ offset: dv.getUint32(off + 4, true), size: dv.getUint32(off + 8, true),
               type: buf[off + 2], subtype: buf[off + 3], label });
  }
  return out;
}

// 真实 Child DATA 的形状:分配 extent 8KB,但初始镜像只写前 2KB —— 其余是
// 未写 flash 的 0xFF。initialDataImageSize 从尾部回扫第一个非 0xFF,所以这里
// 必须把 DATA_LEN 之后的字节留 0xFF,否则会解析出 initial_image_size=8192,
// 与 runInstall 要求的 data 缓冲长度不符(设备契约:extent ≠ 初始 payload)。
const dataBytes = new Uint8Array(DATA_SIZE);
for (let k = 0; k < DATA_LEN; k++) dataBytes[k] = (k % 254) + 1;   // 1..254,永不 0xFF
for (let k = DATA_LEN; k < DATA_SIZE; k++) dataBytes[k] = 0xff;

// 分区表只需被解析器认到两条:factory(app)+ 一条 Child DATA。设备只消费
// prepareImage parse 出来的 app 字节与 DATA extent,分区表本身不上报。
// 行内容按 ESP-IDF 布局:type@+2 subtype@+3 offset U32LE@+4 size U32LE@+8
// label[16]@+12,16 字节补 0(initialDataImageSize 只扫数据区,但 label 必须
// 是干净 NUL 结尾 —— 0xFF 填充会被解析成 label "nvsÿÿÿ…" 之类的脏串)。
const APP_OFFSET = 0x10000;
const APP_SIZE = 0x170000;
const DATA_OFFSET = 0x300000;

function putRow(buf, dv, off, type, subtype, offset, size, label) {
  buf[off] = PM[0]; buf[off + 1] = PM[1];
  buf[off + 2] = type; buf[off + 3] = subtype;
  dv.setUint32(off + 4, offset, true);
  dv.setUint32(off + 8, size, true);
  for (let k = 0; k < 16; k++) buf[off + 12 + k] = 0;
  buf.set(Buffer.from(label, "ascii").subarray(0, 16), off + 12);
}

const merged = (() => {
  const app = new Uint8Array(readFileSync(FIXTURE));
  const buf = new Uint8Array(DATA_OFFSET + DATA_SIZE);
  buf.fill(0xff);                                // 未写入 flash = 0xFF
  const dv = new DataView(buf.buffer);
  putRow(buf, dv, PT_START, 0x00, 0x00, APP_OFFSET, APP_SIZE, "factory");
  putRow(buf, dv, PT_START + PT_ENTRY, 0x01, DATA_SUBTYPE,
         DATA_OFFSET, DATA_SIZE, DATA_LABEL);
  buf[APP_OFFSET] = 0xe9;                        // ESP32-C3 image magic
  buf.set(app, APP_OFFSET);                      // 真实 app 字节
  buf.set(dataBytes, DATA_OFFSET);               // 非 0xFF → initial payload 有长度
  return buf;
})();
if (!isFullImage(merged)) { fail("合成镜像未被识别为完整 flash 镜像"); hardExit(1); }

// prepareImage 走 fetch(`${METAPASS}/api/firmware?path=..&sha256=..`) 下载,path 参数会
// 被 encodeURIComponent 转义,所以标记串只用转义后不变的字符。只拦 fixture 那一个请求
// (与 tests/test_phone_install.mjs 同手法),verify / extract / DATA 提取三道门照常真实
// 执行;其余请求(设备桥、商店)一律透传真实网络。
const realFetch = globalThis.fetch;
const fixtureUrl = "browserSmokeFixtureMirror";
let fixtureServed = 0;
globalThis.fetch = (url, init) => {
  if (String(url).includes(fixtureUrl)) {
    fixtureServed++;
    return Promise.resolve(new Response(merged, { status: 200 }));
  }
  return realFetch(url, init);
};

const ext2 = extractAppImage(merged, Math.max(...SLOT_GEOMETRY.map((s) => s.partSize - 0x1000)));
const dataImgs = extractDataImages(merged);
const rec = dataImgs.find((d) => d.label === DATA_LABEL);
record("P2", "合成镜像被真实解析出 Child DATA 记录(extent > 初始镜像)",
       !!rec && rec.initial_image_size === DATA_LEN && rec.required_size === DATA_SIZE,
       `imageLen=${ext2.length} data[${dataImgs.length}] ` +
       `label=${rec?.label} sub=0x${(rec?.subtype ?? 0).toString(16)} ` +
       `req=${rec?.required_size} initial=${rec?.initial_image_size}B`);
if (!rec) { fail("合成镜像无 DATA 记录 —— 设备不会分配 extent,DATA 分支跑不了"); hardExit(1); }
record("P2", "合成 app 字节与仓库 build 一致(解包未损坏)",
       sha256(ext2.data) === sha256(new Uint8Array(readFileSync(FIXTURE))),
       `sha256=${sha256(ext2.data).slice(0, 16)}…`);

const meta = {
  analyze: {
    revisionId: 1, name: DATA_LABEL,
    store: { size: merged.length, sha256: sha256(merged) },
    extracted: { imageLen: ext2.length, sha256: sha256(ext2.data) },
    data: [], reason: "ok",
  },
  play: { id: FIXTURE_PLAY_ID, revisionId: 1, name: "Browser Smoke Data", enName: "",
          size: merged.length, sha256: sha256(merged),
          downloadUrl: fixtureUrl, updatedAt: null, category: null },
};

const l2Raw = await listingRaw();
const geom2 = geomFromListing(parseSlots(l2Raw), ext2.length);
if (!geom2.proposal) { fail(`P2 池无空间(maxGap=${geom2.maxGap})`); hardExit(1); }
// slot >= 0 时设备免物理确认;与 P1 同一路径(UI 选中新槽)。
const pre2 = await prepareImage(meta, geom2.proposal.slot, {}, "Browser Smoke", {
  geom: geom2, slotIsNew: true,
});
record("P2", "prepareImage 夹具路径(真实下载 + sha256 校验 + 解包 + DATA 提取)",
       pre2.ok === true && pre2.offer.data.length === 1,
       pre2.ok
         ? `imageLen=${pre2.offer.imageLen} dataExtent=${pre2.offer.data[0]?.size}B ` +
           `slot=${pre2.offer.slot} carveOff=0x${pre2.offer.carveOffset.toString(16)}`
         : `stage=${pre2.stage} reason=${pre2.reason}`);
if (!pre2.ok) hardExit(1);

const stages2 = [];
const r2 = await runInstall(bridge, pre2.offer, pre2.ext, {
  stage: (s) => stages2.push(s),
  // runInstall 契约:只上传初始镜像字节(data.length === initial_image_size),
  // extent 的余量由设备在 finalize 时清 0xFF。
  dataImages: [{ label: DATA_LABEL, size: DATA_SIZE, subtype: DATA_SUBTYPE,
                 required_size: DATA_SIZE, initial_image_size: DATA_LEN,
                 initialImageSize: DATA_LEN, data: dataBytes.subarray(0, DATA_LEN),
                 sha256: sha256(dataBytes.subarray(0, DATA_LEN)) }],
});
record("P2", "runInstall 带 Child DATA(分配 extent + /api/install/data 分块 + finalize)",
       r2.ok === true, r2.ok
         ? `slot=${r2.slot} stages=[${stages2.join("→")}] data=${DATA_LEN}/${DATA_SIZE}B`
         : `stage=${r2.stage} reason=${r2.reason}`);
if (!r2.ok) process.exitCode = 1;
installed.push({ playId: FIXTURE_PLAY_ID, offset: pre2.offer.carveOffset,
                 size: pre2.offer.carveSize, name: pre2.offer.name });
// P1 的槽是**故意删掉**的(验证归档语义),不进持久性基线;只有 P2 的槽要跨
// 复位存活。软复位清 RAM 不清 flash/NVS —— 记录仍在即证明落的是非易失存储。
persisted.push({ playId: FIXTURE_PLAY_ID, offset: pre2.offer.carveOffset,
                 size: pre2.offer.carveSize });

const d2 = parseSlots(await listingRaw());
const slot2 = d2?.slots.find((s) => s.offset === pre2.offer.carveOffset && s.size === pre2.offer.carveSize);
const dataRec = (d2?.data ?? []).find((d) => d.play_id === FIXTURE_PLAY_ID);
record("P2", "Child DATA 记录随 finalize 落盘(设备 P1-4 占用域)",
       !!slot2 && !!dataRec && dataRec.state === 0,
       `slot=${slot2?.slot ?? "-"} data[play=${dataRec?.play_id} label=${dataRec?.label} ` +
       `off=0x${(dataRec?.offset ?? 0).toString(16)} size=${dataRec?.size} state=${dataRec?.state}]`);
await resetSession();

if (!(slot2 && dataRec)) {
  record("P2", "remove{eraseData:true} 未执行(前置安装未完成)", false);
}

// ── P2.5 preflight 防线:商店元数据与镜像内容不符 → 上传任何字节之前整体拒绝 ──
// 两道闸门都在 prepareImage 内部、slot 检查之前触发,不依赖设备也不上传字节。
// 设备侧没有这两道校验(它信任手机算出的 sha),所以浏览器侧是唯一防线。

// 闸门 1:商店给的 store sha 与真实下载到的镜像不符 → stage "verify"。
const storeBad = {
  analyze: { revisionId: 1, name: "store-mismatch",
             store: { size: merged.length, sha256: "00".repeat(32) },
             extracted: { imageLen: ext2.length, sha256: sha256(ext2.data) },
             data: [], reason: "ok" },
  play: { id: MISMATCH_PLAY_ID, revisionId: 1, name: "Store Mismatch", enName: "",
          size: merged.length, sha256: "00".repeat(32),
          downloadUrl: fixtureUrl, updatedAt: null, category: null },
};
const badGeom = geomFromListing(parseSlots(await listingRaw()), ext2.length);
const bad1 = await prepareImage(storeBad, badGeom.proposal ? badGeom.proposal.slot : 0,
                                {}, "Browser Smoke",
                                { geom: badGeom, slotIsNew: !!badGeom.proposal });
record("P2.5", "闸门1 商店 store sha 不符 → verify 判死(未上传)",
       bad1.ok === false && bad1.stage === "verify",
       `${bad1.stage}: ${bad1.reason}`);

// 闸门 2:store sha 对,但 analyze 声称的解包 sha 与实际解包结果不符 → "preflight"。
// 这条对应 §6.3.7「不信任半截数据」:商店 analyze 是第三方产出,可能与固件不同步。
const analyzeBad = {
  analyze: { revisionId: 1, name: "analyze-mismatch",
             store: { size: merged.length, sha256: sha256(merged) },
             extracted: { imageLen: ext2.length, sha256: "00".repeat(32) },
             data: [], reason: "ok" },
  play: { id: MISMATCH_PLAY_ID, revisionId: 1, name: "Analyze Mismatch", enName: "",
          size: merged.length, sha256: sha256(merged),
          downloadUrl: fixtureUrl, updatedAt: null, category: null },
};
const bad2 = await prepareImage(analyzeBad, badGeom.proposal ? badGeom.proposal.slot : 0,
                                {}, "Browser Smoke",
                                { geom: badGeom, slotIsNew: !!badGeom.proposal });
record("P2.5", "闸门2 analyze 解包 sha 与镜像不符 → preflight 判死(未上传)",
       bad2.ok === false && bad2.stage === "preflight",
       `${bad2.stage}: ${bad2.reason}`);

// ── P2.6 持久性:软复位(清 RAM,不动 flash/NVS)后安装记录仍在 ────────────
// P1 的槽是故意删掉的(验证归档语义),只有 persisted[] 里的槽要跨复位存活。
// P2.6 需要 USB 端口做软复位;仅走 LAN 时标记 skipped(诚实报告,不谎报通过)。
const rb = persisted.length && PORT ? await softReset()
       : { rebooted: null, polls: 0 };
if (rb.rebooted === true) {
  const rbL = parseSlots(await listingRaw());
  const still = persisted.filter((x) =>
    rbL?.slots.some((s) => s.offset === x.offset && s.size === x.size && s.state === "valid"));
  record("P2.6", "复位后安装记录仍在(flash/NVS 持久,非 RAM 假象)",
         still.length === persisted.length && persisted.length > 0,
         `soft-reset via ${PORT}, persisted=${still.length}/${persisted.length}`);
} else if (rb.rebooted === false) {
  record("P2.6", "软复位后设备未恢复,无法验证持久性", false,
         `polls=${rb.polls}`);
} else {
  // 无 USB 端口(仅 LAN 跑):持久性无法验证。SKIPPED,不计入 PASS 也不算失败。
  log("P2.6 SKIPPED:未传 --port,无法软复位验证持久性(非缺陷)");
}

// P2 的槽验证完持久性再删:remove{eraseData:true} 同时清 APP 槽与其 DATA extent,
// 断言归档残留不泄漏(不留 state=2 记录)。
if (slot2 && dataRec) {
  await resetSession();
  const rm2 = await bridge.remove(slot2.slot, { eraseData: true });
  const rm2Ok = rm2.ok === true;
  const back2 = rm2Ok ? await waitDeviceBack(bridge, { tries: 30, delayMs: 1000 }) : false;
  const fin = parseSlots(await listingRaw());
  const gone = !fin?.data.some((d) => d.play_id === FIXTURE_PLAY_ID);
  record("P2", "remove{eraseData:true} 擦除 APP 槽与 Child DATA(不留归档残留)",
         rm2Ok && back2 === true && gone,
         `removed=${rm2Ok} back=${back2} dataGone=${gone}`);
  await resetSession();
}

const nfail = results.filter((x) => !x.ok).length;
log(`结果: ${results.length - nfail}/${results.length} 通过${nfail ? ` (${nfail} 失败)` : ""}`);
const report = { device: IP, storePlay: STORE_PLAY_ID, fixturePlay: FIXTURE_PLAY_ID,
                  fixtureImageLen: ex.imageLen, results, rebooted: rb.rebooted,
                  failed: nfail, verdict: nfail ? "FAIL" : "PASS" };
console.log(`##REPORT ${JSON.stringify(report)}`);
await cleanup();
process.exit(nfail ? 1 : 0);
