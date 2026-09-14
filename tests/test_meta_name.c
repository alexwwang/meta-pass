// tests/test_meta_name.c —— meta_name 显示名 blob 打包/解包的 host 测试。
// 期望字节序列与 tools/install-slot/test-extract.mjs 中的同名向量逐字节一致(双向锁定)。
#include <assert.h>
#include <string.h>
#include "meta_name.h"

// "Pocket Walkie"(13 字节):magic + len=0x0D + ASCII + XOR checksum 0x36
static const uint8_t BLOB_POCKET_WALKIE[] = {
    0x4d, 0x4e, 0x41, 0x4d, 0x0d,
    0x50, 0x6f, 0x63, 0x6b, 0x65, 0x74, 0x20, 0x57, 0x61, 0x6c, 0x6b, 0x69, 0x65,
    0x36,
};

// "Radar"(5 字节):checksum 0x41
static const uint8_t BLOB_RADAR[] = {
    0x4d, 0x4e, 0x41, 0x4d, 0x05,
    0x52, 0x61, 0x64, 0x61, 0x72,
    0x41,
};

// 恰好 32 字节的名字("0123456789"×3 + "01"),checksum 0x20
static const char NAME_32[META_NAME_MAX + 1] =
    "01234567890123456789012345678901";
static const uint8_t BLOB_32[] = {
    0x4d, 0x4e, 0x41, 0x4d, 0x20,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x30, 0x31,
    0x20,
};

int main(void)
{
    uint8_t buf[64];
    char out[META_NAME_MAX + 1];

    // ---- 打包:与 JS 侧锁定的期望字节序列 ----
    memset(buf, 0xEE, sizeof(buf));
    assert(meta_name_pack("Pocket Walkie", buf, sizeof(buf)) == sizeof(BLOB_POCKET_WALKIE));
    assert(memcmp(buf, BLOB_POCKET_WALKIE, sizeof(BLOB_POCKET_WALKIE)) == 0);

    memset(buf, 0xEE, sizeof(buf));
    assert(meta_name_pack("Radar", buf, sizeof(buf)) == sizeof(BLOB_RADAR));
    assert(memcmp(buf, BLOB_RADAR, sizeof(BLOB_RADAR)) == 0);

    memset(buf, 0xEE, sizeof(buf));
    assert(meta_name_pack(NAME_32, buf, sizeof(buf)) == sizeof(BLOB_32));
    assert(memcmp(buf, BLOB_32, sizeof(BLOB_32)) == 0);

    // ---- 解包:合法 blob 往返 ----
    assert(meta_name_unpack(BLOB_POCKET_WALKIE, sizeof(BLOB_POCKET_WALKIE), out, sizeof(out)));
    assert(strcmp(out, "Pocket Walkie") == 0);
    assert(meta_name_unpack(BLOB_RADAR, sizeof(BLOB_RADAR), out, sizeof(out)));
    assert(strcmp(out, "Radar") == 0);
    assert(meta_name_unpack(BLOB_32, sizeof(BLOB_32), out, sizeof(out)));
    assert(strcmp(out, NAME_32) == 0);

    // ---- 打包拒绝路径 ----
    assert(meta_name_pack(NULL, buf, sizeof(buf)) == 0);            // NULL 名
    assert(meta_name_pack("", buf, sizeof(buf)) == 0);              // 空名
    assert(meta_name_pack("012345678901234567890123456789012",
                          buf, sizeof(buf)) == 0);                  // 33 字节超长
    assert(meta_name_pack("has\ttab", buf, sizeof(buf)) == 0);      // 控制字符 0x09
    assert(meta_name_pack("has\x7f""del", buf, sizeof(buf)) == 0);  // DEL 0x7F
    assert(meta_name_pack("h\xc3\xa9", buf, sizeof(buf)) == 0);     // 非 ASCII(UTF-8 é)
    // 输出缓冲不足:恰好差 1 字节
    assert(meta_name_pack("Radar", buf, META_NAME_BLOB_HEADER + 5 - 1) == 0);
    // 恰好够长必须成功
    assert(meta_name_pack("Radar", buf, META_NAME_BLOB_HEADER + 5) == sizeof(BLOB_RADAR));

    // ---- 解包拒绝路径(一律视为无 blob)----
    memcpy(buf, BLOB_RADAR, sizeof(BLOB_RADAR));
    buf[0] = 0x00;   // 坏 magic
    assert(!meta_name_unpack(buf, sizeof(BLOB_RADAR), out, sizeof(out)));

    memcpy(buf, BLOB_RADAR, sizeof(BLOB_RADAR));
    buf[sizeof(BLOB_RADAR) - 1] ^= 0x01;   // 坏 checksum
    assert(!meta_name_unpack(buf, sizeof(BLOB_RADAR), out, sizeof(out)));

    memcpy(buf, BLOB_RADAR, sizeof(BLOB_RADAR));
    buf[4] = 0;      // len 越界:0
    assert(!meta_name_unpack(buf, sizeof(BLOB_RADAR), out, sizeof(out)));
    buf[4] = META_NAME_MAX + 1;   // len 越界:33
    assert(!meta_name_unpack(buf, sizeof(BLOB_RADAR), out, sizeof(out)));

    memcpy(buf, BLOB_RADAR, sizeof(BLOB_RADAR));
    buf[5] = 0x01;   // 非可打印字节;重算 checksum 隔离该检查
    buf[sizeof(BLOB_RADAR) - 1] = 0x05 ^ 0x01 ^ 0x61 ^ 0x64 ^ 0x61 ^ 0x72;
    assert(!meta_name_unpack(buf, sizeof(BLOB_RADAR), out, sizeof(out)));

    // buf 太短:不足头部长度 / 不足 len 声明的长度
    assert(!meta_name_unpack(BLOB_RADAR, META_NAME_BLOB_HEADER - 1, out, sizeof(out)));
    assert(!meta_name_unpack(BLOB_RADAR, sizeof(BLOB_RADAR) - 2, out, sizeof(out)));

    // 输出缓冲不足(要求 >= META_NAME_MAX + 1)
    assert(!meta_name_unpack(BLOB_RADAR, sizeof(BLOB_RADAR), out, META_NAME_MAX));

    // NULL 参数
    assert(!meta_name_unpack(NULL, sizeof(BLOB_RADAR), out, sizeof(out)));
    assert(!meta_name_unpack(BLOB_RADAR, sizeof(BLOB_RADAR), NULL, sizeof(out)));

    // 全 0xFF(空 sector)不是合法 blob
    memset(buf, 0xFF, sizeof(buf));
    assert(!meta_name_unpack(buf, sizeof(buf), out, sizeof(out)));

    // ---- 40B MNAM 窗口:右对齐存放,checksum 必须在窗口最后一字节 ----
    uint8_t window[META_NAME_BLOB_RESERVE];
    memset(window, 0xFF, sizeof(window));
    assert(meta_name_pack_tail("Radar", window, sizeof(window)) == sizeof(BLOB_RADAR));
    assert(memcmp(window + sizeof(window) - sizeof(BLOB_RADAR),
                  BLOB_RADAR, sizeof(BLOB_RADAR)) == 0);
    for (size_t i = 0; i < sizeof(window) - sizeof(BLOB_RADAR); i++) {
        assert(window[i] == 0xFF);   // 前部安全边界保持擦除态
    }
    assert(meta_name_unpack_tail(window, sizeof(window), out, sizeof(out)));
    assert(strcmp(out, "Radar") == 0);

    // 正向放在窗口开头不再是合法布局:checksum 不在窗口末尾,必须拒绝
    memset(window, 0xFF, sizeof(window));
    memcpy(window, BLOB_RADAR, sizeof(BLOB_RADAR));
    assert(!meta_name_unpack_tail(window, sizeof(window), out, sizeof(out)));

    // 全 0xFF / 短窗口都不是合法 MNAM
    memset(window, 0xFF, sizeof(window));
    assert(!meta_name_unpack_tail(window, sizeof(window), out, sizeof(out)));
    assert(!meta_name_unpack_tail(window, sizeof(window) - 1, out, sizeof(out)));

    return 0;
}
