<p align="right">
  <a href="storage-snapshot-implementation-status.md">English</a> · <strong>简体中文</strong>
</p>

# 存储快照实现状态（2026-10-14）

分支：`feat/storage`。

## 当前分支已核验的事实

- 最近核验的 HEAD：`95eea1911646eefcc5ab679317f7929195ca4601`。
- 该提交的 GitHub Actions **Static checks** 与 **Firmware checks** 均已通过。
- `install-slot/storage-snapshot.js` 提供 v2 快照 manifest、分区几何/保护范围/extent 校验、ZIP 编解码、SHA-256 校验、擦除态稀疏表示，以及不修改设备的恢复计划生成。
- 设备端提供鉴权、同源的只读分区清单和分区相对偏移读取 API；禁止读取当前运行的宿主分区和 `cardid`，调用方不能提交绝对 Flash 地址作为读取目标。
- 手机内嵌页支持 v2 整设备快照备份，并显示敏感数据警告。恢复目前严格限制为预检：验证归档和目标布局，不写 Flash。
- USB 页面保留历史 v1.1 固定槽归档路径。NVS 恢复写入有明确的契约测试；不能用 v2 快照解析替换这条旧版兼容路径。
- USB 页面现有的分段读取路径已通过一轮真机端到端备份：页面生成完整归档，独立的 `esptool.py` dump 与 APP/DATA 载荷逐字节一致，并检查了重启持久性。报告也记录了 Chrome Web Serial/esptool-js 会话偶发失败；失败尝试不得生成 ZIP。
- 自动化测试明确守住归档版本边界：USB 历史 v1 还原入口仍可识别；已退役的 APP+DATA 逻辑 v2 还原 handler 不存在；手机页面只接受 v2 整设备快照 discriminator。

## 尚未完成 / 当前阻塞

- **USB v2 快照备份仍 fail-closed。** USB 串口/bootloader 通路还不能可信地获得当前运行宿主分区的设备报告。不得根据静态分区表、硬编码地址或猜测 OTA 状态推断。
- **两端都未开放 v2 快照写入式恢复。** 设备端尚无具备保护范围校验、恢复日志、断连/断电恢复语义及写后校验的安全事务化擦写 API。恢复必须继续停留在预检阶段；不得增加任意地址写入/擦除接口。
- USB 历史 v1.1 归档是独立的兼容契约。不要把 v1 归档支持扩展到手机页面；手机页面仍然只支持 v2。
- 旧 APP+DATA 逻辑 v2 归档必须作为已退役格式明确拒绝，不能被当成 `meta-pass-storage-snapshot`。
- 真机测试不属于 GitHub CI。已有报告覆盖 USB 分段备份以及旧逻辑归档的删除/恢复场景；它不能证明 USB 已支持新整设备 v2 快照备份或恢复。

## 安全约束

开放任何快照恢复写入前，必须在修改设备前验证完整归档、每个 payload 的长度与 SHA-256、目标设备实时分区布局和保护范围。设备端必须独立强制执行分区身份、边界、sector 对齐、保护范围和事务状态。恢复计划只能覆盖明确允许的分区，绝不可暴露任意 Flash 地址。不得移动 `cardid`，也不得绕过动态分配器。
