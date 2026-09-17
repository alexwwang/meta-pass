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
//   build/host-verify/test_integration --selftest   # 解析器 24B/16B-ext 双布局自检
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
#define PART_TABLE_OFF 0x8000u
#define PART_ENTRY_LEN 32u

// Full 合并镜像识别:0x8000 处分区表 magic(AA 50) —— 与
// tools/signing/locate_app_image.py / install-slot/extract-app-image.js 同一契约。
static int is_full_image(const unsigned char *buf, size_t fsize)
{
    return fsize > PART_TABLE_OFF + 1 &&
           buf[PART_TABLE_OFF] == 0xAA && buf[PART_TABLE_OFF + 1] == 0x50;
}

// 分区表逐条扫描(32B/条):type@+2、subtype@+3、offset U32LE@+4。
// type=0 且 subtype=0 即 factory 应用;未找到返回 0。
static uint32_t find_factory_partition(const unsigned char *buf, size_t fsize)
{
    uint32_t off = PART_TABLE_OFF;
    while (off + PART_ENTRY_LEN <= fsize) {
        if (buf[off] != 0xAA || buf[off + 1] != 0x50) break;
        if (buf[off + 2] == 0x00 && buf[off + 3] == 0x00) {
            return (uint32_t)buf[off + 4] |
                   ((uint32_t)buf[off + 5] << 8) |
                   ((uint32_t)buf[off + 6] << 16) |
                   ((uint32_t)buf[off + 7] << 24);
        }
        off += PART_ENTRY_LEN;
    }
    return 0;
}

static void hex(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

// 镜像长度,与设备侧 esp_image_verify() 语义一致。
// 与 tools/signing/sign-firmware.sh 的 compute_esp_image_len() 及 install-slot/extract-app-image.js
// 保持同一契约:依次尝试 24B 头 + 16B 扩展头与纯 24B 头两种布局(互斥,恰有一种能走通
// segment 表且长度收敛),决策见 docs/development/engineering/debugging-workflow.md §4。
static uint32_t try_layout(const unsigned char *img, size_t fsize, uint32_t ext_hdr_len)
{
    const unsigned char seg_count = img[1];
    uint32_t off = ESP_HDR_LEN + ext_hdr_len;
    for (uint32_t i = 0; i < seg_count; i++) {
        if (off + ESP_SEG_HDR_LEN > fsize) return 0;
        const uint32_t seg_len = (uint32_t)img[off + 4] |
                                 ((uint32_t)img[off + 5] << 8) |
                                 ((uint32_t)img[off + 6] << 16) |
                                 ((uint32_t)img[off + 7] << 24);
        off += ESP_SEG_HDR_LEN + seg_len;
        if (off > fsize) return 0;
    }
    // checksum: 当前偏移处 1 字节,随后整体填充到 16 字节边界
    const uint32_t unpadded = off;
    off = unpadded + ((unpadded + 1 + 15) & ~15u) - unpadded;
    // esp_image_header_t 末字节(偏移 23)的 hash_appended 标志
    if (img[23] & 1) off += 32;
    return off;
}

static uint32_t esp_image_len(const unsigned char *img, size_t fsize)
{
    if (fsize == 0 || img[0] != ESP_IMAGE_MAGIC) return (uint32_t)fsize;
    // 先按带 16B 扩展头解析(与 JS 安装页探测顺序一致),失败回退纯 24B 头布局
    for (int i = 0; i < 2; i++) {
        const uint32_t ext_hdr_len = (i == 0) ? 16u : 0u;
        const uint32_t result = try_layout(img, fsize, ext_hdr_len);
        if (result != 0) return result;
    }
    fprintf(stderr, "error: image segment table does not resolve under any known layout (16B-ext or plain)\n");
    return 0;
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
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        // 解析器自检:官方布局(24B 头,segment0@24)与 16B 扩展头布局都必须精确收敛。
        // fixture 与 tools/install-slot/test-extract.mjs 的 buildLayout() 同源:
        //   plain 24+8+100+8+64=204 → pad 到 %16==15(207)→ +1 checksum → +32 hash = 240
        //   ext   24+16+8+100+8+64=220 → 223 → +1 → +32 = 256
        // 0xab 填充保证未写区域读作 data_len=0xabababab(巨大),错误布局必然越界回退。
        static const uint32_t WANT[2] = {240u, 256u};   // [plain, ext]
        for (int v = 0; v < 2; v++) {
            unsigned char img[256];
            memset(img, 0xab, sizeof(img));
            const uint32_t seg0_off = v ? 40u : 24u;
            img[0] = ESP_IMAGE_MAGIC;
            img[1] = 2;                                 // 2 segments
            img[12] = 5;                                // chip_id = ESP32-C3 (LE)
            img[23] = 1;                                // hash_appended
            img[seg0_off + 0] = 0x00; img[seg0_off + 1] = 0x00;
            img[seg0_off + 2] = 0xc8; img[seg0_off + 3] = 0x3f; // load_addr 0x3fc80000
            img[seg0_off + 4] = 100; img[seg0_off + 5] = 0;     // data_len = 100 (u32 LE)
            img[seg0_off + 6] = 0;   img[seg0_off + 7] = 0;
            const uint32_t seg1_off = seg0_off + 8u + 100u;
            img[seg1_off + 0] = 0x20; img[seg1_off + 1] = 0x00;
            img[seg1_off + 2] = 0x00; img[seg1_off + 3] = 0x42; // load_addr 0x42000020
            img[seg1_off + 4] = 64;  img[seg1_off + 5] = 0;     // data_len = 64 (u32 LE)
            img[seg1_off + 6] = 0;   img[seg1_off + 7] = 0;
            const uint32_t got = esp_image_len(img, sizeof(img));
            if (got != WANT[v]) {
                fprintf(stderr, "selftest FAIL: layout %s resolved %u, want %u\n",
                        v ? "16B-ext" : "plain-24B", got, WANT[v]);
                return 1;
            }
        }
        printf("selftest: plain-24B -> %u, 16B-ext -> %u; probe order [16, 0] picks the converging layout\n",
               WANT[0], WANT[1]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <signed.bin>|--selftest\n", argv[0]);
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

    // image_len 与 tail sector 位置按输入格式解析:
    //   裸 app 镜像  app_off = 0,digest 从文件头起;
    //   Full 合并镜像  app_off = factory 分区偏移(典型 0x10000),
    //                    digest 仅覆盖 app 区域(bootloader/分区表不入 digest),
    //                    tail sector 在 app_off + align4k(image_len)。
    // 算法与设备侧 esp_image_verify()、sign-firmware.sh、install-slot/extract-app-image.js
    // 四方同一契约。
    uint32_t app_off = 0;
    const char *img_mode = "app";
    if (is_full_image(buf, (size_t)fsize)) {
        img_mode = "full";
        app_off = find_factory_partition(buf, (size_t)fsize);
        if (app_off == 0) {
            fprintf(stderr, "error: full flash image detected, but no factory app partition found\n");
            free(buf);
            return 2;
        }
    }
    const uint32_t image_len = esp_image_len(buf + app_off, (size_t)fsize - app_off);
    const uint32_t tail_off = (image_len + META_SIG_SECTOR - 1u) &
                              ~(META_SIG_SECTOR - 1u);
    if (fsize == 0 || image_len == 0 ||
        app_off + tail_off + META_SIG_SECTOR > (uint32_t)fsize) {
        fprintf(stderr, "error: image_len=%u tail_offset=%u (app at 0x%x) exceeds file size %ld\n",
                image_len, tail_off, app_off, fsize);
        free(buf);
        return 2;
    }

    printf("file:        %s (%ld bytes)\n", argv[1], fsize);
    printf("input:       %s image (app at file offset 0x%x)\n", img_mode, app_off);
    printf("image_len:   %u\n", image_len);
    printf("tail_offset: 0x%x in file (slot-relative %u)\n", app_off + tail_off, tail_off);
    const int rc = run(buf + app_off, (size_t)fsize, image_len, tail_off);
    free(buf);
    return rc;
}
