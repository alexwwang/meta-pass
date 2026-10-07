// main/meta_carve.c —— 见 meta_carve.h。纯逻辑:无堆、无 stdio、无 IDF,
// 以便同一份代码链接进 bootloader hook(meta_boot_hooks)与 host 测试。
#include "meta_carve.h"

#include <string.h>

#include "meta_md5.h"

// ---- 池描述符 -------------------------------------------------------------

static const meta_pool_desc_t POOL = {
    .seg = {
        { .start = META_POOL0_START, .end = META_POOL0_END },
        { .start = META_POOL1_START, .end = META_POOL1_END },
    },
    .min_slot = META_CARVE_MIN_SLOT,
    .offset_align = META_CARVE_OFFSET_ALIGN,
    .size_granule = META_CARVE_SIZE_GRANULE,
    .tail = META_CARVE_TAIL,
    .max_slots = META_CARVE_MAX_SLOTS,
};

const meta_pool_desc_t *meta_carve_pool(void)
{
    return &POOL;
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static int pool_of(uint32_t offset, uint32_t size)
{
    for (int i = 0; i < 2; i++) {
        if (offset >= POOL.seg[i].start && offset + size <= POOL.seg[i].end) {
            return i;
        }
    }
    return -1;
}

uint32_t meta_carve_need(uint32_t image_len)
{
    if (image_len > 0x7FFFF000u) {
        return 0;   // 溢出防护:不可能的需求
    }
    uint32_t need = align_up(image_len + POOL.tail, POOL.size_granule);
    return need < POOL.min_slot ? POOL.min_slot : need;
}

uint32_t meta_carve_pool_total(void)
{
    return (POOL.seg[0].end - POOL.seg[0].start) +
           (POOL.seg[1].end - POOL.seg[1].start);
}

uint32_t meta_carve_free(const meta_carve_t *c)
{
    if (!c) return 0;
    uint32_t used = 0;
    if (c->count <= META_CARVE_MAX_SLOTS) {
        for (uint8_t i = 0; i < c->count; i++) {
            used += c->slot[i].size;
        }
    }
    if (c->data_count <= META_DATA_MAX) {   // 数据 carve 同样吃池空间
        for (uint8_t i = 0; i < c->data_count; i++) {
            used += c->data[i].size;
        }
    }
    const uint32_t total = meta_carve_pool_total();
    return used > total ? 0u : total - used;
}

bool meta_carve_slot_fits(const meta_carve_slot_t *s, uint32_t image_len)
{
    if (!s || s->kind != META_CARVE_KIND_APP) {
        return false;
    }
    if (image_len > 0x7FFFF000u) {
        return false;
    }
    return image_len + POOL.tail <= s->size;
}

int meta_carve_find_fit(const meta_carve_t *c, uint32_t image_len)
{
    if (!c || c->count > META_CARVE_MAX_SLOTS) {
        return -1;
    }
    for (uint8_t i = 0; i < c->count; i++) {
        if (meta_carve_slot_fits(&c->slot[i], image_len)) {
            return i;
        }
    }
    return -1;
}

// ---- 池空隙扫描(占用域 = 槽位 ∪ 数据 carve;place/place_data/largest_gap 共用) ----

#define META_RANGE_MAX (META_CARVE_MAX_SLOTS + META_DATA_MAX)

typedef struct { uint32_t offset; uint32_t size; } pool_range_t;

// 收集占用域并按 offset 升序(插入排序,≤16 条;槽位本就升序、数据乱序)。
static uint8_t build_ranges(const meta_carve_t *c, pool_range_t *out)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < c->count && n < META_RANGE_MAX; i++) {
        const pool_range_t r = { c->slot[i].offset, c->slot[i].size };
        uint8_t j = n++;
        while (j > 0 && out[j - 1].offset > r.offset) { out[j] = out[j - 1]; j--; }
        out[j] = r;
    }
    for (uint8_t i = 0; i < c->data_count && n < META_RANGE_MAX; i++) {
        const pool_range_t r = { c->data[i].offset, c->data[i].size };
        uint8_t j = n++;
        while (j > 0 && out[j - 1].offset > r.offset) { out[j] = out[j - 1]; j--; }
        out[j] = r;
    }
    return n;
}

// first-fit:pool_0(小段)优先 → pool_1,cursor 64KB 对齐;找到首个空隙能
// 容 need 字节即返回。原 meta_carve_place 单槽循环的逐语义等价形,仅占用域
// 从“槽位”扩为“槽位 ∪ 数据”(数据 carve 不可被压在下面)。
static bool scan_fit(const meta_carve_t *cur, uint32_t need, uint32_t *found_out)
{
    pool_range_t rg[META_RANGE_MAX];
    const uint8_t n = build_ranges(cur, rg);
    for (int seg = 0; seg < 2; seg++) {
        const uint32_t seg_start = POOL.seg[seg].start;
        const uint32_t seg_end = POOL.seg[seg].end;
        uint32_t cursor = seg_start;
        uint8_t si = 0;
        while (si < n && rg[si].offset + rg[si].size <= seg_start) {
            si++;   // 跳过前一个池里的占用
        }
        for (;;) {
            uint32_t gap_end = seg_end;
            if (si < n && rg[si].offset < seg_end) {
                gap_end = rg[si].offset;
            }
            const uint32_t cand = align_up(cursor, POOL.offset_align);
            if (cand + need <= gap_end) {
                *found_out = cand;
                return true;
            }
            if (si < n && rg[si].offset < seg_end) {
                cursor = rg[si].offset + rg[si].size;
                si++;
            } else {
                break;
            }
        }
    }
    return false;
}

// 同一扫描不早退:取全部空隙中 align 后可放下的最大字节数。
static uint32_t scan_largest(const meta_carve_t *cur)
{
    pool_range_t rg[META_RANGE_MAX];
    const uint8_t n = build_ranges(cur, rg);
    uint32_t best = 0;
    for (int seg = 0; seg < 2; seg++) {
        const uint32_t seg_start = POOL.seg[seg].start;
        const uint32_t seg_end = POOL.seg[seg].end;
        uint32_t cursor = seg_start;
        uint8_t si = 0;
        while (si < n && rg[si].offset + rg[si].size <= seg_start) si++;
        for (;;) {
            uint32_t gap_end = seg_end;
            if (si < n && rg[si].offset < seg_end) gap_end = rg[si].offset;
            const uint32_t cand = align_up(cursor, POOL.offset_align);
            if (gap_end > cand && gap_end - cand > best) best = gap_end - cand;
            if (si < n && rg[si].offset < seg_end) {
                cursor = rg[si].offset + rg[si].size;
                si++;
            } else {
                break;
            }
        }
    }
    return best;
}

int meta_carve_place(const meta_carve_t *cur, uint32_t slot_size,
                     meta_carve_kind_t kind, meta_carve_t *out)
{
    if (!cur || !out) {
        return -1;
    }
    if (slot_size < POOL.min_slot || slot_size % POOL.size_granule != 0) {
        return -1;
    }
    if (kind != META_CARVE_KIND_APP && kind != META_CARVE_KIND_STORAGE) {
        return -1;
    }
    if (cur->count >= POOL.max_slots || !meta_carve_valid(cur)) {
        return -1;
    }

    // first-fit:pool_0(小段)优先,池内按 offset 升序找第一个放得下的空隙。
    uint32_t found = 0;
    if (!scan_fit(cur, slot_size, &found)) {
        return -1;
    }

    // 插入并保持升序。
    if (out != cur) {
        *out = *cur;
    }
    uint8_t ins = cur->count;
    for (uint8_t i = 0; i < cur->count; i++) {
        if (cur->slot[i].offset > found) {
            ins = i;
            break;
        }
    }
    memmove(&out->slot[ins + 1], &out->slot[ins],
            (size_t)(cur->count - ins) * sizeof(out->slot[0]));
    meta_carve_slot_t *s = &out->slot[ins];
    memset(s, 0, sizeof(*s));
    s->kind = (uint8_t)kind;
    s->state = META_SLOT_EMPTY;
    s->offset = found;
    s->size = slot_size;
    out->count = (uint8_t)(cur->count + 1);
    return ins;
}

bool meta_carve_remove(meta_carve_t *c, uint8_t idx)
{
    if (!c || idx >= c->count) {
        return false;
    }
    memmove(&c->slot[idx], &c->slot[idx + 1],
            (size_t)(c->count - idx - 1) * sizeof(c->slot[0]));
    c->count--;
    memset(&c->slot[c->count], 0, sizeof(c->slot[0]));
    return true;
}

// ---- 数据 carve(M1/M5)---------------------------------------------------

bool meta_carve_place_data(const meta_carve_t *cur, uint32_t size,
                           uint32_t *out_offset)
{
    if (!cur || !out_offset) {
        return false;
    }
    // 数据分区无 128KB 下限(16K NVS 也是合法 carve),4KB 粒度/64KB 对齐同源。
    if (size < META_CARVE_MIN_DATA || size % POOL.size_granule != 0) {
        return false;
    }
    if (cur->data_count >= META_DATA_MAX || !meta_carve_valid(cur)) {
        return false;
    }
    return scan_fit(cur, size, out_offset);
}

int meta_carve_find_data(const meta_carve_t *c, uint32_t play_id, const char *label)
{
    if (!c || play_id == 0 || !label) {
        return -1;
    }
    for (uint8_t i = 0; i < c->data_count; i++) {
        if (c->data[i].play_id == play_id && strcmp(c->data[i].label, label) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool meta_carve_data_append(meta_carve_t *c, const meta_carve_data_t *d)
{
    if (!c || !d || !d->label[0]) {
        return false;
    }
    size_t len = 0;
    while (len <= META_DATA_LABEL_MAX && d->label[len] != '\0') len++;
    if (len == 0 || len > META_DATA_LABEL_MAX) {
        return false;
    }
    if (c->data_count >= META_DATA_MAX) {
        return false;
    }
    c->data[c->data_count++] = *d;
    return true;
}

bool meta_carve_remove_data(meta_carve_t *c, uint8_t idx)
{
    if (!c || idx >= c->data_count) {
        return false;
    }
    memmove(&c->data[idx], &c->data[idx + 1],
            (size_t)(c->data_count - idx - 1) * sizeof(c->data[0]));
    c->data_count--;
    memset(&c->data[c->data_count], 0, sizeof(c->data[0]));
    return true;
}

uint32_t meta_carve_largest_gap(const meta_carve_t *c)
{
    if (!c) return 0;
    return scan_largest(c);
}

uint32_t meta_carve_reclaimable(const meta_carve_t *c)
{
    if (!c) return 0;
    uint32_t a = 0, p = 0;
    meta_carve_reclaimable_split(c, &a, &p);
    return a + p;
}

void meta_carve_reclaimable_split(const meta_carve_t *c,
                                  uint32_t *out_archived,
                                  uint32_t *out_pristine)
{
    uint32_t a = 0, p = 0;
    if (c) {
        for (uint8_t i = 0; i < c->data_count && i < META_DATA_MAX; i++) {
            // ARCHIVED = 已卸载归档;PRISTINE = 未被运行时碰过的出厂 carve ——
            // 都是阶梯可回收物;DIRTY = 在用玩法数据,不入阶梯。
            if (c->data[i].state == META_DATA_ARCHIVED) {
                a += c->data[i].size;
            } else if (c->data[i].state == META_DATA_PRISTINE) {
                p += c->data[i].size;
            }
        }
    }
    if (out_archived) *out_archived = a;
    if (out_pristine) *out_pristine = p;
}

// 保留标签 = 固定表条目(FIXED)的 label 集合同源:子固件声明的这些名字会
// 先命中系统分区(nvs/phy_init/factory/cardid/otadata),而 store 更危险 ——
// 子固件拿到系统 store 分区(NVS 语义)可能一擦就把 carve 记录抹掉(L7)。
static const char *const RESERVED_LABELS[] = {
    "nvs", "phy_init", "factory", "cardid", "store", "otadata",
};

bool meta_carve_data_label_reserved(const char *label)
{
    if (!label) return false;
    for (size_t i = 0; i < sizeof(RESERVED_LABELS) / sizeof(RESERVED_LABELS[0]); i++) {
        if (strcmp(label, RESERVED_LABELS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool meta_carve_valid(const meta_carve_t *c)
{
    if (!c || c->count > META_CARVE_MAX_SLOTS) {
        return false;
    }
    for (uint8_t i = 0; i < c->count; i++) {
        const meta_carve_slot_t *s = &c->slot[i];
        if (s->kind > META_CARVE_KIND_STORAGE) {
            return false;
        }
        if (s->state > META_SLOT_INVALID) {
            return false;
        }
        if (s->size < POOL.min_slot || s->size % POOL.size_granule != 0) {
            return false;
        }
        if (s->offset % POOL.offset_align != 0) {
            return false;
        }
        if (pool_of(s->offset, s->size) < 0) {
            return false;
        }
        if (i > 0) {
            const meta_carve_slot_t *p = &c->slot[i - 1];
            if (p->offset >= s->offset) {
                return false;   // 严格升序
            }
            if (p->offset + p->size > s->offset) {
                return false;   // 重叠
            }
        }
    }

    // 数据 carve(M1/M5):字段合法性 + 标签策略 + 与槽位/彼此零重叠。
    if (c->data_count > META_DATA_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < c->data_count; i++) {
        const meta_carve_data_t *d = &c->data[i];
        if (d->play_id == 0 || d->type != 1 || d->state > META_DATA_ARCHIVED) {
            return false;
        }
        if (d->subtype == 0) {
            return false;   // DATA_OTA:子固件条目会在表序里抢单系统 otadata
        }
        if (d->size < META_CARVE_MIN_DATA || d->size % POOL.size_granule != 0) {
            return false;
        }
        if (d->offset % POOL.offset_align != 0) {
            return false;
        }
        if (pool_of(d->offset, d->size) < 0) {
            return false;
        }
        size_t len = 0;
        while (len <= META_DATA_LABEL_MAX && d->label[len] != '\0') len++;
        if (len == 0 || len > META_DATA_LABEL_MAX) {
            return false;   // 空标签或未终结(强制 ≤16B + NUL)
        }
        if (meta_carve_data_label_reserved(d->label)) {
            return false;
        }
        for (size_t k = 0; k < len; k++) {
            if ((unsigned char)d->label[k] < 0x20u || (unsigned char)d->label[k] > 0x7Eu) {
                return false;   // 可打印 ASCII(表条目/分区标签契约)
            }
        }
        for (uint8_t j = 0; j < i; j++) {
            // (label, subtype) 全局唯一 —— 首配语义下重复条目不可区分;
            // 跨玩法同名共享(M4)是未来设备配置项,v1 直接拒。
            if (c->data[j].play_id == d->play_id &&
                c->data[j].subtype == d->subtype &&
                strcmp(c->data[j].label, d->label) == 0) {
                return false;
            }
            if (d->offset < c->data[j].offset + c->data[j].size &&
                c->data[j].offset < d->offset + d->size) {
                return false;   // 数据间重叠
            }
        }
        for (uint8_t s = 0; s < c->count; s++) {
            if (d->offset < c->slot[s].offset + c->slot[s].size &&
                c->slot[s].offset < d->offset + d->size) {
                return false;   // 数据压槽位
            }
        }
    }
    return true;
}

// ---- 表物化 ---------------------------------------------------------------

typedef struct {
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    const char *label;
} fixed_entry_t;

// 固定系统条目(offset 升序):与 partitions.csv 安全表同源。
static const fixed_entry_t FIXED[] = {
    { 1, 2, 0x9000u,    0x6000u,   "nvs" },
    { 1, 1, 0xF000u,    0x1000u,   "phy_init" },
    { 0, 0, 0x10000u,   0x170000u, "factory" },
    { 1, 2, 0x356000u,  0x4000u,   "cardid" },
    { 1, 2, 0x35A000u,  0x6000u,   "store" },
    { 1, 0, 0x7FE000u,  0x2000u,   "otadata" },
};
#define FIXED_COUNT (sizeof(FIXED) / sizeof(FIXED[0]))

// 池占位条目(仅安全表;carved 表不带 —— 未分配空隙不声明,天然不可引导)。
static const fixed_entry_t POOL_PLACEHOLDER[2] = {
    { 1, 0x40, META_POOL0_START, META_POOL0_END - META_POOL0_START, "pool_0" },
    { 1, 0x40, META_POOL1_START, META_POOL1_END - META_POOL1_START, "pool_1" },
};

// legacy v1.x 固定 3 槽表条目(partitions.csv @ 9e591a2,offset 升序)。
static const fixed_entry_t LEGACY_OTA[3] = {
    { 0, 0x10, 0x180000u, 0x1D6000u, "ota_0" },
    { 0, 0x11, 0x360000u, 0x200000u, "ota_1" },
    { 0, 0x12, 0x560000u, 0x29E000u, "ota_2" },
};

static void pt_set_entry(meta_pt_t *t, uint8_t type, uint8_t subtype,
                         uint32_t offset, uint32_t size, const char *label)
{
    meta_pt_entry_t *e = &t->e[t->count++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->subtype = subtype;
    e->offset = offset;
    e->size = size;
    size_t n = 0;
    while (label[n] != '\0' && n < 16) {
        e->label[n] = label[n];
        n++;
    }
    e->label[n] = '\0';
}

void meta_pt_encode(const meta_pt_t *t, uint8_t out[META_PT_SIZE])
{
    memset(out, 0xFF, META_PT_SIZE);
    if (!t || t->count > META_PT_MAX_ENTRIES) {
        return;
    }
    for (uint8_t i = 0; i < t->count; i++) {
        const meta_pt_entry_t *e = &t->e[i];
        uint8_t *p = out + (size_t)i * META_PT_ENTRY_SIZE;
        p[0] = (uint8_t)(META_PT_MAGIC & 0xFF);          // 0xAA
        p[1] = (uint8_t)(META_PT_MAGIC >> 8);            // 0x50
        p[2] = e->type;
        p[3] = e->subtype;
        for (int k = 0; k < 4; k++) {
            p[4 + k] = (uint8_t)(e->offset >> (8 * k));
            p[8 + k] = (uint8_t)(e->size >> (8 * k));
        }
        memset(p + 12, 0, 16);
        size_t n = 0;
        while (e->label[n] != '\0' && n < 16) {
            p[12 + n] = (uint8_t)e->label[n];
            n++;
        }
        for (int k = 0; k < 4; k++) {
            p[28 + k] = (uint8_t)(e->flags >> (8 * k));
        }
    }
    uint8_t *m = out + (size_t)t->count * META_PT_ENTRY_SIZE;
    m[0] = (uint8_t)(META_PT_MAGIC_MD5 & 0xFF);          // 0xEB
    m[1] = (uint8_t)(META_PT_MAGIC_MD5 >> 8);            // 0xEB
    memset(m + 2, 0xFF, 14);                             // 规范:14×0xFF
    meta_md5(out, (size_t)t->count * META_PT_ENTRY_SIZE, m + 16);
}

bool meta_pt_decode(const uint8_t raw[META_PT_SIZE], meta_pt_t *out)
{
    if (!raw || !out) {
        return false;
    }
    meta_pt_t t;
    t.count = 0;
    bool md5_ok = false;
    for (size_t off = 0; off + META_PT_ENTRY_SIZE <= META_PT_SIZE;
         off += META_PT_ENTRY_SIZE) {
        const uint8_t *p = raw + off;
        const uint16_t magic = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if (magic == 0xFFFFu) {
            break;   // 条目区以空 magic 终止;落到"无 marker"判定
        }
        if (magic == META_PT_MAGIC_MD5) {
            // marker:EBEB + 14×0xFF + md5(entries)。
            for (int k = 2; k < 16; k++) {
                if (p[k] != 0xFF) {
                    return false;
                }
            }
            if (t.count == 0) {
                return false;
            }
            uint8_t digest[16];
            meta_md5(raw, off, digest);
            if (memcmp(digest, p + 16, 16) != 0) {
                return false;
            }
            md5_ok = true;
            break;
        }
        if (magic != META_PT_MAGIC) {
            return false;
        }
        if (t.count >= META_PT_MAX_ENTRIES) {
            return false;
        }
        meta_pt_entry_t *e = &t.e[t.count++];
        e->type = p[2];
        e->subtype = p[3];
        e->offset = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                    ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        e->size = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                  ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        memcpy(e->label, p + 12, 16);
        e->label[16] = '\0';
        e->flags = (uint32_t)p[28] | ((uint32_t)p[29] << 8) |
                   ((uint32_t)p[30] << 16) | ((uint32_t)p[31] << 24);
    }
    if (!md5_ok) {
        return false;
    }
    *out = t;
    return true;
}

static void build_fixed_only(uint8_t out[META_PT_SIZE], bool with_pools)
{
    meta_pt_t t;
    t.count = 0;
    for (size_t i = 0; i < FIXED_COUNT; i++) {
        const fixed_entry_t *f = &FIXED[i];
        if (with_pools && f->subtype == 2 && f->offset == 0x356000u) {
            // cardid 前插入 pool_0(升序)。
            pt_set_entry(&t, POOL_PLACEHOLDER[0].type, POOL_PLACEHOLDER[0].subtype,
                         POOL_PLACEHOLDER[0].offset, POOL_PLACEHOLDER[0].size,
                         POOL_PLACEHOLDER[0].label);
        }
        if (with_pools && f->offset == 0x7FE000u) {
            // otadata 前插入 pool_1(升序)。
            pt_set_entry(&t, POOL_PLACEHOLDER[1].type, POOL_PLACEHOLDER[1].subtype,
                         POOL_PLACEHOLDER[1].offset, POOL_PLACEHOLDER[1].size,
                         POOL_PLACEHOLDER[1].label);
        }
        pt_set_entry(&t, f->type, f->subtype, f->offset, f->size, f->label);
    }
    meta_pt_encode(&t, out);
}

void meta_pt_safe(uint8_t out[META_PT_SIZE])
{
    build_fixed_only(out, true);
}

// 仅校验、不物化:与 meta_pt_decode 相同的条目链 + MD5 校验,但栈帧 O(1)
// (decode 内部自持 ~640B meta_pt_t,叠加调用方缓冲后在 app_main(3584B
// 主栈)与 bootloader hook 的记录路径上会二次溢出 —— 2026-10-04 QEMU 实测定案)。
bool meta_pt_check(const uint8_t raw[META_PT_SIZE])
{
    if (!raw) {
        return false;
    }
    size_t count = 0;
    for (size_t off = 0; off + META_PT_ENTRY_SIZE <= META_PT_SIZE;
         off += META_PT_ENTRY_SIZE) {
        const uint8_t *p = raw + off;
        const uint16_t magic = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if (magic == 0xFFFFu) {
            break;
        }
        if (magic == META_PT_MAGIC_MD5) {
            for (int k = 2; k < 16; k++) {
                if (p[k] != 0xFF) {
                    return false;
                }
            }
            if (count == 0) {
                return false;
            }
            uint8_t digest[16];
            meta_md5(raw, off, digest);
            return memcmp(digest, p + 16, 16) == 0;
        }
        if (magic != META_PT_MAGIC || count >= META_PT_MAX_ENTRIES) {
            return false;
        }
        count++;
    }
    return false;   // 无 MD5 marker
}

void meta_pt_legacy(uint8_t out[META_PT_SIZE])
{
    meta_pt_t t;
    t.count = 0;
    static const fixed_entry_t HEAD[3] = {
        { 1, 2, 0x9000u,   0x6000u,   "nvs" },
        { 1, 1, 0xF000u,   0x1000u,   "phy_init" },
        { 0, 0, 0x10000u,  0x170000u, "factory" },
    };
    for (size_t i = 0; i < 3; i++) {
        pt_set_entry(&t, HEAD[i].type, HEAD[i].subtype, HEAD[i].offset,
                     HEAD[i].size, HEAD[i].label);
    }
    pt_set_entry(&t, LEGACY_OTA[0].type, LEGACY_OTA[0].subtype,
                 LEGACY_OTA[0].offset, LEGACY_OTA[0].size, LEGACY_OTA[0].label);
    pt_set_entry(&t, 1, 2, 0x356000u, 0x4000u, "cardid");
    pt_set_entry(&t, LEGACY_OTA[1].type, LEGACY_OTA[1].subtype,
                 LEGACY_OTA[1].offset, LEGACY_OTA[1].size, LEGACY_OTA[1].label);
    pt_set_entry(&t, LEGACY_OTA[2].type, LEGACY_OTA[2].subtype,
                 LEGACY_OTA[2].offset, LEGACY_OTA[2].size, LEGACY_OTA[2].label);
    pt_set_entry(&t, 1, 0, 0x7FE000u, 0x2000u, "otadata");
    meta_pt_encode(&t, out);
}

bool meta_pt_from_carve(const meta_carve_t *c, uint8_t out[META_PT_SIZE])
{
    if (!c || !meta_carve_valid(c)) {
        return false;
    }
    meta_pt_t t;
    t.count = 0;
    // 全量收集(固定 + 槽位 + 数据)再按 offset 升序:数据记录在数组里是
    // 分配序(乱序),表内顺序不影响 IDF 查找,但升序保持黄金产物逐字节一致。
    for (size_t i = 0; i < FIXED_COUNT; i++) {
        const fixed_entry_t *f = &FIXED[i];
        pt_set_entry(&t, f->type, f->subtype, f->offset, f->size, f->label);
    }
    for (uint8_t i = 0; i < c->count; i++) {
        char label[6] = { 'o', 't', 'a', '_', (char)('0' + i), '\0' };
        pt_set_entry(&t, 0, (uint8_t)(0x10 + i),
                     c->slot[i].offset, c->slot[i].size, label);
    }
    for (uint8_t i = 0; i < c->data_count; i++) {
        const meta_carve_data_t *d = &c->data[i];
        pt_set_entry(&t, d->type, d->subtype, d->offset, d->size, d->label);
    }
    // 插入排序按 offset 升序(等偏移不可能:valid 已排除一切重叠)。
    for (uint8_t i = 1; i < t.count; i++) {
        const meta_pt_entry_t key = t.e[i];
        uint8_t j = i;
        while (j > 0 && t.e[j - 1].offset > key.offset) {
            t.e[j] = t.e[j - 1];
            j--;
        }
        t.e[j] = key;
    }
    meta_pt_encode(&t, out);
    return true;
}


bool meta_pt_from_carve_active(const meta_carve_t *c, uint32_t active_play_id,
                               uint8_t out[META_PT_SIZE])
{
    if (!c || !out || !meta_carve_valid(c)) {
        return false;
    }

    meta_pt_t t;
    memset(&t, 0, sizeof(t));

    // Keep every APP entry visible: bootloader/launcher still needs the full
    // child-app set. Data entries are the only entries scoped to the active
    // Child Firmware.
    for (size_t i = 0; i < FIXED_COUNT; i++) {
        const fixed_entry_t *f = &FIXED[i];
        pt_set_entry(&t, f->type, f->subtype, f->offset, f->size, f->label);
    }
    for (uint8_t i = 0; i < c->count; i++) {
        char label[6] = { 'o', 't', 'a', '_', (char)('0' + i), '\0' };
        pt_set_entry(&t, 0, (uint8_t)(0x10 + i),
                     c->slot[i].offset, c->slot[i].size, label);
    }
    for (uint8_t i = 0; i < c->data_count; i++) {
        const meta_carve_data_t *d = &c->data[i];
        if (d->play_id != active_play_id) continue;
        if (t.count >= META_PT_MAX_ENTRIES) return false;
        pt_set_entry(&t, d->type, d->subtype, d->offset, d->size, d->label);
    }

    for (uint8_t i = 1; i < t.count; i++) {
        const meta_pt_entry_t key = t.e[i];
        uint8_t j = i;
        while (j > 0 && t.e[j - 1].offset > key.offset) {
            t.e[j] = t.e[j - 1];
            j--;
        }
        t.e[j] = key;
    }

    meta_pt_encode(&t, out);
    return true;
}

bool meta_pt_equal(const uint8_t a[META_PT_SIZE], const uint8_t b[META_PT_SIZE])
{
    return memcmp(a, b, META_PT_SIZE) == 0;
}

// ---- 迁移种子 --------------------------------------------------------------

bool meta_carve_seed_legacy(const meta_pt_t *t, meta_carve_t *out)
{
    if (!t || !out) {
        return false;
    }
    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    uint8_t ota_index = 0;
    for (uint8_t i = 0; i < t->count; i++) {
        const meta_pt_entry_t *e = &t->e[i];
        const bool is_ota = e->type == 0 && (e->subtype & ~0x0Fu) == 0x10u;
        if (!is_ota) {
            continue;
        }
        // OTA 下标必须从 0x10 连续(乱表拒绝);最多 8 槽。
        if (e->subtype != (uint8_t)(0x10 + ota_index) ||
            ota_index >= META_CARVE_MAX_SLOTS) {
            return false;
        }
        meta_carve_slot_t *s = &c.slot[ota_index];
        s->kind = META_CARVE_KIND_APP;
        s->state = META_SLOT_EMPTY;
        s->offset = e->offset;
        s->size = e->size;
        ota_index++;
    }
    if (ota_index == 0) {
        return false;   // 无 ota 条目:没有可种子的 carve
    }
    c.count = ota_index;
    if (!meta_carve_valid(&c)) {
        return false;   // 条目不在池内/未对齐(外来表)
    }
    *out = c;
    return true;
}

bool meta_carve_seed_images(const meta_pool_image_t *imgs, uint8_t n,
                            meta_carve_t *out)
{
    if (!out || (n > 0 && !imgs) || n == 0 || n > META_CARVE_MAX_SLOTS) {
        return false;
    }
    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    uint32_t prev_end = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint32_t off = imgs[i].offset;
        const uint32_t len = imgs[i].image_len;
        if (len == 0 || off % META_CARVE_OFFSET_ALIGN != 0) {
            return false;
        }
        int seg = -1;
        for (int k = 0; k < 2; k++) {
            if (off >= POOL.seg[k].start && off < POOL.seg[k].end) {
                seg = k;
                break;
            }
        }
        if (seg < 0) {
            return false;   // 出池
        }
        if (i > 0 && off < prev_end) {
            return false;   // 乱序或与前槽重叠
        }
        const uint32_t need = meta_carve_need(len);
        if (need == 0) {
            return false;
        }
        uint32_t clamp_end = POOL.seg[seg].end;
        if (i + 1 < n && imgs[i + 1].offset < POOL.seg[seg].end) {
            uint32_t next_off = imgs[i + 1].offset;
            if (next_off % META_CARVE_OFFSET_ALIGN != 0 || next_off <= off) {
                return false;
            }
            if (next_off < clamp_end) {
                clamp_end = next_off;
            }
        }
        uint32_t end = off + need;
        if (end > clamp_end) {
            end = clamp_end;   // 夹到下一镜像/池尾
        }
        if (end - off < need) {
            return false;   // 放不下(含尾部不足最小槽)
        }
        meta_carve_slot_t *s = &c.slot[i];
        memset(s, 0, sizeof(*s));
        s->kind = META_CARVE_KIND_APP;
        s->state = META_SLOT_EMPTY;
        s->offset = off;
        s->size = end - off;
        prev_end = end;
    }
    c.count = n;
    if (!meta_carve_valid(&c)) {
        return false;
    }
    *out = c;
    return true;
}
