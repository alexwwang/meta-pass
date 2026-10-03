#!/usr/bin/env node
// tests/test_dynslot_pool.mjs —— install-slot/dynslot-pool.js 与 main/meta_carve.c
// 同值同算法验收(design §4.5 L4:手机提案必须与设备分配器逐位吻合,否则
// prepare 整体 400)。向量按 C 侧 meta_carve_need/meta_carve_place 实现逐行对照
// 推导,C 侧行为由 tests/test_meta_carve.c 同步钉死(同一组几何事实)。
//
// 运行:node tests/test_dynslot_pool.mjs(仓库根)。
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import path from "node:path";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pool = await import(path.join(ROOT, "install-slot", "dynslot-pool.js"));
const { POOL, POOL_TOTAL, META_SLOT_COUNT, carveNeed, appLimit,
        carvePlace, geomFromListing } = pool;

// ── 1. 池描述符 == main/meta_carve.h 常量(单一几何事实源) ─────────────────
{
  assert.deepEqual(POOL.seg, [
    { start: 0x180000, end: 0x356000 },   // META_POOL0_START/END(factory 尾 → cardid)
    { start: 0x360000, end: 0x7fe000 },   // META_POOL1_START/END(store 尾 → otadata)
  ]);
  assert.equal(POOL.minSlot, 0x20000);      // META_CARVE_MIN_SLOT(128KB)
  assert.equal(POOL.offsetAlign, 0x10000);  // META_CARVE_OFFSET_ALIGN(64KB)
  assert.equal(POOL.sizeGranule, 0x1000);   // META_CARVE_SIZE_GRANULE(4KB)
  assert.equal(POOL.tail, 0x1000);          // META_CARVE_TAIL(尾 sector)
  assert.equal(POOL.maxSlots, 8);           // META_CARVE_MAX_SLOTS
  assert.equal(META_SLOT_COUNT, 8);         // meta_slots.h META_SLOT_COUNT
  assert.equal(POOL_TOTAL, 6766592);        // meta_carve_pool_total(设计 §4.1)
  console.log("PASS 1: POOL descriptor matches meta_carve.h (total 6,766,592 B)");
}

// ── 2. carveNeed == meta_carve_need(max(minSlot, align4k(len+tail)), 溢出 0) ──
{
  assert.equal(carveNeed(1), 0x20000);            // min_slot 兜底
  assert.equal(carveNeed(0x1f000), 0x20000);      // len+tail 恰 = min_slot
  assert.equal(carveNeed(0x1ff000), 0x200000);    // len+tail 恰 = 4KB 粒度
  assert.equal(carveNeed(0x1ff001), 0x201000);    // 向上取整到 4KB
  assert.equal(carveNeed(0x200000), 0x201000);    // len+tail 已对齐
  assert.equal(carveNeed(400000), 0x63000);       // 中等镜像(测试 10b 同源)
  assert.equal(carveNeed(0x7ffff000), 0x80000000);// 顶格不溢出(C uint32 同值)
  assert.equal(carveNeed(0x7ffff001), 0);         // 溢出防护
  assert.equal(carveNeed(0), 0);
  assert.equal(carveNeed(-1), 0);
  assert.equal(carveNeed(1.5), 0);
  assert.equal(carveNeed(NaN), 0);
  console.log("PASS 2: carveNeed rounding + overflow guard (mirrors meta_carve_need)");
}

// ── 3. appLimit == meta_sign_app_limit(size − tail;storage → 0) ─────────────
{
  assert.equal(appLimit(0x20000), 0x1f000);
  assert.equal(appLimit(0x1d6000), 0x1d5000);     // legacy ota_0
  assert.equal(appLimit(0x29e000), 0x29d000);     // legacy ota_2
  assert.equal(appLimit(0x20000, "storage"), 0);  // L2 预留不可装
  assert.equal(appLimit(0x1000), 0);              // 恰 = tail
  assert.equal(appLimit(0x800), 0);
  console.log("PASS 3: appLimit = size − tail; storage/small → 0");
}

// ── 4. carvePlace == meta_carve_place(first-fit,池序/对齐/上限) ────────────
{
  // 空 carve → 池首(pool_0 起点,64KB 已对齐)。
  assert.deepEqual(carvePlace([], 0x20000), { index: 0, offset: 0x180000 });
  // legacy 三槽全占(ota_0 + ota_1 + ota_2 铺满两池)→ 无处可放。
  const legacy = [
    { offset: 0x180000, size: 0x1d6000 },
    { offset: 0x360000, size: 0x200000 },
    { offset: 0x560000, size: 0x29e000 },
  ];
  assert.equal(carvePlace(legacy, 0x20000), null);
  // 中间槽删除留洞(0x360000..0x560000):落洞,插入下标 1(洞前 1 槽)。
  const hole = [legacy[0], legacy[2]];
  assert.deepEqual(carvePlace(hole, 0x20000), { index: 1, offset: 0x360000 });
  // pool_0 放不下的大槽 → pool_1 首(池序:pool_0 优先)。
  assert.deepEqual(carvePlace([], 0x1d7000), { index: 0, offset: 0x360000 });
  // 占用尾非 64KB 对齐 → 下一候选向上对齐(0x198000 → 0x1A0000)。
  assert.deepEqual(carvePlace([{ offset: 0x180000, size: 0x18000 }], 0x20000),
                   { index: 1, offset: 0x1a0000 });
  // 尾随槽:新槽落在既有槽之后(offset 升序 → 追加下标)。
  assert.deepEqual(carvePlace([{ offset: 0x180000, size: 0x20000 }], 0x20000),
                   { index: 1, offset: 0x1a0000 });
  // 上限与参数门禁。
  const full8 = Array.from({ length: 8 }, (_, i) =>
    ({ offset: 0x180000 + i * 0x10000, size: 0x10000 }));
  assert.equal(carvePlace(full8, 0x20000), null);   // 8 槽满
  assert.equal(carvePlace([], 0x1f000), null);       // < minSlot
  assert.equal(carvePlace([], 0x20500), null);       // 非 4KB 粒度
  assert.equal(carvePlace([], 0x4a0000), null);      // 超两池(0x49E000)放不下
  assert.equal(carvePlace(null, 0x20000), null);
  console.log("PASS 4: carvePlace first-fit (pool order / align / hole / limits)");
}

// ── 5. geomFromListing:清单 → 声称表 + 提案(§4.5 手机侧提案) ─────────────
{
  // 全新设备(空 carve):全走提案,插入下标 0。
  const fresh = geomFromListing({ count: 0, free: POOL_TOTAL, slots: [] }, 100000);
  assert.deepEqual(fresh.current, []);
  assert.ok(fresh.proposal);
  assert.equal(fresh.proposal.slot, 0);
  assert.equal(fresh.proposal.carveOffset, 0x180000);
  assert.equal(fresh.proposal.carveSize, carveNeed(100000));
  assert.equal(fresh.proposal.limit, carveNeed(100000) - 0x1000);
  assert.equal(fresh.suggestedSlot, 0);
  assert.deepEqual(fresh.placed,
    [{ slot: 0, limit: fresh.proposal.limit, fit: true }]);

  // storage 槽:占空间但不进声称表(limit 0 会被设备解析拒);提案跳过它。
  const withStorage = geomFromListing({ count: 2, free: 0, slots: [
    { slot: 0, state: "empty", name: "", size: 0x20000, len: 0,
      limit: 0, offset: 0x180000, kind: "storage" },
    { slot: 1, state: "valid", name: "x", size: 0x40000, len: 1,
      limit: 0x3f000, offset: 0x360000, kind: "app" },
  ] }, 0x10000);
  assert.deepEqual(withStorage.current, [{ slot: 1, limit: 0x3f000, fit: true }]);
  assert.equal(withStorage.suggestedSlot, 1);      // 现有可装优先(免新建/免重启)
  assert.ok(withStorage.proposal);
  assert.equal(withStorage.proposal.carveOffset, 0x1a0000);  // 跳过 storage 占位

  // 镜像非法(NaN):不声称、不提案、无建议。
  const bad = geomFromListing({ count: 1, free: 0, slots: [
    { slot: 0, state: "valid", name: "", size: 0x40000, len: 1,
      limit: 0x3f000, offset: 0x180000, kind: "app" },
  ] }, NaN);
  assert.deepEqual(bad.current.map((s) => s.fit), [false]);
  assert.equal(bad.proposal, null);
  assert.equal(bad.suggestedSlot, -1);

  // 8 槽满 + 全装不下:无处新建,建议 -1(-picker 据此报空间不足)。
  const eight = Array.from({ length: 8 }, (_, i) => ({
    slot: i, state: "empty", name: "", size: 0x40000, len: 0, limit: 0x3f000,
    offset: (i < 4 ? 0x180000 + i * 0x10000 : 0x360000 + (i - 4) * 0x10000),
    kind: "app",
  }));
  const maxed = geomFromListing({ count: 8, free: 0, slots: eight }, 0x3f001);
  assert.deepEqual(maxed.current.map((s) => s.fit), Array(8).fill(false));
  assert.equal(maxed.proposal, null);
  assert.equal(maxed.suggestedSlot, -1);
  console.log("PASS 5: geomFromListing — fresh/storage/invalid/full listing branches");
}

// ── 6. P1-4:数据 carve 记录计入占用域(槽位∪数据,与设备分配器同域) ─────────
{
  // 池首 0x180000 已有一条数据记录占 0x20000 → 新槽提案必须避让,落在其后。
  const listing = {
    count: 0, free: 0, slots: [],
    data: [{ offset: 0x180000, size: 0x20000, state: 1 }],
  };
  const g = geomFromListing(listing, 0x20000);
  assert.ok(g.proposal);
  assert.equal(g.proposal.carveOffset, 0x1a0000);   // 数据记录之后,非池首
  assert.equal(g.proposal.slot, 0);                  // 无 APP 槽 → 插入下标 0

  // 数据记录占满 pool_0 前半段 → 提案落 pool_1 首。
  const g2 = geomFromListing({
    count: 0, free: 0, slots: [],
    data: [{ offset: 0x180000, size: 0x1d6000, state: 2 }],
  }, 0x20000);
  assert.equal(g2.proposal.carveOffset, 0x360000);

  // 无 data 字段(旧固件)→ 行为与 P1-4 前一致(仅槽位占用)。
  const g3 = geomFromListing({ count: 0, free: 0, slots: [] }, 0x20000);
  assert.equal(g3.proposal.carveOffset, 0x180000);

  console.log("PASS 6: geomFromListing counts data-carve occupancy (P1-4)");
}

console.log("ALL dynslot-pool TESTS PASSED");
