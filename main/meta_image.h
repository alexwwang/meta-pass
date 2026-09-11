// main/meta_image.h —— 子固件镜像的纯逻辑预校验。
// 与 ESP-IDF 解耦(不引用 esp_* 头),可在主机侧测试。
// 定位:上传时的"快速失败"检查(第一个分块即可判定);权威校验由设备侧 esp_ota_end() 完成。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define META_IMAGE_MAGIC         0xE9u    // ESP 应用镜像头魔数
#define META_CHIP_ID_ESP32C3     0x0005u  // esp_image_header_t.chip_id 的 ESP32-C3 取值
#define META_IMAGE_HEADER_LEN    24u      // esp_image_header_t 固定 24 字节
#define META_IMAGE_MAX_SEGMENTS  16u      // IDF 镜像 segment 上限(ESP_IMAGE_MAX_SEGMENTS)

typedef enum {
    META_IMG_OK = 0,
    META_IMG_ERR_TRUNCATED,   // buf 不足 24 字节头
    META_IMG_ERR_BAD_MAGIC,   // 不是 ESP 镜像
    META_IMG_ERR_CHIP,        // 不是 ESP32-C3 镜像
    META_IMG_ERR_SEGMENTS,    // segment 数量为 0 或超上限
    META_IMG_ERR_BAD_SIZE,    // 总长度非法(0 或负数由调用方转换)
    META_IMG_ERR_TOO_LARGE,   // 超过槽位容量
} meta_img_err_t;

// 校验镜像头。buf/len 为收到的前若干字节(>=24 才能判定)。
// 纯函数,无副作用,可重入。
meta_img_err_t meta_image_check_header(const uint8_t *buf, size_t len);

// 校验声明的总长度是否可写入 slot_size 字节的槽位。
meta_img_err_t meta_image_check_size(int64_t total_len, uint32_t slot_size);

// 错误对应的英文短串(UI 文案用英文),永不返回 NULL。
const char *meta_image_err_str(meta_img_err_t err);
