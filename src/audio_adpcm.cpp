#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <math.h>
#include <float.h>

// ============================================================================
// 3D Positional Audio, I3DL2 Occlusion & Pitch Conversion Helpers
// ============================================================================

static constexpr uint32_t kCSoundSourceVoiceDataOffset        = 0xE8u;
static constexpr uint32_t kCSoundSourcePositionOffset         = 0x4Cu;
static constexpr uint32_t kCSoundSourceModeOffset             = 0x84u;
static constexpr uint32_t kCSoundSourceI3DL2DirectOffset      = 0xC4u;
static constexpr uint32_t kCSoundSourceI3DL2ObstHfOffset      = 0xD8u;
static constexpr uint32_t kCSoundSourceI3DL2ObstLfRatioOffset = 0xDCu;
static constexpr uint32_t kCSoundSourceI3DL2OcclHfOffset      = 0xE0u;
static constexpr uint32_t kCSoundSourceI3DL2OcclLfRatioOffset = 0xE4u;
static constexpr uint32_t kCSoundSourceLinkedSourceOffset     = 0x150u;

static constexpr uint32_t kVoiceDataDistanceVolOffset         = 0x04u;
static constexpr uint32_t kVoiceDataConeVolOffset             = 0x08u;
static constexpr uint32_t kVoiceDataI3DL2DirectVolOffset      = 0x1Cu;
static constexpr uint32_t kVoiceDataI3DL2RoomVolOffset        = 0x20u;
static constexpr uint32_t kVoiceDataDopplerPitchOffset        = 0x24u;

static constexpr uint32_t kXboxDs3dModeHeadRelative           = 1u;
static constexpr uint32_t kXboxDs3dModeDisable                = 2u;
static constexpr uint32_t kXboxMixBinI3DL2                    = 10u;

static constexpr float    kMaxStereoPanMillibels              = 3500.0f;
static constexpr float    kPiFloat                            = 3.14159265358979323846f;

static constexpr double   kXboxPitchReferenceFreqHz           = 48000.0;
static constexpr double   kXboxPitchUnitsPerOctave            = 4096.0;

DWORD XboxPitchToFrequencyHz(LONG lPitch) {
    // Xbox DirectSound & XACT represent pitch in 1/4096ths of an octave relative to 48000 Hz
    // (see _XAudioCalculatePitch@4 at 0x006E00FA, CMcpxVoiceClient::ConvertPitchValue at 0x006E66CC,
    // and XACT::CSoundCue::UpdateTrackPitch at 0x007063A5).
    // Xbox MCPX hardware supports pitch up to +2 octaves (+8192 = 192,000 Hz).
    double freq = kXboxPitchReferenceFreqHz * pow(2.0, static_cast<double>(lPitch) / kXboxPitchUnitsPerOctave);
    if (freq < static_cast<double>(kMinDirectSoundFreqHz)) return kMinDirectSoundFreqHz;
    if (freq > static_cast<double>(kMaxXboxPitchFreqHz))   return kMaxXboxPitchFreqHz;
    return static_cast<DWORD>(freq + 0.5);
}

LONG FrequencyHzToXboxPitch(DWORD freqHz) {
    if (freqHz < kMinDirectSoundFreqHz) freqHz = kMinDirectSoundFreqHz;
    if (freqHz > kMaxXboxPitchFreqHz)   freqHz = kMaxXboxPitchFreqHz;
    double octaves = log(static_cast<double>(freqHz) / kXboxPitchReferenceFreqHz) / log(2.0);
    double pitch   = floor(octaves * kXboxPitchUnitsPerOctave + 0.5);
    if (pitch < -32768.0) pitch = -32768.0;
    if (pitch >   8192.0) pitch =   8192.0;
    return static_cast<LONG>(pitch);
}

static int32_t GetSourceI3DL2DirectAttenMb(const uint8_t* srcCs) {
    // Extracts I3DL2 direct + obstruction/occlusion attenuation from a CSoundSource instance
    // (populated by cGameVoice::APISetOccluded at 0x00535180 -> CSoundSource::SetI3DL2Source at 0x00702C48).
    if (!srcCs || reinterpret_cast<uintptr_t>(srcCs) < kMinValidHostAddress) return 0;
    int32_t lDirect     = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2DirectOffset);
    int32_t obstHfLevel = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2ObstHfOffset);
    float   obstLfRatio = *reinterpret_cast<const float*>(srcCs + kCSoundSourceI3DL2ObstLfRatioOffset);
    int32_t occlHfLevel = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2OcclHfOffset);
    float   occlLfRatio = *reinterpret_cast<const float*>(srcCs + kCSoundSourceI3DL2OcclLfRatioOffset);

    float lfAtten = 0.0f;
    if (_finite(obstLfRatio) && obstLfRatio >= 0.0f && obstLfRatio <= 1.0f) {
        lfAtten += static_cast<float>(obstHfLevel) * obstLfRatio;
    }
    if (_finite(occlLfRatio) && occlLfRatio >= 0.0f && occlLfRatio <= 1.0f) {
        lfAtten += static_cast<float>(occlHfLevel) * occlLfRatio;
    }
    int32_t total = lDirect + static_cast<int32_t>(lfAtten);
    if (total < kMinDirectSoundVolumeMb) total = kMinDirectSoundVolumeMb;
    if (total > kMaxDirectSoundVolumeMb) total = kMaxDirectSoundVolumeMb;
    return total;
}

struct XboxHrtfFilterPair {
    const uint8_t* pLeft;
    const uint8_t* pRight;
};

using PFN_CFullHRTFSource_GetFilterPair     = void(__stdcall*)(float flAzimuth, float flElevation, int fRearAsFront, XboxHrtfFilterPair* pFilterPair);
using PFN_CHRTFSource_SetFullHRTF5Channel   = void(__stdcall*)();

static inline int DecodeMcpSignMag8(uint8_t b) {
    int mag = static_cast<int>(b & 0x7Fu);
    return (b & 0x80u) ? -mag : mag;
}

static void EvaluateHrtfFirResponse(const uint8_t* fir32, float* outRmsGain, float* outHfRatio) {
    if (!fir32) {
        if (outRmsGain) *outRmsGain = 1.0f;
        if (outHfRatio) *outHfRatio = 1.0f;
        return;
    }
    int32_t dcSum   = 0;
    float   eTotal  = 0.0f;
    float   eDiff   = 0.0f;
    int     prevTap = 0;
    for (int i = 0; i < 31; ++i) {
        int tap = DecodeMcpSignMag8(fir32[i]);
        dcSum  += tap;
        eTotal += static_cast<float>(tap * tap);
        int d   = tap - prevTap;
        eDiff  += static_cast<float>(d * d);
        prevTap = tap;
    }
    // Reference bypass FIR at 0x006FDA2C / 0 deg front has RMS ~85..127 and DC sum ~200
    float dcGain  = fabsf(static_cast<float>(dcSum)) / 200.0f;
    float rmsGain = sqrtf(eTotal) / 110.0f;
    float gain    = 0.65f * rmsGain + 0.35f * dcGain;
    if (gain < 0.02f) gain = 0.02f;
    if (gain > 1.25f) gain = 1.25f;
    if (outRmsGain) *outRmsGain = gain;
    if (outHfRatio) {
        float hf = (eTotal > 1.0f) ? sqrtf(eDiff / (eTotal * 2.0f)) : 1.0f;
        if (hf < 0.10f) hf = 0.10f;
        if (hf > 1.00f) hf = 1.00f;
        *outHfRatio = hf;
    }
}

static volatile LONG g_hrtfPageCheckState = 0; // 0 = unchecked, 1 = MEM_COMMIT, -1 = unmapped

static bool IsGhostHrtfCodeMapped() {
    LONG st = g_hrtfPageCheckState;
    if (st == 0) {
        MEMORY_BASIC_INFORMATION mbi = {};
        bool ok = (VirtualQuery(reinterpret_cast<LPCVOID>(0x006E3B11u), &mbi, sizeof(mbi)) != 0 &&
                   mbi.State == MEM_COMMIT);
        st = ok ? 1 : -1;
        InterlockedExchange(&g_hrtfPageCheckState, st);
    }
    if (st < 0) {
        return false;
    }
    const uint8_t* pInit = reinterpret_cast<const uint8_t*>(0x006E3C2Du);
    const uint8_t* pPair = reinterpret_cast<const uint8_t*>(0x006E3B11u);
    return (pInit[0] != 0x00u && pPair[0] != 0x00u);
}

static float DecodeMcpxFilterFreqCoeff(int16_t fcHw, uint32_t sampleRateHz);

void Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb, Xbox3DVoiceMetrics* outMetrics) {
    if (outMetrics) {
        memset(outMetrics, 0, sizeof(*outMetrics));
        outMetrics->hrtfFilterCoeff = 1.0f;
    }
    if (!outPan || !outAttenDb) return;
    if (!pVoiceData || reinterpret_cast<uintptr_t>(pVoiceData) < (kMinValidHostAddress + kCSoundSourceVoiceDataOffset)) return;

    bool hasNativeHrtf = IsGhostHrtfCodeMapped();
    if (hasNativeHrtf && *reinterpret_cast<const uint32_t*>(0x006FE250u) == 0u) {
        reinterpret_cast<PFN_CHRTFSource_SetFullHRTF5Channel>(0x006E3C2Du)();
    }

    // In CSoundSource::Update3DProperties (0x007022AA), pVoiceData == (uint8_t*)pSoundSource + 0xE8 (_DS3DCALCVOICEDATA).
    const uint8_t* vd = reinterpret_cast<const uint8_t*>(pVoiceData);
    const uint8_t* cs = vd - kCSoundSourceVoiceDataOffset;
    uint32_t dwMode = *reinterpret_cast<const uint32_t*>(cs + kCSoundSourceModeOffset);
    if (dwMode == kXboxDs3dModeDisable) {
        *outPan = 0;
        *outAttenDb = 0;
        return;
    }

    int32_t distVol      = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDistanceVolOffset);
    int32_t coneVol      = *reinterpret_cast<const int32_t*>(vd + kVoiceDataConeVolOffset);
    int32_t frontVol     = *reinterpret_cast<const int32_t*>(vd + 0x0Cu);
    int32_t rearVol      = *reinterpret_cast<const int32_t*>(vd + 0x10u);
    int32_t vdI3dl2Room  = *reinterpret_cast<const int32_t*>(vd + kVoiceDataI3DL2RoomVolOffset);
    int32_t dopplerPitch = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDopplerPitchOffset);
    int16_t vdDirectFc   = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(vd + 0x30u) & 0xFFFF);
    if (distVol < kMinDirectSoundVolumeMb) distVol = kMinDirectSoundVolumeMb;
    if (distVol > kMaxDirectSoundVolumeMb) distVol = kMaxDirectSoundVolumeMb;
    if (coneVol < kMinDirectSoundVolumeMb) coneVol = kMinDirectSoundVolumeMb;
    if (coneVol > kMaxDirectSoundVolumeMb) coneVol = kMaxDirectSoundVolumeMb;
    if (vdI3dl2Room < kMinDirectSoundVolumeMb) vdI3dl2Room = kMinDirectSoundVolumeMb;
    if (vdI3dl2Room > kMaxDirectSoundVolumeMb) vdI3dl2Room = kMaxDirectSoundVolumeMb;
    if (dopplerPitch < -16384 || dopplerPitch > 8192) dopplerPitch = 0;

    int32_t i3dl2Vol = GetSourceI3DL2DirectAttenMb(cs);
    if (i3dl2Vol == 0) {
        const uint8_t* linkedCs = *reinterpret_cast<const uint8_t* const*>(cs + kCSoundSourceLinkedSourceOffset);
        if (linkedCs && reinterpret_cast<uintptr_t>(linkedCs) >= kMinValidHostAddress) {
            i3dl2Vol = GetSourceI3DL2DirectAttenMb(linkedCs);
        }
    }
    if (i3dl2Vol == 0) {
        int32_t vdI3dl2 = *reinterpret_cast<const int32_t*>(vd + kVoiceDataI3DL2DirectVolOffset);
        if (vdI3dl2 < 0 && vdI3dl2 >= kMinDirectSoundVolumeMb) {
            i3dl2Vol = vdI3dl2;
        }
    }

    float azimuthDeg   = *reinterpret_cast<const float*>(vd + 0x28u);
    float elevationDeg = *reinterpret_cast<const float*>(vd + 0x2Cu);
    bool  validPolar   = _finite(azimuthDeg) && _finite(elevationDeg) &&
                         (fabsf(azimuthDeg) > 0.001f || fabsf(elevationDeg) > 0.001f);

    float sx = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 0u);
    float sy = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 4u);
    float sz = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 8u);
    if (!validPolar && _finite(sx) && _finite(sy) && _finite(sz)) {
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        float fx = 0.0f, fy = 0.0f, fz = 1.0f;
        float tx = 0.0f, ty = 1.0f, tz = 0.0f;
        if (hasNativeHrtf) {
            lx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x08u);
            ly = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x0Cu);
            lz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x10u);
            fx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x20u);
            fy = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x24u);
            fz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x28u);
            tx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x2Cu);
            ty = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x30u);
            tz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x34u);
        }

        float dx = (dwMode == kXboxDs3dModeHeadRelative) ? sx : (sx - lx);
        float dy = (dwMode == kXboxDs3dModeHeadRelative) ? sy : (sy - ly);
        float dz = (dwMode == kXboxDs3dModeHeadRelative) ? sz : (sz - lz);
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > 0.001f) {
            float rx = 1.0f, ry = 0.0f, rz = 0.0f;
            float ux = 0.0f, uy = 1.0f, uz = 0.0f;
            float zx = 0.0f, zy = 0.0f, zz = 1.0f;
            if (dwMode != kXboxDs3dModeHeadRelative) {
                rx = ty * fz - tz * fy;
                ry = tz * fx - tx * fz;
                rz = tx * fy - ty * fx;
                float rLen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rLen > 0.001f) { rx /= rLen; ry /= rLen; rz /= rLen; }
                else { rx = 1.0f; ry = 0.0f; rz = 0.0f; }
                ux = tx; uy = ty; uz = tz;
                zx = fx; zy = fy; zz = fz;
            }
            float localX = (dx * rx + dy * ry + dz * rz) / dist;
            float localY = (dx * ux + dy * uy + dz * uz) / dist;
            float localZ = (dx * zx + dy * zy + dz * zz) / dist;
            if (localY < -1.0f) localY = -1.0f;
            if (localY >  1.0f) localY =  1.0f;
            elevationDeg = asinf(localY) * (180.0f / kPiFloat);
            azimuthDeg   = atan2f(localX, localZ) * (180.0f / kPiFloat);
            validPolar   = true;
        }
    }

    LONG    computedPan       = 0;
    int32_t hrtfShadowAttenMb = 0;
    int16_t hrtfFcHw          = vdDirectFc;
    int32_t itdSamples        = 0;

    if (validPolar) {
        if (azimuthDeg < -180.0f)  azimuthDeg = -180.0f;
        if (azimuthDeg >  180.0f)  azimuthDeg =  180.0f;
        if (elevationDeg < -90.0f) elevationDeg = -90.0f;
        if (elevationDeg >  90.0f) elevationDeg =  90.0f;

        float azRad = azimuthDeg * (kPiFloat / 180.0f);
        float elRad = elevationDeg * (kPiFloat / 180.0f);
        float sinAz = sinf(azRad) * cosf(elRad);

        float gainL = 1.0f, gainR = 1.0f;
        float hfL   = 1.0f, hfR   = 1.0f;
        if (hasNativeHrtf) {
            XboxHrtfFilterPair filterPair = { nullptr, nullptr };
            reinterpret_cast<PFN_CFullHRTFSource_GetFilterPair>(0x006E3B11u)(azimuthDeg, elevationDeg, 1, &filterPair);
            EvaluateHrtfFirResponse(filterPair.pLeft,  &gainL, &hfL);
            EvaluateHrtfFirResponse(filterPair.pRight, &gainR, &hfR);
            if (filterPair.pLeft && filterPair.pRight) {
                itdSamples = (azimuthDeg >= 0.0f)
                    ? static_cast<int32_t>(filterPair.pLeft[31] & 0x3Fu)
                    : -static_cast<int32_t>(filterPair.pRight[31] & 0x3Fu);
            }
        } else {
            // Analytical Woodworth spherical-head ITD (r = 0.0875m, c = 343 m/s @ 48 kHz) & head-shadow ILD
            float latAngle = asinf(sinAz < -1.0f ? -1.0f : (sinAz > 1.0f ? 1.0f : sinAz));
            float absLat   = fabsf(latAngle);
            int32_t woodworthSamples = static_cast<int32_t>((0.0875f / 343.0f) * (absLat + sinf(absLat)) * 48000.0f + 0.5f);
            if (woodworthSamples > 63) woodworthSamples = 63;
            itdSamples = (azimuthDeg >= 0.0f) ? woodworthSamples : -woodworthSamples;
            gainL = 1.0f - 0.35f * sinAz;
            gainR = 1.0f + 0.35f * sinAz;
            hfL   = (sinAz > 0.0f) ? (1.0f - 0.45f * sinAz) : 1.0f;
            hfR   = (sinAz < 0.0f) ? (1.0f + 0.45f * sinAz) : 1.0f;
        }

        float firIldDb    = 20.0f * log10f((gainR + 0.01f) / (gainL + 0.01f));
        float itdPanMb    = static_cast<float>(itdSamples) * 28.0f;
        float geomPanMb   = sinAz * kMaxStereoPanMillibels;
        float combinedPan = 0.45f * (firIldDb * 100.0f + itdPanMb) + 0.55f * geomPanMb;
        if (combinedPan < -kMaxStereoPanMillibels) combinedPan = -kMaxStereoPanMillibels;
        if (combinedPan >  kMaxStereoPanMillibels) combinedPan =  kMaxStereoPanMillibels;
        computedPan = static_cast<LONG>(combinedPan);

        float absAz = fabsf(azimuthDeg);
        if (absAz > 90.0f) {
            float rearFactor = (absAz - 90.0f) / 90.0f;
            hrtfShadowAttenMb -= static_cast<int32_t>(rearFactor * 450.0f);
            int16_t rearFc = static_cast<int16_t>(-2200.0f * rearFactor * (1.1f - 0.3f * (hfL + hfR)));
            if (rearFc < hrtfFcHw) {
                hrtfFcHw = rearFc;
            }
        }
        if (frontVol < 0 && rearVol < 0 && frontVol > kMinDirectSoundVolumeMb && rearVol > kMinDirectSoundVolumeMb) {
            float pFront = powf(10.0f, static_cast<float>(frontVol) / 1000.0f);
            float pRear  = powf(10.0f, static_cast<float>(rearVol)  / 1000.0f) * 0.5f;
            float pTotal = pFront + pRear;
            if (pTotal > 0.0001f && pTotal < 1.0f) {
                int32_t foldMb = static_cast<int32_t>(1000.0f * log10f(pTotal));
                if (foldMb < hrtfShadowAttenMb) hrtfShadowAttenMb = foldMb;
            }
        }
    }

    float hrtfFilterCoeff = 1.0f;
    if (hrtfFcHw < 0) {
        hrtfFilterCoeff = DecodeMcpxFilterFreqCoeff(hrtfFcHw, 48000u);
        if (hrtfFilterCoeff > 0.91f) hrtfFilterCoeff = 0.91f;
    }

    LONG totalAtten = static_cast<LONG>(distVol + coneVol + i3dl2Vol + hrtfShadowAttenMb);
    if (totalAtten < kMinDirectSoundVolumeMb) totalAtten = kMinDirectSoundVolumeMb;
    if (totalAtten > kMaxDirectSoundVolumeMb) totalAtten = kMaxDirectSoundVolumeMb;
    *outAttenDb = totalAtten;
    *outPan     = computedPan;

    if (outMetrics) {
        outMetrics->pan             = computedPan;
        outMetrics->totalAttenMb    = totalAtten;
        outMetrics->distanceVol     = distVol;
        outMetrics->coneVol         = coneVol;
        outMetrics->dopplerPitch    = dopplerPitch;
        outMetrics->i3dl2DirectVol  = i3dl2Vol;
        outMetrics->i3dl2RoomVol    = vdI3dl2Room;
        outMetrics->itdDelaySamples = itdSamples;
        outMetrics->hrtfFilterCoeff = hrtfFilterCoeff;
        outMetrics->active3D        = true;
    }
}

void ComputeMixBinPairsPan(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, LONG* outPan, LONG* outAttenMb) {
    if (!outPan || !pairs || count == 0u) return;
    if (count > 8u) count = 8u;

    float leftGain   = 0.0f;
    float rightGain  = 0.0f;
    bool  hasSpeaker = false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bin = pairs[i].dwMixBin;
        int32_t  vol = pairs[i].lVolume;
        if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
        if (vol > 0)                       vol = 0;
        float g = (vol <= kMinDirectSoundVolumeMb) ? 0.0f : powf(10.0f, static_cast<float>(vol) / 2000.0f);
        if (bin == 0u || bin == 4u) { // FRONT_LEFT or BACK_LEFT
            leftGain += g;
            hasSpeaker = true;
        } else if (bin == 1u || bin == 5u) { // FRONT_RIGHT or BACK_RIGHT
            rightGain += g;
            hasSpeaker = true;
        } else if (bin == 2u) { // FRONT_CENTER (-3 dB equal-power split)
            leftGain  += g * 0.70710678f;
            rightGain += g * 0.70710678f;
            hasSpeaker = true;
        } else if (bin == 3u) { // LFE (-6 dB stereo fold-down)
            leftGain  += g * 0.5f;
            rightGain += g * 0.5f;
            hasSpeaker = true;
        }
    }

    if (!hasSpeaker) {
        return;
    }

    if (leftGain <= 0.00001f && rightGain <= 0.00001f) {
        *outPan = 0;
        if (outAttenMb) *outAttenMb = kMinDirectSoundVolumeMb;
        return;
    }

    float peakGain = (leftGain > rightGain) ? leftGain : rightGain;
    if (peakGain > 1.0f) peakGain = 1.0f;
    if (outAttenMb) {
        LONG attMb = (peakGain >= 0.9995f) ? 0 : static_cast<LONG>(2000.0f * log10f(peakGain));
        if (attMb < kMinDirectSoundVolumeMb) attMb = kMinDirectSoundVolumeMb;
        if (attMb > 0)                       attMb = 0;
        *outAttenMb = attMb;
    }

    LONG pan = 0;
    if (fabsf(rightGain - leftGain) > 0.0001f * peakGain) {
        if (leftGain > rightGain) {
            float ratio = (rightGain > 0.00001f) ? (rightGain / leftGain) : 0.00001f;
            pan = static_cast<LONG>(2000.0f * log10f(ratio));
        } else {
            float ratio = (leftGain > 0.00001f) ? (leftGain / rightGain) : 0.00001f;
            pan = static_cast<LONG>(-2000.0f * log10f(ratio));
        }
        if (pan < kMinDirectSoundPanMb) pan = kMinDirectSoundPanMb;
        if (pan > kMaxDirectSoundPanMb) pan = kMaxDirectSoundPanMb;
    }
    *outPan = pan;
}

void ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan, LONG* outAttenMb) {
    if (!outPan || !pMixBins || pMixBins->dwMixBinCount == 0u || pMixBins->dwMixBinCount > 32u || !pMixBins->lpMixBinVolumePairs) return;
    if (reinterpret_cast<uintptr_t>(pMixBins->lpMixBinVolumePairs) < kMinValidHostAddress) return;

    XBOX_DSMIXBINVOLUMEPAIR tempPairs[8] = {};
    uint32_t count = (pMixBins->dwMixBinCount > 8u) ? 8u : pMixBins->dwMixBinCount;
    for (uint32_t i = 0; i < count; ++i) {
        tempPairs[i] = pMixBins->lpMixBinVolumePairs[i];
    }
    ComputeMixBinPairsPan(tempPairs, count, outPan, outAttenMb);
}

void InitVoiceDefaultMixBins(uint32_t dwFlags, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount) {
    if (!outPairs || !outCount) return;
    memset(outPairs, 0, sizeof(XBOX_DSMIXBINVOLUMEPAIR) * 8u);
    // Matches CDirectSoundVoiceSettings::SetMixBins (0x006E1A9C):
    //   If (dwFlags & 0x00200010) != 0 (3D voice), initializes with _DirectSoundDefaultMixBins_3D (0x006FD8BC):
    //     5 bins: {6,0}, {8,0}, {7,0}, {9,0}, {10,0} (3D Front, Back, Center, LFE, I3DL2 Room).
    //   Otherwise initializes with _DirectSoundDefaultMixBins_Stereo (0x006FD8DC):
    //     2 bins: {0,0}, {1,0} (Front Left, Front Right).
    if ((dwFlags & 0x00200010u) != 0u) {
        static const uint32_t kDefault3DBins[5] = { 6u, 8u, 7u, 9u, 10u };
        for (uint32_t i = 0; i < 5u; ++i) {
            outPairs[i].dwMixBin = kDefault3DBins[i];
            outPairs[i].lVolume  = 0;
        }
        *outCount = 5u;
    } else {
        outPairs[0].dwMixBin = 0u;
        outPairs[0].lVolume  = 0;
        outPairs[1].dwMixBin = 1u;
        outPairs[1].lVolume  = 0;
        *outCount = 2u;
    }
}

void AssignVoiceMixBins(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount) {
    if (!outPairs || !outCount || !pMixBins || !pMixBins->lpMixBinVolumePairs) return;
    if (reinterpret_cast<uintptr_t>(pMixBins->lpMixBinVolumePairs) < kMinValidHostAddress) return;
    uint32_t count = pMixBins->dwMixBinCount;
    if (count > 8u) count = 8u;
    for (uint32_t i = 0; i < count; ++i) {
        outPairs[i].dwMixBin = pMixBins->lpMixBinVolumePairs[i].dwMixBin;
        int32_t vol = pMixBins->lpMixBinVolumePairs[i].lVolume;
        if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
        if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;
        outPairs[i].lVolume = vol;
    }
    for (uint32_t i = count; i < 8u; ++i) {
        outPairs[i].dwMixBin = 0xFFFFFFFFu;
        outPairs[i].lVolume  = kMinDirectSoundVolumeMb;
    }
    *outCount = count;
}

void UpdateVoiceMixBinVolumes(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR ioPairs[8], uint32_t* ioCount) {
    if (!ioPairs || !ioCount || !pMixBins || !pMixBins->lpMixBinVolumePairs) return;
    if (reinterpret_cast<uintptr_t>(pMixBins->lpMixBinVolumePairs) < kMinValidHostAddress) return;
    uint32_t inCount  = pMixBins->dwMixBinCount;
    if (inCount > 32u) inCount = 32u;
    uint32_t curCount = (*ioCount > 8u) ? 8u : *ioCount;

    for (uint32_t i = 0; i < inCount; ++i) {
        uint32_t bin = pMixBins->lpMixBinVolumePairs[i].dwMixBin;
        int32_t  vol = pMixBins->lpMixBinVolumePairs[i].lVolume;
        if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
        if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;

        bool found = false;
        for (uint32_t j = 0; j < curCount; ++j) {
            if (ioPairs[j].dwMixBin == bin) {
                ioPairs[j].lVolume = vol;
                found = true;
                break;
            }
        }
        if (!found && curCount < 8u) {
            ioPairs[curCount].dwMixBin = bin;
            ioPairs[curCount].lVolume  = vol;
            ++curCount;
        }
    }
    *ioCount = curCount;
}

void FillVoicePropsMixBins(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, XBOX_DSVOICEPROPS* outProps) {
    if (!outProps) return;
    if (count > 8u) count = 8u;
    outProps->dwMixBinCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        outProps->MixBinVolumePairs[i] = pairs[i];
    }
    // Matches CMcpxVoiceClient::GetVoiceProperties (0x006E72C4): unused MixBin slots filled with {0xFFFFFFFF, -10000}
    for (uint32_t i = count; i < 8u; ++i) {
        outProps->MixBinVolumePairs[i].dwMixBin = 0xFFFFFFFFu;
        outProps->MixBinVolumePairs[i].lVolume  = kMinDirectSoundVolumeMb;
    }
}

void ExtractXboxMixBinI3DL2Send(
    const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, bool* outHasSend, int32_t* outSendMb)
{
    if (!outHasSend || !outSendMb || !pairs) return;
    *outHasSend = false;
    *outSendMb  = kMinDirectSoundVolumeMb;
    if (count > 8u) count = 8u;
    for (uint32_t i = 0; i < count; ++i) {
        if (pairs[i].dwMixBin == kXboxMixBinI3DL2) {
            int32_t v = pairs[i].lVolume;
            if (v > -9000) {
                *outHasSend = true;
                *outSendMb  = (v > 0) ? 0 : v;
            }
            return;
        }
    }
}

// ============================================================================
// Float32 Chamberlin State-Variable Filter, HRTF Head-Shadow & Binaural ITD
// ============================================================================

static void ClampChamberlinSvfCoeffs(float& fCoeff, float& dCoeff) {
    if (!_finite(dCoeff)) dCoeff = 1.0f;
    if (dCoeff < 0.075f)  dCoeff = 0.075f;
    if (dCoeff > 2.0f)    dCoeff = 2.0f;

    if (!_finite(fCoeff)) fCoeff = 0.5f;
    if (fCoeff < 0.004f)  fCoeff = 0.004f;
    // Exact Jury stability bound for Chamberlin 2-integrator SVF: f^2 + 2*f*d < 4 <=> f < sqrt(d^2 + 4) - d.
    // Multiply by 0.92f so discrete poles stay strictly inside the unit circle with headroom against ringing.
    float fMax = 0.92f * (sqrtf(dCoeff * dCoeff + 4.0f) - dCoeff);
    if (fMax > 1.35f) fMax = 1.35f;
    if (fCoeff > fMax) fCoeff = fMax;
}

static inline void SanitizeFloatDspState(float& s) {
    if (!_finite(s) || fabsf(s) < 1e-20f) {
        s = 0.0f;
    } else if (s > 8.0f) {
        s = 8.0f;
    } else if (s < -8.0f) {
        s = -8.0f;
    }
}

static float DecodeMcpxFilterFreqCoeff(int16_t fcHw, uint32_t sampleRateHz) {
    float f48k = 1.0f;
    if (fcHw == -32768) {
        // Cutoff <= 30 Hz floor (0x00708D73: mov eax, 0x8000)
        f48k = 2.0f * sinf(kPiFloat * 30.0f / 48000.0f);
    } else if (fcHw < 0) {
        f48k = powf(2.0f, static_cast<float>(fcHw) / 4096.0f);
    } else {
        f48k = 2.0f * sinf(kPiFloat * 8000.0f / 48000.0f);
    }
    if (sampleRateHz < 8000u) sampleRateHz = 48000u;
    float fScaled = f48k * (48000.0f / static_cast<float>(sampleRateHz));
    if (fScaled < 0.004f) fScaled = 0.004f;
    if (fScaled > 1.35f)  fScaled = 1.35f;
    return fScaled;
}

static float DecodeMcpxFilterQCoeff(uint16_t qHw, uint32_t dwQCoefficient) {
    if (qHw != 0u && qHw != 0x8000u) {
        float dVal = static_cast<float>(qHw) / 32768.0f;
        if (dVal < 0.075f) dVal = 0.075f;
        if (dVal > 2.0f)   dVal = 2.0f;
        return dVal;
    }
    if (dwQCoefficient != 0u) {
        if (dwQCoefficient <= 7u) {
            // 3-bit hardware Q index (verified in CMcpxVoiceClient::SetFilter at 0x006E7069: and eax, 7,
            // and XACT::CSoundCue::UpdateTrackFilter at 0x0070644F): ~3.21 dB resonance steps up to +22.5 dB.
            float resDb = static_cast<float>(dwQCoefficient) * (22.5f / 7.0f);
            float dVal  = powf(10.0f, -resDb / 20.0f);
            if (dVal < 0.075f) dVal = 0.075f;
            if (dVal > 2.0f)   dVal = 2.0f;
            return dVal;
        }
        float dVal = static_cast<float>(dwQCoefficient & 0xFFFFu) / 32768.0f;
        if (dVal < 0.075f) dVal = 0.075f;
        if (dVal > 2.0f)   dVal = 2.0f;
        return dVal;
    }
    return 1.0f;
}

bool ComputeDls2FilterCoeffs(
    const XBOX_DSFILTERDESC* pFilter, uint32_t sampleRateHz,
    float* outF, float* outD,
    uint32_t* outMode, float* outF2, float* outD2)
{
    if (!outF || !outD) return false;
    *outF = 1.0f;
    *outD = 1.0f;
    if (outMode) *outMode = 0u;
    if (outF2)   *outF2   = 1.0f;
    if (outD2)   *outD2   = 1.0f;
    if (!pFilter || reinterpret_cast<uintptr_t>(pFilter) < kMinValidHostAddress) return false;

    // Verified against CMcpxVoiceClient::SetFilter (0x006E6FF1) and XACT::CSoundCue::UpdateTrackFilter (0x007063E9):
    //   dwMode == 0: DSFILTERMODE_BYPASS
    //   dwMode == 1: DSFILTERMODE_DLS2    (2-pole resonant low-pass + optional 2-pole parametric EQ)
    //   dwMode == 2: DSFILTERMODE_PARAMEQ (cascaded 2-pole parametric EQ)
    //   dwMode == 3: DSFILTERMODE_MULTI   (cascaded 2-pole state-variable stages)
    uint32_t mode = pFilter->dwMode & 3u;
    if (mode == 0u) return false;

    uint32_t qOverride = pFilter->dwQCoefficient;
    int16_t  fc0 = static_cast<int16_t>(pFilter->adwCoefficients[0] & 0xFFFFu);
    uint16_t q0  = static_cast<uint16_t>(pFilter->adwCoefficients[1] & 0xFFFFu);
    int16_t  fc1 = static_cast<int16_t>(pFilter->adwCoefficients[2] & 0xFFFFu);
    uint16_t q1  = static_cast<uint16_t>(pFilter->adwCoefficients[3] & 0xFFFFu);

    bool s0Active = !(fc0 == 0 && (q0 == 0u || q0 == 0x8000u) && qOverride == 0u);
    bool s1Active = !(fc1 == 0 && (q1 == 0u || q1 == 0x8000u) && qOverride == 0u);
    // In XACT::CSoundCue::UpdateTrackFilter (0x0070649E..0x007064CB), DLS2 mode (mode == 1)
    // duplicates {fc0, q0} into adwCoefficients[2..3] for stereo voice pairs with dwQCoefficient == 0.
    if ((mode == 1u || mode == 2u) && fc0 == fc1 && q0 == q1 && qOverride == 0u) {
        s1Active = false;
    }
    if (!s0Active && !s1Active) {
        return false;
    }

    if (outMode) *outMode = mode;
    if (s0Active) {
        float f0 = DecodeMcpxFilterFreqCoeff(fc0, sampleRateHz);
        float d0 = DecodeMcpxFilterQCoeff(q0, qOverride);
        ClampChamberlinSvfCoeffs(f0, d0);
        *outF = f0;
        *outD = d0;
    }
    if (s1Active && outF2 && outD2) {
        float f1 = DecodeMcpxFilterFreqCoeff(fc1, sampleRateHz);
        float d1 = DecodeMcpxFilterQCoeff(q1, (mode == 2u) ? qOverride : 0u);
        ClampChamberlinSvfCoeffs(f1, d1);
        *outF2 = f1;
        *outD2 = d1;
    }
    return true;
}

void ApplyVoiceSvfAndHrtfFloatStereo(
    float* ioLeft, float* ioRight, uint32_t numFrames,
    const XBOX_DSFILTERDESC* pFilter, bool hasActiveFilter, float lfoEgCutoffRatio,
    bool active3D, float hrtfFilterCoeff, int32_t itdDelaySamples,
    XboxVoiceFilterState* ioState)
{
    if (!ioLeft || !ioRight || numFrames == 0u || !ioState) return;

    float fCoeff = 1.0f, dCoeff = 1.0f, fCoeff2 = 1.0f, dCoeff2 = 1.0f;
    uint32_t filterMode = 0u;
    bool runSvf = hasActiveFilter &&
                  ComputeDls2FilterCoeffs(pFilter, kDefaultSampleRateHz, &fCoeff, &dCoeff, &filterMode, &fCoeff2, &dCoeff2);
    if (runSvf && lfoEgCutoffRatio > 0.01f && fabsf(lfoEgCutoffRatio - 1.0f) > 0.005f) {
        fCoeff *= lfoEgCutoffRatio;
        ClampChamberlinSvfCoeffs(fCoeff, dCoeff);
    }

    bool hasStage1 = runSvf && !(fCoeff >= 0.999f && fabsf(dCoeff - 1.0f) < 0.001f);
    bool hasStage2 = runSvf && !(fCoeff2 >= 0.999f && fabsf(dCoeff2 - 1.0f) < 0.001f);
    if (hasStage1) ClampChamberlinSvfCoeffs(fCoeff, dCoeff);
    if (hasStage2) ClampChamberlinSvfCoeffs(fCoeff2, dCoeff2);

    bool runHrtfLp = active3D && (hrtfFilterCoeff > 0.0f && hrtfFilterCoeff < 0.94f);
    float hrtfAlpha = hrtfFilterCoeff;
    if (hrtfAlpha < 0.02f) hrtfAlpha = 0.02f;
    if (hrtfAlpha > 0.94f) hrtfAlpha = 0.94f;

    uint32_t delayL = 0u;
    uint32_t delayR = 0u;
    if (active3D && itdDelaySamples != 0) {
        if (itdDelaySamples > 0) {
            delayL = (itdDelaySamples > 63) ? 63u : static_cast<uint32_t>(itdDelaySamples);
        } else {
            int32_t neg = -itdDelaySamples;
            delayR = (neg > 63) ? 63u : static_cast<uint32_t>(neg);
        }
    }

    if (!hasStage1 && !hasStage2 && !runHrtfLp && !active3D) {
        return;
    }

    float low[2]     = { ioState->low[0],     ioState->low[1] };
    float band[2]    = { ioState->band[0],    ioState->band[1] };
    float low2[2]    = { ioState->low2[0],    ioState->low2[1] };
    float band2[2]   = { ioState->band2[0],   ioState->band2[1] };
    float hrtfLow[2] = { ioState->hrtfLow[0], ioState->hrtfLow[1] };
    uint8_t wrPos    = ioState->itdWritePos & 63u;

    for (int ch = 0; ch < 2; ++ch) {
        SanitizeFloatDspState(low[ch]);
        SanitizeFloatDspState(band[ch]);
        SanitizeFloatDspState(low2[ch]);
        SanitizeFloatDspState(band2[ch]);
        SanitizeFloatDspState(hrtfLow[ch]);
    }

    for (uint32_t f = 0u; f < numFrames; ++f) {
        float chSample[2] = { ioLeft[f], ioRight[f] };

        if (hasStage1 || hasStage2) {
            for (int ch = 0; ch < 2; ++ch) {
                float y = chSample[ch];
                if (hasStage1) {
                    low[ch]   += fCoeff * band[ch];
                    float high = y - low[ch] - dCoeff * band[ch];
                    band[ch]  += fCoeff * high;
                    SanitizeFloatDspState(low[ch]);
                    SanitizeFloatDspState(band[ch]);
                    if (filterMode == 2u) {
                        float peakGain = (1.0f / dCoeff) - 1.0f;
                        y = y + peakGain * dCoeff * band[ch];
                    } else {
                        y = low[ch];
                    }
                }
                if (hasStage2) {
                    low2[ch]   += fCoeff2 * band2[ch];
                    float high2 = y - low2[ch] - dCoeff2 * band2[ch];
                    band2[ch]  += fCoeff2 * high2;
                    SanitizeFloatDspState(low2[ch]);
                    SanitizeFloatDspState(band2[ch]);
                    if (filterMode == 1u || filterMode == 2u) {
                        float peakGain2 = (1.0f / dCoeff2) - 1.0f;
                        y = y + peakGain2 * dCoeff2 * band2[ch];
                    } else {
                        y = low2[ch];
                    }
                }
                if (!_finite(y)) y = 0.0f;
                chSample[ch] = y;
            }
        }

        if (runHrtfLp) {
            hrtfLow[0] += hrtfAlpha * (chSample[0] - hrtfLow[0]);
            hrtfLow[1] += hrtfAlpha * (chSample[1] - hrtfLow[1]);
            SanitizeFloatDspState(hrtfLow[0]);
            SanitizeFloatDspState(hrtfLow[1]);
            chSample[0] = hrtfLow[0];
            chSample[1] = hrtfLow[1];
        }

        if (active3D) {
            ioState->itdRingL[wrPos] = chSample[0];
            ioState->itdRingR[wrPos] = chSample[1];
            uint32_t rdL = (static_cast<uint32_t>(wrPos) + 64u - delayL) & 63u;
            uint32_t rdR = (static_cast<uint32_t>(wrPos) + 64u - delayR) & 63u;
            chSample[0] = ioState->itdRingL[rdL];
            chSample[1] = ioState->itdRingR[rdR];
            wrPos = static_cast<uint8_t>((wrPos + 1u) & 63u);
        }

        ioLeft[f]  = chSample[0];
        ioRight[f] = chSample[1];
    }

    ioState->low[0]      = low[0];     ioState->low[1]      = low[1];
    ioState->band[0]     = band[0];    ioState->band[1]     = band[1];
    ioState->low2[0]     = low2[0];    ioState->low2[1]     = low2[1];
    ioState->band2[0]    = band2[0];   ioState->band2[1]    = band2[1];
    ioState->hrtfLow[0]  = hrtfLow[0]; ioState->hrtfLow[1]  = hrtfLow[1];
    ioState->itdWritePos = wrPos;
}

// ============================================================================
// Global Real-Time 48,000 Hz Stereo I3DL2 Environmental Reverb Bus
// ============================================================================

struct XboxGlobalI3DL2ReverbTank {
    static constexpr size_t kPreDelayRingSize = 19200u; // 400 ms at 48,000 Hz (covers max 300ms ReflDelay + 100ms ReverbDelay)
    static constexpr size_t kNumCombsPerCh    = 4u;
    static constexpr size_t kMaxCombLen       = 2400u;  // 50 ms at 48,000 Hz
    static constexpr size_t kNumAllpassPerCh  = 2u;
    static constexpr size_t kMaxAllpassLen    = 768u;

    float    preDelayL[kPreDelayRingSize];
    float    preDelayR[kPreDelayRingSize];
    uint32_t preDelayIdx;

    float    inputHfStateL;
    float    inputHfStateR;

    float    combBufL[kNumCombsPerCh][kMaxCombLen];
    float    combBufR[kNumCombsPerCh][kMaxCombLen];
    uint32_t combIdxL[kNumCombsPerCh];
    uint32_t combIdxR[kNumCombsPerCh];
    float    combLpL[kNumCombsPerCh];
    float    combLpR[kNumCombsPerCh];

    float    allpassBufL[kNumAllpassPerCh][kMaxAllpassLen];
    float    allpassBufR[kNumAllpassPerCh][kMaxAllpassLen];
    uint32_t allpassIdxL[kNumAllpassPerCh];
    uint32_t allpassIdxR[kNumAllpassPerCh];

    float    smoothedEarlyGain;
    float    smoothedLateGain;
    uint32_t silentFramesCount;
    uint32_t quietTankFramesCount;
    bool     gainInitialized;
};

static XboxGlobalI3DL2ReverbTank g_globalReverbTank = {};
static SRWLOCK                   g_reverbTankLock   = SRWLOCK_INIT;

namespace {
struct ReverbTankLockGuard {
    SRWLOCK* lock;
    explicit ReverbTankLockGuard(SRWLOCK* l) : lock(l) { AcquireSRWLockExclusive(lock); }
    ~ReverbTankLockGuard() { ReleaseSRWLockExclusive(lock); }
    ReverbTankLockGuard(const ReverbTankLockGuard&) = delete;
    ReverbTankLockGuard& operator=(const ReverbTankLockGuard&) = delete;
};
}

void ResetGlobalI3DL2Reverb48kLocked() {
    ReverbTankLockGuard guard(&g_reverbTankLock);
    memset(&g_globalReverbTank, 0, sizeof(g_globalReverbTank));
}

void ProcessGlobalI3DL2Reverb48k(
    const float* wetSendL, const float* wetSendR,
    float* ioMasterDryL, float* ioMasterDryR, uint32_t numFrames,
    const XBOX_DSI3DL2LISTENER* listenerSnapshot)
{
    if (!wetSendL || !wetSendR || !ioMasterDryL || !ioMasterDryR || numFrames == 0u) return;

    XBOX_DSI3DL2LISTENER fallbackListener;
    if (!listenerSnapshot) {
        fallbackListener = GetXboxI3DL2ListenerState();
        listenerSnapshot = &fallbackListener;
    }

    ReverbTankLockGuard guard(&g_reverbTankLock);
    XboxGlobalI3DL2ReverbTank& tank = g_globalReverbTank;
    const XBOX_DSI3DL2LISTENER& rev = *listenerSnapshot;

    // Compute target early-reflection and late-reverb linear gains from I3DL2 millibel parameters
    float targetEarlyGain = 0.0f;
    float targetLateGain  = 0.0f;
    if (rev.lRoom > -9500) {
        int32_t earlyMb = rev.lRoom + rev.lReflections;
        int32_t lateMb  = rev.lRoom + rev.lReverb;
        if (earlyMb > -9500) {
            if (earlyMb > 600) earlyMb = 600;
            targetEarlyGain = powf(10.0f, static_cast<float>(earlyMb) / 2000.0f);
            if (targetEarlyGain > 0.70f) targetEarlyGain = 0.70f;
        }
        if (lateMb > -9500) {
            if (lateMb > 600) lateMb = 600;
            targetLateGain = powf(10.0f, static_cast<float>(lateMb) / 2000.0f);
            if (targetLateGain > 0.65f) targetLateGain = 0.65f;
        }
    }

    if (!tank.gainInitialized) {
        tank.smoothedEarlyGain = targetEarlyGain;
        tank.smoothedLateGain  = targetLateGain;
        tank.gainInitialized   = true;
    }

    // Check if there is any wet send input energy in this quantum
    float inputPeak = 0.0f;
    for (uint32_t i = 0u; i < numFrames; ++i) {
        float aL = fabsf(wetSendL[i]);
        float aR = fabsf(wetSendR[i]);
        if (aL > inputPeak) inputPeak = aL;
        if (aR > inputPeak) inputPeak = aR;
    }

    float decaySec = rev.flDecayTime;
    if (!_finite(decaySec) || decaySec < 0.10f) decaySec = 0.10f;
    if (decaySec > 20.0f) decaySec = 20.0f;

    // Pre-delay tap offsets at 48,000 Hz (computed prior to silent-bypass check so pre-delayed impulses are never cut off)
    float reflDelaySec = rev.flReflectionsDelay;
    if (!_finite(reflDelaySec) || reflDelaySec < 0.0f) reflDelaySec = 0.007f;
    if (reflDelaySec > 0.300f) reflDelaySec = 0.300f;

    float revDelaySec = rev.flReverbDelay;
    if (!_finite(revDelaySec) || revDelaySec < 0.0f) revDelaySec = 0.011f;
    if (revDelaySec > 0.100f) revDelaySec = 0.100f;

    uint32_t reflDelaySamples = static_cast<uint32_t>(reflDelaySec * 48000.0f + 0.5f);
    if (reflDelaySamples < 1u) reflDelaySamples = 1u;
    if (reflDelaySamples >= XboxGlobalI3DL2ReverbTank::kPreDelayRingSize - 1600u) {
        reflDelaySamples = static_cast<uint32_t>(XboxGlobalI3DL2ReverbTank::kPreDelayRingSize - 1600u);
    }

    uint32_t lateDelaySamples = reflDelaySamples + static_cast<uint32_t>(revDelaySec * 48000.0f + 0.5f);
    if (lateDelaySamples < 1u) lateDelaySamples = 1u;
    if (lateDelaySamples >= XboxGlobalI3DL2ReverbTank::kPreDelayRingSize - 1u) {
        lateDelaySamples = static_cast<uint32_t>(XboxGlobalI3DL2ReverbTank::kPreDelayRingSize - 1u);
    }

    uint32_t maxTailFrames          = static_cast<uint32_t>((decaySec * 1.5f + 0.5f) * 48000.0f);
    uint32_t minSilenceForPreDelay  = lateDelaySamples + 2500u;
    if (inputPeak > 1e-6f) {
        tank.silentFramesCount    = 0u;
        tank.quietTankFramesCount = 0u;
    } else {
        if (tank.silentFramesCount > maxTailFrames) {
            return;
        }
        if (tank.silentFramesCount > minSilenceForPreDelay && tank.quietTankFramesCount >= 2500u) {
            return;
        }
        if (tank.silentFramesCount > minSilenceForPreDelay &&
            targetEarlyGain < 1e-4f && targetLateGain < 1e-4f &&
            tank.smoothedEarlyGain < 1e-4f && tank.smoothedLateGain < 1e-4f)
        {
            return;
        }
    }

    // Input 1-pole HF attenuation shelf/LPF (lRoomHF at flHFReference)
    float hfRefHz = rev.flHFReference;
    if (!_finite(hfRefHz) || hfRefHz < 200.0f) hfRefHz = 5000.0f;
    if (hfRefHz > 18000.0f) hfRefHz = 18000.0f;
    float roomHfLin = (rev.lRoomHF <= -9500) ? 0.02f : powf(10.0f, static_cast<float>(rev.lRoomHF) / 2000.0f);
    if (roomHfLin < 0.02f) roomHfLin = 0.02f;
    if (roomHfLin > 1.00f) roomHfLin = 1.00f;
    float baseOmega = 2.0f * kPiFloat * hfRefHz / 48000.0f;
    if (baseOmega > 1.2f) baseOmega = 1.2f;
    float inputLpAlpha = (roomHfLin >= 0.99f) ? 1.0f : (baseOmega * roomHfLin + 0.05f);
    if (inputLpAlpha < 0.04f) inputLpAlpha = 0.04f;
    if (inputLpAlpha > 1.00f) inputLpAlpha = 1.00f;

    // Stereo decorrelated prime comb delays at 48,000 Hz scaled by flDensity [0..100%]
    float density = rev.flDensity;
    if (!_finite(density) || density < 0.0f) density = 100.0f;
    if (density > 100.0f) density = 100.0f;
    float densityScale = 0.65f + 0.35f * (density / 100.0f);

    static const uint32_t kPrimeCombBaseL[4] = { 1489u, 1693u, 1901u, 2099u };
    static const uint32_t kPrimeCombBaseR[4] = { 1513u, 1721u, 1931u, 2131u };

    float hfRatio = rev.flDecayHFRatio;
    if (!_finite(hfRatio) || hfRatio < 0.10f) hfRatio = 0.10f;
    if (hfRatio > 2.00f) hfRatio = 2.00f;

    uint32_t combLenL[4], combLenR[4];
    float    combFbL[4],  combFbR[4];
    float    combDamp[4];

    for (uint32_t c = 0u; c < 4u; ++c) {
        uint32_t dL = static_cast<uint32_t>(static_cast<float>(kPrimeCombBaseL[c]) * densityScale + 0.5f);
        uint32_t dR = static_cast<uint32_t>(static_cast<float>(kPrimeCombBaseR[c]) * densityScale + 0.5f);
        if (dL < 128u) dL = 128u;
        if (dR < 128u) dR = 128u;
        if (dL >= XboxGlobalI3DL2ReverbTank::kMaxCombLen) dL = static_cast<uint32_t>(XboxGlobalI3DL2ReverbTank::kMaxCombLen - 1u);
        if (dR >= XboxGlobalI3DL2ReverbTank::kMaxCombLen) dR = static_cast<uint32_t>(XboxGlobalI3DL2ReverbTank::kMaxCombLen - 1u);
        combLenL[c] = dL;
        combLenR[c] = dR;

        float fbL = powf(10.0f, (-3.0f * static_cast<float>(dL)) / (48000.0f * decaySec));
        float fbR = powf(10.0f, (-3.0f * static_cast<float>(dR)) / (48000.0f * decaySec));
        if (fbL > 0.965f) fbL = 0.965f;
        if (fbR > 0.965f) fbR = 0.965f;
        combFbL[c] = fbL;
        combFbR[c] = fbR;

        float hfDecaySec = decaySec * hfRatio;
        if (hfDecaySec < 0.05f) hfDecaySec = 0.05f;
        float fbHf = powf(10.0f, (-3.0f * static_cast<float>(dL)) / (48000.0f * hfDecaySec));
        float ratio = (fbL > 0.001f) ? (fbHf / fbL) : 1.0f;
        float damp  = (ratio < 1.0f) ? ((1.0f - ratio) * 3.2f) : 0.06f;
        if (damp < 0.05f) damp = 0.05f;
        if (damp > 0.82f) damp = 0.82f;
        combDamp[c] = damp;
    }

    // Schroeder series all-pass diffusers scaled by flDiffusion [0..100%]
    float diffusion = rev.flDiffusion;
    if (!_finite(diffusion) || diffusion < 0.0f) diffusion = 100.0f;
    if (diffusion > 100.0f) diffusion = 100.0f;
    float apGain = 0.50f * (diffusion / 100.0f);

    static const uint32_t kAllpassLenL[2] = { 269u, 593u };
    static const uint32_t kAllpassLenR[2] = { 293u, 617u };
    static const uint32_t kEarlyTapOffsets[6] = { 0u, 206u, 466u, 725u, 1051u, 1358u }; // 0..28.3 ms @ 48 kHz
    static const float    kEarlyTapWeightsL[6] = { 0.42f, 0.28f, 0.22f, 0.16f, 0.12f, 0.09f };
    static const float    kEarlyTapWeightsR[6] = { 0.40f, 0.30f, 0.20f, 0.18f, 0.11f, 0.10f };

    constexpr uint32_t kRingSize   = static_cast<uint32_t>(XboxGlobalI3DL2ReverbTank::kPreDelayRingSize);
    constexpr float    kGainSmooth = 0.006f; // ~3.5 ms time constant at 48 kHz
    float wetOutPeak    = 0.0f;
    float tankStatePeak = 0.0f;

    for (uint32_t f = 0u; f < numFrames; ++f) {
        tank.smoothedEarlyGain += kGainSmooth * (targetEarlyGain - tank.smoothedEarlyGain);
        tank.smoothedLateGain  += kGainSmooth * (targetLateGain  - tank.smoothedLateGain);

        // 1. Input HF attenuation filter into stereo pre-delay ring buffer
        float inL = wetSendL[f];
        float inR = wetSendR[f];
        if (!_finite(inL)) inL = 0.0f;
        if (!_finite(inR)) inR = 0.0f;

        tank.inputHfStateL += inputLpAlpha * (inL - tank.inputHfStateL);
        tank.inputHfStateR += inputLpAlpha * (inR - tank.inputHfStateR);
        SanitizeFloatDspState(tank.inputHfStateL);
        SanitizeFloatDspState(tank.inputHfStateR);

        uint32_t wr = tank.preDelayIdx;
        tank.preDelayL[wr] = tank.inputHfStateL;
        tank.preDelayR[wr] = tank.inputHfStateR;

        // 2. Multi-tap decorrelated stereo Early Reflections
        float earlyL = 0.0f;
        float earlyR = 0.0f;
        for (uint32_t t = 0u; t < 6u; ++t) {
            uint32_t tapDist = reflDelaySamples + kEarlyTapOffsets[t];
            uint32_t rdIdx   = (wr + kRingSize - tapDist) % kRingSize;
            float tapL = tank.preDelayL[rdIdx];
            float tapR = tank.preDelayR[rdIdx];
            if ((t & 1u) == 0u) {
                earlyL += tapL * kEarlyTapWeightsL[t];
                earlyR += tapR * kEarlyTapWeightsR[t];
            } else {
                // Cross-lateral wall bounce
                earlyL += tapR * kEarlyTapWeightsL[t];
                earlyR += tapL * kEarlyTapWeightsR[t];
            }
        }

        // 3. Late Reverb FDN / Parallel Damped Comb Tank
        uint32_t lateRd = (wr + kRingSize - lateDelaySamples) % kRingSize;
        float lateInL   = (0.75f * tank.preDelayL[lateRd] + 0.25f * tank.preDelayR[lateRd]) * 0.25f;
        float lateInR   = (0.75f * tank.preDelayR[lateRd] + 0.25f * tank.preDelayL[lateRd]) * 0.25f;

        if (++wr >= kRingSize) wr = 0u;
        tank.preDelayIdx = wr;

        float combSumL = 0.0f;
        float combSumR = 0.0f;
        for (uint32_t c = 0u; c < 4u; ++c) {
            uint32_t idxL = tank.combIdxL[c];
            if (idxL >= combLenL[c]) idxL = 0u;
            float outL = tank.combBufL[c][idxL];
            float lpL  = outL * (1.0f - combDamp[c]) + tank.combLpL[c] * combDamp[c];
            SanitizeFloatDspState(lpL);
            tank.combLpL[c] = lpL;
            float nextL = lateInL + lpL * combFbL[c];
            SanitizeFloatDspState(nextL);
            tank.combBufL[c][idxL] = nextL;
            if (++idxL >= combLenL[c]) idxL = 0u;
            tank.combIdxL[c] = idxL;
            combSumL += outL;

            uint32_t idxR = tank.combIdxR[c];
            if (idxR >= combLenR[c]) idxR = 0u;
            float outR = tank.combBufR[c][idxR];
            float lpR  = outR * (1.0f - combDamp[c]) + tank.combLpR[c] * combDamp[c];
            SanitizeFloatDspState(lpR);
            tank.combLpR[c] = lpR;
            float nextR = lateInR + lpR * combFbR[c];
            SanitizeFloatDspState(nextR);
            tank.combBufR[c][idxR] = nextR;
            if (++idxR >= combLenR[c]) idxR = 0u;
            tank.combIdxR[c] = idxR;
            combSumR += outR;
        }

        // 4. Cascaded Schroeder All-Pass Diffusers
        float diffL = combSumL;
        float diffR = combSumR;
        for (uint32_t a = 0u; a < 2u; ++a) {
            uint32_t aIdxL = tank.allpassIdxL[a];
            if (aIdxL >= kAllpassLenL[a]) aIdxL = 0u;
            float bufOutL = tank.allpassBufL[a][aIdxL];
            float newBufL = diffL + bufOutL * apGain;
            SanitizeFloatDspState(newBufL);
            tank.allpassBufL[a][aIdxL] = newBufL;
            diffL = bufOutL - diffL * apGain;
            if (++aIdxL >= kAllpassLenL[a]) aIdxL = 0u;
            tank.allpassIdxL[a] = aIdxL;

            uint32_t aIdxR = tank.allpassIdxR[a];
            if (aIdxR >= kAllpassLenR[a]) aIdxR = 0u;
            float bufOutR = tank.allpassBufR[a][aIdxR];
            float newBufR = diffR + bufOutR * apGain;
            SanitizeFloatDspState(newBufR);
            tank.allpassBufR[a][aIdxR] = newBufR;
            diffR = bufOutR - diffR * apGain;
            if (++aIdxR >= kAllpassLenR[a]) aIdxR = 0u;
            tank.allpassIdxR[a] = aIdxR;
        }

        float wetL = earlyL * tank.smoothedEarlyGain + diffL * tank.smoothedLateGain;
        float wetR = earlyR * tank.smoothedEarlyGain + diffR * tank.smoothedLateGain;
        ioMasterDryL[f] += wetL;
        ioMasterDryR[f] += wetR;

        float absWet = (fabsf(wetL) > fabsf(wetR)) ? fabsf(wetL) : fabsf(wetR);
        if (absWet > wetOutPeak) wetOutPeak = absWet;

        float absComb = (fabsf(combSumL) > fabsf(combSumR)) ? fabsf(combSumL) : fabsf(combSumR);
        float absDiff = (fabsf(diffL)    > fabsf(diffR))    ? fabsf(diffL)    : fabsf(diffR);
        float absInternal = (absComb > absDiff) ? absComb : absDiff;
        if (absInternal > tankStatePeak) tankStatePeak = absInternal;
    }

    if (inputPeak <= 1e-6f) {
        tank.silentFramesCount += numFrames;
        if (wetOutPeak <= 1e-6f && tankStatePeak <= 1e-6f) {
            tank.quietTankFramesCount += numFrames;
        } else {
            tank.quietTankFramesCount = 0u;
        }
    }
}

void ProcessGlobalI3DL2Reverb48kLocked(
    const float* wetSendL, const float* wetSendR,
    float* ioMasterDryL, float* ioMasterDryR, uint32_t numFrames)
{
    XBOX_DSI3DL2LISTENER snap = GetXboxI3DL2ListenerState();
    ProcessGlobalI3DL2Reverb48k(wetSendL, wetSendR, ioMasterDryL, ioMasterDryR, numFrames, &snap);
}

// ============================================================================
// Sample-Accurate MCPX LFO (1500 Hz) & Envelope Generator (93.75 Hz)
// ============================================================================

void EvaluateXboxVoiceLfoAndEgSamples(
    const XBOX_DSLFODESC lfo[2], const bool lfoActive[2],
    const XBOX_DSENVELOPEDESC eg[2], const bool egActive[2],
    uint64_t activeSamples48k, bool isReleasing, uint64_t releaseSamples48k,
    int32_t* outVolModMb, float* outPitchRatio, float* outFilterCutoffRatio,
    bool* outAmpReleaseDone)
{
    if (outVolModMb)          *outVolModMb          = 0;
    if (outPitchRatio)        *outPitchRatio        = 1.0f;
    if (outFilterCutoffRatio) *outFilterCutoffRatio = 1.0f;
    if (outAmpReleaseDone)    *outAmpReleaseDone    = false;

    // Xbox MCPX APU hardware clocks at 48,000 Hz output rate:
    //   - LFO steps every 32 samples: 48000 Hz / 32 = 1500.0 Hz.
    //   - EG steps every 512 samples: 48000 Hz / 512 = 93.75 Hz.
    float   lfoFrameCount = static_cast<float>(static_cast<double>(activeSamples48k) / 32.0);
    float   egStepCount   = static_cast<float>(static_cast<double>(activeSamples48k) / 512.0);
    float   relStepCount  = static_cast<float>(static_cast<double>(releaseSamples48k) / 512.0);
    float   pitchOctaves  = 0.0f;
    float   cutoffOctaves = 0.0f;
    int32_t volModMb      = 0;

    if (lfo && lfoActive) {
        for (int i = 0; i < 2; ++i) {
            if (!lfoActive[i]) continue;
            const XBOX_DSLFODESC& d = lfo[i];
            float delayFrames = static_cast<float>(d.dwDelay & 0x7FFFu);
            if (lfoFrameCount < delayFrames) continue;
            float activeFrames = lfoFrameCount - delayFrames;
            uint32_t delta = d.dwDelta & 0x3FFu;
            if (delta == 0u) continue;
            float phase = fmodf(activeFrames * (static_cast<float>(delta) / 65536.0f), 1.0f);
            float tri   = (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);

            if (i == 0 && d.lAmplitudeModulation != 0) {
                float ampNorm = static_cast<float>(d.lAmplitudeModulation) / 128.0f;
                volModMb += static_cast<int32_t>((tri - 1.0f) * 0.5f * fabsf(ampNorm) * 1200.0f);
            }
            if (d.lPitchModulation != 0) {
                pitchOctaves += tri * (static_cast<float>(d.lPitchModulation) / 128.0f);
            }
            if (i == 0 && d.lFilterCutOffRange != 0) {
                cutoffOctaves += tri * (static_cast<float>(d.lFilterCutOffRange) / 64.0f);
            }
        }
    }

    if (eg && egActive) {
        for (int i = 0; i < 2; ++i) {
            if (!egActive[i]) continue;
            const XBOX_DSENVELOPEDESC& e = eg[i];
            if (e.dwMode == 0u) continue; // DSEGMODE_DISABLE

            float envLevel    = 1.0f;
            float sustainNorm = static_cast<float>(e.dwSustain > 255u ? 255u : e.dwSustain) / 255.0f;
            uint32_t relStepsMax = e.dwRelease & 0xFFFu;

            if (isReleasing && relStepsMax > 0u) {
                float relFrac = 1.0f - (relStepCount / static_cast<float>(relStepsMax));
                if (relFrac <= 0.0f) {
                    relFrac = 0.0f;
                    if (i == 1 && outAmpReleaseDone) {
                        *outAmpReleaseDone = true;
                    }
                }
                envLevel = sustainNorm * relFrac;
            } else {
                float delaySteps  = static_cast<float>(e.dwDelay & 0xFFFu);
                float attackSteps = static_cast<float>(e.dwAttack & 0xFFFu);
                float holdSteps   = static_cast<float>(e.dwHold & 0xFFFu);
                float decaySteps  = static_cast<float>(e.dwDecay & 0xFFFu);

                if (egStepCount < delaySteps) {
                    envLevel = 0.0f;
                } else {
                    float t = egStepCount - delaySteps;
                    if (attackSteps > 0.0f && t < attackSteps) {
                        envLevel = t / attackSteps;
                    } else {
                        t -= attackSteps;
                        if (t < holdSteps) {
                            envLevel = 1.0f;
                        } else {
                            t -= holdSteps;
                            if (decaySteps > 0.0f && t < decaySteps) {
                                float decFrac = t / decaySteps;
                                envLevel = 1.0f - decFrac * (1.0f - sustainNorm);
                            } else {
                                envLevel = sustainNorm;
                            }
                        }
                    }
                }
            }

            if (i == 1) {
                // dwEG == 1 (DSEG_AMPLITUDE): modulates voice amplitude ONLY
                if (e.dwDelay > 0u || e.dwAttack > 0u || e.dwDecay > 0u || e.dwSustain < 255u || isReleasing) {
                    if (envLevel <= 0.0001f) {
                        volModMb -= 10000;
                    } else {
                        int32_t envMb = static_cast<int32_t>(2000.0f * log10f(envLevel));
                        volModMb += envMb;
                    }
                }
            } else {
                // dwEG == 0 (DSEG_MULTI): modulates pitch & filter cutoff ONLY
                if (e.lPitchScale != 0) {
                    pitchOctaves += (envLevel - 1.0f) * (static_cast<float>(e.lPitchScale) / 128.0f);
                }
                if (e.lFilterCutOff != 0) {
                    cutoffOctaves += (envLevel - 1.0f) * (static_cast<float>(e.lFilterCutOff) / 64.0f);
                }
            }
        }
    }

    if (outVolModMb) {
        if (volModMb < -10000) volModMb = -10000;
        if (volModMb >  2400)  volModMb =  2400;
        *outVolModMb = volModMb;
    }
    if (outPitchRatio) {
        if (pitchOctaves < -4.0f) pitchOctaves = -4.0f;
        if (pitchOctaves >  4.0f) pitchOctaves =  4.0f;
        *outPitchRatio = powf(2.0f, pitchOctaves);
    }
    if (outFilterCutoffRatio) {
        if (cutoffOctaves < -4.0f) cutoffOctaves = -4.0f;
        if (cutoffOctaves >  4.0f) cutoffOctaves =  4.0f;
        *outFilterCutoffRatio = powf(2.0f, cutoffOctaves);
    }
}

void EvaluateXboxVoiceLfoAndEg(
    const XBOX_DSLFODESC lfo[2], const bool lfoActive[2],
    const XBOX_DSENVELOPEDESC eg[2], const bool egActive[2],
    uint32_t elapsedMs, bool isReleasing, uint32_t releaseElapsedMs,
    int32_t* outVolModMb, float* outPitchRatio, float* outFilterCutoffRatio)
{
    uint64_t activeSamples48k  = static_cast<uint64_t>(elapsedMs) * 48ULL;
    uint64_t releaseSamples48k = static_cast<uint64_t>(releaseElapsedMs) * 48ULL;
    EvaluateXboxVoiceLfoAndEgSamples(
        lfo, lfoActive, eg, egActive,
        activeSamples48k, isReleasing, releaseSamples48k,
        outVolModMb, outPitchRatio, outFilterCutoffRatio, nullptr);
}

// ============================================================================
// Xbox IMA ADPCM & N-Channel PCM Stereo Folddown Decoders
// ============================================================================

static const int kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t DecodeImaNibble(uint8_t nibble, int& predictor, int& stepIndex) {
    int step = kImaStepTable[stepIndex];
    int diff = step >> 3;
    if (nibble & 1u) diff += step >> 2;
    if (nibble & 2u) diff += step >> 1;
    if (nibble & 4u) diff += step;
    if (nibble & 8u) predictor -= diff;
    else             predictor += diff;
    if (predictor > 32767)  predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    stepIndex += kImaIndexTable[nibble & 0x0Fu];
    if (stepIndex < 0)  stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    return static_cast<int16_t>(predictor);
}

static inline void FoldMultichannelFloatToStereo(
    const float* chSamples, uint32_t channels, float* outL, float* outR)
{
    if (channels <= 1u) {
        *outL = chSamples[0];
        *outR = chSamples[0];
    } else if (channels == 2u) {
        *outL = chSamples[0];
        *outR = chSamples[1];
    } else if (channels == 3u) {
        float c = chSamples[2] * 0.70710678f;
        *outL = chSamples[0] + c;
        *outR = chSamples[1] + c;
    } else if (channels == 4u) {
        *outL = chSamples[0] + chSamples[2] * 0.70710678f;
        *outR = chSamples[1] + chSamples[3] * 0.70710678f;
    } else if (channels == 5u) {
        float c = chSamples[2] * 0.70710678f;
        *outL = chSamples[0] + c + chSamples[3] * 0.70710678f;
        *outR = chSamples[1] + c + chSamples[4] * 0.70710678f;
    } else {
        // 5.1 surround: FL, FR, FC, LFE, BL, BR
        float c   = chSamples[2] * 0.70710678f;
        float lfe = chSamples[3] * 0.50f;
        *outL = chSamples[0] + c + lfe + chSamples[4] * 0.70710678f;
        *outR = chSamples[1] + c + lfe + chSamples[5] * 0.70710678f;
    }
}

void DecodeXboxAdpcmBlockToStereoFloat(
    const uint8_t* blockPtr, uint32_t channels, float outLeft[64], float outRight[64])
{
    if (!blockPtr || !outLeft || !outRight) return;
    if (channels == 0u) channels = 1u;
    uint32_t decCh = (channels > 6u) ? 6u : channels;

    int pred[6]    = {};
    int stepIdx[6] = {};
    float chNorm[6] = {};
    constexpr float kInv32768 = 1.0f / 32768.0f;

    for (uint32_t ch = 0u; ch < decCh; ++ch) {
        pred[ch]    = static_cast<int>(*reinterpret_cast<const int16_t*>(blockPtr + ch * 4u));
        stepIdx[ch] = static_cast<int>(blockPtr[ch * 4u + 2u]);
        if (stepIdx[ch] < 0)  stepIdx[ch] = 0;
        if (stepIdx[ch] > 88) stepIdx[ch] = 88;
        chNorm[ch]  = static_cast<float>(pred[ch]) * kInv32768;
    }
    FoldMultichannelFloatToStereo(chNorm, decCh, &outLeft[0], &outRight[0]);

    const uint8_t* dataPtr = blockPtr + 4u * channels;
    uint32_t frameIdx = 1u;
    for (uint32_t sub = 0u; sub < 8u; ++sub) {
        uint32_t words[6] = {};
        for (uint32_t ch = 0u; ch < decCh; ++ch) {
            words[ch] = *reinterpret_cast<const uint32_t*>(dataPtr + (sub * channels + ch) * 4u);
        }
        uint32_t nibblesInSubBlock = (sub == 7u) ? 7u : 8u;
        for (uint32_t s = 0u; s < nibblesInSubBlock; ++s) {
            for (uint32_t ch = 0u; ch < decCh; ++ch) {
                uint8_t nib = static_cast<uint8_t>((words[ch] >> (s * 4u)) & 0x0Fu);
                int16_t smp = DecodeImaNibble(nib, pred[ch], stepIdx[ch]);
                chNorm[ch]  = static_cast<float>(smp) * kInv32768;
            }
            FoldMultichannelFloatToStereo(chNorm, decCh, &outLeft[frameIdx], &outRight[frameIdx]);
            ++frameIdx;
        }
    }
}

void SamplePcmFrameToStereoFloat(
    const uint8_t* rawBase, uint32_t frameIdx, uint32_t channels, uint16_t bitsPerSample,
    float* outLeft, float* outRight)
{
    if (!rawBase || !outLeft || !outRight || channels == 0u) {
        if (outLeft)  *outLeft  = 0.0f;
        if (outRight) *outRight = 0.0f;
        return;
    }
    uint32_t decCh = (channels > 6u) ? 6u : channels;
    float chNorm[6] = {};
    constexpr float kInv32768 = 1.0f / 32768.0f;

    if (bitsPerSample == 8u) {
        const uint8_t* p = rawBase + static_cast<size_t>(frameIdx) * channels;
        for (uint32_t ch = 0u; ch < decCh; ++ch) {
            chNorm[ch] = (static_cast<float>(static_cast<int32_t>(p[ch]) - 128)) / 128.0f;
        }
    } else {
        const int16_t* p = reinterpret_cast<const int16_t*>(rawBase) + static_cast<size_t>(frameIdx) * channels;
        for (uint32_t ch = 0u; ch < decCh; ++ch) {
            chNorm[ch] = static_cast<float>(p[ch]) * kInv32768;
        }
    }
    FoldMultichannelFloatToStereo(chNorm, decCh, outLeft, outRight);
}

void UnpackXboxWaveToStereoPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels,
    bool isAdpcm, uint16_t bitsPerSample, int16_t* dstStereo, uint32_t maxFrames, uint32_t* outFrames)
{
    if (outFrames) *outFrames = 0u;
    if (!src || srcBytes == 0u || !dstStereo || maxFrames == 0u) return;
    if (channels == 0u) channels = 1u;

    if (isAdpcm) {
        uint32_t blockAlign = 36u * channels;
        uint32_t numBlocks  = srcBytes / blockAlign;
        uint32_t totalFrames = numBlocks * 64u;
        if (totalFrames > maxFrames) {
            numBlocks   = maxFrames / 64u;
            totalFrames = numBlocks * 64u;
        }
        float blockL[64];
        float blockR[64];
        for (uint32_t b = 0u; b < numBlocks; ++b) {
            DecodeXboxAdpcmBlockToStereoFloat(src + b * blockAlign, channels, blockL, blockR);
            int16_t* outPtr = dstStereo + static_cast<size_t>(b) * 128u;
            for (uint32_t f = 0u; f < 64u; ++f) {
                int32_t sL = static_cast<int32_t>(blockL[f] * 32768.0f);
                int32_t sR = static_cast<int32_t>(blockR[f] * 32768.0f);
                if (sL >  32767) sL =  32767;
                if (sL < -32768) sL = -32768;
                if (sR >  32767) sR =  32767;
                if (sR < -32768) sR = -32768;
                outPtr[f * 2u + 0u] = static_cast<int16_t>(sL);
                outPtr[f * 2u + 1u] = static_cast<int16_t>(sR);
            }
        }
        if (outFrames) *outFrames = totalFrames;
    } else {
        uint32_t bytesPerSample = (bitsPerSample == 8u) ? 1u : 2u;
        uint32_t frameBytes     = channels * bytesPerSample;
        if (frameBytes == 0u) return;
        uint32_t totalFrames    = srcBytes / frameBytes;
        if (totalFrames > maxFrames) totalFrames = maxFrames;

        if (bitsPerSample == 16u && channels == 2u) {
            memcpy(dstStereo, src, static_cast<size_t>(totalFrames) * 4u);
        } else if (bitsPerSample == 16u && channels == 1u) {
            const int16_t* in16 = reinterpret_cast<const int16_t*>(src);
            for (uint32_t f = 0u; f < totalFrames; ++f) {
                int16_t v = in16[f];
                dstStereo[f * 2u + 0u] = v;
                dstStereo[f * 2u + 1u] = v;
            }
        } else {
            for (uint32_t f = 0u; f < totalFrames; ++f) {
                float fL = 0.0f, fR = 0.0f;
                SamplePcmFrameToStereoFloat(src, f, channels, bitsPerSample, &fL, &fR);
                int32_t sL = static_cast<int32_t>(fL * 32768.0f);
                int32_t sR = static_cast<int32_t>(fR * 32768.0f);
                if (sL >  32767) sL =  32767;
                if (sL < -32768) sL = -32768;
                if (sR >  32767) sR =  32767;
                if (sR < -32768) sR = -32768;
                dstStereo[f * 2u + 0u] = static_cast<int16_t>(sL);
                dstStereo[f * 2u + 1u] = static_cast<int16_t>(sR);
            }
        }
        if (outFrames) *outFrames = totalFrames;
    }
}
