// install-slot/dynslot-pool.js —— dynslot 池几何与分配器(手机侧几何事实源)。
//
// 与 main/meta_carve.h/.c 的 meta_pool_desc_t / meta_carve_need / meta_carve_place
// 逐项同值同算法(设备端 meta_carve.h 注释明确约定:JS 侧本文件是同一份数值)。
// 设备在 prepare 时用同一分配器重跑提案、逐位吻合才放行(design §4.5 L4),
// 手机侧提案必须与设备端逐字节一致,否则整体 400 —— 改这里必须同步改
// meta_carve.c,并跑 tools/validate.sh(C 与 Node 两侧测试钉死同一组向量)。
//
// 零依赖纯函数:boot 页 module 与 Node 测试共用;不 import 任何本仓库模块
// (phone-install.js 反向 import 本文件,避免循环;legacy 三槽回退视图仍归
// store-analyze.js SLOT_GEOMETRY 管)。

// 池描述符(= meta_carve.h META_POOL0/1_* 与 META_CARVE_* 常量)。
export const POOL = {
  seg: [
    { start: 0x180000, end: 0x356000 },   // pool_0(factory 尾 → cardid)
    { start: 0x360000, end: 0x7fe000 },   // pool_1(store 尾 → otadata)
  ],
  minSlot: 0x20000,        // META_CARVE_MIN_SLOT(128KB)
  offsetAlign: 0x10000,    // META_CARVE_OFFSET_ALIGN(64KB)
  sizeGranule: 0x1000,     // META_CARVE_SIZE_GRANULE(4KB)
  tail: 0x1000,            // META_CARVE_TAIL(每槽尾 sector:MSIG/MNAM/MAEG)
  maxSlots: 8,             // META_CARVE_MAX_SLOTS
};

// 槽位下标上界(= 设备 meta_slots.h META_SLOT_COUNT):清单/提案/声称共用。
export const META_SLOT_COUNT = POOL.maxSlots;

// 两池总字节数(meta_carve_pool_total,6,766,592 B;设计 §4.1)。
export const POOL_TOTAL = POOL.seg.reduce((a, s) => a + (s.end - s.start), 0);

const alignUp = (v, a) => Math.ceil(v / a) * a;   // JS 安全整数:v < 2^53

// meta_carve_need 镜像:安装所需槽位尺寸 = max(minSlot, align4k(imageLen + tail));
// 非法/溢出(> 0x7FFFF000)→ 0(提案直接判不可用)。
export function carveNeed(imageLen) {
  if (!Number.isInteger(imageLen) || imageLen <= 0 || imageLen > 0x7ffff000) return 0;
  return Math.max(POOL.minSlot, alignUp(imageLen + POOL.tail, POOL.sizeGranule));
}

// meta_sign_app_limit 镜像:APP 槽可装上限 = 槽尺寸 − 尾 sector;
// storage 预留槽不可安装(L2)→ 0。
export function appLimit(slotSize, kind = "app") {
  if (kind !== "app" || !Number.isFinite(slotSize) || slotSize <= POOL.tail) return 0;
  return slotSize - POOL.tail;
}

// meta_carve_place 镜像:first-fit(pool_0 小段优先 → pool_1,池内按 offset 升序
// 找第一个放得下的空隙,cursor 落点再做 64KB 对齐)。cur 按 offset 升序
// (设备 carve 恒满足),字段只需 offset/size(含 storage 占位 —— 它占空间)。
// 返回 { index, offset }(插入下标 + 落点偏移)或 null(放不下/超上限/参数非法)。
export function carvePlace(cur, slotSize) {
  if (!Number.isInteger(slotSize) || slotSize < POOL.minSlot ||
      slotSize % POOL.sizeGranule !== 0) {
    return null;
  }
  if (!Array.isArray(cur) || cur.length >= POOL.maxSlots) return null;
  const slots = [...cur].sort((a, b) => a.offset - b.offset);
  let si = 0;
  let found = -1;
  for (let seg = 0; seg < POOL.seg.length && found < 0; seg++) {
    const segStart = POOL.seg[seg].start;
    const segEnd = POOL.seg[seg].end;
    let cursor = segStart;
    while (si < slots.length && slots[si].offset + slots[si].size <= segStart) si++;
    for (;;) {
      let gapEnd = segEnd;
      if (si < slots.length && slots[si].offset < segEnd) gapEnd = slots[si].offset;
      const cand = alignUp(cursor, POOL.offsetAlign);
      if (cand + slotSize <= gapEnd) {
        found = cand;
        break;
      }
      if (si < slots.length && slots[si].offset < segEnd) {
        cursor = slots[si].offset + slots[si].size;
        si++;
      } else {
        break;
      }
    }
  }
  if (found < 0) return null;
  let index = slots.length;
  for (let i = 0; i < slots.length; i++) {
    if (slots[i].offset > found) {
      index = i;
      break;
    }
  }
  return { index, offset: found };
}

// 设备 GET /api/install/slots 清单(phone-install.js parseSlots 的输出形状)
// → 槽位表 + 提案。imageLen = 剥离后 app 镜像长度。返回:
//   current       现有 APP 槽 [{slot, limit, fit}](fit = imageLen ≤ limit;
//                 slot = 当前 carve 下标 = 清单序)。
//   proposal      可 first-fit 出新槽时 {slot, carveOffset, carveSize, limit},
//                 否则 null;slot = 插入下标(创建后原 ≥slot 下标 +1)。
//   placed        提案成立时的「插入后」下标视图 [{slot, limit, fit}]:
//                 提案槽 fit=true,被顶移的现有槽 fit=false —— 设备 geom 只对
//                 提案下标预填 offer_ok 才不会把手机声称当错报拒掉(design §4.5);
//                 提案不成立时为 null。
//   suggestedSlot 建议槽位:现有可装优先(零副作用),否则提案槽;-1 = 都不行。
export function geomFromListing(listing, imageLen) {
  const all = Array.isArray(listing?.slots) ? listing.slots : [];
  const current = all.filter((s) => s.kind === "app")
    .map((s) => ({ slot: s.slot, limit: s.limit, fit: s.limit > 0 && imageLen <= s.limit }));
  const need = carveNeed(imageLen);
  const place = need ? carvePlace(all.map((s) => ({ offset: s.offset, size: s.size })), need)
                     : null;
  const proposal = place
    ? { slot: place.index, carveOffset: place.offset, carveSize: need,
        limit: appLimit(need) }
    : null;
  let placed = null;
  if (proposal) {
    placed = [
      ...current.filter((s) => s.slot < proposal.slot)
        .map((s) => ({ slot: s.slot, limit: s.limit, fit: false })),
      { slot: proposal.slot, limit: proposal.limit, fit: true },
      ...current.filter((s) => s.slot >= proposal.slot)
        .map((s) => ({ slot: s.slot + 1, limit: s.limit, fit: false })),
    ];
  }
  const suggestedSlot = current.find((s) => s.fit)?.slot ?? proposal?.slot ?? -1;
  return { current, proposal, placed, suggestedSlot };
}
