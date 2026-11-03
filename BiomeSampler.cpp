#include "BiomeSampler.h"
#include "Bench.h"
#include "PerlinTable.h"
#include "OctaveFieldCache.h"
#include "cubiomes/biomes.h"
#include "cubiomes/noise.h"
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

static constexpr double kDoublePerlinF = 337.0 / 331.0;

namespace {

std::mutex g_scratchMutex;
std::unordered_map<std::thread::id, std::unique_ptr<PreciseScratch>> g_scratches;

} // namespace

PreciseScratch &preciseScratch()
{
    const std::thread::id id = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(g_scratchMutex);
    std::unique_ptr<PreciseScratch> &slot = g_scratches[id];
    if (!slot)
        slot = std::make_unique<PreciseScratch>();
    /* unordered_map nodes are stable, so the reference stays valid while other
     * workers insert their own entries. */
    return *slot;
}

void clearPreciseScratchPool()
{
    std::lock_guard<std::mutex> lock(g_scratchMutex);
    g_scratches.clear();
}

/*
 * G0: the perlin is evaluated by lowyd::samplePerlinTable(), which is bit-identical to
 * samplePerlin() but takes its gradients from a 16x3 table instead of the
 * 16-way switch (no unpredictable branch on the hashed index).
 */
static double sampleOctALive(const DoublePerlinNoise *dpn, int idx, double x, double y, double z)
{
    if (idx < 0 || idx >= dpn->octA.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octA.octaves + idx;
    const double lf = p->lacunarity;
    return p->amplitude * lowyd::samplePerlinTable(p, x * lf, y * lf, z * lf);
}

/**
 * Cont octA[i] with the A-table fast path.
 *
 * C2: `cache`/`probe` are resolved once per sample by the caller, so the
 * lattice test and the cache read run once instead of once per octave, and the
 * index itself is a shift+mask instead of two integer divisions per axis.
 */
static inline double contOctA(
    const OctaveACache *cache, const ClimateProbe &probe,
    const DoublePerlinNoise *dpn, int idx, double x, double y, double z)
{
    if (probe.onLattice && y == 0.0 && idx >= 0 && idx < OctaveACache::CONT_OCT)
        return (double) cache->contA[idx].atIndex(probe.ix, probe.iz);
    return sampleOctALive(dpn, idx, x, y, z);
}

/** Cache pointer usable for this thread, or nullptr. */
static inline const OctaveACache *activeCache()
{
    const OctaveACache *cache = getActiveOctaveACache();
    return (cache && cache->ready) ? cache : nullptr;
}

static double sampleContOctB(const DoublePerlinNoise *dpn, int idx, double x, double y, double z)
{
    if (idx < 0 || idx >= dpn->octB.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octB.octaves + idx;
    const double lf = p->lacunarity;
    return p->amplitude * lowyd::samplePerlinTable(p, x * lf * kDoublePerlinF,
                                            y * lf * kDoublePerlinF,
                                            z * lf * kDoublePerlinF);
}

static double sampleWeirdnessCached(const BiomeNoise *bn, double px, double pz)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_WEIRDNESS];
    const OctaveACache *cache = activeCache();
    double v = 0.0;
    const int octCnt = dpn->octA.octcnt < dpn->octB.octcnt ? dpn->octA.octcnt : dpn->octB.octcnt;
    const int bx = (int) px;
    const int bz = (int) pz;

    /* C2: one lattice probe per sample, shared by all three ridge octaves. */
    const bool exact = px == (double) bx && pz == (double) bz;
    const ClimateProbe probe = (cache && exact) ? probeClimate(cache->ridgeA[0], bx, bz)
                                                : ClimateProbe{};

    for (int i = 0; i < octCnt; i++)
    {
        if (probe.onLattice && i < OctaveACache::RIDGE_OCT)
            v += (double) cache->ridgeA[i].atIndex(probe.ix, probe.iz);
        else
            v += sampleOctALive(dpn, i, px, 0.0, pz);
        v += sampleContOctB(dpn, i, px, 0.0, pz);
    }
    return v * dpn->amplitude;
}

/**
 * N7: the weirdness gate keeps |w| < 0.05 or |w| > 1.28. After the first two
 * octave pairs the remaining pair (A2+B2) can move w by at most
 * 1.25 * 2 * (a2A + a2B) * max|perlin|, so a partial sum strictly inside
 * (0.05 + m, 1.28 - m) can never come back into either tail. The margin m is
 * empirical: measured on 4.19 M cells, m = 0.14 gives 58% exits at a
 * 0.11/1000 miss rate (budget is 1/1000).
 */
static constexpr double kWeirdEarlyLo = 0.05 + 0.14;
static constexpr double kWeirdEarlyHi = 1.28 - 0.14;

/** Escape hatch for A/B measurements: LOWYD_NO_WEIRD_EARLY=1 disables N7. */
static inline bool weirdEarlyExitEnabled()
{
    static const bool enabled = [] {
        const char *s = std::getenv("LOWYD_NO_WEIRD_EARLY");
        return !(s && *s && *s != '0');
    }();
    return enabled;
}

/**
 * Weirdness gate for whole-number climate coordinates with the A tables active.
 * Returns the same boolean as passCaveWeirdness(bn, bx, bz, COARSE_SAMPLE_FLAGS),
 * except for the ~1e-4 fraction of samples the early exit gives up on.
 */
bool passCaveWeirdnessFast(const BiomeNoise *bn, int bx, int bz)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_WEIRDNESS];
    const OctaveACache *cache = activeCache();
    const int octCnt = dpn->octA.octcnt < dpn->octB.octcnt ? dpn->octA.octcnt : dpn->octB.octcnt;
    const ClimateProbe probe = cache ? probeClimate(cache->ridgeA[0], bx, bz) : ClimateProbe{};

    double v = 0.0;
    for (int i = 0; i < octCnt; i++)
    {
        if (probe.onLattice && i < OctaveACache::RIDGE_OCT)
            v += (double) cache->ridgeA[i].atIndex(probe.ix, probe.iz);
        else
            v += sampleOctALive(dpn, i, bx, 0.0, bz);
        v += sampleContOctB(dpn, i, bx, 0.0, bz);

        if (i == 1 && weirdEarlyExitEnabled())
        {
            const double p = std::fabs(v * dpn->amplitude);
            /* Cannot fall back under the low threshold nor climb over the high one. */
            if (p > kWeirdEarlyLo && p < kWeirdEarlyHi)
                return false;
        }
    }

    int wi = (int) (10000.0 * (v * dpn->amplitude));
    if (wi < 0)
        wi = -wi;
    return wi < 500 || wi > 12800;
}


bool passContinentalnessPartialThr(const BiomeNoise *bn, int bx, int bz, double threshold)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_CONTINENTALNESS];
    // sampleDoublePerlin scales octA+octB by this; keep running sum in climate units
    const double amp = dpn->amplitude;
    const double x = bx;
    const double y = 0.0;
    const double z = bz;
    double sum = 0.0;

    /* C2: all five cont octaves share one scale, so probe the lattice once. */
    const OctaveACache *cache = activeCache();
    const ClimateProbe probe = cache ? probeClimate(cache->contA[0], bx, bz) : ClimateProbe{};

    sum += amp * (contOctA(cache, probe, dpn, 0, x, y, z) + sampleContOctB(dpn, 0, x, y, z));
    if (sum < -0.2)
        return false;

    sum += amp * contOctA(cache, probe, dpn, 1, x, y, z);
    if (sum < -0.1)
        return false;

    sum += amp * sampleContOctB(dpn, 1, x, y, z);
    if (sum < 0.0)
        return false;

    sum += amp * contOctA(cache, probe, dpn, 2, x, y, z);
    if (sum < 0.13)
        return false;

    sum += amp * sampleContOctB(dpn, 2, x, y, z);
    if (sum < 0.3)
        return false;

    sum += amp * contOctA(cache, probe, dpn, 3, x, y, z);
    if (sum < 0.37)
        return false;

    sum += amp * sampleContOctB(dpn, 3, x, y, z);
    if (sum < 0.44)
        return false;

    sum += amp * (contOctA(cache, probe, dpn, 4, x, y, z) + sampleContOctB(dpn, 4, x, y, z));
    return sum > threshold;
}

bool passContinentalnessPartial(const BiomeNoise *bn, int bx, int bz)
{
    return passContinentalnessPartialThr(bn, bx, bz, CONT_PARTIAL_FINAL_THRESHOLD);
}

static int quantWeirdness(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    double px = bx, pz = bz;
    if (!(sample_flags & SAMPLE_NO_SHIFT))
    {
        px += sampleDoublePerlin(&bn->climate[NP_SHIFT], bx, 0, bz) * 4.0;
        pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], bz, bx, 0) * 4.0;
    }
    float w;
    if (activeCache() != nullptr)
        w = (float) sampleWeirdnessCached(bn, px, pz);
    else
        w = sampleDoublePerlin(&bn->climate[NP_WEIRDNESS], px, 0, pz);
    int wi = (int) (10000.0 * w);
    return wi < 0 ? -wi : wi;
}

bool passCaveWeirdness(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    /* N7: the whole-number, no-shift case can use the early-exiting variant
     * (identical gate, ~1e-4 of samples given up on by design). */
    if ((sample_flags & SAMPLE_NO_SHIFT) && getActiveOctaveACache())
        return passCaveWeirdnessFast(bn, bx, bz);

    int wi = quantWeirdness(bn, bx, bz, sample_flags);
    return wi < 500 || wi > 12800;
}

bool passCoarseCaveCell(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    if (!passContinentalnessPartial(bn, bx, bz))
        return false;
    if (!passCaveWeirdness(bn, bx, bz, sample_flags))
        return false;

    double px = bx, pz = bz;
    if (!(sample_flags & SAMPLE_NO_SHIFT))
    {
        px += sampleDoublePerlin(&bn->climate[NP_SHIFT], bx, 0, bz) * 4.0;
        pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], bz, bx, 0) * 4.0;
    }

    int ei = (int) (10000.0 * sampleDoublePerlin(&bn->climate[NP_EROSION], px, 0, pz));
    return ei > -3750;
}

bool passCaveClimate(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    return passCoarseCaveCell(bn, bx, bz, sample_flags);
}

void samplePreciseCell(const Generator *g, int worldX, int worldZ,
                       const lowyd::PreciseYRange &yRange, int *riverHits, int *caveHits)
{
    *riverHits = 0;
    *caveHits = 0;

    const int bx = worldX / 4;
    const int bz = worldZ / 4;
    if (!passCaveClimate(&g->bn, bx, bz, COARSE_SAMPLE_FLAGS))
        return;

    // One column over the requested layer window, reusing this thread's cache.
    const int y0 = yRange.yMin;
    const int layers = yRange.count();
    Range r = {1, worldX, worldZ, 1, 1, y0, layers};

    PreciseScratch &scratch = preciseScratch();
    const size_t need = getMinCacheSize(g, r.scale, r.sx, r.sy, r.sz);
    if (need == 0)
        return;
    if (scratch.cache.size() < need)
        scratch.cache.resize(need);
    if (genBiomes(g, scratch.cache.data(), r) != 0)
        return;

    for (int i = 0; i < layers; i++)
    {
        const int id = scratch.cache[(size_t) i];
        *riverHits += (id == river);
        *caveHits += (id == dripstone_caves);
    }
}

void fillPreciseWindow(Generator *g, int startX, int startZ, int W, int H,
                       const lowyd::PreciseYRange &yRange,
                       std::vector<int> &rawRiver, std::vector<int> &rawCave)
{
    const size_t n = (size_t) W * (size_t) H;
    rawRiver.assign(n, 0);
    rawCave.assign(n, 0);
    if (W <= 0 || H <= 0)
        return;

    /* B2: every buffer below is this thread's, sized once and reused for the
     * next candidate instead of being allocated 7..8 times per candidate. */
    PreciseScratch &scratch = preciseScratch();
    std::vector<uint8_t> &climatePass = scratch.climatePass;
    climatePass.assign(n, 0);
    std::unordered_map<uint64_t, uint8_t> &climCache = scratch.climCache;
    climCache.clear();
    const size_t wantedBuckets = (size_t) (W / 4 + 2) * (size_t) (H / 4 + 2);
    if (climCache.bucket_count() < wantedBuckets)
        climCache.reserve(wantedBuckets);

    bool anyPass = false;
    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x;
            const int bx = worldX / 4;
            const int bz = worldZ / 4;
            const uint64_t key = ((uint64_t) (uint32_t) bx << 32) | (uint32_t) bz;
            auto it = climCache.find(key);
            if (it == climCache.end())
            {
                const uint8_t ok = passCaveClimate(&g->bn, bx, bz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
                it = climCache.emplace(key, ok).first;
            }
            climatePass[(size_t) x + (size_t) z * W] = it->second;
            anyPass = anyPass || it->second;
        }
    }
    if (!anyPass)
        return;

    const int layers = yRange.count();
    Range r = {1, startX, startZ, W, H, yRange.yMin, layers};

    const size_t need = getMinCacheSize(g, r.scale, r.sx, r.sy, r.sz);
    if (need == 0)
        return;
    if (scratch.cache.size() < need)
        scratch.cache.resize(need);
    int *cache = scratch.cache.data();
    if (genBiomes(g, cache, r) != 0)
        return;

    const size_t plane = n;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            const size_t idx = (size_t) x + (size_t) z * W;
            if (!climatePass[idx])
                continue;
            int riverHits = 0;
            int caveHits = 0;
            for (int i = 0; i < layers; i++)
            {
                const int id = cache[(size_t) i * plane + idx];
                riverHits += (id == river);
                caveHits += (id == dripstone_caves);
            }
            rawRiver[idx] = riverHits;
            rawCave[idx] = caveHits;
        }
    }
}
