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

- `riverSearch(...)`：每次命中 5 个 int：`x, z, total, cave, river`；无结果返回 `null`。
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
| `--threads` | CPU 数 | 1–256 |
| `--limit` | `0` | 最多输出条数；0 表示全部 |
| `--out <file>` | 未设置 | 同时把同一份文本报告写入 txt |
| `--weirdness-y-offset <seed>` | 未设置 | 只输出 `weirdnessYOffset(seed)` 分数 |
| `--help` / `-h` | — | 显示帮助 |
| `--version` | — | 显示版本 |

文本报告同时打印到 stdout；指定 `--out` 时再写入文件。搜索报告每行与 GUI
导出一致：

```text
/tp x 64 z s=total/49640 = pct% cave=N river=M
```

pit 模式省略 `cave` / `river`。结果先按 (x,z) 去重，再在每个 64×64 区块保留
加权总面积最大的点，最后按总面积降序输出，与 Java GUI 的显示过滤一致。

## 目录

```text
lowydripstonecavefinder.h / .cpp   C++ 引擎与公开 API（含 weirdness 计算）
dripstonecave_finder.cpp           扫描 / 精筛基础算法
LargePitSearch.cpp                 全图大坑搜索
BiomeSampler.cpp / .h              群系采样
OctaveFieldCache.cpp / .h          Cont / Weirdness A 周期表
SearchConfig.h                     搜索参数常量
Thread.h                           线程池与线程安全结果容器
cxx_compat/pre_cubiomes.h          C++20 std::lerp 兼容处理
lowydripstonecavefinder_cli.cpp    独立 CLI
jni/                               JNI 桥接与签入头文件
cubiomes/                          嵌套 git 子模块
build_jni_msys2.ps1                MSYS2 构建 JNI DLL
build_cli_msys2.ps1                MSYS2 构建独立 exe
```

## 许可

本项目以 **MIT** 许可证发布，详见 `LICENSE`（Copyright (c) 2026 SunnySlopes）。

原生引擎基于 [cubiomes](https://github.com/Cubitect/cubiomes) 及其活跃分支
[xpple/cubiomes](https://github.com/xpple/cubiomes)（MIT，Copyright (c) 2020 Cubitect），
上游许可文本保留在 `cubiomes/LICENSE`；战利品相关代码另有
`cubiomes/loot/LICENSE_loot_library.h.txt`（MIT，Copyright (c) 2024 ScriptLine）。
