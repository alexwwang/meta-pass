<p align="right">
  <a href="play563-appstore-download-reverse.md">English</a> · <strong>简体中文</strong>
</p>

# Play 563 AppStore 下载链路逆向分析

日期：2026-09-30  
范围：用户提供的 v50 串口日志、托管 play 563（`https://ai-passport.folotoy.cn/plays/563/`）、设备本地页面 `http://192.168.0.19/appStore?t=aa54e022`，以及 meta-pass 的 `main/meta_store_api.c`。  
边界：只做 review 与逆向分析；未修改固件或 Worker 代码。

## 结论先行

剩余速度问题主要在 **meta-pass 网络路径**，不在 ESP32-C3 下载循环本身。

catalog API 显示 play 563 不是官方标记玩法（`source=community`、`isOfficial=false`）；它是托管在官方域名下的社区 AppStore 玩法。它的二进制从同一 nginx 源站直接下载：服务器忽略 `Range`，固件用一次完整 `200` 下载、边下边校验 catalog 给出的 SHA-256、直接写 flash。

meta-pass 走的是带票据的 Cloudflare Worker 路径：先提取应用镜像，再从 edge cache/R2 发字节。该路径现在已有正确的 Range 续传，但 host 与真机观察到的链路仍明显慢于 AppStore 直连源站。串口日志中的成功续传证明 r10.17-r10.19 解决的是**正确性**问题，不是**路径延迟/停顿**问题。r10.21 已修复最严重的固件侧僵尸连接等待，但它无法把慢源站变快。

## 已收集证据

### Play 563 元数据

公开 `/plays/563/` 页面只渲染通用 AppStore 前端载荷，因此有效事实来源是设备代理的 catalog API：

- `id/projectId`：`563`
- `slug`：`ai-passport-9`
- 标题：`AI Passport 应用商店` / `AI Passport AppStore`
- `source`：`community`
- `isOfficial`：`false`
- 作者：`新空气`
- `firmwareSize`：`3,219,456`
- `firmwareSha256`：`2956f77bb1db312b5796cf8982bceed66be564d4590441a9fa8abfddb3fbdf01`
- `downloadUrl`：`/api/download/community/ai-passport-9`
- `firmware.format`：`esp-merged-0x0`
- `downloads`：`1560`
- `publishedAt`：`2026-09-29T02:50:44+00:00`

### 托管二进制行为

探测请求：

```text
GET https://ai-passport.folotoy.cn/api/download/community/ai-passport-9
Range: bytes=0-1023
```

实际响应：

- `HTTP/2 200`，不是 `206`
- `Content-Length: 3219456`
- 没有 `Accept-Ranges` 头
- `Cache-Control: private, no-store`
- body 从第 0 字节开始
- 完整 body 的 SHA-256 与 catalog 元数据一致

结论：托管 play 563 的端点没有实现 Range。它之所以能不带续传，是因为观察到的直连源站足够快。

### Host 侧耗时对比

同一台 host、同一时间窗口：

| 路径 | 协议 | 大小 | TLS | TTFB | 总耗时 | 吞吐 |
|---|---:|---:|---:|---:|---:|---:|
| Play 563 直连 | HTTP/2 | 3,219,456 B | 0.105s | 0.142s | 0.395s | 8.15 MB/s |
| Play 563 直连 | HTTP/1.1 | 3,219,456 B | 0.091s | 0.130s | 0.397s | 8.12 MB/s |
| meta-pass play 675 | HTTP/2 | 2,664,256 B | 0.435-0.844s | 0.969-3.380s | 6.78-9.50s | 0.39-0.47 MB/s |
| meta-pass play 675 | HTTP/1.1 | 2,664,256 B | 0.464s | 0.969s | 2.67s | 1.00 MB/s |

meta-pass 响应带 `x-source: edge`、`accept-ranges: bytes`、`age: 893`，以及正确的 `x-image-len` / `x-image-sha256`。因此这次没有走旧的冷转换路径；即使命中 edge，剩余差距仍来自 Cloudflare/Worker 网络路径及其 TLS/TTFB 表现。

本机 DNS 返回的是本地代理地址（`198.18.0.0/16`），所以这里没有证明绝对地域路由。但两条路径的耗时差异是直接可复测事实。

## 设备本地 AppStore 逆向

`/appStore?t=aa54e022` 是当前运行的 AppStore play 提供的薄 UI。play 563 二进制中的内嵌字符串与该页面的标题、路由、API 形状、User-Agent 和上游 origin 完全对应。

### 本地 API 面

页面使用这些设备本地路由：

- `GET /api/cats`
- `GET /api/list`
- `GET /api/one?slug=...`
- `GET /api/featured`
- `GET /api/status`
- `GET /api/installed`
- `POST /api/install`，表单字段为 `slug` 与 `title`
- `POST /api/remove`

详情与变更类路由需要 `t` token。`/api/status` 返回：

```json
{"busy":false,"percent":0,"name":"","slug":"","err":"","hrev":0}
```

安装期间浏览器每秒轮询一次 `/api/status`；空闲时每 5 秒轮询一次。浏览器只看到百分比，看不到 bytes/s、重连次数或服务器阶段。

### Catalog 代理

固件内嵌字符串显示上游端点：

- `https://ai-passport.folotoy.cn/api/plays?multiDevice=false&q=...`
- `https://ai-passport.folotoy.cn/api/plays/%s`
- `https://ai-passport.folotoy.cn/api/plays/recommendations`
- base URL：`https://ai-passport.folotoy.cn`

设备本地 API 先把 catalog 元数据代理给浏览器；用户点击安装后，由设备自己下载选中的 `downloadUrl`。

## Play 563 固件下载实现

该 play 未发布源码（`githubUrl` 为空），所以本节结论来自发布二进制的字符串与 RISC-V 反汇编。

### HTTP 行为

`store_ota` 路径的反汇编显示：

1. 用 `https://ai-passport.folotoy.cn` + `downloadUrl` 拼最终 URL。
2. `esp_http_client` 超时配置为 **20,000 ms**（`0x4e20`）。
3. 打开连接。
4. 要求状态码为 `200`。
5. 以 **2,048 字节**为块读取流。
6. 边读边更新 SHA-256。
7. 按内嵌镜像布局把字节直接写入 flash。
8. 提前 EOF 或读错误直接判安装失败。

配置不发送 `Range`；状态检查只接受完整 body 的 `200`。端点探测也独立确认了 Range 会被忽略。

观察到的配置没有显式调大 `esp_http_client` 接收缓冲。ESP-IDF 5.5.3 中未设置 `buffer_size` 时回退到 `DEFAULT_HTTP_BUF_SIZE == 512`（`esp_http_client.h`）。应用层读缓冲是 2,048 字节。它小于 meta-pass 的 4 KiB buffer/chunk，这进一步说明缓冲大小不是主要速度差来源。

### 镜像处理与校验

二进制是 `esp-merged-0x0` 格式。其 `0x8000` 处的内嵌分区表为：

```text
nvs      0x009000   24K
phy_init 0x00f000    4K
factory  0x010000    3M
otadata  0x310000    8K
cardid   0x356000   16K
ota_0    0x360000    3M
store    0x660000   16K
easter   0x664000  388K
recovery 0x700000    1M
```

下载器在流式读取时捕获 merged-image 的头部/分区表前缀，生成安装计划，把选定范围直接写入 flash，并用 catalog 元数据校验整流 SHA-256。分区表写入最多重试 3 次。失败字符串包括：

- `下载中断`
- `固件下载不完整`
- `固件校验失败（SHA-256 不匹配）`
- `写入 ota_0 失败`
- `写入分区表失败`

没有续传逻辑，也没有多连接重试循环。这是更简单的信任模型：元数据和二进制来自同一个 HTTPS origin，因此 catalog SHA-256 就能认证流。meta-pass 使用每响应 `x-image-sha256`，是因为它的 analyze 与 extracted 是两个独立 Worker 路径。

## 小程序参考安装器对比

开源的 `SHLcy/ai-passport-miniapp-installer` Recovery 安装器不是 play 563，但它实现了同一类模式：

- 小程序直接给出 HTTPS URL
- `timeout_ms = 15000`
- HTTP 状态必须是 `200`
- `Content-Length` 必须等于元数据大小
- 4 KiB 读缓冲
- 流式 SHA-256 校验
- 直接写 flash
- 无 Range、无续传

源码位置：`/tmp/ai-passport-miniapp-installer-src/recovery/main/wifi_install.c:210-298`（review 时克隆）。

这证明 AppStore 生态里的常见快速路径是简单的一次性直连下载，而不是多跳续传协议。

## v50 串口日志解读

用户提供的 v50 / play 675 日志证明了 Range 续传正确性：

- 第一条连接收到 `900,188 / 2,664,256` 字节（33.8%）。
- 随后进入旧的 30 秒读超时阶梯，并以 `ESP_ERR_TIMEOUT` 死亡。
- 下一条连接从 `900,188` 继续，完成剩余 66.2%。
- 没有整包重下。

按日志摘要中的时间戳计算：

- 第一条连接到 900,188 B 的平均速度约 **2.7 kB/s**（约 334s）。
- 最严重的僵尸区间从 t=225s 到 t=373s 只推进约 36 KiB，约 **0.24 kB/s**。
- 续传段移动 `1,764,068` 字节，约 132s，平均约 **13.3 kB/s**。
- 端到端约 **469s**：仍在初始整包请求的 600s 票据窗口内，更远低于 Range 续传的 3600s 窗口。
- 新 TLS 连接重建约 2.4s，所以在死连接上等待约 150s 明显不合理。

当前代码树已经包含针对该僵尸行为的固件侧修复：r10.21 使用两个连续 15s 读超时后重连，并把连接预算提高到 8。日志来自旧的 3x30s 阶梯，因此不要用它重新打开已经修复的超时问题。

## meta-pass 当前仍不合理之处

### 1. 热路径上的网络机制过多

当前固件路径（`main/meta_store_api.c:415-443`）：

1. `analyze` 产生元数据与 600s 票据。
2. `extracted?id&ts&sig` 进入 Cloudflare Worker 路由。
3. Worker 从 edge cache/R2 取内容并输出自定义长度/SHA 头。
4. 固件每条连接都严格校验 `Content-Length` / `Content-Range` / `x-image-sha256`。

这些机制对 meta-pass 的镜像提取与多槽位安全是合理的，但相比 AppStore 的同源直连下载，它明显更慢、失败面更大。下一步应优先处理 serving/path selection，而不是继续加大 ESP 侧缓冲。

### 2. 票据降级是安全的，但不可见

当前 Worker 有两个窗口：初始整包请求用 `DL_TICKET_MAX_AGE_S = 600`，Range 续传请求用 `DL_RANGE_TICKET_MAX_AGE_S = 3600`（`install-slot/_worker.js:137-141`、`297-300`）。票据过期或缺失不是授权失败；请求会降级到旧的现算/edge-cache 路径。固件每个 segment 复用同一个 `ts/sig`（`main/meta_store_api.c:423-431`），所以第一条连接产生 partial 后，长安装仍享有 3600s 续传窗口。

v50 消耗约 469s，因此同时落在初始 600s 与续传 3600s 窗口内。剩余问题是可观测性：票据过期后，安装可能静默失去 R2 快路径并变慢，但固件看不到原因。

可落地方向：保留双 TTL 设计，在响应或 debug 头里暴露 fast-path/fallback 原因。不要把 600s 初始票据误认为整个安装的硬截止。

### 3. 端点没有 fast-origin fallback

固件有从 `206` 安全降级到整包 `200` 的协议 fallback，但没有备用 origin fallback。Cloudflare 停顿时，所有 resume 仍继续走同一路径。play 563 证明，同一 host/网络下直连 nginx 源站可以快一个数量级。

可落地方向：为预提取镜像定义一个可测量的备用 origin，或在信任模型允许时让不可变内容绕过 Worker。这属于架构调整，实施前需要单独确认设计。

### 4. 固件遥测仍不能说明时间花在哪一步

现有串口日志能识别慢读与重连，但不能拆分：

- DNS
- TCP connect
- TLS handshake
- 请求发送
- TTFB
- body 吞吐

host 探测显示 TLS/TTFB 差异已经很大。没有分阶段时间戳，下一份慢速真机日志仍只能靠猜。

可落地方向：在 open/fetch/read 完成处增加单行连接阶段计时。除非确有需求，不增加持久计数器或 UI 功能。

### 5. 继续调 buffer 不是下一个杠杆

meta-pass 已经使用 4 KiB HTTP 接收缓冲和 4 KiB 读块（`main/meta_store_api.c:433-443`、`541-553`）。play 563 只用 2 KiB 应用读块和 IDF 默认 512 B 内部接收缓冲，但直连路径仍快得多。小程序参考实现使用 4 KiB 与 15s 超时。

结论：继续增大 ESP buffer 大概率解决不了当前测得的路径差异。

## 建议的下一步实验

先实验，后改代码：

1. **同设备 A/B 计时**：同一 Wi-Fi 下连续安装一次 play 563 与一次 meta-pass play 675。记录 open、首字节、每 5% 进度、重连与总耗时。
2. **Host 矩阵**：对以下端点分别跑 HTTP/1.1 与 HTTP/2 curl 计时：
   - play 563 直连源站
   - meta-pass `analyze`
   - meta-pass 带票据的完整 `extracted`
   - meta-pass 大 offset 的 `Range`
3. **票据过期测试**：设备开始安装后断网，等初始票据超过 600s 再恢复。预期当前结果：Range 请求仍使用 3600s 续传窗口；超过该窗口后降级到旧链路，而不是授权失败。
4. **Worker 绕过原型**：把一个不可变的预提取镜像放到直连源站或静态 CDN，用同一设备测速。如果吞吐接近 play 563，就能证明端点路径是瓶颈。
5. **r10.21 真机复测**：用当前 build 重跑 play 675。预期不是峰值吞吐变高，而是每条僵尸连接最多浪费约 30s，而不是约 150s。

## 如果后续批准修改，建议顺序

1. 暴露票据 fast-path/fallback 可观测性；保留初始 600s / Range 3600s 的双 TTL。
2. 增加每条连接的阶段计时日志。
3. 选择并验证快速 immutable binary origin / Worker bypass。
4. 最后再考虑固件侧备用 origin fallback 或更激进的超时。

不要为了模仿 play 563 而移除 Range 续传。play 563 能省略续传，是因为观察到的源站快且稳定；meta-pass 的 Cloudflare 路径已经证明会出现停顿，因此续传仍然是必要的。

## 本次 review 已执行的验证

- 查询设备本地 AppStore catalog，将 play 563 解析到 `ai-passport-9`。
- 从托管端点下载 play 563，并用完整 body SHA-256 对齐 catalog 元数据。
- 证明托管端点忽略 Range，并返回完整 `200` body。
- 在同一 host 上测量直连源站与 meta-pass 端点耗时。
- 反汇编发布的 play 563 app 镜像，并检查 `store_ota` 下载路径。
- 与开源小程序 Recovery 安装器做实现对比。
- 未触发设备安装，未修改本地代码。
