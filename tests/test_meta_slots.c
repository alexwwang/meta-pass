// tests/test_meta_slots.c —— meta_slots 槽位注册表的 host 测试。
#include <assert.h>
#include <string.h>
#include "meta_slots.h"

static const char SHA[META_SHA256_HEX_LEN + 1] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

int main(void)
{
    meta_slot_info_t s;

    // 初始为空,不可启动
    meta_slot_clear(&s);
    assert(s.state == META_SLOT_EMPTY);
    assert(!meta_slot_bootable(&s));
    assert(s.name[0] == '\0');
    assert(s.size == 0);

    // 写入合法元数据 → VALID,可启动
    assert(meta_slot_set_valid(&s, "tetris-game", "v1.0.0", 1513760, SHA));
    assert(s.state == META_SLOT_VALID);
    assert(meta_slot_bootable(&s));
    assert(strcmp(s.name, "tetris-game") == 0);
    assert(strcmp(s.version, "v1.0.0") == 0);
    assert(s.size == 1513760);
    assert(strcmp(s.sha256_hex, SHA) == 0);

    // 边界:名称恰好 32 字符(esp_app_desc_t 定长上限)可接受
    assert(meta_slot_set_valid(&s, "01234567890123456789012345678901", "v", 1, SHA));
    // 超长名称拒绝且不破坏原状态
    assert(!meta_slot_set_valid(&s, "012345678901234567890123456789012", "v", 1, SHA));
    assert(s.state == META_SLOT_VALID);
    assert(strcmp(s.name, "01234567890123456789012345678901") == 0);

    // sha256_hex 必须恰好 64 个十六进制字符
    assert(!meta_slot_set_valid(&s, "a", "v", 1, "abc"));
    assert(!meta_slot_set_valid(&s, "a", "v", 1,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));

    // 标记无效 → 不可启动
    meta_slot_mark_invalid(&s);
    assert(s.state == META_SLOT_INVALID);
    assert(!meta_slot_bootable(&s));

    // 清空 → 回到 EMPTY
    meta_slot_clear(&s);
    assert(s.state == META_SLOT_EMPTY);
    assert(!meta_slot_bootable(&s));

    // 核心名:剥掉 "FoloToy-" 前缀用于列表显示
    assert(meta_slot_set_valid(&s, "FoloToy-AI-Passport", "1", 1, SHA));
    assert(strcmp(meta_slot_core_name(&s), "AI-Passport") == 0);
    assert(meta_slot_set_valid(&s, "FoloToy-Card", "1", 1, SHA));
    assert(strcmp(meta_slot_core_name(&s), "Card") == 0);
    assert(meta_slot_set_valid(&s, "FoloToy_Card", "1", 1, SHA));
    assert(strcmp(meta_slot_core_name(&s), "Card") == 0);
    // 无前缀 / 剥完为空 → 原名原样返回
    assert(meta_slot_set_valid(&s, "tetris-game", "1", 1, SHA));
    assert(strcmp(meta_slot_core_name(&s), "tetris-game") == 0);
    assert(meta_slot_set_valid(&s, "FoloToy", "1", 1, SHA));
    assert(strcmp(meta_slot_core_name(&s), "FoloToy") == 0);
    // 空槽 / NULL → 空串
    meta_slot_clear(&s);
    assert(strcmp(meta_slot_core_name(&s), "") == 0);
    assert(strcmp(meta_slot_core_name(NULL), "") == 0);
    return 0;
}
