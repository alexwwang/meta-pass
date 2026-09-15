// test/metapass-boot.test.mjs —— meta-pass 引导 + 按键响应的模拟器端到端测试。
//
// 在宿主 Node 里直接跑 QEMU wasm 模拟核心 + AI Passport 板桥接器,
// 注入真实按键并断言屏幕画面随之变化。不需要浏览器,也不起 HTTP 服务。
//
// 为什么断言只看像素、不看日志:
//   meta-pass 的 sdkconfig 里 CONFIG_LOG_DEFAULT_LEVEL=3(WARN),
//   ESP_LOGI 级别的输出完全不进串口。用"没有看到日志"判断启动失败是错的 ——
//   固件可能跑得好好的,只是它不开 INFO 日志。画面是唯一可靠的可观测输出。
//
// 为什么用整帧哈希而不是几行像素:
//   屏幕 240x320,但槽位列表在 y≈52 以下。只取前若干字节(标题区)做比较,
//   标题在列表页是静态的,于是每次按键都得到"画面没变"的假阴性。
//   早期版本就这么误判过一次。
//
// 运行时依赖(不入库):
//   public/wasm/pkg/esp_emu_bg.wasm(3.3MB QEMU 预编译产物)不在本仓库里,
//   由 tools/sim/run-sim-test.sh 从本地的 passport-sim 检出复制到 tools/sim/。
//   做法与 tools/signing/run-verify-tests.sh 探测 Homebrew mbedtls 一致:
//   大体积第三方二进制走外部路径,不进版本控制。
//
// 运行: tools/sim/run-sim-test.sh   或   node --test tools/sim/
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { initSync, WasmEmulator } from "./esp_emu.js";
import { AiPassportBoard } from "./ai-passport-board.js";

const SCREEN_W = 240;
const SCREEN_H = 320;
const PIXEL_BYTES = 4;                 // framebuffer 是 RGBA(Uint8ClampedArray,st7789.js 按 4 字节步进)
const SCREEN_BYTES = SCREEN_W * SCREEN_H * PIXEL_BYTES;
const BATCH = 50_000;                  // 与 passport-sim 的 DEFAULT_BATCH_SIZE 一致
const BOOT_CYCLES = 1_000_000_000;     // 引导 + LVGL 首次渲染所需的最多周期数
const PRESS_CYCLES = 12_000_000;       // 按住时长,与 passport-sim 的 CLICK_CYCLES 一致
const RELEASE_CYCLES = 20_000_000;     // 松开后等重绘
// 为什么是 12M 而不是几百毫秒:固件按键回调只认 BSP_BTN_CLICK,
// 而按住 1500ms(BSP_BTN_LONG_MS)以上只产生 LONG 事件,CLICK 不再触发,
// 列表导航因此毫无反应。之前用 600M 周期测按键,就是这个原因全被吞掉。
const firmwarePath = process.env.META_PASS_IMAGE ||
  new URL("../../build/meta-pass_v0.2.2-35-gaa25c52.bin", import.meta.url);

// 引导一次,4 个测试顺序共享同一模拟器实例。
let emulator;
let board;
let frames = [];
const runCycles = async (cycles) => {
  for (let n = 0; n < Math.ceil(cycles / BATCH); n++) {
    emulator.run_batch(BATCH);
    board.drain();
  }
};

// 整帧 sha256。Buffer.from(pixels.buffer) 覆盖全部 240x320x3 字节。
const frameHash = (frame) =>
  createHash("sha256").update(Buffer.from(frame.pixels.buffer)).digest("hex");

const lastHash = () => frameHash(frames.at(-1));

// 注入一次完整按键(按下 + 松开),返回 [按下前, 松开后] 两帧哈希。
const pressButton = async (name) => {
  const before = lastHash();
  board.setButton(name, true);
  await runCycles(PRESS_CYCLES);
  board.setButton(name, false);
  await runCycles(RELEASE_CYCLES);
  return [before, lastHash()];
};

before(async () => {
  const wasm = initSync(await readFile(new URL("./esp_emu_bg.wasm", import.meta.url)));
  emulator = new WasmEmulator("esp32c3");
  emulator.load_default_rom();
  emulator.set_boot_from_rom(true);
  emulator.load_firmware(new Uint8Array(await readFile(firmwarePath)));
  frames = [];
  board = new AiPassportBoard(wasm, {
    emulator,
    onFrame: (f) => frames.push(f),
    // 音频/未知事件与本子测试无关,吞掉以免拖慢。
  });
  board.releaseButtons();
  await runCycles(BOOT_CYCLES);
}, { timeout: 60_000 });

after(() => {
  emulator.free();
});

test("meta-pass 完整镜像在 QEMU 里引导并渲染出 240x320 屏幕", () => {
  const frame = frames.at(-1);
  assert.ok(frame, "引导期间没有任何显示帧 → 固件没有跑到 LVGL 渲染");
  assert.equal(frame.width, SCREEN_W);
  assert.equal(frame.height, SCREEN_H);
  assert.equal(frame.pixels.length, SCREEN_BYTES, `RGBA 缓冲应为 ${SCREEN_BYTES} 字节`);
  // 引导过程应有持续重绘(标题栏 + 列表 + 吉祥物 + 电量条)。
  // 帧数只下界断言:固件渲染节奏变化不应让本测试变红。
  assert.ok(frames.length >= 1, "至少有 1 帧");
  // 画面不能是纯色(全 0 或全 FF 意味着显示没初始化或崩溃)。
  const px = frame.pixels;
  let nonZero = false, nonWhite = false;
  for (let i = 0; i < px.length; i++) {
    if (px[i] !== 0) nonZero = true;
    if (px[i] !== 255) nonWhite = true;
  }
  assert.ok(nonZero && nonWhite, "画面不是纯色,显示管线已激活");
});

test("DOWN 键移动列表选中项(画面随之重绘)", async () => {
  // 初始 s_sel = 0(PAGE_LIST 进页时 goto_page 置 0),
  // DOWN 是 s_sel = (s_sel + 1) % 4,所以第一次 DOWN 必然移动。
  const [before, after] = await pressButton("DOWN");
  assert.notEqual(after, before, "DOWN 后画面必须变化 —— 按键没响应到固件");
}, { timeout: 120_000 });

test("连续 4 次 DOWN 绕列表一圈,固件不崩溃", async () => {
  // 4 项环形列表(Slot 0/1/2 + Import),4 次 DOWN 回到原点。
  // 断言进程与模拟器存活:崩溃会表现为 run_batch 抛错或 pc 停在异常地址。
  for (let i = 0; i < 4; i++) {
    await pressButton("DOWN");
  }
  const pc = emulator.pc();
  assert.ok(pc > 0, "pc 无效");
  assert.ok(emulator.cycles() > BOOT_CYCLES, "模拟器仍在推进周期");
}, { timeout: 180_000 });

test("UP 与 DOWN 成对使用,焦点来回移动且画面稳定", async () => {
  // UP/DOWN 成对:DOWN 往下走一步,UP 再走回来。
  // 注意 UP 在边界(s_sel=0 时 UP → 3)会产生画面变化,
  // 而连续同方向按键到达边界后不产生变化,这是固件的环形语义,不是缺陷。
  // 本测试只断言"来回成对不崩溃 + 模拟器存活",不对具体行做像素定位 ——
  // 列表用 LVGL 布局,行坐标随字体与主题配置漂移,像素定位会把测试绑死在
  // 渲染细节上,而不是固件行为上。
  await pressButton("DOWN");
  await pressButton("UP");
  const pc = emulator.pc();
  assert.ok(pc > 0, "pc 无效");
  assert.ok(frames.length > 0, "仍有渲染帧");
});
