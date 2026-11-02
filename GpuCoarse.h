//
// OpenCL coarse-noise backend for the phase-1 prefilter stages.
//
// The three phase-1 stages evaluate the same kind of object: a coarse grid of
// climate cells whose 0/1 value is a threshold test on the double-perlin
// climate noises (continentalness, weirdness, erosion). This module moves the
// *noise evaluation* of those grids to a GPU and leaves the ring/prefix/peak
// logic in the existing CPU pipeline, so the search semantics (and the hit
// list) stay identical.
//
// Design constraints, from docs/GPU_ACCEL_PLAN.md:
//   - no build-time OpenCL dependency: the DLL is loaded at run time and every
//     entry point is resolved with GetProcAddress (MSYS2 and Gradle/JNI builds
//     stay unchanged);
//   - every platform / device is enumerated and filtered by capability;
//   - the kernel source is generated as a string with #define constants;
//   - coordinates/phases are double, in-cell coordinates / gradients /
//     interpolation / accumulation are float (see the plan's numeric contract);
//   - failures are returned as reason strings ("no-opencl", "no-device",
//     "build-failed:<log>") and always fall back to the CPU path.
//
// The algorithm is implemented here from the semantics of cubiomes'
// samplePerlin()/sampleDoublePerlin() and this repository's PerlinTable.h.
//
#ifndef LOWYD_GPU_COARSE_H
#define LOWYD_GPU_COARSE_H

#include <cstdint>
#include <string>
#include <vector>

struct Generator;

namespace lowyd_gpu
{

/** Which backend the caller wants. Auto picks the GPU only above the gate. */
enum class Backend
{
    Auto = 0,
    Cpu = 1,
    Gpu = 2
};

struct Info
{
    bool probed = false;
    bool usable = false;
    /** Human readable device description ("<name> (<vendor>, <version>)"). */
    std::string device;
    /** Failure reason when usable == false, one of the plan's reason strings. */
    std::string reason;
    int maxWorkGroup = 0;
    int workGroupX = 16;
    int workGroupY = 16;
    /** Number of host->device grid submissions (one per kernel chain). */
    uint64_t submissions = 0;
    /** Coarse cells whose value the device computed. */
    uint64_t cells = 0;
    /** Submissions the device refused and the CPU had to redo. */
    uint64_t fallbacks = 0;
    /** Host time spent queueing commands (all threads) / waiting for the flush. */
    uint64_t hostEnqueueNs = 0;
    uint64_t hostWaitNs = 0;
};

/** Selects the backend; call before a search starts. */
void setBackend(Backend backend);
Backend backend();
/** True while a search has an activated, working device. */
bool enabled();

/** "auto"/"cpu"/"gpu" (case insensitive); false for anything else. */
bool parseBackend(const char* text, Backend* out);
const char* backendName(Backend backend);

/**
 * Probes the device (once) and uploads the seed's noise parameters.
 * `allow` == false keeps the GPU path off without touching OpenCL at all.
 * Returns true when the GPU path may be used for this search.
 */
bool activate(const Generator* g, bool allow);
/** Ends the GPU section of a search (keeps the device open for the next one). */
void deactivate();

/** Device information; only valid after a probe. */
const Info& info();

/**
 * G2 microbenchmark: evaluates a `side` x `side` weirdness grid a few times and
 * returns the sustained device throughput in coarse cells per second (0 when the
 * backend is unavailable). Also reports the single-submission latency.
 */
double benchmarkGrid(int side, double* latencyMicros);

/** Grid kinds the kernels can evaluate. */
enum GridMode
{
    GRID_CONT = 0,    /* passContinentalnessPartialThr(bx, bz, thr) */
    GRID_WEIRD = 1,   /* passCaveWeirdness(bx, bz, COARSE_SAMPLE_FLAGS) */
    GRID_CLIMATE = 2  /* passCoarseCaveCell(bx, bz, COARSE_SAMPLE_FLAGS) */
};

/**
 * Evaluates one coarse grid.
 *
 * Cell (x, y) is centred on block (startX + x*scale + scale/2,
 * startZ + y*scale + scale/2) and is queried at climate coordinates
 * (blockX / 4, blockZ / 4), exactly like the CPU loops.
 *
 * `mask` (optional) reproduces contPrefilterAllows(): when non-null a cell is
 * only evaluated when mask[tz*maskW + tx] != 0, with
 * tx = (blockX - maskStartX) / maskScale.
 *
 * `out` is resized to W*H and filled with 0/1. Returns false when the GPU path
 * is not available or the device failed; the caller must then run the CPU path.
 */
bool evalGrid(const Generator* g,
              int startX, int startZ, int W, int H, int scale,
              GridMode mode, double contThreshold,
              const std::uint8_t* mask, int maskStartX, int maskStartZ,
              int maskScale, int maskW, int maskH,
              std::vector<std::uint8_t>& out, std::string* reason);

/**
 * The full phase-1 chunk in one submission: the continentalness prefilter mask
 * (dilated by `dilate` tiles, exactly like buildDilatedContPrefilterMask) and
 * the weirdness grid restricted to that mask (exactly like the per-cell
 * contPrefilterAllows() check of findBiggestRiver()).
 *
 * `contMask` receives (chunkW/contScale)*(chunkH/contScale) bytes,
 * `weirdGrid` receives (chunkW/weirdScale)*(chunkH/weirdScale) bytes.
 * Returns false when the GPU path is unavailable; nothing is written then.
 */
bool evalChunk(const Generator* g,
               int startX, int startZ, int chunkW, int chunkH,
               int contScale, int weirdScale, double contThreshold, int dilate,
               std::vector<std::uint8_t>& contMask,
               std::vector<std::uint8_t>& weirdGrid,
               std::string* reason);

// ---------------------------------------------------------------------------
// Ring stage on the device.
//
// The kernels above produce the 0/1 cell grid; findBiggestRiver() then does
// prefix sums, annular sums around every candidate centre and the max-area
// filter on the CPU. Those three steps are integer work, so the device can do
// them bit-exactly; the result is a handful of candidates instead of a
// 64 KiB grid per chunk (and no CPU ring loop at all).
// ---------------------------------------------------------------------------

/** One ring candidate in world block coordinates. */
struct Candidate
{
    int x = 0;
    int z = 0;
    int area = 0;
};

/**
 * One window the ring stage is run for.
 *
 * `climate` == false: phase-1 chunk (Cont -> dilation -> Weirdness -> ring).
 * `climate` == true:  ClimateCoarse window (climate grid -> ring), where only
 *                     the single PQ-top candidate is returned, matching
 *                     findBiggestRiver(..., f = 1.0) exactly.
 *
 * The ring geometry is getRingMask<scale>() flattened to (dz, out, in) triples;
 * every window of one call shares it.
 */
struct CoarseSpec
{
    int startX = 0;
    int startZ = 0;
    int chunkW = 0;
    int chunkH = 0;
    int contScale = 0;
    int gridScale = 0;
    int minArea = 0;
    double f = 0.8;
    double contThreshold = 0.525; /* continentalness prefilter / pass threshold */
    int dilate = 1;               /* Chebyshev dilation of the cont mask, in tiles */
    bool climate = false;
    int ringOut = 0;
    int ringRows = 0;
    const int* ring = nullptr; /* 3 ints per row */
};

/**
 * Evaluates a batch of phase-1 chunk windows in one submission (one flush for
 * the whole batch). outLists[i] receives the candidates of window i, already
 * filtered with the CPU rule `area >= max(minArea, maxArea * f)`.
 * Returns false when the GPU path is unavailable; the caller then runs the CPU
 * path for every window of the batch.
 */
bool evalChunkBatch(const Generator* g,
                    const CoarseSpec* specs, int count,
                    std::vector<Candidate>* outLists,
                    std::string* reason);

/**
 * ClimateCoarse window: returns true and fills `out` with the PQ-top candidate
 * when the window's maximum area passes `miniArea`, false otherwise (and on
 * device failure).
 */
bool evalClimate(const Generator* g, const CoarseSpec& spec, Candidate* out,
                 std::string* reason);

} // namespace lowyd_gpu

#endif // LOWYD_GPU_COARSE_H
