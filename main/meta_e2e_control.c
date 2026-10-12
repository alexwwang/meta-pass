#include "sdkconfig.h"

#if CONFIG_META_E2E_TEST_CONTROL

#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "meta_carve_flash.h"
#include "meta_store.h"
#include "meta_slots.h"
#include "meta_store_net.h"
#include "meta_store_install.h"

static const char *TAG = "meta_e2e";
static meta_slot_info_t *s_slots;

static const char *slot_state_name(meta_slot_state_t state)
{
    switch (state) {
    case META_SLOT_EMPTY: return "empty";
    case META_SLOT_VALID: return "valid";
    case META_SLOT_INVALID: return "invalid";
    default: return "unknown";
    }
}

static void response_error(const char *code)
{
    printf("{\"ok\":false,\"error\":\"%s\"}\n", code);
    fflush(stdout);
}

static void print_safe_name(const char *name)
{
    for (size_t i = 0; name && name[i] && i < 40; i++) {
        const unsigned char ch = (unsigned char)name[i];
        putchar((isalnum(ch) || ch == '-' || ch == '_' || ch == '.') ? ch : '_');
    }
}

static void response_slots(void)
{
    const meta_carve_t *carve = meta_carve_flash_carve();
    printf("{\"ok\":true,\"command\":\"SLOTS\",\"protocol\":1,\"slots\":[");
    bool first = true;
    if (carve) {
        for (uint8_t i = 0; i < carve->count; i++) {
            const meta_carve_slot_t *slot = &carve->slot[i];
            if (!first) printf(",");
            first = false;
            printf("{\"slot\":%u,\"playId\":%" PRIu32
                   ",\"offset\":%" PRIu32 ",\"size\":%" PRIu32
                   ",\"state\":\"%s\",\"name\":\"",
                   (unsigned)i, slot->play_id, slot->offset, slot->size,
                   slot_state_name(s_slots[i].state));
            print_safe_name(s_slots[i].name);
            printf("\"}");
        }
    }
    printf("],\"data\":[");
    first = true;
    if (carve) {
        for (uint8_t i = 0; i < carve->data_count; i++) {
            const meta_carve_data_t *data = &carve->data[i];
            if (!first) printf(",");
            first = false;
            printf("{\"playId\":%" PRIu32 ",\"label\":\"",
                   data->play_id);
            print_safe_name(data->label);
            printf("\",\"offset\":%" PRIu32 ",\"size\":%" PRIu32
                   ",\"state\":%u}",
                   data->offset, data->size, (unsigned)data->state);
        }
    }
    printf("]}\n");
    fflush(stdout);
}

static void handle_command(char *line)
{
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
        *--end = '\0';
    }
    if (strcmp(line, "E2E HELLO") == 0) {
        printf("{\"ok\":true,\"command\":\"HELLO\",\"protocol\":1,"
               "\"build\":\"test-control\"}\n");
        fflush(stdout);
        return;
    }
    if (strcmp(line, "E2E SLOTS") == 0) {
        response_slots();
        return;
    }
    uint32_t play_id = 0;
    char extra = '\0';
    if (sscanf(line, "E2E BOOT_PLAY %" SCNu32 " %c", &play_id, &extra) == 1) {
        const meta_carve_t *carve = meta_carve_flash_carve();
        int found = -1;
        if (carve) {
            for (uint8_t i = 0; i < carve->count; i++) {
                if (carve->slot[i].play_id == play_id) {
                    found = (int)i;
                    break;
                }
            }
        }
        if (found < 0) {
            response_error("SLOT_NOT_BOOTABLE");
            return;
        }
        (void)snprintf(line, 96, "E2E BOOT_SLOT %d", found);
        handle_command(line);
        return;
    }
    int slot = -1;
    if (sscanf(line, "E2E BOOT_SLOT %d %c", &slot, &extra) == 1) {
        const meta_carve_t *carve = meta_carve_flash_carve();
        if (slot < 0 || slot >= META_SLOT_COUNT || !carve ||
            slot >= (int)carve->count || !meta_slot_bootable(&s_slots[slot]) ||
            carve->slot[slot].kind != META_CARVE_KIND_APP ||
            !meta_store_slot_partition(slot)) {
            response_error("SLOT_NOT_BOOTABLE");
            return;
        }
        const esp_err_t err = meta_store_boot_slot(slot);
        if (err != ESP_OK) {
            response_error("BOOT_SLOT_FAILED");
            return;
        }
        printf("{\"ok\":true,\"command\":\"BOOT_SLOT\",\"slot\":%d,"
               "\"playId\":%" PRIu32 "}\n", slot, carve->slot[slot].play_id);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        return;
    }
    if (strcmp(line, "E2E SESSION_RESET") == 0) {
        // 测试通道专用:远程兑换入口。token_stop 作废并擦除持久化 token,
        // token_start 重新生成并打 UART("pair ready: code=... token=..."),
        // 免读屏完成自动化取 token —— 与 token_generate 的 UART 日志设计意图一致。
        // 仅限 CONFIG_META_E2E_TEST_CONTROL 构建;生产无此通道。
        meta_install_token_stop();
        if (meta_install_token_start() != ESP_OK) {
            response_error("SESSION_RESET_FAILED");
            return;
        }
        // INSTALL_STOP 之后 httpd 已停:测试通道的"换发会话"语义包含
        // 把 LAN install 服务拉回在线(幂等,已在线时立即返回)。
        // store_net 同理:生产里服务只活在商店页(net 已上线),e2e 通道
        // 绕过了页面流转,必须自己把 STA 拉起来,否则 LAN 校验全断。
        if (meta_store_net_begin() != ESP_OK) {
            response_error("SESSION_RESET_FAILED");
            return;
        }
        if (meta_install_net_start() != ESP_OK) {
            response_error("SESSION_RESET_FAILED");
            return;
        }
        printf("{\"ok\":true,\"command\":\"SESSION_RESET\"}\n");
        fflush(stdout);
        return;
    }
    if (strcmp(line, "E2E INSTALL_STOP") == 0) {
        // 测试通道专用:走产品 teardown 同款 meta_install_net_stop()
        // (httpd 排干停服 + token 作废 + 清 inst_active 续连标志),
        // 把设备恢复"列表页安装服务未激活"的冷态 —— USB Web Serial E2E 的
        // E1 安全门要求 installer idle;真机无按键自动化只能走这里。
        meta_install_net_stop();
        printf("{\"ok\":true,\"command\":\"INSTALL_STOP\"}\n");
        fflush(stdout);
        return;
    }
    response_error("UNKNOWN_COMMAND");
}

static void control_task(void *arg)
{
    (void)arg;
    char line[96];
    ESP_LOGW(TAG, "test-only USB control enabled; do not ship this build");
    printf("{\"event\":\"E2E_READY\",\"protocol\":1}\n");
    fflush(stdout);
    for (;;) {
        // 无人值守 E2E 策略:待确认 offer 出现即代按 OK(等价人工在 P2 页按
        // 确认键;仅限本测试构建)。手机页 install 流程否则永远等物理确认。
        // 每 200ms 巡检一次,offer 生命周期秒级,不会错过。
        meta_install_session_status_t st;
        meta_install_session_poll(&st);
        if (st.offer_ready && !st.confirmed && st.slot >= 0) {
            const esp_err_t e = meta_install_confirm_slot(st.slot);
            ESP_LOGW(TAG, "auto-confirm offer slot=%d: %s", (int)st.slot,
                     e == ESP_OK ? "confirmed" : esp_err_to_name(e));
        }
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (strstr(line, "\n") == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { }
            response_error("LINE_TOO_LONG");
            continue;
        }
        handle_command(line);
    }
}

void meta_e2e_control_start(meta_slot_info_t slots[META_SLOT_COUNT])
{
    s_slots = slots;
    if (xTaskCreate(control_task, "meta_e2e_ctl", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create test control task");
    }
}

#endif /* CONFIG_META_E2E_TEST_CONTROL */
