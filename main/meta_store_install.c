// main/meta_store_install.c —— LAN 手机辅助安装的设备侧通道(feat/mota v1)。
//
// 实现范围(设计文档 docs/assets/lan-pair-install-design.md):
//   §4.1 设备自托管最小 boot 页面(GET /,含非脚本回退与同源 DeviceBridge);
//   §6.5 本地 install HTTP 面(prepare/session/chunk/finalize/status/cancel/pair);
//   §8   一次性 128-bit token、配对码兜底(5 分钟 TTL / 5 次上限 / 一次性)、
//         全部本地 API 要求 X-Meta-Session、带 Origin 时必须等于设备 origin、
//         上传不先于物理槽位确认、失败路径槽位 INVALID。
// manifest 形状/边界、槽位几何复核、session/chunk 偏移判定在 meta_install_model
// (纯逻辑,host 测试同一份);本模块只做 HTTP/OTA/token 副作用与上屏快照。
//
// 线程模型:URI handler 全部运行在本地 httpd 任务内,状态读写在 handler 与
// UI 回调(确认/拒绝/取消)之间按"单写者 + 标志后置"约定交接:UI 提交的
// 确认先写 confirmed_slot 再置 confirmed 标志;结构快照拷贝(offer_copy/poll)
// 允许读到过渡态,最终一致性由 session 开启时的实时校验兜底。与 meta_store_net
// 的 httpd 纪律一致,UI 不直接碰阻塞网络调用。
//
// v1 信任模型(文档 §3/§12):设备不向 metapass 重查 analyze,只校验 manifest
// 形状/边界、上传长度/sha256、ESP 镜像结构;剩余保护 = 一次性 LAN token、
// 物理槽位确认、image_len/sha256/slot 强绑定、esp_ota_end/esp_image_verify、
// 失败后槽位 INVALID。签名 install manifest 是后续加固项。
#include "meta_store_install.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <esp_netif.h>
#include <mbedtls/sha256.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "meta_store.h"
#include "meta_store_json.h"
#include "meta_name.h"
#include "meta_sign.h"
#include "meta_backup.h"
#include "meta_carve_flash.h"

static const char *TAG = "install_local";

// ---- 常量 ----

// chunk 流式落盘的单次读缓冲(设备内部按 flash 写尺度拆块,§6.5)。
#define INSTALL_IO_BUF       4096
// prepare body 上限:manifest 是有界小 JSON(定长字段 + 3 槽位)。
#define INSTALL_PREPARE_MAX  1536
#define INSTALL_JSON_MAX     256

// ---- 状态(单例;v1 只允许一个 install 会话,§8) ----

typedef struct {
    bool     token_valid;
    char     token_hex[META_INSTALL_TOKEN_HEX_LEN + 1];
    char     pair_code[META_INSTALL_PAIR_DIGITS + 1];
    bool     pair_valid;         // 配对码未作废(未过期、未超次)
    bool     pair_used;          // 已被成功兑换(一次性)
    int64_t  pair_created_ms;    // 生成时刻(esp_timer 单调毫秒)
    int      pair_tries;         // 已累计错误次数
} session_token_t;

typedef struct {
    uint32_t old_offset;
    uint32_t old_size;
    uint32_t new_offset;
    uint32_t new_size;
} data_move_t;

typedef struct {
    // ---- 快照字段(状态机唯一事实源;poll/status JSON 由此组装) ----
    bool     active;             // 店内 token 会话存活
    bool     offer_ready;        // 手机已 prepare,等待设备物理确认
    bool     confirmed;          // 已物理确认(上传可开始,§8)
    bool     session_opened;     // 手机已开上传 session
    int8_t   confirmed_slot;     // 物理确认的槽位(-1 = 无)
    uint32_t session_offset;     // 设备已写偏移(断点续传基准)
    uint32_t data_offset[META_DATA_MAX]; // per-child-data initial payload progress
    uint32_t data_done_mask;       // bit i = data i is complete/skipped
    uint32_t data_erased_mask;     // bit i = data i allocation has been erased
    char     name[META_NAME_LEN + 1];
    const char *state;           // pairing/offer/confirmed/uploading/done/failed/cancelled
    const char *message;

    // ---- offer 负载 ----
    meta_install_manifest_t manifest;
    bool     manifest_valid;

    // ---- 上传执行态 ----
    mbedtls_sha256_context sha;
    bool     sha_started;
    esp_ota_handle_t ota;
    bool     ota_open;
    bool     flash_touched;      // esp_ota_begin 成功过 = 旧内容已擦,失败必作废
    int64_t  last_activity_ms;   // 上次上传活动(session 打开/末次 chunk;停滞判定基准)

    // ---- P0-5 carve 事务(方案B,§1.0) ----
    // carved_new_slot:本会话 prepare 新建的槽位下标(取消/覆盖/拒绝时回收;
    //   -1 = 无)。静态零初始化会是 0 —— 消费处必须同时守卫 manifest_valid
    //   (两者总在 prepare 同生、clear 同灭,boot 态 manifest_valid=false)。
    // table_changed:本会话物化过表(新槽或数据 backfill)→ 成功结束时复位。
    // data_dirty_mask:F4 —— bit i = manifest.data[i] 是 prepare 前已存在的
    //   记录(finish 时标 DIRTY);本会话新建的记录保持 PRISTINE(tier 4 回收
    //   的数字来源;状态只在 finish 翻转 + 单会话 ⇒ 全标 DIRTY 会让
    //   reclaimablePristine 恒 0,tier 4 死代码)。
    int8_t   carved_new_slot;
    bool     table_changed;
    uint32_t data_dirty_mask;
    data_move_t data_moves[META_DATA_MAX];
    uint8_t data_move_count;
    meta_carve_t carve_before;
    bool carve_committed;
    bool carve_snapshot_valid;
} install_session_t;

static meta_slot_info_t *s_slots;           // 启动器槽位注册表(由 init 登记)
static httpd_handle_t    s_httpd;            // 本地 install httpd(端口 80,STA 模式)
static bool              s_init;             // init 完成
static session_token_t   s_token;
static install_session_t s_session;
// 会话互斥锁(审计 B4):httpd 任务(chunk/finalize/session/prepare)与 UI 任务
// (confirm/reject/cancel/teardown)跨任务碰 s_session;esp_ota_abort/sha256_free
// 与在途 esp_ota_write/sha256_update 无锁并发 = heap UAF(IDF esp_ota_ops.c:438-448
// free 无锁)。凡写 s_session 的执行态(ota/sha/offset/flags)必须持锁;持锁期间
// 不做网络读。status/poll 只读快照字段(原子字宽/常量字面量),不上锁。
static SemaphoreHandle_t s_session_mu;

// ---- 小工具 ----

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// 执行态锁(审计 B4)。凡改 ota/sha/offset/confirmed 等执行态必须先 take;
// 持锁期间不做网络读(chunk 处理器把 recv 放在锁外,锁内只 flash 写)。
// httpd_stop 会先排干在途 handler,net_stop 内再加锁不会自死锁。
static void session_lock(void)
{
    if (s_session_mu) xSemaphoreTake(s_session_mu, portMAX_DELAY);
}
static void session_unlock(void)
{
    if (s_session_mu) xSemaphoreGive(s_session_mu);
}

static esp_err_t confirm_slot_locked(int8_t slot);   // 定义见 offer 状态机节
static esp_err_t finalize_locked(void);              // 定义见 finalize 节

static void status_set(const char *state, const char *msg)
{
    s_session.state = state;
    s_session.message = msg;
}

// 本地分区几何快照(注入 meta_install_model;分区不存在的槽位上限为 0)。
static void geom_refresh(meta_install_geom_t *g)
{
    memset(g, 0, sizeof(*g));
    if (!s_slots) return;
    for (int i = 0; i < META_SLOT_COUNT; i++) {
        const esp_partition_t *part = meta_store_slot_partition(i);
        if (part) g->limit[i] = meta_sign_app_limit(part->size);
    }
    // P0-5:缓存未命中的 carved 槽位从 carve 记录补(本次 prepare 物化、
    // 尚未复位;设备确认/默认槽/session 校验因此对本会话新槽一致可见)。
    meta_install_geom_merge_carve(g, meta_carve_flash_carve());
}

void meta_install_local_geom(meta_install_geom_t *out)
{
    if (out) geom_refresh(out);
}

// ---- P0-5 方案B:carved 槽位的分区句柄 -------------------------------
// 本次 prepare carve 的槽位在当前启动的 esp_partition 表缓存(首访后驻留
// SRAM)中不可见。OTA 写/擦/校验只消费 address/size/subtype/label,按 carve
// 记录伪造句柄即可;下一个启动(bootloader 与 esp_partition)从 flash 读表,
// 天然一致。单会话保证无并发使用;句柄静态存活,防 IDF 内部留存指针
// (esp_ota_begin 的 handle 语义按值拷贝,静态存放双保险)。
static esp_partition_t s_fake_part;

static const esp_partition_t *carved_partition(int8_t slot)
{
    const meta_carve_t *cv = meta_carve_flash_carve();
    if (!cv || slot < 0 || slot >= (int8_t)cv->count) return NULL;
    const meta_carve_slot_t *s = &cv->slot[slot];
    if (s->kind != META_CARVE_KIND_APP) return NULL;
    char label[16];
    snprintf(label, sizeof(label), "ota_%d", slot);
    // IDF 5.5 的 esp_ota_begin 先 esp_partition_verify(handle):对照首次
    // flash 访问时建立的 SRAM 分区缓存,伪造句柄不在缓存里 → NOT_FOUND
    // (2026-10-04 真机:carve 物化成功后 chunk 写 500 的根因)。
    // 运行时装了 carved 表的新槽必须同时注册进缓存,拿官方规范指针;
    // 重启后 esp_partition 从表原生加载,find_first 直接命中,不会
    // 走到注册分支,无重复注册。
    const esp_partition_subtype_t sub =
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot);
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, sub, NULL);
    if (part) return part;
    if (esp_partition_register_external(esp_flash_default_chip,
            s->offset, s->size, label,
            ESP_PARTITION_TYPE_APP, sub, &part) == ESP_OK && part) {
        return part;
    }
    // 注册失败(内存/参数):退回伪造句柄,行为同旧实现(ota_begin 会拒)。
    memset(&s_fake_part, 0, sizeof(s_fake_part));
    s_fake_part.type = ESP_PARTITION_TYPE_APP;
    s_fake_part.subtype = sub;
    s_fake_part.address = s->offset;
    s_fake_part.size = s->size;
    snprintf(s_fake_part.label, sizeof(s_fake_part.label), "%s", label);
    return &s_fake_part;
}

// 缓存查找优先(既有槽位走原路径),未命中回退伪造句柄。
static const esp_partition_t *slot_partition_any(int8_t slot)
{
    const esp_partition_t *part = meta_store_slot_partition(slot);
    return part ? part : carved_partition(slot);
}

// ---- P0-5:物化表后的结束复位(推迟到退出商店页) ----------------------
// 2026-10-04 交互修订:装完立即复位会把 RAM token 一起清掉,手机连接随
// 之作废、装第二个玩法必须重新配对(真机反馈)。运行时物化 carved 表 +
// 注册外部分区后,当前 boot 背着过期状态(esp_partition 外部注册项、
// legacy 槽扫描),确需一次复位清账 —— 但推迟到 goto_page(STORE→LIST)
// 统一执行:手机会话从"装完"一直活到用户退出商店页。挂起标志在 carve
// 层(meta_carve_flash_reboot_pending),物化的唯一漏斗是 commit(materialize)。

// 清理本次 prepare 新建的 DATA 记录。既有记录属于升级保留数据,
// 绝不能因 APP 上传失败而擦掉;新建记录则必须连同其字节一起移除,
// 否则下一次 prepare 会把它误判成既有用户数据而永久占坑。
static void cleanup_new_data_locked(void)
{
    if (!s_session.manifest_valid) return;
    for (uint8_t i = 0; i < s_session.manifest.data_count &&
                        i < META_DATA_MAX; i++) {
        if (s_session.manifest.data[i].play_id == 0) continue;
        if (s_session.data_dirty_mask & (1u << i)) continue;
        (void)meta_carve_flash_erase_data(s_session.manifest.data[i].play_id,
                                           s_session.manifest.data[i].label);
    }
}

static bool data_ranges_overlap(uint32_t a_off, uint32_t a_size,
                                 uint32_t b_off, uint32_t b_size)
{
    const uint64_t a_end = (uint64_t)a_off + a_size;
    const uint64_t b_end = (uint64_t)b_off + b_size;
    return (uint64_t)a_off < b_end && (uint64_t)b_off < a_end;
}

static esp_err_t prepare_data_moves_locked(const meta_carve_t *before,
                                           const meta_carve_t *after,
                                           data_move_t moves[META_DATA_MAX],
                                           uint8_t *out_n)
{
    uint8_t n = 0;
    uint8_t grow_n = 0;
    uint32_t grow_offset[META_DATA_MAX];
    uint32_t grow_size[META_DATA_MAX];
    if (out_n) *out_n = 0;
    if (!before || !after || !moves || !s_session.manifest_valid) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Preflight every move before erasing anything. The old carve remains the
     * durable source of truth until commit, so no destination may overlap any
     * old DATA extent (including a different APP's DATA). Otherwise an early
     * erase could destroy a source needed by this or a later move.
     */
    for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
        const uint32_t pid = s_session.manifest.data[i].play_id;
        if (pid == 0) continue;
        const int old_idx = meta_carve_find_data(before, pid,
                                                  s_session.manifest.data[i].label);
        const int new_idx = meta_carve_find_data(after, pid,
                                                  s_session.manifest.data[i].label);
        if (old_idx < 0 || new_idx < 0) continue;

        const meta_carve_data_t *old = &before->data[old_idx];
        const meta_carve_data_t *now = &after->data[new_idx];
        if (now->size < old->size) return ESP_ERR_INVALID_STATE;

        if (old->offset == now->offset) {
            /* In-place growth preserves the old filesystem bytes. Erase only
             * the newly acquired tail, and prove that tail was not owned by
             * any old DATA extent before touching flash. */
            if (now->size > old->size) {
                const uint64_t tail_start64 = (uint64_t)old->offset + old->size;
                if (tail_start64 > UINT32_MAX) return ESP_ERR_INVALID_STATE;
                const uint32_t tail_offset = (uint32_t)tail_start64;
                const uint32_t tail_size = now->size - old->size;
                for (uint8_t j = 0; j < before->data_count; j++) {
                    const meta_carve_data_t *source = &before->data[j];
                    if (data_ranges_overlap(tail_offset, tail_size,
                                            source->offset, source->size)) {
                        ESP_LOGE(TAG, "in-place DATA growth tail overlaps existing DATA source");
                        return ESP_ERR_INVALID_STATE;
                    }
                }
                if (grow_n >= META_DATA_MAX) return ESP_ERR_INVALID_STATE;
                grow_offset[grow_n] = tail_offset;
                grow_size[grow_n] = tail_size;
                grow_n++;
            }
            continue;
        }
        if (n >= META_DATA_MAX) return ESP_ERR_INVALID_STATE;

        for (uint8_t j = 0; j < before->data_count; j++) {
            const meta_carve_data_t *source = &before->data[j];
            if (data_ranges_overlap(now->offset, now->size,
                                    source->offset, source->size)) {
                ESP_LOGE(TAG, "data migration target overlaps existing DATA source");
                return ESP_ERR_INVALID_STATE;
            }
        }
        for (uint8_t j = 0; j < n; j++) {
            if (data_ranges_overlap(now->offset, now->size,
                                    moves[j].new_offset, moves[j].new_size)) {
                ESP_LOGE(TAG, "data migration targets overlap");
                return ESP_ERR_INVALID_STATE;
            }
        }

        moves[n].old_offset = old->offset;
        moves[n].old_size = old->size;
        moves[n].new_offset = now->offset;
        moves[n].new_size = now->size;
        n++;
    }

    /* All destructive operations start only after the full preflight passes.
     * In-place growth tails are unowned by the old carve; erasing them cannot
     * damage the rollback source if a later moved-DATA copy fails. */
    for (uint8_t i = 0; i < grow_n; i++) {
        esp_err_t e = esp_flash_erase_region(NULL, grow_offset[i], grow_size[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "in-place DATA growth tail erase failed @0x%08lx: %s",
                     (unsigned long)grow_offset[i], esp_err_to_name(e));
            return e;
        }
    }
    for (uint8_t i = 0; i < n; i++) {
        esp_err_t e = esp_flash_erase_region(NULL, moves[i].new_offset,
                                             moves[i].new_size);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "data migration erase failed @0x%08lx: %s",
                     (unsigned long)moves[i].new_offset, esp_err_to_name(e));
            goto rollback;
        }
        e = meta_carve_flash_data_copy(moves[i].old_offset, moves[i].old_size,
                                       moves[i].new_offset);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "data migration copy failed old=0x%08lx new=0x%08lx: %s",
                     (unsigned long)moves[i].old_offset,
                     (unsigned long)moves[i].new_offset, esp_err_to_name(e));
            goto rollback;
        }
    }

    if (out_n) *out_n = n;
    return ESP_OK;

rollback:
    /* Preflight guarantees these extents are disjoint from every old source. */
    for (uint8_t i = 0; i < n; i++) {
        (void)esp_flash_erase_region(NULL, moves[i].new_offset, moves[i].new_size);
    }
    if (out_n) *out_n = 0;
    return ESP_FAIL;
}

static void cleanup_migrated_sources(const data_move_t moves[META_DATA_MAX],
                                     uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        const esp_err_t e = esp_flash_erase_region(NULL, moves[i].old_offset,
                                                   moves[i].old_size);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "old data extent remains after migration @0x%08lx: %s",
                     (unsigned long)moves[i].old_offset, esp_err_to_name(e));
        }
    }
}

static void cleanup_reclaimed_sources(const meta_carve_t *before,
                                      const meta_carve_t *after)
{
    if (!before || !after) return;
    for (uint8_t i = 0; i < before->data_count; i++) {
        const meta_carve_data_t *old = &before->data[i];
        const int idx = meta_carve_find_data(after, old->play_id, old->label);
        if (idx >= 0 && after->data[idx].offset == old->offset) continue;
        const esp_err_t e = esp_flash_erase_region(NULL, old->offset, old->size);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "reclaimed data extent remains @0x%08lx: %s",
                     (unsigned long)old->offset, esp_err_to_name(e));
        }
    }
}

typedef struct {
    uint32_t offset;
    uint32_t size;
} data_new_extent_t;

static esp_err_t prepare_new_data_extents_locked(
    const meta_carve_t *before, const meta_carve_t *after,
    data_new_extent_t extents[META_DATA_MAX], uint8_t *out_n)
{
    uint8_t n = 0;
    if (out_n) *out_n = 0;
    if (!before || !after || !s_session.manifest_valid) return ESP_ERR_INVALID_ARG;

    for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
        const uint32_t pid = s_session.manifest.data[i].play_id;
        if (pid == 0) continue;
        const int old_idx = meta_carve_find_data(before, pid,
                                                  s_session.manifest.data[i].label);
        const int new_idx = meta_carve_find_data(after, pid,
                                                  s_session.manifest.data[i].label);
        if (old_idx >= 0 || new_idx < 0) continue;

        if (n >= META_DATA_MAX) return ESP_ERR_INVALID_STATE;
        extents[n].offset = after->data[new_idx].offset;
        extents[n].size = after->data[new_idx].size;
        if (esp_flash_erase_region(NULL, extents[n].offset, extents[n].size) != ESP_OK) {
            for (uint8_t j = 0; j < n; j++) {
                (void)esp_flash_erase_region(NULL, extents[j].offset, extents[j].size);
            }
            return ESP_FAIL;
        }
        n++;
    }
    if (out_n) *out_n = n;
    return ESP_OK;
}

static void cleanup_new_data_extents(const data_new_extent_t extents[META_DATA_MAX],
                                     uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        (void)esp_flash_erase_region(NULL, extents[i].offset, extents[i].size);
    }
}

// 清 offer 与上传残留(不改 state/message,由各终态自己给出文案)。
static void offer_and_upload_clear(void)
{
    if (s_session.ota_open) {
        esp_ota_abort(s_session.ota);
        s_session.ota_open = false;
    }
    if (s_session.sha_started) {
        mbedtls_sha256_free(&s_session.sha);
        s_session.sha_started = false;
    }
    memset(&s_session.manifest, 0, sizeof(s_session.manifest));
    s_session.manifest_valid = false;
    s_session.offer_ready = false;
    s_session.confirmed = false;
    s_session.confirmed_slot = -1;
    s_session.session_opened = false;
    s_session.session_offset = 0;
    memset(s_session.data_offset, 0, sizeof(s_session.data_offset));
    s_session.data_done_mask = 0;
    s_session.data_erased_mask = 0;
    s_session.flash_touched = false;
    s_session.carved_new_slot = -1;
    s_session.table_changed = false;
    s_session.data_dirty_mask = 0;
    s_session.data_move_count = 0;
    s_session.carve_committed = false;
    s_session.carve_snapshot_valid = false;
    memset(&s_session.carve_before, 0, sizeof(s_session.carve_before));
}

// 终态失败(文档 §6.5):中止 OTA;flash 被动过即槽位 INVALID;清 offer 手机侧重来。
static void fail_locked(const char *msg)
{
    const int8_t slot = s_session.confirmed_slot;
    const bool touched = s_session.flash_touched;
    const int8_t carved = s_session.carved_new_slot;
    const bool have_carved_new = s_session.manifest_valid && carved >= 0;

    cleanup_new_data_locked();

    /* A newly allocated APP+DATA group is only a reservation until finalize.
     * If prepare already committed a replacement carve, rollback restores the
     * exact pre-prepare allocation after discarding any copied DATA extents. */
    if (touched && slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[slot]);
        ESP_LOGW(TAG, "install failed; slot %d marked INVALID", slot);
    }

    if (s_session.carve_committed) {
        for (uint8_t i = 0; i < s_session.data_move_count; i++) {
            (void)esp_flash_erase_region(NULL,
                                         s_session.data_moves[i].new_offset,
                                         s_session.data_moves[i].new_size);
        }
        const esp_err_t rr = meta_carve_flash_commit(&s_session.carve_before, true);
        if (rr != ESP_OK) {
            ESP_LOGE(TAG, "install failed; carve rollback failed: %s",
                     esp_err_to_name(rr));
        }
    } else if (s_session.carve_snapshot_valid) {
        // ARC may have committed metadata before the install itself was carved.
        // Its physical bytes are intentionally still intact, so restore the exact
        // pre-prepare carve on every deterministic failure path.
        const esp_err_t rr = meta_carve_flash_commit(&s_session.carve_before, true);
        if (rr != ESP_OK) {
            ESP_LOGE(TAG, "install failed; pre-prepare carve rollback failed: %s",
                     esp_err_to_name(rr));
        }
    } else if (have_carved_new) {
        (void)meta_carve_flash_remove((int)carved);
        ESP_LOGW(TAG, "install failed; reclaimed new carved slot %d", carved);
    }

    offer_and_upload_clear();
    status_set("failed", msg);
}

// ---- LAN 地址(实时查 esp_netif;配网层不需要回调注入) ----

uint32_t meta_install_lan_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return 0;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return 0;
    return ip.ip.addr;   // 网络字节序;小端下内存序即 a.b.c.d
}

bool meta_install_lan_ready(void)
{
    return s_init && s_httpd != NULL && meta_install_lan_ip() != 0;
}

// ---- token / 配对码 / QR 信息 ----
// NVS 持久化 helpers 定义在 resume 标志段(见下);此处仅前置声明。
static void token_persist(void);
static bool token_restore(void);

static void token_to_hex(const uint8_t token[META_INSTALL_TOKEN_BYTES],
                         char out[META_INSTALL_TOKEN_HEX_LEN + 1])
{
    static const char k_hex[] = "0123456789abcdef";
    for (int i = 0; i < META_INSTALL_TOKEN_BYTES; i++) {
        out[i * 2]     = k_hex[token[i] >> 4];
        out[i * 2 + 1] = k_hex[token[i] & 0x0F];
    }
    out[META_INSTALL_TOKEN_HEX_LEN] = '\0';
}

// 从 IP 构造 QR 用的 http URL(v1 只考虑 IPv4,文档 §2)。
static void lan_url_into(char *out, size_t out_sz, const char *token_hex)
{
    const uint32_t ip = meta_install_lan_ip();
    if (ip == 0) {
        snprintf(out, out_sz, "http://0.0.0.0/#s=%s", token_hex);
        return;
    }
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, out_sz, "http://%u.%u.%u.%u/#s=%s",
             b[0], b[1], b[2], b[3], token_hex);
}

static esp_err_t token_generate(void)
{
    // 128 bit 随机(esp_random 每次 32 bit,拼 4 次;文档 §8 下限)。
    // token 只以 hex 形态驻留(比较也走 hex),不单独存字节副本。
    uint8_t raw[META_INSTALL_TOKEN_BYTES];
    for (int i = 0; i < META_INSTALL_TOKEN_BYTES; i++) {
        raw[i] = (uint8_t)esp_random();
    }
    token_to_hex(raw, s_token.token_hex);
    s_token.token_valid = true;
    snprintf(s_token.pair_code, sizeof(s_token.pair_code), "%06u",
             (unsigned)(esp_random() % 1000000U));
    s_token.pair_code[META_INSTALL_PAIR_DIGITS] = '\0';
    s_token.pair_valid = true;
    s_token.pair_used = false;
    s_token.pair_tries = 0;
    s_token.pair_created_ms = esp_timer_get_time() / 1000;
    token_persist();   // 受控重启后 token_start 原样恢复,手机连接不断
    // 打 UART:真机自动化冒烟(tools/realdevice/smoke.py)从串口日志取
    // 配对码换 token,免读屏。token 本就印在设备 QR 上给近场任何人看,
    // 串口(持有者物理接触)不扩大暴露面。
    ESP_LOGI(TAG, "pair ready: code=%s token=%s",
             s_token.pair_code, s_token.token_hex);
    return ESP_OK;
}

void meta_install_qr_info(const char **token_hex_out, const char **pair_code_out,
                          char url_buf[256])
{
    if (token_hex_out) {
        *token_hex_out = s_token.token_valid ? s_token.token_hex : NULL;
    }
    if (pair_code_out) {
        *pair_code_out = (s_token.pair_valid && !s_token.pair_used)
                             ? s_token.pair_code : NULL;
    }
    if (url_buf) {
        lan_url_into(url_buf, 256, s_token.token_valid ? s_token.token_hex : "");
    }
}

// ---- 中断续连持久标志(用户决策①,NVS "metapass"/"inst_active") ----
// token_start 置位、token_stop 清除;复位后 app_main 读它决定是否自动恢复
// STA + install 服务,手机凭持久化 token 重发 prepare 免重扫 QR。
// 方案B 下 prepare 不再中途复位,本标志服务的是"上传中途断电/崩溃"的
// 非受控复位 —— 无它,用户必须重扫 QR 且上传从头再来(chunk 进度在 RAM)。
// 一切写入 best-effort:标志丢失的最坏结果只是退化为重新配对,不丢正确性。
static const char k_nvs_ns_install[] = "metapass";
static const char k_nvs_key_active[] = "inst_active";

static void resume_flag_set(bool active)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns_install, NVS_READWRITE, &h) != ESP_OK) return;
    if (active) {
        nvs_set_u8(h, k_nvs_key_active, 1);
        nvs_commit(h);
    } else {
        nvs_erase_key(h, k_nvs_key_active);
        nvs_commit(h);
    }
    nvs_close(h);
}

bool meta_install_resume_pending(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(k_nvs_ns_install, NVS_READONLY, &h) != ESP_OK) return false;
    const esp_err_t e = nvs_get_u8(h, k_nvs_key_active, &v);
    nvs_close(h);
    return e == ESP_OK && v == 1;
}

// ---- 会话 token 持久化(2026-10-04 交互修订②) ----
// 手机连接须活过受控重启("装完/退出商店页重启后手机还能继续操作")。
// token 签发即落 NVS;token_stop(正常离店/闲置到期)擦除。配对码保持
// 一次性语义:恢复出来的会话 pair_used=true、pair_valid=false,码不复活。
static const char k_nvs_key_token[] = "inst_tok";

static void token_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns_install, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, k_nvs_key_token, s_token.token_hex);
    nvs_commit(h);
    nvs_close(h);
}

static void token_persist_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns_install, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, k_nvs_key_token);
    nvs_commit(h);
    nvs_close(h);
}

static bool token_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns_install, NVS_READONLY, &h) != ESP_OK) return false;
    char hex[META_INSTALL_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(hex);
    const esp_err_t e = nvs_get_str(h, k_nvs_key_token, hex, &len);
    nvs_close(h);
    if (e != ESP_OK || len != META_INSTALL_TOKEN_HEX_LEN + 1) return false;
    for (int i = 0; i < META_INSTALL_TOKEN_HEX_LEN; i++) {
        const char ch = hex[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    memcpy(s_token.token_hex, hex, sizeof(s_token.token_hex));
    s_token.token_valid = true;
    s_token.pair_valid  = false;
    s_token.pair_used   = true;
    s_token.pair_tries  = 0;
    ESP_LOGI(TAG, "session token restored across reboot");
    return true;
}

esp_err_t meta_install_token_start(void)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (!s_token.token_valid) {
        if (!token_restore()) token_generate();
    }
    resume_flag_set(true);   // 置续连标志;正常离店 token_stop 时清除
    s_session.active = true;
    // 无在途 offer/session 时归位 pairing(首次进店、失败/取消/拒绝回退);
    // 有在途 offer 则保持现状,不覆盖手机侧流程。
    if (!s_session.offer_ready && !s_session.confirmed && !s_session.session_opened) {
        status_set("pairing", "scan QR or enter pair code");
    }
    return ESP_OK;
}

void meta_install_token_stop(void)
{
    // 离店:在途会话终止;flash 被动过即槽位 INVALID(§8),token/配对码全作废。
    // 持执行态锁(审计 B4):与在途 chunk 写互斥后清场,abort 不再撞在途 write。
    session_lock();
    if (s_session.flash_touched && s_session.confirmed_slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[s_session.confirmed_slot]);
    }
    offer_and_upload_clear();
    memset(&s_token, 0, sizeof(s_token));
    s_session.active = false;
    resume_flag_set(false);   // 正常离店:清除续连标志
    token_persist_erase();    // token 一并作废:之后重启不再恢复
    status_set("idle", "");
    session_unlock();
}

bool meta_install_token_from_hex(const char *hex, size_t hex_len)
{
    if (!hex || hex_len != META_INSTALL_TOKEN_HEX_LEN) return false;
    if (!s_token.token_valid) return false;
    // 先验字符集(只接受小写 hex),再与签发 token 定长比较。
    for (int i = 0; i < META_INSTALL_TOKEN_HEX_LEN; i++) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return memcmp(hex, s_token.token_hex, META_INSTALL_TOKEN_HEX_LEN) == 0;
}

// 配对码兑换(§8:短 TTL + 尝试上限 + 一次性)。
static bool pair_redeem(const char *code, size_t code_len)
{
    if (!s_token.token_valid || !s_token.pair_valid || s_token.pair_used) {
        return false;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_token.pair_created_ms > (int64_t)META_INSTALL_PAIR_TTL_MS) {
        s_token.pair_valid = false;
        return false;
    }
    if (code_len != META_INSTALL_PAIR_DIGITS ||
        strncmp(code, s_token.pair_code, META_INSTALL_PAIR_DIGITS) != 0) {
        if (++s_token.pair_tries >= META_INSTALL_PAIR_MAX_TRIES) {
            s_token.pair_valid = false;   // 到达上限即作废(重新进店才再生成)
            ESP_LOGW(TAG, "pair code invalidated after %d tries", s_token.pair_tries);
        }
        return false;
    }
    s_token.pair_used = true;
    return true;
}

// ---- offer 状态机(文档 §6.4) ----

bool meta_install_offer_copy(meta_install_manifest_t *out)
{
    if (!out) return false;
    if (!s_session.manifest_valid || !s_session.offer_ready) return false;
    if (s_session.confirmed) return false;   // 只有待确认 offer 对 UI 有意义
    *out = s_session.manifest;
    return true;
}

void meta_install_session_poll(meta_install_session_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active         = s_session.active;
    out->offer_ready    = s_session.offer_ready;
    out->confirmed      = s_session.confirmed;
    out->session_opened = s_session.session_opened;
    out->slot           = s_session.confirmed_slot;
    out->offset         = s_session.session_offset;
    // 与 /api/install/status 的 "expected" 字段同一事实源(确认前显示 0)。
    out->expected       = s_session.manifest_valid ? s_session.manifest.image_len : 0u;
    memcpy(out->name, s_session.name, sizeof(out->name));
    out->state   = s_session.state;
    out->message = s_session.message;
    out->upload_idle_ms = 0;
    if (s_session.state && strcmp(s_session.state, "uploading") == 0) {
        const int64_t idle = now_ms() - s_session.last_activity_ms;
        out->upload_idle_ms = idle > 0 ? idle : 0;
    }
}

esp_err_t meta_install_confirm_slot(int8_t slot)
{
    session_lock();
    const esp_err_t rc = confirm_slot_locked(slot);
    session_unlock();
    return rc;
}

static esp_err_t confirm_slot_locked(int8_t slot)
{
    if (!s_init || !s_session.manifest_valid || !s_session.offer_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.confirmed) return ESP_ERR_INVALID_STATE;
    meta_install_geom_t g;
    geom_refresh(&g);
    // 只要求本地几何可装(文档 §6.4:用户可选任意本地 fit 槽位)。
    if (!meta_install_model_slot_fit(&g, slot, s_session.manifest.image_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_session.confirmed_slot = slot;   // 先写槽位,后置标志(读侧以标志为序)
    s_session.confirmed = true;
    status_set("confirmed", "waiting for phone upload");
    ESP_LOGI(TAG, "slot %d confirmed for %s (%u bytes)", slot, s_session.name,
             s_session.manifest.image_len);
    return ESP_OK;
}

esp_err_t meta_install_offer_reject(void)
{
    session_lock();
    esp_err_t rc = ESP_OK;
    if (!s_session.manifest_valid || !s_session.offer_ready) {
        rc = ESP_ERR_INVALID_STATE;
    } else if (s_session.confirmed || s_session.session_opened) {
        rc = ESP_ERR_INVALID_STATE;   // 已确认的走 cancel,不走拒绝
    } else {
        // P0-5:拒绝同取消 —— 新建槽回收(守卫同 cancel)。
        const int8_t carved = s_session.carved_new_slot;
        const bool have_carve = s_session.manifest_valid && carved >= 0;
        cleanup_new_data_locked();
        offer_and_upload_clear();
        if (have_carve) meta_carve_flash_remove((int)carved);
        s_session.name[0] = '\0';
        status_set("pairing", "offer declined on device");
    }
    session_unlock();
    return rc;
}

// ---- 手机侧动作(由 HTTP handler 调用) ----

esp_err_t meta_install_session_open(const meta_install_session_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    // 执行态锁(审计 B4):sha init/会话标志与取消/离店互斥。
    session_lock();
    esp_err_t rc = ESP_OK;
    if (!s_session.manifest_valid || !s_session.offer_ready) {
        rc = ESP_ERR_INVALID_STATE;
    } else if (!s_session.confirmed) {       // 上传不得先于确认(§8)
        rc = ESP_ERR_INVALID_STATE;
    } else if (s_session.session_opened) {   // 单 session(§8)
        rc = ESP_ERR_INVALID_STATE;
    } else {
        meta_install_geom_t g;
        geom_refresh(&g);
        if (!meta_install_model_session_ok(&s_session.manifest, s_session.confirmed_slot,
                                           req, &g)) {
            // 分字段日志:session rejected 曾无处归因(真机 2026-10-04)。
            ESP_LOGW(TAG, "session rejected: req(slot=%d,len=%u) vs offer(slot=%d,"
                     "len=%u) sha_eq=%d fit=%d", (int)req->slot,
                     (unsigned)req->image_len, (int)s_session.confirmed_slot,
                     (unsigned)s_session.manifest.image_len,
                     (int)(memcmp(req->sha256, s_session.manifest.sha256, 32) == 0),
                     (int)meta_install_model_slot_fit(&g, req->slot, req->image_len));
            rc = ESP_ERR_INVALID_ARG;
        } else {
            s_session.session_opened = true;
            s_session.session_offset = 0;
            s_session.ota_open = false;
            s_session.flash_touched = false;
            s_session.data_done_mask = 0;
            s_session.data_erased_mask = 0;
            memset(s_session.data_offset, 0, sizeof(s_session.data_offset));
            for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
                /* Existing DATA is user state: preserve it. Empty initial images
                 * need no transfer either. */
                if (s_session.manifest.data[i].play_id == 0 ||
                    s_session.manifest.data[i].initial_image_size == 0 ||
                    (s_session.data_dirty_mask & (1u << i))) {
                    s_session.data_done_mask |= (1u << i);
                }
            }
            mbedtls_sha256_init(&s_session.sha);
            mbedtls_sha256_starts(&s_session.sha, 0);
            s_session.sha_started = true;
            s_session.last_activity_ms = now_ms();   // 停滞判定基准(审计 M6)
            status_set("confirmed", "upload session ready");
        }
    }
    session_unlock();
    return rc;
}

esp_err_t meta_install_chunk_accept(uint32_t offset, uint32_t length, bool *duplicate)
{
    if (duplicate) *duplicate = false;
    const meta_chunk_verdict_t v = meta_install_model_chunk(
        s_session.session_opened,
        s_session.session_offset,
        s_session.manifest_valid ? s_session.manifest.image_len : 0,
        offset, length, META_INSTALL_MAX_CHUNK);
    if (v == META_CHUNK_OK) return ESP_OK;
    if (v == META_CHUNK_DUP) {
        if (duplicate) *duplicate = true;   // 幂等重复:整段已写过,跳过写(§6.5)
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t meta_install_chunk_write(const void *data, uint32_t length)
{
    if (!data || length == 0) return ESP_ERR_INVALID_ARG;

    // 执行态锁覆盖 begin+write 全程(审计 B4):UI 取消/离店的 abort 与 free
    // 必须等锁内完成,esp_ota_abort 才不释放仍在写的 handle。网络 recv 在
    // 调用方(handler)锁外进行,锁内无网络读。
    session_lock();
    esp_err_t rc = ESP_OK;
    if (!s_session.session_opened || !s_session.sha_started) {
        rc = ESP_ERR_INVALID_STATE;
        goto out;
    }

    if (!s_session.ota_open) {
        const esp_partition_t *part = slot_partition_any(s_session.confirmed_slot);
        if (!part) {
            fail_locked("partition missing");
            rc = ESP_ERR_INVALID_STATE;
            goto out;
        }
        // begin 内部即同步擦除旧内容(IDF esp_ota_ops.c:189-197),擦除一
        // 开始槽位就已被摧毁:flash_touched 必须先于 begin 置位(审计 B5),
        // 否则擦除窗口内到来的取消既不 abort 也不标 INVALID,状态还会复活。
        s_session.flash_touched = true;
        const esp_err_t err = esp_ota_begin(part, s_session.manifest.image_len,
                                            &s_session.ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            fail_locked("flash begin failed");
            rc = err;
            goto out;
        }
        s_session.ota_open = true;
    }

    mbedtls_sha256_update(&s_session.sha, data, length);
    const esp_err_t werr = esp_ota_write(s_session.ota, data, length);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed at offset %u: %s",
                 s_session.session_offset, esp_err_to_name(werr));
        fail_locked("flash write failed");
        rc = werr;
        goto out;
    }

    s_session.session_offset += length;
    s_session.last_activity_ms = now_ms();   // 停滞判定:活动戳而非闩锁状态(审计 M6)
    if (strcmp(s_session.state, "uploading") != 0) {
        status_set("uploading", "uploading");
    }
out:
    session_unlock();
    return rc;
}

static const meta_carve_data_t *manifest_data_record(uint8_t idx)
{
    if (idx >= s_session.manifest.data_count || idx >= META_DATA_MAX) return NULL;
    const uint32_t pid = s_session.manifest.data[idx].play_id;
    if (pid == 0) return NULL;
    const int rec = meta_carve_find_data(meta_carve_flash_carve(), pid,
                                         s_session.manifest.data[idx].label);
    if (rec < 0) return NULL;
    return &meta_carve_flash_carve()->data[rec];
}

esp_err_t meta_install_data_write(uint8_t data_index, uint32_t offset,
                                   const void *data, uint32_t length,
                                   bool *duplicate)
{
    if (duplicate) *duplicate = false;
    if (!data || length == 0 || length > META_INSTALL_MAX_CHUNK) {
        return ESP_ERR_INVALID_ARG;
    }

    session_lock();
    esp_err_t rc = ESP_OK;
    if (!s_session.session_opened || !s_session.manifest_valid ||
        data_index >= s_session.manifest.data_count ||
        data_index >= META_DATA_MAX) {
        rc = ESP_ERR_INVALID_STATE;
        goto out;
    }

    const uint32_t expected = s_session.manifest.data[data_index].initial_image_size;
    if (expected == 0 || s_session.manifest.data[data_index].play_id == 0) {
        /* Nothing to upload for this entry. */
        if (duplicate) *duplicate = true;
        goto out;
    }
    if (s_session.data_dirty_mask & (1u << data_index)) {
        /* Existing child DATA is deliberately preserved across upgrade. */
        if (duplicate) *duplicate = true;
        goto out;
    }

    const meta_chunk_verdict_t v = meta_install_model_chunk(
        true, s_session.data_offset[data_index], expected,
        offset, length, META_INSTALL_MAX_CHUNK);
    if (v == META_CHUNK_DUP) {
        if (duplicate) *duplicate = true;
        goto out;
    }
    if (v != META_CHUNK_OK) {
        rc = ESP_ERR_INVALID_ARG;
        goto out;
    }

    const meta_carve_data_t *rec = manifest_data_record(data_index);
    if (!rec || rec->size < s_session.manifest.data[data_index].size) {
        rc = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* First DATA byte destroys the old allocation contents. Mark the whole
     * install touched before erase, exactly like APP/esp_ota_begin. */
    if (!(s_session.data_erased_mask & (1u << data_index))) {
        s_session.flash_touched = true;
        rc = esp_flash_erase_region(NULL, rec->offset, rec->size);
        if (rc != ESP_OK) {
            fail_locked("data erase failed");
            goto out;
        }
        s_session.data_erased_mask |= (1u << data_index);
    }

    rc = esp_flash_write(NULL, (const uint8_t *)data,
                         rec->offset + offset, length);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "data write failed index=%u offset=%u: %s",
                 (unsigned)data_index, (unsigned)offset, esp_err_to_name(rc));
        fail_locked("data write failed");
        goto out;
    }

    s_session.data_offset[data_index] += length;
    if (s_session.data_offset[data_index] == expected) {
        s_session.data_done_mask |= (1u << data_index);
    }
    s_session.last_activity_ms = now_ms();
out:
    session_unlock();
    return rc;
}

static bool data_upload_ready_locked(void)
{
    if (s_session.manifest.data_count == 0) return true;
    const uint32_t all = (1u << s_session.manifest.data_count) - 1u;
    return (s_session.data_done_mask & all) == all;
}

/* Verify only newly-created DATA initial payloads. Existing DATA belongs to
 * the user and must never be compared with the factory image on upgrade. */
static esp_err_t verify_new_data_locked(void)
{
    uint8_t buf[INSTALL_IO_BUF];
    for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
        if (s_session.data_dirty_mask & (1u << i)) continue;
        const uint32_t len = s_session.manifest.data[i].initial_image_size;
        if (len == 0) continue;
        const meta_carve_data_t *rec = manifest_data_record(i);
        if (!rec || rec->size < s_session.manifest.data[i].size) return ESP_ERR_INVALID_STATE;

        mbedtls_sha256_context ctx;
        uint8_t digest[32];
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        uint32_t off = 0;
        while (off < len) {
            const uint32_t n = (len - off > sizeof(buf)) ? sizeof(buf) : (len - off);
            if (esp_flash_read(NULL, buf, rec->offset + off, n) != ESP_OK) {
                mbedtls_sha256_free(&ctx);
                return ESP_FAIL;
            }
            mbedtls_sha256_update(&ctx, buf, n);
            off += n;
        }
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
        if (memcmp(digest, s_session.manifest.data[i].sha256, 32) != 0) {
            ESP_LOGE(TAG, "data sha256 mismatch index=%u label=%s",
                     (unsigned)i, s_session.manifest.data[i].label);
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

esp_err_t meta_install_finalize(void)
{
    // 执行态锁(审计 B4):sha finish/free 与 ota_end 不得与取消/离店并发。
    session_lock();
    esp_err_t rc = finalize_locked();
    session_unlock();
    return rc;
}

static esp_err_t finalize_locked(void)
{
    if (!s_session.session_opened) return ESP_ERR_INVALID_STATE;

    if (!data_upload_ready_locked()) {
        ESP_LOGE(TAG, "finalize: child data upload incomplete");
        fail_locked("data upload incomplete");
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t data_verify = verify_new_data_locked();
    if (data_verify != ESP_OK) {
        fail_locked("data verify failed");
        return data_verify;
    }

    const uint32_t expected = s_session.manifest_valid ? s_session.manifest.image_len : 0;
    if (!meta_install_model_finalize_ready(s_session.session_offset, expected)) {
        ESP_LOGE(TAG, "finalize: received %u != imageLen %u",
                 s_session.session_offset, expected);
        fail_locked("length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s_session.sha, digest);
    mbedtls_sha256_free(&s_session.sha);
    s_session.sha_started = false;

    if (memcmp(digest, s_session.manifest.sha256, 32) != 0) {
        ESP_LOGE(TAG, "finalize: app sha256 mismatch");
        fail_locked("checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    // esp_ota_end:OTA 正式提交 + 镜像校验(设备端权威校验点之一,§6.5)。
    if (!s_session.ota_open) {
        fail_locked("no ota session");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t end_err = esp_ota_end(s_session.ota);
    s_session.ota_open = false;
    if (end_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(end_err));
        fail_locked("verify failed");
        return ESP_ERR_INVALID_CRC;
    }

    // esp_image_verify(SILENT):权威复核 + 取 image_len 与 offer 比对(§6.5)。
    // P0-5:本会话 carve 的新槽在表缓存不可见 → 回退 carve 伪造句柄
    // (verify 只消费 pos 值结构,不查表)。
    const int8_t slot = s_session.confirmed_slot;
    const esp_partition_t *part = slot_partition_any(slot);
    if (!part) {
        fail_locked("partition missing");
        return ESP_ERR_INVALID_STATE;
    }
    esp_image_metadata_t meta = {0};
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
        ESP_LOGE(TAG, "image_verify failed on slot %d", slot);
        fail_locked("image verify failed");
        return ESP_ERR_INVALID_CRC;
    }
    if (meta.image_len != s_session.manifest.image_len) {
        ESP_LOGE(TAG, "image length mismatch: verify=%u manifest=%u",
                 meta.image_len, s_session.manifest.image_len);
        fail_locked("image length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }




    // 展示名 + 注册表(先算好全部入参,避免终态后读已清字段)。
    char sha_hex[META_SHA256_HEX_LEN + 1];
    {
        static const char k_hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            sha_hex[i * 2]     = k_hex[digest[i] >> 4];
            sha_hex[i * 2 + 1] = k_hex[digest[i] & 0x0F];
        }
        sha_hex[META_SHA256_HEX_LEN] = '\0';
    }
    char name[META_NAME_LEN + 1];
    memcpy(name, s_session.name, sizeof(name));
    char ver[META_VERSION_LEN + 1] = "?";
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        memcpy(ver, desc.version, sizeof(ver) - 1);
        ver[sizeof(ver) - 1] = '\0';
    }

    // MNAM 显示名写入尾部 sector(与既有安装路径同一手法)。写失败必须让
    // finalize 失败(审计 M5):注册表已标 VALID 而显示名缺失的槽位会通过
    // 校验却没有名字 —— 设计要求 blob 写入成功才算安装完成。
    {
        const uint32_t tail_off = meta_sign_sector_offset(meta.image_len);
        if (tail_off + META_SIG_SECTOR <= part->size) {
            uint8_t window[META_NAME_BLOB_RESERVE];
            memset(window, 0xFF, sizeof(window));
            if (meta_name_pack_tail(name, window, sizeof(window)) != 0) {
                esp_err_t werr = esp_partition_erase_range(part, tail_off,
                                                           META_SIG_SECTOR);
                if (werr == ESP_OK) {
                    werr = esp_partition_write(part, tail_off + META_NAME_BLOB_OFF,
                                               window, sizeof(window));
                }
                if (werr != ESP_OK) {
                    ESP_LOGE(TAG, "MNAM write on slot %d failed: %s", slot,
                             esp_err_to_name(werr));
                    fail_locked("name write failed");
                    return ESP_ERR_INVALID_STATE;
                }
                ESP_LOGI(TAG, "MNAM write on slot %d: ok", slot);
            }
        }
    }

    /*
     * Promote the durable carve first.  Registry metadata remains the old
     * authority until this succeeds, so a carve-write failure can roll back
     * without invalidating an otherwise valid previous registry entry.
     */
    {
        const esp_err_t cv = meta_carve_flash_set_valid(slot, name,
                                                        meta.image_len, digest);
        if (cv != ESP_OK) {
            ESP_LOGE(TAG, "finalize: carve set_valid failed: %s",
                     esp_err_to_name(cv));
            fail_locked("carve state update failed");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!meta_slot_set_valid(&s_slots[slot], name, ver, meta.image_len, sha_hex)) {
        fail_locked("registry write failed");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Only now is the new APP+DATA group bootable/valid. Retire migrated
     * source DATA after both registry and carve validity have committed.
     * Until this point the old extent remains the rollback source.
     */
    cleanup_migrated_sources(s_session.data_moves, s_session.data_move_count);
    cleanup_reclaimed_sources(s_session.carve_snapshot_valid ? &s_session.carve_before : NULL,
                              meta_carve_flash_carve());
    s_session.data_move_count = 0;

    // 成功:清 offer 与上传态,保留 name/slot 供完成页展示;token 留到离店作废。

    // M5: existing-DATA resize migration is journaled in s_session.data_moves.
    // Source extents are deliberately retained until registry + carve validity
    // commit above; failed sessions therefore keep the previous DATA intact.

    // M5 F4(仲裁②):只对"升级保留"的既有记录标 DIRTY;本会话新建记录
    // 保持 PRISTINE —— 新建区域刚擦除无用户数据,且全标 DIRTY 会让
    // reclaimablePristine 恒 0(状态只在 finish 翻转 + 单会话 ⇒ PRISTINE
    // 不可见于任何 no-fit 决策点),tier 4 回收阶梯死代码。
    if (s_session.manifest_valid && s_session.data_dirty_mask) {
        meta_carve_data_key_t keys[META_DATA_MAX];
        uint8_t n_keys = 0;
        for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
            if (!(s_session.data_dirty_mask & (1u << i))) continue;
            keys[n_keys].play_id = s_session.manifest.data[i].play_id;
            strncpy(keys[n_keys].label, s_session.manifest.data[i].label,
                    META_DATA_LABEL_MAX);
            keys[n_keys].label[META_DATA_LABEL_MAX] = '\0';
            n_keys++;
        }
        const esp_err_t de = meta_carve_flash_mark_dirty_selected(keys, n_keys);
        if (de != ESP_OK && de != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mark_dirty_selected failed: %s", esp_err_to_name(de));
        }
    }

    // 成功路径:复位需求在清场前读走;新建槽归属设备,清场不得回收它。
    const bool needs_done_reboot = s_session.table_changed;
    s_session.carved_new_slot = -1;   // 防后续任何清理路径误回收
    offer_and_upload_clear();
    memcpy(s_session.name, name, sizeof(name));
    s_session.confirmed_slot = slot;
    status_set("done", "installed");
    ESP_LOGI(TAG, "LAN install slot %d done: %s (%u bytes)", slot, name,
             meta.image_len);
    // 表若在本会话物化过,复位推迟到退出商店页(见 meta_carve_flash_reboot_pending);
    // 这里只清场,不断手机连接。
    (void)needs_done_reboot;
    return ESP_OK;
}

esp_err_t meta_install_cancel(void)
{
    // 取消(§6.5 status/cancel):中止 OTA;flash 被动过即槽位 INVALID。
    // 持执行态锁(审计 B4):abort/sha free 与在途 chunk 写严格互斥 ——
    // 这是 OK 长按取消路径,UAF 风险点就在这。
    session_lock();
    const int8_t slot = s_session.confirmed_slot;
    const bool touched = s_session.flash_touched;
    // P0-5:本会话新建槽取消即回收(槽内无用户数据 —— 要么空、要么半截
    // 垃圾镜像);既有槽保持 INVALID 路径。manifest_valid 守卫防 boot 零态
    // 下 carved_new_slot=0(静态零初始化)误删槽 0。
    const int8_t carved = s_session.carved_new_slot;
    const bool have_carve = s_session.manifest_valid && carved >= 0;
    cleanup_new_data_locked();
    offer_and_upload_clear();
    if (have_carve) {
        meta_carve_flash_remove((int)carved);
        ESP_LOGW(TAG, "install cancelled; carved slot %d reclaimed", carved);
    } else if (touched && slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[slot]);
        ESP_LOGW(TAG, "install cancelled; slot %d marked INVALID", slot);
    }
    s_session.name[0] = '\0';
    status_set("cancelled", "cancelled");
    session_unlock();
    return ESP_OK;
}

int8_t meta_install_default_slot_from_manifest(const meta_install_manifest_t *m)
{
    if (!m) return -1;
    meta_install_geom_t g;
    geom_refresh(&g);
    return meta_install_model_default_slot(m, &g);
}

// ---- 本地 HTTP:公共小工具 ----

static bool origin_allowed(httpd_req_t *req)
{
    char origin[96];
    const esp_err_t e = httpd_req_get_hdr_value_str(req, "Origin", origin,
                                                    sizeof(origin));
    if (e == ESP_ERR_NOT_FOUND) return true;   // 非浏览器客户端:无 Origin
    if (e != ESP_OK) return false;             // 存在但超长:不可能等于设备 origin
    const uint32_t ip = meta_install_lan_ip();
    if (ip == 0) return false;                 // 地址未知时带 Origin 一律不放行
    const uint8_t *b = (const uint8_t *)&ip;
    char expect[32];
    snprintf(expect, sizeof(expect), "http://%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return strcmp(origin, expect) == 0;        // §8:必须与设备 origin 完全一致
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, msg);
}

static bool req_token_ok(httpd_req_t *req)
{
    char val[META_INSTALL_TOKEN_HEX_LEN + 2];
    if (httpd_req_get_hdr_value_str(req, META_INSTALL_SESSION_HDR, val,
                                    sizeof(val)) != ESP_OK) {
        return false;   // 缺头 / 超长都不算合法 token
    }
    return meta_install_token_from_hex(val, strlen(val));
}

// 读满请求体进调用方静态缓冲(httpd 任务内串行,静态缓冲安全)。
static esp_err_t req_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    const size_t need = req->content_len;
    if (need == 0) return ESP_ERR_INVALID_ARG;
    if (need + 1 > cap) return ESP_ERR_INVALID_SIZE;   // 调用方映射 413
    size_t total = 0;
    while (total < need) {
        const int got = httpd_req_recv(req, buf + total, need - total);
        if (got <= 0) return ESP_FAIL;                 // 超时 / 连接断
        total += (size_t)got;
    }
    buf[total] = '\0';
    *out_len = total;
    return ESP_OK;
}

// 十进制 u32 解析(X-Meta-Offset):拒绝空串、负号、尾随垃圾。
static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s == '\0' || *s == '-') return false;
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

// JSON 字符串转义(仅 name 上屏字段可能含引号/反斜杠;其余为字面量)。
static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t w = 0;
    for (; *in != '\0' && w + 2 < out_sz; in++) {
        const char c = *in;
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = c;
        } else if ((unsigned char)c >= 0x20u) {
            out[w++] = c;
        }
    }
    out[w] = '\0';
}

// ---- 本地 HTTP:boot 页面(文档 §4.1) ----

// 极简壳,不是产品 UI。占位符恰好 4 个 %s(ip / state 各二,顺序勿动),
// 模板内禁止出现其他 '%'。页面职责:非脚本回退信息、同源 DeviceBridge
// (status/prepare/session/chunk/finalize/cancel)、配对码兑换入口。
// token 只来自 URL fragment 或 pair 响应,绝不进 HTML 响应体 —— 任何无 token
// 的 GET 都拿不到它。
static const char SHELL_HTML[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>meta-pass install</title>"
"<style>body{font-family:sans-serif;max-width:24em;margin:2em auto}"
"#st{color:#555}#ps{color:#a00}</style>"
"<h3>meta-pass install</h3>"
"<div id=st>device %s &middot; session %s</div>"
"<noscript><p>JavaScript is off. Device %s, session %s. "
"Enable JavaScript and reload this page.</p></noscript>"
"<div id=pair>Pairing code: <input id=pc maxlength=6 inputmode=numeric "
"autocomplete=one-time-code> <button onclick=doPair()>Connect</button> "
"<span id=ps></span></div>"
"<script>"
"var T=new URLSearchParams(location.hash.slice(1)).get('s')||'';"
// 已有 token(QR 扫码进入,或配对成功后的 reload):配对框已完成使命,收起,
// 市场模块(phone-install.js)以带 token 状态启动。
"if(T)document.getElementById('pair').style.display='none';"
"function bridge(path,opt){opt=opt||{};var h={};"
"var oh=opt.headers||{};Object.keys(oh).forEach(function(k){h[k]=oh[k];});"
"if(T)h['X-Meta-Session']=T;"
"if(opt.json!==undefined){h['Content-Type']='application/json';"
"opt.body=JSON.stringify(opt.json);}"
"return fetch(path,opt).then(function(r){return r.text().then(function(t){"
"return {ok:r.ok,status:r.status,text:t};});});}"
"window.DeviceBridge={"
"token:function(){return T;},"
"status:function(){return bridge('/api/install/status');},"
"prepare:function(o){return bridge('/api/install/prepare',{method:'POST',json:o});},"
"session:function(o){return bridge('/api/install/session',{method:'POST',json:o});},"
"chunk:function(off,buf){return bridge('/api/install/chunk',{method:'POST',"
"headers:{'X-Meta-Offset':String(off),'Content-Type':'application/octet-stream'},"
"body:buf});},"
"finalize:function(){return bridge('/api/install/finalize',{method:'POST'});},"
"data:function(i,off,buf){return bridge('/api/install/data',{method:'POST',"
"headers:{'X-Meta-Data-Index':String(i),'X-Meta-Offset':String(off),"
"'Content-Type':'application/octet-stream'},body:buf});},"
"cancel:function(){return bridge('/api/install/cancel',{method:'POST'});}};"
"function doPair(){ps.textContent='...';"
"fetch('/api/install/pair',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({code:pc.value})})"
".then(function(r){return r.text().then(function(t){return [r.ok,t];});})"
".then(function(a){if(a[0]){T=JSON.parse(a[1]).token;location.hash='s='+T;"
"ps.textContent='paired';"
// 配对成功后 reload:模块在页面加载时已按空 token 初始化,不刷新它永远拿不到
// 新 token(真机 bring-up 实证:提示 paired 但市场页无任何反应)。hash 跨
// reload 保留,重载后模块带 token 启动、配对框收起。一次性配对码第二次
// 必 rejected(§8),reload 前留 600ms 让用户看到 paired。
"setTimeout(function(){location.reload();},600);}"
"else ps.textContent='pair rejected';})"
".catch(function(){ps.textContent='pair failed';});}"
"</script>"
"<script type=module "
"src=https://metapass.chuanxilu.net/phone-install.js></script>";

static esp_err_t h_boot_index(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    char ip[16] = "0.0.0.0";
    const uint32_t addr = meta_install_lan_ip();
    if (addr != 0) {
        const uint8_t *b = (const uint8_t *)&addr;
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    const char *state = s_session.state ? s_session.state : "idle";
    static char page[sizeof(SHELL_HTML) + 64];
    snprintf(page, sizeof(page), SHELL_HTML, ip, state, ip, state);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// ---- 本地 HTTP:install API(文档 §6.5) ----

// POST /api/install/pair —— 配对码换 token(§8 文本兜底;一次性)。
static esp_err_t h_install_pair(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    static char body[96];
    size_t len = 0;
    if (req_body(req, body, sizeof(body), &len) != ESP_OK) {
        return reply(req, "400 Bad Request", "bad body");
    }
    char code[META_INSTALL_PAIR_DIGITS + 2] = {0};
    if (!meta_store_json_get_string(body, len, "code", code, sizeof(code)) ||
        !pair_redeem(code, strlen(code))) {
        return reply(req, "403 Forbidden", "pair rejected");
    }
    char out[META_INSTALL_TOKEN_HEX_LEN + 32];
    snprintf(out, sizeof(out), "{\"token\":\"%s\"}", s_token.token_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// GET /api/install/status —— loader 兼容性握手(protocol)与轮询快照。
static esp_err_t h_install_status(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");

    char name_esc[META_NAME_LEN * 2 + 1];
    json_escape(s_session.name, name_esc, sizeof(name_esc));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *fw_ver = (app_desc && app_desc->version[0] != '\0')
                         ? app_desc->version : "0.0.0-placeholder";
    char fw_esc[META_BACKUP_VERSION_MAX * 2 + 1];
    json_escape(fw_ver, fw_esc, sizeof(fw_esc));
    static char body[1024];
    int off = snprintf(body, sizeof(body),
        "{\"protocol\":%d,\"state\":\"%s\",\"message\":\"%s\","
        "\"active\":%s,\"offer\":%s,\"confirmed\":%s,\"session\":%s,"
        "\"slot\":%d,\"offset\":%" PRIu32 ",\"expected\":%" PRIu32 ","
        "\"name\":\"%s\",\"firmware_version\":\"%s\",\"data\":[",
        META_INSTALL_PROTOCOL_V1,
        s_session.state ? s_session.state : "idle",
        s_session.message ? s_session.message : "",
        s_session.active ? "true" : "false",
        s_session.offer_ready ? "true" : "false",
        s_session.confirmed ? "true" : "false",
        s_session.session_opened ? "true" : "false",
        s_session.confirmed_slot,
        s_session.session_offset,
        s_session.manifest_valid ? s_session.manifest.image_len : 0u,
        name_esc, fw_esc);
    for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
        const bool done = (s_session.data_done_mask & (1u << i)) != 0;
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"index\":%u,\"offset\":%" PRIu32
                        ",\"expected\":%" PRIu32 ",\"done\":%s}",
                        i ? "," : "", (unsigned)i, s_session.data_offset[i],
                        s_session.manifest.data[i].initial_image_size,
                        done ? "true" : "false");
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // sendstr(strlen) — off 未计入 "]}" 尾巴(与 h_install_session 同款);
    // 用 send(req, body, off) 会丢最后 2 字节,JSON 在 data[] 收尾处截断。
    return httpd_resp_sendstr(req, body);
}

// POST /api/install/prepare —— 手机提交 offer(§6.4);物理确认前可被新 offer 覆盖。
static esp_err_t h_install_prepare(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (s_session.confirmed || s_session.session_opened) {
        return reply(req, "409 Conflict", "already confirmed");
    }

    static char body[INSTALL_PREPARE_MAX];
    size_t len = 0;
    const esp_err_t rd = req_body(req, body, sizeof(body), &len);
    if (rd == ESP_ERR_INVALID_SIZE) {
        return reply(req, "413 Payload Too Large", "manifest too large");
    }
    if (rd != ESP_OK) return reply(req, "400 Bad Request", "read error");

    meta_install_manifest_t m;
    if (!meta_install_model_parse(body, len, &m)) {
        ESP_LOGW(TAG, "prepare: manifest shape/bounds rejected");
        return reply(req, "400 Bad Request", "manifest rejected");
    }
    // 每次新安装都必须从动态回收池创建新的 APP carve；不再允许旧式
    // 固定/既有槽位原位安装。删除只释放 carve，后续安装由分配器重新选址。
    if (!m.has_carve) {
        ESP_LOGW(TAG, "prepare: fresh APP carve required");
        return reply(req, "400 Bad Request", "fresh APP carve required");
    }
    meta_install_geom_t g;
    geom_refresh(&g);
    // 新安装在上方已强制要求 carve 提案；新槽尚未物化，因此提案的
    // 几何裁决权属于 place_offer，不能用当前 live partition table 的 fit 覆盖。
    if (!m.has_carve && !meta_install_model_offer_ok(&m, &g)) {
        ESP_LOGW(TAG, "prepare: local geometry re-check rejected");
        return reply(req, "400 Bad Request", "manifest rejected");
    }

    // 复查确认态(审计 M7):入口检查到 body 读完之间用户可能已完成物理确认,
    // 此时绝不可清场 —— 否则确认被抹掉,UI 死等 upload、手机 session 409。
    session_lock();
    if (s_session.confirmed || s_session.session_opened) {
        session_unlock();
        return reply(req, "409 Conflict", "already confirmed");
    }
    // ── P0-5 carve 提案裁决(方案B,§1.0/§3/§5) ──
    // 纯函数在副本上重放逐条目放置;OK 且 changed → 记录+表一次提交(当前
    // 启动缓存过期无妨:chunk/finalize 走伪造句柄,复位在成功结束时)。
    meta_carve_t placed;
    int place_idx = -1;
    bool place_changed = false;
    char place_label[META_DATA_LABEL_MAX + 1];
    meta_install_no_fit_t nf;
    meta_install_place_verdict_t verdict = META_PLACE_OK;
    int8_t new_group_slot = -1;
    data_new_extent_t new_data_extents[META_DATA_MAX];
    uint8_t new_data_extent_count = 0;
    if (m.has_carve) {
        const meta_carve_t *cur0 = meta_carve_flash_carve();
        /*
         * Snapshot the exact durable allocation before ANY tier-3 reclaim or
         * DATA migration.  fail_locked()/no-fit rollback relies on this copy;
         * without it, an ARC commit made during prepare becomes irreversible
         * when a later placement, migration, or carve commit fails.
         */
        if (cur0) {
            s_session.carve_before = *cur0;
            s_session.carve_snapshot_valid = true;
        } else {
            s_session.carve_snapshot_valid = false;
            memset(&s_session.carve_before, 0, sizeof(s_session.carve_before));
        }
        verdict = meta_install_model_place_offer(&m, cur0, &placed, &place_idx,
                                                 &place_changed, place_label, &nf);
        // F4 掩码须在放置前对"既有记录"快照(prepare 重发幂等时 carve 已含
        // 新建记录,事后 diff 会把它们误判为既有)。
        s_session.data_dirty_mask = 0;
        if (cur0) {
            for (uint8_t i = 0; i < m.data_count && i < META_DATA_MAX; i++) {
                if (m.data[i].play_id != 0 &&
                    meta_carve_find_data(cur0, m.data[i].play_id,
                                         m.data[i].label) >= 0) {
                    s_session.data_dirty_mask |= (1u << i);
                }
            }
        }
        // 回收阶梯 tier 3:no-fit 且有 ARCHIVED 可收 → 收一次重试。
        if ((verdict == META_PLACE_NO_FIT_SLOT || verdict == META_PLACE_NO_FIT_DATA) &&
            nf.reclaimable_archived > 0) {
            const uint32_t free_b = cur0 ? meta_carve_free(cur0) : 0;
            const uint32_t want = nf.needed > free_b ? nf.needed - free_b : nf.needed;
            ESP_LOGI(TAG, "prepare: ARC reclaim %u bytes before retry", (unsigned)want);
            (void)meta_carve_flash_arc_prepare(want);
            verdict = meta_install_model_place_offer(&m, meta_carve_flash_carve(),
                                                     &placed, &place_idx,
                                                     &place_changed, place_label, &nf);
        }
        if (verdict == META_PLACE_REJECTED) {
            if (s_session.carve_snapshot_valid) {
                (void)meta_carve_flash_commit(&s_session.carve_before, true);
            }
            session_unlock();
            ESP_LOGW(TAG, "prepare: carve proposal rejected (label=%s)", place_label);
            return reply(req, "400 Bad Request", "carve proposal rejected");
        }
        if (verdict == META_PLACE_OK && place_changed && place_idx >= 0 && cur0) {
            const meta_carve_slot_t *ps = &placed.slot[place_idx];
            for (uint8_t i = 0; i < cur0->count; i++) {
                if (cur0->slot[i].offset == ps->offset &&
                    cur0->slot[i].size == ps->size) {
                    new_group_slot = -1;
                    goto new_group_checked;
                }
            }
            new_group_slot = (int8_t)place_idx;
        }
new_group_checked:
        if (verdict == META_PLACE_NO_FIT_SLOT || verdict == META_PLACE_NO_FIT_DATA) {
            if (s_session.carve_snapshot_valid) {
                (void)meta_carve_flash_commit(&s_session.carve_before, true);
            }
            session_unlock();
            ESP_LOGW(TAG, "prepare: no-fit (%s) needed=%u gap=%u arch=%u pri=%u",
                     verdict == META_PLACE_NO_FIT_SLOT ? "slot" : "data",
                     (unsigned)nf.needed, (unsigned)nf.largest_gap,
                     (unsigned)nf.reclaimable_archived,
                     (unsigned)nf.reclaimable_pristine);
            char js[256];
            const int n = snprintf(js, sizeof(js),
                "{\"reason\":\"no-fit\",\"for\":\"%s\",\"needed\":%u,"
                "\"largestGap\":%u,\"reclaimableArchived\":%u,"
                "\"reclaimablePristine\":%u%s%s%s}",
                verdict == META_PLACE_NO_FIT_SLOT ? "slot" : "data",
                (unsigned)nf.needed, (unsigned)nf.largest_gap,
                (unsigned)nf.reclaimable_archived,
                (unsigned)nf.reclaimable_pristine,
                place_label[0] ? ",\"label\":\"" : "",
                place_label,
                place_label[0] ? "\"" : "");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, n > 0 && n < (int)sizeof(js) ? js :
                                      "{\"reason\":\"no-fit\"}");
        }
    }

    // 交互 v2:prepare 带手机选定的 slot → 本地几何复核后直接 confirmed,
    // 设备跳过 P2/P3(用户决策:安装交互全部收拢到手机,与 metapass 网页装
    // 的选槽→确认一致);不带 slot 的旧 manifest 走原物理确认流程。
    // P0-5:carve 提案的 fit 权威是 place_offer(新槽不在 geom/缓存中),
    // 不再走 slot_fit;无提案路径保持原样。
    const bool phone_picked = (m.phone_slot >= 0);
    if (!m.has_carve && phone_picked &&
        !meta_install_model_slot_fit(&g, m.phone_slot, m.image_len)) {
        session_unlock();
        return reply(req, "400 Bad Request", "chosen slot does not fit");
    }
    // 覆盖旧 offer:上一 offer 若物化过新建槽且从未上传,先回收(幂等:
    // 槽里无镜像,remove 即回到 prepare 前状态)。
    if (s_session.manifest_valid) {
        cleanup_new_data_locked();
        if (s_session.carved_new_slot >= 0) {
            meta_carve_flash_remove((int)s_session.carved_new_slot);
        }
    }
    offer_and_upload_clear();          // 覆盖旧 offer 时清残留(未确认路径)
    s_session.manifest = m;
    s_session.manifest_valid = true;
    s_session.offer_ready = true;

    /*
     * DATA resize is a real migration, not a metadata-only resize.  Keep the
     * old carve authoritative until all old bytes have been copied to the
     * new extent; only then commit the new carve record.
     */
    if (m.has_carve && place_changed) {
        const meta_carve_t *before = meta_carve_flash_carve();
        if (before) {
                s_session.carve_committed = false;
            const esp_err_t ne = prepare_new_data_extents_locked(
                before, &placed, new_data_extents, &new_data_extent_count);
            const esp_err_t me = (ne == ESP_OK)
                ? prepare_data_moves_locked(before, &placed,
                                            s_session.data_moves,
                                            &s_session.data_move_count)
                : ne;
            if (me != ESP_OK) {
                cleanup_new_data_extents(new_data_extents, new_data_extent_count);
                for (uint8_t i = 0; i < s_session.data_move_count; i++) {
                    (void)esp_flash_erase_region(NULL, s_session.data_moves[i].new_offset,
                                                 s_session.data_moves[i].new_size);
                }
                s_session.data_move_count = 0;
                offer_and_upload_clear();
                session_unlock();
                ESP_LOGE(TAG, "prepare: data migration failed: %s",
                         esp_err_to_name(me));
                return reply(req, "500 Internal Server Error", "data migration failed");
            }
        }
    }

    // P0-5:carve 提交(记录+表一次事务)。失败 → 400(副本未入 carve,
    // 设备状态未被污染)。
    if (m.has_carve && place_changed) {
        const esp_err_t ce = meta_carve_flash_commit(&placed, true);
        if (ce != ESP_OK) {
            /*
             * commit() writes the durable carve before the runtime table.
             * If it reports an error, the record may already have advanced.
             * Do NOT erase any newly prepared extent here: doing so could turn
             * a partially committed record into a durable pointer to erased
             * DATA. An unreferenced extent is reclaimable; a referenced one
             * must remain intact for recovery.
             */
            s_session.manifest_valid = false;
            s_session.offer_ready = false;
            s_session.data_dirty_mask = 0;
            session_unlock();
            ESP_LOGE(TAG, "prepare: carve commit failed: %s; leaving prepared extents for recovery",
                     esp_err_to_name(ce));
            return reply(req, "500 Internal Server Error", "carve commit failed");
        }
        s_session.carve_committed = true;
    }
    s_session.table_changed = m.has_carve && place_changed;
    s_session.carved_new_slot = new_group_slot;
    memcpy(s_session.name, m.name, sizeof(s_session.name));
    if (m.has_carve) {
        // carve 路径:槽位下标以设备分配器为准(phone_slot 已在 place_offer 核对)。
        s_session.confirmed_slot = (int8_t)place_idx;
        s_session.confirmed = phone_picked;   // 手机选槽 → 直确认;否则等设备确认
        status_set(phone_picked ? "confirmed" : "offer",
                   phone_picked ? "slot carved on phone pick" : "confirm on device");
    } else if (phone_picked) {
        s_session.confirmed_slot = m.phone_slot;
        s_session.confirmed = true;   // 先写槽位,后置标志(读侧以标志为序)
        status_set("confirmed", "slot chosen on phone");
    } else {
        status_set("offer", "confirm on device");
    }
    session_unlock();
    ESP_LOGI(TAG, "offer ready: %s (%u bytes, slot %s)", m.name, m.image_len,
             m.has_carve ? "carved" : (phone_picked ? "phone-picked" : "device-confirm"));
    return reply(req, "200 OK", "ok");
}

// POST /api/install/session —— 设备物理确认后开启上传 session(§6.5)。
static esp_err_t h_install_session(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed) return reply(req, "409 Conflict", "not confirmed");
    if (s_session.session_opened) return reply(req, "409 Conflict", "session already open");

    static char body[INSTALL_JSON_MAX];
    size_t len = 0;
    const esp_err_t rd = req_body(req, body, sizeof(body), &len);
    if (rd == ESP_ERR_INVALID_SIZE) {
        return reply(req, "413 Payload Too Large", "session body too large");
    }
    if (rd != ESP_OK) return reply(req, "400 Bad Request", "read error");

    meta_install_session_req_t sreq;
    if (!meta_install_model_parse_session_req(body, len, &sreq)) {
        return reply(req, "400 Bad Request", "session rejected");
    }
    if (meta_install_session_open(&sreq) != ESP_OK) {
        ESP_LOGW(TAG, "session rejected: fields differ from confirmed offer");
        return reply(req, "400 Bad Request", "session rejected");
    }

    static char resp[768];
    int roff = snprintf(resp, sizeof(resp),
             "{\"state\":\"ready\",\"offset\":%" PRIu32 ",\"maxChunk\":%u,\"data\":[",
             s_session.session_offset, META_INSTALL_MAX_CHUNK);
    for (uint8_t i = 0; i < s_session.manifest.data_count && i < META_DATA_MAX; i++) {
        const bool done = (s_session.data_done_mask & (1u << i)) != 0;
        roff += snprintf(resp + roff, sizeof(resp) - (size_t)roff,
                         "%s{\"index\":%u,\"offset\":%" PRIu32
                         ",\"expected\":%" PRIu32 ",\"done\":%s}",
                         i ? "," : "", (unsigned)i, s_session.data_offset[i],
                         s_session.manifest.data[i].initial_image_size,
                         done ? "true" : "false");
    }
    snprintf(resp + roff, sizeof(resp) - (size_t)roff, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// POST /api/install/chunk —— 顺序分块写(§6.5:偏移来自 X-Meta-Offset 头)。
static esp_err_t h_install_chunk(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed || !s_session.session_opened) {
        return reply(req, "409 Conflict", "not confirmed");
    }

    char off_str[16];
    uint32_t offset = 0;
    if (httpd_req_get_hdr_value_str(req, META_INSTALL_OFFSET_HDR, off_str,
                                    sizeof(off_str)) != ESP_OK ||
        !parse_u32(off_str, &offset)) {
        return reply(req, "400 Bad Request", "missing/bad X-Meta-Offset");
    }
    const uint32_t length = (uint32_t)req->content_len;
    if (length == 0 || length > META_INSTALL_MAX_CHUNK) {
        return reply(req, "400 Bad Request", "chunk length out of range");
    }

    bool dup = false;
    if (meta_install_chunk_accept(offset, length, &dup) != ESP_OK) {
        ESP_LOGW(TAG, "chunk rejected: offset %u len %u (written %u)",
                 offset, length, s_session.session_offset);
        return reply(req, "400 Bad Request", "chunk rejected");
    }

    static uint8_t io_buf[INSTALL_IO_BUF];
    size_t got_total = 0;
    if (dup) {
        // 幂等重复:读完丢弃,不写(§6.5 duplicate offsets are idempotent)。
        while (got_total < length) {
            const int got = httpd_req_recv(req, (char *)io_buf,
                                           length - got_total > INSTALL_IO_BUF
                                               ? INSTALL_IO_BUF
                                               : length - got_total);
            if (got <= 0) return reply(req, "400 Bad Request", "chunk read error");
            got_total += (size_t)got;
        }
        return reply(req, "200 OK", "ok");
    }

    // 流式写入:按 flash 写尺度拆块,不在 RAM 里攒整块(§6.5)。
    while (got_total < length) {
        const uint32_t want = (length - got_total > INSTALL_IO_BUF)
                                  ? INSTALL_IO_BUF : (length - got_total);
        const int got = httpd_req_recv(req, (char *)io_buf, want);
        if (got <= 0) {
            // 半块已写:offset 停在实际落盘处,手机按 status.offset 续传。
            return reply(req, "400 Bad Request", "chunk read error");
        }
        if (meta_install_chunk_write(io_buf, (uint32_t)got) != ESP_OK) {
            return reply(req, "500 Internal Server Error", "chunk write failed");
        }
        got_total += (size_t)got;
    }
    return reply(req, "200 OK", "ok");
}

// POST /api/install/data —— Child DATA 初始镜像流。
static esp_err_t h_install_data(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed || !s_session.session_opened) {
        return reply(req, "409 Conflict", "not confirmed");
    }

    char idx_str[8], off_str[16];
    uint32_t offset = 0;
    uint32_t index = 0;
    if (httpd_req_get_hdr_value_str(req, META_INSTALL_DATA_INDEX_HDR,
                                    idx_str, sizeof(idx_str)) != ESP_OK ||
        !parse_u32(idx_str, &index) || index >= META_DATA_MAX ||
        httpd_req_get_hdr_value_str(req, META_INSTALL_OFFSET_HDR,
                                    off_str, sizeof(off_str)) != ESP_OK ||
        !parse_u32(off_str, &offset)) {
        return reply(req, "400 Bad Request", "missing/bad data headers");
    }

    const uint32_t length = (uint32_t)req->content_len;
    if (length == 0 || length > META_INSTALL_MAX_CHUNK) {
        return reply(req, "400 Bad Request", "data chunk length out of range");
    }

    static uint8_t io_buf[INSTALL_IO_BUF];
    uint32_t got_total = 0;
    while (got_total < length) {
        const uint32_t want = (length - got_total > INSTALL_IO_BUF)
                                  ? INSTALL_IO_BUF : (length - got_total);
        const int got = httpd_req_recv(req, (char *)io_buf, want);
        if (got <= 0) return reply(req, "400 Bad Request", "data chunk read error");
        bool dup = false;
        const esp_err_t e = meta_install_data_write((uint8_t)index,
                                                     offset + got_total,
                                                     io_buf, (uint32_t)got, &dup);
        if (e != ESP_OK) {
            return reply(req, "500 Internal Server Error", "data write failed");
        }
        got_total += (uint32_t)got;
    }
    return reply(req, "200 OK", "ok");
}

// POST /api/install/finalize —— 长度/哈希/OTA/镜像/名称/注册表六连检(§6.5)。
static esp_err_t h_install_finalize(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed || !s_session.session_opened) {
        return reply(req, "409 Conflict", "not confirmed");
    }
    if (meta_install_finalize() != ESP_OK) {
        return reply(req, "500 Internal Server Error", "finalize failed");
    }
    return reply(req, "200 OK", "ok");
}

// POST /api/install/cancel —— 手机侧取消(设备 UI 取消也走同一内部函数)。
static esp_err_t h_install_cancel(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    meta_install_cancel();
    return reply(req, "200 OK", "ok");
}


// Forward declaration for handlers defined after meta_install_net_start

// GET /api/install/slots —— dynslot 槽位列表(含归档数据数)
static esp_err_t h_install_slots(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");

    const meta_carve_t *carve = meta_carve_flash_carve();
    if (!carve) return reply(req, "500 Internal Server Error", "carve unavailable");

    uint32_t free_bytes = meta_carve_free(carve);
    uint32_t archived_count = 0;
    for (uint8_t j = 0; j < carve->data_count; j++) {
        if (carve->data[j].state == META_DATA_ARCHIVED) {
            archived_count++;
        }
    }

    // 2026-10-04 真机事故:这份 3072B 响应曾开在 httpd 任务栈上,而安装
    // httpd 的 stack_size 只有 4096;3 个带名槽 + 归档数据记录时 handler
    // 溢出 → 栈保护 panic → 设备复位(token 在 RAM → "需要配对")。
    // 静态化后由 session_lock 串行化填充(当前 httpd 同步 handler 单任务,
    // 锁是对未来多任务化的保险;尾部 send 后已配对 unlock)。
    static char resp[3072];
    session_lock();
    int off = 0;
    // Use PRId32 for count (int), PRIu32 for uint32_t
    off += snprintf(resp + off, sizeof(resp) - off,
        "{\"protocol_version\":%d,\"count\":%d,\"free\":%" PRIu32 ",\"archived\":%" PRIu32 ",\"slots\":[",
        (int)META_PROTOCOL_VERSION, carve->count, free_bytes, archived_count);

    bool first = true;
    for (uint8_t j = 0; j < carve->count; j++) {
        const meta_carve_slot_t *s = &carve->slot[j];
        const char *state_str = "empty";
        if (s->state == META_SLOT_VALID) state_str = "valid";
        else if (s->state == META_SLOT_INVALID) state_str = "invalid";

        const char *kind_str = "app";
        if (s->kind == META_CARVE_KIND_STORAGE) kind_str = "storage";

        // Count archived data records for this play
        uint32_t arc_records = 0;
        // Note: slots don't have ARCHIVED state, only data does
        if (s->state == META_SLOT_VALID || s->state == META_SLOT_INVALID) {
            for (uint8_t k = 0; k < carve->data_count; k++) {
                if (carve->data[k].play_id == s->play_id &&
                    carve->data[k].state == META_DATA_ARCHIVED) {
                    arc_records++;
                }
            }
        }

        if (!first) off += snprintf(resp + off, sizeof(resp) - off, ",");
        first = false;

        off += snprintf(resp + off, sizeof(resp) - off,
            "{\"slot\":%d,\"state\":\"%s\",\"name\":\"%s\","
            "\"size\":%" PRIu32 ",\"len\":%" PRIu32 ",\"limit\":%" PRIu32
            ",\"offset\":%" PRIu32 ",\"kind\":\"%s\",\"arc\":%" PRIu32 "}",
            j, state_str, s->name,
            s->size, s->image_len, s->size - 0x1000,
            s->offset, kind_str, arc_records);
    }

    // dynslot P1-4:数据 carve 记录也占池空间,必须暴露给手机侧分配器 ——
    // 否则手机提案会落进数据区、被设备 carve_ok 拒(L4 分歧)。只给
    // offset/size/state(占用所需);label 不输出,避免分区标签含引号时的
    // JSON 注入面(手机侧占用计算不需要 label)。
    // data[] 在 P1-4 占用域之外再携带 play_id/label:手机侧"导出归档数据"
    // 按 play_id 分组生成备份清单(M5 备份闭环);label 走 json_escape,
    // 与上方 name 同一注入面处理。
    off += snprintf(resp + off, sizeof(resp) - off, "],\"data\":[");
    for (uint8_t j = 0; j < carve->data_count; j++) {
        const meta_carve_data_t *d = &carve->data[j];
        char label_esc[META_DATA_LABEL_MAX * 2 + 1];
        json_escape(d->label, label_esc, sizeof(label_esc));
        if (j) off += snprintf(resp + off, sizeof(resp) - off, ",");
        off += snprintf(resp + off, sizeof(resp) - off,
            "{\"play_id\":%" PRIu32 ",\"offset\":%" PRIu32
            ",\"size\":%" PRIu32 ",\"state\":%u,\"label\":\"%s\"}",
            d->play_id, d->offset, d->size, (unsigned)d->state, label_esc);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t send_rc = httpd_resp_send(req, resp, off);
    session_unlock();
    return send_rc;
}

static esp_err_t h_install_remove(httpd_req_t *req);
static esp_err_t h_backup_import(httpd_req_t *req);
static esp_err_t remove_recover_pending(bool *did_recover);

/* Durable uninstall intent lives in NVS and is committed before any erase.
 * The full APP identity is re-found after reboot; stale intent cleanup must never
 * delete another APP that shifted into the previous array index. */
static bool remove_nvs_key_missing(esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) return true;
#ifdef ESP_ERR_NVS_NOT_FOUND
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
#endif
    return false;
}

#define REMOVE_NVS_NS "meta_rm"
static esp_err_t remove_nvs_set_u32(nvs_handle_t nvs, const char *prefix, uint32_t value)
{
    char key[8];
    for (unsigned i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "%s%u", prefix, i);
        const esp_err_t e = nvs_set_u8(nvs, key, (uint8_t)(value >> (i * 8)));
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

static esp_err_t remove_nvs_get_u32(nvs_handle_t nvs, const char *prefix, uint32_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    char key[8];
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t b = 0;
        snprintf(key, sizeof(key), "%s%u", prefix, i);
        const esp_err_t e = nvs_get_u8(nvs, key, &b);
        if (e != ESP_OK) return e;
        v |= ((uint32_t)b) << (i * 8);
    }
    *value = v;
    return ESP_OK;
}

static esp_err_t remove_intent_write(int slot, const meta_carve_slot_t *target)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(REMOVE_NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(nvs, "slot", (uint8_t)slot);
    if (e == ESP_OK) e = remove_nvs_set_u32(nvs, "p", target->play_id);
    if (e == ESP_OK) e = remove_nvs_set_u32(nvs, "o", target->offset);
    if (e == ESP_OK) e = remove_nvs_set_u32(nvs, "z", target->size);
    if (e == ESP_OK) e = nvs_set_u8(nvs, "active", 1);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

static esp_err_t remove_intent_read(bool *pending, uint32_t *pid,
                                    uint32_t *offset, uint32_t *size)
{
    if (!pending || !pid || !offset || !size) return ESP_ERR_INVALID_ARG;
    *pending = false;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(REMOVE_NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    uint8_t active = 0;
    e = nvs_get_u8(nvs, "active", &active);
    if (remove_nvs_key_missing(e)) { nvs_close(nvs); return ESP_OK; }
    if (e != ESP_OK) { nvs_close(nvs); return e; }
    if (active == 0) { nvs_close(nvs); return ESP_OK; }
    e = remove_nvs_get_u32(nvs, "p", pid);
    if (e == ESP_OK) e = remove_nvs_get_u32(nvs, "o", offset);
    if (e == ESP_OK) e = remove_nvs_get_u32(nvs, "z", size);
    nvs_close(nvs);
    if (e != ESP_OK) return ESP_ERR_INVALID_STATE;
    *pending = true;
    return ESP_OK;
}

static esp_err_t remove_intent_clear(void)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(REMOVE_NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(nvs, "active", 0);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

static esp_err_t remove_app_bytes_and_commit(int slot, uint32_t pid,
                                             uint32_t offset, uint32_t size)
{
    const meta_carve_t *carve = meta_carve_flash_carve();
    if (slot < 0 || slot >= (int)carve->count) return ESP_ERR_NOT_FOUND;
    const meta_carve_slot_t *target = &carve->slot[slot];
    if (target->play_id != pid || target->offset != offset || target->size != size) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Legacy APPs with unknown play_id cannot safely claim or delete DATA.
     * Fail closed rather than create orphan DATA or erase another APP's data. */
    if (pid == 0 && carve->data_count > 0) return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0; i < carve->data_count; i++) {
        const meta_carve_data_t *data = &carve->data[i];
        if (pid == 0 || data->play_id != pid) continue;
        const esp_err_t e = esp_flash_erase_region(NULL, data->offset, data->size);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "cascade erase failed: play_id=%u label=%s err=%s",
                     (unsigned)pid, data->label, esp_err_to_name(e));
            return e;
        }
    }
    const esp_partition_t *part = slot_partition_any((int8_t)slot);
    if (part) {
        const esp_err_t e = esp_partition_erase_range(part, 0, META_SIG_SECTOR);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "remove: image header erase failed: %s", esp_err_to_name(e));
            return e;
        }
    }
    return meta_carve_flash_remove_app_and_data(slot);
}

static esp_err_t remove_recover_pending(bool *did_recover)
{
    if (did_recover) *did_recover = false;
    bool pending;
    uint32_t pid, offset, size;
    esp_err_t e = remove_intent_read(&pending, &pid, &offset, &size);
    if (e != ESP_OK || !pending) return e;
    if (did_recover) *did_recover = true;
    const meta_carve_t *carve = meta_carve_flash_carve();
    int found = -1;
    for (uint8_t i = 0; i < carve->count; i++) {
        const meta_carve_slot_t *slot = &carve->slot[i];
        if (slot->play_id == pid && slot->offset == offset && slot->size == size) {
            found = i; break;
        }
    }
    if (found >= 0) {
        e = remove_app_bytes_and_commit(found, pid, offset, size);
        if (e != ESP_OK) return e;
    }
    return remove_intent_clear();
}

// POST /api/install/remove —— APP + 全部关联 DATA 级联删除
static esp_err_t h_install_remove(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    static char body[512];
    size_t len = 0;
    const esp_err_t rd = req_body(req, body, sizeof(body), &len);
    if (rd == ESP_ERR_INVALID_SIZE) return reply(req, "413 Payload Too Large", "body too large");
    if (rd != ESP_OK) return reply(req, "400 Bad Request", "read error");
    meta_install_remove_req_t rm;
    if (!meta_install_model_parse_remove(body, len, &rm)) {
        return reply(req, "400 Bad Request", "invalid remove request");
    }
    bool recovered = false;
    const esp_err_t recovery = remove_recover_pending(&recovered);
    if (recovery != ESP_OK) {
        return reply(req, "500 Internal Server Error", "previous uninstall recovery failed; retry");
    }
    if (recovered) {
        return reply(req, "409 Conflict", "previous uninstall recovered; refresh slot list");
    }
    const meta_carve_t *carve = meta_carve_flash_carve();
    if (!meta_install_model_remove_ok(carve, rm.slot)) {
        return reply(req, "404 Not Found", "no such slot");
    }
    const meta_carve_slot_t target = carve->slot[rm.slot];
    if (target.play_id == 0 && carve->data_count > 0) {
        return reply(req, "409 Conflict",
                     "DATA ownership is unknown for this legacy APP; refusing unsafe uninstall");
    }
    if (remove_intent_write(rm.slot, &target) != ESP_OK) {
        return reply(req, "500 Internal Server Error", "cannot persist uninstall intent");
    }
    const esp_err_t e = remove_app_bytes_and_commit(rm.slot, target.play_id,
                                                     target.offset, target.size);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uninstall remains pending for slot=%d: %s", rm.slot, esp_err_to_name(e));
        return reply(req, "500 Internal Server Error", "uninstall interrupted; retry to resume safely");
    }
    if (remove_intent_clear() != ESP_OK) {
        ESP_LOGW(TAG, "uninstall committed but intent cleanup deferred");
    }
    ESP_LOGI(TAG, "slot %d and associated DATA removed", rm.slot);
    return reply(req, "200 OK", "ok");
}

// ---- 服务生命周期 ----

esp_err_t meta_install_net_init(meta_slot_info_t slots[META_SLOT_COUNT])
{
    if (!slots) return ESP_ERR_INVALID_ARG;
    if (s_init) return ESP_OK;
    s_slots = slots;
    memset(&s_session, 0, sizeof(s_session));
    s_session.confirmed_slot = -1;
    status_set("idle", "");
    if (!s_session_mu) s_session_mu = xSemaphoreCreateMutex();
    if (!s_session_mu) return ESP_ERR_NO_MEM;
    s_init = true;
    return ESP_OK;
}

esp_err_t meta_install_net_start(void)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (s_httpd) return ESP_OK;   // 幂等:已在跑

    /* Resume interrupted APP/DATA deletion before exposing the management API. */
    bool recovered = false;
    const esp_err_t recovery = remove_recover_pending(&recovered);
    if (recovery != ESP_OK) {
        ESP_LOGE(TAG, "pending uninstall recovery failed: %s", esp_err_to_name(recovery));
        /* Fail closed: do not expose install/remove APIs while a destructive
         * transaction is unresolved. A new install could otherwise change the
         * carve before the persisted uninstall target can be recovered. */
        return recovery;
    }

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 12;    // / + pair/status/prepare/session/chunk/data/finalize/cancel
    hcfg.max_open_sockets = 3;
    hcfg.backlog_conn = 2;
    hcfg.lru_purge_enable = true;
    // 2026-10-04 真机栈溢出:handler 帧 + newlib _svfprintf_r(snprintf
    // 内部,含 FP 格式化路径)在 4096B 上无安全余量(SP 越界 ~1KB 实测)。
    // 3072B 响应虽已静态化,仍上调到 8192 留一倍余量。
    hcfg.stack_size = 8192;
    hcfg.recv_wait_timeout = 10;
    hcfg.send_wait_timeout = 10;

    const esp_err_t err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "install httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { "/",                    HTTP_GET,  h_boot_index,     NULL },
        { "/api/install/pair",    HTTP_POST, h_install_pair,    NULL },
        { "/api/install/status",  HTTP_GET,  h_install_status,  NULL },
        { "/api/install/prepare", HTTP_POST, h_install_prepare, NULL },
        { "/api/install/session", HTTP_POST, h_install_session, NULL },
        { "/api/install/chunk",   HTTP_POST, h_install_chunk,   NULL },
        { "/api/install/data",    HTTP_POST, h_install_data,    NULL },
        { "/api/install/finalize",HTTP_POST, h_install_finalize,NULL },
        { "/api/install/cancel",  HTTP_POST, h_install_cancel,  NULL },
        { "/api/install/remove",  HTTP_POST, h_install_remove,  NULL },
        { "/api/backup/import",   HTTP_POST, h_backup_import,   NULL },
        { "/api/install/slots",   HTTP_GET,  h_install_slots,   NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_httpd, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed", uris[i].uri);
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "LAN install service ready");
    return ESP_OK;
}

void meta_install_net_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    meta_install_token_stop();   // 幂等:未开 token 时也只是清空状态
}

// POST /api/backup/import —— 暂停旧版仅元数据导入，防止创建没有真实内容的 DATA。
static esp_err_t h_backup_import(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    return reply(req, "501 Not Implemented",
                 "byte-level backup restore is not implemented; metadata-only import is disabled");
}
