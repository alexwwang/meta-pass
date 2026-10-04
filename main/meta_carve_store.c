// main/meta_carve_store.c —— 见 meta_carve_store.h。纯逻辑,host/bootloader 共用。
#include "meta_carve_store.h"

#include <string.h>

// ---- CRC32(标准反射型,zlib 同值) ---------------------------------------

uint32_t meta_carve_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        p[k] = (uint8_t)(v >> (8 * k));
    }
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

// ---- encode ---------------------------------------------------------------

bool meta_carve_rec_encode(const meta_carve_rec_t *rec,
                           uint8_t out[META_CARVE_REC_SIZE])
{
    if (!rec || !out) {
        return false;
    }
    if (rec->carve.count > META_CARVE_MAX_SLOTS ||
        !meta_carve_valid(&rec->carve)) {
        return false;
    }
    if (rec->seq == 0u || rec->seq == 0xFFFFFFFFu) {
        return false;
    }

    if (rec->carve.data_count > META_DATA_MAX) {
        return false;
    }

    memset(out, 0xFF, META_CARVE_REC_SIZE);   // erase-before-write 语义
    wr_u32(out + 0, META_CARVE_REC_MAGIC);
    wr_u16(out + 4, (uint16_t)META_CARVE_REC_VERSION);
    wr_u16(out + 6, rec->carve.count);
    wr_u32(out + 8, rec->seq);
    wr_u16(out + 12, rec->carve.data_count);
    // [14,16) reserved 保持 0xFF

    for (uint8_t i = 0; i < rec->carve.count; i++) {
        const meta_carve_slot_t *s = &rec->carve.slot[i];
        uint8_t *p = out + META_CARVE_REC_HEADER + (size_t)i * META_CARVE_REC_SLOT_SIZE;
        p[0] = s->state;
        p[1] = s->kind;
        // [2,4) reserved 保持 0xFF
        wr_u32(p + 4, s->offset);
        wr_u32(p + 8, s->size);
        wr_u32(p + 12, s->image_len);
        wr_u32(p + 16, s->play_id);
        memcpy(p + 20, s->image_sha256, 32);
        memset(p + 52, 0, 40);
        size_t n = 0;
        while (s->name[n] != '\0' && n < 40) {
            p[52 + n] = (uint8_t)s->name[n];
            n++;
        }
    }
    memcpy(out + META_CARVE_REC_TABLE_OFF, rec->table, META_PT_SIZE);

    for (uint8_t i = 0; i < rec->carve.data_count; i++) {
        const meta_carve_data_t *d = &rec->carve.data[i];
        uint8_t *p = out + META_CARVE_REC_DATA_OFF +
                     (size_t)i * META_CARVE_REC_DATA_SIZE;
        wr_u32(p + 0, d->play_id);
        wr_u32(p + 4, d->offset);
        wr_u32(p + 8, d->size);
        p[12] = d->state;
        p[13] = d->subtype;
        p[14] = d->type;
        // p[15] reserved 保持 0xFF
        memset(p + 16, 0, 16);
        size_t n = 0;
        while (d->label[n] != '\0' && n < META_DATA_LABEL_MAX) {
            p[16 + n] = (uint8_t)d->label[n];
            n++;
        }
    }
    wr_u32(out + META_CARVE_REC_CRC_OFF,
           meta_carve_crc32(out, META_CARVE_REC_CRC_OFF));
    return true;
}

// ---- decode ---------------------------------------------------------------

// 版本 → 该布局的 CRC 覆盖长度与 CRC 存放偏移(v1 只读兼容)。
static bool rec_layout(uint16_t version, size_t *crc_len, size_t *crc_off)
{
    if (version == META_CARVE_REC_VERSION) {
        *crc_len = META_CARVE_REC_CRC_OFF;
        *crc_off = META_CARVE_REC_CRC_OFF;
        return true;
    }
    if (version == META_CARVE_REC_V1_VERSION) {
        *crc_len = META_CARVE_REC_V1_CRC_OFF;
        *crc_off = META_CARVE_REC_V1_CRC_OFF;
        return true;
    }
    return false;
}

bool meta_carve_rec_raw_info(const uint8_t raw[META_CARVE_REC_SIZE],
                             uint32_t *seq_out)
{
    if (!raw) {
        return false;
    }
    if (rd_u32(raw + 0) != META_CARVE_REC_MAGIC) {
        return false;
    }
    const uint16_t version = (uint16_t)(raw[4] | (raw[5] << 8));   // version u16 @4
    size_t crc_len = 0, crc_off = 0;
    if (!rec_layout(version, &crc_len, &crc_off)) {
        return false;
    }
    const uint16_t count = (uint16_t)(raw[6] | (raw[7] << 8));
    if (count > META_CARVE_MAX_SLOTS) {
        return false;
    }
    const uint32_t seq = rd_u32(raw + 8);
    if (seq == 0u || seq == 0xFFFFFFFFu) {
        return false;
    }
    if (meta_carve_crc32(raw, crc_len) != rd_u32(raw + crc_off)) {
        return false;
    }
    if (seq_out) {
        *seq_out = seq;
    }
    return true;
}

bool meta_carve_rec_decode(const uint8_t raw[META_CARVE_REC_SIZE],
                           meta_carve_rec_t *out)
{
    uint32_t seq = 0;
    if (!out || !meta_carve_rec_raw_info(raw, &seq)) {
        return false;
    }
    const uint16_t version = (uint16_t)(raw[4] | (raw[5] << 8));
    const uint16_t count = (uint16_t)(raw[6] | (raw[7] << 8));   // raw_info 已验边界
    const bool v1 = (version == META_CARVE_REC_V1_VERSION);
    const size_t slot_stride = v1 ? META_CARVE_REC_V1_SLOT_SIZE
                                  : META_CARVE_REC_SLOT_SIZE;
    const size_t table_off = v1 ? META_CARVE_REC_V1_TABLE_OFF
                                : META_CARVE_REC_TABLE_OFF;

    // 直接物化 *out:不做 3.9KB 栈上临时记录(bootloader 栈小,§4.4);
    // 失败路径 *out 未定义 —— 所有调用方先判返回值再使用(头文件契约)。
    memset(out, 0, sizeof(*out));
    out->seq = seq;
    out->carve.count = count;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = raw + META_CARVE_REC_HEADER + (size_t)i * slot_stride;
        meta_carve_slot_t *s = &out->carve.slot[i];
        s->state = p[0];
        s->kind = p[1];
        s->offset = rd_u32(p + 4);
        s->size = rd_u32(p + 8);
        s->image_len = rd_u32(p + 12);
        if (!v1) {
            s->play_id = rd_u32(p + 16);
        }   // v1 无 play_id 字段 → 0(旧记录不与数据记录联动)
        memcpy(s->image_sha256, p + (v1 ? 16 : 20), 32);
        memcpy(s->name, p + (v1 ? 48 : 52), 40);
        s->name[40] = '\0';   // 记录内 name[40] 不带 NUL → 强制终结(防越界)
        if (s->state > META_SLOT_INVALID || s->kind > META_CARVE_KIND_STORAGE) {
            return false;
        }
    }
    if (!v1) {
        const uint16_t data_count = (uint16_t)(raw[12] | (raw[13] << 8));
        if (data_count > META_DATA_MAX) {
            return false;
        }
        out->carve.data_count = (uint8_t)data_count;
        for (uint16_t i = 0; i < data_count; i++) {
            const uint8_t *p = raw + META_CARVE_REC_DATA_OFF +
                               (size_t)i * META_CARVE_REC_DATA_SIZE;
            meta_carve_data_t *d = &out->carve.data[i];
            d->play_id = rd_u32(p + 0);
            d->offset = rd_u32(p + 4);
            d->size = rd_u32(p + 8);
            d->state = p[12];
            d->subtype = p[13];
            d->type = p[14];
            memcpy(d->label, p + 16, META_DATA_LABEL_MAX);
            d->label[META_DATA_LABEL_MAX] = '\0';   // 强制终结(防越界)
            if (d->state > META_DATA_ARCHIVED || d->type != 1u ||
                d->play_id == 0u) {
                return false;   // 格式层拒:非法状态/非 data 型/无归属玩法
            }
        }
    }   // v1 无 data 区 → data_count 保持 0
    memcpy(out->table, raw + table_off, META_PT_SIZE);
    // 内嵌表 MD5 校验走 O(1) 栈版本:本函数会被 bootloader hook 与
    // app_main ensure 调用,decode 的 ~640B 结构缓冲在此是二次溢出源。
    if (!meta_pt_check(out->table)) {
        return false;   // 内嵌表 MD5 损坏
    }
    return true;
}

bool meta_carve_rec_validate(const meta_carve_rec_t *rec)
{
    if (!rec) {
        return false;
    }
    // 一致性边界(2026-10-04 修订,根因:QEMU 定案的栈溢出 + dram_seg 溢出):
    // 旧实现在此 materialize 整张表做字节比对(uint8_t expect[3072] 栈帧 +
    // meta_pt_from_carve 内部 ~640B)——调用方含 bootloader hook(8KB 栈、
    // dram_seg 预算仅剩 <1.6KB)与 app_main(3584B 主栈),任何"flash 存在
    // 合法记录"的启动都会栈溢出 → panic → software_reset(真机 = 变砖;
    // 只有装过玩法的设备有记录,故 fresh 设备测试永远发现不了)。
    // 该比对的防御价值由既有检查完整覆盖,故移除:
    //   - decode 已验内嵌表的 MD5(meta_pt_decode)——表字节损坏拒于门外;
    //   - meta_carve_valid 已验 carve 结构不变量(数量/对齐/池内/不重叠);
    //   - decide/ensure 逐字节比对 live 表 vs rec.table——表与 carve 语义
    //     分歧的检测点本就在那里,且下一次 commit 会用 carve 重铸表自愈。
    // 剩余风险(表内部合法但与 carve 语义不符)仅为瞬态物化分歧,无安全影响。
    return meta_carve_valid(&rec->carve);
}

// ---- A/B pick -------------------------------------------------------------

bool meta_carve_rec_pick(const uint8_t a[META_CARVE_REC_SIZE],
                         const uint8_t b[META_CARVE_REC_SIZE],
                         meta_carve_rec_t *out, bool *from_a)
{
    if (!a || !b || !out) {
        return false;
    }
    meta_carve_rec_t ra, rb;
    const bool va = meta_carve_rec_decode(a, &ra) && meta_carve_rec_validate(&ra);
    const bool vb = meta_carve_rec_decode(b, &rb) && meta_carve_rec_validate(&rb);
    if (!va && !vb) {
        return false;
    }
    if (va && vb) {
        // 有符号差回绕比较:新者胜(1 新于 0xFFFFFFFE)。
        const bool a_newer = (int32_t)(ra.seq - rb.seq) > 0;
        *out = a_newer ? ra : rb;
        if (from_a) {
            *from_a = a_newer;
        }
        return true;
    }
    *out = va ? ra : rb;
    if (from_a) {
        *from_a = va;
    }
    return true;
}
