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

#include "cubiomes/biomes.h"
#include "cubiomes/util.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
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

namespace {

constexpr const char *kCliVersion = "1.0.0";
constexpr const char *kEngineVersion = "1.0.0";

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
    int threads = 0;
    int mode = lowyd::SEARCH_MODE_FAST;

    int limit = 0;
    std::string outPath;

    bool weirdnessOnly = false;
    long long weirdnessSeed = 0;

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
        "  --threads <int>         worker threads, default min(CPU count, 256)\n"
        "\n"
        "Output:\n"
        "  --limit <int>           maximum result rows, default 0 = all\n"
        "  --out <file>            also write the text report to <file>\n"
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

} // namespace

int run(int argc, char **argv)
{
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

    lowyd::SearchParams params;
    params.seed = static_cast<uint64_t>(options.seed);
    params.startX = options.minX - kRingOuter;
    params.startZ = options.minZ - kRingOuter;
    params.width = static_cast<int>((static_cast<long long>(options.maxX) - options.minX + 1) + 2LL * kRingOuter);
    params.height = static_cast<int>((static_cast<long long>(options.maxZ) - options.minZ + 1) + 2LL * kRingOuter);
    params.minArea = options.minArea;
    params.preserveRange = lowyd::DEFAULT_PRESERVE_RANGE;
    params.riverWeight = static_cast<float>(options.riverWeight);
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
              << " area=[" << options.minX << "," << options.maxX << "]x["
              << options.minZ << "," << options.maxZ << "]"
              << " minArea=" << options.minArea
              << " riverWeight=" << options.riverWeight
              << " threads=" << options.threads << "\n";

    const auto started = std::chrono::steady_clock::now();

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
