//
// Table-gradient Perlin sampler.
//
// `samplePerlinTable()` evaluates exactly the same field as cubiomes'
// samplePerlin(): same hash chain, same smoothing, same summation order
// ((gx*a + gy*b) + gz*c, where a zero coefficient contributes an exact 0).
// The only difference is that the gradient dot product comes from a 16x3 table
// instead of the 16-way switch in indexedLerp(), which removes an
// unpredictable branch on the hashed index from the hot loop.
//
// Measured on the coarse climate path: 1.169x end to end, hit lists identical.
// It lives here, not in cubiomes/, because cubiomes is an upstream submodule
// pinned to a fixed commit and must not carry local patches.
//
#ifndef LOWYD_PERLIN_TABLE_H
#define LOWYD_PERLIN_TABLE_H

#include "cubiomes/noise.h"

#include <cmath>

namespace lowyd
{

/** Same expression as cubiomes' lerp(): from + part * (to - from). */
inline double perlinLerp(double part, double from, double to)
{
    return from + part * (to - from);
}

/** The sixteen gradient directions of indexedLerp(), rows 12..15 repeated. */
inline double gradientTable(uint8_t index, double a, double b, double c)
{
    static const double g[16][3] = {
        { 1.0,  1.0,  0.0}, {-1.0,  1.0,  0.0},
        { 1.0, -1.0,  0.0}, {-1.0, -1.0,  0.0},
        { 1.0,  0.0,  1.0}, {-1.0,  0.0,  1.0},
        { 1.0,  0.0, -1.0}, {-1.0,  0.0, -1.0},
        { 0.0,  1.0,  1.0}, { 0.0, -1.0,  1.0},
        { 0.0,  1.0, -1.0}, { 0.0, -1.0, -1.0},
        { 1.0,  1.0,  0.0}, { 0.0, -1.0,  1.0},
        {-1.0,  1.0,  0.0}, { 0.0, -1.0, -1.0}
    };
    const double *v = g[index & 15];
    return (v[0] * a + v[1] * b) + v[2] * c;
}

/**
 * Bit-identical to samplePerlin(noise, x, y, z, 0, 0).
 * The yamp/ymax terrain parameters are not supported; every call site in this
 * engine samples the climate at y = 0.
 */
inline double samplePerlinTable(const PerlinNoise *noise, double d1, double d2, double d3)
{
    uint8_t h1, h2, h3;
    double t1, t2, t3;

    /* y == 0: reuse the per-octave precomputed offset, exactly like
     * samplePerlin()'s d2 == 0.0 branch. */
    if (d2 == 0.0)
    {
        d2 = noise->d2;
        h2 = noise->h2;
        t2 = noise->t2;
    }
    else
    {
        d2 += noise->b;
        const double i2 = std::floor(d2);
        d2 -= i2;
        h2 = (uint8_t) (int) i2;
        t2 = d2*d2*d2 * (d2 * (d2*6.0-15.0) + 10.0);
    }

    d1 += noise->a;
    d3 += noise->c;

    const double i1 = std::floor(d1);
    const double i3 = std::floor(d3);
    d1 -= i1;
    d3 -= i3;

    h1 = (uint8_t) (int) i1;
    h3 = (uint8_t) (int) i3;

    t1 = d1*d1*d1 * (d1 * (d1*6.0-15.0) + 10.0);
    t3 = d3*d3*d3 * (d3 * (d3*6.0-15.0) + 10.0);

    const uint8_t *idx = noise->d;

    /* Same hash chain as samplePerlin(); the +1 lookups rely on idx[256] == idx[0]. */
    const uint8_t a1 = (uint8_t) (idx[h1]      + h2);
    const uint8_t b1 = (uint8_t) (idx[h1 + 1]  + h2);
    const uint8_t a2 = (uint8_t) (idx[a1]      + h3);
    const uint8_t b2 = (uint8_t) (idx[a1 + 1]  + h3);
    const uint8_t c2 = (uint8_t) (idx[b1]      + h3);
    const uint8_t d2h = (uint8_t) (idx[b1 + 1] + h3);

    const uint8_t e1 = idx[a2],  e2 = idx[a2 + 1];
    const uint8_t e3 = idx[b2],  e4 = idx[b2 + 1];
    const uint8_t e5 = idx[c2],  e6 = idx[c2 + 1];
    const uint8_t e7 = idx[d2h], e8 = idx[d2h + 1];

    double l1 = gradientTable(e1, d1,   d2,   d3);
    double l5 = gradientTable(e2, d1,   d2,   d3-1);
    double l2 = gradientTable(e5, d1-1, d2,   d3);
    double l6 = gradientTable(e6, d1-1, d2,   d3-1);
    double l3 = gradientTable(e3, d1,   d2-1, d3);
    double l7 = gradientTable(e4, d1,   d2-1, d3-1);
    double l4 = gradientTable(e7, d1-1, d2-1, d3);
    double l8 = gradientTable(e8, d1-1, d2-1, d3-1);

    l1 = perlinLerp(t1, l1, l2);
    l3 = perlinLerp(t1, l3, l4);
    l5 = perlinLerp(t1, l5, l6);
    l7 = perlinLerp(t1, l7, l8);

    l1 = perlinLerp(t2, l1, l3);
    l5 = perlinLerp(t2, l5, l7);

    return perlinLerp(t3, l1, l5);
}

} // namespace lowyd

#endif // LOWYD_PERLIN_TABLE_H
