<p align="right">
  <strong>简体中文</strong> · <a href="storage-test-report-20261011.md">English</a>
</p>

# Storage 分支真机测试报告 — feat/storage handoff 执行

**日期**:2026-10-11
**分支**:`feat/storage`
**最终 commit**:`f708a2f`(被测固件对应工作树一致;烧录的 app 构建于 `60fc73c`,之后两个 commit 仅文档)
**固件**:本地 ESP-IDF 5.5.3 构建,叠加 `CONFIG_META_E2E_TEST_CONTROL=y`(`sdkconfig.defaults` + `sdkconfig.e2e-test.defaults`)
**固件 SHA-256**:`3e7ae68a4b138b59ed2d465eac46651f76c3fc6008a65c93c50748d945e05f89`(1,133,184 字节,app 镜像位于 0x10000;设备上版本串 `v2.1.0-668-g115efd9-dirty`)
**设备**:专用测试机(网络地址与本机串口标识不公开),机上无需要保留的用户数据
**Handoff**:[mobile-page-storage-local-test-handoff](assets/mobile-page-storage-local-test-handoff.zh_CN.md),基线 `8259c37`
**原始证据**:`tools/realdevice/logs/mobile-page-e2e-2026-10-11T04-21-15-137Z/`(最佳一轮;更早轮次见同级时间戳目录)

> ⚠️ 设备当前运行的是**测试控制固件**,不能当发布镜像使用。

---

## 1. 总结论

**FAIL — 但设备恢复验证为 RESTORED。** 唯一失败的断言链是子固件运行时串口协议(M04A/M05B):市场固件不响应 `e2edata` 测试协议,用市场玩法拿不到运行时 DATA 证据,B 用例被安全门正确阻断。市场玩法可达的部分全部真机通过,包括史上首次通过手机页面完成 Play 28(3 MiB DATA carve)的安装。

这不是产品回归:生产默认行为无一改变;失败项仅为专用固件独有的运行时证据,以及 §5 记录的容量上限。

## 2. 实际执行环境(不含秘密)

```bash
./tools/validate.sh --static            # PASS
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/m' --cdp-url 'http://127.0.0.1:9222'   # PASS 15/15

# session token 由测试专用串口通道无人值守协商:
#   E2E SESSION_RESET  ->  串口输出 "pair ready: code=… token=…"

META_PASS_E2E_SERIAL_PORT=<launcher-serial-port> \
META_PASS_SESSION=<串口取得的 token> \
node tools/realdevice/mobile_page_e2e.mjs --real-device \
  --url 'http://<device-host>/?mp_test_data_profile=play28-recordings-3m#s=<token>' \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation --play-a 28 --play-b 582
```

## 3. 分项结果

| 用例 | 结果 | 证据 |
|---|---|---|
| 静态检查(`validate.sh --static`) | **PASS** | 含新增契约测试 |
| 手机页多视口 smoke(本地 `/m` 壳页) | **PASS 15/15** | 4 个视口,无溢出/控制台错误 |
| M01 页面健康/视口 | **PASS** | 真机设备页 |
| M02 基线安全门 | **PASS** | 安装器空闲,几何合法,free=6,766,592 |
| M03 搜索 + 详情(Play 28) | **PASS** | playId=28 |
| M04 页面安装 A + 设备落槽 | **PASS** | UI 完成;槽位 VALID;free 6,766,592→2,158,592 |
| M04 A 的 DATA reservation 创建 | **PASS** | newReservations=1(`recordings`,3 MiB profile) |
| 取消卸载严格无操作 | **PASS** | 槽表未变,安装器空闲 |
| M04A 运行时 A(串口 `e2edata` 协议) | **FAIL(预期)** | 市场固件永不回答 READY,超时 |
| M05 安装 B / M05B / M06 / M06C / M07 | **BLOCKED** | 安全门正确拒绝继续 |
| M09 收尾页面健康 | **PASS** | 无控制台/页面错误 |
| 失败清理 + 恢复独立读回 | **PASS** | `deviceRecovery=RESTORED`,0 槽 / 0 DATA / free 复原 |

**DATA 真实写入/读回/checksum/重启持久性:未覆盖** —— 市场固件拿不到该证据;只有 `e2edata` 专用固件支持。

**USB E2E(handoff 阶段 D):SKIP** —— 本轮只验手机页 DATA 路径,handoff 明确禁止无目的重复烧写;共用安装端点(prepare/session/chunk/finalize/remove)已由上面的手机页运行全部覆盖。

## 4. 过程中落地修复(全部在 `feat/storage`)

| Commit | 修复 |
|---|---|
| `7b9eadc` | handoff 英文版 + `.gitignore` 忽略 node_modules(修 validate 失败) |
| `115efd9` | 本地 dev 手机壳页 `GET /m`(原文档 smoke URL 永远渲染不出 `#mp-install-root`)+ `/author-coffee.jpg` 白名单 |
| `b33a571` | runner 空闲门修正(`active` 是店内会话标志,有 token 时恒真);`E2E SESSION_RESET` 串口命令;设备 `/favicon.ico` 204;`max_uri_handlers` 12→16(第 13 个 handler 注册失败会令整个 install 服务启动失败) |
| `60fc73c` | runner 驱动 DATA 容量选择页(`#mp-data-confirm`);`play28-recordings-3m` profile(worker + 本地服务 + 测试);测试控制任务 offer 自动确认 |
| `ae94be8` / `f708a2f` | 本报告 + CHANGELOG/文档更新 |

## 5. 真机实证发现

1. **池分段天花板**:固定 `cardid`(0x356000)/`store`(0x35A000)条目把 6.44 MiB 动态池分成约 1.88 MiB + 4.61 MiB 两段,单段连续 DATA carve 上限约 4.6 MiB。市场上有 DATA 声明的 31 个玩法全部需要 ≥5.7 MiB,**按声明大小均无法安装**;两个市场 DATA 玩法完整 A/B 共存从容量上即不可行。`play28-recordings-4m`(总计约 5.5 MiB)同样放不进单段;只有新增 3 MiB 变体真机安装成功。
2. **legacy 卸载死锁**:legacy `play_id=0` APP 在存在任何 DATA 条目时永远无法卸载(设计上的 fail-closed),早年失败运行留下的孤儿/归档 DATA 不会被回收。恢复只需擦除 carve 记录(`esptool erase_region 0x35A000 0x2000`);NVS 身份与 Wi-Fi 凭证未受影响。
3. **首个手机页驱动市场 DATA 安装**:Play 28 + `play28-recordings-3m` 走通真实设备页面全流程:analyze → 容量选择 → prepare → session → chunk → finalize → carve 提交,设备独立回报 3 MiB `recordings` reservation。

## 6. 剩余缺口 → 完整 E2E

发布两个专用 `data_child` 测试条目(app 约 0.17 MiB,远低于段上限,可共存,串口协议完整)是拿到含运行时 DATA 证据的完整 M01–M08 E2E 的唯一路径,需要创作者中心一次性授权码。
