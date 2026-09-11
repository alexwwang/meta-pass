// tests/test_meta_image.c —— meta_image 镜像预校验逻辑的 host 测试。
// 构造最小合法 ESP 镜像头(24 字节 esp_image_header_t 布局)逐项破坏,验证拒绝路径。
#include <assert.h>
#include <string.h>
#include "meta_image.h"
#include "meta_name.h"

// 填充一个"最小合法 ESP32-C3 镜像头":magic 0xE9、1 个 segment、chip_id=0x0005。
static void make_good_header(uint8_t buf[META_IMAGE_HEADER_LEN])
{
    memset(buf, 0, META_IMAGE_HEADER_LEN);
    buf[0] = 0xE9;          // magic
    buf[1] = 1;             // segment_count
    buf[12] = 0x05;         // chip_id 低字节(ESP32-C3 = 0x0005,小端)
    buf[13] = 0x00;         // chip_id 高字节
}

int main(void)
{
    uint8_t hdr[META_IMAGE_HEADER_LEN];

    // 合法头必须通过
    make_good_header(hdr);
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_OK);

    // 截断:不足头部长度
    assert(meta_image_check_header(hdr, META_IMAGE_HEADER_LEN - 1) == META_IMG_ERR_TRUNCATED);
    assert(meta_image_check_header(hdr, 0) == META_IMG_ERR_TRUNCATED);

    // 坏 magic
    make_good_header(hdr);
    hdr[0] = 0x00;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_ERR_BAD_MAGIC);

    // 错误芯片(ESP32 = 0x0000 / ESP32-S3 = 0x0009)
    make_good_header(hdr);
    hdr[12] = 0x00;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_ERR_CHIP);
    make_good_header(hdr);
    hdr[12] = 0x09;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_ERR_CHIP);

    // segment 数量:0 非法,超过上限非法
    make_good_header(hdr);
    hdr[1] = 0;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_ERR_SEGMENTS);
    make_good_header(hdr);
    hdr[1] = META_IMAGE_MAX_SEGMENTS + 1;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_ERR_SEGMENTS);
    make_good_header(hdr);
    hdr[1] = META_IMAGE_MAX_SEGMENTS;
    assert(meta_image_check_header(hdr, sizeof(hdr)) == META_IMG_OK);

    // 尺寸策略:0 与超过上限都拒绝,边界值(恰好等于上限)允许。
    // 上限为 META_NAME_MAX_APP_SIZE(0x1FF000):槽位尾部最后 4KB 保留给显示名 blob。
    assert(meta_image_check_size(0, META_NAME_MAX_APP_SIZE) == META_IMG_ERR_BAD_SIZE);
    assert(meta_image_check_size(META_NAME_MAX_APP_SIZE, META_NAME_MAX_APP_SIZE) == META_IMG_OK);
    assert(meta_image_check_size(META_NAME_MAX_APP_SIZE + 1, META_NAME_MAX_APP_SIZE)
           == META_IMG_ERR_TOO_LARGE);
    // 旧的整槽容量 0x200000 现在超限(尾部 4KB 不再可用于应用镜像)
    assert(meta_image_check_size(0x200000, META_NAME_MAX_APP_SIZE) == META_IMG_ERR_TOO_LARGE);
    // 通用语义不变:调用方传入的上限即判定边界
    assert(meta_image_check_size(0x200000, 0x200000) == META_IMG_OK);

    // 错误串不可为空(用于 UI 显示)
    assert(meta_image_err_str(META_IMG_OK) != 0);
    assert(meta_image_err_str(META_IMG_ERR_CHIP) != 0);
    return 0;
}
