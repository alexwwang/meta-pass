// tests/fixtures/gen_carve_record.c —— 生成 carve 记录黄金夹具(设备侧 C 代码为字节权威)。
//
// 用途: install-slot/dynslot-record.js(USB 安装页)的编解码必须与设备
// main/meta_carve_store.c 逐字节一致 —— JS decode(本夹具) 后 re-encode 必须
// 回到同一份字节,否则页面写出的记录会被设备拒收(CRC/结构/表 MD5 任一层)。
// v1 夹具用于"只读兼容"路径:旧设备记录是 v1 布局(stride 88 / table@720 /
// crc@3792 / 无 play_id / 无 data 区),JS 必须能正确读出同样的字段。
// 对拍测试: tools/install-slot/test-dynslot-record.mjs。
//
// 重新生成(改了记录格式时必须重跑并更新夹具):
//   cc -std=c11 -Wall -Wextra -Werror -Imain \
//      tests/fixtures/gen_carve_record.c main/meta_carve_store.c \
//      main/meta_carve.c main/meta_md5.c -o /tmp/gen_carve_record
//   /tmp/gen_carve_record tests/fixtures/carve_record_golden.bin \
//                         tests/fixtures/carve_record_v1_golden.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta_carve_store.h"

static void fill_common_carve(meta_carve_t *c)
{
    memset(c, 0, sizeof(*c));
    c->count = 2;

    c->slot[0].kind = META_CARVE_KIND_APP;
    c->slot[0].state = META_SLOT_VALID;
    c->slot[0].offset = 0x180000u;
    c->slot[0].size = 0x1D6000u;
    c->slot[0].image_len = 0x1A2B3Cu;
    c->slot[0].play_id = 255u;
    for (int k = 0; k < 32; k++) {
        c->slot[0].image_sha256[k] = (uint8_t)((k * 3 + 1) & 0xFF);
    }
    memcpy(c->slot[0].name, "Demo Play", 10);   /* 含 NUL */

    c->slot[1].kind = META_CARVE_KIND_APP;
    c->slot[1].state = META_SLOT_EMPTY;
    c->slot[1].offset = 0x360000u;
    c->slot[1].size = 0x200000u;
}

static int write_bytes(const char *path, const uint8_t *bytes, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    if (fwrite(bytes, 1, len, f) != len) {
        perror(path);
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

/* v2 当前格式:meta_carve_rec_encode 产出。 */
static int emit_v2(const char *path)
{
    meta_carve_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq = 42u;
    fill_common_carve(&rec.carve);

    rec.carve.data_count = 1;
    rec.carve.data[0].play_id = 42u;
    rec.carve.data[0].offset = 0x700000u;
    rec.carve.data[0].size = 0x10000u;
    rec.carve.data[0].state = META_DATA_ARCHIVED;
    rec.carve.data[0].subtype = 0x41u;
    rec.carve.data[0].type = 1u;
    memcpy(rec.carve.data[0].label, "assets", 7);   /* 含 NUL */

    if (!meta_pt_from_carve(&rec.carve, rec.table)) {
        fprintf(stderr, "v2: meta_pt_from_carve failed\n");
        return 0;
    }

    uint8_t out[META_CARVE_REC_SIZE];
    if (!meta_carve_rec_encode(&rec, out)) {
        fprintf(stderr, "v2: meta_carve_rec_encode failed\n");
        return 0;
    }

    meta_carve_rec_t back;
    if (!meta_carve_rec_decode(out, &back) || !meta_carve_rec_validate(&back) ||
        back.seq != 42u || back.carve.count != 2 || back.carve.data_count != 1 ||
        strcmp(back.carve.slot[0].name, "Demo Play") != 0 ||
        back.carve.slot[0].play_id != 255u) {
        fprintf(stderr, "v2: round-trip verify failed\n");
        return 0;
    }
    return write_bytes(path, out, sizeof(out));
}

/* v1 老布局(已部署设备的记录只读兼容):手写 + 设备解码器自检。 */
static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        p[k] = (uint8_t)(v >> (8 * k));
    }
}

static int emit_v1(const char *path)
{
    meta_carve_t c;
    fill_common_carve(&c);
    c.slot[0].play_id = 0;   /* v1 无 play_id 字段:解码后恒 0 */

    uint8_t table[META_PT_SIZE];
    if (!meta_pt_from_carve(&c, table)) {
        fprintf(stderr, "v1: meta_pt_from_carve failed\n");
        return 0;
    }

    uint8_t out[META_CARVE_REC_SIZE];
    memset(out, 0xFF, META_CARVE_REC_SIZE);
    wr_u32(out + 0, META_CARVE_REC_MAGIC);
    wr_u16(out + 4, (uint16_t)META_CARVE_REC_V1_VERSION);
    wr_u16(out + 6, 2);            /* slot_count */
    wr_u32(out + 8, 7);            /* seq */
    /* [12,16) 保留(v1 无 data_count 字段) */

    for (uint8_t i = 0; i < c.count; i++) {
        const meta_carve_slot_t *s = &c.slot[i];
        uint8_t *p = out + META_CARVE_REC_HEADER +
                     (size_t)i * META_CARVE_REC_V1_SLOT_SIZE;
        p[0] = s->state;
        p[1] = s->kind;
        wr_u32(p + 4, s->offset);
        wr_u32(p + 8, s->size);
        wr_u32(p + 12, s->image_len);
        memcpy(p + 16, s->image_sha256, 32);
        memset(p + 48, 0, 40);
        memcpy(p + 48, s->name, strlen(s->name));   /* 零填充至 40B */
    }
    memcpy(out + META_CARVE_REC_V1_TABLE_OFF, table, META_PT_SIZE);
    wr_u32(out + META_CARVE_REC_V1_CRC_OFF,
           meta_carve_crc32(out, META_CARVE_REC_V1_CRC_OFF));

    meta_carve_rec_t back;
    if (!meta_carve_rec_decode(out, &back) || !meta_carve_rec_validate(&back)) {
        fprintf(stderr, "v1: device decode rejected the fixture\n");
        return 0;
    }
    if (back.seq != 7u || back.carve.count != 2 || back.carve.data_count != 0 ||
        back.carve.slot[0].play_id != 0u ||
        back.carve.slot[0].offset != 0x180000u ||
        back.carve.slot[0].size != 0x1D6000u ||
        back.carve.slot[0].image_len != 0x1A2B3Cu ||
        strcmp(back.carve.slot[0].name, "Demo Play") != 0 ||
        back.carve.slot[1].state != META_SLOT_EMPTY) {
        fprintf(stderr, "v1: field verify failed\n");
        return 0;
    }
    return write_bytes(path, out, sizeof(out));
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <v2_out.bin> [v1_out.bin]\n", argv[0]);
        return 2;
    }
    if (!emit_v2(argv[1])) {
        return 1;
    }
    printf("wrote %s (v2, seq=42 slots=2 data=1)\n", argv[1]);

    if (argc == 3) {
        if (!emit_v1(argv[2])) {
            return 1;
        }
        printf("wrote %s (v1, seq=7 slots=2, read-compat)\n", argv[2]);
    }
    return 0;
}
