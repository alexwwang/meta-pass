#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_crc.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#define TAG_LABEL "e2edata"
#define PROTOCOL_VERSION 1u
#define RECORD_MAGIC 0x44415441u /* DATA */
#define RECORD_VERSION 1u
#define RECORD_PAYLOAD_SIZE 192u
#define RX_LINE_SIZE 96u
#define IO_TIMEOUT_MS 1000

typedef struct {
    uint32_t magic;
    uint32_t version;
    char nonce[32];
    uint32_t sequence;
    uint8_t payload[RECORD_PAYLOAD_SIZE];
    uint8_t sha256[32];
    uint32_t crc32;
} data_record_t;

_Static_assert(sizeof(data_record_t) == 272, "DATA record must remain 272 bytes");

static const esp_partition_t *s_data;
static uint32_t s_sequence;

static void send_json(const char *json)
{
    char line[768];
    const int n = snprintf(line, sizeof(line), "%s\n", json);
    if (n > 0 && (size_t)n < sizeof(line)) {
        (void)usb_serial_jtag_write_bytes(line, n, pdMS_TO_TICKS(IO_TIMEOUT_MS));
    }
}

static void send_error(const char *code, const char *detail)
{
    char out[256];
    (void)snprintf(out, sizeof(out),
                   "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}",
                   code, detail);
    send_json(out);
}

static bool data_partition(void)
{
    s_data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40,
                                      TAG_LABEL);
    return s_data != NULL;
}

static void hash_record(data_record_t *record)
{
    (void)mbedtls_sha256((const unsigned char *)record,
                         offsetof(data_record_t, sha256),
                         record->sha256, 0);
    record->crc32 = esp_crc32_le(0, (const uint8_t *)record,
                                 offsetof(data_record_t, crc32));
}

static bool record_valid(const data_record_t *record)
{
    if (record->magic != RECORD_MAGIC || record->version != RECORD_VERSION) return false;
    data_record_t copy = *record;
    uint8_t expected[32];
    (void)mbedtls_sha256((const unsigned char *)&copy,
                         offsetof(data_record_t, sha256), expected, 0);
    if (memcmp(expected, record->sha256, sizeof(expected)) != 0) return false;
    return esp_crc32_le(0, (const uint8_t *)record,
                        offsetof(data_record_t, crc32)) == record->crc32;
}

static void command_hello(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char out[256];
    (void)snprintf(out, sizeof(out),
                   "{\"ok\":true,\"protocol\":%u,\"command\":\"HELLO\","
                   "\"project\":\"%.32s\",\"version\":\"%.32s\","
                   "\"chip\":\"esp32c3\",\"uptimeMs\":%" PRIu64 "}",
                   PROTOCOL_VERSION, app->project_name, app->version,
                   (uint64_t)(esp_timer_get_time() / 1000));
    send_json(out);
}

static void command_info(void)
{
    if (!data_partition()) {
        send_error("DATA_NOT_FOUND", TAG_LABEL);
        return;
    }
    char out[256];
    (void)snprintf(out, sizeof(out),
                   "{\"ok\":true,\"command\":\"INFO\",\"label\":\"%s\","
                   "\"address\":%" PRIu32 ",\"size\":%" PRIu32 ","
                   "\"subtype\":%u}",
                   s_data->label, s_data->address, s_data->size, s_data->subtype);
    send_json(out);
}

static void command_write(const char *nonce)
{
    if (strlen(nonce) != 32) {
        send_error("BAD_NONCE", "expected exactly 32 hexadecimal characters");
        return;
    }
    for (size_t i = 0; i < 32; i++) {
        const char c = nonce[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            send_error("BAD_NONCE", "nonce must be hexadecimal");
            return;
        }
    }
    if (!data_partition()) {
        send_error("DATA_NOT_FOUND", TAG_LABEL);
        return;
    }
    if (s_data->size < 4096u) {
        send_error("DATA_TOO_SMALL", "DATA partition is smaller than one erase sector");
        return;
    }

    data_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    memcpy(record.nonce, nonce, sizeof(record.nonce));
    /* Store a compact, deterministic nonce-derived payload, not user data. */
    for (size_t i = 0; i < sizeof(record.payload); i++) {
        record.payload[i] = (uint8_t)(nonce[i % 32] ^ (uint8_t)(i * 31u));
    }
    record.sequence = ++s_sequence;
    hash_record(&record);

    esp_err_t err = esp_partition_erase_range(s_data, 0, 4096u);
    if (err == ESP_OK) err = esp_partition_write(s_data, 0, &record, sizeof(record));
    if (err != ESP_OK) {
        send_error("DATA_WRITE_FAILED", esp_err_to_name(err));
        return;
    }

    data_record_t verify;
    err = esp_partition_read(s_data, 0, &verify, sizeof(verify));
    if (err != ESP_OK || memcmp(&record, &verify, sizeof(record)) != 0 ||
        !record_valid(&verify)) {
        send_error("DATA_READBACK_MISMATCH", "device-side readback or checksum failed");
        return;
    }

    char out[256];
    (void)snprintf(out, sizeof(out),
                   "{\"ok\":true,\"command\":\"WRITE\",\"label\":\"%s\","
                   "\"sequence\":%" PRIu32 ",\"nonce\":\"%.32s\","
                   "\"sha256\":\"%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x\","
                   "\"crc32\":\"%08" PRIx32 "\",\"readback\":true}",
                   s_data->label, verify.sequence, nonce,
                   verify.sha256[0], verify.sha256[1], verify.sha256[2], verify.sha256[3],
                   verify.sha256[4], verify.sha256[5], verify.sha256[6], verify.sha256[7],
                   verify.sha256[8], verify.sha256[9], verify.sha256[10], verify.sha256[11],
                   verify.sha256[12], verify.sha256[13], verify.sha256[14], verify.sha256[15],
                   verify.sha256[16], verify.sha256[17], verify.sha256[18], verify.sha256[19],
                   verify.sha256[20], verify.sha256[21], verify.sha256[22], verify.sha256[23],
                   verify.sha256[24], verify.sha256[25], verify.sha256[26], verify.sha256[27],
                   verify.sha256[28], verify.sha256[29], verify.sha256[30], verify.sha256[31],
                   verify.crc32);
    send_json(out);
}

static void command_read(void)
{
    if (!data_partition()) {
        send_error("DATA_NOT_FOUND", TAG_LABEL);
        return;
    }
    data_record_t record;
    const esp_err_t err = esp_partition_read(s_data, 0, &record, sizeof(record));
    if (err != ESP_OK) {
        send_error("DATA_READ_FAILED", esp_err_to_name(err));
        return;
    }
    if (!record_valid(&record)) {
        send_error("DATA_INVALID", "record missing or checksum mismatch");
        return;
    }
    char out[256];
    (void)snprintf(out, sizeof(out),
                   "{\"ok\":true,\"command\":\"READ\",\"label\":\"%s\","
                   "\"sequence\":%" PRIu32 ",\"nonce\":\"%.32s\","
                   "\"sha256\":\"%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x%02x%02x\","
                   "\"crc32\":\"%08" PRIx32 "\",\"readback\":true}",
                   s_data->label, record.sequence, record.nonce,
                   record.sha256[0], record.sha256[1], record.sha256[2], record.sha256[3],
                   record.sha256[4], record.sha256[5], record.sha256[6], record.sha256[7],
                   record.sha256[8], record.sha256[9], record.sha256[10], record.sha256[11],
                   record.sha256[12], record.sha256[13], record.sha256[14], record.sha256[15],
                   record.sha256[16], record.sha256[17], record.sha256[18], record.sha256[19],
                   record.sha256[20], record.sha256[21], record.sha256[22], record.sha256[23],
                   record.sha256[24], record.sha256[25], record.sha256[26], record.sha256[27],
                   record.sha256[28], record.sha256[29], record.sha256[30], record.sha256[31],
                   record.crc32);
    send_json(out);
}

static void command_erase(void)
{
    if (!data_partition()) {
        send_error("DATA_NOT_FOUND", TAG_LABEL);
        return;
    }
    const esp_err_t err = esp_partition_erase_range(s_data, 0, 4096u);
    if (err != ESP_OK) {
        send_error("DATA_ERASE_FAILED", esp_err_to_name(err));
        return;
    }
    send_json("{\"ok\":true,\"command\":\"ERASE\",\"erasedBytes\":4096}");
}

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    while (*line && (line[strlen(line) - 1] == '\r' || line[strlen(line) - 1] == '\n')) {
        line[strlen(line) - 1] = '\0';
    }
    if (strcmp(line, "HELLO") == 0) command_hello();
    else if (strcmp(line, "INFO") == 0) command_info();
    else if (strcmp(line, "READ") == 0) command_read();
    else if (strcmp(line, "ERASE") == 0) command_erase();
    else if (strncmp(line, "WRITE ", 6) == 0) command_write(line + 6);
    else if (strcmp(line, "REBOOT") == 0) {
        send_json("{\"ok\":true,\"command\":\"REBOOT\"}");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        send_error("UNKNOWN_COMMAND", "allowed: HELLO INFO WRITE READ ERASE REBOOT");
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 1024,
    };
    const esp_err_t install_err = usb_serial_jtag_driver_install(&cfg);
    if (install_err != ESP_OK) {
        /* USB is the only control channel in this test image. */
        return;
    }
    send_json("{\"ok\":true,\"event\":\"READY\",\"protocol\":1}");

    char line[RX_LINE_SIZE];
    size_t used = 0;
    for (;;) {
        uint8_t byte;
        const int got = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(IO_TIMEOUT_MS));
        if (got <= 0) continue;
        if (byte == '\n' || byte == '\r') {
            if (used == 0) continue;
            line[used] = '\0';
            handle_line(line);
            used = 0;
        } else if (used + 1 < sizeof(line)) {
            line[used++] = (char)byte;
        } else {
            used = 0;
            send_error("LINE_TOO_LONG", "command line exceeds protocol limit");
        }
    }
}
