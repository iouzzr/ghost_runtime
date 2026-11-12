#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <math.h>
#include <float.h>
#include <vector>

// ============================================================================
// Xbox DirectSoundBuffer Structures & Constants
// ============================================================================

#pragma pack(push, 1)
struct XBOX_DSBUFFERDESC {
    uint32_t            dwSize;
    uint32_t            dwFlags;
    uint32_t            dwBufferBytes;
    const WAVEFORMATEX* lpwfxFormat;
    const void*         lpMixBins;
    uint32_t            dwInputMixBin;
};
#pragma pack(pop)

static constexpr uint32_t kDefaultStreamBytesPerSec = 192000u;
static constexpr DWORD    kXboxDsbStatusPlaying     = 0x00000001u; // DSBSTATUS_PLAYING
static constexpr DWORD    kXboxDsbStatusLooping     = 0x00000004u; // DSBSTATUS_LOOPING (verified at 0x006E81D3)
static constexpr float    kMillibelsPerTwentyDb     = 2000.0f;

static XBOX_DSI3DL2LISTENER g_i3dl2Listener = {
    -10000, 0, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f
};
static XBOX_DSI3DL2LISTENER g_i3dl2ListenerDeferred = g_i3dl2Listener;
static bool                 g_hasDeferredI3DL2      = false;

const XBOX_DSI3DL2LISTENER& GetXboxI3DL2ListenerState() {
    return g_i3dl2Listener;
}

CRITICAL_SECTION g_audioCs;
volatile LONG    g_audioCsInitState = 0;

void EnsureXboxAudioCsInitialized() {
    if (InterlockedCompareExchange(&g_audioCsInitState, 0, 0) == 2) return;
    if (InterlockedCompareExchange(&g_audioCsInitState, 1, 0) == 0) {
        InitializeCriticalSection(&g_audioCs);
        InterlockedExchange(&g_audioCsInitState, 2);
    } else {
        while (InterlockedCompareExchange(&g_audioCsInitState, 0, 0) != 2) {
            SwitchToThread();
        }
    }
}

static inline void EnsureAudioLockInitialized() {
    EnsureXboxAudioCsInitialized();
}

struct XboxDSBufferObject {
    void**                  vtable;   // +0x00: 4-entry Xbox IDirectSoundBuffer vtable
    LONG                    refCount; // +0x04
    WAVEFORMATEX            wfx;
    bool                    isAdpcm;
    uint8_t*                rawBuf;
    uint32_t                rawSize;
    bool                    ownsRawBuf;
    uint32_t                playStart;
    uint32_t                playLength;
    uint32_t                loopStart;
    uint32_t                loopLength;
    double                  playFramePos;
    bool                    hasExplicitStartPos;
    LONG                    volume;
    LONG                    headroomMb;
    LONG                    pan;
    LONG                    mixBinAttenMb;
    LONG                    atten3D;
    LONG                    lPitch;
    DWORD                   freq;
    DWORD                   status;
    uint32_t                lockCount;
    bool                    isStreamingLockBuf;
    bool                    isLooping;
    bool                    isReleasing;
    bool                    releaseOnlyEnvelope;
    bool                    use3DVoiceData;
    bool                    hasI3DL2Send;
    int32_t                 i3dl2SendMb;
    uint32_t                fxSendBinMask; // Bits [0..20] correspond to MixBins 11..31
    uint32_t                inputMixBin;   // 0xFFFFFFFFu unless configured as FXIN/submix target
    XBOX_DSMIXBINVOLUMEPAIR mixBinPairs[8];
    uint32_t                mixBinCount;
    Xbox3DVoiceMetrics      voice3D;
    XBOX_DSFILTERDESC       filterDesc;
    bool                    hasActiveFilter;
    XboxVoiceFilterState    filterState;
    XBOX_DSLFODESC          lfo[2];
    bool                    lfoActive[2];
    XBOX_DSENVELOPEDESC     eg[2];
    bool                    egActive[2];
    XboxDSBufferObject*     outputBuffer;
    uint64_t                activeSamples48k;
    uint64_t                releaseSamples48k;

    // Zero-allocation 64-frame decoded block cache for Xbox IMA ADPCM voices
    uint32_t                cachedAdpcmBlockIdx;
    float                   cachedAdpcmL[64];
    float                   cachedAdpcmR[64];

    // Pre-allocated submix stereo bus (allocated lazily on API call, never on mixer thread)
    std::vector<float>      submixL;
    std::vector<float>      submixR;
    bool                    submixHasAudio;
};

static std::vector<XboxDSBufferObject*> g_activeBuffers;

static uint32_t ExtractFxSendBinMask(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count) {
    if (!pairs) return 0u;
    if (count > 8u) count = 8u;
    uint32_t mask = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t bin = pairs[i].dwMixBin;
        if (bin >= 11u && bin <= 31u && pairs[i].lVolume > -9000) {
            mask |= (1u << (bin - 11u));
        }
    }
    return mask;
}

static void EnsureSubmixBusAllocatedLocked(XboxDSBufferObject* submix) {
    if (!submix) return;
    if (submix->submixL.size() < kAudioMaxQuantumFrames) {
        submix->submixL.assign(kAudioMaxQuantumFrames, 0.0f);
        submix->submixR.assign(kAudioMaxQuantumFrames, 0.0f);
    }
}

void EnsureXboxBufferSubmixReadyLocked(XboxDSBufferObject* submix) {
    EnsureSubmixBusAllocatedLocked(submix);
}

void EnsureXboxFxSendTargetsAllocatedLocked(uint32_t fxSendBinMask) {
    if (fxSendBinMask == 0u) return;
    for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
        XboxDSBufferObject* sb = g_activeBuffers[i];
        if (!sb || sb->inputMixBin < 11u || sb->inputMixBin > 31u) continue;
        if ((fxSendBinMask & (1u << (sb->inputMixBin - 11u))) != 0u) {
            EnsureSubmixBusAllocatedLocked(sb);
        }
    }
}

static bool HasActiveFxSendTargetLocked(uint32_t fxSendBinMask, const XboxDSBufferObject* excludeSelf) {
    if (fxSendBinMask == 0u) return false;
    for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
        const XboxDSBufferObject* sb = g_activeBuffers[i];
        if (!sb || sb == excludeSelf || sb->inputMixBin < 11u || sb->inputMixBin > 31u) continue;
        if ((fxSendBinMask & (1u << (sb->inputMixBin - 11u))) != 0u) {
            return true;
        }
    }
    return false;
}

struct BufferRegionFrameGeometry {
    uint32_t regionStartByte;
    uint32_t regionLengthBytes;
    uint32_t rawBlockAlign;
    uint32_t totalFrames;
    uint32_t loopStartFrame;
    uint32_t loopEndFrame;
    bool     hasCustomLoop;
};

static BufferRegionFrameGeometry ComputeBufferFrameGeometry(const XboxDSBufferObject* b) {
    BufferRegionFrameGeometry g = {};
    if (!b || !b->rawBuf || b->rawSize == 0u) return g;

    uint32_t nCh  = b->wfx.nChannels ? b->wfx.nChannels : 1u;
    uint32_t bits = (!b->isAdpcm && b->wfx.wBitsPerSample == 8u) ? 8u : 16u;
    g.rawBlockAlign = b->isAdpcm ? (36u * nCh) : ((nCh * bits) / 8u);
    if (g.rawBlockAlign == 0u) g.rawBlockAlign = 1u;

    g.regionStartByte = (!b->isStreamingLockBuf && b->playStart < b->rawSize) ? b->playStart : 0u;
    g.regionLengthBytes = (!b->isStreamingLockBuf && b->playLength > 0u && g.regionStartByte + b->playLength <= b->rawSize)
                          ? b->playLength
                          : (b->rawSize - g.regionStartByte);

    if (b->isAdpcm) {
        g.totalFrames = (g.regionLengthBytes / g.rawBlockAlign) * 64u;
    } else {
        g.totalFrames = g.regionLengthBytes / g.rawBlockAlign;
    }

    g.loopStartFrame = 0u;
    g.loopEndFrame   = g.totalFrames;

    if (!b->isStreamingLockBuf && b->loopLength >= g.rawBlockAlign && g.totalFrames > 0u) {
        uint32_t alignedStart  = (b->loopStart / g.rawBlockAlign) * g.rawBlockAlign;
        uint32_t alignedLength = (b->loopLength / g.rawBlockAlign) * g.rawBlockAlign;
        if (alignedLength >= g.rawBlockAlign && alignedStart + alignedLength <= g.regionLengthBytes) {
            uint32_t lsFrame = b->isAdpcm
                ? ((alignedStart / g.rawBlockAlign) * 64u)
                : (alignedStart / g.rawBlockAlign);
            uint32_t leFrame = b->isAdpcm
                ? (((alignedStart + alignedLength) / g.rawBlockAlign) * 64u)
                : ((alignedStart + alignedLength) / g.rawBlockAlign);
            if (leFrame > g.totalFrames) leFrame = g.totalFrames;
            if (leFrame > lsFrame) {
                g.loopStartFrame = lsFrame;
                g.loopEndFrame   = leFrame;
                g.hasCustomLoop  = (lsFrame > 0u || leFrame < g.totalFrames);
            }
        }
    }
    return g;
}

static inline void ReadBufferFrameStereoFloat(
    XboxDSBufferObject* b, const BufferRegionFrameGeometry& g, uint32_t frameIdx,
    float* outL, float* outR)
{
    if (!b || !b->rawBuf || g.totalFrames == 0u) {
        *outL = 0.0f;
        *outR = 0.0f;
        return;
    }
    if (frameIdx >= g.totalFrames) {
        frameIdx = g.totalFrames - 1u;
    }
    const uint8_t* regionBase = b->rawBuf + g.regionStartByte;
    uint32_t nCh = b->wfx.nChannels ? b->wfx.nChannels : 1u;

    if (b->isAdpcm) {
        uint32_t blockIdx   = frameIdx >> 6u;
        uint32_t inBlockIdx = frameIdx & 63u;
        if (b->cachedAdpcmBlockIdx != blockIdx) {
            uint32_t byteOff = blockIdx * g.rawBlockAlign;
            if (byteOff + g.rawBlockAlign <= g.regionLengthBytes) {
                DecodeXboxAdpcmBlockToStereoFloat(regionBase + byteOff, nCh, b->cachedAdpcmL, b->cachedAdpcmR);
                b->cachedAdpcmBlockIdx = blockIdx;
            } else {
                *outL = 0.0f;
                *outR = 0.0f;
                return;
            }
        }
        *outL = b->cachedAdpcmL[inBlockIdx];
        *outR = b->cachedAdpcmR[inBlockIdx];
    } else {
        uint16_t bits = (b->wfx.wBitsPerSample == 8u) ? 8u : 16u;
        SamplePcmFrameToStereoFloat(regionBase, frameIdx, nCh, bits, outL, outR);
    }
}

static inline void ComputeVoiceStereoAndWetGains(
    LONG volumeMb, LONG headroomMb, int32_t lfoEgVolModMb,
    bool active3D, LONG atten3DMb, const Xbox3DVoiceMetrics& voice3D,
    LONG mixBinAttenMb, LONG panMb,
    bool hasI3DL2Send, int32_t i3dl2SendMb,
    float* outDryGainL, float* outDryGainR,
    float* outWetGainL, float* outWetGainR)
{
    LONG baseMb      = volumeMb - headroomMb + lfoEgVolModMb;
    LONG drySpatial  = active3D ? atten3DMb : mixBinAttenMb;
    LONG totalDryMb  = baseMb + drySpatial;
    if (totalDryMb < kMinDirectSoundVolumeMb) totalDryMb = kMinDirectSoundVolumeMb;
    if (totalDryMb > kMaxDirectSoundVolumeMb) totalDryMb = kMaxDirectSoundVolumeMb;

    float dryGain = (totalDryMb <= kMinDirectSoundVolumeMb)
        ? 0.0f
        : ((totalDryMb < 0) ? powf(10.0f, static_cast<float>(totalDryMb) / kMillibelsPerTwentyDb) : 1.0f);

    if (panMb < kMinDirectSoundPanMb) panMb = kMinDirectSoundPanMb;
    if (panMb > kMaxDirectSoundPanMb) panMb = kMaxDirectSoundPanMb;
    float panScaleL = (panMb > 0) ? powf(10.0f, -static_cast<float>(panMb) / kMillibelsPerTwentyDb) : 1.0f;
    float panScaleR = (panMb < 0) ? powf(10.0f,  static_cast<float>(panMb) / kMillibelsPerTwentyDb) : 1.0f;

    *outDryGainL = dryGain * panScaleL;
    *outDryGainR = dryGain * panScaleR;

    if (hasI3DL2Send && i3dl2SendMb > -9500) {
        // On Xbox MCPX + I3DL2, direct obstruction attenuates the dry path while the wet room path
        // uses distance + cone + room occlusion + MixBin 10 send level (so occluded/obstructed sounds keep room tail).
        int32_t wetSpatialMb = active3D
            ? (voice3D.distanceVol + voice3D.coneVol + voice3D.i3dl2RoomVol + i3dl2SendMb)
            : (mixBinAttenMb + i3dl2SendMb);
        int32_t totalWetMb = baseMb + wetSpatialMb;
        if (totalWetMb < kMinDirectSoundVolumeMb) totalWetMb = kMinDirectSoundVolumeMb;
        if (totalWetMb > kMaxDirectSoundVolumeMb) totalWetMb = kMaxDirectSoundVolumeMb;
        float wetGain = (totalWetMb <= -9500)
            ? 0.0f
            : powf(10.0f, static_cast<float>(totalWetMb) / kMillibelsPerTwentyDb);
        *outWetGainL = wetGain * (0.72f * panScaleL + 0.28f * panScaleR);
        *outWetGainR = wetGain * (0.72f * panScaleR + 0.28f * panScaleL);
    } else {
        *outWetGainL = 0.0f;
        *outWetGainR = 0.0f;
    }
}

bool AccumulateToXboxSubmixBufferLocked(
    XboxDSBufferObject* targetBuf, uint32_t fxSendBinMask,
    const float* inL, const float* inR, uint32_t frameOffset, uint32_t numFrames)
{
    if (!inL || !inR || numFrames == 0u || frameOffset + numFrames > kAudioMaxQuantumFrames) return false;
    bool routed = false;

    if (targetBuf &&
        targetBuf->submixL.size() >= frameOffset + numFrames &&
        targetBuf->submixR.size() >= frameOffset + numFrames)
    {
        float* dstL = targetBuf->submixL.data() + frameOffset;
        float* dstR = targetBuf->submixR.data() + frameOffset;
        for (uint32_t i = 0u; i < numFrames; ++i) {
            dstL[i] += inL[i];
            dstR[i] += inR[i];
        }
        targetBuf->submixHasAudio = true;
        routed = true;
    }

    if (fxSendBinMask != 0u) {
        for (size_t bi = 0u; bi < g_activeBuffers.size(); ++bi) {
            XboxDSBufferObject* sb = g_activeBuffers[bi];
            if (!sb || sb == targetBuf || sb->inputMixBin < 11u || sb->inputMixBin > 31u) continue;
            uint32_t bit = 1u << (sb->inputMixBin - 11u);
            if ((fxSendBinMask & bit) != 0u &&
                sb->submixL.size() >= frameOffset + numFrames &&
                sb->submixR.size() >= frameOffset + numFrames)
            {
                float* dstL = sb->submixL.data() + frameOffset;
                float* dstR = sb->submixR.data() + frameOffset;
                for (uint32_t i = 0u; i < numFrames; ++i) {
                    dstL[i] += inL[i];
                    dstR[i] += inR[i];
                }
                sb->submixHasAudio = true;
            }
        }
    }
    return routed;
}

void PrepareXboxSubmixBuffers48kLocked(uint32_t numFrames) {
    if (numFrames > kAudioMaxQuantumFrames) numFrames = kAudioMaxQuantumFrames;
    for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
        XboxDSBufferObject* b = g_activeBuffers[i];
        if (b && !b->submixL.empty()) {
            uint32_t clearCount = (numFrames <= b->submixL.size()) ? numFrames : static_cast<uint32_t>(b->submixL.size());
            memset(b->submixL.data(), 0, static_cast<size_t>(clearCount) * sizeof(float));
            memset(b->submixR.data(), 0, static_cast<size_t>(clearCount) * sizeof(float));
            b->submixHasAudio = false;
        }
    }
}

void MixActiveXboxBuffers48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames)
{
    if (!ioMasterDryL || !ioMasterDryR || !ioReverbSendL || !ioReverbSendR || numFrames == 0u) return;
    if (numFrames > kAudioMaxQuantumFrames) numFrames = kAudioMaxQuantumFrames;

    float voiceScratchL[kAudioMixSubQuantumFrames];
    float voiceScratchR[kAudioMixSubQuantumFrames];
    float postDryL[kAudioMixSubQuantumFrames];
    float postDryR[kAudioMixSubQuantumFrames];

    for (size_t bi = 0u; bi < g_activeBuffers.size(); ++bi) {
        XboxDSBufferObject* b = g_activeBuffers[bi];
        if (!b || b->status == 0u || !b->rawBuf || b->rawSize == 0u) continue;

        BufferRegionFrameGeometry geom = ComputeBufferFrameGeometry(b);
        if (geom.totalFrames == 0u) {
            b->status = 0u;
            continue;
        }

        uint32_t offset = 0u;
        while (offset < numFrames && b->status != 0u) {
            uint32_t chunk = numFrames - offset;
            if (chunk > kAudioMixSubQuantumFrames) chunk = kAudioMixSubQuantumFrames;

            int32_t lfoEgVolModMb    = 0;
            float   lfoEgPitchRatio  = 1.0f;
            float   lfoEgCutoffRatio = 1.0f;
            bool    ampRelDone       = false;
            if (b->lfoActive[0] || b->lfoActive[1] || b->egActive[0] || b->egActive[1]) {
                EvaluateXboxVoiceLfoAndEgSamples(
                    b->lfo, b->lfoActive, b->eg, b->egActive,
                    b->activeSamples48k, b->isReleasing, b->releaseSamples48k,
                    &lfoEgVolModMb, &lfoEgPitchRatio, &lfoEgCutoffRatio, &ampRelDone);
                if (b->isReleasing && ampRelDone && (b->releaseOnlyEnvelope || !b->isLooping)) {
                    b->status              = 0u;
                    b->isLooping           = false;
                    b->isReleasing         = false;
                    b->releaseOnlyEnvelope = false;
                    break;
                }
            }

            bool active3D = b->use3DVoiceData && b->voice3D.active3D;
            DWORD effFreq = b->freq ? b->freq : kDefaultSampleRateHz;
            if (active3D && b->voice3D.dopplerPitch != 0) {
                effFreq = XboxPitchToFrequencyHz(b->lPitch + b->voice3D.dopplerPitch);
            }
            if (lfoEgPitchRatio > 0.01f && fabsf(lfoEgPitchRatio - 1.0f) > 0.0005f) {
                double modHz = static_cast<double>(effFreq) * static_cast<double>(lfoEgPitchRatio);
                if (modHz < static_cast<double>(kMinDirectSoundFreqHz)) modHz = static_cast<double>(kMinDirectSoundFreqHz);
                if (modHz > static_cast<double>(kMaxXboxPitchFreqHz))   modHz = static_cast<double>(kMaxXboxPitchFreqHz);
                effFreq = static_cast<DWORD>(modHz + 0.5);
            }
            if (effFreq < kMinDirectSoundFreqHz) effFreq = kMinDirectSoundFreqHz;
            if (effFreq > kMaxXboxPitchFreqHz)   effFreq = kMaxXboxPitchFreqHz;

            double step = static_cast<double>(effFreq) / static_cast<double>(kDefaultSampleRateHz);
            double pos  = b->playFramePos;
            uint32_t rendered = 0u;

            for (; rendered < chunk; ++rendered) {
                if (b->isLooping) {
                    uint32_t loopStart = geom.loopStartFrame;
                    uint32_t loopEnd   = geom.loopEndFrame;
                    if (loopEnd > loopStart && pos >= static_cast<double>(loopEnd)) {
                        double loopLen = static_cast<double>(loopEnd - loopStart);
                        pos = static_cast<double>(loopStart) + fmod(pos - static_cast<double>(loopStart), loopLen);
                    }
                } else if (pos >= static_cast<double>(geom.totalFrames)) {
                    b->status              = 0u;
                    b->isReleasing         = false;
                    b->releaseOnlyEnvelope = false;
                    break;
                }

                uint32_t idx0 = static_cast<uint32_t>(pos);
                float    frac = static_cast<float>(pos - static_cast<double>(idx0));
                if (idx0 >= geom.totalFrames) idx0 = geom.totalFrames - 1u;

                uint32_t idx1 = idx0 + 1u;
                if (b->isLooping && geom.loopEndFrame > geom.loopStartFrame && idx1 >= geom.loopEndFrame) {
                    idx1 = geom.loopStartFrame;
                } else if (idx1 >= geom.totalFrames) {
                    idx1 = b->isStreamingLockBuf ? 0u : idx0;
                }

                float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
                ReadBufferFrameStereoFloat(b, geom, idx0, &l0, &r0);
                if (idx1 == idx0 || frac <= 1e-6f) {
                    voiceScratchL[rendered] = l0;
                    voiceScratchR[rendered] = r0;
                } else {
                    ReadBufferFrameStereoFloat(b, geom, idx1, &l1, &r1);
                    voiceScratchL[rendered] = l0 + (l1 - l0) * frac;
                    voiceScratchR[rendered] = r0 + (r1 - r0) * frac;
                }

                pos += step;
            }

            if (b->status != 0u && b->isLooping && geom.loopEndFrame > geom.loopStartFrame &&
                pos >= static_cast<double>(geom.loopEndFrame))
            {
                double loopLen = static_cast<double>(geom.loopEndFrame - geom.loopStartFrame);
                pos = static_cast<double>(geom.loopStartFrame) + fmod(pos - static_cast<double>(geom.loopStartFrame), loopLen);
            } else if (b->status != 0u && !b->isLooping && pos >= static_cast<double>(geom.totalFrames)) {
                b->status              = 0u;
                b->isReleasing         = false;
                b->releaseOnlyEnvelope = false;
            }
            b->playFramePos = pos;

            if (rendered > 0u) {
                b->activeSamples48k += rendered;
                if (b->isReleasing) {
                    b->releaseSamples48k += rendered;
                }

                ApplyVoiceSvfAndHrtfFloatStereo(
                    voiceScratchL, voiceScratchR, rendered,
                    &b->filterDesc, b->hasActiveFilter, lfoEgCutoffRatio,
                    active3D, b->voice3D.hrtfFilterCoeff, b->voice3D.itdDelaySamples,
                    &b->filterState);

                float dryGainL = 0.0f, dryGainR = 0.0f, wetGainL = 0.0f, wetGainR = 0.0f;
                LONG  effPanMb = active3D ? b->voice3D.pan : b->pan;
                ComputeVoiceStereoAndWetGains(
                    b->volume, b->headroomMb, lfoEgVolModMb,
                    active3D, b->atten3D, b->voice3D,
                    b->mixBinAttenMb, effPanMb,
                    b->hasI3DL2Send, b->i3dl2SendMb,
                    &dryGainL, &dryGainR, &wetGainL, &wetGainR);

                for (uint32_t i = 0u; i < rendered; ++i) {
                    postDryL[i] = voiceScratchL[i] * dryGainL;
                    postDryR[i] = voiceScratchR[i] * dryGainR;
                    if (wetGainL > 0.0f || wetGainR > 0.0f) {
                        ioReverbSendL[offset + i] += voiceScratchL[i] * wetGainL;
                        ioReverbSendR[offset + i] += voiceScratchR[i] * wetGainR;
                    }
                }

                bool routedToOutputBuf = false;
                if (b->outputBuffer || b->fxSendBinMask != 0u) {
                    XboxDSBufferObject* outTarget = (b->outputBuffer != b) ? b->outputBuffer : nullptr;
                    routedToOutputBuf = AccumulateToXboxSubmixBufferLocked(
                        outTarget, b->fxSendBinMask, postDryL, postDryR, offset, rendered);
                    if (b->fxSendBinMask != 0u && !b->hasI3DL2Send && !HasActiveFxSendTargetLocked(b->fxSendBinMask, b)) {
                        for (uint32_t i = 0u; i < rendered; ++i) {
                            ioReverbSendL[offset + i] += postDryL[i] * 0.35f;
                            ioReverbSendR[offset + i] += postDryR[i] * 0.35f;
                        }
                    }
                }
                if (!routedToOutputBuf) {
                    for (uint32_t i = 0u; i < rendered; ++i) {
                        ioMasterDryL[offset + i] += postDryL[i];
                        ioMasterDryR[offset + i] += postDryR[i];
                    }
                }
            }

            offset += chunk;
        }
    }
}

void FinalizeXboxSubmixBuffers48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames)
{
    if (!ioMasterDryL || !ioMasterDryR || !ioReverbSendL || !ioReverbSendR || numFrames == 0u) return;
    if (numFrames > kAudioMaxQuantumFrames) numFrames = kAudioMaxQuantumFrames;

    // Support up to 4 chained submix hops (Voice -> Submix A -> Submix B / FXIN -> Master) without cycles
    for (uint32_t pass = 0u; pass < 4u; ++pass) {
        bool anyProcessed = false;
        for (size_t bi = 0u; bi < g_activeBuffers.size(); ++bi) {
            XboxDSBufferObject* sb = g_activeBuffers[bi];
            if (!sb || !sb->submixHasAudio || sb->submixL.size() < numFrames) continue;

            // Defer this submix to the next hop if another un-flushed submix still feeds into it
            // via either outputBuffer or fxSendBinMask -> inputMixBin
            bool hasPendingChildSubmix = false;
            if (pass < 3u) {
                uint32_t sbFxBit = (sb->inputMixBin >= 11u && sb->inputMixBin <= 31u)
                    ? (1u << (sb->inputMixBin - 11u))
                    : 0u;
                for (size_t ci = 0u; ci < g_activeBuffers.size(); ++ci) {
                    XboxDSBufferObject* child = g_activeBuffers[ci];
                    if (!child || child == sb || !child->submixHasAudio) continue;
                    if (child->outputBuffer == sb || (sbFxBit != 0u && (child->fxSendBinMask & sbFxBit) != 0u)) {
                        hasPendingChildSubmix = true;
                        break;
                    }
                }
            }
            if (hasPendingChildSubmix) continue;

            sb->submixHasAudio = false;
            anyProcessed       = true;

            int32_t lfoEgVolModMb    = 0;
            float   lfoEgPitchRatio  = 1.0f;
            float   lfoEgCutoffRatio = 1.0f;
            if (sb->lfoActive[0] || sb->lfoActive[1] || sb->egActive[0] || sb->egActive[1]) {
                EvaluateXboxVoiceLfoAndEgSamples(
                    sb->lfo, sb->lfoActive, sb->eg, sb->egActive,
                    sb->activeSamples48k, sb->isReleasing, sb->releaseSamples48k,
                    &lfoEgVolModMb, &lfoEgPitchRatio, &lfoEgCutoffRatio, nullptr);
                sb->activeSamples48k += numFrames;
                if (sb->isReleasing) sb->releaseSamples48k += numFrames;
            }

            bool   active3D = sb->use3DVoiceData && sb->voice3D.active3D;
            float* busL     = sb->submixL.data();
            float* busR     = sb->submixR.data();

            ApplyVoiceSvfAndHrtfFloatStereo(
                busL, busR, numFrames,
                &sb->filterDesc, sb->hasActiveFilter, lfoEgCutoffRatio,
                active3D, sb->voice3D.hrtfFilterCoeff, sb->voice3D.itdDelaySamples,
                &sb->filterState);

            float dryGainL = 0.0f, dryGainR = 0.0f, wetGainL = 0.0f, wetGainR = 0.0f;
            LONG  effPanMb = active3D ? sb->voice3D.pan : sb->pan;
            ComputeVoiceStereoAndWetGains(
                sb->volume, sb->headroomMb, lfoEgVolModMb,
                active3D, sb->atten3D, sb->voice3D,
                sb->mixBinAttenMb, effPanMb,
                sb->hasI3DL2Send, sb->i3dl2SendMb,
                &dryGainL, &dryGainR, &wetGainL, &wetGainR);

            XboxDSBufferObject* parent = (sb->outputBuffer && sb->outputBuffer != sb && sb->outputBuffer->submixL.size() >= numFrames)
                ? sb->outputBuffer
                : nullptr;

            bool fxClaimed = false;
            if (sb->fxSendBinMask != 0u) {
                for (size_t fi = 0u; fi < g_activeBuffers.size(); ++fi) {
                    XboxDSBufferObject* fxDst = g_activeBuffers[fi];
                    if (!fxDst || fxDst == sb || fxDst == parent || fxDst->inputMixBin < 11u || fxDst->inputMixBin > 31u) continue;
                    if ((sb->fxSendBinMask & (1u << (fxDst->inputMixBin - 11u))) != 0u &&
                        fxDst->submixL.size() >= numFrames && fxDst->submixR.size() >= numFrames)
                    {
                        float* fxL = fxDst->submixL.data();
                        float* fxR = fxDst->submixR.data();
                        for (uint32_t i = 0u; i < numFrames; ++i) {
                            fxL[i] += busL[i] * dryGainL;
                            fxR[i] += busR[i] * dryGainR;
                        }
                        fxDst->submixHasAudio = true;
                        fxClaimed = true;
                    }
                }
            }
            const bool sendUnclaimedFxToReverb = (sb->fxSendBinMask != 0u && !fxClaimed && !sb->hasI3DL2Send);

            if (parent) {
                float* pL = parent->submixL.data();
                float* pR = parent->submixR.data();
                for (uint32_t i = 0u; i < numFrames; ++i) {
                    float dL = busL[i] * dryGainL;
                    float dR = busR[i] * dryGainR;
                    pL[i] += dL;
                    pR[i] += dR;
                    if (wetGainL > 0.0f || wetGainR > 0.0f) {
                        ioReverbSendL[i] += busL[i] * wetGainL;
                        ioReverbSendR[i] += busR[i] * wetGainR;
                    } else if (sendUnclaimedFxToReverb) {
                        ioReverbSendL[i] += dL * 0.35f;
                        ioReverbSendR[i] += dR * 0.35f;
                    }
                    busL[i] = 0.0f;
                    busR[i] = 0.0f;
                }
                parent->submixHasAudio = true;
            } else {
                for (uint32_t i = 0u; i < numFrames; ++i) {
                    float dL = busL[i] * dryGainL;
                    float dR = busR[i] * dryGainR;
                    ioMasterDryL[i] += dL;
                    ioMasterDryR[i] += dR;
                    if (wetGainL > 0.0f || wetGainR > 0.0f) {
                        ioReverbSendL[i] += busL[i] * wetGainL;
                        ioReverbSendR[i] += busR[i] * wetGainR;
                    } else if (sendUnclaimedFxToReverb) {
                        ioReverbSendL[i] += dL * 0.35f;
                        ioReverbSendR[i] += dR * 0.35f;
                    }
                    busL[i] = 0.0f;
                    busR[i] = 0.0f;
                }
            }
        }
        if (!anyProcessed) break;
    }
}

// ============================================================================
// IDirectSoundBuffer Virtual & C-API Methods
// ============================================================================

static ULONG __stdcall Buffer_AddRef(XboxDSBufferObject* b) {
    return b ? static_cast<ULONG>(InterlockedIncrement(&b->refCount)) : 1u;
}

static ULONG __stdcall Buffer_Release(XboxDSBufferObject* b) {
    if (!b) return 0u;
    LONG r = InterlockedDecrement(&b->refCount);
    if (r <= 0) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
            if (g_activeBuffers[i] == b) {
                g_activeBuffers.erase(g_activeBuffers.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
            if (g_activeBuffers[i]->outputBuffer == b) {
                g_activeBuffers[i]->outputBuffer = nullptr;
            }
        }
        ClearStreamsReferencingOutputBufferLocked(b);
        if (b->ownsRawBuf && b->rawBuf) {
            free(b->rawBuf);
            b->rawBuf = nullptr;
        }
        LeaveCriticalSection(&g_audioCs);
        delete b;
        return 0u;
    }
    return static_cast<ULONG>(r);
}

static HRESULT __stdcall Buffer_Play(XboxDSBufferObject* b, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags) {
    (void)dwReserved1; (void)dwReserved2;
    if (!b) return S_OK;
    EnsureXboxWasapiMixerStarted();
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);

    bool isLooping  = (dwFlags & 1u) != 0u;
    bool wasStopped = (b->status == 0u);
    b->status              = 1u;
    b->isLooping           = isLooping;
    b->isReleasing         = false;
    b->releaseOnlyEnvelope = false;

    if (wasStopped) {
        b->activeSamples48k  = 0ULL;
        b->releaseSamples48k = 0ULL;
        if (b->isStreamingLockBuf && isLooping) {
            // Preserve circular buffer play cursor across Stop/Play pause cycles (e.g. Bink audio)
        } else if (b->hasExplicitStartPos) {
            b->hasExplicitStartPos = false;
        } else {
            b->playFramePos = 0.0;
            memset(&b->filterState, 0, sizeof(b->filterState));
        }
    } else if (!isLooping && !b->isStreamingLockBuf) {
        b->playFramePos        = 0.0;
        b->hasExplicitStartPos = false;
        b->activeSamples48k    = 0ULL;
        b->releaseSamples48k   = 0ULL;
    }

    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Stop(XboxDSBufferObject* b) {
    if (!b) return S_OK;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    b->status              = 0u;
    b->isLooping           = false;
    b->isReleasing         = false;
    b->releaseOnlyEnvelope = false;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_StopEx(XboxDSBufferObject* b, uint32_t rtLow, uint32_t rtHigh, DWORD dwFlags) {
    (void)rtLow; (void)rtHigh;
    if (!b) return S_OK;
    // Verified against CMcpxBuffer::Stop (0x006E8718) & CMcpxBuffer::StopEx (0x006E8981):
    //   1. DSBSTOPEX_ENVELOPE (0x1): triggers amplitude EG release ramp while continuing playback until envelope decays.
    //   2. DSBSTOPEX_RELEASEWAVEFORM (0x2): clears loop enable so playback exits sustain loop into release tail [loopEnd .. regionEnd).
    if ((dwFlags & 3u) != 0u) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (b->status != 0u) {
            bool hasEnvRel  = ((dwFlags & 1u) != 0u) && b->egActive[1] && ((b->eg[1].dwRelease & 0xFFFu) > 0u);
            bool hasWaveRel = ((dwFlags & 2u) != 0u) && b->isLooping;
            if (hasEnvRel || hasWaveRel) {
                if (hasEnvRel && !b->isReleasing) {
                    b->isReleasing         = true;
                    b->releaseSamples48k   = 0ULL;
                    b->releaseOnlyEnvelope = !hasWaveRel;
                }
                if (hasWaveRel) {
                    b->isLooping           = false;
                    b->releaseOnlyEnvelope = false;
                }
                LeaveCriticalSection(&g_audioCs);
                return S_OK;
            }
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return Buffer_Stop(b);
}

static HRESULT __stdcall Buffer_GetStatus(XboxDSBufferObject* b, DWORD* pdwStatus) {
    if (!pdwStatus) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (b && b->status != 0u) {
        *pdwStatus = kXboxDsbStatusPlaying | (b->isLooping ? kXboxDsbStatusLooping : 0u);
    } else {
        *pdwStatus = 0u;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static DWORD ConvertFrameToXboxRawOffsetLocked(const XboxDSBufferObject* b, const BufferRegionFrameGeometry& g, uint32_t frameIdx) {
    if (!b || g.regionLengthBytes == 0u || g.totalFrames == 0u) return 0u;
    if (frameIdx >= g.totalFrames) {
        frameIdx %= g.totalFrames;
    }
    uint32_t rawBytes = b->isAdpcm
        ? ((frameIdx >> 6u) * g.rawBlockAlign)
        : (frameIdx * g.rawBlockAlign);
    if (b->isStreamingLockBuf && b->rawSize > 0u) {
        return rawBytes % b->rawSize;
    }
    if (rawBytes >= g.regionLengthBytes) {
        rawBytes %= g.regionLengthBytes;
    }
    return rawBytes;
}

static double ConvertXboxRawOffsetToFrameLocked(const XboxDSBufferObject* b, const BufferRegionFrameGeometry& g, DWORD xboxByteOffset) {
    if (!b || g.regionLengthBytes == 0u || g.totalFrames == 0u) return 0.0;
    uint32_t clampedBytes = xboxByteOffset;
    if (clampedBytes >= g.regionLengthBytes) {
        clampedBytes %= g.regionLengthBytes;
    }
    uint32_t frame = b->isAdpcm
        ? ((clampedBytes / g.rawBlockAlign) * 64u)
        : (clampedBytes / g.rawBlockAlign);
    if (frame >= g.totalFrames) frame = 0u;
    return static_cast<double>(frame);
}

static HRESULT __stdcall Buffer_GetCurrentPosition(XboxDSBufferObject* b, DWORD* pdwPlay, DWORD* pdwWrite) {
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (b && b->rawBuf && b->rawSize > 0u) {
        BufferRegionFrameGeometry g = ComputeBufferFrameGeometry(b);
        if (g.totalFrames > 0u) {
            uint32_t playFrame = static_cast<uint32_t>(b->playFramePos >= 0.0 ? b->playFramePos : 0.0);
            if (playFrame >= g.totalFrames) playFrame %= g.totalFrames;

            DWORD effFreq = b->freq ? b->freq : kDefaultSampleRateHz;
            uint32_t writeLeadFrames = (effFreq * 15u) / 1000u; // ~15 ms safe write-ahead margin
            if (writeLeadFrames < 64u) writeLeadFrames = 64u;
            uint32_t writeFrame = playFrame + writeLeadFrames;
            if (b->isLooping || b->isStreamingLockBuf) {
                writeFrame %= g.totalFrames;
            } else if (writeFrame >= g.totalFrames) {
                writeFrame = g.totalFrames - 1u;
            }

            if (pdwPlay)  *pdwPlay  = ConvertFrameToXboxRawOffsetLocked(b, g, playFrame);
            if (pdwWrite) *pdwWrite = ConvertFrameToXboxRawOffsetLocked(b, g, writeFrame);
        } else {
            if (pdwPlay)  *pdwPlay  = 0u;
            if (pdwWrite) *pdwWrite = 0u;
        }
    } else {
        if (pdwPlay)  *pdwPlay  = 0u;
        if (pdwWrite) *pdwWrite = 0u;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetCurrentPosition(XboxDSBufferObject* b, DWORD dwNewPosition) {
    if (!b) return S_OK;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    BufferRegionFrameGeometry g = ComputeBufferFrameGeometry(b);
    b->playFramePos        = ConvertXboxRawOffsetToFrameLocked(b, g, dwNewPosition);
    b->hasExplicitStartPos = (b->status == 0u);
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Lock(
    XboxDSBufferObject* b, DWORD dwOffset, DWORD dwBytes,
    void** ppvAudioPtr1, DWORD* pdwAudioBytes1,
    void** ppvAudioPtr2, DWORD* pdwAudioBytes2, DWORD dwFlags)
{
    if (!b) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (!b->rawBuf) {
        b->rawSize    = dwBytes ? dwBytes : 65536u;
        b->rawBuf     = static_cast<uint8_t*>(calloc(1, b->rawSize));
        b->ownsRawBuf = true;
    }
    ++b->lockCount;
    b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;

    if (!b->isAdpcm &&
        ((dwFlags & 1u) != 0u ||
         b->status != 0u ||
         (b->lockCount > 1u && (dwOffset > 0u || (dwBytes > 0u && dwBytes < b->rawSize && (dwFlags & 2u) == 0u)))))
    {
        b->isStreamingLockBuf = true;
    }

    if ((dwFlags & 1u) != 0u) { // DSBLOCK_FROMWRITECURSOR
        BufferRegionFrameGeometry g = ComputeBufferFrameGeometry(b);
        if (g.totalFrames > 0u) {
            uint32_t playFrame = static_cast<uint32_t>(b->playFramePos >= 0.0 ? b->playFramePos : 0.0) % g.totalFrames;
            DWORD effFreq = b->freq ? b->freq : kDefaultSampleRateHz;
            uint32_t writeLeadFrames = (effFreq * 15u) / 1000u;
            if (writeLeadFrames < 64u) writeLeadFrames = 64u;
            uint32_t writeFrame = (playFrame + writeLeadFrames) % g.totalFrames;
            dwOffset = ConvertFrameToXboxRawOffsetLocked(b, g, writeFrame);
        }
    }
    if ((dwFlags & 2u) != 0u) { // DSBLOCK_ENTIREBUFFER
        dwBytes = b->rawSize;
    }
    if (b->rawSize > 0u) {
        dwOffset %= b->rawSize;
        if (dwBytes > b->rawSize) dwBytes = b->rawSize;
    } else {
        dwOffset = 0u;
        dwBytes  = 0u;
    }

    DWORD bytes1 = (dwOffset + dwBytes <= b->rawSize) ? dwBytes : (b->rawSize - dwOffset);
    DWORD bytes2 = (bytes1 < dwBytes) ? (dwBytes - bytes1) : 0u;

    if (ppvAudioPtr1)   *ppvAudioPtr1   = b->rawBuf + dwOffset;
    if (pdwAudioBytes1) *pdwAudioBytes1 = bytes1;
    if (ppvAudioPtr2)   *ppvAudioPtr2   = (bytes2 > 0u) ? b->rawBuf : nullptr;
    if (pdwAudioBytes2) *pdwAudioBytes2 = bytes2;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Unlock(
    XboxDSBufferObject* b, void* pvAudioPtr1, DWORD dwAudioBytes1, void* pvAudioPtr2, DWORD dwAudioBytes2)
{
    (void)pvAudioPtr1; (void)dwAudioBytes1; (void)pvAudioPtr2; (void)dwAudioBytes2;
    if (!b) return S_OK;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetBufferData(XboxDSBufferObject* b, void* pvBufferData, DWORD dwBufferBytes) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (b->ownsRawBuf && b->rawBuf) free(b->rawBuf);
        b->rawBuf              = reinterpret_cast<uint8_t*>(pvBufferData);
        b->rawSize             = dwBufferBytes;
        b->ownsRawBuf          = false;
        b->playStart           = 0u;
        b->playLength          = 0u;
        b->loopStart           = 0u;
        b->loopLength          = 0u;
        b->playFramePos        = 0.0;
        b->hasExplicitStartPos = false;
        b->lockCount           = 0u;
        b->isStreamingLockBuf  = false;
        b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFormat(XboxDSBufferObject* b, const WAVEFORMATEX* pwfx) {
    if (b && pwfx) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->wfx     = *pwfx;
        b->isAdpcm = (pwfx->wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
        if (pwfx->nSamplesPerSec) {
            b->freq   = pwfx->nSamplesPerSec;
            b->lPitch = FrequencyHzToXboxPitch(b->freq);
        }
        b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetVolume(XboxDSBufferObject* b, LONG lVolume) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->volume = lVolume;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetHeadroom(XboxDSBufferObject* b, DWORD dwHeadroom) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->headroomMb = (dwHeadroom <= 10000u) ? static_cast<LONG>(dwHeadroom) : 10000;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetPitch(XboxDSBufferObject* b, LONG lPitch) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->lPitch = lPitch;
        b->freq   = XboxPitchToFrequencyHz(lPitch);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFrequency(XboxDSBufferObject* b, DWORD dwFrequency) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (dwFrequency < kMinDirectSoundFreqHz) dwFrequency = kMinDirectSoundFreqHz;
        if (dwFrequency > kMaxXboxPitchFreqHz)   dwFrequency = kMaxXboxPitchFreqHz;
        b->freq   = dwFrequency;
        b->lPitch = FrequencyHzToXboxPitch(dwFrequency);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetLFO(XboxDSBufferObject* b, const XBOX_DSLFODESC* pLFODesc) {
    if (!b) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (pLFODesc && reinterpret_cast<uintptr_t>(pLFODesc) >= kMinValidHostAddress) {
        uint32_t idx = (pLFODesc->dwLFO == 1u) ? 1u : 0u;
        b->lfo[idx] = *pLFODesc;
        b->lfoActive[idx] = (pLFODesc->dwDelta > 0u) &&
                            (pLFODesc->lPitchModulation != 0 ||
                             pLFODesc->lFilterCutOffRange != 0 ||
                             pLFODesc->lAmplitudeModulation != 0);
    } else {
        b->lfoActive[0] = false;
        b->lfoActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetEG(XboxDSBufferObject* b, const XBOX_DSENVELOPEDESC* pEnvelopeDesc) {
    if (!b) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (pEnvelopeDesc && reinterpret_cast<uintptr_t>(pEnvelopeDesc) >= kMinValidHostAddress) {
        uint32_t idx = (pEnvelopeDesc->dwEG == 1u) ? 1u : 0u;
        b->eg[idx] = *pEnvelopeDesc;
        b->egActive[idx] = (pEnvelopeDesc->dwMode != 0u);
    } else {
        b->egActive[0] = false;
        b->egActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetOutputBuffer(XboxDSBufferObject* b, XboxDSBufferObject* pOutputBuffer) {
    if (!b) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    XboxDSBufferObject* target = (pOutputBuffer != b) ? pOutputBuffer : nullptr;
    b->outputBuffer = target;
    if (target) {
        EnsureSubmixBusAllocatedLocked(target);
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFilter(XboxDSBufferObject* b, const XBOX_DSFILTERDESC* pFilterDesc) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (pFilterDesc && reinterpret_cast<uintptr_t>(pFilterDesc) >= kMinValidHostAddress && (pFilterDesc->dwMode & 3u) != 0u) {
            b->filterDesc      = *pFilterDesc;
            b->hasActiveFilter = true;
        } else {
            memset(&b->filterDesc, 0, sizeof(b->filterDesc));
            b->hasActiveFilter = false;
            b->filterState.low[0] = b->filterState.low[1] = 0.0f;
            b->filterState.band[0] = b->filterState.band[1] = 0.0f;
            b->filterState.low2[0] = b->filterState.low2[1] = 0.0f;
            b->filterState.band2[0] = b->filterState.band2[1] = 0.0f;
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_Set3DVoiceData(XboxDSBufferObject* b, const void* pVoiceData) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        LONG pan3D = 0;
        Compute3DAudioPanAndAtten(pVoiceData, &pan3D, &b->atten3D, &b->voice3D);
        if (b->voice3D.active3D && !b->hasI3DL2Send) {
            b->hasI3DL2Send = true;
            if (b->i3dl2SendMb <= kMinDirectSoundVolumeMb) {
                b->i3dl2SendMb = 0;
            }
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

// Verified against CDirectSoundVoice::Use3DVoiceData (0x006E059E): 2nd parameter is BOOL fUse3DVoiceData
static HRESULT __stdcall Buffer_Use3DVoiceData(XboxDSBufferObject* b, DWORD dwUse3DVoiceData) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->use3DVoiceData = (dwUse3DVoiceData != 0u);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetMixBins(XboxDSBufferObject* b, const XBOX_DSMIXBINS* pMixBins) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        AssignVoiceMixBins(pMixBins, b->mixBinPairs, &b->mixBinCount);
        ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan, &b->mixBinAttenMb);
        ExtractXboxMixBinI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
        b->fxSendBinMask = ExtractFxSendBinMask(b->mixBinPairs, b->mixBinCount);
        EnsureXboxFxSendTargetsAllocatedLocked(b->fxSendBinMask);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetMixBinVolumes(XboxDSBufferObject* b, const XBOX_DSMIXBINS* pMixBins) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        UpdateVoiceMixBinVolumes(pMixBins, b->mixBinPairs, &b->mixBinCount);
        ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan, &b->mixBinAttenMb);
        ExtractXboxMixBinI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
        b->fxSendBinMask = ExtractFxSendBinMask(b->mixBinPairs, b->mixBinCount);
        EnsureXboxFxSendTargetsAllocatedLocked(b->fxSendBinMask);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetPlayRegion(XboxDSBufferObject* b, DWORD dwPlayStart, DWORD dwPlayLength) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->playStart           = dwPlayStart;
        b->playLength          = dwPlayLength;
        b->playFramePos        = 0.0;
        b->hasExplicitStartPos = false;
        b->isStreamingLockBuf  = false;
        b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetLoopRegion(XboxDSBufferObject* b, DWORD dwLoopStart, DWORD dwLoopLength) {
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        b->loopStart  = dwLoopStart;
        b->loopLength = dwLoopLength;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_GetVoiceProperties(XboxDSBufferObject* b, XBOX_DSVOICEPROPS* pProps) {
    if (!pProps) return E_INVALIDARG;
    memset(pProps, 0, sizeof(*pProps));
    if (b) {
        EnsureAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        FillVoicePropsMixBins(b->mixBinPairs, b->mixBinCount, pProps);
        pProps->lPitch             = b->lPitch;
        pProps->l3DDistanceVolume  = b->voice3D.distanceVol;
        pProps->l3DConeVolume      = b->voice3D.coneVol;
        pProps->l3DDopplerPitch    = b->voice3D.dopplerPitch;
        pProps->lI3DL2DirectVolume = b->voice3D.i3dl2DirectVol;
        pProps->lI3DL2RoomVolume   = b->voice3D.i3dl2RoomVol;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

// Xbox DirectSound COM interfaces omit QueryInterface: slot [0] = AddRef, slot [1] = Release
static void* g_bufferVtable[4] = {
    reinterpret_cast<void*>(&Buffer_AddRef),
    reinterpret_cast<void*>(&Buffer_Release),
    reinterpret_cast<void*>(&Buffer_Release),
    reinterpret_cast<void*>(&Buffer_Release),
};

// ============================================================================
// IDirectSound Device Creation & Methods
// ============================================================================

static XboxDSObject g_singletonDS = { nullptr, 0 };

static ULONG __stdcall DS_AddRef(XboxDSObject* ds) {
    return ds ? static_cast<ULONG>(InterlockedIncrement(&ds->refCount)) : 1u;
}

static ULONG __stdcall DS_Release(XboxDSObject* ds) {
    if (!ds) return 0u;
    LONG cur = ds->refCount;
    while (cur > 0) {
        LONG prev = InterlockedCompareExchange(&ds->refCount, cur - 1, cur);
        if (prev == cur) {
            return static_cast<ULONG>(cur - 1);
        }
        cur = prev;
    }
    return 0u;
}

static void* g_dsVtable[4] = {
    reinterpret_cast<void*>(&DS_AddRef),
    reinterpret_cast<void*>(&DS_Release),
    reinterpret_cast<void*>(&DS_Release),
    reinterpret_cast<void*>(&DS_Release),
};

static HRESULT __stdcall Bridge_DirectSoundCreate(const GUID* pguid, XboxDSObject** ppDS, void* pUnkOuter) {
    (void)pguid; (void)pUnkOuter;
    EnsureAudioLockInitialized();
    EnsureXboxWasapiMixerStarted();
    g_singletonDS.vtable = g_dsVtable;
    InterlockedIncrement(&g_singletonDS.refCount);
    if (ppDS) *ppDS = &g_singletonDS;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(ADDR_DSOUND_SINGLETON_PTR), &mbi, sizeof(mbi)) != 0 &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0u)
    {
        *reinterpret_cast<XboxDSObject**>(ADDR_DSOUND_SINGLETON_PTR) = &g_singletonDS; // m_pDirectSound
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CreateSoundBuffer(
    XboxDSObject* pDS, const XBOX_DSBUFFERDESC* pDesc, XboxDSBufferObject** ppBuffer, void* pUnkOuter)
{
    (void)pDS; (void)pUnkOuter;
    if (!ppBuffer) return E_INVALIDARG;
    EnsureAudioLockInitialized();
    EnsureXboxWasapiMixerStarted();

    XboxDSBufferObject* b = new XboxDSBufferObject();
    b->vtable              = g_bufferVtable;
    b->refCount            = 1;
    memset(&b->wfx, 0, sizeof(b->wfx));
    b->isAdpcm             = false;
    b->rawBuf              = nullptr;
    b->rawSize             = 0u;
    b->ownsRawBuf          = false;
    b->playStart           = 0u;
    b->playLength          = 0u;
    b->loopStart           = 0u;
    b->loopLength          = 0u;
    b->playFramePos        = 0.0;
    b->hasExplicitStartPos = false;
    b->volume              = 0;
    b->headroomMb          = 0;
    b->pan                 = 0;
    b->mixBinAttenMb       = 0;
    b->atten3D             = 0;
    b->lPitch              = 0;
    b->freq                = kDefaultSampleRateHz;
    b->status              = 0u;
    b->lockCount           = 0u;
    b->isStreamingLockBuf  = false;
    b->isLooping           = false;
    b->isReleasing         = false;
    b->releaseOnlyEnvelope = false;
    b->use3DVoiceData      = true;
    b->hasI3DL2Send        = false;
    b->i3dl2SendMb         = kMinDirectSoundVolumeMb;
    b->fxSendBinMask       = 0u;
    b->inputMixBin         = 0xFFFFFFFFu;
    memset(b->mixBinPairs, 0, sizeof(b->mixBinPairs));
    b->mixBinCount         = 0u;
    memset(&b->voice3D, 0, sizeof(b->voice3D));
    b->voice3D.hrtfFilterCoeff = 1.0f;
    memset(&b->filterDesc, 0, sizeof(b->filterDesc));
    b->hasActiveFilter     = false;
    memset(&b->filterState, 0, sizeof(b->filterState));
    memset(b->lfo, 0, sizeof(b->lfo));
    b->lfoActive[0]        = b->lfoActive[1] = false;
    memset(b->eg, 0, sizeof(b->eg));
    b->egActive[0]         = b->egActive[1] = false;
    b->outputBuffer        = nullptr;
    b->activeSamples48k    = 0ULL;
    b->releaseSamples48k   = 0ULL;
    b->cachedAdpcmBlockIdx = 0xFFFFFFFFu;
    memset(b->cachedAdpcmL, 0, sizeof(b->cachedAdpcmL));
    memset(b->cachedAdpcmR, 0, sizeof(b->cachedAdpcmR));
    b->submixHasAudio      = false;

    InitVoiceDefaultMixBins(pDesc ? pDesc->dwFlags : 0u, b->mixBinPairs, &b->mixBinCount);
    ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan, &b->mixBinAttenMb);
    ExtractXboxMixBinI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
    b->fxSendBinMask = ExtractFxSendBinMask(b->mixBinPairs, b->mixBinCount);

    if (pDesc) {
        if (pDesc->lpwfxFormat) {
            b->wfx     = *pDesc->lpwfxFormat;
            b->isAdpcm = (b->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            if (b->wfx.nSamplesPerSec) {
                b->freq   = b->wfx.nSamplesPerSec;
                b->lPitch = FrequencyHzToXboxPitch(b->freq);
            }
        } else {
            b->wfx.wFormatTag      = WAVE_FORMAT_PCM;
            b->wfx.nChannels       = 2u;
            b->wfx.nSamplesPerSec  = kDefaultSampleRateHz;
            b->wfx.wBitsPerSample  = 16u;
            b->wfx.nBlockAlign     = 4u;
            b->wfx.nAvgBytesPerSec = kDefaultStreamBytesPerSec;
        }
        if (pDesc->dwBufferBytes > 0u) {
            b->rawSize    = pDesc->dwBufferBytes;
            b->rawBuf     = static_cast<uint8_t*>(calloc(1, b->rawSize));
            b->ownsRawBuf = true;
        }
        if (pDesc->lpMixBins) {
            const XBOX_DSMIXBINS* pMixBins = reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins);
            AssignVoiceMixBins(pMixBins, b->mixBinPairs, &b->mixBinCount);
            ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan, &b->mixBinAttenMb);
            ExtractXboxMixBinI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
            b->fxSendBinMask = ExtractFxSendBinMask(b->mixBinPairs, b->mixBinCount);
        }
        if (((pDesc->dwFlags & 0x00040080u) != 0u || pDesc->dwBufferBytes == 0u) &&
            pDesc->dwInputMixBin >= 11u && pDesc->dwInputMixBin <= 31u)
        {
            b->inputMixBin = pDesc->dwInputMixBin;
            EnsureSubmixBusAllocatedLocked(b);
        }
    }

    EnterCriticalSection(&g_audioCs);
    g_activeBuffers.push_back(b);
    EnsureXboxFxSendTargetsAllocatedLocked(b->fxSendBinMask);
    LeaveCriticalSection(&g_audioCs);
    *ppBuffer = b;
    return S_OK;
}

static XBOX_DSEFFECTIMAGEDESC* g_activeDspImageDesc = nullptr;
static std::vector<uint8_t>    g_dspScratchMemory;

static HRESULT __stdcall Bridge_IDirectSound_DownloadEffectsImage(
    XboxDSObject* pDS, const void* pvImageBuffer, DWORD dwImageSize, const void* pImageLoc, void** ppImageDesc)
{
    (void)pDS; (void)pImageLoc;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);

    // Parse the Xbox MCPX GP DSP effects image (e.g. dsstdfx.bin / nsidsp.bin) verified against
    // CMcpxGPDspManager::ParseEffectImageInfo (0x006E3D30).
    const uint8_t* img = static_cast<const uint8_t*>(pvImageBuffer);
    if (img && dwImageSize > 0x820u && reinterpret_cast<uintptr_t>(img) >= kMinValidHostAddress) {
        uint32_t codeWords  = *reinterpret_cast<const uint32_t*>(img + 0x804u);
        uint32_t stateWords = *reinterpret_cast<const uint32_t*>(img + 0x80Cu);
        uint64_t descOff64  = 0x818ULL + static_cast<uint64_t>(codeWords + stateWords) * 4ULL;
        if (descOff64 + 8ULL <= dwImageSize) {
            const XBOX_DSEFFECTIMAGEDESC* srcDesc = reinterpret_cast<const XBOX_DSEFFECTIMAGEDESC*>(img + descOff64);
            uint32_t fxCount = srcDesc->dwEffectCount;
            if (fxCount <= 64u && descOff64 + 8ULL + static_cast<uint64_t>(fxCount) * sizeof(XBOX_DSEFFECTMAP) <= dwImageSize) {
                size_t allocBytes = 8u + static_cast<size_t>(fxCount) * sizeof(XBOX_DSEFFECTMAP);
                if (g_activeDspImageDesc) {
                    free(g_activeDspImageDesc);
                    g_activeDspImageDesc = nullptr;
                }
                g_activeDspImageDesc = static_cast<XBOX_DSEFFECTIMAGEDESC*>(calloc(1, allocBytes));
                memcpy(g_activeDspImageDesc, srcDesc, allocBytes);

                static constexpr uint32_t kDspStateArenaSize = 0x84000u;
                uint32_t minScratch = (srcDesc->dwTotalScratchSize > 0x10000u) ? srcDesc->dwTotalScratchSize : 0x40000u;
                uint32_t scratchCap = 0x40000u;
                while (scratchCap < minScratch && scratchCap < 0x1000000u) {
                    scratchCap <<= 1u;
                }
                g_dspScratchMemory.assign(kDspStateArenaSize + scratchCap + 0x4000u, 0u);
                uint8_t* stateBase   = g_dspScratchMemory.data();
                uint8_t* scratchBase = stateBase + kDspStateArenaSize;

                uint64_t stateImgOff = 0x818ULL + static_cast<uint64_t>(codeWords) * 4ULL;
                uint64_t stateBytes  = static_cast<uint64_t>(stateWords) * 4ULL;
                if (stateImgOff + stateBytes <= dwImageSize && stateBytes <= kDspStateArenaSize) {
                    memcpy(stateBase, img + stateImgOff, static_cast<size_t>(stateBytes));
                }

                for (uint32_t i = 0u; i < fxCount; ++i) {
                    XBOX_DSEFFECTMAP& m = g_activeDspImageDesc->aEffectMaps[i];
                    uint32_t stOff = reinterpret_cast<uintptr_t>(m.lpvStateSegment) & 0x7FFFCu;
                    if (m.dwStateSize <= kDspStateArenaSize && stOff + m.dwStateSize > kDspStateArenaSize) {
                        stOff = 0u;
                    }
                    uint32_t scOff = reinterpret_cast<uintptr_t>(m.lpvScratchSegment) & (scratchCap - 1u);
                    if (m.dwScratchSize <= scratchCap && scOff + m.dwScratchSize > scratchCap) {
                        scOff = 0u;
                    }
                    m.lpvStateSegment   = stateBase + stOff;
                    m.lpvScratchSegment = scratchBase + scOff;
                }

                if (ppImageDesc) {
                    *ppImageDesc = g_activeDspImageDesc;
                }
                LeaveCriticalSection(&g_audioCs);
                return S_OK;
            }
        }
    }

    if (!g_activeDspImageDesc) {
        g_activeDspImageDesc = static_cast<XBOX_DSEFFECTIMAGEDESC*>(calloc(1, 256u));
    }
    if (ppImageDesc) *ppImageDesc = g_activeDspImageDesc;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_SetMixBinHeadroom(XboxDSObject*, DWORD, DWORD) { return S_OK; }

static HRESULT __stdcall Bridge_IDirectSound_SetI3DL2Listener(
    XboxDSObject* pDS, const void* pI3DL2, DWORD dwFlags)
{
    (void)pDS;
    if (!pI3DL2 || reinterpret_cast<uintptr_t>(pI3DL2) < kMinValidHostAddress) return S_OK;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    const XBOX_DSI3DL2LISTENER* src = reinterpret_cast<const XBOX_DSI3DL2LISTENER*>(pI3DL2);
    if ((dwFlags & 1u) != 0u) { // DS3D_DEFERRED
        g_i3dl2ListenerDeferred = *src;
        g_hasDeferredI3DL2      = true;
    } else {
        g_i3dl2Listener         = *src;
        g_i3dl2ListenerDeferred = *src;
        g_hasDeferredI3DL2      = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CommitDeferredSettings(XboxDSObject* pDS) {
    (void)pDS;
    EnsureAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (g_hasDeferredI3DL2) {
        g_i3dl2Listener    = g_i3dl2ListenerDeferred;
        g_hasDeferredI3DL2 = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

void ShutdownXboxAudioBuffersLocked() {
    for (size_t i = 0u; i < g_activeBuffers.size(); ++i) {
        XboxDSBufferObject* b = g_activeBuffers[i];
        if (b) {
            b->status = 0u;
        }
    }
    if (g_activeDspImageDesc) {
        free(g_activeDspImageDesc);
        g_activeDspImageDesc = nullptr;
    }
    g_dspScratchMemory.clear();
    g_dspScratchMemory.shrink_to_fit();
}

void InitXboxAudioBuffers() {
    InstallBridge(0x006E3096, &Bridge_DirectSoundCreate);                   // _DirectSoundCreate@12

    // IDirectSound methods
    InstallBridge(0x006E00AA, &DS_Release);                                 // _IDirectSound_Release@4
    InstallBridge(0x006E2E88, &Bridge_IDirectSound_CreateSoundBuffer);      // _IDirectSound_CreateSoundBuffer@16
    InstallBridge(0x006E1799, &Bridge_IDirectSound_DownloadEffectsImage);   // _IDirectSound_DownloadEffectsImage@20
    InstallBridge(0x006E17C0, &Bridge_IDirectSound_SetMixBinHeadroom);      // _IDirectSound_SetMixBinHeadroom@12
    InstallBridge(0x006E2ED0, &Bridge_IDirectSound_SetI3DL2Listener);       // _IDirectSound_SetI3DL2Listener@12
    InstallBridge(0x006E2C20, &Bridge_IDirectSound_CommitDeferredSettings); // _IDirectSound_CommitDeferredSettings@4

    // IDirectSoundBuffer methods
    InstallBridge(0x006E00C5, &Buffer_Release);                             // _IDirectSoundBuffer_Release@4
    InstallBridge(0x006E00C0, &Buffer_Unlock);                              // _IDirectSoundBuffer_Unlock@20
    InstallBridge(0x006E18A4, &Buffer_Play);                                // _IDirectSoundBuffer_Play@16
    InstallBridge(0x006E18C8, &Buffer_Stop);                                // _IDirectSoundBuffer_Stop@4
    InstallBridge(0x006E18E0, &Buffer_StopEx);                              // _IDirectSoundBuffer_StopEx@16
    InstallBridge(0x006E1904, &Buffer_SetLoopRegion);                       // _IDirectSoundBuffer_SetLoopRegion@12
    InstallBridge(0x006E1924, &Buffer_GetStatus);                           // _IDirectSoundBuffer_GetStatus@8
    InstallBridge(0x006E1940, &Buffer_GetCurrentPosition);                  // _IDirectSoundBuffer_GetCurrentPosition@12
    InstallBridge(0x006E1960, &Buffer_SetCurrentPosition);                  // _IDirectSoundBuffer_SetCurrentPosition@8
    InstallBridge(0x006E197C, &Buffer_Lock);                                // _IDirectSoundBuffer_Lock@32
    InstallBridge(0x006E19AC, &Buffer_GetVoiceProperties);                  // _IDirectSoundBuffer_GetVoiceProperties@8
    InstallBridge(0x006E19C8, &Buffer_Set3DVoiceData);                      // _IDirectSoundBuffer_Set3DVoiceData@8
    InstallBridge(0x006E19E4, &Buffer_Use3DVoiceData);                      // _IDirectSoundBuffer_Use3DVoiceData@8
    InstallBridge(0x006E17E0, &Buffer_SetVolume);                           // _IDirectSoundBuffer_SetVolume@8
    InstallBridge(0x006E17FC, &Buffer_SetPitch);                            // _IDirectSoundBuffer_SetPitch@8
    InstallBridge(0x006E1818, &Buffer_SetLFO);                              // _IDirectSoundBuffer_SetLFO@8
    InstallBridge(0x006E1834, &Buffer_SetEG);                               // _IDirectSoundBuffer_SetEG@8
    InstallBridge(0x006E1850, &Buffer_SetFilter);                           // _IDirectSoundBuffer_SetFilter@8
    InstallBridge(0x006E186C, &Buffer_SetHeadroom);                         // _IDirectSoundBuffer_SetHeadroom@8
    InstallBridge(0x006E1888, &Buffer_SetMixBinVolumes);                    // _IDirectSoundBuffer_SetMixBinVolumes@8
    InstallBridge(0x006E207D, &Buffer_SetFrequency);                        // _IDirectSoundBuffer_SetFrequency@8
    InstallBridge(0x006E2099, &Buffer_SetOutputBuffer);                     // _IDirectSoundBuffer_SetOutputBuffer@8
    InstallBridge(0x006E20B5, &Buffer_SetMixBins);                          // _IDirectSoundBuffer_SetMixBins@8
    InstallBridge(0x006E20D1, &Buffer_SetPlayRegion);                       // _IDirectSoundBuffer_SetPlayRegion@12
    InstallBridge(0x006E271F, &Buffer_SetFormat);                           // _IDirectSoundBuffer_SetFormat@8
    InstallBridge(0x006E273B, &Buffer_SetBufferData);                       // _IDirectSoundBuffer_SetBufferData@12
}
