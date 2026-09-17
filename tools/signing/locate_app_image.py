#!/usr/bin/env python3
# tools/signing/locate_app_image.py —— 输入镜像定位器(单一事实源)。
#
# 回答一个问题:给定一个输入文件(裸 app 镜像 或 Full 合并镜像),
# 应用镜像从哪里开始、有多长、签名 sector 应该落在哪个位置。
#
# 支持两种输入(与 install-slot/extract-app-image.js 同一契约):
#   1. 裸 app 镜像:   文件头即 ESP 镜像头(0xE9)。
#   2. Full 合并镜像: 0x8000 处有分区表 magic(AA 50);应用位于
#      factory 分区(type=0, subtype=0)的 offset 处。典型布局:
#      bootloader@0x0000, 分区表@0x8000, app@0x10000。
#
# ESP image_len 语义与设备侧 esp_image_verify() 一致:
#   24B 头 + segments + checksum pad(+1B 后对齐 16)+ hash_appended 时 +32B。
# 部分工具链在 24B 头后插入 16B 扩展头 —— 双布局探测,恰有一种收敛。
#
# 用法:
#   locate_app_image.py locate <file>
#     输出(空格分隔): <mode> <app_offset> <image_len>
#     mode: "app"(裸镜像, app_offset=0) 或 "full"(合并镜像)
#   locate_app_image.py tail-offset <file>
#     输出: <mode> <app_offset> <image_len> <tail_offset_slot_rel>
#     tail_offset_slot_rel = image_len 向上对齐 4K(即槽位内 MSIG 落点)。

import struct
import sys

ESP_IMAGE_MAGIC = 0xE9
ESP_HEADER_LEN = 24
ESP_EXT_HEADER_LEN = 16
ESP_MAX_SEGMENTS = 16
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY_LEN = 32


class LocateError(SystemExit):
    """定位失败:带英文错误信息退出(退出码 1,供脚本与测试断言)。"""


def u32le(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def try_layout(image, start, ext_hdr_len):
    """按给定扩展头长度走 segment 表,返回镜像总长;结构不合法返回 None。"""
    seg_count = image[start + 1]
    offset = start + ESP_HEADER_LEN + ext_hdr_len
    for _ in range(seg_count):
        if offset + 8 > len(image):
            return None
        seg_len = u32le(image, offset + 4)
        offset += 8 + seg_len
        if offset > len(image):
            return None
    # checksum: 当前偏移处 1 字节,随后整体填充到 16 字节边界
    unpadded = offset
    offset = unpadded + ((unpadded + 1 + 15) & ~15) - unpadded
    # esp_image_header_t 末字节(偏移 23)的 hash_appended 标志
    if image[start + 23] & 1:
        offset += 32
    return offset - start


def esp_image_len(image, start):
    """从 start 处解析 ESP 镜像,返回 image_len(相对 start)。

    双布局探测:先 16B 扩展头,失败回退纯 24B 头。都不收敛则报错。
    """
    if start + ESP_HEADER_LEN > len(image):
        raise LocateError(f"error: file too small for an ESP image header at offset {start}")
    if image[start] != ESP_IMAGE_MAGIC:
        raise LocateError(
            f"error: bad magic byte 0x{image[start]:02x} at offset {start} — expected 0xE9 (not an ESP app image)"
        )
    seg_count = image[start + 1]
    if seg_count == 0 or seg_count > ESP_MAX_SEGMENTS:
        raise LocateError(f"error: invalid segment count {seg_count} (expected 1..{ESP_MAX_SEGMENTS})")
    for ext_hdr_len in (ESP_EXT_HEADER_LEN, 0):
        result = try_layout(image, start, ext_hdr_len)
        if result is not None:
            return result
    raise LocateError(
        "error: image segment table does not resolve under any known layout (16B-ext or plain)"
    )


def is_full_image(image):
    """Full 合并镜像识别:0x8000 处分区表 magic(AA 50)。"""
    return len(image) > PARTITION_TABLE_OFFSET + 1 and (
        image[PARTITION_TABLE_OFFSET] == 0xAA and image[PARTITION_TABLE_OFFSET + 1] == 0x50
    )


def find_factory_partition(image):
    """分区表逐条扫描(32B/条):type@+2、subtype@+3、offset U32LE@+4、size U32LE@+8。

    type=0 且 subtype=0 即 factory 应用;返回 offset 或 None。
    """
    off = PARTITION_TABLE_OFFSET
    while off + PARTITION_ENTRY_LEN <= len(image):
        if image[off] != 0xAA or image[off + 1] != 0x50:
            break
        if image[off + 2] == 0x00 and image[off + 3] == 0x00:
            return u32le(image, off + 4)
        off += PARTITION_ENTRY_LEN
    return None


def locate(image):
    """返回 (mode, app_offset, image_len)。

    mode: "app" —— 裸应用镜像(app_offset=0);
          "full" —— Full 合并镜像(应用在 factory 分区)。
    """
    if not image:
        raise LocateError("error: input file is empty")
    if is_full_image(image):
        app_off = find_factory_partition(image)
        if app_off is None:
            raise LocateError(
                "error: full flash image detected, but no factory app partition "
                "(type=0, subtype=0) found in the partition table"
            )
        return "full", app_off, esp_image_len(image, app_off)
    return "app", 0, esp_image_len(image, 0)


def tail_offset(image_len, sector=4096):
    """槽位内 metadata sector 落点:image_len 向上对齐 sector。"""
    return (image_len + sector - 1) // sector * sector


def main(argv):
    if len(argv) != 3 or argv[1] not in ("locate", "tail-offset"):
        print(
            "usage: locate_app_image.py (locate|tail-offset) <file>\n"
            "  locate       -> MODE APP_OFFSET IMAGE_LEN\n"
            "  tail-offset  -> MODE APP_OFFSET IMAGE_LEN TAIL_OFFSET(slot-relative)",
            file=sys.stderr,
        )
        raise SystemExit(2)
    try:
        with open(argv[2], "rb") as f:
            image = f.read()
    except OSError as exc:
        raise LocateError(f"error: cannot read {argv[2]}: {exc}")
    mode, app_off, image_len = locate(image)
    if argv[1] == "locate":
        print(f"{mode} {app_off} {image_len}")
    else:
        print(f"{mode} {app_off} {image_len} {tail_offset(image_len)}")


if __name__ == "__main__":
    main(sys.argv)
