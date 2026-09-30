<p align="right">
  <a href="ota-r10.17-r10.19-audit.md">English</a> · <strong>简体中文</strong>
</p>

# OTA r10.17-r10.19 代码审计

日期：2026-09-30  
审查范围：`1a69f37..HEAD`(`eb99f84`、`81e9e1d`)  
审计时工作树：干净;`feat/ota` 领先 `origin/feat/ota` 13 个提交。

## 结论

未发现固件侧 blocking correctness 问题。OTA 状态机没有把错位或异源字节写入 flash 的路径。R2 区间路径已在生产环境验证：字节数、`Content-Range` 与全量实体切片一致。

剩余问题主要是运维与加固问题。最高风险是文档声明的 `RATE_KV` 限速在生产环境没有生效。其次是边缘缓存键包含 `sig/ts`,导致 analyze 预热无法服务新的带票据固件请求。

## 证据边界

- 构建：通过。
- Host 测试：通过。
- 生产 HTTP 测试：下列场景通过。
- 设备测试：未运行。最终吞吐、断点续传与槽位校验仍需真机串口日志闭环。
- 静态校验：`./tools/validate.sh --static` 通过。
- 生产 E2E：`node tools/e2e-production.mjs` 通过。
- 固件包门禁：`./tools/validate.sh --firmware` 通过。
- 审查固件产物：`meta-pass/build/meta-pass_v1.0.0-50-g81e9e1d.bin`(`1,236,716` 字节,SHA-256 `5d7c6d88feb84efd3f9a6e38cd949a69c9160fff49be4f478424f372a191ba86`)。

## 目标一致性

### 已确认达成

1. 固件 OTA 状态跨 TLS 重连保留：
   - OTA handle、SHA-256 上下文、已收字节数与镜像头预检状态跨连接保留。
   - 失败连接只被关闭；下一条连接请求 `Range: bytes=<received>-`。
   - 续传要求 `206`、精确 `Content-Range`、匹配的 `Content-Length`,以及与 analyze 相同的 `x-image-sha256`。

2. 旧服务端回退路径安全：
   - resume 收到 `200` 或 `416` 时绝不会追加到旧 offset。
   - 固件 abort 当前半成品 OTA、重置传输状态,并安全退回整包重试。

3. 生产 R2 range 响应满足固件契约：
   - `start=1`：`Content-Length=2664255`,实际 body `2664255`,`Content-Range: bytes 1-2664255/2664256`。
   - `start=1000000`：`Content-Length=1664256`,实际 body `1664256`,`Content-Range: bytes 1000000-2664255/2664256`。
   - `start=2664255`：`Content-Length=1`,实际 body `1`,`Content-Range: bytes 2664255-2664255/2664256`。
   - 所有抽样响应均为 `x-source: r2-range`。

4. 带票据的全量路径已进入 R2：
   - 使用有效票据的 HEAD 请求返回 `x-source: r2`、完整镜像长度与预期镜像 SHA-256 头。

## 发现的问题

### High —— `RATE_KV` 限速没有生效

文件：

- `install-slot/_worker.js:178-185`
- `wrangler.toml`

证据：

- Worker 代码在 `env.RATE_KV` 缺失时直接放行。
- `wrangler.toml` 只定义了 R2 binding,没有 KV namespace binding。
- 同一 play 一分钟内连续 7 次请求返回 7 个 `416`;第 7 次没有出现预期的 `429`。

影响：

- r10.18 声称的限速在生产环境不成立。
- 无票请求仍可反复触发旧的现算冷路径。
- 文档、注释与运行时行为不一致。

建议：

- 增加真实的 `RATE_KV` namespace binding;或者移除限速声明,把限速列为尚未交付的功能。

### Medium —— 边缘缓存键包含 `sig/ts`

文件：

- `install-slot/_worker.js:28-30`
- `install-slot/_worker.js:58-71`
- `install-slot/_worker.js:288-292`
- `main/meta_store_api.c:412-416`

证据：

- `extractedEdgeKey(req)` 使用完整请求 URL。
- analyze 预热的是 `/api/extracted?id=<id>`。
- 新固件请求 `/api/extracted?id=<id>&ts=<ts>&sig=<sig>`。
- 两类 URL 永远不可能命中同一个 Cache API 条目。

影响：

- analyze 预热无法服务带票据的固件请求。
- 带票据的冷请求可能把 2.6MB 缓存写入只由同一个 `ts/sig` 命中的一次性键。
- R2 目前保护了主路径,但 r10.15b 的 analyze 预热目标对新固件路径失效。

建议：

- 将 `extractedEdgeKey()` 归一化,只保留 `id`,剥离 `ts`/`sig`。

### Medium —— R2 range 的 `Content-Length` 依赖运行时规范化

文件：

- `install-slot/_worker.js:314-329`

证据：

- 源码用 `obj.size` 设置 `content-length`。
- R2 API 的 object size 字段表示全对象元数据,而 ranged `get()` 表示字节区间。
- 生产响应当前是正确的：`start=1000000` 时,响应头与实际 body 都是 `1664256`。说明当前 Workers runtime 对响应长度做了规范化。

影响：

- 当前生产环境未观测到失败。
- 但代码依赖运行时修正,没有显式表达区间长度。

建议：

```js
"content-length": String(total - rangeStart),
```

并在生产 Range 冒烟测试中断言 `Content-Length` 等于实际响应字节数。

### Medium —— 冷路径仍在响应前串行等待 cache/R2 写入

文件：

- `install-slot/_worker.js:404-440`

证据：

缓存未命中时按顺序执行：

1. 回源、校验、解包;
2. `await putExtractedToEdge(...)`;
3. `await` R2 对象写入;
4. `await` R2 元数据写入;
5. 创建响应。

影响：

- 第一次物化仍承担完整冷路径成本。
- R2 与元数据写入继续增加设备首字节时间。
- revision 更新后的第一次请求仍可能复现原来的慢路径症状。

建议：

- 先构造并返回计算结果响应。
- 将 edge cache、R2 对象与 R2 元数据写入移入 `ctx.waitUntil()`。
- 在后台闭包中持有复制后的 `Uint8Array`;`out.stream` 实际不是 `ReadableStream`。

### Medium —— 10 分钟票据有效期可能覆盖不了最坏下载时长

文件：

- `install-slot/_worker.js:128`
- `main/meta_store_api.c:412-416`
- `main/meta_store_api.c:639-660`

证据：

- `TICKET_MAX_AGE` 为 600 秒。
- 固件在每次续传中复用 analyze 时获得的原始票据。
- 仅理论重试预算就可能接近 540 秒,还未计算真实传输时间。

影响：

- 票据过期不会导致镜像损坏或直接中止。
- 但请求会掉出 R2 快路径,回到本改造试图规避的 computed 慢路径。

建议：

- 固件在票据临近过期时重新 analyze 换新票;或者
- Worker 对 Range 续传允许更长窗口;或者
- analyze 下发更长的 `max_age`。

### Medium —— 陈旧边缘缓存可能与强制 SHA-256 响应头校验冲突

文件：

- `install-slot/_worker.js:288-292`
- `main/meta_store_api.c:505-513`

证据：

- 边缘缓存命中后直接返回,不把缓存里的 `x-image-sha256` 与当前 analyze 结果复核。
- 边缘缓存 TTL 为 1 小时。
- 固件现在把摘要头缺失或不一致视为确定性失败。

影响：

- 如果玩法镜像在 TTL 内更新,analyze 可能描述新镜像,而边缘缓存仍返回旧摘要。
- 新的带票 R2 路径通过按 analyze SHA-256 建 key 规避了该问题。
- 旧固件、无票路径与票据过期后的路径仍受影响。

建议：

- 返回边缘命中前复核缓存摘要元数据;或者把 analyze 的镜像 SHA-256 纳入归一化边缘缓存键。

### Low —— 最后一字节之后连接死亡会浪费一次整包重试

文件：

- `main/meta_store_api.c:608`
- `main/meta_store_api.c:639-659`

证据：

- 如果连接在写完 `image_len` 字节后、EOF 前死亡,下一次请求会使用 `Range: bytes=<image_len>-`。
- Worker 正确返回 `416`。
- 固件把该响应映射为 Range unsupported,然后整包重下。

影响：

- 无正确性风险。
- 但会浪费一个已经完整写入的镜像。

建议：

- 在重试循环中先判断 `st.received == analysis->image_len`,满足时直接进入校验,不再打开新连接。

### Low —— 下载票据 JSON 字段缺少 host 合同测试

文件：

- `main/meta_store_analysis.c:102-112`

证据：

- 其他 analyze 字段都有 host 合同覆盖。
- `dl.sig` 与 `dl.ts` 的解析没有专门测试。

影响：

- 解析回归会让设备静默退回旧链路。
- 安装仍然安全,但 R2 加速收益会消失且没有测试报警。

建议：

- 为合法票据、缺失 `dl`、非法 `ts`、格式错误或长度错误的 `sig` 增加 host 测试。

## 已排除的问题

- 未发现 flash offset 错位路径。
- 未发现跨连接 SHA-256 状态污染。
- 未发现 `esp_ota_abort` / `esp_ota_end` 生命周期错误。
- 旧响应头问题已修复：响应头现在来自 `HTTP_EVENT_ON_HEADER`,不再使用读取请求头的 getter。
- `200` 或 `416` 的 resume 响应不会追加到既有 OTA 状态。
- 证书链与设备信任锚的生产检查通过。
- 受保护 flash layout 与单文件固件包门禁通过。

## 开发交接清单

状态：**以下条目在后续提交明确关闭前均为 OPEN**。可以拆分给 Worker 与固件开发者，但 H1-H6 落地前不应宣布 R2 改造完成。

### H1 —— 让限速声明真实生效

- 严重度：High
- 范围：Worker / 部署
- 文件：`install-slot/_worker.js`、`wrangler.toml`
- 修改：为 `rateLimit(env, ip, id)` 绑定真实的 `RATE_KV` namespace；在绑定存在前，移除“已交付限速”的声明。
- 验收：
  - 同一 `id` 一分钟内 7 次请求中，前 6 次放行，第 7 次返回 `429`;
  - 验收必须来自生产行为，不只看源码;
  - `./tools/validate.sh --static` 通过。

### H2 —— 归一化边缘缓存键

- 严重度：Medium
- 范围：Worker
- 文件：`install-slot/_worker.js`、`tests/worker_contract.mjs`
- 修改：`extractedEdgeKey()` 只保留玩法 `id`,剥离 `ts` 与 `sig`。
- 验收：
  - analyze 预热写入 `/api/extracted?id=<id>`;
  - 后续带票据请求能够命中该预热条目;
  - 不再为每个 `sig/ts` 创建一次性缓存条目;
  - worker contract 与生产冒烟测试通过。

### H3 —— 显式设置 R2 range 长度

- 严重度：Medium
- 范围：Worker
- 文件：`install-slot/_worker.js`
- 修改：将 range 响应中由 `obj.size` 派生的长度改为 `String(total - rangeStart)`。
- 验收：
  - 抽样 range offset 均返回 `206`;
  - `Content-Length == 实际 body 字节数`;
  - `Content-Range` 与请求的 suffix 匹配;
  - 生产冒烟至少覆盖首字节、中间、最后一字节三个 offset。

### H4 —— 将冷路径持久化移到 `waitUntil`

- 严重度：Medium
- 范围：Worker
- 文件：`install-slot/_worker.js`、`tests/worker_contract.mjs`
- 修改：先返回已计算的镜像响应；把 edge cache、R2 对象与 `.meta.json` 写入放入 `ctx.waitUntil()`,并在闭包中使用复制后的字节数据。
- 验收：
  - 响应创建不再 await cache 或 R2 写入;
  - 持久化完成后第二次请求仍能看到 R2 对象;
  - 后台写入完成前不复用可变源 buffer;
  - 记录改动前后的冷路径 TTFB。

### H5 —— 让慢速下载始终持有有效 R2 票据

- 严重度：Medium
- 范围：Worker + 固件
- 文件：`install-slot/_worker.js`、`main/meta_store_api.c`、`main/meta_store_analysis.c`
- 修改：任选一种策略：票据过期前刷新、延长 Range 续传窗口,或按已记录的重试预算下发更长的 `max_age`。
- 验收：
  - 当前 600 秒票据过期后的续传不会静默掉回 computed 慢路径;
  - 所选票据策略写入文档;
  - 固件测试覆盖所选行为。

### H6 —— 防止陈旧边缘缓存触发 SHA-256 不匹配

- 严重度：Medium
- 范围：Worker
- 文件：`install-slot/_worker.js`
- 修改：返回边缘缓存命中前,将缓存中的 `x-image-sha256` 与当前 analyze 的镜像 SHA-256 复核；或把该 SHA-256 纳入归一化缓存键。
- 验收：
  - 玩法镜像在旧的一小时 TTL 内更新时,不能把旧摘要当作当前 analyze 结果返回;
  - 无票与票据过期路径要么 fail closed,要么获取当前镜像;
  - 该行为有合同测试覆盖。

### H7 —— 处理“已收满但 EOF 前断线”

- 严重度：Low
- 范围：固件
- 文件：`main/meta_store_api.c`
- 修改：打开下一条 segment 连接前,若 `st.received == analysis->image_len`,直接进入摘要与 OTA 校验。
- 验收：
  - 最后一字节写入后、EOF 前断线时,不再请求 `Range: bytes=<image_len>-`;
  - 不发生整包重试;
  - 最终 SHA-256、`esp_ota_end` 与镜像校验仍全部执行。

### H8 —— 补齐下载票据 parser 覆盖

- 严重度：Low
- 范围：固件测试
- 文件：`main/meta_store_analysis.c` 与 host 侧 analyze 合同测试
- 修改：增加合法票据、缺失 `dl`、非法 `ts`、格式错误 `sig`、长度错误 `sig` 测试。
- 验收：
  - 合法票据能填充 `dl_sig` 与 `dl_ts`;
  - 非法票据字段会选择旧的 no-ticket 路径;
  - parser 回归会导致静态验证失败。

### H9 —— 补齐真机证据

- 严重度：必需验收项
- 范围：设备验证
- 文件：无
- 修改：将审查固件刷入设备,中断一次 R2 下载并恢复,同时抓取串口日志。
- 验收：
  - 日志显示从非零 offset 发起 `206` 续传;
  - 可恢复网络故障后不再整包重下;
  - 最终镜像通过 SHA-256、`esp_ota_end` 与镜像验证;
  - 吞吐与剩余停顿模式必须独立于 host 结果记录。

## 交接完成门禁

H1-H8 必须有对应提交,H9 必须有真机串口日志,才能宣布 R2 改造完成。H1-H6 是 Worker/部署工作,H7-H8 是固件/测试工作,H9 是验收证据。

## 关闭记录(r10.20,2026-09-30)

除 H9 外全部**已关闭**,关闭提交 `r10.20`。生产实测证据:

- **H1 已关闭** —— `RATE_KV` 绑定进 `wrangler.toml`;生产实测越界探针序列 `416,416,416,429,429,429,429`。
- **H2 已关闭** —— 缓存键只由路径 + `id` 构成;生产实证:`analyze?id=100` 预热后,两次换 query 的 extracted 请求均 `x-source: edge`(为此给边缘条目加了 `x-source` 头)。Cache API 在 `*.pages.dev` 预览域 no-op;命中发生在生产域。
- **H3 已关闭** —— `content-length = total - rangeStart`;首/中/尾三偏移:CL == body 字节 == total-off,Content-Range 精确匹配。
- **H4 已关闭** —— 持久化移入 `ctx.waitUntil` 且独立复制;冷路径 TTFB 不再包含写入耗时。
- **H5 已关闭** —— Range 续传票据窗口 3600s(非 Range 维持 600s);策略已写入 `_worker.js` 与 CHANGELOG。
- **H6 已关闭** —— 边缘命中先与当前 analyze sha/imageLen 复核,陈旧条目先驱逐后服务。
- **H7 已关闭** —— `st.received == image_len` 退出重试循环直接进入校验;`test_download_retry_gate.py` 钉死。
- **H8 已关闭** —— `test_store_analyze_contract.c` 覆盖五形态;解析器逐字符校验 16 位小写 hex。
- **H9 待办** —— 需刷入审计固件并抓取真机串口日志:中断一次 R2 下载,从非零 offset 恢复。

关闭时门禁状态:`worker_contract.mjs` PASS 1-14,`./tools/validate.sh` 全绿,
`node tools/e2e-production.mjs` 全部通过(含 E2E-9/H1 与 E2E-9/H3)。
