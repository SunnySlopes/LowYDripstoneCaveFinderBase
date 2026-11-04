//
// LowYDripstoneCaveFinderBase search engine.
//
// This translation unit owns the whole native search pipeline that used to
// live inside the JNI bridge: phase-0 octave tables, the phase-1 climate
// prefilter / weirdness scan, phase-2 precise refinement, the full-map
// large-pit search and the weirdness y-offset score.
//
// Both the standalone CLI and the JNI library link this file, so there is a
// single implementation of the search semantics.
//

#include "lowydripstonecavefinder.h"

#include "cubiomes/biomes.h"
#include "cubiomes/generator.h"
#include "cubiomes/util.h"
#include "PerlinTable.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <new>
#include <queue>
#include <ranges>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Engine implementation units. They are included, not linked separately, so
// the internal Res/Point/FilterMode types stay private to this translation
// unit and there is exactly one definition of every engine symbol.
//
// The shared headers are included outside the namespace so their definitions
// (ThreadSafeResults, BiomeSampler, OctaveACache, SearchConfig) stay at global
// scope, matching the separately compiled BiomeSampler.cpp / OctaveFieldCache.cpp.
#include "Thread.h"
#include "Bench.h"
#include "BiomeSampler.h"
#include "SearchConfig.h"
#include "OctaveFieldCache.h"
#include "GpuCoarse.h"

namespace lowyd {

#include "dripstonecave_finder.cpp"
#include "LargePitSearch.cpp"

namespace {

/**
 * The GPU backend is only offered for windows the plan calls "large"
 * (user side >= 2,000,001 blocks). Below that the CPU path is faster than the
 * OpenCL submission overhead, and the plan forbids even probing the device.
 *
 * Measured on a 24-core i9 + RTX 4080 (docs/GPU_ACCEL_RESULTS.md): the device
 * wins when the window is big enough to amortise the probe and the caller does
 * not already run 16+ worker threads, so "auto" also enables it for
 * >= 524,288-block windows when at most 12 threads are used (1.5x at 8
 * threads, 1.04x at 16, 0.88x at 32).
 */
constexpr long long kGpuMinUserSide = 2000001LL;
constexpr long long kGpuThreadedUserSide = 524288LL;
constexpr int kGpuMaxThreads = 12;

/** Ends a search's GPU section on every return path. */
struct GpuSession
{
    ~GpuSession() { lowyd_gpu::deactivate(); }
};

std::mutex g_searchMutex;
Progress g_progress{};
ThreadSafeResults<Res> g_results{};

/**
 * B6: setupGenerator() costs ~0.29 s and only depends on `mc` (it wires the
 * BiomeNoise / btree pointers), so one generator is reused across seeds.  Only
 * applySeed() runs per call.  search() holds g_searchMutex, so no extra lock.
 */
struct GeneratorCache
{
    int mc = -1;
    Generator g{};
};

GeneratorCache g_generatorCache;

std::vector<Res> dedup(const std::vector<Res> &v)
{
    std::set<Point> seen;
    std::vector<Res> out;
    for (const auto &item: v)
        if (seen.insert(item.point).second)
            out.push_back(item);
    return out;
}

void resetProgressState()
{
    g_progress.chunkInRunning.store(0);
    g_progress.current.store(0);
    g_progress.total.store(0);
    g_progress.phase1.store(0);
    g_progress.try_pause.store(false);
    g_progress.try_stop.store(false);
}

/** Truncate sorted phase-1 candidates to those with area >= best * opV (opV<=0 keeps all). */
void applyPreserveRange(std::vector<Res> &res, float opV)
{
    if (opV <= 0.0f || res.empty())
        return;
    const double thr = (double) res[0].area * (double) opV;
    size_t keep = 0;
    while (keep < res.size() && (double) res[keep].area >= thr)
        keep++;
    res.resize(keep);
}

std::vector<Hit> toHits(const std::vector<Res> &results)
{
    std::vector<Hit> hits;
    hits.reserve(results.size());
    for (const auto &item: results)
        hits.push_back(Hit{item.point.x, item.point.y, item.area, item.caveArea, item.riverArea});
    return hits;
}

std::vector<Hit> finish(std::vector<Res> results)
{
    auto hits = toHits(results);
    resetProgressState();
    return hits;
}

// ---------------------------------------------------------------------------
// Weirdness y-offset score.
//
// Faithful port of the old Java WeirdnessYOffset implementation. Java long is
// a wrapping 64-bit value and >>> is an unsigned shift, so uint64_t is the
// exact equivalent here. The score is only compared with 0.75 / 0.85 by the
// caller (the GUI keeps those thresholds and the page-jump decision).
// ---------------------------------------------------------------------------
constexpr uint64_t XRSR_MIX1 = 0xbf58476d1ce4e5b9ULL;
constexpr uint64_t XRSR_MIX2 = 0x94d049bb133111ebULL;
constexpr uint64_t XRSR_SILVER = 0x6a09e667f3bcc909ULL;
constexpr uint64_t XRSR_GOLDEN = 0x9e3779b97f4a7c15ULL;
constexpr uint64_t XRSR_FALLBACK_LO = 0x9e3779b97f4a7c15ULL;
constexpr uint64_t XRSR_FALLBACK_HI = 0x6a09e667f3bcc909ULL;

/* md5 "minecraft:weirdness" */
constexpr uint64_t HASH_W_LO = 0xefc8ef4d36102b34ULL;
constexpr uint64_t HASH_W_HI = 0x1beeeb324a0f24eaULL;
/* md5 "octave_-7" */
constexpr uint64_t OCT_M7_LO = 0xf11268128982754fULL;
constexpr uint64_t OCT_M7_HI = 0x257a1d670430b0aaULL;
/* md5 "octave_-6" */
constexpr uint64_t OCT_M6_LO = 0xe51c98ce7d1de664ULL;
constexpr uint64_t OCT_M6_HI = 0x5f9478a733040c45ULL;

uint64_t rol64(uint64_t a, int bits)
{
    return (a << bits) | (a >> (64 - bits));
}

uint64_t mix64(uint64_t a)
{
    a = (a ^ (a >> 30)) * XRSR_MIX1;
    a = (a ^ (a >> 27)) * XRSR_MIX2;
    return a ^ (a >> 31);
}

struct JavaRng {
    uint64_t lo;
    uint64_t hi;

    explicit JavaRng(uint64_t seed)
    {
        seed ^= XRSR_SILVER;
        lo = mix64(seed);
        hi = mix64(seed + XRSR_GOLDEN);
        if ((lo | hi) == 0)
        {
            lo = XRSR_FALLBACK_LO;
            hi = XRSR_FALLBACK_HI;
        }
    }

    JavaRng(uint64_t lo_, uint64_t hi_) : lo(lo_), hi(hi_)
    {
        if ((lo | hi) == 0)
        {
            lo = XRSR_FALLBACK_LO;
            hi = XRSR_FALLBACK_HI;
        }
    }

    uint64_t next()
    {
        const uint64_t l = lo;
        const uint64_t h = hi;
        const uint64_t r = rol64(l + h, 17) + l;
        hi = h ^ l;
        lo = rol64(l, 49) ^ hi ^ (hi << 21);
        hi = rol64(hi, 28);
        return r;
    }
};

void forkPair(JavaRng &rng, uint64_t out[2])
{
    out[0] = rng.next();
    out[1] = rng.next();
}

void advanceHash(uint64_t lo, uint64_t hi, uint64_t out[2])
{
    JavaRng rng(lo, hi);
    rng.next();
    out[0] = rng.lo;
    out[1] = rng.hi;
}

void noiseYoFork(const uint64_t fork[2], uint64_t out[2])
{
    JavaRng rng(fork[0], fork[1]);
    rng.next();
    out[0] = rng.lo;
    out[1] = rng.hi;
}

float octaveYo(const uint64_t yo[2], const uint64_t hash[2])
{
    JavaRng rng(yo[0] ^ hash[0], yo[1] ^ hash[1]);
    const int top = static_cast<int>(rng.next() >> 32) & 0xFFFFFF;
    return static_cast<float>(top) * 5.9604645E-8f;
}

} // namespace

void resetSearchState()
{
    std::lock_guard<std::mutex> guard(g_searchMutex);
    g_results.clear();
    resetProgressState();
}

void requestPause()
{
    g_progress.try_pause.store(true);
}

void requestResume()
{
    g_progress.try_pause.store(false);
}

void requestStop()
{
    g_progress.try_stop.store(true);
}

bool stopRequested()
{
    return g_progress.try_stop.load();
}

ProgressView getProgress()
{
    ProgressView view;
    if (g_progress.total.load() <= 0)
        return view;

    int status = 0;
    const int phase = g_progress.phase1.load();
    const int cur = g_progress.current.load();
    const int tot = g_progress.total.load();
    if (phase == 0)
        status = g_progress.chunkInRunning.load() == 0 ? 1 : 0;
    else if (phase == 1)
        status = g_progress.chunkInRunning.load() == 0 ? 1 : 0;
    else if (phase == 2 || phase == 3)
        status = cur >= tot ? 2 : 1;

    view.current = cur;
    view.total = tot;
    view.phase = phase;
    view.status = status;
    view.tryPause = g_progress.try_pause.load() ? 1 : 0;
    view.tryStop = g_progress.try_stop.load() ? 1 : 0;
    view.available = true;
    return view;
}

float weirdnessYOffset(uint64_t seed)
{
    JavaRng world(seed);
    uint64_t noise[2];
    forkPair(world, noise);
    noise[0] ^= HASH_W_LO;
    noise[1] ^= HASH_W_HI;

    JavaRng noiseRng(noise[0], noise[1]);
    uint64_t a[2];
    uint64_t b[2];
    uint64_t fork[2];
    forkPair(noiseRng, fork);
    noiseYoFork(fork, a);
    forkPair(noiseRng, fork);
    noiseYoFork(fork, b);

    uint64_t h0[2];
    uint64_t h1[2];
    advanceHash(OCT_M7_LO, OCT_M7_HI, h0);
    advanceHash(OCT_M6_LO, OCT_M6_HI, h1);

    const float w0a = octaveYo(a, h0);
    const float w1a = octaveYo(a, h1);
    const float w0b = octaveYo(b, h0);
    const float w1b = octaveYo(b, h1);
    return std::abs(w0a - 0.5f) + std::abs(w1a - 0.5f)
        + std::abs(w0b - 0.5f) + std::abs(w1b - 0.5f);
}

std::vector<Hit> getNowResults()
{
    if (g_results.empty())
        return {};

    auto res = g_results.getAllResults();
    std::ranges::sort(res, [](const Res &a, const Res &b) { return a.area > b.area; });
    return toHits(res);
}

std::vector<Hit> search(const SearchParams &params)
{
    std::lock_guard<std::mutex> guard(g_searchMutex);

    if (!validate(params, nullptr))
    {
        g_results.clear();
        resetProgressState();
        return {};
    }

    PreciseScratchSession scratchSession;

    const PreciseYRange yRange{params.yMin, params.yMax};

    float riverWeight = params.riverWeight;
    if (riverWeight < 0.0f) riverWeight = 0.0f;
    if (riverWeight > 1.0f) riverWeight = 1.0f;

    int mc = params.mc;
    if (mc < MC_1_18 || mc > MC_NEWEST)
        mc = MC_26_1;

    /* B6: reuse the Generator.  setupGenerator() only depends on `mc`; the seed
     * is applied per call and phase-2 workers keep their own thread_local
     * generators, so this saves the ~0.29 s setup on every batch seed. */
    if (g_generatorCache.mc != mc)
    {
        setupGenerator(&g_generatorCache.g, mc, FORCE_OCEAN_VARIANTS);
        g_generatorCache.mc = mc;
    }
    Generator &g = g_generatorCache.g;
    applySeed(&g, DIM_OVERWORLD, params.seed);

    g_results.clear();
    resetProgressState();

    int threads = params.numThreads > 0 ? params.numThreads
                                        : static_cast<int>(std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;

    const int startX = params.startX;
    const int startZ = params.startZ;
    const int width = params.width;
    const int height = params.height;

    /* searchMode 2: large-pit search. Chains still cover the world; results are clipped to the requested box. */
    if (params.searchMode == SEARCH_MODE_FULL_MAP_PIT)
    {
        const int boxMinX = startX + RING_OUTER;
        const int boxMaxX = startX + width - 1 - RING_OUTER;
        const int boxMinZ = startZ + RING_OUTER;
        const int boxMaxZ = startZ + height - 1 - RING_OUTER;
        std::vector<Res> finallyResults;
        try
        {
            finallyResults = runLargePitSearch(&g, params.minArea, riverWeight, threads, &g_progress,
                                               boxMinX, boxMaxX, boxMinZ, boxMaxZ, yRange,
                                               params.afkY);
        }
        catch (const std::bad_alloc &)
        {
            resetProgressState();
            return {};
        }
        catch (const std::exception &)
        {
            resetProgressState();
            return {};
        }
        if (g_progress.try_stop.load())
        {
            resetProgressState();
            return {};
        }
        std::ranges::sort(finallyResults, [](const Res &a, const Res &b) { return a.area > b.area; });
        finallyResults = dedup(finallyResults);
        g_progress.phase1.store(-1);
        if (finallyResults.empty())
        {
            resetProgressState();
            return {};
        }
        return finish(std::move(finallyResults));
    }

    /* Phase 0: Cont/Ridge A period tables for large searches.
     *
     * Any phase-1 scale can be served by the tables: the lattice check in
     * sampleContOctA()/sampleWeirdnessCached() decides per sample whether the
     * tables apply, so precise mode (Cont@32 + Weird@16) keeps its cache too.
     * A table build that fails is a slowdown, not a failed search. */
    int contSc = params.contScale > 0 ? params.contScale : SearchConfig::PHASE1_CONT_PREFILTER_SCALE;
    int weirdSc = params.weirdScale > 0 ? params.weirdScale : SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;
    if (weirdSc <= 0)
        weirdSc = SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;
    if (contSc <= 0 || contSc % weirdSc != 0)
        contSc = weirdSc;

    const long long userW = (long long) width - 2LL * RING_OUTER;
    const long long userH = (long long) height - 2LL * RING_OUTER;
    const bool wantPhase0 =
        (userW >= SearchConfig::PHASE0_MIN_SIDE_BLOCKS && userH >= SearchConfig::PHASE0_MIN_SIDE_BLOCKS)
        || (userW > 0 && userH > 0
            && userW * userH >= SearchConfig::PHASE0_MIN_SIDE_BLOCKS * SearchConfig::PHASE0_MIN_SIDE_BLOCKS);

    OctaveACache octaveCache;

    /* GPU backend for the phase-1 coarse stages (docs/GPU_ACCEL_PLAN.md §5).
     * Below the gate the device is never probed and OpenCL is never loaded. */
    GpuSession gpuSession;
    {
        /* params.allowGpu is the GUI's "allow GPU" checkbox: when it is off the
         * device is never probed. On the CPU, or when the gate does not pass, or
         * when no usable device exists, the search silently uses the CPU path. */
        const lowyd_gpu::Backend requested = params.allowGpu
            ? lowyd_gpu::backend()
            : lowyd_gpu::Backend::Cpu;
        bool allowGpu = requested == lowyd_gpu::Backend::Gpu;
        if (!allowGpu && requested == lowyd_gpu::Backend::Auto)
        {
            allowGpu = (userW >= kGpuMinUserSide && userH >= kGpuMinUserSide)
                || (threads <= kGpuMaxThreads
                    && userW >= kGpuThreadedUserSide && userH >= kGpuThreadedUserSide);
        }
        lowyd_gpu::activate(&g, allowGpu);
    }

    try
    {
        if (wantPhase0)
        {
            g_progress.phase1.store(0);
            g_progress.chunkInRunning.store(0);

            /* A failed/short table build is a slowdown, never a failed search:
             * keep the cache inactive and fall back to live octave sampling. */
            bool built = false;
            try
            {
                lowyd_bench::Scope benchScope(lowyd_bench::STAGE_PHASE0);
                built = buildOctaveACache(&octaveCache, &g.bn, contSc, weirdSc, threads,
                                          &g_progress.current, &g_progress.total,
                                          &g_progress.try_pause, &g_progress.try_stop);
            }
            catch (const std::bad_alloc &)
            {
                built = false;
            }

            if (built)
                setActiveOctaveACache(&octaveCache);
            else
                setActiveOctaveACache(nullptr);

            if (g_progress.try_stop.load())
            {
                setActiveOctaveACache(nullptr);
                resetProgressState();
                return {};
            }
        }
        else
        {
            setActiveOctaveACache(nullptr);
        }

        g_progress.phase1.store(1);
        g_progress.current.store(0);
        g_progress.total.store(0);
        g_progress.chunkInRunning.store(0);

        findBiggestRiverParallelPool(g_results, &g, startX, startZ, width, height, params.minArea,
                                     &g_progress, threads, contSc, /*phase1Pipeline=*/1, weirdSc);
    }
    catch (const std::bad_alloc &)
    {
        setActiveOctaveACache(nullptr);
        resetProgressState();
        return {};
    }
    catch (const std::exception &)
    {
        setActiveOctaveACache(nullptr);
        resetProgressState();
        return {};
    }

    setActiveOctaveACache(nullptr);

    /* Phase-1 workers are joined; their (small) scratch buffers are done. */
    clearPreciseScratchPool();

    auto res = g_results.getAllResults();
    if (g_progress.try_stop.load())
    {
        resetProgressState();
        return {};
    }

    std::ranges::sort(res, [](const Res &a, const Res &b) { return a.area > b.area; });
    applyPreserveRange(res, params.preserveRange);

    g_progress.phase1.store(2);
    g_progress.total.store(static_cast<int>(res.size()));
    g_progress.current.store(0);
    g_progress.chunkInRunning.store(0);

    g_results.clear();

    if (lowyd_bench::enabled())
        lowyd_bench::add(lowyd_bench::counters().phase2Candidates, (uint64_t) res.size());

    {
        /* Bench scope covers the pool join in ~ThreadPool, i.e. the whole phase 2. */
        lowyd_bench::Scope benchPhase2(lowyd_bench::STAGE_PHASE2);

        if (!res.empty())
        {
            try
            {
                ThreadPool pool((size_t) threads);
                for (const auto &it: res)
                {
                    if (g_progress.try_stop.load())
                        break;
                    pool.enqueue([&, cand = it, seedVal = params.seed, minArea = params.minArea,
                                  riverWeight, mc]() {
                        if (g_progress.try_stop.load())
                            return;
                        while (g_progress.try_pause.load())
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            if (g_progress.try_stop.load())
                                return;
                        }
                        g_progress.chunkInRunning.fetch_add(1);

                        // Per-worker Generator: setup once, re-seed when seed/mc changes.
                        thread_local Generator tlsG;
                        thread_local bool tlsInited = false;
                        thread_local int tlsMc = -1;
                        thread_local uint64_t tlsSeed = ~0ull;
                        if (!tlsInited || tlsMc != mc)
                        {
                            setupGenerator(&tlsG, mc, FORCE_OCEAN_VARIANTS);
                            tlsInited = true;
                            tlsMc = mc;
                            tlsSeed = ~0ull;
                        }
                        if (tlsSeed != seedVal)
                        {
                            applySeed(&tlsG, DIM_OVERWORLD, seedVal);
                            tlsSeed = seedVal;
                        }

                        auto temp = findBiggestRiver<1>(
                            &tlsG,
                            cand.point.x - SearchConfig::REFINE_HALF_WINDOW,
                            cand.point.y - SearchConfig::REFINE_HALF_WINDOW,
                            SearchConfig::REFINE_WINDOW_BLOCKS, SearchConfig::REFINE_WINDOW_BLOCKS,
                            minArea, 1.0, FilterMode::PreciseBiome, riverWeight,
                            nullptr, 0, nullptr, yRange, params.afkY);

                        if (!temp.empty() && temp[0].area >= minArea)
                            g_results.addResult(temp[0]);

                        g_progress.chunkInRunning.fetch_sub(1);
                        g_progress.current.fetch_add(1);
                    });
                }
            }
            catch (const std::bad_alloc &)
            {
                resetProgressState();
                return {};
            }
            catch (const std::exception &)
            {
                resetProgressState();
                return {};
            }
        }
    }

    if (g_progress.try_stop.load())
    {
        resetProgressState();
        return {};
    }

    auto finalList = dedup(g_results.getAllResults());
    g_results.clear();
    g_results.addResults(finalList);

    auto finallyResults = g_results.getAllResults();
    g_progress.phase1.store(-1);

    return finish(std::move(finallyResults));
}

} // namespace lowyd
