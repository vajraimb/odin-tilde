# Odin + tilde (Cuik TB) backend — Beta Quickstart

This fork of the [Odin](https://odin-lang.org) compiler adds a second code-generation
backend: **tilde**, built on [Cuik](https://github.com/RealNeGate/Cuik)'s TB
(TildeBackend) IR. The stock LLVM backend is untouched — you choose the backend
per invocation with the `-tilde` flag.

**Status: public beta.** Validated on macOS (x86_64; Apple Silicon machines run it
under Rosetta 2). Other platforms are not yet verified — see
[Known limitations](#known-limitations).

---

## Requirements

- macOS with Xcode Command Line Tools (`clang`)
- On Apple Silicon: Rosetta 2 (`softwareupdate --install-rosetta`) — all current
  artifacts are x86_64
- Two sibling clones:

```sh
mkdir odin-tilde-workspace && cd odin-tilde-workspace
git clone https://github.com/vajraimb/odin-tilde.git
git clone https://github.com/vajraimb/cuik-tilde.git   # TB library, required
```

## Build

```sh
cd odin-tilde
sh tilde/scripts/build_tb.sh release      # builds the TB static library
sh tilde/scripts/build_odin.sh release    # builds ./odin with -tilde support
```

Use `debug` instead of `release` for assertion-enabled builds (slower, but much
better diagnostics if you hit a compiler bug — please report with a debug build's
output).

## Usage

The `-tilde` flag selects the backend; everything else is stock Odin:

```sh
./odin build . -tilde                    # compile with the tilde backend
./odin run   . -tilde
./odin test  tests/core/os -tilde        # run a package's tests through tilde
```

Without `-tilde` you get the stock LLVM backend, so one binary serves both.
A ready-made wrapper keeps tilde one command away without touching your stock
`odin` install — symlink it onto your PATH:

```sh
ln -s "$(pwd)/tilde/scripts/odin-tilde" ~/.local/bin/odin-tilde
odin-tilde build .        # = odin build . -tilde -thread-count:1
odin-tilde test  tests/core/os
```

The wrapper appends the flags *after* your arguments (Odin's parser accepts
flags there) and pins `-thread-count:1` for reproducible builds; pass an
explicit `-llvm`, `-tilde`, or `-thread-count:N` to override.

## What has been verified (2026-08)

- **31 `core` packages** pass `odin test -tilde` with zero failures, including
  strings, strconv, bytes, slice, fmt, math (+bits, big, noise), time, hash, mem,
  unicode, reflect, compress, container, crypto (48/48: RSA, mlkem, mldsa, aegis…),
  encoding (12 subpackages, 114 tests), os, net, flags, sync, thread, runtime,
  c/libc, sys/kqueue, sys/posix, testing, odin, fixed, rand, nbio.
- **38-case dual-backend regression suite**: tilde vs LLVM output compared
  byte-for-byte.
- **Real-world server**: [odin-http](https://github.com/laytan/odin-http) example
  served ~280,000 HTTP requests under `ab -n 20000 -c 100` across repeated runs —
  zero failed requests, zero runtime assertions. Throughput ≈ 60–67 % of the
  LLVM build on this I/O- and logging-bound workload (both at `-o:none` and
  `-o:speed`, on Rosetta).
- **Tooling**: `ols` (Odin language server) built with tilde passes an LSP
  feature comparison at the LLVM-vs-LLVM noise floor; `odinfmt` built with tilde
  passes differential formatting tests.
- Single-threaded builds (`-thread-count:1`) are byte-for-byte reproducible.

## Known limitations

- **Platform coverage**: only macOS x86_64 (incl. Rosetta on Apple Silicon) has
  been exercised. Native arm64, Linux, and Windows are untested; the aarch64
  paths in the backend are known to have rough edges.
- **C ABI corner cases**: mixed-class 16-byte struct returns (e.g. `{i64, f64}`)
  can read the wrong register; `complex` values inside `c_vararg` calls are not
  split per the SysV eightbyte rules.
- **Compiler capacity**: a single TB node with more than 65,535 users still
  panics (clamped, not fixed — needs an upstream redesign).
- **Multi-threaded builds** (`-thread-count:N`, N>1) are not reproducible —
  upstream checker nondeterminism that also affects the LLVM backend.
- **`@(thread_local)`** works via a pthread-key shim (one `pthread_getspecific`
  per access); a true Mach-O TLV sequence is a future optimization.
- **`-o:speed`** is not yet deeply wired into TB's optimizer — expect roughly
  `-o:none` performance today.
- `core/image` tests need downloadable assets; `core/path/filepath` tests expect
  a specific Odin version — both fail identically under LLVM, i.e. not backend
  bugs.

## Reporting issues

Please open an issue at <https://github.com/vajraimb/odin-tilde/issues> with:

1. the exact command line,
2. a minimal reproducing `.odin` file (or the package + test name),
3. whether the **LLVM build of the same source** behaves differently,
4. for crashes, the assertion message from a **debug** build.

Backend bugs are the interesting ones — if LLVM and tilde disagree, we want to
know.

---

## 中文摘要

本 fork 为 Odin 编译器增加了基于 Cuik TB 的 **tilde 后端**（LLVM 后端保持原样，
`-tilde` 标志按次切换）。当前为 **公开 beta**：仅在 macOS x86_64（含 Apple
Silicon 跑 Rosetta 2）上验证过。

```sh
git clone https://github.com/vajraimb/odin-tilde.git
git clone https://github.com/vajraimb/cuik-tilde.git   # 必须并排放置
cd odin-tilde
sh tilde/scripts/build_tb.sh release
sh tilde/scripts/build_odin.sh release
./odin build . -tilde
```

已验证：31 个 core 包测试全绿、38 探针双后端逐字节一致、odin-http 实测
28 万请求零失败（吞吐约为 LLVM 的 60–67%）、ols / odinfmt 工具链对比通过。
已知限制：arm64 原生 / Linux / Windows 未验证；C ABI 混合类 16 字节返回、
vararg 中 complex 有角落问题；多线程构建不可复现（上游问题）；`-o:speed`
尚未接入 TB 优化器。遇到问题请到 odin-tilde 仓提 issue，附上命令行、最小
复现、以及 LLVM 后端对照结果。
