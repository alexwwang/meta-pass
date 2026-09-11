// main/meta_image.c —— 实现见头文件注释。
#include "meta_image.h"

// esp_image_header_t 内部字段偏移(固定布局,IDF v4/v5 一致):
//   [0] magic  [1] segment_count  [2] flash_mode  [3] flash_size_freq
//   [4..7] entry_addr  [8] wp_pin  [9..11] spi_pin_drv  [12..13] chip_id(小端)
#define OFF_MAGIC       0u
#define OFF_SEG_COUNT   1u
#define OFF_CHIP_ID_LO  12u
#define OFF_CHIP_ID_HI  13u

meta_img_err_t meta_image_check_header(const uint8_t *buf, size_t len)
{
    if (!buf || len < META_IMAGE_HEADER_LEN) {
        return META_IMG_ERR_TRUNCATED;
    }
    if (buf[OFF_MAGIC] != META_IMAGE_MAGIC) {
        return META_IMG_ERR_BAD_MAGIC;
    }
    const uint16_t chip_id = (uint16_t)(buf[OFF_CHIP_ID_LO] | (buf[OFF_CHIP_ID_HI] << 8));
    if (chip_id != META_CHIP_ID_ESP32C3) {
        return META_IMG_ERR_CHIP;
    }
    if (buf[OFF_SEG_COUNT] == 0 || buf[OFF_SEG_COUNT] > META_IMAGE_MAX_SEGMENTS) {
        return META_IMG_ERR_SEGMENTS;
    }
    return META_IMG_OK;
}

meta_img_err_t meta_image_check_size(int64_t total_len, uint32_t slot_size)
{
    if (total_len <= 0) {
        return META_IMG_ERR_BAD_SIZE;
    }
    if ((uint64_t)total_len > (uint64_t)slot_size) {
        return META_IMG_ERR_TOO_LARGE;
    }
    return META_IMG_OK;
}

const char *meta_image_err_str(meta_img_err_t err)
{
    switch (err) {
    case META_IMG_OK:            return "OK";
    case META_IMG_ERR_TRUNCATED: return "truncated image";
    case META_IMG_ERR_BAD_MAGIC: return "not an ESP image";
    case META_IMG_ERR_CHIP:      return "wrong chip (need ESP32-C3)";
    case META_IMG_ERR_SEGMENTS:  return "bad segment count";
    case META_IMG_ERR_BAD_SIZE:  return "bad image size";
    case META_IMG_ERR_TOO_LARGE: return "image too large for slot";
    }
    return "unknown error";
}
