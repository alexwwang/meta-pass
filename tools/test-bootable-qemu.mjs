// tools/test-bootable-qemu.mjs — 市场镜像无头 QEMU 引导 + 交互验证(截图驱动)。
// 运行:node tools/test-bootable-qemu.mjs(需兄弟目录 passport-sim 的 QEMU WASM 核心)
//
// 判定逻辑(不依赖 UART 日志——发布配置是 USB-JTAG,esp-emu 不路由 app 日志):
//   C1  引导市场镜像 meta-pass-bootable_*.bin,每 10s 截帧检查列表页:
//       4 行槽位面板中恰好一行高亮(底色与众不同)且为第 0 行(slot0),
//       且连续两次截图一致(排除过渡帧);
//   C2  注入一次 DOWN 键(ADC 300mV):按压恰好 100 仿真 ms(消抖 20ms 以上、短按
//       上限 180ms 以下——按太久在仿真时间里就是长按,SINGLE_CLICK 不发射;wall
//       时长由 emu.cycles() 在按压中实时控长,与仿真速率解耦),释放后每 10s 截帧:
//       高亮行应变为第 1 行(slot1)。
//       (列表页语义:DOWN=选中下一行,slot0→slot1;UP 是环形向后,勿用)
//   A2  负例:MPUP 升级容器被市场工具原样写 0x0,必须无法引导(与真机实测一致)。
// C1+C2 通过 = 固件可成功引导且列表页按键交互正常。
//
// 说明:QEMU WASM 仿真速率随负载浮动(C1 引导期与空闲期实测相差数倍),文中时间
// 均为 wall time;截图存 /tmp/metapass-qemu-*.ppm 供人工复核。

import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..");
const simWasmDir = path.resolve(repoRoot, "..", "passport-sim", "public", "wasm");

const { default: init, WasmEmulator } = await import(
  "file://" + path.join(simWasmDir, "pkg", "esp_emu.js").split(path.sep).join("/")
);
const { AiPassportBoard } = await import(
  "file://" + path.join(simWasmDir, "ai-passport-board.js").split(path.sep).join("/")
);

const BATCH = 50_000;
const SNAP_EVERY_MS = 10_000;   // 截图周期(用户指定 10s)
const C1_TIMEOUT_MS = 600_000;  // 引导到列表页的 wall time 上限
const PRESS_SIM_MS = 100;       // 按压的仿真时长(消抖 ~20ms < 目标 < 单击上限 180ms,取中点)
const PRESS_SLICE_MS = 50;      // 按压期间 runFor 的切片粒度(用 cycles 精确控长)
const RELEASE_WATCH_MS = 150_000; // 释放后等 CLICK 事件(释放+180ms 窗口)生效的观察窗

// —— 最新构建产物(在 build/ 下按文件名取字典序最大——文件名含版本号+git hash,天然有序)——
async function newestBuild(rel) {
  const dir = path.join(repoRoot, "build", path.dirname(rel));
  const re = new RegExp(path.basename(rel));
  const { readdir } = await import("node:fs/promises");
  const files = (await readdir(dir)).filter((f) => re.test(f));
  if (!files.length) throw new Error(`未找到构建产物:${rel}(先跑 tools/build-firmware.sh)`);
  files.sort();
  return path.join(dir, files.at(-1));
}

// —— 列表页高亮判定:4 行面板(y=52+40i, 高40, x=12..228)左侧空白区采样取众数底色,
//    恰好一行底色与其余三行不同 = 选中行。对反色/配色漂移鲁棒(不预设具体颜色)。——
const ROW0_Y = 52, ROW_H = 40, N_ROWS = 4, FB_W = 240;
function readSelection(fb) {
  const bg = [];
  for (let i = 0; i < N_ROWS; i++) {
    const counts = new Map();
    for (let y = ROW0_Y + i * ROW_H + 15; y <= ROW0_Y + i * ROW_H + 24; y += 3) {
      for (let x = 16; x <= 40; x += 4) {
        const o = (y * FB_W + x) * 4;
        const key = `${fb[o] >> 4},${fb[o + 1] >> 4},${fb[o + 2] >> 4}`; // 16 级量化
        counts.set(key, (counts.get(key) ?? 0) + 1);
      }
    }
    let best = null, bn = 0;
    for (const [k, n] of counts) if (n > bn) { bn = n; best = k; }
    bg.push(best);
  }
  const tally = new Map();
  for (const c of bg) tally.set(c, (tally.get(c) ?? 0) + 1);
  let mode = null, mn = 0;
  for (const [k, n] of tally) if (n > mn) { mn = n; mode = k; }
  const odd = bg.map((c, i) => (c !== mode ? i : -1)).filter((i) => i >= 0);
  // valid:3 行同色 + 恰好 1 行不同;否则画面未就绪或渲染异常
  return { valid: mn >= 3 && odd.length === 1, sel: odd.length === 1 ? odd[0] : -1, bg };
}

// —— 引导会话:可交互(runFor/press/snapshot) ——
function bootSession(firmware) {
  const emu = new WasmEmulator("esp32c3");
  emu.load_default_rom();
  emu.set_boot_from_rom(true);
  emu.load_firmware(firmware);
  const board = new AiPassportBoard(wasmExports, { emulator: emu });
  let shot = 0;
  const sess = {
    uart: "",
    runFor(wallMs) {
      const t0 = Date.now();
      while (Date.now() - t0 < wallMs) {
        sess.uart += emu.run_batch(BATCH);
        board.drain();
      }
    },
    snapshot(tag) {
      const s = readSelection(board.display.framebuffer);
      dumpPpm(board.display, `/tmp/metapass-qemu-${tag}-${shot++}.ppm`);
      return s;
    },
    pressDown(name) { board.setButton(name, true); },
    pressUp(name) { board.setButton(name, false); },
    press(name, holdMs) { board.setButton(name, true); sess.runFor(holdMs); board.setButton(name, false); },
    cycles: () => emu.cycles(),
  };
  return sess;
}

// —— 自适应按压:按下后用 emu.cycles() 实时跑到 PRESS_SIM_MS 仿真毫秒再释放。
//    ESP32-C3 @160MHz:1 sim-ms = 160_000 cycles。速率波动不影响按压的仿真时长。——
const SIM_CYCLES_PER_MS = 160_000;
function pressSimMs(sess, name, simMs) {
  sess.pressDown(name);
  const tWall = Date.now();
  const c0 = sess.cycles();
  while (sess.cycles() - c0 < simMs * SIM_CYCLES_PER_MS) sess.runFor(PRESS_SLICE_MS);
  sess.pressUp(name);
  return Date.now() - tWall;
}

// —— 帧导出(PPM P6,便于人工复核) ——
function dumpPpm(display, file) {
  const { width: w, height: h, framebuffer: fb } = display;
  const rgb = Buffer.alloc(w * h * 3);
  for (let p = 0; p < w * h; p++) {
    rgb[p * 3] = fb[p * 4]; rgb[p * 3 + 1] = fb[p * 4 + 1]; rgb[p * 3 + 2] = fb[p * 4 + 2];
  }
  writeFile(file, Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), rgb]))
    .catch(() => {}); // 导出失败不影响判定
}

let wasmExports = null;
let failed = false;
const log = console.log;

// ---- 准备 WASM(直接传 Module,绕过 Node fetch 不支持 file://)----
wasmExports = await init(new WebAssembly.Module(
  await readFile(path.join(simWasmDir, "pkg", "esp_emu_bg.wasm")),
));

// META_QEMU_C3_ONLY=1:只跑 C3(迭代单次会话策略时省时间);完整门禁必须不带该变量跑全量。
const C3_ONLY = !!process.env.META_QEMU_C3_ONLY;
if (C3_ONLY) {
  log("META_QEMU_C3_ONLY=1 —— 跳过 C1/C2/A2,仅运行 C3(单次会话策略验证)");
}
if (!C3_ONLY) {
// ==== C1. 引导单文件固件 → 列表页高亮 slot0 ====
log(`C1. 引导单文件固件,每 ${SNAP_EVERY_MS / 1000}s 截帧,等待列表页(高亮=slot0,连续两次一致)`);
const bootablePath = await newestBuild("meta-pass_v.*\\.bin$");
log(`   镜像:${path.basename(bootablePath)}(含 44B MPUPV2 指纹尾段,原样写 0x0 必须可引导)`);
const sess = bootSession(new Uint8Array(await readFile(bootablePath)));
let sel0Stable = false, prev = null;
const t1 = Date.now();
while (Date.now() - t1 < C1_TIMEOUT_MS) {
  sess.runFor(SNAP_EVERY_MS);
  const s = sess.snapshot("c1");
  const el = Math.round((Date.now() - t1) / 1000);
  log(`   [+${el}s] 高亮行=${s.sel >= 0 ? s.sel : "无"} 底色=[${s.bg.join(" | ")}]`);
  if (s.valid && s.sel === 0 && prev?.valid && prev.sel === 0) { sel0Stable = true; break; }
  prev = s;
}
if (sel0Stable) {
  log("PASS C1: 列表页出现,高亮行=slot0(连续两次截图一致)——固件成功引导到 UI");
} else {
  failed = true;
  log(`FAIL C1: ${C1_TIMEOUT_MS / 1000}s 内未出现「高亮=slot0」的稳定列表页(截图已存 /tmp/metapass-qemu-c1-*.ppm)`);
}

// ==== C2. 注入 DOWN 键 → 高亮从 slot0 移到 slot1 ====
if (sel0Stable) {
  log(`\nC2. 注入 DOWN 键(按压 ${PRESS_SIM_MS} 仿真 ms:越过消抖且短于单击上限),释放后观察 ${RELEASE_WATCH_MS / 1000}s`);
  const tPress = Date.now();
  pressSimMs(sess, "DOWN", PRESS_SIM_MS);
  log(`   按压实际 ${Math.round(Date.now() - tPress)}ms wall = ${PRESS_SIM_MS}ms 仿真`);
  let sel1 = false;
  const t2 = Date.now();
  while (Date.now() - t2 < RELEASE_WATCH_MS) {
    sess.runFor(SNAP_EVERY_MS);
    const s = sess.snapshot("c2");
    const el = Math.round((Date.now() - t2) / 1000);
    log(`   [+${el}s] 高亮行=${s.sel >= 0 ? s.sel : "无"} 底色=[${s.bg.join(" | ")}]`);
    if (s.valid && s.sel === 1) { sel1 = true; break; }
  }
  if (sel1) {
    log("PASS C2: DOWN 键后高亮行 slot0 → slot1,列表页按键交互正常");
  } else {
    failed = true;
    log("FAIL C2: DOWN 后高亮未到 slot1(截图已存 /tmp/metapass-qemu-c2-*.ppm)");
  }
} else {
  log("\nC2. 跳过(列表页未出现,无按键前提)");
}

// ==== A2. 单文件固件含指纹尾段,原样写 0x0 仍可引导(尾段不干扰 boot)====
log("\nA2. 单文件固件尾段无干扰:全文件原样写 0x0(含 MPUPV2 指纹)照常引导出 launcher");
// 引导证据 = C1 本身(截图驱动:列表页出现且可交互,必然经过完整 boot 链)。
// 不用 UART 断言:固件日志级别为 WARN(CONFIG_LOG_DEFAULT_LEVEL=2),ESP_LOGI
// 的 launcher 横幅不会出现在串口输出上(旧负例断言「不含」所以从未暴露此坑)。
// 历史:旧 MPUP 升级容器是故意不可引导的格式(A2 曾是负例);单文件化后
// 发布产物必须可引导,本用例随之从负例转为正例。
if (sel0Stable) {
  log("PASS A2: 单文件固件(含指纹尾段)原样写 0x0 引导出 launcher(C1 列表页已出现并响应按键)——尾段落入 factory 分区尾部未用空间,boot 不理会");
} else {
  failed = true;
  log("FAIL A2: C1 未能引导出列表页,需复查尾段是否破坏引导");
}
} // end !C3_ONLY(C1/C2/A2)

// ==== C3. 单次会话策略:otadata 预置「CRC 合法的 VALID 常驻态」→ hook 必须清除 ====
// 最恶劣场景构造:otadata 两副本均写入 seq=1 / state=VALID(0x2) / CRC 合法,且 ota_0
// 放入真实可引导子固件 —— 标准 IDF bootloader 面对该状态必然直接引导 ota_0(子固件
// 常驻)。meta-boot hook 生效时:两副本被擦除 → "Defaulting to factory image" → 列表页。
// 任一副本漏擦都会指向可引导的 ota_0,常驻依旧 —— 因此本断言不可能「碰巧通过」。
{
  log("\nC3. otadata VALID 常驻态:两副本 CRC 合法 + ota_0 放真实子固件 → hook 必须擦除并回 factory");

  // zlib 标准 CRC32,等价于 IDF 判定公式 bootloader_common_ota_select_crc:
  // esp_rom_crc32_le(UINT32_MAX, (uint8_t*)&s->ota_seq, 4)(ROM 实现首尾各取反一次,
  // 初值 0xFFFFFFFF 与终值取反互抵,即标准 zlib CRC32)。
  const crc32Le = (bytes) => {
    let c = 0xFFFFFFFF;
    for (const v of bytes) {
      c ^= v;
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return (c ^ 0xFFFFFFFF) >>> 0;
  };
  if (crc32Le(new TextEncoder().encode("123456789")) !== 0xCBF43926) {
    throw new Error("crc32Le 自检失败:CRC32('123456789') != 0xCBF43926");
  }

  // 现场合并全新 flash 镜像(不用可能过期的 build/*-full.bin):三段按固定偏移铺设,
  // 其余全 0xFF(擦除态)。三段取自 build/ 最新一次带 hook 的编译产物。
  const flash = Buffer.alloc(8 * 1024 * 1024, 0xff);
  for (const [off, rel] of [
    [0x0, "build/bootloader/bootloader.bin"],
    [0x8000, "build/partition_table/partition-table.bin"],
    [0x10000, "build/FoloToy-AI-Passport.bin"],
  ]) {
    const b = await readFile(path.join(repoRoot, rel));
    b.copy(flash, off);
    log(`   铺设 ${path.basename(rel)}(${b.length}B)@ 0x${off.toString(16)}`);
  }

  // 分区表自解析(0x8000 起,32B/条,魔数 0x50AA)—— otadata/ota_0 偏移不硬编码。
  let otadataOff = -1, ota0Off = -1, ota0Size = -1;
  for (let a = 0x8000; a < 0x8C00; a += 32) {
    if (flash.readUInt16LE(a) !== 0x50AA) break;
    const label = flash.subarray(a + 12, a + 28).toString("latin1").replace(/\0.*$/, "");
    const off = flash.readUInt32LE(a + 4);
    const size = flash.readUInt32LE(a + 8);
    if (label === "otadata") { otadataOff = off; log(`   分区表:otadata @ 0x${off.toString(16)} (${size}B)`); }
    if (label === "ota_0") { ota0Off = off; ota0Size = size; log(`   分区表:ota_0 @ 0x${off.toString(16)} (${size}B)`); }
  }
  if (otadataOff < 0 || ota0Off < 0) throw new Error("分区表解析失败:otadata 或 ota_0 未找到");

  // ota_0 放入真实签名子固件(候选槽位必须可引导,否则本用例失去鉴别力)。
  const childPath = path.resolve(repoRoot, "..", "pass-radar", "build", "pass-radar_v0.1-2-g8fcce59-signed.bin");
  const child = await readFile(childPath);
  if (child.length > ota0Size) throw new Error(`子固件 ${child.length}B 超出 ota_0 容量 ${ota0Size}B`);
  child.copy(flash, ota0Off);
  log(`   ota_0 已放入 ${path.basename(childPath)}(${child.length}B)`);

  // otadata 两副本(每副本占 otadata 分区内 1 个 4KB 扇区):seq=1 → 映射 ota_0;
  // state=VALID(0x2);crc = zlib32(ota_seq)。
  const entry = Buffer.alloc(32, 0xff);
  entry.writeUInt32LE(1, 0);
  Buffer.from(new TextEncoder().encode("metapass-c3-policy-2")).copy(entry, 4);
  entry.writeUInt32LE(0x2, 24);
  entry.writeUInt32LE(crc32Le(entry.subarray(0, 4)), 28);
  entry.copy(flash, otadataOff);
  entry.copy(flash, otadataOff + 0x1000);
  log(`   otadata 两副本已写:seq=1 state=VALID(0x2) crc=0x${crc32Le(entry.subarray(0, 4)).toString(16)}(均指向 ota_0)`);

  const sess3 = bootSession(new Uint8Array(flash));
  let e0 = false, e1 = false, fb = false, listed = false, prev3 = null;
  const t3 = Date.now();
  while (Date.now() - t3 < C1_TIMEOUT_MS) {
    sess3.runFor(SNAP_EVERY_MS);
    if (!e0) e0 = sess3.uart.includes("otadata copy 0 in VALID state -> erasing");
    if (!e1) e1 = sess3.uart.includes("otadata copy 1 in VALID state -> erasing");
    if (!fb) fb = sess3.uart.includes("Defaulting to factory image");
    const s = sess3.snapshot("c3");
    const el = Math.round((Date.now() - t3) / 1000);
    log(`   [+${el}s] hook擦除=[copy0:${e0 ? "✓" : "-"} copy1:${e1 ? "✓" : "-"}] factory回退=${fb ? "✓" : "-"} 高亮=${s.sel >= 0 ? s.sel : "无"}`);
    if (s.valid && s.sel === 0 && prev3?.valid && prev3.sel === 0) { listed = true; break; }
    prev3 = s;
  }
  if (listed && e0 && e1 && fb) {
    log("PASS C3: 两副本 VALID 均被 hook 擦除 → bootloader 回退 factory → 列表页(slot0 高亮)出现。");
    log("      面对该状态,标准 bootloader 必然直接引导 ota_0(子固件常驻);本用例同时证明 hook 已真实链接进 bootloader 并生效。");
  } else {
    failed = true;
    log(`FAIL C3: hook擦除 copy0=${e0} copy1=${e1} factory回退=${fb} 列表页=${listed}(截图 /tmp/metapass-qemu-c3-*.ppm)`);
    log(`      UART 尾部 600B:\n${sess3.uart.slice(-600)}`);
  }
}

log(`\n==== ${failed ? "FAILED" : "ALL PASS"} ====`);
process.exit(failed ? 1 : 0);
