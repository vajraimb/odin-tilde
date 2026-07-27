# AGENTS.md — Tildebackend 项目协作指引

本项目为 Odin 编译器的 tilde (TB) 后端移植/恢复工程，复杂度高、构建链路长。
本文件约束 agent 的工作方式，**首要目标：用最少的步数完成每个回合**（单回合有步数上限，超限会被强制中断）。

## 项目结构

- `Odin/` — Odin 编译器源码（tilde 后端在 `Odin/src/tilde*.cpp`、`Odin/src/tilde/`）
- `Cuik/` — Cuik/TB 库上游源码（`Cuik/tb/`、`Cuik/common/` 等）
- `tb-macos-build/` — TB 库的 macOS 构建目录（含 `shim/`，产物 `libtb_macos.a` / `libtb_macos_dbg.a`）
- `tests/` — 测试（`tb_harness/`、`hello/`）
- `scripts/` — **固化构建脚本，构建必须走这里**

## 构建规则（最重要）

**禁止手敲临时编译命令。** 一律使用固化脚本，一次调用完成整个构建：

| 目的 | 命令 |
|---|---|
| 重编 TB 库 (debug) | `sh scripts/build_tb.sh debug` |
| 重编 TB 库 (release) | `sh scripts/build_tb.sh release` |
| 重编 Odin (debug) | `sh scripts/build_odin.sh debug` |
| 重编 Odin (release) | `sh scripts/build_odin.sh release` |
| 编译/运行 harness | `sh scripts/build_harness.sh [run]` |

- 脚本输出以 `*_OK` / `*_FAIL` 结尾，先看结尾标记再决定是否展开日志。
- 构建失败后**先定位根因再改代码**，不要改一行就全量重编一次碰运气。
- 改了 `Cuik/` 下的源码才需要重编 TB 库；只改 `Odin/src/` 时直接 build_odin，不要重编 TB。

## 编辑规则

- 修改源码**必须用 Edit 工具**，禁止用 Bash heredoc + python/sed 做文本替换。
- 修改前先 Read 目标文件的相关片段，不要凭记忆猜 old_string。

## 长任务与进度管理

单回合步数有上限（当前 500）。复杂任务必须：

1. **开工即建/更新 `PROGRESS.md`**（项目根目录），内容包含：
   - 总体目标与当前里程碑
   - 已完成事项与关键决策（为什么这么做）
   - 关键文件与行号索引（例：`Cuik/tb/x64/x64_target.c:1234 — 寄存器分配入口`）
   - 下一步具体动作
2. **一个回合只推一个里程碑**；预计步数将尽时主动收尾：把进度写入 PROGRESS.md，明确告诉用户"发『继续』即可接着做"。
3. **上下文压缩（compaction）发生后，先读 PROGRESS.md 恢复状态**，禁止重新通读 `x64_target.c` 等大文件——PROGRESS.md 里应有行号索引，按需定点读取。
4. TodoList 只在状态实际变化时更新，不要重复发送相同内容。

## 调试

- lldb 批处理调试使用固定模式，例如：
  `lldb -b -o run -o bt -o quit -- <程序> 2>/dev/null | grep -E "assertion|frame #[0-5]" | head -8`
- 调试输出重定向到 `/tmp/` 下的日志文件，用 grep 提取关键行，不要把整份日志倒进上下文。
