// 一次性夹具:读文件喂给与真机相同的 analyze 解析器。
#include <stdio.h>
#include <string.h>
#include "meta_store_analysis.h"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static char buf[65536];
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    meta_store_analysis_t a;
    if (!meta_store_analysis_parse(buf, n, &a)) { printf("PARSE FAIL\n"); return 1; }
    printf("PARSE OK: supported=%d reason='%s' detail='%s' name='%s' slot=%d image_len=%u\n",
           (int)a.supported, a.reason, a.detail, a.name, (int)a.suggested_slot,
           (unsigned)a.image_len);
    return 0;
}
