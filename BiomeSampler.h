#ifndef BIOME_SAMPLER_H
#define BIOME_SAMPLER_H

#include "lowydripstonecavefinder.h"
#include "cubiomes/biomenoise.h"
#include "cubiomes/generator.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

static constexpr uint32_t COARSE_SAMPLE_FLAGS = SAMPLE_NO_SHIFT | SAMPLE_NO_DEPTH;
static constexpr int COARSE_Y_PARAM = 256 / 4 + 1;
/** Match legacy full-C gate: (int)(10000*C) > 5500 ⇒ C > 0.55 */
static constexpr double CONT_PARTIAL_FINAL_THRESHOLD = 0.55;

/**
 * Reusable buffers of the precise pass (B2).
 *
 * findBiggestRiver<1>() and fillPreciseWindow() used to allocate ~7 vectors and
 * one biome cache per candidate.  The vectors keep their capacity across calls
 * (thousands of candidates reuse the same memory) and only the biome cache is
 * sized through getMinCacheSize().  genBiomeNoiseScaled() writes every cell of
 * the requested range, including its internal Voronoi source block, so a reused
 * buffer never needs zeroing.
 *
 * One instance exists per worker thread of the current search; the engine owns
 * it (see preciseScratch()), so no buffer is freed from a worker's exit path.
 */
struct PreciseScratch {
    /** Biome id cache of the current precise window (getMinCacheSize elements). */
    std::vector<int> cache;
    std::vector<int> rawRiver;
    std::vector<int> rawCave;
    std::vector<int> prefixRiver;
    std::vector<int> prefixCave;
    std::vector<uint8_t> climatePass;
    std::vector<uint8_t> occ;
    std::vector<uint8_t> centerCand;
    std::vector<int32_t> diff;
    /** (bx,bz) -> climate gate result; cleared, never reallocated. */
    std::unordered_map<uint64_t, uint8_t> climCache;
};

/** This thread's scratch buffers (created on first use). */
PreciseScratch &preciseScratch();

/**
 * Drops every scratch buffer.  Searches are serialized by the engine mutex, so
 * the caller may only do this when no worker thread is running (search start,
 * between phases, and before search() returns).  Keeping the buffers out of
 * thread_local storage avoids freeing them from a pooled worker's exit path,
 * which has corrupted the heap with MinGW/UCRT.
 */
void clearPreciseScratchPool();

/**
 * RAII helper for a search: clears the scratch pool at entry and exit.  Both
 * call sites (the engine and the All-In-One host) run it only while no worker
 * thread is alive.
 */
struct PreciseScratchSession {
    PreciseScratchSession() { clearPreciseScratchPool(); }
    ~PreciseScratchSession() { clearPreciseScratchPool(); }

    PreciseScratchSession(const PreciseScratchSession &) = delete;
    PreciseScratchSession &operator=(const PreciseScratchSession &) = delete;
};

bool passCaveWeirdness(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags);

/**
 * N7: same gate as passCaveWeirdness(bn, bx, bz, COARSE_SAMPLE_FLAGS) for
 * whole-number climate coordinates, with the early exit after the second
 * octave pair. Gives up on ~1e-4 of samples by design.
 */
bool passCaveWeirdnessFast(const BiomeNoise *bn, int bx, int bz);

bool passCaveClimate(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags);
bool passContinentalnessPartial(const BiomeNoise *bn, int bx, int bz);
/** Octave early-exit Cont; final compare uses threshold (climate units). */
bool passContinentalnessPartialThr(const BiomeNoise *bn, int bx, int bz, double threshold);
bool passCoarseCaveCell(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags);

/** One column: river/cave hit counts over the given layer window (0..L). */
void samplePreciseCell(const Generator *g, int worldX, int worldZ,
                       const lowyd::PreciseYRange &yRange, int *riverHits, int *caveHits);

/**
 * Fill W×H precise river/cave hit-count grids for a refine window.
 * Uses (bx,bz) climate-gate cache and one scale-1 volume genBiomes over
 * yRange.yMin..yRange.yMax.  rawRiver/rawCave hold 0..L hits per column; the
 * caller divides by L (volume integral, see lowyd::aggregateLayerAverages).
 */
void fillPreciseWindow(Generator *g, int startX, int startZ, int W, int H,
                       const lowyd::PreciseYRange &yRange,
                       std::vector<int> &rawRiver, std::vector<int> &rawCave);

#endif
