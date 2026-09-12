// main/meta_name.h —— 槽位分区尾部"显示名 blob"的打包/解包。
//
// 约定:每个 OTA 分区尾部的最后 4KB sector 存放 blob;不同槽位分区大小不同,
// 故 blob 偏移按槽位分区大小动态计算(分区大小 - 4KB):
//   slot_offset + meta_name_blob_offset(part_size) = partition end - 4096
// 应用镜像的合法上限由此收紧为 part_size - 4KB(blob sector)。
//
// Blob 布局(从 sector 头起):
//   [0..3]  magic: 'M' 'N' 'A' 'M'  (0x4D 0x4E 0x41 0x4D)
//   [4]     name_len: 1..META_NAME_MAX(1..32)
//   [5..5+len)  name bytes (只接受可打印 ASCII 0x20..0x7E)
//   [5+len] checksum: 对 [4..5+len) 全部字节做 XOR 折叠
//
// 校验失败(魔术错 / 长度越界 / 含非可打印字节 / checksum 不符)一律视为无 blob。
//
// 与 meta_store 配合:扫描时优先读 blob 真名,真名缺失则回退 project_name(现有
// meta_slot_core_name 逻辑不动)。与 install-slot 的 name-blob.js 保持字节级一致。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define META_NAME_MAX            32u   // 显示名最大字节数(对齐 meta_slots 的 META_NAME_LEN,小屏也够)
#define META_NAME_BLOB_HEADER    6u          // magic(4) + len(1) + xor(1) 最小开销
#define META_NAME_BLOB_SECTOR    4096u       // blob 所在 sector 字节数(仅用于擦除)

// 按槽位分区大小计算 blob sector 距槽位起始的偏移(分区末尾 4KB)。
// 不同槽位分区大小不同(ota_0=0x1D6000、ota_1=0x200000、ota_2=0x29E000),
// 故不再用全局常量,改由调用方传入分区大小动态计算。
static inline uint32_t meta_name_blob_offset(uint32_t part_size)
{
    return part_size - META_NAME_BLOB_SECTOR;
}

// 按槽位分区大小计算应用镜像字节上限(分区大小减去尾部 4KB blob sector)。
static inline uint32_t meta_name_max_app_size(uint32_t part_size)
{
    return part_size - META_NAME_BLOB_SECTOR;
}

// 打包 name 到 out,不超过 cap 字节。返回写入的总字节数(不含溢出截断);
// name 非法(NULL/空/超 META_NAME_MAX/含非可打印 ASCII)或 cap 不足 → 返回 0,out 不变。
// 成功写 blob 共 META_NAME_BLOB_HEADER + len 字节。
size_t meta_name_pack(const char *name, uint8_t *out, size_t cap);

// 解包:buf 需至少含 META_NAME_BLOB_HEADER 字节。校验通过把名字拷到 out(带 '\0'),
// 返回 true;校验失败(out 可能部分被写)返回 false。out_cap 必须 ≥ META_NAME_MAX + 1。
bool meta_name_unpack(const uint8_t *buf, size_t len, char *out, size_t out_cap);
