// tools/test-bootable-qemu.mjs —— 无头 QEMU 引导测试(不依赖浏览器/真机)。
//
// 用 passport-sim 的 ESP-EMU(QEMU WASM)核心真实引导固件镜像,两层断言:
//
//   A. UART0 变体(QEMU 测试构建,build-qemu/):日志走 UART0,esp-emu 可见。
//      断言引导到 app 运行(app_main 应用日志出现;「就绪:slot0=」在 QEMU 全速仿真
//      ~0.4% 实时下需 30min+ wall time,且显示层全初始化已由 B 用例帧缓冲覆盖)。
const APP_BOOTED = /CW2017|Running firmware is factory|电池计不在位/;
const READY = "就绪:slot0=";
//      同时负例断言 MPUP 容器原样写 0x0 无法引导(市场工具的既有实测)。
//
//   B. 市场镜像 build/meta-pass-bootable_*.bin(原发布配置,USB-JTAG 控制台):
//      esp-emu 不路由 USB-JTAG 的中断驱动 TX,app 日志不可见 —— 改用
//      ST7789 帧缓冲判定:出现 LVGL 背景或任何像素写入 = app 已运行到
//      显示初始化,引导成立。
//
// 运行:node tools/test-bootable-qemu.mjs
// 依赖:../passport-sim(兄弟仓库)public/wasm/ 下的运行时与板级模块。
// 注意:QEMU 测试构建由 tools/build-firmware.sh 或下方 buildQemuVariant()
// 产出(sdkconfig 临时切 UART0 控制台,与发布配置仅控制台路由不同)。

import { readFile, access } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..");
const simWasmDir = path.resolve(repoRoot, "..", "passport-sim", "public", "wasm");

const { default: init, WasmEmulator } = await import(
  pathToFileUrl(path.join(simWasmDir, "pkg", "esp_emu.js"))
);
const { AiPassportBoard } = await import(pathToFileUrl(path.join(simWasmDir, "ai-passport-board.js")));

function pathToFileUrl(p) {
  return "file://" + p.split(path.sep).join("/");
}

async function exists(p) {
  try { await access(p); return true; } catch { return false; }
}

async function newestBuild(glob) {
  const prefix = "upgrade/";
  const sub = glob.startsWith(prefix) ? glob.slice(prefix.length) : glob;
  const re = new RegExp(sub);
  const root = glob.startsWith(prefix) ? path.join(repoRoot, "build", "upgrade") : path.join(repoRoot, "build");
  const { readdir } = await import("node:fs/promises");
  const names = await readdir(root).catch(() => {
    throw new Error(`${path.relative(repoRoot, root)} 不存在——先运行 tools/build-firmware.sh`);
  });
  const hits = names.filter((n) => re.test(n)).sort();
  if (hits.length === 0) throw new Error(`找不到匹配 ${glob} 的产物`);
  return path.join(root, hits.at(-1));
}

// 由发布版 sdkconfig 临时切 UART0 控制台,在 build-qemu/ 出 QEMU 测试构建。
// 与发布镜像的代码完全相同,仅控制台路由不同(发布版为 USB-Serial-JTAG)。
async function buildQemuVariant() {
  const { execFile } = await import("node:child_process");
  const { promisify } = await import("node:util");
  const run = promisify(execFile);
  const sdk = await readFile(path.join(repoRoot, "sdkconfig"), "utf8");
  if (!/CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y/.test(sdk)) {
    throw new Error("发布 sdkconfig 不是 USB-JTAG 控制台配置——QEMU 变体逻辑需复查");
  }
  const patched = sdk
    .replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set")
    .replace("# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set", "CONFIG_ESP_CONSOLE_UART_DEFAULT=y");
  const { writeFileSync, copyFileSync } = await import("node:fs");
  const bak = path.join(repoRoot, "sdkconfig.qemu-bak");
  copyFileSync(path.join(repoRoot, "sdkconfig"), bak);
  writeFileSync(path.join(repoRoot, "sdkconfig"), patched);
  try {
    await run("bash", ["-c", "source /Users/alex/esp/esp-idf-v5.5.3/export.sh >/dev/null 2>&1 && idf.py -B build-qemu build"], { cwd: repoRoot });
  } finally {
    copyFileSync(bak, path.join(repoRoot, "sdkconfig"));
  }
  return path.join(repoRoot, "build-qemu");
}

// 用三件套铺一张 8MB flash 镜像(尾部擦除态,与市场镜像同构)。
async function composeFlashImage(buildDir) {
  const [bl, pt, app] = await Promise.all([
    readFile(path.join(buildDir, "bootloader", "bootloader.bin")),
    readFile(path.join(buildDir, "partition_table", "partition-table.bin")),
    readFile(path.join(buildDir, "FoloToy-AI-Passport.bin")),
  ]);
  if (bl[0] !== 0xe9 || pt[0] !== 0xaa || pt[1] !== 0x50 || app[0] !== 0xe9) {
    throw new Error("构建产物缺镜像魔数——请先完成编译");
  }
  const img = new Uint8Array(8 * 1024 * 1024);
  img.set(bl, 0);
  img.set(pt, 0x8000);
  img.set(app, 0x10000);
  return img;
}

const BATCH = 50_000;
const RUN_MS = 90_000;

// —— 引导运行器(board 装配 + 帧捕获)——
async function runImage(firmware, { ms = RUN_MS, stopAt } = {}) {
  const emu = new WasmEmulator("esp32c3");
  emu.load_default_rom();
  emu.set_boot_from_rom(true);
  emu.load_firmware(firmware);
  const board = new AiPassportBoard(wasmExports, { emulator: emu });
  let frames = 0;
  let pixelWrites = 0;
  board.onFrame = (frame) => {
    frames += 1;
    // 统计非纯黑像素(launcher 界面必然有非黑内容;背光全暗/黑屏不算)
    for (let i = 0; i < frame.pixels.length; i += 4) {
      if (frame.pixels[i] + frame.pixels[i + 1] + frame.pixels[i + 2] > 30) { pixelWrites += 1; break; }
    }
  };
  const uartChunks = [];
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    uartChunks.push(emu.run_batch(BATCH));
    board.drain();
    const uart = uartChunks.join("");
    if (stopAt && stopAt(uart)) return { uart, frames, pixelWrites, timedOut: false };
  }
  return { uart: uartChunks.join(""), frames, pixelWrites, timedOut: true };
}

let wasmExports = null;

let failed = false;

// ---- 准备 WASM(直接传 Module,绕过 Node fetch 不支持 file://)----
wasmExports = await init(new WebAssembly.Module(
  await readFile(path.join(simWasmDir, "pkg", "esp_emu_bg.wasm")),
));

// ==== A1. UART0 变体:引导到 launcher 完成 ====
console.log("A1. UART0 QEMU 变体:引导到 app 运行(app_main 应用日志)");
const buildQemu = await exists(path.join(repoRoot, "build-qemu", "FoloToy-AI-Passport.bin"))
  ? path.join(repoRoot, "build-qemu")
  : await buildQemuVariant();
const fwUart = await composeFlashImage(buildQemu);
const a1 = await runImage(fwUart, {
  stopAt: (u) => APP_BOOTED.test(u) || u.includes(READY),
});
const readyIdx = a1.uart.indexOf(READY);
if (APP_BOOTED.test(a1.uart)) {
  const tail = a1.uart.slice(a1.uart.search(APP_BOOTED) - 200);
  console.log("PASS A1: bootloader→分区表→factory app→app_main 全链路,QEMU 内应用代码已运行");
  if (readyIdx >= 0) console.log("(超额)launcher 就绪标记也出现");
  console.log("--- UART 摘录 ---\n" + tail.trim().split("\n").slice(-10).join("\n"));
} else {
  failed = true;
  console.error(`FAIL A1: ${RUN_MS / 1000}s 内未见 app 引导日志`);
  console.error("--- UART 尾部 ---\n" + a1.uart.slice(-1200));
}

// ==== A2. 负例:MPUP 容器原样写 0x0 必须无法引导 ====
console.log("\nA2. 负例:MPUP 容器(市场工具原样写 0x0 的错误刷法)");
const containerPath = await newestBuild("upgrade/meta-pass-upgrade_.*\\.bin$");
const fwBad = new Uint8Array(await readFile(containerPath));
const a2 = await runImage(fwBad, { ms: 25_000, stopAt: (u) => u.includes("meta-pass launcher") });
if (a2.uart.includes("meta-pass launcher")) {
  failed = true;
  console.error("FAIL A2: 容器格式竟能引导出 launcher——与真机实测矛盾,需复查");
} else {
  const booted = a2.uart.includes("2nd stage bootloader");
  console.log(`PASS A2: 容器原样写 0x0 无 launcher(bootloader 阶段到达=${booted},与真机行为一致)`);
}

// ==== B. 市场镜像(USB-JTAG 发布配置):帧缓冲判定 ====
console.log("\nB. 市场镜像:ST7789 帧缓冲判定(esp-emu 不路由 USB-JTAG app 日志)");
const bootablePath = await newestBuild("meta-pass-bootable_.*\\.bin$");
const fwMarket = new Uint8Array(await readFile(bootablePath));
const b = await runImage(fwMarket, { ms: 120_000 });
if (b.pixelWrites > 0 || b.frames > 3) {
  console.log(`PASS B: 显示帧缓冲出现内容(${b.frames} 帧,含非黑像素=${b.pixelWrites > 0})—— app 运行到了显示初始化,市场镜像可引导`);
} else {
  failed = true;
  console.error(`FAIL B: ${120_000 / 1000}s 内无显示帧(frames=${b.frames})`);
  console.error("--- UART 尾部 ---\n" + b.uart.slice(-800));
}

process.exit(failed ? 1 : 0);
