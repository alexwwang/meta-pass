// main/meta_sign.h —— meta-pass 应用层签名徽章(软件级,可逆)。
//
// 设计见 docs/assets/meta-pass-design.md §7。
// 尾部元数据 sector 附加在 ESP app image 的 image_len 之后,4K 对齐。
// 布局(单一 4KB metadata sector,三段位置全部固定):
//   [0..127]         MSIG 签名徽章预留区(变长 blob,实际按 payload_len 读取)
//   [128..4055]      MAEG 彩蛋窗口(定长 3928B:文本 ≤3919B,不足部分 0xFF padding)
//   [4056..4095]     MNAM 显示名窗口(40B,blob 在窗口内右对齐)
// MSIG blob 格式(变长,实际按 payload_len 读取):
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
#define META_SIG_RESERVE_LEN  128u          // 单一尾部 sector 内 MSIG 区固定预留
#define META_SIG_SECTOR       4096u         // 尾部元数据 sector 字节数
#define META_NAME_BLOB_SECTOR 4096u         // 向后兼容别名:实际同 META_SIG_SECTOR
#define META_NAME_BLOB_OFF    4056u         // 尾部 sector 内 MNAM 区起点(4056..4095，末尾 40B)
#define META_NAME_BLOB_LEN    40u           // MNAM 区固定预留(40B)

// 彩蛋元数据存放在尾部 sector 中部:不覆盖 MSIG/MNAM,也不进入签名 digest。
// MAEG 字段定长 3928B:magic(4) + payload_len(4) + text 区(3919B,0xFF padding) + xor(1)。
// xor 固定在窗口最后一字节,覆盖前 3927 字节(含 padding)——位置与文本长度无关。
#define META_EGG_MAGIC       0x4D414547u    // "MAEG" (字节序为 'M','A','E','G')
#define META_EGG_MAGIC_BYTES { 'M', 'A', 'E', 'G' }
#define META_EGG_HEADER_LEN  8u             // magic(4) + payload_len(4)
#define META_EGG_TEXT_LEN    3919u          // 彩蛋文本最大 ASCII 字节数
#define META_EGG_WINDOW_LEN  3928u          // 128..4055
#define META_EGG_TOTAL_LEN   (META_EGG_HEADER_LEN + META_EGG_TEXT_LEN + 1u)
#define META_EGG_XOR_OFF     (META_EGG_TOTAL_LEN - 1u)  // xor 固定位置: 窗口内偏移 3927
#define META_EGG_WINDOW_OFF  META_SIG_RESERVE_LEN

// 签名 sector 在槽位中的偏移:image_len 之后,4K 对齐。
static inline uint32_t meta_sign_sector_offset(uint32_t image_len)
{
    return (image_len + META_SIG_SECTOR - 1u) & ~(META_SIG_SECTOR - 1u);
}

// 应用镜像上限:分区大小减去单一尾部元数据 sector。
static inline uint32_t meta_sign_app_limit(uint32_t part_size)
{
    return part_size - META_SIG_SECTOR;
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

// 轻量格式探测:仅校验 MSIG magic + xor checksum，不验 pubkey。
// 用于判断上传镜像的尾部分区是否已包含合法签名(签名镜像不允许覆盖)。
bool meta_sign_detect_sector(const uint8_t *sig_sector, size_t sig_len);

// 彩蛋结果。
typedef enum {
    META_EGG_OK = 0,       // 彩蛋段存在且格式/校验有效
    META_EGG_ABSENT,       // 无彩蛋段
    META_EGG_BAD_FORMAT,   // magic/长度/字符集不合法
    META_EGG_BAD_CHECKSUM, // XOR 校验失败
} meta_egg_result_t;

// 从签名 sector 固定尾部窗口解析彩蛋文本。文本只接受可打印 ASCII。
// out 必须以 '\0' 结尾;buf 可为任意大小,函数会拒绝不足窗口偏移的输入。
meta_egg_result_t meta_egg_parse(const uint8_t *sig_sector, size_t sig_sector_len,
                                  char *out, size_t out_cap);
