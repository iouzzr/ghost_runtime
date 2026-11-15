#include "profiler.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <algorithm>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Global Profiler State & Synchronization
// ============================================================================

GhostActiveFrameAccum g_perfAccum        = {};
volatile bool         g_osdShowPerfPanel = false;

static SRWLOCK                    g_profSnapshotLock        = SRWLOCK_INIT;
static SRWLOCK                    g_profDumpFileLock        = SRWLOCK_INIT;
static GhostFrameSnapshot         g_frameRing[kProfileFrameRingSize] = {};
static uint32_t                   g_frameRingHead           = 0u; // Index of oldest when full
static uint32_t                   g_frameRingCount          = 0u; // 0..kProfileFrameRingSize
static XboxNativeProfilerSnapshot g_latchedNativeProfiler   = {};
static uint32_t                   g_lastEndFrameHookSwap    = 0u;
static bool                       g_hasPreResetNativeLatch  = false;

// Game-thread scratch buffer for sorting active VS/FVF handles without large stack frames
static PerfVsHandleStat           s_sortVsScratch[kMaxTrackedVsHandles] = {};

// Paired key-down tracking for VK_F3 / VK_F10 so forwarded Alt/bound combos never stick down
static bool                       s_f3ForwardedDown         = false;
static bool                       s_f10ForwardedDown        = false;

// Previous frame's cumulative diagnostic counters for per-frame delta computation
static bool     g_hasPrevDiagTotals       = false;
static int64_t  g_lastSwapEndQpc          = 0;
static uint64_t g_prevStaticVbHits        = 0ULL;
static uint64_t g_prevStaticIbHits        = 0ULL;
static uint64_t g_prevStaticPbHits        = 0ULL;
static uint64_t g_prevPsPointerHits       = 0ULL;
static uint64_t g_prevBackBufferCopies    = 0ULL;
static uint64_t g_prevSurfaceReadbacks    = 0ULL;

// F10 dump sequence & on-screen confirmation banner state
static volatile LONG g_dumpSnapshotSeq    = 0;
static SRWLOCK       g_toastLock          = SRWLOCK_INIT;
static DWORD         g_toastExpireTick    = 0u;
static wchar_t       g_toastMessageW[192] = {};

// ============================================================================
// High-Resolution QPC Conversion & Safe Memory Probing
// ============================================================================

static int64_t GetHostQpcFrequency() {
    static int64_t s_freq = 0;
    if (s_freq == 0) {
        LARGE_INTEGER f = {};
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
            s_freq = f.QuadPart;
        } else {
            s_freq = 10000000LL;
        }
    }
    return s_freq;
}

double PerfQpcTicksToMs(int64_t ticks) {
    if (ticks <= 0) return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(GetHostQpcFrequency());
}

static bool IsReadableHostMemoryRange(const void* ptr, size_t byteLen) {
    if (!ptr || byteLen == 0u) return false;
    uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
    if (cur < kMinValidHostAddress) return false;
    if (UINTPTR_MAX - cur < byteLen) return false;
    uintptr_t end = cur + byteLen;

    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD prot = mbi.Protect;
        if ((prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0u) {
            return false;
        }
        const DWORD readableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((prot & readableMask) == 0u) {
            return false;
        }
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (UINTPTR_MAX - regionBase < mbi.RegionSize) {
            cur = end;
        } else {
            uintptr_t regionEnd = regionBase + mbi.RegionSize;
            if (regionEnd <= cur) return false;
            cur = regionEnd;
        }
    }
    return true;
}

// ============================================================================
// Per-Draw Vertex Shader / FVF Handle Tracking (Lock-Free on Game Thread)
// ============================================================================

void RecordPerfVsDraw(
    uint32_t vsHandle, uint32_t stride, int32_t vsType, bool isXvu,
    uint32_t verts, bool staticVbHit, int64_t cpuTicks)
{
    uint32_t hashSlot = ((vsHandle * 2654435761u) ^ (stride * 40503u) ^ (isXvu ? 0x9E3779B9u : 0u)) &
                        (kMaxTrackedVsHandles - 1u);
    PerfVsHandleStat* target = nullptr;

    for (uint32_t probe = 0u; probe < kMaxTrackedVsHandles; ++probe) {
        uint32_t idx = (hashSlot + probe) & (kMaxTrackedVsHandles - 1u);
        PerfVsHandleStat& e = g_perfAccum.vsTable[idx];
        if (!e.used) {
            e.used         = true;
            e.handle       = vsHandle;
            e.stride       = stride;
            e.vsType       = vsType;
            e.isXvu        = isXvu;
            e.draws        = 0u;
            e.verts        = 0u;
            e.staticVbHits = 0u;
            e.cpuTicks     = 0;
            e.cpuMs        = 0.0f;
            target         = &e;
            break;
        }
        if (e.handle == vsHandle && e.stride == stride && e.isXvu == isXvu) {
            target = &e;
            break;
        }
    }

    if (!target) {
        // Never alias or corrupt an unrelated vsTable slot on overflow (>512 distinct tuples)
        g_perfAccum.vsOverflowDraws        += 1u;
        g_perfAccum.vsOverflowVerts        += verts;
        g_perfAccum.vsOverflowStaticVbHits += staticVbHit ? 1u : 0u;
        if (cpuTicks > 0) {
            g_perfAccum.vsOverflowTicks += cpuTicks;
        }
        return;
    }

    target->draws        += 1u;
    target->verts        += verts;
    target->staticVbHits += staticVbHit ? 1u : 0u;
    if (cpuTicks > 0) {
        target->cpuTicks += cpuTicks;
    }
}

// ============================================================================
// Ghost.exe Native cProfiler Reader (stdProfiler_pProfile at 0x00880438)
// ============================================================================

void SnapshotXboxNativeProfilerFromPtr(const void* profilerPtr, XboxNativeProfilerSnapshot* outSnap) {
    if (!outSnap) return;
    memset(outSnap, 0, sizeof(*outSnap));

    if (!IsReadableHostMemoryRange(profilerPtr, sizeof(XboxProfilerObj))) {
        return;
    }

    const XboxProfilerObj* prof = reinterpret_cast<const XboxProfilerObj*>(profilerPtr);
    if (prof->m_count <= 0 || prof->m_count > static_cast<int32_t>(kMaxXboxProfileVars)) {
        return;
    }
    const uint32_t count = static_cast<uint32_t>(prof->m_count);
    if (!IsReadableHostMemoryRange(prof->m_vars, count * sizeof(XboxProfileVar*))) {
        return;
    }

    uint32_t validVars = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        const XboxProfileVar* v = prof->m_vars[i];
        if (!IsReadableHostMemoryRange(v, sizeof(XboxProfileVar))) {
            continue;
        }

        XboxNativeVarSnapshot& dst = outSnap->vars[i];
        dst.present = true;
        memcpy(dst.name, v->name, 31u);
        dst.name[31] = '\0';
        dst.valueType     = v->valueType;
        dst.resetPerFrame = v->resetPerFrame;

        uint32_t sc   = (v->count <= 80u) ? v->count : 80u;
        uint32_t head = v->head % 80u;
        dst.sampleCount = sc;

        if (v->valueType == 3u) {
            // String variable ("Notes"): samples[0] is char* buffer (128 bytes allocated in AddVariable)
            const char* notePtr = reinterpret_cast<const char*>(static_cast<uintptr_t>(v->samples[0]));
            if (IsReadableHostMemoryRange(notePtr, 1u)) {
                size_t pos = 0u;
                while (pos < sizeof(dst.noteText) - 1u) {
                    if (!IsReadableHostMemoryRange(notePtr + pos, 1u)) break;
                    char c = notePtr[pos];
                    if (c == '\0') break;
                    dst.noteText[pos++] = (c >= 32 && c < 127) ? c : '?';
                }
                dst.noteText[pos] = '\0';
            }
            ++validVars;
            continue;
        }

        if (sc > 0u && v->valueType <= 2u) {
            auto readSlot = [&](uint32_t ringIdx) -> double {
                uint32_t raw = v->samples[ringIdx % 80u];
                if (v->valueType == 0u) {
                    // cProfiler::EndTimer (0x0042626E) multiplies elapsed ms by 0.001f (0x3A83126F),
                    // storing seconds in samples[]; multiply by 1000.0 to restore milliseconds!
                    float sec = 0.0f;
                    memcpy(&sec, &raw, sizeof(sec));
                    return isfinite(sec) ? (static_cast<double>(sec) * 1000.0) : 0.0;
                }
                if (v->valueType == 1u) {
                    return static_cast<double>(static_cast<int32_t>(raw));
                }
                return static_cast<double>(raw);
            };

            uint32_t latestIdx = (head + sc - 1u) % 80u;
            dst.latestVal = readSlot(latestIdx);
            dst.prevVal   = (sc >= 2u) ? readSlot((head + sc - 2u) % 80u) : 0.0;

            double sum = 0.0;
            double mx  = -1e30;
            for (uint32_t k = 0u; k < sc; ++k) {
                double sv = readSlot((head + k) % 80u);
                sum += sv;
                if (k == 0u || sv > mx) mx = sv;
            }
            dst.avgVal = sum / static_cast<double>(sc);
            dst.maxVal = (mx > -1e29) ? mx : 0.0;
        }

        ++validVars;
    }

    if (validVars == 0u) {
        return;
    }

    outSnap->valid             = true;
    outSnap->capturedPreReset  = false;
    outSnap->varCount          = count;
    outSnap->swapFrameCaptured = g_swapFrameCount;

    // Unpack eProfileVars 0x0A ("Physics Time"):
    // tSequence::MainLoop calls SetFValue(0x0A, ...) TWICE per frame:
    //   1) 0x0043C6EB after simPhysics_Update() -> lands in slot (N - 2)
    //   2) 0x0043C7B9 after simPhysics_UpdateConstraintWorld() -> lands in slot (N - 1)
    if (count > 0x0Au && outSnap->vars[0x0Au].present && outSnap->vars[0x0Au].valueType == 0u) {
        const XboxNativeVarSnapshot& phys = outSnap->vars[0x0Au];
        if (phys.sampleCount >= 2u) {
            outSnap->physicsSimUpdateMs  = phys.prevVal;
            outSnap->physicsConstraintMs = phys.latestVal;
            outSnap->physicsCombinedMs   = phys.prevVal + phys.latestVal;
        } else if (phys.sampleCount == 1u) {
            outSnap->physicsSimUpdateMs  = phys.latestVal;
            outSnap->physicsConstraintMs = 0.0;
            outSnap->physicsCombinedMs   = phys.latestVal;
        }
    }
}

void __fastcall Bridge_ProfilerEndFrame(void* thisPtr, void* edxUnused) {
    (void)edxUnused;
    XboxNativeProfilerSnapshot snap = {};
    const void* profPtr = thisPtr;
    if (!profPtr && IsReadableHostMemoryRange(reinterpret_cast<const void*>(ADDR_STD_PROFILER_PTR), sizeof(uint32_t))) {
        profPtr = *reinterpret_cast<const void* const*>(ADDR_STD_PROFILER_PTR);
    }
    SnapshotXboxNativeProfilerFromPtr(profPtr, &snap);
    if (snap.valid) {
        snap.capturedPreReset = true;
        AcquireSRWLockExclusive(&g_profSnapshotLock);
        g_latchedNativeProfiler  = snap;
        g_lastEndFrameHookSwap   = g_swapFrameCount;
        g_hasPreResetNativeLatch = true;
        ReleaseSRWLockExclusive(&g_profSnapshotLock);
    }

    typedef void (__thiscall *PFN_ProfilerEndFrame)(void* self);
    reinterpret_cast<PFN_ProfilerEndFrame>(ADDR_PROFILER_ENDFRAME_FN)(thisPtr);
}

// ============================================================================
// Non-Blocking 6-Slot D3D11 GPU Frame Timestamp Query Ring
// ============================================================================

static constexpr uint32_t kGpuTimestampRingSlots = 6u;
static constexpr uint32_t kMaxPendingGpuHarvest  = 16u;

struct GpuFrameTimestampSlot {
    ID3D11Query* disjointQuery;
    ID3D11Query* beginTsQuery;
    ID3D11Query* endTsQuery;
    uint32_t     swapFrame;
    bool         inFlight;
};

struct HarvestedGpuFrameSample {
    uint32_t swapFrame;
    float    gpuMs;
};

static GpuFrameTimestampSlot   s_gpuTsRing[kGpuTimestampRingSlots]       = {};
static ID3D11Device*           s_gpuTsDevice                             = nullptr;
static uint32_t                s_gpuTsWriteIdx                           = 0u;
static bool                    s_gpuTsFrameBracketOpen                   = false;
static uint32_t                s_gpuTsDisjointTotal                      = 0u;
static HarvestedGpuFrameSample s_pendingGpuHarvest[kMaxPendingGpuHarvest] = {};
static uint32_t                s_pendingGpuHarvestCount                  = 0u;
static bool                    s_gpuPolledSinceLastLatch                 = false;

void ReleaseGpuTimestampQueries() {
    for (uint32_t i = 0u; i < kGpuTimestampRingSlots; ++i) {
        if (s_gpuTsRing[i].disjointQuery) { s_gpuTsRing[i].disjointQuery->Release(); s_gpuTsRing[i].disjointQuery = nullptr; }
        if (s_gpuTsRing[i].beginTsQuery)  { s_gpuTsRing[i].beginTsQuery->Release();  s_gpuTsRing[i].beginTsQuery  = nullptr; }
        if (s_gpuTsRing[i].endTsQuery)    { s_gpuTsRing[i].endTsQuery->Release();    s_gpuTsRing[i].endTsQuery    = nullptr; }
        s_gpuTsRing[i].swapFrame = 0u;
        s_gpuTsRing[i].inFlight  = false;
    }
    s_gpuTsDevice             = nullptr;
    s_gpuTsWriteIdx           = 0u;
    s_gpuTsFrameBracketOpen   = false;
    s_gpuTsDisjointTotal      = 0u;
    s_pendingGpuHarvestCount  = 0u;
    s_gpuPolledSinceLastLatch = false;
}

static void SyncGpuTimestampRingDevice() {
    if (s_gpuTsDevice != g_d3dDevice) {
        ReleaseGpuTimestampQueries();
        s_gpuTsDevice = g_d3dDevice;
    }
}

static void PushHarvestedGpuSample(uint32_t swapFrame, float gpuMs) {
    if (s_pendingGpuHarvestCount < kMaxPendingGpuHarvest) {
        s_pendingGpuHarvest[s_pendingGpuHarvestCount++] = { swapFrame, gpuMs };
    } else {
        memmove(
            &s_pendingGpuHarvest[0],
            &s_pendingGpuHarvest[1],
            (kMaxPendingGpuHarvest - 1u) * sizeof(HarvestedGpuFrameSample));
        s_pendingGpuHarvest[kMaxPendingGpuHarvest - 1u] = { swapFrame, gpuMs };
    }
}

static bool TryHarvestGpuTimestampSlot(GpuFrameTimestampSlot& slot, bool* outValidSample, float* outSampleMs) {
    if (outValidSample) *outValidSample = false;
    if (outSampleMs)    *outSampleMs    = 0.0f;
    if (!slot.inFlight || !g_d3dCtx || !slot.disjointQuery || !slot.beginTsQuery || !slot.endTsQuery) {
        return false;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disj = {};
    HRESULT hrDisj = g_d3dCtx->GetData(slot.disjointQuery, &disj, sizeof(disj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hrDisj == S_FALSE) return false;

    uint64_t tBegin = 0ULL;
    HRESULT hrBeg = g_d3dCtx->GetData(slot.beginTsQuery, &tBegin, sizeof(tBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hrBeg == S_FALSE) return false;

    uint64_t tEnd = 0ULL;
    HRESULT hrEnd = g_d3dCtx->GetData(slot.endTsQuery, &tEnd, sizeof(tEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hrEnd == S_FALSE) return false;

    const uint32_t slotSwapFrame = slot.swapFrame;
    slot.inFlight  = false;
    slot.swapFrame = 0u;

    if (hrDisj == S_OK && hrBeg == S_OK && hrEnd == S_OK) {
        if (!disj.Disjoint && disj.Frequency > 0ULL && tEnd >= tBegin) {
            float ms = static_cast<float>(
                static_cast<double>(tEnd - tBegin) * 1000.0 / static_cast<double>(disj.Frequency));
            PushHarvestedGpuSample(slotSwapFrame, ms);
            if (outValidSample) *outValidSample = true;
            if (outSampleMs)    *outSampleMs    = ms;
        } else if (disj.Disjoint) {
            ++s_gpuTsDisjointTotal;
        }
    }
    return true;
}

void BeginGpuFrameTimestampIfReady() {
    if (!g_d3dDevice || !g_d3dCtx) return;
    SyncGpuTimestampRingDevice();
    if (g_lastSwapEndQpc == 0) {
        g_lastSwapEndQpc = PerfQpcNow();
    }
    if (s_gpuTsFrameBracketOpen) return;

    GpuFrameTimestampSlot& slot = s_gpuTsRing[s_gpuTsWriteIdx];
    if (slot.inFlight && !TryHarvestGpuTimestampSlot(slot, nullptr, nullptr)) {
        // Never flush or stall if GPU is >= 6 frames behind; skip bracketing this frame
        return;
    }
    if (!slot.disjointQuery) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0u };
        if (FAILED(g_d3dDevice->CreateQuery(&qd, &slot.disjointQuery))) return;
    }
    if (!slot.beginTsQuery) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0u };
        if (FAILED(g_d3dDevice->CreateQuery(&qd, &slot.beginTsQuery))) return;
    }
    if (!slot.endTsQuery) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0u };
        if (FAILED(g_d3dDevice->CreateQuery(&qd, &slot.endTsQuery))) return;
    }

    g_d3dCtx->Begin(slot.disjointQuery);
    g_d3dCtx->End(slot.beginTsQuery);
    s_gpuTsFrameBracketOpen = true;
}

void EndAndPollGpuFrameTimestamp(float* outGpuMs, bool* outValid, uint32_t* outDisjointCount) {
    SyncGpuTimestampRingDevice();
    s_gpuPolledSinceLastLatch = true;

    if (s_gpuTsFrameBracketOpen && g_d3dCtx) {
        GpuFrameTimestampSlot& slot = s_gpuTsRing[s_gpuTsWriteIdx];
        if (slot.endTsQuery && slot.disjointQuery) {
            g_d3dCtx->End(slot.endTsQuery);
            g_d3dCtx->End(slot.disjointQuery);
            slot.swapFrame = g_swapFrameCount;
            slot.inFlight  = true;
        }
        s_gpuTsFrameBracketOpen = false;
        s_gpuTsWriteIdx = (s_gpuTsWriteIdx + 1u) % kGpuTimestampRingSlots;
    }

    bool  anyValidThisCall = false;
    float latestMsThisCall = 0.0f;
    for (uint32_t k = 0u; k < kGpuTimestampRingSlots; ++k) {
        uint32_t idx = (s_gpuTsWriteIdx + k) % kGpuTimestampRingSlots;
        if (s_gpuTsRing[idx].inFlight) {
            bool  slotValid = false;
            float slotMs    = 0.0f;
            TryHarvestGpuTimestampSlot(s_gpuTsRing[idx], &slotValid, &slotMs);
            if (slotValid) {
                anyValidThisCall = true;
                latestMsThisCall = slotMs;
            }
        }
    }

    if (outGpuMs)         *outGpuMs         = anyValidThisCall ? latestMsThisCall : 0.0f;
    if (outValid)         *outValid         = anyValidThisCall;
    if (outDisjointCount) *outDisjointCount = s_gpuTsDisjointTotal;
}

// ============================================================================
// Per-Frame Host Latch (Called at End of Finishing Bridge_D3DDevice_Swap)
// ============================================================================

void EndHostProfileFrameAndLatch(float gpuFrameMs, bool gpuQueryValid, uint32_t gpuDisjointCount) {
    int64_t nowQpc       = PerfQpcNow();
    bool    hasValidWall = (g_lastSwapEndQpc != 0 && nowQpc > g_lastSwapEndQpc);
    double  wallMs       = hasValidWall ? PerfQpcTicksToMs(nowQpc - g_lastSwapEndQpc) : 0.0;
    g_lastSwapEndQpc     = nowQpc;

    GhostFrameSnapshot snap = {};
    snap.swapFrame            = g_swapFrameCount;
    snap.wallTimeValid        = hasValidWall;
    snap.frameWallMs          = static_cast<float>(wallMs);
    snap.presentMs            = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.presentTicks));
    snap.paceWaitMs           = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.paceWaitTicks));
    double activeMs           = hasValidWall
                              ? (wallMs - static_cast<double>(snap.presentMs) - static_cast<double>(snap.paceWaitMs))
                              : 0.0;
    snap.cpuActiveMs          = static_cast<float>((activeMs > 0.0) ? activeMs : 0.0);
    if (s_gpuPolledSinceLastLatch) {
        snap.gpuFrameMs    = 0.0f;
        snap.gpuQueryValid = false;
    } else {
        snap.gpuFrameMs    = gpuQueryValid ? gpuFrameMs : 0.0f;
        snap.gpuQueryValid = gpuQueryValid;
    }
    snap.gpuDisjointCount     = gpuDisjointCount;

    snap.compositeOsdMs       = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.compositeOsdTicks));
    snap.decodeUnifiedMs      = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.decodeUnifiedTicks));
    snap.submitUnifiedMs      = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.submitUnifiedTicks));
    snap.submitXvuMs          = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.submitXvuTicks));
    snap.runPushBufferTotalMs = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.runPushBufferTotalTicks));
    int64_t pbSelfTicks       = g_perfAccum.runPushBufferTotalTicks - g_perfAccum.runPushBufferNestedTicks;
    snap.runPushBufferSelfMs  = static_cast<float>(PerfQpcTicksToMs(pbSelfTicks > 0 ? pbSelfTicks : 0));
    snap.srvResolveMs         = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.srvResolveTicks));
    snap.texUploadMs          = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.texUploadTicks));
    snap.gpuReadbackMs        = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.gpuReadbackTicks));
    snap.visWaitMs            = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.visWaitTicks));
    snap.skinSetupMs          = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.skinSetupTicks));
    snap.shaderCompileMs      = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.shaderCompileTicks));
    snap.hostBridgeCpuMs      = snap.decodeUnifiedMs + snap.submitUnifiedMs + snap.submitXvuMs +
                                snap.runPushBufferSelfMs + snap.skinSetupMs + snap.visWaitMs;

    snap.xvuDraws                = g_perfAccum.xvuDraws;
    snap.xvuVerts                = g_perfAccum.xvuVerts;
    snap.xvuStaticVbDraws        = g_perfAccum.xvuStaticVbDraws;
    snap.xvuDynamicSliceDraws    = g_perfAccum.xvuDynamicSliceDraws;
    snap.unifiedDraws            = g_perfAccum.unifiedDraws;
    snap.unifiedVerts            = g_perfAccum.unifiedVerts;
    snap.unifiedRhwDraws         = g_perfAccum.unifiedRhwDraws;
    snap.unifiedRhwVerts         = g_perfAccum.unifiedRhwVerts;
    snap.unifiedFvf0x152Draws    = g_perfAccum.unifiedFvf0x152Draws;
    snap.unifiedFvf0x152Verts    = g_perfAccum.unifiedFvf0x152Verts;
    snap.unifiedFvf0x112Draws    = g_perfAccum.unifiedFvf0x112Draws;
    snap.unifiedFvf0x112Verts    = g_perfAccum.unifiedFvf0x112Verts;
    snap.unifiedOtherDraws       = g_perfAccum.unifiedOtherDraws;
    snap.unifiedOtherVerts       = g_perfAccum.unifiedOtherVerts;
    snap.totalDraws              = snap.xvuDraws + snap.unifiedDraws;
    snap.totalVerts              = snap.xvuVerts + snap.unifiedVerts;

    snap.unifiedGpuLitDraws      = g_perfAccum.unifiedGpuLitDraws;
    snap.unifiedCpuLitDraws      = g_perfAccum.unifiedCpuLitDraws;
    snap.unifiedGpuSkinDraws     = g_perfAccum.unifiedGpuSkinDraws;
    snap.unifiedCpuSkinDraws     = g_perfAccum.unifiedCpuSkinDraws;
    snap.unifiedPointSpriteDraws = g_perfAccum.unifiedPointSpriteDraws;
    snap.unifiedPointSpriteVerts = g_perfAccum.unifiedPointSpriteVerts;

    snap.pushBufferCalls         = g_perfAccum.pushBufferCalls;
    snap.pushBufferFixupCalls    = g_perfAccum.pushBufferFixupCalls;
    snap.pushBufferDynRingCalls  = g_perfAccum.pushBufferDynRingCalls;
    snap.pushBufferBytesParsed   = g_perfAccum.pushBufferBytesParsed;

    snap.skinSetupCalls          = g_perfAccum.skinSetupCalls;
    snap.skinSetupVerts          = g_perfAccum.skinSetupVerts;

    snap.rtSwitchCount           = g_perfAccum.rtSwitchCount;
    snap.clearFastCount          = g_perfAccum.clearFastCount;
    snap.clearQuadCount          = g_perfAccum.clearQuadCount;
    snap.srvResolveCalls         = g_perfAccum.srvResolveCalls;
    snap.texUploadCount          = g_perfAccum.texUploadCount;
    snap.texUploadBytes          = g_perfAccum.texUploadBytes;
    snap.shaderRuntimeCompiles   = g_perfAccum.shaderRuntimeCompiles;

    snap.dynVbDiscardCount       = g_perfAccum.dynVbDiscardCount;
    snap.dynStreamVbDiscardCount = g_perfAccum.dynStreamVbDiscardCount;
    snap.dynIbDiscardCount       = g_perfAccum.dynIbDiscardCount;

    snap.visBeginCount           = g_perfAccum.visBeginCount;
    snap.visGetResultCount       = g_perfAccum.visGetResultCount;
    snap.visWaitCallCount        = g_perfAccum.visWaitCallCount;
    snap.visIncompleteCount      = g_perfAccum.visIncompleteCount;
    snap.visSpinCount            = g_perfAccum.visSpinCount;

    uint64_t curVbHits  = GetStaticVbCacheHitCount();
    uint64_t curIbHits  = GetStaticIbCacheHitCount();
    uint64_t curPbHits  = GetStaticPushBufferHitCount();
    uint64_t curPsHits  = GetPsPointerCacheHitCount();
    uint64_t curBbCopy  = GetBackBufferCopyCount();
    uint64_t curSurfRbk = GetSurfaceGpuReadbackCount();

    if (g_hasPrevDiagTotals) {
        snap.staticVbHits     = (curVbHits  >= g_prevStaticVbHits)     ? static_cast<uint32_t>(curVbHits  - g_prevStaticVbHits)     : static_cast<uint32_t>(curVbHits);
        snap.staticIbHits     = (curIbHits  >= g_prevStaticIbHits)     ? static_cast<uint32_t>(curIbHits  - g_prevStaticIbHits)     : static_cast<uint32_t>(curIbHits);
        snap.staticPbHits     = (curPbHits  >= g_prevStaticPbHits)     ? static_cast<uint32_t>(curPbHits  - g_prevStaticPbHits)     : static_cast<uint32_t>(curPbHits);
        snap.psPointerHits    = (curPsHits  >= g_prevPsPointerHits)    ? static_cast<uint32_t>(curPsHits  - g_prevPsPointerHits)    : static_cast<uint32_t>(curPsHits);
        snap.backBufferCopies = (curBbCopy  >= g_prevBackBufferCopies) ? static_cast<uint32_t>(curBbCopy  - g_prevBackBufferCopies) : static_cast<uint32_t>(curBbCopy);
        snap.surfaceReadbacks = (curSurfRbk >= g_prevSurfaceReadbacks) ? static_cast<uint32_t>(curSurfRbk - g_prevSurfaceReadbacks) : static_cast<uint32_t>(curSurfRbk);
    } else {
        snap.staticVbHits     = static_cast<uint32_t>(curVbHits);
        snap.staticIbHits     = static_cast<uint32_t>(curIbHits);
        snap.staticPbHits     = static_cast<uint32_t>(curPbHits);
        snap.psPointerHits    = static_cast<uint32_t>(curPsHits);
        snap.backBufferCopies = static_cast<uint32_t>(curBbCopy);
        snap.surfaceReadbacks = static_cast<uint32_t>(curSurfRbk);
        g_hasPrevDiagTotals   = true;
    }
    g_prevStaticVbHits     = curVbHits;
    g_prevStaticIbHits     = curIbHits;
    g_prevStaticPbHits     = curPbHits;
    g_prevPsPointerHits    = curPsHits;
    g_prevBackBufferCopies = curBbCopy;
    g_prevSurfaceReadbacks = curSurfRbk;

    snap.cachedTextures   = GetCachedTextureCount();
    snap.cachedGpuBuffers = GetCachedGpuBufferCount();
    snap.viewportW        = g_viewport.Width  ? g_viewport.Width  : 640u;
    snap.viewportH        = g_viewport.Height ? g_viewport.Height : 480u;
    snap.flipModel        = IsSwapChainFlipModel();
    snap.allowTearing     = DoesSwapChainAllowTearing();
    if (IsReadableHostMemoryRange(reinterpret_cast<const void*>(ADDR_LIGHTING_MODE), sizeof(uint32_t))) {
        snap.lightingMode = *reinterpret_cast<const uint32_t*>(ADDR_LIGHTING_MODE);
    }
    GetActiveSceneCameraSnapshot(snap.camPos, snap.camFwd);

    // Collect & sort active VS/FVF handles by descending CPU ms (tie-broken by draw count)
    uint32_t numVs = 0u;
    for (uint32_t i = 0u; i < kMaxTrackedVsHandles; ++i) {
        if (g_perfAccum.vsTable[i].used && g_perfAccum.vsTable[i].draws > 0u) {
            PerfVsHandleStat e = g_perfAccum.vsTable[i];
            e.cpuMs = static_cast<float>(PerfQpcTicksToMs(e.cpuTicks));
            s_sortVsScratch[numVs++] = e;
        }
    }
    std::sort(s_sortVsScratch, s_sortVsScratch + numVs, [](const PerfVsHandleStat& a, const PerfVsHandleStat& b) {
        if (a.cpuTicks != b.cpuTicks) return a.cpuTicks > b.cpuTicks;
        return a.draws > b.draws;
    });
    snap.totalDistinctVs        = numVs;
    snap.vsOverflowDraws        = g_perfAccum.vsOverflowDraws;
    snap.vsOverflowVerts        = g_perfAccum.vsOverflowVerts;
    snap.vsOverflowStaticVbHits = g_perfAccum.vsOverflowStaticVbHits;
    snap.vsOverflowCpuMs        = static_cast<float>(PerfQpcTicksToMs(g_perfAccum.vsOverflowTicks));
    snap.numTopVs               = (numVs < kTopVsHandlesInSnapshot) ? numVs : kTopVsHandlesInSnapshot;
    for (uint32_t i = 0u; i < snap.numTopVs; ++i) {
        snap.topVs[i] = s_sortVsScratch[i];
    }

    // Reset current-frame game-thread accumulator for the next frame
    memset(&g_perfAccum, 0, sizeof(g_perfAccum));

    // Only attempt an unhooked fallback cProfiler read BEFORE Bridge_ProfilerEndFrame has ever latched
    // a pre-reset MainLoop snapshot. Once Bridge_ProfilerEndFrame has fired, non-MainLoop swaps (loading
    // screens, modal menus, videos) leave cProfiler in its post-EndFrame zeroed state and must NEVER
    // overwrite the last valid pre-reset MainLoop snapshot.
    bool alreadyHasPreReset = false;
    AcquireSRWLockShared(&g_profSnapshotLock);
    alreadyHasPreReset = g_hasPreResetNativeLatch;
    ReleaseSRWLockShared(&g_profSnapshotLock);

    XboxNativeProfilerSnapshot fallbackNative = {};
    bool useFallbackNative = false;
    if (!alreadyHasPreReset &&
        IsReadableHostMemoryRange(reinterpret_cast<const void*>(ADDR_STD_PROFILER_PTR), sizeof(uint32_t)))
    {
        const void* profPtr = *reinterpret_cast<const void* const*>(ADDR_STD_PROFILER_PTR);
        if (profPtr) {
            SnapshotXboxNativeProfilerFromPtr(profPtr, &fallbackNative);
            useFallbackNative = fallbackNative.valid;
        }
    }

    AcquireSRWLockExclusive(&g_profSnapshotLock);
    if (g_frameRingCount < kProfileFrameRingSize) {
        uint32_t writeIdx = (g_frameRingHead + g_frameRingCount) % kProfileFrameRingSize;
        g_frameRing[writeIdx] = snap;
        ++g_frameRingCount;
    } else {
        g_frameRing[g_frameRingHead] = snap;
        g_frameRingHead = (g_frameRingHead + 1u) % kProfileFrameRingSize;
    }

    if (s_gpuPolledSinceLastLatch) {
        for (uint32_t h = 0u; h < s_pendingGpuHarvestCount; ++h) {
            const uint32_t targetSwap = s_pendingGpuHarvest[h].swapFrame;
            const float    harvestMs  = s_pendingGpuHarvest[h].gpuMs;
            for (uint32_t k = g_frameRingCount; k > 0u; --k) {
                uint32_t idx = (g_frameRingHead + k - 1u) % kProfileFrameRingSize;
                if ((targetSwap != 0u && g_frameRing[idx].swapFrame == targetSwap) ||
                    (targetSwap == 0u && !g_frameRing[idx].gpuQueryValid))
                {
                    g_frameRing[idx].gpuFrameMs    = harvestMs;
                    g_frameRing[idx].gpuQueryValid = true;
                    break;
                }
            }
        }
        s_pendingGpuHarvestCount  = 0u;
        s_gpuPolledSinceLastLatch = false;
    }

    if (useFallbackNative && !g_hasPreResetNativeLatch) {
        g_latchedNativeProfiler = fallbackNative;
    }
    ReleaseSRWLockExclusive(&g_profSnapshotLock);
}

void ResetProfilerState() {
    ReleaseGpuTimestampQueries();
    memset(&g_perfAccum, 0, sizeof(g_perfAccum));
    g_lastSwapEndQpc       = 0;
    g_hasPrevDiagTotals    = false;
    g_prevStaticVbHits     = 0ULL;
    g_prevStaticIbHits     = 0ULL;
    g_prevStaticPbHits     = 0ULL;
    g_prevPsPointerHits    = 0ULL;
    g_prevBackBufferCopies = 0ULL;
    g_prevSurfaceReadbacks = 0ULL;
    s_f3ForwardedDown      = false;
    s_f10ForwardedDown     = false;
    InterlockedExchange(&g_dumpSnapshotSeq, 0);

    AcquireSRWLockExclusive(&g_profSnapshotLock);
    memset(g_frameRing, 0, sizeof(g_frameRing));
    g_frameRingHead          = 0u;
    g_frameRingCount         = 0u;
    memset(&g_latchedNativeProfiler, 0, sizeof(g_latchedNativeProfiler));
    g_lastEndFrameHookSwap   = 0u;
    g_hasPreResetNativeLatch = false;
    ReleaseSRWLockExclusive(&g_profSnapshotLock);

    AcquireSRWLockExclusive(&g_toastLock);
    g_toastExpireTick    = 0u;
    g_toastMessageW[0]   = L'\0';
    ReleaseSRWLockExclusive(&g_toastLock);
}

bool GetLatestProfileSnapshots(GhostFrameSnapshot* outFrame, XboxNativeProfilerSnapshot* outNative) {
    AcquireSRWLockShared(&g_profSnapshotLock);
    bool hasFrame = (g_frameRingCount > 0u);
    if (outFrame) {
        if (hasFrame) {
            uint32_t latestIdx = (g_frameRingHead + g_frameRingCount - 1u) % kProfileFrameRingSize;
            *outFrame = g_frameRing[latestIdx];
        } else {
            memset(outFrame, 0, sizeof(*outFrame));
        }
    }
    if (outNative) {
        *outNative = g_latchedNativeProfiler;
    }
    ReleaseSRWLockShared(&g_profSnapshotLock);
    return hasFrame || (outNative && outNative->valid);
}

// ============================================================================
// Input Guard, Toast Notifications & F3 Panel Text Formatting
// ============================================================================

bool IsVkBoundInGameplayBindings(int vk) {
    if (vk <= 0) return false;
    const int* slots = reinterpret_cast<const int*>(&g_keyBindings);
    for (size_t i = 0u; i < kNumKeyBindSlots; ++i) {
        if (slots[i] == vk) return true;
    }
    return false;
}

void NotifyProfilerWindowFocusLost() {
    if (s_f3ForwardedDown) {
        s_f3ForwardedDown = false;
        PushDebugKeystroke(VK_F3, 0, true);
    }
    if (s_f10ForwardedDown) {
        s_f10ForwardedDown = false;
        PushDebugKeystroke(VK_F10, 0, true);
    }
}

bool HandleProfilerWindowKeyMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (wParam != VK_F3 && wParam != VK_F10) {
        return false;
    }

    const int vk = static_cast<int>(wParam);
    bool& fwdDown = (vk == VK_F3) ? s_f3ForwardedDown : s_f10ForwardedDown;

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        const bool altHeld  = (lParam & (1L << 29)) != 0;
        const bool isRepeat = (lParam & 0x40000000L) != 0;
        if (!altHeld && !IsVkBoundInGameplayBindings(vk)) {
            if (fwdDown) {
                fwdDown = false;
                PushDebugKeystroke(static_cast<uint16_t>(vk), 0, true);
            }
            if (!isRepeat) {
                if (vk == VK_F3) {
                    TogglePerfPanel(hWnd);
                } else {
                    TriggerProfileSnapshotDump();
                }
            }
            return true;
        }
        fwdDown = true;
        return false;
    }

    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (fwdDown || IsVkBoundInGameplayBindings(vk)) {
            fwdDown = false;
            return false;
        }
        return true;
    }

    return false;
}

static void SetPerfToastBanner(const wchar_t* msg, DWORD durationMs) {
    AcquireSRWLockExclusive(&g_toastLock);
    if (msg && msg[0]) {
        wcsncpy_s(g_toastMessageW, msg, _TRUNCATE);
        DWORD now = GetTickCount();
        g_toastExpireTick = now + (durationMs ? durationMs : 1u);
        if (g_toastExpireTick == 0u) g_toastExpireTick = 1u;
    } else {
        g_toastMessageW[0] = L'\0';
        g_toastExpireTick  = 0u;
    }
    ReleaseSRWLockExclusive(&g_toastLock);
}

bool GetActivePerfToastTextW(wchar_t* outBuf, size_t maxChars) {
    if (!outBuf || maxChars == 0u) return false;
    outBuf[0] = L'\0';
    DWORD now = GetTickCount();

    AcquireSRWLockShared(&g_toastLock);
    bool active = (g_toastExpireTick != 0u) &&
                  (static_cast<int32_t>(g_toastExpireTick - now) > 0) &&
                  (g_toastMessageW[0] != L'\0');
    if (active) {
        wcsncpy_s(outBuf, maxChars, g_toastMessageW, _TRUNCATE);
    }
    ReleaseSRWLockShared(&g_toastLock);
    return active;
}

void TogglePerfPanel(HWND hWnd) {
    g_osdShowPerfPanel = !g_osdShowPerfPanel;
    SyncOsdSystemMenuCheckmarks(hWnd);
}

static double GetNativeVarLatest(const XboxNativeProfilerSnapshot& np, uint32_t idx) {
    if (!np.valid || idx >= np.varCount || idx >= kMaxXboxProfileVars || !np.vars[idx].present) {
        return 0.0;
    }
    return np.vars[idx].latestVal;
}

void FormatPerfPanelTextW(wchar_t* outBuf, size_t maxChars) {
    if (!outBuf || maxChars == 0u) return;
    outBuf[0] = L'\0';

    uint32_t ringCount = 0u;
    GhostFrameSnapshot latest = {};
    XboxNativeProfilerSnapshot np = {};

    double avgWall60 = 0.0, maxWall60 = 0.0;
    uint32_t wallValid60 = 0u;

    double sWall = 0.0, sCpu = 0.0, sPace = 0.0, sGpu = 0.0, sPres = 0.0;
    double sDec = 0.0, sSubU = 0.0, sXvu = 0.0, sPbSelf = 0.0, sSkin = 0.0;
    double sSrv = 0.0, sRbk = 0.0, sVisW = 0.0;
    uint32_t smoothN = 0u;
    uint32_t smoothWallCnt = 0u;
    uint32_t gpuValidCnt = 0u;

    AcquireSRWLockShared(&g_profSnapshotLock);
    ringCount = g_frameRingCount;
    if (ringCount > 0u) {
        latest  = g_frameRing[(g_frameRingHead + ringCount - 1u) % kProfileFrameRingSize];
        smoothN = (ringCount < 15u) ? ringCount : 15u;

        for (uint32_t i = 0u; i < ringCount; ++i) {
            const GhostFrameSnapshot& f = g_frameRing[(g_frameRingHead + i) % kProfileFrameRingSize];
            if (f.wallTimeValid) {
                double w = f.frameWallMs;
                avgWall60 += w;
                if (wallValid60 == 0u || w > maxWall60) maxWall60 = w;
                ++wallValid60;
            }
        }
        for (uint32_t i = ringCount - smoothN; i < ringCount; ++i) {
            const GhostFrameSnapshot& f = g_frameRing[(g_frameRingHead + i) % kProfileFrameRingSize];
            if (f.wallTimeValid) {
                sWall += f.frameWallMs;
                sCpu  += f.cpuActiveMs;
                ++smoothWallCnt;
            }
            sPace   += f.paceWaitMs;
            sPres   += f.presentMs;
            sDec    += f.decodeUnifiedMs;
            sSubU   += f.submitUnifiedMs;
            sXvu    += f.submitXvuMs;
            sPbSelf += f.runPushBufferSelfMs;
            sSkin   += f.skinSetupMs;
            sSrv    += f.srvResolveMs;
            sRbk    += f.gpuReadbackMs;
            sVisW   += f.visWaitMs;
            if (f.gpuQueryValid) {
                sGpu += f.gpuFrameMs;
                ++gpuValidCnt;
            }
        }
    }
    np = g_latchedNativeProfiler;
    ReleaseSRWLockShared(&g_profSnapshotLock);

    wchar_t toastBuf[192] = {};
    bool hasToast = GetActivePerfToastTextW(toastBuf, 192u);

    if (ringCount == 0u && !np.valid) {
        swprintf(outBuf, maxChars,
            L"[GhostXR Perf] Collecting frame metrics...\n"
            L"%s",
            hasToast ? toastBuf : L"[F3] Hide Panel   [F10] Save GhostXR_Data\\profile_dump.txt");
        return;
    }

    if (wallValid60 > 0u) {
        avgWall60 /= static_cast<double>(wallValid60);
    }
    if (smoothWallCnt > 0u) {
        double invWall = 1.0 / static_cast<double>(smoothWallCnt);
        sWall *= invWall;
        sCpu  *= invWall;
    }
    if (smoothN > 0u) {
        double inv = 1.0 / static_cast<double>(smoothN);
        sPace *= inv; sPres *= inv;
        sDec  *= inv; sSubU *= inv; sXvu *= inv; sPbSelf *= inv; sSkin *= inv;
        sSrv  *= inv; sRbk *= inv; sVisW *= inv;
    }
    if (gpuValidCnt > 0u) {
        sGpu /= static_cast<double>(gpuValidCnt);
    }

    double dispFps = (smoothWallCnt > 0u && sWall > 0.1) ? (1000.0 / sWall) : static_cast<double>(g_osdCurrentFps);
    wchar_t gpuStr[24] = {};
    if (gpuValidCnt > 0u) {
        swprintf(gpuStr, 24u, L"%5.2fms", sGpu);
    } else {
        wcscpy_s(gpuStr, L"  n/a  ");
    }

    uint32_t dynDiscTotal = latest.dynVbDiscardCount + latest.dynStreamVbDiscardCount + latest.dynIbDiscardCount;

    wchar_t hostSection[1024] = {};
    swprintf(hostSection, 1024u,
        L"[GhostXR Perf] %5.1f FPS | Frame %5.2fms (60f Avg %5.2f Max %5.2f)\n"
        L"Host : CPU %5.2fms | GPU %s | Pres %5.2fms | Pace %5.2fms\n"
        L"Draw : %4u calls (XVU %4u / FVF %4u) | %5.1fk vtx (FVF %4.1fk)\n"
        L"CPUms: FVFDec %5.2f | FVFSub %4.2f | XVU %4.2f | PBuf %4.2f | Skin %4.2f\n"
        L"TexRT: SRV %4.2fms(Up %u) | RdBk %4.2fms(%u) | BBCpy %u | RT %2u | ShdC %u\n"
        L"Cache: VB %4u IB %4u PB %3u | Disc %2u | Vis %2u/%2u (Wait %4.2fms)",
        dispFps, sWall, avgWall60, maxWall60,
        sCpu, gpuStr, sPres, sPace,
        latest.totalDraws, latest.xvuDraws, latest.unifiedDraws,
        static_cast<double>(latest.totalVerts) * 0.001,
        static_cast<double>(latest.unifiedVerts) * 0.001,
        sDec, sSubU, sXvu, sPbSelf, sSkin,
        sSrv, latest.texUploadCount, sRbk, latest.surfaceReadbacks,
        latest.backBufferCopies, latest.rtSwitchCount, latest.shaderRuntimeCompiles,
        latest.staticVbHits, latest.staticIbHits, latest.staticPbHits,
        dynDiscTotal, latest.visBeginCount, latest.visGetResultCount, sVisW);

    wchar_t nativeSection[1024] = {};
    if (np.valid) {
        double totRend  = GetNativeVarLatest(np, 0x0Eu);
        double showFrm  = GetNativeVarLatest(np, 0x17u);
        double sortLst  = GetNativeVarLatest(np, 0x23u);
        double shellMs  = GetNativeVarLatest(np, 0x13u);
        double rendObj  = GetNativeVarLatest(np, 0x1Cu);
        double objDip   = GetNativeVarLatest(np, 0x1Du);
        double vertSet  = GetNativeVarLatest(np, 0x1Fu);
        double updSkel  = GetNativeVarLatest(np, 0x1Eu);
        double modLit   = GetNativeVarLatest(np, 0x28u);
        double wallsMs  = GetNativeVarLatest(np, 0x20u);
        double shadMs   = GetNativeVarLatest(np, 0x29u);
        double fxAllMs  = GetNativeVarLatest(np, 0x24u);
        double fxPreMs  = GetNativeVarLatest(np, 0x25u);
        double fxMgdMs  = GetNativeVarLatest(np, 0x26u);
        double physMs   = np.physicsCombinedMs;
        double simMs    = GetNativeVarLatest(np, 0x14u);
        double aiMs     = GetNativeVarLatest(np, 0x19u);
        double plrAiMs  = GetNativeVarLatest(np, 0x18u);
        double animMs   = GetNativeVarLatest(np, 0x1Au);
        double sndMs    = GetNativeVarLatest(np, 0x15u);
        double rayMs    = GetNativeVarLatest(np, 0x21u);
        double flrMs    = GetNativeVarLatest(np, 0x22u);

        uint32_t wPolys = static_cast<uint32_t>(GetNativeVarLatest(np, 0x02u) + 0.5);
        uint32_t objCnt = static_cast<uint32_t>(GetNativeVarLatest(np, 0x0Fu) + 0.5);
        uint32_t sect   = static_cast<uint32_t>(GetNativeVarLatest(np, 0x09u) + 0.5);
        uint32_t strips = static_cast<uint32_t>(GetNativeVarLatest(np, 0x03u) + 0.5);
        uint32_t lists  = static_cast<uint32_t>(GetNativeVarLatest(np, 0x04u) + 0.5);
        uint32_t fxTris = static_cast<uint32_t>(GetNativeVarLatest(np, 0x2Du) + 0.5);
        uint32_t rays   = static_cast<uint32_t>(GetNativeVarLatest(np, 0x06u) + 0.5);
        uint32_t texChg = static_cast<uint32_t>(GetNativeVarLatest(np, 0x05u) + 0.5);
        uint32_t modChg = static_cast<uint32_t>(GetNativeVarLatest(np, 0x08u) + 0.5);

        wchar_t nativeHdr[96] = {};
        if (ringCount > 0u && np.capturedPreReset && latest.swapFrame > np.swapFrameCaptured + 2u) {
            swprintf(nativeHdr, 96u, L"--- Ghost.exe cProfiler (last MainLoop #%u) ---", np.swapFrameCaptured);
        } else {
            wcscpy_s(nativeHdr, L"--- Ghost.exe cProfiler (ms / counts) ---");
        }

        swprintf(nativeSection, 1024u,
            L"\n%s\n"
            L"Rend : Total %5.2f | Show %5.2f | Sort %4.2f | Shell %4.2f\n"
            L"Objs : %5.2f (DIP %4.2f VSet %4.2f Skel %4.2f Lit %4.2f)\n"
            L"World: Walls %5.2f | Shad %5.2f | FX %4.2f (Pre %4.2f Mgd %4.2f)\n"
            L"Sim  : Phys %5.2f | Sim %4.2f (AI %4.2f Plr %4.2f Anim %4.2f) | Snd %4.2f\n"
            L"Scene: WPoly %5u | Obj %3u | Sec %2u | Strp %3u Lst %3u | FXTri %4u\n"
            L"Misc : Ray %3u(%4.2fms) | Flr %4.2fms | TexChg %3u | ModeChg %3u",
            nativeHdr,
            totRend, showFrm, sortLst, shellMs,
            rendObj, objDip, vertSet, updSkel, modLit,
            wallsMs, shadMs, fxAllMs, fxPreMs, fxMgdMs,
            physMs, simMs, aiMs, plrAiMs, animMs, sndMs,
            wPolys, objCnt, sect, strips, lists, fxTris,
            rays, rayMs, flrMs, texChg, modChg);
    } else {
        wcscpy_s(nativeSection, L"\n--- Ghost.exe cProfiler: waiting for MainLoop ---");
    }

    const wchar_t* footer = hasToast
        ? toastBuf
        : L"[F3] Hide Panel   [F10] Save GhostXR_Data\\profile_dump.txt";

    swprintf(outBuf, maxChars, L"%s%s\n%s", hostSection, nativeSection, footer);
}

// ============================================================================
// F10 Detailed Frame Snapshot Exporter (GhostXR_Data\profile_dump.txt)
// ============================================================================

struct MetricStats60 {
    double latest;
    double avg;
    double minVal;
    double maxVal;
    double p99Val;
};

template <typename Getter>
static MetricStats60 ComputeRingStats(const GhostFrameSnapshot* ring, uint32_t count, Getter fn) {
    MetricStats60 st = {};
    if (count == 0u) return st;

    double vals[kProfileFrameRingSize];
    double sum = 0.0;
    for (uint32_t i = 0u; i < count; ++i) {
        double v = static_cast<double>(fn(ring[i]));
        vals[i] = v;
        sum += v;
    }
    st.latest = vals[count - 1u];
    st.avg    = sum / static_cast<double>(count);

    std::sort(vals, vals + count);
    st.minVal = vals[0];
    st.maxVal = vals[count - 1u];
    uint32_t p99Idx = (count > 1u) ? ((count * 99u) / 100u) : 0u;
    if (p99Idx >= count) p99Idx = count - 1u;
    st.p99Val = vals[p99Idx];
    return st;
}

template <typename Pred, typename Getter>
static MetricStats60 ComputeFilteredRingStats(
    const GhostFrameSnapshot* ring, uint32_t count, Pred pred, Getter fn, uint32_t* outValidCount = nullptr)
{
    MetricStats60 st = {};
    if (outValidCount) *outValidCount = 0u;
    if (count == 0u) return st;

    double vals[kProfileFrameRingSize];
    uint32_t nValid = 0u;
    double sum = 0.0;
    for (uint32_t i = 0u; i < count; ++i) {
        if (!pred(ring[i])) continue;
        double v = static_cast<double>(fn(ring[i]));
        vals[nValid++] = v;
        sum += v;
    }
    if (outValidCount) *outValidCount = nValid;
    if (nValid == 0u) return st;

    st.latest = vals[nValid - 1u];
    st.avg    = sum / static_cast<double>(nValid);

    std::sort(vals, vals + nValid);
    st.minVal = vals[0];
    st.maxVal = vals[nValid - 1u];
    uint32_t p99Idx = (nValid > 1u) ? ((nValid * 99u) / 100u) : 0u;
    if (p99Idx >= nValid) p99Idx = nValid - 1u;
    st.p99Val = vals[p99Idx];
    return st;
}

static void ResolveVsHandleLabel(const PerfVsHandleStat& v, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0u) return;
    outBuf[0] = '\0';

    if (v.vsType >= 0 && v.vsType < 66) {
        uintptr_t entryAddr = static_cast<uintptr_t>(ADDR_VERTEX_SHADERS_TABLE) +
                              static_cast<uintptr_t>(v.vsType) * 24u;
        int lightSlot = -1;
        char rawName[64] = {};

        if (IsReadableHostMemoryRange(reinterpret_cast<const void*>(entryAddr), 24u)) {
            const char* namePtr = *reinterpret_cast<const char* const*>(entryAddr);
            const uint32_t* slots = reinterpret_cast<const uint32_t*>(entryAddr + 4u);
            for (uint32_t s = 0u; s < 5u; ++s) {
                if (slots[s] == v.handle) {
                    lightSlot = static_cast<int>(s);
                    break;
                }
            }
            if (namePtr && reinterpret_cast<uintptr_t>(namePtr) >= kMinValidXboxAddress &&
                IsReadableHostMemoryRange(namePtr, 1u))
            {
                size_t k = 0u;
                while (k < sizeof(rawName) - 1u && IsReadableHostMemoryRange(namePtr + k, 1u)) {
                    char c = namePtr[k];
                    if (c == '\0') break;
                    rawName[k++] = (c >= 32 && c < 127) ? c : '?';
                }
                rawName[k] = '\0';
            }
        }

        const char* trimmed = rawName;
        if (_strnicmp(trimmed, "VertexShaders/xbox/", 19) == 0 ||
            _strnicmp(trimmed, "VertexShaders\\xbox\\", 19) == 0)
        {
            trimmed += 19;
        }
        if (trimmed[0] != '\0') {
            if (lightSlot >= 0) {
                snprintf(outBuf, outCap, "%s[L%d]", trimmed, lightSlot);
            } else {
                snprintf(outBuf, outCap, "%s", trimmed);
            }
        } else if (lightSlot >= 0) {
            snprintf(outBuf, outCap, "aVertexShaders[%d][L%d]", v.vsType, lightSlot);
        } else {
            snprintf(outBuf, outCap, "aVertexShaders[%d]", v.vsType);
        }
        return;
    }

    if (!v.isXvu && (v.handle & 1u) == 0u) {
        switch (v.handle) {
            case 0x152u: snprintf(outBuf, outCap, "FVF_XYZ_NORM_DIFF_TEX1"); return;
            case 0x112u: snprintf(outBuf, outCap, "FVF_XYZ_NORM_TEX1");      return;
            case 0x144u: snprintf(outBuf, outCap, "FVF_XYZRHW_DIFF_TEX1");   return;
            case 0x1C4u: snprintf(outBuf, outCap, "FVF_XYZRHW_DIFF_SPEC_T1");return;
            case 0x044u: snprintf(outBuf, outCap, "FVF_XYZRHW_DIFF");        return;
            case 0x104u: snprintf(outBuf, outCap, "FVF_XYZRHW_TEX1");        return;
            case 0x042u: snprintf(outBuf, outCap, "FVF_XYZ_DIFF");           return;
            case 0x142u: snprintf(outBuf, outCap, "FVF_XYZ_DIFF_TEX1");      return;
            case 0x242u: snprintf(outBuf, outCap, "FVF_XYZ_DIFF_TEX2");      return;
            default:     snprintf(outBuf, outCap, "FVF_0x%03X", v.handle);   return;
        }
    }

    snprintf(outBuf, outCap, "%s", v.isXvu ? "CustomXvuShader" : "CustomVertexDecl");
}

static const char* DescribeXboxProfileType(uint32_t vt) {
    switch (vt) {
        case 0u: return "Float(ms)";
        case 1u: return "Int      ";
        case 2u: return "UInt     ";
        case 3u: return "String   ";
        default: return "Unknown  ";
    }
}

bool TriggerProfileSnapshotDump(wchar_t* outSavedPathW, size_t outMaxChars, uint32_t* outSnapshotIdx) {
    if (outSavedPathW && outMaxChars > 0u) outSavedPathW[0] = L'\0';
    if (outSnapshotIdx) *outSnapshotIdx = 0u;

    // 1. Copy latched ring buffer + native profiler state under shared lock (non-blocking to game thread)
    auto ringCopy = std::make_unique<GhostFrameSnapshot[]>(kProfileFrameRingSize);
    uint32_t ringCount = 0u;
    XboxNativeProfilerSnapshot np = {};

    AcquireSRWLockShared(&g_profSnapshotLock);
    ringCount = g_frameRingCount;
    for (uint32_t i = 0u; i < ringCount; ++i) {
        ringCopy[i] = g_frameRing[(g_frameRingHead + i) % kProfileFrameRingSize];
    }
    np = g_latchedNativeProfiler;
    ReleaseSRWLockShared(&g_profSnapshotLock);

    // If no native profiler snapshot was latched yet, attempt a safe on-demand read
    if (!np.valid && IsReadableHostMemoryRange(reinterpret_cast<const void*>(ADDR_STD_PROFILER_PTR), sizeof(uint32_t))) {
        const void* profPtr = *reinterpret_cast<const void* const*>(ADDR_STD_PROFILER_PTR);
        if (profPtr) {
            SnapshotXboxNativeProfilerFromPtr(profPtr, &np);
        }
    }

    // 2. Resolve <AppDataDir>\profile_dump.txt
    wchar_t dataDirW[kMaxHostPath] = {};
    if (g_appRootDir[0] != '\0') {
        Utf8ToWide(g_appRootDir, dataDirW, kMaxHostPath);
    }
    if (dataDirW[0] == L'\0') {
        GetAppDataAndIniPathsW(dataDirW, kMaxHostPath, nullptr, 0u);
    }
    if (dataDirW[0] == L'\0') {
        return false;
    }
    CreateDirectoryW(dataDirW, nullptr);

    wchar_t dumpPathW[kMaxHostPath] = {};
    JoinPathW(dumpPathW, kMaxHostPath, dataDirW, L"profile_dump.txt");
    if (dumpPathW[0] == L'\0') {
        return false;
    }

    AcquireSRWLockExclusive(&g_profDumpFileLock);
    uint32_t snapIdx = static_cast<uint32_t>(InterlockedIncrement(&g_dumpSnapshotSeq));
    const wchar_t* openMode = (snapIdx == 1u) ? L"wb" : L"ab";
    FILE* fp = _wfopen(dumpPathW, openMode);
    if (!fp) {
        ReleaseSRWLockExclusive(&g_profDumpFileLock);
        SetPerfToastBanner(L"[F10] ERROR: Failed to open GhostXR_Data\\profile_dump.txt", 4000u);
        return false;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char adapterUtf8[256] = {};
    WideToUtf8(g_selectedAdapterName[0] ? g_selectedAdapterName : L"Default D3D11 Adapter", adapterUtf8, sizeof(adapterUtf8));
    char dumpPathUtf8[kMaxHostPath] = {};
    WideToUtf8(dumpPathW, dumpPathUtf8, sizeof(dumpPathUtf8));

    const GhostFrameSnapshot latest = (ringCount > 0u) ? ringCopy[ringCount - 1u] : GhostFrameSnapshot{};

    if (snapIdx > 1u) {
        fprintf(fp, "\n\n");
    }
    fprintf(fp,
        "========================================================================================\n"
        "GhostXR Performance Profile Snapshot #%u\n"
        "Timestamp        : %04u-%02u-%02u %02u:%02u:%02u.%03u\n"
        "Output File      : %s\n"
        "Swap Frame       : %u (Rolling Window: %u frames)\n"
        "D3D11 Adapter    : %s\n"
        "SwapChain Mode   : %ux%u | FlipModel=%s | AllowTearing=%s\n"
        "Camera World Pos : (%.3f, %.3f, %.3f) | View Forward: (%.3f, %.3f, %.3f)\n"
        "Engine Lighting  : cRenderState::m_lightingMode = %u\n"
        "Cached Resources : %u D3D11 Textures | %u Static GPU Buffers (VB/IB/PB)\n"
        "========================================================================================\n\n",
        snapIdx,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        dumpPathUtf8,
        latest.swapFrame, ringCount,
        adapterUtf8,
        latest.viewportW ? latest.viewportW : 640u,
        latest.viewportH ? latest.viewportH : 480u,
        latest.flipModel ? "true" : "false",
        latest.allowTearing ? "true" : "false",
        latest.camPos[0], latest.camPos[1], latest.camPos[2],
        latest.camFwd[0], latest.camFwd[1], latest.camFwd[2],
        latest.lightingMode,
        latest.cachedTextures, latest.cachedGpuBuffers);

    // --- Section 1: Frame Timing & Bottleneck Summary ---
    uint32_t validWallSamples = 0u;
    uint32_t validGpuSamples  = 0u;
    MetricStats60 msWall   = ComputeFilteredRingStats(
        ringCopy.get(), ringCount,
        [](const GhostFrameSnapshot& f) { return f.wallTimeValid; },
        [](const GhostFrameSnapshot& f) { return f.frameWallMs; },
        &validWallSamples);
    MetricStats60 msCpu    = ComputeFilteredRingStats(
        ringCopy.get(), ringCount,
        [](const GhostFrameSnapshot& f) { return f.wallTimeValid; },
        [](const GhostFrameSnapshot& f) { return f.cpuActiveMs; });
    MetricStats60 msPace   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.paceWaitMs; });
    MetricStats60 msGpu    = ComputeFilteredRingStats(
        ringCopy.get(), ringCount,
        [](const GhostFrameSnapshot& f) { return f.gpuQueryValid; },
        [](const GhostFrameSnapshot& f) { return f.gpuFrameMs; },
        &validGpuSamples);
    MetricStats60 msPres   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.presentMs; });
    MetricStats60 msComp   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.compositeOsdMs; });
    MetricStats60 msBridge = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.hostBridgeCpuMs; });
    MetricStats60 msDec    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.decodeUnifiedMs; });
    MetricStats60 msSubU   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.submitUnifiedMs; });
    MetricStats60 msXvu    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.submitXvuMs; });
    MetricStats60 msPbT    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.runPushBufferTotalMs; });
    MetricStats60 msPbS    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.runPushBufferSelfMs; });
    MetricStats60 msSkin   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.skinSetupMs; });
    MetricStats60 msSrv    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.srvResolveMs; });
    MetricStats60 msTUp    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.texUploadMs; });
    MetricStats60 msRbk    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.gpuReadbackMs; });
    MetricStats60 msVis    = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.visWaitMs; });
    MetricStats60 msShdC   = ComputeRingStats(ringCopy.get(), ringCount, [](const GhostFrameSnapshot& f) { return f.shaderCompileMs; });

    double latestFps = (validWallSamples > 0u && msWall.latest > 0.05) ? (1000.0 / msWall.latest) : 0.0;
    double avgFps60  = (validWallSamples > 0u && msWall.avg    > 0.05) ? (1000.0 / msWall.avg)    : 0.0;

    const char* classification = "No frame samples captured yet";
    if (ringCount > 0u) {
        if (validWallSamples > 0u && msWall.avg <= 17.2) {
            classification = "60 Hz Target Met (Frame within 16.67 ms budget)";
        } else if (msVis.avg >= 4.0 || msRbk.avg >= 4.0) {
            classification = "CPU-GPU Pipeline Sync Stall (Blocking Occlusion Query GetData or Staging Surface Readback Map)";
        } else if (validGpuSamples > 0u && msGpu.avg >= 16.8 && msGpu.avg >= msCpu.avg) {
            classification = "GPU Execution Bound (D3D11 GPU Timestamp exceeds 16.67 ms budget)";
        } else if (msPres.avg >= 6.0 && (validGpuSamples == 0u || msGpu.avg < 16.8) && msPres.avg >= msCpu.avg) {
            classification = "DXGI Present / VSync Wait Bound (SwapChain::Present wait dominates frame time)";
        } else if (validGpuSamples > 0u && msGpu.avg > msCpu.avg * 0.85 && msPres.avg >= 4.0) {
            classification = "GPU / Present Backpressure Bound (GPU Timestamp & Present wait dominate frame time)";
        } else if (msCpu.avg >= 16.7 && msPres.avg >= 6.0) {
            classification = "CPU Bound + VSync Wait (CPU Active > 16.67 ms missed VBlank; Present waited for next sync)";
        } else {
            classification = "CPU Bound on Game/Render Thread (Unpaced CPU work exceeds 16.67 ms; PaceWait ~0 ms)";
        }
    }

    fprintf(fp,
        "[1] FRAME TIMING SUMMARY (FPS: Latest %.2f | %uf Rolling Avg %.2f)\n"
        "    Classification : %s\n"
        "    ------------------------------------------------------------------------------------\n"
        "    Metric                                  Latest(ms)   Avg(ms)   Min(ms)   Max(ms)   P99(ms)\n"
        "    ------------------------------------------------------------------------------------\n"
        "    Frame Wall Time                          %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    CPU Active Time (Wall - Present - Pace)  %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    60Hz Pacer Wait (PaceSwapToXbox60Hz)     %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    GPU Frame Time (D3D11 TIMESTAMP%s)    %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    DXGI SwapChain::Present                  %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    Present Composite + OSD Overlay          %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n\n",
        latestFps, ringCount, avgFps60,
        classification,
        msWall.latest, msWall.avg, msWall.minVal, msWall.maxVal, msWall.p99Val,
        msCpu.latest,  msCpu.avg,  msCpu.minVal,  msCpu.maxVal,  msCpu.p99Val,
        msPace.latest, msPace.avg, msPace.minVal, msPace.maxVal, msPace.p99Val,
        (validGpuSamples > 0u) ? "     " : ": n/a",
        msGpu.latest,  msGpu.avg,  msGpu.minVal,  msGpu.maxVal,  msGpu.p99Val,
        msPres.latest, msPres.avg, msPres.minVal, msPres.maxVal, msPres.p99Val,
        msComp.latest, msComp.avg, msComp.minVal, msComp.maxVal, msComp.p99Val);

    // --- Section 2: Host D3D8 -> D3D11 Bridge Sub-Timers & Counters ---
    fprintf(fp,
        "[2] HOST D3D8 -> D3D11 BRIDGE SUB-TIMERS & COUNTERS\n"
        "    ------------------------------------------------------------------------------------\n"
        "    Host Bridge Stage                       Latest(ms)   Avg(ms)   Min(ms)   Max(ms)   P99(ms)\n"
        "    ------------------------------------------------------------------------------------\n"
        "    Host Bridge Render CPU Sum (Disjoint)    %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    DecodeVerticesToUnified (CPU FVF/Lit)    %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    SubmitUnifiedVerticesD3D11               %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    SubmitXvuDrawD3D11 (.xvu GPU VS)         %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    RunPushBuffer (Self Parse/Fixup/IB)      %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    RunPushBuffer (Total incl. Nested Draws) %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    RenderSetupBlendWeightedSkin             %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    GetOrCreateD3D11SRV (Total Lookup+Up)    %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    UploadXboxTextureToD3D11 (Tex Unswizzle) %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    SyncSurfaceFromGpuIfNeeded (Readback)    %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    GetVisibilityTestResult (Occlusion Wait) %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    Runtime HLSL->DXBC Shader Compile        %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n"
        "    ------------------------------------------------------------------------------------\n"
        "    Draw & Vertex Breakdown (Latest Frame #%u):\n"
        "      Total Draws / Verts         : %6u draws | %8u verts\n"
        "      .xvu GPU VS Path            : %6u draws (StaticVB: %u, DynSlice: %u) | %8u verts\n"
        "      Unified / FVF CPU Path      : %6u draws | %8u verts\n"
        "        - RHW 2D Screen-Space     : %6u draws | %8u verts\n"
        "        - FVF 0x152 (Stride 36)   : %6u draws | %8u verts\n"
        "        - FVF 0x112 (Stride 32)   : %6u draws | %8u verts\n"
        "        - Other FVF / Decl        : %6u draws | %8u verts\n"
        "      Unified Sub-Features        : GPULit=%u | CPULitFallback=%u | GPUSkin=%u | CPUSkinFallback=%u | PtSprite=%u(%u vtx)\n"
        "      PushBuffers                 : %6u calls (Fixup=%u, DynRing=%u, BytesParsed=%u)\n"
        "      Skinning Setup (0x00607390) : %6u calls | %8u source verts\n"
        "      Static Cache Hits (Frame)   : VB=%u | IB=%u | PushBuffer=%u | PSObjectPtr=%u\n"
        "      Dynamic Ring DISCARD Wraps  : UnifiedVB=%u | XvuStreamVB=%u | XvuIB=%u\n"
        "      RT / Clear / Texture / Sync : SetRT=%u | ClearFast=%u | ClearQuad=%u | SRVResolve=%u | TexUp=%u(%u B)\n"
        "      GPU Copies & Readbacks      : BackBufferCopy=%u | SurfaceGpuReadback=%u | GPUDisjoint=%u\n"
        "      Hardware Occlusion Queries  : Begin=%u | GetResult=%u | WaitFlag=%u | Incomplete=%u | SpinPolls=%u\n\n",
        msBridge.latest, msBridge.avg, msBridge.minVal, msBridge.maxVal, msBridge.p99Val,
        msDec.latest,    msDec.avg,    msDec.minVal,    msDec.maxVal,    msDec.p99Val,
        msSubU.latest,   msSubU.avg,   msSubU.minVal,   msSubU.maxVal,   msSubU.p99Val,
        msXvu.latest,    msXvu.avg,    msXvu.minVal,    msXvu.maxVal,    msXvu.p99Val,
        msPbS.latest,    msPbS.avg,    msPbS.minVal,    msPbS.maxVal,    msPbS.p99Val,
        msPbT.latest,    msPbT.avg,    msPbT.minVal,    msPbT.maxVal,    msPbT.p99Val,
        msSkin.latest,   msSkin.avg,   msSkin.minVal,   msSkin.maxVal,   msSkin.p99Val,
        msSrv.latest,    msSrv.avg,    msSrv.minVal,    msSrv.maxVal,    msSrv.p99Val,
        msTUp.latest,    msTUp.avg,    msTUp.minVal,    msTUp.maxVal,    msTUp.p99Val,
        msRbk.latest,    msRbk.avg,    msRbk.minVal,    msRbk.maxVal,    msRbk.p99Val,
        msVis.latest,    msVis.avg,    msVis.minVal,    msVis.maxVal,    msVis.p99Val,
        msShdC.latest,   msShdC.avg,   msShdC.minVal,   msShdC.maxVal,   msShdC.p99Val,
        latest.swapFrame,
        latest.totalDraws, latest.totalVerts,
        latest.xvuDraws, latest.xvuStaticVbDraws, latest.xvuDynamicSliceDraws, latest.xvuVerts,
        latest.unifiedDraws, latest.unifiedVerts,
        latest.unifiedRhwDraws, latest.unifiedRhwVerts,
        latest.unifiedFvf0x152Draws, latest.unifiedFvf0x152Verts,
        latest.unifiedFvf0x112Draws, latest.unifiedFvf0x112Verts,
        latest.unifiedOtherDraws, latest.unifiedOtherVerts,
        latest.unifiedGpuLitDraws, latest.unifiedCpuLitDraws,
        latest.unifiedGpuSkinDraws, latest.unifiedCpuSkinDraws,
        latest.unifiedPointSpriteDraws, latest.unifiedPointSpriteVerts,
        latest.pushBufferCalls, latest.pushBufferFixupCalls, latest.pushBufferDynRingCalls, latest.pushBufferBytesParsed,
        latest.skinSetupCalls, latest.skinSetupVerts,
        latest.staticVbHits, latest.staticIbHits, latest.staticPbHits, latest.psPointerHits,
        latest.dynVbDiscardCount, latest.dynStreamVbDiscardCount, latest.dynIbDiscardCount,
        latest.rtSwitchCount, latest.clearFastCount, latest.clearQuadCount,
        latest.srvResolveCalls, latest.texUploadCount, latest.texUploadBytes,
        latest.backBufferCopies, latest.surfaceReadbacks, latest.gpuDisjointCount,
        latest.visBeginCount, latest.visGetResultCount, latest.visWaitCallCount,
        latest.visIncompleteCount, latest.visSpinCount);

    // --- Section 3: Top Vertex Shader / FVF Handles ---
    fprintf(fp,
        "[3] TOP ACTIVE VERTEX SHADER / FVF HANDLES (Latest Frame #%u | Distinct Tuples: %u)\n"
        "    --------------------------------------------------------------------------------------------------------------\n"
        "    Rank  VS Handle    Path   vsType  Stride   Draws     Verts   StaticVB    HostCPU(ms)  Shader / FVF Label\n"
        "    --------------------------------------------------------------------------------------------------------------\n",
        latest.swapFrame, latest.totalDistinctVs);
    if (latest.numTopVs == 0u && latest.vsOverflowDraws == 0u) {
        fprintf(fp, "    (No draw calls recorded in latest frame)\n\n");
    } else {
        for (uint32_t i = 0u; i < latest.numTopVs; ++i) {
            const PerfVsHandleStat& v = latest.topVs[i];
            char labelBuf[72] = {};
            ResolveVsHandleLabel(v, labelBuf, sizeof(labelBuf));
            fprintf(fp,
                "    #%2u   0x%08X   %-4s   %6d  %6u  %6u  %8u     %6u       %8.3f  %s\n",
                i + 1u, v.handle, v.isXvu ? "XVU" : "FVF", v.vsType,
                v.stride, v.draws, v.verts, v.staticVbHits, v.cpuMs, labelBuf);
        }
        if (latest.vsOverflowDraws > 0u) {
            fprintf(fp,
                "    [OVF] >%3u slots   MIXED       -       -  %6u  %8u     %6u       %8.3f  [Unbinned >512 Tuple Overflow]\n",
                kMaxTrackedVsHandles,
                latest.vsOverflowDraws, latest.vsOverflowVerts,
                latest.vsOverflowStaticVbHits, latest.vsOverflowCpuMs);
        }
        fprintf(fp, "\n");
    }

    // --- Section 4: All 46 Native Ghost.exe cProfiler Variables ---
    fprintf(fp,
        "[4] GHOST.EXE NATIVE cProfiler (stdProfiler_pProfile @ 0x%08X)\n",
        ADDR_STD_PROFILER_PTR);
    if (!np.valid) {
        fprintf(fp, "    (stdProfiler_pProfile not initialized or no MainLoop frame captured yet)\n\n");
    } else {
        fprintf(fp,
            "    Captured at Swap Frame #%u | Registered Variables: %u | Source: %s\n",
            np.swapFrameCaptured, np.varCount,
            np.capturedPreReset ? "Pre-Reset MainLoop Hook (0x0043C830)" : "On-Demand Read (Pre-MainLoop)");
        if (ringCount > 0u && np.capturedPreReset && latest.swapFrame > np.swapFrameCaptured + 2u) {
            fprintf(fp,
                "    Status: Swap #%u is %u non-MainLoop swaps past last MainLoop EndFrame (#%u); preserving pre-reset values.\n",
                latest.swapFrame, latest.swapFrame - np.swapFrameCaptured, np.swapFrameCaptured);
        }
        fprintf(fp,
            "    Note: [0x0A] Physics Time pushes 2 samples/frame in MainLoop:\n"
            "          simPhysics_Update = %.3f ms + simPhysics_UpdateConstraintWorld = %.3f ms => Total = %.3f ms\n"
            "    ------------------------------------------------------------------------------------\n"
            "    Idx    Variable Name                      Type       Rst  Samples      Latest      80fAvg      80fMax\n"
            "    ------------------------------------------------------------------------------------\n",
            np.physicsSimUpdateMs, np.physicsConstraintMs, np.physicsCombinedMs);

        for (uint32_t i = 0u; i < np.varCount && i < kMaxXboxProfileVars; ++i) {
            const XboxNativeVarSnapshot& v = np.vars[i];
            if (!v.present) continue;
            if (v.valueType == 3u) {
                fprintf(fp,
                    "    [0x%02X] %-34s %s   %u        -   \"%s\"\n",
                    i, v.name, DescribeXboxProfileType(v.valueType), v.resetPerFrame ? 1u : 0u, v.noteText);
            } else if (v.valueType == 0u) {
                fprintf(fp,
                    "    [0x%02X] %-34s %s   %u      %3u   %10.3f  %10.3f  %10.3f\n",
                    i, v.name, DescribeXboxProfileType(v.valueType), v.resetPerFrame ? 1u : 0u,
                    v.sampleCount, v.latestVal, v.avgVal, v.maxVal);
            } else {
                fprintf(fp,
                    "    [0x%02X] %-34s %s   %u      %3u   %10.0f  %10.1f  %10.0f\n",
                    i, v.name, DescribeXboxProfileType(v.valueType), v.resetPerFrame ? 1u : 0u,
                    v.sampleCount, v.latestVal, v.avgVal, v.maxVal);
            }
        }
        fprintf(fp, "\n");
    }

    // --- Section 5: Recent 16-Frame Timeline (detects steady 33ms vs alternating stutter vs VSync lock) ---
    uint32_t timelineCount = (ringCount < 16u) ? ringCount : 16u;
    fprintf(fp,
        "[5] RECENT %u-FRAME TIMELINE (Oldest -> Newest)\n"
        "    ----------------------------------------------------------------------------------------------------\n"
        "      Swap#   Wall(ms)  CPUAct(ms)   GPU(ms)  Pres(ms)  Pace(ms)  Draws   XVU   FVF  FVFDec  XVUMs  PBufS\n"
        "    ----------------------------------------------------------------------------------------------------\n",
        timelineCount);
    for (uint32_t i = ringCount - timelineCount; i < ringCount; ++i) {
        const GhostFrameSnapshot& f = ringCopy[i];
        char wCol[16];
        char cCol[16];
        char gCol[16];
        if (f.wallTimeValid) {
            snprintf(wCol, sizeof(wCol), "%8.2f", f.frameWallMs);
            snprintf(cCol, sizeof(cCol), "%8.2f", f.cpuActiveMs);
        } else {
            snprintf(wCol, sizeof(wCol), "     n/a");
            snprintf(cCol, sizeof(cCol), "     n/a");
        }
        if (f.gpuQueryValid) {
            snprintf(gCol, sizeof(gCol), "%8.2f", f.gpuFrameMs);
        } else {
            snprintf(gCol, sizeof(gCol), "     n/a");
        }
        fprintf(fp,
            "    #%6u   %s    %s  %s  %8.2f  %8.2f  %5u  %4u  %4u  %6.2f %6.2f %6.2f\n",
            f.swapFrame, wCol, cCol, gCol, f.presentMs, f.paceWaitMs,
            f.totalDraws, f.xvuDraws, f.unifiedDraws,
            f.decodeUnifiedMs, f.submitXvuMs, f.runPushBufferSelfMs);
    }
    fprintf(fp, "\n");

    fflush(fp);
    fclose(fp);
    ReleaseSRWLockExclusive(&g_profDumpFileLock);

    if (outSavedPathW && outMaxChars > 0u) {
        wcsncpy_s(outSavedPathW, outMaxChars, dumpPathW, _TRUNCATE);
    }
    if (outSnapshotIdx) {
        *outSnapshotIdx = snapIdx;
    }

    wchar_t toastMsg[192] = {};
    swprintf(toastMsg, 192u,
        L"[F10] Saved %s\\profile_dump.txt (Snapshot #%u)",
        kAppDataFolderNameW, snapIdx);
    SetPerfToastBanner(toastMsg, 3500u);
    return true;
}
