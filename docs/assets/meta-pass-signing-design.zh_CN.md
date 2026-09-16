# meta-pass 子固件签名徽章 · 标准设计

> English: [meta-pass-signing-design.md](meta-pass-signing-design.md)
>
> 状态:标准契约(Standard Contract)。各文件实现向本设计对齐,漂移即缺陷。
> 依据:docs/assets/meta-pass-design.md §6/§7、历史修复提交(ea21e03 → dbbd091 → 325d01f → aa25c52 → 9d9fc98)、
> ESP-IDF v5.5.3 权威语义(components/bootloader_support/include/esp_app_format.h、src/esp_image_format.c)、
> docs/BUGS.md。
> 最后修订:2026-09-16。

## 1. 目标与原则

- 应用层软件签名徽章(application-layer signature badge),**可逆**:不使用 eFuse / Secure Boot v2。
- 目标:子固件可被 meta-pass 启动器验明来源;验签通过 → 详情页直接启动;未签名 → BOOT/CANCEL 警告页。
- 单一信任锚:**一个 ECDSA-P256 密钥对**。私钥只在 macOS Keychain(标签 `com.folotoy.meta-pass.signing`);
  公钥编译期嵌入启动器固件。任何"另一把私钥/旧公钥"组合 = 验签必然失败。
- 布局契约以 IDF 二进制格式为准(非"我们认为的格式"),解析器必须与 `esp_image_verify` 逐字对齐。

## 2. 镜像与分区(权威事实,IDF v5.5.3)

- 芯片 ESP32-C3;`esp_image_header_t` 为 **packed 24 字节**,`hash_appended` 是**第 23 字节**(bit0)。
- `esp_image_segment_header_t` = **[load_addr u32][data_len u32]**(注意:load_addr 在前!)。
- 镜像总长(`esp_image_verify` 的 `meta.image_len`):
  `24 + Σ(8 + data_len)` → 补 1 字节 checksum 后 **16 字节对齐** → `hash_appended=1` 时 **+32**(appended SHA-256)。
- 分区(partitions.csv):ota_0@0x180000/0x1D6000、ota_1@0x360000/0x200000、ota_2@0x560000/0x29E000。

## 3. 槽位布局(单一 4KB metadata sector,dbbd091 起)

```
[app image (image_len 字节)] [0xFF pad] [metadata sector (4096B) @ round_up(image_len, 4096)]
```

- metadata sector 偏移:`tail_off = (image_len + 4095) & ~4095`;必须满足 `tail_off + 4096 ≤ 分区大小`。
- 应用镜像上限:`分区大小 − 4096`。
- sector 内固定三段(位置恒定,互不重叠):
  - `[0..127]`    MSIG 签名预留区(变长 blob ≤ 81B)
  - `[128..4055]` MAEG 彩蛋窗口(定长 3928B)
  - `[4056..4095]` MNAM 显示名窗口(40B,blob 右对齐)

### 3.1 MSIG blob(对 image 前 image_len 字节的 SHA-256 做 ECDSA-P256 签名)

```
[4B "MSIG"] [4B payload_len u32 LE (64..72)] [ECDSA-P256 DER 签名 payload_len 字节] [1B xor]
xor = 前 (8 + payload_len) 字节逐字节异或
```

### 3.2 MAEG(彩蛋,不入签名 digest)

```
窗口 128..4055 定长 3928B:
[4B "MAEG"] [4B payload_len u32 LE] [text ≤ 3919B 可打印 ASCII] [0xFF padding] [xor @ 窗口末字节]
xor 覆盖窗口前 3927 字节(含 padding)
```

### 3.3 MNAM(显示名)

```
窗口 4056..4095 右对齐:
[4B "MNAM"] [1B len (1..32)] [name 可打印 ASCII] [1B xor(对 len..len+name 折叠)]
窗口前部保持 0xFF
```

## 4. 密钥链

- **标准事实(信任锚)= macOS Keychain 私钥**,标签 `com.folotoy.meta-pass.signing`
  (`keychain_keygen.c` 生成,`keychain_sign.c` 对 32B 摘要做 ECDSA-P256 签名)。
- 派生链(以 Keychain 为准,逐级校验,任何一级不一致即缺陷):
  1. Keychain 公钥点(`extract-pubkey.c`:`SecKeyCopyExternalRepresentation` → 65B `0x04+X+Y`);
  2. `public.pem`(SPKI DER 91B)——**其尾部 65B 点必须 == Keychain 点**;
  3. `gen-pubkey.py` 从 `public.pem` 重写 `main/meta_sign_pubkey.h` + `main/metapass_hook.h`
     两处内嵌 DER 数组(须 == public.pem DER,91B 逐字节相等;validate.sh 比对二者)。
- 校验命令:
  - 点比对:`./extract-pubkey` 输出 vs `openssl pkey -pubin -in public.pem -outform DER | tail -c 65`
  - 签名回路:`openssl pkeyutl -verify -pubin -inkey public.pem -sigfile <sig.der> -in <digest.bin>`
    (对 `keychain-sign` 输出验签,勿用 `openssl dgst -verify`——它会对输入再哈希)
- 禁止 `private.pem` fallback(8988bab 已删除该路径)。
- 不变式:**Keychain ↔ public.pem ↔ 两处内嵌 DER 四点同步**;任一更换以 Keychain 为准重新派生。

## 5. 签名流程(sign-firmware.sh)

1. 解析 `image_len`:语义必须与 `esp_image_verify` 一致(§2 公式,`hash_appended` 读 byte 23 bit0,含 +32)。
   - 注意:部分工具链会在 24B 头后插入 **16B 扩展头**——解析器应支持自动探测(先按带扩展头解析,失败回退)。
2. `digest = SHA-256(file[0:image_len])`(覆盖完整 image_len,含 appended hash)。
3. Keychain 签名 → DER(64..72B)。
4. 组装 4096B sector:MSIG@0(blob ≤81B),可选 MAEG@128,其余 0xFF。
5. 输出:`[app 原样][0xFF pad 至 round_up(image_len,4096))][4096B sector]`;输出名 `{name}_{version}-signed.bin`。

> 教训(2026-09-16 实测):工具链修复前签出的产物(如 `pass-radar_signed_v2.bin`)布局/魔数/xor 全对,
> 但 ECDSA 签名对应的 digest 与正确语义(完整 image_len 含 appended hash)不一致 → 设备验签必然
> `META_SIG_VERIFY_FAIL` → 显示"未签名"。文件级测试必须对**实际线刷的那一个文件**执行,
> 旧 signed 产物应在重签后删除/归档,禁止混用。

## 6. 设备侧验证(启动扫描 scan_one / 父固件自检)

1. `esp_image_verify(ESP_IMAGE_VERIFY)` 权威校验(magic/chip/segment/checksum/appended hash)→ `meta.image_len`。
2. 流式计算 `SHA-256(分区[0:image_len])`(4KB 分块,不整包入 RAM)。
3. `tail_off = round_up(image_len, 4096)`;`tail_off + 4096 ≤ part->size` 才读 sector。
4. `meta_sign_verify(digest, image_len, sector, 4096)`:
   magic → payload_len ∈ [64..72] → xor → mbedtls 用内嵌公钥验 DER 签名。
5. `signed_fw = (result == META_SIG_OK)`;未签名 → BOOT/CANCEL 警告页(默认停在 CANCEL)。

## 7. 安装通道

### 7.1 USB 串口安装页(install-slot/)

- **单写契约**:签名扇区必须与 app 同批次正确落位。规范流程:
  1. `extractAppImage(file)`:解析 `imgLen`(§5.1 同语义)→ 从文件 `round_up(imgLen,4096)` 处探测 MSIG,
     提取完整 4096B tail sector(`tailSector`),记录 `tailSectorOffset`。
  2. writeFlash #1:`[0:imgLen)` 应用字节。
  3. 若 dispName:把 MNAM 窗口合并进已提取的 tail sector(**保留 MSIG 字节**)。
  4. writeFlash #2:完整 4096B tail sector @ `slot + tailSectorOffset`。
  - 禁止:把签名/彩蛋/名字拆成多个独立 writeFlash 写同一扇区(esptool 每写必擦整扇区,先写者被抹)。
  - 部署要求:页面与 `_worker.js` 必须与仓库 `install-slot/` 同步发布;缓存策略不得让浏览器滞留旧版。

### 7.2 Wi-Fi 导入(meta_net.c h_upload)——签名保护参考实现

- OTA 整镜像写入(含签名文件的 tail sector);回读 flash 计算 digest(与 scan_one 同语义)。
- tail sector 处理:`meta_sign_detect_sector` 探测到 MSIG → **禁止写 MNAM**(签名保护);
  未签名且给名 → 擦整扇区后写 MNAM 窗口;未签名无名字 → 擦除防残留。

### 7.3 删除槽位

- 整分区擦除(含 metadata sector)。

## 8. 四方一致性契约(image_len 必须逐字节相等)

| 方 | 载体 | 语义 |
|---|---|---|
| 签名脚本 | sign-firmware.sh(Python) | §2 公式 |
| 宿主验签测试 | tools/signing/test_integration.c | 与脚本逐字对齐 |
| 安装页解析 | install-slot/extract-app-image.js | §2 公式 + 16B 扩展头自动探测 + byte23 `& 1` |
| 设备侧 | esp_image_verify `meta.image_len` | IDF 权威 |

任一方向性偏差(hash_appended 读错字节 / 漏 +32 / 扩展头 / checksum pad 对齐)都会导致
**签名扇区写入位置或 digest 与设备读取错位 → 设备判 unsigned**——文件级测试无法覆盖设备侧实现,
必须用 IDF 源码/真实 bin 对账(本设计 §2 即对账结论)。

## 9. 不变式清单

1. `tail_off = round_up(image_len, 4096)`,sector 完整 4096B,`tail_off+4096 ≤ part_size`。
2. 已签名扇区禁止被任何 MNAM/MAEG 写入覆盖(只能读-改-写,保留 MSIG 字节)。
3. 三方解析器(脚本/C/JS)+ IDF 的 image_len 一致(§8)。
4. 公钥四方同步(§4)。
5. 安装页部署版本 = 仓库 `install-slot/` 最新提交;禁止旧版长期在线。
