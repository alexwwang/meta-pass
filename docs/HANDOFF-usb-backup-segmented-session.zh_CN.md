# Handoff：USB 分段会话备份读取

语言：[English](HANDOFF-usb-backup-segmented-session.md)

## 目标

验证 USB 页面本地测试 USB 页面备份按钮：将单个长会话的 Flash 读取改为每段 512 KiB，并在段间重建 Transport/ESPLoader。必须由页面本身生成完整归档；不能用宿主脚本拼装的分段归档来证明这条本地测试路径有效。

## 前置条件

- 分支：`feat/storage`
- 被测 HEAD 的 CI 必须通过。
- 使用 Stage D 报告中已有的 Play 28 录音笔设备与固件场景。
- 首轮测试保持设备上的 APP/DATA 组完整。在只读备份完整且独立校验通过之前，不执行删除或恢复。
- 必须从当前 checkout 启动本地服务，确保实际提供的是被测版本的 `install-slot/install-slot.html`。

## 测试步骤

1. 执行 `./tools/validate.sh` 和 `./tools/validate.sh --sim`，记录结果和精确 HEAD。
2. 启动 USB 页面服务，并使用已获准访问设备 Web Serial 端口的 Chrome 持久化 profile。
3. 使用新的证据目录运行 `tools/realdevice/usb_data_backup_read_once.mjs`。该脚本驱动真实生产 `#btn-backup` 路径；页面应自动按 512 KiB 分段读取、在段间关闭并重开 transport/loader，最终下载一个 ZIP。
4. 确认脚本捕获到下载，`manifest.complete === true`，归档包含且仅包含一个 APP 组及其关联 DATA 预留区，并且载荷哈希校验通过。记录总字节数、耗时、各段日志和归档 SHA-256。
5. 使用 `esptool.py` 以只读方式独立 dump APP 镜像和 DATA 区间，与归档对应载荷逐字节比较并比对 SHA-256。不能用设备侧 MD5 代替读取和比较归档中的实际字节。
6. 正常重启设备，确认原 APP/DATA 组仍可见且元数据未变化。这是只读备份测试，不是物理断电测试。
7. 若任一段会话重建失败，页面必须报告备份失败且不得下载不完整归档。保存日志后停止，不要继续破坏性恢复测试。

## 通过标准

- 只需在本地测试 UI 点击一次备份；无需手动重载页面或运行宿主分段脚本。
- 每个归档载荷的字节数和 SHA-256 均准确。
- 独立 `esptool.py` dump 与归档 APP/DATA 载荷逐字节一致。
- 备份前后及重启后 APP/DATA 所有权记录不变。
- 任一分段或必要 DATA 读取失败时，不得把 ZIP 呈现为成功归档。

## 失败时返回的证据

提供被测 commit SHA、主机/Chrome/OS 版本、波特率、归档 manifest、归档 SHA-256、APP/DATA SHA-256、分段与会话重建日志，以及首个失败偏移和错误。脱敏设备 token、IP 和串口标识。在只读门禁通过前，不执行删除/恢复测试。
