// main/meta_store_api_fail.c —— 契约见头文件。纯逻辑,零依赖。
#include "meta_store_api_fail.h"

#include <stdio.h>

const char *meta_store_api_fail_text(int code, bool clock_unsynced,
                                     char *buf, size_t cap)
{
    const char *s;
    switch (code) {
    // ---- 传输阶段 × 时钟矩阵 ----
    case MSAF_STAGE_OPEN:
        // 时钟停在 1970 时 mbedTLS 证书时间校验必败(BADCERT_FUTURE)——
        // 真机最可能的死因,显式点名,不让用户去猜证书/网络。
        s = clock_unsynced ? "TLS failed (clock unsynced)."
                           : "TLS/DNS failed. Retry.";
        break;
    case MSAF_STAGE_HEADERS:
        // 连接已建立但拿不到响应头:读时钟无意义(死因在服务端响应),只按
        // 阶段给建议;时钟未同步时 TLS 握手同样会先死在这一步之前。
        s = clock_unsynced ? "TLS failed (clock unsynced)."
                           : "No response. Retry.";
        break;
    case MSAF_STAGE_READ:
        s = "Connection lost. Retry.";
        break;
    case MSAF_STAGE_PARSE:
        // 连接与读取都正常但响应不是契约形态:服务器实现漂移,重试无用。
        s = "Bad response from server.";
        break;
    // ---- 业务 reason 码:原样透传(契约名,主表在 analysis 模块) ----
    case MSAF_REASON_NOT_FOUND: s = "not-found";  break;
    case MSAF_REASON_FORMAT:    s = "format";     break;
    case MSAF_REASON_TOO_LARGE: s = "too-large";  break;
    default:
        if (code >= 0) {
            // 未登记的服务端 reason 序号:回显序号,不吞。
            if (buf && cap > 0) snprintf(buf, cap, "reason-%d", code);
            return buf ? buf : "";
        }
        if (code <= -100 && code > -1000) {
            // HTTP 状态码取负(非 200/404):带码号,方便对服务端日志。
            if (buf && cap > 0) snprintf(buf, cap, "Server error %d", -code);
            return buf ? buf : "";
        }
        s = "Connection lost. Retry.";
        break;
    }
    if (buf && cap > 0) {
        // 截断恒 NUL 结尾;cap 足够时与静态字面量逐字节一致(测试断言依据)。
        snprintf(buf, cap, "%s", s);
        return buf;
    }
    return s;
}
