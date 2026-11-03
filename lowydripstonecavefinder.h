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
#include <string>
#include <vector>

namespace lowyd {

/** Ring geometry used by the Java GUI: results are 128 blocks around the area. */
constexpr int RING_OUTER = 128;
/** Cap for the weighted total (block-cell centers in the 24..128 annulus). */
constexpr double RIVER_AREA_FULL = 49640.0;
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
 * The precise pass samples every layer in [yMin, yMax] and integrates the
 * counts as a volume: the per-layer average is reported, so every score keeps
 * the historical 0..49640 "effective cave cells" range.  The span is capped:
 * fillPreciseWindow() allocates 320x320xLx4 B per worker (19.7 MB at L=48) and
 * above L=48 the 128-block shell no longer covers a full layer, so the model
 * itself stops making sense.
 */
constexpr int Y_LIMIT_MIN = -63;
constexpr int Y_LIMIT_MAX = 319;
constexpr int Y_MAX_SPAN = 48;
constexpr int Y_DEFAULT_MIN = -63;
constexpr int Y_DEFAULT_MAX = -51;
constexpr int COARSE_Y_DEFAULT = -51;

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
 * Export / result tag including the aggregation model and the layer count,
 * e.g. "drowned-weighted-volume-b/13".  Results of different versions are not
 * comparable, so this must travel with every result set.
 *
 * Header-inline: the All-In-One host includes the engine sources at global
 * scope and does not compile lowydripstonecavefinder.cpp, so shared helpers
 * must not live in that translation unit.
 */
inline std::string modelId(int yMin, int yMax)
{
    const int layers = yMax - yMin + 1;
    return "drowned-weighted-volume-b/" + std::to_string(layers > 0 ? layers : 0);
}

/**
 * Volume-integral aggregation of one ring (B1).
 *
 * sumCave / sumRiver are the summed per-layer hit counts of the annulus,
 * range 0..49640*L.  Returns the weighted total as the per-layer average
 * (0..49640), so minArea, the refinement coefficient and sorting keep their
 * historical meaning.  caveAvg / riverAvg receive the unweighted per-layer
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
    float riverWeight = 0.75f;
    /**
     * Precise layer window (B1).  yMax - yMin + 1 must be <= Y_MAX_SPAN; see
     * validate().  The score is the per-layer average of the cave/river volume,
     * so it keeps the 0..49640 scale regardless of the layer count.
     */
    int yMin = Y_DEFAULT_MIN;
    int yMax = Y_DEFAULT_MAX;
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
 * Validates the layer window: yMin <= yMax, both inside [-63, 319], and
 * yMax - yMin + 1 <= Y_MAX_SPAN.  Returns false and fills *error (when non-null)
 * on failure; the GUI / CLI reject the search before calling search().
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
