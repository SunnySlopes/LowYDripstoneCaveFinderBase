//
// B4: frozen original-generation reference for the block-biome layer.
//
// The TSV holds the two generation bases the finder actually searches
// ("1.18~26.1" -> MC_26_1, "26.2+" -> MC_26_3), one seed (6900457) and the
// four search-site windows around (816,-542), (316087,-267972),
// (16043916,15695385) and (-875950,346633), 3x3x13 block biomes per window:
// 2 versions * 4 windows * 117 = 936 rows.  The dripstone cave finder must
// reproduce every row through cubiomes' own point query, which catches
// "self-consistent but wrong" drift that a build-to-build diff cannot see.
//
// Every (version, window) must be present: a missing window is a failure, not
// a silently smaller test.  Rows outside the searched y range are allowed so
// the windows can be extended for diagnosis.
//
// Also checks that genBiomes() over a 5x5x13 volume agrees cell for cell with
// getBiomeAt() for both generation bases: the precise pass reads genBiomes()
// output directly, so this is the aggregate/window equivalent of the TSV.
//
// Usage: column_reference [drowned-original-reference.tsv]
//

#include "cubiomes/biomes.h"
#include "cubiomes/generator.h"
#include "cubiomes/util.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *what, const std::string &detail)
{
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

/** Minecraft's decimal seed parsing (Java Long.parseLong), negatives allowed. */
bool parseSeed(const std::string &text, uint64_t &seed)
{
    try
    {
        size_t used = 0;
        const long long value = std::stoll(text, &used, 10);
        if (used != text.size())
            return false;
        seed = static_cast<uint64_t>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

int mcForVersion(const std::string &version)
{
    if (version == "1.18~26.1")
        return MC_26_1;
    if (version == "26.2+")
        return MC_26_3;
    /* Any other label is accepted for diagnosis (str2mc), as long as cubiomes
     * knows it; the frozen asset itself only ships the two bases above. */
    const int mc = str2mc(version.c_str());
    if (mc == MC_UNDEF || mc < MC_1_18 || mc > MC_NEWEST)
        return -1;
    return mc;
}

/** The generation bases the finder searches, and the windows they must cover. */
struct Window
{
    int cx;
    int cz;
};

const char *const GENDERS[] = {"1.18~26.1", "26.2+"};

constexpr Window WINDOWS[] = {
    {816, -542},
    {316087, -267972},
    {16043916, 15695385},
    {-875950, 346633},
};

constexpr int WINDOW_SIZE = 3; /* 3x3 footprint, centred on the search site */

/**
 * Verify that one genBiomes() volume equals independent getBiomeAt() point
 * queries.  Mirrors river-native/tests/column_reference.cpp.
 */
uint64_t verifyVolume(int mc, uint64_t seed)
{
    std::unique_ptr<Generator> g(new Generator());
    setupGenerator(g.get(), mc, 0);
    applySeed(g.get(), DIM_OVERWORLD, seed);

    constexpr int sx = 5, sy = 13, sz = 5;
    uint64_t checked = 0;
    for (int base : {-29999800, -257, 17, 29999800})
    {
        Range r{1, base, -base, sx, sz, -63, sy};
        const size_t need = getMinCacheSize(g.get(), r.scale, r.sx, r.sy, r.sz);
        std::vector<int> volume(need, 0);
        if (genBiomes(g.get(), volume.data(), r) != 0)
        {
            check(false, "genBiomes", std::string("version ") + std::to_string(mc));
            continue;
        }
        for (int y = 0; y < sy; ++y)
        {
            for (int z = 0; z < sz; ++z)
            {
                for (int x = 0; x < sx; ++x)
                {
                    const int expected = getBiomeAt(g.get(), 1, base + x, -63 + y, -base + z);
                    const int actual = volume[(size_t) (y * sx * sz + z * sx + x)];
                    if (expected != actual)
                    {
                        check(false, "genBiomes/getBiomeAt",
                              "version " + std::to_string(mc) + " base "
                                  + std::to_string(base) + " xyz "
                                  + std::to_string(x) + "," + std::to_string(y) + ","
                                  + std::to_string(z));
                        return checked;
                    }
                    ++checked;
                }
            }
        }
    }
    return checked;
}

/** Offset of a reference coordinate from the nearest search-site window, or -1. */
int windowIndex(int x, int z)
{
    for (size_t i = 0; i < sizeof(WINDOWS) / sizeof(WINDOWS[0]); ++i)
    {
        const int dx = x - WINDOWS[i].cx;
        const int dz = z - WINDOWS[i].cz;
        if (std::abs(dx) <= WINDOW_SIZE / 2 && std::abs(dz) <= WINDOW_SIZE / 2)
            return (int) i;
    }
    return -1;
}

/** Replay the frozen TSV: version seed x y z biomeId biomeName. */
uint64_t verifyReference(const std::string &path)
{
    std::ifstream input(path);
    if (!input)
    {
        std::fprintf(stderr, "cannot open reference TSV: %s\n", path.c_str());
        ++failures;
        return 0;
    }

    /* coverage[base][window] -> rows seen, so a dropped window fails loudly. */
    uint64_t coverage[sizeof(GENDERS) / sizeof(GENDERS[0])]
                      [sizeof(WINDOWS) / sizeof(WINDOWS[0])] = {};
    int gender = -1;

    std::string version, seedText, name, previousVersion;
    uint64_t seed = 0;
    int x = 0, y = 0, z = 0, id = 0;
    std::unique_ptr<Generator> g;
    int mc = 0;
    uint64_t applied = 0;
    uint64_t checked = 0;

    while (input >> version >> seedText >> x >> y >> z >> id >> name)
    {
        if (version != previousVersion)
        {
            mc = mcForVersion(version);
            if (mc < 0)
            {
                check(false, "version", version);
                return checked;
            }
            gender = -1;
            for (size_t i = 0; i < sizeof(GENDERS) / sizeof(GENDERS[0]); ++i)
                if (version == GENDERS[i])
                    gender = (int) i;
            g = std::make_unique<Generator>();
            setupGenerator(g.get(), mc, 0);
            if (!parseSeed(seedText, seed))
            {
                check(false, "seed", seedText);
                return checked;
            }
            applySeed(g.get(), DIM_OVERWORLD, seed);
            previousVersion = version;
            applied = seed;
        }
        else if (!parseSeed(seedText, seed) || applied != seed)
        {
            if (!parseSeed(seedText, seed))
            {
                check(false, "seed", seedText);
                return checked;
            }
            applySeed(g.get(), DIM_OVERWORLD, seed);
            applied = seed;
        }

        const int w = windowIndex(x, z);
        if (w < 0)
        {
            check(false, "window",
                  version + " row at " + std::to_string(x) + "," + std::to_string(y) + ","
                      + std::to_string(z) + " is outside every search-site window");
            return checked;
        }
        if (gender < 0)
        {
            check(false, "version", version + " (not a searched generation base)");
            return checked;
        }
        ++coverage[gender][w];

        const int actual = getBiomeAt(g.get(), 1, x, y, z);
        if (actual != id)
        {
            check(false, "TSV block biome",
                  version + " seed " + seedText + " at " + std::to_string(x) + ","
                      + std::to_string(y) + "," + std::to_string(z) + " expected "
                      + std::to_string(id) + " (" + name + ") got "
                      + std::to_string(actual));
            return checked;
        }
        ++checked;
    }

    for (size_t gi = 0; gi < sizeof(GENDERS) / sizeof(GENDERS[0]); ++gi)
    {
        for (size_t wi = 0; wi < sizeof(WINDOWS) / sizeof(WINDOWS[0]); ++wi)
        {
            if (coverage[gi][wi] != 0)
                continue;
            check(false, "TSV coverage",
                  std::string(GENDERS[gi]) + " around (" + std::to_string(WINDOWS[wi].cx)
                      + "," + std::to_string(WINDOWS[wi].cz) + ") has no rows");
        }
    }
    return checked;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1]
                                      : "drowned-original-reference.tsv";

    /* The volume check is per generation base; the found sites are always
     * searched in one of the two bases the GUI exposes. */
    uint64_t volumeCells = 0;
    for (int mc : {MC_26_1, MC_26_3})
        volumeCells += verifyVolume(mc, 8050192242802360875ULL);

    const uint64_t referenceCells = verifyReference(path);

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    /* Every generation base x every search window x (3x3 footprint x 13 y
     * levels), derived from the tables above so adding a window cannot leave
     * a stale expectation behind. */
    constexpr uint64_t kExpectedRows =
        (uint64_t) (sizeof(GENDERS) / sizeof(GENDERS[0]))
        * (uint64_t) (sizeof(WINDOWS) / sizeof(WINDOWS[0]))
        * (uint64_t) (WINDOW_SIZE * WINDOW_SIZE * 13);
    if (referenceCells != kExpectedRows)
    {
        std::fprintf(stderr, "reference TSV has %llu rows, expected %llu"
                             " (%zu generation bases x %zu search windows x 3x3x13)\n",
                     (unsigned long long) referenceCells, (unsigned long long) kExpectedRows,
                     sizeof(GENDERS) / sizeof(GENDERS[0]),
                     sizeof(WINDOWS) / sizeof(WINDOWS[0]));

        return 1;
    }
    std::printf("%llu volume cells and %llu frozen reference cells match independent point queries\n",
                (unsigned long long) volumeCells, (unsigned long long) referenceCells);
    return 0;
}
