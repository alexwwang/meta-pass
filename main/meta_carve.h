// main/meta_carve.h —— dynslot 核心纯逻辑:池描述符、carve、分配器、迁移种子、
// 分区表编解码与物化(零 ESP-IDF 依赖;bootloader hook、启动器、host 测试共用)。
//
// 设计:docs/assets/dynslot-design.md §4.1/§4.2/§4.5/§4.6,边界 §5(B1/B2)。
// 关键不变量:
//   - 槽位 offset 64KB 对齐、size 4KB 粒度且 ≥ 128KB、≤ 8 槽;
//   - slots 按 offset 升序,表内 subtype = 0x10 + 下标(IDF otadata 映射依赖顺序);
//   - carved 表 = 固定系统条目(nvs/phy/factory/cardid/store/otadata,offset 升序)
//     + 槽位条目;安全表额外把两段池声明为 data 占位(不可引导);
//   - 表字节格式与 IDF gen_esp32part.py 逐字节一致(条目 + 0xEBEB + 0xFF×14 +
//     md5(entries) marker,黄金产物 tests/fixtures/*.bin)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meta_slots.h"   // meta_slot_state_t(EMPTY/VALID/INVALID 与扫描态同源)

// ---- 分区表编解码 ---------------------------------------------------------

#define META_PT_SIZE        0xC00u   // ESP_PARTITION_TABLE_MAX_LEN
#define META_PT_ENTRY_SIZE  32u
#define META_PT_MAX_ENTRIES 24u      // 固定 6 + 8 槽 + 8 数据 = 22,上限 24(IDF 硬上限 127)
#define META_PT_MAGIC       0x50AAu  // 小端字节 AA 50
#define META_PT_MAGIC_MD5   0xEBEBu

typedef struct {
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    char     label[17];              // 16B NUL 填充 + 终结
} meta_pt_entry_t;

typedef struct {
    meta_pt_entry_t e[META_PT_MAX_ENTRIES];
    uint8_t         count;
} meta_pt_t;

// 解析 0xC00 表字节:条目链 + MD5 marker 逐字节校验;格式异常返回 false。
bool meta_pt_decode(const uint8_t raw[META_PT_SIZE], meta_pt_t *out);

// 仅校验、不物化:与 decode 相同的条目链 + MD5 校验,但栈帧为 O(1)
// (decode 内部持 ~640B 的 meta_pt_t + 调用方缓冲,bootloader hook 与
// app_main(3584B 主栈)路径必须走这里 —— 2026-10-04 QEMU 定案:记录态
// 启动在 ensure 的 decode 链上二次溢出,首个 DOWN 测试夹具暴露)。
bool meta_pt_check(const uint8_t raw[META_PT_SIZE]);

// 物化:条目 + 0xEBEB marker(md5 over entries)+ 余量 0xFF。
// 条目 > MAX 时写全 0xFF(无效表),不越界。
void meta_pt_encode(const meta_pt_t *t, uint8_t out[META_PT_SIZE]);

// ---- carve ----------------------------------------------------------------

#define META_CARVE_MAX_SLOTS    8      // 设计上限(IDF 硬上限 16,§9.1 决策取 8)
#define META_CARVE_MIN_SLOT     0x20000u   // 128KB(§9.1 决策)
#define META_CARVE_OFFSET_ALIGN 0x10000u   // 64KB
#define META_CARVE_SIZE_GRANULE 0x1000u    // 4KB
#define META_CARVE_TAIL         0x1000u    // 每槽尾 sector(MSIG/MNAM/MAEG)

// 数据 carve(M1/M5,调研文档 §4/§5):子固件自己声明的数据分区在池内的记录。
// 与槽位不同源:按 (play_id, label) 键控、无尾扇区、最小 4KB;记录上限 8 条
// (记录扇区尾部容量决定,阶梯回收就是在耗尽前腾位置)。
#define META_DATA_MAX           8
#define META_CARVE_MIN_DATA     0x1000u    // 4KB(数据分区无 128KB 下限)
#define META_DATA_LABEL_MAX     16         // 表条目 label 字段同源(16B 含 NUL)

#define META_POOL0_START 0x180000u    // factory 尾
#define META_POOL0_END   0x356000u    // cardid 起
#define META_POOL1_START 0x360000u    // store 尾
#define META_POOL1_END   0x7FE000u    // otadata 起

typedef enum {
    META_CARVE_KIND_APP     = 0,   // 可安装/可引导槽
    META_CARVE_KIND_STORAGE = 1,   // L2: carve 时预留的存储槽(录音双用可选项)
} meta_carve_kind_t;

typedef struct {
    uint32_t start;   // 含
    uint32_t end;     // 不含
} meta_pool_seg_t;

// 池描述符:JS 侧 install-slot/dynslot-pool.js 是同一份数值(单一几何事实源,
// 设备端 offer 复核再跑一遍分配器拒绝分歧 —— §4.5)。
typedef struct {
    meta_pool_seg_t seg[2];
    uint32_t min_slot;
    uint32_t offset_align;
    uint32_t size_granule;
    uint32_t tail;
    uint8_t  max_slots;
} meta_pool_desc_t;

typedef struct {
    uint8_t kind;      // meta_carve_kind_t
    uint8_t state;     // meta_slot_state_t(派生态,store 只是缓存)
    uint32_t offset;
    uint32_t size;
    uint32_t image_len;
    uint8_t  image_sha256[32];
    char     name[41]; // 记录内 name[40] + 强制 NUL(防越界)
    uint32_t play_id;  // 归属玩法 id(记录 v2;0 = 旧记录/未知,不与数据记录联动
                       // —— 卸载归档链路靠它把槽位与其数据 carve 对上,M5 键控
                       // 身份是玩法而非槽位下标)
} meta_carve_slot_t;

// 数据记录生命周期(M5):PRISTINE = 刚 carve、运行时未碰;
// DIRTY = 玩法启动过(启动器在 OK 启动时落标,隐式信号);
// ARCHIVED = 卸载时默认归档(用户数据保留,可被回收阶梯回收)。
typedef enum {
    META_DATA_PRISTINE = 0,
    META_DATA_DIRTY    = 1,
    META_DATA_ARCHIVED = 2,
} meta_data_state_t;

// 子固件声明的数据分区在池内的一条 carve 记录(M1 别名 + M5 生命周期)。
typedef struct {
    uint32_t play_id;   // 归属玩法 id(>0;卸载归档/升级保留按它匹配)
    uint32_t offset;    // 池内 64KB 对齐(与槽位同一分配器语义)
    uint32_t size;      // 4KB 粒度且 ≥ META_CARVE_MIN_DATA;无尾扇区
    uint8_t  state;     // meta_data_state_t
    uint8_t  subtype;   // 子固件声明的 data subtype(0 = DATA_OTA,拒 —— 会抢单 otadata)
    uint8_t  type;      // 必须 1(data);app 型分区只能走槽位
    char     label[META_DATA_LABEL_MAX + 1];  // 子固件自己的分区标签(≤16B)
} meta_carve_data_t;

typedef struct {
    uint8_t            count;
    meta_carve_slot_t  slot[META_CARVE_MAX_SLOTS];
    uint8_t            data_count;
    meta_carve_data_t  data[META_DATA_MAX];
} meta_carve_t;

const meta_pool_desc_t *meta_carve_pool(void);

// 安装所需槽位尺寸:max(min_slot, align4k(image_len + tail));溢出返回 0。
uint32_t meta_carve_need(uint32_t image_len);

// 两池总字节数(6,766,592 B;设计 §4.1)。
uint32_t meta_carve_pool_total(void);

// 池剩余可分配字节 = 总池 - 已占槽位(含 storage 预留)。仅算容量不算碎片
// (first-fit 可能"总量够但放不下",可行性仍以 meta_carve_place 为准);
// 列表页"剩余空间"行与手机侧提案可行性共用此值。c 为 NULL → 0。
uint32_t meta_carve_free(const meta_carve_t *c);

// 复用判定:APP 槽且 image_len + tail ≤ size(状态不挡覆盖安装)。
bool meta_carve_slot_fits(const meta_carve_slot_t *s, uint32_t image_len);

// 首个可复用 APP 槽下标;无 → -1。
int meta_carve_find_fit(const meta_carve_t *c, uint32_t image_len);

// first-fit 放置新槽(pool_0 小段优先 → pool_1),保持 offset 升序。
// 占用域 = 槽位 ∪ 数据记录(数据 carve 吃池空间,分配器必须避让)。
// 成功返回插入下标;空间/上限/参数非法 → -1。*out = *cur + 新槽(cur≠out 可同址)。
int meta_carve_place(const meta_carve_t *cur, uint32_t slot_size,
                     meta_carve_kind_t kind, meta_carve_t *out);

// 移除槽位(数组压缩;被删区域的擦除由调用方负责 —— 设计 §4.5 删槽先擦
// 的既有语义不变)。越界/NULL → false。
bool meta_carve_remove(meta_carve_t *c, uint8_t idx);

// 数据 carve 放置(M1):与槽位同一池 first-fit / 64KB 对齐,最小 4KB。
// 成功 → true 且 *out_offset = 落点;放不下/参数非法 → false。
bool meta_carve_place_data(const meta_carve_t *cur, uint32_t size,
                           uint32_t *out_offset);

// 按 (play_id, label) 查数据记录下标;无 → -1。play_id 0 永不匹配。
int meta_carve_find_data(const meta_carve_t *c, uint32_t play_id, const char *label);

// 追加数据记录(容量/字段基本检查;完整不变量由 meta_carve_valid 收口)。
bool meta_carve_data_append(meta_carve_t *c, const meta_carve_data_t *d);

// 移除数据记录(数组压缩;被删区域的擦除由调用方负责)。越界 → false。
bool meta_carve_remove_data(meta_carve_t *c, uint8_t idx);

// 最大单一空闲区间(64KB 对齐游标语义下实际可放下的字节数)——
// no-fit 拒绝时把「需要 vs 最大空闲」的数字报给 UI(design §4.5 阶梯第 5 条)。
uint32_t meta_carve_largest_gap(const meta_carve_t *c);

// 可回收字节总数:ARCHIVED + PRISTINE 数据记录之和(DIRTY = 在用玩法数据,
// 不入阶梯;归档最旧优先的顺序 = data[] 数组序,即分配序)。
uint32_t meta_carve_reclaimable(const meta_carve_t *c);

// P0-5: 按授权级别拆分可回收字节。ARCHIVED = tier 3 可自动回收;
// PRISTINE = tier 4 需用户显式同意。no-fit JSON 两字段分别上报,
// UI 必须能区分"多少无需点头可腾"。
void meta_carve_reclaimable_split(const meta_carve_t *c,
                                  uint32_t *out_archived,
                                  uint32_t *out_pristine);

// 保留标签(与固定表条目同源):子固件声明的这些 label 会先命中系统分区,
// carve 出同名条目轻则无意义、重则(如 store)把系统区暴露给子固件擦写 ——
// analyze 跳过、设备端硬拒(双层门禁)。
bool meta_carve_data_label_reserved(const char *label);

// 结构不变量:数量/顺序/对齐/粒度/池内/不重叠/kind/state 合法。
bool meta_carve_valid(const meta_carve_t *c);

// ---- 表物化 ---------------------------------------------------------------

// 安全表(§4.2 状态1):固定条目 + 两池 data 占位,无 ota_* 条目。
void meta_pt_safe(uint8_t out[META_PT_SIZE]);

// legacy v1.x 固定 3 槽表(编译内置):hook 白名单 + 迁移检测的字节基准。
void meta_pt_legacy(uint8_t out[META_PT_SIZE]);

// carved 表(§4.2 状态2):固定条目 + 槽位条目(offset 升序合并)。
// carve 非法 → false 且不写 out。
bool meta_pt_from_carve(const meta_carve_t *c, uint8_t out[META_PT_SIZE]);

bool meta_pt_equal(const uint8_t a[META_PT_SIZE], const uint8_t b[META_PT_SIZE]);

// ---- 迁移种子(§4.6) ------------------------------------------------------

// (a) legacy 表 → carve:ota_* 条目同偏移同尺寸,状态 EMPTY(扫描后再回填)。
//     无 ota 条目/OTA 下标不连续/越界 → false。
bool meta_carve_seed_legacy(const meta_pt_t *t, meta_carve_t *out);

typedef struct {
    uint32_t offset;      // 池内 64KB 对齐的镜像起点
    uint32_t image_len;   // 有效镜像长度(SILENT 校验通过的)
} meta_pool_image_t;

// (b) 池扫描 → shrink-wrap 种子:槽 = max(min_slot, align4k(len+tail)),
//     夹到下一镜像/池尾;输入必须升序且在池内,任何放不下 → false。
bool meta_carve_seed_images(const meta_pool_image_t *imgs, uint8_t n,
                            meta_carve_t *out);

#define META_PROTOCOL_VERSION 2U   // 递增:carving协议结构变更时更新
