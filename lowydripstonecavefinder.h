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
#include <cstdint>
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
    int numThreads = 0;
    int mc = 35; // cubiomes MC_26_3
    int contScale = 0;  // <= 0 uses the SearchConfig default (128)
    int weirdScale = 0; // <= 0 uses the SearchConfig default (32)
    int searchMode = SEARCH_MODE_FAST;
};

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
