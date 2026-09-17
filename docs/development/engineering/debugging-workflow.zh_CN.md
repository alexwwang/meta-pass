<p align="right">
  <strong>简体中文</strong> · <a href="debugging-workflow.md">English</a>
</p>

# 根因排查工作流 —— 签名验证排查的经验沉淀

> 状态:工程规则(对本仓库的 AI 辅助与人工调试均有约束力)。
> 源自 2026-09"子固件 USB 线刷后显示未签名"排查(BUG-01…04、BUG-03 安装路径损坏、
> 线上安装页滞后)。案例档案:`docs/BUGS.zh_CN.md`、
> `docs/assets/handoff-unsigned-rootcause.zh_CN.md`;契约:
> `docs/assets/meta-pass-signing-design.zh_CN.md`。

## 1. 案例历史(四轮排查,实际发生了什么)

**第一轮 —— 对照设计文档的全分支静态审查。** 对照 `docs/assets/meta-pass-design.md`
审查全分支,产出 BUG-01…04(未初始化电量标签、`HOST_TEST` stub 彩蛋魔数反转、开发版
安装页漂移、`size_t` 用 `%d`),每条带 file:line 证据;同时记录了**已排除嫌疑清单**
(LVGL 定时器自删除、滚动语义、大小上限算术、按键长按接线)及其排除证据。记录已排除项
很重要:后来者不必重查。

**第二轮 —— "重签的固件仍显示未签名"。** 最诱人的假设(签名链坏了)**最先被证伪**:
宿主集成测试编译的是*真实*固件验签器,对最新签名镜像返回 `META_SIG_OK`,且两个 launcher
构建都嵌着当前公钥。真正原因在**安装路径**:开发版安装页(README 首选本地工作流)用第二次
`writeFlash` 向已含签名的同一 4KB 尾扇区写显示名 blob —— esptool 每写必擦整扇区,签名在
烧写瞬间被抹掉,.bin 文件本身毫无问题。结构性修复:安装页单一规范目录,本地 dev server 与
Cloudflare 部署同源;尾扇区永远单次 `writeFlash` 写入。

**第三轮 —— 关闭"host PASS + 真机 FAIL"的 gap。** 静态分析无法证明*线上部署的*旧版
安装页实际往 flash 写了什么。不靠猜,在宿主上**回放安装页的字节路径**:提取 → MNAM 补丁 →
组装槽位字节 → 喂给真实固件验签器(真 mbedtls)→ `META_SIG_OK`。随后用户经修复后的页面
重刷,真机行为符合预期。通用教训:**当宿主与设备结论相悖,在宿主上逐字节复现变换过程,把
结果喂给为宿主编译的设备侧代码** —— 这把无法证伪的"flash 内容可能不同"变成具体的通过/失败。

**第四轮 —— 备份必败:"Packet content transfer stopped / No serial data received",
重试从不恢复。** 三轮看似合理的补丁(分块读、重试前重同步、流水线 ACK 窗口)都没修好,
其中两次还引入回归(超过 stub 上限的读取块被静默忽略 → 会话假死;无超时的探针 → 串口
占死)。真正的根因只有读了 **stub 源码**(`flasher_stub/stub_commands.c`)才实锤:
`handle_flash_read` 在数据帧发完后**无条件**追加一帧 16 字节 MD5 digest,而久经验证的
参照实现(`esptool.py read_flash`)会读取并校验它 —— **esptool-js(所有版本,含我们的
vendored 副本)从不读这一帧**。残帧滞留在传输缓冲里毒化下一条命令的响应 → 协议错位随
每次 `readFlash` 调用累积 → 大量读取(备份)必败,且每次重试继承错位的缓冲 —— 这就是
盲重试永远无法恢复的原因。写入路径根本不走 readFlash,所以安装/升级一直正常而备份一直
失败。修复:逐帧 ACK(esptool.py 语义)、读取并校验 digest 帧(本地 MD5)、块大小 ≤ 4KB
(stub 硬上限,超限静默忽略)、以及回放 mock stub 的协议级单测
(`tools/install-slot/test-readflash-protocol.mjs`)。元教训:**当第三方客户端库与同一
协议的可用参照实现行为不一致时,先读服务端(stub/固件)源码和参照客户端,再考虑在坏
客户端外围修补失败处理** —— 重试逻辑永远无法补偿协议一致性 bug。

## 2. 经验教训(下次怎么做)

1. **先验签名者,再查传输路径。** "文件有效但设备不认"时,先用*真实*验签器证明文件本身
   (`tools/signing/run-verify-tests.sh` 编译 `main/meta_sign.c` + mbedtls),再沿写入路径回溯。
2. **设备读的是 flash,不是文件。** 任何针对 `.bin` 的测试都无法说明安装器在槽位偏移处写了什么。
   端到端的定义:源文件 → 安装器变换 → 组装后的槽位字节 → 设备验签器。
3. **一个扇区,一次写入。** esptool/esp_ota 按写擦除;对同一 4KB 扇区写两次 = 第一次静默丢失。
   这就是尾扇区(MSIG/MAEG/MNAM)必须在内存拼好、单次写入的原因
   (`meta-pass-signing-design.md` §7.1 单写契约)。
4. **副本必漂移;部署必滞后。** 仓库已修复时,滞后的 `https://meta-pass.pages.dev/` 旧页面
   仍造成真机症状。任何安装器/页面的两份副本都是常驻 bug(BUG-03):服务规范目录
   (`server.mjs` → `install-slot/`),且任何修复后要**逐字节校验线上部署的资源**,而非仓库里的。
5. **抄送已排除嫌疑清单。** 带证据的已排除项能防止反复走进死胡同
   (BUGS.md "cleared" 部分;handoff §2 "ruled out")。
6. **信任边界采用不对称严格性。** 接收第三方二进制的解析器(安装页)可以自动探测布局变体;
   签名/信任链内部的解析器(脚本、宿主测试、设备)对单一布局保持严格。16B 扩展头决策见 §4。
7. **HOST_TEST 变体就是产品代码。** `#ifdef HOST_TEST` 分支每次 `validate.sh` 都会编译 ——
   那里的 bug(魔数反转,BUG-02)即使没有设备路径执行它也是真实缺陷。要像其他代码一样
   给它回归测试。

## 3. 设备行为类 bug 的标准工作流

1. **低成本复现,能上宿主就别上设备。** 优先级:单测 → 真实模块的宿主集成测试
   (`run-verify-tests.sh`、`test_meta_net_upload.c`)→ QEMU 模拟器(`tools/sim/`)→ 最后才是真机。
2. **修复前先闭合证据环。** 对每一层(密钥链、image_len 语义、digest 范围、布局偏移)
   记录排除/确认它的检查 —— 格式见 handoff §2。
3. **静态分析搞不定就 dump 或回放字节。** flash dump 分流脚本与按结果分流表在
   `handoff-unsigned-rootcause.md` §3.3。
4. **修一类问题,不是一例。** 签名擦除的修复同时消除了"页面副本"这一类问题(单一来源),
   并加了锁定布局的回归测试(`tests/test_meta_net_upload.c` m1–m4 与 `sign-firmware.sh` 互锁)。
5. **过完整门禁交付,并把案例写下来。** `tools/validate.sh --static`,然后同一变更里更新
   `docs/BUGS.md` / handoff 文档(双语)。

## 4. 16B 扩展头问题 —— 背景与决策

**背景。** 四方契约(`meta-pass-signing-design.md` §8)要求签名脚本、宿主测试、安装页 JS、
IDF 的 `esp_image_verify` 推导出逐字节相同的 `image_len`。安装页额外自动探测 24B
`esp_image_header_t` 之后假想的"16B 扩展头"(`extract-app-image.js` 依次试 `[16, 0]`
两种布局,取 segment 表能走通的那个 —— 两种布局互斥,恰有一个收敛)。脚本与宿主测试
只解析纯 24B 布局。

**已确立的事实(IDF v5.5.3 + 真实镜像)。**
- `esp_app_format.h:110` 断言 `sizeof(esp_image_header_t) == 24`(packed);官方文档的
  esptool 示例显示 segment 0 位于文件偏移 `0x18` = 24。esptool 所称的 "Extended Image
  Header"(WP pin、flash pin drive settings、chip_id、min/max rev —— 字节 8..23)是 24B
  头的**内部**字段,不是头后附加的 16 字节 —— 这个命名陷阱很可能就是"多 16B"说法的来源。
- 真实 pass-radar 镜像:偏移 24 处段头为 `load_addr=0x3c0b0020`(合法 C3 DROM)、
  `data_len=147660`;image_len 962416 四方一致。对官方工具链镜像,ext=16 探测分支从不胜出,
  处于休眠状态。
- 即使这类镜像到达设备,IDF 自身没有探测:`esp_image_verify` 会在偏移 24 处读到伪段表,
  直接判槽位 INVALID。设备不能、也不应该接受非标准布局。

**决策(兼容性优先,2026-09-16 采纳)。** 把同样的 `[16, 0]` 自动探测移植进
`sign-firmware.sh`(`compute_esp_image_len()`)与 `tools/signing/test_integration.c`
(`esp_image_len()`),使三个解析器共享同一契约,§8 表不再列出已知不一致。理由:安装页是
第三方二进制的开放输入边界;让签名/验签链也能*解析*同样的变体(设备的
`esp_image_verify` 仍是拒绝畸形镜像的最终仲裁者),避免"页面接受、签名链无法处理"的契约
分裂。对官方工具链镜像探测处于休眠,故该变更对现有构建行为中立。行动项已登记在
`handoff-unsigned-rootcause.md` §4;尚未实现。

**实施记录(2026-09-16,已完成)。** 三个解析器均已实现 `[16, 0]` 探测。锁定测试:
`test_integration.c --selftest`(纯 24B → 240、24B+16B-ext → 256;fixture 以 0xab 填充,
错误布局会读出巨大 `data_len` 而自然回退)与 `test-extract.mjs` PASS 3c(同一 fixture +
不可解析截断镜像的负例)。对合成 fixture 的三方回放确认 Python/C/JS 结果完全一致。
fixture 的两个坑值得记住:`data_len` 必须按完整 u32 LE 写入(只写 1 字节会在高位留下
0xab,长度静默变大),C 测试缓冲区必须 memset —— JS 侧等价 fixture 能通过是因为
`Uint8Array.fill` 初始化了全部字节。变更后对真实 pass-radar 镜像重签,image_len 仍为
962416、验签 `META_SIG_OK`。

### 第五轮(vendored esptool-js 传输层):定时器与 Promise 的隐性自毁

**教训:库代码里的 `finally{}` 与未决 Promise 会在你以为早已结束的时刻杀死你。**

备份读取"约 2 分钟必死、重试恢复又静默挂死 30 分钟",前三轮在分块/重试/ACK 策略
层面打转,直到逐行读 vendor 压缩产物才定位到两个结构性缺陷:

1. `readLoop` 超时 throw 后 `finally{buffer=new Uint8Array(0)}` 清空传输缓冲 —— 长会话
   里这是**定时炸弹**:遗弃的 generator 超时定时器仍会触发,把正在流失效的数据整段
   吞掉。`FLASH_READ_TIMEOUT=100s` 与实测"~2 分钟死链"精确吻合,先前误判为 USB 抖动。
2. `flushInput()` 首行 `await this.reader.closed` 在活跃串口上**永不落定** —— 恢复路径
   一旦走到它就无限期挂死,连错误都不抛。这就是"重试(1/5) 后 30 分钟零输出"。

方法论沉淀:**排障超时链路时,先审计库内部所有 `Promise.race` 的落定路径与
`finally` 块的副作用,再怀疑外部因素**;对上层"加重试/加超时"无效的挂死,几乎必然是
库层有永不落定的 await。修复模式:未决 read 持久化(`_pendingRead` 复用)、generator
显式 `return()` 关闭、超时定时器附 `catch(()=>{})` 防未决拒绝、恢复链每步硬超时。
另外:给页面加 Debug 模式(每块耗时/恢复步骤)远比事后猜日志有效。

### 第六轮(2026-09-17):"stub 的 error/status 帧未被消费" —— 结论撤回,保留两条真教训

> **撤回声明(同日晚,源码实锤):**所谓独立 2 字节 error/status SLIP 帧在线上
> **不存在**。`stub_flasher.c` 对每条命令只调用一次 `SLIP_send_frame_delimiter()` ——
> 响应头、error、status 在**同一个 SLIP 帧内**;`checkCommand` 整帧消费。真机反证:
> 探针 4KB 读取 OK —— 若真有 +2 残帧,严格尺寸校验每次都会抛
> `Read more than expected: 4098 > 4096`,从未出现。完整撤回分析见
> `backup-readflash-error-status-frame.md`。

**真教训 1:给协议路径加严格不变量(精确读取长度)时,先数清对端发出的每一帧 ——
且必须读对端的**源码本身**,而不是对源码的转述。**第六轮的"验证"依据是摘要里的代码
片段而非真正的 C 源码 —— 与下面 mock 保真度教训同源。永远直接读服务端源码。

**真教训 2(方法论):单测 mock 没有逐字节还原真 stub 的帧序列,单测全绿而真机失败。**
规则:协议 mock 必须从服务端源码逐字节导出,包括客户端当前忽略的帧。

### 第七轮(2026-09-17):重试连败的根因 —— 恢复 ACK 给了 stub 不确定的退出条件;
恢复协议必须确定性,罚时要诚实定价

**教训 1:断言对端的退出条件前,必须对着对端源码核验 —— 对自己的恢复代码也一样。**
块读取失败后,stub 阻塞在 `handle_flash_read` 的 `SLIP_recv` 等下一个 ACK。第七轮最初
断言恢复 ACK `0x8000` 给了 stub "不确定的退出条件"(num_sent < 0x8000 时会继续吐流)
—— 重读 `stub_commands.c:110` 推翻了它:循环条件
`num_acked < len && num_acked <= num_sent` 在 `num_acked >= len` 时**已经**退出,所以
`ACK = chunkSize` 对 chunk ≤ 0x8000 本来就是确定性中止。该断言凭记忆作出,违反了
刚在上一轮学到的这条规则本身。真实遥测:退出后 digest 立即发出,但 desync 残流
会拖着尾音到达 —— 300ms 排空窗口收得过早,把毒帧留给了 sync。实际修复:排空静默
300→800ms;`ACK = 0xFFFFFFFF` 保留作无条件中止的加固(不依赖 chunk ≤ ACK 的
不变量);罚时诚实定价;重试 5→8。诚实的预期:单次失败罚时下降、级联减少 ——
而非底层瞬态失败率必然下降,后者在设备侧,客户端无法根除。

**教训 2:诚实为每次失败定价 —— 远超链路物理量的超时只会把瞬态错误放大成分钟级
罚时。** 921600 波特下 4KB 帧线时 ~44ms;数据帧超时却是 8 秒(物理量的 180 倍),恢复
预算 ≈16 秒。设备侧瞬态失败率 ~3-5%/块(USB-Serial-JTAG 丢字节;外加 stub 同步发送
digest 与重开异步接收之间的竞态可能整帧吞掉下一条命令 —— 客户端无法根除,重试是
正确答案),25% 的块失败 × 每次罚 ~10 秒。修复:数据帧超时 8s→1.5s(vendor)、恢复
sync 8s→1s、排空静默 300→800ms、重试 5→8 次(p⁸ ≈ 万分之一)。

**教训 3(证据纪律):对比实验必须同量同载。** esptool.py "5×128KB 零失败"被引为
"我们客户端有病"的证据 —— 但它的 read() **没有超时**(我们超时恢复之处它会永远
挂死),且 128KB 只相当于我们单槽 57 个恢复周期中的 1 个。小样本稀有事件证明不了
任何事;A/B 必须保持负载一致。

**禁止事项:commit 信息里写 AI 工具署名(本仓库历史中的此类署名已全部移除)。**
