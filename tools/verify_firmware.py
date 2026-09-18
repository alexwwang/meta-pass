#!/usr/bin/env python3
"""Verify the merged ESP32-C3 firmware layout produced by idf.py merge-bin."""

from __future__ import annotations

import hashlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


EXPECTED_IMAGES = (
    (0x0000, "bootloader/bootloader.bin"),
    (0x8000, "partition_table/partition-table.bin"),
    (0x10000, "FoloToy-AI-Passport.bin"),
)

FLASH_SIZE = 8 * 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
APP_MAX_SIZE = 0x170000  # factory 缩至 1.44MB(feat/shrink 瘦身)
CARDID_OFFSET = 0x356000
CARDID_SIZE = 0x4000
ENTRY = struct.Struct("<HBBII16sI")

# 升级安全契约:发布镜像只携带 factory/bootloader/分区表,以下区域必须保持擦除态
# (全 0xFF),这样"仅刷这些区域"的升级路径才不会覆盖设备上的用户数据:
#   nvs      0x9000  0x6000  NVS 存储数据:Wi-Fi 配置、应用内部状态(升级必须保留)
#   ota_0    0x180000 0x1D6000  子固件槽位(升级必须保留)
#   ota_1    0x360000 0x200000  子固件槽位(升级必须保留)
#   ota_2    0x560000 0x29E000  子固件槽位 / littlefs 录音(升级必须保留)
#   otadata  0x7FE000 0x2000  OTA 启动选择(升级时重置为擦除态,回到 factory)
UPGRADE_PRESERVED_REGIONS = (
    ("nvs", 0x9000, 0x6000),
    ("ota_0", 0x180000, 0x1D6000),
    ("ota_1", 0x360000, 0x200000),
    ("ota_2", 0x560000, 0x29E000),
    ("otadata", 0x7FE000, 0x2000),
)


@dataclass(frozen=True)
class Partition:
    kind: int
    subtype: int
    offset: int
    size: int
    label: str

    @property
    def end(self) -> int:
        return self.offset + self.size


def parse_partition_table(raw: bytes) -> tuple[list[Partition], bool]:
    """Parse an ESP-IDF table and verify its optional MD5 marker."""
    if len(raw) < PARTITION_TABLE_SIZE:
        raise ValueError("partition table is truncated")

    partitions: list[Partition] = []
    found_md5 = False
    for cursor in range(0, PARTITION_TABLE_SIZE, ENTRY.size):
        magic = int.from_bytes(raw[cursor : cursor + 2], "little")
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            expected = hashlib.md5(raw[:cursor]).digest()
            actual = raw[cursor + 16 : cursor + 32]
            if actual != expected:
                raise ValueError("partition table MD5 marker does not match")
            found_md5 = True
            break
        if magic != 0x50AA:
            raise ValueError(f"invalid partition entry at table offset 0x{cursor:x}")

        _, kind, subtype, offset, size, label_raw, _ = ENTRY.unpack_from(raw, cursor)
        label = label_raw.split(b"\0", 1)[0].decode("ascii", "strict")
        if not label or not size or offset < 0x9000 or offset + size > FLASH_SIZE:
            raise ValueError(f"invalid partition bounds for {label!r}")
        partitions.append(Partition(kind, subtype, offset, size, label))

    if not partitions:
        raise ValueError("partition table is empty")
    return partitions, found_md5


def verify_protected_layout(merged: bytes, build_dir: Path) -> None:
    """Enforce the protected partition and merged-artifact layout."""
    table = merged[
        PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE
    ]
    partitions, found_md5 = parse_partition_table(table)
    if not found_md5:
        raise ValueError("partition table has no MD5 marker")

    by_label = {item.label: item for item in partitions}
    expected = {
        "factory": Partition(0, 0, 0x10000, APP_MAX_SIZE, "factory"),
        "cardid": Partition(1, 2, CARDID_OFFSET, CARDID_SIZE, "cardid"),
    }
    for label, wanted in expected.items():
        if by_label.get(label) != wanted:
            raise ValueError(f"partition {label!r} must remain {wanted}, got {by_label.get(label)}")

    ordered = sorted(partitions, key=lambda item: item.offset)
    for left, right in zip(ordered, ordered[1:]):
        if left.end > right.offset:
            raise ValueError(f"partitions {left.label!r} and {right.label!r} overlap")
    for item in partitions:
        if item.label != "cardid" and item.offset < CARDID_OFFSET + CARDID_SIZE and CARDID_OFFSET < item.end:
            raise ValueError(f"partition {item.label!r} overlaps protected cardid")

    app_path = build_dir / "FoloToy-AI-Passport.bin"
    app_size = app_path.stat().st_size
    if app_size > APP_MAX_SIZE:
        raise ValueError(f"application is {app_size} bytes; limit is {APP_MAX_SIZE}")
    if len(merged) <= 0x10000 or merged[0x10000] != 0xE9:
        raise ValueError("merged artifact has no ESP application image at 0x10000")

    # A derivative may add resource partitions after cardid. The merged file is
    # still acceptable only if the protected cardid region contains padding,
    # never real device identity data.
    payload = merged[
        CARDID_OFFSET : min(len(merged), CARDID_OFFSET + CARDID_SIZE)
    ]
    if any(byte != 0xFF for byte in payload):
        raise ValueError("merged artifact contains forbidden cardid payload bytes")

    print(f"Protected firmware layout: PASS (app {app_size} / {APP_MAX_SIZE} bytes)")


def verify_upgrade_safety(merged: bytes) -> None:
    """发布镜像不得携带任何会覆盖用户数据的内容。

    升级策略:launcher 升级只写 bootloader / 分区表 / factory app / otadata,
    其余分区(NVS、cardid、三个 OTA 槽位)在设备上原样保留。为此,发布镜像
    里这些区域必须全 0xFF——若未来构建流程把数据烧进了这些区域,说明布局
    契约被破坏,必须立即失败而不是静默抹掉用户数据。
    """
    for label, offset, size in UPGRADE_PRESERVED_REGIONS:
        region = merged[offset : offset + size]
        if len(region) < size:
            raise ValueError(f"merged artifact truncated inside {label} region")
        dirty = next((i for i, b in enumerate(region) if b != 0xFF), None)
        if dirty is not None:
            raise ValueError(
                f"merged artifact must keep {label} erased (byte 0x{offset + dirty:x} "
                f"is 0x{region[dirty]:02x}) — upgrades rely on never touching {label}"
            )
    print(
        "Upgrade safety: PASS (nvs/ota_0/ota_1/ota_2/otadata erased "
        "— launcher upgrade preserves them)"
    )


def verify_single_file_artifact(merged: bytes, build_dir: Path) -> None:
    """唯一发布单文件(meta-pass_v*.bin):可引导本体 + 44B MPUPV2 指纹尾段。

    同一个文件服务两个通道(契约见 install-slot/launcher-upgrade.js
    parseUpgradeArtifact):
      市场直接安装 —— 原样写 0x0,本体 = bootloader+分区表+phy+app,ROM 引导;
      网页升级     —— 页面校验指纹后从本体切片升级三段,otadata 写擦除态。
    校验项:指纹布局/长度、body_len 一致、body SHA-256、本体与合并镜像头部
    逐字节同源、无陈旧双产物(bootable_*/upgrade 容器)残留。
    指纹 44B:magic "MPUPV2"(8B,NUL 结尾)+ body_len u32le + body SHA-256 32B。
    """
    artifacts = sorted(build_dir.glob("meta-pass_v*.bin"))
    if not artifacts:
        raise ValueError("single-file firmware missing: build/meta-pass_v*.bin")
    if len(artifacts) > 1:
        raise ValueError(
            f"multiple meta-pass_v*.bin in build/ ({[a.name for a in artifacts]}) "
            "— stale artifact? clean and rebuild"
        )
    raw = artifacts[0].read_bytes()
    FOOTER = 44
    if len(raw) < FOOTER + 0x10000:
        raise ValueError(f"{artifacts[0].name}: too small to carry a bootable body + footer")
    body_len_decl = struct.unpack_from("<I", raw, len(raw) - FOOTER + 8)[0]
    if body_len_decl != len(raw) - FOOTER:
        raise ValueError(f"{artifacts[0].name}: footer body_len {body_len_decl} != {len(raw) - FOOTER}")
    if raw[len(raw) - FOOTER : len(raw) - FOOTER + 8] != b"MPUPV2\x00\x00":
        raise ValueError(f"{artifacts[0].name}: bad MPUPV2 footer magic")
    digest = hashlib.sha256(raw[: len(raw) - FOOTER]).digest()
    if digest != raw[len(raw) - FOOTER + 12 :]:
        raise ValueError(f"{artifacts[0].name}: footer sha256 mismatch")
    body = raw[: len(raw) - FOOTER]
    if body[0] != 0xE9:
        raise ValueError(f"{artifacts[0].name}: first byte is not the ESP image magic 0xE9")
    if body[0x8000 : 0x8000 + 2] != b"\xAA\x50":
        raise ValueError(f"{artifacts[0].name}: no partition table magic at 0x8000")
    if body[0x10000] != 0xE9:
        raise ValueError(f"{artifacts[0].name}: no app image at 0x10000")
    if body != merged[: len(body)]:
        raise ValueError(f"{artifacts[0].name}: body differs from the merged image head")
    # 陈旧双产物检测:单文件化后这两个产物必须不存在,防止新旧格式混发
    stale = sorted(build_dir.glob("meta-pass-bootable_*.bin")) + sorted(
        (build_dir / "upgrade").glob("meta-pass-upgrade_*.bin")
    ) if (build_dir / "upgrade").is_dir() else sorted(build_dir.glob("meta-pass-bootable_*.bin"))
    if stale:
        raise ValueError(
            f"stale dual-channel artifacts present: {[s.name for s in stale]} "
            "— the single file replaces them; clean and rebuild"
        )
    print(
        f"Single-file firmware: PASS ({artifacts[0].name}, {len(raw)} bytes = "
        f"{len(body)}B bootable body + 44B MPUPV2 footer; market install + web upgrade)"
    )


def main() -> int:
    build_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    merged_path = build_dir / "FoloToy-AI-Passport-full.bin"
    flash_args_path = build_dir / "flash_args"

    if not merged_path.is_file() or not flash_args_path.is_file():
        print("ERROR: merged firmware or flash_args is missing", file=sys.stderr)
        return 1

    flash_args = flash_args_path.read_text(encoding="utf-8")
    if "--flash_size 8MB" not in flash_args:
        print("ERROR: flash_args does not select the required 8 MB flash size", file=sys.stderr)
        return 1

    merged = merged_path.read_bytes()
    for offset, relative_name in EXPECTED_IMAGES:
        image_path = build_dir / relative_name
        if not image_path.is_file():
            print(f"ERROR: missing image {image_path}", file=sys.stderr)
            return 1
        image = image_path.read_bytes()
        if merged[offset : offset + len(image)] != image:
            print(f"ERROR: {relative_name} differs at merged offset 0x{offset:x}", file=sys.stderr)
            return 1
        print(f"Verified {relative_name}: {len(image)} bytes at 0x{offset:x}")

    if len(merged) > FLASH_SIZE:
        print("ERROR: merged firmware exceeds 8 MB", file=sys.stderr)
        return 1

    try:
        verify_protected_layout(merged, build_dir)
        verify_upgrade_safety(merged)
        verify_single_file_artifact(merged, build_dir)
    except (OSError, UnicodeDecodeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Merged firmware: PASS ({len(merged)} bytes, flash at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
