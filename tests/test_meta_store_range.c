// tests/test_meta_store_range.c —— Content-Range 续传契约边界测试。
#include "meta_store_range.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    meta_store_range_t r;

    assert(meta_store_range_parse("bytes 0-999/1000", &r));
    assert(r.first == 0 && r.last == 999 && r.total == 1000);
    assert(meta_store_range_matches(&r, 0, 1000, 1000));

    assert(meta_store_range_parse("bytes 173387-2664255/2664256", &r));
    assert(meta_store_range_matches(&r, 173387, 2664256, 2490869));

    const char *bad[] = {
        "bytes 1000-999/1000",      // first > last
        "bytes 999-1000/1000",      // last == total
        "bytes 0-999/0",            // empty resource
        "bytes 0-999/*",            // device requires concrete total
        "bytes 0-/1000",            // open-ended response is not Content-Range
        "items 0-999/1000",         // wrong unit
        "bytes 0-999/1000 ",        // trailing junk
        "bytes 4294967296-0/1000",  // uint32 overflow
        NULL,
    };
    for (int i = 0; bad[i]; i++) {
        assert(!meta_store_range_parse(bad[i], &r));
    }

    assert(meta_store_range_parse("bytes 10-19/1000", &r));
    assert(!meta_store_range_matches(&r, 11, 1000, 10));   // 起点漂移
    assert(!meta_store_range_matches(&r, 10, 1001, 10));   // 上游换版
    assert(!meta_store_range_matches(&r, 10, 1000, 9));    // CL 与区间不符
    assert(!meta_store_range_matches(&r, 10, 1000, 0));    // 无 CL 不可写 flash

    puts("meta_store_range: all checks passed");
    return 0;
}
