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
            printf("{\"playId\":%" PRIu32 ",\"label\":\"%.16s\","
                   "\"offset\":%" PRIu32 ",\"size\":%" PRIu32
                   ",\"state\":%u}",
                   data->play_id, data->label, data->offset, data->size,
                   (unsigned)data->state);
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
    int slot = -1;
    char extra = '\0';
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
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(20));
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
