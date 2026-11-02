//
// OpenCL coarse-noise backend.  See GpuCoarse.h for the contract.
//
// Everything below is plain C++ with a run-time resolved OpenCL library: the
// repository keeps no build-time OpenCL dependency (no headers, no import
// library), which is what lets both the MSYS2 scripts and the Gradle/JNI build
// stay untouched.
//

#include "GpuCoarse.h"

#include "cubiomes/biomes.h"
#include "cubiomes/generator.h"
#include "cubiomes/noise.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// Minimal OpenCL 1.2 surface, declared locally so the build needs no SDK.
// ---------------------------------------------------------------------------
namespace
{

typedef int8_t cl_char;
typedef uint8_t cl_uchar;
typedef int16_t cl_short;
typedef uint16_t cl_ushort;
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef int64_t cl_long;
typedef uint64_t cl_ulong;
typedef uint32_t cl_bool;
typedef uint64_t cl_bitfield;
typedef intptr_t cl_context_properties;

struct _cl_platform_id { int unused; };
struct _cl_device_id { int unused; };
struct _cl_context { int unused; };
struct _cl_command_queue { int unused; };
struct _cl_mem { int unused; };
struct _cl_program { int unused; };
struct _cl_kernel { int unused; };
struct _cl_event { int unused; };

typedef _cl_platform_id *cl_platform_id;
typedef _cl_device_id *cl_device_id;
typedef _cl_context *cl_context;
typedef _cl_command_queue *cl_command_queue;
typedef _cl_mem *cl_mem;
typedef _cl_program *cl_program;
typedef _cl_kernel *cl_kernel;
typedef _cl_event *cl_event;

typedef cl_uint cl_device_type; /* really cl_bitfield; see deviceQualified() */
typedef cl_uint cl_platform_info;
typedef cl_uint cl_device_info;
typedef cl_uint cl_program_build_info;
typedef cl_uint cl_kernel_work_group_info;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_int CL_DEVICE_NOT_FOUND = -1;
constexpr cl_int CL_MEM_OBJECT_ALLOCATION_FAILURE = -4;

constexpr cl_uint CL_PLATFORM_NAME = 0x0902;
constexpr cl_uint CL_PLATFORM_VENDOR = 0x0903;
constexpr cl_uint CL_PLATFORM_VERSION = 0x0901;

constexpr cl_uint CL_DEVICE_TYPE = 0x1000;
constexpr cl_uint CL_DEVICE_TYPE_CPU = 0x00000002;
constexpr cl_uint CL_DEVICE_TYPE_GPU = 0x00000004;
constexpr cl_uint CL_DEVICE_TYPE_ACCELERATOR = 0x00000008;
constexpr cl_uint CL_DEVICE_TYPE_ALL = 0xFFFFFFFF;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_DEVICE_VENDOR = 0x102C;
constexpr cl_uint CL_DEVICE_VERSION = 0x102F;
constexpr cl_uint CL_DEVICE_MAX_COMPUTE_UNITS = 0x1002;
constexpr cl_uint CL_DEVICE_MAX_WORK_GROUP_SIZE = 0x1004;
constexpr cl_uint CL_DEVICE_MAX_WORK_ITEM_SIZES = 0x1005;
constexpr cl_uint CL_DEVICE_LOCAL_MEM_SIZE = 0x1023;
constexpr cl_uint CL_DEVICE_MAX_CLOCK_FREQUENCY = 0x100C;
constexpr cl_uint CL_DEVICE_DOUBLE_FP_CONFIG = 0x1032;
constexpr cl_uint CL_DEVICE_MAX_MEM_ALLOC_SIZE = 0x1010;

constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;

constexpr cl_bitfield CL_MEM_READ_WRITE = 0x1;
constexpr cl_bitfield CL_MEM_WRITE_ONLY = 0x2;
constexpr cl_bitfield CL_MEM_READ_ONLY = 0x4;
constexpr cl_bitfield CL_MEM_COPY_HOST_PTR = 0x20;

constexpr cl_bool CL_TRUE = 1;
constexpr cl_bool CL_FALSE = 0;

struct ClApi
{
    cl_int (*clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
    cl_int (*clGetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void *, size_t *);
    cl_int (*clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
    cl_int (*clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void *, size_t *);
    cl_context (*clCreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *,
                                  void *, void *, cl_int *);
    cl_command_queue (*clCreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int *);
    cl_program (*clCreateProgramWithSource)(cl_context, cl_uint, const char **, const size_t *,
                                            cl_int *);
    cl_int (*clBuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *, void *, void *);
    cl_int (*clGetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info, size_t,
                                    void *, size_t *);
    cl_kernel (*clCreateKernel)(cl_program, const char *, cl_int *);
    cl_int (*clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
    cl_mem (*clCreateBuffer)(cl_context, cl_bitfield, size_t, void *, cl_int *);
    cl_int (*clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                   const void *, cl_uint, const cl_event *, cl_event *);
    cl_int (*clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *,
                                  cl_uint, const cl_event *, cl_event *);
    cl_int (*clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t *,
                                     const size_t *, const size_t *, cl_uint, const cl_event *,
                                     cl_event *);
    cl_int (*clFinish)(cl_command_queue);
    cl_int (*clReleaseMemObject)(cl_mem);
    cl_int (*clReleaseKernel)(cl_kernel);
    cl_int (*clReleaseProgram)(cl_program);
    cl_int (*clReleaseCommandQueue)(cl_command_queue);
    cl_int (*clReleaseContext)(cl_context);
};

// ---------------------------------------------------------------------------
// Kernel source.
//
// The numeric contract is the plan's: coordinates and phases are double, the
// in-cell coordinates, gradients, interpolation and accumulation are float.
// The 16x3 gradient table is folded into four selects (see gradDot) instead of
// a memory lookup: rows 12..15 repeat rows 0/9/1/11, and rows 0..11 are
// "sign-selected pair sum with the a,b / a,c / b,c pair chosen by index >> 2".
// ---------------------------------------------------------------------------
const char *const kKernelSource = R"CLC(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

#define PERM_STRIDE 260
#define DBL_F 1.0181268882175227

/* indexedLerp() of cubiomes as a table-free expression (same 16 rows). */
static inline float gradDot(uchar idx, float a, float b, float c)
{
    int i = (int) (idx & 15u);
    int r = (i == 12) ? 0 : (i == 13) ? 9 : (i == 14) ? 1 : (i == 15) ? 11 : i;
    int p = r >> 2;
    float x = (p == 2) ? b : a;
    float y = (p == 0) ? b : c;
    float sx = (r & 1) ? -x : x;
    float sy = (r & 2) ? -y : y;
    return sx + sy;
}

/* One perlin octave, y == 0 fast path (same hash chain as samplePerlin). */
static inline float perlinOct(__global const uchar *pm, __global const float *fp,
                              __global const double *dp, int oct,
                              double wx, double wz, int isB)
{
    double lac = dp[oct * 3 + 2];
    double px = wx * lac;
    double pz = wz * lac;
    if (isB != 0)
    {
        px *= DBL_F;
        pz *= DBL_F;
    }
    double ax = px + dp[oct * 3 + 0];
    double az = pz + dp[oct * 3 + 1];
    double fi1 = floor(ax);
    double fi3 = floor(az);
    float d1 = (float) (ax - fi1);
    float d3 = (float) (az - fi3);
    uchar h1 = (uchar) (int) fi1;
    uchar h3 = (uchar) (int) fi3;

    /* y == 0: reuse the per-octave precomputed b offset. */
    float d2 = fp[oct * 4 + 1];
    float t2 = fp[oct * 4 + 2];
    uchar h2 = pm[oct * PERM_STRIDE + 257];

    float t1 = d1 * d1 * d1 * (d1 * (d1 * 6.0f - 15.0f) + 10.0f);
    float t3 = d3 * d3 * d3 * (d3 * (d3 * 6.0f - 15.0f) + 10.0f);

    __global const uchar *p = pm + oct * PERM_STRIDE;
    uchar a1 = (uchar) (p[h1] + h2);
    uchar b1 = (uchar) (p[h1 + 1] + h2);
    uchar a2 = (uchar) (p[a1] + h3);
    uchar b2 = (uchar) (p[a1 + 1] + h3);
    uchar c2 = (uchar) (p[b1] + h3);
    uchar d2h = (uchar) (p[b1 + 1] + h3);

    float l1 = gradDot(p[a2], d1, d2, d3);
    float l5 = gradDot(p[a2 + 1], d1, d2, d3 - 1.0f);
    float l2 = gradDot(p[c2], d1 - 1.0f, d2, d3);
    float l6 = gradDot(p[c2 + 1], d1 - 1.0f, d2, d3 - 1.0f);
    float l3 = gradDot(p[b2], d1, d2 - 1.0f, d3);
    float l7 = gradDot(p[b2 + 1], d1, d2 - 1.0f, d3 - 1.0f);
    float l4 = gradDot(p[d2h], d1 - 1.0f, d2 - 1.0f, d3);
    float l8 = gradDot(p[d2h + 1], d1 - 1.0f, d2 - 1.0f, d3 - 1.0f);

    l1 = l1 + t1 * (l2 - l1);
    l3 = l3 + t1 * (l4 - l3);
    l5 = l5 + t1 * (l6 - l5);
    l7 = l7 + t1 * (l8 - l7);

    l1 = l1 + t2 * (l3 - l1);
    l5 = l5 + t2 * (l7 - l5);

    return l1 + t3 * (l5 - l1);
}

/* amplitude * octave i of one half; 0 past the octave count like the CPU. */
static inline float octv(__global const uchar *pm, __global const float *fp,
                         __global const double *dp, int base, int cnt, int i,
                         double wx, double wz, int isB)
{
    if (i >= cnt)
        return 0.0f;
    int oct = base + i;
    return fp[oct * 4] * perlinOct(pm, fp, dp, oct, wx, wz, isB);
}

/* groups: 0 = cont, 5 = weirdness, 10 = erosion; [baseA,nA,baseB,nB,ampBits] */
static inline int contPass(__global const uchar *pm, __global const float *fp,
                           __global const double *dp, __global const int *g,
                           double wx, double wz, double thr)
{
    int bA = g[0], nA = g[1], bB = g[2], nB = g[3];
    float amp = as_float(g[4]);
    float sum = 0.0f;

    sum += amp * (octv(pm, fp, dp, bA, nA, 0, wx, wz, 0) + octv(pm, fp, dp, bB, nB, 0, wx, wz, 1));
    if (sum < -0.2f) return 0;
    sum += amp * octv(pm, fp, dp, bA, nA, 1, wx, wz, 0);
    if (sum < -0.1f) return 0;
    sum += amp * octv(pm, fp, dp, bB, nB, 1, wx, wz, 1);
    if (sum < 0.0f) return 0;
    sum += amp * octv(pm, fp, dp, bA, nA, 2, wx, wz, 0);
    if (sum < 0.13f) return 0;
    sum += amp * octv(pm, fp, dp, bB, nB, 2, wx, wz, 1);
    if (sum < 0.3f) return 0;
    sum += amp * octv(pm, fp, dp, bA, nA, 3, wx, wz, 0);
    if (sum < 0.37f) return 0;
    sum += amp * octv(pm, fp, dp, bB, nB, 3, wx, wz, 1);
    if (sum < 0.44f) return 0;
    sum += amp * (octv(pm, fp, dp, bA, nA, 4, wx, wz, 0) + octv(pm, fp, dp, bB, nB, 4, wx, wz, 1));
    return (sum > (float) thr) ? 1 : 0;
}

static inline int weirdPass(__global const uchar *pm, __global const float *fp,
                            __global const double *dp, __global const int *g,
                            double wx, double wz)
{
    int bA = g[5], nA = g[6], bB = g[7], nB = g[8];
    float amp = as_float(g[9]);
    int cnt = (nA < nB) ? nA : nB;
    float v = 0.0f;
    for (int i = 0; i < cnt; ++i)
    {
        v += octv(pm, fp, dp, bA, nA, i, wx, wz, 0);
        v += octv(pm, fp, dp, bB, nB, i, wx, wz, 1);
    }
    int wi = (int) (10000.0f * (v * amp));
    wi = (wi < 0) ? -wi : wi;
    return (wi < 500 || wi > 12800) ? 1 : 0;
}

static inline int erosionPass(__global const uchar *pm, __global const float *fp,
                              __global const double *dp, __global const int *g,
                              double wx, double wz)
{
    int bA = g[10], nA = g[11], bB = g[12], nB = g[13];
    float amp = as_float(g[14]);
    float v = 0.0f;
    for (int i = 0; i < nA; ++i)
        v += octv(pm, fp, dp, bA, nA, i, wx, wz, 0);
    for (int i = 0; i < nB; ++i)
        v += octv(pm, fp, dp, bB, nB, i, wx, wz, 1);
    int ei = (int) (10000.0f * (v * amp));
    return (ei > -3750) ? 1 : 0;
}

__kernel void k_coarse(__global const uchar *pm,
                       __global const float *fp,
                       __global const double *dp,
                       __global const int *g,
                       __global const uchar *mask,
                       __global uchar *out,
                       int startX, int startZ, int scale, int W, int H,
                       int mode, double thr,
                       int maskStartX, int maskStartZ, int maskScale, int maskW, int maskH,
                       int outOffset)
{
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= W || y >= H)
        return;

    int bx = startX + x * scale + (scale >> 1);
    int bz = startZ + y * scale + (scale >> 1);
    int nx = bx / 4;
    int nz = bz / 4;
    int pass = 0;

    if (mode == 0)
    {
        pass = contPass(pm, fp, dp, g, (double) nx, (double) nz, thr);
    }
    else if (mode == 1)
    {
        int allow = 1;
        if (maskScale > 0 && maskW > 0 && maskH > 0)
        {
            int tx = (bx - maskStartX) / maskScale;
            int tz = (bz - maskStartZ) / maskScale;
            allow = (tx >= 0 && tz >= 0 && tx < maskW && tz < maskH)
                ? (mask[tz * maskW + tx] != 0) : 0;
        }
        pass = allow ? weirdPass(pm, fp, dp, g, (double) nx, (double) nz) : 0;
    }
    else
    {
        if (contPass(pm, fp, dp, g, (double) nx, (double) nz, thr) != 0
            && weirdPass(pm, fp, dp, g, (double) nx, (double) nz) != 0)
            pass = erosionPass(pm, fp, dp, g, (double) nx, (double) nz);
    }

    out[outOffset + y * W + x] = (uchar) pass;
}

/* Chebyshev dilation, the same mask buildDilatedContPrefilterMask() produces. */
__kernel void k_dilate(__global const uchar *in, __global uchar *out,
                       int W, int H, int radius, int outOffset)
{
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= W || y >= H)
        return;
    int x0 = (x - radius > 0) ? x - radius : 0;
    int x1 = (x + radius < W) ? x + radius : W - 1;
    int y0 = (y - radius > 0) ? y - radius : 0;
    int y1 = (y + radius < H) ? y + radius : H - 1;
    uchar v = 0;
    for (int yy = y0; yy <= y1 && v == 0; ++yy)
    {
        for (int xx = x0; xx <= x1; ++xx)
        {
            if (in[yy * W + xx] != 0)
            {
                v = 1;
                break;
            }
        }
    }
    out[outOffset + y * W + x] = v;
}

/* Zero fills the weirdness region: cells of tiles that are not in the
 * compacted list must read as 0, not as the previous chunk's leftovers. */
__kernel void k_clear(__global uchar *out, int count, int offset)
{
    int i = get_global_id(0);
    if (i >= count)
        return;
    out[offset + i] = 0;
}

/* Compacts the set tiles of the dilated mask into a list.
 *
 * The mask is per continentalness tile, so a passing tile means the whole
 * ratio x ratio block of weirdness cells has to be evaluated.  Without this
 * list every weirdness cell would be a work item and ~82% of the GPU lanes
 * would sit in mask-zero warps that still issue the full octave code. */
__kernel void k_compact(__global const uchar *mask, __global int *list,
                        __global int *count, int cW, int cH)
{
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= cW || y >= cH)
        return;
    if (mask[y * cW + x] == 0)
        return;
    int slot = atomic_inc(count);
    list[slot] = y * cW + x;
}

/* Weirdness grid over the compacted tile list: one work item per cell of a
 * listed tile.  Items past `count[0]` exit immediately. */
__kernel void k_weird_list(__global const uchar *pm, __global const float *fp,
                           __global const double *dp, __global const int *g,
                           __global const int *list, __global const int *count,
                           __global uchar *out,
                           int startX, int startZ, int weirdScale,
                           int wW, int wH, int cW, int ratio, int outOffset)
{
    int t = get_global_id(1);
    int i = get_global_id(0);
    if (t >= count[0])
        return;
    int tile = list[t];
    int tx = tile % cW;
    int ty = tile / cW;
    int x = tx * ratio + (i % ratio);
    int y = ty * ratio + (i / ratio);
    if (x >= wW || y >= wH)
        return;
    int bx = startX + x * weirdScale + (weirdScale >> 1);
    int bz = startZ + y * weirdScale + (weirdScale >> 1);
    out[outOffset + y * wW + x] =
        (uchar) weirdPass(pm, fp, dp, g, (double) (bx / 4), (double) (bz / 4));
}

/* ---------------------------------------------------------------------------
 * Ring stage: 2D prefix sums, annular sums, max-area filter.
 *
 * The layout and the arithmetic mirror findBiggestRiver() exactly: an
 * inclusive 2D prefix grid of stride W+1 (row 0 and column 0 are zero), the
 * same annular ring rows (dz, out, in) with `in < 0` meaning "full disc row",
 * and integer areas, so the device result is bit-identical to the CPU.
 * ------------------------------------------------------------------------- */
__kernel void k_scan_rows(__global const uchar *cells, __global int *pref, int W, int H)
{
    int z = get_global_id(0);
    if (z >= H)
        return;
    int stride = W + 1;
    __global int *out = pref + (z + 1) * stride;
    out[0] = 0;
    if (z == 0)
    {
        for (int x = 0; x <= W; ++x)
            pref[x] = 0;
    }
    int run = 0;
    __global const uchar *row = cells + z * W;
    for (int x = 0; x < W; ++x)
    {
        run += (int) row[x];
        out[x + 1] = run;
    }
}

__kernel void k_scan_cols(__global int *pref, int W, int H)
{
    int x = get_global_id(0);
    if (x > W)
        return;
    int stride = W + 1;
    for (int z = 0; z < H; ++z)
        pref[(z + 1) * stride + x] += pref[z * stride + x];
}

/* Separable Chebyshev max filter: the CPU's centerCand (dilation of the pass
 * cells by R_out) as two linear passes instead of one O(r^2) box filter. */
__kernel void k_maxfilt_h(__global const uchar *in, __global uchar *out,
                          int W, int H, int radius)
{
    int x = get_global_id(0);
    int z = get_global_id(1);
    if (x >= W || z >= H)
        return;
    int x0 = (x - radius > 0) ? x - radius : 0;
    int x1 = (x + radius < W) ? x + radius : W - 1;
    int base = z * W;
    uchar v = 0;
    for (int xx = x0; xx <= x1; ++xx)
    {
        if (in[base + xx] != 0)
        {
            v = 1;
            break;
        }
    }
    out[base + x] = v;
}

__kernel void k_maxfilt_v(__global const uchar *in, __global uchar *out,
                          int W, int H, int radius)
{
    int x = get_global_id(0);
    int z = get_global_id(1);
    if (x >= W || z >= H)
        return;
    int z0 = (z - radius > 0) ? z - radius : 0;
    int z1 = (z + radius < H) ? z + radius : H - 1;
    uchar v = 0;
    for (int zz = z0; zz <= z1; ++zz)
    {
        if (in[zz * W + x] != 0)
        {
            v = 1;
            break;
        }
    }
    out[z * W + x] = v;
}

__kernel void k_ring_areas(__global const int *pref, int W, int H, int ringOut,
                           __global const int *ring, int ringRows, int scale,
                           int minArea, __global int *areas,
                           __global uint *maxArea, int maxAreaSlot,
                           __global const uchar *centreCand)
{
    int cx = get_global_id(0);
    int cz = get_global_id(1);
    if (cx < ringOut || cx >= W - ringOut || cz < ringOut || cz >= H - ringOut)
        return;
    if (centreCand[cz * W + cx] == 0)
    {
        areas[cz * W + cx] = 0;
        return;
    }
    int stride = W + 1;
    int area = 0;
    for (int i = 0; i < ringRows; ++i)
    {
        int dz = ring[i * 3];
        int out = ring[i * 3 + 1];
        int in = ring[i * 3 + 2];
        int row = cz + dz;
        int base0 = row * stride;
        int base1 = (row + 1) * stride;
        int L = cx - out;
        int R = cx + out;
        if (in < 0)
        {
            area += pref[base1 + R + 1] - pref[base1 + L]
                  - pref[base0 + R + 1] + pref[base0 + L];
        }
        else
        {
            int Lin = cx - in;
            int Rin = cx + in;
            area += pref[base1 + Lin] - pref[base1 + L]
                  - pref[base0 + Lin] + pref[base0 + L];
            area += pref[base1 + R + 1] - pref[base1 + Rin + 1]
                  - pref[base0 + R + 1] + pref[base0 + Rin + 1];
        }
    }
    int worldTotal = area * scale * scale;
    areas[cz * W + cx] = worldTotal;
    if (worldTotal >= minArea)
        atomic_max(maxArea + maxAreaSlot, (uint) worldTotal);
}

/* Emits every centre with area >= max(minArea, windowMax * f). */
__kernel void k_select_list(__global const int *areas, __global const uint *maxArea,
                            int maxAreaSlot, double f, int minArea, int W, int H, int ringOut,
                            __global int *counts, int countSlot,
                            __global int *cands, int candSlot, int cap)
{
    int cx = get_global_id(0);
    int cz = get_global_id(1);
    if (cx < ringOut || cx >= W - ringOut || cz < ringOut || cz >= H - ringOut)
        return;
    int area = areas[cz * W + cx];
    if (area < minArea)
        return;
    if ((double) area < (double) maxArea[maxAreaSlot] * f)
        return;
    int slot = atomic_inc(counts + countSlot);
    if (slot >= cap)
        return;
    __global int *out = cands + candSlot;
    out[slot * 2] = (cx << 16) | (cz & 0xFFFF);
    out[slot * 2 + 1] = area;
}

/* PQ-top of findBiggestRiver(..., f = 1.0): max area, then min x, then min z. */
__kernel void k_select_best(__global const int *areas, __global const uint *maxArea,
                            int maxAreaSlot, int minArea, int W, int H, int ringOut,
                            __global uint *best, int bestSlot)
{
    int cx = get_global_id(0);
    int cz = get_global_id(1);
    if (cx < ringOut || cx >= W - ringOut || cz < ringOut || cz >= H - ringOut)
        return;
    int area = areas[cz * W + cx];
    if (area < minArea || (uint) area != maxArea[maxAreaSlot])
        return;
    atomic_min(best + bestSlot, ((uint) cx << 16) | (uint) cz);
}
)CLC";

constexpr int kPermStride = 260;
constexpr int kSlotFloats = 4;
constexpr int kSlotDoubles = 3;

struct HostParams
{
    std::vector<uint8_t> perm;
    std::vector<float> fp;
    std::vector<double> dp;
    int gdesc[16] = { 0 };
    int octaves = 0;
};

/**
 * Per-thread device state.
 *
 * Windows/WDDM measures ~78-100 us per OpenCL submission round trip, which is
 * several times the device time of one phase-1 chunk. Serialising the search
 * threads on one queue therefore adds the round trip of every worker to the
 * critical path. Each worker instead owns a queue, its own kernel objects and
 * its own scratch buffers (the noise parameter buffers stay shared and read
 * only), so the round trips of the workers overlap while the device executes
 * their kernels back to back.
 */
constexpr int kMaxSlots = 64;

struct Slot
{
    cl_command_queue queue = nullptr;
    cl_kernel kernelCoarse = nullptr;
    cl_kernel kernelDilate = nullptr;
    cl_kernel kernelCompact = nullptr;
    cl_kernel kernelWeirdList = nullptr;
    cl_kernel kernelClear = nullptr;
    cl_kernel kernelScanRows = nullptr;
    cl_kernel kernelScanCols = nullptr;
    cl_kernel kernelRingAreas = nullptr;
    cl_kernel kernelSelectList = nullptr;
    cl_kernel kernelSelectBest = nullptr;
    cl_kernel kernelMaxH = nullptr;
    cl_kernel kernelMaxV = nullptr;
    cl_mem bufMask = nullptr;
    cl_mem bufGrid = nullptr;
    cl_mem bufRaw = nullptr;
    cl_mem bufList = nullptr;
    cl_mem bufCount = nullptr;
    cl_mem bufPref = nullptr;
    cl_mem bufAreas = nullptr;
    cl_mem bufCand = nullptr;
    cl_mem bufSmall = nullptr;
    cl_mem bufRing = nullptr;
    cl_mem bufCandCell = nullptr;
    cl_mem bufCandTmp = nullptr;
    size_t maskBytes = 0;
    size_t gridBytes = 0;
    size_t rawBytes = 0;
    size_t listBytes = 0;
    size_t prefBytes = 0;
    size_t areasBytes = 0;
    size_t candBytes = 0;
    size_t smallBytes = 0;
    size_t ringBytes = 0;
    size_t candCellBytes = 0;
    size_t candTmpBytes = 0;
};

/** Ring candidates one window may emit before the chunk falls back to the CPU. */
constexpr int kCandidateCap = 256;

struct State
{
    bool probed = false;
    bool ok = false;
    std::string device;
    std::string reason;

    ClApi api{};
    void *library = nullptr;
    cl_platform_id platform = nullptr;
    cl_device_id deviceId = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_program program = nullptr;

    int maxWorkGroup = 0;
    int wgx = 16;
    int wgy = 16;

    cl_mem bufPerm = nullptr;
    cl_mem bufFp = nullptr;
    cl_mem bufDp = nullptr;
    cl_mem bufDesc = nullptr;
    size_t bufPermBytes = 0;
    size_t bufFpBytes = 0;
    size_t bufDpBytes = 0;
    size_t bufDescBytes = 0;

    Slot slots[kMaxSlots];
    std::atomic<int> slotNext{ 0 };
    std::atomic<uint64_t> generation{ 0 };

    HostParams params;
    uint64_t seedUploaded = ~0ull;
    int mcUploaded = -1;

    std::atomic<bool> active{ false };
    std::atomic<uint64_t> submissions{ 0 };
    std::atomic<uint64_t> cells{ 0 };
    std::atomic<uint64_t> fallbacks{ 0 };
    std::atomic<uint64_t> hostEnqueueNs{ 0 };
    std::atomic<uint64_t> hostWaitNs{ 0 };
};

State &state()
{
    static State instance;
    return instance;
}

std::mutex &gpuMutex()
{
    static std::mutex instance;
    return instance;
}

std::atomic<int> &backendSlot()
{
    static std::atomic<int> instance{ static_cast<int>(lowyd_gpu::Backend::Auto) };
    return instance;
}

template <typename T>
bool loadSymbol(void *library, const char *name, T &fn)
{
    if (!library)
        return false;
#if defined(_WIN32)
    FARPROC symbol = GetProcAddress(static_cast<HMODULE>(library), name);
#else
    void *symbol = dlsym(library, name);
#endif
    fn = reinterpret_cast<T>(symbol);
    return symbol != nullptr;
}

void *openLibrary()
{
#if defined(_WIN32)
    /* LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800): never pick up a DLL from the cwd. */
    HMODULE handle = LoadLibraryExW(L"OpenCL.dll", nullptr, 0x00000800);
    if (!handle)
        handle = LoadLibraryExW(L"OpenCL.dll", nullptr, 0);
    return static_cast<void *>(handle);
#else
    return dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

std::string deviceString(cl_device_id device, const ClApi &api)
{
    char name[256] = { 0 };
    char vendor[256] = { 0 };
    char version[256] = { 0 };
    api.clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(name), name, nullptr);
    api.clGetDeviceInfo(device, CL_DEVICE_VENDOR, sizeof(vendor), vendor, nullptr);
    api.clGetDeviceInfo(device, CL_DEVICE_VERSION, sizeof(version), version, nullptr);
    std::string text = name;
    text += " (";
    text += vendor;
    text += ", ";
    text += version;
    text += ")";
    return text;
}

/** Capability filter from the plan: >=256 work-items, >=32 KiB local memory. */
bool deviceQualified(cl_device_id device, const ClApi &api, int &maxWorkGroup,
                     cl_ulong &localMem, cl_device_type &type, cl_uint &computeUnits,
                     cl_uint &clockMHz, cl_bitfield &doubleConfig)
{
    /* These four are size_t / cl_bitfield / cl_ulong in the OpenCL ABI; the
     * query size has to match or the ICD rejects the call. */
    size_t workGroupSize = 0;
    if (api.clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(workGroupSize),
                            &workGroupSize, nullptr) != CL_SUCCESS)
        return false;
    maxWorkGroup = static_cast<int>(workGroupSize);
    if (maxWorkGroup < 256)
        return false;

    localMem = 0;
    if (api.clGetDeviceInfo(device, CL_DEVICE_LOCAL_MEM_SIZE, sizeof(localMem), &localMem,
                            nullptr) != CL_SUCCESS)
        return false;
    if (localMem < 32 * 1024)
        return false;

    cl_bitfield typeBits = 0;
    if (api.clGetDeviceInfo(device, CL_DEVICE_TYPE, sizeof(typeBits), &typeBits, nullptr)
        != CL_SUCCESS)
        return false;
    type = static_cast<cl_device_type>(typeBits);

    computeUnits = 0;
    api.clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(computeUnits),
                        &computeUnits, nullptr);
    clockMHz = 0;
    api.clGetDeviceInfo(device, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(clockMHz), &clockMHz,
                        nullptr);
    doubleConfig = 0;
    api.clGetDeviceInfo(device, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof(doubleConfig), &doubleConfig,
                        nullptr);
    return true;
}

/** Optional substring selector, so a benchmark can pin iGPU or dGPU. */
bool deviceMatchesFilter(const std::string &description)
{
    const char *filter = std::getenv("LOWYD_GPU_DEVICE");
    if (!filter || !*filter)
        return true;
    return description.find(filter) != std::string::npos;
}

bool debugEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("LOWYD_GPU_DEBUG");
        return value && *value && *value != '0';
    }();
    return enabled;
}

bool buildProgram(std::string *reason)
{
    State &s = state();
    cl_int status = 0;
    const char *source = kKernelSource;
    const size_t length = std::strlen(kKernelSource);
    s.program = s.api.clCreateProgramWithSource(s.context, 1, &source, &length, &status);
    if (!s.program || status != CL_SUCCESS)
    {
        *reason = "create-program-failed";
        return false;
    }
    status = s.api.clBuildProgram(s.program, 1, &s.deviceId, "-cl-std=CL1.2", nullptr, nullptr);
    if (status != CL_SUCCESS)
    {
        size_t logSize = 0;
        s.api.clGetProgramBuildInfo(s.program, s.deviceId, CL_PROGRAM_BUILD_LOG, 0, nullptr,
                                    &logSize);
        std::string log(logSize ? logSize + 1 : 2, '\0');
        if (logSize)
            s.api.clGetProgramBuildInfo(s.program, s.deviceId, CL_PROGRAM_BUILD_LOG, logSize,
                                        log.data(), nullptr);
        while (!log.empty() && (log.back() == '\0' || log.back() == '\n'))
            log.pop_back();
        *reason = "build-failed:" + log;
        return false;
    }
    /* Kernel objects belong to the per-thread slots (see Slot/ensureSlot). */
    return true;
}

bool probe(std::string *reason)
{
    State &s = state();
    s.probed = true;

    s.library = openLibrary();
    if (!s.library)
    {
        *reason = "no-opencl";
        return false;
    }

    ClApi &api = s.api;
    bool resolved = true;
    resolved &= loadSymbol(s.library, "clGetPlatformIDs", api.clGetPlatformIDs);
    resolved &= loadSymbol(s.library, "clGetPlatformInfo", api.clGetPlatformInfo);
    resolved &= loadSymbol(s.library, "clGetDeviceIDs", api.clGetDeviceIDs);
    resolved &= loadSymbol(s.library, "clGetDeviceInfo", api.clGetDeviceInfo);
    resolved &= loadSymbol(s.library, "clCreateContext", api.clCreateContext);
    resolved &= loadSymbol(s.library, "clCreateCommandQueue", api.clCreateCommandQueue);
    resolved &= loadSymbol(s.library, "clCreateProgramWithSource", api.clCreateProgramWithSource);
    resolved &= loadSymbol(s.library, "clBuildProgram", api.clBuildProgram);
    resolved &= loadSymbol(s.library, "clGetProgramBuildInfo", api.clGetProgramBuildInfo);
    resolved &= loadSymbol(s.library, "clCreateKernel", api.clCreateKernel);
    resolved &= loadSymbol(s.library, "clSetKernelArg", api.clSetKernelArg);
    resolved &= loadSymbol(s.library, "clCreateBuffer", api.clCreateBuffer);
    resolved &= loadSymbol(s.library, "clEnqueueWriteBuffer", api.clEnqueueWriteBuffer);
    resolved &= loadSymbol(s.library, "clEnqueueReadBuffer", api.clEnqueueReadBuffer);
    resolved &= loadSymbol(s.library, "clEnqueueNDRangeKernel", api.clEnqueueNDRangeKernel);
    resolved &= loadSymbol(s.library, "clFinish", api.clFinish);
    resolved &= loadSymbol(s.library, "clReleaseMemObject", api.clReleaseMemObject);
    resolved &= loadSymbol(s.library, "clReleaseKernel", api.clReleaseKernel);
    resolved &= loadSymbol(s.library, "clReleaseProgram", api.clReleaseProgram);
    resolved &= loadSymbol(s.library, "clReleaseCommandQueue", api.clReleaseCommandQueue);
    resolved &= loadSymbol(s.library, "clReleaseContext", api.clReleaseContext);
    if (!resolved)
    {
        *reason = "no-opencl:mising-entry-point";
        return false;
    }

    /* Every platform, every device: laptops often list the iGPU first. */
    cl_uint platformCount = 0;
    if (api.clGetPlatformIDs(0, nullptr, &platformCount) != CL_SUCCESS || platformCount == 0)
    {
        *reason = "no-opencl:no-platforms";
        return false;
    }
    std::vector<cl_platform_id> platforms(platformCount);
    api.clGetPlatformIDs(platformCount, platforms.data(), nullptr);

    int bestScore = -1;
    for (cl_platform_id platform: platforms)
    {
        cl_uint deviceCount = 0;
        if (api.clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &deviceCount) != CL_SUCCESS
            || deviceCount == 0)
            continue;
        std::vector<cl_device_id> devices(deviceCount);
        if (api.clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, deviceCount, devices.data(), nullptr)
            != CL_SUCCESS)
            continue;
        for (cl_device_id device: devices)
        {
            int maxWorkGroup = 0;
            cl_ulong localMem = 0;
            cl_device_type type = 0;
            cl_uint computeUnits = 0;
            cl_uint clockMHz = 0;
            cl_bitfield doubleConfig = 0;
            const std::string description = deviceString(device, api);
            const bool qualified = deviceQualified(device, api, maxWorkGroup, localMem, type,
                                                   computeUnits, clockMHz, doubleConfig);
            if (debugEnabled())
                std::fprintf(stderr,
                             "[gpu] device: %s type=0x%x wg=%d local=%llu fp64cfg=%llu%s\n",
                             description.c_str(), (unsigned) type, maxWorkGroup,
                             (unsigned long long) localMem, (unsigned long long) doubleConfig,
                             qualified ? "" : " (disqualified)");
            if (!qualified)
                continue;
            /* The kernels need fp64 phases; devices without it are skipped. */
            if (doubleConfig == 0)
            {
                if (debugEnabled())
                    std::fprintf(stderr, "[gpu]   skipped: no fp64\n");
                continue;
            }
            if (!deviceMatchesFilter(description))
            {
                if (debugEnabled())
                    std::fprintf(stderr, "[gpu]   skipped: LOWYD_GPU_DEVICE\n");
                continue;
            }

            int score = static_cast<int>(computeUnits);
            if (type & CL_DEVICE_TYPE_GPU)
                score += 1000;
            if (type & CL_DEVICE_TYPE_ACCELERATOR)
                score += 500;
            score += static_cast<int>(clockMHz) / 100;
            if (score > bestScore)
            {
                bestScore = score;
                s.platform = platform;
                s.deviceId = device;
                s.device = description;
                s.maxWorkGroup = maxWorkGroup;
            }
        }
    }

    if (!s.deviceId)
    {
        *reason = "no-device";
        return false;
    }

    s.wgx = 16;
    s.wgy = 16;
    if (s.maxWorkGroup < 256)
    {
        *reason = "no-device:work-group-too-small";
        return false;
    }

    cl_int status = 0;
    s.context = api.clCreateContext(nullptr, 1, &s.deviceId, nullptr, nullptr, &status);
    if (!s.context || status != CL_SUCCESS)
    {
        *reason = "no-device:context-failed";
        return false;
    }
    s.queue = api.clCreateCommandQueue(s.context, s.deviceId, 0, &status);
    if (!s.queue || status != CL_SUCCESS)
    {
        *reason = "no-device:queue-failed";
        return false;
    }
    if (!buildProgram(reason))
        return false;

    s.ok = true;
    return true;
}

/** Adds one perlin octave slot to the packed host parameter block. */
void addSlot(HostParams &params, const PerlinNoise *p)
{
    const size_t offset = params.perm.size();
    params.perm.resize(offset + kPermStride, 0);
    std::memcpy(params.perm.data() + offset, p->d, 257);

    const double b = p->b;
    const double bi = std::floor(b);
    const double d2 = b - bi;
    const double t2 = d2 * d2 * d2 * (d2 * (d2 * 6.0 - 15.0) + 10.0);
    params.perm[offset + 257] = static_cast<uint8_t>(static_cast<int>(bi));

    params.fp.push_back(static_cast<float>(p->amplitude));
    params.fp.push_back(static_cast<float>(d2));
    params.fp.push_back(static_cast<float>(t2));
    params.fp.push_back(0.0f);

    params.dp.push_back(p->a);
    params.dp.push_back(p->c);
    params.dp.push_back(p->lacunarity);

    params.octaves++;
}

/** cont -> gdesc[0..4], weirdness -> [5..9], erosion -> [10..14]. */
void addNoise(HostParams &params, const DoublePerlinNoise &noise, int slot)
{
    const int baseA = params.octaves;
    for (int i = 0; i < noise.octA.octcnt; i++)
        addSlot(params, noise.octA.octaves + i);
    const int nA = noise.octA.octcnt;
    const int baseB = params.octaves;
    for (int i = 0; i < noise.octB.octcnt; i++)
        addSlot(params, noise.octB.octaves + i);
    const int nB = noise.octB.octcnt;

    params.gdesc[slot + 0] = baseA;
    params.gdesc[slot + 1] = nA;
    params.gdesc[slot + 2] = baseB;
    params.gdesc[slot + 3] = nB;
    const float amp = static_cast<float>(noise.amplitude);
    std::memcpy(&params.gdesc[slot + 4], &amp, sizeof(amp));
}

bool ensureBuffer(cl_mem &buffer, size_t &capacity, size_t bytes, cl_bitfield flags)
{
    State &s = state();
    if (buffer && capacity >= bytes)
        return true;
    if (buffer)
    {
        s.api.clReleaseMemObject(buffer);
        buffer = nullptr;
        capacity = 0;
    }
    cl_int status = 0;
    buffer = s.api.clCreateBuffer(s.context, flags, bytes, nullptr, &status);
    if (!buffer || status != CL_SUCCESS)
    {
        buffer = nullptr;
        return false;
    }
    capacity = bytes;
    return true;
}

/** Creates (once) the queue, kernel objects and scratch buffers of a slot. */
bool ensureRingBuffers(Slot &slot, int cW, int cH, int W, int H, int windows, int ringRows)
{
    const size_t contCells = static_cast<size_t>(cW) * static_cast<size_t>(cH);
    const size_t cells = static_cast<size_t>(W) * static_cast<size_t>(H);
    const size_t prefInts = static_cast<size_t>(W + 1) * static_cast<size_t>(H + 1);
    return ensureBuffer(slot.bufRaw, slot.rawBytes, contCells, CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufMask, slot.maskBytes, contCells, CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufGrid, slot.gridBytes, cells, CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufPref, slot.prefBytes, prefInts * sizeof(cl_int), CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufAreas, slot.areasBytes, cells * sizeof(cl_int),
                        CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufSmall, slot.smallBytes,
                        static_cast<size_t>(windows) * 3 * sizeof(cl_int), CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufCand, slot.candBytes,
                        static_cast<size_t>(windows) * kCandidateCap * 2 * sizeof(cl_int),
                        CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufCandCell, slot.candCellBytes, cells, CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufCandTmp, slot.candTmpBytes, cells, CL_MEM_READ_WRITE)
        && ensureBuffer(slot.bufRing, slot.ringBytes,
                        static_cast<size_t>(ringRows) * 3 * sizeof(cl_int), CL_MEM_READ_WRITE);
}

/** Forward declarations; the definitions follow further down this file. */
Slot &currentSlot();
bool ensureSlot(Slot &slot);
bool enqueueCoarse(Slot &slot, int startX, int startZ, int W, int H, int scale, int mode,
                   double threshold, cl_mem mask, int maskStartX, int maskStartZ, int maskScale,
                   int maskW, int maskH, int outOffset, cl_mem out, cl_int &failure);
bool enqueueDilate(Slot &slot, int W, int H, int radius, cl_mem in, cl_mem out, int outOffset,
                   cl_int &failure);
void noteFailure(const char *what);
size_t roundUp(size_t value, size_t multiple);

} // namespace

namespace lowyd_gpu
{

namespace
{

/**
 * Runs one or more windows: optional cell-grid chain, 2D prefix sums, annular
 * sums, then either the candidate list (phase-1 chunk) or the PQ-top centre
 * (ClimateCoarse). Everything is queued and flushed once.
 */
bool runWindows(const CoarseSpec *specs, int count, std::vector<Candidate> *outLists,
                Candidate *bestOut)
{
    State &s = state();
    if (!s.active.load() || !s.ok)
        return false;
    if (!specs || count <= 0)
        return false;
    const CoarseSpec &first = specs[0];
    if (first.ringRows <= 0 || !first.ring || first.ringOut <= 0)
        return false;

    Slot &slot = currentSlot();
    if (!slot.queue)
        return false;

    const int gridScale = first.gridScale;
    const int contScale = first.contScale;
    const int W = first.chunkW / gridScale;
    const int H = first.chunkH / gridScale;
    const int cW = first.chunkW / contScale;
    const int cH = first.chunkH / contScale;
    if (W <= 2 * first.ringOut || H <= 2 * first.ringOut || cW <= 0 || cH <= 0)
        return false;
    const bool compact = contScale >= gridScale && contScale % gridScale == 0
        && contScale / gridScale <= 16;
    if (!first.climate && !compact)
        return false;
    const int ratio = compact ? contScale / gridScale : 1;

    if (!ensureRingBuffers(slot, cW, cH, W, H, count, first.ringRows))
    {
        noteFailure("alloc-ring");
        return false;
    }
    if (!first.climate && !ensureBuffer(slot.bufList, slot.listBytes,
                                        static_cast<size_t>(cW) * cH * sizeof(cl_int),
                                        CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-list");
        return false;
    }

    cl_int failure = CL_SUCCESS;
    /* Profiling switch: LOWYD_GPU_SKIP_RING=1 drops the ring/select stage (the
     * candidate list is then empty) so the cell-grid cost can be isolated. */
    static const bool skipRing = [] {
        const char *value = std::getenv("LOWYD_GPU_SKIP_RING");
        return value && *value && *value != '0';
    }();
    /* Ring geometry (shared by all windows of the batch). */
    failure = s.api.clEnqueueWriteBuffer(slot.queue, slot.bufRing, CL_FALSE, 0,
                                         static_cast<size_t>(first.ringRows) * 3 * sizeof(cl_int),
                                         first.ring, 0, nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("write-ring");
        return false;
    }

    /* Per-window scratch: max area (0), best key (UINT_MAX), candidate count (0). */
    std::vector<cl_int> small(static_cast<size_t>(count) * 3, 0);
    for (int i = 0; i < count; i++)
        small[static_cast<size_t>(count) + i] = -1; /* 0xFFFFFFFF */
    failure = s.api.clEnqueueWriteBuffer(slot.queue, slot.bufSmall, CL_FALSE, 0,
                                         small.size() * sizeof(cl_int), small.data(), 0,
                                         nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("write-slots");
        return false;
    }
    static const cl_int zero = 0;

    const size_t cells = static_cast<size_t>(W) * static_cast<size_t>(H);
    const size_t grid[2] = { roundUp(static_cast<size_t>(W), 16), roundUp(static_cast<size_t>(H), 16) };
    const size_t local2[2] = { 16, 16 };
    const size_t ring1 = roundUp(static_cast<size_t>(H), 64);
    const size_t cols1 = roundUp(static_cast<size_t>(W + 1), 64);
    const size_t local1 = 64;

    for (int i = 0; i < count; i++)
    {
        const CoarseSpec &spec = specs[i];
        if (spec.gridScale != gridScale || spec.ringRows != first.ringRows
            || spec.climate != first.climate)
            return false;

        if (first.climate)
        {
            /* 1. climate 0/1 grid at bufGrid[0]. */
            if (!enqueueCoarse(slot, spec.startX, spec.startZ, W, H, gridScale,
                               static_cast<int>(GRID_CLIMATE), spec.contThreshold,
                               nullptr, spec.startX, spec.startZ, 0, 0, 0, 0, slot.bufGrid,
                               failure))
            {
                noteFailure("kernel-climate");
                return false;
            }
        }
        else
        {
            /* 1. raw continentalness grid -> 2. dilation -> 3. tile compaction
             * -> 4. weirdness grid inside the dilated mask. */
            if (!enqueueCoarse(slot, spec.startX, spec.startZ, cW, cH, contScale,
                               static_cast<int>(GRID_CONT), spec.contThreshold, nullptr,
                               spec.startX, spec.startZ, 0, 0, 0, 0, slot.bufRaw, failure))
            {
                noteFailure("kernel-cont");
                return false;
            }
            if (!enqueueDilate(slot, cW, cH, spec.dilate, slot.bufRaw, slot.bufMask, 0, failure))
            {
                noteFailure("kernel-dilate");
                return false;
            }
            failure = s.api.clEnqueueWriteBuffer(slot.queue, slot.bufCount, CL_FALSE, 0,
                                                 sizeof(cl_int), &zero, 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("reset-count");
                return false;
            }
            {
                cl_kernel kernel = slot.kernelCompact;
                cl_uint index = 0;
                cl_mem mask = slot.bufMask;
                cl_int cw = cW;
                cl_int ch = cH;
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &mask);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufList);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufCount);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &cw);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ch);
                failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                       &grid[0], &local2[0], 0, nullptr, nullptr);
                if (failure != CL_SUCCESS)
                {
                    noteFailure("kernel-compact");
                    return false;
                }
            }
            /* Zero the cell region: tiles outside the compacted list must read 0,
             * not the previous window's leftover grid. */
            {
                cl_kernel kernel = slot.kernelClear;
                cl_uint index = 0;
                cl_mem out = slot.bufGrid;
                cl_int count = static_cast<cl_int>(W * H);
                cl_int offset = 0;
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &count);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &offset);
                const size_t local = 64;
                const size_t total = roundUp(static_cast<size_t>(W) * static_cast<size_t>(H),
                                             local);
                failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 1, nullptr, &total,
                                                       &local, 0, nullptr, nullptr);
                if (failure != CL_SUCCESS)
                {
                    noteFailure("kernel-clear");
                    return false;
                }
            }
            {
                cl_kernel kernel = slot.kernelWeirdList;
                cl_uint index = 0;
                cl_mem out = slot.bufGrid;
                cl_int kStartX = spec.startX;
                cl_int kStartZ = spec.startZ;
                cl_int wScale = gridScale;
                cl_int ww = W;
                cl_int wh = H;
                cl_int cw = cW;
                cl_int r = ratio;
                cl_int outOffset = 0;
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufPerm);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufFp);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDp);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDesc);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufList);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufCount);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &kStartX);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &kStartZ);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &wScale);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ww);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &wh);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &cw);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &r);
                s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &outOffset);
                const size_t local[2] = { static_cast<size_t>(s.wgx),
                                          static_cast<size_t>(s.wgy) };
                const size_t global[2] = {
                    roundUp(static_cast<size_t>(ratio * ratio), local[0]),
                    roundUp(static_cast<size_t>(cW) * static_cast<size_t>(cH), local[1])
                };
                failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr, global,
                                                       local, 0, nullptr, nullptr);
                if (failure != CL_SUCCESS)
                {
                    noteFailure("kernel-weird-list");
                    return false;
                }
            }
        }

        /* centreCand = Chebyshev dilation of the cell grid by ringOut: the same
         * prune the CPU does, so only ~21% of the centres need a ring sum. */
        {
            cl_kernel kernel = slot.kernelMaxH;
            cl_uint index = 0;
            cl_mem inArg = slot.bufGrid;
            cl_mem outArg = slot.bufCandTmp;
            cl_int w = W;
            cl_int h = H;
            cl_int radius = first.ringOut;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &inArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &outArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &radius);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                   &grid[0], &local2[0], 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-maxh");
                return false;
            }
            kernel = slot.kernelMaxV;
            index = 0;
            cl_mem candArg = slot.bufCandCell;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &outArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &candArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &radius);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                   &grid[0], &local2[0], 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-maxv");
                return false;
            }
        }

        /* prefix sums, row pass then column pass (in order on this queue). */
        {
            cl_kernel kernel = slot.kernelScanRows;
            cl_uint index = 0;
            cl_mem cellsArg = slot.bufGrid;
            cl_mem prefArg = slot.bufPref;
            cl_int w = W;
            cl_int h = H;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &cellsArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &prefArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 1, nullptr, &ring1,
                                                   &local1, 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-scan-rows");
                return false;
            }
            kernel = slot.kernelScanCols;
            index = 0;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &prefArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 1, nullptr, &cols1,
                                                   &local1, 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-scan-cols");
                return false;
            }
        }

        /* annular sums around every interior centre (also yields the max area). */
        if (skipRing)
            continue;
        {
            cl_kernel kernel = slot.kernelRingAreas;
            cl_uint index = 0;
            cl_mem prefArg = slot.bufPref;
            cl_int w = W;
            cl_int h = H;
            cl_int ringOut = first.ringOut;
            cl_mem ringArg = slot.bufRing;
            cl_int ringRows = first.ringRows;
            cl_int scale = gridScale;
            cl_int minArea = spec.minArea;
            cl_mem areasArg = slot.bufAreas;
            cl_mem smallArg = slot.bufSmall;
            cl_int slotIndex = i;
            cl_mem candArg = slot.bufCandCell;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &prefArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ringOut);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &ringArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ringRows);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &scale);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &minArea);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &areasArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &smallArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &slotIndex);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &candArg);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                   &grid[0], &local2[0], 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-ring");
                return false;
            }
        }

        /* candidate selection. */
        if (first.climate)
        {
            cl_kernel kernel = slot.kernelSelectBest;
            cl_uint index = 0;
            cl_mem areasArg = slot.bufAreas;
            cl_mem smallArg = slot.bufSmall;
            cl_int maxAreaSlot = i;
            cl_int minArea = spec.minArea;
            cl_int w = W;
            cl_int h = H;
            cl_int ringOut = first.ringOut;
            cl_int bestSlot = count + i;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &areasArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &smallArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &maxAreaSlot);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &minArea);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ringOut);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &smallArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &bestSlot);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                   &grid[0], &local2[0], 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-best");
                return false;
            }
        }
        else
        {
            cl_kernel kernel = slot.kernelSelectList;
            cl_uint index = 0;
            cl_mem areasArg = slot.bufAreas;
            cl_mem smallArg = slot.bufSmall;
            cl_int maxAreaSlot = i;
            double f = spec.f;
            cl_int minArea = spec.minArea;
            cl_int w = W;
            cl_int h = H;
            cl_int ringOut = first.ringOut;
            cl_int countSlot = 2 * count + i;
            cl_mem candArg = slot.bufCand;
            cl_int candSlot = i * kCandidateCap * 2;
            cl_int cap = kCandidateCap;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &areasArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &smallArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &maxAreaSlot);
            s.api.clSetKernelArg(kernel, index++, sizeof(double), &f);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &minArea);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &w);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &h);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ringOut);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &smallArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &countSlot);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &candArg);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &candSlot);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &cap);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr,
                                                   &grid[0], &local2[0], 0, nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-select");
                return false;
            }
        }
    }

    /* one flush for the whole batch, one readback for all scratch slots. */
    const auto enqueueDoneAt = std::chrono::steady_clock::now();
    std::vector<cl_int> hostSmall(small.size());
    failure = s.api.clEnqueueReadBuffer(slot.queue, slot.bufSmall, CL_FALSE, 0,
                                        hostSmall.size() * sizeof(cl_int), hostSmall.data(), 0,
                                        nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("readback-slots");
        return false;
    }
    std::vector<cl_int> hostCand;
    if (!first.climate)
    {
        hostCand.resize(static_cast<size_t>(count) * kCandidateCap * 2);
        failure = s.api.clEnqueueReadBuffer(slot.queue, slot.bufCand, CL_FALSE, 0,
                                            hostCand.size() * sizeof(cl_int), hostCand.data(),
                                            0, nullptr, nullptr);
        if (failure != CL_SUCCESS)
        {
            noteFailure("readback-candidates");
            return false;
        }
    }
    const auto flushStart = std::chrono::steady_clock::now();
    failure = s.api.clFinish(slot.queue);
    if (failure != CL_SUCCESS)
    {
        noteFailure("flush");
        return false;
    }
    {
        const auto now = std::chrono::steady_clock::now();
        s.hostEnqueueNs.fetch_add((uint64_t) std::chrono::duration<double, std::nano>(
            flushStart - enqueueDoneAt).count());
        s.hostWaitNs.fetch_add((uint64_t) std::chrono::duration<double, std::nano>(
            now - flushStart).count());
    }

    for (int i = 0; i < count; i++)
    {
        const uint32_t maxArea = static_cast<uint32_t>(hostSmall[static_cast<size_t>(i)]);
        if (first.climate)
        {
            const uint32_t best = static_cast<uint32_t>(hostSmall[static_cast<size_t>(count) + i]);
            if (!bestOut)
                continue;
            if (maxArea == 0 || maxArea < static_cast<uint32_t>(specs[i].minArea)
                || best == 0xFFFFFFFFu)
            {
                bestOut[i] = Candidate{};
                continue;
            }
            const int cx = static_cast<int>(best >> 16);
            const int cz = static_cast<int>(best & 0xFFFFu);
            bestOut[i].x = specs[i].startX + cx * gridScale;
            bestOut[i].z = specs[i].startZ + cz * gridScale;
            bestOut[i].area = static_cast<int>(maxArea);
            continue;
        }

        const int used = hostSmall[static_cast<size_t>(2) * count + i];
        if (used > kCandidateCap)
        {
            /* Candidate list overflow: let the caller redo this window on the CPU. */
            s.fallbacks.fetch_add(1);
            return false;
        }
        outLists[i].clear();
        for (int c = 0; c < used; c++)
        {
            const size_t base = (static_cast<size_t>(i) * kCandidateCap + c) * 2;
            const int packed = hostCand[base];
            Candidate candidate;
            candidate.x = specs[i].startX + (packed >> 16) * gridScale;
            candidate.z = specs[i].startZ + (packed & 0xFFFF) * gridScale;
            candidate.area = hostCand[base + 1];
            outLists[i].push_back(candidate);
        }
    }

    s.submissions.fetch_add(1);
    s.cells.fetch_add(static_cast<uint64_t>(count) * (cells + static_cast<size_t>(cW) * cH));
    (void) s;
    return true;
}

} // namespace

bool evalChunkBatch(const Generator *g, const CoarseSpec *specs, int count,
                    std::vector<Candidate> *outLists, std::string *reason)
{
    (void) g;
    (void) reason;
    if (!outLists)
        return false;
    for (int i = 0; i < count; i++)
        outLists[i].clear();
    return runWindows(specs, count, outLists, nullptr);
}

bool evalClimate(const Generator *g, const CoarseSpec &spec, Candidate *out,
                 std::string *reason)
{
    (void) g;
    (void) reason;
    if (!out)
        return false;
    *out = Candidate{};
    Candidate best;
    if (!runWindows(&spec, 1, nullptr, &best))
        return false;
    *out = best;
    return out->area > 0;
}

} // namespace lowyd_gpu

namespace
{

/** Creates (once) the queue, kernel objects and scratch buffers of a slot. */
bool ensureSlot(Slot &slot)
{
    if (slot.queue)
        return true;
    State &s = state();
    std::lock_guard<std::mutex> lock(gpuMutex());
    if (slot.queue)
        return true;

    cl_int status = 0;
    slot.queue = s.api.clCreateCommandQueue(s.context, s.deviceId, 0, &status);
    if (!slot.queue || status != CL_SUCCESS)
        return false;
    slot.kernelCoarse = s.api.clCreateKernel(s.program, "k_coarse", &status);
    if (!slot.kernelCoarse || status != CL_SUCCESS)
        return false;
    slot.kernelDilate = s.api.clCreateKernel(s.program, "k_dilate", &status);
    if (!slot.kernelDilate || status != CL_SUCCESS)
        return false;
    slot.kernelCompact = s.api.clCreateKernel(s.program, "k_compact", &status);
    if (!slot.kernelCompact || status != CL_SUCCESS)
        return false;
    slot.kernelWeirdList = s.api.clCreateKernel(s.program, "k_weird_list", &status);
    if (!slot.kernelWeirdList || status != CL_SUCCESS)
        return false;
    slot.kernelClear = s.api.clCreateKernel(s.program, "k_clear", &status);
    if (!slot.kernelClear || status != CL_SUCCESS)
        return false;
    slot.kernelScanRows = s.api.clCreateKernel(s.program, "k_scan_rows", &status);
    if (!slot.kernelScanRows || status != CL_SUCCESS)
        return false;
    slot.kernelScanCols = s.api.clCreateKernel(s.program, "k_scan_cols", &status);
    if (!slot.kernelScanCols || status != CL_SUCCESS)
        return false;
    slot.kernelRingAreas = s.api.clCreateKernel(s.program, "k_ring_areas", &status);
    if (!slot.kernelRingAreas || status != CL_SUCCESS)
        return false;
    slot.kernelSelectList = s.api.clCreateKernel(s.program, "k_select_list", &status);
    if (!slot.kernelSelectList || status != CL_SUCCESS)
        return false;
    slot.kernelSelectBest = s.api.clCreateKernel(s.program, "k_select_best", &status);
    if (!slot.kernelSelectBest || status != CL_SUCCESS)
        return false;
    slot.kernelMaxH = s.api.clCreateKernel(s.program, "k_maxfilt_h", &status);
    if (!slot.kernelMaxH || status != CL_SUCCESS)
        return false;
    slot.kernelMaxV = s.api.clCreateKernel(s.program, "k_maxfilt_v", &status);
    if (!slot.kernelMaxV || status != CL_SUCCESS)
        return false;
    slot.bufCount = s.api.clCreateBuffer(s.context, CL_MEM_READ_WRITE, sizeof(cl_int), nullptr,
                                         &status);
    if (!slot.bufCount || status != CL_SUCCESS)
        return false;
    return true;
}

/** The calling thread's slot. Threads of a search are pinned for its duration. */
Slot &currentSlot()
{
    static thread_local int index = -1;
    static thread_local uint64_t generation = 0;
    State &s = state();
    const uint64_t gen = s.generation.load(std::memory_order_relaxed);
    if (index < 0 || generation != gen)
    {
        const int next = s.slotNext.fetch_add(1, std::memory_order_relaxed);
        index = next % kMaxSlots;
        generation = gen;
    }
    Slot &slot = s.slots[index];
    ensureSlot(slot);
    return slot;
}

bool uploadParams(cl_int &failure)
{
    State &s = state();
    const HostParams &p = s.params;

    if (!ensureBuffer(s.bufPerm, s.bufPermBytes, p.perm.size(), CL_MEM_READ_ONLY)
        || !ensureBuffer(s.bufFp, s.bufFpBytes, p.fp.size() * sizeof(float), CL_MEM_READ_ONLY)
        || !ensureBuffer(s.bufDp, s.bufDpBytes, p.dp.size() * sizeof(double), CL_MEM_READ_ONLY)
        || !ensureBuffer(s.bufDesc, s.bufDescBytes, sizeof(int) * 16, CL_MEM_READ_ONLY))
    {
        failure = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        return false;
    }

    failure = s.api.clEnqueueWriteBuffer(s.queue, s.bufDesc, CL_TRUE, 0, sizeof(int) * 16,
                                         s.params.gdesc, 0, nullptr, nullptr);
    if (failure != CL_SUCCESS)
        return false;

    failure = s.api.clEnqueueWriteBuffer(s.queue, s.bufPerm, CL_TRUE, 0, p.perm.size(),
                                         p.perm.data(), 0, nullptr, nullptr);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clEnqueueWriteBuffer(s.queue, s.bufFp, CL_TRUE, 0,
                                         p.fp.size() * sizeof(float), p.fp.data(), 0, nullptr,
                                         nullptr);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clEnqueueWriteBuffer(s.queue, s.bufDp, CL_TRUE, 0,
                                         p.dp.size() * sizeof(double), p.dp.data(), 0, nullptr,
                                         nullptr);
    return failure == CL_SUCCESS;
}

/** Rounds up to a multiple of the work-group size. */
size_t roundUp(size_t value, size_t multiple)
{
    return multiple > 1 ? ((value + multiple - 1) / multiple) * multiple : value;
}

/**
 * Queues one coarse-grid kernel. The caller must hold the GPU mutex and must
 * have uploaded the parameters.
 */
bool enqueueCoarse(Slot &slot, int startX, int startZ, int W, int H, int scale, int mode,
                   double threshold,
                   cl_mem mask, int maskStartX, int maskStartZ, int maskScale, int maskW,
                   int maskH, int outOffset, cl_mem out, cl_int &failure)
{
    State &s = state();
    cl_kernel kernel = slot.kernelCoarse;
    cl_uint index = 0;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufPerm);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufFp);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDp);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDesc);
    if (failure != CL_SUCCESS)
        return false;
    cl_mem maskArg = mask;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &maskArg);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &startX);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &startZ);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &scale);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &W);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &H);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &mode);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(double), &threshold);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &maskStartX);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &maskStartZ);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &maskScale);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &maskW);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &maskH);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &outOffset);
    if (failure != CL_SUCCESS)
        return false;

    const size_t local[2] = { static_cast<size_t>(s.wgx), static_cast<size_t>(s.wgy) };
    const size_t global[2] = { roundUp(static_cast<size_t>(W), local[0]),
                               roundUp(static_cast<size_t>(H), local[1]) };
    failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr, global, local, 0,
                                           nullptr, nullptr);
    return failure == CL_SUCCESS;
}

bool enqueueDilate(Slot &slot, int W, int H, int radius, cl_mem in, cl_mem out, int outOffset,
                   cl_int &failure)
{
    State &s = state();
    cl_kernel kernel = slot.kernelDilate;
    cl_uint index = 0;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &in);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &W);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &H);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &radius);
    if (failure != CL_SUCCESS)
        return false;
    failure = s.api.clSetKernelArg(kernel, index++, sizeof(int), &outOffset);
    if (failure != CL_SUCCESS)
        return false;

    const size_t local[2] = { static_cast<size_t>(s.wgx), static_cast<size_t>(s.wgy) };
    const size_t global[2] = { roundUp(static_cast<size_t>(W), local[0]),
                               roundUp(static_cast<size_t>(H), local[1]) };
    failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr, global, local, 0,
                                           nullptr, nullptr);
    return failure == CL_SUCCESS;
}

void noteFailure(const char *what)
{
    State &s = state();
    s.active.store(false);
    s.reason = std::string("runtime-failed:") + what;
    s.fallbacks.fetch_add(1);
}

// ---------------------------------------------------------------------------
// Submission model.
//
// One chunk costs ~20-30 us of device work but ~78-100 us of round-trip
// latency on Windows/WDDM (measured: 82 us for a 64x64 grid, 128 us for
// 256x256), so a submission is: enqueue every kernel of the chunk, enqueue the
// readbacks without blocking, then one clFinish on the worker's own queue.
// Every worker owns a queue and its own scratch buffers (see Slot), so those
// round trips overlap instead of serialising the search threads. Grouping
// several chunks per flush through one shared queue was implemented and
// measured first: it lost 20% end to end, because the wakeup cost of a
// dedicated submission thread is of the same order as the round trip it saves.
// ---------------------------------------------------------------------------

} // namespace

namespace lowyd_gpu
{

void setBackend(Backend backend)
{
    backendSlot().store(static_cast<int>(backend));
}

Backend backend()
{
    return static_cast<Backend>(backendSlot().load());
}

bool enabled()
{
    return state().active.load(std::memory_order_relaxed);
}

bool parseBackend(const char *text, Backend *out)
{
    if (!text || !out)
        return false;
    std::string value(text);
    for (char &c: value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (value == "auto")
    {
        *out = Backend::Auto;
        return true;
    }
    if (value == "cpu" || value == "none" || value == "off")
    {
        *out = Backend::Cpu;
        return true;
    }
    if (value == "gpu" || value == "opencl")
    {
        *out = Backend::Gpu;
        return true;
    }
    return false;
}

const char *backendName(Backend value)
{
    switch (value)
    {
    case Backend::Cpu: return "cpu";
    case Backend::Gpu: return "gpu";
    default: return "auto";
    }
}

const Info &info()
{
    static Info snapshot;
    const State &s = state();
    snapshot.probed = s.probed;
    snapshot.usable = s.ok;
    snapshot.device = s.device;
    snapshot.reason = s.reason;
    snapshot.maxWorkGroup = s.maxWorkGroup;
    snapshot.workGroupX = s.wgx;
    snapshot.workGroupY = s.wgy;
    snapshot.submissions = s.submissions.load();
    snapshot.cells = s.cells.load();
    snapshot.fallbacks = s.fallbacks.load();
    snapshot.hostEnqueueNs = s.hostEnqueueNs.load();
    snapshot.hostWaitNs = s.hostWaitNs.load();
    return snapshot;
}

bool activate(const Generator *g, bool allow)
{
    std::lock_guard<std::mutex> lock(gpuMutex());
    State &s = state();

    if (!allow || !g)
    {
        s.active.store(false);
        return false;
    }

    if (!s.probed)
    {
        std::string reason;
        if (!probe(&reason))
        {
            s.reason = reason;
            s.active.store(false);
            std::fprintf(stderr, "[gpu] backend unavailable: %s\n", reason.c_str());
            return false;
        }
        std::fprintf(stderr, "[gpu] device: %s\n", s.device.c_str());
    }
    if (!s.ok)
    {
        s.active.store(false);
        return false;
    }

    /* (Re)build the packed parameter block when seed or version changes. */
    const uint64_t seed = g->seed;
    const int mc = g->mc;
    if (s.seedUploaded != seed || s.mcUploaded != mc || s.params.perm.empty())
    {
        s.params = HostParams{};
        addNoise(s.params, g->bn.climate[NP_CONTINENTALNESS], 0);
        addNoise(s.params, g->bn.climate[NP_WEIRDNESS], 5);
        addNoise(s.params, g->bn.climate[NP_EROSION], 10);
        s.params.gdesc[15] = s.params.octaves;

        cl_int failure = CL_SUCCESS;
        if (!uploadParams(failure))
        {
            s.reason = "upload-failed:" + std::to_string(static_cast<int>(failure));
            s.active.store(false);
            std::fprintf(stderr, "[gpu] backend unavailable: %s\n", s.reason.c_str());
            return false;
        }
        s.seedUploaded = seed;
        s.mcUploaded = mc;
    }

    /* New search: re-pin the worker threads to their own slot. */
    s.slotNext.store(0, std::memory_order_relaxed);
    s.generation.fetch_add(1, std::memory_order_relaxed);
    s.submissions.store(0, std::memory_order_relaxed);
    s.cells.store(0, std::memory_order_relaxed);
    s.fallbacks.store(0, std::memory_order_relaxed);

    s.active.store(true);
    return true;
}

void deactivate()
{
    state().active.store(false);
}

namespace
{

/** Body of one grid job; runs on the GPU worker thread with the queue in order. */
bool evalGridLocked(const Generator *g, int startX, int startZ, int W, int H, int scale,
                    GridMode mode, double contThreshold,
                    const std::uint8_t *mask, int maskStartX, int maskStartZ, int maskScale,
                    int maskW, int maskH, std::vector<std::uint8_t> &out)
{
    (void) g;
    State &s = state();
    if (!s.active.load() || !s.ok)
        return false;
    if (W <= 0 || H <= 0 || scale <= 0)
        return false;

    const size_t cells = static_cast<size_t>(W) * static_cast<size_t>(H);
    /* Tiny grids are not worth a submission: leave them to the CPU. */
    if (cells < 1024)
        return false;
    Slot &slot = currentSlot();
    if (!slot.queue)
        return false;

    if (!ensureBuffer(slot.bufGrid, slot.gridBytes, cells, CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-grid");
        return false;
    }
    const size_t maskBytes = mask ? static_cast<size_t>(maskW) * static_cast<size_t>(maskH) : 0;
    /* The kernel always binds a mask buffer; a zero maskScale disables the test. */
    if (!ensureBuffer(slot.bufMask, slot.maskBytes, maskBytes > 0 ? maskBytes : 1,
                      CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-mask");
        return false;
    }

    cl_int failure = CL_SUCCESS;
    if (maskBytes > 0)
    {
        failure = s.api.clEnqueueWriteBuffer(slot.queue, slot.bufMask, CL_TRUE, 0, maskBytes, mask, 0,
                                             nullptr, nullptr);
        if (failure != CL_SUCCESS)
        {
            noteFailure("write-mask");
            return false;
        }
    }

    if (!enqueueCoarse(slot, startX, startZ, W, H, scale, static_cast<int>(mode), contThreshold,
                       slot.bufMask,
                       maskStartX, maskStartZ, maskBytes > 0 ? maskScale : 0,
                       maskBytes > 0 ? maskW : 0, maskBytes > 0 ? maskH : 0, 0,
                       slot.bufGrid, failure))
    {
        noteFailure("kernel");
        return false;
    }

    out.resize(cells);
    failure = s.api.clEnqueueReadBuffer(slot.queue, slot.bufGrid, CL_FALSE, 0, cells, out.data(), 0,
                                        nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("readback");
        return false;
    }
    s.submissions.fetch_add(1);
    s.cells.fetch_add(cells);
    return true;
}

/** Body of one phase-1 chunk job. */
bool evalChunkLocked(const Generator *g, int startX, int startZ, int chunkW, int chunkH,
                     int contScale, int weirdScale, double contThreshold, int dilate,
                     std::vector<std::uint8_t> &contMask,
                     std::vector<std::uint8_t> &weirdGrid)
{
    (void) g;
    State &s = state();
    if (!s.active.load() || !s.ok)
        return false;
    if (chunkW <= 0 || chunkH <= 0 || contScale <= 0 || weirdScale <= 0)
        return false;

    const int cW = chunkW / contScale;
    const int cH = chunkH / contScale;
    const int wW = chunkW / weirdScale;
    const int wH = chunkH / weirdScale;
    if (cW <= 0 || cH <= 0 || wW <= 0 || wH <= 0)
        return false;
    Slot &slot = currentSlot();
    if (!slot.queue)
        return false;

    const size_t contCells = static_cast<size_t>(cW) * static_cast<size_t>(cH);
    const size_t weirdCells = static_cast<size_t>(wW) * static_cast<size_t>(wH);
    const bool compact = weirdScale <= contScale && contScale % weirdScale == 0
        && contScale / weirdScale <= 16;
    const int ratio = compact ? contScale / weirdScale : 1;

    /* One readback: the dilated mask followed by the weirdness grid, so a chunk
     * costs a single host<->device round trip. */
    const size_t comboBytes = contCells + weirdCells;
    if (!ensureBuffer(slot.bufRaw, slot.rawBytes, contCells, CL_MEM_READ_WRITE)
        || !ensureBuffer(slot.bufGrid, slot.gridBytes, comboBytes, CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-chunk");
        return false;
    }
    if (compact && !ensureBuffer(slot.bufList, slot.listBytes, contCells * sizeof(cl_int),
                                 CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-list");
        return false;
    }
    cl_int failure = CL_SUCCESS;
    /* 1. raw continentalness grid at `contScale` */
    if (!enqueueCoarse(slot, startX, startZ, cW, cH, contScale, static_cast<int>(GRID_CONT),
                       contThreshold, nullptr, startX, startZ, 0, 0, 0, 0, slot.bufRaw, failure))
    {
        noteFailure("kernel-cont");
        return false;
    }
    /* 2. Chebyshev dilation (the same mask the CPU builder returns), written to
     *    the head of the combo buffer. */
    if (!enqueueDilate(slot, cW, cH, dilate, slot.bufRaw, slot.bufGrid, 0, failure))
    {
        noteFailure("kernel-dilate");
        return false;
    }

    if (compact)
    {
        /* 3a. zero the weirdness region, 3b. compact the set mask tiles,
         * 3c. evaluate only the cells of the listed tiles. */
        static const cl_int zero = 0;
        failure = s.api.clEnqueueWriteBuffer(slot.queue, slot.bufCount, CL_FALSE, 0, sizeof(cl_int),
                                             &zero, 0, nullptr, nullptr);
        if (failure != CL_SUCCESS)
        {
            noteFailure("reset-count");
            return false;
        }
        {
            cl_kernel kernel = slot.kernelClear;
            cl_uint index = 0;
            cl_mem out = slot.bufGrid;
            cl_int count = static_cast<cl_int>(weirdCells);
            cl_int offset = static_cast<cl_int>(contCells);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &count);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &offset);
            const size_t local = 64;
            const size_t global = roundUp(static_cast<size_t>(weirdCells), local);
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 1, nullptr, &global, &local, 0,
                                                   nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-clear");
                return false;
            }
        }
        {
            cl_kernel kernel = slot.kernelCompact;
            cl_uint index = 0;
            cl_mem mask = slot.bufGrid;
            cl_int cw = cW;
            cl_int ch = cH;
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &mask);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufList);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufCount);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &cw);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ch);
            const size_t local[2] = { static_cast<size_t>(s.wgx),
                                      static_cast<size_t>(s.wgy) };
            const size_t global[2] = { roundUp(static_cast<size_t>(cW), local[0]),
                                       roundUp(static_cast<size_t>(cH), local[1]) };
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr, global, local, 0,
                                                   nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-compact");
                return false;
            }
        }
        {
            cl_kernel kernel = slot.kernelWeirdList;
            cl_uint index = 0;
            cl_mem out = slot.bufGrid;
            cl_int kStartX = startX;
            cl_int kStartZ = startZ;
            cl_int wScale = weirdScale;
            cl_int ww = wW;
            cl_int wh = wH;
            cl_int cw = cW;
            cl_int r = ratio;
            cl_int outOffset = static_cast<cl_int>(contCells);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufPerm);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufFp);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDp);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &s.bufDesc);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufList);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &slot.bufCount);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_mem), &out);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &kStartX);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &kStartZ);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &wScale);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &ww);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &wh);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &cw);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &r);
            s.api.clSetKernelArg(kernel, index++, sizeof(cl_int), &outOffset);
            const size_t local[2] = { static_cast<size_t>(s.wgx),
                                      static_cast<size_t>(s.wgy) };
            const size_t global[2] = { roundUp(static_cast<size_t>(ratio * ratio), local[0]),
                                       roundUp(static_cast<size_t>(contCells), local[1]) };
            failure = s.api.clEnqueueNDRangeKernel(slot.queue, kernel, 2, nullptr, global, local, 0,
                                                   nullptr, nullptr);
            if (failure != CL_SUCCESS)
            {
                noteFailure("kernel-weird-list");
                return false;
            }
        }
    }
    else
    {
        cl_mem mask = slot.bufGrid;
        if (!enqueueCoarse(slot, startX, startZ, wW, wH, weirdScale, static_cast<int>(GRID_WEIRD), 0.0,
                           mask, startX, startZ, contScale, cW, cH,
                           static_cast<cl_int>(contCells), slot.bufGrid, failure))
        {
            noteFailure("kernel-weird");
            return false;
        }
    }

    contMask.resize(contCells);
    weirdGrid.resize(weirdCells);
    /* Non-blocking: the worker flushes once for the whole batch, and the caller
     * only touches these buffers after that flush. */
    failure = s.api.clEnqueueReadBuffer(slot.queue, slot.bufGrid, CL_FALSE, 0, contCells,
                                        contMask.data(), 0, nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("readback-mask");
        return false;
    }
    failure = s.api.clEnqueueReadBuffer(slot.queue, slot.bufGrid, CL_FALSE, contCells, weirdCells,
                                        weirdGrid.data(), 0, nullptr, nullptr);
    if (failure != CL_SUCCESS)
    {
        noteFailure("readback-grid");
        return false;
    }

    s.submissions.fetch_add(1);
    s.cells.fetch_add(contCells + weirdCells);
    return true;
}

} // namespace

bool evalGrid(const Generator *g, int startX, int startZ, int W, int H, int scale, GridMode mode,
              double contThreshold,
              const std::uint8_t *mask, int maskStartX, int maskStartZ, int maskScale, int maskW,
              int maskH,
              std::vector<std::uint8_t> &out, std::string *reason)
{
    (void) reason;
    State &s = state();
    if (!s.active.load())
        return false;
    if (!evalGridLocked(g, startX, startZ, W, H, scale, mode, contThreshold, mask,
                        maskStartX, maskStartZ, maskScale, maskW, maskH, out))
        return false;
    if (s.api.clFinish(currentSlot().queue) != CL_SUCCESS)
    {
        noteFailure("flush");
        return false;
    }
    return true;
}

bool evalChunk(const Generator *g, int startX, int startZ, int chunkW, int chunkH, int contScale,
               int weirdScale, double contThreshold, int dilate,
               std::vector<std::uint8_t> &contMask, std::vector<std::uint8_t> &weirdGrid,
               std::string *reason)
{
    (void) reason;
    State &s = state();
    if (!s.active.load())
        return false;
    if (!evalChunkLocked(g, startX, startZ, chunkW, chunkH, contScale, weirdScale,
                         contThreshold, dilate, contMask, weirdGrid))
        return false;
    if (s.api.clFinish(currentSlot().queue) != CL_SUCCESS)
    {
        noteFailure("flush");
        return false;
    }
    return true;
}

double benchmarkGrid(int side, double *latencyMicros)
{
    if (latencyMicros)
        *latencyMicros = 0.0;
    State &s = state();
    if (!s.active.load() || !s.ok || side < 64)
        return 0.0;

    /* Slot first: ensureSlot() takes the GPU mutex, so this thread must not
     * already hold it. */
    Slot &slot = currentSlot();
    if (!slot.queue)
        return 0.0;
    std::lock_guard<std::mutex> lock(gpuMutex());

    const size_t cells = static_cast<size_t>(side) * static_cast<size_t>(side);
    if (!ensureBuffer(slot.bufGrid, slot.gridBytes, cells, CL_MEM_READ_WRITE)
        || !ensureBuffer(slot.bufMask, slot.maskBytes, 1, CL_MEM_READ_WRITE))
    {
        noteFailure("alloc-bench");
        return 0.0;
    }

    static std::vector<std::uint8_t> readback;
    readback.resize(cells);

    const int repeats = cells > (size_t) 1 << 20 ? 3 : 6;
    cl_int failure = CL_SUCCESS;
    double best = 0.0;
    double firstMicros = 0.0;
    for (int rep = 0; rep < repeats; rep++)
    {
        /* startX/startZ chosen on the climate lattice of a typical window. */
        const int startX = -30000128;
        const int startZ = -30000128;
        const auto begin = std::chrono::steady_clock::now();
        if (!enqueueCoarse(slot, startX, startZ, side, side, 32, static_cast<int>(GRID_WEIRD), 0.0,
                           slot.bufMask, startX, startZ, 0, 0, 0, 0, slot.bufGrid, failure))
        {
            noteFailure("bench-kernel");
            return 0.0;
        }
        if (s.api.clEnqueueReadBuffer(slot.queue, slot.bufGrid, CL_TRUE, 0, cells, readback.data(), 0,
                                      nullptr, nullptr) != CL_SUCCESS)
        {
            noteFailure("bench-readback");
            return 0.0;
        }
        const double micros = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - begin).count();
        if (rep == 0)
            firstMicros = micros;
        const double rate = static_cast<double>(cells) / (micros * 1e-6);
        if (rate > best)
            best = rate;
    }
    if (latencyMicros)
        *latencyMicros = firstMicros;
    return best;
}

} // namespace lowyd_gpu