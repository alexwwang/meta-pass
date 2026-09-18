# meta-pass USB 串口安装页

[English](README.md) | 简体中文

零依赖的 Chrome 页面，通过 USB 串口把子固件直接写入 meta-pass 的 OTA 槽位。
支持两种部署方式：GitHub Pages + Cloudflare Workers（推荐），或本地 Node 服务。

## 两种部署路径

### A. Cloudflare Pages（推荐，已部署）

页面 + API 代理由 Cloudflare Pages 提供：

```
https://meta-pass.pages.dev/
```

本目录下的 `_worker.js` 在 `/` 提供安装页面，将 `/api/plays`、`/api/play`、
`/api/firmware` 转发到 `https://ai-passport.folotoy.cn` 并补上 CORS 头。
重新部署：

```bash
wrangler pages deploy
```

（CI 在 `install-slot/**` 或 `wrangler.toml` 变更时自动部署。）

### B. 本地 Node 服务（无需部署）

开发或临时使用时直接用本地服务：

```bash
node tools/install-slot/server.mjs
# → install-slot server: http://localhost:4191/
```

然后 Chrome 打开 **http://localhost:4191/**。

路径 A 和 B 功能等价——区别仅在于 API 代理部署在哪里。路径 B 适合不想依赖
外部代理的场景。

## 要求

- **Chrome 或 Edge** 桌面版（Web Serial API；Safari/Firefox 不支持）；
- **Node.js** ≥ 18（仅路径 B 本地服务器需要；npm install 零依赖）；
- USB **数据线**（仅充电的线识别不到设备）。

## 使用方法

1. **进入下载模式**：按住 **UP** 键开机（UP 短接 GPIO0 低电平，触发 ROM
   下载模式），再插 USB 线。
2. **连接**：点 *Connect*，选择串口。日志应显示
   `Connected: ESP32-C3`。非 C3 芯片仅警告不中断。
3. **选槽位**：Slot 0（`0x180000`）、Slot 1（`0x360000`）、Slot 2
  （`0x560000`）。
4. **固件来源**：
   - *本地文件*：应用 `.bin` 或 Full Flash 合成镜像——页面用 JS 自动解包
     （读分区表 `0x8000` + ESP image segment walk）；
   - *社区玩法*：粘贴玩法链接，如
     `https://ai-passport.folotoy.cn/plays/105/`（或只填 `105`）。代理
     拉取下载，页面按商店公布的 SHA-256 校验。
5. **显示名**（可选，≤32 可打印 ASCII）：默认从玩法标题或文件名预填；
   写入槽位尾部 4KB 的 blob 区，之后在 meta-pass 菜单显示。
6. **安装** → 等 `Done.` → **断电重启**设备，然后在 meta-pass 菜单选槽位。
   未签名固件启动前走 BOOT / CANCEL 警告菜单（UP/DOWN 选择，OK 单击确认）。

## 注意事项

- 单镜像体积上限为**分区大小 − 4KB**（槽位尾部最后 4KB sector 保留给显示
  名 blob）。三槽上限：slot 0 ≈ 1.88 MB、slot 1 = 2 MB、slot 2 ≈ 2.68 MB。
- 未适配的子固件无法返回启动器，只能断电重启（回滚机制）。适配过的固件
  可通过 `metapass_return_to_launcher()` 接 OK 长按返回启动器，见
  `docs/assets/meta-pass-design.zh_CN.md` §5。
- 页面不触碰 `factory`、`cardid`、`otadata`，只写三个槽位偏移。

## 故障排查

| 现象 | 原因 / 处理 |
| --- | --- |
| 串口选择框空 | 设备没进下载模式（按住 UP 再开机），或 USB 线只能充电 |
| `esptool-js still loading…` | vendor 包还在加载；等一两秒重试（纯本地，无 CDN） |
| `SHA-256 mismatch` | 下载损坏或被篡改；不要安装，重新拉取 |
| 页面按钮全部失效 | Cmd/Ctrl+Shift+R 强制刷新（可能缓存了旧页） |
| 玩法拉取返回 502 | 代理挂了——若用 GitHub Pages，确认 Cloudflare Worker 已部署且健康 |

## 开发

- `extract-app-image.js`、`name-blob.js`、`slot-backup.js`：纯 ES 模块，页面与 Node 测试共享。
- `vendor/`：esptool-js 0.5.6 + 依赖（pako、atob-lite、ESP32-C3 目标与
  stub flasher），从 jsDelivr `+esm` 构建本地化、import 路径重写——页面除了
  API 代理外**零外部网络请求**。
- 测试：`node tools/install-slot/test-extract.mjs`（镜像解包、名字 blob
  向量、与 C 侧 `tests/test_meta_name.c` 字节级锁定、尺寸边界）与
  `node tools/install-slot/test-slot-backup.mjs`（备份切片 / manifest 往返 /
  恢复空间自检 / dd 镜像兜底）。两者均已接入 `tools/validate.sh --static` 门禁。
- 备份格式门禁：槽位只有在能识别出「应用镜像 + 签名/彩蛋/名称」结构时才按
  固件/尾扇区/额外数据三段结构化备份；否则自动改为 dd 式整槽镜像兜底
  （`slot{N}_raw.bin`，manifest 中 `type: "raw"`）——恢复时仅校验
  总长 ≤ 目标分区与 SHA-256。
- NVS 自动备份/还原（2026-09-18）：从**设备自身分区表**定位 NVS 分区（data/nvs
  子类型，排除 `cardid`），备份时自动读入打包为 `nvs.bin`，SHA-256 写入 manifest
  的 `nvs` 字段；还原时校验文件存在性、字节数与摘要，然后写回**目标设备**定位到的
  NVS 偏移（自适应——manifest 中的 `source_offset` 仅为溯源信息）。两侧均无需用户
  选择；擦除态 NVS 跳过，无 `nvs` 字段的旧备份包行为不变。背景：裸刷单文件固件
  （市场刷机工具）会擦除 0x9000 处的 NVS——应用数据只有经备份/还原往返才能跨刷机保留。

## 仓库布局

```
install-slot/                 # GitHub Pages 根目录
  install-slot.html           # 页面本体（根路径 /）
  extract-app-image.js        # 镜像解包器（ES module）
  name-blob.js                # blob 打包/解包（ES module）
  slot-backup.js              # 槽位备份/恢复逻辑（ES module）
  vendor/                     # 内联 esptool-js + 依赖
  proxy.mjs                   # Cloudflare Worker（单独部署）

tools/install-slot/           # 本地服务器与测试（直接服务上方规范目录）
  server.mjs                  # 本地 Node HTTP 代理（直接服务 install-slot/,proxy.mjs 的替代）
  test-extract.mjs            # 镜像解包 / 名字 blob / i18n 测试
  test-slot-backup.mjs        # 备份切片 / manifest / 恢复测试
```
