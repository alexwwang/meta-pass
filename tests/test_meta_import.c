// tests/test_meta_import.c —— meta_import 导入状态机与配对码的 host 测试。
#include <assert.h>
#include <string.h>
#include "meta_import.h"

int main(void)
{
    mi_ctx_t c;

    // ---- 状态机主路径:IDLE→AP_UP→PAIRED→RECEIVING→VERIFYING→DONE ----
    mi_init(&c);
    assert(c.state == MI_IDLE);
    assert(mi_handle(&c, MI_EV_AP_READY) == MI_AP_UP);
    assert(mi_handle(&c, MI_EV_PAIR_OK) == MI_PAIRED);
    assert(mi_handle(&c, MI_EV_UPLOAD_BEGIN) == MI_RECEIVING);
    assert(mi_handle(&c, MI_EV_UPLOAD_DONE) == MI_VERIFYING);
    assert(mi_handle(&c, MI_EV_VERIFY_OK) == MI_DONE);
    assert(mi_handle(&c, MI_EV_RESET) == MI_IDLE);

    // ---- 非法事件不改变状态 ----
    mi_init(&c);
    assert(mi_handle(&c, MI_EV_UPLOAD_BEGIN) == MI_IDLE);   // 未配对不能上传
    assert(mi_handle(&c, MI_EV_VERIFY_OK) == MI_IDLE);
    mi_handle(&c, MI_EV_AP_READY);
    assert(mi_handle(&c, MI_EV_UPLOAD_BEGIN) == MI_AP_UP);  // 未配对仍不能上传
    assert(mi_handle(&c, MI_EV_PAIR_OK) == MI_PAIRED);
    assert(mi_handle(&c, MI_EV_PAIR_OK) == MI_PAIRED);      // 重复配对幂等

    // ---- 配对失败计数:3 次后锁死为 ERROR(AUTH) ----
    mi_init(&c);
    mi_handle(&c, MI_EV_AP_READY);
    mi_handle(&c, MI_EV_PAIR_FAIL);
    mi_handle(&c, MI_EV_PAIR_FAIL);
    assert(c.state == MI_AP_UP);
    assert(mi_handle(&c, MI_EV_PAIR_FAIL) == MI_ERROR);
    assert(c.last_error == MI_ERR_AUTH);
    assert(mi_handle(&c, MI_EV_RESET) == MI_IDLE);          // ERROR 只能由 RESET 恢复

    // ---- 接收中断:RECEIVING 中 ABORT → ERROR(BROKEN) ----
    mi_init(&c);
    mi_handle(&c, MI_EV_AP_READY);
    mi_handle(&c, MI_EV_PAIR_OK);
    mi_handle(&c, MI_EV_UPLOAD_BEGIN);
    assert(mi_handle(&c, MI_EV_ABORT) == MI_ERROR);
    assert(c.last_error == MI_ERR_BROKEN);

    // ---- 校验失败:VERIFYING 中 VERIFY_FAIL → ERROR(BAD_IMAGE) ----
    mi_init(&c);
    mi_handle(&c, MI_EV_AP_READY);
    mi_handle(&c, MI_EV_PAIR_OK);
    mi_handle(&c, MI_EV_UPLOAD_BEGIN);
    mi_handle(&c, MI_EV_UPLOAD_DONE);
    assert(mi_handle(&c, MI_EV_VERIFY_FAIL) == MI_ERROR);
    assert(c.last_error == MI_ERR_BAD_IMAGE);

    // ---- 配对码:6 位数字、确定性、比较函数严格 ----
    char code[MI_CODE_LEN + 1];
    assert(mi_code_gen(0, code));
    assert(strlen(code) == MI_CODE_LEN);
    for (int i = 0; i < MI_CODE_LEN; i++) assert(code[i] >= '0' && code[i] <= '9');
    char code2[MI_CODE_LEN + 1];
    mi_code_gen(123456789u, code2);
    assert(strlen(code2) == MI_CODE_LEN);
    assert(mi_code_equal(code, code));
    assert(!mi_code_equal(code, code2));
    assert(!mi_code_equal(code, "12345"));      // 长度不同
    assert(!mi_code_equal(code, "1234567"));    // 长度不同

    // ---- Content-Length 策略 ----
    assert(!mi_content_length_ok(0, 0x200000));
    assert(!mi_content_length_ok(-1, 0x200000));
    assert(mi_content_length_ok(1, 0x200000));
    assert(mi_content_length_ok(0x200000, 0x200000));
    assert(!mi_content_length_ok(0x200001, 0x200000));
    return 0;
}
