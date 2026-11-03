# LowYDripstoneCaveFinderBase

低 Y 滴水石洞穴 + 河流搜索核心。本仓库拥有**全部** C++ / cubiomes 计算代码；
[cubiomes](https://github.com/xpple/cubiomes) 作为嵌套 git 子模块放在
`cubiomes/`，因此可以独立构建，不依赖 Java 模块的源码树。

主仓库 [LowYDripstoneCaveFinderGUI](https://github.com/SunnySlopes/LowYDripstoneCaveFinderGUI)
通过 git 子模块引用本仓库，只保留 Swing 界面、查询编排、阈值判断和页面跳转逻辑。

## 产物与契约

| 产物 | 路径 | 用途 |
| --- | --- | --- |
| 静态引擎库 | CMake 中的 `lowydripstonecavefinder-core` | CLI 与 JNI 共用的 C++ 引擎 |
| 独立 CLI | `build/cli/lowydripstonecavefinder-cli.exe` | 纯命令行搜索 / weirdness，不依赖 Java / DLL |
| JNI 动态库 | `build/native/libDripstoneCaveFinderLibJ.dll` | 供 Java GUI 查询 |

JNI 契约（`LowYDripstoneCaveFinderBridge`）与旧版保持兼容，并新增一个
**独立**的 weirdness 接口：

- `riverSearch(seed, startX, startZ, width, height, yMin, yMax, coarseY, minArea,
  preserveRange, riverWeight, numThreads, mcVersion, contScale, weirdScale,
  searchMode, allowGpu)`：每次命中 5 个 int：`x, z, total, cave, river`；无结果返回 `null`。
  `yMin..yMax` 为精采样层范围（跨度 1..48）；`coarseY` 仅为协议保留字段——本引擎
  的粗筛气候门与 Y 无关。
- `getSearchProgress()`：返回 `[current, total, phase, status, tryPause, tryStop]`。
- `getNowResult()`：与 `riverSearch` 相同的扁平布局。
- `pause()` / `resume()` / `stop()`。
- `weirdnessYOffset(long seed)`：返回种子奇异值 y-offset 分数。
  阈值（`0.75` / `0.85`）比较以及是否跳转“快速单种子搜索”页面的判断
  **全部留在 Java 主模块**。

`jni/generated/sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.h`
是签入的 JNI 头文件，使本仓库不依赖 Java 源码即可构建。修改
`LowYDripstoneCaveFinderBridge.java` 后需要重新生成并同步该头文件；主模块的
`verifyJniContract` Gradle 任务会检查两边是否一致。

## 子模块与构建

首次克隆后初始化嵌套子模块：

```powershell
git submodule update --init --recursive
```

本仓库也可以由主模块的 Gradle 构建直接调用
（`:lowydripstonecavefinderbase:copyNativeArtifacts` /
`:lowydripstonecavefinderbase:copyCliArtifact`）。

### 独立 CLI（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File build_cli_msys2.ps1
.\build\cli\lowydripstonecavefinder-cli.exe --help
```

也可以使用 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\bin\lowydripstonecavefinder-cli.exe --help
```

MinGW 下 exe 静态链接 cubiomes 和 C++ 运行库，可以直接复制到其他 Windows
机器运行。

### JNI 库

```powershell
powershell -ExecutionPolicy Bypass -File build_jni_msys2.ps1
```

产物为 `build/native/libDripstoneCaveFinderLibJ.dll`。也可以使用 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target DripstoneCaveFinderLibJ
```

## 命令行用法

```powershell
# 默认快速模式，131072 x 131072 区域，结果同时写入 txt
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --out result.txt

# 精确模式 + 正方区域
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --mode precise `
    --square 262144 --out precise.txt

# 大坑模式（全图，按用户区域裁剪）
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --mode pit `
    --min-x -30000000 --max-x 30000000 --min-z -30000000 --max-z 30000000 `
    --out pit.txt

# 只计算 weirdness y-offset（独立接口）
.\build\cli\lowydripstonecavefinder-cli.exe --weirdness-y-offset 12345

# 自定义精采样层范围（默认 -63..-51；跨度上限 48 层）
.\build\cli\lowydripstonecavefinder-cli.exe --seed 12345 --square 131072 `
    --y-min -60 --y-max -52 --out y9.txt
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--seed <int64>` | 搜索时必填 | 世界种子，可为负数 |
| `--mode fast\|precise\|pit` | `fast` | Cont@128+Weird@32 / Cont@32+Weird@16 / 大坑 |
| `--mc <id\|version>` | `26.3` | 版本名、`1.18~26.1`、`26.2+` 或 cubiomes `MCVersion` 数值 |
| `--min-x` / `--max-x` / `--min-z` / `--max-z` | `-65536..65535` | 用户搜索区域（含端点） |
| `--square <side>` | 未设置 | 以 (0,0) 为中心的正方形；不能与显式边界同用 |
| `--min-area` | fast/precise `34748`，pit `44676` | 加权总面积下限（满值 49640） |
| `--river-weight <float>` | `0.75` | `total = cave + river * weight`，范围 `[0,1]` |
| `--y-min` / `--y-max` | `-63` / `-51` | 精采样层范围；`yMax-yMin+1` 必须 ≤ 48，越界或倒置直接报错 |
| `--coarse-y` | `-51` | 粗筛召回高度；本引擎粗筛与 Y 无关，仅为协议兼容保留 |
| `--threads` | CPU 数 | 1–256 |
| `--limit` | `0` | 最多输出条数；0 表示全部 |
| `--out <file>` | 未设置 | 同时把同一份文本报告写入 txt |
| `--bench` | 未设置 | 运行结束后向 **stderr** 输出分阶段计时（阶段 0 / Cont 掩膜 / Weird 环扫 / ClimateCoarse / 阶段 2）、工作量计数与进程峰值工作集 |
| `--weirdness-y-offset <seed>` | 未设置 | 只输出 `weirdnessYOffset(seed)` 分数 |
| `--help` / `-h` | — | 显示帮助 |
| `--version` | — | 显示版本 |

文本报告同时打印到 stdout；指定 `--out` 时再写入文件。搜索报告每行与 GUI
导出一致：

```text
/tp x 64 z s=total/49640 = pct% cave=N river=M
```

报告头部现在带模型与层范围，切换模型/层数后结果不可比：

```text
# model=drowned-weighted-volume-b/13 y=[-63,-51] L=13 coarseY=-51
```

pit 模式省略 `cave` / `river`。结果先按 (x,z) 去重，再在每个 64×64 区块保留
加权总面积最大的点，最后按总面积降序输出，与 Java GUI 的显示过滤一致。

### 层范围与体积积分（B1）

旧版只读 `-60/-56/-52` 三层并做“多数票压 0/1”。现在：

- 逐层采样 `yMin..yMax`（默认 13 层），`fillPreciseWindow` 的 `sy = L`；
- 计分改为**体积积分**：`total = round((Σcave + w·Σriver) / L)`，`cave`/`river`
  也取逐层平均，因此分数保持 `0..49640`，`minArea`、剪枝和排序语义不变；
- `L > 48` 属于硬错误（窗口缓存约 `320×320×L×4 B`，且 128 格球壳模型不再成立）；
- 与 C（river-native）是同一模型的不同单位：B 用等效溶洞格，
  `C = B / 0.69620`（`w = 0.6962` 时）。

`model` 标识随层数变化，`done`/导出必须携带。

## 回归测试（B4）

`tests/` 里是从 C 项目（`river-native/tests/`）搬来的冻结参照与解析用例：

| 用例 | 内容 |
| --- | --- |
| `volume_unit` | 体积积分解析用例（k/L 层满覆盖 ⇒ `round(49640·k/L)`）、层范围硬校验、model id |
| `column_reference` | 8 版本 × 1 种子 × 3 个 3×3×13 窗口，共 2808 格逐格比对；另验证 `genBiomes` 与 `getBiomeAt` 逐格一致 |

运行（无需 CMake，与正式构建同一套 MSYS2 工具链）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests_msys2.ps1
```

也可以走 CTest：

```powershell
cmake -S . -B build\tests -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DLOWYDRIPSTONE_BUILD_CLI=OFF -DLOWYDRIPSTONE_BUILD_JNI=OFF `
    -DLOWYDRIPSTONE_BUILD_TESTS=ON
ctest --test-dir build\tests --output-on-failure
```

## 搜索流程 / Search Pipeline

引擎不做全图蛮力采样：先用气候噪声的周期性把候选点压缩几个数量级，再用真实群系逐级确认。下面的阶段划分与 `--bench` 输出、源码一一对应。

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
320×320 的窗口内，以 1:1 逐层采样 `[yMin, yMax]` 的真实群系（默认 13 层），
把洞穴/河流计数按体积积分取逐层平均。明显过小的结果被剔除，其余按总面积降序输出。

In a 320×320 window around each candidate, sample the real biomes at 1:1 on every
layer in `[yMin, yMax]` (13 by default) and aggregate the cave/river counts as a
per-layer volume integral. Results that are obviously too small are dropped; the rest
are emitted in descending total area.

### 大坑全图搜索步骤 / Large-pit search steps

对应「快速单种子搜索」页与 CLI `--mode pit`。最完美的低 Y 溶洞往往不在河流下方：
很难有大面积河流都那么深，而且深度 > 0.9 会强制转回地面群系。大坑（离河流最远的
地方）反而常常是最好的选址：当奇异值绝对值峰值在 1.6–1.9 之间时，地表高度恰好
合适，能在基岩层刷出半径 128 内满环的溶洞。这条路线通常比全图搜索快上百倍，
但对约 80% 的种子可能一个结果都找不到。

This is the route behind the Fast Single Seed Search tab and `--mode pit`. The best
low-Y caves often do not sit under rivers: large areas of deep river are rare, and
depth > 0.9 forces the surface biome back to land. Large pits (the farthest points from
rivers) are often the best sites instead: when the `|weirdness|` peak is between 1.6
and 1.9, the surface height is just right for a full 128-block-radius ring of caves at
bedrock level. This route is usually about two orders of magnitude faster than the
full-map search, but for about 80% of seeds it may find nothing at all.

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
窗口内按 1:1 逐层采样 `[yMin, yMax]` 的真实群系，按体积积分取逐层平均，达标才输出。
步骤 4–5 的耗时从几秒到几分钟不等，取决于步骤 2 的结果数：结果越多，越可能藏着全图最大的溶洞。

The same 512×512 window is sampled at 1:1 on every layer in `[yMin, yMax]`, aggregated
as a per-layer volume integral; only qualifying results are emitted. Stages 4–5 take
anywhere from seconds to minutes depending on how many peaks survived step 2: the more
survivors, the better the chance that the map's largest cave is among them.

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
> 精确模式（Cont@32 + Weird@16）会全量实时计算 A 八度，因此峰值内存从约
> 1.75 GB 降到 <50 MB，但同一区域的耗时会比"带表"的实现更长。
> 这一取舍见 1.4.0 的变更说明。

## 目录

```text
lowydripstonecavefinder.h / .cpp   C++ 引擎与公开 API（含 weirdness 计算）
dripstonecave_finder.cpp           扫描 / 精筛基础算法
LargePitSearch.cpp                 全图大坑搜索
BiomeSampler.cpp / .h              群系采样
OctaveFieldCache.cpp / .h          Cont / Weirdness A 周期表
Bench.h                            --bench 分阶段计时与工作量计数（C0）
SearchConfig.h                     搜索参数常量
Thread.h                           线程池与线程安全结果容器
cxx_compat/pre_cubiomes.h          C++20 std::lerp 兼容处理
lowydripstonecavefinder_cli.cpp    独立 CLI
jni/                               JNI 桥接与签入头文件
cubiomes/                          嵌套 git 子模块
tests/                             冻结参照回归 + 体积积分解析用例（B4）
build_jni_msys2.ps1                MSYS2 构建 JNI DLL
build_cli_msys2.ps1                MSYS2 构建独立 exe
```

## 未发布变更（层范围 + 体积积分，B1–B6）

| 项 | 内容 |
| --- | --- |
| **B6** | `search()` 复用 `Generator`（只随 `mc` 重建），批量搜索省掉每个种子约 0.29 s 的 `setupGenerator` |
| **B1** | `SearchParams` 新增 `yMin`/`yMax`/`coarseY` 与 `validate()`；精采样逐层跑满 `yMin..yMax`；计分改为体积积分（逐层平均，仍为 0..49640）；跨度上限 48 层 |
| **B3** | Java GUI 两页传 `riverWeight = 0.7`；base 默认仍为 `0.75` |
| **B2** | 精采样缓冲按“线程 → 本次搜索”复用（`PreciseScratch`），不再每个候选分配 7~8 次；缓冲由引擎统一释放，避免线程退出时释放导致堆损坏 |
| **B4** | `tests/` 加入 C 项目的 2808 格冻结参照与体积积分解析用例 |
| 构建 | `buildNativeMsys2`/`buildCliMsys2` 声明真实源码输入，头文件改动不再被 Gradle 误判为 UP-TO-DATE |
| 模型 | `model=drowned-weighted-volume-b/{L}` 写入 CLI 报告与 GUI 导出 |

注意：v1.1 的三层多数票结果与 v1.2 的体积积分结果**不可比**；Gui 默认占比为 70%
（见主 README），按需调低可复现更接近旧模型的命中数量。

## 1.1.0 变更（CPU 优化一期 C0–C3）

引擎版本 **1.0.0 → 1.1.0**，全部改动保持结果逐位一致（8 个 golden 组合
fast/precise × 131072²…2000001² 结果行 diff = 0）：

| 项 | 内容 |
| --- | --- |
| **C0** | 新增 `Bench.h` 与 CLI `--bench`：五阶段计时 + 工作量计数 + 峰值工作集 |
| **C1** | A 周期表只服务快速模式：`buildOctaveACache()` 拒绝 Cont<128 / Weird<32；建表失败只降级为实时采样，不再让整次搜索返回空 |
| **C2** | A 表索引由"每八度 2 次除法 + 2 次取模"改为"每样本 1 次探针 + 移位/掩码"，5（3）个八度共用一次格点判定 |
| **C3** | `centerCand` 的 Chebyshev 膨胀由 `O(pass×(2R+1)²)` 写入改为 2D 差分数组 + 一次前缀重建 |

## 许可

本项目以 **MIT** 许可证发布，详见 `LICENSE`（Copyright (c) 2026 SunnySlopes）。

原生引擎基于 [cubiomes](https://github.com/Cubitect/cubiomes) 及其活跃分支
[xpple/cubiomes](https://github.com/xpple/cubiomes)（MIT，Copyright (c) 2020 Cubitect），
上游许可文本保留在 `cubiomes/LICENSE`；战利品相关代码另有
`cubiomes/loot/LICENSE_loot_library.h.txt`（MIT，Copyright (c) 2024 ScriptLine）。
