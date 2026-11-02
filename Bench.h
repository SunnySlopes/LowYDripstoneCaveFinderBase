//
// LowYDripstoneCaveFinderBase C0: optional stage-timing and diagnostics.
//
// The pipeline is measured with five scoped timers (phase 0 octave tables,
// continentalness prefilter mask, weirdness ring scan, ClimateCoarse
// subsearches, phase-2 precise refinement). Everything here is inert unless
// the CLI is started with --bench: the only cost when disabled is one relaxed
// atomic load per scope.
//
// Deliberately *not* instrumented per sample (ringSum / atClimate /
// coarseCellValue). Counting inside those loops would perturb exactly what the
// optimisations change, so the diagnostics stay at chunk / subsearch
// granularity and the A-table lattice alignment is derived from the window
// geometry once per chunk instead of being counted per probe.
//
#ifndef LOWYD_BENCH_H
#define LOWYD_BENCH_H

#include <atomic>
#include <chrono>
#include <cstdint>

namespace lowyd_bench {

enum Stage
{
    STAGE_PHASE0 = 0,      /* Cont/Ridge octave period tables */
    STAGE_CONT_MASK = 1,   /* phase-1 continentalness prefilter mask */
    STAGE_WEIRD_RING = 2,  /* phase-1 weirdness ring scan */
    STAGE_CLIMATE_COARSE = 3, /* ClimateCoarse subsearches around peaks */
    STAGE_PHASE2 = 4,      /* phase-2 precise refinement */
    STAGE_GPU = 5,         /* host wall time inside OpenCL submissions */
    STAGE_COUNT = 6
};

inline const char *stageName(Stage stage)
{
    switch (stage)
    {
    case STAGE_PHASE0: return "phase0 octave cache";
    case STAGE_CONT_MASK: return "cont prefilter mask";
    case STAGE_WEIRD_RING: return "weird ring scan";
    case STAGE_CLIMATE_COARSE: return "climate coarse";
    case STAGE_PHASE2: return "phase2 precise";
    case STAGE_GPU: return "gpu coarse grid (host)";
    default: return "?";
    }
}

struct Counters
{
    std::atomic<uint64_t> ns[STAGE_COUNT];
    std::atomic<uint64_t> calls[STAGE_COUNT];
    std::atomic<bool> enabled{false};

    /* Geometry diagnostics: one increment per chunk / subsearch, never per sample. */
    std::atomic<uint64_t> chunks{0};
    std::atomic<uint64_t> latticeChunks{0}; /* chunks whose prefilter lands on the A-table lattice */
    std::atomic<uint64_t> contTiles{0};
    std::atomic<uint64_t> contTilesPass{0};
    std::atomic<uint64_t> ringCenters{0};
    std::atomic<uint64_t> coarseSubsearches{0};
    std::atomic<uint64_t> coarseCells{0};
    std::atomic<uint64_t> phase2Candidates{0};
    std::atomic<uint64_t> phase0Bytes{0};
    std::atomic<uint64_t> phase0Samples{0};
    /** Chebyshev cell writes the naive dilation loop would have performed (C3). */
    std::atomic<uint64_t> dilateWrites{0};
    /** Weirdness cells actually evaluated (N0: ns/cell tracking). */
    std::atomic<uint64_t> weirdCells{0};
    /** Phase-1 ring candidates emitted by the max-area filter. */
    std::atomic<uint64_t> ringCandidates{0};
    /** Highest number of candidates one chunk/subsearch produced. */
    std::atomic<uint64_t> ringCandidatesMax{0};

    Counters() { reset(); }

    void reset()
    {
        for (int i = 0; i < STAGE_COUNT; i++)
        {
            ns[i].store(0, std::memory_order_relaxed);
            calls[i].store(0, std::memory_order_relaxed);
        }
        chunks.store(0, std::memory_order_relaxed);
        latticeChunks.store(0, std::memory_order_relaxed);
        contTiles.store(0, std::memory_order_relaxed);
        contTilesPass.store(0, std::memory_order_relaxed);
        ringCenters.store(0, std::memory_order_relaxed);
        coarseSubsearches.store(0, std::memory_order_relaxed);
        coarseCells.store(0, std::memory_order_relaxed);
        phase2Candidates.store(0, std::memory_order_relaxed);
        phase0Bytes.store(0, std::memory_order_relaxed);
        phase0Samples.store(0, std::memory_order_relaxed);
        dilateWrites.store(0, std::memory_order_relaxed);
        weirdCells.store(0, std::memory_order_relaxed);
        ringCandidates.store(0, std::memory_order_relaxed);
        ringCandidatesMax.store(0, std::memory_order_relaxed);
    }
};

inline Counters &counters()
{
    static Counters instance;
    return instance;
}

inline bool enabled()
{
    return counters().enabled.load(std::memory_order_relaxed);
}

inline void setEnabled(bool on)
{
    counters().enabled.store(on, std::memory_order_relaxed);
}

inline void reset()
{
    counters().reset();
}

inline uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void add(std::atomic<uint64_t> &slot, uint64_t value)
{
    if (value == 0)
        return;
    slot.fetch_add(value, std::memory_order_relaxed);
}

/** Times one call of a pipeline stage; no-op unless --bench is active. */
class Scope
{
public:
    explicit Scope(Stage stage) : stage_(stage), startNs_(0), active_(enabled())
    {
        if (active_)
            startNs_ = nowNs();
    }

    ~Scope()
    {
        if (!active_)
            return;
        Counters &c = counters();
        c.ns[stage_].fetch_add(nowNs() - startNs_, std::memory_order_relaxed);
        c.calls[stage_].fetch_add(1, std::memory_order_relaxed);
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Stage stage_;
    uint64_t startNs_;
    bool active_;
};

} // namespace lowyd_bench

#endif // LOWYD_BENCH_H
