#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define GHOST_STR_IMPL(x)   #x
#define GHOST_STR(x)        GHOST_STR_IMPL(x)
#define GHOST_WIDEN_IMPL(x) L#x
#define GHOST_WIDEN(x)      GHOST_WIDEN_IMPL(x)
#ifndef GHOST_PROJECT_NAME
#define GHOST_PROJECT_NAME  GhostXR
#endif
static constexpr const wchar_t* kAppDataFolderNameW         = GHOST_WIDEN(GHOST_PROJECT_NAME) L"_Data";
static constexpr size_t         kMaxHostPath                = 1024u;
static constexpr uintptr_t      kMinValidHostAddress        = 0x00010000u;
static constexpr uint32_t       kCurrentShaderCacheVersion  = 5u;
static constexpr uint64_t       kXboxCpuClockHz             = 733333333ULL; // 733.333 MHz Pentium III TSC
static constexpr uint64_t       kXboxAcpiTimerHz            = 3375000ULL;   // 3.375 MHz MCPX ACPI PM Timer

// Global handles and paths
extern HWND g_hWnd;
extern HINSTANCE g_hInstance;
extern char g_appRootDir[kMaxHostPath];
extern char g_gameRootDir[kMaxHostPath];
extern volatile bool g_appRunning;
extern volatile DWORD g_mainGameThreadId;
extern volatile uint32_t g_keTickCount;
extern volatile uint32_t g_swapFrameCount;

// UTF-8 <-> UTF-16 host filesystem helpers & static image bounds guard
inline bool Utf8ToWide(const char* src, wchar_t* dst, size_t dstChars) {
    if (!dst || dstChars == 0) return false;
    dst[0] = L'\0';
    if (!src || !src[0]) return true;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, static_cast<int>(dstChars));
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, static_cast<int>(dstChars));
    }
    dst[dstChars - 1] = L'\0';
    return n > 0;
}

inline bool WideToUtf8(const wchar_t* src, char* dst, size_t dstBytes) {
    if (!dst || dstBytes == 0) return false;
    dst[0] = '\0';
    if (!src || !src[0]) return true;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, static_cast<int>(dstBytes), nullptr, nullptr);
    dst[dstBytes - 1] = '\0';
    return n > 0;
}

inline FILE* OpenFileUtf8(const char* pathUtf8, const wchar_t* modeW) {
    wchar_t pathW[kMaxHostPath];
    if (!Utf8ToWide(pathUtf8, pathW, kMaxHostPath) || !pathW[0]) return nullptr;
    return _wfopen(pathW, modeW);
}

inline DWORD GetFileAttributesUtf8(const char* pathUtf8) {
    wchar_t pathW[kMaxHostPath];
    if (!Utf8ToWide(pathUtf8, pathW, kMaxHostPath) || !pathW[0]) return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesW(pathW);
}

inline BOOL DeleteFileUtf8(const char* pathUtf8) {
    wchar_t pathW[kMaxHostPath];
    if (!Utf8ToWide(pathUtf8, pathW, kMaxHostPath) || !pathW[0]) return FALSE;
    return DeleteFileW(pathW);
}

inline BOOL RemoveDirectoryUtf8(const char* pathUtf8) {
    wchar_t pathW[kMaxHostPath];
    if (!Utf8ToWide(pathUtf8, pathW, kMaxHostPath) || !pathW[0]) return FALSE;
    return RemoveDirectoryW(pathW);
}

inline BOOL SetCurrentDirectoryUtf8(const char* pathUtf8) {
    wchar_t pathW[kMaxHostPath];
    if (!Utf8ToWide(pathUtf8, pathW, kMaxHostPath) || !pathW[0]) return FALSE;
    return SetCurrentDirectoryW(pathW);
}

inline bool IsStaticXboxImageOrNullPtr(const void* ptr) {
    uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    if (a < 0x00010000u) return true;
    static uintptr_t s_hostImageEnd = 0;
    if (s_hostImageEnd == 0) {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(0x00400000u);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) && dos->e_lfanew < 0x1000) {
            const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(0x00400000u + static_cast<uintptr_t>(dos->e_lfanew));
            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.SizeOfImage >= 0x00551000u) {
                s_hostImageEnd = 0x00400000u + static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
            }
        }
        if (s_hostImageEnd == 0) {
            s_hostImageEnd = 0x00960000u;
        }
    }
    return (a >= 0x00400000u && a < s_hostImageEnd);
}

inline int ResolveDistinguishedVk(WPARAM wParam, LPARAM lParam) {
    int vk = static_cast<int>(wParam);
    if (vk == VK_SHIFT) {
        UINT scancode = (static_cast<UINT>(lParam) >> 16) & 0xFFu;
        UINT mapped = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
        if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
            return static_cast<int>(mapped);
        }
        return VK_LSHIFT;
    }
    if (vk == VK_CONTROL) {
        return (lParam & (1L << 24)) ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU) {
        return (lParam & (1L << 24)) ? VK_RMENU : VK_LMENU;
    }
    return vk;
}

// Pre-launch Win32 UI configuration & In-Game OSD state (osd.cpp)
extern int g_selectedAdapterIndex;
extern wchar_t g_selectedAdapterName[128];
extern bool g_enableHavokDebugServer;
extern volatile bool g_osdShowFps;
extern volatile float g_osdCurrentFps;
void InitOsdWindowHooks(HWND hWnd);
void SyncOsdSystemMenuCheckmarks(HWND hWnd);
bool HandleOsdSysCommand(HWND hWnd, WPARAM wParam);
bool HandleOsdTimer(HWND hWnd, WPARAM wParam);
void DrawOsdOverlayOnBackbuffer();

struct GhostKeyBindings {
    int vkCrouch;      // L-Trigger (bAnalogButtons[6])
    int vkFire;        // R-Trigger (bAnalogButtons[7])
    int vkMoveUp;      // Left Stick Up (+LY)
    int vkMoveDown;    // Left Stick Down (-LY)
    int vkMoveLeft;    // Left Stick Left (-LX)
    int vkMoveRight;   // Left Stick Right (+LX)
    int vkSight;       // D-Pad Up
    int vkMenuStart;   // Start button
    int vkMenuBack;    // Back button
    int vkCloak;       // D-Pad Left
    int vkGunLight;    // D-Pad Down (Gun Light / Psi Charge)
    int vkDpadRight;   // D-Pad Right (--)
    int vkAimUp;       // Right Stick Up (+RY)
    int vkAimDown;     // Right Stick Down (-RY)
    int vkAimLeft;     // Right Stick Left (-RX)
    int vkAimRight;    // Right Stick Right (+RX)
    int vkAimClick;    // Right Stick Click (Sniper zoom)
    int vkMelee;       // X button (Melee Attack)
    int vkWeaponMode;  // Y button (Weapon Mode)
    int vkAction;      // B button (Action)
    int vkJump;        // A button (Jump)
    int vkNextFirearm; // Black button (Next Firearm)
    int vkNextGrenade; // White button (Next Grenade)
};

struct KeyBindSlotMeta {
    const wchar_t* iniKey;
    const wchar_t* shortLabel; // Sub-label inside box (or empty if single-slot box)
    const wchar_t* actionDesc; // Full description for status bar
    int*           pVk;        // Pointer into g_editKeyBindings
    RECT           rcSlot;     // Clickable key box rect on the 900x608 dialog canvas
};

static constexpr size_t kNumKeyBindSlots = 23;
static_assert(sizeof(GhostKeyBindings) == kNumKeyBindSlots * sizeof(int),
              "GhostKeyBindings field count must match kNumKeyBindSlots");
extern GhostKeyBindings g_keyBindings;
extern GhostKeyBindings g_editKeyBindings;
extern KeyBindSlotMeta  g_keySlots[kNumKeyBindSlots];

void ResetKeyBindingsToDefault(GhostKeyBindings* kb);
bool ShowLauncherDialog(HINSTANCE hInst);
bool LoadGhostXboxPeImage(const char* ghostExePath);

// Shader Cache Backend (d3d8_shader.cpp)
typedef bool (*PFN_ShaderCompileProgressCallback)(uint32_t doneCount, uint32_t totalCount, const char* currentShaderName, void* userData);
bool IsShaderCacheReadyOnDisk();
void ClearShaderCacheDirectory();
bool CompileAllGameShadersToCache(PFN_ShaderCompileProgressCallback progressCb, void* userData);

// Config & Path Helpers (config.cpp)
void TrimTrailingSlashesW(wchar_t* path);
void JoinPathW(wchar_t* outBuf, size_t maxCount, const wchar_t* baseDir, const wchar_t* subPath);
void GetExeDirW(wchar_t* outDirW, size_t maxCount);
void GetAppDataAndIniPathsW(wchar_t* outDataDirW, size_t dataDirMax, wchar_t* outIniPathW, size_t iniPathMax);
bool IsValidGameDirW(const wchar_t* dirW);
void FormatVkNameW(int vk, wchar_t* outBuf, size_t maxCount);
void LoadKeyBindingsFromIniW(const wchar_t* iniPathW);
void SaveKeyBindingsToIniW(const wchar_t* iniPathW);
void SaveOsdStateToIni();
bool IsShaderCacheCompiledInIni();
void SetShaderCacheCompiledInIni(bool compiled);
void PersistAndInitUserDataDir(const wchar_t* gameDirW);

// Hooking / Bridging API (patch.cpp)
// Overwrites the first 5 bytes of an Xbox function at `xboxVA` with `JMP rel32` to `hostFn`.
void InstallBridge(uint32_t xboxVA, const void* hostFn);

// Writes raw bytes to an Xbox virtual address
void PatchBytes(uint32_t xboxVA, const void* data, size_t len);
void PatchU32(uint32_t xboxVA, uint32_t val);
void PatchRet(uint32_t xboxVA, uint16_t stackBytes = 0, uint32_t retVal = 0, bool setEax = false);

// Path translation helper: translates Xbox paths ("d:\...", "u:\...", "t:\...", "z:\...")
// and remaps Xbox-specific .xvu vertex shader paths to standard vs_1_1 .xvu files.
void EnsureDirExists(const char* dirPath);
void TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t outMaxLen);
void RegisterPeSection(const char* name8, uint32_t va, uint32_t vsize);

// Shared Xbox Kernel / XAPI Internal Helpers (xbox_kernel.cpp / xbox_xapi.cpp)
typedef void (*XboxMemoryFreeCallback)(const void* ptr, size_t byteLen);
void RegisterXboxMemoryFreeCallback(XboxMemoryFreeCallback cb);
void NotifyXboxMemoryFreed(const void* ptr, size_t byteLen);
void* AllocAlignedTracked(uint32_t size, uint32_t alignment);
void FreeAlignedTracked(void* ptr);
uint32_t SizeAlignedTracked(void* ptr);
void* __stdcall Bridge_XMemAlloc(SIZE_T dwSize, DWORD dwAllocAttributes);
void  __stdcall Bridge_XMemFree(void* pAddress, DWORD dwAllocAttributes);
void __stdcall Krnl_XcSHAInit(uint8_t* pbSHAContext);
void __stdcall Krnl_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength);
void __stdcall Krnl_XcSHAFinal(uint8_t* pbSHAContext, uint8_t* pbDigest);
void __stdcall Krnl_XcHMAC(
    const uint8_t* pbKeyMaterial, uint32_t cbKeyMaterial,
    const uint8_t* pbInput, uint32_t dwInputLength,
    const uint8_t* pbInput2, uint32_t dwInputLength2,
    uint8_t* pbDigest);
void __stdcall Krnl_DbgBreakPoint();
int32_t __stdcall Krnl_ExQueryNonVolatileSetting(
    uint32_t ValueIndex, uint32_t* Type, void* Value, uint32_t ValueLength, uint32_t* ResultLength);
BOOL __stdcall Bridge_CloseHandle(HANDLE hObject);
uint64_t QueryXboxCpuPerformanceCounterTicks();
uint64_t QueryXboxCpuPerformanceFrequencyTicks();
uint64_t QueryXboxAcpiPerformanceCounterTicks();
uint64_t QueryXboxAcpiPerformanceFrequencyTicks();
uint64_t QueryXbox100NsInterruptTime();
uint64_t QueryXbox733MHzCyclesTimes3Shared();
uint64_t QueryXboxPerformanceCounterTicks();
uint64_t QueryXboxPerformanceFrequencyTicks();
uint32_t QueryXboxTickCountMs();
void PumpXboxKernelTimers();
bool IsXboxKernelClockThreadRunning();
uint32_t __cdecl GetXboxSimulatedDpcIrql();
struct XBOX_CRITICAL_SECTION;
void __stdcall Bridge_RtlInitializeCriticalSection(XBOX_CRITICAL_SECTION* cs);
uint8_t __stdcall Bridge_RtlTryEnterCriticalSection(XBOX_CRITICAL_SECTION* cs);
void __stdcall Bridge_RtlEnterCriticalSection(XBOX_CRITICAL_SECTION* cs);
void __stdcall Bridge_RtlLeaveCriticalSection(XBOX_CRITICAL_SECTION* cs);
void CleanupCurrentXboxThreadState();
HANDLE SpawnManagedXboxThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId,
    void (__stdcall *systemRoutine)(void*, void*) = nullptr,
    bool isXapiThread = true);
void InitXboxKernel();
void ShutdownXboxKernel();
void InitXboxXapi();

// Shared Xbox Audio Structures & Helpers (audio_adpcm.cpp / audio_buffer.cpp / audio_stream.cpp)
#define XBOX_WAVE_FORMAT_XBOX_ADPCM 0x0069

static constexpr LONG  kMinDirectSoundVolumeMb = -10000;
static constexpr LONG  kMaxDirectSoundVolumeMb = 0;
static constexpr LONG  kMinDirectSoundPanMb    = -10000;
static constexpr LONG  kMaxDirectSoundPanMb    = 10000;
static constexpr DWORD kMinDirectSoundFreqHz   = 100u;
static constexpr DWORD kMaxDirectSoundFreqHz   = 100000u;
static constexpr DWORD kMaxXboxPitchFreqHz     = 192000u;
static constexpr DWORD kDefaultSampleRateHz    = 48000u;

#pragma pack(push, 1)
struct XBOX_DSMIXBINVOLUMEPAIR {
    uint32_t dwMixBin;
    int32_t  lVolume;
};

struct XBOX_DSMIXBINS {
    uint32_t                       dwMixBinCount;
    const XBOX_DSMIXBINVOLUMEPAIR* lpMixBinVolumePairs;
};

struct XBOX_DSFILTERDESC {
    uint32_t dwMode;              // +0x00: 0 = BYPASS, 1 = DLS2, 2 = PARAMEQ, 3 = MULTI
    uint32_t dwQCoefficient;      // +0x04
    uint32_t adwCoefficients[4];  // +0x08..+0x17
};
static_assert(sizeof(XBOX_DSFILTERDESC) == 0x18, "XBOX_DSFILTERDESC must be 0x18 bytes");

struct XBOX_DSVOICEPROPS {
    uint32_t                dwMixBinCount;        // +0x00
    XBOX_DSMIXBINVOLUMEPAIR MixBinVolumePairs[8]; // +0x04 (64 bytes)
    int32_t                 lPitch;               // +0x44
    int32_t                 l3DDistanceVolume;    // +0x48
    int32_t                 l3DConeVolume;        // +0x4C
    int32_t                 l3DDopplerPitch;      // +0x50
    int32_t                 lI3DL2DirectVolume;   // +0x54
    int32_t                 lI3DL2RoomVolume;     // +0x58
};
static_assert(sizeof(XBOX_DSVOICEPROPS) == 0x5C, "XBOX_DSVOICEPROPS must be 0x5C bytes");

struct XBOX_DSLFODESC {
    uint32_t dwLFO;                 // +0x00: 0 = DSLFO_MULTI, 1 = DSLFO_PITCH
    uint32_t dwDelay;               // +0x04: delay in 32-sample frames (0..0x7FFF)
    uint32_t dwDelta;               // +0x08: phase increment per frame (0..0x3FF)
    int32_t  lPitchModulation;      // +0x0C: [-128..127]
    union {
        int32_t lFcModulation;      // +0x10: [-128..127]
        int32_t lFilterCutOffRange; // +0x10: [-128..127]
    };
    int32_t  lAmplitudeModulation;  // +0x14: [-128..127]
};
static_assert(sizeof(XBOX_DSLFODESC) == 0x18, "XBOX_DSLFODESC must be 0x18 bytes");

struct XBOX_DSENVELOPEDESC {
    uint32_t dwEG;                  // +0x00: 0 = DSEG_MULTI (pitch/cutoff), 1 = DSEG_AMPLITUDE (volume)
    uint32_t dwMode;                // +0x04: 0 = DISABLE, 1 = DELAY, 2 = ATTACK, 3 = HOLD, 4 = DECAY, 5 = RELEASE
    uint32_t dwDelay;               // +0x08: 0..0xFFF (512-sample blocks)
    uint32_t dwAttack;              // +0x0C: 0..0xFFF
    uint32_t dwHold;                // +0x10: 0..0xFFF
    uint32_t dwDecay;               // +0x14: 0..0xFFF
    uint32_t dwRelease;             // +0x18: 0..0xFFF
    uint32_t dwSustain;             // +0x1C: 0..0xFF
    int32_t  lPitchScale;           // +0x20: [-128..127]
    int32_t  lFilterCutOff;         // +0x24: [-128..127]
};
static_assert(sizeof(XBOX_DSENVELOPEDESC) == 0x28, "XBOX_DSENVELOPEDESC must be 0x28 bytes");

struct XBOX_DSI3DL2LISTENER {
    int32_t lRoom;
    int32_t lRoomHF;
    float   flRoomRolloffFactor;
    float   flDecayTime;
    float   flDecayHFRatio;
    int32_t lReflections;
    float   flReflectionsDelay;
    int32_t lReverb;
    float   flReverbDelay;
    float   flDiffusion;
    float   flDensity;
    float   flHFReference;
};

struct XBOX_DSEFFECTMAP {
    void*    lpvCodeSegment;        // +0x00
    uint32_t dwCodeSize;            // +0x04
    void*    lpvStateSegment;       // +0x08
    uint32_t dwStateSize;           // +0x0C
    void*    lpvScratchSegment;     // +0x10
    uint32_t dwScratchSize;         // +0x14
    uint32_t dwFlags;               // +0x18
    uint32_t dwReserved;            // +0x1C
};
static_assert(sizeof(XBOX_DSEFFECTMAP) == 0x20, "XBOX_DSEFFECTMAP must be 0x20 bytes");

struct XBOX_DSEFFECTIMAGEDESC {
    uint32_t         dwEffectCount;      // +0x00
    uint32_t         dwTotalScratchSize; // +0x04
    XBOX_DSEFFECTMAP aEffectMaps[1];     // +0x08
};
#pragma pack(pop)

struct Xbox3DVoiceMetrics {
    LONG     pan;
    LONG     totalAttenMb;
    int32_t  distanceVol;
    int32_t  coneVol;
    int32_t  dopplerPitch;
    int32_t  i3dl2DirectVol;
    int32_t  i3dl2RoomVol;
    int32_t  itdDelaySamples;
    float    hrtfFilterCoeff;
    bool     active3D;
};

struct XboxVoiceFilterState {
    static constexpr size_t kItdRingSize = 64u; // Covers full 0..63 sample MCPX ITD delay at 48 kHz
    float   low[2];
    float   band[2];
    float   low2[2];
    float   band2[2];
    float   hrtfLow[2];
    float   itdRingL[kItdRingSize];
    float   itdRingR[kItdRingSize];
    uint8_t itdWritePos;
};

struct XboxDSObject {
    void** vtable;
    LONG   refCount;
};

static constexpr uint32_t kAudioMixSubQuantumFrames = 64u;
static constexpr uint32_t kAudioMaxQuantumFrames    = 2048u;

extern CRITICAL_SECTION g_audioCs;
extern volatile LONG    g_audioCsInitState;
void EnsureXboxAudioCsInitialized();
inline bool IsXboxAudioCsInitialized() {
    return InterlockedCompareExchange(&g_audioCsInitState, 0, 0) == 2;
}

const XBOX_DSI3DL2LISTENER& GetXboxI3DL2ListenerState();
DWORD XboxPitchToFrequencyHz(LONG lPitch);
LONG  FrequencyHzToXboxPitch(DWORD freqHz);
struct XboxDSBufferObject;
void  Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb, Xbox3DVoiceMetrics* outMetrics = nullptr);
void  ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan, LONG* outAttenMb = nullptr);
void  InitVoiceDefaultMixBins(uint32_t dwFlags, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount);
void  AssignVoiceMixBins(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount);
void  UpdateVoiceMixBinVolumes(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR ioPairs[8], uint32_t* ioCount);
void  ComputeMixBinPairsPan(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, LONG* outPan, LONG* outAttenMb = nullptr);
void  FillVoicePropsMixBins(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, XBOX_DSVOICEPROPS* outProps);
void  ExtractXboxMixBinI3DL2Send(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, bool* outHasSend, int32_t* outSendMb);
void  ClearStreamsReferencingOutputBufferLocked(XboxDSBufferObject* buf);
void  EnsureXboxFxSendTargetsAllocatedLocked(uint32_t fxSendBinMask);
bool  AccumulateToXboxSubmixBufferLocked(
    XboxDSBufferObject* targetBuf, uint32_t fxSendBinMask,
    const float* inL, const float* inR, uint32_t frameOffset, uint32_t numFrames);
bool  ComputeDls2FilterCoeffs(
    const XBOX_DSFILTERDESC* pFilter, uint32_t sampleRateHz,
    float* outF, float* outD,
    uint32_t* outMode = nullptr, float* outF2 = nullptr, float* outD2 = nullptr);
void  ApplyVoiceSvfAndHrtfFloatStereo(
    float* ioLeft, float* ioRight, uint32_t numFrames,
    const XBOX_DSFILTERDESC* pFilter, bool hasActiveFilter, float lfoEgCutoffRatio,
    bool active3D, float hrtfFilterCoeff, int32_t itdDelaySamples,
    XboxVoiceFilterState* ioState);
void  ProcessGlobalI3DL2Reverb48k(
    const float* wetSendL, const float* wetSendR,
    float* ioMasterDryL, float* ioMasterDryR, uint32_t numFrames,
    const XBOX_DSI3DL2LISTENER* listenerSnapshot);
void  ProcessGlobalI3DL2Reverb48kLocked(
    const float* wetSendL, const float* wetSendR,
    float* ioMasterDryL, float* ioMasterDryR, uint32_t numFrames);
void  ResetGlobalI3DL2Reverb48kLocked();
void  EvaluateXboxVoiceLfoAndEgSamples(
    const XBOX_DSLFODESC lfo[2], const bool lfoActive[2],
    const XBOX_DSENVELOPEDESC eg[2], const bool egActive[2],
    uint64_t activeSamples48k, bool isReleasing, uint64_t releaseSamples48k,
    int32_t* outVolModMb, float* outPitchRatio, float* outFilterCutoffRatio,
    bool* outAmpReleaseDone = nullptr);
void  EvaluateXboxVoiceLfoAndEg(
    const XBOX_DSLFODESC lfo[2], const bool lfoActive[2],
    const XBOX_DSENVELOPEDESC eg[2], const bool egActive[2],
    uint32_t elapsedMs, bool isReleasing, uint32_t releaseElapsedMs,
    int32_t* outVolModMb, float* outPitchRatio, float* outFilterCutoffRatio);
void  DecodeXboxAdpcmBlockToStereoFloat(
    const uint8_t* blockPtr, uint32_t channels, float outLeft[64], float outRight[64]);
void  SamplePcmFrameToStereoFloat(
    const uint8_t* rawBase, uint32_t frameIdx, uint32_t channels, uint16_t bitsPerSample,
    float* outLeft, float* outRight);
void  UnpackXboxWaveToStereoPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels,
    bool isAdpcm, uint16_t bitsPerSample, int16_t* dstStereo, uint32_t maxFrames, uint32_t* outFrames);
void  PrepareXboxSubmixBuffers48kLocked(uint32_t numFrames);
void  MixActiveXboxBuffers48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames);
void  MixActiveXboxStreams48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames);
void  FinalizeXboxSubmixBuffers48kLocked(
    float* ioMasterDryL, float* ioMasterDryR,
    float* ioReverbSendL, float* ioReverbSendR, uint32_t numFrames);
void  EnsureXboxWasapiMixerStarted();
void  ShutdownXboxAudioBuffersLocked();
uint32_t QueryXboxMcpxSampleCounter();
void  SetXboxAudioManualMixModeForTest(bool manualOnly);
void  PumpXboxAudioMixerFramesForTest(uint32_t totalFrames, float* outInterleavedStereo = nullptr);
void  InitXboxAudioBuffers();

// Subsystem initialization functions
bool InitD3D11HostDevice();
void InitXboxKernelAndXapi();
void InitXboxInput();
void InitXboxD3D8();
void InitXboxAudio();
void ShutdownXboxAudio();
void InitXboxVideo();

// Per-frame tick updates
void UpdateXboxKernelTick();
void UpdateXboxInputTick();
void NotifyHostDeviceChange();
void UpdateXboxAudioTick();
void StopAllControllerVibration();
void PushDebugKeystroke(uint16_t vk, wchar_t ch, bool keyUp);
