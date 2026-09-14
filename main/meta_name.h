// main/meta_name.h —— 槽位显示名 blob 的打包/解包。
//
// 约定:每个槽位的 metadata sector 紧跟 app image(image_len 之后,4K 对齐)。
// sector 内布局见 meta_sign.h: MSIG@0 / MAEG@128..4055 / MNAM@4056..4095。
// MNAM 使用 sector 末尾固定 40B 窗口;变长 blob 在窗口内右对齐存放,
// 窗口最后一字节必须是 checksum,前部未用字节保持 0xFF 作为安全边界。
//
// Blob 字节格式不变:
//   [0..3]  magic: 'M' 'N' 'A' 'M'
//   [4]     name_len: 1..META_NAME_MAX(1..32)
//   [5..5+len)  name bytes (只接受可打印 ASCII 0x20..0x7E)
//   [5+len] checksum: 对 [4..5+len) 全部字节做 XOR 折叠
//
// 校验失败(魔术错 / 长度越界 / 含非可打印字节 / checksum 不符 / 未右对齐)一律视为无 blob。
// 与 install-slot 的 name-blob.js 保持字节级一致。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "meta_sign.h"

#define META_NAME_MAX          32u          // name 最大 ASCII 字节数(不含 NUL)
#define META_NAME_BLOB_HEADER  6u           // magic(4) + len(1) + xor(1) 最小开销
#define META_NAME_BLOB_MAX     38u          // magic(4) + len(1) + name(32) + xor(1)
#define META_NAME_BLOB_RESERVE 40u          // 末尾 40B 固定窗口

// 按 app image 长度计算 metadata sector 距槽位起始的偏移(image_len 后 4K 对齐)。
static inline uint32_t meta_name_sector_offset(uint32_t image_len)
{
    return meta_sign_sector_offset(image_len);
}

// 按 app image 长度计算 MNAM 40B 窗口起点(metadata sector + 4056)。
static inline uint32_t meta_name_blob_offset(uint32_t image_len)
{
    return meta_name_sector_offset(image_len) + META_NAME_BLOB_OFF;
}

// 按槽位分区大小计算应用镜像字节上限(分区大小减去单一 metadata sector)。
static inline uint32_t meta_name_max_app_size(uint32_t part_size)
{
    return part_size - META_SIG_SECTOR;
}

// 打包 name 到 out,不超过 cap 字节。返回写入的总字节数(不含溢出截断);
// name 非法(NULL/空/超 META_NAME_MAX/含非可打印 ASCII)或 cap 不足 → 返回 0,out 不变。
// 成功写 blob 共 META_NAME_BLOB_HEADER + len 字节。
size_t meta_name_pack(const char *name, uint8_t *out, size_t cap);

// 解包紧凑 blob:buf 从 magic 开始。校验通过把名字拷到 out(带 '\0'),
// 返回 true;校验失败(out 可能部分被写)返回 false。out_cap 必须 ≥ META_NAME_MAX + 1。
bool meta_name_unpack(const uint8_t *buf, size_t len, char *out, size_t out_cap);

// 打包 name 到 40B MNAM 窗口并右对齐:窗口最后一字节是 checksum,前部由调用方保持 0xFF。
// 返回实际 blob 字节数;失败返回 0 且不改 window。
size_t meta_name_pack_tail(const char *name, uint8_t *window, size_t cap);

// 解包 40B MNAM 窗口:只接受右对齐 blob(窗口最后一字节必须是 checksum)。
bool meta_name_unpack_tail(const uint8_t *window, size_t len, char *out, size_t out_cap);
