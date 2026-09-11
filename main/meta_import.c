// main/meta_import.c —— 实现见头文件注释。
#include "meta_import.h"

#include <string.h>

void mi_init(mi_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->state = MI_IDLE;
    ctx->last_error = MI_ERR_NONE;
    ctx->pair_fails = 0;
}

static mi_state_t to_error(mi_ctx_t *ctx, mi_error_t err)
{
    ctx->state = MI_ERROR;
    ctx->last_error = err;
    return ctx->state;
}

mi_state_t mi_handle(mi_ctx_t *ctx, mi_event_t ev)
{
    if (!ctx) return MI_ERROR;

    // RESET 是全局事件:任意状态回到 IDLE 并清空错误与计数。
    if (ev == MI_EV_RESET) {
        mi_init(ctx);
        return ctx->state;
    }

    switch (ctx->state) {
    case MI_IDLE:
        if (ev == MI_EV_AP_READY) ctx->state = MI_AP_UP;
        break;
    case MI_AP_UP:
        if (ev == MI_EV_PAIR_OK) {
            ctx->state = MI_PAIRED;
            ctx->pair_fails = 0;
        } else if (ev == MI_EV_PAIR_FAIL) {
            if (++ctx->pair_fails >= MI_MAX_PAIR_FAILS) to_error(ctx, MI_ERR_AUTH);
        } else if (ev == MI_EV_ABORT) {
            to_error(ctx, MI_ERR_BROKEN);
        }
        break;
    case MI_PAIRED:
        if (ev == MI_EV_UPLOAD_BEGIN) ctx->state = MI_RECEIVING;
        else if (ev == MI_EV_ABORT) to_error(ctx, MI_ERR_BROKEN);
        break;
    case MI_RECEIVING:
        if (ev == MI_EV_UPLOAD_DONE) ctx->state = MI_VERIFYING;
        else if (ev == MI_EV_ABORT) to_error(ctx, MI_ERR_BROKEN);
        break;
    case MI_VERIFYING:
        if (ev == MI_EV_VERIFY_OK) ctx->state = MI_DONE;
        else if (ev == MI_EV_VERIFY_FAIL) to_error(ctx, MI_ERR_BAD_IMAGE);
        break;
    case MI_DONE:
    case MI_ERROR:
        // 终态:仅 RESET(上面已处理)可离开。
        break;
    }
    return ctx->state;
}

bool mi_code_gen(uint32_t random32, char out[MI_CODE_LEN + 1])
{
    if (!out) return false;
    uint32_t v = random32 % 1000000u;
    for (int i = MI_CODE_LEN - 1; i >= 0; i--) {
        out[i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    out[MI_CODE_LEN] = '\0';
    return true;
}

bool mi_code_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    if (strlen(a) != MI_CODE_LEN || strlen(b) != MI_CODE_LEN) return false;
    uint8_t diff = 0;
    for (int i = 0; i < MI_CODE_LEN; i++) {
        diff |= (uint8_t)((uint8_t)a[i] ^ (uint8_t)b[i]);
    }
    return diff == 0;
}

bool mi_content_length_ok(int64_t declared, uint32_t slot_size)
{
    return declared > 0 && (uint64_t)declared <= (uint64_t)slot_size;
}
