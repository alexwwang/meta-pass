# meta-pass — Bug 报告(当前分支)

[English](BUGS.md) | 简体中文

依据 `docs/assets/meta-pass-design.zh_CN.md` 对当前分支的代码审查结果。
每条:现象 → 根因 → 修复方案。严重级别:**高** = 数据损坏 / 功能失效,
**中** = 真实路径上的错误行为或潜在缺陷,**低** = 健壮性 / 规范性。
已排查并排除的疑点附在文末,附证据。

| ID | 严重级别 | 组件 | 一句话概述 | 状态(本分支已修复) |
|----|---------|------|-----------|---------------------|
| BUG-01 | 高 | `main/main.c` | 电量标签用未初始化栈缓冲构造;SOC 从未渲染 | **已修复** — `main.c:69-71` 加守卫与 `snprintf` |
| BUG-02 | 高 | `main/meta_sign.c` | `HOST_TEST` 变体的彩蛋 magic 判断反了 | **已修复**(上游提交)— `tests/test_meta_net_upload.c` 已加 m1–m4 回归测试 |
| BUG-03 | 高 | `tools/install-slot/`(开发副本) | 与线上安装页脱节:显示名 blob 写到分区外、双写擦掉签名、头部标志位判断漂移 | **已修复 + 结构性修复完成** — 开发页重复文件已删除;`server.mjs` 直接服务规范的 `install-slot/` |
| BUG-04 | 低 | `main/meta_net.c` | 上传成功日志用 `%d` 打印 `size_t` | **已修复** — `meta_net.c:407` 改 `%zu` |

---

## 商店 OTA / 配网失效链(r9 周期,2026-09-27/28)

商店下载通道真机 bring-up 期间发现。每条:现象 → 根因 → 修复 → 教训。
五条链全部是模拟器(无射频桥接)与既有 host 测试的盲区;可修部分由新增
host 测试钉死,数据面由 `tools/e2e-production.mjs` 对正式站设防。

### BUG-05(高)—— 扫描残留态同时冻结扫描与连接

- **现象**:一次空扫描结果之后,手机再也搜不到目标网络;提交凭证后
  "Connecting to WiFi…" 吃满 30s 死线回落热点。
- **根因**:`/api/scan` 把「起扫」(网络任务)与「取结果」(HTTP handler)拆在
  两处,且 **count==0 时 handler 不调 `esp_wifi_scan_get_ap_records` 直接返回**。
  ESP-IDF 中「扫描完成但记录未取」的残留态会同时卡死下一次
  `esp_wifi_scan_start` 与 `esp_wifi_connect`。
- **修复**:起扫 → 有界等待 → **无条件**取记录收进 handler 内同步一步,
  跨任务接力删除。
- **教训**:IDF WiFi 里,*取结果是完成扫描的一部分*——每条扫描路径都必须
  收尾,空结果路径尤其如此。

### BUG-06(致命)—— 一段"兜底"重试反而阻止了关联

- **现象**:(静态审查预判,真机确认)每秒重试循环存在期间,重连永不收敛。
- **根因**:自伤。每秒一次 `esp_wifi_connect()` 本意是"丢事件兜底";IDF 对
  连接中重调的处理是断开重连,每次重调都重启 connect 的内部信道扫描——
  重试循环*就是*关联永不完成的成因。事件组位是粘性的,兜底防的场景
  根本不存在。
- **修复**:删除;信任断连事件(粘性位),原因码经 `meta_store_wifi_fail_text`
  上屏(15/201/202/204/205)。
- **教训**:不要用重复的带副作用调用去覆盖对事件投递的怀疑;先查 API 的
  重入语义(`esp_wifi_connect` 不幂等),再查原语的记忆模型(粘性位)。

### BUG-07(高)—— 300s 会话浮层冻结配网面板

- **现象**:屏幕停在 "Syncing clock…" 长达 200+ 秒,而 `timeout in Ns` 标签
  照常倒数;重启后设备又能用(凭证其实一直存上了)。
- **根因**:会话超时浮层在配网期间上弦(此时作业队列空闲,`busy=false`),
  触发后 `store_tick` 每拍提前 return——面板冻在最后一帧。"timeout 标签仍在
  走"直接证伪了第一版"浮层冻结一切"的错误理论;正确的链是"tick 活着 →
  只有 `s_info` 更新路径停摆 → 浮层上弦 → 提前 return"。
- **修复**:AP_UP/CONNECTING/ERROR 期间不触发浮层;面板实时刷新已耗时
  ("Syncing clock… (7s)");配网网页不再谎称 "saved!"(改 "Received…" 并指向
  设备屏幕)。
- **教训**:"一个标签卡死、旁边标签还在走"能把冻结定位到卡死标签的更新
  路径——先做这个差分,再提理论。时间因素核对("代码里哪个超时能产生
  200s?")本可当场证伪第一版理论。

### BUG-08(高)—— ONLINE 跳变从未被检测 → 永不自动翻页

- **现象**:面板冻在 "Syncing clock… (3s)",timeout 标签照走;WiFi 明明
  连上了,设备却停在配网页。
- **根因**:2s 自动翻页计时锚在**建页时刻**(`s_net_online_at` 只在建页时
  取样一次;建页发生在 CONNECTING 时为 0),`s_net_online_at != 0` 永不成立;
  且 ONLINE 无面板刷新分支,最后一帧 CONNECTING 画面长驻。
- **修复**:tick 自行检测 ONLINE *跳变*(首次见到 → 记时刻 + 重画面板),
  翻页与建页时机彻底解耦。
- **教训**:计时器锚**状态跳变**,不锚建页时刻;每个可达状态都必须有渲染
  分支(缺分支不是角落里的美化问题,是冻住的画面)。

### BUG-09(高)—— 硬拒政策把半个市场锁在门外

- **现象**:r8 上线一天内,真机连试五个玩法全部不支持。
- **根因**:市场数据在我们脚下变了——玩法开始携带额外数据分区
  (`easter` subtype 0x82、voicefs 类 0x81),而 r8 规则对白名单外且
  subtype≠0x40 的数据分区硬拒。政策前提("装了必坏")是错的:实际只写
  解包出的 factory 应用,分区载荷从不进设备。
- **修复**:所有白名单外的*数据*分区警告放行并带 `detail=<label>`(分析器
  PASS 5/5b/5c + 固件合同测试钉死);当日复验正式站 563/675/200。
- **教训**:接受/拒绝政策必须对着**线上生产数据**再验证,不能拿上周的
  样本想当然——正是生产基线检查(黄金 ID 上的 E2E-1/2)在数小时内抓到了
  这次漂移。

### BUG-10(高)—— 商店页面流里藏着逃不出去的环

- **现象**:P1 没有任何离开商店的路径:OK LONG 回 P0,而 ONLINE 后 P0 又在 2s
  后自动进 P1。唯一的真出口(P0 OK LONG → 列表页)只有 2 秒窗口。
- **根因**:页面流从来没有画过。每条迁移都是局部设计的("P1 长按总得去个
  合理的地方"→P0),P0 的自动翻页与 P1 的出口组合成了环。
- **修复**:规范流转图写入方案 §8-r10;OK LONG 从每一页退出商店;P0 自动
  翻页被任何按键取消;P0 显示显式 `> CHANGE WIFI` 行 —— 改网络不再需要先
  等一次连接失败。
- **教训**:多页 UI 必须在接线之前**画出迁移图**,并检查:(a) 每个节点都有
  可达出口;(b) 不存在困住用户的环。组合后的流是整张图的性质,不是单条边
  的性质。

### BUG-11(高)—— 短按导航映射到了看不见的光标

- **现象**:P1 键盘上短按 UP/DOWN 像死的;只有长按换行能移动。
- **根因**:r9 的键映射(短按 = 移光标)移的是*插入光标*,空输入时根本不
  渲染——可见的键选中态没有任何短按路径。
- **修复**:短按 UP/DOWN = 选中键环移 ±1(`mpd_idedit_move_ring`,host 测试
  双向覆盖全部 15 键);屏上 ◀▶ 保持移光标语义。同轮一并修掉:P1 面板高
  100→84(键盘压进面板 10px)、OK 键 58→64(下缘与 0 键对齐)——两者都用
  `_Static_assert` 锁死,几何回归从此构建期失败。
- **教训**:键 → 动作映射必须对着**屏上可见的东西**审查,不能对着模型内部
  状态;布局不变式(不重叠、边缘对齐)应进编译期断言,不能靠 code review。

### BUG-12(致命)—— 一切传输失败塔缩成一个词

- **现象**:连续多个固件版本,真机上输任何玩法 ID 都是 "unavailable",而同样的
  玩法在 host 上分析全部正常。服务端修复(404 覆盖、警告放行)始终治不好它,
  因为设备自己的传输失败从来不可见。
- **根因**:`meta_store_api_analyze` 对*所有*失败路径都写死 reason="unavailable"
  —— TLS 握手失败、DNS 失败、读超时、非 200 状态、响应超缓冲上限。真机上
  最关键的一条:SNTP 未同步时 `time()` 停在 1970 附近,**mbedTLS 证书时间
  校验必然失败**,HTTPS 永远建不起来 —— 屏上与"服务器挂了"完全无法区分。
  顺藤摸出的连带伤:响应超限路径根本不写 reason(会把上一次分析的残留
  文案显示出来)。
- **修复**:传输失败按**阶段 × 时钟状态**分类(`meta_store_api_fail`,纯逻辑,
  host 测试):`TLS failed (clock unsynced).` / `TLS/DNS failed. Retry.` /
  `No response. Retry.` / `Connection lost. Retry.` / `Bad response from
  server.` / `Server error <码号>`;业务 reason 码(not-found/format/too-large)
  原样透传不受影响;P2 对传输类文案逐字展示并给 RETRY 行,不再套
  "Not supported:" 包装。reason 缓冲 24→40 容纳句子形态。
- **教训**:一个 catch-all 错误串 = 诊断黑洞 —— 设备能区分的每一层失败都
  必须上屏;且"host 上正常"只证明服务端,永远证明不了设备的 TLS/时钟栈
  (host 上根本不跑这两样)。

### BUG-13(致命)—— 设备从来没有过 TLS 信任锚

- **现象**:v25 真机:输任何玩法 id 都显示 `TLS/DNS failed. Retry.`,而同一个站
  在 host 上分析全部正常、生产 E2E 15 项全过。
- **根因,两层**:① `esp_http_client_config_t` 从未挂接任何证书源 —— 没有
  `crt_bundle_attach` 也没有 `cert_pem`,mbedTLS 会话零信任锚,无论网络什么
  状态所有握手必败(SNTP 正常工作,证明 DNS 与路由都通 —— 这正是分类器的
  阶段归因足够可信、可以把目光收窄到 TLS 层的原因)。② 打包的
  `main/certs/gtsr4.pem` 是 **cross-signed** 版 GTS Root R4(issuer =
  GlobalSign Root CA),作为 mbedTLS 信任锚不可用:锚自身还要链到缺失的
  issuer(`unable to get issuer certificate`),host 上用 `openssl s_client
  -CAfile main/certs/gtsr4.pem` 复现返回 code 2。cross-signed 根只有在
  其 issuer 同时在场时才可用,而设备包里什么都没有。
- **修复**:换成自签版 GTS Root R4(issuer == subject,指纹
  `71:CC:A5:39:…:A4:BD`);analyze 与 install 两处配置都挂
  `esp_crt_bundle_attach`(main/certs 包,REQUIRES 加 esp-tls)。host 端到端
  验证:用设备锚对正式站握手返回 **code 0 (ok)**。
- **永久门**:E2E-8 用设备的真实锚包对正式站跑完整握手 —— 正是缺失的这个
  测试让坏链穿过了五个固件版本;E2E-8b 拒绝包内任何 cross-signed 锚
  (subject != issuer)。
- **教训**:"E2E 全过"必须包含设备真实的密码学材料 —— issuer 字符串比对
  (旧 E2E-6)不是链验证。任何信任锚变更都必须对生产端点做一次完整握手,
  和设备将要做的完全一样。

### BUG-14(致命)—— 进过一次商店后列表页全废(s_keys[10] 越界)

- **现象**:进过一次商店 —— 必然构建 P1,因为 ONLINE 后 2 秒自动进输入页 ——
  回到槽位列表页后 STORE DOWNLOAD 无法高亮,按键全部失灵;重启后一切正常,
  直到再次进商店。
- **根因,链接 map 实证**:`s_keys` 仍声明为 `static lv_obj_t *s_keys[10]` ——
  r8 的键盘是 10 键。r10 键盘扩到 15 键(`MPD_KEY_COUNT`),构建循环继续索引
  这个硬编码数组:每次进入 P1 写 `s_keys[0..14]`,越界 5 个指针(.bss 40 字节)。
  map 显示被砸者正在现场:`.bss.s_keys 0x3fc999e0 0x28` 紧跟
  `.bss.s_rows 0x3fc99a08 0x10`(列表页四行的行对象指针)与 `.bss.s_slots`
  头部。退出商店后屏幕对象已销毁,`list_refresh`/`rows_refresh` 解引用键盘
  构建器写入的悬垂 `lv_obj_t*` —— 列表页高亮与按键分发全被砸烂,死活取决于
  堆复用。构建日志里唯一的痕迹:`warning: iteration 10 invokes undefined
  behavior`(`-Waggressive-loop-optimizations`,main.c:495 与 :514)—— 从 r10
  起就在那里,而 UB 一直在吃列表页。
- **修复**:声明改为 `static lv_obj_t *s_keys[MPD_KEY_COUNT]` —— 数组尺寸与
  构建循环同源,键盘键数再变也不可能静默越界;净室构建两个 UB 警告消失。
- **教训**:硬编码尺寸配另一个常量限界的循环是不需要真机测试就会爆的定时
  炸弹;编译器第一天就在报警。固件构建警告必须当失败处置,不能 grep 掉 ——
  main 组件现已启用 `-Werror`。

### BUG-15(高)—— RETRY 把用户踢回清空后的输 ID 页

- **现象(v31 真机)**:分析失败页上,在上一次 analyze 还在跑(30s 超时窗)或已掉线
  时按 RETRY,静默回到输入页且键盘**被清空** —— 输了 3 位以上的 ID 得从零重打。
  连按三次 RETRY = 三次无提示的踢回。
- **根因**:`cmd_analyze` 在作业 RUNNING 时返回 ESP_ERR_INVALID_STATE;OK 分发把一切
  失败都当成 "goto P1",而 `page_store_id_build` 里的 `mpd_idedit_init` 把缓冲清零。
  失败没有任何用户可见痕迹。
- **修复**:RETRY 失败只在页上显示 `Busy/Offline. Hold OK = exit` 并停留;
  `page_store_id_build` 用新增的 `mpd_idedit_set_digits` 预填上次确认的玩法 ID
  (纯逻辑 host 测试:拒空/非法/超长,光标在末尾续输即追加)。

### BUG-16(低)—— "timeout in Ns" 倒计时永远不动

- **现象(v31 真机)**:P0 显示 `timeout in 300s` 冻在同一个数,永远不变。
- **根因**:配网/连接态每拍都调 `store_touch()`(BUG-07 教训:那里不能冻结面板),
  deadline 恒为满值 —— 但渲染层还在把它显示出来。对一个无意义的数做了正确的显示。
- **修复**:P0 状态行显示真实状态句(`WiFi setup AP ready.` / 连接失败原因),
  不再显示任何倒计时。

### BUG-17(高)—— OPEN 失败仍只说 "TLS/DNS failed. Retry."

- **现象(v31 真机)**:每次 analyze 都以同一句两行文案失败,而 host(经可用代理)
  能访问同一站点。DNS 失败/连接超时/证书被拒在屏上完全无法区分。
- **根因**:`esp_http_client_open` 把一切死因塔缩成一个 ESP_FAIL;分类器只有
  阶段×时钟矩阵。当前网络最可能的死因是路由器级 DNS 劫持或代理/TUN 干扰 ——
  恰是屏上叫不出名字的东西。
- **修复**:HTTP 事件回调在 OPEN 期间捕获底层错误类型
  (`esp_tls_get_and_clear_error_type`:esp 系 DNS/超时/拒连,mbedTLS 证书标志),
  `meta_store_api_fail_open_text` 渲染具体死因:`DNS failed.\nCheck WiFi/router.` /
  `Connect timeout.\nCheck network.` / `Connection refused.` /
  `Cert check failed[(clock unsynced)].`。未知死因回落原阶段句。host 测试覆盖;
  下次真机失败屏上直接报层位,不用再隔着一台挂代理的 Mac 猜。

### BUG-18(严重)—— 加入 WE1 后握手照败:链尾的 issuer 从来不在包里

- **症状(v34 与 v35 真机,日志逐字节相同)**:`esp-x509-crt-bundle: No matching
  trusted root certificate found` → `mbedtls_ssl_handshake returned -0x3000` →
  屏显 `TLS failed [esp=0x801a]`。v35 已加 GTS WE1 中间证书,毫无变化。
- **静态复现(无需真机)** —— `tools/verify-crt-bundle-match.py` 用裸 ASN.1 解析器
  (不经 cryptography 再编码)逐字节重推设备查找:解析
  `build/esp-idf/mbedtls/x509_crt_bundle`,走一遍从 `metapass.chuanxilu.net`
  抓的实发链,对每一层跑精确的 `esp_crt_find_cert` 二分查找(DER issuer 名上
  memcmp、前缀语义)。v35 包上的结果:第 0 层(leaf → WE1)FOUND,第 1 层
  (WE1 → GTS Root R4)FOUND,**第 2 层(cross-signed GTS Root R4 →
  GlobalSign Root CA)NOT FOUND** —— 与设备报错行完全一致。
- **根因**:服务器实发 3 张链,末尾是 cross-signed 的 GTS Root R4(issuer =
  GlobalSign Root CA)。`esp_crt_verify_callback` 对链上**每一层**都做
  issuer 必须在包内的查找,不只 leaf:链走到尾张时,它的 issuer 不在包里。
  此前的两个假说 —— 包内名字编码漂移、二分排序错 —— 被同一次运行否定:
  WE1 存的名字与 PEM 原始 subject DER 逐字节一致,offset 表排序正确。
- **为何此前每道门都绿**:E2E-8(用设备锚做 openssl 握手)能过,是因为
  openssl 会自建到它已持有的自签 R4 的路径;E2E-8b 只要求中间证书的 issuer
  在包内,而 cross-signed R4 作为 subject 在包内、issuer 不在;E2E-8c 只比了
  leaf 的 issuer,且是 RFC2253 文本。没有任何一道门把逐层查找放到完整实发链
  上重演。
- **修复**:`main/certs/globalsign-root-ca.pem` —— 自签根,subject 与链尾的
  `issuer_raw` 逐字节相等(89 B),SHA-256 指纹
  `EB:D4:10:40:E4:BB:3E:C7:42:C9:E3:81:D3:1E:F2:A4:1A:48:B6:68:5C:96:E7:CE:F3:C1:DF:6C:D4:33:1C:99`;
  `openssl verify` 经它闭环全链。设备查找模拟现在三层全 FOUND,报
  "chain would VALIDATE on device"。
- **新增门**:E2E-8d 在每次 E2E 中对实发链跑字节级设备查找复现;同一工具核验
  bundle 结构/排序/名字字节,并验证最新 `build/meta-pass_v*.bin` 原样内嵌当前
  bundle(陈旧镜像守卫 —— v35 旧 bin 正确地在该项失败)。E2E 计数 17→19。

### BUG-19(严重)—— HTTP 状态码读错了 API:analyze/install 从不可能成功,与 TLS 无关

- **症状**:TLS 修好(BUG-18)后,analyze 仍会以 `Server error 600` 全灭,install
  全部拒于长度检查 —— 代码比较的根本不是状态码。
- **根因**:`esp_http_client_fetch_headers()` 返回的是 **Content-Length**(IDF 5.5.3
  `esp_http_client.h:639`:"Download data length defined by content-length header"),
  不是 HTTP 状态码。`meta_store_api.c` 把返回值赋给 `status` 再比 `status != 200`:
  analyze 响应约 477B → "HTTP 477",extracted 响应 1 882 272B → "HTTP 1882272",
  全部被拒。host 桩声明返回 `int`,纯语法门抓不住语义误用。发现手段:与样例
  恢复安装器(`ai-passport-miniapp-installer`,真机验证)交叉比对 —— 它用的
  `esp_http_client_get_status_code()` 恰是我们从未调用的 API。
- **修复**:两个端点的状态码一律来自 `esp_http_client_get_status_code()`,
  `fetch_headers()` 只留作 `< 0` 传输错误信号(analyze 200 为定长响应,实测
  `content-length: 477`;301 空 body 显式拒绝)。install 在 read() 报 EOF 后追加
  `received == content_len` 硬校验 —— 残留字节不符现在是硬错误,不再静默吞掉截断。
- **请求面加固(同轮,样例启发)**:设备 User-Agent 单一定义点
  (`META_STORE_API_USER_AGENT`,meta_store_api.h),E2E 复演从该源读取(过去文档
  声称的"设备 UA"没有任何检查兜底);`disable_auto_redirect = true` 让 301 无法
  静默把长度契约重锚到别的源;`buffer_size` 1024→4096(头部解析缓冲,样例验证值,
  CDN 边缘头部可超 1KB);TLS 重协商显式关闭(`CONFIG_MBEDTLS_SSL_RENEGOTIATION=n`);
  `JOB_STACK` 8192→10240(样例恢复器用真机验证过的 10K 栈跑同款 TLS 栈)。
- **门禁**:`tests/test_http_contract.py` 钉死 IDF 契约原文(有 checkout 用原文,
  无则用记录片段 —— CI 裸 checkout 规则)、桩签名(`int64_t`)、两个访问器、
  BUG-19 误用模式绝迹、两项配置的全部加固字段。host 桩同步真实 `int64_t` 契约。

### 措辞(r10,非 bug 而是契约)—— 槽位状态说人话

`(invalid)` 有歧义(暗示设备/槽位损坏),而它最常指 ota_2 装着 littlefs 录音
——那是用户数据,不是损坏。状态现读作 `(empty)`(已擦除)与 `(no firmware)`
(有数据但非可引导镜像),两者都可直接覆盖安装(esp_ota_begin 先擦除);措辞
住在 `meta_slot_list_word`/`meta_slot_detail_word`,host 测试钉死,不会静默
漂移。

### 流程教训(r9–r10.6)

1. **E2E 必须打正式站**——本地 server.mjs 证明的是代码,不是部署。
   `tools/e2e-production.mjs` 是数据面的验收门;每次刷机前重跑。
2. **只有设备能跑的路径靠静态审查 + 屏上诊断**——射频、任务生命周期、
   UI 状态机无法 host 测试;F1–F4 每条都来自对照已知 IDF 失效模式的
   checklist,且剩余的每个失败都会把层位与原因码印在屏上。
3. **设备决策逻辑抽成纯逻辑模块**(`meta_store_idedit`、`meta_store_prov`、
   `meta_store_analysis`、`meta_store_api_fail`),让 host 测试跑的就是
   真机那份代码——本规则采纳之前,"模拟器上验过"的说辞在这里死过两次。
4. **先画页面流转图再接按键**(BUG-10/11)——逃不出去的 P0↔P1 死环与
   失灵的短按导航,都源于在纸上没有迁移模型时就动手写按键处理。流转图
   (方案 §8 r10)现在是规范本体:代码里存在的每条迁移先在图上存在。
5. **任何失败不许塔缩成一个词**(BUG-12,r10.4)——"unavailable" 让用户与
   开发者瞎了五个版本。设备传输失败现按阶段 × 时钟状态分类;服务端每条
   错误路径都带 upstream 状态/阶段的 `detail`。可调试性是双边契约,
   不是锦上添花。
6. **"E2E 全过"必须包含设备真实材料与请求面**(BUG-13)——issuer 字符串
   比对不是链验证,host 的 fetch 不等于设备的 HTTP 栈。E2E-8 用设备真实
   锚包跑完整 TLS 握手;契约检查用设备的原始请求面复演(HTTP/1.1、设备
   UA、无附加头)。设备跑什么,就测什么。
7. **构建警告就是失败**(BUG-14)——两条 `iteration 10 invokes undefined
   behavior` 警告自 r10 起躺在每次构建日志里,而 UB 一直在吃列表页;
   grep 掉警告只看 PASS 行,正是让它活下来的流程错误。main 组件启用
   `-Werror`;警告不得被过滤出视野。
8. **数组尺寸必须与限界循环的常量同源**(BUG-14)——`s_keys[10]` 配
   `for (i < MPD_KEY_COUNT)` 是真机测试盖不住、代码评审三个版本没抓住的
   炸弹;声明与循环必须源自同一个常量。
9. **测试工具必须区分环境失败与服务失败**(2026-09-28 出口事件)——本机
   代理 TUN 把 DNS 劫持到 fake-ip 并重置 TLS,症状形态与生产宕机完全一样;
   层位定位前,"服务是不是挂了"白耗数小时。E2E-0 现做出口预检:fake-ip
   + 握手失败 exit 2(环境),绝不误报为服务失败(exit 1)。
10. **发布产物必须出自净室重建**(v26–v28 Kconfig 漂移)——增量构建带着
   陈旧的全量 Mozilla 证书包穿过了三个版本,而 sdkconfig.defaults 声称
   相反。每次出发布产物前 `rm -rf build`,然后核验生效的 sdkconfig(与
   产物本身),绝不假设。
11. **重演设备的失败面,而不是它的代理物**(BUG-18)——三道绿门与一台握不了
   手的设备并存,因为每道门检查的都是简化切片:openssl 的路径构建代替了
   bundle 的逐层 issuer 查找;中间证书卫生规则少走了一跳;RFC2253 文本比对
   代替了设备真正在做的 DER 字节比对。真机失败一旦可确定地复现,先用静态
   字节级手段复现它(`tools/verify-crt-bundle-match.py`),再让一切修复接受
   该复现的检验。

---

## PASS-RADAR "仍然提示未签名" — 根因与结论

实测现象:用修复后的签名工具签出的 pass-radar 固件,设备引导时仍提示
"Unsigned firmware!"。

排查过程(全部在本机可复现):

1. **签名链本身是正确的。** `tools/signing/run-verify-tests.sh` 直接编译
   固件真实验签代码(`main/meta_sign.c`,不含 `HOST_TEST`),用固件内嵌的
   `meta_sign_pubkey.h` 验签:最新签名镜像
   (`../pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin`,19:09)
   返回 `META_SIG_OK`;修复前签的旧镜像(`pass-radar_signed_v2.bin`,13:53)
   正确返回 `META_SIG_VERIFY_FAIL`(其摘要算法有误,必须重新签名)。
   `build/` 下两个 launcher 构建(12:07 与 19:06)均已内嵌当前公钥,签名端
   与验签端一致。
2. **真正的 bug 在安装路径:即 BUG-03。** README 首选的本地安装流程是
   `node tools/install-slot/server.mjs` → 开发副本安装页。该副本(a)用
   第二次 `writeFlash` 向已含 MSIG 签名的同一 4KB 尾扇区写显示名 blob,
   把签名擦掉;(b)按分区大小计算 blob 地址,烧写到槽位分区之外。经它
   安装必然破坏签名扇区 → launcher 读到全 0xFF 的尾部 → "Unsigned
   firmware!" —— 尽管 .bin 文件本身的签名完全正确。

**结论:** 用当前 `sign-firmware.sh` 重新签名后,经(已修复的)开发页或
Cloudflare Pages 正式页安装;不要再用旧开发页此前烧写过的产物直接重装
而不重写尾扇区。BUG-03 修复后,该复现路径已被关闭。

**本地线刷已端到端调通(后续 2):** 本地线刷服务(`node
tools/install-slot/server.mjs`,服务规范 `install-slot/` 页面)冒烟通过
(页面、ES 模块、vendor 资源全部 200;SSRF 防护与 404 行为正确),并对真实
签名镜像完整回放了安装字节路径:`extractAppImage()` 解析出 image_len 962416 /
尾扇区偏移 0xEB000 / 完整 4KB tail sector;套用安装页的 MNAM 显示名补丁
(4056 处右对齐)后,组装出的槽位字节通过**固件验签代码**(`main/meta_sign.c`
+ 真实 mbedtls):`META_SIG_OK`、`meta_sign_detect_sector() == true`、彩蛋文本
完好。安装页 `tailSectorOffset`(image_len 后 4K 对齐)与设备端
`meta_sign_sector_offset()` 及设计文档 §7 布局完全一致
(MSIG [0..127] / MAEG [128..4055] / MNAM [4056..4095],尾扇区单次写入)。

验证命令:

```bash
tools/signing/run-verify-tests.sh ../pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin
node tools/install-slot/test-extract.mjs   # 安装页单测(规范模块)
bash tools/validate.sh --static            # 静态检查 + 主机测试,全部 PASS
PORT=4191 node tools/install-slot/server.mjs  # 本地线刷;浏览器打开 http://localhost:4191/
```

---

## BUG-01(高)— `add_battery()` 渲染出垃圾电量标签

**文件:** `main/main.c:64-71`
**状态:已修复** — 已按下述方案修复(`main.c:69` 加入 `soc < 0` 守卫);
代码块保留作缺陷记录。

```c
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    char text[12];
    lv_obj_t *lbl = ui_pixel_label(parent, text, &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(lbl, 204, 30);
}
```

**现象。** 标签由 `text` 构造,而 `text` 是**完全未初始化的栈缓冲**。
`ui_pixel_label()` 内部立即调用 `lv_label_set_text(label, text)`
(`ui_pixel.c:18-26`),会对其 `strlen()`:画出的是栈上的随机字节,若 12 字节
内没有 NUL 还会越界读取。注释承诺"读数 −1(不可用)时不画,避免显示假
数字"——`bsp_battery_soc()` 失败时确实返回 −1(`bsp_battery.h:13`)——但这个
返回值从未被使用。`soc` 成了死变量;主机测试链(`tools/validate.sh`)从不
编译 `main.c`,`-Werror` 因此从未发现。

**根因。** 半成品功能:SOC → 文本格式化和"不可用则不画"的守卫从未编写。
两处调用点都受影响——列表页(`main.c:143`)和详情页(`main.c:308`),即
最常用的两个页面右上角都是垃圾内容。

**修复方案。**

```c
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    if (soc < 0) return;                       // 无电量计 → 不画
    char text[12];
    snprintf(text, sizeof(text), "%d%%", soc);
    lv_obj_t *lbl = ui_pixel_label(parent, text, &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(lbl, 204, 30);
}
```

(确认已包含 `<stdio.h>` 以使用 `snprintf`。)

---

## BUG-02(高)— `HOST_TEST` 解析器的彩蛋 magic 判断反了

**文件:** `main/meta_sign.c:47`(`meta_egg_parse` 的 `#ifdef HOST_TEST` 变体)
**状态:已修复** — 上游提交已去掉多余的 `!`;该行现为
`if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;`,与设备变体
及主机 stub 一致。代码块保留作缺陷记录。
**已加回归守卫:** `tests/test_meta_net_upload.c` 现已覆盖彩蛋路径 ——
`m1_egg_parse_valid`(有效 MAEG → `META_EGG_OK`)、`m2_egg_parse_absent`
(擦除态扇区 → `META_EGG_ABSENT`)、`m3_upload_signed_tail_preserved`
(含 MAEG 的已签名尾扇区在上传后完整保留,dispname 被拒)、
`m4_upload_unsigned_tail_rebuilt`(未签名上传重建 MNAM,无 MAEG 残留)。
旧的反转变体会在 m1/m3 立即失败。

```c
static const unsigned char egg_magic[4] = META_EGG_MAGIC_BYTES;
if (!memcmp(egg, egg_magic, 4)) return META_EGG_ABSENT;
```

**现象。** 与同一函数的其它所有实现语义相反:

- 设备变体 `main/meta_sign.c:97`:magic **缺失** 才返回 `META_EGG_ABSENT`
- 主机 stub `tests/esp_stubs/meta_sign_stub.c:58`:`memcmp(...) != 0` 返回 ABSENT
- 格式测试 `tests/test_meta_sign.c:158`:"擦除态(无 MAEG)→ `META_EGG_ABSENT`"

HOST_TEST 变体在 magic **匹配** 时返回 `META_EGG_ABSENT`,即有效彩蛋被报
为"无彩蛋",空白(0xFF)扇区反被当作"有彩蛋"进入解析。

**根因。** 多写了一个 `!`。注意这不是死代码:`tools/validate.sh` 用真实的
`main/meta_sign.c` 加 `-DHOST_TEST` 编译 `tests/test_meta_net_upload.c`
(validate.sh:73-78),每次 CI 都会构建该变体——只是上传测试尚未覆盖彩蛋
路径,所以一直没被发现。

**修复方案。** 与另外两处实现对齐:

```c
if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;
```

更彻底的做法:删除 `main/meta_sign.c` 中的 HOST_TEST 块,让
`test_meta_net_upload` 链接 `tests/esp_stubs/meta_sign_stub.c` 的
`meta_egg_parse`/`meta_sign_verify`(与 `test_meta_sign` 的做法一致),
只保留一份实现,杜绝再次漂移。

---

## BUG-03(高)— 开发版安装页已漂移出正确性 bug

**文件:** `tools/install-slot/install-slot.html`、
`tools/install-slot/extract-app-image.js`(对照线上 `install-slot/` 副本)。
**状态:已修复** — 已把线上页的单次写入尾扇区流程与 `& 1` 位测试移植进
开发副本;`diff` 曾确认两份副本一致(仅 `name-blob.js` 头部一行路径注释
不同)。
**结构性修复(去重)也已完成:** 重复的开发页文件已删除,
`tools/install-slot/` 现仅保留 `server.mjs` 与 `test-extract.mjs`,
`server.mjs` 直接服务规范目录 `install-slot/`(`PAGE_DIR =
../../install-slot`),即 Cloudflare Pages 部署的同源字节。
`test-extract.mjs` 改为从规范目录导入模块、读取规范 HTML 做 i18n 测试。
`README.md`、`README.zh_CN.md` 与 `install-slot/README(.zh_CN).md` 的目录
结构说明已同步更新。验证:`node tools/install-slot/test-extract.mjs` →
8/8 PASS;`server.mjs` 冒烟(`/`、`/name-blob.js`、
`/extract-app-image.js`、`/vendor/esptool-js.js` 均 200)。

仓库维护两份安装页(README:152-153):`install-slot/` 是部署在 Cloudflare
Pages 的正式页;`tools/install-slot/` 是由 `server.mjs:91-94` 服务的本地
开发副本。没有任何机制校验两者一致,开发副本已落后出三处影响行为的差异。

### (a) 显示名 blob 写到槽位分区之外(破坏下一个分区)

开发副本 `tools/install-slot/install-slot.html:133,561-563`:

```js
import { packNameBlob, sanitizeDisplayName, blobOffset, maxAppImageSize } from "./name-blob.js";
...
const blob = packNameBlob(dispName);
const blobAddr = address + blobOffset(s.size);
```

`blobOffset(len)` = `ceil(len/4096)*4096 + 4056`(`name-blob.js:23-29`),
期望的入参是**镜像长度**,开发页却传入了**分区大小** `s.size`。`s.size`
是 4K 对齐的,于是 blob 落在

```
槽位起始 + part_size + 4056
```

即槽位末尾之外 4056 字节。以槽 0 为例(`ota_0`,0x180000 + 0x1D6000 =
0x356000 结束):blob 被烧写到 **0x356FD8 —— `cardid` NVS 分区内**
(0x356000,0x4000);对另两个槽,则落在下一个 app 分区的起始扇区。
即便按旧的尾扇区布局理解,这也是差了整整一个 sector("分区末尾"的 blob
本应在 `槽位起始 + part_size − 40`)。结果:显示名功能静默失效,**且安装
器破坏了无关分区**(设备的 card ID,或相邻槽位的前几个扇区)。

线上页已是正确的"按镜像长度 + 单次写入"流程
(`install-slot/install-slot.html:564-584`,`packNameBlobTail` 合入 4KB
`image.tailSector`,写到 `image.tailSectorOffset`)。

### (b) 双写擦除签名/彩蛋扇区

开发页先写 app 镜像,再发**第二次** `writeFlash` 写 blob(开发页:552-569)。
esptool 写 flash 前会擦除目标扇区,第二次调用落在第一次已写过的同一 4KB
尾扇区上,把先写入的内容抹掉。签名镜像的 MNAM 窗口与 MSIG 同扇区,因此
**经开发页安装的签名固件会丢失签名**,启动器对可信构建显示 "Unsigned
firmware!"。线上页的注释正是为此而写("分次 writeFlash 会重复擦除同一
sector,把先写入的签名/彩蛋擦掉",`install-slot/install-slot.html:562-563`)
——修复从未同步回开发副本。

### (c) `hashAppended` 标志位判断漂移

`tools/install-slot/extract-app-image.js:29`:

```js
const hashAppended = buf[start + 23] === 1;              // 开发副本
const hashAppended = (buf[start + 23] & 1) === 1;        // 线上副本(第 29 行)
```

ESP 镜像头第 23 字节是标志字节(bit0 = hash_appended)。当前 ESP-IDF 只写
0 或 1,两者行为一致;但开发副本的相等判断在标志字节出现其它置位时会把
镜像长度算短 32 字节,导致烧写的 app 镜像被截断。开发副本应采用线上副本
的位测试。

**根因。** 代码复制且无同步机制:`tools/check_repo.py` 不比较两份副本,
`tools/install-slot/test-extract.mjs` 只测开发版 `extract-app-image.js`,
不与线上版对照;开发页 HTML 在引入尾扇区单次写入时没有同步更新。

**修复方案。**
1. 短期:把线上页的写入流程移植进 `tools/install-slot/install-slot.html`,
   位测试移植进其 `extract-app-image.js`。
2. 结构性:让 `tools/install-slot/server.mjs` 直接服务规范的
   `install-slot/` 目录(单一事实来源),或在 `tools/check_repo.py` / CI 中
   增加漂移检查:除头部注释行外两份副本必须逐字节一致。

---

## BUG-04(低)— 上传日志用 `%d` 打印 `size_t`

**文件:** `main/meta_net.c:407`
**状态:已修复** — 已改为 `%zu`,直接传 `size_t`。

```c
ESP_LOGI(TAG, "槽位 %d 写入成功: %s %s (%d B)", slot, name, ver, req->content_len);
```

`req->content_len` 是 `size_t`(ESP-IDF `httpd_req_t`;项目自己的 stub 也是
如此定义,`tests/esp_stubs/esp_http_server.h:32`)。在 32 位 ESP 目标上无害,
但在其它主机上是潜在的 `-Wformat` 告警,也与项目其它地方的 `%u` 风格不一
致。建议改为 `(unsigned)req->content_len` 配 `%u`。仅影响规范性。

---

## BUG-05(高)— SoftAP 期间的周期 WiFi 扫描导致热点不可见

**文件:** `main/meta_store_net.c`(r7 配网扫描)
**状态:r8 已修复** — 按需扫描(`/api/scan` 处理器置 `s_scan_req` 并等待至多
`PROV_SCAN_WAIT_MS` 2.5s;每次请求只扫一次,无周期定时器)。

真机现象:配网热点开着,手机的网络列表里根本搜不到 `metapass-XXXX`——而同一
根射频几分钟前连家里路由器毫无问题。

根因:r7 为保持配网页网络列表新鲜,后台任务每 3s 扫描一次。
`esp_wifi_scan_start` 必须逐信道驻留侦听 beacon,期间 SoftAP 自己的 beacon
停发。3s 一轮意味着每个周期内有相当比例时间 beacon 处于黑暗;手机侧扫描
(本身也隔几秒一次)不断撞进空窗。台架上看似"页面一切正常",因为 QEMU/模拟器
只覆盖 HTTP 层——射频层面的交互对所有 host 测试不可见。

教训:**空口上的所有东西共用一根射频。** AP "搜不到"时,共存的周期性射频操作
(扫描、BLE、轮询)必须列为嫌疑人;AP 可见性只能拿真手机对真硬件验证。默认
事件驱动、按需执行;为 UI 方便做的"周期刷新"不是免费的。

## BUG-06(高)— 固定六位高位补零的玩法 ID 与市场 ID 空间不匹配

**文件:** `main/main.c`(P1 ID 输入页,r7 及以前)
**状态:r8 已修复** — P1 重建为屏上数字键盘,ID 变长(1–7 位,
`ID_MAX_DIGITS`;第 7 位自动提交),与服务端 `\d{1,7}` 校验一致。

现象:数字页只能产出恰好六个字符;更短的 ID 只能脑补前导零——563 这种五位数
输入别扭,四位数直接不可能。

根因:输入 UI 围绕一个它所对接的产品里根本不存在的定宽格式设计——市场用
变长数字 ID 标识玩法(服务端 `\d{1,7}`)。固定位数不是收紧校验,而是凭空
发明的约束。

教训:**输入格式必须镜像权威 ID 空间,而不是迁就 UI 便利。** 屏幕显示不了
输入框,答案是在屏上画键盘——不是重新解释用户的数据。数字 ID 高位补零会
静默指向*另一个*资源(在这个市场里 01 ≠ 1),所以这是正确性 bug,不是
观感问题。

---

## 已排查并排除的疑点(附证据)

- **LVGL 9.5 定时器自删除** — `lv_timer.c` 用 `act_timer_deleted` 守护回调,
  回调内删除自身定时器是受支持的行为,UI 拆除路径无问题。
- **彩蛋页程序化滚动** — `block()` 会剥掉自身子对象的
  `LV_OBJ_FLAG_SCROLLABLE`,但彩蛋面板(`main.c:189-190`)通过
  `lv_obj_set_scroll_dir()` 重新启用了滚动;`lv_obj_scroll_by_raw()` 根本不
  检查该标志。方向语义符合 LVGL 9.5 头文件契约(`dy > 0` 向开头滚动),
  `btn == BSP_BTN_UP ? +step : −step`(`main.c:440`)映射正确。
- **大小上限算术** — 对 `partitions.csv` 中 4K 对齐的分区大小(0x1D6000 /
  0x200000 / 0x29E000),`meta_sign_app_limit()`(`meta_sign.h:56`)、
  `meta_name_max_app_size()`(`meta_name.h:42`)与 JS `maxAppImageSize()`
  (`name-blob.js:33`)共用的 `part_size − 4096` 上限是**恰好紧**的:对任何
  被接受的 `image_len`,必有 `ceil(image_len/4096)*4096 ≤ part_size − 4096`,
  尾部 metadata sector 总放得下。无 off-by-one。
- **按键长按接线** — `bsp_button.c:72-76` 以栈上 `button_event_args_t`
  注册 `BUTTON_LONG_PRESS_START`(`press_time = BSP_BTN_LONG_MS (1500)`);
  `iot_button_register_cb` 返回前已把 `press_time` 拷入内部 cb_info 数组
  (`iot_button.c:378,402-411`),栈生命周期无问题。零初始化的
  `button_config_t` 使 `TIME_TO_TICKS(0, LONG_TICKS)` 回退到
  `CONFIG_BUTTON_LONG_PRESS_TIME_MS`(Kconfig 默认 1500ms),与显式参数一致。
- **`meta_seq` 匹配器** — 全路径推演(索引 > 0 时的间隔超时、按键失配重启、
  重复首键、无符号减法容忍回绕):行为与设计文档一致。
- **`meta_name_pack_tail`/`unpack_tail` ↔ JS `packNameBlobTail`** — 右对齐
  40B 窗口、xor 在末字节;C 与 JS 逐字节一致。
- **`meta_egg_parse` 窗口边界** — 编译期检查
  (`META_EGG_WINDOW_OFF + META_EGG_TOTAL_LEN ≤ META_NAME_BLOB_OFF`,
  即 128 + 3928 ≤ 4056),与 `sign-firmware.sh` 的布局(MSIG@0、MAEG@128、
  MNAM@4056)一致。
- **`image_len` 来源** — 上传路径用 `esp_image_verify` 元数据,正确避开了
  "镜像头 +20 处不是长度字段"的坑(`meta_net.c:314-318`)。
- **线上安装器的尾扇区提取** — 将 MSIG/MAEG/MNAM 提取进同一个
  `image.tailSector` 并单次 `writeFlash` 写入,正确。

---

*报告基于静态审查 + 主机测试源码验证。行号对应当前分支。姊妹文档:
[BUGS.md](BUGS.md)。*
