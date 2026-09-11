// main/meta_slots.c —— 实现见头文件注释。
#include "meta_slots.h"

#include <string.h>

void meta_slot_clear(meta_slot_info_t *slot)
{
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
    slot->state = META_SLOT_EMPTY;
}

bool meta_slot_bootable(const meta_slot_info_t *slot)
{
    return slot && slot->state == META_SLOT_VALID;
}

// 恰好 64 位小写十六进制(0-9a-f)。其他字符或长度不符都拒绝。
static bool sha256_hex_ok(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < META_SHA256_HEX_LEN; i++) {
        const char c = s[i];
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        if (!digit && !lower) return false;
    }
    return s[META_SHA256_HEX_LEN] == '\0';
}

static bool copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!src) return false;
    const size_t n = strlen(src);
    if (n > cap - 1) return false;   // 必须留结尾零字节
    memcpy(dst, src, n + 1);
    return true;
}

bool meta_slot_set_valid(meta_slot_info_t *slot, const char *name, const char *version,
                         uint32_t size, const char *sha256_hex)
{
    if (!slot) return false;
    if (!sha256_hex_ok(sha256_hex)) return false;
    // 先写入局部副本,全部成功才落到 slot,保证失败路径不破坏原状态。
    char name_copy[META_NAME_LEN + 1];
    char ver_copy[META_VERSION_LEN + 1];
    if (!copy_bounded(name_copy, sizeof(name_copy), name)) return false;
    if (!copy_bounded(ver_copy, sizeof(ver_copy), version)) return false;
    memcpy(slot->name, name_copy, sizeof(name_copy));
    memcpy(slot->version, ver_copy, sizeof(ver_copy));
    memcpy(slot->sha256_hex, sha256_hex, META_SHA256_HEX_LEN + 1);
    slot->size = size;
    slot->state = META_SLOT_VALID;
    return true;
}

void meta_slot_mark_invalid(meta_slot_info_t *slot)
{
    if (!slot) return;
    slot->state = META_SLOT_INVALID;
}

const char *meta_slot_core_name(const meta_slot_info_t *slot)
{
    if (!slot) return "";
    const char *name = slot->name;
    static const char PREFIX[] = "FoloToy";
    const size_t plen = sizeof(PREFIX) - 1;
    if (strncmp(name, PREFIX, plen) == 0) {
        const char sep = name[plen];
        if (sep == '-' || sep == '_' || sep == ' ') {
            const char *core = name + plen + 1;
            if (core[0] != '\0') return core;
        }
    }
    return name;
}
