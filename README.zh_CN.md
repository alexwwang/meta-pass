[English](README.md) | 简体中文

# meta-pass —— FoloToy AI Passport 多固件启动器

meta-pass 把 AI Passport 变成多固件设备：它作为常驻启动器住在 factory 分区，可以通过
Wi-Fi 把其他适配本硬件的固件导入本地两个 2 MB Flash 槽位并引导启动，无需每次重新烧录；
同时提供本机菜单查看、启动、删除已存储的固件。

设计文档（单一权威来源）：[`docs/assets/meta-pass-design.zh_CN.md`](docs/assets/meta-pass-design.zh_CN.md)。

## 功能

- **双固件槽位**（`ota_0@0x360000`、`ota_1@0x560000`，各 2 MB）；启动器自身保持在受保护的
  基线布局中（`factory@0x10000`、`cardid@0x356000` 原样不动，门禁强制校验）。
- **免线缆导入**：Import 页开启 WPA2 SoftAP，随机密码与 6 位一次性配对码显示在屏幕上；
  在任意浏览器打开 `http://192.168.4.1/` 上传应用 `.bin`。上传以 1024 字节分块流式写入。
- **完整性校验**：镜像 magic、chip id（ESP32-C3）、槽位容量、全镜像 SHA-256（屏上显示供人工
  比对），外加 `esp_ota_end()` 权威复核。未签名固件启动前需超长按（LONG2）确认。
- **默认防变砖**：启用应用回滚，未适配的子固件以「试运行」运行——任何重启（崩溃、断电）都会
  回到启动器。已适配子固件可常驻（见下）。
- **本地管理**：列表显示名称/版本/大小/哈希，启动与删除均需 LONG2 二次确认。

## 按键

| 按键 | 动作 |
| --- | --- |
| UP/DOWN 短按 | 移动选中项 |
| OK 短按 | 进入 / 执行选中动作 / 确认页取消 |
| OK 长按（LONG，1.5 秒） | 返回上一级 |
| OK 超长按（LONG2，3 秒） | 确认启动/删除；在已适配子固件中 = 退回启动器 |

## 子固件适配（可选）

在子固件中包含 `main/metapass_hook.h`：

1. 自检通过后调用 `metapass_mark_valid()` → 跨重启常驻（否则表现为试运行，下次重启回启动器）。
2. 在 OK 键的 `BSP_BTN_LONG2` 事件中调用 `metapass_return_to_launcher()`。注意 1.5 秒处会先触发
   一次 LONG；请把 LONG 安排为无害的应用内动作（如返回上一页）。

未适配的固件也能用：只是每次重启都会回到启动器，且不需要重新烧录——从菜单再次选择即可。


## USB 串口安装（免 Wi-Fi，第二通道）

不用开热点，数据线直连 Chrome 即可安装子固件（设备固件零改动，走 ROM 下载模式）：

```bash
node tools/install-slot/server.mjs   # 打开 http://localhost:4191/
```

1. 设备**按住 UP 键**开机（GPIO0 拉低进入 ROM 下载模式），USB 连电脑；
2. 页面里连接串口 → 选槽位（Slot 0/1）→ 选来源：本地 `.bin`（Full 合并镜像自动解包出应用部分）
   或粘贴社区玩法链接（自动下载、按社区公布的 SHA-256 校验）；
3. 点 Install 写入并校验，断电重启后在 meta-pass 菜单引导。

设计与权衡见 `docs/assets/meta-pass-design.zh_CN.md` §6.1。解包逻辑有独立测试：
`node tools/install-slot/test-extract.mjs`。

## 构建与烧录

```bash
source <esp-idf-v5.5.3>/export.sh        # 必须是 ESP-IDF v5.5.3
./tools/validate.sh --firmware           # 构建 + 受保护布局校验
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 build/FoloToy-AI-Passport-full.bin
```

已写入身份（`cardid`）的设备严禁 `idf.py erase-flash`。详见
`docs/development/engineering/protected-flash-layout.zh_CN.md`。

## 与上游基线的结构差异

- `main/`：启动器 UI（`main.c`）、存储层（`meta_store`）、导入通道（`meta_net`）、纯逻辑
  （`meta_image`、`meta_slots`、`meta_import`）、子固件 hook（`metapass_hook.h`）；移除 demo 页。
- `components/bsp`：新增 `BSP_BTN_LONG2` 按键事件（向后兼容）。
- `partitions.csv`、`sdkconfig.defaults`：OTA 槽位 + 应用回滚；移除 BLE 配置（启动器不用）。
- `tests/` + `tools/validate.sh`：三个新 host 测试套件接入静态门禁；`tools/install-slot/` 为
  USB 串口安装页（本地服务，见上文）。
- `docs/assets/`：fork 自有的设计文档。
