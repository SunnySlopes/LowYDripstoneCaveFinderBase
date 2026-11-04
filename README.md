# LowYDripstoneCaveFinderBase

低 Y 滴水石洞穴 + 河流搜索核心。本仓库包含全部 C++ / cubiomes 计算代码，
cubiomes 作为嵌套 git 子模块放在 `cubiomes/`，可以独立构建，不依赖 Java 源码树。

主仓库 [LowYDripstoneCaveFinderGUI](https://github.com/SunnySlopes/LowYDripstoneCaveFinderGUI)
通过 git 子模块引用本仓库；Java 端只负责界面、任务编排、阈值判断和页面跳转。

当前 GUI 发布版本为 **1.4.3**。

## 计分模型

对每个候选点 `(x, z)`：

- 挂机点/球心：`(x, afkY, z)`，默认 `afkY = -40`，允许 `-64..320`；
- 采样层：`yMin..yMax`，默认 `-63..-51`（13 层），跨度上限 48 层；
- 每层 `dy = y - afkY`，只统计满足
  `24² < dx² + dy² + dz² ≤ 128²` 的洞穴/河流格；
- **球壳内的所有格都计入**，不做区块级过滤；
- 逐层求和后取平均：

```text
total = round((Σcave + riverWeight × Σriver) / L)
cave  = round(Σcave / L)
river = round(Σriver / L)
```

`total` 以 `49659`（默认 AFK=-40 的满球壳格数）作为 GUI 显示标定；`riverWeight`
范围 `[0,1]`。满面积溶洞的 total 为 49659，占比 100.00%。

结果集模型标识：

```text
# model=drowned-weighted-shell-b/{L}/afk-40
```

层数、挂机 Y 或模型不同，结果不可直接比较。

## 产物

| 产物 | 路径 | 用途 |
| --- | --- | --- |
| 静态引擎库 | CMake target `lowydripstonecavefinder-core` | CLI 与 JNI 共用 |
| 独立 CLI | `build/cli/lowydripstonecavefinder-cli.exe` | 命令行搜索 / 点位诊断 / weirdness |
| JNI 动态库 | `build/native/libDripstoneCaveFinderLibJ.dll` | 供 Java GUI 调用 |

## 构建

首次克隆后初始化嵌套子模块：

```powershell
git submodule update --init --recursive
```

主模块 Gradle 会自动调用本仓库的 `copyNativeArtifacts` / `copyCliArtifact`。

### 独立 CLI

```powershell
powershell -ExecutionPolicy Bypass -File build_cli_msys2.ps1
.\build\cli\lowydripstonecavefinder-cli.exe --help
```

或使用 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\bin\lowydripstonecavefinder-cli.exe --help
```

MinGW 下 exe 静态链接 cubiomes 和 C++ 运行库，可直接复制运行。

### JNI 库

```powershell
powershell -ExecutionPolicy Bypass -File build_jni_msys2.ps1
```

或：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target DripstoneCaveFinderLibJ
```

## 命令行

```powershell
# 默认快速模式，131072 × 131072 区域
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --out result.txt

# 精确模式 + 正方形区域
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --mode precise `
    --square 262144 --out precise.txt

# 大坑模式
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --mode pit `
    --min-x -30000000 --max-x 30000000 --min-z -30000000 --max-z 30000000 `
    --out pit.txt

# 自定义采样层与挂机 Y
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --square 131072 `
    --y-min -60 --y-max -52 --afk-y -40 --out y9.txt

# weirdness y-offset（独立接口）
.\build\cli\lowydripstonecavefinder-cli.exe --weirdness-y-offset 12345
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--seed <int64>` | 搜索时必填 | 世界种子，可为负数 |
| `--mode fast\|precise\|pit` | `fast` | Cont@128+Weird@32 / Cont@32+Weird@16 / 大坑 |
| `--mc <id\|version>` | `26.3` | 版本名、`1.18~26.1`、`26.2+` 或 cubiomes `MCVersion` 数值 |
| `--min-x` / `--max-x` / `--min-z` / `--max-z` | `-65536..65535` | 用户搜索区域（含端点） |
| `--square <side>` | 未设置 | 以 (0,0) 为中心的正方形；不能与显式边界同用 |
| `--min-area` | fast/precise `34761`，pit `44693` | 加权总面积下限（满值 49659） |
| `--river-weight <float>` | `0.75` | `total = cave + river * weight`，范围 `[0,1]` |
| `--y-min` / `--y-max` | `-63` / `-51` | 采样层范围；跨度必须 ≤ 48，越界或倒置直接报错 |
| `--afk-y` | `-40` | 挂机/玩家 Y；球壳中心为 `(x, afk-y, z)` |
| `--coarse-y` | `-51` | 粗筛召回高度；粗筛气候门与 Y 无关，仅保留协议字段 |
| `--threads` | CPU 数 | 1–256 |
| `--limit` | `0` | 最多输出条数；0 表示全部 |
| `--out <file>` | 未设置 | 同时写入文本报告 |
| `--bench` | 未设置 | 向 stderr 输出分阶段计时、工作量与峰值内存 |
| `--weirdness-y-offset <seed>` | 未设置 | 只输出 `weirdnessYOffset(seed)` |
| `--help` / `-h` | — | 显示帮助 |
| `--version` | — | 显示版本 |

### 点位诊断

这些参数替代搜索，只计算一个点，适合复现结果和比较模型：

| 参数 | 说明 |
| --- | --- |
| `--score-point <x> <z>` | 以 1:1 采样给单点打分，输出 total/cave/river 与耗时 |
| `--model shell\|cylinder` | `shell` 为当前 24–128 球壳；`cylinder` 为水平环对照 |
| `--repeat <n>` | 计时重复次数，默认 5 |
| `--sweep-afk-y <a> <b>` | 只采样一次生物群系，扫描 `[a,b]` 内每个挂机 Y，输出最佳 Y |
| `--centroid` | 输出洞穴格质心与结果点的距离（visual/scored） |

```powershell
REM 当前模型在该点：total=49659 cave=49659 river=0（100.00%）
.\build\cli\lowydripstonecavefinder-cli.exe --seed 6900457 `
    --score-point 16043916 15695385 --model shell --repeat 5
.\build\cli\lowydripstonecavefinder-cli.exe --seed 6900457 `
    --score-point 16043916 15695385 --sweep-afk-y -64 0
.\build\cli\lowydripstonecavefinder-cli.exe --seed 6900457 `
    --score-point 16043916 15695385 --centroid
```

`--model` 只影响点位诊断；完整搜索始终使用当前 `shell` 模型。

### 报告格式

```text
/tp x -40 z s=total/49659 = pct% cave=N river=M
```

pit 模式省略 `cave` / `river`。`/tp` 的 y 值就是当前挂机 Y（默认 -40），可直接传送到球心所在高度。

结果先按 `(x,z)` 去重，再在每个 64×64 区块保留加权总面积最大的点，最后按总面积降序输出，
与 Java GUI 的显示过滤一致。报告头部携带模型标识：

```text
# model=drowned-weighted-shell-b/13/afk-40 y=[-63,-51] L=13 afkY=-40 coarseY=-51
```

## JNI 契约

`LowYDripstoneCaveFinderBridge`：

```java
riverSearch(long seed, int startX, int startZ, int width, int height,
            int yMin, int yMax, int coarseY, int afkY, int minArea,
            float preserveRange, float riverWeight, int numThreads,
            String mcVersion, int contScale, int weirdScale,
            int searchMode, boolean allowGpu)
```

每次命中 5 个 `int`：`x, z, total, cave, river`；无结果返回 `null`。

- `getSearchProgress()`：`[current, total, phase, status, tryPause, tryStop]`。
- `getNowResult()`：与 `riverSearch` 相同的扁平布局。
- `pause()` / `resume()` / `stop()`。
- `weirdnessYOffset(long seed)`：返回种子奇异值 y-offset 分数；阈值比较与页面跳转由 Java 端负责。

`jni/generated/sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.h`
是签入的 JNI 头文件；修改 Java 声明后需重新生成。主模块的 `verifyJniContract`
任务会检查两边一致。

## 搜索流程 / Search Pipeline

引擎不做全图逐格采样：先用气候噪声的周期性把候选点压缩几个数量级，再用真实群系逐级确认。
下面的阶段划分与 `--bench` 输出、源码一一对应。

The engine does not brute-force the map. It exploits the periodicity of the climate
noise to shrink the candidate set by orders of magnitude first, then confirms the
survivors with real biome sampling. The stages below map directly onto the `--bench`
output and the source.

### 八度噪声基础 / Octave basics

每个气候参数（大陆性 `Cont`、奇异值 `Weirdness` 等）都是若干 A 侧与 B 侧八度之和，
按频率从低到高记作 `0A`、`0B`、`1A`、`1B`……。A 侧的重复周期：大陆性 `0A` 为 524288 格、
奇异值 `0A` 为 131072 格，之后每级减半。B 侧以 `337/331` 的频率倍率采样，使 A、B 的
周期不会同时回到起点；主世界群系的重复周期因此被拉长到约 0.34B 格（3.4 亿格）。

大陆性共 18 个八度（9A + 9B），是最慢的气候参数；奇异值只有 6 个（3A + 3B），
因此快得多。前 10 个大陆性八度（`0A`–`4A` 与 `0B`–`4B`）已足以决定 0.55 阈值附近的
绝大多数判定；剩下 8 个造成超过 0.025 波动的概率约为万分之一，所以在追求速度的
环节直接忽略。

快速模式把前 5 个大陆性 A 八度与 3 个奇异值 A 八度预计算成周期表
（`OctaveFieldCache.h`）。查表与实时采样逐位一致，因此后续阶段可以只读内存。

Every climate parameter (continentalness, weirdness, ...) is a sum of A-side and B-side
octaves, numbered by increasing frequency as `0A`, `0B`, `1A`, `1B`, ... On the A side,
`0A` repeats every 524288 blocks for continentalness and every 131072 blocks for
weirdness, halving for every later octave. The B side is sampled at a 337/331 frequency
ratio, so the A and B cycles never line up and the overworld biome pattern repeats only
about every 0.34 billion blocks.

Continentalness has 18 octaves (9 A + 9 B) and is the slowest climate parameter;
weirdness has only 6 (3 A + 3 B), which makes it much cheaper. The first 10
continentalness octaves (`0A`–`4A` and `0B`–`4B`) already decide virtually every case
near the 0.55 threshold; the remaining 8 move the result by more than 0.025 in only
about one sample in ten thousand, so speed-critical stages ignore them.

Fast mode precomputes the first five continentalness A octaves and the first three
weirdness A octaves as period tables (`OctaveFieldCache.h`). A lookup is bit-identical
to a live sample, so later stages can read memory instead of noise.

### 全图搜索步骤 / Full-map search steps

对应 GUI 的「快速模式」与 CLI `--mode fast`；精确模式（Cont@32 + Weird@16）走同一条
流程，只是更慢、漏检更少。

These are the stages behind Fast Mode in the GUI and `--mode fast` in the CLI.
Precise Mode (`Cont@32 + Weird@16`) runs the same pipeline, just slower and with fewer
misses.

**步骤 0 · 缓存 A 表 / Step 0 · Cache the A tables.** 预计算大陆性 `0A`–`4A` 与
奇异值 `0A`–`2A`：常驻约 170–180 MB，现代 CPU 上 1 秒内完成，却能为后续阶段省下
数小时。搜索边长达到 `PHASE0_MIN_SIDE_BLOCKS`（100 万格）或面积足够大时才会建表。

Precompute continentalness `0A`–`4A` and weirdness `0A`–`2A`: about 170–180 MB
resident, built in roughly one second on a modern CPU, saving hours later. The tables
are only built when the search side reaches `PHASE0_MIN_SIDE_BLOCKS` (1,000,000
blocks) or the area is large enough.

**步骤 1 · Cont@128 预筛 / Step 1 · Continentalness prefilter at 128.** 以 128 格
步长扫描大陆性。溶洞只生成在大陆性 > 0.3 的地方；要在基岩层出现，需要 > 0.55。
实现只累加前 10 个八度，逐级早退：

Scan continentalness on a 128-block grid. Caves only spawn where continentalness > 0.3,
and reaching bedrock level needs > 0.55. The engine sums only the first 10 octaves and
bails out at each gate:

| 累加到的八度 / Octaves summed | 早退阈值 / Gate |
| --- | --- |
| `0A + 0B` | `>= -0.2` |
| `+ 1A` | `>= -0.1` |
| `+ 1B` | `>= 0` |
| `+ 2A` | `>= 0.13` |
| `+ 2B` | `>= 0.3` |
| `+ 3A` | `>= 0.37` |
| `+ 3B` | `>= 0.44` |
| `+ 4A + 4B` | `> 0.525`（快速模式掩膜 / fast-mode mask）；`> 0.55`（大坑 / pit） |

任一步不满足立即放弃，平均每个点位只算约 5 个八度。0.525 的末级阈值略低于 0.55，
是给随后的 1 tile 掩膜膨胀留余量；大坑模式直接用 0.55。

Any failed gate rejects the point, and a point costs about 5 octaves on average. The
final 0.525 gate sits slightly below 0.55 to leave room for the following 1-tile mask
dilation; the large-pit mode uses 0.55 directly.

**步骤 2 · Weird@32 环扫 / Step 2 · Weirdness ring scan at 32.** 对通过预筛的区域
以 32 格步长计算小范围奇异值。低 Y 溶洞能出现在基岩层，只有两种地形：足够深的
河流（`|weirdness| < 0.05`）或大坑（`|weirdness| > 1.28`）；连沼泽的最低地表也
不够低。128 格半径的环内没有满足条件的点时，整块区域排除。

For each region that passes the prefilter, sample weirdness on a 32-block grid. A low-Y
cave reaches bedrock under only two kinds of terrain: a deep enough river
(`|weirdness| < 0.05`) or a large pit (`|weirdness| > 1.28`); even the lowest swamp
surface is too high. If no qualifying point lies inside the 128-block radius ring, the
whole tile is dropped.

优选：只算前两个 B 八度，若 `0A+0B+1A+1B` 的绝对值落在 0.19–1.14 之间，剩余的八度
最多只能把结果推到 0.05/1.28 之外——直接判失败。约 40% 的候选继续，漏检概率约
万分之一。

Optimization: after only two B octaves, a partial `|0A+0B+1A+1B|` inside 0.19–1.14
can no longer be pushed back into either tail, so the point fails immediately. About
40% of candidates continue, with a miss rate of roughly one in ten thousand.

**步骤 3 · 4 格气候精筛 / Step 3 · Climate coarse pass at 4.** 对环扫留下的环心，
在 512×512 子区域内以 4 格步长（`PHASE1_CLIMATE_GRID_SCALE`）重新验证气候，把候选
收敛到具体方块。此阶段只算气候，不生成完整群系。

Each surviving ring center is re-checked at a 4-block step
(`PHASE1_CLIMATE_GRID_SCALE`) inside a 512×512 sub-region, which narrows the candidate
down to a specific block. This stage evaluates climate only, not full biomes.

**步骤 4 · 1:1 完整群系评分 / Step 4 · Full-biome scoring at 1:1.** 在候选点周围
320×320 的窗口内，以 1:1 逐层采样 `[yMin, yMax]` 的真实群系（默认 13 层）。
每层只统计以挂机点 `(x, -40, z)` 为球心的 24–128 球壳截面，球壳内全部洞穴/河流格
都计入；逐层求和后取平均：

```text
total = round((Σcave + riverWeight × Σriver) / L)
```

明显过小的结果被剔除，其余按总面积降序输出。精算阶段用每层一张 2D 前缀表：
`fillPreciseShellWindow()` 一次 `genBiomes` 覆盖 `yMin..yMax`，
`findBiggestRiver<1>()` 对每层球壳截面求和。

In a 320×320 window around each candidate, sample the real biomes at 1:1 on every
layer in `[yMin, yMax]` (13 by default). Each layer counts only its 24–128 shell
cross-section around `(x, -40, z)`; every cave/river cell inside the shell counts.
The layer sums are averaged:

```text
total = round((Σcave + riverWeight × Σriver) / L)
```

Results that are obviously too small are dropped; the rest are emitted by total area.

### 大坑全图搜索步骤 / Large-pit search steps

对应「快速单种子搜索」页与 CLI `--mode pit`。最完美的低 Y 溶洞往往不在河流下方：
很难有大面积河流都那么深，而且深度 > 0.9 会强制转回地面群系。大坑（离河流最远的
地方）反而常常是最好的选址：当奇异值绝对值峰值在 1.6–1.9 之间时，地表高度恰好
合适，能在基岩层刷出 24–128 球壳内的溶洞。这条路线通常比全图搜索快上百倍，
但对约 80% 的种子可能一个结果都找不到。

This is the route behind the Fast Single Seed Search tab and `--mode pit`. The best
low-Y caves often do not sit under rivers: large areas of deep river are rare, and
depth > 0.9 forces the surface biome back to land. Large pits (the farthest points from
rivers) are often the best sites instead: when the `|weirdness|` peak is between 1.6
and 1.9, the surface height is just right for caves inside the 24–128 shell at bedrock
level. This route is usually about two orders of magnitude faster than the full-map
search, but for about 80% of seeds it may find nothing at all.

**步骤 0 · 缓存奇异值 A 表 / Step 0 · Cache the weirdness A tables.** 只缓存奇异值的
`0A`、`1A`、`2A`，1 秒内完成。

Cache only weirdness `0A`, `1A`, `2A`; it finishes in about a second.

**步骤 1 · A 链 / Step 1 · A chain.** 直接在缓存上累加 A 八度，正、负两个瓣都保留，
逐级早退：

Sum the A octaves straight from the tables, keeping both the positive and negative
lobes, with early exits:

| 累加 / Sum | 门槛 / Gate |
| --- | --- |
| `0A` | `abs(0A) > 0.21` |
| `0A + 1A` | `abs(0A + 1A) > 0.55` |
| `0A + 1A + 2A` | `abs(0A + 1A + 2A) > 0.6` |

**步骤 2 · B 链与外推 / Step 2 · B chain and replication.** 对通过 A 链的点计算 B
八度。A 侧以 131072 格为周期，每个 A 峰值会按该周期在全图复制；B 侧不周期，每一份
复制都必须单独采样：

For each point that passes the A chain, evaluate the B octaves. The A side repeats
every 131072 blocks, so every A peak is replicated across the map at that period; the
B side is not periodic, so each replica must be sampled:

| 累加 / Sum | 门槛 / Gate |
| --- | --- |
| `A + 0B` | `> 1.0` |
| `A + 0B + 1B` | `> 1.4` |
| `A + 0B + 1B + 2B`（完整奇异值 / full weirdness） | `> 1.6` |

1–2 步在全图约 1 分钟完成。每个通过的点再在 128 格内细化峰值，并在 256 格内去重。

Steps 1–2 take about a minute over the whole map. Each survivor is refined to a peak
within 128 blocks and deduplicated within 256 blocks.

**步骤 3 · 大陆性门槛 / Step 3 · Continentalness gate.** 剩余点位（全图通常几万到
几十万）套用与全图搜索相同的前 10 八度早退链，末级门槛 0.55；约 2/3 的结果在此
出局，一般 1 秒内完成。

The survivors (usually tens to hundreds of thousands across the map) go through the
same 10-octave early-exit chain, with a 0.55 final gate. About two thirds drop out
here, usually within a second.

**步骤 4 · 4 格气候粗筛 / Step 4 · Climate coarse pass at 4.** 在峰值周围 512×512
的窗口内以 4 格步长重新验证气候，确认溶洞区域足够大。

Re-check the climate at a 4-block step in a 512×512 window around the peak to confirm
the cave area is large enough.

**步骤 5 · 1:1 完整群系评分 / Step 5 · Full-biome scoring at 1:1.** 在同一 512×512
窗口内按 1:1 逐层采样 `[yMin, yMax]` 的真实群系，每层按以 `(x, -40, z)` 为球心的
24–128 球壳统计（球壳内全部计入），逐层平均，达标才输出。步骤 4–5 的耗时从几秒到
几分钟不等，取决于步骤 2 的结果数：结果越多，越可能藏着全图最大的溶洞。

The same 512×512 window is sampled at 1:1 on every layer in `[yMin, yMax]`; each layer
is scored over the 24–128 shell around `(x, -40, z)` with every cell counted, then the
layers are averaged. Only qualifying results are emitted. Stages 4–5 take anywhere from
seconds to minutes depending on how many peaks survived step 2: the more survivors,
the better the chance that the map's largest cave is among them.

### 种子是否适合大坑搜索 / Is a seed worth the pit search

全图尺度下，每个种子的奇异值均值都接近 0，但方差差异很大。方差大的种子大坑多，
也就是步骤 2 留下的候选多：通常要有 5 万–10 万条（极端情况 20 万条）才比较有机会
找到全图最大的溶洞。可以只算 4 个八度的分项来快速估计这个方差：

Across a whole map the weirdness mean is close to 0 for every seed, but the variance
differs a lot. High-variance seeds have many large pits, i.e. many step-2 survivors:
about 50k–100k of them (up to 200k in extreme cases) are usually needed for a good
chance at the map's largest cave. The variance can be estimated quickly from four
octave terms:

`score = |w0A - 0.5| + |w1A - 0.5| + |w0B - 0.5| + |w1B - 0.5|`

每个分项在 0–0.5 之间，总分 0–2，集中在 1 附近；分数越小，全图奇异值方差越大、
大坑越多。实测分数 ≤ 0.75 才能比较稳定地用大坑法找到全图最大溶洞，约 20% 的
种子满足。

Each term lies in 0–0.5, so the total lies in 0–2 and clusters around 1; a lower score
means a more varied weirdness map and more pits. Empirically, 0.75 or below is needed
to reliably find the map's largest cave this way, and about 20% of seeds qualify.

| 分数 / Score | 含义 / Meaning |
| --- | --- |
| `≤ 0.75` | 大坑法通常能出全图最大溶洞；单种子搜索页会提示可以试试 / the pit search usually finds the largest cave; the single-seed tab offers it |
| `0.75 – 0.85` | 可以碰运气，不保证 / worth a try, not guaranteed |
| `> 0.85` | 大概率空手而归；快速单种子搜索页会提醒回退 / likely empty; the fast tab warns you to go back |

分数由 `lowyd::weirdnessYOffset(seed)` 计算（`lowydripstonecavefinder.cpp`），CLI 用
`--weirdness-y-offset` 直接查询。`0.75` / `0.85` 的阈值比较与页面跳转留在 Java
主模块。

The score is computed by `lowyd::weirdnessYOffset(seed)`
(`lowydripstonecavefinder.cpp`) and exposed by `--weirdness-y-offset` in the CLI. The
`0.75` / `0.85` comparisons and tab jumps stay in the Java main module.

## 性能与 `--bench`

`--bench` 在搜索结束后把分阶段计时打印到 **stderr**（stdout 仍只有结果报告），
不传该参数时引擎只额外做一次 relaxed 原子读，几乎没有开销：

```powershell
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --square 2000001 --threads 8 --bench
```

输出包含五个阶段（阶段 0 octave 表 / Cont 预筛掩膜 / Weird 环扫 / ClimateCoarse /
阶段 2 精筛）的线程时间与占比、工作量计数（chunk 数、落在 A 表格点上的 chunk 数、
Cont 掩膜通过率、环心总数、ClimateCoarse 子搜索数、阶段 2 候选数、被差分膨胀省掉的
Chebyshev 写入次数、A 表体积与样本数），以及进程峰值工作集。

注意：阶段计时是**各工作线程累计的线程时间**，8 线程并行时其总和会明显高于墙钟时间；
占比的分母是这个总和。

> **精确模式的取舍**：A 周期表只为快速模式（Cont@128 + Weird@32）服务。
> 精确模式（Cont@32 + Weird@16）实时计算八度，内存更省，但同一区域的耗时可能更长。


## 回归测试

`tests/` 包含：

| 用例 | 内容 |
| --- | --- |
| `volume_unit` | 球壳几何（逐行半径/内孔/`|dy|>128`）、层范围/挂机 Y 校验、model id |
| `column_reference` | 8 个版本 × 1 种子 × 3 个 3×3×13 窗口，共 2808 格逐格比对；另验证 `genBiomes` 与 `getBiomeAt` 一致 |

运行：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests_msys2.ps1
```

或 CTest：

```powershell
cmake -S . -B build\tests -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DLOWYDRIPSTONE_BUILD_CLI=OFF -DLOWYDRIPSTONE_BUILD_JNI=OFF `
    -DLOWYDRIPSTONE_BUILD_TESTS=ON
ctest --test-dir build\tests --output-on-failure
```

## 目录

```text
BiomeSampler.*                真实群系采样、每层前缀表、精算 scratch
dripstonecave_finder.cpp      球壳掩膜、精算/环扫、搜索主循环
LargePitSearch.cpp            大坑链搜索
lowydripstonecavefinder.*     公共 API、搜索编排、JNI 入口
lowydripstonecavefinder_cli.cpp CLI 与点位诊断
GpuCoarse.*                   OpenCL 粗筛
SearchConfig.h                搜索参数
tests/                        回归测试
jni/                          JNI 桥接与签入头文件
cubiomes/                     嵌套 git 子模块
```

## 许可

本项目以 **MIT** 许可证发布，详见 `LICENSE`（Copyright (c) 2026 SunnySlopes）。

原生引擎基于 [cubiomes](https://github.com/Cubitect/cubiomes) 及其活跃分支
[xpple/cubiomes](https://github.com/xpple/cubiomes)（MIT，Copyright (c) 2020 Cubitect），
上游许可文本保留在 `cubiomes/LICENSE`；战利品相关代码另有
`cubiomes/loot/LICENSE_loot_library.h.txt`（MIT，Copyright (c) 2024 ScriptLine）。
