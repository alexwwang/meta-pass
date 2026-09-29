// tests/esp_stubs/meta_store_host_fixture.c — host fixture for the meta-store
// scan path (BUG-21 regression): an 8 MB in-memory flash model mirroring the
// device partition table (partitions.csv), the esp_partition_* primitives,
// and the two esp_ota_ops accessors meta_store.c consumes.
//
// Semantics that matter for the tests:
//   - erase_range resets bytes to 0xFF (erased state — the BUG-21 payload),
//   - partition lookup by (type, subtype) mirrors esp_partition_find_first,
//   - esp_ota_get_partition_description reads at
//     sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t) and
//     checks ESP_APP_DESC_MAGIC_WORD, exactly like IDF 5.5.3 esp_ota_ops.c:811.
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"

#include <stddef.h>
#include <string.h>

#define HOST_FLASH_SIZE 0x800000u // 8 MB, mirrors the device flash

static uint8_t host_flash[HOST_FLASH_SIZE];
static bool host_flash_ready;

esp_partition_t host_parts[HOST_PART_MAX];
int host_part_count;

void host_fixture_reset(void)
{
    memset(host_flash, 0xFF, sizeof(host_flash));
    memset(host_parts, 0, sizeof(host_parts));
    host_part_count = 0;
    host_flash_ready = true;
}

const esp_partition_t *host_fixture_install(uint32_t addr, uint32_t size,
                                            uint8_t type, uint8_t subtype,
                                            const char *label)
{
    if (!host_flash_ready || host_part_count >= HOST_PART_MAX) return NULL;
    esp_partition_t *p = &host_parts[host_part_count++];
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->subtype = subtype;
    p->address = addr;
    p->size = size;
    p->erase_size = 4096;
    if (label) {
        strncpy(p->label, label, sizeof(p->label) - 1);
    }
    return p;
}

uint8_t *host_fixture_flash(void)
{
    return host_flash;
}

const esp_partition_t *esp_partition_find_first(uint8_t type, uint8_t subtype,
                                                const char *label)
{
    for (int i = 0; i < host_part_count; i++) {
        const esp_partition_t *p = &host_parts[i];
        if (p->type != type || p->subtype != subtype) continue;
        if (label && p->label[0] && strcmp(p->label, label) != 0) continue;
        return p;
    }
    return NULL;
}

static esp_err_t part_span_ok(const esp_partition_t *p, size_t offset, size_t size)
{
    if (!p || !host_flash_ready) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)offset + size > p->size) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)p->address + offset + size > HOST_FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *dest, size_t size)
{
    esp_err_t err = part_span_ok(partition, offset, size);
    if (err != ESP_OK) return err;
    memcpy(dest, &host_flash[partition->address + offset], size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *src, size_t size)
{
    esp_err_t err = part_span_ok(partition, offset, size);
    if (err != ESP_OK) return err;
    memcpy(&host_flash[partition->address + offset], src, size);
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset,
                                    size_t size)
{
    esp_err_t err = part_span_ok(partition, offset, size);
    if (err != ESP_OK) return err;
    memset(&host_flash[partition->address + offset], 0xFF, size);
    return ESP_OK;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                            esp_app_desc_t *app_desc)
{
    if (!partition || !app_desc) return ESP_ERR_INVALID_ARG;
    if (partition->type != ESP_PARTITION_TYPE_APP) return ESP_ERR_NOT_SUPPORTED;
    // IDF reads past the image header + first segment header (64 B total).
    const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    esp_image_header_t hdr;
    esp_image_segment_header_t seg;
    if (esp_partition_read(partition, 0, &hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
    if (esp_partition_read(partition, sizeof(hdr), &seg, sizeof(seg)) != ESP_OK) return ESP_FAIL;
    if (off + sizeof(*app_desc) > partition->size) return ESP_ERR_NOT_FOUND;
    if (esp_partition_read(partition, off, app_desc, sizeof(*app_desc)) != ESP_OK) return ESP_FAIL;
    if (app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) return ESP_ERR_NOT_FOUND;
    (void)hdr;
    (void)seg;
    return ESP_OK;
}

static const esp_partition_t *host_boot_partition;

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    if (!partition) return ESP_ERR_INVALID_ARG;
    host_boot_partition = partition;
    return ESP_OK;
}

const esp_partition_t *host_fixture_boot_partition(void)
{
    return host_boot_partition;
}
