# 86Box Voodoo 虚拟 GPU 优化计划（AVX-512 / x86-64 v4）

> 目标机器：本机 CPU 支持 x86-64 v4 微架构级别（含 AVX-512F/BW/DQ/VL 等）。
> 本计划只含规划，所有实施操作由人工执行。
> 生成日期：2026-10-19；基于 86Box master @ 316fe15d8 的代码侦察。
> 复核修订：2026-10-19，修正函数定位与默认路径判断（见文末"修订记录"）。

---

## 1. 现状（代码侦察结论）

| 项目 | 现状 | 证据 |
|---|---|---|
| 渲染方式 | 纯主机 CPU 软件光栅化，无主机 GPU 参与 | `src/video/vid_voodoo*` 无 vulkan/opengl 引用 |
| 渲染路径 | **两条互斥路径**：默认 `recompiler=1` 时 span 内层由 JIT 生成代码执行；关闭 recompiler 或取块失败才进入 C 解释器的 span 循环 | `vid_voodoo_render.c:923-928`、`:1094-1099`；配置默认值 `vid_voodoo.c:1913-1917` |
| JIT | 运行时发射 x86-64 指令流，**已使用 128-bit SSE2 打包指令（XMM）**，主要用于双线性纹理过滤；无 AVX/AVX-512 编码；按渲染状态特化，8 个 block 缓存 | `vid_voodoo_codegen_x86-64.h`（3646 行；`__m128i` 表 `alookup/aminuslookup/bilinear_lookup` 在 :77-85，SSE2 发射见 :435-565 等；`BLOCK_NUM 8` 在 :13-15） |
| C 解释器 | span 循环纯标量，无 SIMD intrinsics | `src/video/vid_voodoo_render.c` 无 `__m128/_mm_`（仅 :1759 一处 `yield` 内联汇编） |
| 编译基线 | Release = `-march=x86-64 -msse2`；仅 Optimized preset = `-march=native` | `cmake/CompilerFlags.cmake:22,45-49` |
| 线程模型 | 主线程 MMIO → FIFO 线程 → 1/2/4 渲染线程；默认 2 线程，按扫描线奇偶分摊，4 线程按 `real_y mod 4` 分摊 | `vid_voodoo.c:1299-1324`；`vid_voodoo.c:1212-1213, 1885-1899`；`vid_voodoo_render.c:971-974, 1721-1766` |
| 纹理解码 | 每次纹理缓存未命中时，把 8/16 位纹素整层展开为 RGBA32 存入 64 项缓存 | `vid_voodoo_texture.c:289-605`（LOD 循环 :366，格式分支与 x 内层循环 :378 起），`TEX_CACHE_MAX 64` |
| mip 布局计算 | 只计算每级 LOD 的 base/mask/shift 元数据，不解码纹素 | `voodoo_recalc_tex12()` `vid_voodoo_texture.c:108-192`；`voodoo_recalc_tex3()` `:195-284` |
| 扫描输出 | 默认每扫描线 `video_16to32[src[x]]` 查表转换（65536 项表）；可选 scrfilter 走 3 通道滤波 | `vid_voodoo_display.c:554-570` |
| 单元测试 | Voodoo 无测试覆盖（tests/video 仅 s3/mach64/wd90c31/rage128） | `tests/video/CMakeLists.txt` |

**关键结论：默认配置下负载最重的逐像素路径是 JIT 生成的 span 代码，不是 C 解释器。** 任何只改 C 侧的计划都必须先证明该路径在目标场景中确实被执行。

## 2. 目标与约束

**目标**：在 x86-64 v4 主机上提升 Voodoo（1/2/Banshee/3）渲染吞吐，目标场景为高分辨率（800×600+）Glide/D3D 游戏。

**约束（红线）**：

1. **位精确**：86Box 是 accuracy-first 项目。所有插值为定点整数，只能用整数 SIMD，禁止改用 float SIMD。向量路径必须与标量路径逐像素一致。
2. **上游兼容**：Release 基线（x86-64 + SSE2）必须可编译、零警告；AVX-512 代码只能走**运行时检测分派**，不得要求编译期 `-march`。
3. **AI 披露**：若提交上游，AI 生成代码须在 PR 描述中披露（CONTRIBUTING.md 要求）。

## 3. 分阶段计划

### P0 — 基线构建与热点画像（约 0.5-1 人日）

目的：用数据决定后续投入方向，避免盲改。**本阶段必须区分两条渲染路径，否则后续所有判断都会失真。**

1. 构建优化版：`make optimized`（自带 `-march=native`，零改动先吃到 GCC 自动向量化的收益）。
2. 分别画像两组配置，各跑至少 3 个代表性负载（Glide 游戏、D3D 游戏、高分辨率场景）：
   - 默认组：`recompiler=1`（默认）、`render_threads=2`（默认）、`scrfilter` 关闭（默认）。
   - 对照 A：`recompiler=0`，用来评估 C span 路径的实际占比。
   - 对照 B：开启 `scrfilter`，用来评估 P2 的目标路径是否值得投入。
3. 记录指标：
   - 帧率，以及代码已有的 `render_time[]` / `pixel_count[]` / `texel_count[]` / `fbiPixelsIn/Out`。
   - JIT span 代码与 C span 路径各自的执行时间占比（用 perf 按符号区分：JIT 代码块无符号名，可用 `perf map` 或 `voodoo_recomp` 计数辅助）。
   - **纹理缓存未命中率**：每帧实际解码的 texel 数占总纹素访问的比例。这是 P1 收益的唯一决定因素。
   - `scrfilterEnabled` 在目标场景中的实际开关状态。
4. 产出：热点函数排名表 + 上述对照数据。**判定门槛**：
   - 若默认配置下 JIT span 代码占比 ≥ 40%，则 P3（只作用于 C 解释器路径）对默认用户没有直接收益，应直接评估 P4；P3 降级为 P4 的算法预研。
   - 若纹理缓存未命中率很低（解码被缓存充分摊销），P1 优先级下调。
   - 若 `scrfilter` 在目标场景中未开启，P2 收益为零，直接跳过。
   - 若 C span 占比 < 40% 且默认 JIT 占主导，P1/P2 仍可做（它们是 setup/扫描输出路径），但不要指望帧率大幅提升。

### P1 — 纹理解码向量化（约 1-2 人日，收益取决于缓存未命中率）

**目标函数**：`voodoo_use_texture()`（`src/video/vid_voodoo_texture.c:289-605`），核心是 `:366` 的 LOD 循环与 `:378` 起按 `tformat` 分派的各格式 x 内层循环。

循环特征：每个 texel 从 `tex_mem[tmu]` 取 8/16 位，查表或位运算展开为 RGBA32（`makergba`，定义在 :286），写入 `texture_cache[][].data`。逐 texel 无跨像素依赖，**但外层存在 LOD 与格式分支，且行步长由 `tex_shift` 决定，行间可能不连续**。

适合 SIMD 的格式（共 14 种，见 `vid_voodoo_regs.h:405-418`）：

- 纯位运算、无查表：`TEX_A8`、`TEX_I8`、`TEX_AI8`、`TEX_APAL8`（APAL8 的通道拼接逻辑见 :459-464）。
- 定长查表：`TEX_RGB332`、`TEX_PAL8`、`TEX_Y4I2Q2`（NCC 表）、`TEX_ARGB1555`、`TEX_ARGB4444`、`TEX_RGB565`。后三种的 5/5/5、4/4/4、5/6/5 展开可用纯整数 SIMD；调色板类需要 `vpgatherdd` 或按通道拆分查表。
- 8/16 位组合格式（`TEX_APAL88`、`TEX_A8I8` 等）按实际特化程度决定是否纳入。

**明确不要作为目标**：

- `voodoo_recalc_tex12()` / `voodoo_recalc_tex3()` **不是解码函数**，只计算每级 mip 的 `tex_base/tex_end/tex_w_mask/tex_h_mask/tex_shift/tex_lod`。循环次数 ≤ 10，分支密集，每次纹理参数变化才调用一次，向量化无意义。
- `voodoo_update_ncc()`（`vid_voodoo_display.c:60-102`）只构建 256×2 项查找表，一次性调用，向量化收益可忽略。

**方案**：

- 首选 `__attribute__((target_clones("default", "avx512f")))` 函数多版本；GCC 不支持的目标平台改用独立编译单元 + `cpuid` 运行时分派。
- 按 `tformat` 分特化函数，避免在向量循环内保留 switch。
- 行尾不足一个向量宽度时走标量收尾；小 mip 层（宽度 < 16）直接走标量。
- 宏开关 `VOODOO_TEX_AVX512`，默认 ON，可一键回退。

**验证**：对全部 14 种 tformat，用同一纹理输入对比标量/向量实现解码结果，`memcmp` 字节级一致；覆盖 1×1、非 2 的幂、`tex_shift` 导致的行间不连续等边界。收益验证必须在**纹理缓存未命中率高的场景**下做帧率对比，并同时给出缓存命中率高时的数据（预期无提升）。

### P2 — 扫描滤镜与行转换（约 0.5-1 人日，**有前提条件**）

**前提**：`voodoo_filterline_v1()` / `voodoo_filterline_v2()` 只在 `voodoo->scrfilter && voodoo->scrfilterEnabled` 时被调用（`vid_voodoo_display.c:554-565`）。这是**可选的 Voodoo 屏幕滤镜（去抖动滤镜），默认关闭**。若目标场景不开启该滤镜，P2 收益为零，应直接跳过。

**目标函数**：`voodoo_filterline_v1()`（:388-443）、`voodoo_filterline_v2()`（:444-505）。

循环特征：每行对 3 通道交错字节做 256×256 表查找，且**存在行内前向依赖**——当前像素的滤波结果依赖前一像素的滤波结果（`fil[x*3] = thefilterb[fil[x*3]][fil[(x-1)*3]]`）。这不是"无依赖的规则变换"，SIMD 化需要处理前缀依赖（分段并行或重排），难度高于原估。实际可向量化的是三通道之间的并行（同像素的 R/G/B 独立），而不是跨像素。

**次要目标**：`voodoo_calc_clutData()`（:128-149）构建 256 + 65536 项表，仅在 CLUT/DAC 变化时调用，属 setup 路径。可作为低风险练手，但**不要计入性能预期**。

**关于默认扫描输出**：`vid_voodoo_display.c:567-569` 的 `p[x] = video_16to32[src[x]]` 才是默认配置下每帧 C 侧热点。它是每像素 2 次内存操作（读表 + 写目标）的查表转换，内存带宽受限；AVX-512 `vpgatherdd` 通常不优于标量顺序访问。P0 画像确认后再决定是否值得尝试，优先级低于 JIT。

**验证**：同场景同帧，滤波后扫描线输出逐像素 diff 为零（需在 `scrfilterEnabled=1` 下采集基准）。

### P3 — span 内层循环向量原型（研究性，约 3-5 人日）

**适用路径（重要）**：P3 只对 **C 解释器** 生效。默认配置 `recompiler=1` 时，`vid_voodoo_render.c:1094-1099` 会跳过整个 C span 循环并直接调用 JIT 生成的 `voodoo_draw()`。因此：

- 对默认用户配置，P3 不会带来任何收益。
- P3 的直接收益只能在 `recompiler=0` 下测量。
- P3 的真正价值是**为 P4 提供算法原型**：证明哪些状态组合适合 16-wide、掩码语义如何映射、gather 是否划算。若 P0 显示默认配置由 JIT 主导，应把 P3 定位为 P4 的预研，而不是独立的性能交付项。

**前置条件**：P0 已完成；若默认配置 JIT span 占比 ≥ 40%，P3 不作为独立优化项，直接并入 P4 预研。

**范围限定**（控制爆炸半径）：只对最常见状态组合做特化原型——无 z-test、无 alpha blend、无 fog、单 TMU；其余状态回落标量路径。

**技术要点**：

- 插值：`ir/ig/ib/ia/z/s/t/w` 为逐像素定点累加链 → 用 16 像素增量向量构造，再做前缀和形式展开（注意与标量的溢出/截断顺序严格一致）。
- 纹理取址：`vpgatherdd` 从已解码的 RGBA32 缓存取值。**需要实测**：纹理缓存通常能驻留 L1/L2，gather 的吞吐在多代 CPU 上未必优于标量加载，不能先验假设收益。
- z-test / alpha-test / stipple：AVX-512 mask 寄存器 + 掩码存储天然匹配，这是相对标量最确定的收益点。
- span 过短（< 16 像素）时回落标量，避免向量利用率反噬。
- 双线性过滤已有 SSE2 实现（见 P4 证据），P3 不要重复造轮子；先复用现有 128-bit 逻辑，再评估扩到 512-bit 是否划算。

**验证**：与标量路径同场景逐像素 diff 必须为零；在 `recompiler=0` 下帧率提升 ≥ 20% 才算原型成功。若该算法无法映射到 P4，即使 C 路径有收益也不应作为最终交付。

### P4 — JIT 向量后端（P3 验证后启动）

**不是从零开始**：现有 x86-64 codegen 已具备 SSE2 发射能力与配套数据：

- `__m128i` 查找表：`alookup[257]`、`aminuslookup[256]`、`bilinear_lookup[256*2]`、`xmm_01_w/xmm_ff_w/xmm_ff_b`（`vid_voodoo_codegen_x86-64.h:74-85`）。
- 已发射的打包指令：`PMULLW`、`PADDW`、`PSUBW`、`PSRLW`、`PACKUSWB`、`PACKSSDW`、`PUNPCKLBW`、`PUNPCKLWD`、`PUNPCKLDQ`、`PINSRW`、`PSHUFLW`、`PXOR` 等（:435-565、:1086-1304、:1386-1661、:1716-1812、:1943-2072、:2129-2339、:2574-3095、:3326-3488）。
- 8 个 code block 缓存，`BLOCK_SIZE 8192`（:13-15）。

因此 P4 应定义为**在现有发射器上增加 AVX-512 路径**，而不是"新建向量后端"：复用现有寄存器分配与编码辅助，新增 512-bit 变体和运行时 CPUID 分派。block 缓存键必须包含 ISA 选择，避免缓存串用。

## 4. 整体验证方案

| 类型 | 方法 |
|---|---|
| 正确性 | 双实现字节级/像素级对比（核心手段，因 Voodoo 无单元测试）；纹理解码覆盖全部 14 种 tformat 与 LOD 边界；span 对比覆盖有/无 z、alpha、fog、双 TMU 的状态组合 |
| 基准 | 参照 `tests/video/vid_ati_mach64_*_test.c` 的自包含 C-in-C++ 模式，用 `BUILD_BENCHMARKS=ON` 自建 Voodoo benchmark；必须分别测量 `recompiler=1/0` 与 `scrfilter=on/off` 四种组合 |
| 编译回归 | `make debug` 与 `make regular`（基线 -march=x86-64）均零警告；`ctest` 全量通过 |
| 实机回归 | 至少 3 个游戏场景截图 diff + 帧率记录，前后对比；记录 CPU 型号与 AVX-512 降频行为 |

## 5. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| **P1/P2/P3 改动不触及默认配置的热点** | **高** | P0 必须先按 `recompiler` 分路径画像；P3 明确限定为 C 路径/P4 预研 |
| span 循环短三角形导致向量利用率低 | 高 | P3 先做原型验证，设 ≥20% 收益门槛，不达标即弃 |
| AVX-512 gather 在 L1/L2 驻留数据上不优于标量 | 中高 | P3 原型中做 A/B 实测，不先验假设；优先做 mask 运算类收益确定的优化 |
| 上游拒收 AVX-512 路径 | 中 | 运行时检测分派 + 宏开关，保持基线构建纯净；fork 先行验证 |
| 定点改 float 引入精度偏差 | 高（禁止） | 只用整数 SIMD；memcmp/像素 diff 零容忍 |
| AVX-512 降频拖累同核其他线程 | 低 | 本机为现代 v4 CPU（Zen4/5 或 Ice Lake+），降频惩罚可忽略；仍需实测 |
| 纹理解码受内存带宽封顶 | 中 | P0 记录 cache miss；P1 以缓存未命中率为收益前提 |
| 行内前向依赖使 filterline 难以向量化 | 中 | P2 先评估前缀依赖处理成本；不达标则只做三通道并行或放弃 |

## 6. 里程碑

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| M1 | P0 完成 | 分路径热点画像（默认/C span/scrfilter）+ 基线帧率表 + 纹理缓存未命中率 |
| M2 | P1+P2 完成 | 解码/滤镜字节级一致；对应场景帧率提升可测（P2 需 scrfilter 开启） |
| M3 | P3 原型裁定 | 像素级一致，且 recompiler-off 下帧率提升 ≥20%，并可映射到 P4 → 继续；否则归档 |
| M4 | （可选）P4 立项 | 独立评审后决定；在现有 SSE2 发射器上扩展 AVX-512 |

---

## 修订记录（2026-10-19 复核）

1. **修正 P1 目标函数**：原文把 `voodoo_recalc_tex12/tex3` 当作纹理解码函数，实际它们只计算 mip 布局元数据（`vid_voodoo_texture.c:108-192, 195-284`）。真正的解码在 `voodoo_use_texture()`（:289-605）。已改写 P1。
2. **修正 JIT 描述**：原文称 codegen "纯标量、无 SSE/AVX 编码"，实际已发射 128-bit SSE2 打包指令并配有 `__m128i` 查表（双线性过滤路径）；无 AVX/AVX-512。P4 相应改为"扩展现有发射器"。
3. **修正 P3 适用路径**：默认 `recompiler=1` 时 C span 循环被跳过（`vid_voodoo_render.c:1094-1099`），P3 对默认配置无收益，定位改为 P4 预研。
4. **补全 P2 前提**：`voodoo_filterline_v1/v2` 仅在可选 scrfilter 开启时执行，且存在行内前向依赖；`voodoo_calc_clutData` 属 setup 路径。
5. **删除 `voodoo_update_ncc` 作为 SIMD 目标**：仅 256×2 项表构建，一次性调用，无优化价值。
6. **修正线程模型细节**：默认 2 线程；4 线程按 `real_y mod 4` 分摊（`odd_even_mask = render_threads - 1`），并非严格奇偶。
7. **P0 门槛改为分路径判定**，并新增纹理缓存未命中率与 scrfilter 状态两项必测指标。

---

## 7. P0/P1 实施结果（2026-10-19）

**环境**：Fedora 44、GCC 16.2.1、Intel i5-1135G7（Tiger Lake，AVX-512F/BW/DQ/VL）。

### P0 基线

- `optimized` preset（`-march=native -mtune=native -O3 -flto=auto`）构建成功，产物约 40 MB，零错误。
- 初期在本工作区内没有游戏镜像与 ROM，P0 的画像改用**纹理解码内核微基准**量化解码吞吐（P1 收益的上限）。后续在本地 86Box 运行环境（roms + Win98 虚拟机）中补做了 3DMark 99 Max 端到端验证，结果见下。
- `perf` 热点画像仍未做；`render_time[]`/`pixel_count[]` 等计数器也未采集，纹理缓存未命中率与 `scrfilter` 开关状态仍待补测。

### P1 实现

- 新增 `src/include/86box/vid_voodoo_texture_avx512.h`：14 种 `tformat` 的 16-wide 整数行解码器，纯整数运算，palette/NCC 走 `vpgatherdd`。
- `voodoo_use_texture()` 在每个 LOD 进入标量 switch 前先尝试向量路径；宽度 < 16、宽度非 16 倍数、或该 LOD 会跨越纹理内存回绕时返回标量路径。
- 运行时 `__builtin_cpu_supports` 分派；宏开关 `VOODOO_TEX_AVX512` 默认 ON。Release 基线 `-msse2` 可正常编译。

### 正确性

- `tests/video/vid_voodoo_texture_test.c` 对 14 种格式、宽度 16/32/64/128/256 做字节级对比；8 位格式覆盖全部 256 个输入值，16 位格式覆盖全部 65536 个输入值。
- 测试发现并修正了一处真实偏差：RGB332 蓝通道在原代码中是两步顺序自或，不能合并为单条表达式。
- `ctest -R Voodoo.Texture` 通过；`regular` 应用目标零警告。
- 全量 `ctest` 被既有问题阻断：`tests/floppy/fdc.c` 在 GCC 16 下报“跳转至 case 标号跨过初始化”，与本次改动无关。

### 解码内核微基准（256×256 mip，Mtex/s）

| 格式 | 基线标量<br>(-msse2) | AVX-512 | 加速 | optimized 标量<br>(-march=native) | AVX-512 | 加速 |
|---|---:|---:|---:|---:|---:|---:|
| TEX_A8 | 5764 | 16446 | 2.85x | 7167 | 16330 | 2.28x |
| TEX_I8 | 6769 | 16372 | 2.42x | 7375 | 16663 | 2.26x |
| TEX_AI8 | 3124 | 9098 | 2.91x | 4879 | 9784 | 2.01x |
| TEX_RGB332 | 1596 | 6422 | 4.02x | 1651 | 6474 | 3.92x |
| TEX_PAL8 | 1632 | 2062 | 1.26x | 1468 | 2098 | 1.43x |
| TEX_R5G6B5 | 1271 | 7649 | 6.02x | 1258 | 7784 | 6.19x |
| TEX_ARGB1555 | 2017 | 6355 | 3.15x | 1960 | 6146 | 3.14x |
| TEX_ARGB4444 | 2020 | 6090 | 3.02x | 2012 | 6277 | 3.12x |
| TEX_APAL88 | 1377 | 2035 | 1.48x | 1378 | 2077 | 1.51x |

**结论**：纯位运算格式收益 2-6 倍；gather 类调色板格式只有 1.3-1.5 倍，验证了计划中“gather 受内存带宽封顶”的风险判断。

### 端到端验证（Win98 + Voodoo3 + 3DMark 99 Max）

环境：本地 86Box 运行环境（roms + Win98 虚拟机，`voodoo3_3k_pci`、800×600×16、`render_threads=4`、`recompiler=1`）。测试用 VM 副本，避免改动原盘。

| 构建 | 3DMark Score | CPU 3DMark |
|---|---:|---:|
| 原版 `86Box-NDR-Linux-x86_64-b10461.AppImage` | 1624 | 1979 |
| optimized（第 1 次） | 1654 | 2001 |
| optimized（第 2 次） | 1630 | 1985 |
| optimized 均值 | 1642 | 1993 |
| 原版 → optimized 均值 | +1.1% | +0.7% |
| **同一二进制两次运行之差** | **24 分（1.5%）** | **16 分（0.8%）** |

**结论：两个构建之间的差异落在运行间波动之内，不能认定有真实收益。**

- 同一个 optimized 二进制两次运行相差 24 分（1.5%），而原版与 optimized 均值之间只差 1.1%。**测量噪声大于待测差异**，上一轮仅凭单次运行得出的 "+1.85%" 应视为噪声，已作废。
- 纯 CPU 子项（与纹理解码无关）的差异只有 +0.7%，进一步说明编译选项带来的通用提速也很小。
- 要在这种负载上分辨 1% 级别的差异，每个构建至少需要 3-5 次运行并比较中位数；单次对比没有判别力。
- 因此：**P1 是正确且无害的优化，但端到端收益在测量精度以下**。这也再次说明，在确定瓶颈位置之前，不应该继续在光栅化上投入（见第 8 节）。


### 本机优化 AppImage

- 产物：`86Box-7.0-AVX512-x86_64.AppImage`（约 58 MB）。
- 基于 `optimized` preset（`-march=native`），捆绑 Qt6、图像/平台/输入法插件与所需共享库；已验证 `--help`、offscreen 启动与 Win98 实机启动。
- 该 AppImage 含 AVX-512 指令（240 条 zmm），只能在支持 AVX-512F/BW/DQ/VL 的主机上运行；ROM 未捆绑。
- 注：该 AppImage 构建于 `VOODOO_TEX_AVX512` 开关语义修正之前，运行时行为相同（默认开启），仅编译期 `-DVOODOO_TEX_AVX512=0` 无法关闭；源码已修正。

---

## 8. P4 可行性评估（2026-10-19 侦察结论）

**结论：完整的 P4 不宜按原计划直接启动。** 不是工作量问题，而是三个具体的技术阻塞点，叠加实测余量很小。

### 8.1 JIT 是逐像素发射，向量化等于重写整条流水线

`voodoo_generate()`（`vid_voodoo_codegen_x86-64.h:689-3571`）逐字节发射 x86-64 机器码。第 826 行到第 3533 行构成**一整条逐像素流水线**，末尾以 `JNZ` 回跳（:3533）形成 x 循环。所有中间状态（`ir/ig/ib/ia/z/s/t/w`、`tex_s/tex_t/lod`、`dest_rgb`）都活在寄存器和栈槽里，一次只处理一个像素。

要让 AVX-512 真正生效，必须让 16 个像素同时在飞：16 份插值状态、16 宽取纹理、掩码做 z/alpha 测试、16 宽写帧缓冲。这意味着把那条约 2700 行的流水线用向量指令重写一遍，并按渲染状态组合特化。现有 SSE2 只用在**单个像素内部的 4 个纹素**上（`codegen_texture_fetch`，:435-565），属于"像素内 SIMD"，无法直接扩展为"跨像素 SIMD"。ARM64 后端（`vid_voodoo_codegen_arm64.h`，4807 行）同理，且改动需双后端同步。

### 8.2 逐像素的 64 位整数除法没有精确的 SIMD 等价物

透视校正纹理在**每个像素、每个 TMU** 都要算一次 `_w = (1ULL << 48) / W`（C 侧 `vid_voodoo_render.c:508-520`；JIT 侧发射 128/64 位 `IDIV`，`vid_voodoo_codegen_x86-64.h:113-116`）。

关键事实：`voodoo_state_t.tmu0_w` 声明为 **`int64_t`**（`vid_voodoo_render.c:80`），即除数是 64 位、范围未知的值；被除数是常量 2^48；除法是**无符号**的（`1ULL`），所以负的 `W` 会被转成巨大的无符号数、商为 0。要按位复现这个语义，SIMD 侧需要：无符号 64 位除法 + 64×64 精确乘法（用于牛顿修正）。AVX-512 两者都没有——它有 `vpmuludq`（32×32→64），但没有向量化的 64×64→128 乘法，更没有整数除法。

也就是说，**最常见的透视校正路径无法用现有 SIMD 指令精确向量化**；任何近似都会破坏 accuracy-first 前提。这是 P4 最硬的技术阻塞点，且恰好落在最热的路径上。

### 8.3 实测端到端余量很小

第 7 节的实测：整个 optimized 构建相对原版只快 1.85%，其中纯 CPU 子项就占 1.11%，说明 3DMark 99 Max 这个负载主要是被 guest CPU 模拟拖住的，不是被 Voodoo 光栅化拖住的。**在一个光栅化占比未知、且已知 CPU 主导的负载上投入数周重写光栅化，收益无法预期。**

### 8.4 若要继续，建议的路径（按顺序）

1. **先补宿主侧 perf 画像**，确认 Voodoo 光栅化在单帧中的真实占比。这是原 P0 的门槛，也是判断 P4 是否值得的唯一依据。判定：若 JIT span 代码占比 < 20%，P4 不值得做。
2. **若确认光栅化占比高，先查那个除法的实际代价，再决定是否替换。** 候选做法：JIT 目前发射的是 128/64 位 `IDIV`（最慢的一种形式），而商 `2^48/W` 在常见 W 范围内远小于 2^32，因此当除数也落在 32 位内时，理论上可改用 `div r32`（令 `EDX:EAX = 2^48`）得到按位相同的结果。**但这条路的可行性尚未验证**：`W = w_true × 2^32`（注释见 `vid_voodoo_render.c:136`），当 `w_true > 1` 时 W 就超过 32 位，守卫条件是否在真实场景中经常成立需要先测 W 的分布。先确认除法确实是热点，再评估替换；不要预设收益。
3. 只有在第 1 步证明光栅化确实是主瓶颈、且第 2 步的收益被吃尽之后，再评估把 span 流水线改写成"每 16 像素"的向量实现，即真正的 P4。


### 8.5 本轮未交付 P4 的原因

本轮停在 8.1-8.3：16 宽 span 实现只要覆盖透视校正，就必须精确处理 8.2 的除法，而这一点用 AVX-512 整数指令做不到。绕开它的窄子集（仿射纹理、最近邻、无混合）在真实游戏里几乎不触发，属于"零收益但碰渲染路径"的改动，风险与收益不成比例。按 accuracy-first 红线，不在未经验证的情况下改动渲染路径。




