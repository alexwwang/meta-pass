# meta-pass USB 槽位安装器

[English](README.md)

一个 localhost 网页,经 USB 串口把子固件直接写进 meta-pass 的 OTA 槽位——不走 Wi-Fi
导入,**设备固件零改动**(利用 ESP32-C3 的 ROM 下载模式)。

## 前提

- 桌面版 **Chrome 或 Edge**(Web Serial API;Safari/Firefox 不行);
- **Node.js** ≥ 18(仅用于本地小服务器;零依赖,不用 npm install);
- 一条 USB **数据**线(纯充电线看不到设备)。

## 架设本地服务

在仓库根目录:

```bash
node tools/install-slot/server.mjs
# → install-slot server: http://localhost:4191/
```

然后用 Chrome 打开 **http://localhost:4191/**。改端口:`PORT=8080 node tools/install-slot/server.mjs`。

页面必须从 `localhost` 访问——Web Serial 要求安全上下文,设备自己的
`http://192.168.4.1` 页面满足不了,所以这个工具只能以本地服务的形式存在。

## 使用

1. **进下载模式**:**按住 UP 键**开机(UP 把 GPIO0 拉低,即 ROM strapping 引脚),
   再插 USB 线。
2. **连接**:点 *Connect* 选串口。日志应显示 `Connected: ESP32-C3`;
   非 C3 芯片只会警告。
3. **选槽位**:Slot 0(`0x360000`)或 Slot 1(`0x560000`)。
4. **固件来源**:
   - *Local file*:应用单镜像 `.bin`,或 Full Flash 合并镜像——页面在 JS 里自动解包
     (读 `0x8000` 分区表 + 走 ESP 镜像 segment 表);
   - *Community play*:粘贴玩法链接,如 `https://ai-passport.folotoy.cn/plays/105/`
     (或纯数字 `105`)。本地服务器代理下载(只允许 `ai-passport.folotoy.cn`),
     页面按商店公布的 SHA-256 校验。
5. **Display name**(可选,≤32 个可打印 ASCII):自动预填玩法标题或文件名;写入槽位
   的名字 blob sector,显示在 meta-pass 菜单里。
6. **Install** → 等到 `Done.` → **断电重启**设备(拔插 USB 或电源键),在 meta-pass
   菜单里选槽位引导。未签名固件启动前需超长按 OK(LONG2)确认。

## 注意

- 应用镜像上限 **2044KB**(槽位尾部最后 4KB 保留给名字 blob)。
- 未适配的子固件退出方式只有断电重启(回滚机制自动回 meta-pass);适配后的固件把
  `metapass_return_to_launcher()` 挂到 LONG2 上,详见 `docs/assets/meta-pass-design.zh_CN.md` §5。
- 页面只写两个槽位偏移,绝不触碰 `factory`/`cardid`/`otadata`。

## 排障

| 现象 | 原因 / 处理 |
| --- | --- |
| 串口选择框是空的 | 设备没进下载模式(按住 UP 开机),或 USB 线只能充电 |
| `esptool-js still loading…` | 本地 vendor 包还在加载,等一秒重试(完全本地化,无 CDN) |
| `SHA-256 mismatch` | 下载损坏或被篡改;不要安装,重试 |
| 页面按钮全部无响应 | Cmd/Ctrl+Shift+R 强制刷新(缓存了旧页面) |

## 开发

- `extract-app-image.js`、`name-blob.js`:页面与 Node 测试共用的纯 ES 模块。
- `vendor/`:esptool-js 0.5.6 及依赖(pako、atob-lite、ESP32-C3 target 与 stub
  flasher),取自 jsDelivr `+esm` 构建并改写了 import 路径——页面零外部请求。
- 测试:`node tools/install-slot/test-extract.mjs`(镜像解包、名字 blob 测试向量与
  `tests/test_meta_name.c` 字节级双向锁定、尺寸上限边界)。
