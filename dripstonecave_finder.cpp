#include "lowydripstonecavefinder.h"

#include "cubiomes/generator.h"
#include "cubiomes/biomes.h"
#include <vector>
#include <algorithm>
#include <queue>
#include <iostream>
#include <chrono>
#include "Thread.h"
#include "BiomeSampler.h"
#include "SearchConfig.h"
#include <functional>
#include <ranges>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <thread>

// Bench.h and OctaveFieldCache.h are included at global scope by
// lowydripstonecavefinder.cpp before this file is pulled in, so lowyd_bench::,
// OctaveACache and probeClimate() are already visible here.

enum class FilterMode {
    WeirdnessOnly,
    ContinentalnessOnly,
    ContThenWeird,
    WeirdThenCont,
    ClimateCoarse,
    PreciseBiome,
    /** Cell value taken from contPrefilter mask (no extra noise). */
    UsePrefilterMask
};

struct RingMask {
    struct Row { int dz; int out; int in; };
    std::vector<Row> rows;
    int R_out = 0;
};

template<int scale>
static const RingMask &getRingMask()
{
    static const RingMask mask = [] {
        RingMask m;
        m.R_out = 128 / scale;
        const int R_in = 24 / scale;
        std::vector<int> dxOut(2 * m.R_out + 1);
        std::vector<int> dxIn(2 * R_in + 1);
        for (int dz = -m.R_out; dz <= m.R_out; dz++)
            dxOut[dz + m.R_out] = (int) std::floor(std::sqrt((double) m.R_out * m.R_out - dz * dz));
        for (int dz = -R_in; dz <= R_in; dz++)
            dxIn[dz + R_in] = (int) std::floor(std::sqrt((double) R_in * R_in - dz * dz));
        m.rows.reserve(2 * m.R_out + 1);
        for (int dz = -m.R_out; dz <= m.R_out; dz++)
        {
            RingMask::Row row{};
            row.dz = dz;
            row.out = dxOut[dz + m.R_out];
            row.in = (std::abs(dz) <= R_in) ? dxIn[dz + R_in] : -1;
            m.rows.push_back(row);
        }
        return m;
    }();
    return mask;
}

static inline int coarseCellValue(Generator *g, int worldX, int worldZ, FilterMode mode)
{
    const int nx = worldX / 4;
    const int nz = worldZ / 4;
    if (mode == FilterMode::WeirdnessOnly)
        return passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
    if (mode == FilterMode::ContinentalnessOnly)
        return passContinentalnessPartial(&g->bn, nx, nz) ? 1 : 0;
    if (mode == FilterMode::ContThenWeird)
    {
        if (!passContinentalnessPartial(&g->bn, nx, nz))
            return 0;
        return passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
    }
    if (mode == FilterMode::WeirdThenCont)
    {
        if (!passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS))
            return 0;
        return passContinentalnessPartial(&g->bn, nx, nz) ? 1 : 0;
    }

    return passCoarseCaveCell(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
}

/** Build Cont@scale AND Weirdness@scale mask, then Chebyshev-dilate by `dilate` tiles. */
static std::vector<uint8_t> buildDilatedContWeirdPrefilterMask(
    Generator *g, int startX, int startZ, int sx, int sz,
    int scale, double contThr, int dilate)
{
    const int W = sx / scale;
    const int H = sz / scale;
    std::vector<uint8_t> keep((size_t) std::max(0, W) * std::max(0, H), 0);
    if (W <= 0 || H <= 0)
        return keep;

    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z * scale + scale / 2;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x * scale + scale / 2;
            const int nx = worldX / 4;
            const int nz = worldZ / 4;
            if (!passContinentalnessPartialThr(&g->bn, nx, nz, contThr))
                continue;
            if (!passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS))
                continue;
            keep[(size_t) z * W + x] = 1;
        }
    }

    if (dilate <= 0)
        return keep;

    std::vector<uint8_t> out = keep;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            if (!keep[(size_t) z * W + x])
                continue;
            for (int dz = -dilate; dz <= dilate; dz++)
            {
                for (int dx = -dilate; dx <= dilate; dx++)
                {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= W || nz >= H)
                        continue;
                    out[(size_t) nz * W + nx] = 1;
                }
            }
        }
    }
    return out;
}

/** Build C>thr mask at `scale`, then Chebyshev-dilate by `dilate` tiles. */
static std::vector<uint8_t> buildDilatedContPrefilterMask(
    Generator *g, int startX, int startZ, int sx, int sz,
    int scale, double thr, int dilate)
{
    const int W = sx / scale;
    const int H = sz / scale;
    std::vector<uint8_t> keep((size_t) std::max(0, W) * std::max(0, H), 0);
    if (W <= 0 || H <= 0)
        return keep;

    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z * scale + scale / 2;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x * scale + scale / 2;
            if (passContinentalnessPartialThr(&g->bn, worldX / 4, worldZ / 4, thr))
                keep[(size_t) z * W + x] = 1;
        }
    }

    if (dilate <= 0)
        return keep;

    std::vector<uint8_t> out = keep;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            if (!keep[(size_t) z * W + x])
                continue;
            for (int dz = -dilate; dz <= dilate; dz++)
            {
                for (int dx = -dilate; dx <= dilate; dx++)
                {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= W || nz >= H)
                        continue;
                    out[(size_t) nz * W + nx] = 1;
                }
            }
        }
    }
    return out;
}

static inline bool contPrefilterAllows(
    const std::vector<uint8_t> &mask, int maskW, int maskH,
    int startX, int startZ, int preScale, int worldX, int worldZ)
{
    if (mask.empty() || maskW <= 0 || maskH <= 0 || preScale <= 0)
        return true;
    const int tx = (worldX - startX) / preScale;
    const int tz = (worldZ - startZ) / preScale;
    if (tx < 0 || tz < 0 || tx >= maskW || tz >= maskH)
        return false;
    return mask[(size_t) tz * maskW + tx] != 0;
}

/** OpenCL grid kind for a filter mode, or -1 when the CPU has to do it. */
static int gpuGridMode(FilterMode mode)
{
    switch (mode)
    {
    case FilterMode::WeirdnessOnly: return (int) lowyd_gpu::GRID_WEIRD;
    case FilterMode::ContinentalnessOnly: return (int) lowyd_gpu::GRID_CONT;
    case FilterMode::ClimateCoarse: return (int) lowyd_gpu::GRID_CLIMATE;
    default: return -1;
    }
}

/**
 * Evaluates a W x H coarse-cell grid on the GPU.
 *
 * Returns true and fills `cells` (W*H bytes, 1 = pass) when the device ran it.
 * The CPU semantics the kernel reproduces are described in GpuCoarse.h; the
 * grid is the *only* thing the device produces, so the prefix/ring/peak logic
 * downstream is shared bit for bit with the CPU path.
 */
static bool gpuCoarseCells(
    Generator *g, int startX, int startZ, int W, int H, int scale,
    FilterMode mode, double contThreshold,
    const std::vector<uint8_t> *mask, int maskScale, int maskW, int maskH,
    std::vector<uint8_t> &cells)
{
    if (!lowyd_gpu::enabled())
        return false;
    const int gridMode = gpuGridMode(mode);
    if (gridMode < 0 || scale <= 1)
        return false;
    if ((size_t) W * (size_t) H < 1024)
        return false;

    const uint8_t *maskData = nullptr;
    if (mask && !mask->empty() && maskScale > 0 && maskW > 0 && maskH > 0)
    {
        if (mask->size() < (size_t) maskW * (size_t) maskH)
            return false;
        maskData = mask->data();
    }

    lowyd_bench::Scope benchScope(lowyd_bench::STAGE_GPU);
    cells.clear();
    return lowyd_gpu::evalGrid(g, startX, startZ, W, H, scale,
                               (lowyd_gpu::GridMode) gridMode, contThreshold,
                               maskData, startX, startZ, maskData ? maskScale : 0,
                               maskData ? maskW : 0, maskData ? maskH : 0,
                               cells, nullptr);
}

static void fillPreciseGrid(Generator *g, int startX, int startZ, int W, int H,
                            const lowyd::PreciseYRange &yRange,
                            std::vector<int> &rawRiver, std::vector<int> &rawCave)
{
    fillPreciseWindow(g, startX, startZ, W, H, yRange, rawRiver, rawCave);
}

struct Point {
    int x = 0;
    int y = 0;
    std::strong_ordering operator<=>(const Point &) const = default;
};

struct Res {
    Point point;
    int area = 0;
    int caveArea = 0;
    int riverArea = 0;

    std::strong_ordering operator<=>(const Res &other) const noexcept
    {
        if (auto c = area <=> other.area; c != 0) return c;
        return point <=> other.point;
    }

    Res() = default;
    Res(Point p, int total, int cave, int river = 0)
        : point(p), area(total), caveArea(cave), riverArea(river) {}
};

template<int scale>
std::vector<Res> findBiggestRiver(
    Generator *g,
    int startX, int startZ,
    int sx, int sz,
    int min,
    double f,
    FilterMode mode,
    float riverWeight = 0.7f,
    const std::vector<uint8_t> *contPrefilter = nullptr,
    int contPrefilterScale = 0,
    const std::vector<uint8_t> *coarseCells = nullptr,
    const lowyd::PreciseYRange &yRange = lowyd::PreciseYRange{}) noexcept
{
    std::vector<Res> result;
    const int W = sx / scale;
    const int H = sz / scale;
    if (W <= 0 || H <= 0) return result;

    const int preW = (contPrefilter && contPrefilterScale > 0) ? (sx / contPrefilterScale) : 0;
    const int preH = (contPrefilter && contPrefilterScale > 0) ? (sz / contPrefilterScale) : 0;

    /* B2: one set of per-thread buffers for every candidate of this worker. */
    PreciseScratch &scratch = preciseScratch();
    std::vector<int> &rawRiver = scratch.rawRiver;
    std::vector<int> &prefixRiver = scratch.prefixRiver;
    std::vector<int> &rawCave = scratch.rawCave;
    std::vector<int> &prefixCave = scratch.prefixCave;

    const int stride = W + 1;
    rawRiver.assign((size_t) W * H, 0);
    prefixRiver.assign((size_t) (W + 1) * (H + 1), 0);

    // Cave grids only needed for precise (scale==1) path.
    if constexpr (scale == 1)
    {
        rawCave.assign((size_t) W * H, 0);
        prefixCave.assign((size_t) (W + 1) * (H + 1), 0);
    }
    else
    {
        rawCave.clear();
        prefixCave.clear();
    }

#define RAWR(x,z) rawRiver[(size_t)(x) + (size_t)(z) * W]
#define RAWC(x,z) rawCave[(size_t)(x) + (size_t)(z) * W]
#define ARRR(x,z) prefixRiver[(size_t)(x) + (size_t)(z) * stride]
#define ARRC(x,z) prefixCave[(size_t)(x) + (size_t)(z) * stride]

    const int R_out = 128 / scale;
    const int occTile = std::max(8, R_out);
    const int oW = (W + occTile - 1) / occTile;
    const int oH = (H + occTile - 1) / occTile;
    std::vector<uint8_t> &occ = scratch.occ;
    occ.assign((size_t) std::max(0, oW) * std::max(0, oH), 0);

    auto markOcc = [&](int x, int z) {
        if (oW <= 0 || oH <= 0) return;
        occ[(size_t) (z / occTile) * oW + (size_t) (x / occTile)] = 1;
    };

    if constexpr (scale > 1)
    {
        /* The 0/1 cell grid comes from the device when it can run it: the
         * OpenCL path replaces exactly the coarseCellValue() loop below, so
         * everything after this block (prefix sums, ring areas, peaks) stays
         * shared with the CPU path. */
        std::vector<uint8_t> deviceCells;
        const std::vector<uint8_t> *precomputed = nullptr;
        if (coarseCells && coarseCells->size() == (size_t) W * (size_t) H)
        {
            precomputed = coarseCells;
        } else if (mode != FilterMode::UsePrefilterMask)
        {
            const double gpuThreshold = mode == FilterMode::ContinentalnessOnly
                || mode == FilterMode::ClimateCoarse
                ? CONT_PARTIAL_FINAL_THRESHOLD : 0.0;
            if (gpuCoarseCells(g, startX, startZ, W, H, scale, mode, gpuThreshold,
                               contPrefilter, contPrefilterScale, preW, preH, deviceCells))
                precomputed = &deviceCells;
        }

        uint64_t weirdCells = 0;
        if (precomputed)
        {
            const uint8_t *src = precomputed->data();
            for (int z = 0; z < H; z++)
            {
                for (int x = 0; x < W; x++)
                {
                    const int v = src[(size_t) z * (size_t) W + (size_t) x] != 0 ? 1 : 0;
                    RAWR(x, z) = v;
                    if (v)
                        markOcc(x, z);
                }
            }
            /* The device evaluates the whole grid; the CPU skips masked cells. */
            if (mode == FilterMode::WeirdnessOnly)
                weirdCells = (uint64_t) W * (uint64_t) H;
        }
        else
        {
            for (int z = 0; z < H; z++)
            {
                const int worldZ = startZ + z * scale + scale / 2;
                for (int x = 0; x < W; x++)
                {
                    const int worldX = startX + x * scale + scale / 2;
                    if (mode == FilterMode::UsePrefilterMask)
                    {
                        const int v = contPrefilter &&
                            contPrefilterAllows(*contPrefilter, preW, preH,
                                                startX, startZ, contPrefilterScale, worldX, worldZ)
                            ? 1 : 0;
                        RAWR(x, z) = v;
                        if (v)
                            markOcc(x, z);
                        continue;
                    }
                    if (contPrefilter &&
                        !contPrefilterAllows(*contPrefilter, preW, preH,
                                             startX, startZ, contPrefilterScale, worldX, worldZ))
                    {
                        RAWR(x, z) = 0;
                        continue;
                    }
                    const int v = coarseCellValue(g, worldX, worldZ, mode);
                    if (mode == FilterMode::WeirdnessOnly)
                        weirdCells++;
                    RAWR(x, z) = v;
                    if (v)
                        markOcc(x, z);
                }
            }
        }
        if (lowyd_bench::enabled() && weirdCells)
            lowyd_bench::add(lowyd_bench::counters().weirdCells, weirdCells);
    } else
    {
        fillPreciseGrid(g, startX, startZ, W, H, yRange, rawRiver, rawCave);
        for (int z = 0; z < H; z++)
        {
            for (int x = 0; x < W; x++)
            {
                if (RAWR(x, z) || RAWC(x, z))
                    markOcc(x, z);
            }
        }
    }

    for (int z = 1; z <= H; z++)
    {
        for (int x = 1; x <= W; x++)
        {
            ARRR(x, z) = RAWR(x - 1, z - 1) + ARRR(x - 1, z) + ARRR(x, z - 1) - ARRR(x - 1, z - 1);
            if constexpr (scale == 1)
            {
                ARRC(x, z) = RAWC(x - 1, z - 1) + ARRC(x - 1, z) + ARRC(x, z - 1) - ARRC(x - 1, z - 1);
            }
        }
    }

    auto occAnyInRect = [&](int x0, int x1, int z0, int z1) -> bool {
        if (oW <= 0 || oH <= 0) return true;
        const int tx0 = std::max(0, x0 / occTile);
        const int tx1 = std::min(oW - 1, x1 / occTile);
        const int tz0 = std::max(0, z0 / occTile);
        const int tz1 = std::min(oH - 1, z1 / occTile);
        if (tx0 > tx1 || tz0 > tz1) return false;
        for (int tz = tz0; tz <= tz1; tz++)
        {
            for (int tx = tx0; tx <= tx1; tx++)
            {
                if (occ[(size_t) tz * oW + (size_t) tx])
                    return true;
            }
        }
        return false;
    };

    struct CandidateArea {
        int area;
        int caveArea;
        int riverArea;
        int startX;
        int startZ;

        bool operator<(const CandidateArea &other) const noexcept
        {
            if (area != other.area) return area < other.area;
            if (startX != other.startX) return startX > other.startX;
            return startZ > other.startZ;
        }
    };

    std::priority_queue<CandidateArea> pq;
    const auto &ring = getRingMask<scale>();
    uint64_t ringCalls = 0;

    auto ringSum = [&](const std::vector<int> &pref, int cx, int cz) -> int {
        /* Hot path: annular sum via 2D prefix; keep branch light and locals in registers. */
        int area = 0;
        const int *prefData = pref.data();
        for (const auto &m: ring.rows)
        {
            const int row = cz + m.dz;
            const int row1 = row + 1;
            const size_t base1 = (size_t) row1 * (size_t) stride;
            const size_t base0 = (size_t) row * (size_t) stride;
            const int L = cx - m.out;
            const int R = cx + m.out;
            if (m.in < 0)
            {
                area += prefData[base1 + (size_t) (R + 1)]
                      - prefData[base1 + (size_t) L]
                      - prefData[base0 + (size_t) (R + 1)]
                      + prefData[base0 + (size_t) L];
            }
            else
            {
                const int Lin = cx - m.in;
                const int Rin = cx + m.in;
                area += prefData[base1 + (size_t) Lin]
                      - prefData[base1 + (size_t) L]
                      - prefData[base0 + (size_t) Lin]
                      + prefData[base0 + (size_t) L];
                area += prefData[base1 + (size_t) (R + 1)]
                      - prefData[base1 + (size_t) (Rin + 1)]
                      - prefData[base0 + (size_t) (R + 1)]
                      + prefData[base0 + (size_t) (Rin + 1)];
            }
        }
        return area;
    };

    CandidateArea maxA{0, 0, 0, 0, 0};

    /* Sparse ring centers: Chebyshev dilate of pass cells by R_out (keeps recall vs raw==1-only).
     *
     * C3: the naive form writes (2R_out+1)^2 cells per pass cell, which at scale 4
     * is 4225 writes for every passing cell. A 2D difference array makes each source
     * cell cost four increments, followed by one reconstruction pass. The write range
     * is clamped to the same [R_out, W-R_out-1] x [R_out, H-R_out-1] box the scan loop
     * reads, so the resulting mask is identical. */
    std::vector<uint8_t> &centerCand = scratch.centerCand;
    std::vector<int32_t> &diff = scratch.diff;
    if constexpr (scale > 1)
    {
        centerCand.assign((size_t) W * (size_t) H, 0);

        const int diffStride = W + 1;
        diff.assign((size_t) diffStride * (size_t) (H + 1), 0);
        uint64_t naiveWrites = 0;

        for (int z = 0; z < H; z++)
        {
            for (int x = 0; x < W; x++)
            {
                if (!RAWR(x, z))
                    continue;
                const int z0 = std::max(R_out, z - R_out);
                const int z1 = std::min(H - R_out - 1, z + R_out);
                const int x0 = std::max(R_out, x - R_out);
                const int x1 = std::min(W - R_out - 1, x + R_out);
                if (x0 > x1 || z0 > z1)
                    continue;
                diff[(size_t) z0 * (size_t) diffStride + (size_t) x0] += 1;
                diff[(size_t) z0 * (size_t) diffStride + (size_t) (x1 + 1)] -= 1;
                diff[(size_t) (z1 + 1) * (size_t) diffStride + (size_t) x0] -= 1;
                diff[(size_t) (z1 + 1) * (size_t) diffStride + (size_t) (x1 + 1)] += 1;
                if (lowyd_bench::enabled())
                    naiveWrites += (uint64_t) (x1 - x0 + 1) * (uint64_t) (z1 - z0 + 1);
            }
        }

        for (int z = 0; z < H; z++)
        {
            int rowRun = 0;
            const size_t base = (size_t) z * (size_t) diffStride;
            const size_t prev = z > 0 ? base - (size_t) diffStride : base;
            uint8_t *outRow = centerCand.data() + (size_t) z * (size_t) W;
            for (int x = 0; x < W; x++)
            {
                rowRun += diff[base + (size_t) x];
                const int value = rowRun + (z > 0 ? diff[prev + (size_t) x] : 0);
                diff[base + (size_t) x] = value;
                outRow[x] = value > 0 ? 1 : 0;
            }
        }

        if (lowyd_bench::enabled())
            lowyd_bench::add(lowyd_bench::counters().dilateWrites, naiveWrites);
    }

    for (int cz = R_out; cz < H - R_out; cz++)
    {
        for (int cx = R_out; cx < W - R_out; cx++)
        {
            if constexpr (scale > 1)
            {
                if (!centerCand[(size_t) cz * (size_t) W + (size_t) cx])
                    continue;
            }
            else
            {
                /* Skip ring centers whose bounding box has no positive cells */
                if (!occAnyInRect(cx - R_out, cx + R_out, cz - R_out, cz + R_out))
                    continue;
            }

            if (lowyd_bench::enabled())
                ringCalls++;

            int worldTotal;
            int worldCave;
            int worldRiver;

            if constexpr (scale == 1)
            {
                /* B1 volume integral (same model as C, different unit):
                 * sumC/sumR are 0..49640*L; the reported score is the
                 * per-layer average, so total stays 0..49640 and every
                 * minArea / f prune / sort keeps its old meaning. */
                const int sumR = ringSum(prefixRiver, cx, cz);
                const int sumC = ringSum(prefixCave, cx, cz);
                worldRiver = worldCave = 0;
                worldTotal = lowyd::aggregateLayerAverages(sumC, sumR, riverWeight, yRange.count(),
                                                    &worldCave, &worldRiver);
            } else
            {
                const int sum = ringSum(prefixRiver, cx, cz);
                worldTotal = sum * scale * scale;
                worldCave = 0;
                worldRiver = 0;
            }

            if (worldTotal >= maxA.area * f && worldTotal >= min)
            {
                const int worldX = startX + cx * scale;
                const int worldZ = startZ + cz * scale;
                pq.push({worldTotal, worldCave, worldRiver, worldX, worldZ});
                if (worldTotal > maxA.area)
                {
                    maxA.area = worldTotal;
                    maxA.caveArea = worldCave;
                    maxA.riverArea = worldRiver;
                }
            }
        }
    }

    while (!pq.empty())
    {
        if (const auto &ra = pq.top(); ra.area >= maxA.area * f)
        {
            result.emplace_back(Point{ra.startX, ra.startZ}, ra.area, ra.caveArea, ra.riverArea);
            if (f == 1.0) break;
        }
        pq.pop();
    }

    if (lowyd_bench::enabled())
    {
        lowyd_bench::Counters &c = lowyd_bench::counters();
        lowyd_bench::add(c.ringCandidates, (uint64_t) result.size());
        uint64_t previous = c.ringCandidatesMax.load(std::memory_order_relaxed);
        while ((uint64_t) result.size() > previous
               && !c.ringCandidatesMax.compare_exchange_weak(previous,
                                                             (uint64_t) result.size()))
        {
        }
    }

#undef RAWR
#undef RAWC
#undef ARRR
#undef ARRC

    if (lowyd_bench::enabled())
    {
        lowyd_bench::Counters &c = lowyd_bench::counters();
        lowyd_bench::add(c.ringCenters, ringCalls);
        if constexpr (scale > 1)
            lowyd_bench::add(c.coarseCells, (uint64_t) W * (uint64_t) H);
    }
    return result;
}

/** Flat (dz, out, in) triples of getRingMask<scale>(), for the device. */
template<int scale>
static const std::vector<int> &ringTriples()
{
    static const std::vector<int> triples = [] {
        const RingMask &mask = getRingMask<scale>();
        std::vector<int> flat;
        flat.reserve(mask.rows.size() * 3);
        for (const auto &row: mask.rows)
        {
            flat.push_back(row.dz);
            flat.push_back(row.out);
            flat.push_back(row.in);
        }
        return flat;
    }();
    return triples;
}

/** Fills the (constant) ring geometry of a spec. */
template<int scale>
static void fillRingSpec(lowyd_gpu::CoarseSpec &spec)
{
    spec.ring = ringTriples<scale>().data();
    spec.ringRows = (int) getRingMask<scale>().rows.size();
    spec.ringOut = getRingMask<scale>().R_out;
}

/** Phase-1 chunks one OpenCL submission covers. */
static constexpr int kGpuChunkBatch = 8;

/** LOWYD_COARSE_DEBUG=1: per chunk, compare the device candidate list with the
 *  CPU ring scan and print the first differences (regression aid). */
static bool coarseDebugEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("LOWYD_COARSE_DEBUG");
        return value && *value && *value != '0';
    }();
    return enabled;
}

static void reportCoarseDiff(const std::vector<Res> &device, const std::vector<Res> &cpu,
                             int csx, int csz)
{
    static std::atomic<int> reported{ 0 };
    static std::atomic<uint64_t> compared{ 0 };
    static std::atomic<uint64_t> mismatched{ 0 };
    compared.fetch_add(1);
    bool same = device.size() == cpu.size();
    if (same)
    {
        for (size_t i = 0; i < device.size(); i++)
        {
            if (device[i].point.x != cpu[i].point.x || device[i].point.y != cpu[i].point.y
                || device[i].area != cpu[i].area)
            {
                same = false;
                break;
            }
        }
    }
    if (same)
        return;
    mismatched.fetch_add(1);
    if (reported.fetch_add(1) >= 6)
        return;
    std::fprintf(stderr, "[coarse] chunk %d,%d device=%zu cpu=%zu (compared %llu, mismatched %llu)\n",
                 csx, csz, device.size(), cpu.size(),
                 (unsigned long long) compared.load(), (unsigned long long) mismatched.load());
    const size_t limit = std::min<size_t>(std::max(device.size(), cpu.size()), 6);
    for (size_t i = 0; i < limit; i++)
    {
        const bool differ = i >= device.size() || i >= cpu.size()
            || device[i].point.x != cpu[i].point.x || device[i].point.y != cpu[i].point.y
            || device[i].area != cpu[i].area;
        std::fprintf(stderr, "[coarse]  %s #%zu device=(%d,%d,%d) cpu=(%d,%d,%d)\n",
                     differ ? "!" : " ", i,
                     i < device.size() ? device[i].point.x : 0,
                     i < device.size() ? device[i].point.y : 0,
                     i < device.size() ? device[i].area : 0,
                     i < cpu.size() ? cpu[i].point.x : 0,
                     i < cpu.size() ? cpu[i].point.y : 0,
                     i < cpu.size() ? cpu[i].area : 0);
    }
}

/** Emission order of findBiggestRiver()'s priority queue. */
static void sortCoarseCandidates(std::vector<Res> &list)
{
    std::ranges::sort(list, [](const Res &a, const Res &b) {
        if (a.area != b.area) return a.area > b.area;
        if (a.point.x != b.point.x) return a.point.x < b.point.x;
        return a.point.y < b.point.y;
    });
}

/**
 * Phase-1 driver.
 *
 * The pipeline is unchanged semantically; what changed is where the work runs
 * and the submission granularity:
 *
 *   device, one submission per batch of up to kGpuChunkBatch chunks
 *     cont grid -> dilation -> tile compaction -> weirdness grid
 *     -> 2D prefix sums -> annular sums -> candidate list
 *   CPU
 *     per-tile best, ClimateCoarse subsearches, overlap filtering
 *
 * When the device is not available (or refuses a batch) every chunk of that
 * batch runs the original CPU path, so the semantics stay identical.
 */
void findBiggestRiverParallelPool(
    ThreadSafeResults<Res> &globalResults,
    Generator *g,
    int startX, int startZ,
    int sx, int sz,
    int minArea,
    Progress *progress = nullptr,
    int numThreads = static_cast<int>(std::thread::hardware_concurrency()),
    int contPrefilterScaleOverride = -1,
    int phase1Pipeline = 1,
    int weirdnessGridScale = -1
)
{
    const int chunkSize = 4096 * 2;
    const int tile = SearchConfig::CANDIDATE_TILE_BLOCKS;
    const int overlap = tile;
    const int step = chunkSize - overlap;
    const int contScale = contPrefilterScaleOverride > 0
        ? contPrefilterScaleOverride
        : SearchConfig::PHASE1_CONT_PREFILTER_SCALE;
    const int subR = SearchConfig::SUBSEARCH_RADIUS_BLOCKS;
    const int subSz = SearchConfig::SUBSEARCH_SIZE_BLOCKS;
    const int weirdScale = weirdnessGridScale > 0
        ? weirdnessGridScale
        : SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;
    (void) phase1Pipeline; /* product always Cont+WeirdnessOnly; retained for bench CLI */
    std::atomic<int> completedChunks{0};

    /* Grid scale of the weirdness scan: findBiggestRiver<16> or <32>. */
    const int gridScale = weirdScale <= 16 ? 16 : SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;

    struct Rect
    {
        int x, z, w, h;
    };
    std::vector<Rect> rects;
    for (int x = 0; x < sx; x += step)
    {
        /* 超大范围下这里会构造数百万个批次；停止后不要再继续准备。 */
        if (progress && progress->try_stop.load())
            return;
        for (int z = 0; z < sz; z += step)
        {
            const int currentSx = std::min(chunkSize, sx - x);
            const int currentSz = std::min(chunkSize, sz - z);
            if (currentSx >= tile && currentSz >= tile)
                rects.push_back({ x, z, currentSx, currentSz });
        }
    }
    const int totalChunks = static_cast<int>(rects.size());

    if (progress)
        progress->total.store(totalChunks);

    /* Batching only groups consecutive chunks of identical size (the last
     * column/row of a window is smaller). */
    struct Batch
    {
        int first;
        int count;
    };
    const bool gpu = lowyd_gpu::enabled();
    std::vector<Batch> batches;
    for (int i = 0; i < totalChunks;)
    {
        if (progress && progress->try_stop.load())
            return;
        int n = 1;
        if (gpu)
        {
            while (n < kGpuChunkBatch && i + n < totalChunks
                   && rects[i + n].w == rects[i].w && rects[i + n].h == rects[i].h)
                n++;
        }
        batches.push_back({ i, n });
        i += n;
    }

    const uint64_t seedVal = g->seed;
    const int mcVer = g->mc > 0 ? g->mc : MC_26_1;

#ifdef LOWYD_VERBOSE
    auto startTime = std::chrono::high_resolution_clock::now();
#endif

    /* Declared last so that it joins before any captured object is destroyed. */
    ThreadPool pool(numThreads);
    for (const Batch &batch: batches)
    {
        /* 停止时不要再把剩余批次塞进队列：大批量范围下逐批 enqueue + 队列唤醒
         * 本身就要花几十秒，期间搜索线程无法发布完成状态，界面会一直停在“停止中”。 */
        if (progress && progress->try_stop.load())
            break;
        pool.enqueue([&, batch, totalChunks, seedVal, contScale, weirdScale, gridScale, mcVer]() {
            if (progress)
            {
                if (progress->try_stop.load()) return;
                while (progress->try_pause.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (progress->try_stop.load()) return;
                }
                progress->chunkInRunning.fetch_add(batch.count);
            }

            thread_local Generator tlsG;
            thread_local bool tlsInited = false;
            thread_local int tlsMc = -1;
            thread_local uint64_t tlsSeed = ~0ull;
            if (!tlsInited || tlsMc != mcVer)
            {
                setupGenerator(&tlsG, mcVer, FORCE_OCEAN_VARIANTS);
                tlsInited = true;
                tlsMc = mcVer;
                tlsSeed = ~0ull;
            }
            if (tlsSeed != seedVal)
            {
                applySeed(&tlsG, DIM_OVERWORLD, seedVal);
                tlsSeed = seedVal;
            }

            auto finishChunk = [&]() {
                int completed = completedChunks.fetch_add(1) + 1;
                if (progress)
                {
                    progress->current.store(completed);
                    progress->chunkInRunning.fetch_sub(1);
                }
#ifdef LOWYD_VERBOSE
                if (completed % 500 == 0)
                {
                    auto currentTime = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        currentTime - startTime).count();
                    double speed = static_cast<double>(completed) / elapsed * 1000;
                    std::cout << "Progress: " << completed << "/" << totalChunks
                            << " (" << int(completed * 100.0 / totalChunks)
                            << "%) - " << speed << " chunks/sec\n";
                }
#endif
            };

            /* ---------------------------------------------------------------
             * Device: candidate lists for the whole batch, one flush.
             * ------------------------------------------------------------- */
            std::vector<std::vector<lowyd_gpu::Candidate>> batchCandidates;
            bool batchOnDevice = false;
            if (lowyd_gpu::enabled())
            {
                lowyd_gpu::CoarseSpec spec{};
                spec.contScale = contScale;
                spec.gridScale = gridScale;
                spec.minArea = minArea;
                spec.f = 0.8;
                spec.contThreshold = SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD;
                spec.dilate = SearchConfig::PHASE1_CONT_PREFILTER_DILATE;
                spec.climate = false;
                if (gridScale <= 16)
                    fillRingSpec<16>(spec);
                else
                    fillRingSpec<SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE>(spec);

                std::vector<lowyd_gpu::CoarseSpec> specs((size_t) batch.count, spec);
                for (int k = 0; k < batch.count; k++)
                {
                    const Rect &rect = rects[batch.first + k];
                    specs[(size_t) k].startX = startX + rect.x;
                    specs[(size_t) k].startZ = startZ + rect.z;
                    specs[(size_t) k].chunkW = rect.w;
                    specs[(size_t) k].chunkH = rect.h;
                }
                batchCandidates.resize((size_t) batch.count);
                lowyd_bench::Scope benchGpu(lowyd_bench::STAGE_GPU);
                batchOnDevice = lowyd_gpu::evalChunkBatch(&tlsG, specs.data(), batch.count,
                                                          batchCandidates.data(), nullptr);
            }

            for (int k = 0; k < batch.count; k++)
            {
                if (progress && progress->try_stop.load())
                    return;

                const Rect &rect = rects[batch.first + k];
                const int csx = startX + rect.x;
                const int csz = startZ + rect.z;
                const int currentSx = rect.w;
                const int currentSz = rect.h;

                if (lowyd_bench::enabled())
                {
                    lowyd_bench::Counters &bench = lowyd_bench::counters();
                    lowyd_bench::add(bench.chunks, 1);
                    /* C2/C1 diagnostic: does this window land on the A-table lattice? */
                    const OctaveACache *cache = getActiveOctaveACache();
                    if (cache && cache->ready && contScale > 0)
                    {
                        const int nx0 = (csx + contScale / 2) / 4;
                        const int nz0 = (csz + contScale / 2) / 4;
                        if (probeClimate(cache->contA[0], nx0, nz0).onLattice)
                            lowyd_bench::add(bench.latticeChunks, 1);
                    }
                }

                std::vector<Res> blockResultsCoarse;
                if (batchOnDevice)
                {
                    const std::vector<lowyd_gpu::Candidate> &list = batchCandidates[(size_t) k];
                    blockResultsCoarse.reserve(list.size());
                    for (const auto &candidate: list)
                        blockResultsCoarse.emplace_back(Point{ candidate.x, candidate.z },
                                                        candidate.area, 0, 0);
                    sortCoarseCandidates(blockResultsCoarse);
                    if (lowyd_bench::enabled())
                    {
                        lowyd_bench::add(lowyd_bench::counters().weirdCells,
                                         (uint64_t) ((size_t) (currentSx / gridScale) * (size_t) (currentSz / gridScale)));
                        lowyd_bench::add(lowyd_bench::counters().ringCandidates,
                                         (uint64_t) blockResultsCoarse.size());
                    }
                    if (coarseDebugEnabled())
                    {
                        /* Regression aid: the CPU ring scan for the same chunk. */
                        std::vector<uint8_t> debugMask = buildDilatedContPrefilterMask(
                            &tlsG, csx, csz, currentSx, currentSz, contScale,
                            SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD,
                            SearchConfig::PHASE1_CONT_PREFILTER_DILATE);
                        std::vector<Res> cpuList;
                        if (gridScale <= 16)
                            cpuList = findBiggestRiver<16>(&tlsG, csx, csz, currentSx,
                                                           currentSz, minArea, 0.8,
                                                           FilterMode::WeirdnessOnly, 0.7f,
                                                           &debugMask, contScale);
                        else
                            cpuList = findBiggestRiver<SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE>(
                                &tlsG, csx, csz, currentSx, currentSz, minArea, 0.8,
                                FilterMode::WeirdnessOnly, 0.7f, &debugMask, contScale);
                        reportCoarseDiff(blockResultsCoarse, cpuList, csx, csz);
                        if (blockResultsCoarse.size() != cpuList.size()
                            || (!blockResultsCoarse.empty()
                                && blockResultsCoarse[0].area != cpuList[0].area))
                        {
                            /* Also compare the 0/1 cell grid the ring runs on. */
                            const int W = currentSx / gridScale;
                            const int H = currentSz / gridScale;
                            const int cW = currentSx / contScale;
                            std::vector<uint8_t> gpuMask;
                            std::vector<uint8_t> gpuGrid;
                            if (lowyd_gpu::evalChunk(&tlsG, csx, csz, currentSx, currentSz,
                                                     contScale, gridScale,
                                                     SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD,
                                                     SearchConfig::PHASE1_CONT_PREFILTER_DILATE,
                                                     gpuMask, gpuGrid, nullptr)
                                && gpuGrid.size() == (size_t) W * H)
                            {
                                size_t gridDiff = 0;
                                int firstX = -1;
                                int firstZ = -1;
                                for (int z = 0; z < H; z++)
                                {
                                    for (int x = 0; x < W; x++)
                                    {
                                        const int bx = csx + x * gridScale + gridScale / 2;
                                        const int bz = csz + z * gridScale + gridScale / 2;
                                        const int tx = (bx - csx) / contScale;
                                        const int tz = (bz - csz) / contScale;
                                        const bool allow = tx >= 0 && tz >= 0 && tx < cW
                                            && debugMask[(size_t) tz * cW + tx] != 0;
                                        const int cpuValue = allow && passCaveWeirdness(
                                            &tlsG.bn, bx / 4, bz / 4, COARSE_SAMPLE_FLAGS) ? 1 : 0;
                                        if (cpuValue != (int) gpuGrid[(size_t) z * W + x])
                                        {
                                            if (!gridDiff)
                                            {
                                                firstX = x;
                                                firstZ = z;
                                            }
                                            gridDiff++;
                                        }
                                    }
                                }
                                std::fprintf(stderr, "[coarse]   grid diff=%zu first=(%d,%d)\n",
                                             gridDiff, firstX, firstZ);
                            }
                        }
                    }
                }
                else
                {
                    /* CPU path: cont mask + full weirdness ring scan. */
                    std::vector<uint8_t> contMask;
                    {
                        lowyd_bench::Scope benchScope(lowyd_bench::STAGE_CONT_MASK);
                        contMask = buildDilatedContPrefilterMask(
                            &tlsG, csx, csz, currentSx, currentSz,
                            contScale,
                            SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD,
                            SearchConfig::PHASE1_CONT_PREFILTER_DILATE);
                    }
                    if (lowyd_bench::enabled())
                    {
                        lowyd_bench::Counters &bench = lowyd_bench::counters();
                        lowyd_bench::add(bench.contTiles, (uint64_t) contMask.size());
                        lowyd_bench::add(bench.contTilesPass, (uint64_t) std::count_if(
                            contMask.begin(), contMask.end(), [](uint8_t v) { return v != 0; }));
                    }
                    if (std::none_of(contMask.begin(), contMask.end(),
                                     [](uint8_t v) { return v != 0; }))
                    {
                        finishChunk();
                        continue;
                    }
                    {
                        lowyd_bench::Scope benchScope(lowyd_bench::STAGE_WEIRD_RING);
                        if (gridScale <= 16)
                        {
                            blockResultsCoarse = findBiggestRiver<16>(
                                &tlsG, csx, csz, currentSx, currentSz,
                                minArea, 0.8, FilterMode::WeirdnessOnly,
                                0.7f, &contMask, contScale);
                        }
                        else
                        {
                            blockResultsCoarse =
                                findBiggestRiver<SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE>(
                                    &tlsG, csx, csz, currentSx, currentSz,
                                    minArea, 0.8, FilterMode::WeirdnessOnly,
                                    0.7f, &contMask, contScale);
                        }
                    }
                }

                const int bx = currentSx / tile + 2;
                const int bz = currentSz / tile + 2;
                std::vector<Res> flags((size_t) bx * bz);
                for (const auto &it: blockResultsCoarse)
                {
                    int x2 = (it.point.x - csx) / tile;
                    int z2 = (it.point.y - csz) / tile;
                    if (x2 >= 0 && x2 < bx && z2 >= 0 && z2 < bz)
                    {
                        auto &itf = flags[(size_t) x2 + (size_t) bx * z2];
                        if (itf.area < it.area) itf = it;
                    }
                }

                std::vector<Res> pqX16;
                for (auto &kv: flags)
                    if (kv.area > 0) pqX16.push_back(kv);
                std::ranges::sort(pqX16, [](const Res &a, const Res &b) { return a.area > b.area; });

                std::unordered_map<uint64_t, Res> blockResultsX4;
                int max = 0;
                for (auto &res: pqX16)
                {
                    std::vector<Res> subResults;
                    {
                        lowyd_bench::Scope benchScope(lowyd_bench::STAGE_CLIMATE_COARSE);
                        bool found = false;
                        if (lowyd_gpu::enabled())
                        {
                            lowyd_gpu::CoarseSpec spec{};
                            spec.startX = res.point.x - subR;
                            spec.startZ = res.point.y - subR;
                            spec.chunkW = subSz;
                            spec.chunkH = subSz;
                            spec.contScale = SearchConfig::PHASE1_CLIMATE_GRID_SCALE;
                            spec.gridScale = SearchConfig::PHASE1_CLIMATE_GRID_SCALE;
                            spec.minArea = minArea;
                            spec.f = 1.0;
                            spec.contThreshold = CONT_PARTIAL_FINAL_THRESHOLD;
                            spec.climate = true;
                            fillRingSpec<SearchConfig::PHASE1_CLIMATE_GRID_SCALE>(spec);
                            lowyd_gpu::Candidate candidate;
                            if (lowyd_gpu::evalClimate(&tlsG, spec, &candidate, nullptr))
                            {
                                found = true;
                                if (candidate.area > 0)
                                {
                                    subResults.emplace_back(
                                        Point{ candidate.x, candidate.z }, candidate.area, 0, 0);
                                }
                            }
                        }
                        if (!found)
                        {
                            subResults = findBiggestRiver<SearchConfig::PHASE1_CLIMATE_GRID_SCALE>(
                                &tlsG,
                                res.point.x - subR, res.point.y - subR,
                                subSz, subSz,
                                minArea, 1.0, FilterMode::ClimateCoarse);
                        }
                        if (lowyd_bench::enabled())
                            lowyd_bench::add(lowyd_bench::counters().coarseSubsearches, 1);
                    }

                    if (subResults.empty()) break;
                    if (subResults[0].area < max * 0.9) break;

                    auto &r = subResults[0];
                    uint64_t key = ((uint64_t) (uint32_t) r.point.x << 32) | (uint32_t) r.point.y;
                    auto it = blockResultsX4.find(key);
                    if (it == blockResultsX4.end())
                        blockResultsX4.emplace(key, r);
                    else if (it->second.area < r.area)
                        it->second = r;

                    if (subResults[0].area > max) max = subResults[0].area;
                }

                std::vector<Res> filteredResults;
                for (const auto &kv: blockResultsX4)
                {
                    const auto &result = kv.second;
                    int relX = result.point.x - csx;
                    int relZ = result.point.y - csz;
                    if (relX > overlap / 2 && relX < currentSx - overlap / 2 &&
                        relZ > overlap / 2 && relZ < currentSz - overlap / 2 &&
                        result.area > 0)
                    {
                        filteredResults.push_back(result);
                    }
                }
                std::ranges::sort(filteredResults, [](const Res &a, const Res &b) { return a.area > b.area; });
                if (!filteredResults.empty())
                    globalResults.addResults(filteredResults);

                finishChunk();
            }
        });
    }
}


#if defined(CONT_PREFILTER_BENCH) && defined(LOWYD_VERBOSE)

#include "cubiomes/noise.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static constexpr double kBenchDblF = 337.0 / 331.0;
static constexpr double kRingFullArea = 49640.0;

static double benchOctA(const DoublePerlinNoise *dpn, int idx, double x, double z, int *perlins)
{
    if (idx < 0 || idx >= dpn->octA.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octA.octaves + idx;
    const double lf = p->lacunarity;
    (*perlins)++;
    return p->amplitude * samplePerlin(p, maintainPrecision(x * lf), 0.0, maintainPrecision(z * lf), 0, 0);
}

static double benchOctB(const DoublePerlinNoise *dpn, int idx, double x, double z, int *perlins)
{
    if (idx < 0 || idx >= dpn->octB.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octB.octaves + idx;
    const double lf = p->lacunarity;
    (*perlins)++;
    return p->amplitude * samplePerlin(p,
        maintainPrecision(x * lf * kBenchDblF), 0.0, maintainPrecision(z * lf * kBenchDblF), 0, 0);
}

/** Instrumented Cont matching legacy mid-thresholds; counts Perlin calls (max 10). */
struct ContFunnelStats {
    long long reached[9]{};   /* gate 1..8 */
    long long rejected[9]{};
    long long accepted = 0;
    long long samples = 0;
    long long perlin_sum = 0;
};

static bool contFunnelSample(const BiomeNoise *bn, int bx, int bz, double thr, ContFunnelStats &st)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_CONTINENTALNESS];
    const double amp = dpn->amplitude;
    double sum = 0.0;
    int perlins = 0;
    st.samples++;

    auto gate = [&](int g, double midThr, bool isFinal) -> int {
        /* return: -1 reject, 0 continue, 1 accept */
        st.reached[g]++;
        if (isFinal)
        {
            if (sum > thr)
            {
                st.accepted++;
                return 1;
            }
            st.rejected[g]++;
            return -1;
        }
        if (sum < midThr)
        {
            st.rejected[g]++;
            return -1;
        }
        return 0;
    };

    sum += amp * (benchOctA(dpn, 0, bx, bz, &perlins) + benchOctB(dpn, 0, bx, bz, &perlins));
    if (gate(1, -0.2, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 1, bx, bz, &perlins);
    if (gate(2, -0.1, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 1, bx, bz, &perlins);
    if (gate(3, 0.0, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 2, bx, bz, &perlins);
    if (gate(4, 0.13, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 2, bx, bz, &perlins);
    if (gate(5, 0.3, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 3, bx, bz, &perlins);
    if (gate(6, 0.37, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 3, bx, bz, &perlins);
    if (gate(7, 0.44, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * (benchOctA(dpn, 4, bx, bz, &perlins) + benchOctB(dpn, 4, bx, bz, &perlins));
    const int r = gate(8, 0.0, true);
    st.perlin_sum += perlins;
    return r > 0;
}

static void runContFunnel(Generator *g, int startX, int startZ, int sx, int sz,
                          int sampleScale, double thr, ContFunnelStats &st)
{
    for (int z = 0; z < sz; z += sampleScale)
    {
        const int worldZ = startZ + z + sampleScale / 2;
        for (int x = 0; x < sx; x += sampleScale)
        {
            const int worldX = startX + x + sampleScale / 2;
            contFunnelSample(&g->bn, worldX / 4, worldZ / 4, thr, st);
        }
    }
}

static void printContFunnel(const ContFunnelStats &st, int sampleScale, double thr,
                            double maskMs, double fillMs)
{
    std::printf("=== Cont octave funnel (scale=%d, thr=%.3f, N=%lld) ===\n",
                sampleScale, thr, (long long) st.samples);
    std::printf("gate  reached     reject     pass%%    cum_reject%%\n");
    long long cumRej = 0;
    for (int g = 1; g <= 8; g++)
    {
        const long long rch = st.reached[g];
        const long long rej = st.rejected[g];
        cumRej += rej;
        double passPct = 0.0;
        if (rch > 0)
        {
            if (g < 8)
                passPct = 100.0 * (double) (rch - rej) / (double) rch;
            else
                passPct = 100.0 * (double) st.accepted / (double) rch;
        }
        const double cumRejPct = st.samples > 0 ? 100.0 * (double) cumRej / (double) st.samples : 0.0;
        std::printf("%-4d  %-10lld  %-10lld  %6.2f    %6.2f\n",
                    g, (long long) rch, (long long) rej, passPct, cumRejPct);
    }
    const double avgOct = st.samples > 0 ? (double) st.perlin_sum / (double) st.samples : 0.0;
    const double contOnly = avgOct > 0.0 ? 10.0 / avgOct : 0.0;
    std::printf("avg_perlins = %.3f / 10  => Cont-only ~%.2fx vs full-10\n", avgOct, contOnly);
    const double total = maskMs + fillMs;
    const double estFull = total - maskMs + maskMs * (avgOct > 0 ? 10.0 / avgOct : 1.0);
    /* If early-exit Cont is what we use now, estimate time if Cont were full-10:
       T_full ≈ T_mask * (10/avg) + T_fill ; overall speedup of early-exit ≈ T_full / T_now */
    const double tIfFull10 = (avgOct > 0.0)
        ? (maskMs * (10.0 / avgOct) + fillMs)
        : total;
    const double overall = tIfFull10 > 0.0 ? tIfFull10 / total : 1.0;
    std::printf("phase1 wall: mask=%.1fms fill+ring=%.1fms total=%.1fms\n", maskMs, fillMs, total);
    std::printf("est. phase1 if Cont full-10: %.1fms  => overall ~%.2fx from Cont early-exit\n",
                tIfFull10, overall);
    std::printf("accept_rate=%.3f%% (%lld/%lld)\n\n",
                st.samples ? 100.0 * (double) st.accepted / (double) st.samples : 0.0,
                (long long) st.accepted, (long long) st.samples);
    (void) estFull;
}

static uint64_t tileKey(int x, int z, int tile)
{
    const int tx = x >= 0 ? x / tile : (x - tile + 1) / tile;
    const int tz = z >= 0 ? z / tile : (z - tile + 1) / tile;
    return ((uint64_t) (uint32_t) tx << 32) | (uint32_t) tz;
}

static std::set<uint64_t> peakTiles(const std::vector<Res> &peaks, int tile)
{
    std::set<uint64_t> s;
    for (const auto &p: peaks)
        s.insert(tileKey(p.point.x, p.point.y, tile));
    return s;
}

static std::vector<Res> runPhase1(Generator *g, int startX, int startZ, int sx, int sz,
                                  int minArea, int threads, int contScale,
                                  double *outMaskMs, double *outFillMs,
                                  int phase1Pipeline = 1,
                                  int weirdnessGridScale = -1)
{
    if (outMaskMs)
        *outMaskMs = 0.0;

    ThreadSafeResults<Res> results;
    auto t2 = std::chrono::high_resolution_clock::now();
    findBiggestRiverParallelPool(results, g, startX, startZ, sx, sz, minArea,
                                 nullptr, threads, contScale, phase1Pipeline, weirdnessGridScale);
    auto t3 = std::chrono::high_resolution_clock::now();
    if (outFillMs)
        *outFillMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto all = results.getAllResults();
    std::ranges::sort(all, [](const Res &a, const Res &b) { return a.area > b.area; });
    return all;
}

int main(int argc, char **argv)
{
    int64_t seed = -8180004378910677489LL;
    int cx = 0, cz = 0, half = 8192;
    int threads = (int) std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    bool cmp64Only = false; /* skip funnel + scale16; 64 = reference, report 128 miss */
    bool cmpWeird = false;  /* Weird@16 legacy vs Cont+Weird@128 joint */

    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--cx") && i + 1 < argc)
            cx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cz") && i + 1 < argc)
            cz = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--half") && i + 1 < argc)
            half = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cmp64"))
            cmp64Only = true;
        else if (!std::strcmp(argv[i], "--cmp-weird"))
            cmpWeird = true;
    }

    const int startX = cx - half;
    const int startZ = cz - half;
    const int sx = half * 2;
    const int sz = half * 2;
    const int minArea = (int) (kRingFullArea * 0.70);
    const int tile = SearchConfig::CANDIDATE_TILE_BLOCKS;
    const double thr = SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD;

    std::printf("Cont prefilter bench%s%s\n",
                cmp64Only ? " (--cmp64)" : "",
                cmpWeird ? " (--cmp-weird: Weird@16 vs joint@128)" : "");
    std::printf("seed=%lld window=[%d,%d)x[%d,%d) half=%d side=%d threads=%d minArea=%d (70%% of %.1f)\n\n",
                (long long) seed, startX, startX + sx, startZ, startZ + sz,
                half, half * 2, threads, minArea, kRingFullArea);

    Generator g;
    setupGenerator(&g, MC_1_21_3, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, (uint64_t) seed);

    if (cmpWeird)
    {
        /* Cont@128 + WeirdnessOnly@{16,32,64,128} + Climate@4; oracle = Weird@16 final �?0% results. */
        const int weirdScales[] = {16, 32, 64, 128};
        struct Run {
            int weird;
            std::vector<Res> peaks;
            double ms;
        } runs[4]{};

        for (int i = 0; i < 4; i++)
        {
            runs[i].weird = weirdScales[i];
            std::printf("Running Cont@128 + WeirdnessOnly@%d + Climate@4 (final �?0%%)...\n",
                        runs[i].weird);
            runs[i].peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 128,
                                      nullptr, &runs[i].ms, /*pipeline=*/1, runs[i].weird);
            std::printf("  final_results=%zu time=%.1fms (%.2fs)\n",
                        runs[i].peaks.size(), runs[i].ms, runs[i].ms / 1000.0);
        }

        const auto ref = peakTiles(runs[0].peaks, tile);
        std::printf("\n=== Final �?0%% results: Weird@16 oracle vs @32/@64/@128 (half=%d, tile=%d) ===\n",
                    half, tile);
        std::printf("oracle Weird@16: %zu final peak-tiles  time=%.1fms\n",
                    ref.size(), runs[0].ms);

        for (int i = 1; i < 4; i++)
        {
            const auto got = peakTiles(runs[i].peaks, tile);
            int miss = 0;
            for (uint64_t k: ref)
                if (!got.count(k))
                    miss++;
            int extra = 0;
            for (uint64_t k: got)
                if (!ref.count(k))
                    extra++;
            const double missPct = ref.empty() ? 0.0 : 100.0 * miss / (double) ref.size();
            const double speedup = runs[i].ms > 0.0 ? runs[0].ms / runs[i].ms : 0.0;
            std::printf("Weird@%d: final=%zu  miss=%d/%zu (%.2f%%) ~%.1f/100  extra=%d  "
                        "time=%.1fms  speedup=%.2fx\n",
                        runs[i].weird, got.size(), miss, ref.size(), missPct, missPct,
                        extra, runs[i].ms, speedup);
        }
        return 0;
    }

    if (!cmp64Only)
    {
        ContFunnelStats st{};
        double maskMs = 0, fillMs = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        runContFunnel(&g, startX, startZ, sx, sz, 64, thr, st);
        auto t1 = std::chrono::high_resolution_clock::now();
        maskMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ThreadSafeResults<Res> tmp;
        auto t2 = std::chrono::high_resolution_clock::now();
        findBiggestRiverParallelPool(tmp, &g, startX, startZ, sx, sz, minArea, nullptr, threads, 64);
        auto t3 = std::chrono::high_resolution_clock::now();
        fillMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        printContFunnel(st, 64, thr, maskMs, fillMs);
    }

    struct ScaleRun {
        int scale;
        std::vector<Res> peaks;
        double maskMs;
        double fillMs;
    };

    if (cmp64Only)
    {
        ScaleRun run64{}, run128{};
        run64.scale = 64;
        run128.scale = 128;

        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=64 (reference)...\n");
        run64.peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 64,
                                &run64.maskMs, &run64.fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    run64.peaks.size(), run64.maskMs, run64.fillMs,
                    run64.maskMs + run64.fillMs);

        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=128...\n");
        run128.peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 128,
                                 &run128.maskMs, &run128.fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    run128.peaks.size(), run128.maskMs, run128.fillMs,
                    run128.maskMs + run128.fillMs);

        const auto ref = peakTiles(run64.peaks, tile);
        const auto got = peakTiles(run128.peaks, tile);
        int miss = 0;
        std::vector<uint64_t> missed;
        for (uint64_t k: ref)
        {
            if (!got.count(k))
            {
                miss++;
                missed.push_back(k);
            }
        }
        int extra = 0;
        for (uint64_t k: got)
            if (!ref.count(k))
                extra++;

        const double missPct = ref.empty() ? 0.0 : 100.0 * miss / (double) ref.size();
        std::printf("\n=== Cont prefilter: 128 miss vs 64 reference (minArea=70%%, tile=%d) ===\n", tile);
        std::printf("ref(scale=64): %zu peak-tiles\n", ref.size());
        std::printf("scale=128: %zu peak-tiles\n", got.size());
        std::printf("128 misses vs 64: %d/%zu (%.2f%%)  => ~%.1f per 100 candidates\n",
                    miss, ref.size(), missPct, missPct);
        std::printf("128 extras not in 64: %d\n", extra);
        std::printf("time 64=%.1fms  128=%.1fms\n",
                    run64.maskMs + run64.fillMs, run128.maskMs + run128.fillMs);
        const int show = (int) std::min<size_t>(missed.size(), 16);
        for (int j = 0; j < show; j++)
        {
            const int tx = (int) (missed[j] >> 32);
            const int tz = (int) (uint32_t) missed[j];
            std::printf("  missed tile (%d,%d) ~ block (%d,%d)\n",
                        tx, tz, tx * tile, tz * tile);
        }
        if ((int) missed.size() > show)
            std::printf("  ... +%d more\n", (int) missed.size() - show);
        return 0;
    }

    const int scales[] = {16, 64, 128};
    ScaleRun runs[3]{};
    for (int i = 0; i < 3; i++)
    {
        runs[i].scale = scales[i];
        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=%d...\n", scales[i]);
        runs[i].peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, scales[i],
                                  &runs[i].maskMs, &runs[i].fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    runs[i].peaks.size(), runs[i].maskMs, runs[i].fillMs,
                    runs[i].maskMs + runs[i].fillMs);
    }

    const auto oracle = peakTiles(runs[0].peaks, tile);
    std::printf("\n=== Cont prefilter miss (minArea=70%%, tile=%d) ===\n", tile);
    std::printf("oracle(scale=16): %zu peak-tiles\n", oracle.size());

    for (int i = 1; i < 3; i++)
    {
        const auto got = peakTiles(runs[i].peaks, tile);
        int miss = 0;
        std::vector<uint64_t> missed;
        for (uint64_t k: oracle)
        {
            if (!got.count(k))
            {
                miss++;
                missed.push_back(k);
            }
        }
        const double missPct = oracle.empty() ? 0.0 : 100.0 * miss / (double) oracle.size();
        std::printf("scale=%d: miss %d/%zu (%.2f%%)  time=%.1fms\n",
                    runs[i].scale, miss, oracle.size(), missPct,
                    runs[i].maskMs + runs[i].fillMs);
        const int show = (int) std::min<size_t>(missed.size(), 8);
        for (int j = 0; j < show; j++)
        {
            const int tx = (int) (missed[j] >> 32);
            const int tz = (int) (uint32_t) missed[j];
            std::printf("  missed tile (%d,%d) ~ block (%d,%d)\n",
                        tx, tz, tx * tile, tz * tile);
        }
        if ((int) missed.size() > show)
            std::printf("  ... +%d more\n", (int) missed.size() - show);
    }

    return 0;
}

#elif !defined(DRIPSTONECAVE_FINDER_JNI_LIB)
int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    int64_t seed = -8180004378910677489;
    int px = 0, pz = 0, d = 4096;
    std::cout << "seed: ";
    std::cin >> seed;
    std::cout << "center_x: ";
    std::cin >> px;
    std::cout << "center_z: ";
    std::cin >> pz;
    std::cout << "r: ";
    std::cin >> d;

    int startX = px - d;
    int startZ = pz - d;
    int xRange = 2 * d;
    int zRange = 2 * d;
    int minArea = 40000;
    const char *outFile = "out1.txt";
    int outLimit = 1000;

    Generator g;
    setupGenerator(&g, MC_1_21_3, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, seed);

    ThreadSafeResults<Res> globalResults;
    findBiggestRiverParallelPool(globalResults, &g, startX, startZ, xRange, zRange, minArea, nullptr);

    auto res = globalResults.getAllResults();
    std::ranges::sort(res, [](const Res &a, const Res &b) { return a.area > b.area; });

    int max = 0;
    std::vector<Res> finallyResults;
    for (const auto &it: res)
    {
        auto temp = findBiggestRiver<1>(
            &g,
            it.point.x - SearchConfig::REFINE_HALF_WINDOW,
            it.point.y - SearchConfig::REFINE_HALF_WINDOW,
            SearchConfig::REFINE_WINDOW_BLOCKS, SearchConfig::REFINE_WINDOW_BLOCKS,
            1, 1.0, FilterMode::PreciseBiome);
        if (!temp.empty() && temp[0].area > max * 0.90)
        {
            finallyResults.push_back(temp[0]);
            if (temp[0].area > max) max = temp[0].area;
        } else
        {
            break;
        }
    }

    FILE *fp = fopen(outFile, "w");
    if (fp)
    {
        fprintf(fp, "Cave Analysis Results (Seed: %lld)\n", (long long) seed);
        fprintf(fp, "Search Area: X=%d to %d, Z=%d to %d\n",
                startX, startX + xRange, startZ, startZ + zRange);
        fprintf(fp, "========================================\n");
    }

    int count = 0;
    for (const auto &it: finallyResults)
    {
        std::cout << "x:" << it.point.x << " y:" << it.point.y
                << "  Area:" << it.area << " cave:" << it.caveArea
                << " river:" << it.riverArea << std::endl;
        if (fp)
        {
            fprintf(fp, "x:%d y:%d  Area:%d cave:%d river:%d\n",
                    it.point.x, it.point.y, it.area, it.caveArea, it.riverArea);
        }
        count++;
        if (count > outLimit) break;
    }
    if (fp)
    {
        fprintf(fp, "\nTotal results: %d\n", count);
        fclose(fp);
        std::cout << "\nResults saved to: " << outFile << std::endl;
    }
    return 0;
}
#endif
