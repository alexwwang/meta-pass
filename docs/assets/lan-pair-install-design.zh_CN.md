[English](lan-pair-install-design.md) | 简体中文

# 局域网手机辅助安装 — 设计方案

> 状态:**设备端已在 `feat/mota` 实现(2026-10-01):QR 页、本地 HTTP 安装服务、
> prepare → 确认 → chunk → finalize UI;手机端模块与 Worker 保持不变**  
> 分支:`feat/mota`  
> 日期:2026-09-30  
> 取代:本文件之前的 r10.22 局域网配对提案

## 0. 已确认的产品决策

本版按 2026-09-30 确认的取舍编写:

1. 手机网页负责市场搜索、元数据展示、analyze、固件下载、merged image 校验、
   app image 剥离和局域网写入。
2. `metapass.chuanxilu.net` 以动态加载 JavaScript 的方式提供手机端能力;
   启动器固件不内建市场渲染或固件剥离逻辑。
3. analyze 是手机侧“点击安装”时预检的一部分;设备端没有独立 analyze 节点。
   v1 设备接受手机提交的 analysis manifest。
4. `feat/mota` 移除当前设备端“输入编号 → 下载”的市场安装流程。旧代码与旧行为
   保留在上一分支/历史中,便于维护或回退,但不进入新版构建。
5. v1 传输只支持局域网 WiFi HTTP。未来 Bluetooth 可复用同一套
   install-session/chunk 状态机。

## 1. 目标与非目标

### 目标

- 用手机扫码安装替代设备上的数字编号输入。
- 手机浏览和搜索官方市场。
- 展示玩法名称、固件大小、市场 revision、更新时间和校验 hash。
- 点击安装时,由手机完成是否可安装的判断。
- 固件下载与剥离都在手机上完成。
- 只把剥离后的 app image 经局域网写入用户确认的 meta-pass slot。
- slot 推荐和物理确认仍保留在设备上。
- 市场与剥离代码不进入启动器固件,控制固件体积。

### v1 非目标

- 不做 Bluetooth 传输。
- 不做设备端官方市场浏览 UI。
- 不在设备上解包 merged image。
- 不做签名 install manifest。
- 不做设备后台复核 analyze。
- 安装后不自动启动新 slot;用户回到启动器后自行启动。

## 2. 浏览器约束决定的拓扑

二维码不能直接打开一个 HTTPS 页面、再让该页面访问设备本地 HTTP。浏览器会用
mixed-content 与 private-network 规则阻断这个组合。

v1 唯一稳健的拓扑是:

```text
设备屏
  └─ 二维码: http://<设备局域网IP>/#s=<128位会话token>

手机浏览器
  ├─ 打开设备自托管的最小 boot 页面(本地 HTTP)
  ├─ 从 https://metapass.chuanxilu.net 加载版本化 JS 模块
  ├─ 经 HTTPS 访问 metapass 的市场/analyze/固件代理
  └─ 经同源本地 HTTP 访问设备
```

token 放在 URL fragment 中,因此不会随初始 HTTP 请求发送。boot 页面读取 token,
并在后续本地 API 调用中通过自定义 header 携带。

## 3. v1 信任模型

v1 有意采用一个降级的临时信任模型:

- 手机页面通过 HTTPS 从 metapass 获取 analyze。
- 手机把分析结果组装成 install manifest,经本地 HTTP 发给设备。
- 设备校验 manifest 结构、slot 适配、实际上传长度、实际上传 SHA-256 和 ESP
  镜像结构。
- 设备不再次向 metapass 请求 analyze。
- 因此设备无法证明手机提交的 analysis 一定来自 metapass。

v1 仍保留的保护:

- 一次性局域网会话 token;
- 设备上的物理 slot 确认;
- 上传长度与手机声明 hash 的强绑定;
- `esp_ota_end` 与 `esp_image_verify`;
- 任一写失败都会让 slot 保持或进入 INVALID。

这可以防止局域网随机访客或传输损坏写入 slot。它不能防止一个已被攻陷的手机页面
同时伪造展示元数据与上传镜像。后续硬化方向是 metapass 签发带签名的
install manifest,由设备内置公钥验证。

## 4. 组件

### 4.1 设备 boot 页面与本地代理

设备连上路由器 STA 后启动局域网安装 HTTP 服务。设备自托管页面刻意做成
**最小 boot 页面**,不是完整产品 UI。它负责:

- 持有二维码/会话 token;
- 加载版本化远程 loader;
- 暴露一个很小的同源 `DeviceBridge`,封装 status、prepare、session、chunk、
  finalize、cancel 等本地请求;
- 接收远程模块传来的用户操作与固件数据块;
- 作为页面中唯一访问设备写入 API 的对象。

市场 UI、官方市场元数据处理、analyze 调用、固件下载、剥离和进度渲染全部位于
远程加载模块。boot 页面是这些模块与设备之间的本地代理/桥。

```html
<script type="module"
        src="https://metapass.chuanxilu.net/phone-install/loader.v1.js">
</script>
```

boot 页面同时提供无脚本兜底文案,显示设备 IP 与当前会话状态。远程 loader 必须
版本化,并在准备安装前通过协议兼容检查。

### 4.2 metapass 提供的手机端模块

Worker/静态站点提供以下模块:

| 模块 | 职责 |
|---|---|
| `loader.v1.js` | 版本握手、动态导入、全局错误展示 |
| `market.js` | 官方市场搜索、列表、详情、元数据规范化 |
| `install-core.js` | 设备会话、确认轮询、分块上传 |
| `extract-app-image.js` | 现有 merged image 校验与 factory 剥离 |
| `name-blob.js` | 显示名清洗与 MNAM 打包规则 |
| `sha256.js` | 纯 JS SHA-256;本地 HTTP origin 下没有 `crypto.subtle` |

这些模块都是带 CORS 的静态资源,不编进启动器固件。

### 4.3 Worker API

现有 metapass Worker 仍是唯一广域服务。

需要具备:

1. `GET /api/plays?...`
   - 转发官方市场支持的 query,尤其是 `q`;
   - 返回带 CORS 的 JSON;
   - 不缓存市场元数据。
2. `GET /api/play?id=<id>`
   - 返回官方玩法详情;
   - 保留 `revisionId`、`updatedAt`、`firmwareSize`、`firmwareSha256`、
     `downloadUrl` 与嵌套 `firmware` 对象。
3. `GET /api/analyze?id=<id>`
   - 沿用现有契约;
   - 返回 `supported`、`reason`、`slots`、`suggestedSlot`、
     `extracted.imageLen`、`extracted.sha256`。
4. `GET /api/firmware?path=<官方下载路径>`
   - 以流式方式转发官方 merged image,不在 Worker 中整包缓存;
   - 路径白名单仍限定 `/api/download/`;
   - 带 CORS。
5. 手机端静态模块
   - 版本化 URL;
   - `access-control-allow-origin: *`;
   - 只有版本化或内容寻址资源允许长缓存。

`/api/extracted` 继续服务旧启动器与诊断用途,但不是手机安装主路径。

## 5. 官方市场元数据契约

官方详情 API 没有语义化的 `version` 字段。

手机 UI 显示:

- 名称:`title.zh` 或 `title.en`;
- 市场固件大小:`firmware.size`,兜底 `firmwareSize`;
- 市场固件 SHA-256:`firmware.sha256`,兜底 `firmwareSha256`;
- 下载路径:`firmware.url`,兜底 `downloadUrl`;
- 市场 revision:`revisionId`;
- 更新时间:`updatedAt`;
- 剥离后固件内部版本:剥离成功后从 ESP image 中解析(如存在)。

搜索通过 Worker 代理使用官方 `q` 参数。手机可以额外做本地过滤,但远端 `q`
是主搜索路径。

## 6. 安装流程

### 6.1 进入手机安装模式

1. 用户在启动器选择安装入口。
2. 启动器连接已保存的 WiFi。
3. 成功后启动局域网安装 HTTP 服务。
4. 屏幕显示:
   - `http://<device-ip>/#s=<token>` 的二维码;
   - IP 与短配对码的文本兜底;
   - 超时/取消入口。

如果没有 WiFi 凭证,先走现有配网流程。

### 6.2 手机浏览与选择

1. 手机打开设备 boot 页面。
2. boot 页面从 metapass 加载 `loader.v1.js`。
3. loader 检查设备/会话协议版本。
4. 手机经 Worker 搜索官方市场。
5. 用户打开玩法详情,查看名称、大小、市场 revision、更新时间与固件 SHA-256。

### 6.3 手机侧安装预检

用户点击安装后,手机一次完成预检:

1. `GET /api/analyze?id=<id>`。
2. `supported=false` 时展示原因并停止。
3. 经 `/api/firmware` 下载 merged image。
4. 用官方 `firmware.sha256` 校验 merged image。
5. 用 `extract-app-image.js` 剥离 factory app。
6. 在本地计算 extracted app 的 SHA-256。
7. 比对本地 `imageLen`/SHA-256 与 analyze 结果。
8. 按 analyze 的 slot 表判断可安装性。
9. 向设备提交 install offer。

设备端没有独立 analyze 页面或 analyze 作业。

### 6.4 设备确认

install offer 内容:

```json
{
  "protocol": 1,
  "playId": 563,
  "revisionId": 1499,
  "name": "ai-passport-9",
  "storeSha256": "<merged-image-sha256>",
  "imageLen": 1892032,
  "sha256": "<extracted-app-sha256>",
  "suggestedSlot": 0,
  "slots": [
    { "slot": 0, "limit": 1921024, "fit": true },
    { "slot": 1, "limit": 2093056, "fit": true },
    { "slot": 2, "limit": 2740224, "fit": true }
  ],
  "reason": "ok"
}
```

设备随后:

1. 校验 manifest 结构与边界;
2. 展示名称、大小、警告/原因码;
3. 展示三个 slot;
4. 仅当本地分区几何确认可装时默认选中 `suggestedSlot`;
5. 允许用户选择任何本地可装 slot;
6. 要求物理确认;
7. 把会话置为可上传。

手机不能绕过设备端确认,也不能在设备确认状态不变的情况下改最终 slot。

### 6.5 分块上传

v1 使用 install-session/chunk 协议,而不是一个大 POST。

#### Prepare

```text
POST /api/install/prepare
X-Meta-Session: <qr-token>
Content-Type: application/json
```

Body 为上述 install offer。

#### 建立写入会话

设备完成 slot 确认后:

```text
POST /api/install/session
X-Meta-Session: <qr-token>
Content-Type: application/json

{
  "imageLen": 1892032,
  "sha256": "<extracted-app-sha256>",
  "slot": 0
}
```

响应:

```json
{
  "state": "ready",
  "offset": 0,
  "maxChunk": 65536
}
```

设备拒绝任何与已确认 offer 不一致的字段。

#### 写数据块

```text
POST /api/install/chunk
X-Meta-Session: <qr-token>
X-Meta-Offset: 0
Content-Length: 65536
Content-Type: application/octet-stream
```

规则:

- offset 必须精确且顺序递增;
- 单块不得超过 `maxChunk`;
- 设备内部再拆成适合 flash 写入的小块;
- 已写入 offset 的重复块按幂等成功处理;
- 跳块或回退一律拒绝;
- 中断后会话保留到超时或显式取消;
- 重试从设备返回的 offset 继续。

#### Finalize

```text
POST /api/install/finalize
X-Meta-Session: <qr-token>
```

设备检查:

1. 已收字节数等于 `imageLen`;
2. 流式 SHA-256 等于已确认 offer;
3. `esp_ota_end` 成功;
4. `esp_image_verify` 成功;
5. 显示名 blob 写入成功;
6. slot 注册表进入 VALID。

任一失败都 abort OTA;只要 flash 已被触碰,就把 slot 标记为 INVALID。

#### 状态与取消

```text
GET  /api/install/status
POST /api/install/cancel
```

状态包含 state、已确认 slot、offset、期望长度与短错误文案。设备屏展示同一进度。

## 7. `feat/mota` 固件变化

> 实现注记(2026-10-01):下述设备端净切已落地。新增模块:
> `meta_store_install.{c,h}`(HTTP/OTA/token 副作用)与
> `meta_install_model.{c,h}`(纯逻辑 manifest/session/chunk/finalize 判定,
> host 测试 `tests/test_meta_install_model.c`)。旧商店下载模块
> (`meta_store_api/analysis/info_page/idedit/api_fail/range`)、WAN 用的
> TLS/SNTP/证书包依赖及其测试/工具一并删除。
> 验证:`tools/validate.sh --static` 与 `--firmware` 均通过。

### 从新版启动器移除

- 数字玩法 ID 输入页;
- 设备端广域下载页;
- 设备端下载重试/取消流程;
- `meta_store_api_install()`;
- Range 续传下载逻辑;
- 设备端市场 analysis parser/client;
- 若无其他启动器路径使用,则一并移除设备商店 TLS/SNTP 依赖。

旧实现保留在上一分支/历史中。`feat/mota` 是干净切换,不是兼容模式。

### 保留并复用

- WiFi 凭证存储与配网;
- slot 注册表与本地 fit 检查;
- `esp_ota_begin/write/end`;
- 流式 SHA-256;
- `esp_image_verify`;
- 显示名/MNAM 写入;
- boot/delete/rollback policy;
- 商店会话超时纪律。

### 新增设备组件

- QR 渲染与文本兜底;
- STA 模式下的局域网安装 HTTP 服务;
- install-session 状态机;
- 有严格边界的 manifest parser;
- chunk/offset 校验器;
- token 与配对码门禁;
- 本地上传进度快照。

## 8. 安全规则

- 二维码 token 至少 128 bit 随机数。
- 手输兜底为 6 位短码,带短 TTL、失败次数限制,且只能绑定单会话。
- 所有本地变更类 API 都要求 `X-Meta-Session`。
- 设备安装 API 不返回 `Access-Control-Allow-Origin`。
- 如果请求带 `Origin`,必须等于设备本地 origin。
- 同一时间只能有一个 install session。
- 未完成物理 slot 确认前禁止上传。
- 上传 session 的 `imageLen`、`sha256`、`slot` 必须与已确认 offer 完全一致。
- 设备不执行也不信任手机提供的代码。
- 手机页面不获得任何持久设备凭证。

## 9. 性能预期

从设备移除的慢路径是:

```text
ESP32-C3 TLS -> Cloudflare Worker/R2 -> 多 MB body
```

v1 路径是:

```text
手机浏览器 -> metapass/官方市场代理 -> 手机内存
手机浏览器 -> 局域网 HTTP -> ESP32-C3 flash
```

预期收益:

- ESP32-C3 不再承担广域大文件下载;
- 手机浏览器处理 TLS、重传与拥塞的能力更强;
- 只有剥离后的 app image 经过局域网;
- chunk resume 避免手机中断后整包重传。

实际局域网吞吐必须在首次硬件 bring-up 实测;本设计不预设具体速率。

## 10. 未来 Bluetooth 路径

v1 不实现 Bluetooth。

session/chunk 协议刻意保持 transport-neutral,未来 BLE 可映射为:

- `prepare` -> GATT manifest 写;
- `session` -> GATT session-open characteristic;
- `chunk` -> 带 offset 确认的连续 GATT 写;
- `finalize` -> GATT finalize 命令;
- `status` -> GATT notification。

浏览器限制仍显著:Web Bluetooth 不能保证 iOS Safari 可用;若要覆盖多数手机,
可能需要原生 App。

## 11. 验收标准

### 市场浏览

- 中文与英文关键词都能搜索官方市场。
- 详情页正确显示名称、大小、市场 revision、更新时间与固件 SHA-256。
- 缺少必要固件元数据的玩法不可安装。

### 手机剥离

- 市场 hash 不匹配时拒绝。
- 非 ESP32-C3 镜像拒绝。
- 无 factory partition 拒绝。
- 超过所有 slot 上限拒绝。
- 本地剥离出的长度/hash 必须等于 analyze。

### 设备确认

- 未经设备确认不能上传。
- slot 不匹配拒绝。
- 不可装 slot 不能选择。
- 已占用 slot 在确认前显示覆盖警告。

### 写入

- 错误 token 拒绝。
- 错误 offset 拒绝。
- 超大 chunk 拒绝。
- 中断后可从设备报告的 offset 续传。
- hash 不匹配时 finalize 失败并标记 INVALID。
- 成功安装后启动器以选定名称显示该 slot。

### 兼容性

- Android Chrome 可用。
- iOS Safari 可用。
- 手机使用蜂窝且不在同局域网时给出明确的 same-WiFi 错误。
- 发布前覆盖微信内置浏览器。

## 12. v1 明确限制

v1 信任手机提交的 analyze manifest。这是为了控制固件体积与简化流程而接受的
临时取舍。下一步硬化方向是 metapass 签发带签名的 install manifest,设备用内置
公钥验证;这样无需恢复设备端广域 analyze,也能消除剩余的元数据伪造风险。
