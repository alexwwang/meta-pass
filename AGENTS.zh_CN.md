<p align="right">
  <strong>简体中文</strong> · <a href="AGENTS.md">English</a>
</p>

# AI Agent 仓库规范

本文是本仓库 AI 辅助工作的唯一必读入口。根据下方路由表读取当前任务所需文档，不要默认加载全部 README。

## 项目与安全基线

- 目标平台：ESP32-C3、8 MB Flash、无 PSRAM、ESP-IDF 5.5.3。
- 必须保持受保护的 Flash 布局：3 MB 应用上限与 `cardid@0x356000`
  均为模板强制契约。
- 保留用户已有修改。先执行 `git status --short --branch`，不得覆盖或清理无关文件。
- 硬件事实优先级：产品规格与实测结果 → `components/bsp/include/bsp_pins.h` → BSP 头文件与实现 → 硬件指南 → README/demo。任务所需硬件细节未在这些来源中定义时，直接询问用户，不得猜测。
- 可复用板级逻辑放入 `components/bsp`；页面、状态机、动画和应用任务放入 `main`。
- LVGL 非线程安全。LVGL 任务之外访问 LVGL 对象时必须持有 `bsp_lvgl_lock()`。
- 按键回调不得阻塞。音频、存储、网络等慢操作必须放入工作任务。
- demo 删除 screen 前，必须停止所有可能访问其 UI 的任务、定时器、回调和事件处理器。
- 系统级策略（开机策略、otadata 状态、Flash 布局）必须在不可绕过的层（bootloader 或校验器）强制执行，不得依赖子固件配合；子固件 hook 仅作纵深防御。
- 交付固件或 bootloader 改动前，必须验证改动真实存在于构建产物中（map 文件符号、日志字符串）——编译通过不等于已链接。
- 测试与静态门禁不得依赖本地未提交的构建状态；`build/` 产物缺失时（CI 裸 checkout），测试回退到合成 fixture 并保持同等断言。
- 可测试的状态机、协议、计时和布局计算应与 ESP-IDF/LVGL 解耦，并由 host tests 覆盖。
- 禁止提交凭证、设备二维码秘密、私钥、个人数据或未脱敏日志。
- 所有维护中的 Markdown 默认 `.md` 路径必须为英文，简体中文使用配对的 `.zh_CN.md` 文件。两种语言必须保持一致并保留互相切换链接。

## 按任务加载上下文

| 任务 | 修改前读取 |
| --- | --- |
| 任意代码修改 | `docs/development/ai-guide.zh_CN.md`、相关头文件和相邻实现 |
| 环境引导或缺少工具链 | `docs/development/engineering/environment-setup.zh_CN.md` |
| BSP、引脚、总线、显示、音频、电池 | `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md`、`components/bsp/include/bsp_pins.h` |
| 启动器 UI 或菜单 | `main/main.c`、`components/bsp/` 的按键与显示 API |
| 构建、测试、依赖、分区 | `docs/development/engineering/build-and-test.zh_CN.md`、`docs/development/engineering/protected-flash-layout.zh_CN.md`、`sdkconfig.defaults`、`partitions.csv` |
| 排查设备行为或签名/安装器问题 | `docs/development/engineering/debugging-workflow.zh_CN.md`、`docs/BUGS.zh_CN.md`、`docs/assets/handoff-unsigned-rootcause.zh_CN.md` |
| CI 或发布 | `docs/development/ci/CI-*.zh_CN.md` 中的对应文件与 `.github/workflows/` |
| 项目开发完成 | `./tools/validate.sh` 全门禁 + `docs/assets/meta-pass-design.zh_CN.md` §10 验收清单 |
| 文档 | `docs/contribution/doc-conventions.zh_CN.md`、`docs/README.zh_CN.md` |
| Commit 或 PR | `docs/contribution/commit-and-pr.zh_CN.md` |

项目总览见根 `README.zh_CN.md`，文档索引见 `docs/README.zh_CN.md`。详细的 AI 开发工作流（上下文建立、事实来源优先级、应用/BSP 边界、运行时规则、素材放置、交付格式）见 `docs/development/ai-guide.zh_CN.md`。`docs/fork-guide.zh_CN.md` 为上游对 fork 约定的背景参考。

## 必须执行的验证与交付格式

迭代时运行最小相关检查，交付前运行完整门禁：

```bash
./tools/validate.sh --static    # 仓库检查 + host tests
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像验证
./tools/validate.sh             # 完整门禁
```

完整门禁要求已激活 ESP-IDF 5.5.3 环境。不得把编译成功描述成硬件验证成功。最终交付必须分别报告：

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: 仍需板卡、仪器或用户确认的事项
```

仅在用户请求或当前工作流明确要求时创建 commit 和 push。用户可见变化记录到 `docs/CHANGELOG.zh_CN.md`；内部重构、CI 维护、拼写修复和生成文件刷新无需记录。

Commit 信息中禁止任何 AI/agent 署名尾注——不得出现 `Co-Authored-By:`（指名工具或
agent）或 `Generated with ...` 行。本仓库历史曾于 2026-09-17 专门重写以清除此类
尾注，不得再次引入。

社区规范见 `.github/CONTRIBUTING.zh_CN.md`、`.github/CODE_OF_CONDUCT.zh_CN.md`、`.github/SECURITY.zh_CN.md` 与 `.github/SUPPORT.zh_CN.md`。
