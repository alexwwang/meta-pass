// tests/test_meta_seq.c —— 隐藏按键序列匹配器 meta_seq 的 host 测试。
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "meta_seq.h"

static const meta_seq_key_t SEQ[META_SEQ_LEN] = {
    META_SEQ_KEY_UP, META_SEQ_KEY_UP,
    META_SEQ_KEY_DOWN, META_SEQ_KEY_DOWN,
};

int main(void)
{
    meta_seq_state_t st;

    // 1. 快速完整序列 → 命中
    meta_seq_reset(&st);
    for (uint32_t i = 0; i < META_SEQ_LEN; i++) {
        bool hit = meta_seq_feed(&st, SEQ[i], 1000 + i * 100);
        assert(hit == (i == META_SEQ_LEN - 1));
    }

    // 2. 命中后状态复位 → 可立即再来一轮
    for (uint32_t i = 0; i < META_SEQ_LEN; i++) {
        bool hit = meta_seq_feed(&st, SEQ[i], 2000 + i * 100);
        assert(hit == (i == META_SEQ_LEN - 1));
    }

    // 3. 间隔 499ms(< 上限)→ 仍连续,命中
    meta_seq_reset(&st);
    for (uint32_t i = 0; i < META_SEQ_LEN; i++) {
        bool hit = meta_seq_feed(&st, SEQ[i], 3000 + i * 499);
        assert(hit == (i == META_SEQ_LEN - 1));
    }

    // 4. 间隔恰好 500ms(= 上限)→ 进度作废,不命中
    meta_seq_reset(&st);
    for (uint32_t i = 0; i < META_SEQ_LEN; i++) {
        bool hit = meta_seq_feed(&st, SEQ[i], 5000 + i * 500);
        assert(!hit);
    }
    // 序列作废后从当前键重新起匹配:最后补一段完整快序列仍应命中
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 9000) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 9100) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 9200) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 9300) == true);

    // 5. 中间按错键 → 序列中断;错键 UP 恰是新序列首键,从它重新起匹配后仍可命中
    //    (UP UP 后期望 DOWN,此时按 UP 是错键;重叠前缀语义:UP UP UP DOWN DOWN 命中)
    meta_seq_reset(&st);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 10000) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 10100) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 10200) == false);   // 期望 DOWN,打乱;UP=首键 → idx1
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 10300) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 10400) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 10500) == true);

    // 6. 重叠前缀: "U U U U D D" 的最后 4 键是合法序列 → 命中
    meta_seq_reset(&st);
    const meta_seq_key_t overlap[] = {
        META_SEQ_KEY_UP, META_SEQ_KEY_UP, META_SEQ_KEY_UP, META_SEQ_KEY_UP,
        META_SEQ_KEY_DOWN, META_SEQ_KEY_DOWN,
    };
    for (uint32_t i = 0; i < sizeof(overlap) / sizeof(overlap[0]); i++) {
        bool hit = meta_seq_feed(&st, overlap[i], 11000 + i * 100);
        assert(hit == (i == 5));
    }

    // 7. 首键无间隔检查:单个 UP 后隔很久再接完整序列,互不影响
    meta_seq_reset(&st);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 0) == false);
    for (uint32_t i = 0; i < META_SEQ_LEN; i++) {
        bool hit = meta_seq_feed(&st, SEQ[i], 60000 + i * 100);
        assert(hit == (i == META_SEQ_LEN - 1));
    }

    // 8. NULL 安全
    meta_seq_reset(NULL);
    assert(meta_seq_feed(NULL, META_SEQ_KEY_UP, 0) == false);

    // 9. 时间回绕:now_ms 接近 uint32 上限时无符号差仍正确
    meta_seq_reset(&st);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 0xFFFFFFF0u) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_UP, 0xFFFFFFF0u + 50u) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 0xFFFFFFF0u + 100u) == false);
    assert(meta_seq_feed(&st, META_SEQ_KEY_DOWN, 0xFFFFFFF0u + 150u) == true);

    return 0;
}
