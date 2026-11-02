#ifndef OCTAVE_FIELD_CACHE_H
#define OCTAVE_FIELD_CACHE_H

#include "cubiomes/biomenoise.h"
#include "cubiomes/noise.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

/* floor division and non-negative remainder for a positive divisor */
inline int octaveFloorDiv(int a, int b)
{
    int q = a / b;
    const int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        --q;
    return q;
}

inline int octaveModPositive(int a, int m)
{
    int r = a % m;
    if (r < 0)
        r += m;
    return r;
}

/**
 * Period tables for Cont octA[0..4] (@contScale) and Ridge/Weirdness octA[0..2] (@weirdScale).
 * Sample lattice matches Cont/Weird prefilter centers: climate = i*(scale/4) + (scale/4)/2.
 *
 * C2: the climate step (`scale/4`) and the table period (`n = periodWorld/scale`) are
 * powers of two for every supported scale, so an index lookup is a shift plus a mask.
 * The generic (division based) implementation is kept for any other configuration.
 */
struct OctaveACache {
    static constexpr int CONT_OCT = 5;
    static constexpr int RIDGE_OCT = 3;

    struct Field {
        int periodWorld = 0;
        int sampleScale = 0;
        int n = 0;       /* periodWorld / sampleScale */
        int stepClim = 0; /* sampleScale / 4: climate cells between lattice points */
        int half = 0;    /* stepClim / 2: climate offset of lattice index 0 */
        int shift = 0;   /* log2(stepClim), valid when fastIndex */
        int stepMask = 0; /* stepClim - 1, valid when fastIndex */
        int indexMask = 0; /* n - 1, valid when fastIndex */
        bool fastIndex = false;
        std::vector<float> data; /* n * n, row-major */

        /** Generic lookup for any (bx, bz); includes the period wrap. */
        float atClimate(int bx, int bz) const;

        /**
         * Lookup by lattice step index (see probeClimate). The index is NOT
         * wrapped by the caller: every octave has its own period `n`, so the
         * period wrap belongs here.
         */
        inline float atIndex(int ix, int iz) const
        {
            if (fastIndex)
                return data[(size_t) (iz & indexMask) * (size_t) n + (size_t) (ix & indexMask)];
            return data[(size_t) octaveModPositive(iz, n) * (size_t) n + (size_t) octaveModPositive(ix, n)];
        }
    };

    Field contA[CONT_OCT];
    Field ridgeA[RIDGE_OCT];
    bool ready = false;
};

/**
 * Per-sample lattice probe. All octaves of one climate field share the same
 * `sampleScale`, therefore the same step/half, so the lattice test only has to
 * run once per sample instead of once per octave (C2).
 */
struct ClimateProbe {
    bool onLattice = false;
    /** Lattice step index (climate coordinate / stepClim); not period-wrapped. */
    int ix = 0;
    int iz = 0;
};

/**
 * Resolve (bx, bz) against the field lattice. onLattice is false when the
 * sample is not a lattice point, in which case the caller must sample live.
 */
inline ClimateProbe probeClimate(const OctaveACache::Field &f, int bx, int bz)
{
    ClimateProbe probe;
    if (f.n <= 0 || f.stepClim <= 0 || f.data.empty())
        return probe;

    const int ax = bx - f.half;
    const int az = bz - f.half;
    if (f.fastIndex)
    {
        if ((ax & f.stepMask) != 0 || (az & f.stepMask) != 0)
            return probe;
        /* Arithmetic right shift == floor; the period wrap happens in atIndex. */
        probe.ix = ax >> f.shift;
        probe.iz = az >> f.shift;
    }
    else
    {
        if (octaveModPositive(ax, f.stepClim) != 0 || octaveModPositive(az, f.stepClim) != 0)
            return probe;
        probe.ix = octaveFloorDiv(ax, f.stepClim);
        probe.iz = octaveFloorDiv(az, f.stepClim);
    }
    probe.onLattice = true;
    return probe;
}

/** World-block periods for Cont A0..A4 and Ridge A0..A2. */
inline constexpr int kContAPeriodWorld[OctaveACache::CONT_OCT] = {
    524288, 262144, 131072, 65536, 32768
};
inline constexpr int kRidgeAPeriodWorld[OctaveACache::RIDGE_OCT] = {
    131072, 65536, 32768
};

/**
 * Build Cont+Ridge A period tables from seeded BiomeNoise.
 * Progress: current/total over all cells; phase should be 0. Honors pause/stop atomics.
 *
 * Returns false (and leaves out->ready == false) for scales that the cache cannot
 * serve: the tables only exist for the fast-mode product (Cont >= 128, Weird >= 32).
 */
bool buildOctaveACache(
    OctaveACache *out,
    const BiomeNoise *bn,
    int contScale,
    int weirdScale,
    int numThreads,
    std::atomic_int *progressCurrent,
    std::atomic_int *progressTotal,
    const std::atomic_bool *tryPause,
    const std::atomic_bool *tryStop);

void setActiveOctaveACache(const OctaveACache *cache); /* nullptr disables */
const OctaveACache *getActiveOctaveACache();

#endif
