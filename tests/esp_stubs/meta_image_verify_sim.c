// tests/esp_stubs/meta_image_verify_sim.c — host simulation of the IDF
// esp_image_verify() segment walk (IDF 5.5.3 esp_image_format.c), scoped to
// what the meta-store scan path needs: header magic/chip/segment_count, the
// interleaved segment table, and verify_segment_header's checks:
//   (data_len & 3) != 0  ||  data_len >= ESP_IMAGE_MAX_FLASH_ADDR_SIZE (0x1000000)
// An erased-state (0xFFFFFFFF) segment header — the on-device shape left by a
// download that died mid-write (BUG-21) — fails both checks and reproduces the
// device's "invalid segment length 0xffffffff" rejection path byte-exactly.
//
// Layout note (why a half-written image is rejected here): ESP app images
// interleave segment headers and segment data. A download stopped at ~14%
// leaves [valid header + first segments + 0xFF tail]; the walk reads the next
// segment header position inside the erased tail.
//
// Checksum is NOT computed in this simulation (checksum byte verification is
// IDF-internal); the metadata.image_len is filled so callers (meta_sign
// sector offset, sha256) can proceed on valid images.
#include "esp_image_format.h"
#include "esp_partition.h"

#include <stddef.h>
#include <string.h>

#define SIM_CHIP_ID_ESP32C3 0x0005u
#define SIM_MAX_SEGMENTS 16u
#define SIM_MAX_FLASH_ADDR_SIZE 0x1000000u

static esp_err_t sim_read(const esp_partition_pos_t *part, uint32_t off,
                          void *dest, size_t size)
{
    // Mirror esp_partition_read against the registered host flash fixture.
    esp_partition_t p = {0};
    p.address = part->offset;
    p.size = part->size;
    return esp_partition_read(&p, off, dest, size);
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part,
                           esp_image_metadata_t *data)
{
    (void)mode; // SILENT vs verbose is a logging distinction on device; host sim is silent.
    if (!part || !data || part->size == 0) return ESP_ERR_INVALID_ARG;

    esp_image_header_t hdr;
    if (sim_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
    if (hdr.magic != 0xE9) return ESP_ERR_IMAGE_INVALID;
    if (hdr.chip_id != SIM_CHIP_ID_ESP32C3) return ESP_ERR_IMAGE_INVALID;
    if (hdr.segment_count == 0 || hdr.segment_count > SIM_MAX_SEGMENTS) {
        return ESP_ERR_IMAGE_INVALID;
    }

    uint32_t offs = sizeof(esp_image_header_t);
    esp_image_segment_header_t seg;
    for (int i = 0; i < hdr.segment_count; i++) {
        if (sim_read(part, offs, &seg, sizeof(seg)) != ESP_OK) return ESP_FAIL;
        // verify_segment_header (esp_image_format.c:819-827): an erased
        // 0xFFFFFFFF data_len violates both the 4-byte alignment and the
        // flash-address bound — exactly the on-device BUG-21 rejection.
        if ((seg.data_len & 3u) != 0 || seg.data_len >= SIM_MAX_FLASH_ADDR_SIZE) {
            return ESP_ERR_IMAGE_INVALID;
        }
        data->segments[i] = seg;
        offs += (uint32_t)sizeof(esp_image_segment_header_t) + seg.data_len;
    }

    // Checksum byte + (if hash_appended) SHA-256 trailer follow the last segment.
    uint32_t image_len = offs + 1u;
    if (hdr.hash_appended) {
        image_len += ESP_IMAGE_HASH_LEN;
    }
    if (image_len > part->size) return ESP_ERR_IMAGE_INVALID;

    data->start_addr = part->offset;
    data->image_len = image_len;
    data->image = hdr;
    return ESP_OK;
}
