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

// ==== C1. 引导市场镜像 → 列表页高亮 slot0 ====
log(`C1. 引导市场镜像,每 ${SNAP_EVERY_MS / 1000}s 截帧,等待列表页(高亮=slot0,连续两次一致)`);
const bootablePath = await newestBuild("meta-pass-bootable_.*\\.bin$");
log(`   镜像:${path.basename(bootablePath)}`);
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

// ==== A2. 负例:MPUP 容器原样写 0x0 必须无法引导 ====
log("\nA2. 负例:MPUP 容器(市场工具原样写 0x0 的错误刷法)");
const containerPath = await newestBuild("upgrade/meta-pass-upgrade_.*\\.bin$");
const bad = bootSession(new Uint8Array(await readFile(containerPath)));
bad.runFor(25_000);
if (bad.uart.includes("meta-pass launcher")) {
  failed = true;
  log("FAIL A2: 容器格式竟能引导出 launcher——与真机实测矛盾,需复查");
} else {
  const booted = bad.uart.includes("2nd stage bootloader");
  log(`PASS A2: 容器原样写 0x0 无 launcher(bootloader 阶段到达=${booted},与真机行为一致)`);
}

log(`\n==== ${failed ? "FAILED" : "ALL PASS"} ====`);
process.exit(failed ? 1 : 0);
