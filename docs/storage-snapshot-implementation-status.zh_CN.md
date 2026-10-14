# 存储快照实现状态（2026-10-14）

分支：`feat/storage`。

## 已提交的代码基础

- `install-slot/storage-snapshot.js`：v2 快照 manifest、分区几何/保护范围/extent 校验、ZIP 生成与解析、SHA-256 校验，以及非变更式恢复计划生成。
- `main/meta_store_install.c`：新增带同源与 session token 检查的只读分区清单和分区相对偏移读取 API；禁止读取当前运行的宿主分区和 `cardid`，不接受绝对 Flash 地址作为读写目标。
- `install-slot/phone-install.js`：新增受保护的分区读取桥接、完整稀疏快照采集、归档生成、目标布局预检和恢复计划生成函数。
- `install-slot/_worker.js`：将快照模块及相关依赖纳入 no-store 静态模块路由。
- 自动化测试已增加到 `tests/test_storage_snapshot.mjs`、`tests/test_phone_install.mjs` 和 `tests/test_storage_snapshot_device_contract.py`，并登记到 `tools/validate.sh`。

## 尚未完成

- USB 页面尚未切换到新快照归档；USB v1.1 兼容路径目前没有改动。
- 手机页面 UI 尚未接入新快照归档，现有 APP+DATA 备份/还原 UI 尚未完成迁移。
- 设备端只有受保护的只读 API，尚无事务性擦写/恢复 API。当前不应开始写入实现，直到确定失败回滚策略、擦写边界和写后读回验证。
- 未在当前运行环境执行 `tools/validate.sh` 或 ESP-IDF 固件构建；GitHub Actions 状态未确认。所有新增测试目前都属于已提交但尚未验证通过的状态。
- 真机测试未执行；完成代码与 CI 检查后再交给本地 agent 执行 passport-sim 和真机回归。

## 安全约束

快照恢复必须先校验完整归档、所有 payload 的长度与 SHA-256、目标设备实时分区布局和保护范围，再允许任何写入。旧 APP+DATA v2 归档不能被当成 `meta-pass-storage-snapshot`；USB v1.1 历史归档继续由旧路径处理。
