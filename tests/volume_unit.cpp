//
// B4: analytic unit tests for the B1 shell model, the layer average and the
// layer window / AFK Y validation.
//
// The aggregation case: a per-layer shell sum of exactly k of L layers of one
// biome must report lround(49659 * k / L) as the per-layer average.  The shell
// cases pin the exact discrete 24..128 cross-section.  These are equations,
// not frozen outputs, so they stay valid for every layer count and weight.
//

#include "lowydripstonecavefinder.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char *what, const std::string &detail)
{
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", what, detail.c_str());
}

long long expectedAverage(int full, int k, int layers)
{
    return std::lround((double) full * (double) k / (double) layers);
}

void checkAnalyticCase(int layers, float riverWeight)
{
    const int full = (int) lowyd::RIVER_AREA_FULL;
    for (int caveLayers = 0; caveLayers <= layers; ++caveLayers)
    {
        const int riverLayers = layers - caveLayers;
        const int sumCave = full * caveLayers;
        const int sumRiver = full * riverLayers;
        int caveAvg = -1;
        int riverAvg = -1;
        const int total = lowyd::aggregateLayerAverages(sumCave, sumRiver, riverWeight, layers,
                                                        &caveAvg, &riverAvg);

        const long long expectCave = expectedAverage(full, caveLayers, layers);
        const long long expectRiver = expectedAverage(full, riverLayers, layers);
        const long long expectTotal = std::lround(
            ((double) sumCave + (double) riverWeight * (double) sumRiver) / (double) layers);
        const std::string where = "L=" + std::to_string(layers) + " k="
            + std::to_string(caveLayers) + " w=" + std::to_string(riverWeight);

        check(caveAvg == expectCave, "cave average", where);
        check(riverAvg == expectRiver, "river average", where);
        check(total == expectTotal, "weighted total", where);
        check(total >= 0 && total <= full, "total in 0..49659", where);
    }
}

void checkRangeValidation()
{
    lowyd::SearchParams params;
    std::string error;

    check(lowyd::validate(params, &error), "defaults valid", error);
    check(params.yMin == -63 && params.yMax == -51, "defaults are -63..-51", "");

    params.yMax = -16; // L = 48
    check(lowyd::validate(params, &error), "L=48 valid", error);

    params.yMax = -15; // L = 49
    check(!lowyd::validate(params, &error), "L=49 rejected", error);
    check(!error.empty(), "L=49 has message", "");

    params.yMin = -64;
    params.yMax = -51;
    check(!lowyd::validate(params, &error), "yMin<-63 rejected", error);

    params.yMin = -51;
    params.yMax = 320;
    check(!lowyd::validate(params, &error), "yMax>319 rejected", error);

    params.yMin = -50;
    params.yMax = -55;
    check(!lowyd::validate(params, &error), "inverted range rejected", error);
    check(!error.empty(), "inverted range has message", "");
}

void checkModelIds()
{
    check(lowyd::modelId(-63, -51, lowyd::AFK_Y_DEFAULT)
              == "drowned-weighted-shell-b/13/afk-40",
          "default model id", "");
    check(lowyd::modelId(-58, -54, lowyd::AFK_Y_DEFAULT)
              == "drowned-weighted-shell-b/5/afk-40",
          "5-layer model id", "");
    check(lowyd::modelId(-63, -16, lowyd::AFK_Y_DEFAULT)
              == "drowned-weighted-shell-b/48/afk-40",
          "48-layer model id", "");
    check(lowyd::modelId(-63, -51, -40) == "drowned-weighted-shell-b/13/afk-40",
          "custom AFK Y model id", "");
}

/** Shell cross-sections: 24 < distance <= 128 around (x, afkY, z). */
void checkShellGeometry()
{
    int outer = 0;
    int inner = 0;

    /* Default window and AFK Y=-40: dy = -23..-11 for layers -63..-51. */
    check(lowyd::shellRingRadii(-63, -40, outer, inner) && outer == 125 && inner == 6,
          "shell radii layer -63", std::to_string(outer) + "/" + std::to_string(inner));    check(lowyd::shellRingRadii(-51, -40, outer, inner) && outer == 127 && inner == 21,
          "shell radii layer -51", std::to_string(outer) + "/" + std::to_string(inner));
    check(lowyd::shellRingRadii(-40, -40, outer, inner) && outer == 128 && inner == 24,
          "shell radii at AFK Y", std::to_string(outer) + "/" + std::to_string(inner));
    /* |dy| = 128 keeps only the point straight above/below the AFK point. */
    check(lowyd::shellRingRadii(-168, -40, outer, inner) && outer == 0 && inner == -1,
          "shell radii |dy|=128", std::to_string(outer) + "/" + std::to_string(inner));
    /* Outside the shell: no cells. */
    check(!lowyd::shellRingRadii(100, -40, outer, inner), "|dy|>128 has no shell cells", "");

    /* Per-row radii are exact (layer-level floor() would under-estimate here). */
    check(lowyd::shellRowRadii(-23, 125, outer, inner) && outer == 15 && inner == -1,
          "exact shell row dy=-23 dz=125", std::to_string(outer) + "/" + std::to_string(inner));
    check(!lowyd::shellRowRadii(0, 129, outer, inner), "row outside sphere", "");

    lowyd::SearchParams params;
    std::string error;
    check(lowyd::validate(params, &error), "default AFK Y valid", error);
    params.afkY = lowyd::AFK_Y_LIMIT_MIN - 1;
    check(!lowyd::validate(params, &error), "AFK Y below limit rejected", error);
    params.afkY = lowyd::AFK_Y_LIMIT_MAX + 1;
    check(!lowyd::validate(params, &error), "AFK Y above limit rejected", error);
    params.afkY = lowyd::AFK_Y_DEFAULT;
    check(lowyd::validate(params, &error), "AFK Y restored valid", error);

    /* C platform: -63..-39 are all within ±24 of the layer window; the
     * default -40 is on the platform, -37 is just outside it. */
    check(lowyd::shellPlatformPlayerY(-63, -51) == -63, "platform anchor", "");
    check(lowyd::onShellPlatform(-63, -51, -39), "platform edge -39", "");
    check(lowyd::onShellPlatform(-63, -51, -40), "platform -40", "");
    check(lowyd::onShellPlatform(-63, -51, lowyd::AFK_Y_DEFAULT),
          "default -40 is on platform", "");
    check(!lowyd::onShellPlatform(-63, -51, -37), "-37 is off platform", "");
}

/** Display denominator: derived from the shell geometry, not fitted. */
void checkShellDenominator()
{
    check(lowyd::RIVER_AREA_FULL_CELLS == 49659, "default denominator is 49659",
          std::to_string(lowyd::RIVER_AREA_FULL_CELLS));
    check(lowyd::fullShellArea(-63, -51, lowyd::defaultAfkY(-63)) == 49659,
          "denominator follows defaultAfkY", "");
    check(lowyd::fullShellArea(-63, -51, -40) == 49659, "denominator at afkY=-40", "");

    /* One layer is exactly that layer's 24..128 cross-section. */
    check(lowyd::shellLayerCellCount(0) == 49640, "dy=0 cross-section",
          std::to_string(lowyd::shellLayerCellCount(0)));
    check(lowyd::fullShellArea(-63, -63, -63) == 49640, "single layer at dy=0", "");

    /* AFK Y defaults to the lowest sampled layer + 23 and tracks yMin. */
    check(lowyd::resolveAfkY(-63, lowyd::AFK_Y_AUTO) == -40, "auto afkY default", "");
    check(lowyd::resolveAfkY(-60, lowyd::AFK_Y_AUTO) == -37, "auto afkY tracks yMin", "");
    check(lowyd::resolveAfkY(-63, -30) == -30, "explicit afkY wins", "");

    /* Default river weight is the page default (0.7), not the legacy 0.75. */
    const lowyd::SearchParams defaults;
    check(std::fabs(defaults.riverWeight - 0.7f) < 1e-6f, "default river weight 0.7",
          std::to_string(defaults.riverWeight));
}

} // namespace

int main()
{
    for (int layers : {1, 3, 5, 9, 13, 24, 48})
        for (float weight : {0.0f, 0.7f, 0.75f, 1.0f})
            checkAnalyticCase(layers, weight);

    checkRangeValidation();
    checkShellGeometry();
    checkModelIds();
    checkShellDenominator();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("shell geometry, layer window and model id checks passed\n");
    return 0;
}
