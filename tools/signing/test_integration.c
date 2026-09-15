// tools/signing/test_integration.c —— 固件验签集成测试
//
// 直接链接固件的 main/meta_sign.c,调用固件自己的 meta_sign_verify() /
// meta_sign_detect_sector() / meta_egg_parse(),而不是在测试里复现 mbedtls 逻辑。
// 这样测的是"设备上真实运行的那 100 行代码",测试与固件不可能漂移。
//
// image_len 与 tail_offset 自动从镜像解析(镜像格式即参数),无需手写:
//   image_len  = esp_image_header_t(24B) + segments + checksum pad + appended hash
//   tail_offset = image_len 向上对齐到 4K
// 该算法与设备侧 esp_image_verify() 及 sign-firmware.sh 的实现一致。
//
// 编译(从仓库根目录):
//   mkdir -p build/host-verify && \
//   cc -O2 -I main -I$(brew --prefix mbedtls)/include \
//      tools/signing/test_integration.c main/meta_sign.c \
//      -L$(brew --prefix mbedtls)/lib -lmbedcrypto -lmbedx509 \
//      -o build/host-verify/test_integration
// 运行:
//   build/host-verify/test_integration build/<signed>.bin
//
// 退出码: 0 = 验签通过; 1 = 验签失败; 2 = 用法/解析错误
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "meta_sign.h"
#include "mbedtls/sha256.h"

#define ESP_IMAGE_MAGIC 0xE9u
#define ESP_HDR_LEN 24u
#define ESP_SEG_HDR_LEN 8u

static void hex(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

// 镜像长度,与设备侧 esp_image_verify() 语义一致。
// 与 tools/signing/sign-firmware.sh 内 compute_esp_image_len() 保持逐字对齐。
static uint32_t esp_image_len(const unsigned char *img, size_t fsize)
{
    if (fsize == 0 || img[0] != ESP_IMAGE_MAGIC) return (uint32_t)fsize;

    const unsigned char seg_count = img[1];
    uint32_t off = ESP_HDR_LEN;
    for (uint32_t i = 0; i < seg_count; i++) {
        if (off + ESP_SEG_HDR_LEN > fsize) {
            fprintf(stderr, "error: truncated image header at segment %u\n", i);
            return 0;
        }
        const uint32_t seg_len = (uint32_t)img[off + 4] |
                                 ((uint32_t)img[off + 5] << 8) |
                                 ((uint32_t)img[off + 6] << 16) |
                                 ((uint32_t)img[off + 7] << 24);
        off += ESP_SEG_HDR_LEN + seg_len;
        if (off > fsize) {
            fprintf(stderr, "error: segment %u exceeds file size\n", i);
            return 0;
        }
    }
    // checksum: 当前偏移处 1 字节,随后整体填充到 16 字节边界
    const uint32_t unpadded = off;
    const uint32_t length = (unpadded + 1 + 15) & ~15u;
    off = unpadded + (length - unpadded);
    // esp_image_header_t 末字节(偏移 23)的 hash_appended 标志
    if (img[23] & 1) off += 32;
    return off;
}

static int run(const unsigned char *buf, size_t fsize, uint32_t image_len,
               uint32_t tail_off)
{
    unsigned char digest[32];
    mbedtls_sha256(buf, image_len, digest, 0);
    printf("digest: ");
    hex(digest, 32);

    printf("\n=== meta_sign_verify() ===\n");
    const meta_sig_result_t result =
        meta_sign_verify(digest, image_len, buf + tail_off, META_SIG_SECTOR);
    switch (result) {
    case META_SIG_OK:          printf("  result: META_SIG_OK\n"); break;
    case META_SIG_ABSENT:      printf("  result: META_SIG_ABSENT\n"); break;
    case META_SIG_BAD_MAGIC:   printf("  result: META_SIG_BAD_MAGIC\n"); break;
    case META_SIG_BAD_FORMAT:  printf("  result: META_SIG_BAD_FORMAT\n"); break;
    case META_SIG_BAD_CHECKSUM: printf("  result: META_SIG_BAD_CHECKSUM\n"); break;
    case META_SIG_VERIFY_FAIL: printf("  result: META_SIG_VERIFY_FAIL\n"); break;
    default:                   printf("  result: UNKNOWN(%d)\n", (int)result); break;
    }

    printf("\n=== meta_sign_detect_sector() ===\n");
    printf("  detected: %s\n",
           meta_sign_detect_sector(buf + tail_off, META_SIG_SECTOR) ? "true" : "false");

    printf("\n=== meta_egg_parse() ===\n");
    char egg[4096];
    const meta_egg_result_t egg_result =
        meta_egg_parse(buf + tail_off, META_SIG_SECTOR, egg, sizeof(egg));
    switch (egg_result) {
    case META_EGG_OK:           printf("  result: META_EGG_OK\n"); break;
    case META_EGG_ABSENT:       printf("  result: META_EGG_ABSENT\n"); break;
    case META_EGG_BAD_FORMAT:   printf("  result: META_EGG_BAD_FORMAT\n"); break;
    case META_EGG_BAD_CHECKSUM: printf("  result: META_EGG_BAD_CHECKSUM\n"); break;
    default:                    printf("  result: UNKNOWN(%d)\n", (int)egg_result); break;
    }
    if (egg_result == META_EGG_OK) printf("  text: \"%s\"\n", egg);

    const int pass = (result == META_SIG_OK &&
                      meta_sign_detect_sector(buf + tail_off, META_SIG_SECTOR));
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <signed.bin>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    long fsize = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "error: cannot size %s\n", argv[1]);
        fclose(f);
        return 2;
    }
    unsigned char *buf = malloc((size_t)fsize);
    if (!buf) { fprintf(stderr, "error: malloc(%ld)\n", fsize); fclose(f); return 2; }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "error: short read on %s\n", argv[1]);
        free(buf);
        fclose(f);
        return 2;
    }
    fclose(f);

    const uint32_t image_len = esp_image_len(buf, (size_t)fsize);
    const uint32_t tail_off = (image_len + META_SIG_SECTOR - 1u) &
                              ~(META_SIG_SECTOR - 1u);
    if (fsize == 0 || image_len == 0 || tail_off + META_SIG_SECTOR > (uint32_t)fsize) {
        fprintf(stderr, "error: image_len=%u tail_offset=%u exceeds file size %ld\n",
                image_len, tail_off, fsize);
        free(buf);
        return 2;
    }

    printf("file:        %s (%ld bytes)\n", argv[1], fsize);
    printf("image_len:   %u\n", image_len);
    printf("tail_offset: %u\n", tail_off);
    const int rc = run(buf, (size_t)fsize, image_len, tail_off);
    free(buf);
    return rc;
}
