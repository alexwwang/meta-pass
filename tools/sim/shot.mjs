// 模拟器截图:固件引导 -> 列表页 -> framebuffer 落 BMP(24-bit)。
// 用法: node tools/sim/shot.mjs <out.bmp> [storeRecord.bin]
// 用 build/FoloToy-AI-Passport-full.bin 做 8MB 底图;给了 record 就 seed 到 0x35A000。
import { readFile, writeFile } from "node:fs/promises";
import { initSync, WasmEmulator } from "./esp_emu.js";
import { AiPassportBoard } from "./ai-passport-board.js";

const [outPath, recordPath] = process.argv.slice(2);
const W = 240, H = 320, BATCH = 50_000, BOOT_CYCLES = 1_200_000_000;

const wasm = initSync(await readFile(new URL("./esp_emu_bg.wasm", import.meta.url)));
const emulator = new WasmEmulator("esp32c3");
emulator.load_default_rom();
emulator.set_boot_from_rom(true);

const repo = new URL("../../", import.meta.url);
const image = new Uint8Array(await readFile(new URL("build/FoloToy-AI-Passport-full.bin", repo)));
if (recordPath) {
  const rec = new Uint8Array(await readFile(recordPath));
  image.set(rec, 0x35A000);
  console.error(`seeded store record ${rec.length}B @0x35A000`);
}
emulator.load_firmware(image);

let frame = null;
const board = new AiPassportBoard(wasm, { emulator, onFrame: (f) => { frame = f; } });
board.releaseButtons();
for (let n = 0; n < Math.ceil(BOOT_CYCLES / BATCH); n++) {
  emulator.run_batch(BATCH);
  board.drain();
}
if (!frame) { console.error("no frame"); process.exit(1); }

// RGBA -> 24-bit BMP(自底向上行序)
const rowSize = Math.ceil((W * 3) / 4) * 4;
const imgSize = rowSize * H;
const bmp = Buffer.alloc(54 + imgSize);
bmp.write("BM"); bmp.writeUInt32LE(54 + imgSize, 2); bmp.writeUInt32LE(54, 10);
bmp.writeUInt32LE(40, 14); bmp.writeInt32LE(W, 18); bmp.writeInt32LE(H, 22);
bmp.writeUInt16LE(1, 26); bmp.writeUInt16LE(24, 28); bmp.writeUInt32LE(imgSize, 34);
const px = frame.pixels;
for (let y = 0; y < H; y++) {
  const srcY = H - 1 - y;
  for (let x = 0; x < W; x++) {
    const si = (srcY * W + x) * 4, di = 54 + y * rowSize + x * 3;
    bmp[di] = px[si + 2]; bmp[di + 1] = px[si + 1]; bmp[di + 2] = px[si];
  }
}
await writeFile(outPath, bmp);
console.error(`saved ${outPath} (${54 + imgSize}B)`);
emulator.free();
