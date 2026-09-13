// main/meta_sign.h —— meta-pass 应用层签名徽章(软件级,可逆)。
//
// 设计见 docs/assets/meta-pass-design.md §7。
// 签名段附加在 ESP app image 的 image_len 之后、name blob sector 之前。
// 布局(OTA 分区内):
//   [app image (image_len 字节)] [sig sector (4KB)] [name blob sector (4KB)]
// 格式(可变长度,签名段最大 81 字节,写入时 pad 到 4K):
//   [0..3]   magic "MSIG"
//   [4..7]   payload_len (uint32 LE) = DER 签名长度(70..72 字节)
//   [8..8+len)  ECDSA-P256 DER 签名(对 image 的 SHA-256 digest 签)
//   [8+len] xor checksum(前 8+len 字节异或)
//
// esp_image_verify 只校验 image_len 范围,不读签名段,无冲突。
// scan_one 在 esp_image_verify 通过后尝试验签。
//
// 密钥:私钥 tools/signing/private.pem(不入仓库);公钥编译期嵌入固件。
// 签名工具:tools/signing/sign-firmware.sh
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define META_SIG_MAGIC       0x4D534947u   // "MSIG" (字节序为 'M','S','I','G')
#define META_SIG_MAGIC_BYTES { 'M', 'S', 'I', 'G' }
#define META_SIG_HEADER_LEN  8u             // magic(4) + payload_len(4)
#define META_SIG_SIG_LEN     72u            // ECDSA-P256 DER 签名最大长度
#define META_SIG_TOTAL_LEN   (META_SIG_HEADER_LEN + META_SIG_SIG_LEN + 1u)  // 81
#define META_SIG_SECTOR       4096u         // 签名段占用 1 个 4K sector
#define META_NAME_BLOB_SECTOR 4096u         // 显示名 blob 占用 1 个 4K sector

// 签名 sector 在槽位中的偏移:image_len 之后,4K 对齐。
static inline uint32_t meta_sign_sector_offset(uint32_t image_len)
{
    return (image_len + META_SIG_SECTOR - 1u) & ~(META_SIG_SECTOR - 1u);
}

// 应用镜像上限:分区大小减去签名 sector 和显示名 blob sector。
static inline uint32_t meta_sign_app_limit(uint32_t part_size)
{
    return part_size - META_SIG_SECTOR - META_NAME_BLOB_SECTOR;
}

// 验签结果。
typedef enum {
    META_SIG_OK = 0,           // 签名段存在且验签通过
    META_SIG_ABSENT,           // 无签名段(未签名固件,合法,显示警告)
    META_SIG_BAD_MAGIC,        // 有数据但 magic 不是 MSIG(视为未签名)
    META_SIG_BAD_FORMAT,       // magic 对但 payload_len 不对
    META_SIG_BAD_CHECKSUM,     // xor 校验失败
    META_SIG_VERIFY_FAIL,     // 验签失败
} meta_sig_result_t;

// 验签函数声明(实现见 meta_sign.c)。
// digest 必须是 image 前 image_len 字节的 SHA-256(调用方流式计算,避免整包入 RAM)。
// sig_sector 为签名 sector 的前 META_SIG_TOTAL_LEN 字节。
// 返回 meta_sig_result_t。
meta_sig_result_t meta_sign_verify(const uint8_t digest[32], uint32_t image_len,
                                    const uint8_t *sig_sector, size_t sig_len);
