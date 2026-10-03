// tests/test_meta_store_scan.c — BUG-21 regression (host, behavior-level).
//
// BUG-21: on device, every boot logged `E (1557) esp_image: invalid segment
// length 0xffffffff`. Root cause: scan_one() ran esp_image_verify() in
// non-silent mode (meta_store.c:85), and IDF's verify_segment_header
// (esp_image_format.c:819-827) logs ESP_LOGE when it reads an erased-state
// (0xFFFFFFFF) segment header — the exact shape a download that died
// mid-write leaves in a slot: [valid header + first segments + 0xFF tail]
// (ESP images interleave segment headers and data, so the walk hits erased
// bytes whenever the download stopped between segment boundaries).
//
// Rejection of a half-written slot is CORRECT behavior (the slot must scan
// INVALID and stay installable-over); the bug is only the error-level
// bootloader-format noise. This file pins the behavior: a half-written slot
// scans INVALID (not VALID, not EMPTY), an erased slot scans EMPTY, a fully
// written valid image scans VALID, and scan_invalid/scan_empty/erase_slot
// keep working. The SILENT-mode call itself is pinned by
// tests/test_bug21_scan_silent.py (static gate, mirrors test_http_contract.py).
//
// Build: see tools/validate.sh (meta_image_verify_sim.c provides the IDF
// segment-walk semantics incl. the 4-byte alignment rule that makes
// 0xFFFFFFFF the trigger value).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "meta_store.h"
#include "meta_sign.h"

void host_fixture_reset(void);
const esp_partition_t *host_fixture_install(uint32_t addr, uint32_t size,
                                            uint8_t type, uint8_t subtype,
                                            const char *label);
uint8_t *host_fixture_flash(void);

#define SLOT_SIZE 0x200000u // ota_1-sized slot

static const esp_partition_t *install_slot(int n)
{
    return host_fixture_install(0x10000u + n * SLOT_SIZE, SLOT_SIZE,
                                ESP_PARTITION_TYPE_APP,
                                ESP_PARTITION_SUBTYPE_APP_OTA_0 + n, "ota");
}

// Install the legacy-shaped 3-slot map (the migrated-device shape; 8 slots of
// 2 MB each would not fit the 8 MB host flash) so scan sees a device-shaped
// partition table. dynslot leaves the remaining indices unallocated — scan
// must mark them EMPTY without a "partition missing" error.
#define SLOTS_PRESENT 3

static void install_all_slots(void)
{
    for (int i = 0; i < SLOTS_PRESENT; i++) {
        assert(install_slot(i) != NULL);
    }
}

// Build a minimal valid ESP32-C3 image at `addr`: one segment whose data is
// the 32-byte app descriptor (magic word first, so the desc read succeeds).
static size_t write_min_image(uint32_t addr)
{
    uint8_t *flash = host_fixture_flash();
    memset(&flash[addr], 0, 64 + 32);
    flash[addr] = 0xE9;          // magic
    flash[addr + 1] = 1;         // segment_count
    flash[addr + 12] = 0x05;     // chip_id = ESP32-C3 (0x0005 LE)
    flash[addr + 13] = 0x00;
    const uint32_t seg_data_off = addr + 24;
    const uint32_t seg_len = 32; // desc-sized segment data
    flash[seg_data_off + 0] = (uint8_t)(seg_len & 0xFF);
    flash[seg_data_off + 1] = (uint8_t)(seg_len >> 8);
    flash[seg_data_off + 2] = 0;
    flash[seg_data_off + 3] = 0;
    uint32_t desc = 0xABCD5432u;
    memcpy(&flash[seg_data_off + 8], &desc, sizeof(desc)); // desc lives at +32 (64B header block)
    const uint32_t data_off = addr + 24 + 8;
    const uint32_t image_len = (data_off - addr) + seg_len + 1; // +checksum byte
    flash[addr + image_len] = 0; // checksum byte
    return image_len;
}

// BUG-21 device shape: an image whose header declares 4 segments but whose
// data stream died after the first segment — [header + seg0 + 0xFF tail]. The
// segment walk then reads the NEXT segment header position inside the erased
// tail; on device that byte-exact shape produced the logged
// `invalid segment length 0xffffffff` (0xFFFFFFFF fails the 4-byte alignment
// check). With only a single declared segment the walk would end before
// touching erased bytes — not the BUG-21 shape.
static void half_write_slot(const esp_partition_t *slot)
{
    uint8_t *flash = host_fixture_flash();
    const uint32_t addr = slot->address;
    memset(flash + addr, 0xFF, slot->size);
    flash[addr] = 0xE9;          // magic
    flash[addr + 1] = 4;         // segment_count=4 declared; data for 1 arrived
    flash[addr + 12] = 0x05;     // chip_id = ESP32-C3 (0x0005 LE)
    flash[addr + 13] = 0x00;
    const uint32_t seg0 = addr + 24;
    const uint32_t seg0_len = 16;
    flash[seg0 + 0] = (uint8_t)(seg0_len & 0xFF); // seg0 header, data_len=16
    // seg0 data occupies addr+32..47; everything from addr+48 on stays 0xFF —
    // the walk's next segment-header read (addr+48) hits 0xFFFFFFFF.
}

int main(void)
{
    meta_slot_info_t slots[META_SLOT_COUNT];

    // 1. Erased slot scans EMPTY — no logging path involved.
    host_fixture_reset();
    install_all_slots();
    memset(slots, 0xAA, sizeof(slots));
    assert(meta_store_scan(slots) == ESP_OK);
    assert(slots[0].state == META_SLOT_EMPTY);
    // 未分配下标(dynslot carve 里没有)必须静默 EMPTY,不是错误。
    for (int i = SLOTS_PRESENT; i < META_SLOT_COUNT; i++) {
        assert(slots[i].state == META_SLOT_EMPTY);
        assert(slots[i].size == 0);
    }

    // 2. Half-written slot scans INVALID and stays installable-over (the
    //    BUG-21 rejection must remain; only the error-level log was noise).
    host_fixture_reset();
    install_all_slots();
    const esp_partition_t *s1 = install_slot(1);
    half_write_slot(s1);
    memset(slots, 0xAA, sizeof(slots));
    assert(meta_store_scan(slots) == ESP_OK);
    assert(slots[1].state == META_SLOT_INVALID);

    // 3. Fully written minimal image scans VALID (positive control).
    host_fixture_reset();
    install_all_slots();
    const esp_partition_t *s2 = install_slot(2);
    memset(host_fixture_flash() + s2->address, 0xFF, s2->size);
    (void)write_min_image(s2->address);
    memset(slots, 0xAA, sizeof(slots));
    assert(meta_store_scan(slots) == ESP_OK);
    assert(slots[2].state == META_SLOT_VALID);

    // 4. Erase turns an INVALID slot back into EMPTY (the documented remedy).
    host_fixture_reset();
    install_all_slots();
    const esp_partition_t *s1b = install_slot(1);
    half_write_slot(s1b);
    assert(meta_store_erase_slot(1) == ESP_OK);
    memset(slots, 0xAA, sizeof(slots));
    assert(meta_store_scan(slots) == ESP_OK);
    assert(slots[1].state == META_SLOT_EMPTY);

    printf("PASS test_meta_store_scan: empty/invalid/valid states + erase recovery\n");
    return 0;
}
