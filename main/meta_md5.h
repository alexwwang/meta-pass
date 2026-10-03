// main/meta_md5.h —— 可移植 MD5(RFC 1321),host 测试与固件/bootloader 共用。
//
// 用途仅限分区表 MD5 marker(格式校验,非安全哈希):设备端 IDF 自身也用
// esp_rom_md5 校验 0x8000 表(flash_partitions.c esp_partition_table_verify),
// 我方物化/校验必须逐字节一致。自带实现的原因:host 测试无 mbedtls,且
// bootloader 链接面最小;黄金向量见 tests/test_meta_md5.c(RFC 1321 A.5 +
// play 563 真机表条目)。
#pragma once

#include <stddef.h>
#include <stdint.h>

// data[0..len) 的 MD5 → out[16]。
void meta_md5(const void *data, size_t len, uint8_t out[16]);
