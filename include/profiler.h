#pragma once

#include "ghost_runtime.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// GhostXR Real-Time Performance Panel (F3) & Frame Snapshot Profiler (F10)
// ============================================================================

// Verified binary layout of Ghost.exe's per-variable profiler entry
// (allocated as 0x178 bytes in cProfiler::AddVariable at 0x005A8D60).
#pragma pack(push, 1)
struct XboxProfileVar {
    char     name[32];        // +0x00: Null-terminated ASCII variable name
    uint32_t valueType;       // +0x20: 0 = Float (seconds!), 1 = Int, 2 = UInt, 3 = String ("Notes")
    uint32_t samples[80];     // +0x24: 80-slot ring buffer (or const char* at samples[0] when valueType == 3)
    uint32_t count;           // +0x164: Valid sample count (0..80)
    uint32_t head;            // +0x168: Oldest sample index (0..79); latest is (head + count - 1) % 80
    uint32_t resetPerFrame;   // +0x16C: Non-zero if cProfiler::EndFrame pushes a fresh 0 each frame
    int64_t  qpcStart;        // +0x170: LARGE_INTEGER start timestamp written by cProfiler::StartTimer
};
static_assert(sizeof(XboxProfileVar) == 0x178, "XboxProfileVar must match Ghost.exe 0x178-byte layout");
static_assert(offsetof(XboxProfileVar, valueType)     == 0x20,  "XboxProfileVar::valueType offset mismatch");
static_assert(offsetof(XboxProfileVar, samples)       == 0x24,  "XboxProfileVar::samples offset mismatch");
static_assert(offsetof(XboxProfileVar, count)         == 0x164, "XboxProfileVar::count offset mismatch");
static_assert(offsetof(XboxProfileVar, head)          == 0x168, "XboxProfileVar::head offset mismatch");
static_assert(offsetof(XboxProfileVar, resetPerFrame) == 0x16C, "XboxProfileVar::resetPerFrame offset mismatch");
static_assert(offsetof(XboxProfileVar, qpcStart)      == 0x170, "XboxProfileVar::qpcStart offset mismatch");

// Verified binary layout of Ghost.exe's cProfiler singleton (*0x00880438, 0xB4 bytes at 0x005A9330).
struct XboxProfilerObj {
    void**           vtable;          // +0x00
    XboxProfileVar** m_vars;          // +0x04: Array of eProfileVars pointers (0..45)
    uint32_t         m_capacity;      // +0x08
    uint32_t         m_growStep;      // +0x0C
    int32_t          m_count;         // +0x10: Registered variable count (46 = 0x2E)
    uint8_t          m_pad14[0xA0];   // +0x14..+0xB3 (log FILE*, flags, resource log handle)
};
static_assert(sizeof(XboxProfilerObj) == 0xB4, "XboxProfilerObj must match Ghost.exe 0xB4-byte layout");
static_assert(offsetof(XboxProfilerObj, m_vars)  == 0x04, "XboxProfilerObj::m_vars offset mismatch");
static_assert(offsetof(XboxProfilerObj, m_count) == 0x10, "XboxProfilerObj::m_count offset mismatch");
#pragma pack(pop)

static constexpr uint32_t kMaxXboxProfileVars      = 48u;
static constexpr uint32_t kMaxTrackedVsHandles     = 512u;
static constexpr uint32_t kTopVsHandlesInSnapshot  = 16u;
static constexpr uint32_t kProfileFrameRingSize    = 60u;

struct PerfVsHandleStat {
    uint32_t handle;
    uint32_t stride;
    int32_t  vsType;
    bool     isXvu;
    bool     used;
    uint32_t draws;
    uint32_t verts;
    uint32_t staticVbHits;
    int64_t  cpuTicks;
    float    cpuMs;
};

// Game-thread per-frame accumulator (mutated exclusively on the rendering thread, zero locks)
struct GhostActiveFrameAccum {
    int64_t  decodeUnifiedTicks;
    int64_t  submitUnifiedTicks;
    int64_t  submitXvuTicks;
    int64_t  runPushBufferTotalTicks;
    int64_t  runPushBufferNestedTicks;
    int64_t  srvResolveTicks;
    int64_t  texUploadTicks;
    int64_t  gpuReadbackTicks;
    int64_t  visWaitTicks;
    int64_t  skinSetupTicks;
    int64_t  shaderCompileTicks;
    int64_t  compositeOsdTicks;
    int64_t  presentTicks;
    int64_t  paceWaitTicks;

    uint32_t pushBufferDepth;

    uint32_t xvuDraws;
    uint32_t xvuVerts;
    uint32_t xvuStaticVbDraws;
    uint32_t xvuDynamicSliceDraws;

    uint32_t unifiedDraws;
    uint32_t unifiedVerts;
    uint32_t unifiedRhwDraws;
    uint32_t unifiedRhwVerts;
    uint32_t unifiedFvf0x152Draws;
    uint32_t unifiedFvf0x152Verts;
    uint32_t unifiedFvf0x112Draws;
    uint32_t unifiedFvf0x112Verts;
    uint32_t unifiedOtherDraws;
    uint32_t unifiedOtherVerts;

    uint32_t unifiedGpuLitDraws;
    uint32_t unifiedCpuLitDraws;
    uint32_t unifiedGpuSkinDraws;
    uint32_t unifiedCpuSkinDraws;
    uint32_t unifiedPointSpriteDraws;
    uint32_t unifiedPointSpriteVerts;

    uint32_t pushBufferCalls;
    uint32_t pushBufferFixupCalls;
    uint32_t pushBufferDynRingCalls;
    uint32_t pushBufferBytesParsed;

    uint32_t skinSetupCalls;
    uint32_t skinSetupVerts;

    uint32_t rtSwitchCount;
    uint32_t clearFastCount;
    uint32_t clearQuadCount;

    uint32_t srvResolveCalls;
    uint32_t texUploadCount;
    uint32_t texUploadBytes;
    uint32_t shaderRuntimeCompiles;

    uint32_t dynVbDiscardCount;
    uint32_t dynStreamVbDiscardCount;
    uint32_t dynIbDiscardCount;

    uint32_t visBeginCount;
    uint32_t visGetResultCount;
    uint32_t visWaitCallCount;
    uint32_t visIncompleteCount;
    uint32_t visSpinCount;

    uint32_t vsOverflowDraws;
    uint32_t vsOverflowVerts;
    uint32_t vsOverflowStaticVbHits;
    int64_t  vsOverflowTicks;

    PerfVsHandleStat vsTable[kMaxTrackedVsHandles];
};

struct GhostFrameSnapshot {
    uint32_t swapFrame;
    bool     wallTimeValid;
    float    frameWallMs;
    float    cpuActiveMs;
    float    paceWaitMs;
    float    gpuFrameMs;
    bool     gpuQueryValid;
    uint32_t gpuDisjointCount;

    float    presentMs;
    float    compositeOsdMs;
    float    hostBridgeCpuMs;
    float    decodeUnifiedMs;
    float    submitUnifiedMs;
    float    submitXvuMs;
    float    runPushBufferTotalMs;
    float    runPushBufferSelfMs;
    float    srvResolveMs;
    float    texUploadMs;
    float    gpuReadbackMs;
    float    visWaitMs;
    float    skinSetupMs;
    float    shaderCompileMs;

    uint32_t totalDraws;
    uint32_t totalVerts;
    uint32_t xvuDraws;
    uint32_t xvuVerts;
    uint32_t xvuStaticVbDraws;
    uint32_t xvuDynamicSliceDraws;

    uint32_t unifiedDraws;
    uint32_t unifiedVerts;
    uint32_t unifiedRhwDraws;
    uint32_t unifiedRhwVerts;
    uint32_t unifiedFvf0x152Draws;
    uint32_t unifiedFvf0x152Verts;
    uint32_t unifiedFvf0x112Draws;
    uint32_t unifiedFvf0x112Verts;
    uint32_t unifiedOtherDraws;
    uint32_t unifiedOtherVerts;

    uint32_t unifiedGpuLitDraws;
    uint32_t unifiedCpuLitDraws;
    uint32_t unifiedGpuSkinDraws;
    uint32_t unifiedCpuSkinDraws;
    uint32_t unifiedPointSpriteDraws;
    uint32_t unifiedPointSpriteVerts;

    uint32_t pushBufferCalls;
    uint32_t pushBufferFixupCalls;
    uint32_t pushBufferDynRingCalls;
    uint32_t pushBufferBytesParsed;

    uint32_t skinSetupCalls;
    uint32_t skinSetupVerts;

    uint32_t staticVbHits;
    uint32_t staticIbHits;
    uint32_t staticPbHits;
    uint32_t psPointerHits;
    uint32_t backBufferCopies;
    uint32_t surfaceReadbacks;

    uint32_t rtSwitchCount;
    uint32_t clearFastCount;
    uint32_t clearQuadCount;
    uint32_t srvResolveCalls;
    uint32_t texUploadCount;
    uint32_t texUploadBytes;
    uint32_t shaderRuntimeCompiles;

    uint32_t dynVbDiscardCount;
    uint32_t dynStreamVbDiscardCount;
    uint32_t dynIbDiscardCount;

    uint32_t visBeginCount;
    uint32_t visGetResultCount;
    uint32_t visWaitCallCount;
    uint32_t visIncompleteCount;
    uint32_t visSpinCount;

    uint32_t cachedTextures;
    uint32_t cachedGpuBuffers;
    uint32_t viewportW;
    uint32_t viewportH;
    uint32_t lightingMode;
    bool     flipModel;
    bool     allowTearing;
    float    camPos[3];
    float    camFwd[3];

    uint32_t         totalDistinctVs;
    uint32_t         vsOverflowDraws;
    uint32_t         vsOverflowVerts;
    uint32_t         vsOverflowStaticVbHits;
    float            vsOverflowCpuMs;
    uint32_t         numTopVs;
    PerfVsHandleStat topVs[kTopVsHandlesInSnapshot];
};

struct XboxNativeVarSnapshot {
    bool     present;
    char     name[32];
    uint32_t valueType;     // 0=Float(ms after *1000), 1=Int, 2=UInt, 3=String
    uint32_t resetPerFrame;
    uint32_t sampleCount;
    double   latestVal;     // Converted to ms when valueType == 0
    double   prevVal;       // Previous ring slot (converted to ms when valueType == 0)
    double   avgVal;        // 80-slot average (converted to ms when valueType == 0)
    double   maxVal;        // 80-slot max (converted to ms when valueType == 0)
    char     noteText[128]; // Populated when valueType == 3
};

struct XboxNativeProfilerSnapshot {
    bool                  valid;
    bool                  capturedPreReset;
    uint32_t              varCount;
    uint32_t              swapFrameCaptured;
    double                physicsSimUpdateMs;   // Var 0x0A slot N-2 (simPhysics_Update)
    double                physicsConstraintMs;  // Var 0x0A slot N-1 (simPhysics_UpdateConstraintWorld)
    double                physicsCombinedMs;    // Sum of both physics steps in tSequence::MainLoop
    XboxNativeVarSnapshot vars[kMaxXboxProfileVars];
};

extern GhostActiveFrameAccum g_perfAccum;
extern volatile bool         g_osdShowPerfPanel;

inline int64_t PerfQpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

double PerfQpcTicksToMs(int64_t ticks);

// Records per-VS/FVF draw call statistics into g_perfAccum.vsTable (lock-free on game thread)
void RecordPerfVsDraw(
    uint32_t vsHandle, uint32_t stride, int32_t vsType, bool isXvu,
    uint32_t verts, bool staticVbHit, int64_t cpuTicks);

// Snapshots a Ghost.exe cProfiler instance (safe against null/uninitialized pointers)
void SnapshotXboxNativeProfilerFromPtr(const void* profilerPtr, XboxNativeProfilerSnapshot* outSnap);

// Hook target for CALL ?EndFrame@cProfiler@@QAEXXZ at 0x0043C830 in tSequence::MainLoop
void __fastcall Bridge_ProfilerEndFrame(void* thisPtr, void* edxUnused);

// Non-blocking 6-slot D3D11 GPU frame timestamp query ring (never flushes or stalls WDDM)
void BeginGpuFrameTimestampIfReady();
void EndAndPollGpuFrameTimestamp(float* outGpuMs, bool* outValid, uint32_t* outDisjointCount);
void ReleaseGpuTimestampQueries();

// Called at the end of each finishing Bridge_D3DDevice_Swap after PaceSwapToXbox60Hz()
void EndHostProfileFrameAndLatch(float gpuFrameMs, bool gpuQueryValid, uint32_t gpuDisjointCount);

// Resets all profiler accumulators, ring history, and snapshot counters (used by tests / device reset)
void ResetProfilerState();

// Copies the latest latched frame & native profiler snapshots (returns false if no frames latched yet)
bool GetLatestProfileSnapshots(GhostFrameSnapshot* outFrame, XboxNativeProfilerSnapshot* outNative);

// Formats the compact multi-line F3 real-time breakdown panel text (fits comfortably inside 640x480)
void FormatPerfPanelTextW(wchar_t* outBuf, size_t maxChars);

// Returns true and populates outBuf if an F10 confirmation banner is currently active
bool GetActivePerfToastTextW(wchar_t* outBuf, size_t maxChars);

// Toggles the F3 real-time performance panel and syncs the window System Menu checkmark
void TogglePerfPanel(HWND hWnd);

// Exports a full diagnostic snapshot to <AppDataDir>\profile_dump.txt and activates the OSD confirmation banner
bool TriggerProfileSnapshotDump(
    wchar_t* outSavedPathW = nullptr, size_t outMaxChars = 0, uint32_t* outSnapshotIdx = nullptr);

// Returns true if `vk` is currently bound to any of the 23 gameplay controls in g_keyBindings
bool IsVkBoundInGameplayBindings(int vk);

// Processes WM_KEYDOWN/WM_SYSKEYDOWN/WM_KEYUP/WM_SYSKEYUP for VK_F3 & VK_F10 with paired
// key-down/key-up tracking so forwarded debug keys can never stick down in Ghost.exe.
// Returns true if consumed by the host profiler, false if caller should forward to PushDebugKeystroke.
bool HandleProfilerWindowKeyMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void NotifyProfilerWindowFocusLost();

// Camera/View helper implemented in d3d8_draw.cpp for diagnostic context
void GetActiveSceneCameraSnapshot(float outCamPos[3], float outCamFwd[3]);
