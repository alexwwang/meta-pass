#!/usr/bin/env node
// tools/install-slot/test-dynslot-record.mjs —— USB 安装页 dynslot 记录编解码 /
// 分配规划 / 视图模型 / mock 设备交互的 host 验收(设计 §11 T1–T6)。
//
// 为什么这些断言必须是"逐字节":页面在 ROM 下载模式下自己改写 store 里的 carve
// 记录与 0x8000 表,任何偏移/填充/CRC 的偏差都会被设备拒收(CRC / 结构 / 表 MD5),
// 而失败现场只在真机上出现。字节权威 = 设备侧 C 代码与 gen_esp32part.py 的
// 黄金产物(tests/fixtures),与 tests/test_meta_carve.c 用的是同一组黄金表。
//
// 夹具重新生成(见 tests/fixtures/gen_carve_record.c 头部注释):
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/fixtures/gen_carve_record.c \
//      main/meta_carve_store.c main/meta_carve.c main/meta_md5.c -o /tmp/gen_rec
//   /tmp/gen_rec tests/fixtures/carve_record_golden.bin \
//                tests/fixtures/carve_record_v1_golden.bin
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const rec = await import(path.join(ROOT, "install-slot", "dynslot-record.js"));
const mock = await import(path.join(ROOT, "install-slot", "mock-device.js"));
const lu = await import(path.join(ROOT, "install-slot", "launcher-upgrade.js"));
const pool = await import(path.join(ROOT, "install-slot", "dynslot-pool.js"));

const fixture = (name) => new Uint8Array(readFileSync(path.join(ROOT, "tests", "fixtures", name)));

function assertBytes(got, want, label) {
  const a = Buffer.from(got);
  const b = Buffer.from(want);
  if (a.equals(b)) return;
  let i = 0;
  while (i < Math.min(a.length, b.length) && a[i] === b[i]) i++;
  assert.fail(
    `${label}: byte mismatch at offset ${i} (len ${a.length} vs ${b.length})\n` +
    `  got : ${a.subarray(Math.max(0, i - 8), i + 16).toString("hex")}\n` +
    `  want: ${b.subarray(Math.max(0, i - 8), i + 16).toString("hex")}`,
  );
}

const u8ToBin = (u8) => {
  const CHUNK = 0x8000;
  const parts = [];
  for (let i = 0; i < u8.length; i += CHUNK) {
    parts.push(String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + CHUNK, u8.length))));
  }
  return parts.join("");
};

const hex = (n) => "0x" + n.toString(16);
const emptySlot = (offset, size, extra = {}) => ({
  kind: rec.SLOT_KIND.APP, state: rec.SLOT_STATE.EMPTY,
  offset, size, imageLen: 0, playId: 0, sha: new Uint8Array(32), name: "",
  ...extra,
});

// ── 1. CRC32 黄金向量(C 头文件同一断言:"123456789" == 0xCBF43926) ──────────
{
  assert.equal(rec.crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
  console.log("PASS 1: crc32 golden vector");
}

// ── 2. 表物化黄金字节对拍(= tests/test_meta_carve.c 的同一组夹具) ──────────
{
  assertBytes(
    rec.materializeTable({ slots: [emptySlot(0x180000, 0x1d6000), emptySlot(0x360000, 0x200000),
      emptySlot(0x560000, 0x29e000)], data: [] }),
    fixture("carve_migration_table.bin"),
    "materialize(legacy-seeded 3 slots) == carve_migration_table.bin",
  );
  assertBytes(
    rec.materializeTable({ slots: [emptySlot(0x180000, 0x80000), emptySlot(0x360000, 0x200000)], data: [] }),
    fixture("carve_shrunk_table.bin"),
    "materialize(shrunk carve) == carve_shrunk_table.bin",
  );
  assertBytes(mock.safeTableBytes(), fixture("safe_table.bin"), "mock safe table == safe_table.bin");
  assertBytes(mock.legacyTableBytes(), fixture("legacy_table.bin"), "mock legacy table == legacy_table.bin");

  // 数据条目也要进表（标签/subtype 原样），且按 offset 升序合并。
  const withData2 = {
    slots: [emptySlot(0x180000, 0x40000)],
    data: [{ playId: 42, offset: 0x1c0000, size: 0x10000, state: 0, subtype: 0x41, type: 1, label: "assets" }],
  };
  assert.equal(rec.carveValid(withData2), true);
  assert.equal(
    rec.materializeTable({
      slots: [],
      data: [{ playId: 42, offset: 0x10000, size: 0x10000, state: 0, subtype: 0x41, type: 1, label: "assets" }],
    }),
    null,
    "data outside the pools is rejected (pool_of)",
  );
  const t2 = rec.materializeTable(withData2);
  const ents = [];
  for (let o = 0; o + 32 <= t2.length; o += 32) {
    const magic = t2[o] | (t2[o + 1] << 8);
    if (magic === 0xebeb) break;
    ents.push(String.fromCharCode(...t2.subarray(o + 12, o + 28)).replace(/\0.*$/, ""));
  }
  assert.deepEqual(ents, ["nvs", "phy_init", "factory", "ota_0", "assets", "cardid", "store", "otadata"],
    "entries merged by ascending offset with data entry interleaved");
  assert.ok(rec.checkTable(t2), "materialized table passes meta_pt_check rules");
  console.log("PASS 2: materialize == gen_esp32part goldens (migration/shrunk/safe/legacy + data entry)");
}

// ── 3. v2 黄金记录:decode 字段 + 再 encode 逐字节还原(C↔JS 编码等价) ──────
{
  const golden = fixture("carve_record_golden.bin");
  const d = rec.decodeRecord(golden);
  assert.ok(d, "v2 golden decodes");
  assert.equal(d.version, 2);
  assert.equal(d.seq, 42);
  assert.equal(d.slots.length, 2);
  assert.equal(d.data.length, 1);

  const [s0, s1] = d.slots;
  assert.equal(s0.state, rec.SLOT_STATE.VALID);
  assert.equal(s0.kind, rec.SLOT_KIND.APP);
  assert.equal(s0.offset, 0x180000);
  assert.equal(s0.size, 0x1d6000);
  assert.equal(s0.imageLen, 0x1a2b3c);
  assert.equal(s0.playId, 255);
  assert.equal(s0.name, "Demo Play");
  assert.equal(s0.sha[0], 1);
  assert.equal(s0.sha[1], 4);
  assert.equal(s0.sha[31], (31 * 3 + 1) & 0xff);
  assert.equal(s1.state, rec.SLOT_STATE.EMPTY);
  assert.equal(s1.offset, 0x360000);
  assert.equal(s1.size, 0x200000);

  const d0 = d.data[0];
  assert.deepEqual(
    { playId: d0.playId, offset: d0.offset, size: d0.size, state: d0.state, subtype: d0.subtype, type: d0.type, label: d0.label },
    { playId: 42, offset: 0x700000, size: 0x10000, state: rec.DATA_STATE.ARCHIVED, subtype: 0x41, type: 1, label: "assets" },
  );

  // 物化(carve) == 记录内嵌表:JS 表构建器与 C 逐字节一致。
  assertBytes(rec.materializeTable({ slots: d.slots, data: d.data }), d.table,
    "JS materialize == C-encoded embedded table");

  // 再编码逐字节还原:JS encodeRecord == meta_carve_rec_encode。
  const re = rec.encodeRecord({ seq: d.seq, slots: d.slots, data: d.data });
  assertBytes(re, golden, "JS re-encode == C encodeRecord output");
  assert.equal(rec.crc32(re.subarray(0, rec.REC_CRC_OFF)),
    (re[rec.REC_CRC_OFF] | (re[rec.REC_CRC_OFF + 1] << 8) |
     (re[rec.REC_CRC_OFF + 2] << 16) | (re[rec.REC_CRC_OFF + 3] << 24)) >>> 0);
  console.log("PASS 3: v2 golden record decode + byte-exact re-encode");
}

// ── 4. v1 只读兼容(已部署设备的老记录) ──────────────────────────────────────
{
  const v1 = fixture("carve_record_v1_golden.bin");
  const d = rec.decodeRecord(v1);
  assert.ok(d, "v1 record decodes (read-compat path)");
  assert.equal(d.version, 1);
  assert.equal(d.seq, 7);
  assert.equal(d.slots.length, 2);
  assert.equal(d.data.length, 0, "v1 has no data section");
  assert.equal(d.slots[0].playId, 0, "v1 has no play_id field → 0");
  assert.equal(d.slots[0].offset, 0x180000);
  assert.equal(d.slots[0].size, 0x1d6000);
  assert.equal(d.slots[0].imageLen, 0x1a2b3c);
  assert.equal(d.slots[0].name, "Demo Play");
  assert.equal(d.slots[0].sha[0], 1);
  assert.equal(d.slots[1].state, rec.SLOT_STATE.EMPTY);
  assertBytes(rec.materializeTable({ slots: d.slots, data: [] }), d.table,
    "v1 embedded table == JS materialize");
  // 页面永远写出 v2:同一 carve 用 v2 编码必须可解码且字段等价。
  const asV2 = rec.encodeRecord({ seq: 8, slots: d.slots, data: [] });
  const back = rec.decodeRecord(asV2);
  assert.ok(back && back.version === 2 && back.slots[0].name === "Demo Play");
  console.log("PASS 4: v1 read-compat decode (legacy device records survive)");
}

// ── 5. 拒收路径(magic / 版本 / CRC / count / state / seq / 表 MD5) ──────────
{
  const golden = fixture("carve_record_golden.bin");
  const patch = (mutate) => {
    const b = golden.slice();
    mutate(b);
    // 重新计算 CRC（篡改点之外的一切保持原样）
    const crc = rec.crc32(b.subarray(0, rec.REC_CRC_OFF));
    b[rec.REC_CRC_OFF] = crc & 0xff;
    b[rec.REC_CRC_OFF + 1] = (crc >>> 8) & 0xff;
    b[rec.REC_CRC_OFF + 2] = (crc >>> 16) & 0xff;
    b[rec.REC_CRC_OFF + 3] = (crc >>> 24) & 0xff;
    return b;
  };

  // 坏 magic
  let b = golden.slice(); b[0] = 0x00;
  assert.equal(rec.decodeRecord(b), null, "bad magic rejected");
  assert.equal(rec.rawRecordInfo(b), null, "bad magic rejected (raw info)");

  // 只翻一个字节不改 CRC → CRC 拒
  b = golden.slice(); b[0x40] ^= 0x01;
  assert.equal(rec.decodeRecord(b), null, "CRC mismatch rejected");

  // 未知版本
  b = patch((x) => { x[4] = 7; x[5] = 0; });
  assert.equal(rec.decodeRecord(b), null, "unknown version rejected");

  // seq 非法
  b = patch((x) => { x[8] = 0; x[9] = 0; x[10] = 0; x[11] = 0; });
  assert.equal(rec.decodeRecord(b), null, "seq 0 rejected");
  b = patch((x) => { x[8] = 0xff; x[9] = 0xff; x[10] = 0xff; x[11] = 0xff; });
  assert.equal(rec.decodeRecord(b), null, "seq 0xFFFFFFFF rejected");

  // slot_count > 8
  b = patch((x) => { x[6] = 9; x[7] = 0; });
  assert.equal(rec.decodeRecord(b), null, "slot_count 9 rejected");

  // state 越界
  b = patch((x) => { x[rec.REC_HEADER] = 3; });
  assert.equal(rec.decodeRecord(b), null, "slot state 3 rejected");

  // data_count > 8
  b = patch((x) => { x[12] = 9; x[13] = 0; });
  assert.equal(rec.decodeRecord(b), null, "data_count 9 rejected");

  // 表字节篡改 + CRC 修正 → 表 MD5 拦截(表与 carve 分歧的防线)
  b = patch((x) => { x[rec.REC_TABLE_OFF + 8] ^= 0x01; });
  assert.equal(rec.decodeRecord(b), null, "embedded table MD5 mismatch rejected");

  // 撕裂写(CRC 区保持擦除态)→ 拒,旧 A/B 胜(§4.7)
  b = golden.slice(); b.fill(0xff, rec.REC_CRC_OFF, rec.REC_CRC_OFF + 4);
  assert.equal(rec.decodeRecord(b), null, "torn record (CRC erased) rejected");

  console.log("PASS 5: rejection paths (magic/version/seq/count/state/CRC/table-MD5/torn)");
}

// ── 6. A/B 选取(新者胜 / 回绕 / 轮转 / MPCK 凭据护栏) ───────────────────────
{
  const FF = new Uint8Array(4096).fill(0xff);
  const mk = (seq) => rec.encodeRecord({ seq, slots: [], data: [] });
  const cred = (() => {
    const b = new Uint8Array(4096).fill(0xff);
    b.set([0x4d, 0x50, 0x43, 0x4b], 0);   // MPCK
    return b;
  })();

  let p = rec.pickRecord(FF, FF);
  assert.equal(p.rec, null, "both invalid → no record");
  assert.equal(p.targetSector, rec.STORE_SECTOR_A, "fresh device targets sector A");
  assert.equal(p.credInA, false);

  p = rec.pickRecord(mk(42), FF);
  assert.equal(p.rec.seq, 42);
  assert.equal(p.fromA, true);
  assert.equal(p.targetSector, rec.STORE_SECTOR_B, "commit rotates to B");

  p = rec.pickRecord(FF, mk(43));
  assert.equal(p.rec.seq, 43);
  assert.equal(p.fromA, false);
  assert.equal(p.targetSector, rec.STORE_SECTOR_A);

  p = rec.pickRecord(mk(42), mk(43));
  assert.equal(p.rec.seq, 43, "newest seq wins");
  assert.equal(p.targetSector, rec.STORE_SECTOR_A);

  // 回绕:1 新于 0xFFFFFFFE(带符号差)
  p = rec.pickRecord(mk(0xfffffffe), mk(1));
  assert.equal(p.rec.seq, 1, "seq wrap-around comparison");

  // E1:扇区 A 还是 MPCK 凭据备份 → 绝不覆盖,改写 B
  p = rec.pickRecord(cred, FF);
  assert.equal(p.rec, null, "MPCK is not a record");
  assert.equal(p.credInA, true);
  assert.equal(p.targetSector, rec.STORE_SECTOR_B, "credential backup in A → write B");
  assert.equal(rec.isCredentialBackup(cred), true);
  assert.equal(rec.isCredentialBackup(mk(42)), false);

  console.log("PASS 6: pickRecord (A/B newest-wins, wrap, rotation, MPCK guard)");
}

// ── 7. carve 合法性(= meta_carve_valid 的关键否决) ──────────────────────────
{
  const good = { slots: [emptySlot(0x180000, 0x40000)], data: [] };
  assert.equal(rec.carveValid(good), true);
  assert.equal(rec.carveValid({ slots: [emptySlot(0x180001, 0x40000)], data: [] }), false, "64KB alignment");
  assert.equal(rec.carveValid({ slots: [emptySlot(0x180000, 0x1f000)], data: [] }), false, "min slot 128KB");
  assert.equal(rec.carveValid({ slots: [emptySlot(0x180000, 0x40001)], data: [] }), false, "4KB granule");
  assert.equal(rec.carveValid({ slots: [emptySlot(0x10000, 0x40000)], data: [] }), false, "inside factory");
  assert.equal(rec.carveValid({ slots: [emptySlot(0x350000, 0x40000)], data: [] }), false, "crosses cardid");
  assert.equal(rec.carveValid({
    slots: [emptySlot(0x180000, 0x40000), emptySlot(0x1a0000, 0x40000)], data: [],
  }), false, "overlap rejected");
  assert.equal(rec.carveValid({
    slots: [emptySlot(0x360000, 0x40000), emptySlot(0x180000, 0x40000)], data: [],
  }), false, "descending offsets rejected");
  assert.equal(rec.carveValid({ slots: Array.from({ length: 9 }, (_, i) =>
    emptySlot(0x180000 + i * 0x40000, 0x40000)), data: [] }), false, ">8 slots rejected");

  const d = (over) => ({ playId: 42, offset: 0x200000, size: 0x10000, state: 0, subtype: 0x41, type: 1, label: "assets", ...over });
  assert.equal(rec.carveValid({ slots: [], data: [d({})] }), true);
  assert.equal(rec.carveValid({ slots: [], data: [d({ playId: 0 })] }), false, "play_id 0 rejected");
  assert.equal(rec.carveValid({ slots: [], data: [d({ subtype: 0 })] }), false, "DATA_OTA rejected");
  assert.equal(rec.carveValid({ slots: [], data: [d({ type: 0 })] }), false, "app-type data rejected");
  assert.equal(rec.carveValid({ slots: [], data: [d({ label: "store" })] }), false, "reserved label rejected");
  assert.equal(rec.carveValid({ slots: [], data: [d({ label: "x".repeat(17) })] }), false, "label >16 rejected");
  assert.equal(rec.carveValid({ slots: [], data: [d({ offset: 0x10000 })] }), false, "data outside pool rejected");
  assert.equal(rec.carveValid({
    slots: [], data: [d({}), d({ subtype: 0x41, label: "assets" })],
  }), false, "(label,subtype) duplicate rejected");
  assert.equal(rec.carveValid({
    slots: [emptySlot(0x1f0000, 0x40000)], data: [d({})],
  }), false, "data overlapping slot rejected");
  console.log("PASS 7: carveValid rules (alignment/pool/overlap/count/data)");
}

// ── 8. 分配器(= meta_carve_place:first-fit,上限只看槽位数) ──────────────────
{
  // 空池:落 pool_0 池首。
  let p = rec.placeNewSlot({ slots: [], data: [] }, 0x20000);
  assert.deepEqual(p, { index: 0, offset: 0x180000, size: 0x20000 });

  // 数据记录占用池首 → 落其后(与 tests/test_dynslot_pool.mjs PASS 6 同向量)。
  p = rec.placeNewSlot({
    slots: [],
    data: [{ playId: 42, offset: 0x180000, size: 0x20000, state: 0, subtype: 0x41, type: 1, label: "assets" }],
  }, 0x20000);
  assert.equal(p.offset, 0x1a0000, "first-fit avoids data carve");
  assert.equal(p.index, 0);

  // 两池都满 → 拒。
  const full = { slots: [emptySlot(0x180000, 0x1d6000), emptySlot(0x360000, 0x49e000)], data: [] };
  assert.equal(rec.placeNewSlot(full, 0x20000), null, "no gap → null");

  // 8 槽上限(按槽位数,不按占用域长度)。
  const eight = { slots: Array.from({ length: 8 }, (_, i) =>
    emptySlot(i < 4 ? 0x180000 + i * 0x20000 : 0x360000 + (i - 4) * 0x20000, 0x20000)), data: [] };
  assert.equal(rec.placeNewSlot(eight, 0x20000), null, "8 slots cap");

  // 7 槽 + 2 数据(占用域 9 条):设备按槽位数放行,JS 镜像必须一致。
  const seven = {
    slots: [
      emptySlot(0x180000, 0x20000), emptySlot(0x1a0000, 0x20000),
      emptySlot(0x1c0000, 0x20000), emptySlot(0x1e0000, 0x20000),
      emptySlot(0x360000, 0x20000), emptySlot(0x380000, 0x20000),
      emptySlot(0x3a0000, 0x20000),
    ],
    data: [
      { playId: 42, offset: 0x400000, size: 0x10000, state: 0, subtype: 0x41, type: 1, label: "assets" },
      { playId: 43, offset: 0x420000, size: 0x10000, state: 0, subtype: 0x42, type: 1, label: "save" },
    ],
  };
  assert.equal(rec.carveValid(seven), true);
  p = rec.placeNewSlot(seven, 0x20000);
  assert.ok(p, "7 slots + 2 data still places (device rule: slot count only)");
  assert.equal(p.offset, 0x200000);
  assert.equal(p.index, 4, "insert index = slots with smaller offset");

  // 非法参数
  assert.equal(rec.placeNewSlot({ slots: [], data: [] }, 0x1f000), null, "below minSlot");
  assert.equal(rec.placeNewSlot({ slots: [], data: [] }, 0x20001), null, "granule");
  assert.equal(rec.placeNewSlot({ slots: [], data: [] }, 0x20000 + 1), null, "granule 2");
  console.log("PASS 8: placeNewSlot (first-fit / data avoidance / caps / device slot-count rule)");
}

// ── 9. planInstall / planRemove(页面动作的决策源) ──────────────────────────
{
  const carve = {
    slots: [emptySlot(0x180000, 0x40000, { state: rec.SLOT_STATE.VALID, name: "Old", imageLen: 0x30000 }),
      emptySlot(0x360000, 0x200000)],
    data: [],
  };

  // 复用:空槽装得下 → reuse
  let pl = rec.planInstall(carve, 0x100000, { target: "slot", index: 1 });
  assert.equal(pl.action, rec.PLAN.REUSE);
  assert.equal(pl.index, 1);
  assert.equal(pl.offset, 0x360000);
  assert.equal(pl.limit, 0x1ff000, "limit = size - 4KB tail");

  // 显式选中已占用槽 = 覆盖安装(允许,状态不挡)
  pl = rec.planInstall(carve, 0x20000, { target: "slot", index: 0 });
  assert.equal(pl.action, rec.PLAN.REUSE);
  assert.equal(pl.index, 0);

  // 装不下 → too_large 带上限数字
  pl = rec.planInstall(carve, 0x200000, { target: "slot", index: 0 });
  assert.equal(pl.action, rec.PLAN.NONE);
  assert.equal(pl.reason, "too_large");
  assert.equal(pl.limit, 0x3f000);

  // 自动分配 → create(第一个空隙)
  pl = rec.planInstall(carve, 0x100000, { target: "auto" });
  assert.equal(pl.action, rec.PLAN.CREATE);
  assert.equal(pl.offset, 0x1c0000, "after slot0 (0x180000+0x40000, 64KB aligned)");
  assert.equal(pl.index, 1, "insert before slot1");
  assert.equal(pl.size, 0x101000);
  assert.equal(pl.limit, 0x100000);

  // 无图
  pl = rec.planInstall(carve, 0, { target: "auto" });
  assert.equal(pl.action, rec.PLAN.NONE);
  assert.equal(pl.reason, "no_image");

  // 池满 → no_space 带诊断数字
  const full = { slots: [emptySlot(0x180000, 0x1d6000), emptySlot(0x360000, 0x49e000)], data: [] };
  pl = rec.planInstall(full, 0x20000, { target: "auto" });
  assert.equal(pl.action, rec.PLAN.NONE);
  assert.equal(pl.reason, "no_space");
  assert.equal(pl.need, 0x21000, "carveNeed(0x20000) = align4k(0x20000+tail)");
  assert.equal(pl.largestGap, 0);
  assert.equal(pl.totalFree, 0);

  // storage 槽不可作安装目标
  const stor = { slots: [emptySlot(0x180000, 0x40000, { kind: rec.SLOT_KIND.STORAGE })], data: [] };
  pl = rec.planInstall(stor, 0x20000, { target: "slot", index: 0 });
  assert.equal(pl.action, rec.PLAN.NONE);
  assert.equal(pl.reason, "bad_slot");

  // 删除:数组压缩 + 数据记录保留 + 越界拒
  const rm = rec.planRemove(carve, 0);
  assert.ok(rm);
  assert.equal(rm.removed.offset, 0x180000);
  assert.equal(rm.carve.slots.length, 1);
  assert.equal(rm.carve.slots[0].offset, 0x360000);
  assert.equal(rec.planRemove(carve, 5), null, "out-of-range remove rejected");
  assert.equal(rec.planRemove({ slots: [], data: [] }, 0), null, "empty carve remove rejected");
  console.log("PASS 9: planInstall / planRemove (reuse-first, create, diagnostics, storage guard)");
}

// ── 10. 视图模型(渲染与动作同一决策源,BUG-20) ──────────────────────────────
{
  const carve = {
    slots: [
      emptySlot(0x180000, 0x1d6000, { state: rec.SLOT_STATE.VALID, name: "Demo Play", imageLen: 4144 }),
      emptySlot(0x360000, 0x200000),
    ],
    data: [{ playId: 42, offset: 0x700000, size: 0x10000, state: rec.DATA_STATE.ARCHIVED, subtype: 0x41, type: 1, label: "assets" }],
  };
  const imageLen = 0x100000;
  const view = rec.buildSlotView({ carve, imageLen, mode: rec.MODE.DYN_SLOT });

  assert.equal(view.dyn, true);
  assert.equal(view.rows.length, 2);
  assert.equal(view.rows[0].state, "valid");
  assert.equal(view.rows[0].name, "Demo Play");
  assert.equal(view.rows[0].occupied, true);
  assert.equal(view.rows[0].targetable, true, "explicit overwrite allowed");
  assert.equal(view.rows[0].recommended, false, "occupied slot never auto-recommended");
  assert.equal(view.rows[1].state, "empty");
  assert.equal(view.rows[1].recommended, true, "reuse-first recommendation");
  assert.equal(view.rows[1].removeEnabled, true);

  // 摘要数字
  assert.equal(view.summary.count, 2);
  assert.equal(view.summary.usedSlots, 0x3d6000);
  assert.equal(view.summary.usedData, 0x10000);
  assert.equal(view.summary.total, 0x674000);
  assert.equal(view.summary.free, 0x28e000);
  assert.equal(view.summary.largestGap, 0x1a0000);

  // Auto 提案(有空槽可复用时:auto 可用但不被推荐)
  assert.equal(view.auto.enabled, true);
  assert.equal(view.auto.recommended, false);
  assert.equal(view.auto.offset, 0x560000, "first gap in pool_1 after empty slot");

  // 渲染行 ↔ 规划器一致性(同一决策源):推荐行必须真是 reuse,auto 必须真是 create
  for (const row of view.rows.filter((r) => r.recommended)) {
    const p = rec.planInstall(carve, imageLen, { target: "slot", index: row.slot });
    assert.equal(p.action, rec.PLAN.REUSE);
    assert.equal(p.offset, row.offset);
  }
  if (view.auto.enabled && view.auto.recommended) {
    const p = rec.planInstall(carve, imageLen, { target: "auto" });
    assert.equal(p.action, rec.PLAN.CREATE);
    assert.equal(p.offset, view.auto.offset);
  }
  assert.equal(view.rows.some((r) => r.recommended) !== view.auto.recommended, true,
    "exactly one recommendation target");

  // 未选镜像:auto 等图,行仍可选
  const noImg = rec.buildSlotView({ carve, imageLen: 0, mode: rec.MODE.DYN_SLOT });
  assert.equal(noImg.auto.enabled, false);
  assert.equal(noImg.auto.reason.code, "no_image");
  assert.equal(noImg.rows.every((r) => r.targetable), true);

  // 池满:auto 禁用并给数字
  const fullCarve = { slots: [emptySlot(0x180000, 0x1d6000), emptySlot(0x360000, 0x49e000)], data: [] };
  const fullView = rec.buildSlotView({ carve: fullCarve, imageLen: 0x20000, mode: rec.MODE.DYN_SLOT });
  assert.equal(fullView.auto.enabled, false);
  assert.deepEqual(fullView.auto.reason, { code: "no_space" });
  assert.equal(fullView.auto.need, 0x21000);
  assert.equal(fullView.auto.largestGap, 0);
  assert.equal(fullView.auto.recommended, false, "no recommendation when nothing fits");

  // legacy 模式:表派生行、无 auto、禁删除
  const legacyParts = lu.parsePartitionTable(mock.legacyTableBytes());
  const legacyView = rec.buildSlotView({
    carve: null, imageLen, mode: rec.MODE.LEGACY_FIXED,
    legacySlots: lu.discoverSlots(legacyParts),
  });
  assert.equal(legacyView.dyn, false);
  assert.equal(legacyView.rows.length, 3);
  assert.equal(legacyView.rows[0].offset, 0x180000);
  assert.equal(legacyView.rows[0].state, "unknown");
  assert.equal(legacyView.rows.every((r) => r.removeEnabled === false), true);
  assert.equal(legacyView.auto.enabled, false);
  assert.equal(legacyView.auto.reason.code, "legacy_mode");
  console.log("PASS 10: buildSlotView (rows/recommendation/auto/summary + action-source consistency)");
}

// ── 11. 模式判定(设计 §3) ──────────────────────────────────────────────────
{
  const golden = rec.decodeRecord(fixture("carve_record_golden.bin"));
  const safeParts = lu.parsePartitionTable(mock.safeTableBytes());
  const legacyParts = lu.parsePartitionTable(mock.legacyTableBytes());
  const carvedParts = lu.parsePartitionTable(rec.materializeTable({
    slots: [emptySlot(0x180000, 0x1d6000), emptySlot(0x360000, 0x200000)], data: [],
  }));

  assert.equal(rec.detectSlotMode({ record: golden, partitions: safeParts }), rec.MODE.DYN_SLOT);
  assert.equal(rec.detectSlotMode({ record: null, partitions: safeParts }), rec.MODE.DYN_FRESH);
  assert.equal(rec.detectSlotMode({ record: null, partitions: legacyParts }), rec.MODE.LEGACY_FIXED);
  assert.equal(rec.detectSlotMode({ record: null, partitions: [] }), rec.MODE.LEGACY_FALLBACK);
  // carved 表（含 store 条目）同样是 dynslot 模式
  assert.equal(rec.detectSlotMode({ record: null, partitions: carvedParts }), rec.MODE.DYN_FRESH);
  // MPCK 占着记录扇区（记录不可解码）+ dynslot 表 → 仍是 DYN_FRESH，绝不误判 legacy
  const mpck = new Uint8Array(4096).fill(0xff);
  mpck.set([0x4d, 0x50, 0x43, 0x4b], 0);
  assert.equal(rec.decodeRecord(mpck), null);
  assert.equal(rec.detectSlotMode({ record: null, partitions: safeParts }), rec.MODE.DYN_FRESH);
  console.log("PASS 11: detectSlotMode (record → store → ota_N → fallback)");
}

// ── 12. mock 设备交互往返(页面流程的字节级回放,设计 T6) ────────────────────
{
  const dev = mock.createMockDevice({ profile: "dyn" });
  const bin = (u8) => u8ToBin(u8);

  // 连接:读表 + 读 A/B 记录(页面连接流程的两步)
  const tableRaw = await dev.readFlash(mock.TABLE_OFFSET, 0x1000);
  const parts = lu.parsePartitionTable(tableRaw);
  const rawA = await dev.readFlash(rec.STORE_OFFSET, 4096);
  const rawB = await dev.readFlash(rec.STORE_OFFSET + 4096, 4096);
  const pick = rec.pickRecord(rawA, rawB);
  assert.ok(pick.rec, "mock seed record decodes");
  const mode = rec.detectSlotMode({ record: pick.rec, partitions: parts });
  assert.equal(mode, rec.MODE.DYN_SLOT);

  let carve = { slots: pick.rec.slots, data: pick.rec.data };
  const img = mock.makeFakeImage();
  const imageLen = img.length;
  assert.equal(imageLen, 4144, "synthetic image parses to a fixed length");

  let view = rec.buildSlotView({ carve, imageLen, mode });
  assert.equal(view.rows.length, 2);
  assert.equal(view.rows[0].name, "Demo Play");

  // ① 安装到空槽(reuse):写镜像 → 记录元数据提交
  let pl = rec.planInstall(carve, imageLen, { target: "slot", index: 1 });
  assert.equal(pl.action, rec.PLAN.REUSE);
  await dev.writeFlash({ fileArray: [{ data: bin(img), address: pl.offset }] });
  const sha = new Uint8Array(32).fill(0x5a);
  const afterReuse = {
    slots: carve.slots.map((s, i) => i === 1
      ? { ...s, state: rec.SLOT_STATE.VALID, imageLen, name: "New Play", sha } : s),
    data: carve.data,
  };
  let bytes = rec.encodeRecord({ seq: pick.rec.seq + 1, slots: afterReuse.slots, data: afterReuse.data });
  let addr = rec.STORE_OFFSET + pick.targetSector * 4096;
  await dev.writeFlash({ fileArray: [{ data: bin(bytes), address: addr }] });
  let back = rec.decodeRecord(await dev.readFlash(addr, 4096));
  assert.ok(back && back.seq === pick.rec.seq + 1, "record commit lands in target sector");
  assert.equal(back.slots[1].state, rec.SLOT_STATE.VALID);
  assert.equal(back.slots[1].name, "New Play");
  assert.equal(back.slots[1].imageLen, imageLen);
  carve = { slots: back.slots, data: back.data };

  // 几何未动 → 表不变(不重写)
  assertBytes(dev.readTable(), rec.materializeTable(carve), "table untouched after metadata-only commit");

  // ② 自动分配新槽(create):记录先写 → 表随后 → 再写镜像
  pl = rec.planInstall(carve, imageLen, { target: "auto" });
  assert.equal(pl.action, rec.PLAN.CREATE);
  assert.equal(pl.offset, 0x560000, "auto lands in the free pool_1 gap");
  const carved = rec.insertSlot(carve, pl.index, { offset: pl.offset, size: pl.size });
  assert.equal(rec.carveValid(carved), true);
  bytes = rec.encodeRecord({ seq: back.seq + 1, slots: carved.slots, data: carved.data });
  const nextSector = pick.targetSector ^ 1;
  addr = rec.STORE_OFFSET + nextSector * 4096;
  await dev.writeFlash({ fileArray: [{ data: bin(bytes), address: addr }] });
  const table2 = rec.materializeTable(carved);
  await dev.writeFlash({ fileArray: [{ data: bin(table2), address: mock.TABLE_OFFSET }] });
  // 读回校验(页面的 read-back 契约)
  assertBytes(await dev.readFlash(mock.TABLE_OFFSET, 0xc00), table2, "table read-back matches");
  back = rec.decodeRecord(await dev.readFlash(addr, 4096));
  assert.ok(back && back.slots.length === 3, "new slot visible in record");
  assert.equal(back.slots[2].state, rec.SLOT_STATE.EMPTY);
  assert.equal(back.slots[2].offset, 0x560000);
  assertBytes(dev.readTable(), table2, "device table == materialize(carve)");
  await dev.writeFlash({ fileArray: [{ data: bin(img), address: pl.offset }] });

  view = rec.buildSlotView({ carve: { slots: back.slots, data: back.data }, imageLen, mode });
  assert.equal(view.rows.length, 3, "UI list grows after dynamic allocation");
  assert.equal(view.summary.count, 3);

  // ③ 删除槽位:擦镜像头 → 记录去槽 → 表去槽 → 摘要空间回落
  const freeBefore = view.summary.free;
  const rm = rec.planRemove({ slots: back.slots, data: back.data }, 0);
  assert.ok(rm);
  await dev.writeFlash({
    fileArray: [{ data: "\xff".repeat(4096), address: 0x180000 }],
  });
  bytes = rec.encodeRecord({ seq: back.seq + 1, slots: rm.carve.slots, data: rm.carve.data });
  addr = rec.STORE_OFFSET + (nextSector ^ 1) * 4096;
  await dev.writeFlash({ fileArray: [{ data: bin(bytes), address: addr }] });
  await dev.writeFlash({ fileArray: [{ data: bin(rec.materializeTable(rm.carve)), address: mock.TABLE_OFFSET }] });
  const afterDel = rec.decodeRecord(await dev.readFlash(addr, 4096));
  assert.ok(afterDel && afterDel.slots.length === 2, "row gone from record");
  assert.equal(afterDel.slots.some((s) => s.offset === 0x180000), false);
  const delView = rec.buildSlotView({
    carve: { slots: afterDel.slots, data: afterDel.data }, imageLen, mode,
  });
  assert.equal(delView.summary.free, freeBefore + 0x1d6000, "freed bytes shown in summary");
  const entries = lu.parsePartitionTable(dev.readTable());
  assert.equal(entries.some((p) => p.label === "ota_0" && p.offset === 0x180000), false,
    "removed slot gone from live table");

  // ④ 三个 profile 的模式判定（mock 页面三种可点状态）
  const freshParts = lu.parsePartitionTable(mock.createMockDevice({ profile: "fresh" }).readTable());
  assert.equal(rec.detectSlotMode({ record: null, partitions: freshParts }), rec.MODE.DYN_FRESH);
  const legacyProfileParts = lu.parsePartitionTable(mock.createMockDevice({ profile: "legacy" }).readTable());
  assert.equal(rec.detectSlotMode({ record: null, partitions: legacyProfileParts }), rec.MODE.LEGACY_FIXED);
  assert.equal(rec.detectSlotMode({ record: afterDel, partitions: freshParts }), rec.MODE.DYN_SLOT,
    "record alone decides dynslot mode");
  assert.equal(mock.mockProfileFromLocation(new URL("http://x/?mock=1")), "dyn");
  assert.equal(mock.mockProfileFromLocation(new URL("http://x/?mock=fresh")), "fresh");
  assert.equal(mock.mockProfileFromLocation(new URL("http://x/")), null);

  console.log("PASS 12: mock device round trip (connect → install → allocate → remove)");
}

console.log("ALL dynslot-record TESTS PASSED");
