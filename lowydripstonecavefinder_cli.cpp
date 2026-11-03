/*
 * LowYDripstoneCaveFinderBase standalone command-line program.
 *
 * The executable links the same static engine and nested cubiomes checkout as
 * the JNI library, so it is self-contained and needs no Java or Minecraft
 * runtime.
 *
 * Contract:
 *   - parameters are --long-options
 *   - the text report is printed to stdout
 *   - --out <file> additionally writes the same report to a UTF-8 txt file
 *   - --weirdness-y-offset <seed> prints only the weirdness y-offset score
 */

#include "lowydripstonecavefinder.h"

#include "Bench.h"
#include "BiomeSampler.h"
#include "GpuCoarse.h"
#include "SearchConfig.h"
#include "cubiomes/biomes.h"
#include "cubiomes/util.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif

namespace {

constexpr const char *kCliVersion = "1.1.0";
constexpr const char *kEngineVersion = "1.1.0";

constexpr int kRingOuter = lowyd::RING_OUTER;
constexpr double kFullRingArea = lowyd::RIVER_AREA_FULL;
constexpr int kOutputCell = lowyd::OUTPUT_REGION_SIZE;

constexpr int kDefaultMinX = -65536;
constexpr int kDefaultMaxX = 65535;
constexpr int kDefaultMinZ = -65536;
constexpr int kDefaultMaxZ = 65535;
constexpr int kMaxSide = 60000001;
constexpr int kWorldLimit = 30000000;
constexpr int64_t kMaxSearchArea = 60000001LL * 60000001LL;
constexpr int kMaxThreads = 256;

struct Options {
    long long seed = 0;
    bool seedSet = false;

    int mc = MC_26_3;

    int minX = kDefaultMinX;
    int maxX = kDefaultMaxX;
    int minZ = kDefaultMinZ;
    int maxZ = kDefaultMaxZ;
    int square = 0;
    bool boundsSet = false;

    int minArea = 0;
    bool minAreaSet = false;
    double riverWeight = 0.75;
    int yMin = lowyd::Y_DEFAULT_MIN;
    int yMax = lowyd::Y_DEFAULT_MAX;
    int coarseY = lowyd::COARSE_Y_DEFAULT;
    int threads = 0;
    int mode = lowyd::SEARCH_MODE_FAST;

    int limit = 0;
    std::string outPath;

    bool weirdnessOnly = false;
    long long weirdnessSeed = 0;

    bool bench = false;
    /** >0: run the GPU-vs-CPU coarse-grid verification over chunks of this side. */
    int gpuVerify = 0;

    bool help = false;
    bool version = false;
};

std::string_view modeName(int mode)
{
    switch (mode)
    {
    case lowyd::SEARCH_MODE_PRECISE: return "precise";
    case lowyd::SEARCH_MODE_FULL_MAP_PIT: return "pit";
    default: return "fast";
    }
}

std::string_view mcDisplayName(int mc)
{
    const char *name = mc2str(mc);
    return (name != nullptr && name[0] != '?') ? std::string_view(name) : std::string_view("unknown");
}

void printUsage(std::ostream &os)
{
    os <<
        "LowYDripstoneCaveFinder standalone CLI\n"
        "\n"
        "Usage:\n"
        "  lowydripstonecavefinder-cli --seed <seed> [options]\n"
        "  lowydripstonecavefinder-cli --weirdness-y-offset <seed> [--out <file>]\n"
        "\n"
        "Required for a search:\n"
        "  --seed <int64>          Minecraft world seed (decimal, may be negative)\n"
        "\n"
        "Search area (default: 131072 x 131072 around 0,0, same as the GUI):\n"
        "  --min-x <int>           default -65536\n"
        "  --max-x <int>           default 65535\n"
        "  --min-z <int>           default -65536\n"
        "  --max-z <int>           default 65535\n"
        "  --square <side>         symmetric area centered on 0; cannot be combined\n"
        "                          with explicit min/max bounds\n"
        "\n"
        "Search parameters:\n"
        "  --mode fast|precise|pit default fast\n"
        "                          fast    = Cont@128 + Weird@32 (GUI Fast Mode)\n"
        "                          precise = Cont@32  + Weird@16 (GUI Precise Mode)\n"
        "                          pit     = full-map large-pit chains\n"
        "  --mc <id|version>       generation base, default 26.3\n"
        "  --min-area <int>        minimum weighted total area; defaults to 70% of\n"
        "                          49640 (34748) for fast/precise and 90% (44676) for pit\n"
        "  --river-weight <float>  river area weight in total = cave + river * weight,\n"
        "                          default 0.75, range [0, 1]\n"
        "  --y-min <int>           lowest sampled layer, default -63 (floor -63)\n"
        "  --y-max <int>           highest sampled layer, default -51 (ceiling 319)\n"
        "                          y-max - y-min + 1 must be <= 48 (hard limit);\n"
        "                          the score is the per-layer average volume\n"
        "  --coarse-y <int>        coarse-stage recall height, default -51.  The\n"
        "                          climate prefilter is Y-independent in this\n"
        "                          engine, so the value is carried for protocol\n"
        "                          compatibility only\n"
        "  --threads <int>         worker threads, default min(CPU count, 256)\n"
        "\n"
        "Output:\n"
        "  --limit <int>           maximum result rows, default 0 = all\n"
        "  --out <file>            also write the text report to <file>\n"
        "  --backend auto|cpu|gpu  coarse-noise backend, default auto\n"
        "                          auto = OpenCL for windows >= 524288 blocks per\n"
        "                          side when at most 12 threads are used, and for\n"
        "                          windows >= 2000001 blocks per side regardless\n"
        "                          (below that the CPU path is faster);\n"
        "                          cpu  = never load OpenCL;\n"
        "                          gpu  = use OpenCL for any window (falls back with\n"
        "                          a reason string when no device is usable).\n"
        "                          The LOWYD_BACKEND environment variable sets the\n"
        "                          default, LOWYD_GPU_DEVICE picks a device by name\n"
        "                          substring (iGPU vs dGPU).\n"
        "  --gpu-verify <side>     compare the OpenCL coarse grids against the CPU\n"
        "                          implementation over chunks of <side> blocks and\n"
        "                          print the mismatch counts (plan §7.2); no search\n"
        "  --bench                 print per-stage timings to stderr after the run\n"
        "                          (phase0 / cont mask / weird ring / climate coarse /\n"
        "                          phase2 / gpu, plus candidate counts and peak RAM)\n"
        "  --help, -h              show this help\n"
        "  --version               show CLI / engine versions\n"
        "\n"
        "Weirdness y-offset (separate interface):\n"
        "  --weirdness-y-offset <seed>\n"
        "                          compute and print only the seed score; the GUI\n"
        "                          itself owns the 0.75 / 0.85 threshold decisions\n"
        "\n"
        "Version names:\n"
        "  1.18, 1.18.2, 1.19, 1.19.2, 1.19.4, 1.20, 1.20.6, 1.21, 1.21.1,\n"
        "  1.21.3, 1.21.4, 1.21.5, 1.21.6, 1.21.9, 1.21.11, 26.1, 26.2, 26.3,\n"
        "  latest. The GUI labels '1.18~26.1' and '26.2+' are accepted as aliases.\n"
        "  A raw cubiomes MCVersion id between MC_1_18 and MC_NEWEST is also\n"
        "  accepted.\n"
        "\n"
        "Examples:\n"
        "  lowydripstonecavefinder-cli --seed 12345 --out result.txt\n"
        "  lowydripstonecavefinder-cli --seed 12345 --mode precise --min-area 20000 \\\n"
        "      --square 262144 --out precise.txt\n"
        "  lowydripstonecavefinder-cli --seed 12345 --mode pit --mc 26.3 \\\n"
        "      --min-x -30000000 --max-x 30000000 --min-z -30000000 --max-z 30000000 \\\n"
        "      --out pit.txt\n"
        "  lowydripstonecavefinder-cli --weirdness-y-offset 12345\n";
}

[[noreturn]] void usageError(const std::string &message)
{
    std::cerr << "error: " << message << "\n\n";
    printUsage(std::cerr);
    std::exit(2);
}

[[noreturn]] void runtimeError(const std::string &message)
{
    std::cerr << "error: " << message << "\n";
    std::exit(1);
}

bool parseLongLong(std::string_view text, long long &value)
{
    if (text.empty())
        return false;
    const char *first = text.data();
    const char *last = text.data() + text.size();
    if (*first == '+')
        ++first;
    if (first == last)
        return false;
    auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc() && result.ptr == last;
}

bool parseInt(std::string_view text, int &value)
{
    long long parsed = 0;
    if (!parseLongLong(text, parsed))
        return false;
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parseDouble(std::string_view text, double &value)
{
    if (text.empty())
        return false;
    std::string copy(text);
    char *end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

bool parseMode(std::string_view text, int &mode)
{
    int numeric = 0;
    if (parseInt(text, numeric))
    {
        if (numeric < lowyd::SEARCH_MODE_FAST || numeric > lowyd::SEARCH_MODE_FULL_MAP_PIT)
            return false;
        mode = numeric;
        return true;
    }
    if (text == "fast")
    {
        mode = lowyd::SEARCH_MODE_FAST;
        return true;
    }
    if (text == "precise")
    {
        mode = lowyd::SEARCH_MODE_PRECISE;
        return true;
    }
    if (text == "pit" || text == "large-pit" || text == "full-map-pit")
    {
        mode = lowyd::SEARCH_MODE_FULL_MAP_PIT;
        return true;
    }
    return false;
}

bool parseMc(std::string_view text, int &mc)
{
    int numeric = 0;
    if (parseInt(text, numeric))
    {
        if (numeric < MC_1_18 || numeric > MC_NEWEST)
            return false;
        mc = numeric;
        return true;
    }

    // GUI version-range labels.
    if (text == "1.18~26.1")
    {
        mc = MC_26_1;
        return true;
    }
    if (text == "26.2+")
    {
        mc = MC_26_3;
        return true;
    }
    if (text == "latest")
    {
        mc = MC_NEWEST;
        return true;
    }

    const std::string copy(text);
    const int parsed = str2mc(copy.c_str());
    if (parsed == MC_UNDEF || parsed < MC_1_18 || parsed > MC_NEWEST)
        return false;
    mc = parsed;
    return true;
}

std::string_view requireValue(int argc, char **argv, int &index, std::string_view option)
{
    if (index + 1 >= argc)
        usageError("missing value for " + std::string(option));
    ++index;
    return argv[index];
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            options.help = true;
        }
        else if (arg == "--version")
        {
            options.version = true;
        }
        else if (arg == "--seed")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseLongLong(value, options.seed))
                usageError("invalid --seed: " + std::string(value));
            options.seedSet = true;
        }
        else if (arg == "--weirdness-y-offset")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseLongLong(value, options.weirdnessSeed))
                usageError("invalid --weirdness-y-offset: " + std::string(value));
            options.weirdnessOnly = true;
            options.seedSet = true;
            options.seed = options.weirdnessSeed;
        }
        else if (arg == "--mc")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseMc(value, options.mc))
                usageError("invalid --mc: " + std::string(value));
        }
        else if (arg == "--min-x")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.minX))
                usageError("invalid --min-x: " + std::string(value));
            options.boundsSet = true;
        }
        else if (arg == "--max-x")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.maxX))
                usageError("invalid --max-x: " + std::string(value));
            options.boundsSet = true;
        }
        else if (arg == "--min-z")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.minZ))
                usageError("invalid --min-z: " + std::string(value));
            options.boundsSet = true;
        }
        else if (arg == "--max-z")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.maxZ))
                usageError("invalid --max-z: " + std::string(value));
            options.boundsSet = true;
        }
        else if (arg == "--square")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.square))
                usageError("invalid --square: " + std::string(value));
            if (options.square <= 0 || options.square > kMaxSide)
                usageError("--square must be between 1 and " + std::to_string(kMaxSide));
        }
        else if (arg == "--min-area")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.minArea))
                usageError("invalid --min-area: " + std::string(value));
            if (options.minArea < 0)
                usageError("--min-area must not be negative");
            options.minAreaSet = true;
        }
        else if (arg == "--river-weight")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseDouble(value, options.riverWeight))
                usageError("invalid --river-weight: " + std::string(value));
            if (options.riverWeight < 0.0 || options.riverWeight > 1.0)
                usageError("--river-weight must be between 0 and 1");
        }
        else if (arg == "--y-min")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.yMin))
                usageError("invalid --y-min: " + std::string(value));
        }
        else if (arg == "--y-max")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.yMax))
                usageError("invalid --y-max: " + std::string(value));
        }
        else if (arg == "--coarse-y")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.coarseY))
                usageError("invalid --coarse-y: " + std::string(value));
        }
        else if (arg == "--threads")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.threads))
                usageError("invalid --threads: " + std::string(value));
            if (options.threads < 1 || options.threads > kMaxThreads)
                usageError("--threads must be between 1 and " + std::to_string(kMaxThreads));
        }
        else if (arg == "--mode")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseMode(value, options.mode))
                usageError("invalid --mode: " + std::string(value));
        }
        else if (arg == "--limit")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.limit))
                usageError("invalid --limit: " + std::string(value));
            if (options.limit < 0)
                usageError("--limit must not be negative");
        }
        else if (arg == "--out")
        {
            options.outPath = std::string(requireValue(argc, argv, i, arg));
            if (options.outPath.empty())
                usageError("--out requires a non-empty path");
        }
        else if (arg == "--bench")
        {
            options.bench = true;
        }
        else if (arg == "--backend")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            lowyd_gpu::Backend backend = lowyd_gpu::Backend::Auto;
            if (!lowyd_gpu::parseBackend(std::string(value).c_str(), &backend))
                usageError("invalid --backend: " + std::string(value));
            lowyd_gpu::setBackend(backend);
        }
        else if (arg == "--gpu-verify")
        {
            const std::string_view value = requireValue(argc, argv, i, arg);
            if (!parseInt(value, options.gpuVerify) || options.gpuVerify <= 0)
                usageError("invalid --gpu-verify: " + std::string(value));
        }
        else
        {
            usageError("unknown option: " + std::string(arg));
        }
    }
    return options;
}

void validateOptions(Options &options)
{
    if (!options.seedSet)
        usageError("--seed is required");

    if (options.square > 0 && options.boundsSet)
        usageError("--square cannot be combined with explicit --min-x/--max-x/--min-z/--max-z");

    if (options.square > 0)
    {
        const int min = -(options.square / 2);
        options.minX = min;
        options.maxX = min + options.square - 1;
        options.minZ = min;
        options.maxZ = min + options.square - 1;
    }

    if (options.minX > options.maxX)
        usageError("--min-x must be <= --max-x");
    if (options.minZ > options.maxZ)
        usageError("--min-z must be <= --max-z");

    if (options.minX < -kWorldLimit || options.maxX > kWorldLimit
        || options.minZ < -kWorldLimit || options.maxZ > kWorldLimit)
        usageError("search bounds must be within the world border (-30000000..30000000)");

    /* B1: same hard rules as lowyd::validate(); the CLI rejects before the run. */
    if (options.yMin > options.yMax)
        usageError("--y-min must be <= --y-max");
    if (options.yMin < lowyd::Y_LIMIT_MIN || options.yMax > lowyd::Y_LIMIT_MAX)
        usageError("layer range must stay within [" + std::to_string(lowyd::Y_LIMIT_MIN) + ", "
                   + std::to_string(lowyd::Y_LIMIT_MAX) + "]");
    if (options.yMax - options.yMin + 1 > lowyd::Y_MAX_SPAN)
        usageError("layer span must not exceed " + std::to_string(lowyd::Y_MAX_SPAN)
                   + " layers (current " + std::to_string(options.yMax - options.yMin + 1)
                   + "); the window cache and the shell model both stop holding");
    if (options.coarseY < lowyd::Y_LIMIT_MIN || options.coarseY > lowyd::Y_LIMIT_MAX)
        usageError("--coarse-y must stay within [" + std::to_string(lowyd::Y_LIMIT_MIN) + ", "
                   + std::to_string(lowyd::Y_LIMIT_MAX) + "]");

    const long long rawWidth = static_cast<long long>(options.maxX) - options.minX + 1;
    const long long rawHeight = static_cast<long long>(options.maxZ) - options.minZ + 1;
    if (rawWidth <= 0 || rawHeight <= 0)
        usageError("search area must be non-empty");
    if (rawWidth * rawHeight > kMaxSearchArea)
        usageError("search area is too large (maximum 60000001 x 60000001 blocks)");

    const long long expandedWidth = rawWidth + 2LL * kRingOuter;
    const long long expandedHeight = rawHeight + 2LL * kRingOuter;
    if (expandedWidth > std::numeric_limits<int>::max()
        || expandedHeight > std::numeric_limits<int>::max())
        usageError("expanded search area is too large for the native API");

    if (!options.minAreaSet)
    {
        const double ratio = options.mode == lowyd::SEARCH_MODE_FULL_MAP_PIT ? 0.90 : 0.70;
        options.minArea = static_cast<int>(kFullRingArea * ratio);
    }

    if (options.threads <= 0)
    {
        const unsigned int hardware = std::thread::hardware_concurrency();
        options.threads = static_cast<int>(std::min<unsigned int>(
            static_cast<unsigned int>(kMaxThreads), hardware == 0 ? 1u : hardware));
    }
}

int floorDiv(int value, int divisor)
{
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
        --quotient;
    return quotient;
}

uint64_t packXZ(int x, int z)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
        | static_cast<uint32_t>(z);
}

int64_t originDistanceSquared(const lowyd::Hit &hit)
{
    return static_cast<int64_t>(hit.x) * hit.x + static_cast<int64_t>(hit.z) * hit.z;
}

bool betterHit(const lowyd::Hit &candidate, const lowyd::Hit &current)
{
    if (candidate.total != current.total)
        return candidate.total > current.total;
    if (candidate.cave != current.cave)
        return candidate.cave > current.cave;
    if (candidate.river != current.river)
        return candidate.river > current.river;
    const int64_t candidateDistance = originDistanceSquared(candidate);
    const int64_t currentDistance = originDistanceSquared(current);
    if (candidateDistance != currentDistance)
        return candidateDistance < currentDistance;
    if (candidate.x != current.x)
        return candidate.x < current.x;
    return candidate.z < current.z;
}

/** Same display filtering as LowYDripstoneCaveSearchRunner.filterResultsForOutput. */
std::vector<lowyd::Hit> filterHits(const std::vector<lowyd::Hit> &raw, int limit)
{
    std::unordered_map<uint64_t, lowyd::Hit> byXZ;
    byXZ.reserve(raw.size());
    for (const auto &hit: raw)
    {
        const uint64_t key = packXZ(hit.x, hit.z);
        auto found = byXZ.find(key);
        if (found == byXZ.end())
            byXZ.emplace(key, hit);
        else if (betterHit(hit, found->second))
            found->second = hit;
    }

    std::unordered_map<uint64_t, lowyd::Hit> byCell;
    byCell.reserve(byXZ.size());
    for (const auto &entry: byXZ)
    {
        const lowyd::Hit &hit = entry.second;
        const uint64_t key = packXZ(floorDiv(hit.x, kOutputCell), floorDiv(hit.z, kOutputCell));
        auto found = byCell.find(key);
        if (found == byCell.end())
            byCell.emplace(key, hit);
        else if (betterHit(hit, found->second))
            found->second = hit;
    }

    std::vector<lowyd::Hit> filtered;
    filtered.reserve(byCell.size());
    for (const auto &entry: byCell)
        filtered.push_back(entry.second);

    std::ranges::sort(filtered, [](const lowyd::Hit &left, const lowyd::Hit &right) {
        if (left.total != right.total)
            return left.total > right.total;
        if (left.cave != right.cave)
            return left.cave > right.cave;
        if (left.river != right.river)
            return left.river > right.river;
        const int64_t leftDistance = originDistanceSquared(left);
        const int64_t rightDistance = originDistanceSquared(right);
        if (leftDistance != rightDistance)
            return leftDistance < rightDistance;
        if (left.x != right.x)
            return left.x < right.x;
        return left.z < right.z;
    });

    if (limit > 0 && filtered.size() > static_cast<size_t>(limit))
        filtered.resize(static_cast<size_t>(limit));
    return filtered;
}

std::string formatSearchReport(const Options &options, int startX, int startZ,
                               int width, int height, const std::vector<lowyd::Hit> &hits,
                               size_t totalHits)
{
    std::ostringstream out;
    out << "# LowYDripstoneCaveFinder CLI " << kCliVersion << "\n";
    out << "# model=" << lowyd::modelId(options.yMin, options.yMax)
        << " y=[" << options.yMin << "," << options.yMax << "]"
        << " L=" << (options.yMax - options.yMin + 1)
        << " coarseY=" << options.coarseY << "\n";
    out << "# engine=LowYDripstoneCaveFinderBase " << kEngineVersion
        << " seed=" << options.seed
        << " mc=" << mcDisplayName(options.mc) << "(" << options.mc << ")"
        << " mode=" << modeName(options.mode)
        << " search=[" << options.minX << "," << options.maxX << "]x["
        << options.minZ << "," << options.maxZ << "]"
        << " start=" << startX << "," << startZ
        << " size=" << width << "x" << height
        << " minArea=" << options.minArea
        << " riverWeight=" << options.riverWeight
        << " threads=" << options.threads
        << " hits=" << totalHits;
    if (hits.size() != totalHits)
        out << " shown=" << hits.size();
    out << "\n";

    const int fullAreaInt = static_cast<int>(kFullRingArea);
    out << std::fixed << std::setprecision(2);
    for (const lowyd::Hit &hit: hits)
    {
        const double ratioPercent = hit.total / kFullRingArea * 100.0;
        if (options.mode == lowyd::SEARCH_MODE_FULL_MAP_PIT)
        {
            out << "/tp " << hit.x << " 64 " << hit.z
                << " s=" << hit.total << "/" << fullAreaInt
                << " = " << ratioPercent << "%\n";
        }
        else
        {
            out << "/tp " << hit.x << " 64 " << hit.z
                << " s=" << hit.total << "/" << fullAreaInt
                << " = " << ratioPercent << "%"
                << " cave=" << hit.cave
                << " river=" << hit.river << "\n";
        }
    }
    return out.str();
}

std::string formatWeirdnessReport(long long seed, float score)
{
    std::ostringstream out;
    out << "# LowYDripstoneCaveFinder CLI " << kCliVersion << "\n";
    out << "# engine=LowYDripstoneCaveFinderBase " << kEngineVersion << "\n";
    out << std::fixed << std::setprecision(6);
    out << "weirdnessYOffset(" << seed << ") = " << score << "\n";
    out << "# GUI thresholds: lucky < 0.75 (first tab prompt), fast-search warning > 0.85\n";
    return out.str();
}

void writeReport(const std::string &report, const std::string &outPath)
{
    if (outPath.empty())
        return;
    std::ofstream outFile(outPath, std::ios::out | std::ios::trunc);
    if (!outFile)
        runtimeError("cannot open --out file: " + outPath);
    outFile << report;
    if (!outFile)
        runtimeError("failed to write --out file: " + outPath);
    std::cerr << "report written to " << outPath << "\n";
}

/** Peak working set of this process in MiB, or 0 when unsupported. */
double peakWorkingSetMiB()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

/**
 * C0: per-stage timing summary. Printed to stderr so stdout keeps exactly the
 * text report that --out writes.
 */
std::string formatBenchReport(double wallSeconds)
{
    const lowyd_bench::Counters &c = lowyd_bench::counters();
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);

    double totalMs = 0.0;
    for (int i = 0; i < lowyd_bench::STAGE_COUNT; i++)
        totalMs += static_cast<double>(c.ns[i].load()) / 1e6;

    out << "=== bench: stage timings ===\n";
    out << "(ms is thread time summed over all workers; % shares are of that sum,\n"
        << " so a well-parallelised run has a sum well above the wall clock)\n";
    out << std::left << std::setw(24) << "stage" << std::right << std::setw(11) << "ms"
        << std::setw(8) << "%" << std::setw(10) << "calls" << "\n";
    for (int i = 0; i < lowyd_bench::STAGE_COUNT; i++)
    {
        const double ms = static_cast<double>(c.ns[i].load()) / 1e6;
        const double pct = totalMs > 0.0 ? 100.0 * ms / totalMs : 0.0;
        out << std::left << std::setw(24) << lowyd_bench::stageName(static_cast<lowyd_bench::Stage>(i))
            << std::right << std::setw(11) << ms << std::setw(8) << pct
            << std::setw(10) << c.calls[i].load() << "\n";
    }
    out << std::left << std::setw(24) << "sum of stages" << std::right << std::setw(11) << totalMs
        << "\n";
    out << std::left << std::setw(24) << "wall clock" << std::right << std::setw(11)
        << wallSeconds * 1000.0 << "\n";

    const uint64_t tiles = c.contTiles.load();
    const uint64_t passed = c.contTilesPass.load();
    out << "\n=== bench: work counted ===\n";
    out << "chunks                " << c.chunks.load()
        << "   on A-table lattice  " << c.latticeChunks.load() << "\n";
    out << "cont prefilter tiles  " << tiles << "   passed  " << passed;
    if (tiles > 0)
        out << std::setprecision(2) << " (" << (100.0 * static_cast<double>(passed) / static_cast<double>(tiles)) << "%)";
    out << std::setprecision(1) << "\n";
    out << "ring centers summed   " << c.ringCenters.load() << "\n";
    out << "ring candidates       " << c.ringCandidates.load()
        << "   max per chunk  " << c.ringCandidatesMax.load() << "\n";
    out << "climate subsearches   " << c.coarseSubsearches.load()
        << "   coarse cells  " << c.coarseCells.load() << "\n";
    out << "phase2 candidates     " << c.phase2Candidates.load() << "\n";
    out << "weird cells evaluated " << c.weirdCells.load() << "\n";
    out << "dilation writes saved " << c.dilateWrites.load()
        << "   (naive Chebyshev writes replaced by the difference array)\n";

    /* N0: per-unit cost, the only figure that is comparable across variants. */
    const uint64_t wcells = c.weirdCells.load();
    const uint64_t ccells = c.coarseCells.load();
    if (wcells || ccells || tiles)
    {
        out << "\n=== bench: ns per unit (thread time) ===\n";
        if (tiles)
            out << "cont prefilter  " << (double) c.ns[lowyd_bench::STAGE_CONT_MASK].load() / (double) tiles
                << " ns/tile\n";
        if (wcells)
            out << "weird ring      " << (double) c.ns[lowyd_bench::STAGE_WEIRD_RING].load() / (double) wcells
                << " ns/cell\n";
        if (ccells)
            out << "climate coarse  " << (double) c.ns[lowyd_bench::STAGE_CLIMATE_COARSE].load() / (double) ccells
                << " ns/cell\n";
    }

    const double tableMiB = static_cast<double>(c.phase0Bytes.load()) / (1024.0 * 1024.0);
    out << "A tables              " << tableMiB << " MiB, "
        << c.phase0Samples.load() << " cells built (0 = phase 0 skipped)\n";

    const double peakMiB = peakWorkingSetMiB();
    if (peakMiB > 0.0)
        out << "peak working set      " << peakMiB << " MiB\n";
    return out.str();
}

/** Backend device / counters, printed next to --bench. */
std::string formatGpuReport()
{
    const lowyd_gpu::Info &info = lowyd_gpu::info();
    std::ostringstream out;
    out << "\n=== bench: coarse backend ===\n";
    out << "requested             " << lowyd_gpu::backendName(lowyd_gpu::backend()) << "\n";
    if (!info.probed)
    {
        out << "state                 not probed (window below the GPU gate)\n";
        return out.str();
    }
    if (!info.usable)
    {
        out << "state                 cpu fallback (" << info.reason << ")\n";
        return out.str();
    }
    out << "device                " << info.device << "\n";
    out << "work group            " << info.workGroupX << "x" << info.workGroupY
        << " (device max " << info.maxWorkGroup << ")\n";
    out << "submissions           " << info.submissions
        << "   coarse cells " << info.cells << "\n";
    if (info.fallbacks)
        out << "runtime fallbacks     " << info.fallbacks << " (" << info.reason << ")\n";
    out << "host queueing/waiting " << (info.hostEnqueueNs / 1e6) << " / "
        << (info.hostWaitNs / 1e6) << " ms (all threads)\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// --gpu-verify: the numeric acceptance test of docs/GPU_ACCEL_PLAN.md §7.2.
//
// The OpenCL coarse grids are compared cell by cell against the frozen CPU
// implementation over real chunk-sized windows: the phase-1 chain (cont grid ->
// dilation -> masked weirdness grid) and the phase-1 ClimateCoarse grid.
// ---------------------------------------------------------------------------
struct GridDiff
{
    size_t cells = 0;
    size_t diff = 0;
    int firstX = 0;
    int firstZ = 0;
    bool any = false;
};

GridDiff compareGrids(const std::vector<uint8_t> &cpu, const std::vector<uint8_t> &gpu, int W)
{
    GridDiff result;
    result.cells = std::min(cpu.size(), gpu.size());
    for (size_t i = 0; i < result.cells; i++)
    {
        if ((cpu[i] != 0) == (gpu[i] != 0))
            continue;
        result.diff++;
        if (!result.any)
        {
            result.any = true;
            result.firstX = static_cast<int>(i % static_cast<size_t>(W));
            result.firstZ = static_cast<int>(i / static_cast<size_t>(W));
        }
    }
    return result;
}

void printGridDiff(const char *label, const GridDiff &diff)
{
    const double fraction = diff.cells ? static_cast<double>(diff.diff)
                                             / static_cast<double>(diff.cells) : 0.0;
    std::printf("%-40s cells=%-8zu diff=%-6zu (%.3e)", label, diff.cells, diff.diff, fraction);
    if (diff.any)
        std::printf(" first@(%d,%d)", diff.firstX, diff.firstZ);
    std::printf("\n");
}

int runGpuVerify(const Options &options)
{
    Generator g;
    setupGenerator(&g, options.mc, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, static_cast<uint64_t>(options.seed));

    if (!lowyd_gpu::activate(&g, true))
    {
        const lowyd_gpu::Info &info = lowyd_gpu::info();
        std::printf("gpu verify: backend unavailable (%s)\n", info.reason.c_str());
        return 1;
    }

    const int contScale = options.mode == lowyd::SEARCH_MODE_PRECISE
        ? lowyd::PHASE1_CONT_SCALE_PRECISE : lowyd::PHASE1_CONT_SCALE_FAST;
    const int weirdScale = options.mode == lowyd::SEARCH_MODE_PRECISE
        ? lowyd::PHASE1_WEIRD_SCALE_PRECISE : lowyd::PHASE1_WEIRD_SCALE_FAST;
    const int side = options.gpuVerify;
    const int startX = options.minX - kRingOuter;
    const int startZ = options.minZ - kRingOuter;

    std::printf("gpu verify: device=%s mode=%s chunk=%d contScale=%d weirdScale=%d\n",
                lowyd_gpu::info().device.c_str(), std::string(modeName(options.mode)).c_str(),
                side, contScale, weirdScale);

    /* G2: sustained device throughput and the latency of one submission. */
    {
        for (int benchSide: { 64, 256, 2048 })
        {
            double latencyMicros = 0.0;
            const double rate = lowyd_gpu::benchmarkGrid(benchSide, &latencyMicros);
            if (rate > 0.0)
                std::printf("gpu verify: side=%-5d throughput %8.1f M cells/s, "
                            "%.1f ns/cell, one submission %.0f us\n",
                            benchSide, rate / 1e6, 1e9 / rate, latencyMicros);
        }
    }

    const int contW = side / contScale;
    const int contH = side / contScale;
    const int weirdW = side / weirdScale;
    const int weirdH = side / weirdScale;
    const int dilate = 1;

    GridDiff totalCont;
    GridDiff totalWeird;
    for (int k = 0; k < 2; k++)
    {
        const int cx = startX + k * side;
        const int cz = startZ + k * side;

        /* CPU reference: continentalness grid, then Chebyshev dilation, then
         * the weirdness gate inside the dilated mask. */
        std::vector<uint8_t> cpuCont(static_cast<size_t>(contW) * contH, 0);
        for (int y = 0; y < contH; y++)
        {
            const int wz = cz + y * contScale + contScale / 2;
            for (int x = 0; x < contW; x++)
            {
                const int wx = cx + x * contScale + contScale / 2;
                cpuCont[static_cast<size_t>(y) * contW + x] =
                    passContinentalnessPartialThr(&g.bn, wx / 4, wz / 4,
                                                  SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD)
                    ? 1 : 0;
            }
        }
        std::vector<uint8_t> cpuMask = cpuCont;
        for (int y = 0; y < contH; y++)
        {
            for (int x = 0; x < contW; x++)
            {
                if (!cpuCont[static_cast<size_t>(y) * contW + x])
                    continue;
                for (int dy = -dilate; dy <= dilate; dy++)
                {
                    for (int dx = -dilate; dx <= dilate; dx++)
                    {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= contW || ny >= contH)
                            continue;
                        cpuMask[static_cast<size_t>(ny) * contW + nx] = 1;
                    }
                }
            }
        }

        std::vector<uint8_t> cpuWeird(static_cast<size_t>(weirdW) * weirdH, 0);
        for (int y = 0; y < weirdH; y++)
        {
            const int wz = cz + y * weirdScale + weirdScale / 2;
            for (int x = 0; x < weirdW; x++)
            {
                const int wx = cx + x * weirdScale + weirdScale / 2;
                const int tx = (wx - cx) / contScale;
                const int tz = (wz - cz) / contScale;
                if (!cpuMask[static_cast<size_t>(tz) * contW + tx])
                    continue;
                cpuWeird[static_cast<size_t>(y) * weirdW + x] =
                    passCaveWeirdness(&g.bn, wx / 4, wz / 4, COARSE_SAMPLE_FLAGS) ? 1 : 0;
            }
        }

        std::vector<uint8_t> gpuCont;
        std::vector<uint8_t> gpuWeird;
        if (!lowyd_gpu::evalChunk(&g, cx, cz, side, side, contScale, weirdScale,
                                  SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD, dilate,
                                  gpuCont, gpuWeird, nullptr))
        {
            std::printf("gpu verify: evalChunk failed\n");
            lowyd_gpu::deactivate();
            return 1;
        }

        const GridDiff contDiff = compareGrids(cpuMask, gpuCont, contW);
        const GridDiff weirdDiff = compareGrids(cpuWeird, gpuWeird, weirdW);
        char label[96];
        std::snprintf(label, sizeof(label), "cont mask (dilate) chunk %d @%d,%d", k, cx, cz);
        printGridDiff(label, contDiff);
        std::snprintf(label, sizeof(label), "weird grid  chunk %d @%d,%d", k, cx, cz);
        printGridDiff(label, weirdDiff);
        totalCont.cells += contDiff.cells;
        totalCont.diff += contDiff.diff;
        totalWeird.cells += weirdDiff.cells;
        totalWeird.diff += weirdDiff.diff;
    }
    printGridDiff("cont mask  TOTAL", totalCont);
    printGridDiff("weird grid TOTAL", totalWeird);

    /* ClimateCoarse: 512-block windows at scale 4, as phase 1 uses them. */
    GridDiff totalClimate;
    for (int k = 0; k < 2; k++)
    {
        const int cx = startX + k * 512;
        const int cz = startZ + k * 512;
        const int W = 512 / SearchConfig::PHASE1_CLIMATE_GRID_SCALE;
        const int H = W;
        std::vector<uint8_t> cpu(static_cast<size_t>(W) * H, 0);
        for (int y = 0; y < H; y++)
        {
            const int wz = cz + y * 4 + 2;
            for (int x = 0; x < W; x++)
            {
                const int wx = cx + x * 4 + 2;
                cpu[static_cast<size_t>(y) * W + x] =
                    passCoarseCaveCell(&g.bn, wx / 4, wz / 4, COARSE_SAMPLE_FLAGS) ? 1 : 0;
            }
        }
        std::vector<uint8_t> gpu;
        if (!lowyd_gpu::evalGrid(&g, cx, cz, W, H, 4, lowyd_gpu::GRID_CLIMATE,
                                 CONT_PARTIAL_FINAL_THRESHOLD, nullptr, 0, 0, 0, 0, 0,
                                 gpu, nullptr))
        {
            std::printf("gpu verify: climate evalGrid failed\n");
            lowyd_gpu::deactivate();
            return 1;
        }
        char label[96];
        std::snprintf(label, sizeof(label), "climate grid 512 @%d,%d", cx, cz);
        const GridDiff climateDiff = compareGrids(cpu, gpu, W);
        printGridDiff(label, climateDiff);
        totalClimate.cells += climateDiff.cells;
        totalClimate.diff += climateDiff.diff;
    }
    printGridDiff("climate grid TOTAL", totalClimate);

    lowyd_gpu::deactivate();
    return 0;
}

} // namespace

int run(int argc, char **argv)
{
    /* Backend default from the environment; --backend overrides it. */
    if (const char *envBackend = std::getenv("LOWYD_BACKEND"))
    {
        lowyd_gpu::Backend backend = lowyd_gpu::Backend::Auto;
        if (lowyd_gpu::parseBackend(envBackend, &backend))
            lowyd_gpu::setBackend(backend);
    }

    Options options = parseOptions(argc, argv);

    if (options.help)
    {
        printUsage(std::cout);
        return 0;
    }
    if (options.version)
    {
        std::cout << "lowydripstonecavefinder-cli " << kCliVersion
                  << " (LowYDripstoneCaveFinderBase " << kEngineVersion
                  << ", cubiomes MC_1_18=" << MC_1_18
                  << " MC_26_1=" << MC_26_1
                  << " MC_26_3=" << MC_26_3 << ")\n";
        return 0;
    }

    if (options.weirdnessOnly)
    {
        const float score = lowyd::weirdnessYOffset(static_cast<uint64_t>(options.weirdnessSeed));
        const std::string report = formatWeirdnessReport(options.weirdnessSeed, score);
        std::cout << report;
        writeReport(report, options.outPath);
        return 0;
    }

    validateOptions(options);

    if (options.gpuVerify > 0)
        return runGpuVerify(options);

    lowyd::SearchParams params;
    params.seed = static_cast<uint64_t>(options.seed);
    params.startX = options.minX - kRingOuter;
    params.startZ = options.minZ - kRingOuter;
    params.width = static_cast<int>((static_cast<long long>(options.maxX) - options.minX + 1) + 2LL * kRingOuter);
    params.height = static_cast<int>((static_cast<long long>(options.maxZ) - options.minZ + 1) + 2LL * kRingOuter);
    params.minArea = options.minArea;
    params.preserveRange = lowyd::DEFAULT_PRESERVE_RANGE;
    params.riverWeight = static_cast<float>(options.riverWeight);
    params.yMin = options.yMin;
    params.yMax = options.yMax;
    params.coarseY = options.coarseY;
    params.numThreads = options.threads;
    params.mc = options.mc;
    params.searchMode = options.mode;
    if (options.mode == lowyd::SEARCH_MODE_FAST)
    {
        params.contScale = lowyd::PHASE1_CONT_SCALE_FAST;
        params.weirdScale = lowyd::PHASE1_WEIRD_SCALE_FAST;
    }
    else if (options.mode == lowyd::SEARCH_MODE_PRECISE)
    {
        params.contScale = lowyd::PHASE1_CONT_SCALE_PRECISE;
        params.weirdScale = lowyd::PHASE1_WEIRD_SCALE_PRECISE;
    }

    std::cerr << "searching seed=" << options.seed
              << " mc=" << mcDisplayName(options.mc)
              << " mode=" << modeName(options.mode)
              << " backend=" << lowyd_gpu::backendName(lowyd_gpu::backend())
              << " area=[" << options.minX << "," << options.maxX << "]x["
              << options.minZ << "," << options.maxZ << "]"
              << " minArea=" << options.minArea
              << " riverWeight=" << options.riverWeight
              << " y=[" << options.yMin << "," << options.yMax << "]"
              << " threads=" << options.threads << "\n";

    const auto started = std::chrono::steady_clock::now();

    if (options.bench)
    {
        lowyd_bench::reset();
        lowyd_bench::setEnabled(true);
    }

    // Lightweight stderr progress; stdout stays reserved for the report.
    std::atomic_bool monitorDone{false};
    std::thread monitor([&monitorDone]() {
        int lastPercent = -1;
        while (!monitorDone.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const lowyd::ProgressView progress = lowyd::getProgress();
            if (!progress.available || progress.total <= 0)
                continue;
            const int percent = static_cast<int>(
                std::min<long long>(100, static_cast<long long>(progress.current) * 100 / progress.total));
            if (percent != lastPercent)
            {
                std::cerr << "phase=" << progress.phase
                          << " " << progress.current << "/" << progress.total
                          << " (" << percent << "%)\n";
                lastPercent = percent;
            }
        }
    });

    auto rawHits = std::vector<lowyd::Hit>{};
    try
    {
        rawHits = lowyd::search(params);
    }
    catch (...)
    {
        monitorDone.store(true);
        monitor.join();
        throw;
    }
    monitorDone.store(true);
    monitor.join();

    const size_t totalHits = rawHits.size();
    auto hits = filterHits(rawHits, options.limit);
    const std::string report = formatSearchReport(
        options, params.startX, params.startZ, params.width, params.height, hits, totalHits);

    std::cout << report;
    writeReport(report, options.outPath);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cerr << "done in " << elapsed.count() << " ms, " << hits.size() << " hit(s)";
    if (hits.size() != totalHits)
        std::cerr << " (shown; " << totalHits << " raw)";
    std::cerr << "\n";

    if (options.bench)
    {
        lowyd_bench::setEnabled(false);
        std::cerr << "\n" << formatBenchReport(static_cast<double>(elapsed.count()) / 1000.0);
        std::cerr << formatGpuReport();
    }
    return 0;
}

int main(int argc, char **argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception &error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "error: search failed with an unknown error\n";
        return 1;
    }
}
