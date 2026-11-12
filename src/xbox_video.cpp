#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <math.h>
#include <vector>

// ============================================================================
// cDivxVideo (.vid Movie Player in VIDPlayerXB.obj) 733.333 MHz RDTSC Virtualization
// ============================================================================
// Original cDivxVideo::FrameMove (0x0043DE80) and cDivxVideo::Render (0x0043DEE0)
// compute elapsed milliseconds via `(rdtsc * 3) / 0x2191C0` (2,200,000), assuming the
// Original Xbox 733.333 MHz Pentium III CPU. We virtualize the `rdtsc * 3` step to
// return exact 733.333 MHz Xbox CPU cycles * 3 in edx:eax while letting Ghost.exe's
// native cDivxVideo::FrameMove and cDivxVideo::Render execute unmodified.

static uint64_t __cdecl Bridge_DivxRdtscTimes3_FrameMove() {
    return QueryXbox733MHzCyclesTimes3Shared();
}

static uint64_t __cdecl Bridge_DivxRdtscTimes3_Render() {
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
    SwitchToThread();
    return QueryXbox733MHzCyclesTimes3Shared();
}

static void PatchCallAndNops(uint32_t addr, const void* targetFn, size_t totalBytes) {
    DWORD oldProt = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, PAGE_EXECUTE_READWRITE, &oldProt);
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
    p[0] = 0xE8; // CALL rel32
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(targetFn) - (static_cast<uintptr_t>(addr) + 5u));
    for (size_t i = 5; i < totalBytes; ++i) {
        p[i] = 0x90; // NOP
    }
    VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, oldProt, &oldProt);
}

// ============================================================================
// YUY2 Hardware Overlay Emulation for .vid Movies
// Faithfully reproduces _D3DDevice_UpdateOverlay@20 (0x006B30D0),
// _D3DDevice_EnableOverlay@4 (0x006B3250), and _D3DDevice_GetOverlayUpdateStatus@0 (0x006B30B0).
// ============================================================================

static ID3D11Texture2D*          g_videoOverlayTex    = nullptr;
static ID3D11ShaderResourceView* g_videoOverlaySRV    = nullptr;
static uint32_t                  g_videoOverlayTexW   = 0u;
static uint32_t                  g_videoOverlayTexH   = 0u;
static bool                      g_overlayEnabled     = false;
static bool                      g_overlayHasData     = false;
static RECT                      g_overlayDstRect     = { 0, 0, 640, 480 };
static bool                      g_overlayUseColorKey = false;
static uint32_t                  g_overlayColorKey    = 0u;

static inline uint32_t YuvBt601StudioToBgra(int yVal, int uVal, int vVal) {
    // NV_PVIDEO_FORMAT_MATRIX_ITURBT601 (0x00010000 at 0x006B31FD):
    // Studio-range ITU-R BT.601 Y in [16..235], U/V in [16..240]
    int c = yVal - 16;
    int d = uVal - 128;
    int e = vVal - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void __stdcall Bridge_D3DDevice_EnableOverlay(BOOL bEnable) {
    // In Ghost.exe (0x006B3250), EnableOverlay stops any active NV_PVIDEO_BUFFER ([eax+8704h] = 1)
    // and resets the NV_PVIDEO overlay registers; the overlay scans out persistently once UpdateOverlay
    // sets NV_PVIDEO_BUFFER = 1 ([esi+8700h] = 1) until EnableOverlay stops it again.
    g_overlayEnabled = (bEnable != FALSE);
    g_overlayHasData = false;
    if (!g_overlayEnabled) {
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C1C) = 0u;
    }
}

static BOOL __stdcall Bridge_D3DDevice_GetOverlayUpdateStatus() {
    // In Ghost.exe (0x006B30B0): returns (pDevice->m_OverlaySwapCount != pDevice->m_SwapCount).
    // Since FreeVideoAndResetGamma (0x0043B4B3) spins on this without calling Swap inside the loop
    // (relying on Xbox hardware VBlank interrupt to advance m_SwapCount), advance m_SwapCount here
    // so the Device state fields match and the spin loop completes cleanly.
    uint32_t* pOverlaySwap = reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x2410);
    uint32_t* pSwapCount   = reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1DE8);
    if (*pOverlaySwap == *pSwapCount) {
        (*pSwapCount)++;
    }
    return (*pOverlaySwap != *pSwapCount) ? TRUE : FALSE;
}

bool GetActiveXboxVideoOverlay(XboxVideoOverlayState* outState) {
    if (!outState) return false;
    outState->active      = false;
    outState->dstRect     = { 0, 0, 0, 0 };
    outState->useColorKey = false;
    outState->colorKey    = 0u;
    outState->srv         = nullptr;

    if (!g_overlayEnabled || !g_overlayHasData || !g_videoOverlaySRV) {
        return false;
    }
    if (g_overlayDstRect.right <= g_overlayDstRect.left ||
        g_overlayDstRect.bottom <= g_overlayDstRect.top)
    {
        return false;
    }

    outState->active      = true;
    outState->dstRect     = g_overlayDstRect;
    outState->useColorKey = g_overlayUseColorKey;
    outState->colorKey    = g_overlayColorKey;
    outState->srv         = g_videoOverlaySRV;
    return true;
}

void FlushXboxOverlayToBackbuffer() {
    // NV_PVIDEO hardware overlay is now composited non-destructively onto g_swapChainRTV
    // during Bridge_D3DDevice_Swap without mutating g_backBufferTex or clearing g_overlayHasData.
}

static void __stdcall Bridge_D3DDevice_UpdateOverlay(
    XboxPixelContainer* pSurface, const RECT* pSrcRect, const RECT* pDstRect,
    BOOL bColorKey, DWORD ColorKey)
{
    if (!pSurface || !pSurface->Data || static_cast<uintptr_t>(pSurface->Data) < kMinValidHostAddress) return;

    // 1. Update D3DDevice overlay tracking fields (0x006B30D3..0x006B30E9)
    uint32_t swapCount = *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1DE8);
    *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1C1C) = pSurface;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x2410)            = swapCount;

    uint32_t surfW = 0, surfH = 0, levels = 1, xboxFmt = 0, pitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pSurface, &surfW, &surfH, &levels, &xboxFmt, &isSwizzled, &pitch);
    if (surfW == 0 || surfH == 0 || surfW > 2048 || surfH > 2048) return;

    bool is32Bit = (xboxFmt == 0x06u || xboxFmt == 0x07u || xboxFmt == 0x12u || xboxFmt == 0x1Eu ||
                    xboxFmt == 0x3Au || xboxFmt == 0x3Bu || xboxFmt == 0x3Cu ||
                    xboxFmt == 0x3Fu || xboxFmt == 0x40u || xboxFmt == 0x41u);
    uint32_t bpp = is32Bit ? 4u : 2u;
    if (pitch == 0) pitch = surfW * bpp;

    // 2. Extract pSrcRect & pDstRect (0x006B30F8..0x006B3165)
    int32_t srcLeft = pSrcRect ? (pSrcRect->left & ~1) : 0;
    int32_t srcTop  = pSrcRect ? (pSrcRect->top  & ~1) : 0;
    int32_t srcW    = (pSrcRect && pSrcRect->right  > pSrcRect->left) ? (pSrcRect->right  - pSrcRect->left) : static_cast<int32_t>(surfW);
    int32_t srcH    = (pSrcRect && pSrcRect->bottom > pSrcRect->top)  ? (pSrcRect->bottom - pSrcRect->top)  : static_cast<int32_t>(surfH);

    int32_t dstLeft = pDstRect ? pDstRect->left : 0;
    int32_t dstTop  = pDstRect ? pDstRect->top  : 0;
    int32_t dstW    = (pDstRect && pDstRect->right  > pDstRect->left) ? (pDstRect->right  - pDstRect->left) : srcW;
    int32_t dstH    = (pDstRect && pDstRect->bottom > pDstRect->top)  ? (pDstRect->bottom - pDstRect->top)  : srcH;

    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcLeft < 0) srcLeft = 0;
    if (srcTop  < 0) srcTop  = 0;
    if (srcLeft + srcW > static_cast<int32_t>(surfW)) srcW = static_cast<int32_t>(surfW) - srcLeft;
    if (srcTop  + srcH > static_cast<int32_t>(surfH)) srcH = static_cast<int32_t>(surfH) - srcTop;
    if (srcW <= 0 || srcH <= 0) return;

    g_overlayDstRect.left   = dstLeft;
    g_overlayDstRect.top    = dstTop;
    g_overlayDstRect.right  = dstLeft + dstW;
    g_overlayDstRect.bottom = dstTop  + dstH;
    g_overlayUseColorKey    = (bColorKey != FALSE);
    g_overlayColorKey       = ColorKey;

    // 3. Unswizzle if swizzled RGB, then decode source YUY2 (0x24) / UYVY (0x25) / RGB region to BGRA8
    std::vector<uint8_t> unswizzled;
    const uint8_t* linearSurf = reinterpret_cast<const uint8_t*>(pSurface->Data);
    if (isSwizzled && xboxFmt != 0x24u && xboxFmt != 0x25u) {
        typedef void (__stdcall *PFN_XGUnswizzleRect)(
            const void*, uint32_t, uint32_t, const RECT*, void*, uint32_t, const POINT*, uint32_t);
        PFN_XGUnswizzleRect pfnUnswizzle = reinterpret_cast<PFN_XGUnswizzleRect>(ADDR_XG_UNSWIZZLE_RECT);
        unswizzled.resize(static_cast<size_t>(surfW) * static_cast<size_t>(surfH) * bpp);
        pfnUnswizzle(linearSurf, surfW, surfH, nullptr, unswizzled.data(), surfW * bpp, nullptr, bpp);
        linearSurf = unswizzled.data();
        pitch      = surfW * bpp;
    }

    std::vector<uint32_t> decodedSrc(static_cast<size_t>(srcW) * static_cast<size_t>(srcH));
    const uint8_t* srcBase = linearSurf + static_cast<size_t>(srcTop) * pitch + static_cast<size_t>(srcLeft) * bpp;

    for (int32_t y = 0; y < srcH; ++y) {
        const uint8_t* sRow = srcBase + static_cast<size_t>(y) * pitch;
        uint32_t* dRow = &decodedSrc[static_cast<size_t>(y) * static_cast<size_t>(srcW)];
        if (bpp == 4u) {
            const uint32_t* s32 = reinterpret_cast<const uint32_t*>(sRow);
            for (int32_t x = 0; x < srcW; ++x) {
                uint32_t v = s32[x];
                if (xboxFmt == 0x3Au || xboxFmt == 0x3Fu) { // A8B8G8R8
                    v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                } else if (xboxFmt == 0x3Bu || xboxFmt == 0x40u) { // B8G8R8A8
                    v = ((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16) | (((v >> 16) & 0xFFu) << 8) | ((v >> 24) & 0xFFu);
                } else if (xboxFmt == 0x3Cu || xboxFmt == 0x41u) { // R8G8B8A8
                    v = ((v & 0xFFu) << 24) | ((v >> 8) & 0xFFFFFFu);
                }
                dRow[x] = v | 0xFF000000u;
            }
        } else if (xboxFmt == 0x05u || xboxFmt == 0x11u) { // R5G6B5 / LIN_R5G6B5
            const uint16_t* s16 = reinterpret_cast<const uint16_t*>(sRow);
            for (int32_t x = 0; x < srcW; ++x) {
                uint16_t p = s16[x];
                uint32_t r = ((p >> 11) & 0x1Fu); r = (r << 3) | (r >> 2);
                uint32_t g = ((p >> 5)  & 0x3Fu); g = (g << 2) | (g >> 4);
                uint32_t b = ( p        & 0x1Fu); b = (b << 3) | (b >> 2);
                dRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else if (xboxFmt == 0x02u || xboxFmt == 0x03u || xboxFmt == 0x10u || xboxFmt == 0x1Cu) { // A1R5G5B5 / X1R5G5B5 / LIN_A1R5G5B5 / LIN_X1R5G5B5
            const uint16_t* s16 = reinterpret_cast<const uint16_t*>(sRow);
            for (int32_t x = 0; x < srcW; ++x) {
                uint16_t p = s16[x];
                uint32_t r = ((p >> 10) & 0x1Fu); r = (r << 3) | (r >> 2);
                uint32_t g = ((p >> 5)  & 0x1Fu); g = (g << 3) | (g >> 2);
                uint32_t b = ( p        & 0x1Fu); b = (b << 3) | (b >> 2);
                dRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else if (xboxFmt == 0x04u || xboxFmt == 0x1Du) { // A4R4G4B4 / LIN_A4R4G4B4
            const uint16_t* s16 = reinterpret_cast<const uint16_t*>(sRow);
            for (int32_t x = 0; x < srcW; ++x) {
                uint16_t p = s16[x];
                uint32_t r = ((p >> 8) & 0x0Fu) * 17u;
                uint32_t g = ((p >> 4) & 0x0Fu) * 17u;
                uint32_t b = ( p       & 0x0Fu) * 17u;
                dRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else {
            for (int32_t x = 0; x < srcW; x += 2) {
                if (x + 1 < srcW) {
                    int y0, u, y1, v;
                    if (xboxFmt == 0x25u) { // D3DFMT_UYVY
                        u  = static_cast<int>(sRow[x * 2 + 0]);
                        y0 = static_cast<int>(sRow[x * 2 + 1]);
                        v  = static_cast<int>(sRow[x * 2 + 2]);
                        y1 = static_cast<int>(sRow[x * 2 + 3]);
                    } else {                // D3DFMT_YUY2 (0x24)
                        y0 = static_cast<int>(sRow[x * 2 + 0]);
                        u  = static_cast<int>(sRow[x * 2 + 1]);
                        y1 = static_cast<int>(sRow[x * 2 + 2]);
                        v  = static_cast<int>(sRow[x * 2 + 3]);
                    }
                    dRow[x]     = YuvBt601StudioToBgra(y0, u, v);
                    dRow[x + 1] = YuvBt601StudioToBgra(y1, u, v);
                } else {
                    int y0 = (xboxFmt == 0x25u) ? static_cast<int>(sRow[x * 2 + 1]) : static_cast<int>(sRow[x * 2 + 0]);
                    int u  = (xboxFmt == 0x25u) ? static_cast<int>(sRow[x * 2 + 0]) : static_cast<int>(sRow[x * 2 + 1]);
                    dRow[x] = YuvBt601StudioToBgra(y0, u, 128);
                }
            }
        }
    }

    // 4. Upload decoded source frame to GPU SRV for persistent non-destructive Present compositing
    if (g_d3dDevice && g_d3dCtx) {
        uint32_t uw = static_cast<uint32_t>(srcW);
        uint32_t uh = static_cast<uint32_t>(srcH);
        if (!g_videoOverlayTex || g_videoOverlayTexW != uw || g_videoOverlayTexH != uh) {
            if (g_videoOverlaySRV) { g_videoOverlaySRV->Release(); g_videoOverlaySRV = nullptr; }
            if (g_videoOverlayTex) { g_videoOverlayTex->Release(); g_videoOverlayTex = nullptr; }
            g_videoOverlayTexW = uw;
            g_videoOverlayTexH = uh;

            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = uw;
            td.Height           = uh;
            td.MipLevels        = 1u;
            td.ArraySize        = 1u;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1u;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem     = decodedSrc.data();
            initData.SysMemPitch = uw * 4u;
            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, &initData, &g_videoOverlayTex))) {
                g_d3dDevice->CreateShaderResourceView(g_videoOverlayTex, nullptr, &g_videoOverlaySRV);
            }
        } else {
            g_d3dCtx->UpdateSubresource(g_videoOverlayTex, 0u, nullptr, decodedSrc.data(), uw * 4u, 0u);
        }
    }

    // Setting NV_PVIDEO_BUFFER = 1 ([esi+8700h] = 1 at 0x006B3233) activates the persistent overlay plane
    g_overlayEnabled = true;
    g_overlayHasData = (g_videoOverlaySRV != nullptr);

    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
}

void InitXboxVideo() {
    // 1. Virtualize the 13-byte `rdtsc * 3` blocks in cDivxVideo::FrameMove (0x0043DE97..0x0043DEA3)
    //    and cDivxVideo::Render (0x0043DF56..0x0043DF62) to 733.333 MHz Xbox CPU cycles while letting
    //    both functions execute their native Ghost.exe machine code.
    PatchCallAndNops(0x0043DE97, &Bridge_DivxRdtscTimes3_FrameMove, 13u);
    PatchCallAndNops(0x0043DF56, &Bridge_DivxRdtscTimes3_Render,    13u);

    // 2. D3DDevice YUY2 Hardware Overlay bridges
    InstallBridge(0x006B3250, &Bridge_D3DDevice_EnableOverlay);              // _D3DDevice_EnableOverlay@4
    InstallBridge(0x006B30B0, &Bridge_D3DDevice_GetOverlayUpdateStatus);     // _D3DDevice_GetOverlayUpdateStatus@0
    InstallBridge(0x006B30D0, &Bridge_D3DDevice_UpdateOverlay);              // _D3DDevice_UpdateOverlay@20
}
