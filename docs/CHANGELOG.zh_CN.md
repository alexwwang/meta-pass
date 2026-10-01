<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## Unreleased

- **v3.2-r10.22 (2026-10-01) LAN 手机辅助安装 —— 设备端净切(`feat/mota`)**:商店下载
  通道撤出设备。启动器改为展示 QR 页(120px `lv_qrcode` + IP + 6 位配对码,文本回退),
  附一次性 token;手机端网页模块扫码后完成市场搜索/analyze/下载/镜像提取,经 LAN
  HTTP 把应用镜像上传到设备。设备端:`meta_store_install.{c,h}` —— 80 端口本地安装
  HTTP 服务(prepare/session/chunk/finalize/status/cancel/pair + boot 页,与配网门户
  生命周期互斥),128-bit 一次性上传 token、强制 `X-Meta-Session`、同源 Origin 校验、
  4KB 流式 chunk 写入 + SHA-256 + `esp_image_verify`,上传先于物理槽位确认不放行,
  失败路径槽位标 INVALID;`meta_install_model.{c,h}` —— 纯逻辑
  manifest/session/chunk/finalize 判定,新增 host 测试 `tests/test_meta_install_model.c`
  钉死。删除:数字玩法 ID 键盘页、ID 编辑模型、analyze 解析/客户端与详情页
  (`meta_store_{api,api_fail,analysis,info_page,idedit,range}`)、WAN 用的
  TLS/SNTP/证书包依赖(`esp_http_client`、`esp-tls`、`main/certs/`、SNTP 配置)及其
  门禁/工具(`test_http_contract.py`、`test_download_{speed_config,retry_gate}.py`、
  `e2e-production.mjs`、`verify-crt-bundle-match.py`、`e2e-feed-fixture.c`)。
  `sdkconfig.defaults` 启用 `LV_USE_CANVAS`/`LV_USE_QRCODE`(qrcode 基于 canvas 类
  实现);lwIP 32KB 窗口保留 —— LAN 上传与当年 WAN 下载同样受益。手机端模块
  (`install-slot/`)与 Worker(`_worker.js`)本期不动;`tests/worker_contract.mjs`
  继续钉死 Worker API 面。

- **v3.2-r10.21 (2026-09-30) OTA 僵尸连接快速判死**:r10.17-r10.19 栈的首次真机运行
  (v50,675)端到端证明了续传 —— `install resume 2/6 at 900188/2664256`,2.4s TLS
  重连,无整包重来,镜像校验通过 —— 但第一条死连接在宣告死亡前烧了 ~150s:3x30s
  EAGAIN 阶梯 + 20-29s 慢读(t=225s 到 t=373s 只推进了 36KB)。连接进入僵尸态
  (TCP 未关但无数据)时,吊着等严格劣于换线重连:断点续传只花 ~2.5s 握手,且已写
  字节全保留。因此 install 每读超时 30s 收到 15s(`DL_READ_TIMEOUT_MS`),连续
  EAGAIN 容忍 3 收到 2(僵尸判死最坏 30s,此前 ~150s),连接预算 6 提到 8,把省下
  的时间花在重连上。analyze 维持 30s(单次小响应,无续传语义)。
  `test_download_retry_gate.py` 钉死。同一轮运行的背景:链路爆发到过 67kB/s 而
  均值只有 5.7kB/s —— 停顿在路径/WiFi 侧,不在服务端(R2 对 host curl 吐
  ~630kB/s);固件修不了链路,只能保证持续前进,这正是快速判死 + 续传做的事。

- **v3.2-r10.20 (2026-09-30) 审计 H1-H8 关闭**:r10.17-r10.19 代码审计的九项交接
  全部处理(H9 = 真机证据,按设计保持待真机)。H1 限速真实生效:`wrangler.toml`
  绑定 `RATE_KV` KV namespace(审计发现计数代码存在但从未绑定 → 生产从未回
  429);生产实测 —— 越界探针序列 `416,416,416,429,429,429,429`。H2 边缘缓存键
  只含玩法 id(剥离 ts/sig),analyze 预热从此能服务带票固件请求,不再按
  (ts,sig) 堆一次性 2.6MB 条目;生产实测 —— `analyze?id=100` 预热后,换 query 的
  extracted 请求连续两次 `x-source: edge`(为此给边缘条目加了 `x-source` 响应头)。
  注:Workers Cache API 在 `*.pages.dev` 预览域是 no-op(无 zone),缓存命中只发生
  在生产自定义域。H3 区间响应显式置 `content-length:
  total - rangeStart`,不再依赖运行时对 `obj.size` 的规范化;首/中/尾三偏移实测
  `CL == body == total-off`。H4 冷路径持久化(边缘缓存/R2 对象/.meta.json)移入
  `ctx.waitUntil` 且各自持独立 buffer 副本 —— 设备先拿到首字节,再等任何一笔
  2.6MB 写;`x-r2-write` 头随之取消,写入结果记入后台日志。H5 Range 续传请求按
  3600s 票据窗口受信(600s 盖不住慢链路续传;非 Range 请求维持 600s;r2check
  加 `?range=1` 诊断)。H6 边缘缓存命中先与当前 analyze sha/imageLen 复核、不符即
  驱逐 —— 固件对摘要头不一致是确定性失败,绝不拿旧字节冒新声明。H7 已收满
  镜像直接退出重试循环进入校验(最后一字节后断线不再浪费一次
  `Range: bytes=<image_len>-` 探测与整包重下);`test_download_retry_gate.py`
  钉死。H8 dl.sig/dl.ts 解析获得五形态 host 覆盖(合法/缺失/ts=0/非 hex/错长),
  且解析器逐字符校验 16 位小写 hex,残缺票据不再进 install URL;
  `test_store_analyze_contract.c` 钉死。门禁:`worker_contract.mjs` PASS 12/13/14
  钉键归一化+新鲜度复核、后台化持久化、RATE_KV 接线;`e2e-production.mjs`
  E2E-9 增加 H1/H3 生产验收。
- **v3.2-r10.19 (2026-09-30) Worker 续传(206),激活固件 r10.17**:worker 现以
  `206` + `Content-Range: bytes <start>-<total-1>/<total>` 应答固件续传请求
  (`Range: bytes=<start>-`),把 r10.17 出厂时休眠的断点续传路径真正激活(此前固件
  对续传收到 200 会安全降级为整单重试)。只接受固件会发出的 suffix-range 形态;
  其余 Range 形态按 RFC 9110 忽略(回 200 全量),越界 start 回 416(固件作废 OTA
  会话整单重来)。Range 请求绕过边缘缓存读、也绝不写入 —— 缓存键不含 Range 且恒存
  200 全量体,206 绝不能落进去。两条供给路径:有票走 R2 原生区间读
  (`x-source: r2-range`,不在 isolate 内物化整对象),其余对 analyze 缓存的
  `Uint8Array` 切片(`x-source: computed-range`) —— 票据仍是 R2 专属钥匙,下载中途
  票据过期依旧不可能弄断续传(永不拒绝语义不变)。206 携带全镜像
  `x-image-sha256`(续传完成后设备校验完整镜像),且先于任何缓存/R2 写路径返回。
  `worker_contract.mjs` PASS 11 直接执行 `parseSuffixRange`(恰好接受 `bytes=<n>-`,
  其余形态拒绝),钉死固件同构 Content-Range 模板、恰好两条 206 路径、双双 416、
  206 先于写路径的顺序。675 生产实测:206 + `x-source: r2-range`,切片字节与全量体
  同偏移逐位一致,全镜像 SHA-256 等于 analyze 声明值,无票续传降级不被拒绝,
  畸形 Range → 200,start≥total → 416。
- **v3.2-r10.18 (2026-09-30) R2 物化 + HMAC 下载票据**:解包产物现物化进 R2
  (`extracted/<id>/<storeFwSha256>.bin` + `.meta.json` 审计映射),有票请求直接 R2
  直出(`x-source: r2`),彻底消除每 isolate 冷启动回源这一段(残余的数秒级 TTFB
  停顿来源)。票据是 R2 专属钥匙而非准入门槛(定稿规则):analyze 签发
  `sig = HMAC-SHA256(DL_SECRET, id:ts)` 截 16 hex 字符;有效票解锁 R2 读/写路径,
  无票/坏票/过期票一律降级老链路(边缘缓存+现算) —— 旧固件(不带 sig)零成本兼容,
  任何票态都不会被 403 拒绝。设备端解析 analyze 可选的 `dl.sig`/`dl.ts` 并在
  install URL 回带。限速(每 IP+id)在票据校验前对两条链路无条件生效。两个生产 bug
  由实测定出并已入门:截断在 hex 化之后(且一度宽度也错),导致每张票 32 字符、
  永远 invalid(生产实测 `x-r2-write: skipped`) —— `worker_contract.mjs` PASS 10
  直接执行 `hmacHex16` 断言输出 16 字符,PASS 9 钉死 R2 门控/永不拒绝语义。生产
  端到端实测:有票首请求物化(`x-r2-write: ok`),再请求 `x-source: r2`,各路径字节
  sha256 全部等于 analyze 声明值(`40de1562…`)。部署注记:Bun 的 `node` 包装器下
  wrangler 4.80/4.143 在第一个 API 请求后静默死,部署必须用真 Node
  (`PATH=/usr/local/bin:$PATH`);`DL_SECRET` 为 Pages 生产 secret,不入仓库。
- **v3.2-r10.17 (2026-09-29) OTA 断点续传 + 响应头信任链修复**:固件下载不再因可恢复读错误从 0 字节整单重来。OTA/SHA-256 状态跨 TLS 重连保留;下一条连接发送 `Range: bytes=<received>-`,要求 `206` 与精确 `Content-Range`,只有最终失败/取消或校验失败才 abort OTA。旧的三次整单重试改为六条分段连接(1..5s 退避);旧服务端对续传回 `200` 时绝不把全量流追加到旧 offset,而是安全降级为原整单重试。同步修复信任链静默空操作:IDF 5.5.3 `esp_http_client_get_header()` 读取*请求*头,旧 `x-image-sha256` 响应头比对从未真正执行;现在通过 `HTTP_EVENT_ON_HEADER` 捕获响应头,摘要缺失/不一致或 `Content-Range` 非法会在继续写 flash 前失败。新增零 IDF 依赖的 `meta_store_range.[ch]` host 测试,并重写 `test_download_retry_gate.py` 钉住 EAGAIN、200/206 区分、旧服务端回退、OTA 状态生命周期与请求头/响应头 API 陷阱。`sdkconfig.defaults` 现在钉住 r10.16 的 WiFi 动态 RX 缓冲数(48),门禁同时拒绝本地 `sdkconfig` 中 RX buffer 与 `CONFIG_MBEDTLS_SSL_RENEGOTIATION=n` 漂移。CF worker 按 R2 改造节奏暂不修改;服务端一旦输出 206/Content-Range,新续传路径自动启用。
  OTA begin/write 失败现在显示 `Flash operation failed.`,不再误报为普通下载失败。

- **商店下载改由边缘直出，不再每次请求现场渲染（r10.15，服务端）**：v44 真机日志显示安装以 ~4kB/s 龟速爬行并伴随 502 与 30s 级 TTFB 停顿，而同一分钟 curl 拉同一产物只需数秒 —— Pages worker 每次缓存冷启动都在现场回源 folotoy.cn 拉 3MB 合并镜像（再校验、再解包），而回源这一跳本身就是被节流的慢链路。参照项目之所以快正是这一课：它的设备下载的是静态固件 URL，从不做每请求现算。现在 worker 把两笔回源产物放进 CF Cache API（合并镜像按 URL 缓存 1h；玩法元数据缓存 60s，revisionId 复核窗口有界），统一走 `cachedOriginFetch` 层 —— analyze（P2 详情页）与 extracted（下载）共用，浏览详情页即预热边缘，点安装时直接从缓存流式吐字节。信任链未动：每次冷构建仍强制校验商店公布的 SHA-256 后才解包，缓存字节永远绕不过校验。回归门：`worker_contract.mjs` PASS 8 钉死缓存层、双 TTL 与无条件信任链。部署提示：CI 只从 `main` 部署 worker —— 合并（或手动 `wrangler pages deploy`）后设备路径才生效。
- **安装不再死于服务端第一次停顿(r10.13)**:v43 真机日志证明吞吐配置已生效(0% 时
  571kB/s),但下载在 45KB/2.6MB 处死掉,每次读停 10-30 秒并以 `errno=11` 告终 ——
  同一分钟同端点用设备 UA 的 curl 却是 3.3s/16.3s/4.4s,ping 0% 丢包。商店以
  `cf-cache-status: DYNAMIC` + `no-store` 下发这个确定性产物,Cloudflare 每次请求
  都回源现生成,源站间歇性停顿;Range 续传不支持(返回 200 全量)。IDF 读超时返回
  `-ESP_ERR_HTTP_EAGAIN` 且连接仍存活 —— 旧代码把一切负值当致命错。现在:EAGAIN
  续读(有界,连续 3 次才判死);其他读错误或 EOF 截断则 abort OTA 并用全新 TLS
  连接整单重试(3 次,退避 1s/2s,尊重取消);长度/摘要契约违约仍是确定性错误,
  照旧直接报错。回归门:`tests/test_download_retry_gate.py`。
- **商店下载提速:拆掉三个叠加的默认限制(r10.12)**:下载稳态只有几十 KB/s,是三个
  独立上限叠加的结果,现已逐一拆除。WiFi 调制解调器省电从未关闭(IDF 默认
  `WIFI_PS_MIN_MODEM` 在 DTIM 信标间休眠射频)—— STA 与 APSTA 两条启动路径均加
  `esp_wifi_set_ps(WIFI_PS_NONE)`。lwIP TCP 接收窗口仅 5760B(4×MSS 默认),在到商店
  主机 ~100ms RTT 下吞吐封顶 窗口/RTT ≈ 57KB/s —— 现改 65535(无缩放上限;
  `LWIP_WND_SCALE` 依赖 PSRAM,C3 没有),发送缓冲同步放大,tcpip 收包邮箱 32→64。
  下载块 1KB,每 KB 都付一次 read+sha256+`esp_ota_write` 调用开销 —— 现改 4096
  (flash 页大小,样例参照实现真机验证值)。CPU 160MHz、flash 80MHz DIO、WiFi
  AMPDU TX/RX、硬件 AES 原本就已就位。新门 `tests/test_download_speed_config.py`
  全部钉死;r10.10 的每 5% 串口遥测可直接给真机前后对比。
- **开机槽位扫描不再为半成品镜像打 bootloader 格式错误日志(r10.11,BUG-21)**:启动 USB monitor 会复位芯片(USB-Serial-JTAG 硬复位,monitor 默认行为——用 `idf.py monitor -- --no-reset` 或 `ESP_IDF_MONITOR_NO_RESET=1` 可保持设备运行),复位后的开机扫描用 `esp_image_verify(ESP_IMAGE_VERIFY, …)` 校验失败下载留下的半成品槽位——该非静默模式下,IDF 段表遍历读到擦除态 0xFFFFFFFF 段头即打 `invalid segment length 0xffffffff`(ESP_LOGE)。拒绝本身始终正确:槽位状态为 INVALID,且仍可覆盖安装。现扫描改为静默模式,并打一行写明原因与处置的 WARN。回归覆盖:`tests/test_meta_store_scan.c`(host fixture 复现 IDF 段表遍历;empty/INVALID/VALID 三态 + 擦除恢复)与 `tests/test_bug21_scan_silent.py`(静默调用静态门,对照 IDF 源码验证);`tests/test_http_contract.py` 已挂进 `tools/validate.sh`(r10.8 起一直只手动运行)。
- **商店 UX 修正 + 下载遥测(r10.10)**:安装页行动行改标 CONTINUE(原 CONFIRM,
  语义是继续去选槽位);槽位选择页信息窗加倍高度、底色弱化,不再与按钮行混为
  一排,并显示玩法名与 KB;不适配槽位改为一行短标签 "SLOT n too small" 并置灰
  (旧 "TOO SMALL (will erase)" 超宽被截成乱码样)。下载循环每 5% 打点吞吐,
  read 失败时串口打印已收/应收字节与上一读阻塞时长 —— 真机 ~13% 卡住再失败
  这类现象,串口直接报位置与耗时,不再是裸 "Download failed"。host 吞吐基线
  实测 0.8-1.3 MB/s,快后慢的形态属设备侧射频/链路,服务端无节流;固件每
  1KB 直写 flash,不存在缓存层。
- **警告页 CONFIRM 行回归(r10.9,BUG-20)**:真机首次走到 supported analyze 结果
  (此前 BUG-18/19 把所有握手拦死在 P2 之前),custom-partitions 警告页却只渲染
  一个 BACK 行 —— 设计的"警告+确认可装"落空。r10.4 的渲染器丢了 supported
  分支而 OK 路由保留着(行 0 甚至标着 BACK 却按 CONFIRM 响应)。三形态现收成
  单一纯逻辑模块(`meta_store_info_page`),渲染与路由共用,
  `tests/test_meta_store_info_page.c` 钉死;重填时清理残留第二行。
- **HTTP 状态码读错 API 已修(r10.8,BUG-19)**:与样例恢复安装器
  (ai-passport-miniapp-installer,真机验证)交叉比对暴露 ——
  `esp_http_client_fetch_headers()` 返回的是 Content-Length 而非状态码;我们的
  analyze/install 拿返回值与 200 比较,真机上 analyze(477B)全灭于 "HTTP 477"、
  install(1.9MB)全拒于长度检查,与 TLS 是否修好无关。现在两处状态码一律来自
  `esp_http_client_get_status_code()`;EOF 后强制 `received == content_len`;
  设备 User-Agent 单源化并被 E2E 复演;禁自动重定向(301 不再能重锚长度契约);
  接收/头部缓冲 1024→4096;TLS 重协商显式关闭;网络作业栈 8192→10240(样例
  验证值)。新门 `tests/test_http_contract.py` 钉死 IDF 契约、桩签名与全部加固
  字段;host 桩同步真实 `int64_t` 契约。
- **链尾 issuer 缺失已修(r10.7.2,BUG-18)**:v35 已加 GTS WE1 中间证书,真机却仍以
  一字不差的 `No matching trusted root certificate found` / `-0x3000` 握手日志失败。
  新的静态复现(`tools/verify-crt-bundle-match.py`)用裸 ASN.1 解析器对实发链逐字节
  重演 `esp_crt_verify_callback` 的逐层 issuer 查找,钉死真因:服务器实发 3 张链,
  末尾是 cross-signed 的 GTS Root R4,其 issuer(GlobalSign Root CA)不在包里 ——
  mbedTLS 走每一层,不只 leaf。修复:`main/certs/globalsign-root-ca.pem`(subject
  与链尾 `issuer_raw` 逐字节相等;`openssl verify` 闭环全链)。新增 E2E-8d 每次运行
  跑字节级设备查找模拟 + 陈旧镜像守卫(最新 `build/meta-pass_v*.bin` 必须原样内嵌
  当前 bundle);E2E 计数 17→19。三层现已全部命中:chain would VALIDATE on device。
- **真机轮 r10.7(v1.0.0-33)**:RETRY 不再把用户踢回清空的输 ID 页(BUG-15:
  忙/离线时只提示并停留;键盘用 host 测试过的 `mpd_idedit_set_digits` 预填
  上次确认的玩法 ID);P0 冻结的 "timeout in 300s" 换成真实状态句(BUG-16);
  OPEN 传输失败屏上直接报死因 —— DNS failed / Connect timeout / Connection
  refused / Cert check failed(BUG-17,事件回调捕获 + `meta_store_api_fail_open_text`)。
- **进过一次商店后列表页全废已修(r10.6,BUG-14)**:P1 键盘面板数组 `s_keys`
  仍是 r8 十键时代的硬编码 `[10]`,而 r10 键盘已是 15 键 —— 每次进入 P1
  (ONLINE 后 2 秒自动发生)越界写 5 个指针,正好砸进 `.bss.s_rows` 与
  `.bss.s_slots` 头部(链接 map 实证),列表页从此无法高亮 STORE DOWNLOAD、
  按键失灵。  数组尺寸改为与 `MPD_KEY_COUNT` 单源,main 组件启用 `-Werror` ——
  自 r10 起就躺在构建日志里的两条 `iteration 10 invokes undefined behavior`
  警告再也不可能被无视。净室重建 v1.0.0-31:两条警告消失,validate 112 项 +
  固件门全绿。流程教训归并为十条(r9–r10.6,见 `docs/BUGS.md`),
  其中七条升格为 `AGENTS.md` 可强制执行的基线规则。
- **analyze 传输失败分类(r10.2,v1.0.0-23)**:长期存在的"真机一切玩法都显示
  unavailable"类失败现在屏上可诊断 —— analyze 传输失败按阶段 × 时钟状态分类,
  经新纯逻辑模块 `meta_store_api_fail`(host 测试):`TLS failed (clock
  unsynced).`(真机杀手:SNTP 未同步 → mbedTLS 证书时间校验必败)、
  `TLS/DNS failed.`、`No response.`、`Connection lost.`、`Bad response from
  server.`、`Server error <码号>`;业务 reason 码原样透传;P2 逐字展示并给
  RETRY 行。另:响应超限路径不再残留旧 reason;P1 提示文字回到一行;P0 改网
  手势 = 600ms 内双击 UP(误按安全,host 测试 `meta_prov_upclick`)。
- **商店页面流修复 + P1 键盘打磨(r10,v1.0.0-19..21)**:① 页面流显式化且无环 ——
  OK LONG 从每个商店页退出到列表页,P0 ONLINE 自动翻页被任何按键取消(此前 P1 的
  OK LONG → P0 而 P0 又自动进 P1 → 死循环,真出口只有 2s 窗口);P0 新增可见的
  `> CHANGE WIFI (OK=confirm)` 行(UP/DOWN 切换)。② P1 UP/DOWN 短按 = 选中键环移
  (r9 错映射到移光标——移的是看不见的插入点,短按导航全废);屏上 ◀▶ 移插入光标;
  `test_ring_navigation` 钉死。③ P1 几何修正(ID 面板 100→84,键盘不再压进面板;
  OK 高 58→64,下缘与 0 键对齐)并以 `_Static_assert` 锁死。④ 槽位措辞:
  `(invalid)` → `(no firmware)`,单一事实源(`meta_slot_list_word`/
  `meta_slot_detail_word`,host 测试)——EMPTY = 已擦除,NO FIRMWARE = 有数据但非
  可引导(ota_2 littlefs 录音);两者都可直接覆盖安装(esp_ota_begin 先擦除);
  详情页改 `Install overwrites it.`(原 `Delete it and re-install.`)
- **商店配网加固 + 政策修正(r9,v1.0.0-14..18)**:六条真机失效链逐一定根因并修复,
  外加正式站 E2E 证据工装。① **配网扫描/连接死锁**:`/api/scan` 原本把「起扫」(网络
  任务)与「取结果」(HTTP handler)拆在两处,且结果为空时 handler 不调
  `esp_wifi_scan_get_ap_records` 就返回——ESP-IDF 中「扫描完成但记录未取」会让驱动
  停在残留态,同时卡死下一次扫描与 `esp_wifi_connect()`,表现为扫描列表冻结、凭证
  提交后吃满 30s 死线。现改为 handler 内同步完成(起扫→有界等待→无条件取记录),
  `pmf_cfg.capable=true`(兼容 WPA2/WPA3 混合路由),断连原因码上屏(`密码错误? /
  AP 未找到,重扫 / 认证失败:密码/PMF? / 握手超时`,新 `meta_store_wifi_fail_text`)。
  ② **清除一个自伤回归**:同周期加过的「每秒兜底重发 `esp_wifi_connect()`」反而阻断
  关联——IDF 对连接中重调的处理是断开重连,每次重调都会重启 connect 的内部信道扫描;
  事件位是粘性的,重试纯属伤害。由延迟执行的静态分析轮(F1–F4)抓到,同轮还把作业栈
  6144→8192(mbedTLS 峰值)、WiFi teardown/start 收敛到作业任务单点、P0 RESET 预选
  收窄到 ERROR 态。③ **面板冻结根因**:300s 会话浮层在配网期间上弦(此时
  `busy=false`)且触发后 `store_tick` 直接 return——面板冻在 "Syncing clock..." 而
  timeout 标签照常倒数;现配网 AP_UP/CONNECTING/ERROR 期间不再触发浮层,面板实时
  刷新已耗时秒数,SNTP 主服务器改 `ntp.aliyun.com`(5s 上限)。④ **ONLINE 自动翻页
  重新锚定**:`s_net_online_at` 在建页时取样(建页发生在 CONNECTING 时为 0),2s 翻页
  永不触发且 ONLINE 无刷新分支;tick 现在自己检测 ONLINE 跳变。配网页文案不再谎称
  "saved!"(改为 "Received…" 并指向设备屏幕)。⑤ **自定义分区政策修正**:市场玩法
  开始携带额外数据分区(`easter` 0x82、voicefs 类 0x81),r8 的硬拒规则把半个市场
  锁在门外(一天内即被生产基线检查抓到);现所有白名单外的*数据*分区一律警告放行
  并带 `detail=<label>`(实际只装解包出的 factory 应用——分区载荷从不进设备),由
  分析器 PASS 5/5b/5c 与固件合同测试钉死。⑥ **P1 键盘按 TDD 重建**:编辑模型抽出为
  纯逻辑 `main/meta_store_idedit.{c,h}`(host 测试,真机同一份代码),4×4 布局——
  1-9/0 数字、DEL/CLR 独立列、`◀ 0 ▶`、OK 纵跨两行——插入光标编辑带退格,长按=
  换行,显式提交;测试当场抓到首次布局引入的数字映射错误。**证据工装**:
  `tools/e2e-production.mjs` 对正式站跑 14 项检查(analyze 契约、extracted
  大小/sha/magic 三重校验、真实响应字节反喂设备 C 解析器、叶子证书 issuer 对设备
  GTS Root R4 锚、服务端↔固件 reason 码对齐)——正是抓到政策回归的那套检查;
  `tests/worker_contract.mjs` 钉住 Pages worker API 面。factory 预算
  ~1,147,000/1,507,328 B(约 24% 余量);产物 `meta-pass_v1.0.0-18-g0fd6194.bin`
  及后续。
- **商店体验 + 自定义分区策略(r8)**:四项真机修复。① **按需扫描**:r7 的 3s 周期后台
  扫描在 SoftAP 期间反复切信道,配网 beacon 出现空窗,手机根本搜不到热点;`/api/scan`
  改为触发一次扫描并等待至多 2.5s(`PROV_SCAN_WAIT_MS`),其余时间 AP beacon 不被打断。
  ② **P1 ID 输入重建为屏上数字键盘**,ID 变长(1–7 位,与服务端 `\d{1,7}` 一致):
  0–9 两行键 + 高个 GO 键 + CLR 条,UP/DOWN 移动选择,OK 追加,第 7 位自动提交,
  GO 在 ≥1 位时提交;固定位数高位补零废弃(空时显示 "ID: -")。③ **P0 新增
  "Reset WiFi"**:NVS 凭证自动重连是预期默认,但存错网络原本无解——UP/DOWN 选中
  "> RESET WIFI (OK=confirm)",OK 擦除 `sta_ssid`/`sta_pass` 并重启热点(新接口
  `meta_store_net_reset_wifi()`);ONLINE 后停留 2s 自动进 P1。④ **玩法 675
  "unavailable" 根因**:其 `rec` 分区是 subtype 0x40 的自定义数据分区(type=1),
  被分析器硬拒规则判为不可装,且线上部署的服务端仍是旧版(`/api/analyze` 全量 404,
  设备把一切非契约响应渲染成 "unavailable"——本次排查的可见触发点)。分析器改为对
  subtype 0x40 自定义数据区**警告放行**(`supported=true`,reason=custom-partitions,
  `detail=<label>`;subtype ≠ 0x40 才硬拒),analyze 契约新增 `detail` 字段,槽位页
  渲染 "NOTE: custom 'rec' part / not installed; some features may lack it" 且仍可
  CONFIRM 安装。  **部署**:生产 worker(`install-slot/_worker.js`)现实现完整设备通道
  （analyze/extracted 走同一份 `store-analyze.js` + WebCrypto SHA-256；该模块
  依 BUG-03 单一副本原则从 `tools/install-slot/` 移入 Pages 部署根），发布方式 =
  推送 main（CI `wrangler pages deploy`）。此前线上 worker 完全没有商店通道 ——
  `/api/analyze` 全量 404。同周期追加契约修复:服务端对不可装玩法回
  `name:null`/`extracted:null`(sha256 惰性计算),而固件解析器把 `name`/`extracted`
  当恒必填,too-large/wrong-chip/not-found 会被吞成 "format" 上屏;现改为仅
  `supported=true` 时才要求 `name`/`extracted`(新增 host 合同测试
  `tests/test_store_analyze_contract.c` 锁定 —— 与真机链接同一份
  `meta_store_analysis.c`,并以本地服务端真实响应喂入验证)。r8 后 factory 预算:
  1,146,544/1,507,328 B(约 24% 余量);发布产物 `meta-pass_v1.0.0-8-g2595d26.bin`
  (1,212,124 B 含 MPUPV2 尾段),校验门
  4/4 PASS。
- **商店下载通道(feat/ota,方案 v3.2-r6)**:SoftAP 上传导入通道退役(`meta_net`/`meta_import`
  及其测试删除),设备经 metapass.chuanxilu.net(唯一 TLS 信任锚)直接从应用商店 OTA。流程:
  配网(SoftAP 表单,凭证存 NVS)或已存凭证直连 → SNTP → 数字键盘输玩法 ID → 一次
  `/api/analyze`(名称/可装性/最小可装槽位;业务结果一律 200 + 原因码)→ 选槽 → 流式
  `/api/extracted` 刷写,边下边算 SHA-256 与 analyze 摘要、`x-image-sha256` 响应头双重比对,
  再经 `esp_ota_end`/`esp_image_verify` 权威校验后槽位置 VALID 并写 MNAM 显示名 blob。解包在
  服务端完成(`tools/install-slot/store-analyze.js`,纯 ESM,与安装页同一算法;analyze/extracted
  共享同一份按 play id 缓存、revisionId 回源复核的已验证字节)。取消经确认页(CANCEL/RETRY/
  BACK);会话超时问用户而非强关(可用 `CONFIG_META_STORE_SESSION_TIMEOUT_MS` /
  `CONFIG_META_STORE_HTTP_TIMEOUT_MS` 或 `meta_store_session_set_timeout_ms()` 配置)。自定义
  双根证书包(`main/certs/`,GTS Root R4 + ISRG Root X1)替代默认 Mozilla 全量包。host 检查:
  `test_meta_store_json`(有界 JSON 提取器)、12 个 store-analyzer node 用例、IDF 5.x 签名桩
  `-fsyntax-only` 检查 `meta_store_net`/`meta_store_api`;`validate.sh --static` 全绿。剩余验收
  (方案 §5 Phase 4/5):esp_emu 端到端与真机 OTA。真机构建已复验:factory 预算 0x115d20
  (r6)→ 0x1178d0(r7),余量约 24%。
- **配网易用性(r7)**:SoftAP 热点改为开放(无密码——不用再对着小屏抄 8 位密钥),SSID
  仍随机化(`metapass-XXXX`);配网页新增 "Scan networks" 按钮,走新增的 `/api/scan` 端点
  (后台周期扫描、只读缓存、最多 20 条、SSID JSON 转义、仅 textContent 渲染);UDP/53 DNS
  劫持 + 兜底 302 构成 Captive Portal,手机连上热点自动弹配网页。`/api/wifi` 拒绝空密码。
  方案文档 r7 条目记录设计动机。

## v1.0.0 (2026-09-18)

首个正式版:市场安装 / 保数据升级 / 槽位备份还原 / 固件签名工具链四大链路全部
闭环;单文件混合格式(MPUPV2)、备份 manifest(v1)、签名格式、3-Slot 分区表四项
契约自本版起冻结(详见下方条目;自适应设计保证未来布局微调不破坏既有备份与
升级路径)。

- 开机策略升级为 **bootloader 强制(2026-09-18)**:新增
  `bootloader_components/meta_boot_hooks/`(IDF hooks 机制,`bootloader_after_init`
  在任何应用运行之前执行),检查 otadata 两个副本,凡 `ota_state == VALID` 一律擦除
  ——子固件写 VALID 也无法跨重启常驻,开机策略由 meta-pass 单方面决定,与子固件行为
  无关;被旧模型子固件锁死的设备断电重启即自愈,无需重刷。PENDING 不碰(trial-run
  回滚不受影响)、深睡眠唤醒跳过、flash 加密启用时放弃干预。策略纯逻辑独立为
  `main/meta_boot_policy.h`(宿主测试 `tests/test_meta_boot_policy.c` 钉死 32B 副本
  布局与全部状态判定);QEMU 新增 C3 用例:otadata 预置「CRC 合法 VALID + ota_0 放
  真实子固件」的最恶劣常驻态,断言两副本被擦除并回退 factory 列表页。
- 安装页备份/还原纳入 **NVS 自动打包/自动写回(2026-09-18)**:从设备自身分区表
  定位 NVS(data/nvs 子类型,排除 `cardid`),备份自动读入打包为 `nvs.bin`(SHA-256
  入 manifest `nvs` 字段),还原校验后写回目标设备定位的偏移(自适应,不沿用源偏移);
  两侧均无用户选项,擦除态跳过,旧备份包兼容。背景:裸刷单文件固件会擦除 0x9000
  处 NVS,应用数据只有经备份/还原才能跨刷机保留。测试:`test-slot-backup.mjs`
  PASS 9(定位规则 + manifest 兼容)与 PASS 10(备份→还原数据路径契约,含篡改负例)。
- 唯一发布工件(构建/打包):`tools/build-firmware.sh` 现在只产出一个文件——
  `meta-pass_v<版本>.bin`(~1.1MB),取代原来的三件套(8MB 合并镜像、
  `meta-pass-bootable_*`、MPUP 升级容器)。格式:可引导本体(bootloader + 分区表
  + phy + app 按 flash 偏移铺平,与合并镜像头部逐字节一致)+ 44 字节 `MPUPV2` 指纹
  尾段(魔数 + body 长度 u32le + body SHA-256)。市场刷机工具原样写 0x0(ROM 引导
  本体;尾段落入 factory 分区尾部未用空间);安装页「升级 launcher」现在接受这同一个
  文件——`parseUpgradeArtifact`(launcher-upgrade.js)校验指纹后从本体切片
  bootloader/分区表/app,otadata 视为动态生成的全 0xFF 段;已分发的旧版 `MPUPV1`
  容器仍兼容。8MB 合并镜像只留在 `build/` 供 `verify_firmware.py`/QEMU 使用;
  校验器拒绝陈旧的 `bootable_*`/`upgrade` 产物,并强制指纹长度与本体 parity。
  覆盖测试:`test-launcher-upgrade.mjs` 新增 PASS 7/8(真实产物切片 parity、篡改
  负例、旧版兼容);QEMU 工装 C1/C2/A2 现在端到端引导混合格式文件(A2 从负例转为
  正例——单文件必须可引导)。
- 单次会话模型(启动器):每次上电都回到启动器列表页 —— 子固件不再跨重启常驻。
  根因:签名字固件调用 `metapass_mark_valid()` → `esp_ota_mark_app_valid_cancel_rollback()`
  把 otadata 写成 VALID(flash 持久),此后每次上电 bootloader 直接引导子固件槽位,
  启动器永不运行;若子固件占用 OK 长按又没接返回钩子,设备被锁死(常驻子固件若崩溃循环
  则是真·重启死循环)。修复分两层:hook 的 `metapass_mark_valid()` 改为纯签名自诊断
  (不再调 `cancel_rollback`;ota 状态保持待验证 → 任何重启/掉电自动回退 factory,
  崩溃自恢复走同一回滚机制);启动器在 `app_main` 早期擦除 otadata
  (`meta_store_mark_factory_valid()`,契约已同步到 `meta_store.h`),维护不变量
  "启动器运行 ⇒ otadata 为空 ⇒ 下次上电默认引导 factory"。边界:已被旧模型常驻子固件
  锁住的设备到不了启动器 —— 需用该子固件的返回钩子(OK 长按)或重刷解锁。
  `meta_store.h` 注释修正(`5cbadca`):mark_factory_valid 是擦 otadata,不是标记有效。
  设计文档/README/sdkconfig 已同步单次会话契约。
- sign-firmware.sh 现已支持 Full 合并镜像(bootloader+分区表+app,即市场可刷的发布格式),
  裸 app 镜像继续兼容。修复根因:脚本把 bootloader 头当 app 头解析(image_len=21024、
  total 为负、签名落在设备永不查找的位置 → 虽然命令带了 --egg-text 真机仍报"未签名")。
  app 定位改用单一事实源 `tools/signing/locate_app_image.py`(factory 分区 @0x10000,
  与 install-slot/extract-app-image.js 同一契约);合并镜像输出逐字节保留
  bootloader/分区表,仅追加 pad + 4KB 元数据 sector;digest 仅覆盖 app 区域。
  test_integration.c 同步支持合并镜像解析。输出字段含义澄清(`total` = 签名输出文件
  总字节数;`sig_offset` 同时打印槽位相对与文件绝对偏移)。真机代表路径实测:
  合并+裸镜像均 PASS(META_SIG_OK + 彩蛋解析),结构断言全过(头部保留/pad 0xFF/MSIG 位置/MAEG xor)。
- 回退流水线窗口 32768 → 64(停等,与 esptool.py read_flash 完全一致),921600 保留。
  真机 A/B:esptool.py 停等 @921600 连跑 5 遍零失败(87KB/s);自研流水线同波特率失败
  随吞吐量颩升。机制:流水线下 ACK 上行与巨量数据下行在同一 USB CDC 端点交叠,触发
  C3 USB-Serial-JTAG RX 丢失(日志证据:失败后"排空"长达 16s = stub 积压大量在途数据)。
  921600 已把帧间间隙从 300ms 压到 ~10ms,流水线收益 ≤15%,不值得其风险。
- 第六轮排查文档后验修正:所谓"stub 独立 2B error/status 帧未消费"不成立 —— 源码核对
  证实响应头与 error/status 同在一个 SLIP 帧内(单 delimiter 对),探针 4KB OK 与完整
  备份成功均否证残帧存在;保留 chunkT0 作用域(真)与"mock 必须逐字节忠实于服务端源码"
  方法论教训。见 backup-readflash-error-status-frame.md(zh_CN 为原文存档)。
- 真机 CLI 实测定案(esptool.py 4.12,读槽0起始 128KB):115200 = 11.4s,921600 = 1.5s
  (7.6 倍),两种波特率读出数据 SHA256 完全一致,921600 连跑 5 遍全部成功 —— 高波特率
  链路本身可靠,页面备份偶发失败应归因客户端恢复逻辑(已由三级恢复兜住),而非链路。
- 三级读取恢复策略(备份 desync 自愈):L1 软恢复(补发 ACK→排空→sync)→ L2 按会话
  波特率重开串口(修复恢复路径硬编码 115200 导致的"同址 5 连败":921600 会话中重开
  115200,后续全是波特率失配乱码)→ L3 整机 USB-JTAG 复位 + 重传 stub + 恢复高速波特率
  (全新 loader 实例,避免复用半死状态)。每级独立日志、独立失败上抛,绝不静默。
  第七轮加固(对 `stub_commands.c` 源码核实):L1 恢复 ACK 原为 0x8000,仅在
  `num_acked >= num_sent` 时才中止 stub 的 `handle_flash_read` —— 在途字节不足时 stub
  会继续发完剩余数据、再次毒化链路(同址重试连败的成因)。恢复 ACK 改为 0xFFFFFFFF
  (≥ 任何 num_sent → 确定性中止 → 发 digest → 回命令循环),排空静默 300→800ms
  (digest + 在途残余需要更宽窗口),vendor 数据帧超时 8s→1.5s(921600 下 4KB 帧线时
  仅 44ms),恢复 sync 8s→1s,重试 5→8 次(单块 p⁸ ≈ 万分之一),i18n 重试数字跟随常量。
  预期效果:日志仍会出现单块瞬态失败(设备侧、无法根除),但每次代价 ~4s(原 ~10s)
  且不再级联成整槽报废。
- 连接提速至 921600 波特(读取链路 8 倍)。此前"波特率对 C3 原生 USB 是虚设参数"的结论
  被实测推翻:debug 日志显示每 4KB 帧 356ms ≈ 115200 波特的纯线路时间。而"改波特率无效"
  的真相是:页面传 baudrate===romBaudrate,vendor main() 的 changeBaud 分支从未触发。
  现以 baudrate=921600/romBaudrate=115200 连接(main() 自动执行标准 changeBaud 流程),
  并做即时数据路径验证,失败自动回退 115200 重连(最坏等同旧行为)。读超时 15s→8s。
- 备份读取流水线化提速（实测 10.1 KB/s → 预期 5~8 倍）：钉死 stub `handle_flash_read`
  的 `max_in_flight` 语义（`stub_commands.c:111`，`num_sent - num_acked < max_in_flight`
  三者皆为**字节**）——此前传的 64 是 64 字节，小于一帧 4KB，stub 每发一帧就停等 ACK，
  再叠加 USB-CDC 未满 64 字节的尾包要等下一波数据才发出（停等模式下每帧末尾都有
  4 字节尾包，等包 ≈ 300ms）——这两层是读取慢的全部原因。在途窗口改为
  `globalThis.__READFLASH_PARAMS__ = [4096, 32768]`（字节窗口 = 一个 32KB 块，stub 连发
  8 帧再等确认）；ACK 仍逐帧发（与 esptool.py 一致），累计值域 0x1000~0x8000 安全
  （无 0xC0/0xDB）。由 `test-readflash-protocol.mjs` 新增「窗口字节语义」用例覆盖：
  32KB 读取必须零 ACK 续发 8 帧，兼容 stop-and-wait 模式。
- 修复传输层两个挂死缺陷（实测表现为：读取会话约 2 分钟后必然死链，重试恢复又
  静默挂死 30 分钟无任何日志）：
  ① vendor `readLoop` 超时触发时遗弃未决的 `reader.read()`，其超时定时器后续触发
  会把传输缓冲整体清空（`finally{buffer=new Uint8Array(0)}`）——持续超过
  `FLASH_READ_TIMEOUT` 的读取会话自毁，且每次 `newRead` 都新建 generator、旧 generator
  的超时定时器仍在计时。现在改为持久 `_pendingRead`、显式关闭 generator、移除缓冲
  清空；`FLASH_READ_TIMEOUT` 100s→15s。
  ② vendor `flushInput()` 首行 `await this.reader.closed` 在活跃串口上永不落定——
  恢复路径走到这里就永久挂死。改为有界取消（cancel + 500ms 竞速），页面恢复链
  每步硬超时、sync 失败自动关闭/重开串口再同步。
- 安装备份区新增 Debug 模式复选框：开启后输出协议级诊断（每块耗时、恢复步骤、
  超时位置），供远程排障。
- 修复备份读取反复失败（"Packet content transfer stopped" / "No serial data received",
  重试永不恢复）的根因：esptool stub 在 flash 读取数据帧结束后会无条件追加一帧 16 字节
  MD5 digest（`stub_commands.c`）,而 esptool-js 从不读它——残帧滞留传输缓冲、毒化下一条
  命令的响应，协议错位随每次 `readFlash` 累积。`install-slot/vendor/esptool-js.js` 现在
  逐帧 ACK 并读取/校验 digest 帧（与 `esptool.py read_flash` 完全对齐）；新增
  `install-slot/vendor/md5.js` 提供 digest 校验；读取参数钉死为官方值（4KB 块——stub 硬
  上限——与 64 帧在途窗口）。由 `tools/install-slot/test-readflash-protocol.mjs`
  （mock stub 协议测试，已接入 `validate.sh`）覆盖。
- 新增 `tools/test-bootable-qemu.mjs`：无头 QEMU 引导验证——用 passport-sim 的 QEMU WASM 核心
  实际引导构建产物，断言三件事：UART0 测试变体走完 bootloader→分区表→factory app→app_main
  全链路；MPUP 升级容器被原样写 0x0 时确实无法引导（负例，与真机实测一致）；市场镜像
  （USB-JTAG 配置）渲染出非黑 ST7789 帧缓冲（LVGL 显示层初始化）。从此市场镜像
  `meta-pass-bootable_*.bin` 的可引导性有了自动化证据，不再依赖真机试刷。
- 修复安装页连接失败后设备再也连不上的问题：连接任何一步失败都会释放串口（此前连接挂死/失败后端口保持打开，重试必报 "The port is already open"）；连接流程不可重入（`busy`/`connecting` 双闸）；半开连接不再污染已有连接（全部步骤成功后才提交到全局变量）；设备静默丢命令不再永久挂死流程（探针 15 秒超时 `withTimeout`）。移除无意义的 `changeBaud()` 断开/重连舞蹈——波特率对 C3 原生 USB 是虚设参数；替换掉错误的 16KB 读取块探针（stub 的 `handle_flash_read` 用 4KB 栈缓冲，块超限**静默 return 不报错**），改用官方同款提速方式：块大小维持 stub 上限 4KB，把在途窗口从 4KB 提到 64 块 × 4KB = 256KB，ACK 往返次数降 64 倍（对齐上游 `esptool.py`：官方就是 4KB 块/64 深窗口）。探针校验 bootloader 魔数与数据长度，异常即回退保守的 1KB/4KB 参数。
- 保数据 launcher 升级（§7.2）：升级只写 bootloader + 分区表 + factory 应用 + 擦除态
  OTA 数据重置四项；NVS（存储数据:Wi-Fi 配置、应用内部状态）、`cardid` 与三个子固件槽位永不触碰。USB 安装页
  新增「7. 升级 launcher」章节，写入前读回设备分区表并与升级包逐字节比对（布局不一致
  即拒绝升级）。`tools/build-firmware.sh` 新增产出 `build/upgrade/` 升级包（4 文件 +
  `flash-args.txt`）；`tools/verify_firmware.py` 强制合并镜像中 `nvs`/`ota_0-2`/`otadata`
  保持擦除态，使完整镜像永远不可能携带破坏用户数据的内容。核心逻辑在
  `install-slot/launcher-upgrade.js`，配 Node 测试并接入 `tools/validate.sh --static` 门禁。
  升级以**单文件 MPUP 容器**分发（`build/upgrade/meta-pass-upgrade_<版本>.bin`:魔数 +
  段表 + 逐段 SHA-256）——安装页只选这一个文件，解包校验后把四段镜像写到各分区地址；
  `verify_firmware.py` 额外强制容器与完整镜像逐段同源。
- USB 安装页新增槽位备份与恢复（`install-slot/`）：备份按槽位整分区读取，切分为
  `slot{N}_firmware.bin`（解析出的 ESP app 镜像）+ `slot{N}_tail.bin`（4KB MSIG/MAEG/MNAM
  元数据扇区）+ 可选 `slot{N}_extra.bin`（尾扇区之后的额外存储数据），逐文件计算 SHA-256,
  连同 `manifest.json` 打包为带时间戳的 zip。恢复时用户把每个备份槽位映射到任意目标槽位，
  按 manifest 长度做空间自检（自适应未来的槽位大小调整），写入前逐文件校验 SHA-256,再按
  firmware → extra → tail 顺序写入（tail 最后写，防扇区重擦毁掉先写数据）。槽内有数据但
  既非擦除态也无法按 app 语义识别时（如 slot2 兼做数据存储区、littlefs 卷、非 ESP 镜像
  资源包），不再跳过，改为 dd 式整槽镜像兜底 —— `slot{N}_raw.bin`（尾部擦除态字节裁剪）
  + manifest `type: "raw"` 条目，恢复时从槽位起点原样写回，仅做总长 ≤ 分区大小与 SHA-256
  校验（不预留尾扇区）。备份/恢复位于
  独立章节（§5/§6），与安装流程互不干扰；核心逻辑沉淀在纯 ES 模块 `slot-backup.js`,
  配 Node 测试并接入 `tools/validate.sh --static` 门禁。顺带修复 zip 读取器的
  `DataView(TypedArray)` 兼容性问题（旧引擎只接受 ArrayBuffer）。
- 签名验证链路加固（`feat/sign` 分支）：修复 BUG-01/02/04（未初始化电量标签、`HOST_TEST` 彩蛋魔数反转并新增 m1–m4 回归测试、`size_t` 日志改 `%zu`），安装页单一来源化（`server.mjs` 直接服务规范 `install-slot/`，关闭开发副本漂移，BUG-03），一键本地编译脚本（`tools/build-firmware.sh`），双语 bug 报告与根因知识库（`docs/BUGS.zh_CN.md`、`docs/assets/handoff-unsigned-rootcause.zh_CN.md`、`docs/assets/meta-pass-signing-design.zh_CN.md`、`docs/development/engineering/debugging-workflow.zh_CN.md`）。真机"未签名"症状的根因是线上部署的旧版安装页而非签名链；经修复页重刷后行为符合预期。
- 新增 meta-pass 多固件启动器（`feature/meta-pass` 分支）：分区表在保留 `factory`/`cardid` 契约的前提下新增 `otadata` 与三个大小不等的 OTA 槽位（`ota_0@0x180000` / `0x1D6000`、`ota_1@0x360000` / `0x200000`、`ota_2@0x560000` / `0x29E000`）；启用应用回滚（未适配子固件任何重启后自动回退启动器）；Wi-Fi SoftAP + 网页导入固件（随机密码 + 屏幕一次性配对码，1024 字节分块流式写入）；镜像强制完整性校验（magic/chip-id/大小/SHA-256 显示，`esp_ota_end()` 权威复核），未签名固件启动前弹警告页走 BOOT / CANCEL 菜单确认；本地管理界面支持查看/启动/删除槽位固件；BSP 按键暴露显式 `BSP_BTN_LONG`（1.5 秒）阈值；纯逻辑模块（镜像校验、槽位注册表、导入状态机）配 host tests 并接入静态门禁。设计文档见 `docs/assets/meta-pass-design.zh_CN.md`。
- 第二导入通道（USB 串口，`tools/install-slot/`）：Chrome + Web Serial + esptool-js 在
  ROM 下载模式（按住 UP 键开机）下把子固件直接写入槽位；本地 `.bin`（Full 镜像自动
  解包）或社区玩法链接（SHA-256 校验后写入）。设计见
  `docs/assets/meta-pass-design.zh_CN.md` §6.1。
- 槽位显示名 blob（§6.2）：安装时把固件真名写入槽位分区尾部 4KB sector
  （`slot_offset + 分区大小 − 4KB`，按槽位动态推导，因三个槽位大小已不等：
  `0x1D6000`/`0x200000`/`0x29E000`；`magic "MNAM"` + 长度 + 可打印 ASCII + XOR 校验，
  ≤32 字节）；启动器扫描优先显示真名，缺失回退 `project_name` 剥 `FoloToy-` 前缀的核心名
  （新增 `meta_slot_core_name`）。`ota_2` 为双用途区域（可启动子槽位，或空时作 littlefs
  录音存储）。factory 应用镜像上限收紧为 1.44 MB（`0x170000`），`ota_0` 移入 cardid 之前
  的空隙（`0x180000`）。USB 安装页自动用社区玩法英文标题/本地文件名，Wi-Fi 导入页新增可选名字输入框。

- 加入厂家为优特利 520mAh 电芯生成的 80 字节 CW2017 profile，并实现内容与更新标志检查、写入后校验、规定的重启时序以及有上限的 SOC 就绪等待。

- 扩充环境引导文档：新增乐鑫 Git 服务镜像（`git.espressif.com.cn`）作为中国大陆首选线路，覆盖 ESP-IDF v5.5.3 及其子模块；补充子模块长等待/超时处理、原地修复，以及 `esp32-wifi-lib` 等大仓的按钉死 commit 浅取；提示按仓库残留的 Jihulab `insteadOf` 旧配置；并把官方离线 release 压缩包加入兜底方案（经验来自 `esp-mosaico/esp-mosaico-vibe`）。

- 按功能域整理文档并采用双入口：根目录 `AGENTS.md` 变为薄路由（只保留硬约束与任务路由），详细的 AI 开发工作流下沉到 `docs/development/ai-guide.md`，`agent-guide.md` 并入其中。为 `docs/development/` 增加二级分区（`engineering/`、`ci/`、`release/`），把 `plays/` 应用档案与 `experiences/` 移入带专属 README 的 `docs/reference/` 参考区；删除 `docs/software-design/`（空脚手架）；把 `assets/{fonts,images,music}/README` 三个叶子 README 并入 `assets/` README；把 `project-completion` 的六个子文档压平为单文件；并把每个目录统一为单一 README，消除所有 `INDEX` 文件与一处重复经验索引。所有交叉引用与文献链接已更新；未丢弃任何内容。

- 删除位于 `0x700000` 的旧 app/test 分区，以及相关的 bootloader、校验和
  文档要求；固定的 `cardid` 保护分区及其 CI 校验保持不变。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
