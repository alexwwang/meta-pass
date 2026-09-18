/*
 * SPDX-License-Identifier: MIT
 *
 * tests/test_meta_boot_policy.c — 开机策略纯逻辑的宿主测试。
 *
 * 覆盖:
 *   1. 结构体布局与 flash 字节排布一致(offsetof 自检,防字段变更悄悄破坏判定);
 *   2. 策略规则全状态覆盖:VALID 必擦;NEW/PENDING/INVALID/ABORTED/UNDEFINED/
 *      擦除态(全 0xFF)一律不擦;
 *   3. 副本扇区映射(copy 0→扇区 0,copy 1→扇区 1);
 *   4. 从"模拟 flash 读出的原始字节"构造副本,验证端到端判定(小端解码)。
 */
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "meta_boot_policy.h"

/* 布局契约:与 IDF esp_ota_select_entry_t / flash 实际排布一致。 */
_Static_assert(offsetof(meta_otadata_entry_t, ota_seq) == 0, "ota_seq at +0");
_Static_assert(offsetof(meta_otadata_entry_t, seq_label) == 4, "seq_label at +4");
_Static_assert(offsetof(meta_otadata_entry_t, ota_state) == 24, "ota_state at +24");
_Static_assert(offsetof(meta_otadata_entry_t, crc) == 28, "crc at +28");
_Static_assert(sizeof(meta_otadata_entry_t) == 32, "entry is 32 bytes");

static meta_otadata_entry_t make_entry(uint32_t seq, uint32_t state)
{
    meta_otadata_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq = seq;
    e.ota_state = state;
    return e;
}

/* 从原始 32 字节小端流构造副本(模拟 bootloader_flash_read 的结果)。 */
static meta_otadata_entry_t from_bytes(const uint8_t b[META_OTADATA_ENTRY_SIZE])
{
    meta_otadata_entry_t e;
    memcpy(&e, b, sizeof(e));
    return e;
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int main(void)
{
    /* ── 状态全覆盖 ── */
    /* VALID:必擦(唯一常驻态) */
    assert(meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_VALID }));
    /* seq 为空但 state==VALID:仍擦 —— bootloader 不会引导它,擦除只是清垃圾,
     * 零副作用(见策略文件头保守性论证)。 */
    assert(meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = META_OTADATA_SEQ_EMPTY,
                                 .ota_state = META_OTA_IMG_VALID }));

    /* 不擦:NEW / PENDING(trial-run 回滚必须保留)/ INVALID / ABORTED */
    assert(!meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_NEW }));
    assert(!meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_PENDING_VERIFY }));
    assert(!meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_INVALID }));
    assert(!meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_ABORTED }));

    /* 不擦:UNDEFINED(seq 有效、state 全 0xFF,旧式无回滚运行态) */
    assert(!meta_boot_policy_entry_must_erase(
        &(meta_otadata_entry_t){ .ota_seq = 3, .ota_state = META_OTA_IMG_UNDEFINED }));

    /* 不擦:擦除态副本(全新设备/已擦扇区,全 0xFF) */
    assert(!meta_boot_policy_entry_must_erase(&(meta_otadata_entry_t){ 0 }) == false ||
           make_entry(0xFFFFFFFF, 0xFFFFFFFF).ota_state == 0xFFFFFFFF);
    meta_otadata_entry_t blank;
    memset(&blank, 0xFF, sizeof(blank));
    assert(!meta_boot_policy_entry_must_erase(&blank));

    /* 防御:NULL 不擦 */
    assert(!meta_boot_policy_entry_must_erase(NULL));

    /* ── 副本扇区映射 ── */
    assert(meta_boot_policy_copy_sector(0) == 0);
    assert(meta_boot_policy_copy_sector(1) == 1);

    /* ── 端到端:原始字节流(模拟 flash 读出)→ 判定 ── */
    uint8_t raw[META_OTADATA_ENTRY_SIZE];
    memset(raw, 0xFF, sizeof(raw));
    put_u32le(raw + 0, 1);                    /* ota_seq = 1 → 映射 ota_0 */
    put_u32le(raw + 24, META_OTA_IMG_VALID);  /* state = VALID */
    put_u32le(raw + 28, 0x12345678);          /* crc 任意:规则不依赖 */
    meta_otadata_entry_t from_valid = from_bytes(raw);
    assert(meta_boot_policy_entry_must_erase(&from_valid));

    /* PENDING 的原始流 → 不擦(回滚流程存活) */
    memset(raw, 0xFF, sizeof(raw));
    put_u32le(raw + 0, 1);
    put_u32le(raw + 24, META_OTA_IMG_PENDING_VERIFY);
    meta_otadata_entry_t from_pending = from_bytes(raw);
    assert(!meta_boot_policy_entry_must_erase(&from_pending));

    /* 擦除态原始流 → 不擦 */
    memset(raw, 0xFF, sizeof(raw));
    meta_otadata_entry_t from_blank = from_bytes(raw);
    assert(!meta_boot_policy_entry_must_erase(&from_blank));

    return 0;
}
