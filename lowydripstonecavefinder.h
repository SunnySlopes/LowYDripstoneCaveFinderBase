//
// LowYDripstoneCaveFinderBase public engine API.
//
// This header is the contract shared by the standalone command-line program
// (lowydripstonecavefinder-cli) and the JNI library
// (libDripstoneCaveFinderLibJ).  Every Minecraft / cubiomes computation lives
// in lowydripstonecavefinder.cpp, the moved engine sources and the nested
// cubiomes submodule; the Java GUI never compiles native code of its own.
//

#ifndef LOWYDRIPSTONECAVEFINDER_LOWYDRIPSTONECAVEFINDER_H
#define LOWYDRIPSTONECAVEFINDER_LOWYDRIPSTONECAVEFINDER_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

/** cubiomes generator (defined by the nested submodule; only used by the CLI helper). */
struct Generator;

namespace lowyd {

/** Ring geometry used by the Java GUI: results are 128 blocks around the area. */
constexpr int RING_OUTER = 128;
/** Inner no-spawn sphere used by the shell model (distance 24 around the AFK point). */
constexpr int RING_INNER = 24;
/* RIVER_AREA_FULL (the display denominator) is derived below from the exact
 * shell cell counts; see fullShellArea(). */
/** One maximum per 64x64 block cell when results are prepared for display. */
constexpr int OUTPUT_REGION_SIZE = 64;

/** Native search modes, matching LowYDripstoneCaveSearchRunner in the GUI. */
constexpr int SEARCH_MODE_FAST = 0;
constexpr int SEARCH_MODE_PRECISE = 1;
constexpr int SEARCH_MODE_FULL_MAP_PIT = 2;

/** Phase-1 climate grid scales used by the GUI's Fast / Precise modes. */
constexpr int PHASE1_CONT_SCALE_FAST = 128;
constexpr int PHASE1_WEIRD_SCALE_FAST = 32;
constexpr int PHASE1_CONT_SCALE_PRECISE = 32;
constexpr int PHASE1_WEIRD_SCALE_PRECISE = 16;

/** Matches the Java runner's default refinement coefficient. */
constexpr float DEFAULT_PRESERVE_RANGE = 0.9f;

/*
 * Precise layer window (B1).
 *
 * The precise pass samples every layer in [yMin, yMax] and scores the 24..128
 * spherical shell around (x, afkY, z); the per-layer cross-sections are
 * averaged, so every score is in the 0..49659 "effective cave cells" range.  The span is capped: the precise pass keeps two 320x320xL
 * prefix planes per worker and above L=48 the 128-block shell no longer
 * covers a full layer, so the model itself stops making sense.
 */
constexpr int Y_LIMIT_MIN = -63;
constexpr int Y_LIMIT_MAX = 319;
constexpr int Y_MAX_SPAN = 48;
constexpr int Y_DEFAULT_MIN = -63;
constexpr int Y_DEFAULT_MAX = -51;
constexpr int COARSE_Y_DEFAULT = -51;
/**
 * AFK / player Y used by the shell model and by the /tp result line.  The
 * scored region is the 3D shell 24 < distance <= 128 around
 * (result x, afkY, result z).
 *
 * Default: the lowest sampled layer plus 23 blocks.  While every layer stays
 * within +-24 blocks of the AFK Y the shell cross-section area is identical on
 * every layer (the dy^2 terms cancel), so the whole plateau is equivalent;
 * +23 keeps the window inside it with one block of margin at the lower edge.
 * For the default -63..-51 window this is -40.
 */
constexpr int AFK_OFFSET_FROM_Y_MIN = 23;
constexpr int defaultAfkY(int yMin) { return yMin + AFK_OFFSET_FROM_Y_MIN; }
constexpr int AFK_Y_DEFAULT = defaultAfkY(Y_DEFAULT_MIN);
constexpr int AFK_Y_LIMIT_MIN = -64;
constexpr int AFK_Y_LIMIT_MAX = 320;
/** Marker for "derive AFK Y from yMin"; SearchParams.afkY may carry it. */
constexpr int AFK_Y_AUTO = std::numeric_limits<int>::min();
/**
 * Resolve SearchParams::afkY: AFK_Y_AUTO means lowest layer + 23, clamped into
 * the legal AFK range so a very high layer window cannot produce an AFK Y that
 * validate() then rejects for the wrong reason.
 */
constexpr int resolveAfkY(int yMin, int afkY)
{
    if (afkY != AFK_Y_AUTO)
        return afkY;
    const int derived = defaultAfkY(yMin);
    if (derived < AFK_Y_LIMIT_MIN)
        return AFK_Y_LIMIT_MIN;
    if (derived > AFK_Y_LIMIT_MAX)
        return AFK_Y_LIMIT_MAX;
    return derived;
}

/** Layer range of the precise pass.  L = yMax - yMin + 1, capped at 1..Y_MAX_SPAN. */
struct PreciseYRange {
    int yMin = Y_DEFAULT_MIN;
    int yMax = Y_DEFAULT_MAX;

    /** Layer count, clamped to a legal 1..Y_MAX_SPAN (validate() rejects the rest). */
    int count() const
    {
        const int n = yMax - yMin + 1;
        if (n < 1) return 1;
        return n > Y_MAX_SPAN ? Y_MAX_SPAN : n;
    }
};

/**
 * Exact per-row radii of the 24..128 shell cross-section.
 *
 * For a layer dy and a dz row, block centres satisfy
 * 24^2 < dx^2 + dy^2 + dz^2 <= 128^2, so
 *   outer = floor(sqrt(128^2 - dy^2 - dz^2))
 *   inner = floor(sqrt(24^2  - dy^2 - dz^2))  (>= 0 only inside the no-spawn sphere)
 *
 * @return false when this row is outside the shell.
 */
inline bool shellRowRadii(int dy, int dz, int &outer, int &inner)
{
    const long long dy2 = (long long) dy * (long long) dy;
    const long long dz2 = (long long) dz * (long long) dz;
    const long long out2 = (long long) RING_OUTER * RING_OUTER - dy2 - dz2;
    if (out2 < 0)
    {
        outer = -1;
        inner = -1;
        return false;
    }
    outer = (int) std::floor(std::sqrt((double) out2));
    const long long in2 = (long long) RING_INNER * RING_INNER - dy2 - dz2;
    inner = in2 >= 0 ? (int) std::floor(std::sqrt((double) in2)) : -1;
    return true;
}

/**
 * Layer-level horizontal radii of the 24..128 shell cross-section (the dz=0 row).
 *
 * dy = layerY - afkY; the shell contains block centers with
 * 24^2 < dx^2 + dy^2 + dz^2 <= 128^2, i.e. the per-layer annulus
 * inner < sqrt(dx^2+dz^2) <= outer (inner < 0 means no hole).
 *
 * @return false when the layer is outside the shell (|dy| > 128).
 */
inline bool shellRingRadii(int layerY, int afkY, int &outer, int &inner)
{
    return shellRowRadii(layerY - afkY, 0, outer, inner);
}

/**
 * river-native's platform (plateau) solution: when every sampled layer is
 * within ±24 of the AFK Y, the 24..128 shell cross-section area is the same
 * on every layer (the dy^2 term cancels), so the score does not depend on the
 * exact AFK Y on that plateau.  This reproduces core/platform/player_y.hpp:
 *   span <= 25 : yMin
 *   span <= 49 : yMin + 24
 *   span >  49 : (yMin + yMax) / 2
 */
inline int shellPlatformPlayerY(int yMin, int yMax)
{
    const int span = yMax - yMin + 1;
    if (span <= 25)
        return yMin;
    if (span <= 49)
        return yMin + 24;
    return (yMin + yMax) / 2;
}

/** True when all sampled layers are within ±24 of playerY (the plateau). */
inline bool onShellPlatform(int yMin, int yMax, int playerY)
{
    return playerY - yMin <= RING_INNER && yMax - playerY <= RING_INNER;
}

/** Integer floor square root; constexpr so the denominator is a compile-time value. */
constexpr int isqrtFloor(long long value)
{
    if (value <= 0)
        return 0;
    long long lo = 0;
    long long hi = 1;
    while (hi * hi <= value)
        hi *= 2;
    while (lo + 1 < hi)
    {
        const long long mid = (lo + hi) / 2;
        if (mid * mid <= value)
            lo = mid;
        else
            hi = mid;
    }
    return (int) lo;
}

/**
 * Block centres inside the 24..128 shell at vertical offset dy.
 *
 * Exactly the count the runtime masks sum: for each dz row
 *   outer = floor(sqrt(128^2 - dy^2 - dz^2))
 *   inner = floor(sqrt(24^2  - dy^2 - dz^2))  (>= 0 only inside the no-spawn sphere)
 * and a row contributes 2*outer+1, minus 2*inner+1 when the inner sphere cuts it.
 * Returns 0 when |dy| > 128.
 */
constexpr long long shellLayerCellCount(int dy)
{
    const long long dy2 = (long long) dy * (long long) dy;
    const long long outer2 = (long long) RING_OUTER * RING_OUTER;
    if (dy2 > outer2)
        return 0;
    long long total = 0;
    for (int dz = -RING_OUTER; dz <= RING_OUTER; ++dz)
    {
        const long long dz2 = (long long) dz * (long long) dz;
        const long long out2 = outer2 - dy2 - dz2;
        if (out2 < 0)
            continue;
        total += 2 * (long long) isqrtFloor(out2) + 1;
        const long long in2 = (long long) RING_INNER * RING_INNER - dy2 - dz2;
        if (in2 >= 0)
            total -= 2 * (long long) isqrtFloor(in2) + 1;
    }
    return total;
}

/**
 * Exact display denominator of the current model: the shell cell counts summed
 * over the sampled layers, divided by the layer count and rounded.
 *
 * 1.0 therefore means "every layer is a full 24..128 shell cross-section", and
 * `total / fullShellArea(yMin, yMax, afkY)` is the reported percentage.  The
 * value is derived from the geometry, not fitted: the default -63..-51 window
 * with afkY = yMin + 23 gives exactly 49659 (proved by the static_assert below).
 */
constexpr int fullShellArea(int yMin, int yMax, int afkY)
{
    int layers = yMax - yMin + 1;
    if (layers < 1)
        layers = 1;
    if (layers > Y_MAX_SPAN)
        layers = Y_MAX_SPAN;
    long long sum = 0;
    for (int layer = 0; layer < layers; ++layer)
        sum += shellLayerCellCount(yMin + layer - afkY);
    return (int) ((sum + layers / 2) / layers);
}

/** Display denominator of the default model; equals the historical 49659. */
constexpr int RIVER_AREA_FULL_CELLS =
    fullShellArea(Y_DEFAULT_MIN, Y_DEFAULT_MAX, defaultAfkY(Y_DEFAULT_MIN));
static_assert(RIVER_AREA_FULL_CELLS == 49659,
              "the default 24..128 shell denominator is expected to be 49659");
constexpr double RIVER_AREA_FULL = RIVER_AREA_FULL_CELLS;

/**
 * Export / result tag including the aggregation model, layer count and AFK Y.
 * Results of different versions are not comparable, so this must travel with
 * every result set.
 *
 * Header-inline: the All-In-One host includes the engine sources at global
 * scope and does not compile lowydripstonecavefinder.cpp, so shared helpers
 * must not live in that translation unit.
 */
inline std::string modelId(int yMin, int yMax, int afkY)
{
    const int layers = yMax - yMin + 1;
    return "drowned-weighted-shell-b/" + std::to_string(layers > 0 ? layers : 0)
        + "/afk" + std::to_string(afkY);
}

/**
 * Volume-integral aggregation of one ring (B1).
 *
 * sumCave / sumRiver are the summed per-layer hit counts of the annulus,
 * range 0..49659*L.  Returns the weighted total as the per-layer average
 * (0..49659), so minArea, the refinement coefficient and sorting keep their
 * existing meaning.  caveAvg / riverAvg receive the unweighted per-layer
 * averages when non-null.
 */
inline int aggregateLayerAverages(int sumCave, int sumRiver, float riverWeight, int layers,
                                  int *caveAvg = nullptr, int *riverAvg = nullptr)
{
    const int L = layers < 1 ? 1 : (layers > Y_MAX_SPAN ? Y_MAX_SPAN : layers);
    const int cave = (int) std::lround((double) sumCave / (double) L);
    const int riverAvgValue = (int) std::lround((double) sumRiver / (double) L);
    if (caveAvg != nullptr)
        *caveAvg = cave;
    if (riverAvg != nullptr)
        *riverAvg = riverAvgValue;
    /* int64 intermediate would also fit, but int32 is already safe at L<=48. */
    return (int) std::lround(((double) sumCave + (double) riverWeight * (double) sumRiver)
                             / (double) L);
}

/**
 * Live counters published by a running search.
 * Kept binary-compatible with the old JNI bridge layout:
 * [current, total, phase, status, tryPause, tryStop].
 */
struct Progress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int chunkInRunning{0};
    std::atomic_int phase1{0};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

/** Snapshot returned by getProgress(). */
struct ProgressView {
    int current = 0;
    int total = 0;
    int phase = 0;
    int status = 0;
    int tryPause = 0;
    int tryStop = 0;
    /** False when no search has published progress yet. */
    bool available = false;
};

/** One cave/river hit. total = cave + river * riverWeight. */
struct Hit {
    int x = 0;
    int z = 0;
    int total = 0;
    int cave = 0;
    int river = 0;
};

/**
 * Search parameters.
 *
 * startX/startZ/width/height are the already-expanded native window used by
 * the Java runner: start = userMin - RING_OUTER, size = userSize + 2*RING_OUTER.
 * The CLI applies the same expansion before calling search().
 */
struct SearchParams {
    uint64_t seed = 0;
    int startX = 0;
    int startZ = 0;
    int width = 0;
    int height = 0;
    int minArea = 0;
    float preserveRange = DEFAULT_PRESERVE_RANGE;
    float riverWeight = 0.7f;
    /**
     * Precise layer window (B1).  yMax - yMin + 1 must be <= Y_MAX_SPAN; see
     * validate().  The score is the per-layer average of the cave/river volume,
     * so it stays on the 0..fullShellArea(yMin, yMax, afkY) scale regardless of
     * the layer count (the default window gives 49659).
     */
    int yMin = Y_DEFAULT_MIN;
    int yMax = Y_DEFAULT_MAX;
    /**
     * AFK / player Y of the shell model.  The scored region is the 3D shell
     * 24 < distance <= 128 around (x, afkY, z).
     *
     * Defaults to {@link AFK_Y_AUTO}, which the engine resolves to
     * {@link defaultAfkY}(yMin) = lowest layer + 23.  Pass an explicit value to
     * pin it (the CLI's --afk-y and the Java GUI do).
     */
    int afkY = AFK_Y_AUTO;
    /**
     * Recall knob of the coarse stage.  The climate prefilter used by this
     * engine (continentalness + weirdness) is Y-independent, so the value is
     * carried for protocol/CLI compatibility and does not change the scan.
     */
    int coarseY = COARSE_Y_DEFAULT;
    int numThreads = 0;
    int mc = 35; // cubiomes MC_26_3
    int contScale = 0;  // <= 0 uses the SearchConfig default (128)
    int weirdScale = 0; // <= 0 uses the SearchConfig default (32)
    int searchMode = SEARCH_MODE_FAST;
    /**
     * Non-zero: the OpenCL backend may be used when its gate is satisfied (the
     * window is large enough and a usable device exists). Zero: never touch
     * OpenCL, always run the CPU path. Either way an unusable device is silent
     * and the CPU path produces the same results.
     */
    int allowGpu = 1;
};

/**
 * Validates the layer window and the AFK Y: yMin <= yMax, both inside
 * [-63, 319], yMax - yMin + 1 <= Y_MAX_SPAN, and afkY inside
 * [-64, 320].  Returns false and fills *error (when non-null) on failure;
 * the GUI / CLI reject the search before calling search().
 *
 * Header-inline for the All-In-One host (see modelId()).
 */
inline bool validate(const SearchParams &params, std::string *error)
{
    const int yMin = params.yMin;
    const int yMax = params.yMax;
    const int layers = yMax - yMin + 1;

    auto fail = [error](std::string message) {
        if (error != nullptr)
            *error = std::move(message);
        return false;
    };

    /* AFK Y: AFK_Y_AUTO resolves to the lowest layer + 23 before the range check. */
    const int afkY = resolveAfkY(yMin, params.afkY);
    if (afkY < AFK_Y_LIMIT_MIN || afkY > AFK_Y_LIMIT_MAX)
        return fail("AFK Y must stay within [" + std::to_string(AFK_Y_LIMIT_MIN) + ", "
                    + std::to_string(AFK_Y_LIMIT_MAX) + "] (got "
                    + std::to_string(afkY) + ")");
    if (yMin > yMax)
        return fail("yMin (" + std::to_string(yMin) + ") must not be greater than yMax ("
                    + std::to_string(yMax) + ")");
    if (yMin < Y_LIMIT_MIN || yMax > Y_LIMIT_MAX)
        return fail("layer range must stay within [" + std::to_string(Y_LIMIT_MIN) + ", "
                    + std::to_string(Y_LIMIT_MAX) + "] (got [" + std::to_string(yMin) + ", "
                    + std::to_string(yMax) + "])");
    if (layers > Y_MAX_SPAN)
        return fail("layer span must not exceed " + std::to_string(Y_MAX_SPAN)
                    + " layers (got " + std::to_string(layers)
                    + "); the window cache and the 128-block shell model both stop holding");
    if (error != nullptr)
        error->clear();
    return true;
}

/** Clears pause/stop flags, progress counters and the "now" result list. */
void resetSearchState();

/** Requests pause / resume / stop. All are safe to call while idle. */
void requestPause();
void requestResume();
void requestStop();

/** Progress snapshot; available == false when no search has started. */
ProgressView getProgress();

/** True when a stop was requested by any caller. */
bool stopRequested();

/**
 * Runs a synchronous search. Returns an empty vector on stop, allocation
 * failure, or when there are no hits. The function serializes concurrent
 * callers exactly like the historical JNI bridge.
 */
std::vector<Hit> search(const SearchParams &params);

/** Point-score coverage models, used for diagnostics and before/after comparisons. */
enum PointCoverage
{
    /** Current model: the 24..128 shell, with no chunk-level culling. */
    POINT_COVERAGE_SHELL = 0,
    /** Pre-shell model: the full horizontal 24..128 annulus on every layer. */
    POINT_COVERAGE_CYLINDER = 1
};

struct PointScore
{
    int total = 0;
    int cave = 0;
    int river = 0;
};

struct PointScoreAtY
{
    int afkY = 0;
    PointScore score;
};

/** Cave-cell centroid (block coordinates) of one diagnostic point window. */
struct PointCentroid
{
    double x = 0.0;
    double z = 0.0;
    int cells = 0;
};

struct PointCentroids
{
    /** All cave cells in the 128-block horizontal disk (the visual blob centre). */
    PointCentroid visual;
    /** Only the cells counted by the current shell score. */
    PointCentroid scored;
};

/**
 * Computes where the cave cells actually sit relative to the AFK point.  This
 * explains why the score optimum can differ from the visual centre of the
 * cave: the score excludes the 24-block inner sphere, so it is not a centroid
 * objective.
 */
PointCentroids pointCaveCentroids(Generator *g, int x, int z,
                                  const SearchParams &params);

/**
 * Scores one exact (x, z) point with the same 1:1 layer sampling as the
 * precise pass.  The window is [x-128, x+128] x [z-128, z+128], so the whole
 * 128 shell is visible.  g must already be seeded.  Used by the CLI's
 * --score-point diagnostic; it is not part of the GUI search path.
 */
PointScore scorePoint(Generator *g, int x, int z, const SearchParams &params,
                      int coverage = POINT_COVERAGE_SHELL);

/**
 * Scores one point for every AFK Y in [afkYFrom, afkYTo] while sampling the
 * biome window only once.  This is how the platform plateau is verified on a
 * real seed without repeating the expensive genBiomes call.
 */
std::vector<PointScoreAtY> sweepPointAfkY(Generator *g, int x, int z,
                                          const SearchParams &params,
                                          int coverage,
                                          int afkYFrom, int afkYTo);

/** Snapshot of results collected so far (empty once a search finished). */
std::vector<Hit> getNowResults();

/**
 * Weirdness y-offset score for one seed.
 *
 * score = |yo0A-0.5| + |yo1A-0.5| + |yo0B-0.5| + |yo1B-0.5|,
 * yo = top 24 bits of the octave fork * 2^-24.
 *
 * The Java module only compares the result with its thresholds and decides
 * whether to offer / perform the fast-search page jump; the computation
 * itself belongs to this library.
 */
float weirdnessYOffset(uint64_t seed);

} // namespace lowyd

#endif // LOWYDRIPSTONECAVEFINDER_LOWYDRIPSTONECAVEFINDER_H
