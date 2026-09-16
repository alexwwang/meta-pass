# Handoff — 子固件 USB 线刷后"未签名"根因排查

> English: [handoff-unsigned-rootcause.md](handoff-unsigned-rootcause.md)
>
> 2026-09-16。承接会话用:先读 `docs/assets/meta-pass-signing-design.md`(标准设计),再读本文件。
> 分支 `feat/sign`。父固件 `08128a0`(最新,含全部修复)。子固件 `pass-radar_v0.1-2-g8fcce59-signed.bin`。

## 0. 结论(2026-09-16,真机实测确认)

**问题已解决。** 经修复后的安装页重刷签名子固件后,真机签名验证行为符合预期(SIGNED,无警告页)。
根因是 **https://meta-pass.pages.dev/ 部署的旧版安装页(pre-dbbd091)**,而非签名链本身:

- 签名链(密钥链、image_len 语义、digest 范围、设备验签器)已端到端验证正确——见 §2。
- 线上页面是 pre-dbbd091 旧版:三写流程只写 81 字节 MSIG blob(不写完整 4096B tail sector)、
  旧版 `signature`/`signatureOffset` API、`hashAppended` 无 `& 1`、旧的 2-sector(8KB)预留。
  经它安装后,tail sector 处于设备判"未签名"的状态(§3)。
- 关闭复现路径的修复是 `docs/BUGS.md` 的 BUG-03(单写 tail-sector 流程、`& 1` 位测试),
  以及 `server.mjs` 改为服务规范 `install-slot/`(单一来源,杜绝开发副本漂移)。
  设备必须经修复后的页面重刷;被旧页破坏过的槽位在重写前仍是 unsigned。

## 1. 复现条件(修复前实测)

| 变量 | 值 | 状态 |
|---|---|---|
| 父固件(设备 meta-pass) | `meta-pass/build/meta-pass_v0.2.2-37-g08128a0.bin` | **最新**,含 dbbd091 布局 + 325d01f/aa25c52 修复 + 08128a0 |
| 子固件(被刷的) | `pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin` | **host 验签 PASS**(META_SIG_OK) |
| 安装页 | `https://meta-pass.pages.dev/` | **旧版(pre-dbbd091)**——实测抓取确认 |
| 结果 | boot 显示"未签名固件" | 期望:SIGNED |

## 2. 已排除(证据闭合)

- **密钥链**:Keychain 公钥点 `04299a8d…ab651d13` == `public.pem` == `meta_sign_pubkey.h`/`metapass_hook.h`
  内嵌 DER(91B 逐字节相等)。v0.1-2 签名用 `public.pem` openssl 验签 **Verified**。密钥四方一致,排除密钥漂移。
- **image_len 语义**:IDF v5.5.3 `esp_image_format.c` 权威——`process_segments`(24+Σ(8+data_len)) →
  `process_checksum`(ALIGN_UP(+1,16)) → `process_appended_hash_and_sig`(hash_appended=1 时
  **unconditional** `image_len += 32`,line 974,在 `esp_image_verify` 路径 line 207 被调)。
  设备 image_len = 962416,与脚本/C/JS 四方一致。排除 +32 偏差。
- **digest 范围**:设备 `slot_sha256(part, image_len)` = sha256(flash[0:962416]);签名脚本 digest =
  sha256(file[0:962416])。openssl 实测 v0.1-2 签名对该 digest **Verified**。排除 digest 范围错。
- **子固件文件本身**:`run-verify-tests.sh`(编译真实 `meta_sign.c` + brew mbedtls)→ **META_SIG_OK**;
  `meta_sign_detect_sector` true;MAEG "PASS-RADAR v0.1"。文件正确。
- **父固件代码版本**:08128a0 ≥ 74ef1a0 ≥ dbbd091 ≥ 994caaf,scan_one/meta_sign.c 为最新。排除父固件陈旧。
- **旧签名产物混用**:用户明确用的是 v0.1-2(非 v2)。排除。

## 3. 根因定位

### 3.1 唯一确认的活跃漂移:线上安装页 = pre-dbbd091 旧版

`meta-pass.pages.dev/extract-app-image.js`(实测抓取):
- `hashAppended = buf[start + 23] === 1;`(无 `& 1`,BUGS.md (c))
- 返回 `signature`/`signatureOffset`(旧 API),非 `tailSector`/`tailSectorOffset`
- 注释明确"安装器第三次 writeFlash"——**三写流程**(app / 81B 签名 / blob@part_size−4KB)
- `maxAppImageSize = partSize − SIG_SECTOR − BLOB_SECTOR`(旧 2-sector 设计,保留 8KB)

仓库 `install-slot/`(dbbd091 提交)已是**单写流程**(提取完整 4096B tailSector、合并 MNAM、
一次 writeFlash 写整 sector),但:
- **pages.yml 从 `main` 部署**,main HEAD = 74ef1a0(含 dbbd091),但**线上站点滞后于 main**
  (实测抓取为旧版)。
- 工作树修复(`tools/install-slot/` 去重、`server.mjs` 改服务 `install-slot/`)当时**未提交**。

### 3.2 关键机制:tail sector 不被 esp_image_verify 兜底

IDF `verify_simple_hash`(line 225-228,非安全启动路径)只校验 `[0:image_len)` = `[0:962416)` 的 app
部分。**tail sector `[962560:966656)` 完全不在 esp_image_verify 校验范围内**——其内容可以与文件不同
而 IDF 仍判槽位 valid(→ 显示"未签名"而非"invalid")。

旧页对 tail sector 只写 81 字节签名(writeFlash #2),其余 4015 字节靠 erase 残留 0xFF。
**宿主测试**用文件内完整 4096B tailSector 验签 → PASS;**设备**读到的是旧页写入的 tail sector。
设备实际 flash 内容是唯一未被任何测试覆盖的环节——这是"host PASS + 真机 FAIL"唯一可解释 gap。

### 3.3 精确机制确认(flash dump,如再次复现)

如再次出现回归,dump 设备 flash 并与文件偏移 962560 处比对:

```bash
# 假设刷到 ota_0(0x180000);按实际槽位调整
python -m esptool --port /dev/cu.usbmodem* read_flash 0x26B000 4096 /tmp/device_tailsector.bin
# 0x26B000 = 0x180000 + 0xEB000(962560)
xxd /tmp/device_tailsector.bin | head
```

- **全 0xFF** → 签名根本没写进(writeFlash #2 失败/未执行/地址错)→ META_SIG_ABSENT
- **81B 对、其余不同** → 写入正确但被后续操作破坏
- **完全一致** → tail sector 没问题,抓 boot log `槽位 N 签名: unsigned (X)` 的 sr 码分流

最终无需此步:经修复页重刷后症状消失,与 §3.1 根因一致。

## 4. 修复行动(状态)

1. ✅ **提交修复后的安装流程**:单写 tail-sector 页面 + `& 1` 位测试(BUG-03 修复)及 `server.mjs`
   服务规范 `install-slot/`(去重)——见 `docs/BUGS.md`。
2. ✅ **经修复页重刷 v0.1-2** —— 真机验证行为符合预期(SIGNED)。
3. 若再次出现 unsigned:执行 §3.3 flash dump,按 sr 码分流。
4. 清理 `pass-radar/build/` 旧签名产物(`*_v2.bin` 等),防再混用。
5. pages.dev 重新部署后,验证其 `extract-app-image.js` 返回 `tailSector`(非 `signature`)。

## 5. 次要漂移(同链路,一并处理)

- 脚本 `sign-firmware.sh` / 宿主测试 `test_integration.c` 解析器**无 16B 扩展头探测**
  (`extract-app-image.js` 有)——当前镜像无扩展头不触发,但契约不一致。**决策
  (2026-09-16,兼容性优先):将同样的 `[16, 0]` 自动探测移植进两个解析器**,使三方共享
  同一契约;理由与已确立事实见 `docs/development/engineering/debugging-workflow.zh_CN.md`
  §4。**待实现。**
- Wi-Fi 路径(`meta_net.c`)有签名保护(`meta_sign_detect_sector` → 已签名不写 MNAM);
  USB 页无等价运行时保护,依赖 JS 提取正确。

## 6. 标准设计与对齐矩阵

见 `docs/assets/meta-pass-signing-design.md`。对齐结论:签名脚本/宿主测试/设备侧 C 代码/密钥链
**全部符合标准**;唯一活跃漂移 = **线上部署的安装页**(pre-dbbd091)+ 本地副本未提交/未修复——
两者均已关闭(§0、§4)。
