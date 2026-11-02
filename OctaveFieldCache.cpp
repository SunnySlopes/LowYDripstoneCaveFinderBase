#include "OctaveFieldCache.h"

#include "Bench.h"
#include "PerlinTable.h"
#include "cubiomes/biomes.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <thread>
#include <vector>

static const OctaveACache *g_activeOctaveACache = nullptr;

void setActiveOctaveACache(const OctaveACache *cache)
{
    g_activeOctaveACache = cache;
}

const OctaveACache *getActiveOctaveACache()
{
    return g_activeOctaveACache;
}

static inline bool isPowerOfTwo(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

static inline int ilog2(int v)
{
    int shift = 0;
    while ((1 << shift) < v)
        shift++;
    return shift;
}

/**
 * C2: precompute the shift/mask form of the lattice index. Both `stepClim`
 * (sampleScale/4) and `n` (periodWorld/sampleScale) are powers of two for every
 * supported scale; anything else keeps the division based generic path.
 */
static void fillGeometry(OctaveACache::Field *f)
{
    f->stepClim = f->sampleScale / 4;
    f->half = f->stepClim / 2;
    f->fastIndex = isPowerOfTwo(f->stepClim) && isPowerOfTwo(f->n);
    f->shift = f->fastIndex ? ilog2(f->stepClim) : 0;
    f->stepMask = f->fastIndex ? f->stepClim - 1 : 0;
    f->indexMask = f->fastIndex ? f->n - 1 : 0;
}

float OctaveACache::Field::atClimate(int bx, int bz) const
{
    if (n <= 0 || data.empty() || sampleScale <= 0 || stepClim <= 0)
        return 0.0f;
    /* Map climate to lattice index of prefilter centers (…, half, half+step, …). */
    const int ax = bx - half;
    const int az = bz - half;
    if (fastIndex)
    {
        /* Arithmetic right shift is floor; the mask is the non-negative remainder. */
        const int ix = (ax >> shift) & indexMask;
        const int iz = (az >> shift) & indexMask;
        return data[(size_t) iz * (size_t) n + (size_t) ix];
    }
    const int ix = octaveModPositive(octaveFloorDiv(ax, stepClim), n);
    const int iz = octaveModPositive(octaveFloorDiv(az, stepClim), n);
    return data[(size_t) iz * (size_t) n + (size_t) ix];
}

static double sampleOctALive(const PerlinNoise *p, double x, double z)
{
    /* Same table-gradient Perlin as the live coarse path, so the cached and
     * uncached values agree bit for bit. */
    const double lf = p->lacunarity;
    return p->amplitude * lowyd::samplePerlinTable(p, x * lf, 0.0, z * lf);
}

/**
 * Fills one A field in parallel. Shared by the Cont and Ridge loops; `progress`
 * is optional and advances one row (n cells) at a time.
 */
static void fillFieldRows(
    OctaveACache::Field *f,
    const PerlinNoise *p,
    int threads,
    std::atomic_int *progressCurrent,
    const std::atomic_bool *tryPause,
    const std::atomic_bool *tryStop,
    bool *stopped)
{
    const int n = f->n;
    const int stepClim = f->stepClim;
    const int half = f->half;

    std::vector<std::thread> pool;
    pool.reserve((size_t) threads);
    std::atomic_int nextRow{0};
    std::atomic_bool stopFlag{false};
    for (int t = 0; t < threads; t++)
    {
        pool.emplace_back([&, p, n, stepClim, half]() {
            for (;;)
            {
                if (tryStop && tryStop->load())
                {
                    stopFlag.store(true);
                    return;
                }
                const int iz = nextRow.fetch_add(1);
                if (iz >= n)
                    return;
                if (tryPause)
                {
                    while (tryPause->load())
                    {
                        if (tryStop && tryStop->load())
                        {
                            stopFlag.store(true);
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                const int bz = iz * stepClim + half;
                for (int ix = 0; ix < n; ix++)
                {
                    const int bx = ix * stepClim + half;
                    f->data[(size_t) iz * (size_t) n + (size_t) ix] =
                        (float) sampleOctALive(p, (double) bx, (double) bz);
                }
                if (progressCurrent)
                    progressCurrent->fetch_add(n);
            }
        });
    }
    for (auto &th: pool)
        th.join();
    if (stopped)
        *stopped = stopFlag.load();
}

bool buildOctaveACache(
    OctaveACache *out,
    const BiomeNoise *bn,
    int contScale,
    int weirdScale,
    int numThreads,
    std::atomic_int *progressCurrent,
    std::atomic_int *progressTotal,
    const std::atomic_bool *tryPause,
    const std::atomic_bool *tryStop)
{
    if (!out || !bn || contScale <= 0 || weirdScale <= 0)
        return false;

    out->ready = false;
    const DoublePerlinNoise *cont = &bn->climate[NP_CONTINENTALNESS];
    const DoublePerlinNoise *ridge = &bn->climate[NP_WEIRDNESS];

    if (cont->octA.octcnt < OctaveACache::CONT_OCT || ridge->octA.octcnt < OctaveACache::RIDGE_OCT)
        return false;

    /* Count total cells for progress. */
    long long totalCells = 0;
    for (int i = 0; i < OctaveACache::CONT_OCT; i++)
    {
        const int n = kContAPeriodWorld[i] / contScale;
        totalCells += (long long) n * n;
    }
    for (int i = 0; i < OctaveACache::RIDGE_OCT; i++)
    {
        const int n = kRidgeAPeriodWorld[i] / weirdScale;
        totalCells += (long long) n * n;
    }
    if (progressTotal)
        progressTotal->store((int) std::min(totalCells, (long long) INT32_MAX));
    if (progressCurrent)
        progressCurrent->store(0);

    if (lowyd_bench::enabled())
    {
        lowyd_bench::add(lowyd_bench::counters().phase0Bytes, (uint64_t) totalCells * sizeof(float));
        lowyd_bench::add(lowyd_bench::counters().phase0Samples, (uint64_t) totalCells);
    }

    auto respectPauseStop = [&]() -> bool {
        if (tryStop && tryStop->load())
            return false;
        if (tryPause)
        {
            while (tryPause->load())
            {
                if (tryStop && tryStop->load())
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        return true;
    };

    int threads = numThreads > 0 ? numThreads : 1;
    if (threads < 1)
        threads = 1;

    for (int oi = 0; oi < OctaveACache::CONT_OCT; oi++)
    {
        if (!respectPauseStop())
            return false;
        OctaveACache::Field *f = &out->contA[oi];
        const PerlinNoise *p = cont->octA.octaves + oi;
        f->periodWorld = kContAPeriodWorld[oi];
        f->sampleScale = contScale;
        f->n = f->periodWorld / contScale;
        fillGeometry(f);
        f->data.assign((size_t) f->n * (size_t) f->n, 0.0f);

        bool stopped = false;
        fillFieldRows(f, p, threads, progressCurrent, tryPause, tryStop, &stopped);
        if (stopped)
            return false;
    }

    for (int oi = 0; oi < OctaveACache::RIDGE_OCT; oi++)
    {
        if (!respectPauseStop())
            return false;
        OctaveACache::Field *f = &out->ridgeA[oi];
        const PerlinNoise *p = ridge->octA.octaves + oi;
        f->periodWorld = kRidgeAPeriodWorld[oi];
        f->sampleScale = weirdScale;
        f->n = f->periodWorld / weirdScale;
        fillGeometry(f);
        f->data.assign((size_t) f->n * (size_t) f->n, 0.0f);

        bool stopped = false;
        fillFieldRows(f, p, threads, progressCurrent, tryPause, tryStop, &stopped);
        if (stopped)
            return false;
    }

    out->ready = true;
    if (progressCurrent && progressTotal)
        progressCurrent->store(progressTotal->load());
    return true;
}
