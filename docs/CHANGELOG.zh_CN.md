<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## Unreleased
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
