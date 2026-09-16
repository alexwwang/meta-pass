// main/meta_seq.c —— 隐藏按键序列匹配器实现。
#include "meta_seq.h"

static const meta_seq_key_t k_seq[META_SEQ_LEN] = {
    META_SEQ_KEY_UP, META_SEQ_KEY_UP,
    META_SEQ_KEY_DOWN, META_SEQ_KEY_DOWN,
};

void meta_seq_reset(meta_seq_state_t *st)
{
    if (!st) return;
    st->index = 0;
    st->last_ms = 0;
}

bool meta_seq_feed(meta_seq_state_t *st, meta_seq_key_t key, uint32_t now_ms)
{
    if (!st) return false;

    // 间隔超限:进度作废,当前键按新序列首键重新判定。
    // 无符号减法天然容忍 now_ms 回绕。
    if (st->index > 0 && (uint32_t)(now_ms - st->last_ms) >= META_SEQ_GAP_MS) {
        st->index = 0;
    }

    if (key == k_seq[st->index]) {
        st->index++;
        st->last_ms = now_ms;
        if (st->index == META_SEQ_LEN) {
            st->index = 0;   // 命中即复位,允许紧接下一轮
            return true;
        }
        return false;
    }

    // 不匹配:用当前键重新起匹配(如 "UP UP UP DOWN DOWN LONG" 的后缀仍是合法序列)。
    st->index = 0;
    if (key == k_seq[0]) {
        st->index = 1;
        st->last_ms = now_ms;
    }
    return false;
}
