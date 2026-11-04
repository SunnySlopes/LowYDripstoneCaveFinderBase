//
// JNI bridge for the LowYDripstoneCaveFinder Java GUI.
//
// The bridge is intentionally thin: every search / weirdness computation lives
// in lowydripstonecavefinder.cpp. This file only translates the stable JNI
// contract into engine calls and flattens the result vectors into int arrays.
//

#include "generated/sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.h"

#include "lowydripstonecavefinder.h"

#include "cubiomes/biomes.h"
#include "cubiomes/util.h"

#include <cstdint>
#include <vector>

namespace {

int parseMcVersion(JNIEnv *env, jstring mcVersion)
{
    int mc = MC_26_1;
    if (mcVersion == nullptr)
        return mc;

    const char *mcStr = env->GetStringUTFChars(mcVersion, nullptr);
    if (mcStr != nullptr)
    {
        const int parsed = str2mc(mcStr);
        if (parsed != MC_UNDEF)
            mc = parsed;
        env->ReleaseStringUTFChars(mcVersion, mcStr);
    }
    return mc;
}

jintArray toIntArray(JNIEnv *env, const std::vector<lowyd::Hit> &hits)
{
    if (hits.empty())
        return nullptr;

    std::vector<jint> results;
    results.reserve(hits.size() * 5);
    for (const auto &hit: hits)
    {
        results.push_back(hit.x);
        results.push_back(hit.z);
        results.push_back(hit.total);
        results.push_back(hit.cave);
        results.push_back(hit.river);
    }

    jintArray jResults = env->NewIntArray(static_cast<jsize>(results.size()));
    if (!jResults)
        return nullptr;
    env->SetIntArrayRegion(jResults, 0, static_cast<jsize>(results.size()), results.data());
    return jResults;
}

} // namespace

JNIEXPORT jintArray JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_riverSearch
  (JNIEnv *env, jclass, jlong seed, jint startX, jint startZ,
   jint width, jint height, jint yMin, jint yMax, jint coarseY, jint afkY, jint minArea,
   jfloat opV, jfloat riverWeight, jint numThreads,
   jstring mcVersion, jint contScale, jint weirdScale, jint searchMode, jboolean allowGpu)
{
    lowyd::SearchParams params;
    params.seed = static_cast<uint64_t>(seed);
    params.startX = startX;
    params.startZ = startZ;
    params.width = width;
    params.height = height;
    params.yMin = yMin;
    params.yMax = yMax;
    params.afkY = afkY;
    params.coarseY = coarseY;
    params.minArea = minArea;
    params.preserveRange = opV;
    params.riverWeight = riverWeight;
    params.numThreads = numThreads;
    params.mc = parseMcVersion(env, mcVersion);
    params.contScale = contScale;
    params.weirdScale = weirdScale;
    params.searchMode = searchMode;
    params.allowGpu = (allowGpu != JNI_FALSE) ? 1 : 0;

    /* The GUI validates the layer window; a bad one yields null (no hits). */
    return toIntArray(env, lowyd::search(params));
}

JNIEXPORT jintArray JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_getSearchProgress
  (JNIEnv *env, jclass)
{
    const lowyd::ProgressView view = lowyd::getProgress();
    if (!view.available)
        return nullptr;

    jint data[6] = {
        view.current, view.total, view.phase, view.status, view.tryPause, view.tryStop
    };
    jintArray arr = env->NewIntArray(6);
    if (!arr)
        return nullptr;
    env->SetIntArrayRegion(arr, 0, 6, data);
    return arr;
}

JNIEXPORT jintArray JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_getNowResult
  (JNIEnv *env, jclass)
{
    return toIntArray(env, lowyd::getNowResults());
}

JNIEXPORT jboolean JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_pause(JNIEnv *, jclass)
{
    lowyd::requestPause();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_resume(JNIEnv *, jclass)
{
    lowyd::requestResume();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_stop(JNIEnv *, jclass)
{
    lowyd::requestStop();
    return JNI_TRUE;
}

JNIEXPORT jfloat JNICALL Java_sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge_weirdnessYOffset
  (JNIEnv *, jclass, jlong seed)
{
    return static_cast<jfloat>(lowyd::weirdnessYOffset(static_cast<uint64_t>(seed)));
}
