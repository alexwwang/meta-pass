// tools/install-slot/test-slot-backup.mjs —— slot-backup.js 的 Node 自检。
// 运行:/usr/local/bin/node tools/install-slot/test-slot-backup.mjs
// 覆盖:切片(固件/尾扇区/额外数据/空槽/坏镜像)、manifest 构建与解析、
//       恢复空间自检(自适应核心)、恢复顺序契约。
import assert from "node:assert/strict";
import {
  sliceSlotBackup, buildManifest, parseManifest, manifestFilesForSlot,
  checkRestoreFit, restoreOrder, isAllFF,
  firmwareFileName, extraFileName, tailFileName,
  MANIFEST_NAME, MANIFEST_VERSION,
} from "../../install-slot/slot-backup.js";

// 与 test-extract.mjs PASS 3c 同源 fixture:24+16B 扩展头,总长 256
function buildApp() {
  let total = 24 + 16 + 8 + 100 + 8 + 64;
  while (total % 16 !== 15) total++;
  total += 1 + 32;
  const b = new Uint8Array(total).fill(0xab);
  b[0] = 0xe9; b[1] = 2; b[12] = 5; b[13] = 0; b[23] = 1;
  b.set([0x00, 0x00, 0xc8, 0x3f, 100, 0, 0, 0], 40);   // seg0 @0x3fc80000 len 100
  b.set([0x20, 0x00, 0x00, 0x42, 64, 0, 0, 0], 148);   // seg1 @0x42000020 len 64
  return b;
}

// 1. 完整槽位切片:固件 + 尾扇区(含 MSIG) + 额外数据
{
  const app = buildApp();
  const slot = new Uint8Array(0x200000).fill(0xff);
  slot.set(app, 0);
  slot.set([0x4d, 0x53, 0x49, 0x47], 4096);   // "MSIG" at tail sector
  slot.set([1, 2, 3], 8192);                  // extra storage data
  const r = sliceSlotBackup(slot);
  assert.equal(r.imageLen, app.length);
  assert.equal(r.tailOffset, 4096);
  assert.equal(r.firmware.length, app.length);
  assert.ok(r.tail && r.tail.length === 4096 && r.tail[0] === 0x4d, "tail must carry MSIG");
  assert.ok(r.extra && r.extra.length > 0 && r.extra[0] === 1, "extra must be captured");
  console.log("PASS 1: full slot slices into firmware + tail(MSIG) + extra");
}

// 2. 空槽 → null;固件后无任何数据 → extra/tail 为 null
{
  assert.equal(sliceSlotBackup(new Uint8Array(0x200000).fill(0xff)), null, "empty slot must be null");
  const slot = new Uint8Array(0x200000).fill(0xff);
  slot.set(buildApp(), 0);
  const r = sliceSlotBackup(slot);
  assert.equal(r.tail, null, "erased tail must be omitted");
  assert.equal(r.extra, null, "erased extra must be omitted");
  console.log("PASS 2: empty slot -> null; erased tail/extra omitted");
}

// 3. 坏镜像(非空但 segment 表不可解析)→ BAD_IMAGE
{
  const bad = new Uint8Array(4096).fill(0x00);
  bad[0] = 0xe9; bad[1] = 2;
  assert.throws(() => sliceSlotBackup(bad), (e) => e.code === "BAD_IMAGE");
  console.log("PASS 3: non-image non-erased slot -> BAD_IMAGE");
}

// 4. manifest 构建/解析/按 slot 取文件 + 结构校验负例
{
  const entries = [
    { name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 256 },
    { name: extraFileName(0), sha256: "b".repeat(64), bytes: 3 },
    { name: tailFileName(0), sha256: "c".repeat(64), bytes: 4096 },
  ];
  const man = buildManifest({ slot: 0, entries, notes: "test" });
  const parsed = parseManifest(JSON.stringify(man));
  assert.equal(parsed.manifest_version, MANIFEST_VERSION);
  assert.equal(manifestFilesForSlot(parsed, 0).length, 3);
  assert.equal(manifestFilesForSlot(parsed, 1).length, 0, "absent slot -> empty files");
  assert.throws(() => parseManifest({ hello: 1 }), /kind/);
  assert.throws(() => parseManifest({ kind: "meta-pass-slot-backup", manifest_version: 99, slots: [{ slot: 0, files: [] }] }), /manifest_version/);
  assert.throws(() => parseManifest({ kind: "meta-pass-slot-backup", manifest_version: 1, slots: [{ slot: 0, files: [{ name: "x", sha256: "zz", bytes: 1 }] }] }), /sha256/);
  assert.ok(isAllFF(new Uint8Array(16).fill(0xff)) && !isAllFF(new Uint8Array([0, 0xff])));
  console.log("PASS 4: manifest build/parse roundtrip + malformed rejection");
}

// 5. 恢复空间自检(自适应):备份总长 ≤ 目标分区;固件必须给尾扇区留 4KB
{
  const small = [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 256 }];
  assert.equal(checkRestoreFit(small, 0x1D6000).ok, true, "small backup fits any slot");
  // 恰好塞满整个分区(不含尾扇区)→ 拒绝
  const full = [{ name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 0x1D6000 }];
  assert.equal(checkRestoreFit(full, 0x1D6000).ok, false, "no-room-for-tail must be rejected");
  // 总长超出分区 → 拒绝
  const over = [
    { name: firmwareFileName(0), sha256: "a".repeat(64), bytes: 0x1D6000 },
    { name: extraFileName(0), sha256: "b".repeat(64), bytes: 0x100000 },
  ];
  assert.equal(checkRestoreFit(over, 0x1D6000).ok, false, "oversize backup must be rejected");
  // 旧小分区备份装进大分区 → 通过(自适应方向)
  assert.equal(checkRestoreFit(small, 0x29E000).ok, true, "small backup into bigger slot fits");
  console.log("PASS 5: restore fit check (adaptive across slot sizes)");
}

// 6. 恢复顺序契约:firmware → extra → tail(tail 最后,防扇区重擦)
{
  const order = restoreOrder([
    { name: tailFileName(0) }, { name: extraFileName(0) }, { name: firmwareFileName(0) },
  ]).map((f) => f.name);
  assert.deepEqual(order, [firmwareFileName(0), extraFileName(0), tailFileName(0)]);
  const fwOnly = restoreOrder([{ name: tailFileName(1) }, { name: firmwareFileName(1) }]);
  assert.deepEqual(fwOnly.map((f) => f.name), [firmwareFileName(1), tailFileName(1)]);
  console.log("PASS 6: restore order firmware -> extra -> tail");
}

console.log("All slot-backup tests passed.");
