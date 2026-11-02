#ifndef BIOME_SAMPLER_H
#define BIOME_SAMPLER_H

#include "cubiomes/biomenoise.h"
#include "cubiomes/generator.h"
#include <vector>

static constexpr int PRECISE_Y_COUNT = 3;
extern const int PRECISE_YS[PRECISE_Y_COUNT];

static constexpr uint32_t COARSE_SAMPLE_FLAGS = SAMPLE_NO_SHIFT | SAMPLE_NO_DEPTH;
static constexpr int COARSE_Y_PARAM = 256 / 4 + 1;
/** Match legacy full-C gate: (int)(10000*C) > 5500 ⇒ C > 0.55 */
static constexpr double CONT_PARTIAL_FINAL_THRESHOLD = 0.55;

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
void samplePreciseCell(const Generator *g, int worldX, int worldZ, int *riverHits, int *caveHits);

/**
 * Fill W×H precise river/cave hit grids for a refine window.
 * Uses (bx,bz) climate-gate cache and one scale-1 volume genBiomes over PRECISE_YS.
 */
void fillPreciseWindow(Generator *g, int startX, int startZ, int W, int H,
                       std::vector<int> &rawRiver, std::vector<int> &rawCave);

#endif
