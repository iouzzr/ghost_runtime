#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "profiler.h"
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Hint hybrid GPU laptops (NVIDIA Optimus / AMD PowerXpress) to select the discrete high-performance GPU
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}

HWND          g_hWnd                      = nullptr;
HINSTANCE     g_hInstance                 = nullptr;
char          g_appRootDir[kMaxHostPath]  = { 0 };
char          g_gameRootDir[kMaxHostPath] = { 0 };
volatile bool  g_appRunning                = true;
volatile DWORD g_mainGameThreadId          = 0;

int           g_selectedAdapterIndex = 0;
wchar_t       g_selectedAdapterName[128] = { 0 };

static CRITICAL_SECTION g_vehPatchCs;
static volatile LONG    g_vehPatchCsInit = 0;

static void EnsureVehPatchLockInit() {
    if (g_vehPatchCsInit == 2) return;
    if (InterlockedCompareExchange(&g_vehPatchCsInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_vehPatchCs);
        InterlockedExchange(&g_vehPatchCsInit, 2);
    } else {
        while (g_vehPatchCsInit != 2) {
            SwitchToThread();
        }
    }
}

// ============================================================================
// Vectored Exception Handler (Ring-0 / SSE Emulation) & Last-Chance Crash Filter
// ============================================================================

static bool IsLegacyX86PrefixByte(uint8_t b) {
    switch (b) {
        case 0x26: case 0x2E: case 0x36: case 0x3E:
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0xF0: case 0xF2: case 0xF3:
            return true;
        default:
            return false;
    }
}

// First-chance Vectored Exception Handler:
// Patches ONLY verified Xbox ring-0 instructions (invd/wbinvd/cli/sti) and unaligned 128-bit SSE moves
// (movaps/movapd/movntps/movntpd -> movups/movupd) inside the Xbox image space.
// ALWAYS returns EXCEPTION_CONTINUE_SEARCH for all other exceptions so host DLLs (COM, DXGI, D3D11, audio, RPC)
// and structured exception handlers work normally.
static LONG WINAPI XboxVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD    code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* ctx  = ep->ContextRecord;
    if (ctx->Eip < kXboxPeImageBase || ctx->Eip >= kXboxReservedEndVa - 16u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_DATATYPE_MISALIGNMENT)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Decode up to 4 legacy x86 prefixes
    uint8_t* ins = reinterpret_cast<uint8_t*>(ctx->Eip);
    size_t   prefixLen = 0;
    bool     hasF2F3   = false;
    while (prefixLen < 4u && IsLegacyX86PrefixByte(ins[prefixLen])) {
        if (ins[prefixLen] == 0xF2 || ins[prefixLen] == 0xF3) {
            hasF2F3 = true;
        }
        ++prefixLen;
    }
    uint8_t* op = ins + prefixLen;

    // 1. Handle ring-0 Xbox privileged instructions (wbinvd = 0F 09, invd = 0F 08, cli = FA, sti = FB)
    if (code == EXCEPTION_PRIV_INSTRUCTION) {
        EnsureVehPatchLockInit();
        EnterCriticalSection(&g_vehPatchCs);
        bool handled = false;
        if (op[0] == 0x0F && (op[1] == 0x09 || op[1] == 0x08)) {
            memset(ins, 0x90, prefixLen + 2u);
            FlushInstructionCache(GetCurrentProcess(), ins, prefixLen + 2u);
            ctx->Eip += static_cast<DWORD>(prefixLen + 2u);
            handled = true;
        } else if (op[0] == 0xFA || op[0] == 0xFB) {
            memset(ins, 0x90, prefixLen + 1u);
            FlushInstructionCache(GetCurrentProcess(), ins, prefixLen + 1u);
            ctx->Eip += static_cast<DWORD>(prefixLen + 1u);
            handled = true;
        } else if (ins[0] == 0x90) {
            // Concurrent thread just patched this site to NOPs while we waited on g_vehPatchCs
            FlushInstructionCache(GetCurrentProcess(), ins, 2u);
            handled = true;
        }
        LeaveCriticalSection(&g_vehPatchCs);
        return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }

    // 2. Handle misaligned 16-byte SSE movaps/movapd (0F 28 / 0F 29) & movntps/movntpd (0F 2B)
    //    by hot-patching in-place to unaligned movups/movupd (0F 10 / 0F 11)
    bool isAlignmentFault = (code == EXCEPTION_DATATYPE_MISALIGNMENT) ||
        (code == EXCEPTION_ACCESS_VIOLATION &&
         ep->ExceptionRecord->NumberParameters >= 2 &&
         ep->ExceptionRecord->ExceptionInformation[1] == 0xFFFFFFFFu);

    if (isAlignmentFault && !hasF2F3 && op[0] == 0x0F) {
        EnsureVehPatchLockInit();
        EnterCriticalSection(&g_vehPatchCs);
        bool handled = false;
        if (op[1] == 0x28) {
            op[1] = 0x10; // movups / movupd xmm, m128
            FlushInstructionCache(GetCurrentProcess(), op, 2u);
            handled = true;
        } else if (op[1] == 0x29 || op[1] == 0x2B) {
            op[1] = 0x11; // movups / movupd m128, xmm (also replaces non-temporal movntps/movntpd)
            FlushInstructionCache(GetCurrentProcess(), op, 2u);
            handled = true;
        } else if (op[1] == 0x10 || op[1] == 0x11) {
            // Concurrent thread just patched this instruction while we waited on g_vehPatchCs
            FlushInstructionCache(GetCurrentProcess(), op, 2u);
            handled = true;
        }
        LeaveCriticalSection(&g_vehPatchCs);
        if (handled) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// Last-chance unhandled exception filter (only invoked if an exception goes completely unhandled)
static LONG WINAPI XboxUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    StopAllControllerVibration();
    if (ep && ep->ExceptionRecord && ep->ContextRecord && g_appRootDir[0]) {
        char logPath[kMaxHostPath];
        snprintf(logPath, sizeof(logPath), "%s\\crash.log", g_appRootDir);
        FILE* f = OpenFileUtf8(logPath, L"ab");
        if (f) {
            const EXCEPTION_RECORD* er = ep->ExceptionRecord;
            const CONTEXT*          c  = ep->ContextRecord;
            fprintf(f,
                "[Crash] Code=0x%08lX Addr=0x%p EIP=0x%08lX ESP=0x%08lX EBP=0x%08lX "
                "EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX ESI=0x%08lX EDI=0x%08lX\r\n",
                er->ExceptionCode, er->ExceptionAddress,
                c->Eip, c->Esp, c->Ebp, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
            fclose(f);
        }
    }
    ExitProcess(1);
}

// ============================================================================
// Win32 Game Window & Message Procedure
// ============================================================================

static LRESULT CALLBACK GhostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SYSCOMMAND:
            if (HandleOsdSysCommand(hWnd, wParam)) {
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            const RECT* prcNew = reinterpret_cast<const RECT*>(lParam);
            if (prcNew) {
                SetWindowPos(
                    hWnd, nullptr,
                    prcNew->left, prcNew->top,
                    prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_ACTIVATEAPP:
            if (!wParam) {
                StopAllControllerVibration();
                NotifyProfilerWindowFocusLost();
            }
            break;
        case WM_DEVICECHANGE:
            NotifyHostDeviceChange();
            break;
        case WM_CLOSE:
            g_appRunning = false;
            StopAllControllerVibration();
            SaveOsdStateToIni();
            ShutdownXboxAudio();
            ShutdownXboxKernel();
            ExitProcess(0);
        case WM_DESTROY:
            g_appRunning = false;
            StopAllControllerVibration();
            SaveOsdStateToIni();
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (HandleOsdTimer(hWnd, wParam)) {
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (msg == WM_SYSKEYDOWN && (wParam == VK_F4 || wParam == VK_SPACE)) {
                break; // Preserve Alt+F4 and Alt+Space system menu shortcuts
            }
            if (HandleProfilerWindowKeyMessage(hWnd, msg, wParam, lParam)) {
                return 0;
            }
            int vk = ResolveDistinguishedVk(wParam, lParam);
            PushDebugKeystroke(static_cast<uint16_t>(vk), 0, false);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (HandleProfilerWindowKeyMessage(hWnd, msg, wParam, lParam)) {
                return 0;
            }
            int vk = ResolveDistinguishedVk(wParam, lParam);
            PushDebugKeystroke(static_cast<uint16_t>(vk), 0, true);
            return 0;
        }
        case WM_CHAR:
            PushDebugKeystroke(0, static_cast<wchar_t>(wParam), false);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool InitWin32Window(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = GhostWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"StarCraftGhostPCWndClass";
    RegisterClassExW(&wc);

    // Query system/primary monitor DPI so the 4:3 window scales crisply under Per-Monitor DPI V2
    UINT dpi = 96u;
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)();
        PFN_GetDpiForSystem pfnGetDpi = reinterpret_cast<PFN_GetDpiForSystem>(GetProcAddress(hUser32, "GetDpiForSystem"));
        if (pfnGetDpi) {
            UINT sysDpi = pfnGetDpi();
            if (sysDpi >= 96u && sysDpi <= 480u) {
                dpi = sysDpi;
            }
        }
    }

    int clientW = MulDiv(640, static_cast<int>(dpi), 96);
    int clientH = MulDiv(480, static_cast<int>(dpi), 96);
    RECT rc     = { 0, 0, clientW, clientH };
    DWORD style   = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE;
    DWORD exStyle = 0;

    bool adjusted = false;
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
        PFN_AdjustWindowRectExForDpi pfnAdjust = reinterpret_cast<PFN_AdjustWindowRectExForDpi>(
            GetProcAddress(hUser32, "AdjustWindowRectExForDpi"));
        if (pfnAdjust && pfnAdjust(&rc, style, FALSE, exStyle, dpi)) {
            adjusted = true;
        }
    }
    if (!adjusted) {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int winY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (winX < 0) winX = 0;
    if (winY < 0) winY = 0;

    g_hWnd = CreateWindowExW(
        exStyle, wc.lpszClassName,
        L"StarCraft: Ghost",
        style, winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hWnd) return false;

    InitOsdWindowHooks(g_hWnd);
    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hWnd);
    return true;
}

// ============================================================================
// Ghost.exe PE Section Loader (into reserved 0x00401000..0x00951000)
// ============================================================================

static void RelocateXbeHeaderPointerField(uint32_t fieldOffset) {
    uint32_t origVa = *reinterpret_cast<const uint32_t*>(kXbeHeaderBaseVa + fieldOffset);
    if (origVa >= kOrigXbeBaseVa && origVa < kOrigXbeBaseVa + kXbeHeaderMaxSize) {
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + fieldOffset) = kXbeHeaderBaseVa + (origVa - kOrigXbeBaseVa);
    }
}

bool LoadGhostXboxPeImage(const char* ghostExePath) {
    static bool s_ghostPeLoaded = false;
    if (s_ghostPeLoaded) {
        return true;
    }
    FILE* f = OpenFileUtf8(ghostExePath, L"rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long rawFileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (rawFileSize < static_cast<long>(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS32))) {
        fclose(f);
        return false;
    }
    size_t fileSize = static_cast<size_t>(rawFileSize);

    std::vector<uint8_t> pe(fileSize);
    if (fread(pe.data(), 1, fileSize, f) != fileSize) {
        fclose(f);
        return false;
    }
    fclose(f);

    // Explicitly detect and reject raw Xbox .xbe images (which are linked at 0x00010000 without relocations)
    if (*reinterpret_cast<const uint32_t*>(pe.data()) == 0x48454258u) {
        MessageBoxW(
            nullptr,
            L"The selected 'Ghost.exe' is actually a raw Xbox XBE image ('XBEH' at base 0x00010000), which lacks relocation tables.\n\n"
            L"Please use the original 0x00400000-based PE 'Ghost.exe' from the StarCraft: Ghost build.",
            L"Unsupported Raw XBE Image",
            MB_ICONERROR | MB_OK);
        return false;
    }

    const IMAGE_DOS_HEADER* dosHdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data());
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE ||
        dosHdr->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
        static_cast<size_t>(dosHdr->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > fileSize)
    {
        return false;
    }

    const IMAGE_NT_HEADERS32* ntHdr = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pe.data() + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE ||
        ntHdr->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        ntHdr->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        ntHdr->OptionalHeader.ImageBase != kXboxPeImageBase)
    {
        return false;
    }

    uint16_t numSecs = ntHdr->FileHeader.NumberOfSections;
    const IMAGE_SECTION_HEADER* secHdr = IMAGE_FIRST_SECTION(ntHdr);
    const uint8_t* secEnd = reinterpret_cast<const uint8_t*>(secHdr + numSecs);
    if (numSecs == 0 || numSecs > 64u || secEnd > pe.data() + fileSize) {
        return false;
    }

    // Make the reserved Xbox region (0x00400000..0x00950000) RWX
    // Note: Ghost.exe's .text starts at RVA 0x00000740 (0x00400740), above GhostXR.exe's PE headers
    constexpr uint8_t  kFsPrefixOpcode       = 0x64u;
    constexpr uint8_t  kMovMoffs32EaxOpcode  = 0xA3u;
    constexpr uint8_t  kMovRm32RegOpcode     = 0x89u;
    constexpr uint8_t  kPopRm32Opcode        = 0x8Fu;
    constexpr uint8_t  kModRmDisp32Pop       = 0x05u;
    constexpr uint8_t  kNopOpcode            = 0x90u;
    constexpr size_t   kMovFs0EaxByteLen     = 6u;
    constexpr size_t   kMovFs0RegByteLen     = 7u;

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kXboxPeImageBase), kXboxReservedSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }

    // Zero the Xbox image space (from 0x00400740 to 0x00923000) without touching GhostXR.exe's PE header [0x00400000..0x0040073F]
    memset(reinterpret_cast<void*>(kXboxCodeScanStartVa), 0, kXboxZeroImageSize);

    const uint32_t minSafeRva = kXboxCodeScanStartVa - kXboxPeImageBase; // 0x00000740
    const uint32_t maxSafeRva = kXbeHeaderBaseVa     - kXboxPeImageBase; // 0x0054E000

    for (uint16_t i = 0; i < numSecs; ++i) {
        const IMAGE_SECTION_HEADER& s = secHdr[i];
        char nameBuf[9] = { 0 };
        memcpy(nameBuf, s.Name, 8);

        RegisterPeSection(reinterpret_cast<const char*>(s.Name), s.VirtualAddress, s.Misc.VirtualSize);

        // Skip .reloc at 0x00922280 (no relocation needed since we load at exact 0x00400000!)
        if (strcmp(nameBuf, ".reloc") == 0) continue;

        // Guard against any malformed section overlapping host PE headers or exceeding reserved .xbox space
        if (s.VirtualAddress < minSafeRva || s.VirtualAddress >= maxSafeRva) {
            return false;
        }

        uint32_t dstVA = kXboxPeImageBase + s.VirtualAddress;
        uint32_t copyBytes = s.SizeOfRawData;
        if (s.VirtualAddress + copyBytes > maxSafeRva) {
            copyBytes = maxSafeRva - s.VirtualAddress;
        }
        if (s.PointerToRawData > 0 && copyBytes > 0 && (s.PointerToRawData + copyBytes) <= fileSize) {
            memcpy(reinterpret_cast<void*>(dstVA), pe.data() + s.PointerToRawData, copyBytes);
        }
    }

    // Load full Xbox XBE Header + Certificate + Section Headers (up to 0x2000 bytes) into reserved .xbox space at kXbeHeaderBaseVa (0x0094E000)
    // (0x00010000 is occupied by the Windows process environment on Win11 WOW64)
    void* xbePage = reinterpret_cast<void*>(kXbeHeaderBaseVa);
    memset(xbePage, 0, kXbeHeaderMaxSize);
    bool loadedXbeHeader = false;
    char ghostXbePath[kMaxHostPath];
    snprintf(ghostXbePath, sizeof(ghostXbePath), "%s\\Ghost.xbe", g_gameRootDir);
    FILE* fxbe = OpenFileUtf8(ghostXbePath, L"rb");
    if (fxbe) {
        size_t nRead = fread(xbePage, 1, kXbeHeaderMaxSize, fxbe);
        if (nRead >= 0x1000u && *reinterpret_cast<const uint32_t*>(kXbeHeaderBaseVa) == 0x48454258u) {
            loadedXbeHeader = true;
            *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x104u) = kXbeHeaderBaseVa; // dwBaseAddr
            RelocateXbeHeaderPointerField(0x118u); // dwCertificateAddr
            RelocateXbeHeaderPointerField(0x120u); // dwSectionHeadersAddr
            RelocateXbeHeaderPointerField(0x148u); // dwLibraryVersionsAddr
            RelocateXbeHeaderPointerField(0x14Cu); // dwKernelLibraryVersionAddr
            RelocateXbeHeaderPointerField(0x150u); // dwXAPILibraryVersionAddr
        }
        fclose(fxbe);
    }
    if (!loadedXbeHeader) {
        memset(xbePage, 0, kXbeHeaderMaxSize);
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x000) = 0x48454258u; // 'XBEH'
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x104) = kXbeHeaderBaseVa; // dwBaseAddr
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x118) = kXbeHeaderBaseVa + 0x184u; // dwCertificateAddr
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0x00) = 0x000001ECu; // cbSize
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0x08) = 0x00000002u; // dwTitleId (Ghost.xbe)
        wcsncpy(reinterpret_cast<wchar_t*>(kXbeHeaderBaseVa + 0x184 + 0x0C), L"Starcraft: Ghost (Release)", 40);
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0xAC) = 0x00000100u; // dwVersion
        memcpy(reinterpret_cast<void*>(kXbeHeaderBaseVa + 0x184 + 0xB0), "TESTTESTTESTTEST", 16); // bzLANKey
        memcpy(reinterpret_cast<void*>(kXbeHeaderBaseVa + 0x184 + 0xC0), "TESTTESTTESTTEST", 16); // bzSignatureKey
    }

    // Relocate all 27 XeImageHeader() references (ds:[0x00010108..0x00010138]) in XAPILIB/XNET/XPP to kXbeHeaderBaseVa
    static const uint32_t kXbeHeaderRefVas[] = {
        0x0059FD45u, 0x005A0851u, 0x005A09EBu, 0x005A1C51u, 0x005A1ED1u,
        0x005A2617u, 0x005A26FDu, 0x005A2702u, 0x005A2BE1u, 0x005A2D65u,
        0x005A2E40u, 0x005A3B2Au, 0x005A3B91u, 0x005A3C46u, 0x005A3C60u,
        0x005A3E52u, 0x005A3E58u, 0x005A3EDBu, 0x005A3F06u, 0x005A3F29u,
        0x005A46B8u, 0x005A46BEu, 0x0073BA42u, 0x0073D700u, 0x007464DAu,
        0x007492E3u, 0x0074930Bu
    };
    for (uint32_t refVa : kXbeHeaderRefVas) {
        uint32_t origTarget = *reinterpret_cast<const uint32_t*>(refVa);
        if (origTarget >= kOrigXbeBaseVa && origTarget < kOrigXbeBaseVa + kXbeHeaderMaxSize) {
            *reinterpret_cast<uint32_t*>(refVa) = kXbeHeaderBaseVa + (origTarget - kOrigXbeBaseVa);
        }
    }

    // Neutralize all writes to fs:[0] (NT_TIB.ExceptionList) across all code sections (0x00400740..0x0074FE80,
    // including LIBCMT:trnsctrl.obj mov fs:[0], reg, LIBCMT:exsup.obj __local_unwind2 pop dword ptr fs:[0] at 0x0069349C,
    // and BINK yuv.obj ?checkrdtsc@@YAXXZ) so Windows 11 SEHOP/SafeSEH never aborts on Xbox SEH frames.
    uint8_t* codeStart = reinterpret_cast<uint8_t*>(kXboxCodeScanStartVa);
    uint8_t* codeEnd   = reinterpret_cast<uint8_t*>(kXboxCodeScanEndVa - kMovFs0RegByteLen);
    for (uint8_t* p = codeStart; p < codeEnd; ++p) {
        if (p[0] == kFsPrefixOpcode && p[1] == kMovMoffs32EaxOpcode &&
            p[2] == 0x00 && p[3] == 0x00 && p[4] == 0x00 && p[5] == 0x00)
        {
            memset(p, kNopOpcode, kMovFs0EaxByteLen); // mov fs:[0], eax -> nop x6
        } else if (p[0] == kFsPrefixOpcode && p[1] == kMovRm32RegOpcode &&
                   (p[2] & 0xC7u) == 0x05u &&
                   p[3] == 0x00 && p[4] == 0x00 && p[5] == 0x00 && p[6] == 0x00)
        {
            memset(p, kNopOpcode, kMovFs0RegByteLen); // mov fs:[0], reg32 -> nop x7
        } else if (p[0] == kFsPrefixOpcode && p[1] == kPopRm32Opcode && p[2] == kModRmDisp32Pop &&
                   p[3] == 0x00 && p[4] == 0x00 && p[5] == 0x00 && p[6] == 0x00)
        {
            // pop dword ptr fs:[0] (7 bytes) -> lea esp, [esp + 4]; nop; nop; nop (adjusts ESP cleanly without touching EFLAGS or fs:[0])
            static const uint8_t kLeaEspPlus4Nop3[7] = { 0x8D, 0x64, 0x24, 0x04, 0x90, 0x90, 0x90 };
            memcpy(p, kLeaEspPlus4Nop3, 7);
        }
    }

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kXboxPeImageBase), kXboxReservedSize);
    s_ghostPeLoaded = true;
    return true;
}

// ============================================================================
// Dedicated Xbox Game Thread & Win32 UI Entry Point (WinMain)
// ============================================================================

// Per-frame tick wrapper around cSequence::MainLoop (0x0043C250) called from _main at 0x004354B7
static void __fastcall Bridge_MainLoopStep(void* thisPtr, void* edxUnused, uint32_t flags) {
    (void)edxUnused;
    if (!g_appRunning) {
        StopAllControllerVibration();
        CleanupCurrentXboxThreadState();
        ExitThread(0);
    }
    if (g_mainGameThreadId == 0) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_mainGameThreadId), static_cast<LONG>(GetCurrentThreadId()));
    }
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
    typedef void (__thiscall *PFN_MainLoop)(void* thisPtr, uint32_t flags);
    reinterpret_cast<PFN_MainLoop>(0x0043C250)(thisPtr, flags);
}

static DWORD WINAPI XboxGameThreadProc(LPVOID) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_mainGameThreadId), static_cast<LONG>(GetCurrentThreadId()));
    InitD3D11HostDevice();

    // Install all compatibility runtime bridges (and initialize Xbox TLS on this thread)
    InitXboxKernelAndXapi();
    InitXboxInput();
    InitXboxD3D8();
    InitXboxAudio();

    // Hook the per-frame call to cSequence::MainLoop (0x0043C250) inside native _main at 0x004354B7
    {
        uint8_t* callSite = reinterpret_cast<uint8_t*>(0x004354B7);
        uint32_t rel = reinterpret_cast<uintptr_t>(&Bridge_MainLoopStep) - (0x004354B7u + 5u);
        callSite[0] = 0xE8; // CALL rel32
        *reinterpret_cast<uint32_t*>(callSite + 1) = rel;
        FlushInstructionCache(GetCurrentProcess(), callSite, 5u);
    }

    // Hook cProfiler::EndFrame call at the end of tSequence::MainLoop (0x0043C830 -> 0x0043C870)
    // to snapshot all 46 native Ghost.exe profiler timers/counters right before EndFrame resets them.
    {
        uint8_t* profCallSite = reinterpret_cast<uint8_t*>(ADDR_MAINLOOP_ENDFRAME_CALL);
        if (profCallSite[0] == 0xE8u) {
            uint32_t rel = reinterpret_cast<uintptr_t>(&Bridge_ProfilerEndFrame) - (ADDR_MAINLOOP_ENDFRAME_CALL + 5u);
            *reinterpret_cast<uint32_t*>(profCallSite + 1) = rel;
            FlushInstructionCache(GetCurrentProcess(), profCallSite, 5u);
        }
    }

    // Run native Xbox CRT initializers (__rtinit, _cinit) and native _main (0x00435460)
    __try {
        typedef int (__stdcall *PFN_rtinit)();
        typedef void (__cdecl *PFN_cinit)();
        typedef int (__cdecl *PFN_main)();

        reinterpret_cast<PFN_rtinit>(0x005A430F)(); // __rtinit (.CRT$XRI..XRZ)
        reinterpret_cast<PFN_cinit>(0x005A42B7)();  // _cinit (__FPinit, .CRT$XIA..XIZ, .CRT$XCA..XCZ including mu.obj)
        reinterpret_cast<PFN_main>(0x00435460)();   // _main (0x00434780, XGetLaunchInfo, StartApplication, MainLoop, Shutdown)
    } __except (XboxUnhandledExceptionFilter(GetExceptionInformation())) {
    }

    g_appRunning = false;
    StopAllControllerVibration();
    CleanupCurrentXboxThreadState();
    if (g_hWnd) {
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    }
    return 0;
}

static void EnablePerMonitorDpiV2() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
    PFN_SetProcessDpiAwarenessContext pfnSetCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
        GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
    if (pfnSetCtx) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (-4) with fallback to SYSTEM_AWARE (-2)
        if (pfnSetCtx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) {
            return;
        }
        if (pfnSetCtx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)))) {
            return;
        }
    }
    typedef BOOL (WINAPI *PFN_SetProcessDPIAware)();
    PFN_SetProcessDPIAware pfnLegacy = reinterpret_cast<PFN_SetProcessDPIAware>(
        GetProcAddress(hUser32, "SetProcessDPIAware"));
    if (pfnLegacy) {
        pfnLegacy();
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    EnablePerMonitorDpiV2();
    EnsureVehPatchLockInit();
    g_hInstance = hInstance;

    // Show Native Win32 Pre-Launch Setup UI (and integrated Page 2 Shader Compilation if needed)
    if (!ShowLauncherDialog(hInstance)) {
        return 0;
    }

    SetCurrentDirectoryUtf8(g_gameRootDir);

    char ghostExePath[kMaxHostPath];
    snprintf(ghostExePath, sizeof(ghostExePath), "%s\\Ghost.exe", g_gameRootDir);
    if (!LoadGhostXboxPeImage(ghostExePath)) {
        MessageBoxW(nullptr, L"Failed to load Ghost.exe from the selected Game Directory!", L"Error", MB_ICONERROR);
        return 1;
    }

    // Install Xbox exception handlers only after the Win32 shell/COM launcher UI has closed
    AddVectoredExceptionHandler(1, XboxVectoredHandler);
    SetUnhandledExceptionFilter(XboxUnhandledExceptionFilter);

    if (!InitWin32Window(hInstance)) {
        return 1;
    }

    // Spawn dedicated Xbox Game Thread (2 MB stack)
    DWORD gameTid = 0;
    HANDLE hGameThread = CreateThread(nullptr, 2 * 1024 * 1024, XboxGameThreadProc, nullptr, 0, &gameTid);
    (void)hGameThread;

    // Dedicated Win32 UI Message Loop (never blocked by game rendering or file I/O)
    MSG msg;
    while (g_appRunning) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_appRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_appRunning) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
    }

    g_appRunning = false;
    StopAllControllerVibration();
    ShutdownXboxAudio();
    ShutdownXboxKernel();
    ExitProcess(0);
}
