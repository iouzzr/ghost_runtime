#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <math.h>
#include <float.h>
#include <vector>

// Defined in audio_buffer.cpp to pre-allocate submix storage under g_audioCs
extern void EnsureXboxBufferSubmixReadyLocked(XboxDSBufferObject* submix);

// ============================================================================
// Xbox DirectSoundStream Structures & Constants
// ============================================================================

#pragma pack(push, 1)
struct XBOX_DSSTREAMDESC {
    uint32_t            dwFlags;
    uint32_t            dwMaxAttachedPackets;
    const WAVEFORMATEX* lpwfxFormat;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*               lpvContext;
    const void*         lpMixBins;
};

struct XBOX_XMEDIAPACKET {
    void*     pvBuffer;
    uint32_t  dwMaxSize;
    uint32_t* pdwCompletedSize;
    uint32_t* pdwStatus;
    union {
        HANDLE hCompletionEvent;
        void*  pContext;
    };
    uint64_t* prtTimestamp;
};

struct XBOX_XMEDIAINFO {
    uint32_t dwFlags;
    uint32_t dwInputSize;
    uint32_t dwOutputSize;
    uint32_t dwMaxLookahead;
};
#pragma pack(pop)

static constexpr DWORD    kXboxStreamStatusReady            = 0x00000001u; // XMO_STATUSF_ACCEPT_INPUT_DATA | DSSTREAMSTATUS_READY
static constexpr DWORD    kXboxStreamStatusPlaying          = 0x00010000u; // DSSTREAMSTATUS_PLAYING
static constexpr DWORD    kXboxStreamStatusPaused           = 0x00020000u; // DSSTREAMSTATUS_PAUSED
static constexpr DWORD    kXboxStreamStatusStarved          = 0x00040000u; // DSSTREAMSTATUS_STARVED
static constexpr DWORD    kXboxStreamStatusEnvelopeComplete = 0x00080000u; // Checked by XACT::CSoundCue::DoWork at 0x00708BD0

static constexpr DWORD    kXboxStreamPauseResume          = 0u;          // DSSTREAMPAUSE_RESUME
static constexpr DWORD    kXboxStreamPausePause           = 1u;          // DSSTREAMPAUSE_PAUSE
static constexpr DWORD    kXboxStreamPauseSyncPause       = 2u;          // DSSTREAMPAUSE_SYNCPAUSE
static constexpr DWORD    kXboxStreamPauseResumeSyncPause = 3u;          // DSSTREAMPAUSE_RESUMESYNCPAUSE
static constexpr uint32_t kXboxMediaPacketStatusSuccess = 0u;          // XMEDIAPACKET_STATUS_SUCCESS
static constexpr uint32_t kXboxMediaPacketStatusPending = 0x8000000Au; // XMEDIAPACKET_STATUS_PENDING
static constexpr uint32_t kXboxMediaPacketStatusFlushed = 0x40000005u; // XMEDIAPACKET_STATUS_FLUSHED

static constexpr float    kInvPcm16Scale                = 1.0f / 32768.0f;
static constexpr float    kMillibelsPerTwentyDecibels   = 2000.0f;

static inline void EnsureStreamAudioLockInitialized() {
    EnsureXboxAudioCsInitialized();
}

static uint32_t ExtractStreamFxSendBinMask(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count) {
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

// ============================================================================
// Pooled Stream Sample Chunks & Stream State
// ============================================================================

struct StreamSampleChunk {
    std::vector<int16_t> samplesStereo; // Interleaved stereo PCM16 (2 * numFrames)
    uint32_t             numFrames;
};

struct QueuedStreamPacket {
    StreamSampleChunk* chunk;
    void*              pContext;
    uint32_t*          pdwStatus;
    uint32_t*          pdwCompletedSize;
    uint32_t           completedBytes;
    HANDLE             hEvent;
    bool               hasEvent;
};

struct CompletedStreamPacket {
    void*     pContext;
    uint32_t* pdwStatus;
    uint32_t* pdwCompletedSize;
    uint32_t  completedBytes;
    HANDLE    hEvent;
    bool      hasEvent;
};

struct XboxDSStreamObject {
    void**                  vtable; // 7-entry XMO vtable at +0x00
    LONG                    refCount;
    WAVEFORMATEX            wfx;
    bool                    isAdpcm;
    uint32_t                maxAttachedPackets;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*                   lpvContext;
    LONG                    volume;
    LONG                    headroomMb;
    LONG                    pan;
    LONG                    mixBinAttenMb;
    LONG                    atten3D;
    LONG                    lPitch;
    DWORD                   freq;
    bool                    paused;
    bool                    discontinuity;
    bool                    flushExPending;
    bool                    isReleasing;
    bool                    use3DVoiceData;
    bool                    hasI3DL2Send;
    int32_t                 i3dl2SendMb;
    uint32_t                fxSendBinMask;
    double                  resamplePos;
    float                   prevSampleL;
    float                   prevSampleR;
    bool                    hasPrevSample;
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
    std::vector<QueuedStreamPacket>    activePackets;
    std::vector<CompletedStreamPacket> completedPackets;
};

static std::vector<XboxDSStreamObject*> g_activeStreams;
static std::vector<StreamSampleChunk*>  g_freeStreamChunks;
static std::vector<StreamSampleChunk*>  g_retiredStreamChunks;

static StreamSampleChunk* AcquireStreamSampleChunk(uint32_t requiredFrames) {
    StreamSampleChunk* chunk = nullptr;
    EnsureStreamAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (!g_freeStreamChunks.empty()) {
        chunk = g_freeStreamChunks.back();
        g_freeStreamChunks.pop_back();
    }
    // Pre-reserve pool capacities on the caller thread so RT mixer-thread recycling never reallocates or frees
    if (g_freeStreamChunks.capacity() < g_freeStreamChunks.size() + 256u) {
        g_freeStreamChunks.reserve(g_freeStreamChunks.size() + 512u);
    }
    if (g_retiredStreamChunks.capacity() < g_retiredStreamChunks.size() + 256u) {
        g_retiredStreamChunks.reserve(g_retiredStreamChunks.size() + 512u);
    }
    LeaveCriticalSection(&g_audioCs);

    if (!chunk) {
        chunk = new StreamSampleChunk();
    }
    size_t neededSamples = static_cast<size_t>(requiredFrames) * 2u;
    if (chunk->samplesStereo.size() < neededSamples) {
        chunk->samplesStereo.resize(neededSamples);
    }
    chunk->numFrames = 0u;
    return chunk;
}

// Zero-heap-activity on the real-time WASAPI mixer thread: pushes into pre-reserved g_freeStreamChunks
// or g_retiredStreamChunks (reclaimed on the game thread in UpdateXboxAudioTick / ShutdownXboxAudio).
static void RecycleStreamSampleChunkLocked(StreamSampleChunk* chunk) {
    if (!chunk) return;
    chunk->numFrames = 0u;
    if (g_freeStreamChunks.size() < 256u &&
        g_freeStreamChunks.size() < g_freeStreamChunks.capacity() &&
        chunk->samplesStereo.capacity() <= 262144u)
    {
        g_freeStreamChunks.push_back(chunk);
    } else if (g_retiredStreamChunks.size() < g_retiredStreamChunks.capacity()) {
        g_retiredStreamChunks.push_back(chunk);
    } else {
        // Extreme fallback only if >512 oversized packets retired without a game-thread tick
        g_retiredStreamChunks.push_back(chunk);
    }
}

void ClearStreamsReferencingOutputBufferLocked(XboxDSBufferObject* buf) {
    if (!buf) return;
    for (size_t i = 0u; i < g_activeStreams.size(); ++i) {
        if (g_activeStreams[i] && g_activeStreams[i]->outputBuffer == buf) {
            g_activeStreams[i]->outputBuffer = nullptr;
        }
    }
}

// ============================================================================
// 48,000 Hz Real-Time Stream Voice Synthesis (Called Under g_audioCs)
// ============================================================================

static inline void ComputeStreamVoiceGains(
    LONG volumeMb, LONG headroomMb, int32_t lfoEgVolModMb,
    bool active3D, LONG atten3DMb, const Xbox3DVoiceMetrics& voice3D,
    LONG mixBinAttenMb, LONG panMb,
    bool hasI3DL2Send, int32_t i3dl2SendMb,
    float* outDryGainL, float* outDryGainR,
    float* outWetGainL, float* outWetGainR)
{
    LONG baseMb     = volumeMb - headroomMb + lfoEgVolModMb;
    LONG drySpatial = active3D ? atten3DMb : mixBinAttenMb;
    LONG totalDryMb = baseMb + drySpatial;
    if (totalDryMb < kMinDirectSoundVolumeMb) totalDryMb = kMinDirectSoundVolumeMb;
    if (totalDryMb > kMaxDirectSoundVolumeMb) totalDryMb = kMaxDirectSoundVolumeMb;

    float dryGain = (totalDryMb <= kMinDirectSoundVolumeMb)
        ? 0.0f
        : ((totalDryMb < 0) ? powf(10.0f, static_cast<float>(totalDryMb) / kMillibelsPerTwentyDecibels) : 1.0f);

    if (panMb < kMinDirectSoundPanMb) panMb = kMinDirectSoundPanMb;
    if (panMb > kMaxDirectSoundPanMb) panMb = kMaxDirectSoundPanMb;
    float panScaleL = (panMb > 0) ? powf(10.0f, -static_cast<float>(panMb) / kMillibelsPerTwentyDecibels) : 1.0f;
    float panScaleR = (panMb < 0) ? powf(10.0f,  static_cast<float>(panMb) / kMillibelsPerTwentyDecibels) : 1.0f;

    *outDryGainL = dryGain * panScaleL;
    *outDryGainR = dryGain * panScaleR;

    if (hasI3DL2Send && i3dl2SendMb > -9500) {
        int32_t wetSpatialMb = active3D
            ? (voice3D.distanceVol + voice3D.coneVol + voice3D.i3dl2RoomVol + i3dl2SendMb)
            : (mixBinAttenMb + i3dl2SendMb);
        int32_t totalWetMb = baseMb + wetSpatialMb;
        if (totalWetMb < kMinDirectSoundVolumeMb) totalWetMb = kMinDirectSoundVolumeMb;
        if (totalWetMb > kMaxDirectSoundVolumeMb) totalWetMb = kMaxDirectSoundVolumeMb;
        float wetGain = (totalWetMb <= -9500)
            ? 0.0f
            : powf(10.0f, static_cast<float>(totalWetMb) / kMillibelsPerTwentyDecibels);
        *outWetGainL = wetGain * (0.72f * panScaleL + 0.28f * panScaleR);
        *outWetGainR = wetGain * (0.28f * panScaleL + 0.72f * panScaleR);
    } else {
        *outWetGainL = 0.0f;
        *outWetGainR = 0.0f;
    }
}

static void RetireHeadStreamPacketLocked(XboxDSStreamObject* s) {
    if (!s || s->activePackets.empty()) return;
    QueuedStreamPacket& head = s->activePackets[0];
    if (head.chunk && head.chunk->numFrames > 0u) {
        uint32_t lastIdx = (head.chunk->numFrames - 1u) * 2u;
        s->prevSampleL   = static_cast<float>(head.chunk->samplesStereo[lastIdx + 0u]) * kInvPcm16Scale;
        s->prevSampleR   = static_cast<float>(head.chunk->samplesStereo[lastIdx + 1u]) * kInvPcm16Scale;
        s->hasPrevSample = true;
    }
    CompletedStreamPacket done = {};
    done.pContext         = head.pContext;
    done.pdwStatus        = head.pdwStatus;
    done.pdwCompletedSize = head.pdwCompletedSize;
    done.completedBytes   = head.completedBytes;
    done.hEvent           = head.hEvent;
    done.hasEvent         = head.hasEvent;
    s->completedPackets.push_back(done);

    if (head.chunk) {
        RecycleStreamSampleChunkLocked(head.chunk);
        head.chunk = nullptr;
    }
    s->activePackets.erase(s->activePackets.begin());
}

void MixActiveXboxStreams48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames)
{
    if (numFrames == 0u) return;

    float voiceScratchL[kAudioMixSubQuantumFrames];
    float voiceScratchR[kAudioMixSubQuantumFrames];
    float postDryL[kAudioMixSubQuantumFrames];
    float postDryR[kAudioMixSubQuantumFrames];

    for (size_t si = 0u; si < g_activeStreams.size(); ++si) {
        XboxDSStreamObject* s = g_activeStreams[si];
        if (!s || s->paused) continue;

        // Drain any leading 0-frame marker packets in FIFO order
        while (!s->activePackets.empty() &&
               (!s->activePackets[0].chunk || s->activePackets[0].chunk->numFrames == 0u))
        {
            RetireHeadStreamPacketLocked(s);
        }

        if (s->activePackets.empty()) {
            // Keep advancing release sample clock on starved streams after FlushEx(ENVELOPE)
            if (s->isReleasing) {
                s->releaseSamples48k += numFrames;
            }
            continue;
        }

        uint32_t offset = 0u;
        while (offset < numFrames && !s->activePackets.empty()) {
            uint32_t chunkFrames = numFrames - offset;
            if (chunkFrames > kAudioMixSubQuantumFrames) {
                chunkFrames = kAudioMixSubQuantumFrames;
            }

            int32_t lfoEgVolModMb    = 0;
            float   lfoEgPitchRatio  = 1.0f;
            float   lfoEgCutoffRatio = 1.0f;
            if (s->lfoActive[0] || s->lfoActive[1] || s->egActive[0] || s->egActive[1]) {
                EvaluateXboxVoiceLfoAndEgSamples(
                    s->lfo, s->lfoActive, s->eg, s->egActive,
                    s->activeSamples48k, s->isReleasing, s->releaseSamples48k,
                    &lfoEgVolModMb, &lfoEgPitchRatio, &lfoEgCutoffRatio, nullptr);
            }

            bool  active3D = s->use3DVoiceData && s->voice3D.active3D;
            DWORD effFreq  = s->freq ? s->freq : (s->wfx.nSamplesPerSec ? s->wfx.nSamplesPerSec : kDefaultSampleRateHz);
            if (active3D && s->voice3D.dopplerPitch != 0) {
                effFreq = XboxPitchToFrequencyHz(s->lPitch + s->voice3D.dopplerPitch);
            }
            if (lfoEgPitchRatio > 0.01f && fabsf(lfoEgPitchRatio - 1.0f) > 0.0005f) {
                double modHz = static_cast<double>(effFreq) * static_cast<double>(lfoEgPitchRatio);
                if (modHz < static_cast<double>(kMinDirectSoundFreqHz)) modHz = static_cast<double>(kMinDirectSoundFreqHz);
                if (modHz > static_cast<double>(kMaxXboxPitchFreqHz))   modHz = static_cast<double>(kMaxXboxPitchFreqHz);
                effFreq = static_cast<DWORD>(modHz + 0.5);
            }
            if (effFreq < kMinDirectSoundFreqHz) effFreq = kMinDirectSoundFreqHz;
            if (effFreq > kMaxXboxPitchFreqHz)   effFreq = kMaxXboxPitchFreqHz;

            double   step     = static_cast<double>(effFreq) / static_cast<double>(kDefaultSampleRateHz);
            double   pos      = s->resamplePos;
            uint32_t rendered = 0u;

            for (; rendered < chunkFrames; ++rendered) {
                while (!s->activePackets.empty()) {
                    StreamSampleChunk* hc = s->activePackets[0].chunk;
                    if (!hc || hc->numFrames == 0u) {
                        RetireHeadStreamPacketLocked(s);
                        continue;
                    }
                    if (pos >= static_cast<double>(hc->numFrames)) {
                        pos -= static_cast<double>(hc->numFrames);
                        RetireHeadStreamPacketLocked(s);
                        continue;
                    }
                    break;
                }

                if (s->activePackets.empty()) {
                    // Preserve fractional phase across packet arrivals unless huge overshoot
                    if (pos >= 4.0 || pos < 0.0) pos = 0.0;
                    break;
                }

                const StreamSampleChunk* headChunk = s->activePackets[0].chunk;
                const int16_t*           headData  = headChunk->samplesStereo.data();
                uint32_t                 headLen   = headChunk->numFrames;

                if (!s->hasPrevSample) {
                    s->prevSampleL   = static_cast<float>(headData[0]) * kInvPcm16Scale;
                    s->prevSampleR   = static_cast<float>(headData[1]) * kInvPcm16Scale;
                    s->hasPrevSample = true;
                }

                uint32_t idx0 = static_cast<uint32_t>(pos);
                if (idx0 >= headLen) idx0 = headLen - 1u;
                float frac = static_cast<float>(pos - static_cast<double>(idx0));
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;

                float l0 = static_cast<float>(headData[idx0 * 2u + 0u]) * kInvPcm16Scale;
                float r0 = static_cast<float>(headData[idx0 * 2u + 1u]) * kInvPcm16Scale;

                if (frac <= 1e-6f) {
                    voiceScratchL[rendered] = l0;
                    voiceScratchR[rendered] = r0;
                } else {
                    float l1 = l0;
                    float r1 = r0;
                    if (idx0 + 1u < headLen) {
                        l1 = static_cast<float>(headData[(idx0 + 1u) * 2u + 0u]) * kInvPcm16Scale;
                        r1 = static_cast<float>(headData[(idx0 + 1u) * 2u + 1u]) * kInvPcm16Scale;
                    } else {
                        // Seamlessly peek into the next non-empty queued packet to avoid boundary clicks
                        for (size_t ni = 1u; ni < s->activePackets.size(); ++ni) {
                            const StreamSampleChunk* nextChunk = s->activePackets[ni].chunk;
                            if (nextChunk && nextChunk->numFrames > 0u) {
                                l1 = static_cast<float>(nextChunk->samplesStereo[0]) * kInvPcm16Scale;
                                r1 = static_cast<float>(nextChunk->samplesStereo[1]) * kInvPcm16Scale;
                                break;
                            }
                        }
                    }
                    voiceScratchL[rendered] = l0 + (l1 - l0) * frac;
                    voiceScratchR[rendered] = r0 + (r1 - r0) * frac;
                }

                pos += step;
            }

            // Retire any packets fully consumed on the final step of this sub-quantum
            while (!s->activePackets.empty()) {
                StreamSampleChunk* hc = s->activePackets[0].chunk;
                if (!hc || hc->numFrames == 0u) {
                    RetireHeadStreamPacketLocked(s);
                    continue;
                }
                if (pos >= static_cast<double>(hc->numFrames)) {
                    pos -= static_cast<double>(hc->numFrames);
                    RetireHeadStreamPacketLocked(s);
                    continue;
                }
                break;
            }
            if (s->activePackets.empty() && (pos >= 4.0 || pos < 0.0)) {
                pos = 0.0;
            }
            s->resamplePos = pos;

            if (rendered > 0u) {
                s->activeSamples48k += rendered;
                if (s->isReleasing) {
                    s->releaseSamples48k += rendered;
                }

                ApplyVoiceSvfAndHrtfFloatStereo(
                    voiceScratchL, voiceScratchR, rendered,
                    &s->filterDesc, s->hasActiveFilter, lfoEgCutoffRatio,
                    active3D, s->voice3D.hrtfFilterCoeff, s->voice3D.itdDelaySamples,
                    &s->filterState);

                float dryGainL = 0.0f, dryGainR = 0.0f, wetGainL = 0.0f, wetGainR = 0.0f;
                LONG  effPanMb = active3D ? s->voice3D.pan : s->pan;
                ComputeStreamVoiceGains(
                    s->volume, s->headroomMb, lfoEgVolModMb,
                    active3D, s->atten3D, s->voice3D,
                    s->mixBinAttenMb, effPanMb,
                    s->hasI3DL2Send, s->i3dl2SendMb,
                    &dryGainL, &dryGainR, &wetGainL, &wetGainR);

                for (uint32_t i = 0u; i < rendered; ++i) {
                    postDryL[i] = voiceScratchL[i] * dryGainL;
                    postDryR[i] = voiceScratchR[i] * dryGainR;
                    if (wetGainL > 0.0f || wetGainR > 0.0f) {
                        ioReverbSendL[offset + i] += voiceScratchL[i] * wetGainL;
                        ioReverbSendR[offset + i] += voiceScratchR[i] * wetGainR;
                    }
                }

                bool routedToSubmix = false;
                if (s->outputBuffer || s->fxSendBinMask != 0u) {
                    routedToSubmix = AccumulateToXboxSubmixBufferLocked(
                        s->outputBuffer, s->fxSendBinMask, postDryL, postDryR, offset, rendered);
                }

                if (!s->outputBuffer || !routedToSubmix) {
                    for (uint32_t i = 0u; i < rendered; ++i) {
                        ioMasterDryL[offset + i] += postDryL[i];
                        ioMasterDryR[offset + i] += postDryR[i];
                    }
                }
            }

            if (rendered < chunkFrames) {
                uint32_t rem = chunkFrames - rendered;
                if (s->isReleasing) {
                    s->releaseSamples48k += rem;
                }
                offset += chunkFrames;
                break;
            }
            offset += rendered;
        }

        if (offset < numFrames && s->isReleasing) {
            s->releaseSamples48k += (numFrames - offset);
        }
    }
}

// ============================================================================
// IDirectSoundStream Virtual & Direct Xbox Methods
// ============================================================================

static ULONG __stdcall Stream_AddRef(XboxDSStreamObject* s) {
    return s ? static_cast<ULONG>(InterlockedIncrement(&s->refCount)) : 1u;
}

static ULONG __stdcall Stream_Release(XboxDSStreamObject* s) {
    if (!s) return 0u;
    LONG r = InterlockedDecrement(&s->refCount);
    if (r <= 0) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        for (size_t i = 0u; i < g_activeStreams.size(); ++i) {
            if (g_activeStreams[i] == s) {
                g_activeStreams.erase(g_activeStreams.begin() + i);
                break;
            }
        }
        for (size_t i = 0u; i < s->activePackets.size(); ++i) {
            if (s->activePackets[i].chunk) {
                RecycleStreamSampleChunkLocked(s->activePackets[i].chunk);
                s->activePackets[i].chunk = nullptr;
            }
        }
        s->activePackets.clear();
        s->completedPackets.clear();
        LeaveCriticalSection(&g_audioCs);
        delete s;
        return 0u;
    }
    return static_cast<ULONG>(r);
}

static HRESULT __stdcall Stream_GetInfo(XboxDSStreamObject* s, XBOX_XMEDIAINFO* pInfo) {
    (void)s;
    if (pInfo) {
        pInfo->dwFlags        = 0x0000000Bu; // XMO_STREAMF_FIXED_SAMPLE_SIZE | INPUT_ASYNC
        pInfo->dwInputSize    = 36u;
        pInfo->dwOutputSize   = 0u;
        pInfo->dwMaxLookahead = 4096u;
    }
    return S_OK;
}

static HRESULT __stdcall Stream_GetStatus(XboxDSStreamObject* s, DWORD* pdwStatus) {
    if (pdwStatus) {
        DWORD st = 0u;
        if (s) {
            EnsureStreamAudioLockInitialized();
            EnterCriticalSection(&g_audioCs);
            size_t   attachedCount = s->activePackets.size() + s->completedPackets.size();
            uint32_t maxPkts       = s->maxAttachedPackets ? s->maxAttachedPackets : 16u;
            bool     hasAttached   = (attachedCount > 0u);
            bool     isPaused      = s->paused;
            bool     flushPending  = s->flushExPending || s->discontinuity;
            bool     releaseDone   = true;
            if (s->isReleasing && s->egActive[1] && s->eg[1].dwRelease > 0u) {
                uint32_t relStepsMax = s->eg[1].dwRelease & 0xFFFu;
                uint64_t neededSamples48k = static_cast<uint64_t>(relStepsMax) * 512ULL;
                releaseDone = (s->releaseSamples48k >= neededSamples48k);
            }
            if (attachedCount < maxPkts && !s->flushExPending) {
                st |= kXboxStreamStatusReady;
            }
            LeaveCriticalSection(&g_audioCs);

            // VIDAudioHelper::Update (0x0043D4AB) checks DSSTREAMSTATUS_PAUSED (0x00020000) to unpause streams;
            // CMcpxStream::GetStatus (0x006E4E64) sets 0x00040000 (STARVED) when starved, and XACT::CSoundCue::DoWork
            // (0x00708BD0) checks 0x00080000 after FlushEx/Discontinuity to detect EOF/envelope completion.
            if (isPaused) {
                st |= kXboxStreamStatusPaused;
            }
            if (hasAttached) {
                if (!isPaused) {
                    st |= kXboxStreamStatusPlaying;
                }
            } else {
                st |= kXboxStreamStatusStarved;
                if (flushPending && releaseDone) {
                    st |= kXboxStreamStatusEnvelopeComplete;
                }
            }
        } else {
            st = kXboxStreamStatusReady | kXboxStreamStatusStarved | kXboxStreamStatusEnvelopeComplete;
        }
        *pdwStatus = st;
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Process(
    XboxDSStreamObject* s, const XBOX_XMEDIAPACKET* pInputPacket, const XBOX_XMEDIAPACKET* pOutputPacket)
{
    (void)pOutputPacket;
    if (!s || !pInputPacket) return S_OK;

    EnsureStreamAudioLockInitialized();
    EnsureXboxWasapiMixerStarted();

    uint32_t bytes = pInputPacket->dwMaxSize;
    if (pInputPacket->pdwCompletedSize) {
        *pInputPacket->pdwCompletedSize = bytes;
    }

    WAVEFORMATEX wfxSnap = {};
    bool         isAdpcm = false;
    EnterCriticalSection(&g_audioCs);
    wfxSnap = s->wfx;
    isAdpcm = s->isAdpcm;
    LeaveCriticalSection(&g_audioCs);

    StreamSampleChunk* decodedChunk = nullptr;
    if (pInputPacket->pvBuffer && bytes > 0u) {
        uint32_t ch   = wfxSnap.nChannels ? wfxSnap.nChannels : 2u;
        uint16_t bits = (!isAdpcm && wfxSnap.wBitsPerSample == 8u) ? 8u : 16u;
        uint32_t maxFrames = 0u;
        if (isAdpcm) {
            uint32_t blockAlign = 36u * ch;
            maxFrames = (blockAlign > 0u) ? ((bytes / blockAlign) * 64u) : 0u;
        } else {
            uint32_t frameBytes = ch * (bits / 8u);
            maxFrames = (frameBytes > 0u) ? (bytes / frameBytes) : 0u;
        }

        if (maxFrames > 0u) {
            decodedChunk = AcquireStreamSampleChunk(maxFrames);
            uint32_t actualFrames = 0u;
            UnpackXboxWaveToStereoPcm16(
                reinterpret_cast<const uint8_t*>(pInputPacket->pvBuffer),
                bytes, ch, isAdpcm, bits,
                decodedChunk->samplesStereo.data(), maxFrames, &actualFrames);
            decodedChunk->numFrames = actualFrames;
            if (actualFrames == 0u) {
                EnterCriticalSection(&g_audioCs);
                RecycleStreamSampleChunkLocked(decodedChunk);
                LeaveCriticalSection(&g_audioCs);
                decodedChunk = nullptr;
            }
        }
    }

    // On Original Xbox, XMEDIAPACKET_STATUS_PENDING is 0x8000000A (E_PENDING).
    // Both callback-driven streams (Bink/DSound) and polling-driven streams (XACT CSoundSource)
    // require *pdwStatus == 0x8000000A until _DirectSoundDoWork@0 completes the packet on the game thread.
    if (pInputPacket->pdwStatus) {
        *pInputPacket->pdwStatus = kXboxMediaPacketStatusPending;
    }

    EnterCriticalSection(&g_audioCs);
    s->discontinuity  = false;
    s->flushExPending = false;
    s->isReleasing    = false;
    s->releaseSamples48k = 0ULL;

    size_t minCap = s->activePackets.size() + s->completedPackets.size() + 16u;
    if (s->activePackets.capacity() < minCap) {
        s->activePackets.reserve(minCap * 2u);
    }
    if (s->completedPackets.capacity() < minCap) {
        s->completedPackets.reserve(minCap * 2u);
    }

    QueuedStreamPacket pkt = {};
    pkt.chunk            = decodedChunk;
    pkt.pContext         = pInputPacket->pContext;
    pkt.pdwStatus        = pInputPacket->pdwStatus;
    pkt.pdwCompletedSize = pInputPacket->pdwCompletedSize;
    pkt.completedBytes   = bytes;
    pkt.hEvent           = (!s->lpfnCallback) ? pInputPacket->hCompletionEvent : nullptr;
    pkt.hasEvent         = (!s->lpfnCallback && pInputPacket->hCompletionEvent != nullptr);
    s->activePackets.push_back(pkt);
    LeaveCriticalSection(&g_audioCs);

    return S_OK;
}

static HRESULT __stdcall Stream_Discontinuity(XboxDSStreamObject* s) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->discontinuity = true;
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0.0f;
        s->prevSampleR   = 0.0f;
        s->hasPrevSample = false;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static void DispatchCompletedStreamPackets(
    XboxDSStreamObject* s, const std::vector<CompletedStreamPacket>& list, uint32_t status)
{
    for (size_t i = 0u; i < list.size(); ++i) {
        if (list[i].pdwCompletedSize) {
            *list[i].pdwCompletedSize = list[i].completedBytes;
        }
        if (list[i].pdwStatus) {
            *list[i].pdwStatus = status;
        }
        if (s->lpfnCallback) {
            s->lpfnCallback(s->lpvContext, list[i].pContext, status);
        } else if (list[i].hasEvent && list[i].hEvent &&
                   !IsStaticXboxImageOrNullPtr(list[i].hEvent) &&
                   (reinterpret_cast<uintptr_t>(list[i].hEvent) & 1u) == 0u)
        {
            SetEvent(list[i].hEvent);
        }
    }
}

static HRESULT __stdcall Stream_Flush(XboxDSStreamObject* s) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        Stream_AddRef(s);

        std::vector<CompletedStreamPacket> alreadyDone;
        std::vector<CompletedStreamPacket> flushedActive;

        EnterCriticalSection(&g_audioCs);
        alreadyDone.swap(s->completedPackets);
        flushedActive.reserve(s->activePackets.size());
        for (size_t i = 0u; i < s->activePackets.size(); ++i) {
            QueuedStreamPacket& q = s->activePackets[i];
            CompletedStreamPacket c = {};
            c.pContext         = q.pContext;
            c.pdwStatus        = q.pdwStatus;
            c.pdwCompletedSize = q.pdwCompletedSize;
            c.completedBytes   = q.completedBytes;
            c.hEvent           = q.hEvent;
            c.hasEvent         = q.hasEvent;
            flushedActive.push_back(c);
            if (q.chunk) {
                RecycleStreamSampleChunkLocked(q.chunk);
                q.chunk = nullptr;
            }
        }
        s->activePackets.clear();

        s->discontinuity     = false;
        s->flushExPending    = false;
        s->isReleasing       = false;
        s->activeSamples48k  = 0ULL;
        s->releaseSamples48k = 0ULL;
        s->resamplePos       = 0.0;
        s->prevSampleL       = 0.0f;
        s->prevSampleR       = 0.0f;
        s->hasPrevSample     = false;
        memset(&s->filterState, 0, sizeof(s->filterState));
        LeaveCriticalSection(&g_audioCs);

        // Fire callbacks outside g_audioCs: all un-dispatched packets receive XMEDIAPACKET_STATUS_FLUSHED (0x40000005).
        DispatchCompletedStreamPackets(s, alreadyDone,   kXboxMediaPacketStatusFlushed);
        DispatchCompletedStreamPackets(s, flushedActive, kXboxMediaPacketStatusFlushed);

        Stream_Release(s);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Pause(XboxDSStreamObject* s, DWORD dwPause) {
    if (s) {
        // Verified against CMcpxStream::Pause (0x006E5947) and CMcpxStream::GetStatus (0x006E4E64):
        //   dwPause == 0 (DSSTREAMPAUSE_RESUME) or 3 (DSSTREAMPAUSE_RESUMESYNCPAUSE) clears pause/sync-pause bits (~0x44);
        //   dwPause == 1 (DSSTREAMPAUSE_PAUSE) sets bit 0x04 -> DSSTREAMSTATUS_PAUSED;
        //   dwPause == 2 (DSSTREAMPAUSE_SYNCPAUSE) sets bit 0x40 -> DSSTREAMSTATUS_PAUSED.
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (dwPause == kXboxStreamPauseResume || dwPause == kXboxStreamPauseResumeSyncPause) {
            s->paused = false;
        } else if (dwPause == kXboxStreamPausePause || dwPause == kXboxStreamPauseSyncPause) {
            s->paused = true;
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_FlushEx(XboxDSStreamObject* s, uint32_t rtLow, uint32_t rtHigh, DWORD dwFlags) {
    (void)rtLow; (void)rtHigh;
    // Verified against CMcpxStream::Stop (0x006E4DF2) & XACT::CSoundCue::StreamData (0x00708094):
    // Asynchronous FlushEx (DSSTREAMFLUSHEX_ASYNC = 0x1 | ENVELOPE = 0x4) schedules the stream to
    // transition to starved/envelope-complete AFTER all currently queued input packets finish playing.
    if ((dwFlags & 7u) != 0u) {
        if (s) {
            EnsureStreamAudioLockInitialized();
            EnterCriticalSection(&g_audioCs);
            s->flushExPending = true;
            if ((dwFlags & 6u) != 0u && s->egActive[1] && s->eg[1].dwRelease > 0u) {
                if (!s->isReleasing) {
                    s->isReleasing       = true;
                    s->releaseSamples48k = 0ULL;
                }
            }
            LeaveCriticalSection(&g_audioCs);
        }
        return S_OK;
    }
    return Stream_Flush(s);
}

static HRESULT __stdcall Stream_SetVolume(XboxDSStreamObject* s, LONG lVolume) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->volume = lVolume;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetHeadroom(XboxDSStreamObject* s, DWORD dwHeadroom) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->headroomMb = (dwHeadroom <= 10000u) ? static_cast<LONG>(dwHeadroom) : 10000;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetPitch(XboxDSStreamObject* s, LONG lPitch) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->lPitch = lPitch;
        s->freq   = XboxPitchToFrequencyHz(lPitch);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetFrequency(XboxDSStreamObject* s, DWORD dwFrequency) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (dwFrequency < kMinDirectSoundFreqHz) dwFrequency = kMinDirectSoundFreqHz;
        if (dwFrequency > kMaxXboxPitchFreqHz)   dwFrequency = kMaxXboxPitchFreqHz;
        s->freq   = dwFrequency;
        s->lPitch = FrequencyHzToXboxPitch(dwFrequency);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetLFO(XboxDSStreamObject* s, const XBOX_DSLFODESC* pLFODesc) {
    if (!s) return E_INVALIDARG;
    EnsureStreamAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (pLFODesc && reinterpret_cast<uintptr_t>(pLFODesc) >= kMinValidHostAddress) {
        uint32_t idx = (pLFODesc->dwLFO == 1u) ? 1u : 0u;
        s->lfo[idx] = *pLFODesc;
        s->lfoActive[idx] = (pLFODesc->dwDelta > 0u) &&
                            (pLFODesc->lPitchModulation != 0 ||
                             pLFODesc->lFilterCutOffRange != 0 ||
                             pLFODesc->lAmplitudeModulation != 0);
    } else {
        s->lfoActive[0] = false;
        s->lfoActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetEG(XboxDSStreamObject* s, const XBOX_DSENVELOPEDESC* pEnvelopeDesc) {
    if (!s) return E_INVALIDARG;
    EnsureStreamAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    if (pEnvelopeDesc && reinterpret_cast<uintptr_t>(pEnvelopeDesc) >= kMinValidHostAddress) {
        uint32_t idx = (pEnvelopeDesc->dwEG == 1u) ? 1u : 0u;
        s->eg[idx] = *pEnvelopeDesc;
        s->egActive[idx] = (pEnvelopeDesc->dwMode != 0u);
    } else {
        s->egActive[0] = false;
        s->egActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetOutputBuffer(XboxDSStreamObject* s, XboxDSBufferObject* pOutputBuffer) {
    if (!s) return E_INVALIDARG;
    EnsureStreamAudioLockInitialized();
    EnterCriticalSection(&g_audioCs);
    XboxDSBufferObject* target = (pOutputBuffer != reinterpret_cast<XboxDSBufferObject*>(s)) ? pOutputBuffer : nullptr;
    s->outputBuffer = target;
    if (target) {
        EnsureXboxBufferSubmixReadyLocked(target);
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetFilter(XboxDSStreamObject* s, const XBOX_DSFILTERDESC* pFilterDesc) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        if (pFilterDesc && reinterpret_cast<uintptr_t>(pFilterDesc) >= kMinValidHostAddress && (pFilterDesc->dwMode & 3u) != 0u) {
            s->filterDesc      = *pFilterDesc;
            s->hasActiveFilter = true;
        } else {
            memset(&s->filterDesc, 0, sizeof(s->filterDesc));
            s->hasActiveFilter = false;
            s->filterState.low[0]  = s->filterState.low[1]  = 0.0f;
            s->filterState.band[0] = s->filterState.band[1] = 0.0f;
            s->filterState.low2[0] = s->filterState.low2[1] = 0.0f;
            s->filterState.band2[0]= s->filterState.band2[1]= 0.0f;
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_GetVoiceProperties(XboxDSStreamObject* s, XBOX_DSVOICEPROPS* pProps) {
    if (!pProps) return E_INVALIDARG;
    memset(pProps, 0, sizeof(*pProps));
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        FillVoicePropsMixBins(s->mixBinPairs, s->mixBinCount, pProps);
        pProps->lPitch             = s->lPitch;
        pProps->l3DDistanceVolume  = s->voice3D.distanceVol;
        pProps->l3DConeVolume      = s->voice3D.coneVol;
        pProps->l3DDopplerPitch    = s->voice3D.dopplerPitch;
        pProps->lI3DL2DirectVolume = s->voice3D.i3dl2DirectVol;
        pProps->lI3DL2RoomVolume   = s->voice3D.i3dl2RoomVol;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetFormat(XboxDSStreamObject* s, const WAVEFORMATEX* pwfx) {
    if (s && pwfx) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->wfx     = *pwfx;
        s->isAdpcm = (pwfx->wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
        if (pwfx->nSamplesPerSec) {
            s->freq   = pwfx->nSamplesPerSec;
            s->lPitch = FrequencyHzToXboxPitch(s->freq);
        }
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0.0f;
        s->prevSampleR   = 0.0f;
        s->hasPrevSample = false;
        memset(&s->filterState, 0, sizeof(s->filterState));
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Set3DVoiceData(XboxDSStreamObject* s, const void* pVoiceData) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        LONG pan3D = 0;
        Compute3DAudioPanAndAtten(pVoiceData, &pan3D, &s->atten3D, &s->voice3D);
        if (s->voice3D.active3D && !s->hasI3DL2Send) {
            s->hasI3DL2Send = true;
            if (s->i3dl2SendMb <= kMinDirectSoundVolumeMb) {
                s->i3dl2SendMb = 0;
            }
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

// Verified against CDirectSoundVoice::Use3DVoiceData (0x006E059E): 2nd parameter is BOOL fUse3DVoiceData
static HRESULT __stdcall Stream_Use3DVoiceData(XboxDSStreamObject* s, DWORD dwUse3DVoiceData) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        s->use3DVoiceData = (dwUse3DVoiceData != 0u);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetMixBins(XboxDSStreamObject* s, const XBOX_DSMIXBINS* pMixBins) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        AssignVoiceMixBins(pMixBins, s->mixBinPairs, &s->mixBinCount);
        ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan, &s->mixBinAttenMb);
        ExtractXboxMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
        s->fxSendBinMask = ExtractStreamFxSendBinMask(s->mixBinPairs, s->mixBinCount);
        EnsureXboxFxSendTargetsAllocatedLocked(s->fxSendBinMask);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetMixBinVolumes(XboxDSStreamObject* s, const XBOX_DSMIXBINS* pMixBins) {
    if (s) {
        EnsureStreamAudioLockInitialized();
        EnterCriticalSection(&g_audioCs);
        UpdateVoiceMixBinVolumes(pMixBins, s->mixBinPairs, &s->mixBinCount);
        ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan, &s->mixBinAttenMb);
        ExtractXboxMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
        s->fxSendBinMask = ExtractStreamFxSendBinMask(s->mixBinPairs, s->mixBinCount);
        EnsureXboxFxSendTargetsAllocatedLocked(s->fxSendBinMask);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static void* g_streamVtable[7] = {
    reinterpret_cast<void*>(&Stream_AddRef),
    reinterpret_cast<void*>(&Stream_Release),
    reinterpret_cast<void*>(&Stream_GetInfo),
    reinterpret_cast<void*>(&Stream_GetStatus),
    reinterpret_cast<void*>(&Stream_Process),
    reinterpret_cast<void*>(&Stream_Discontinuity),
    reinterpret_cast<void*>(&Stream_Flush),
};

static HRESULT __stdcall Bridge_DirectSoundCreateStream(const XBOX_DSSTREAMDESC* pDesc, XboxDSStreamObject** ppStream) {
    if (!ppStream) return E_INVALIDARG;

    EnsureStreamAudioLockInitialized();
    EnsureXboxWasapiMixerStarted();

    XboxDSStreamObject* s = new XboxDSStreamObject();
    s->vtable             = g_streamVtable;
    s->refCount           = 1;
    memset(&s->wfx, 0, sizeof(s->wfx));
    s->isAdpcm            = false;
    s->maxAttachedPackets = 16u;
    s->lpfnCallback       = nullptr;
    s->lpvContext         = nullptr;
    s->volume             = 0;
    s->headroomMb         = 0;
    s->pan                = 0;
    s->mixBinAttenMb      = 0;
    s->atten3D            = 0;
    s->lPitch             = 0;
    s->freq               = kDefaultSampleRateHz;
    s->paused             = false;
    s->discontinuity      = false;
    s->flushExPending     = false;
    s->isReleasing        = false;
    s->use3DVoiceData     = true;
    s->hasI3DL2Send       = false;
    s->i3dl2SendMb        = kMinDirectSoundVolumeMb;
    s->fxSendBinMask      = 0u;
    s->resamplePos        = 0.0;
    s->prevSampleL        = 0.0f;
    s->prevSampleR        = 0.0f;
    s->hasPrevSample      = false;
    memset(s->mixBinPairs, 0, sizeof(s->mixBinPairs));
    s->mixBinCount        = 0u;
    memset(&s->voice3D, 0, sizeof(s->voice3D));
    s->voice3D.hrtfFilterCoeff = 1.0f;
    memset(&s->filterDesc, 0, sizeof(s->filterDesc));
    s->hasActiveFilter    = false;
    memset(&s->filterState, 0, sizeof(s->filterState));
    memset(s->lfo, 0, sizeof(s->lfo));
    s->lfoActive[0]       = s->lfoActive[1] = false;
    memset(s->eg, 0, sizeof(s->eg));
    s->egActive[0]        = s->egActive[1]  = false;
    s->outputBuffer       = nullptr;
    s->activeSamples48k   = 0ULL;
    s->releaseSamples48k  = 0ULL;
    s->activePackets.reserve(64u);
    s->completedPackets.reserve(64u);

    InitVoiceDefaultMixBins(pDesc ? pDesc->dwFlags : 0u, s->mixBinPairs, &s->mixBinCount);
    ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan, &s->mixBinAttenMb);
    ExtractXboxMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
    s->fxSendBinMask = ExtractStreamFxSendBinMask(s->mixBinPairs, s->mixBinCount);

    if (pDesc) {
        if (pDesc->dwMaxAttachedPackets > 0u) {
            s->maxAttachedPackets = pDesc->dwMaxAttachedPackets;
        }
        s->lpfnCallback = pDesc->lpfnCallback;
        s->lpvContext   = pDesc->lpvContext;
        if (pDesc->lpwfxFormat) {
            s->wfx     = *pDesc->lpwfxFormat;
            s->isAdpcm = (s->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            if (s->wfx.nSamplesPerSec) {
                s->freq   = s->wfx.nSamplesPerSec;
                s->lPitch = FrequencyHzToXboxPitch(s->freq);
            }
        }
        if (pDesc->lpMixBins) {
            const XBOX_DSMIXBINS* pMixBins = reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins);
            AssignVoiceMixBins(pMixBins, s->mixBinPairs, &s->mixBinCount);
            ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan, &s->mixBinAttenMb);
            ExtractXboxMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
            s->fxSendBinMask = ExtractStreamFxSendBinMask(s->mixBinPairs, s->mixBinCount);
        }
    }

    EnterCriticalSection(&g_audioCs);
    g_activeStreams.push_back(s);
    EnsureXboxFxSendTargetsAllocatedLocked(s->fxSendBinMask);
    LeaveCriticalSection(&g_audioCs);

    *ppStream = s;
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CreateSoundStream(
    XboxDSObject* pDS, const XBOX_DSSTREAMDESC* pDesc, XboxDSStreamObject** ppStream, void* pUnkOuter)
{
    (void)pDS; (void)pUnkOuter;
    return Bridge_DirectSoundCreateStream(pDesc, ppStream);
}

// ============================================================================
// Game-Thread Packet Completion Dispatcher (_DirectSoundDoWork@0)
// ============================================================================

void UpdateXboxAudioTick() {
    if (!IsXboxAudioCsInitialized()) return;

    std::vector<XboxDSStreamObject*> streams;
    std::vector<StreamSampleChunk*>  chunksToDelete;
    EnterCriticalSection(&g_audioCs);
    if (!g_retiredStreamChunks.empty()) {
        chunksToDelete.swap(g_retiredStreamChunks);
        g_retiredStreamChunks.reserve(512u);
    }
    if (g_freeStreamChunks.capacity() < 512u) {
        g_freeStreamChunks.reserve(512u);
    }
    streams.reserve(g_activeStreams.size());
    for (size_t i = 0u; i < g_activeStreams.size(); ++i) {
        XboxDSStreamObject* s = g_activeStreams[i];
        if (s && !s->completedPackets.empty()) {
            Stream_AddRef(s);
            streams.push_back(s);
        }
    }
    LeaveCriticalSection(&g_audioCs);

    for (size_t ci = 0u; ci < chunksToDelete.size(); ++ci) {
        delete chunksToDelete[ci];
    }

    for (size_t si = 0u; si < streams.size(); ++si) {
        XboxDSStreamObject* s = streams[si];
        if (!s) continue;

        std::vector<CompletedStreamPacket> ready;
        bool isPaused = false;
        EnterCriticalSection(&g_audioCs);
        isPaused = s->paused;
        if (!isPaused && !s->completedPackets.empty()) {
            ready.swap(s->completedPackets);
            if (s->completedPackets.capacity() < 64u) {
                s->completedPackets.reserve(64u);
            }
        }
        LeaveCriticalSection(&g_audioCs);

        if (!isPaused && !ready.empty()) {
            DispatchCompletedStreamPackets(s, ready, kXboxMediaPacketStatusSuccess);
        }
        Stream_Release(s);
    }
}

static void __stdcall Bridge_DirectSoundDoWork() {
    UpdateXboxAudioTick();
}

// ============================================================================
// Unified 48,000 Hz Stereo Float32 WASAPI Engine & MCPX Sample Counter
// ============================================================================

static volatile uint32_t g_mcpxSampleCounter       = 0u;
static volatile LONGLONG g_mcpxTotalSamples64      = 0LL;
static volatile LONGLONG g_lastMixCommitQpc        = 0LL;
static volatile LONGLONG g_monotonicSampleMark64   = 0LL;
static LONGLONG          g_audioHostQpcFreq        = 0LL;

static HANDLE            g_hWasapiThread           = nullptr;
static HANDLE            g_hAudioStopEvent         = nullptr;
static CRITICAL_SECTION  g_wasapiCtrlCs;
static volatile LONG     g_wasapiCtrlCsInit        = 0;
static volatile bool     g_manualMixModeForTest    = false;

static void EnsureWasapiCtrlLockInit() {
    if (g_wasapiCtrlCsInit == 2) return;
    if (InterlockedCompareExchange(&g_wasapiCtrlCsInit, 1, 0) == 0) {
        LARGE_INTEGER freq = {};
        QueryPerformanceFrequency(&freq);
        g_audioHostQpcFreq = (freq.QuadPart > 0LL) ? freq.QuadPart : 10000000LL;
        InitializeCriticalSection(&g_wasapiCtrlCs);
        InterlockedExchange(&g_wasapiCtrlCsInit, 2);
    } else {
        while (g_wasapiCtrlCsInit != 2) {
            SwitchToThread();
        }
    }
}

static inline float SoftClipMasterSample(float x) {
    if (!_finite(x)) return 0.0f;
    float ax = fabsf(x);
    if (ax <= 0.90f) {
        return x;
    }
    // Smooth C1 rational soft-knee limiter mapping [0.90, +inf) -> [0.90, 1.00)
    float excess = ax - 0.90f;
    float shaped = 0.90f + 0.10f * (excess / (excess + 0.10f));
    return (x < 0.0f) ? -shaped : shaped;
}

static void MixAudioQuantum48k(uint32_t totalFrames, float* outInterleavedStereo) {
    EnsureWasapiCtrlLockInit();
    EnsureStreamAudioLockInitialized();

    float masterDryL[kAudioMaxQuantumFrames];
    float masterDryR[kAudioMaxQuantumFrames];
    float reverbSendL[kAudioMaxQuantumFrames];
    float reverbSendR[kAudioMaxQuantumFrames];

    uint32_t offset = 0u;
    while (offset < totalFrames) {
        uint32_t frames = totalFrames - offset;
        if (frames > kAudioMaxQuantumFrames) {
            frames = kAudioMaxQuantumFrames;
        }

        memset(masterDryL,  0, frames * sizeof(float));
        memset(masterDryR,  0, frames * sizeof(float));
        memset(reverbSendL, 0, frames * sizeof(float));
        memset(reverbSendR, 0, frames * sizeof(float));

        EnterCriticalSection(&g_audioCs);
        PrepareXboxSubmixBuffers48kLocked(frames);
        MixActiveXboxBuffers48kLocked(masterDryL, masterDryR, reverbSendL, reverbSendR, frames);
        MixActiveXboxStreams48kLocked(masterDryL, masterDryR, reverbSendL, reverbSendR, frames);
        FinalizeXboxSubmixBuffers48kLocked(masterDryL, masterDryR, reverbSendL, reverbSendR, frames);
        XBOX_DSI3DL2LISTENER listenerSnap = GetXboxI3DL2ListenerState();
        LeaveCriticalSection(&g_audioCs);

        ProcessGlobalI3DL2Reverb48k(reverbSendL, reverbSendR, masterDryL, masterDryR, frames, &listenerSnap);

        if (outInterleavedStereo) {
            float* dst = outInterleavedStereo + static_cast<size_t>(offset) * 2u;
            for (uint32_t i = 0u; i < frames; ++i) {
                dst[i * 2u + 0u] = SoftClipMasterSample(masterDryL[i]);
                dst[i * 2u + 1u] = SoftClipMasterSample(masterDryR[i]);
            }
        }

        LARGE_INTEGER nowQpc = {};
        QueryPerformanceCounter(&nowQpc);
        LONGLONG newTotal = InterlockedAdd64(&g_mcpxTotalSamples64, static_cast<LONGLONG>(frames));
        InterlockedExchange64(&g_lastMixCommitQpc, nowQpc.QuadPart);

        LONGLONG curMark = InterlockedCompareExchange64(&g_monotonicSampleMark64, 0LL, 0LL);
        while (newTotal > curMark) {
            LONGLONG prev = InterlockedCompareExchange64(&g_monotonicSampleMark64, newTotal, curMark);
            if (prev == curMark) break;
            curMark = prev;
        }
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&g_mcpxSampleCounter),
            static_cast<LONG>(static_cast<uint32_t>(newTotal < curMark ? curMark : newTotal)));

        offset += frames;
    }
}

static uint64_t QueryInterpolatedMcpxSamples64() {
    EnsureWasapiCtrlLockInit();
    LONGLONG baseSamples = InterlockedCompareExchange64(&g_mcpxTotalSamples64, 0LL, 0LL);
    if (g_manualMixModeForTest) {
        return (baseSamples > 0LL) ? static_cast<uint64_t>(baseSamples) : 0ULL;
    }

    LONGLONG commitQpc = InterlockedCompareExchange64(&g_lastMixCommitQpc, 0LL, 0LL);
    LONGLONG estimate  = baseSamples;
    if (commitQpc > 0LL && g_audioHostQpcFreq > 0LL) {
        LARGE_INTEGER nowQpc = {};
        QueryPerformanceCounter(&nowQpc);
        LONGLONG deltaQpc = nowQpc.QuadPart - commitQpc;
        if (deltaQpc > 0LL) {
            // Interpolate up to 40 ms (1920 samples) ahead of the last mixer quantum commit
            LONGLONG interp = (deltaQpc * static_cast<LONGLONG>(kDefaultSampleRateHz)) / g_audioHostQpcFreq;
            if (interp > 1920LL) interp = 1920LL;
            estimate += interp;
        }
    }

    LONGLONG curMark = InterlockedCompareExchange64(&g_monotonicSampleMark64, 0LL, 0LL);
    while (estimate > curMark) {
        LONGLONG prev = InterlockedCompareExchange64(&g_monotonicSampleMark64, estimate, curMark);
        if (prev == curMark) {
            curMark = estimate;
            break;
        }
        curMark = prev;
    }
    uint64_t finalSamples = (curMark > 0LL) ? static_cast<uint64_t>(curMark) : 0ULL;
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&g_mcpxSampleCounter),
        static_cast<LONG>(static_cast<uint32_t>(finalSamples)));
    return finalSamples;
}

uint32_t QueryXboxMcpxSampleCounter() {
    return static_cast<uint32_t>(QueryInterpolatedMcpxSamples64());
}

static void __fastcall Bridge_XACT_CEngine_GetTime(void* thisPtr, int dummyEdx, int64_t* pTime) {
    (void)thisPtr; (void)dummyEdx;
    uint64_t samples48k = QueryInterpolatedMcpxSamples64();
    if (pTime) {
        // 1 second = 48,000 samples = 10,000,000 (100ns units) -> exact ratio 625 / 3
        uint64_t refTime100Ns = (samples48k / 3ULL) * 625ULL + ((samples48k % 3ULL) * 625ULL) / 3ULL;
        *pTime = static_cast<int64_t>(refTime100Ns);
    }
}

static const GUID kSubtypeIeeeFloatGuid = {
    0x00000003u, 0x0000u, 0x0010u, { 0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u }
};

static bool TryOpenWasapiSharedEndpoint48k(
    IMMDeviceEnumerator* pEnumerator,
    HANDLE hSampleReadyEvent,
    IMMDevice** outDevice,
    IAudioClient** outAudioClient,
    IAudioRenderClient** outRenderClient,
    UINT32* outBufferFrames)
{
    if (!pEnumerator || !hSampleReadyEvent || !outDevice || !outAudioClient || !outRenderClient || !outBufferFrames) {
        return false;
    }
    *outDevice       = nullptr;
    *outAudioClient  = nullptr;
    *outRenderClient = nullptr;
    *outBufferFrames = 0u;

    IMMDevice* pDevice = nullptr;
    HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice) {
        return false;
    }

    IAudioClient* pAudioClient = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pAudioClient));
    if (FAILED(hr) || !pAudioClient) {
        pDevice->Release();
        return false;
    }

    WAVEFORMATEXTENSIBLE wfxExt = {};
    wfxExt.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wfxExt.Format.nChannels            = 2u;
    wfxExt.Format.nSamplesPerSec       = kDefaultSampleRateHz;
    wfxExt.Format.wBitsPerSample       = 32u;
    wfxExt.Format.nBlockAlign          = 8u;
    wfxExt.Format.nAvgBytesPerSec      = kDefaultSampleRateHz * 8u;
    wfxExt.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfxExt.Samples.wValidBitsPerSample = 32u;
    wfxExt.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfxExt.SubFormat                   = kSubtypeIeeeFloatGuid;

    const REFERENCE_TIME requestedBufferDuration = 200000; // 20 ms in 100-ns units
    const DWORD autoConvertFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, autoConvertFlags, requestedBufferDuration, 0, &wfxExt.Format, nullptr);
    if (FAILED(hr)) {
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, requestedBufferDuration, 0, &wfxExt.Format, nullptr);
    }
    if (FAILED(hr)) {
        WAVEFORMATEX wfxSimple = {};
        wfxSimple.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        wfxSimple.nChannels       = 2u;
        wfxSimple.nSamplesPerSec  = kDefaultSampleRateHz;
        wfxSimple.wBitsPerSample  = 32u;
        wfxSimple.nBlockAlign     = 8u;
        wfxSimple.nAvgBytesPerSec = kDefaultSampleRateHz * 8u;
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, autoConvertFlags, requestedBufferDuration, 0, &wfxSimple, nullptr);
    }

    UINT32 bufferFrames = 0u;
    IAudioRenderClient* pRenderClient = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pAudioClient->GetBufferSize(&bufferFrames);
    }
    if (SUCCEEDED(hr) && bufferFrames > 0u) {
        hr = pAudioClient->SetEventHandle(hSampleReadyEvent);
    }
    if (SUCCEEDED(hr)) {
        hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRenderClient));
    }
    if (SUCCEEDED(hr) && pRenderClient) {
        // Pre-fill (prime) the shared-mode buffer before calling Start() per WASAPI event-driven rules
        UINT32 padding = 0u;
        if (SUCCEEDED(pAudioClient->GetCurrentPadding(&padding)) && bufferFrames > padding) {
            UINT32 availFrames = bufferFrames - padding;
            BYTE* pData = nullptr;
            HRESULT hrBuf = pRenderClient->GetBuffer(availFrames, &pData);
            if (SUCCEEDED(hrBuf) && pData) {
                MixAudioQuantum48k(availFrames, reinterpret_cast<float*>(pData));
                pRenderClient->ReleaseBuffer(availFrames, 0u);
            } else if (SUCCEEDED(hrBuf)) {
                pRenderClient->ReleaseBuffer(0u, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        hr = pAudioClient->Start();
    }

    if (FAILED(hr) || !pRenderClient || bufferFrames == 0u) {
        if (pRenderClient) pRenderClient->Release();
        pAudioClient->Release();
        pDevice->Release();
        return false;
    }

    *outDevice       = pDevice;
    *outAudioClient  = pAudioClient;
    *outRenderClient = pRenderClient;
    *outBufferFrames = bufferFrames;
    return true;
}

static void ReleaseWasapiEndpointResources(
    IMMDevice*& pDevice, IAudioClient*& pAudioClient, IAudioRenderClient*& pRenderClient)
{
    if (pAudioClient) {
        pAudioClient->Stop();
    }
    if (pRenderClient) {
        pRenderClient->Release();
        pRenderClient = nullptr;
    }
    if (pAudioClient) {
        pAudioClient->Release();
        pAudioClient = nullptr;
    }
    if (pDevice) {
        pDevice->Release();
        pDevice = nullptr;
    }
}

static DWORD WINAPI WasapiMixerThreadProc(LPVOID lpParam) {
    (void)lpParam;
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool    needCoUninit = SUCCEEDED(hrCom);

    DWORD  mmcssTaskIdx = 0u;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);
    if (!hMmcss) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    HANDLE hSampleReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE hFallbackTimer    = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hFallbackTimer) {
        hFallbackTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    IMMDeviceEnumerator* pEnumerator   = nullptr;
    IMMDevice*           pDevice       = nullptr;
    IAudioClient*        pAudioClient  = nullptr;
    IAudioRenderClient*  pRenderClient = nullptr;
    UINT32               bufferFrames  = 0u;

    CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnumerator));

    bool hasEndpoint = TryOpenWasapiSharedEndpoint48k(
        pEnumerator, hSampleReadyEvent, &pDevice, &pAudioClient, &pRenderClient, &bufferFrames);

    LARGE_INTEGER initQpc = {};
    QueryPerformanceCounter(&initQpc);
    InterlockedCompareExchange64(&g_lastMixCommitQpc, initQpc.QuadPart, 0LL);

    LONGLONG fallbackPrevQpc      = initQpc.QuadPart;
    LONGLONG fallbackRemainderNum = 0LL;
    LONGLONG lastEndpointRetryQpc = initQpc.QuadPart;

    if (!hasEndpoint && hFallbackTimer) {
        LARGE_INTEGER dueTime = {};
        dueTime.QuadPart = -50000LL; // 5 ms
        SetWaitableTimer(hFallbackTimer, &dueTime, 5, nullptr, nullptr, FALSE);
    }

    while (WaitForSingleObject(g_hAudioStopEvent, 0) != WAIT_OBJECT_0) {
        if (hasEndpoint) {
            HANDLE waitHandles[2] = { g_hAudioStopEvent, hSampleReadyEvent };
            DWORD  waitRes = WaitForMultipleObjects(2u, waitHandles, FALSE, 50u);
            if (waitRes == WAIT_OBJECT_0) {
                break;
            }

            UINT32 padding = 0u;
            HRESULT hr = pAudioClient->GetCurrentPadding(&padding);
            if (SUCCEEDED(hr)) {
                UINT32 availFrames = (bufferFrames > padding) ? (bufferFrames - padding) : 0u;
                if (availFrames > 0u) {
                    BYTE* pData = nullptr;
                    hr = pRenderClient->GetBuffer(availFrames, &pData);
                    if (SUCCEEDED(hr) && pData) {
                        MixAudioQuantum48k(availFrames, reinterpret_cast<float*>(pData));
                        hr = pRenderClient->ReleaseBuffer(availFrames, 0u);
                    } else if (SUCCEEDED(hr)) {
                        pRenderClient->ReleaseBuffer(0u, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }
            }

            if (FAILED(hr)) {
                // Seamless fallback on AUDCLNT_E_DEVICE_INVALIDATED or endpoint disconnect
                ReleaseWasapiEndpointResources(pDevice, pAudioClient, pRenderClient);
                hasEndpoint = false;

                LARGE_INTEGER nowQpc = {};
                QueryPerformanceCounter(&nowQpc);
                fallbackPrevQpc      = nowQpc.QuadPart;
                fallbackRemainderNum = 0LL;
                lastEndpointRetryQpc = nowQpc.QuadPart;
                if (hFallbackTimer) {
                    LARGE_INTEGER dueTime = {};
                    dueTime.QuadPart = -50000LL; // 5 ms
                    SetWaitableTimer(hFallbackTimer, &dueTime, 5, nullptr, nullptr, FALSE);
                }
            }
        } else {
            if (hFallbackTimer) {
                HANDLE waitHandles[2] = { g_hAudioStopEvent, hFallbackTimer };
                DWORD  waitRes = WaitForMultipleObjects(2u, waitHandles, FALSE, 10u);
                if (waitRes == WAIT_OBJECT_0) {
                    break;
                }
            } else {
                if (WaitForSingleObject(g_hAudioStopEvent, 5u) == WAIT_OBJECT_0) {
                    break;
                }
            }

            LARGE_INTEGER nowQpc = {};
            QueryPerformanceCounter(&nowQpc);
            LONGLONG deltaQpc = nowQpc.QuadPart - fallbackPrevQpc;
            fallbackPrevQpc   = nowQpc.QuadPart;

            if (deltaQpc > 0LL && g_audioHostQpcFreq > 0LL) {
                LONGLONG scaled = deltaQpc * static_cast<LONGLONG>(kDefaultSampleRateHz) + fallbackRemainderNum;
                LONGLONG frames64 = scaled / g_audioHostQpcFreq;
                fallbackRemainderNum = scaled % g_audioHostQpcFreq;
                if (frames64 > 0LL) {
                    uint32_t framesToMix = (frames64 > static_cast<LONGLONG>(kAudioMaxQuantumFrames))
                        ? kAudioMaxQuantumFrames
                        : static_cast<uint32_t>(frames64);
                    MixAudioQuantum48k(framesToMix, nullptr);
                }
            }

            // Periodically retry opening the default render endpoint every ~500 ms
            if (pEnumerator && g_audioHostQpcFreq > 0LL &&
                (nowQpc.QuadPart - lastEndpointRetryQpc) >= (g_audioHostQpcFreq / 2LL))
            {
                lastEndpointRetryQpc = nowQpc.QuadPart;
                if (TryOpenWasapiSharedEndpoint48k(
                        pEnumerator, hSampleReadyEvent, &pDevice, &pAudioClient, &pRenderClient, &bufferFrames))
                {
                    hasEndpoint = true;
                    if (hFallbackTimer) {
                        CancelWaitableTimer(hFallbackTimer);
                    }
                }
            }
        }
    }

    if (hFallbackTimer) {
        CancelWaitableTimer(hFallbackTimer);
    }
    ReleaseWasapiEndpointResources(pDevice, pAudioClient, pRenderClient);
    if (pEnumerator) {
        pEnumerator->Release();
        pEnumerator = nullptr;
    }
    if (hFallbackTimer) {
        CloseHandle(hFallbackTimer);
    }
    if (hSampleReadyEvent) {
        CloseHandle(hSampleReadyEvent);
    }
    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
    if (needCoUninit) {
        CoUninitialize();
    }
    return 0u;
}

static void StopWasapiMixerThreadInternal() {
    EnsureWasapiCtrlLockInit();
    HANDLE hThread = nullptr;
    EnterCriticalSection(&g_wasapiCtrlCs);
    if (g_hWasapiThread) {
        if (g_hAudioStopEvent) {
            SetEvent(g_hAudioStopEvent);
        }
        hThread = g_hWasapiThread;
        g_hWasapiThread = nullptr;
    }
    LeaveCriticalSection(&g_wasapiCtrlCs);

    if (hThread) {
        WaitForSingleObject(hThread, 5000u);
        CloseHandle(hThread);
    }

    EnterCriticalSection(&g_wasapiCtrlCs);
    if (!g_hWasapiThread && g_hAudioStopEvent) {
        CloseHandle(g_hAudioStopEvent);
        g_hAudioStopEvent = nullptr;
    }
    LeaveCriticalSection(&g_wasapiCtrlCs);
}

void EnsureXboxWasapiMixerStarted() {
    EnsureWasapiCtrlLockInit();
    EnsureStreamAudioLockInitialized();
    if (g_manualMixModeForTest) {
        return;
    }

    EnterCriticalSection(&g_wasapiCtrlCs);
    if (!g_manualMixModeForTest && !g_hWasapiThread) {
        if (!g_hAudioStopEvent) {
            g_hAudioStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        } else {
            ResetEvent(g_hAudioStopEvent);
        }
        if (g_hAudioStopEvent) {
            DWORD tid = 0u;
            g_hWasapiThread = CreateThread(nullptr, 256u * 1024u, WasapiMixerThreadProc, nullptr, 0u, &tid);
        }
    }
    LeaveCriticalSection(&g_wasapiCtrlCs);
}

void SetXboxAudioManualMixModeForTest(bool manualOnly) {
    EnsureWasapiCtrlLockInit();
    EnsureStreamAudioLockInitialized();
    if (manualOnly) {
        g_manualMixModeForTest = true;
        StopWasapiMixerThreadInternal();
    } else {
        g_manualMixModeForTest = false;
    }
}

void PumpXboxAudioMixerFramesForTest(uint32_t totalFrames, float* outInterleavedStereo) {
    if (totalFrames == 0u) return;
    MixAudioQuantum48k(totalFrames, outInterleavedStereo);
}

// ============================================================================
// Initialize & Shutdown Xbox Audio Subsystem
// ============================================================================

static bool IsXboxAudioCodeSectionMapped() {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(0x006E3096u), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0u) {
        return false;
    }
    return (*reinterpret_cast<const uint32_t*>(0x006E3096u) != 0u);
}

// XACT SoundSource C-API wrappers at 0x006FE8D7, 0x006FE900, and 0x006FE905 omitted
// XACTEnterCriticalSection (unlike all sibling IXACTSoundSource_* wrappers at 0x006FE6DA..0x006FE8D4),
// which inverted lock order (CSoundSource::m_cs -> g_audioCs vs _g_XACTCriticalSection -> m_cs -> g_audioCs).
// Bridging them to acquire _g_XACTCriticalSection (0x00708F58) first enforces strict global lock hierarchy.
static HRESULT __stdcall Bridge_IXACTSoundSource_SetPosition(
    void* pSoundSource, float x, float y, float z, DWORD dwFlags)
{
    XBOX_CRITICAL_SECTION* xactCs = reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION);
    Bridge_RtlEnterCriticalSection(xactCs);
    typedef HRESULT (__stdcall *PFN_CSoundSource_SetPosition)(void*, float, float, float, DWORD);
    HRESULT hr = reinterpret_cast<PFN_CSoundSource_SetPosition>(0x00702B5Fu)(pSoundSource, x, y, z, dwFlags);
    Bridge_RtlLeaveCriticalSection(xactCs);
    return hr;
}

static HRESULT __stdcall Bridge_IXACTSoundSource_SetI3DL2Source(
    void* pSoundSource, const void* pds3db, DWORD dwFlags)
{
    XBOX_CRITICAL_SECTION* xactCs = reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION);
    Bridge_RtlEnterCriticalSection(xactCs);
    typedef HRESULT (__stdcall *PFN_CSoundSource_SetI3DL2Source)(void*, const void*, DWORD);
    HRESULT hr = reinterpret_cast<PFN_CSoundSource_SetI3DL2Source>(0x00702C48u)(pSoundSource, pds3db, dwFlags);
    Bridge_RtlLeaveCriticalSection(xactCs);
    return hr;
}

static HRESULT __stdcall Bridge_IXACTSoundSource_GetVoiceStatus(
    void* pSoundSource, DWORD* pdwStatus)
{
    XBOX_CRITICAL_SECTION* xactCs = reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION);
    Bridge_RtlEnterCriticalSection(xactCs);
    typedef HRESULT (__stdcall *PFN_CSoundSource_GetStatus)(void*, DWORD*);
    HRESULT hr = reinterpret_cast<PFN_CSoundSource_GetStatus>(0x00701D5Au)(pSoundSource, pdwStatus);
    Bridge_RtlLeaveCriticalSection(xactCs);
    return hr;
}

void InitXboxAudio() {
    EnsureWasapiCtrlLockInit();
    EnsureStreamAudioLockInitialized();

    EnterCriticalSection(&g_audioCs);
    if (g_freeStreamChunks.capacity() < 512u) {
        g_freeStreamChunks.reserve(512u);
    }
    if (g_retiredStreamChunks.capacity() < 512u) {
        g_retiredStreamChunks.reserve(512u);
    }
    LeaveCriticalSection(&g_audioCs);

    if (IsXboxAudioCodeSectionMapped()) {
        // 1. Neutralize Xbox-specific IRQL & FPU state helpers in DSOUND and XACTENG
        PatchRet(0x006DFE10u, 0, 0, false); // ?Raise@CIrql@DirectSound@@QAEXXZ
        PatchRet(0x006DFE32u, 0, 0, false); // ?Lower@CIrql@DirectSound@@QAEXXZ
        PatchRet(0x006DFE49u, 0, 0, false); // ?Save@CFpState@DirectSound@@QAEXXZ
        PatchRet(0x006DFE6Eu, 0, 0, false); // ?Restore@CFpState@DirectSound@@QAEXXZ
        PatchRet(0x006FE90Au, 0, 0, false); // ?Raise@CIrql@XACT@@QAEXXZ
        PatchRet(0x006FE92Cu, 0, 0, false); // ?Lower@CIrql@XACT@@QAEXXZ

        // Enforce strict _g_XACTCriticalSection -> CSoundSource::m_cs -> g_audioCs lock hierarchy
        InstallBridge(0x006FE8D7u, &Bridge_IXACTSoundSource_SetPosition);        // _IXACTSoundSource_SetPosition@20
        InstallBridge(0x006FE900u, &Bridge_IXACTSoundSource_SetI3DL2Source);     // _IXACTSoundSource_SetI3DL2Source@12
        InstallBridge(0x006FE905u, &Bridge_IXACTSoundSource_GetVoiceStatus);     // _IXACTSoundSource_GetStatus@8

        // Redirect MCPX APU Sample Counter (0xFE80200C) reads in XACTENG & XPP to g_mcpxSampleCounter:
        //   0x00703BF6: A1 0C 20 80 FE    -> mov eax, ds:[0FE80200Ch] (imm32 at 0x00703BF7)
        //   0x0074E548: 8B 1D 0C 20 80 FE -> mov ebx, ds:[0FE80200Ch] (imm32 at 0x0074E54A, preserving sti at 0x0074E54E)
        PatchU32(0x00703BF7u, reinterpret_cast<uint32_t>(&g_mcpxSampleCounter));
        PatchU32(0x0074E54Au, reinterpret_cast<uint32_t>(&g_mcpxSampleCounter));
        InstallBridge(0x00703BDFu, &Bridge_XACT_CEngine_GetTime);                // ?GetTime@CEngine@XACT@@QAEXPA_J@Z

        // 2. Top-level DirectSound Stream & DoWork + Buffer bridges
        InitXboxAudioBuffers();
        InstallBridge(0x006E30DDu, &Bridge_DirectSoundCreateStream);             // _DirectSoundCreateStream@8
        InstallBridge(0x006E2EACu, &Bridge_IDirectSound_CreateSoundStream);      // _IDirectSound_CreateSoundStream@16
        InstallBridge(0x006E1A58u, &Bridge_DirectSoundDoWork);                   // _DirectSoundDoWork@0

        // Initialize CHRTFSource::m_vtable (0x006FE22C) via DirectSoundUseFullHRTF / SetFullHRTF5Channel (0x006E3C2D)
        reinterpret_cast<void (__stdcall *)()>(0x006E3C2Du)();

        // 3. IDirectSoundStream / CDirectSoundStream target methods (called by 5-byte thunks at 0x006E1A00..0x006E275B)
        InstallBridge(0x006E14B9u, &Stream_SetVolume);                           // CDirectSoundStream::SetVolume
        InstallBridge(0x006E1467u, &Stream_SetPitch);                            // CDirectSoundStream::SetPitch
        InstallBridge(0x006E150Bu, &Stream_SetLFO);                              // CDirectSoundStream::SetLFO
        InstallBridge(0x006E155Du, &Stream_SetEG);                               // CDirectSoundStream::SetEG
        InstallBridge(0x006E15AFu, &Stream_SetFilter);                           // CDirectSoundStream::SetFilter
        InstallBridge(0x006E1601u, &Stream_SetHeadroom);                         // CDirectSoundStream::SetHeadroom
        InstallBridge(0x006E1653u, &Stream_SetMixBinVolumes);                    // CDirectSoundStream::SetMixBinVolumes
        InstallBridge(0x006E0D45u, &Stream_Pause);                               // CDirectSoundStream::Pause
        InstallBridge(0x006E0D96u, &Stream_FlushEx);                             // CDirectSoundStream::FlushEx
        InstallBridge(0x006E16A5u, &Stream_GetVoiceProperties);                  // CDirectSoundStream::GetVoiceProperties
        InstallBridge(0x006E16F7u, &Stream_Set3DVoiceData);                      // CDirectSoundStream::Set3DVoiceData
        InstallBridge(0x006E1749u, &Stream_Use3DVoiceData);                      // CDirectSoundStream::Use3DVoiceData
        InstallBridge(0x006E1F87u, &Stream_SetFrequency);                        // CDirectSoundStream::SetFrequency
        InstallBridge(0x006E1FD9u, &Stream_SetOutputBuffer);                     // CDirectSoundStream::SetOutputBuffer
        InstallBridge(0x006E202Bu, &Stream_SetMixBins);                          // CDirectSoundStream::SetMixBins
        InstallBridge(0x006E26CDu, &Stream_SetFormat);                           // CDirectSoundStream::SetFormat
    }

    EnsureXboxWasapiMixerStarted();
}

void ShutdownXboxAudio() {
    StopWasapiMixerThreadInternal();

    if (IsXboxAudioCsInitialized()) {
        EnterCriticalSection(&g_audioCs);
        for (size_t si = 0u; si < g_activeStreams.size(); ++si) {
            XboxDSStreamObject* s = g_activeStreams[si];
            if (!s) continue;
            for (size_t pi = 0u; pi < s->activePackets.size(); ++pi) {
                delete s->activePackets[pi].chunk;
                s->activePackets[pi].chunk = nullptr;
            }
            s->activePackets.clear();
            s->completedPackets.clear();
            s->paused = true;
        }
        for (size_t ci = 0u; ci < g_freeStreamChunks.size(); ++ci) {
            delete g_freeStreamChunks[ci];
        }
        g_freeStreamChunks.clear();
        g_freeStreamChunks.shrink_to_fit();

        for (size_t ri = 0u; ri < g_retiredStreamChunks.size(); ++ri) {
            delete g_retiredStreamChunks[ri];
        }
        g_retiredStreamChunks.clear();
        g_retiredStreamChunks.shrink_to_fit();

        ShutdownXboxAudioBuffersLocked();
        ResetGlobalI3DL2Reverb48kLocked();
        LeaveCriticalSection(&g_audioCs);
    }
}
