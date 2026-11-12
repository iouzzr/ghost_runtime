#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <atomic>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ============================================================================
// LIBCMT Thread-Local Storage (__getptd / __freeptd / __mtinit)
// ============================================================================

static volatile LONG g_crtTlsIndex = static_cast<LONG>(TLS_OUT_OF_INDEXES);

static DWORD EnsureXboxCrtTlsAllocated() {
    LONG cur = InterlockedCompareExchange(&g_crtTlsIndex, 0, 0);
    if (static_cast<DWORD>(cur) != TLS_OUT_OF_INDEXES) {
        return static_cast<DWORD>(cur);
    }
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES) {
        return TLS_OUT_OF_INDEXES;
    }
    LONG prev = InterlockedCompareExchange(
        &g_crtTlsIndex, static_cast<LONG>(idx), static_cast<LONG>(TLS_OUT_OF_INDEXES));
    if (static_cast<DWORD>(prev) != TLS_OUT_OF_INDEXES) {
        TlsFree(idx);
        return static_cast<DWORD>(prev);
    }
    return idx;
}

static void* InitThreadPtd() {
    DWORD tlsIdx = EnsureXboxCrtTlsAllocated();
    if (tlsIdx == TLS_OUT_OF_INDEXES) {
        return nullptr;
    }
    void* existing = TlsGetValue(tlsIdx);
    if (existing) {
        return existing;
    }
    uint8_t* ptd = static_cast<uint8_t*>(calloc(1, 0x100));
    if (ptd) {
        *reinterpret_cast<uint32_t*>(ptd + 0x00) = GetCurrentThreadId();
        *reinterpret_cast<uint32_t*>(ptd + 0x04) = 0xFFFFFFFFu;
        *reinterpret_cast<uint32_t*>(ptd + 0x14) = 1u;          // _holdrand = 1
        *reinterpret_cast<uint32_t*>(ptd + 0x54) = 0x007D37E0u; // __initmbctable
        TlsSetValue(tlsIdx, ptd);
    }
    return ptd;
}

static void* __cdecl Bridge_crt_getptd() {
    DWORD tlsIdx = EnsureXboxCrtTlsAllocated();
    if (tlsIdx == TLS_OUT_OF_INDEXES) {
        return nullptr;
    }
    void* ptd = TlsGetValue(tlsIdx);
    if (!ptd) {
        ptd = InitThreadPtd();
    }
    return ptd;
}

static void FreeXboxCrtSubBufferIfLoaded(void* subPtr) {
    if (!subPtr || IsStaticXboxImageOrNullPtr(subPtr)) {
        return;
    }
    // In Ghost.exe, LIBCMT _free is at 0x006947B8 (starts with `push 0Ch` = 0x6A 0x0C)
    const uint8_t* pFreeFn = reinterpret_cast<const uint8_t*>(0x006947B8u);
    if (pFreeFn[0] == 0x6Au && pFreeFn[1] == 0x0Cu) {
        typedef void (__cdecl *PFN_xbox_free)(void*);
        reinterpret_cast<PFN_xbox_free>(0x006947B8u)(subPtr);
    }
}

static void __cdecl Bridge_crt_freeptd(void* ptd) {
    DWORD tlsIdx = static_cast<DWORD>(InterlockedCompareExchange(&g_crtTlsIndex, 0, 0));
    if (!ptd && tlsIdx != TLS_OUT_OF_INDEXES) {
        ptd = TlsGetValue(tlsIdx);
        if (ptd) {
            TlsSetValue(tlsIdx, nullptr);
        }
    } else if (ptd && tlsIdx != TLS_OUT_OF_INDEXES && TlsGetValue(tlsIdx) == ptd) {
        TlsSetValue(tlsIdx, nullptr);
    }
    if (!ptd) {
        return;
    }

    // Mirror Ghost.exe __freeptd_helper (0x00695392): release any inner LIBCMT buffers
    // allocated via Xbox _malloc/_calloc_crt before freeing the host PTD block.
    uint8_t* bytes = static_cast<uint8_t*>(ptd);
    static const size_t kSubOffsets[] = { 0x24u, 0x2Cu, 0x34u, 0x3Cu, 0x44u, 0x48u };
    for (size_t off : kSubOffsets) {
        void* sub = *reinterpret_cast<void**>(bytes + off);
        if (sub) {
            *reinterpret_cast<void**>(bytes + off) = nullptr;
            FreeXboxCrtSubBufferIfLoaded(sub);
        }
    }
    void* mbcInfo = *reinterpret_cast<void**>(bytes + 0x54u);
    if (mbcInfo && reinterpret_cast<uintptr_t>(mbcInfo) != 0x007D37E0u) {
        *reinterpret_cast<uint32_t*>(bytes + 0x54u) = 0x007D37E0u;
        FreeXboxCrtSubBufferIfLoaded(mbcInfo);
    }

    free(ptd);
}

void CleanupCurrentXboxThreadState() {
    Bridge_crt_freeptd(nullptr);
}

static int __cdecl Bridge_crt_mtinit() {
    EnsureXboxCrtTlsAllocated();
    // Initialize LIBCMT's internal _locktable via __mtinitlocks (0x00697500) if Ghost.exe is mapped
    const uint8_t* pMtInitLocks = reinterpret_cast<const uint8_t*>(0x00697500u);
    if (pMtInitLocks[0] != 0x00u) {
        typedef int (__cdecl *PFN_mtinitlocks)();
        reinterpret_cast<PFN_mtinitlocks>(0x00697500u)();
    }
    InitThreadPtd();
    return 1;
}

// ============================================================================
// Xbox 28-byte RTL_CRITICAL_SECTION Support (WaitOnAddress / WakeByAddressSingle)
// ============================================================================

#pragma pack(push, 1)
struct XBOX_CRITICAL_SECTION {
    uint32_t      HeaderTypeAndSize; // +0x00
    volatile LONG State;             // +0x04: 0 = unlocked, 1 = locked uncontended, 2 = locked with waiters
    uint32_t      WaitListHead[2];   // +0x08..+0x0F
    volatile LONG LockCount;         // +0x10 (-1 = unlocked, >= 0 = locked)
    LONG          RecursionCount;    // +0x14
    volatile LONG OwningThread;      // +0x18
};
#pragma pack(pop)
static_assert(sizeof(XBOX_CRITICAL_SECTION) == 28, "XBOX_CRITICAL_SECTION must be 28 bytes");

// Both static .data critical sections (e.g. _XapiProcessLock at 0x007C7BBC) and zero-initialized
// structs start with State (+0x04) == 0 and OwningThread (+0x18) == 0.
void __stdcall Bridge_RtlInitializeCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->HeaderTypeAndSize = 0x00040001u;
    cs->WaitListHead[0]   = reinterpret_cast<uint32_t>(&cs->WaitListHead[0]);
    cs->WaitListHead[1]   = reinterpret_cast<uint32_t>(&cs->WaitListHead[0]);
    cs->LockCount         = -1;
    cs->RecursionCount    = 0;
    cs->OwningThread      = 0;
}

uint8_t __stdcall Bridge_RtlTryEnterCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return 0;
    LONG tid = static_cast<LONG>(GetCurrentThreadId());
    if (cs->OwningThread == tid && cs->RecursionCount > 0) {
        cs->RecursionCount++;
        InterlockedIncrement(&cs->LockCount);
        return 1;
    }
    if (InterlockedCompareExchange(&cs->State, 1, 0) == 0) {
        cs->OwningThread   = tid;
        cs->RecursionCount = 1;
        cs->LockCount      = 0;
        return 1;
    }
    return 0;
}

void __stdcall Bridge_RtlEnterCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    LONG tid = static_cast<LONG>(GetCurrentThreadId());
    if (cs->OwningThread == tid && cs->RecursionCount > 0) {
        cs->RecursionCount++;
        InterlockedIncrement(&cs->LockCount);
        return;
    }

    // 3-state futex (0 = unlocked, 1 = locked uncontended, 2 = locked with potential waiters)
    LONG c = InterlockedCompareExchange(&cs->State, 1, 0);
    if (c != 0) {
        if (c != 2) {
            c = InterlockedExchange(&cs->State, 2);
        }
        while (c != 0) {
            LONG expected = 2;
            if (!WaitOnAddress(const_cast<LONG*>(&cs->State), &expected, sizeof(LONG), INFINITE)) {
                if (GetLastError() == ERROR_INVALID_PARAMETER) {
                    SwitchToThread();
                }
            }
            c = InterlockedExchange(&cs->State, 2);
        }
    }

    cs->OwningThread   = tid;
    cs->RecursionCount = 1;
    cs->LockCount      = 0;
}

void __stdcall Bridge_RtlLeaveCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    LONG tid = static_cast<LONG>(GetCurrentThreadId());
    if (cs->OwningThread != tid) return;
    if (--cs->RecursionCount > 0) {
        InterlockedDecrement(&cs->LockCount);
        return;
    }

    cs->RecursionCount = 0;
    cs->OwningThread   = 0;
    cs->LockCount      = -1;
    LONG prev = InterlockedExchange(&cs->State, 0);
    if (prev == 2) {
        WakeByAddressSingle(const_cast<LONG*>(&cs->State));
    }
}

// ============================================================================
// Xbox Kernel Data Exports & Functions (0x0074FE80..0x007500A0)
// ============================================================================

volatile uint32_t g_keTickCount = 0;

static uint8_t  g_launchDataPage[0x1000] = { 0 };
static uint8_t  g_xboxEEPROMKey[16]      = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static uint8_t  g_xboxCERTKey[16]        = { 0x5C,0x07,0x33,0xAE,0x04,0x01,0xF7,0xE8,0x3C,0x97,0xF9,0x92,0x32,0x74,0x61,0xE0 };
static uint8_t  g_xboxHDKey[16]          = { 0 };
static uint8_t  g_xboxSignatureKey[16]   = { 0 };
static uint8_t  g_xboxLANKey[16]         = { 0 };
static uint8_t  g_xboxAltSignatureKeys[16][16] = { { 0 } };
static uint32_t g_idexChannelObj[32]     = { 0 };
static uint32_t g_halCachePartCount      = 3;
static uint32_t g_avPackStandard         = 6; // XC_AV_PACK_STANDARD
static uint16_t g_xboxKrnlVersion[4]     = { 1, 0, 5849, 1 };
static uint32_t g_dummyObjectType[16]    = { 0 };

struct XBOX_HARDWARE_INFO {
    uint32_t Flags;
    uint8_t  GpuRevision;
    uint8_t  McpRevision;
    uint8_t  Reserved[2];
};
static XBOX_HARDWARE_INFO g_xboxHwInfo = { 0, 0xA3, 0xD4, { 0, 0 } };

struct XBOX_OBJECT_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char*    Buffer;
};
static char g_xeImageNameBuf[] = "d:\\Ghost.xbe";
static XBOX_OBJECT_STRING g_xeImageFileName = {
    (uint16_t)(sizeof(g_xeImageNameBuf) - 1),
    (uint16_t)sizeof(g_xeImageNameBuf),
    g_xeImageNameBuf
};
static char g_diskModelBuf[] = "ST310014ACE";
static XBOX_OBJECT_STRING g_halDiskModelNumber = {
    (uint16_t)(sizeof(g_diskModelBuf) - 1),
    (uint16_t)sizeof(g_diskModelBuf),
    g_diskModelBuf
};
static char g_diskSerialBuf[] = "3HW0ABCD";
static XBOX_OBJECT_STRING g_halDiskSerialNumber = {
    (uint16_t)(sizeof(g_diskSerialBuf) - 1),
    (uint16_t)sizeof(g_diskSerialBuf),
    g_diskSerialBuf
};
static uint8_t g_xePublicKeyData[284] = { 0 };

void UpdateXboxKernelTick() {
    g_keTickCount = QueryXboxTickCountMs();
    if (!IsXboxKernelClockThreadRunning()) {
        PumpXboxKernelTimers();
    }
}

int32_t __stdcall Krnl_ExQueryNonVolatileSetting(
    uint32_t ValueIndex, uint32_t* Type, void* Value, uint32_t ValueLength, uint32_t* ResultLength)
{
    if (Type) *Type = 4; // REG_DWORD
    if (ResultLength) *ResultLength = 4;
    if (Value && ValueLength >= 4) {
        uint32_t v = 0;
        switch (ValueIndex) {
            case 0x0007: v = 1; break; // XC_LANGUAGE -> 1 (English)
            case 0x0008: v = 1; break; // XC_VIDEO_STANDARD -> NTSC_M
            case 0x0009: v = 0; break; // XC_AUDIO_FLAGS -> Stereo
            case 0x0104: v = 1; break; // XC_GAME_REGION -> NA
            default:     v = 0; break;
        }
        *reinterpret_cast<uint32_t*>(Value) = v;
    }
    return 0; // STATUS_SUCCESS
}

void __stdcall Krnl_DbgBreakPoint() {
}

static int __cdecl Krnl_DbgPrint(const char* fmt, ...) {
    (void)fmt;
    return 0;
}

// ============================================================================
// Hardware Clock Virtualization (733.333 MHz CPU TSC & 3.375 MHz MCPX ACPI PM)
// ============================================================================

static INIT_ONCE g_xboxQpcInitOnce = INIT_ONCE_STATIC_INIT;
static int64_t   g_xboxQpcBaseTick = 0;
static int64_t   g_xboxQpcFreq     = 10000000LL;

static BOOL CALLBACK InitXboxQpcBaseCallback(PINIT_ONCE, PVOID, PVOID*) {
    LARGE_INTEGER f = {}, c = {};
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        g_xboxQpcFreq = f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    g_xboxQpcBaseTick = c.QuadPart;
    return TRUE;
}

// Exact 128-bit unsigned (a * b) / den helper for 32-bit x86 (fast 64-bit path when hi == 0)
static uint64_t MulDivU64(uint64_t a, uint64_t b, uint64_t den) {
    if (den == 0) return 0;
    uint32_t a0 = static_cast<uint32_t>(a);
    uint32_t a1 = static_cast<uint32_t>(a >> 32);
    uint32_t b0 = static_cast<uint32_t>(b);
    uint32_t b1 = static_cast<uint32_t>(b >> 32);

    uint64_t p00 = static_cast<uint64_t>(a0) * b0;
    uint64_t p01 = static_cast<uint64_t>(a0) * b1;
    uint64_t p10 = static_cast<uint64_t>(a1) * b0;
    uint64_t p11 = static_cast<uint64_t>(a1) * b1;

    uint64_t mid = (p00 >> 32) + static_cast<uint32_t>(p01) + static_cast<uint32_t>(p10);
    uint64_t hi  = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    uint64_t lo  = (p00 & 0xFFFFFFFFULL) | (mid << 32);

    if (hi == 0) {
        return lo / den;
    }
    if (hi >= den) {
        hi %= den;
    }
    uint64_t q   = 0;
    uint64_t rem = hi;
    for (int bit = 63; bit >= 0; --bit) {
        uint64_t carry = rem >> 63;
        rem = (rem << 1) | ((lo >> bit) & 1ULL);
        if (carry || rem >= den) {
            rem -= den;
            q |= (1ULL << bit);
        }
    }
    return q;
}

static inline void QueryElapsedHostQpcSecAndRem(uint64_t* outSec, uint64_t* outRem, uint64_t* outFreq) {
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    int64_t rel = now.QuadPart - g_xboxQpcBaseTick;
    if (rel < 0) rel = 0;
    uint64_t urel = static_cast<uint64_t>(rel);
    uint64_t freq = static_cast<uint64_t>(g_xboxQpcFreq > 0 ? g_xboxQpcFreq : 10000000LL);
    *outSec  = urel / freq;
    *outRem  = urel % freq;
    *outFreq = freq;
}

uint64_t QueryXboxCpuPerformanceFrequencyTicks() {
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);
    return kXboxCpuClockHz; // 733,333,333 Hz (0x2BB5C755, exact Ghost.exe 0x0059FB86 value)
}

uint64_t QueryXboxCpuPerformanceCounterTicks() {
    uint64_t sec = 0, rem = 0, freq = 1;
    QueryElapsedHostQpcSecAndRem(&sec, &rem, &freq);
    // Rebase to +1.0s at process launch so tStdTime::Init/Tick (0x005A7D20 / 0x005A79AC)
    // and QueryXboxTickCountMs() share the exact +1000 ms launch epoch.
    return kXboxCpuClockHz + sec * kXboxCpuClockHz + MulDivU64(rem, kXboxCpuClockHz, freq);
}

uint64_t QueryXboxAcpiPerformanceFrequencyTicks() {
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);
    return kXboxAcpiTimerHz; // 3,375,000 Hz MCPX ACPI PM Timer
}

uint64_t QueryXboxAcpiPerformanceCounterTicks() {
    uint64_t sec = 0, rem = 0, freq = 1;
    QueryElapsedHostQpcSecAndRem(&sec, &rem, &freq);
    return kXboxAcpiTimerHz + sec * kXboxAcpiTimerHz + MulDivU64(rem, kXboxAcpiTimerHz, freq);
}

uint64_t QueryXbox100NsInterruptTime() {
    uint64_t sec = 0, rem = 0, freq = 1;
    QueryElapsedHostQpcSecAndRem(&sec, &rem, &freq);
    constexpr uint64_t k100NsPerSec = 10000000ULL;
    return k100NsPerSec + sec * k100NsPerSec + MulDivU64(rem, k100NsPerSec, freq);
}

uint64_t QueryXbox733MHzCyclesTimes3Shared() {
    uint64_t sec = 0, rem = 0, freq = 1;
    QueryElapsedHostQpcSecAndRem(&sec, &rem, &freq);
    constexpr uint64_t kCyclesTimes3PerSec = 2200000000ULL; // 733,333,333.333 Hz * 3 = 0x2191C0 ticks/ms
    return kCyclesTimes3PerSec + sec * kCyclesTimes3PerSec + MulDivU64(rem, kCyclesTimes3PerSec, freq);
}

uint64_t QueryXboxPerformanceFrequencyTicks() {
    return QueryXboxCpuPerformanceFrequencyTicks();
}

uint64_t QueryXboxPerformanceCounterTicks() {
    return QueryXboxCpuPerformanceCounterTicks();
}

uint32_t QueryXboxTickCountMs() {
    uint64_t sec = 0, rem = 0, freq = 1;
    QueryElapsedHostQpcSecAndRem(&sec, &rem, &freq);
    uint64_t ms = 1000ULL + sec * 1000ULL + MulDivU64(rem, 1000ULL, freq);
    return static_cast<uint32_t>(ms);
}

static uint64_t __stdcall Krnl_KeQueryPerformanceCounter() {
    return QueryXboxAcpiPerformanceCounterTicks();
}

static uint64_t __stdcall Krnl_KeQueryPerformanceFrequency() {
    return QueryXboxAcpiPerformanceFrequencyTicks();
}

static void __stdcall Krnl_KeQuerySystemTime(FILETIME* outTime) {
    if (outTime) GetSystemTimeAsFileTime(outTime);
}

static uint8_t __fastcall Krnl_KfRaiseIrql(uint8_t newIrql) {
    (void)newIrql;
    return 0;
}

static void __fastcall Krnl_KfLowerIrql(uint8_t oldIrql) {
    (void)oldIrql;
}

static uint8_t __stdcall Krnl_KeRaiseIrqlToDpcLevel() {
    return 0;
}

static int32_t __stdcall Krnl_KeSaveFloatingPointState(void* pFloatSave) {
    (void)pFloatSave;
    return 0;
}

static int32_t __stdcall Krnl_KeRestoreFloatingPointState(void* pFloatSave) {
    (void)pFloatSave;
    return 0;
}

// ============================================================================
// Kernel Dispatcher Header, Multi-Instance KTIMER / KDPC Queue & 1ms Clock Thread
// ============================================================================

#pragma pack(push, 1)
struct XBOX_DISPATCHER_HEADER {
    uint8_t       Type;            // +0x00: 0=NotificationEvent, 1=SynchronizationEvent, 2=Mutant, 5=Semaphore, 8=NotificationTimer, 9=SynchronizationTimer
    uint8_t       Absolute;        // +0x01
    uint8_t       Size;            // +0x02 (size in DWORDs)
    uint8_t       Inserted;        // +0x03
    volatile LONG SignalState;     // +0x04
    uint32_t      WaitListHead[2]; // +0x08..+0x0F
};
static_assert(sizeof(XBOX_DISPATCHER_HEADER) == 16, "XBOX_DISPATCHER_HEADER must be 16 bytes");
#pragma pack(pop)

#pragma pack(push, 4)
struct XBOX_KDPC {
    uint16_t Type;
    uint8_t  Inserted;
    uint8_t  Padding;
    uint32_t DpcListEntry[2];
    void (__stdcall *DeferredRoutine)(XBOX_KDPC*, void*, void*, void*);
    void*    DeferredContext;
    void*    SystemArgument1;
    void*    SystemArgument2;
};
static_assert(sizeof(XBOX_KDPC) == 0x1C, "XBOX_KDPC must be 0x1C (28) bytes");

struct XBOX_KTIMER {
    XBOX_DISPATCHER_HEADER Header;            // +0x00 (16 bytes)
    uint64_t               DueTime;           // +0x10 (4-byte aligned on 32-bit Xbox!)
    uint32_t               TimerListEntry[2]; // +0x18
    XBOX_KDPC*             Dpc;               // +0x20
    int32_t                Period;            // +0x24 (milliseconds)
};
static_assert(sizeof(XBOX_KTIMER) == 0x28, "XBOX_KTIMER must be 0x28 (40) bytes");
#pragma pack(pop)

static SRWLOCK            g_dispLock        = SRWLOCK_INIT;
static CONDITION_VARIABLE g_dispCv          = CONDITION_VARIABLE_INIT;
static SRWLOCK            g_kernelTimerLock = SRWLOCK_INIT;

struct ActiveKernelTimerEntry {
    XBOX_KTIMER* timer;
    uint64_t     expire100ns;
};

struct PendingDpcCall {
    XBOX_KDPC* dpc;
    void (__stdcall *routine)(XBOX_KDPC*, void*, void*, void*);
    void*      context;
    void*      arg1;
    void*      arg2;
};

static std::vector<ActiveKernelTimerEntry> g_activeTimers;
static std::vector<XBOX_KDPC*>             g_queuedDpcs;
static HANDLE                              g_kernelClockThread    = nullptr;
static HANDLE                              g_kernelClockStopEvent = nullptr;
static volatile LONG                       g_kernelClockRunning   = 0;
static INIT_ONCE                           g_dpcDispatchInitOnce  = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION                    g_dpcDispatchCs;
static __declspec(thread) uint8_t          t_inPumpTimers         = 0u;
static __declspec(thread) uint8_t          t_xboxInDpcRoutine     = 0u;

static BOOL CALLBACK InitDpcDispatchCsCallback(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_dpcDispatchCs);
    return TRUE;
}

bool IsXboxKernelClockThreadRunning() {
    return InterlockedCompareExchange(&g_kernelClockRunning, 0, 0) != 0;
}

uint32_t __cdecl GetXboxSimulatedDpcIrql() {
    return t_xboxInDpcRoutine ? 2u : 0u;
}

void __stdcall Krnl_KeInitializeDpc(XBOX_KDPC* Dpc, void* DeferredRoutine, void* DeferredContext) {
    if (!Dpc) return;
    memset(Dpc, 0, sizeof(*Dpc));
    Dpc->Type            = 19u; // DpcObject
    Dpc->DeferredRoutine = reinterpret_cast<void(__stdcall*)(XBOX_KDPC*, void*, void*, void*)>(DeferredRoutine);
    Dpc->DeferredContext = DeferredContext;
}

uint8_t __stdcall Krnl_KeInsertQueueDpc(XBOX_KDPC* Dpc, void* SystemArgument1, void* SystemArgument2) {
    if (!Dpc || !Dpc->DeferredRoutine) return 0;
    uint8_t inserted = 0;
    AcquireSRWLockExclusive(&g_kernelTimerLock);
    if (!Dpc->Inserted) {
        Dpc->Inserted        = 1;
        Dpc->SystemArgument1 = SystemArgument1;
        Dpc->SystemArgument2 = SystemArgument2;
        g_queuedDpcs.push_back(Dpc);
        inserted = 1;
    }
    ReleaseSRWLockExclusive(&g_kernelTimerLock);
    return inserted;
}

uint8_t __stdcall Krnl_KeRemoveQueueDpc(XBOX_KDPC* Dpc) {
    if (!Dpc) return 0;
    uint8_t removed = 0;
    AcquireSRWLockExclusive(&g_kernelTimerLock);
    for (size_t i = 0; i < g_queuedDpcs.size(); ++i) {
        if (g_queuedDpcs[i] == Dpc) {
            g_queuedDpcs.erase(g_queuedDpcs.begin() + static_cast<ptrdiff_t>(i));
            Dpc->Inserted = 0;
            removed       = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_kernelTimerLock);
    return removed;
}

void __stdcall Krnl_KeInitializeTimerEx(XBOX_KTIMER* Timer, uint32_t Type) {
    if (!Timer) return;
    // Strict global lock hierarchy: g_kernelTimerLock -> g_dispLock
    AcquireSRWLockExclusive(&g_kernelTimerLock);
    for (size_t i = 0; i < g_activeTimers.size(); ++i) {
        if (g_activeTimers[i].timer == Timer) {
            g_activeTimers.erase(g_activeTimers.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    AcquireSRWLockExclusive(&g_dispLock);
    memset(Timer, 0, sizeof(*Timer));
    Timer->Header.Type            = static_cast<uint8_t>(8u + (Type & 1u));
    Timer->Header.Size            = static_cast<uint8_t>(sizeof(XBOX_KTIMER) / sizeof(uint32_t));
    Timer->Header.SignalState     = 0;
    Timer->Header.WaitListHead[0] = reinterpret_cast<uint32_t>(&Timer->Header.WaitListHead[0]);
    Timer->Header.WaitListHead[1] = reinterpret_cast<uint32_t>(&Timer->Header.WaitListHead[0]);
    ReleaseSRWLockExclusive(&g_dispLock);
    ReleaseSRWLockExclusive(&g_kernelTimerLock);
}

uint8_t __stdcall Krnl_KeSetTimerEx(XBOX_KTIMER* Timer, uint32_t DueLow, int32_t DueHigh, int32_t Period, XBOX_KDPC* Dpc) {
    if (!Timer) return 0;
    int64_t  dueRaw   = (static_cast<int64_t>(DueHigh) << 32) | static_cast<uint64_t>(DueLow);
    uint64_t now100ns = QueryXbox100NsInterruptTime();
    uint64_t expire100ns = now100ns;

    if (dueRaw < 0) {
        // Relative timeout in 100-ns units (guarding INT64_MIN)
        uint64_t rel100ns = (dueRaw == (-9223372036854775807LL - 1LL))
            ? 0x8000000000000000ULL
            : static_cast<uint64_t>(-dueRaw);
        expire100ns = now100ns + rel100ns;
    } else if (dueRaw > 0) {
        // Absolute system FILETIME in 100-ns units (used by XACT::CEngine::SetTimer at 0x00704466)
        FILETIME ftNow = {};
        GetSystemTimeAsFileTime(&ftNow);
        int64_t sysNow = (static_cast<int64_t>(ftNow.dwHighDateTime) << 32) | static_cast<uint64_t>(ftNow.dwLowDateTime);
        if (dueRaw > sysNow) {
            expire100ns = now100ns + static_cast<uint64_t>(dueRaw - sysNow);
        }
    }

    uint8_t wasInserted = 0;
    // Strict global lock hierarchy: g_kernelTimerLock -> g_dispLock
    AcquireSRWLockExclusive(&g_kernelTimerLock);
    AcquireSRWLockExclusive(&g_dispLock);
    if (Timer->Header.Type != 8u && Timer->Header.Type != 9u) {
        Timer->Header.Type = 8u;
    }
    Timer->Header.Size        = static_cast<uint8_t>(sizeof(XBOX_KTIMER) / sizeof(uint32_t));
    Timer->Header.SignalState = 0;
    ReleaseSRWLockExclusive(&g_dispLock);

    Timer->DueTime         = expire100ns;
    Timer->Dpc             = Dpc;
    Timer->Period          = Period;
    Timer->Header.Inserted = 1;

    for (size_t i = 0; i < g_activeTimers.size(); ++i) {
        if (g_activeTimers[i].timer == Timer) {
            g_activeTimers.erase(g_activeTimers.begin() + static_cast<ptrdiff_t>(i));
            wasInserted = 1;
            break;
        }
    }

    // Keep g_activeTimers sorted in ascending expire100ns order (true min-priority queue)
    ActiveKernelTimerEntry entry = { Timer, expire100ns };
    size_t insertPos = g_activeTimers.size();
    for (size_t i = 0; i < g_activeTimers.size(); ++i) {
        if (expire100ns < g_activeTimers[i].expire100ns) {
            insertPos = i;
            break;
        }
    }
    g_activeTimers.insert(g_activeTimers.begin() + static_cast<ptrdiff_t>(insertPos), entry);
    ReleaseSRWLockExclusive(&g_kernelTimerLock);
    return wasInserted;
}

uint8_t __stdcall Krnl_KeSetTimer(XBOX_KTIMER* Timer, uint32_t DueLow, int32_t DueHigh, XBOX_KDPC* Dpc) {
    return Krnl_KeSetTimerEx(Timer, DueLow, DueHigh, 0, Dpc);
}

uint8_t __stdcall Krnl_KeCancelTimer(XBOX_KTIMER* Timer) {
    if (!Timer) return 0;
    uint8_t wasInserted = 0;
    AcquireSRWLockExclusive(&g_kernelTimerLock);
    for (size_t i = 0; i < g_activeTimers.size(); ++i) {
        if (g_activeTimers[i].timer == Timer) {
            g_activeTimers.erase(g_activeTimers.begin() + static_cast<ptrdiff_t>(i));
            Timer->Header.Inserted = 0;
            wasInserted = 1;
            break;
        }
    }
    if (Timer->Dpc && Timer->Dpc->Inserted) {
        for (size_t i = 0; i < g_queuedDpcs.size(); ++i) {
            if (g_queuedDpcs[i] == Timer->Dpc) {
                g_queuedDpcs.erase(g_queuedDpcs.begin() + static_cast<ptrdiff_t>(i));
                Timer->Dpc->Inserted = 0;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_kernelTimerLock);
    return wasInserted;
}

void PumpXboxKernelTimers() {
    if (t_inPumpTimers) {
        return;
    }
    InitOnceExecuteOnce(&g_dpcDispatchInitOnce, InitDpcDispatchCsCallback, nullptr, nullptr);
    EnterCriticalSection(&g_dpcDispatchCs);
    t_inPumpTimers = 1u;

    uint64_t now100ns = QueryXbox100NsInterruptTime();
    PendingDpcCall localDpcs[32];
    size_t numLocalDpcs = 0;
    std::vector<PendingDpcCall> overflowDpcs;
    XBOX_KTIMER* signaledTimers[32];
    size_t numSignaledTimers = 0;
    std::vector<XBOX_KTIMER*> overflowTimers;

    AcquireSRWLockExclusive(&g_kernelTimerLock);
    while (!g_activeTimers.empty() && g_activeTimers[0].expire100ns <= now100ns) {
        ActiveKernelTimerEntry cur = g_activeTimers[0];
        g_activeTimers.erase(g_activeTimers.begin());

        XBOX_KTIMER* t = cur.timer;
        if (!t) continue;

        uint64_t firedDue = cur.expire100ns;
        if (t->Period > 0) {
            uint64_t step100ns = static_cast<uint64_t>(t->Period) * 10000ULL;
            uint64_t next100ns = cur.expire100ns + step100ns;
            if (next100ns <= now100ns) {
                next100ns = now100ns + step100ns;
            }
            t->DueTime         = next100ns;
            t->Header.Inserted = 1;
            ActiveKernelTimerEntry nextEntry = { t, next100ns };
            size_t insertPos = g_activeTimers.size();
            for (size_t i = 0; i < g_activeTimers.size(); ++i) {
                if (next100ns < g_activeTimers[i].expire100ns) {
                    insertPos = i;
                    break;
                }
            }
            g_activeTimers.insert(g_activeTimers.begin() + static_cast<ptrdiff_t>(insertPos), nextEntry);
        } else {
            t->Header.Inserted = 0;
        }

        if (numSignaledTimers < 32u) {
            signaledTimers[numSignaledTimers++] = t;
        } else {
            overflowTimers.push_back(t);
        }

        XBOX_KDPC* dpc = t->Dpc;
        if (dpc && dpc->DeferredRoutine && !dpc->Inserted) {
            dpc->SystemArgument1 = reinterpret_cast<void*>(static_cast<uintptr_t>(firedDue & 0xFFFFFFFFu));
            dpc->SystemArgument2 = reinterpret_cast<void*>(static_cast<uintptr_t>(firedDue >> 32));
            PendingDpcCall call  = { dpc, dpc->DeferredRoutine, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2 };
            if (numLocalDpcs < 32u) {
                localDpcs[numLocalDpcs++] = call;
            } else {
                overflowDpcs.push_back(call);
            }
        }
    }

    while (!g_queuedDpcs.empty()) {
        XBOX_KDPC* dpc = g_queuedDpcs[0];
        g_queuedDpcs.erase(g_queuedDpcs.begin());
        if (!dpc) continue;
        dpc->Inserted = 0;
        if (dpc->DeferredRoutine) {
            PendingDpcCall call = { dpc, dpc->DeferredRoutine, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2 };
            if (numLocalDpcs < 32u) {
                localDpcs[numLocalDpcs++] = call;
            } else {
                overflowDpcs.push_back(call);
            }
        }
    }

    bool needWakeWaiters = (numSignaledTimers > 0 || !overflowTimers.empty());
    if (needWakeWaiters) {
        // Set SignalState = 1 atomically under g_kernelTimerLock -> g_dispLock so a concurrent
        // KeSetTimerEx re-arm can never be overwritten by a stale post-unlock SignalState store.
        AcquireSRWLockExclusive(&g_dispLock);
        for (size_t i = 0; i < numSignaledTimers; ++i) {
            signaledTimers[i]->Header.SignalState = 1;
        }
        for (size_t i = 0; i < overflowTimers.size(); ++i) {
            overflowTimers[i]->Header.SignalState = 1;
        }
        ReleaseSRWLockExclusive(&g_dispLock);
    }
    ReleaseSRWLockExclusive(&g_kernelTimerLock);

    if (needWakeWaiters) {
        WakeAllConditionVariable(&g_dispCv);
    }

    auto invokeDpc = [](const PendingDpcCall& c) {
        // XACT::CEngine::DPCTimerCallBack (0x00703FEA) mutates CEngine's CPriorityQueue (+0x160).
        // On single-core Xbox, SetTimer raised IRQL to DISPATCH_LEVEL; on multi-core Windows 11,
        // serialize 0x00703FEA with _g_XACTCriticalSection (0x00708F58) and expose simulated
        // DISPATCH_LEVEL (IRQL 2) via GetXboxSimulatedDpcIrql() for XACT::CEngine::Enqueue (0x00704639).
        const bool isXactDpc = (reinterpret_cast<uintptr_t>(c.routine) == 0x00703FEAu);
        if (isXactDpc) {
            Bridge_RtlEnterCriticalSection(reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION));
        }
        uint8_t prevDpc = t_xboxInDpcRoutine;
        t_xboxInDpcRoutine = 1u;
        c.routine(c.dpc, c.context, c.arg1, c.arg2);
        t_xboxInDpcRoutine = prevDpc;
        if (isXactDpc) {
            Bridge_RtlLeaveCriticalSection(reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION));
        }
    };

    for (size_t i = 0; i < numLocalDpcs; ++i) {
        invokeDpc(localDpcs[i]);
    }
    for (size_t i = 0; i < overflowDpcs.size(); ++i) {
        invokeDpc(overflowDpcs[i]);
    }

    t_inPumpTimers = 0u;
    LeaveCriticalSection(&g_dpcDispatchCs);
}

static DWORD WINAPI XboxKernelClockThreadProc(LPVOID) {
    InitThreadPtd();
    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer) {
        hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    HANDLE waitHandles[2] = { g_kernelClockStopEvent, hTimer };
    DWORD  handleCount    = hTimer ? 2u : 1u;

    while (InterlockedCompareExchange(&g_kernelClockRunning, 0, 0) != 0 && g_appRunning) {
        g_keTickCount = QueryXboxTickCountMs();
        PumpXboxKernelTimers();

        if (hTimer) {
            LARGE_INTEGER due = {};
            due.QuadPart = -10000LL; // 1.0 ms relative (100-ns units)
            SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE);
            DWORD w = WaitForMultipleObjects(handleCount, waitHandles, FALSE, 2u);
            if (w == WAIT_OBJECT_0) {
                break;
            }
        } else {
            if (WaitForSingleObject(g_kernelClockStopEvent, 1u) == WAIT_OBJECT_0) {
                break;
            }
        }
    }

    if (hTimer) {
        CloseHandle(hTimer);
    }
    CleanupCurrentXboxThreadState();
    return 0;
}

void StartXboxKernelClockThread() {
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);
    EnsureXboxCrtTlsAllocated();
    g_keTickCount = QueryXboxTickCountMs();
    if (InterlockedCompareExchange(&g_kernelClockRunning, 1, 0) == 0) {
        if (!g_kernelClockStopEvent) {
            g_kernelClockStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        } else {
            ResetEvent(g_kernelClockStopEvent);
        }
        g_kernelClockThread = CreateThread(nullptr, 65536u, XboxKernelClockThreadProc, nullptr, 0, nullptr);
        if (g_kernelClockThread) {
            SetThreadPriority(g_kernelClockThread, THREAD_PRIORITY_TIME_CRITICAL);
        } else {
            InterlockedExchange(&g_kernelClockRunning, 0);
        }
    }
}

// Kernel Memory Allocators
struct PoolHeader {
    uint32_t magic;
    uint32_t size;
    void*    rawPtr;
    uint32_t reserved;
};
static constexpr uint32_t kXboxPoolMagic = 0x58504F4Cu; // 'XPOL'

static void* __stdcall Krnl_ExAllocatePool(uint32_t NumberOfBytes) {
    uint32_t sz = NumberOfBytes ? NumberOfBytes : 4u;
    size_t total = static_cast<size_t>(sz) + sizeof(PoolHeader) + 16u;
    uint8_t* raw = static_cast<uint8_t*>(calloc(1, total));
    if (!raw) return nullptr;
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(PoolHeader) + 15u) & ~static_cast<uintptr_t>(15u);
    PoolHeader* hdr = reinterpret_cast<PoolHeader*>(aligned - sizeof(PoolHeader));
    hdr->magic    = kXboxPoolMagic;
    hdr->size     = sz;
    hdr->rawPtr   = raw;
    hdr->reserved = 0;
    return reinterpret_cast<void*>(aligned);
}

static void* __stdcall Krnl_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag) {
    (void)Tag;
    return Krnl_ExAllocatePool(NumberOfBytes);
}

static uint32_t __stdcall Krnl_ExQueryPoolBlockSize(void* P) {
    if (!P || IsStaticXboxImageOrNullPtr(P)) return 0;
    if ((reinterpret_cast<uintptr_t>(P) & 15u) == 0) {
        PoolHeader* hdr = reinterpret_cast<PoolHeader*>(reinterpret_cast<uintptr_t>(P) - sizeof(PoolHeader));
        if (hdr->magic == kXboxPoolMagic) {
            return hdr->size;
        }
    }
    return SizeAlignedTracked(P);
}

static SRWLOCK                g_memFreeCbLock = SRWLOCK_INIT;
static XboxMemoryFreeCallback g_memFreeCallbacks[8] = {};
static std::atomic<uint32_t>  g_memFreeCallbackCount{0u};

void RegisterXboxMemoryFreeCallback(XboxMemoryFreeCallback cb) {
    if (!cb) return;
    AcquireSRWLockExclusive(&g_memFreeCbLock);
    uint32_t count = g_memFreeCallbackCount.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_memFreeCallbacks[i] == cb) {
            ReleaseSRWLockExclusive(&g_memFreeCbLock);
            return;
        }
    }
    if (count < 8u) {
        g_memFreeCallbacks[count] = cb;
        g_memFreeCallbackCount.store(count + 1u, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_memFreeCbLock);
}

void NotifyXboxMemoryFreed(const void* ptr, size_t byteLen) {
    if (!ptr || byteLen == 0) return;
    uint32_t count = g_memFreeCallbackCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        XboxMemoryFreeCallback cb = g_memFreeCallbacks[i];
        if (cb) cb(ptr, byteLen);
    }
}

static void __stdcall Krnl_ExFreePool(void* P) {
    if (!P || IsStaticXboxImageOrNullPtr(P)) return;
    if ((reinterpret_cast<uintptr_t>(P) & 15u) == 0) {
        PoolHeader* hdr = reinterpret_cast<PoolHeader*>(reinterpret_cast<uintptr_t>(P) - sizeof(PoolHeader));
        if (hdr->magic == kXboxPoolMagic) {
            uint32_t sz = hdr->size;
            void* raw = hdr->rawPtr;
            hdr->magic = 0;
            NotifyXboxMemoryFreed(P, sz);
            free(raw);
            return;
        }
    }
    FreeAlignedTracked(P);
}

struct PhysHeader {
    uint32_t magic;
    uint32_t size;
    void*    rawPtr;
};

void* AllocAlignedTracked(uint32_t size, uint32_t alignment) {
    if (alignment < 16u) alignment = 16u;
    if ((alignment & (alignment - 1u)) != 0u) {
        uint32_t p2 = 16u;
        while (p2 < alignment && p2 < 0x40000000u) p2 <<= 1;
        alignment = p2;
    }
    if (size == 0u) size = alignment;
    size_t total = static_cast<size_t>(size) + static_cast<size_t>(alignment - 1u) + sizeof(PhysHeader);
    uint8_t* raw = static_cast<uint8_t*>(calloc(1, total));
    if (!raw) return nullptr;
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(PhysHeader) + alignment - 1u) & ~(static_cast<uintptr_t>(alignment) - 1u);
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>(aligned - sizeof(PhysHeader));
    hdr->magic = 0x58504859u; // 'XPHY'
    hdr->size = size;
    hdr->rawPtr = raw;
    return reinterpret_cast<void*>(aligned);
}

void FreeAlignedTracked(void* ptr) {
    if (!ptr || IsStaticXboxImageOrNullPtr(ptr)) return;
    if ((reinterpret_cast<uintptr_t>(ptr) & 15u) != 0) return;
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(PhysHeader));
    if (hdr->magic == 0x58504859u) {
        uint32_t sz = hdr->size;
        void* raw = hdr->rawPtr;
        hdr->magic = 0;
        NotifyXboxMemoryFreed(ptr, sz);
        free(raw);
    }
}

uint32_t SizeAlignedTracked(void* ptr) {
    if (!ptr || IsStaticXboxImageOrNullPtr(ptr)) return 0;
    if ((reinterpret_cast<uintptr_t>(ptr) & 15u) != 0) return 0;
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>((uintptr_t)ptr - sizeof(PhysHeader));
    if (hdr->magic == 0x58504859) {
        return hdr->size;
    }
    return 0;
}

static void* __stdcall Krnl_MmAllocateContiguousMemory(uint32_t NumberOfBytes) {
    return AllocAlignedTracked(NumberOfBytes, 4096);
}

static void* __stdcall Krnl_MmAllocateContiguousMemoryEx(
    uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress, uint32_t HighestAcceptableAddress,
    uint32_t Alignment, uint32_t ProtectionType)
{
    (void)LowestAcceptableAddress; (void)HighestAcceptableAddress; (void)ProtectionType;
    return AllocAlignedTracked(NumberOfBytes, Alignment ? Alignment : 4096);
}

static void __stdcall Krnl_MmFreeContiguousMemory(void* BaseAddress) {
    FreeAlignedTracked(BaseAddress);
}

static uint32_t __stdcall Krnl_MmQueryAllocationSize(void* BaseAddress) {
    return SizeAlignedTracked(BaseAddress);
}

static void __stdcall Krnl_MmPersistContiguousMemory(void* BaseAddress, uint32_t NumberOfBytes, uint32_t Persist) {
    (void)BaseAddress; (void)NumberOfBytes; (void)Persist;
}

static void* __stdcall Krnl_MmGetPhysicalAddress(void* BaseAddress) {
    return BaseAddress;
}

static uint32_t __stdcall Krnl_MmQueryAddressProtect(void* VirtualAddress) {
    return VirtualAddress != nullptr ? PAGE_READWRITE : 0;
}

static void __stdcall Krnl_MmSetAddressProtect(void* BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect) {
    (void)BaseAddress; (void)NumberOfBytes; (void)NewProtect;
}

struct XBOX_MM_STATISTICS {
    uint32_t Length;
    uint32_t TotalPhysicalPages;
    uint32_t AvailablePages;
    uint32_t VirtualMemoryBytesCommitted;
    uint32_t VirtualMemoryBytesReserved;
    uint32_t CachePagesCommitted;
    uint32_t PoolPagesCommitted;
    uint32_t StackPagesCommitted;
    uint32_t ImagePagesCommitted;
};

static int32_t __stdcall Krnl_MmQueryStatistics(XBOX_MM_STATISTICS* Stats) {
    if (Stats) {
        Stats->TotalPhysicalPages = 16384; // 64 MB
        Stats->AvailablePages = 12288;     // 48 MB free
        Stats->VirtualMemoryBytesCommitted = 16 * 1024 * 1024;
        Stats->VirtualMemoryBytesReserved = 16 * 1024 * 1024;
        Stats->CachePagesCommitted = 128;
        Stats->PoolPagesCommitted = 128;
        Stats->StackPagesCommitted = 64;
        Stats->ImagePagesCommitted = 1300;
    }
    return 0;
}

static int32_t __stdcall Krnl_NtAllocateVirtualMemory(
    void** BaseAddress, uint32_t ZeroBits, uint32_t* AllocationSize, uint32_t AllocationType, uint32_t Protect)
{
    (void)ZeroBits;
    if (!BaseAddress || !AllocationSize || *AllocationSize == 0) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }
    void* reqBase = *BaseAddress;
    if (reqBase != nullptr && IsStaticXboxImageOrNullPtr(reqBase)) {
        return static_cast<int32_t>(0xC0000018u); // STATUS_CONFLICTING_ADDRESSES
    }
    DWORD allocFlags = AllocationType & (MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN);
    if (allocFlags == 0) allocFlags = MEM_RESERVE | MEM_COMMIT;
    void* p = VirtualAlloc(reqBase, *AllocationSize, allocFlags, Protect ? Protect : PAGE_READWRITE);
    if (!p) {
        return static_cast<int32_t>(0xC0000017u); // STATUS_NO_MEMORY
    }
    *BaseAddress = p;
    return 0; // STATUS_SUCCESS
}

static int32_t __stdcall Krnl_NtFreeVirtualMemory(void** BaseAddress, uint32_t* FreeSize, uint32_t FreeType) {
    if (!BaseAddress || !*BaseAddress) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }
    void* freedBase = *BaseAddress;
    if (IsStaticXboxImageOrNullPtr(freedBase)) {
        return static_cast<int32_t>(0xC0000018u); // STATUS_CONFLICTING_ADDRESSES
    }
    size_t notifyBytes = (FreeSize && *FreeSize != 0u) ? static_cast<size_t>(*FreeSize) : 0u;
    if (notifyBytes == 0u) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(freedBase, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            notifyBytes = mbi.RegionSize;
        }
    }
    // Win32 VirtualFree requires dwSize == 0 whenever MEM_RELEASE is set
    SIZE_T winFreeSize = (FreeType & MEM_RELEASE) ? 0 : (FreeSize ? *FreeSize : 0);
    if (!VirtualFree(freedBase, winFreeSize, FreeType)) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }
    if (notifyBytes > 0u) {
        NotifyXboxMemoryFreed(freedBase, notifyBytes);
    }
    if (FreeType & MEM_RELEASE) {
        *BaseAddress = nullptr;
        if (FreeSize) *FreeSize = 0;
    }
    return 0; // STATUS_SUCCESS
}

static int32_t __stdcall Krnl_NtClose(HANDLE Handle) {
    if (!Handle || Handle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u); // STATUS_INVALID_HANDLE
    }
    return Bridge_CloseHandle(Handle) ? 0 : static_cast<int32_t>(0xC0000008u);
}

// ============================================================================
// Kernel Dispatcher Objects, NT Handle Sync, RTL Helpers & Managed Threads
// ============================================================================

#pragma pack(push, 1)
struct XBOX_KEVENT {
    XBOX_DISPATCHER_HEADER Header;
};
static_assert(sizeof(XBOX_KEVENT) == 16, "XBOX_KEVENT must be 16 bytes");

struct XBOX_KSEMAPHORE {
    XBOX_DISPATCHER_HEADER Header;
    int32_t                Limit;
};
static_assert(sizeof(XBOX_KSEMAPHORE) == 20, "XBOX_KSEMAPHORE must be 20 bytes");

struct XBOX_UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t* Buffer;
};

struct XBOX_OBJECT_ATTRIBUTES {
    HANDLE              RootDirectory;
    XBOX_OBJECT_STRING* ObjectName;
    uint32_t            Attributes;
};

struct XBOX_TIME_FIELDS {
    int16_t Year;
    int16_t Month;
    int16_t Day;
    int16_t Hour;
    int16_t Minute;
    int16_t Second;
    int16_t Milliseconds;
    int16_t Weekday;
};
#pragma pack(pop)

static constexpr uint32_t kXboxObRefMagic = 0x584F4252u; // 'XOBR'
struct XboxObRefObject {
    XBOX_DISPATCHER_HEADER Header;
    uint32_t               magic;
    volatile LONG          refCount;
    HANDLE                 winHandle;
};

static std::vector<XboxObRefObject*> g_obRefObjects;

static XboxObRefObject* LookupObRefObjectLocked(const void* ptr) {
    if (!ptr) return nullptr;
    for (size_t i = 0; i < g_obRefObjects.size(); ++i) {
        XboxObRefObject* cand = g_obRefObjects[i];
        if (cand == ptr && cand->magic == kXboxObRefMagic) {
            return cand;
        }
    }
    return nullptr;
}

int32_t __stdcall Krnl_ObReferenceObjectByHandle(HANDLE Handle, void* ObjectType, void** Object) {
    (void)ObjectType;
    if (!Object) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }
    *Object = nullptr;
    if (!Handle || Handle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u); // STATUS_INVALID_HANDLE
    }
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), Handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return static_cast<int32_t>(0xC0000008u); // STATUS_INVALID_HANDLE
    }
    XboxObRefObject* obj = static_cast<XboxObRefObject*>(calloc(1, sizeof(XboxObRefObject)));
    if (!obj) {
        CloseHandle(dup);
        return static_cast<int32_t>(0xC0000017u); // STATUS_NO_MEMORY
    }
    obj->Header.Size = static_cast<uint8_t>(sizeof(XBOX_DISPATCHER_HEADER) / sizeof(uint32_t));
    obj->magic       = kXboxObRefMagic;
    obj->refCount    = 1;
    obj->winHandle   = dup;

    AcquireSRWLockExclusive(&g_dispLock);
    g_obRefObjects.push_back(obj);
    ReleaseSRWLockExclusive(&g_dispLock);

    *Object = obj;
    return 0; // STATUS_SUCCESS
}

void __fastcall Krnl_ObfDereferenceObject(void* Object) {
    if (!Object) return;
    HANDLE toClose = nullptr;
    XboxObRefObject* toFree = nullptr;

    AcquireSRWLockExclusive(&g_dispLock);
    for (size_t i = 0; i < g_obRefObjects.size(); ++i) {
        XboxObRefObject* cand = g_obRefObjects[i];
        if (cand == Object && cand->magic == kXboxObRefMagic) {
            if (InterlockedDecrement(&cand->refCount) <= 0) {
                cand->magic = 0;
                toClose     = cand->winHandle;
                toFree      = cand;
                g_obRefObjects.erase(g_obRefObjects.begin() + static_cast<ptrdiff_t>(i));
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_dispLock);

    if (toClose) {
        CloseHandle(toClose);
    }
    if (toFree) {
        free(toFree);
    }
}

void __stdcall Krnl_KeInitializeEvent(XBOX_KEVENT* Event, uint32_t Type, uint8_t State) {
    if (!Event) return;
    AcquireSRWLockExclusive(&g_dispLock);
    memset(Event, 0, sizeof(*Event));
    Event->Header.Type            = static_cast<uint8_t>(Type & 1u); // 0 = Notification, 1 = Synchronization
    Event->Header.Size            = static_cast<uint8_t>(sizeof(XBOX_KEVENT) / sizeof(uint32_t));
    Event->Header.SignalState     = State ? 1 : 0;
    Event->Header.WaitListHead[0] = reinterpret_cast<uint32_t>(&Event->Header.WaitListHead[0]);
    Event->Header.WaitListHead[1] = reinterpret_cast<uint32_t>(&Event->Header.WaitListHead[0]);
    ReleaseSRWLockExclusive(&g_dispLock);
}

struct NT_EVENT_BASIC_INFORMATION {
    LONG EventType;
    LONG EventState;
};

static int32_t QueryWin32EventSignalStateNonDestructive(HANDLE hEvent) {
    if (!hEvent || hEvent == INVALID_HANDLE_VALUE) return 0;
    typedef LONG (NTAPI *PFN_NtQueryEvent)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static PFN_NtQueryEvent s_ntQueryEvent = nullptr;
    static volatile LONG    s_resolved     = 0;
    if (InterlockedCompareExchange(&s_resolved, 0, 0) != 2) {
        if (InterlockedCompareExchange(&s_resolved, 1, 0) == 0) {
            HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
            if (hNtdll) {
                s_ntQueryEvent = reinterpret_cast<PFN_NtQueryEvent>(GetProcAddress(hNtdll, "NtQueryEvent"));
            }
            InterlockedExchange(&s_resolved, 2);
        } else {
            while (InterlockedCompareExchange(&s_resolved, 0, 0) != 2) {
                SwitchToThread();
            }
        }
    }
    if (s_ntQueryEvent) {
        NT_EVENT_BASIC_INFORMATION info = {};
        ULONG retLen = 0;
        if (s_ntQueryEvent(hEvent, 0u /*EventBasicInformation*/, &info, sizeof(info), &retLen) >= 0) {
            return (info.EventState > 0) ? 1 : 0;
        }
    }
    return 0;
}

int32_t __stdcall Krnl_KeSetEvent(void* Event, int32_t Increment, uint8_t Wait) {
    (void)Increment; (void)Wait;
    if (!Event) return 0;

    HANDLE winEv = nullptr;
    AcquireSRWLockExclusive(&g_dispLock);
    XboxObRefObject* ob = LookupObRefObjectLocked(Event);
    if (ob) {
        InterlockedIncrement(&ob->refCount);
        winEv = ob->winHandle;
    } else {
        XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Event);
        int32_t prev = hdr->SignalState;
        hdr->SignalState = 1;
        ReleaseSRWLockExclusive(&g_dispLock);
        // g_dispCv is shared across all in-memory dispatcher objects, so always broadcast
        // to avoid waking an unrelated waiter and leaving the true waiter asleep.
        WakeAllConditionVariable(&g_dispCv);
        return prev;
    }
    ReleaseSRWLockExclusive(&g_dispLock);

    int32_t prev = QueryWin32EventSignalStateNonDestructive(winEv);
    SetEvent(winEv);
    WakeAllConditionVariable(&g_dispCv);
    Krnl_ObfDereferenceObject(Event);
    return prev;
}

void __stdcall Krnl_KeClearEvent(void* Event) {
    if (!Event) return;
    HANDLE winEv = nullptr;
    AcquireSRWLockExclusive(&g_dispLock);
    XboxObRefObject* ob = LookupObRefObjectLocked(Event);
    if (ob) {
        InterlockedIncrement(&ob->refCount);
        winEv = ob->winHandle;
    } else {
        XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Event);
        hdr->SignalState = 0;
        ReleaseSRWLockExclusive(&g_dispLock);
        return;
    }
    ReleaseSRWLockExclusive(&g_dispLock);
    ResetEvent(winEv);
    Krnl_ObfDereferenceObject(Event);
}

int32_t __stdcall Krnl_KeResetEvent(void* Event) {
    if (!Event) return 0;
    HANDLE winEv = nullptr;
    AcquireSRWLockExclusive(&g_dispLock);
    XboxObRefObject* ob = LookupObRefObjectLocked(Event);
    if (ob) {
        InterlockedIncrement(&ob->refCount);
        winEv = ob->winHandle;
    } else {
        XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Event);
        int32_t prev = hdr->SignalState;
        hdr->SignalState = 0;
        ReleaseSRWLockExclusive(&g_dispLock);
        return prev;
    }
    ReleaseSRWLockExclusive(&g_dispLock);
    int32_t prev = QueryWin32EventSignalStateNonDestructive(winEv);
    ResetEvent(winEv);
    Krnl_ObfDereferenceObject(Event);
    return prev;
}

int32_t __stdcall Krnl_KePulseEvent(void* Event, int32_t Increment, uint8_t Wait) {
    (void)Increment; (void)Wait;
    if (!Event) return 0;
    HANDLE winEv = nullptr;
    AcquireSRWLockExclusive(&g_dispLock);
    XboxObRefObject* ob = LookupObRefObjectLocked(Event);
    if (ob) {
        InterlockedIncrement(&ob->refCount);
        winEv = ob->winHandle;
    } else {
        XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Event);
        int32_t prev = hdr->SignalState;
        hdr->SignalState = 0;
        ReleaseSRWLockExclusive(&g_dispLock);
        WakeAllConditionVariable(&g_dispCv);
        return prev;
    }
    ReleaseSRWLockExclusive(&g_dispLock);
    int32_t prev = QueryWin32EventSignalStateNonDestructive(winEv);
    PulseEvent(winEv);
    WakeAllConditionVariable(&g_dispCv);
    Krnl_ObfDereferenceObject(Event);
    return prev;
}

void __stdcall Krnl_KeInitializeSemaphore(XBOX_KSEMAPHORE* Semaphore, int32_t Count, int32_t Limit) {
    if (!Semaphore) return;
    AcquireSRWLockExclusive(&g_dispLock);
    memset(Semaphore, 0, sizeof(*Semaphore));
    Semaphore->Header.Type            = 5u; // SemaphoreObject
    Semaphore->Header.Size            = static_cast<uint8_t>(sizeof(XBOX_KSEMAPHORE) / sizeof(uint32_t));
    Semaphore->Header.SignalState     = Count;
    Semaphore->Header.WaitListHead[0] = reinterpret_cast<uint32_t>(&Semaphore->Header.WaitListHead[0]);
    Semaphore->Header.WaitListHead[1] = reinterpret_cast<uint32_t>(&Semaphore->Header.WaitListHead[0]);
    Semaphore->Limit                  = Limit;
    ReleaseSRWLockExclusive(&g_dispLock);
}

int32_t __stdcall Krnl_KeReleaseSemaphore(XBOX_KSEMAPHORE* Semaphore, int32_t Increment, int32_t Adjustment, uint8_t Wait) {
    (void)Increment; (void)Wait;
    if (!Semaphore || Adjustment <= 0) return 0;
    AcquireSRWLockExclusive(&g_dispLock);
    int32_t prev = Semaphore->Header.SignalState;
    int64_t next = static_cast<int64_t>(prev) + Adjustment;
    if (Semaphore->Limit > 0 && next > Semaphore->Limit) {
        ReleaseSRWLockExclusive(&g_dispLock);
        return prev;
    }
    Semaphore->Header.SignalState = static_cast<LONG>(next);
    ReleaseSRWLockExclusive(&g_dispLock);
    WakeAllConditionVariable(&g_dispCv);
    return prev;
}

static DWORD ConvertNtTimeoutToWin32Ms(const int64_t* Timeout, uint64_t* outDeadline100ns) {
    if (!Timeout) {
        if (outDeadline100ns) *outDeadline100ns = ~0ULL;
        return INFINITE;
    }
    int64_t raw = *Timeout;
    uint64_t now100ns = QueryXbox100NsInterruptTime();
    if (raw == 0) {
        if (outDeadline100ns) *outDeadline100ns = now100ns;
        return 0u;
    }
    uint64_t rel100ns = 0;
    if (raw < 0) {
        rel100ns = (raw == (-9223372036854775807LL - 1LL))
            ? 0x8000000000000000ULL
            : static_cast<uint64_t>(-raw);
    } else {
        FILETIME ftNow = {};
        GetSystemTimeAsFileTime(&ftNow);
        int64_t sysNow = (static_cast<int64_t>(ftNow.dwHighDateTime) << 32) | static_cast<uint64_t>(ftNow.dwLowDateTime);
        if (raw <= sysNow) {
            if (outDeadline100ns) *outDeadline100ns = now100ns;
            return 0u;
        }
        rel100ns = static_cast<uint64_t>(raw - sysNow);
    }
    if (outDeadline100ns) {
        *outDeadline100ns = (rel100ns > (~0ULL - now100ns)) ? ~0ULL : (now100ns + rel100ns);
    }
    uint64_t ms64 = (rel100ns + 9999ULL) / 10000ULL;
    return (ms64 >= 0xFFFFFFFEULL) ? 0xFFFFFFFEu : static_cast<DWORD>(ms64);
}

static int32_t MapWin32WaitResultToNtStatus(DWORD res, uint32_t count) {
    if (res >= WAIT_OBJECT_0 && res < WAIT_OBJECT_0 + count) {
        return static_cast<int32_t>(res - WAIT_OBJECT_0);
    }
    if (res >= WAIT_ABANDONED_0 && res < WAIT_ABANDONED_0 + count) {
        return static_cast<int32_t>(0x00000080u + (res - WAIT_ABANDONED_0));
    }
    if (res == WAIT_IO_COMPLETION) {
        return 0x000000C0; // STATUS_USER_APC
    }
    if (res == WAIT_TIMEOUT) {
        return 0x00000102; // STATUS_TIMEOUT
    }
    return static_cast<int32_t>(0xC0000008u); // STATUS_INVALID_HANDLE
}

static void ConsumeDispatcherHeaderSatisfyLocked(XBOX_DISPATCHER_HEADER* hdr) {
    if (!hdr) return;
    switch (hdr->Type) {
        case 1u: // SynchronizationEvent (auto-reset)
        case 9u: // SynchronizationTimer (auto-reset)
            hdr->SignalState = 0;
            break;
        case 2u: // MutantObject
        case 5u: // SemaphoreObject
            if (hdr->SignalState > 0) {
                hdr->SignalState--;
            }
            break;
        default: // NotificationEvent (0), NotificationTimer (8)
            break;
    }
}

int32_t __stdcall Krnl_KeWaitForMultipleObjects(
    uint32_t Count, void** Objects, uint32_t WaitType, uint32_t WaitReason,
    uint8_t WaitMode, uint8_t Alertable, const int64_t* Timeout, void* WaitBlockArray)
{
    (void)WaitReason; (void)WaitMode; (void)WaitBlockArray;
    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS || !Objects) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }
    for (uint32_t i = 0; i < Count; ++i) {
        if (!Objects[i]) {
            return static_cast<int32_t>(0xC000000Du);
        }
    }

    if (!IsXboxKernelClockThreadRunning()) {
        PumpXboxKernelTimers();
    }

    uint64_t deadline100ns = ~0ULL;
    DWORD winTimeoutMs = ConvertNtTimeoutToWin32Ms(Timeout, &deadline100ns);

    // Fast native Win32 handle path when all objects come from ObReferenceObjectByHandle
    HANDLE winHandles[MAXIMUM_WAIT_OBJECTS] = {};
    bool allObRefs = true;
    AcquireSRWLockExclusive(&g_dispLock);
    for (uint32_t i = 0; i < Count; ++i) {
        XboxObRefObject* ob = LookupObRefObjectLocked(Objects[i]);
        if (!ob) {
            allObRefs = false;
            for (uint32_t j = 0; j < i; ++j) {
                XboxObRefObject* prevOb = LookupObRefObjectLocked(Objects[j]);
                if (prevOb) InterlockedDecrement(&prevOb->refCount);
            }
            break;
        }
        InterlockedIncrement(&ob->refCount);
        winHandles[i] = ob->winHandle;
    }
    if (allObRefs) {
        ReleaseSRWLockExclusive(&g_dispLock);
        DWORD w = (Count == 1u)
            ? WaitForSingleObjectEx(winHandles[0], winTimeoutMs, Alertable ? TRUE : FALSE)
            : WaitForMultipleObjectsEx(Count, winHandles, (WaitType == 0u) ? TRUE : FALSE, winTimeoutMs, Alertable ? TRUE : FALSE);
        for (uint32_t i = 0; i < Count; ++i) {
            Krnl_ObfDereferenceObject(Objects[i]);
        }
        return MapWin32WaitResultToNtStatus(w, Count);
    }

    // General in-memory DISPATCHER_HEADER* (KEVENT / KSEMAPHORE / KTIMER) + mixed wait loop
    for (;;) {
        if (WaitType == 1u) { // WaitAny
            for (uint32_t i = 0; i < Count; ++i) {
                XboxObRefObject* ob = LookupObRefObjectLocked(Objects[i]);
                if (ob) {
                    if (WaitForSingleObject(ob->winHandle, 0) == WAIT_OBJECT_0) {
                        ReleaseSRWLockExclusive(&g_dispLock);
                        return static_cast<int32_t>(i);
                    }
                } else {
                    XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Objects[i]);
                    if (hdr->SignalState > 0) {
                        ConsumeDispatcherHeaderSatisfyLocked(hdr);
                        ReleaseSRWLockExclusive(&g_dispLock);
                        return static_cast<int32_t>(i);
                    }
                }
            }
        } else { // WaitAll (WaitType == 0)
            bool   allSignaled = true;
            HANDLE uniqueObHandles[MAXIMUM_WAIT_OBJECTS] = {};
            DWORD  uniqueObCount = 0u;

            for (uint32_t i = 0; i < Count; ++i) {
                bool dup = false;
                for (uint32_t k = 0; k < i; ++k) {
                    if (Objects[k] == Objects[i]) { dup = true; break; }
                }
                if (dup) continue;

                XboxObRefObject* ob = LookupObRefObjectLocked(Objects[i]);
                if (ob) {
                    uniqueObHandles[uniqueObCount++] = ob->winHandle;
                } else {
                    XBOX_DISPATCHER_HEADER* hdr = static_cast<XBOX_DISPATCHER_HEADER*>(Objects[i]);
                    if (hdr->SignalState <= 0) {
                        allSignaled = false;
                        break;
                    }
                }
            }
            if (allSignaled) {
                // Atomically test and satisfy all unique Win32 ObRef handles in one WaitForMultipleObjects(TRUE, 0) call
                bool obAllOk = (uniqueObCount == 0u) ||
                               (WaitForMultipleObjects(uniqueObCount, uniqueObHandles, TRUE, 0u) == WAIT_OBJECT_0);
                if (obAllOk) {
                    for (uint32_t i = 0; i < Count; ++i) {
                        bool dup = false;
                        for (uint32_t k = 0; k < i; ++k) {
                            if (Objects[k] == Objects[i]) { dup = true; break; }
                        }
                        if (dup) continue;
                        if (!LookupObRefObjectLocked(Objects[i])) {
                            ConsumeDispatcherHeaderSatisfyLocked(static_cast<XBOX_DISPATCHER_HEADER*>(Objects[i]));
                        }
                    }
                    ReleaseSRWLockExclusive(&g_dispLock);
                    return 0; // STATUS_WAIT_0
                }
            }
        }

        uint64_t now100ns = QueryXbox100NsInterruptTime();
        if (deadline100ns != ~0ULL && now100ns >= deadline100ns) {
            ReleaseSRWLockExclusive(&g_dispLock);
            return 0x00000102; // STATUS_TIMEOUT
        }

        DWORD sliceMs = 10u;
        if (deadline100ns != ~0ULL) {
            uint64_t rem100ns = deadline100ns - now100ns;
            uint64_t remMs    = (rem100ns + 9999ULL) / 10000ULL;
            if (remMs == 0) remMs = 1;
            if (remMs < sliceMs) sliceMs = static_cast<DWORD>(remMs);
        }
        SleepConditionVariableSRW(&g_dispCv, &g_dispLock, sliceMs, 0);
        if (!IsXboxKernelClockThreadRunning()) {
            ReleaseSRWLockExclusive(&g_dispLock);
            PumpXboxKernelTimers();
            AcquireSRWLockExclusive(&g_dispLock);
        }
    }
}

int32_t __stdcall Krnl_KeWaitForSingleObject(
    void* Object, uint32_t WaitReason, uint8_t WaitMode, uint8_t Alertable, const int64_t* Timeout)
{
    return Krnl_KeWaitForMultipleObjects(1u, &Object, 1u, WaitReason, WaitMode, Alertable, Timeout, nullptr);
}

int32_t __stdcall Krnl_KeDelayExecutionThread(uint8_t WaitMode, uint8_t Alertable, const int64_t* Interval) {
    (void)WaitMode;
    if (!Interval) return 0;
    uint64_t deadline100ns = ~0ULL;
    DWORD totalMs = ConvertNtTimeoutToWin32Ms(Interval, &deadline100ns);
    if (totalMs == 0u) {
        SwitchToThread();
        if (!IsXboxKernelClockThreadRunning()) {
            PumpXboxKernelTimers();
        }
        return 0;
    }
    for (;;) {
        if (!IsXboxKernelClockThreadRunning()) {
            PumpXboxKernelTimers();
        }
        uint64_t now100ns = QueryXbox100NsInterruptTime();
        if (deadline100ns != ~0ULL && now100ns >= deadline100ns) {
            return 0;
        }
        DWORD sliceMs = 10u;
        if (deadline100ns != ~0ULL) {
            uint64_t remMs = ((deadline100ns - now100ns) + 9999ULL) / 10000ULL;
            if (remMs == 0) break;
            if (remMs < sliceMs) sliceMs = static_cast<DWORD>(remMs);
        }
        DWORD res = SleepEx(sliceMs, Alertable ? TRUE : FALSE);
        if (res == WAIT_IO_COMPLETION) {
            return 0x000000C0; // STATUS_USER_APC
        }
    }
    return 0;
}

void __stdcall Krnl_KeStallExecutionProcessor(uint32_t MicroSeconds) {
    if (MicroSeconds == 0) return;
    if (MicroSeconds > 50000u) MicroSeconds = 50000u; // Safety cap at 50 ms
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);
    LARGE_INTEGER start = {}, now = {};
    QueryPerformanceCounter(&start);
    uint64_t freq = static_cast<uint64_t>(g_xboxQpcFreq > 0 ? g_xboxQpcFreq : 10000000LL);
    uint64_t targetTicks = MulDivU64(MicroSeconds, freq, 1000000ULL);
    if (targetTicks == 0) targetTicks = 1;
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (static_cast<uint64_t>(now.QuadPart - start.QuadPart) < targetTicks);
}

int32_t __stdcall Krnl_NtYieldExecution() {
    return SwitchToThread() ? 0 : 0x40000024; // STATUS_SUCCESS or STATUS_NO_YIELD_PERFORMED
}

static const char* FormatLocalNtObjectName(const XBOX_OBJECT_ATTRIBUTES* oa, char outBuf[256]) {
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer || oa->ObjectName->Length == 0) {
        return nullptr;
    }
    const char* src = oa->ObjectName->Buffer;
    size_t len      = oa->ObjectName->Length;
    const char* leaf = src;
    for (size_t i = 0; i < len; ++i) {
        if (src[i] == '\\' || src[i] == '/') {
            leaf = src + i + 1;
        }
    }
    size_t leafLen = static_cast<size_t>((src + len) - leaf);
    if (leafLen == 0) return nullptr;
    if (leafLen > 180u) leafLen = 180u;
    int prefix = snprintf(outBuf, 256, "Local\\GhostXbox_%lu_", static_cast<unsigned long>(GetCurrentProcessId()));
    if (prefix <= 0 || static_cast<size_t>(prefix) + leafLen + 1u >= 256u) {
        return nullptr;
    }
    memcpy(outBuf + prefix, leaf, leafLen);
    outBuf[static_cast<size_t>(prefix) + leafLen] = '\0';
    return outBuf;
}

int32_t __stdcall Krnl_NtCreateEvent(
    HANDLE* EventHandle, const XBOX_OBJECT_ATTRIBUTES* ObjectAttributes, uint32_t EventType, uint8_t InitialState)
{
    if (!EventHandle) return static_cast<int32_t>(0xC000000Du);
    char nameBuf[256];
    const char* namePtr = FormatLocalNtObjectName(ObjectAttributes, nameBuf);
    SetLastError(0);
    // On Xbox: EventType 0 = NotificationEvent (manual-reset), 1 = SynchronizationEvent (auto-reset)
    HANDLE h = CreateEventA(nullptr, (EventType == 0u) ? TRUE : FALSE, InitialState ? TRUE : FALSE, namePtr);
    if (!h) {
        *EventHandle = nullptr;
        return static_cast<int32_t>(0xC0000017u);
    }
    DWORD lastErr = GetLastError();
    *EventHandle = h;
    return (namePtr && lastErr == ERROR_ALREADY_EXISTS) ? 0x40000000 : 0; // STATUS_OBJECT_NAME_EXISTS : STATUS_SUCCESS
}

int32_t __stdcall Krnl_NtSetEvent(HANDLE EventHandle, int32_t* PreviousState) {
    if (!EventHandle || EventHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    int32_t prev = PreviousState ? QueryWin32EventSignalStateNonDestructive(EventHandle) : 0;
    if (!SetEvent(EventHandle)) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (PreviousState) {
        *PreviousState = prev;
    }
    WakeAllConditionVariable(&g_dispCv);
    return 0;
}

int32_t __stdcall Krnl_NtClearEvent(HANDLE EventHandle) {
    if (!EventHandle || EventHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    return ResetEvent(EventHandle) ? 0 : static_cast<int32_t>(0xC0000008u);
}

int32_t __stdcall Krnl_NtPulseEvent(HANDLE EventHandle, int32_t* PreviousState) {
    if (!EventHandle || EventHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    int32_t prev = PreviousState ? QueryWin32EventSignalStateNonDestructive(EventHandle) : 0;
    if (!PulseEvent(EventHandle)) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (PreviousState) {
        *PreviousState = prev;
    }
    WakeAllConditionVariable(&g_dispCv);
    return 0;
}

int32_t __stdcall Krnl_NtCreateMutant(
    HANDLE* MutantHandle, const XBOX_OBJECT_ATTRIBUTES* ObjectAttributes, uint8_t InitialOwner)
{
    if (!MutantHandle) return static_cast<int32_t>(0xC000000Du);
    char nameBuf[256];
    const char* namePtr = FormatLocalNtObjectName(ObjectAttributes, nameBuf);
    SetLastError(0);
    HANDLE h = CreateMutexA(nullptr, InitialOwner ? TRUE : FALSE, namePtr);
    if (!h) {
        *MutantHandle = nullptr;
        return static_cast<int32_t>(0xC0000017u);
    }
    DWORD lastErr = GetLastError();
    *MutantHandle = h;
    return (namePtr && lastErr == ERROR_ALREADY_EXISTS) ? 0x40000000 : 0;
}

int32_t __stdcall Krnl_NtReleaseMutant(HANDLE MutantHandle, int32_t* PreviousCount) {
    if (!MutantHandle || MutantHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (PreviousCount) *PreviousCount = 0;
    return ReleaseMutex(MutantHandle) ? 0 : static_cast<int32_t>(0xC0000046u); // STATUS_MUTANT_NOT_OWNED
}

int32_t __stdcall Krnl_NtCreateSemaphore(
    HANDLE* SemaphoreHandle, const XBOX_OBJECT_ATTRIBUTES* ObjectAttributes, int32_t InitialCount, int32_t MaximumCount)
{
    if (!SemaphoreHandle || MaximumCount <= 0 || InitialCount < 0 || InitialCount > MaximumCount) {
        return static_cast<int32_t>(0xC000000Du);
    }
    char nameBuf[256];
    const char* namePtr = FormatLocalNtObjectName(ObjectAttributes, nameBuf);
    SetLastError(0);
    HANDLE h = CreateSemaphoreA(nullptr, InitialCount, MaximumCount, namePtr);
    if (!h) {
        *SemaphoreHandle = nullptr;
        return static_cast<int32_t>(0xC0000017u);
    }
    DWORD lastErr = GetLastError();
    *SemaphoreHandle = h;
    return (namePtr && lastErr == ERROR_ALREADY_EXISTS) ? 0x40000000 : 0;
}

int32_t __stdcall Krnl_NtReleaseSemaphore(HANDLE SemaphoreHandle, int32_t ReleaseCount, int32_t* PreviousCount) {
    if (!SemaphoreHandle || SemaphoreHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (ReleaseCount <= 0) {
        return static_cast<int32_t>(0xC000000Du);
    }
    LONG prev = 0;
    if (!ReleaseSemaphore(SemaphoreHandle, ReleaseCount, &prev)) {
        return static_cast<int32_t>(0xC0000047u); // STATUS_SEMAPHORE_LIMIT_EXCEEDED
    }
    if (PreviousCount) *PreviousCount = prev;
    return 0;
}

int32_t __stdcall Krnl_NtWaitForSingleObjectEx(
    HANDLE Handle, uint8_t WaitMode, uint8_t Alertable, const int64_t* Timeout)
{
    (void)WaitMode;
    if (!Handle || Handle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (!IsXboxKernelClockThreadRunning()) {
        PumpXboxKernelTimers();
    }
    uint64_t deadline = ~0ULL;
    DWORD ms = ConvertNtTimeoutToWin32Ms(Timeout, &deadline);
    DWORD res = WaitForSingleObjectEx(Handle, ms, Alertable ? TRUE : FALSE);
    return MapWin32WaitResultToNtStatus(res, 1u);
}

int32_t __stdcall Krnl_NtWaitForSingleObject(HANDLE Handle, uint8_t Alertable, const int64_t* Timeout) {
    return Krnl_NtWaitForSingleObjectEx(Handle, 1u, Alertable, Timeout);
}

int32_t __stdcall Krnl_NtWaitForMultipleObjectsEx(
    uint32_t Count, const HANDLE* Handles, uint32_t WaitType, uint8_t WaitMode, uint8_t Alertable, const int64_t* Timeout)
{
    (void)WaitMode;
    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS || !Handles) {
        return static_cast<int32_t>(0xC000000Du);
    }
    if (!IsXboxKernelClockThreadRunning()) {
        PumpXboxKernelTimers();
    }
    uint64_t deadline = ~0ULL;
    DWORD ms = ConvertNtTimeoutToWin32Ms(Timeout, &deadline);
    // On Xbox NT: WaitType 0 == WaitAll (TRUE), WaitType 1 == WaitAny (FALSE)
    DWORD res = WaitForMultipleObjectsEx(Count, Handles, (WaitType == 0u) ? TRUE : FALSE, ms, Alertable ? TRUE : FALSE);
    return MapWin32WaitResultToNtStatus(res, Count);
}

int32_t __stdcall Krnl_NtResumeThread(HANDLE ThreadHandle, uint32_t* PreviousSuspendCount) {
    if (!ThreadHandle || ThreadHandle == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(0xC0000008u);
    }
    DWORD prev = ResumeThread(ThreadHandle);
    if (prev == static_cast<DWORD>(-1)) {
        return static_cast<int32_t>(0xC0000008u);
    }
    if (PreviousSuspendCount) {
        *PreviousSuspendCount = prev;
    }
    return 0;
}

uint32_t __stdcall Krnl_RtlNtStatusToDosError(int32_t Status) {
    switch (static_cast<uint32_t>(Status)) {
        case 0x00000000u: return ERROR_SUCCESS;
        case 0x00000102u: return WAIT_TIMEOUT;
        case 0x00000103u: return ERROR_IO_PENDING;
        case 0x40000000u: return ERROR_ALREADY_EXISTS;
        case 0x80000005u: return ERROR_MORE_DATA;
        case 0x80000006u: return ERROR_NO_MORE_FILES;
        case 0xC0000001u: return ERROR_GEN_FAILURE;
        case 0xC0000002u: return ERROR_INVALID_FUNCTION;
        case 0xC0000005u: return ERROR_NOACCESS;
        case 0xC0000008u: return ERROR_INVALID_HANDLE;
        case 0xC000000Du: return ERROR_INVALID_PARAMETER;
        case 0xC000000Fu:
        case 0xC0000034u: return ERROR_FILE_NOT_FOUND;
        case 0xC0000011u: return ERROR_HANDLE_EOF;
        case 0xC0000017u:
        case 0xC000009Au: return ERROR_NOT_ENOUGH_MEMORY;
        case 0xC0000018u: return ERROR_INVALID_ADDRESS;
        case 0xC0000022u: return ERROR_ACCESS_DENIED;
        case 0xC0000023u: return ERROR_INSUFFICIENT_BUFFER;
        case 0xC0000033u: return ERROR_INVALID_NAME;
        case 0xC0000035u: return ERROR_ALREADY_EXISTS;
        case 0xC000003Au: return ERROR_PATH_NOT_FOUND;
        case 0xC0000043u: return ERROR_SHARING_VIOLATION;
        case 0xC0000046u: return ERROR_NOT_OWNER;
        case 0xC0000047u: return ERROR_TOO_MANY_POSTS;
        case 0xC000007Fu: return ERROR_DISK_FULL;
        case 0xC0000101u: return ERROR_DIR_NOT_EMPTY;
        case 0xC0000120u: return ERROR_OPERATION_ABORTED;
        default:          return (Status < 0) ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
    }
}

static void __stdcall Krnl_RtlInitAnsiString(XBOX_OBJECT_STRING* DestinationString, const char* SourceString) {
    if (!DestinationString) return;
    if (!SourceString) {
        DestinationString->Length        = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer        = nullptr;
        return;
    }
    size_t len = strlen(SourceString);
    if (len > 0xFFFEu) len = 0xFFFEu;
    DestinationString->Length        = static_cast<uint16_t>(len);
    DestinationString->MaximumLength = static_cast<uint16_t>(len + 1u);
    DestinationString->Buffer        = const_cast<char*>(SourceString);
}

static uint8_t __stdcall Krnl_RtlEqualString(
    const XBOX_OBJECT_STRING* String1, const XBOX_OBJECT_STRING* String2, uint8_t CaseInSensitive)
{
    if (!String1 || !String2) return 0;
    if (String1->Length != String2->Length) return 0;
    if (String1->Length == 0) return 1;
    if (!String1->Buffer || !String2->Buffer) return 0;
    if (CaseInSensitive) {
        return (_strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0) ? 1u : 0u;
    }
    return (memcmp(String1->Buffer, String2->Buffer, String1->Length) == 0) ? 1u : 0u;
}

static int32_t __stdcall Krnl_RtlAnsiStringToUnicodeString(
    XBOX_UNICODE_STRING* DestinationString, const XBOX_OBJECT_STRING* SourceString, uint8_t AllocateDestinationString)
{
    if (!DestinationString || !SourceString) return static_cast<int32_t>(0xC000000Du);
    uint16_t charCount = SourceString->Length;
    uint32_t byteLen   = static_cast<uint32_t>(charCount) * sizeof(wchar_t);
    uint32_t maxBytes  = byteLen + sizeof(wchar_t);
    if (maxBytes > 0xFFFFu) return static_cast<int32_t>(0xC000000Du);

    if (AllocateDestinationString) {
        wchar_t* buf = static_cast<wchar_t*>(Krnl_ExAllocatePool(maxBytes));
        if (!buf) return static_cast<int32_t>(0xC0000017u);
        DestinationString->Buffer        = buf;
        DestinationString->MaximumLength = static_cast<uint16_t>(maxBytes);
    } else if (!DestinationString->Buffer || DestinationString->MaximumLength < maxBytes) {
        return static_cast<int32_t>(0x80000005u); // STATUS_BUFFER_OVERFLOW
    }

    for (uint16_t i = 0; i < charCount; ++i) {
        DestinationString->Buffer[i] = static_cast<uint8_t>(SourceString->Buffer[i]);
    }
    DestinationString->Buffer[charCount] = L'\0';
    DestinationString->Length            = static_cast<uint16_t>(byteLen);
    return 0;
}

static int32_t __stdcall Krnl_RtlUnicodeStringToAnsiString(
    XBOX_OBJECT_STRING* DestinationString, const XBOX_UNICODE_STRING* SourceString, uint8_t AllocateDestinationString)
{
    if (!DestinationString || !SourceString) return static_cast<int32_t>(0xC000000Du);
    uint16_t charCount = static_cast<uint16_t>(SourceString->Length / sizeof(wchar_t));
    uint32_t maxBytes  = static_cast<uint32_t>(charCount) + 1u;
    if (maxBytes > 0xFFFFu) return static_cast<int32_t>(0xC000000Du);

    if (AllocateDestinationString) {
        char* buf = static_cast<char*>(Krnl_ExAllocatePool(maxBytes));
        if (!buf) return static_cast<int32_t>(0xC0000017u);
        DestinationString->Buffer        = buf;
        DestinationString->MaximumLength = static_cast<uint16_t>(maxBytes);
    } else if (!DestinationString->Buffer || DestinationString->MaximumLength < maxBytes) {
        return static_cast<int32_t>(0x80000005u);
    }

    for (uint16_t i = 0; i < charCount; ++i) {
        wchar_t wc = SourceString->Buffer[i];
        DestinationString->Buffer[i] = (wc < 0x100u) ? static_cast<char>(wc) : '?';
    }
    DestinationString->Buffer[charCount] = '\0';
    DestinationString->Length            = charCount;
    return 0;
}

static uint32_t __stdcall Krnl_RtlCompareMemoryUlong(const void* Source, uint32_t Length, uint32_t Pattern) {
    if (!Source || Length < sizeof(uint32_t)) return 0;
    const uint32_t* p = static_cast<const uint32_t*>(Source);
    uint32_t count = Length / sizeof(uint32_t);
    uint32_t i = 0;
    while (i < count && p[i] == Pattern) {
        ++i;
    }
    return i * static_cast<uint32_t>(sizeof(uint32_t));
}

static void __stdcall Krnl_RtlTimeToTimeFields(const int64_t* Time, XBOX_TIME_FIELDS* TimeFields) {
    if (!Time || !TimeFields) return;
    FILETIME ft = {};
    ft.dwLowDateTime  = static_cast<DWORD>(*Time & 0xFFFFFFFFLL);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(*Time) >> 32);
    SYSTEMTIME st = {};
    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = static_cast<int16_t>(st.wYear);
        TimeFields->Month        = static_cast<int16_t>(st.wMonth);
        TimeFields->Day          = static_cast<int16_t>(st.wDay);
        TimeFields->Hour         = static_cast<int16_t>(st.wHour);
        TimeFields->Minute       = static_cast<int16_t>(st.wMinute);
        TimeFields->Second       = static_cast<int16_t>(st.wSecond);
        TimeFields->Milliseconds = static_cast<int16_t>(st.wMilliseconds);
        TimeFields->Weekday      = static_cast<int16_t>(st.wDayOfWeek);
    } else {
        memset(TimeFields, 0, sizeof(*TimeFields));
    }
}

static uint8_t __stdcall Krnl_RtlTimeFieldsToTime(const XBOX_TIME_FIELDS* TimeFields, int64_t* Time) {
    if (!TimeFields || !Time) return 0;
    SYSTEMTIME st = {};
    st.wYear         = static_cast<WORD>(TimeFields->Year);
    st.wMonth        = static_cast<WORD>(TimeFields->Month);
    st.wDay          = static_cast<WORD>(TimeFields->Day);
    st.wHour         = static_cast<WORD>(TimeFields->Hour);
    st.wMinute       = static_cast<WORD>(TimeFields->Minute);
    st.wSecond       = static_cast<WORD>(TimeFields->Second);
    st.wMilliseconds = static_cast<WORD>(TimeFields->Milliseconds);
    FILETIME ft = {};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return 0;
    }
    *Time = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
    return 1;
}

// Managed Xbox Thread Registry & Trampoline
struct ManagedXboxThreadStart {
    LPTHREAD_START_ROUTINE     userStart;
    void*                      userParam;
    void (__stdcall *sysRoutine)(void*, void*);
    void*                      sysCtx1;
    void*                      sysCtx2;
    bool                       isXapiThread;
};

struct ManagedXboxThreadEntry {
    DWORD  tid;
    HANDLE dupHandle;
    bool   isXapiThread;
    bool   notifyAttachDone;
};

static SRWLOCK                             g_managedThreadsLock = SRWLOCK_INIT;
static std::vector<ManagedXboxThreadEntry> g_managedThreads;

static void CallXapiThreadNotifyIfLoaded(BOOL fCreate) {
    // _XapiCallThreadNotifyRoutines@4 at 0x005A2429 starts with `push ebx; push esi; push edi` (53 56 57)
    const uint8_t* p = reinterpret_cast<const uint8_t*>(0x005A2429u);
    if (p[0] == 0x53u && p[1] == 0x56u && p[2] == 0x57u) {
        typedef void (__stdcall *PFN_XapiCallThreadNotify)(BOOL);
        reinterpret_cast<PFN_XapiCallThreadNotify>(0x005A2429u)(fCreate);
    }
}

static void UnregisterCurrentManagedXboxThread(bool runDetachNotify) {
    DWORD tid = GetCurrentThreadId();
    HANDLE toClose = nullptr;
    bool needDetach = false;

    AcquireSRWLockExclusive(&g_managedThreadsLock);
    for (size_t i = 0; i < g_managedThreads.size(); ++i) {
        if (g_managedThreads[i].tid == tid) {
            toClose    = g_managedThreads[i].dupHandle;
            needDetach = runDetachNotify && g_managedThreads[i].isXapiThread && g_managedThreads[i].notifyAttachDone;
            g_managedThreads.erase(g_managedThreads.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_managedThreadsLock);

    if (needDetach) {
        CallXapiThreadNotifyIfLoaded(FALSE);
    }
    CleanupCurrentXboxThreadState();
    if (toClose) {
        CloseHandle(toClose);
    }
}

static DWORD WINAPI ManagedXboxThreadTrampoline(LPVOID rawParam) {
    ManagedXboxThreadStart* start = static_cast<ManagedXboxThreadStart*>(rawParam);
    ManagedXboxThreadStart local  = *start;
    free(start);

    EnsureXboxCrtTlsAllocated();
    InitThreadPtd();

    DWORD tid = GetCurrentThreadId();
    if (local.isXapiThread) {
        CallXapiThreadNotifyIfLoaded(TRUE);
        AcquireSRWLockExclusive(&g_managedThreadsLock);
        for (size_t i = 0; i < g_managedThreads.size(); ++i) {
            if (g_managedThreads[i].tid == tid) {
                g_managedThreads[i].notifyAttachDone = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_managedThreadsLock);
    }

    DWORD exitCode = 0;
    if (local.sysRoutine) {
        local.sysRoutine(local.sysCtx1, local.sysCtx2);
    } else if (local.userStart) {
        exitCode = local.userStart(local.userParam);
    }

    UnregisterCurrentManagedXboxThread(true);
    return exitCode;
}

HANDLE SpawnManagedXboxThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId,
    void (__stdcall *systemRoutine)(void*, void*),
    bool isXapiThread)
{
    if (!lpStartAddress && !systemRoutine) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ManagedXboxThreadStart* ctx = static_cast<ManagedXboxThreadStart*>(calloc(1, sizeof(ManagedXboxThreadStart)));
    if (!ctx) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    ctx->userStart    = lpStartAddress;
    ctx->userParam    = lpParameter;
    ctx->sysRoutine   = systemRoutine;
    ctx->sysCtx1      = reinterpret_cast<void*>(lpStartAddress);
    ctx->sysCtx2      = lpParameter;
    ctx->isXapiThread = isXapiThread;

    SIZE_T stackBytes = (dwStackSize < 65536u) ? 65536u : dwStackSize;
    DWORD  tid        = 0;
    HANDLE hThread    = CreateThread(
        lpThreadAttributes,
        stackBytes,
        ManagedXboxThreadTrampoline,
        ctx,
        dwCreationFlags | CREATE_SUSPENDED,
        &tid);
    if (!hThread) {
        free(ctx);
        return nullptr;
    }

    HANDLE dupHandle = nullptr;
    DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(), &dupHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);

    AcquireSRWLockExclusive(&g_managedThreadsLock);
    for (size_t i = 0; i < g_managedThreads.size(); ) {
        if (g_managedThreads[i].dupHandle && WaitForSingleObject(g_managedThreads[i].dupHandle, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_managedThreads[i].dupHandle);
            g_managedThreads.erase(g_managedThreads.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    ManagedXboxThreadEntry entry = { tid, dupHandle, isXapiThread, false };
    g_managedThreads.push_back(entry);
    ReleaseSRWLockExclusive(&g_managedThreadsLock);

    if ((dwCreationFlags & CREATE_SUSPENDED) == 0) {
        ResumeThread(hThread);
    }
    if (lpThreadId) {
        *lpThreadId = tid;
    }
    return hThread;
}

int32_t __stdcall Krnl_PsCreateSystemThreadEx(
    HANDLE* ThreadHandle,
    uint32_t ThreadExtraSize,
    uint32_t KernelStackSize,
    uint32_t TlsDataSize,
    uint32_t* ThreadId,
    void* StartContext1,
    void* StartContext2,
    uint8_t CreateSuspended,
    uint8_t DebugStack,
    void* SystemRoutine)
{
    (void)ThreadExtraSize; (void)TlsDataSize; (void)DebugStack;
    if (!ThreadHandle || !SystemRoutine) {
        return static_cast<int32_t>(0xC000000Du); // STATUS_INVALID_PARAMETER
    }

    DWORD flags = CreateSuspended ? CREATE_SUSPENDED : 0u;
    DWORD tid   = 0;
    HANDLE h    = nullptr;

    // 0x005A2574 is _XapiThreadStartup@8: StartContext1 = lpStartAddress, StartContext2 = lpParameter
    if (reinterpret_cast<uintptr_t>(SystemRoutine) == 0x005A2574u) {
        h = SpawnManagedXboxThread(
            nullptr,
            KernelStackSize ? KernelStackSize : 65536u,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(StartContext1),
            StartContext2,
            flags,
            &tid,
            nullptr,
            true);
    } else {
        h = SpawnManagedXboxThread(
            nullptr,
            KernelStackSize ? KernelStackSize : 65536u,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(StartContext1),
            StartContext2,
            flags,
            &tid,
            reinterpret_cast<void(__stdcall*)(void*, void*)>(SystemRoutine),
            false);
    }

    if (!h) {
        *ThreadHandle = nullptr;
        return static_cast<int32_t>(0xC0000017u); // STATUS_NO_MEMORY
    }
    *ThreadHandle = h;
    if (ThreadId) {
        *ThreadId = tid;
    }
    return 0; // STATUS_SUCCESS
}

int32_t __stdcall Krnl_PsTerminateSystemThread(int32_t ExitStatus) {
    UnregisterCurrentManagedXboxThread(true);
    ExitThread(static_cast<DWORD>(ExitStatus));
}

static void __stdcall Bridge_XapiThreadStartup(LPTHREAD_START_ROUTINE StartRoutine, LPVOID StartContext) {
    EnsureXboxCrtTlsAllocated();
    InitThreadPtd();
    CallXapiThreadNotifyIfLoaded(TRUE);
    DWORD exitCode = StartRoutine ? StartRoutine(StartContext) : 0u;
    CallXapiThreadNotifyIfLoaded(FALSE);
    UnregisterCurrentManagedXboxThread(false);
    ExitThread(exitCode);
}

void ShutdownXboxKernel() {
    if (InterlockedExchange(&g_kernelClockRunning, 0) != 0) {
        if (g_kernelClockStopEvent) {
            SetEvent(g_kernelClockStopEvent);
        }
        if (g_kernelClockThread) {
            WaitForSingleObject(g_kernelClockThread, 500u);
            CloseHandle(g_kernelClockThread);
            g_kernelClockThread = nullptr;
        }
    } else if (g_kernelClockThread) {
        CloseHandle(g_kernelClockThread);
        g_kernelClockThread = nullptr;
    }
    if (g_kernelClockStopEvent) {
        CloseHandle(g_kernelClockStopEvent);
        g_kernelClockStopEvent = nullptr;
    }

    WakeAllConditionVariable(&g_dispCv);

    DWORD selfTid = GetCurrentThreadId();
    std::vector<HANDLE> waitThreads;
    AcquireSRWLockExclusive(&g_managedThreadsLock);
    for (size_t i = 0; i < g_managedThreads.size(); ++i) {
        if (g_managedThreads[i].dupHandle) {
            if (g_managedThreads[i].tid != selfTid) {
                waitThreads.push_back(g_managedThreads[i].dupHandle);
            } else {
                CloseHandle(g_managedThreads[i].dupHandle);
            }
        }
    }
    g_managedThreads.clear();
    ReleaseSRWLockExclusive(&g_managedThreadsLock);

    for (size_t i = 0; i < waitThreads.size(); ++i) {
        WaitForSingleObject(waitThreads[i], 100u);
        CloseHandle(waitThreads[i]);
    }

    AcquireSRWLockExclusive(&g_kernelTimerLock);
    g_activeTimers.clear();
    g_queuedDpcs.clear();
    ReleaseSRWLockExclusive(&g_kernelTimerLock);

    std::vector<XboxObRefObject*> leakedObRefs;
    AcquireSRWLockExclusive(&g_dispLock);
    leakedObRefs.swap(g_obRefObjects);
    ReleaseSRWLockExclusive(&g_dispLock);

    for (size_t i = 0; i < leakedObRefs.size(); ++i) {
        XboxObRefObject* ob = leakedObRefs[i];
        if (ob) {
            ob->magic = 0;
            if (ob->winHandle) {
                CloseHandle(ob->winHandle);
                ob->winHandle = nullptr;
            }
            free(ob);
        }
    }
}

// ============================================================================
// Xbox Kernel Crypto Functions (FIPS 180-1 SHA-1, RFC 2104 HMAC-SHA1, PKCS#1)
// ============================================================================

#pragma pack(push, 1)
struct XBOX_SHA_CTX {
    uint8_t  Buffer[64]; // +0x00
    uint32_t State[5];   // +0x40
    uint32_t Count[2];   // +0x54 (Count[0] = low bytes, Count[1] = high bytes)
};
#pragma pack(pop)

static inline uint32_t ShaRotl32(uint32_t v, uint32_t s) {
    return (v << s) | (v >> (32u - s));
}

static void Sha1TransformBlock(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8)  |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = ShaRotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = ShaRotl32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = ShaRotl32(b, 30u);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void __stdcall Krnl_XcSHAInit(uint8_t* pbSHAContext) {
    if (!pbSHAContext) return;
    XBOX_SHA_CTX* ctx = reinterpret_cast<XBOX_SHA_CTX*>(pbSHAContext);
    memset(ctx, 0, sizeof(*ctx));
    ctx->State[0] = 0x67452301u;
    ctx->State[1] = 0xEFCDAB89u;
    ctx->State[2] = 0x98BADCFEu;
    ctx->State[3] = 0x10325476u;
    ctx->State[4] = 0xC3D2E1F0u;
}

void __stdcall Krnl_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength) {
    if (!pbSHAContext || !pbInput || dwInputLength == 0) return;
    XBOX_SHA_CTX* ctx = reinterpret_cast<XBOX_SHA_CTX*>(pbSHAContext);
    if (ctx->State[0] == 0 && ctx->State[1] == 0 && ctx->State[2] == 0 && ctx->Count[0] == 0) {
        Krnl_XcSHAInit(pbSHAContext);
    }

    uint32_t idx = ctx->Count[0] & 63u;
    uint32_t newLow = ctx->Count[0] + dwInputLength;
    if (newLow < ctx->Count[0]) {
        ctx->Count[1]++;
    }
    ctx->Count[0] = newLow;

    uint32_t i = 0;
    if (idx > 0) {
        uint32_t fill = 64u - idx;
        if (dwInputLength < fill) {
            memcpy(ctx->Buffer + idx, pbInput, dwInputLength);
            return;
        }
        memcpy(ctx->Buffer + idx, pbInput, fill);
        Sha1TransformBlock(ctx->State, ctx->Buffer);
        i = fill;
    }

    for (; i + 64u <= dwInputLength; i += 64u) {
        Sha1TransformBlock(ctx->State, pbInput + i);
    }

    if (i < dwInputLength) {
        memcpy(ctx->Buffer, pbInput + i, dwInputLength - i);
    }
}

void __stdcall Krnl_XcSHAFinal(uint8_t* pbSHAContext, uint8_t* pbDigest) {
    if (!pbSHAContext || !pbDigest) return;
    XBOX_SHA_CTX* ctx = reinterpret_cast<XBOX_SHA_CTX*>(pbSHAContext);

    uint64_t totalBits = ((static_cast<uint64_t>(ctx->Count[1]) << 32) | ctx->Count[0]) * 8ULL;
    uint8_t padLenByte[8];
    for (int i = 0; i < 8; ++i) {
        padLenByte[7 - i] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFFu);
    }

    uint8_t oneBit = 0x80u;
    Krnl_XcSHAUpdate(pbSHAContext, &oneBit, 1u);

    uint8_t zeroPad[64] = { 0 };
    uint32_t curMod = ctx->Count[0] & 63u;
    uint32_t padZeros = (curMod <= 56u) ? (56u - curMod) : (120u - curMod);
    if (padZeros > 0) {
        Krnl_XcSHAUpdate(pbSHAContext, zeroPad, padZeros);
    }
    Krnl_XcSHAUpdate(pbSHAContext, padLenByte, 8u);

    for (int i = 0; i < 5; ++i) {
        pbDigest[i * 4 + 0] = static_cast<uint8_t>((ctx->State[i] >> 24) & 0xFFu);
        pbDigest[i * 4 + 1] = static_cast<uint8_t>((ctx->State[i] >> 16) & 0xFFu);
        pbDigest[i * 4 + 2] = static_cast<uint8_t>((ctx->State[i] >> 8)  & 0xFFu);
        pbDigest[i * 4 + 3] = static_cast<uint8_t>(ctx->State[i] & 0xFFu);
    }
}

void __stdcall Krnl_XcHMAC(
    const uint8_t* pbKey, uint32_t dwKeyLength,
    const uint8_t* pbInput, uint32_t dwInputLength,
    const uint8_t* pbInput2, uint32_t dwInputLength2,
    uint8_t* pbDigest)
{
    if (!pbDigest) return;
    uint8_t keyBlock[64] = { 0 };
    if (pbKey && dwKeyLength > 0) {
        if (dwKeyLength > 64u) {
            // Xbox XcHMAC truncates keys > 64 bytes to 64 bytes (or hashes them in standard RFC 2104; Xbox kernel truncates at 64)
            memcpy(keyBlock, pbKey, 64u);
        } else {
            memcpy(keyBlock, pbKey, dwKeyLength);
        }
    }

    uint8_t kIpad[64];
    uint8_t kOpad[64];
    for (int i = 0; i < 64; ++i) {
        kIpad[i] = keyBlock[i] ^ 0x36u;
        kOpad[i] = keyBlock[i] ^ 0x5Cu;
    }

    uint8_t innerCtx[116];
    uint8_t innerDigest[20];
    Krnl_XcSHAInit(innerCtx);
    Krnl_XcSHAUpdate(innerCtx, kIpad, 64u);
    if (pbInput && dwInputLength > 0) {
        Krnl_XcSHAUpdate(innerCtx, pbInput, dwInputLength);
    }
    if (pbInput2 && dwInputLength2 > 0) {
        Krnl_XcSHAUpdate(innerCtx, pbInput2, dwInputLength2);
    }
    Krnl_XcSHAFinal(innerCtx, innerDigest);

    uint8_t outerCtx[116];
    Krnl_XcSHAInit(outerCtx);
    Krnl_XcSHAUpdate(outerCtx, kOpad, 64u);
    Krnl_XcSHAUpdate(outerCtx, innerDigest, 20u);
    Krnl_XcSHAFinal(outerCtx, pbDigest);
}

// Multi-precision modular exponentiation: pA = (pB ^ pC) mod pD (little-endian uint32_t[dwLen])
static void BigIntSub(uint32_t* a, const uint32_t* b, uint32_t n) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t diff = static_cast<uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<uint32_t>(diff);
        borrow = (diff >> 63) & 1u;
    }
}

static int BigIntCmp(const uint32_t* a, const uint32_t* b, uint32_t n) {
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        if (a[i] != b[i]) return (a[i] > b[i]) ? 1 : -1;
    }
    return 0;
}

static void BigIntMulMod(uint32_t* out, const uint32_t* a, const uint32_t* b, const uint32_t* m, uint32_t n) {
    std::vector<uint32_t> prod(n * 2u + 1u, 0u);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < n; ++j) {
            uint64_t cur = prod[i + j] + static_cast<uint64_t>(a[i]) * b[j] + carry;
            prod[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        prod[i + n] += static_cast<uint32_t>(carry);
    }

    // Bitwise shift-and-subtract reduction mod m
    std::vector<uint32_t> rem(n + 1u, 0u);
    std::vector<uint32_t> mExt(n + 1u, 0u);
    for (uint32_t i = 0; i < n; ++i) mExt[i] = m[i];

    for (int bit = static_cast<int>(n * 64u) - 1; bit >= 0; --bit) {
        uint32_t carry = 0;
        for (uint32_t i = 0; i <= n; ++i) {
            uint32_t nextCarry = rem[i] >> 31;
            rem[i] = (rem[i] << 1) | carry;
            carry = nextCarry;
        }
        rem[0] |= (prod[bit >> 5] >> (bit & 31)) & 1u;
        if (BigIntCmp(rem.data(), mExt.data(), n + 1u) >= 0) {
            BigIntSub(rem.data(), mExt.data(), n + 1u);
        }
    }
    for (uint32_t i = 0; i < n; ++i) out[i] = rem[i];
}

static uint32_t __stdcall Krnl_XcModExp(
    uint32_t* pA, const uint32_t* pB, const uint32_t* pC, const uint32_t* pD, uint32_t dwLen)
{
    if (!pA || !pB || !pC || !pD || dwLen == 0 || dwLen > 128u) return 0;
    bool modNonZero = false;
    for (uint32_t i = 0; i < dwLen; ++i) {
        if (pD[i] != 0) { modNonZero = true; break; }
    }
    if (!modNonZero) return 0;

    std::vector<uint32_t> result(dwLen, 0u);
    std::vector<uint32_t> base(dwLen, 0u);
    result[0] = 1u;
    for (uint32_t i = 0; i < dwLen; ++i) base[i] = pB[i];

    int highestBit = static_cast<int>(dwLen * 32u) - 1;
    while (highestBit >= 0 && ((pC[highestBit >> 5] >> (highestBit & 31)) & 1u) == 0) {
        --highestBit;
    }
    for (int bit = 0; bit <= highestBit; ++bit) {
        if ((pC[bit >> 5] >> (bit & 31)) & 1u) {
            BigIntMulMod(result.data(), result.data(), base.data(), pD, dwLen);
        }
        if (bit < highestBit) {
            BigIntMulMod(base.data(), base.data(), base.data(), pD, dwLen);
        }
    }
    for (uint32_t i = 0; i < dwLen; ++i) pA[i] = result[i];
    return 1;
}

static uint32_t __stdcall Krnl_XcVerifyPKCS1Signature(const uint8_t* pbSig, const uint8_t* pbPubKey, const uint8_t* pbDigest) {
    if (!pbSig || !pbPubKey || !pbDigest) return 0;
    uint32_t magic  = *reinterpret_cast<const uint32_t*>(pbPubKey + 0x00);
    uint32_t bitlen = *reinterpret_cast<const uint32_t*>(pbPubKey + 0x08);
    uint32_t pubexp = *reinterpret_cast<const uint32_t*>(pbPubKey + 0x10);
    if (magic == 0x31415352u && bitlen >= 512u && bitlen <= 2048u && (bitlen & 31u) == 0u && pubexp != 0u) {
        uint32_t dwLen = bitlen / 32u;
        uint32_t byteLen = bitlen / 8u;
        const uint32_t* pMod = reinterpret_cast<const uint32_t*>(pbPubKey + 0x14);
        bool modValid = false;
        for (uint32_t i = 0; i < dwLen; ++i) {
            if (pMod[i] != 0) { modValid = true; break; }
        }
        if (modValid) {
            std::vector<uint32_t> expWords(dwLen, 0u);
            std::vector<uint32_t> outWords(dwLen, 0u);
            expWords[0] = pubexp;
            if (Krnl_XcModExp(outWords.data(), reinterpret_cast<const uint32_t*>(pbSig), expWords.data(), pMod, dwLen)) {
                const uint8_t* dec = reinterpret_cast<const uint8_t*>(outWords.data());
                // BSAFE little-endian PKCS#1 v1.5 layout: dec[0..19] = reversed SHA-1 digest
                bool match = true;
                for (int i = 0; i < 20; ++i) {
                    if (dec[i] != pbDigest[19 - i] && dec[byteLen - 20 + i] != pbDigest[i]) {
                        match = false;
                        break;
                    }
                }
                return match ? 1u : 0u;
            }
        }
    }
    return 1;
}

// AV functions
static void* g_avSavedDataAddress = nullptr;
static void* __stdcall Krnl_AvGetSavedDataAddress() { return g_avSavedDataAddress; }
static void __stdcall Krnl_AvSetSavedDataAddress(void* addr) { g_avSavedDataAddress = addr; }
static void __stdcall Krnl_AvSendTVEncoderOption(void* a, uint32_t opt, uint32_t param, uint32_t* res) {
    (void)a; (void)opt; (void)param;
    if (res) *res = 0;
}
static uint32_t __stdcall Krnl_AvSetDisplayMode(void* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, void* f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}

// Generic kernel stubs by arg count
static uint32_t __stdcall Krnl_Stub0() { return 0; }
static uint32_t __stdcall Krnl_Stub1(uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub2(uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub3(uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub4(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub7(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub8(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub9(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub10(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static void     __fastcall Krnl_FastStub2(uint32_t, uint32_t) {}
static int32_t  __stdcall Krnl_XeLoadUnloadSection(uint32_t* pSec) {
    if (pSec && (uintptr_t)pSec >= kXboxPeImageBase && (uintptr_t)pSec < kXboxReservedEndVa) {
        pSec[5] = 1; // SectionReferenceCount > 0
    }
    return 0;
}

static void __stdcall Krnl_RtlRaiseException(EXCEPTION_RECORD* ExceptionRecord) {
    if (!ExceptionRecord) {
        RaiseException(STATUS_NONCONTINUABLE_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return;
    }
    DWORD nParams = ExceptionRecord->NumberParameters;
    if (nParams > EXCEPTION_MAXIMUM_PARAMETERS) {
        nParams = EXCEPTION_MAXIMUM_PARAMETERS;
    }
    RaiseException(
        ExceptionRecord->ExceptionCode,
        ExceptionRecord->ExceptionFlags,
        nParams,
        nParams ? ExceptionRecord->ExceptionInformation : nullptr);
}

// XACT Engine DoWork hook: pumps due KTIMER/KDPC entries (if clock thread is stopped) and runs CEngine::DoWork
static void __stdcall Bridge_XACTEngineDoWork() {
    typedef int (__stdcall *PFN_XACTEnterCS)();
    typedef void (__stdcall *PFN_DSDoWork)();
    typedef void (__thiscall *PFN_CEngineDoWork)(void* thisPtr);

    int entered = reinterpret_cast<PFN_XACTEnterCS>(0x006FE3E0)();
    void* pEngine = *reinterpret_cast<void**>(ADDR_XACT_ENGINE_PTR); // _g_pXACTEngine
    if (pEngine) {
        reinterpret_cast<PFN_DSDoWork>(0x006E1A58)();             // _DirectSoundDoWork@0
        reinterpret_cast<PFN_CEngineDoWork>(0x00700056)(pEngine); // ?DoWork@CEngine@XACT@@QAEXXZ
    }
    if (entered) {
        Bridge_RtlLeaveCriticalSection(reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION));
    }
    if (!IsXboxKernelClockThreadRunning()) {
        PumpXboxKernelTimers();
    }
}

void InitXboxKernel() {
    InitOnceExecuteOnce(&g_xboxQpcInitOnce, InitXboxQpcBaseCallback, nullptr, nullptr);

    // Derive XboxLANKey, XboxSignatureKey, and XboxAlternateSignatureKeys from XBE Certificate at ds:[kXbeHeaderBaseVa + 0x118]
    // using the Xbox kernel's XcHMAC(XboxCERTKey, 16, certKey, 16, nullptr, 0, digest)[0..15] derivation
    uint32_t certAddr = *reinterpret_cast<const uint32_t*>(kXbeHeaderBaseVa + 0x118u);
    if (certAddr >= kXbeHeaderBaseVa + 0x100u && certAddr + 0x1D0u <= kXbeHeaderBaseVa + kXbeHeaderMaxSize) {
        uint8_t digest[20];
        const uint8_t* certBytes = reinterpret_cast<const uint8_t*>(certAddr);
        Krnl_XcHMAC(g_xboxCERTKey, 16, certBytes + 0xB0, 16, nullptr, 0, digest);
        memcpy(g_xboxLANKey, digest, 16);
        Krnl_XcHMAC(g_xboxCERTKey, 16, certBytes + 0xC0, 16, nullptr, 0, digest);
        memcpy(g_xboxSignatureKey, digest, 16);
        for (int i = 0; i < 16; ++i) {
            Krnl_XcHMAC(g_xboxCERTKey, 16, certBytes + 0xD0 + i * 16, 16, nullptr, 0, digest);
            memcpy(g_xboxAltSignatureKeys[i], digest, 16);
        }
    }

    // Initialize self-referential empty LIST_ENTRY inside g_idexChannelObj (+0x28) for 0x0074FF20
    g_idexChannelObj[0x28 / 4] = reinterpret_cast<uint32_t>(&g_idexChannelObj[0x28 / 4]);
    g_idexChannelObj[0x2C / 4] = reinterpret_cast<uint32_t>(&g_idexChannelObj[0x28 / 4]);

    // 1. Initialize all 137 xboxkrnl.exe IAT slots (0x0074FE80..0x007500A0) with safe defaults first
    for (uint32_t addr = 0x0074FE80; addr <= 0x007500A0; addr += 4) {
        PatchU32(addr, reinterpret_cast<uint32_t>(&Krnl_Stub0));
    }

    // 2. Populate exact xboxkrnl.exe IAT slots (100% verified against Ghost.exe __imp__ symbols)
    PatchU32(0x0074FE80, reinterpret_cast<uint32_t>(&Krnl_ExQueryNonVolatileSetting)); // ord  24: __imp__ExQueryNonVolatileSetting@20
    PatchU32(0x0074FE84, reinterpret_cast<uint32_t>(&Krnl_DbgBreakPoint));             // ord   5: __imp__DbgBreakPoint@0
    PatchU32(0x0074FE88, reinterpret_cast<uint32_t>(&Krnl_KeQuerySystemTime));         // ord 128: __imp__KeQuerySystemTime@4
    PatchU32(0x0074FE8C, reinterpret_cast<uint32_t>(&Krnl_MmPersistContiguousMemory)); // ord 178: __imp__MmPersistContiguousMemory@12
    PatchU32(0x0074FE90, reinterpret_cast<uint32_t>(&Krnl_MmAllocateContiguousMemory));// ord 165: __imp__MmAllocateContiguousMemory@4
    PatchU32(0x0074FE94, reinterpret_cast<uint32_t>(g_launchDataPage));                // ord 164: _LaunchDataPage (DATA)
    PatchU32(0x0074FE98, reinterpret_cast<uint32_t>(&Krnl_MmFreeContiguousMemory));    // ord 171: __imp__MmFreeContiguousMemory@4
    PatchU32(0x0074FE9C, reinterpret_cast<uint32_t>(&Krnl_RtlNtStatusToDosError));     // ord 301: __imp__RtlNtStatusToDosError@4
    PatchU32(0x0074FEA0, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  49: __imp__HalReturnToFirmware@4
    PatchU32(0x0074FEA4, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 226: __imp__NtSetInformationFile@20
    PatchU32(0x0074FEA8, reinterpret_cast<uint32_t>(&Krnl_Stub9));                     // ord 190: __imp__NtCreateFile@36
    PatchU32(0x0074FEAC, reinterpret_cast<uint32_t>(&Krnl_RtlInitAnsiString));         // ord 289: __imp__RtlInitAnsiString@8
    PatchU32(0x0074FEB0, reinterpret_cast<uint32_t>(&Krnl_XcSHAUpdate));               // ord 336: __imp__XcSHAUpdate@12
    PatchU32(0x0074FEB4, reinterpret_cast<uint32_t>(&Krnl_XcSHAInit));                 // ord 335: __imp__XcSHAInit@4
    PatchU32(0x0074FEB8, reinterpret_cast<uint32_t>(&Krnl_XcSHAFinal));                // ord 337: __imp__XcSHAFinal@8
    PatchU32(0x0074FEBC, reinterpret_cast<uint32_t>(g_xboxHDKey));                     // ord 323: _XboxHDKey (DATA)
    PatchU32(0x0074FEC0, reinterpret_cast<uint32_t>(g_xboxSignatureKey));              // ord 325: __imp__XboxSignatureKey (DATA)
    PatchU32(0x0074FEC4, reinterpret_cast<uint32_t>(g_xboxAltSignatureKeys));          // ord 354: __imp__XboxAlternateSignatureKeys (DATA)
    PatchU32(0x0074FEC8, reinterpret_cast<uint32_t>(&Krnl_NtClose));                   // ord 187: __imp__NtClose@4
    PatchU32(0x0074FECC, reinterpret_cast<uint32_t>(&Krnl_NtWaitForSingleObject));     // ord 233: __imp__NtWaitForSingleObject@12
    PatchU32(0x0074FED0, reinterpret_cast<uint32_t>(&Krnl_Stub8));                     // ord 219: __imp__NtReadFile@32
    PatchU32(0x0074FED4, reinterpret_cast<uint32_t>(&Krnl_Stub8));                     // ord 236: __imp__NtWriteFile@32
    PatchU32(0x0074FED8, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 211: __imp__NtQueryInformationFile@20
    PatchU32(0x0074FEDC, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 218: __imp__NtQueryVolumeInformationFile@20
    PatchU32(0x0074FEE0, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 198: __imp__NtFlushBuffersFile@8
    PatchU32(0x0074FEE4, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 200: __imp__NtFsControlFile@40
    PatchU32(0x0074FEE8, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 196: __imp__NtDeviceIoControlFile@40
    PatchU32(0x0074FEEC, reinterpret_cast<uint32_t>(&Krnl_ObfDereferenceObject));      // ord 250: __imp_@ObfDereferenceObject@4
    PatchU32(0x0074FEF0, reinterpret_cast<uint32_t>(&Krnl_ObReferenceObjectByHandle)); // ord 246: __imp__ObReferenceObjectByHandle@12
    PatchU32(0x0074FEF4, reinterpret_cast<uint32_t>(g_dummyObjectType));               // ord  71: __imp__IoFileObjectType (DATA)
    PatchU32(0x0074FEF8, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord 202: __imp__NtOpenFile@24
    PatchU32(0x0074FEFC, reinterpret_cast<uint32_t>(&Krnl_RtlTimeToTimeFields));       // ord 305: __imp__RtlTimeToTimeFields@8
    PatchU32(0x0074FF00, reinterpret_cast<uint32_t>(&g_keTickCount));                  // ord 156: __imp__KeTickCount (DATA)
    PatchU32(0x0074FF04, reinterpret_cast<uint32_t>(&Krnl_RtlTimeFieldsToTime));       // ord 304: __imp__RtlTimeFieldsToTime@8
    PatchU32(0x0074FF08, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  37: __imp__FscSetCacheSize@4
    PatchU32(0x0074FF0C, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 207: __imp__NtQueryDirectoryFile@40
    PatchU32(0x0074FF10, reinterpret_cast<uint32_t>(&Krnl_FastStub2));                 // ord  87: __imp_@IofCompleteRequest@8
    PatchU32(0x0074FF14, reinterpret_cast<uint32_t>(&Krnl_KeInsertQueueDpc));          // ord 119: __imp__KeInsertQueueDpc@12
    PatchU32(0x0074FF18, reinterpret_cast<uint32_t>(&Krnl_KeInitializeDpc));           // ord 107: __imp__KeInitializeDpc@12
    PatchU32(0x0074FF1C, reinterpret_cast<uint32_t>(&Krnl_KfLowerIrql));               // ord 161: __imp_@KfLowerIrql@4
    PatchU32(0x0074FF20, reinterpret_cast<uint32_t>(g_idexChannelObj));                // ord 357: _IdexChannelObject (DATA)
    PatchU32(0x0074FF24, reinterpret_cast<uint32_t>(&Krnl_KeRaiseIrqlToDpcLevel));     // ord 129: __imp__KeRaiseIrqlToDpcLevel@0
    PatchU32(0x0074FF28, reinterpret_cast<uint32_t>(&g_halCachePartCount));            // ord  40: __imp__HalDiskCachePartitionCount (DATA)
    PatchU32(0x0074FF2C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  67: __imp__IoCreateSymbolicLink@8
    PatchU32(0x0074FF30, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 215: __imp__NtQuerySymbolicLinkObject@12
    PatchU32(0x0074FF34, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 203: __imp__NtOpenSymbolicLinkObject@8
    PatchU32(0x0074FF38, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  69: __imp__IoDeleteSymbolicLink@4
    PatchU32(0x0074FF3C, reinterpret_cast<uint32_t>(&Krnl_MmAllocateContiguousMemoryEx)); // ord 166: __imp__MmAllocateContiguousMemoryEx@20
    PatchU32(0x0074FF40, reinterpret_cast<uint32_t>(&Krnl_MmQueryAllocationSize));     // ord 180: __imp__MmQueryAllocationSize@4
    PatchU32(0x0074FF44, reinterpret_cast<uint32_t>(&Krnl_MmSetAddressProtect));       // ord 182: __imp__MmSetAddressProtect@12
    PatchU32(0x0074FF48, reinterpret_cast<uint32_t>(&Krnl_MmQueryAddressProtect));     // ord 179: __imp__MmQueryAddressProtect@4
    PatchU32(0x0074FF4C, reinterpret_cast<uint32_t>(&Krnl_NtCreateEvent));             // ord 189: __imp__NtCreateEvent@16
    PatchU32(0x0074FF50, reinterpret_cast<uint32_t>(g_dummyObjectType));               // ord  16: _ExEventObjectType (DATA)
    PatchU32(0x0074FF54, reinterpret_cast<uint32_t>(&Krnl_NtSetEvent));                // ord 225: __imp__NtSetEvent@8
    PatchU32(0x0074FF58, reinterpret_cast<uint32_t>(&Krnl_NtCreateMutant));            // ord 192: __imp__NtCreateMutant@12
    PatchU32(0x0074FF5C, reinterpret_cast<uint32_t>(&Krnl_NtReleaseMutant));           // ord 221: __imp__NtReleaseMutant@8
    PatchU32(0x0074FF60, reinterpret_cast<uint32_t>(&Krnl_NtWaitForSingleObjectEx));   // ord 234: __imp__NtWaitForSingleObjectEx@16
    PatchU32(0x0074FF64, reinterpret_cast<uint32_t>(&Krnl_NtWaitForMultipleObjectsEx));// ord 235: __imp__NtWaitForMultipleObjectsEx@24
    PatchU32(0x0074FF68, reinterpret_cast<uint32_t>(&Krnl_KeDelayExecutionThread));    // ord  99: __imp__KeDelayExecutionThread@12
    PatchU32(0x0074FF6C, reinterpret_cast<uint32_t>(&Bridge_RtlLeaveCriticalSection)); // ord 294: __imp__RtlLeaveCriticalSection@4
    PatchU32(0x0074FF70, reinterpret_cast<uint32_t>(&Bridge_RtlEnterCriticalSection)); // ord 277: __imp__RtlEnterCriticalSection@4
    PatchU32(0x0074FF74, reinterpret_cast<uint32_t>(&Krnl_NtResumeThread));            // ord 224: __imp__NtResumeThread@8
    PatchU32(0x0074FF78, reinterpret_cast<uint32_t>(&Krnl_RtlRaiseException));         // ord 302: __imp__RtlRaiseException@4
    PatchU32(0x0074FF7C, reinterpret_cast<uint32_t>(&Krnl_PsTerminateSystemThread));   // ord 258: __imp__PsTerminateSystemThread@4
    PatchU32(0x0074FF80, reinterpret_cast<uint32_t>(&Krnl_PsCreateSystemThreadEx));    // ord 255: __imp__PsCreateSystemThreadEx@40
    PatchU32(0x0074FF84, reinterpret_cast<uint32_t>(&g_xeImageFileName));              // ord 326: __imp__XeImageFileName (DATA)
    PatchU32(0x0074FF88, reinterpret_cast<uint32_t>(&Krnl_XcVerifyPKCS1Signature));    // ord 344: __imp__XcVerifyPKCS1Signature@12
    PatchU32(0x0074FF8C, reinterpret_cast<uint32_t>(g_xePublicKeyData));               // ord 355: _XePublicKeyData (DATA)
    PatchU32(0x0074FF90, reinterpret_cast<uint32_t>(&g_xboxHwInfo));                   // ord 322: _XboxHardwareInfo (DATA)
    PatchU32(0x0074FF94, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  47: __imp__HalRegisterShutdownNotification@8
    PatchU32(0x0074FF98, reinterpret_cast<uint32_t>(g_xboxKrnlVersion));               // ord 324: __imp__XboxKrnlVersion (DATA)
    PatchU32(0x0074FF9C, reinterpret_cast<uint32_t>(&Krnl_XcHMAC));                    // ord 340: __imp__XcHMAC@28
    PatchU32(0x0074FFA0, reinterpret_cast<uint32_t>(&Krnl_RtlEqualString));            // ord 279: __imp__RtlEqualString@12
    PatchU32(0x0074FFA4, reinterpret_cast<uint32_t>(&Krnl_XeLoadUnloadSection));       // ord 327: __imp__XeLoadSection@4
    PatchU32(0x0074FFA8, reinterpret_cast<uint32_t>(&Krnl_XeLoadUnloadSection));       // ord 328: __imp__XeUnloadSection@4
    PatchU32(0x0074FFAC, reinterpret_cast<uint32_t>(&Krnl_NtFreeVirtualMemory));       // ord 199: __imp__NtFreeVirtualMemory@12
    PatchU32(0x0074FFB0, reinterpret_cast<uint32_t>(&Krnl_NtAllocateVirtualMemory));   // ord 184: __imp__NtAllocateVirtualMemory@20
    PatchU32(0x0074FFB4, reinterpret_cast<uint32_t>(&Krnl_RtlCompareMemoryUlong));     // ord 269: __imp__RtlCompareMemoryUlong@12
    PatchU32(0x0074FFB8, reinterpret_cast<uint32_t>(&Bridge_RtlInitializeCriticalSection)); // ord 291: __imp__RtlInitializeCriticalSection@4
    PatchU32(0x0074FFBC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 217: __imp__NtQueryVirtualMemory@8
    PatchU32(0x0074FFC0, reinterpret_cast<uint32_t>(&Krnl_KeSetTimer));                // ord 149: __imp__KeSetTimer@16
    PatchU32(0x0074FFC4, reinterpret_cast<uint32_t>(&Krnl_Stub0));                     // ord 360: __imp__HalInitiateShutdown@0
    PatchU32(0x0074FFC8, reinterpret_cast<uint32_t>(&Krnl_KeInitializeTimerEx));       // ord 113: __imp__KeInitializeTimerEx@8
    PatchU32(0x0074FFCC, reinterpret_cast<uint32_t>(&Krnl_KeSetEvent));                // ord 145: __imp__KeSetEvent@12
    PatchU32(0x0074FFD0, reinterpret_cast<uint32_t>(&Krnl_KeWaitForSingleObject));     // ord 159: __imp__KeWaitForSingleObject@20
    PatchU32(0x0074FFD4, reinterpret_cast<uint32_t>(&Krnl_AvGetSavedDataAddress));     // ord   1: __imp__AvGetSavedDataAddress@0
    PatchU32(0x0074FFD8, reinterpret_cast<uint32_t>(&Krnl_AvSendTVEncoderOption));     // ord   2: __imp__AvSendTVEncoderOption@16
    PatchU32(0x0074FFDC, reinterpret_cast<uint32_t>(&Krnl_AvSetDisplayMode));          // ord   3: __imp__AvSetDisplayMode@24
    PatchU32(0x0074FFE0, reinterpret_cast<uint32_t>(&Krnl_AvSetSavedDataAddress));     // ord   4: __imp__AvSetSavedDataAddress@4
    PatchU32(0x0074FFE4, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord  46: __imp__HalReadWritePCISpace@24
    PatchU32(0x0074FFE8, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  98: __imp__KeConnectInterrupt@4
    PatchU32(0x0074FFEC, reinterpret_cast<uint32_t>(&Krnl_Stub7));                     // ord 109: __imp__KeInitializeInterrupt@28
    PatchU32(0x0074FFF0, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  44: __imp__HalGetInterruptVector@8
    PatchU32(0x0074FFF4, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 100: __imp__KeDisconnectInterrupt@4
    PatchU32(0x0074FFF8, reinterpret_cast<uint32_t>(&Krnl_DbgPrint));                  // ord   8: __imp__DbgPrint
    PatchU32(0x0074FFFC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 168: __imp__MmClaimGpuInstanceMemory@8
    PatchU32(0x00750000, reinterpret_cast<uint32_t>(&Krnl_KeStallExecutionProcessor)); // ord 151: __imp__KeStallExecutionProcessor@4
    PatchU32(0x00750004, reinterpret_cast<uint32_t>(&Krnl_KfRaiseIrql));               // ord 160: __imp_@KfRaiseIrql@4
    PatchU32(0x00750008, reinterpret_cast<uint32_t>(&Krnl_KeSaveFloatingPointState));  // ord 142: __imp__KeSaveFloatingPointState@4
    PatchU32(0x0075000C, reinterpret_cast<uint32_t>(&Krnl_KeRestoreFloatingPointState));// ord 139: __imp__KeRestoreFloatingPointState@4
    PatchU32(0x00750010, reinterpret_cast<uint32_t>(&Krnl_ExQueryPoolBlockSize));      // ord  23: __imp__ExQueryPoolBlockSize@4
    PatchU32(0x00750014, reinterpret_cast<uint32_t>(&Krnl_ExAllocatePoolWithTag));     // ord  15: __imp__ExAllocatePoolWithTag@8
    PatchU32(0x00750018, reinterpret_cast<uint32_t>(&Krnl_ExFreePool));                // ord  17: __imp__ExFreePool@4
    PatchU32(0x0075001C, reinterpret_cast<uint32_t>(&Krnl_KeRemoveQueueDpc));          // ord 137: __imp__KeRemoveQueueDpc@4
    PatchU32(0x00750020, reinterpret_cast<uint32_t>(&Krnl_KeCancelTimer));             // ord  97: __imp__KeCancelTimer@4
    PatchU32(0x00750024, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 153: __imp__KeSynchronizeExecution@12
    PatchU32(0x00750028, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 175: __imp__MmLockUnlockBufferPages@12
    PatchU32(0x0075002C, reinterpret_cast<uint32_t>(&Krnl_MmGetPhysicalAddress));      // ord 173: __imp__MmGetPhysicalAddress@4
    PatchU32(0x00750030, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  95: __imp__KeBugCheck@4
    PatchU32(0x00750034, reinterpret_cast<uint32_t>(&Krnl_Stub4));                     // ord 312: __imp__RtlUnwind@16
    PatchU32(0x00750038, reinterpret_cast<uint32_t>(&Krnl_MmQueryStatistics));         // ord 181: __imp__MmQueryStatistics@4
    PatchU32(0x0075003C, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord  65: __imp__IoCreateDevice@24
    PatchU32(0x00750040, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  74: __imp__IoInvalidDeviceRequest@8
    PatchU32(0x00750044, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  91: __imp__IoDismountVolumeByName@4
    PatchU32(0x00750048, reinterpret_cast<uint32_t>(&g_avPackStandard));               // ord 356: _HalBootSMCVideoMode (DATA)
    PatchU32(0x0075004C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord  83: __imp__IoStartPacket@12
    PatchU32(0x00750050, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  81: __imp__IoStartNextPacket@4
    PatchU32(0x00750054, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 359: __imp__IoMarkIrpMustComplete@4
    PatchU32(0x00750058, reinterpret_cast<uint32_t>(&Krnl_Stub0));                     // ord 358: __imp__HalIsResetOrShutdownPending@0
    PatchU32(0x0075005C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 176: __imp__MmLockUnlockPhysicalPage@8
    PatchU32(0x00750060, reinterpret_cast<uint32_t>(&Krnl_RtlAnsiStringToUnicodeString)); // ord 260: __imp__RtlAnsiStringToUnicodeString@12
    PatchU32(0x00750064, reinterpret_cast<uint32_t>(&Krnl_RtlUnicodeStringToAnsiString)); // ord 308: __imp__RtlUnicodeStringToAnsiString@12
    PatchU32(0x00750068, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 252: __imp__PhyGetLinkState@4
    PatchU32(0x0075006C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 253: __imp__PhyInitialize@8
    PatchU32(0x00750070, reinterpret_cast<uint32_t>(&Krnl_KeWaitForMultipleObjects));  // ord 158: __imp__KeWaitForMultipleObjects@32
    PatchU32(0x00750074, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 346: __imp__XcDESKeyParity@8
    PatchU32(0x00750078, reinterpret_cast<uint32_t>(&Krnl_Stub7));                     // ord 349: __imp__XcBlockCryptCBC@28
    PatchU32(0x0075007C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 347: __imp__XcKeyTable@12
    PatchU32(0x00750080, reinterpret_cast<uint32_t>(&Krnl_XcModExp));                  // ord 345: __imp__XcModExp@20
    PatchU32(0x00750084, reinterpret_cast<uint32_t>(g_xboxLANKey));                    // ord 353: __imp__XboxLANKey (DATA)
    PatchU32(0x00750088, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 339: __imp__XcRC4Crypt@12
    PatchU32(0x0075008C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 338: __imp__XcRC4Key@12
    PatchU32(0x00750090, reinterpret_cast<uint32_t>(&Krnl_KeSetTimerEx));              // ord 150: __imp__KeSetTimerEx@20
    PatchU32(0x00750094, reinterpret_cast<uint32_t>(&Krnl_KeQueryPerformanceCounter)); // ord 126: __imp__KeQueryPerformanceCounter@0
    PatchU32(0x00750098, reinterpret_cast<uint32_t>(&Krnl_KeQueryPerformanceFrequency));// ord 127: __imp__KeQueryPerformanceFrequency@0
    PatchU32(0x0075009C, reinterpret_cast<uint32_t>(&g_halDiskSerialNumber));          // ord  42: _HalDiskSerialNumber (DATA)
    PatchU32(0x007500A0, reinterpret_cast<uint32_t>(&g_halDiskModelNumber));           // ord  41: _HalDiskModelNumber (DATA)

    // 3. Patch all 7 `movzx eax, byte ptr fs:[24h]` (64 0F B6 05 24 00 00 00) in DSOUND & XACTENG:
    //    - Sites 0x006DFE13..0x007016FC return 0 (PASSIVE_LEVEL) so multi-core threads always acquire RtlEnterCriticalSection.
    //    - Site  0x00704639 (XACT::CEngine::Enqueue) calls GetXboxSimulatedDpcIrql() so Enqueue returns 2 (DISPATCH_LEVEL)
    //      when invoked from DPCTimerCallBack (0x00703FEA) and 0 otherwise, preventing redundant KeSetTimer churn inside DPCs.
    static const uint8_t kXorEaxNop6[8] = { 0x33, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    PatchBytes(0x006DFE13, kXorEaxNop6, 8);
    PatchBytes(0x006E001E, kXorEaxNop6, 8);
    PatchBytes(0x006E54DB, kXorEaxNop6, 8);
    PatchBytes(0x006FE3E0, kXorEaxNop6, 8);
    PatchBytes(0x006FE90D, kXorEaxNop6, 8);
    PatchBytes(0x007016FC, kXorEaxNop6, 8);
    {
        uint8_t patch704639[8] = { 0xE8, 0u, 0u, 0u, 0u, 0x90u, 0x90u, 0x90u };
        uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&GetXboxSimulatedDpcIrql) - (0x00704639u + 5u));
        memcpy(patch704639 + 1, &rel, sizeof(uint32_t));
        PatchBytes(0x00704639, patch704639, 8);
    }

    // Also patch both `mov eax, dword ptr fs:[58h]` (64 A1 58 00 00 00, _KPCR::NpxThread) in DSOUND::CFpState
    // so Save/Restore@CFpState never read Win32 WOW64 TEB32.ThreadLocalStoragePointer as an Xbox KTHREAD*
    static const uint8_t kFs58Orig[6]   = { 0x64, 0xA1, 0x58, 0x00, 0x00, 0x00 };
    static const uint8_t kXorEaxNop4[6] = { 0x33, 0xC0, 0x90, 0x90, 0x90, 0x90 };
    if (memcmp(reinterpret_cast<const void*>(0x006DFE49), kFs58Orig, 6) == 0) {
        PatchBytes(0x006DFE49, kXorEaxNop4, 6);
    }
    if (memcmp(reinterpret_cast<const void*>(0x006DFE6E), kFs58Orig, 6) == 0) {
        PatchBytes(0x006DFE6E, kXorEaxNop4, 6);
    }

    // 4. Bridge LIBCMT Thread-Local Storage (__getptd, __freeptd, __mtinit) & _XapiThreadStartup
    Bridge_crt_mtinit();
    InstallBridge(0x00695311, &Bridge_crt_getptd);        // __getptd
    InstallBridge(0x0069545C, &Bridge_crt_freeptd);       // __freeptd
    InstallBridge(0x006954AE, &Bridge_crt_mtinit);        // __mtinit
    InstallBridge(0x005A2574, &Bridge_XapiThreadStartup); // _XapiThreadStartup@8

    // 5. XACT DoWork hook & 1 ms high-resolution background kernel clock thread
    InstallBridge(0x0070010F, &Bridge_XACTEngineDoWork);  // _XACTEngineDoWork@0
    StartXboxKernelClockThread();

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kXboxPeImageBase), kXboxReservedSize);
}
