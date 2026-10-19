# AGENTS.md — 86Box 开发指南（面向 AI 编码代理）

## 项目概览

86Box 是一个低级 x86 模拟器，运行 1981 年至约 2000-01 年间基于 IBM PC 及兼容机（至 PCI 总线时代）的旧操作系统与软件。许可证为 GPL-2.0-or-later。

- **语言标准**：C11（核心）+ C++17（Qt 界面与部分模块）
- **构建系统**：CMake ≥ 3.16 + Ninja + vcpkg（依赖清单见 `vcpkg.json`）
- **仅支持 64 位小端构建**（根 `CMakeLists.txt` 强制检查）
- **主机平台**：Windows、Linux、macOS（含 ARM64），以及 *BSD、Haiku

## 仓库结构

```
src/
  86box.c            主模拟器控制模块
  include/86box/     全部公共头文件（新增头文件放这里）
  cpu/               CPU 解释器与 dynarec（含 softfloat3e）
  codegen/           动态重编译后端
  machine/           机器（主板）定义
  chipset/  mem/  io.c  pci.c  dma.c  pit.c  pic.c  nvr*.c
  video/  sound/  disk/  cdrom/  floppy/  scsi/  network/  sio/  printer/  game/  device/
                     各外设子系统（每个子系统有自己的 CMakeLists.txt）
  qt/                Qt 前端（默认 UI，Qt5 默认，USE_QT6=ON 切 Qt6）
  unix/              Unix/SDL 前端
  mac/               macOS 平台代码
  utils/             INI 解析等通用工具
cmake/               工具链与平台 cmake 模块
tests/               单元测试与基准（结构镜像 src/），见 tests/README.md
doc/hardware/        硬件实现与验证笔记
scripts/             辅助脚本
wl_protocols/        Wayland 协议
```

## 构建

### 常用命令（根目录 Makefile 封装）

```sh
make                # = make regular，Release + Qt
make debug          # Debug
make development    # Release + DEV_BRANCH（实验硬件）
make dev_debug      # Debug + DEV_BRANCH
make optimized      # Optimized
make qt=6           # 任意目标附加：使用 Qt6
make dynarec=off    # 关闭动态重编译
make clean          # 删除 build/
```

### 直接使用 CMake preset

```sh
cmake --preset regular          # 配置到 build/regular
cmake --build build/regular
```

preset 一览：`regular`、`debug`、`optimized`、`development`、`dev_debug`、`ultra_debug`，另有 macOS/ARM64 专用 preset（见 `CMakePresets.json`）。

### 关键 CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `QT` | ON | Qt 界面（OFF 时走 SDL 前端） |
| `USE_QT6` | OFF（vcpkg 构建为 ON） | Qt6 替代 Qt5 |
| `DYNAREC` | ON | 动态重编译 |
| `DEV_BRANCH` | OFF | 启用开发分支实验特性（AMD_K5、G100、WACOM 等） |
| `BUILD_TESTING` | OFF | 构建单元测试（需要 GTest） |
| `BUILD_BENCHMARKS` | OFF | 构建性能基准（需要 google benchmark） |
| `GDBSTUB` | OFF | GDB 调试桩（强制关闭 DYNAREC） |
| `VNC` | OFF | VNC 渲染器 |
| `OPENAL` / `RTMIDI` / `FLUIDSYNTH` / `MUNT` / `SOUNDCANVAS` | ON | 音频相关可选依赖 |
| `LIBASAN` | OFF | AddressSanitizer |

## 测试

```sh
cmake --preset debug -DBUILD_TESTING=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure            # 全量
ctest --test-dir build/debug -R '^Ini' --output-on-failure  # 按正则筛选
```

约定（详见 `tests/README.md`）：

- 测试目录结构镜像 `src/`，文件名 = 被测文件名 + `_test.cpp` / `_benchmark.cpp` 后缀。
- 设备测试惯例：设备实现以 C-in-C++ 方式包含进夹具以访问私有函数，依赖（定时器、PIC、DMA、镜像）用桩/mock。
- 部分测试需要 Vulkan 设备或外部镜像，无法运行时以退出码 77 报告 skipped。
- 改动 CPU、缓存/TLB、软驱、CD-ROM、网卡等已有测试覆盖的子系统时，必须先跑对应套件。

## 代码风格

- 格式化以根目录 `.clang-format` 为准（基于 WebKit）：4 空格缩进、指针右对齐（`char *p`）、case 标签与块均缩进、数组结构体左对齐、不自动排序 include。`.editorconfig`：UTF-8、LF、4 空格。
- **新文件必须带标准版权头**（仿照 `src/86box.c` 的块注释格式：项目说明、模块用途、Authors、Copyright 年份）。
- 日志统一用 `pclog()` / `warning()` / `fatal()`（声明于 `src/include/86box/86box.h`），禁止散落的 `printf`/`fprintf`。
- **提交代码必须零警告编译**。
- 设备实现遵循 `device_t` 模型（见 `src/include/86box/device.h`），新硬件参考同子系统现有设备。
- 模拟要按真实硬件行为实现，不要从驱动期望反推。

## 贡献红线（摘自 CONTRIBUTING.md，必须遵守）

1. 新模拟硬件须在项目范围内：x86 机器及生态，**最晚约 2000-01 年的硬件**。
2. 新硬件必须附带 ROM dump 并提交到 [86Box/roms](https://github.com/86Box/roms) 仓库。
3. 破坏上一稳定版配置的改动必须提供配置迁移逻辑。
4. UI 改动需在 PR 描述中附可视化演示。
5. 不提交半成品；引入新构建依赖须同步更新文档仓库。
6. AI 生成的代码须在 PR 描述中披露；提交前移除 AI 产生的计划、总结、测试说明等工件。

## 给代理的工作守则

- 修改某子系统前，先读该子系统的 `CMakeLists.txt` 与 `doc/hardware/` 下对应笔记（如有），保持与现有设备实现的一致性。
- 不要改动与本任务无关的文件；不要引入新依赖，除非任务明确要求（新依赖需同时更新 `vcpkg.json` 与文档）。
- 涉及 dynarec/codegen 的改动要同时考虑 x86-64 与 ARM64 主机后端。
- 配置键（INI，`src/config.c`）变更属于兼容性敏感区，必须提供迁移。
- 验证标准：至少通过 `make debug` 零警告编译；有对应测试套件的跑对应 ctest 正则。
