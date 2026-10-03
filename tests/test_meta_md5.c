// tests/test_meta_md5.c — meta_md5 黄金向量(host,先红后绿)。
//
// 证据来源:
//   - RFC 1321 标准测试向量(A.5);
//   - play 563 真机分区表条目(288B,来自托管合并镜像
//     SHA-256 2956f77b…3fbdf01 的 0x8000 表前缀,2026-10-02 抓取,
//     tests/fixtures/play563_table.bin),其 MD5 marker
//     = d98a2bec71be5d7d03b53d17d4b98798 与 md5(entries) 逐字节一致。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta_md5.h"

static void expect(const char *msg, const char *input, size_t len, const char *want_hex)
{
    uint8_t digest[16];
    char hex[33];
    meta_md5(input, len, digest);
    for (int i = 0; i < 16; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[32] = '\0';
    if (strcmp(hex, want_hex) != 0) {
        fprintf(stderr, "FAIL %s: got %s want %s\n", msg, hex, want_hex);
        assert(0);
    }
}

static uint8_t *load(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s\n", path);
        assert(f);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(void)
{
    // RFC 1321 A.5 向量。
    expect("empty", "", 0, "d41d8cd98f00b204e9800998ecf8427e");
    expect("a", "a", 1, "0cc175b9c0f1b6a831c399e269772661");
    expect("abc", "abc", 3, "900150983cd24fb0d6963f7d28e17f72");
    expect("message digest", "message digest", 14, "f96b697d7cb7938d525a2f31aaf161d0");
    expect("alphabet", "abcdefghijklmnopqrstuvwxyz", 26,
           "c3fcd3d76192e4007dfb496cca67e13b");

    // play 563 真机分区表条目(前9条×32B=288B)的 MD5 = 表内 marker 值。
    size_t n = 0;
    uint8_t *tbl = load("tests/fixtures/play563_table.bin", &n);
    assert(n == 0xC00);
    expect("play563 entries", (const char *)tbl, 288, "d98a2bec71be5d7d03b53d17d4b98798");
    free(tbl);

    printf("PASS test_meta_md5\n");
    return 0;
}
