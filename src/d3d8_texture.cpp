#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include "profiler.h"
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <malloc.h>
#include <string.h>
#include <unordered_map>
#include <vector>

// Xbox _XGUnswizzleRect@32 (0x0071FE62) & _XGSwizzleRect@32 (0x0071F711) in Ghost.exe.
// Both routines use SSE movaps/movntps (e.g. 0x00720259, 0x0071FABA) requiring 16-byte alignment on the swizzled buffer.
typedef void (__stdcall *PFN_XGUnswizzleRect)(
    const void* pSource, uint32_t Width, uint32_t Height, const RECT* pRect,
    void* pDest, uint32_t Pitch, const POINT* pPoint, uint32_t BytesPerPixel);
typedef void (__stdcall *PFN_XGSwizzleRect)(
    const void* pSource, uint32_t Pitch, const RECT* pRect,
    void* pDest, uint32_t Width, uint32_t Height, const POINT* pPoint, uint32_t BytesPerPixel);
static PFN_XGUnswizzleRect g_pfnXGUnswizzleRect = reinterpret_cast<PFN_XGUnswizzleRect>(ADDR_XG_UNSWIZZLE_RECT);
static PFN_XGSwizzleRect   g_pfnXGSwizzleRect   = reinterpret_cast<PFN_XGSwizzleRect>(ADDR_XG_SWIZZLE_RECT);

struct AlignedByteBuffer {
    uint8_t* ptr = nullptr;
    AlignedByteBuffer() = default;
    explicit AlignedByteBuffer(size_t bytes) {
        if (bytes > 0u) {
            ptr = static_cast<uint8_t*>(_aligned_malloc(bytes, 16u));
        }
    }
    ~AlignedByteBuffer() {
        if (ptr) {
            _aligned_free(ptr);
        }
    }
    AlignedByteBuffer(const AlignedByteBuffer&) = delete;
    AlignedByteBuffer& operator=(const AlignedByteBuffer&) = delete;

    bool allocate(size_t bytes) {
        if (ptr) {
            _aligned_free(ptr);
            ptr = nullptr;
        }
        if (bytes == 0u) return true;
        ptr = static_cast<uint8_t*>(_aligned_malloc(bytes, 16u));
        return ptr != nullptr;
    }
    uint8_t* data() const { return ptr; }
};

static void CallXGUnswizzleRectSafe(
    const void* pSource, uint32_t Width, uint32_t Height, const RECT* pRect,
    void* pDest, uint32_t Pitch, const POINT* pPoint, uint32_t BytesPerPixel)
{
    if (!pSource || !pDest || Width == 0u || Height == 0u || BytesPerPixel == 0u) return;
    if ((reinterpret_cast<uintptr_t>(pSource) & 15u) == 0u) {
        g_pfnXGUnswizzleRect(pSource, Width, Height, pRect, pDest, Pitch, pPoint, BytesPerPixel);
        return;
    }
    size_t swzBytes = static_cast<size_t>(Width) * static_cast<size_t>(Height) * static_cast<size_t>(BytesPerPixel);
    AlignedByteBuffer alignedSrc(swzBytes);
    if (!alignedSrc.data()) return;
    memcpy(alignedSrc.data(), pSource, swzBytes);
    g_pfnXGUnswizzleRect(alignedSrc.data(), Width, Height, pRect, pDest, Pitch, pPoint, BytesPerPixel);
}

static void CallXGSwizzleRectSafe(
    const void* pSource, uint32_t Pitch, const RECT* pRect,
    void* pDest, uint32_t Width, uint32_t Height, const POINT* pPoint, uint32_t BytesPerPixel)
{
    if (!pSource || !pDest || Width == 0u || Height == 0u || BytesPerPixel == 0u) return;
    if ((reinterpret_cast<uintptr_t>(pDest) & 15u) == 0u) {
        g_pfnXGSwizzleRect(pSource, Pitch, pRect, pDest, Width, Height, pPoint, BytesPerPixel);
        return;
    }
    size_t swzBytes = static_cast<size_t>(Width) * static_cast<size_t>(Height) * static_cast<size_t>(BytesPerPixel);
    AlignedByteBuffer alignedDst(swzBytes);
    if (!alignedDst.data()) return;
    if (pRect != nullptr || pPoint != nullptr) {
        memcpy(alignedDst.data(), pDest, swzBytes);
    }
    g_pfnXGSwizzleRect(pSource, Pitch, pRect, alignedDst.data(), Width, Height, pPoint, BytesPerPixel);
    memcpy(pDest, alignedDst.data(), swzBytes);
}

// Lock2DSurface@PixelJar@D3D (0x006B7150) & Lock3DSurface@PixelJar@D3D (0x006B71F0)
typedef void (__stdcall *PFN_Lock2DSurface)(
    XboxPixelContainer* pContainer, uint32_t FaceType, UINT Level,
    XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags);
static PFN_Lock2DSurface g_pfnLock2DSurface = reinterpret_cast<PFN_Lock2DSurface>(ADDR_PIXELJAR_LOCK_2D);

typedef void (__stdcall *PFN_Lock3DSurface)(
    XboxPixelContainer* pContainer, UINT Level,
    void* pLockedVolume, const void* pBox, DWORD Flags);
static PFN_Lock3DSurface g_pfnLock3DSurface = reinterpret_cast<PFN_Lock3DSurface>(ADDR_PIXELJAR_LOCK_3D);

XboxPixelContainer* g_boundTextures[4] = {};

// ============================================================================
// Texture Cache & Xbox Swizzled/Linear/DXT Upload to D3D11
// ============================================================================

struct CachedTexture {
    XboxPixelContainer*       xboxTex;
    uint32_t                  dataPtr;
    uint32_t                  exactByteSize;
    uint32_t                  format;
    uint32_t                  sizeField;
    uint64_t                  contentHash;
    uint32_t                  lastCheckedFrame;
    size_t                    maxReadableBytes;
    bool                      dirty;
    bool                      gpuDirty;
    bool                      isRenderTarget;
    bool                      isCube;
    bool                      isVolume;
    uint32_t                  width;
    uint32_t                  height;
    uint32_t                  depth;
    uint32_t                  mipLevels;
    uint32_t                  texWidth;
    uint32_t                  texHeight;
    ID3D11Texture2D*          tex2D;
    ID3D11Texture3D*          tex3D;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView*   rtv;
    uint64_t                  gpuDirtySubresMask;
};

struct TexExactKey {
    uint32_t dataPtr;
    uint32_t width;
    uint32_t height;
    bool operator==(const TexExactKey& o) const noexcept {
        return dataPtr == o.dataPtr && width == o.width && height == o.height;
    }
};

struct TexExactKeyHash {
    size_t operator()(const TexExactKey& k) const noexcept {
        uint64_t x = (static_cast<uint64_t>(k.dataPtr) * 11400714819323198485ULL) ^
                     (static_cast<uint64_t>(k.width) << 16) ^
                     static_cast<uint64_t>(k.height);
        x ^= (x >> 33);
        x *= 0xff51afd7ed558ccdULL;
        x ^= (x >> 33);
        return static_cast<size_t>(x);
    }
};

struct TexIntervalNode {
    uint32_t       start;
    uint32_t       end;
    uint32_t       maxEndPrefix;
    CachedTexture* tex;
};

// Dual-index texture cache:
// 1. g_texExactMap: O(1) exact (dataPtr, width, height) hash lookup for main SRV/RTV/CopyRects path
// 2. g_texIntervals: O(log N) sorted augmented interval tree [dataPtr, dataPtr + exactByteSize) for subresource RTV/readback/dirty/evict
static std::unordered_map<TexExactKey, CachedTexture*, TexExactKeyHash> g_texExactMap;
static std::unordered_map<uint32_t, CachedTexture*>                     g_texByContainerMap;
static std::vector<TexIntervalNode>                                     g_texIntervals;

static CachedTexture* g_curActiveOffscreenEntry  = nullptr;
static uint32_t       g_curActiveOffscreenSubIdx = 0u;

static std::atomic<uintptr_t> g_minCachedTexAddr{~static_cast<uintptr_t>(0)};
static std::atomic<uintptr_t> g_maxCachedTexAddr{0u};
static DWORD                  g_texRenderThreadId = 0u;
static SRWLOCK                g_pendingTexEvictLock = SRWLOCK_INIT;
struct PendingMemRange {
    uintptr_t addr;
    size_t    byteLen;
};
static std::vector<PendingMemRange> g_pendingTexEvictions;
static std::atomic<bool>            g_hasPendingTexEvictions{false};

uint64_t g_diagBackBufferCopyCount     = 0ULL;
uint64_t g_diagSurfaceGpuReadbackCount = 0ULL;

uint32_t GetCachedTextureCount() {
    return static_cast<uint32_t>(g_texExactMap.size());
}

uint64_t GetSurfaceGpuReadbackCount() {
    return g_diagSurfaceGpuReadbackCount;
}

static void NoteCachedTexAddressSpan(uintptr_t start, uintptr_t end) {
    uintptr_t curMin = g_minCachedTexAddr.load(std::memory_order_relaxed);
    while (start < curMin &&
           !g_minCachedTexAddr.compare_exchange_weak(curMin, start, std::memory_order_relaxed)) {}
    uintptr_t curMax = g_maxCachedTexAddr.load(std::memory_order_relaxed);
    while (end > curMax &&
           !g_maxCachedTexAddr.compare_exchange_weak(curMax, end, std::memory_order_relaxed)) {}
}

static void RebuildTexIntervalMaxEnds(size_t fromIdx) {
    uint32_t runningMax = (fromIdx > 0u) ? g_texIntervals[fromIdx - 1u].maxEndPrefix : 0u;
    for (size_t i = fromIdx; i < g_texIntervals.size(); ++i) {
        if (g_texIntervals[i].end > runningMax) runningMax = g_texIntervals[i].end;
        g_texIntervals[i].maxEndPrefix = runningMax;
    }
}

static void InsertTexInterval(CachedTexture* c) {
    uint32_t start = c->dataPtr;
    uint32_t span  = c->exactByteSize ? c->exactByteSize : 1u;
    uint32_t end   = (0xFFFFFFFFu - start < span) ? 0xFFFFFFFFu : (start + span);
    auto it = std::lower_bound(
        g_texIntervals.begin(), g_texIntervals.end(), start,
        [end](const TexIntervalNode& n, uint32_t s) {
            return (n.start < s) || (n.start == s && n.end < end);
        });
    size_t insIdx = static_cast<size_t>(it - g_texIntervals.begin());
    g_texIntervals.insert(it, { start, end, end, c });
    RebuildTexIntervalMaxEnds(insIdx);
    NoteCachedTexAddressSpan(start, end);
    if (c->xboxTex) {
        uintptr_t hdrAddr = reinterpret_cast<uintptr_t>(c->xboxTex);
        g_texByContainerMap[static_cast<uint32_t>(hdrAddr)] = c;
        NoteCachedTexAddressSpan(hdrAddr, hdrAddr + sizeof(XboxPixelContainer));
    }
}

static void RemoveTexInterval(CachedTexture* c) {
    for (size_t i = 0; i < g_texIntervals.size(); ++i) {
        if (g_texIntervals[i].tex == c) {
            g_texIntervals.erase(g_texIntervals.begin() + static_cast<ptrdiff_t>(i));
            RebuildTexIntervalMaxEnds(i);
            break;
        }
    }
    if (c->xboxTex) {
        uint32_t hdrKey = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c->xboxTex));
        auto it = g_texByContainerMap.find(hdrKey);
        if (it != g_texByContainerMap.end() && it->second == c) {
            g_texByContainerMap.erase(it);
        }
    }
}

struct OffscreenDSVEntry {
    uint32_t                  dataPtr;
    uint32_t                  exactByteSize;
    uint32_t                  width;
    uint32_t                  height;
    bool                      gpuDirty;
    ID3D11Texture2D*          tex;
    ID3D11DepthStencilView*   dsv;
    ID3D11ShaderResourceView* srv;
};
static std::vector<OffscreenDSVEntry> g_offscreenDSVs;
static bool                           g_mainDepthCpuDirty = false;

struct SubresourceRtvEntry {
    ID3D11Texture2D*        parentTex;
    uint32_t                subresourceIdx;
    uint32_t                dataPtr;
    uint32_t                width;
    uint32_t                height;
    ID3D11RenderTargetView* rtv;
};
static std::vector<SubresourceRtvEntry> g_subresourceRtvs;

static void ReleaseSubresourceRtvsForTex(ID3D11Texture2D* tex) {
    if (!tex) return;
    for (size_t i = g_subresourceRtvs.size(); i > 0; --i) {
        if (g_subresourceRtvs[i - 1].parentTex == tex) {
            if (g_subresourceRtvs[i - 1].rtv) {
                g_subresourceRtvs[i - 1].rtv->Release();
            }
            g_subresourceRtvs.erase(g_subresourceRtvs.begin() + (i - 1));
        }
    }
}

static bool IsXboxDepthStencilFormat(uint32_t xboxFmt) {
    switch (xboxFmt) {
        case 0x2Au: // D3DFMT_D24S8
        case 0x2Bu: // D3DFMT_F24S8
        case 0x2Cu: // D3DFMT_D16
        case 0x2Du: // D3DFMT_F16
        case 0x2Eu: // D3DFMT_LIN_D24S8
        case 0x2Fu: // D3DFMT_LIN_F24S8
        case 0x30u: // D3DFMT_LIN_D16
        case 0x31u: // D3DFMT_LIN_F16
            return true;
        default:
            return false;
    }
}

static void FlushRemainingGpuDirtySubresourcesToUma(CachedTexture& c, uint32_t discardSubIdx);

static void MarkCachedTextureCpuDirty(CachedTexture* c) {
    c->dirty = true;
    c->gpuDirty = false;
    c->gpuDirtySubresMask = 0ULL;
    c->isRenderTarget = false;
}

static void MarkTextureDirtyByRange(
    uint32_t dataPtr, uint32_t byteLen,
    CachedTexture* discardTex = nullptr, uint32_t discardSubIdx = 0xFFFFFFFFu)
{
    uint32_t qStart = dataPtr;
    uint32_t span   = byteLen ? byteLen : 1u;
    uint32_t qEnd   = (0xFFFFFFFFu - qStart < span) ? 0xFFFFFFFFu : (qStart + span);

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainZS  = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A20);
    if ((mainBB0 && mainBB0->Data >= qStart && mainBB0->Data < qEnd) ||
        (mainBB1 && mainBB1->Data >= qStart && mainBB1->Data < qEnd) ||
        (mainBB0 && mainBB0->Data == dataPtr) ||
        (mainBB1 && mainBB1->Data == dataPtr))
    {
        g_backBufferCpuDirty = false;
    }
    if (mainZS && ((mainZS->Data >= qStart && mainZS->Data < qEnd) || mainZS->Data == dataPtr)) {
        g_mainDepthCpuDirty = false;
    }
    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        uint32_t dsSpan = g_offscreenDSVs[i].exactByteSize ? g_offscreenDSVs[i].exactByteSize : 1u;
        uint32_t dsEnd  = (0xFFFFFFFFu - g_offscreenDSVs[i].dataPtr < dsSpan)
            ? 0xFFFFFFFFu
            : (g_offscreenDSVs[i].dataPtr + dsSpan);
        if (g_offscreenDSVs[i].dataPtr < qEnd && dsEnd > qStart) {
            g_offscreenDSVs[i].gpuDirty = false;
        }
    }

    // O(1) check if dataPtr matches an XboxPixelContainer header address
    auto hdrIt = g_texByContainerMap.find(dataPtr);
    if (hdrIt != g_texByContainerMap.end() && hdrIt->second) {
        CachedTexture* hc = hdrIt->second;
        FlushRemainingGpuDirtySubresourcesToUma(*hc, (hc == discardTex) ? discardSubIdx : 0xFFFFFFFFu);
        MarkCachedTextureCpuDirty(hc);
    }

    // O(log N) interval overlap query on [qStart, qEnd) using exactByteSize
    auto it = std::lower_bound(
        g_texIntervals.begin(), g_texIntervals.end(), qEnd,
        [](const TexIntervalNode& n, uint32_t val) { return n.start < val; });
    size_t idx = static_cast<size_t>(it - g_texIntervals.begin());
    while (idx > 0u) {
        --idx;
        const TexIntervalNode& n = g_texIntervals[idx];
        if (n.maxEndPrefix <= qStart) break;
        if (n.end > qStart && n.tex) {
            CachedTexture* tc = n.tex;
            FlushRemainingGpuDirtySubresourcesToUma(*tc, (tc == discardTex) ? discardSubIdx : 0xFFFFFFFFu);
            MarkCachedTextureCpuDirty(tc);
        }
    }
}

void EvictTexturesByMemoryRange(uintptr_t addr, size_t byteLen) {
    if (byteLen == 0u || addr < kMinValidHostAddress || addr > 0xFFFFFFFFu) return;
    uint32_t qStart = static_cast<uint32_t>(addr);
    uint32_t qEnd   = (byteLen >= static_cast<size_t>(0xFFFFFFFFu - qStart))
        ? 0xFFFFFFFFu
        : static_cast<uint32_t>(qStart + byteLen);
    if (qEnd <= qStart) return;

    // Scrub active offscreen container pointers if a temporary D3DSurface wrapper is freed
    if (g_curOffscreenRT) {
        uintptr_t rtAddr = reinterpret_cast<uintptr_t>(g_curOffscreenRT);
        if (rtAddr < qEnd && rtAddr + sizeof(XboxPixelContainer) > qStart) {
            g_curOffscreenRT = nullptr;
        }
    }
    if (g_curOffscreenZS) {
        uintptr_t zsAddr = reinterpret_cast<uintptr_t>(g_curOffscreenZS);
        if (zsAddr < qEnd && zsAddr + sizeof(XboxPixelContainer) > qStart) {
            g_curOffscreenZS = nullptr;
        }
    }

    // Scrub bound texture slots referencing freed container or freed pixel data
    for (uint32_t s = 0u; s < 4u; ++s) {
        XboxPixelContainer* bt = g_boundTextures[s];
        if (!bt) continue;
        uintptr_t btAddr = reinterpret_cast<uintptr_t>(bt);
        bool hdrHit  = (btAddr < qEnd && btAddr + sizeof(XboxPixelContainer) > qStart);
        bool dataHit = (!hdrHit && btAddr >= kMinValidHostAddress && bt->Data >= qStart && bt->Data < qEnd);
        if (hdrHit || dataHit) {
            g_boundTextures[s] = nullptr;
        }
    }

    bool evictedAnyD3D = false;

    // Collect CachedTexture* entries overlapping [qStart, qEnd) by pixel data interval
    std::vector<CachedTexture*> toEvict;
    auto it = std::lower_bound(
        g_texIntervals.begin(), g_texIntervals.end(), qEnd,
        [](const TexIntervalNode& n, uint32_t val) { return n.start < val; });
    size_t idx = static_cast<size_t>(it - g_texIntervals.begin());
    while (idx > 0u) {
        --idx;
        const TexIntervalNode& n = g_texIntervals[idx];
        if (n.maxEndPrefix <= qStart) break;
        if (n.end > qStart && n.tex) {
            toEvict.push_back(n.tex);
        }
    }

    // Also check if an XboxPixelContainer header itself lies inside [qStart, qEnd)
    for (auto kv = g_texByContainerMap.begin(); kv != g_texByContainerMap.end(); ++kv) {
        uint32_t hdrStart = kv->first;
        uint32_t hdrEnd   = (0xFFFFFFFFu - hdrStart < sizeof(XboxPixelContainer))
            ? 0xFFFFFFFFu
            : (hdrStart + static_cast<uint32_t>(sizeof(XboxPixelContainer)));
        if (hdrStart < qEnd && hdrEnd > qStart && kv->second) {
            if (std::find(toEvict.begin(), toEvict.end(), kv->second) == toEvict.end()) {
                toEvict.push_back(kv->second);
            }
        }
    }

    for (size_t i = 0; i < toEvict.size(); ++i) {
        CachedTexture* c = toEvict[i];
        if (!c) continue;

        if (g_curActiveOffscreenEntry == c) {
            g_curActiveOffscreenEntry  = nullptr;
            g_curActiveOffscreenSubIdx = 0u;
        }
        bool wasActiveRtv = (c->rtv && c->rtv == g_curActiveRTV);
        if (!wasActiveRtv && c->tex2D && g_curActiveRTV) {
            for (size_t sr = 0; sr < g_subresourceRtvs.size(); ++sr) {
                if (g_subresourceRtvs[sr].parentTex == c->tex2D &&
                    g_subresourceRtvs[sr].rtv == g_curActiveRTV)
                {
                    wasActiveRtv = true;
                    break;
                }
            }
        }
        if (wasActiveRtv) {
            g_curActiveRTV   = g_rtv;
            g_curOffscreenRT = nullptr;
            if (g_d3dCtx) {
                if (g_curActiveRTV) {
                    g_d3dCtx->OMSetRenderTargets(1, &g_curActiveRTV, g_curActiveDSV);
                } else {
                    g_d3dCtx->OMSetRenderTargets(0, nullptr, g_curActiveDSV);
                }
            }
        }

        RemoveTexInterval(c);
        TexExactKey ek = { c->dataPtr, c->width, c->height };
        auto ekIt = g_texExactMap.find(ek);
        if (ekIt != g_texExactMap.end() && ekIt->second == c) {
            g_texExactMap.erase(ekIt);
        }

        if (c->rtv)   { c->rtv->Release();   c->rtv   = nullptr; }
        if (c->srv)   { c->srv->Release();   c->srv   = nullptr; }
        if (c->tex2D) { ReleaseSubresourceRtvsForTex(c->tex2D); c->tex2D->Release(); c->tex2D = nullptr; }
        if (c->tex3D) { c->tex3D->Release(); c->tex3D = nullptr; }
        delete c;
        evictedAnyD3D = true;
    }

    // Evict OffscreenDSVEntry resources overlapping [qStart, qEnd)
    for (size_t i = g_offscreenDSVs.size(); i > 0u; --i) {
        OffscreenDSVEntry& ds = g_offscreenDSVs[i - 1u];
        uint32_t dsSpan = ds.exactByteSize ? ds.exactByteSize : 1u;
        uint32_t dsEnd  = (0xFFFFFFFFu - ds.dataPtr < dsSpan) ? 0xFFFFFFFFu : (ds.dataPtr + dsSpan);
        if (ds.dataPtr < qEnd && dsEnd > qStart) {
            if (g_curActiveDSV && g_curActiveDSV == ds.dsv) {
                g_curActiveDSV   = nullptr;
                g_curOffscreenZS = nullptr;
                if (g_d3dCtx) {
                    if (g_curActiveRTV) {
                        g_d3dCtx->OMSetRenderTargets(1, &g_curActiveRTV, nullptr);
                    } else {
                        g_d3dCtx->OMSetRenderTargets(0, nullptr, nullptr);
                    }
                }
            }
            if (ds.dsv) { ds.dsv->Release(); ds.dsv = nullptr; }
            if (ds.srv) { ds.srv->Release(); ds.srv = nullptr; }
            if (ds.tex) { ds.tex->Release(); ds.tex = nullptr; }
            g_offscreenDSVs.erase(g_offscreenDSVs.begin() + static_cast<ptrdiff_t>(i - 1u));
            evictedAnyD3D = true;
        }
    }

    if (g_texExactMap.empty() && g_offscreenDSVs.empty()) {
        g_minCachedTexAddr.store(~static_cast<uintptr_t>(0), std::memory_order_relaxed);
        g_maxCachedTexAddr.store(0u, std::memory_order_relaxed);
    }

    if (evictedAnyD3D) {
        InvalidateD3D11ShadowState();
    }
}

void DrainPendingTextureEvictions() {
    if (!g_hasPendingTexEvictions.load(std::memory_order_acquire)) return;
    std::vector<PendingMemRange> local;
    AcquireSRWLockExclusive(&g_pendingTexEvictLock);
    local.swap(g_pendingTexEvictions);
    g_hasPendingTexEvictions.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_pendingTexEvictLock);

    for (size_t i = 0; i < local.size(); ++i) {
        EvictTexturesByMemoryRange(local[i].addr, local[i].byteLen);
    }
}

static void OnXboxMemoryFreedTextureHook(const void* ptr, size_t byteLen) {
    if (!ptr || byteLen == 0u) return;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t minA = g_minCachedTexAddr.load(std::memory_order_relaxed);
    uintptr_t maxA = g_maxCachedTexAddr.load(std::memory_order_relaxed);
    if (minA >= maxA) return;
    uintptr_t end = (byteLen >= (~static_cast<uintptr_t>(0) - addr)) ? ~static_cast<uintptr_t>(0) : (addr + byteLen);
    if (end <= minA || addr >= maxA) return;

    DWORD renderTid = g_texRenderThreadId;
    if (renderTid == 0u || GetCurrentThreadId() == renderTid) {
        EvictTexturesByMemoryRange(addr, byteLen);
    } else {
        AcquireSRWLockExclusive(&g_pendingTexEvictLock);
        g_pendingTexEvictions.push_back({ addr, byteLen });
        g_hasPendingTexEvictions.store(true, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_pendingTexEvictLock);
    }
}

bool IsXboxSignedBumpFormat(uint32_t xboxFmt) {
    switch (xboxFmt) {
        case 0x17: // D3DFMT_LIN_V8U8
        case 0x27: // D3DFMT_L6V5U5
        case 0x28: // D3DFMT_V8U8
        case 0x33: // D3DFMT_V16U16
        case 0x36: // D3DFMT_LIN_V16U16
        case 0x37: // D3DFMT_LIN_L6V5U5
            return true;
        default:
            return false;
    }
}

static size_t GetReadableBytesAt(const void* ptr) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < kMinValidHostAddress) return 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    uintptr_t regEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
    return (regEnd > cur) ? (regEnd - cur) : 0;
}

static uint64_t ComputeFastSampleHash(const void* data, size_t byteLen, size_t knownMaxReadable = 0) {
    size_t maxReadable = knownMaxReadable ? knownMaxReadable : GetReadableBytesAt(data);
    if (byteLen > maxReadable) byteLen = maxReadable;
    if (!data || byteLen < 64) return 0;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(data);
    size_t dwords = byteLen / 4;
    size_t step = dwords / 64;
    if (step == 0) step = 1;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < 64 && (i * step) < dwords; ++i) {
        h ^= p[i * step];
        h *= 1099511628211ULL;
    }
    return h;
}

void DecodeXboxTextureFormat(
    const XboxPixelContainer* pTex,
    uint32_t* outWidth, uint32_t* outHeight, uint32_t* outLevels,
    uint32_t* outXboxFmt, bool* outSwizzled, uint32_t* outLinearPitch)
{
    uint32_t fmt = pTex->Format;
    uint32_t size = pTex->Size;
    *outXboxFmt = (fmt >> 8) & 0xFF;
    *outLevels = (fmt >> 16) & 0x0F;
    if (*outLevels == 0) *outLevels = 1;

    if (size != 0) {
        *outWidth = (size & 0xFFF) + 1;
        *outHeight = ((size >> 12) & 0xFFF) + 1;
        *outLinearPitch = (((size >> 24) & 0xFF) + 1) * 64;
        *outSwizzled = false;
    } else {
        *outWidth = 1u << ((fmt >> 20) & 0x0F);
        *outHeight = 1u << ((fmt >> 24) & 0x0F);
        *outLinearPitch = 0;
        *outSwizzled = true;
    }
}

// Helper to decode a 4x4 DXT1/3/5 block into 32-bit BGRA8 (0xAARRGGBB)
static void DecodeDXTBlockToBGRA8(
    const uint8_t* block, uint32_t xboxFmt,
    uint32_t* dstPixels, uint32_t dstWidth, uint32_t dstHeight, uint32_t bx, uint32_t by)
{
    const uint8_t* colorBlock = (xboxFmt == 0x0C) ? block : (block + 8);
    uint16_t c0 = (uint16_t)(colorBlock[0] | (colorBlock[1] << 8));
    uint16_t c1 = (uint16_t)(colorBlock[2] | (colorBlock[3] << 8));
    uint32_t lutBits = colorBlock[4] | (colorBlock[5] << 8) | (colorBlock[6] << 16) | (colorBlock[7] << 24);

    uint32_t r[4], g[4], b[4], a[4] = { 255, 255, 255, 255 };
    r[0] = ((c0 >> 11) & 0x1F) * 255 / 31;
    g[0] = ((c0 >> 5)  & 0x3F) * 255 / 63;
    b[0] = (c0         & 0x1F) * 255 / 31;
    r[1] = ((c1 >> 11) & 0x1F) * 255 / 31;
    g[1] = ((c1 >> 5)  & 0x3F) * 255 / 63;
    b[1] = (c1         & 0x1F) * 255 / 31;

    if (xboxFmt != 0x0C || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
    }

    uint8_t dxt5Alpha[8] = {};
    uint64_t dxt5AlphaBits = 0;
    if (xboxFmt == 0x0F) {
        dxt5Alpha[0] = block[0];
        dxt5Alpha[1] = block[1];
        if (dxt5Alpha[0] > dxt5Alpha[1]) {
            for (int i = 1; i <= 6; ++i)
                dxt5Alpha[i + 1] = (uint8_t)(((7 - i) * dxt5Alpha[0] + i * dxt5Alpha[1]) / 7);
        } else {
            for (int i = 1; i <= 4; ++i)
                dxt5Alpha[i + 1] = (uint8_t)(((5 - i) * dxt5Alpha[0] + i * dxt5Alpha[1]) / 5);
            dxt5Alpha[6] = 0;
            dxt5Alpha[7] = 255;
        }
        for (int i = 0; i < 6; ++i)
            dxt5AlphaBits |= ((uint64_t)block[2 + i]) << (8 * i);
    }

    for (uint32_t py = 0; py < 4; ++py) {
        uint32_t y = by * 4 + py;
        if (y >= dstHeight) continue;
        uint16_t dxt3Row = (xboxFmt == 0x0E) ? (uint16_t)(block[py * 2] | (block[py * 2 + 1] << 8)) : 0;
        for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = bx * 4 + px;
            if (x >= dstWidth) continue;
            uint32_t pIdx = py * 4 + px;
            uint32_t cIdx = (lutBits >> (pIdx * 2)) & 0x3;
            uint32_t alpha = a[cIdx];
            if (xboxFmt == 0x0E) {
                alpha = ((dxt3Row >> (px * 4)) & 0xF) * 17;
            } else if (xboxFmt == 0x0F) {
                uint32_t aIdx = (uint32_t)((dxt5AlphaBits >> (pIdx * 3)) & 0x7);
                alpha = dxt5Alpha[aIdx];
            }
            dstPixels[y * dstWidth + x] = (alpha << 24) | (r[cIdx] << 16) | (g[cIdx] << 8) | b[cIdx];
        }
    }
}

// Computes the 128-byte aligned byte stride between cubemap faces in an Xbox D3DPixelContainer,
// matching FindSurfaceWithinTexture@PixelJar@D3D (0x006B6F9B..0x006B701B).
static size_t ComputeXboxCubemapFaceStride(uint32_t fmt, uint32_t xboxFmt, uint32_t levels, uint32_t bitsPerPixel) {
    constexpr uint32_t kCubeFaceAlignMask = 0x7Fu; // 128-byte alignment (0x80)
    constexpr uint32_t kFmtLogWidthShift  = 20u;
    constexpr uint32_t kFmtLogHeightShift = 24u;
    constexpr uint32_t kFmtLogMask        = 0x0Fu;
    constexpr uint32_t kMinDxtLog2Dim     = 2u;    // 4x4 minimum DXT block dimension (1 << 2)

    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t minLog = isDxt ? kMinDxtLog2Dim : 0u;
    uint32_t logW   = (fmt >> kFmtLogWidthShift)  & kFmtLogMask;
    uint32_t logH   = (fmt >> kFmtLogHeightShift) & kFmtLogMask;

    size_t totalMipBytes = 0;
    for (uint32_t m = 0; m < levels; ++m) {
        uint32_t effW = (logW > minLog) ? logW : minLog;
        uint32_t effH = (logH > minLog) ? logH : minLog;
        size_t mipBytes = ((size_t)1u << (effW + effH)) * bitsPerPixel / 8u;
        totalMipBytes += mipBytes;
        if (logW > 0) --logW;
        if (logH > 0) --logH;
    }
    return (totalMipBytes + kCubeFaceAlignMask) & ~(size_t)kCubeFaceAlignMask;
}

// Unswizzles a single 2D Z-slice from an Xbox 3D Morton-swizzled volume texture,
// matching XGInternalSwizzler::Init (0x0071E71F) and _XGSwizzleBox@40 (0x00720579).
static void UnswizzleBoxSliceToLinear(
    const uint8_t* srcBox, uint32_t width, uint32_t height, uint32_t depth,
    uint32_t zSlice, uint32_t bpp, uint8_t* dstSlice)
{
    uint32_t maskX = 0u, maskY = 0u, maskZ = 0u;
    uint32_t bit = 1u;
    for (uint32_t i = 1u; ; i <<= 1u) {
        uint32_t prev = bit;
        if (i < width)  { maskX |= bit; bit <<= 1u; }
        if (i < height) { maskY |= bit; bit <<= 1u; }
        if (i < depth)  { maskZ |= bit; bit <<= 1u; }
        if (bit == prev) break;
    }

    uint32_t offZ = 0u;
    for (uint32_t z = 0u; z < zSlice; ++z) {
        offZ = (offZ - maskZ) & maskZ;
    }

    uint32_t offY = 0u;
    for (uint32_t y = 0u; y < height; ++y) {
        uint8_t* dstRow = dstSlice + (size_t)y * width * bpp;
        uint32_t offX = 0u;
        for (uint32_t x = 0u; x < width; ++x) {
            uint32_t swizIdx = offX | offY | offZ;
            memcpy(dstRow + (size_t)x * bpp, srcBox + (size_t)swizIdx * bpp, bpp);
            offX = (offX - maskX) & maskX;
        }
        offY = (offY - maskY) & maskY;
    }
}

// Returns bytes per pixel for non-DXT Xbox D3DFORMAT codes, matching g_TextureFormat@D3D (0x006BE3D8..0x006BE419).
static uint32_t GetXboxFormatBytesPerPixel(uint32_t xboxFmt) {
    switch (xboxFmt) {
        // 8-bit (1 BPP) swizzled & linear formats
        case 0x00: // D3DFMT_L8
        case 0x01: // D3DFMT_AL8
        case 0x0B: // D3DFMT_P8
        case 0x13: // D3DFMT_LIN_L8
        case 0x19: // D3DFMT_A8
        case 0x1B: // D3DFMT_LIN_AL8
        case 0x1F: // D3DFMT_LIN_A8
            return 1u;

        // 16-bit (2 BPP) swizzled & linear formats
        case 0x02: // D3DFMT_A1R5G5B5
        case 0x03: // D3DFMT_X1R5G5B5
        case 0x04: // D3DFMT_A4R4G4B4
        case 0x05: // D3DFMT_R5G6B5
        case 0x10: // D3DFMT_LIN_A1R5G5B5
        case 0x11: // D3DFMT_LIN_R5G6B5
        case 0x16: // D3DFMT_LIN_R8B8
        case 0x17: // D3DFMT_LIN_G8B8 / LIN_V8U8
        case 0x1A: // D3DFMT_A8L8
        case 0x1C: // D3DFMT_LIN_X1R5G5B5
        case 0x1D: // D3DFMT_LIN_A4R4G4B4
        case 0x20: // D3DFMT_LIN_A8L8
        case 0x24: // D3DFMT_YUY2
        case 0x25: // D3DFMT_UYVY
        case 0x27: // D3DFMT_L6V5U5
        case 0x28: // D3DFMT_V8U8
        case 0x29: // D3DFMT_G8B8 / R8B8
        case 0x2C: // D3DFMT_D16
        case 0x2D: // D3DFMT_F16
        case 0x30: // D3DFMT_LIN_D16
        case 0x31: // D3DFMT_LIN_F16
        case 0x32: // D3DFMT_L16
        case 0x35: // D3DFMT_LIN_L16
        case 0x37: // D3DFMT_LIN_L6V5U5
        case 0x38: // D3DFMT_R5G5B5A1
        case 0x39: // D3DFMT_R4G4B4A4
        case 0x3D: // D3DFMT_LIN_R5G5B5A1
        case 0x3E: // D3DFMT_LIN_R4G4B4A4
            return 2u;

        // 32-bit (4 BPP) swizzled & linear color, bump, and depth-stencil formats
        // (0x06, 0x07, 0x12 LIN_A8R8G8B8, 0x1E LIN_X8R8G8B8, 0x2A D24S8, 0x2B F24S8,
        //  0x2E LIN_D24S8, 0x2F LIN_F24S8, 0x33 V16U16, 0x36 LIN_V16U16, 0x3A..0x3C, 0x3F..0x41)
        default:
            return 4u;
    }
}

static const uint32_t* FindActiveXboxPaletteEntries() {
    // CDevice+0x0F98 holds D3DPalette* m_pPalettes[4]; Xbox D3DPalette::Data (+0x04) points to 256 A8R8G8B8 entries.
    for (uint32_t stage = 0; stage < 4u; ++stage) {
        const uint32_t* pPalObj = *reinterpret_cast<const uint32_t* const*>(ADDR_D3D_DEVICE + 0x0F98u + stage * 4u);
        if (pPalObj && GetReadableBytesAt(pPalObj) >= 8u) {
            const uint32_t* palData = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(pPalObj[1]));
            if (palData && GetReadableBytesAt(palData) >= 256u * sizeof(uint32_t)) {
                return palData;
            }
        }
    }
    return nullptr;
}

static uint32_t ConvertBt601StudioYuvToBgra(int yVal, int uVal, int vVal) {
    int c = yVal - 16;
    int d = uVal - 128;
    int e = vVal - 128;
    if (c < 0) c = 0;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void DecodeSingleXboxSurfaceToBGRA8(
    const uint8_t* srcData, uint32_t w, uint32_t h, uint32_t xboxFmt,
    bool isSwizzled, uint32_t linearPitch, bool isVolumeSwizzled, uint32_t depth, uint32_t zSlice,
    uint32_t* dstPixels)
{
    if (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F) {
        uint32_t blockBytes = (xboxFmt == 0x0C) ? 8u : 16u;
        uint32_t blocksW = (w + 3u) / 4u;
        uint32_t blocksH = (h + 3u) / 4u;
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                const uint8_t* blk = srcData + (by * blocksW + bx) * blockBytes;
                DecodeDXTBlockToBGRA8(blk, xboxFmt, dstPixels, w, h, bx, by);
            }
        }
        return;
    }

    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);

    AlignedByteBuffer unswizzled;
    const uint8_t* linearSrc = srcData;
    uint32_t srcPitch = linearPitch ? linearPitch : (w * bpp);

    if (isSwizzled) {
        if (!unswizzled.allocate((size_t)w * h * bpp)) return;
        if (isVolumeSwizzled && depth > 1u) {
            UnswizzleBoxSliceToLinear(srcData, w, h, depth, zSlice, bpp, unswizzled.data());
        } else {
            CallXGUnswizzleRectSafe(srcData, w, h, nullptr, unswizzled.data(), w * bpp, nullptr, bpp);
        }
        linearSrc = unswizzled.data();
        srcPitch = w * bpp;
    }

    const uint32_t* palEntries = (xboxFmt == 0x0B) ? FindActiveXboxPaletteEntries() : nullptr;

    if (xboxFmt == 0x24 || xboxFmt == 0x25) { // YUY2 (0x24) / UYVY (0x25)
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* sRow = linearSrc + (size_t)y * srcPitch;
            uint32_t* dRow = &dstPixels[(size_t)y * w];
            for (uint32_t x = 0; x < w; x += 2u) {
                int y0, u, y1, v;
                if (x + 1u < w) {
                    if (xboxFmt == 0x25) {
                        u  = sRow[x * 2u + 0u];
                        y0 = sRow[x * 2u + 1u];
                        v  = sRow[x * 2u + 2u];
                        y1 = sRow[x * 2u + 3u];
                    } else {
                        y0 = sRow[x * 2u + 0u];
                        u  = sRow[x * 2u + 1u];
                        y1 = sRow[x * 2u + 2u];
                        v  = sRow[x * 2u + 3u];
                    }
                    dRow[x]      = ConvertBt601StudioYuvToBgra(y0, u, v);
                    dRow[x + 1u] = ConvertBt601StudioYuvToBgra(y1, u, v);
                } else {
                    y0 = (xboxFmt == 0x25) ? sRow[x * 2u + 1u] : sRow[x * 2u + 0u];
                    u  = (xboxFmt == 0x25) ? sRow[x * 2u + 0u] : sRow[x * 2u + 1u];
                    dRow[x] = ConvertBt601StudioYuvToBgra(y0, u, 128);
                }
            }
        }
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = linearSrc + (size_t)y * srcPitch;
        uint32_t* dRow = &dstPixels[(size_t)y * w];
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t argb = 0xFFFFFFFFu;
            switch (xboxFmt) {
                case 0x06: // Swizzled A8R8G8B8
                case 0x12: // Linear A8R8G8B8
                    argb = reinterpret_cast<const uint32_t*>(sRow)[x];
                    break;
                case 0x07: // Swizzled X8R8G8B8
                case 0x1E: // Linear X8R8G8B8
                    argb = reinterpret_cast<const uint32_t*>(sRow)[x] | 0xFF000000u;
                    break;
                case 0x3A: // Swizzled A8B8G8R8 / Q8W8V8U8
                case 0x3F: { // Linear A8B8G8R8
                    uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                    argb = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                    break;
                }
                case 0x3B: // Swizzled B8G8R8A8 (0xBBGGRRAA)
                case 0x40: { // Linear B8G8R8A8
                    uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                    uint32_t a = v & 0xFFu;
                    uint32_t r = (v >> 8) & 0xFFu;
                    uint32_t g = (v >> 16) & 0xFFu;
                    uint32_t b = (v >> 24) & 0xFFu;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x3C: // Swizzled R8G8B8A8 (0xRRGGBBAA)
                case 0x41: { // Linear R8G8B8A8
                    uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                    argb = ((v & 0xFFu) << 24) | ((v >> 8) & 0xFFFFFFu);
                    break;
                }
                case 0x2A: // Swizzled D24S8
                case 0x2B: // Swizzled F24S8
                case 0x2E: // Linear D24S8
                case 0x2F: { // Linear F24S8
                    uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                    uint32_t d8 = (v >> 24) & 0xFFu;
                    argb = (d8 << 24) | (d8 << 16) | (d8 << 8) | d8;
                    break;
                }
                case 0x33: // Swizzled V16U16
                case 0x36: { // Linear V16U16
                    uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                    int16_t su = static_cast<int16_t>(v & 0xFFFFu);
                    int16_t sv = static_cast<int16_t>((v >> 16) & 0xFFFFu);
                    uint32_t r = static_cast<uint32_t>((static_cast<int32_t>(su) + 32768) >> 8) & 0xFFu;
                    uint32_t g = static_cast<uint32_t>((static_cast<int32_t>(sv) + 32768) >> 8) & 0xFFu;
                    argb = 0xFF0000FFu | (r << 16) | (g << 8);
                    break;
                }
                case 0x02: // Swizzled A1R5G5B5
                case 0x10: { // Linear A1R5G5B5
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t a = (v & 0x8000u) ? 0xFFu : 0x00u;
                    uint32_t r = ((v >> 10) & 0x1Fu) * 255u / 31u;
                    uint32_t g = ((v >> 5)  & 0x1Fu) * 255u / 31u;
                    uint32_t b = (v         & 0x1Fu) * 255u / 31u;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x03: // Swizzled X1R5G5B5
                case 0x1C: { // Linear X1R5G5B5
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t r = ((v >> 10) & 0x1Fu) * 255u / 31u;
                    uint32_t g = ((v >> 5)  & 0x1Fu) * 255u / 31u;
                    uint32_t b = (v         & 0x1Fu) * 255u / 31u;
                    argb = 0xFF000000u | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x04: // Swizzled A4R4G4B4
                case 0x1D: { // Linear A4R4G4B4
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t a = ((v >> 12) & 0x0Fu) * 17u;
                    uint32_t r = ((v >> 8)  & 0x0Fu) * 17u;
                    uint32_t g = ((v >> 4)  & 0x0Fu) * 17u;
                    uint32_t b = (v         & 0x0Fu) * 17u;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x05: // Swizzled R5G6B5
                case 0x11: { // Linear R5G6B5
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t r = ((v >> 11) & 0x1Fu) * 255u / 31u;
                    uint32_t g = ((v >> 5)  & 0x3Fu) * 255u / 63u;
                    uint32_t b = (v         & 0x1Fu) * 255u / 31u;
                    argb = 0xFF000000u | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x38: // Swizzled R5G5B5A1
                case 0x3D: { // Linear R5G5B5A1
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t r = ((v >> 11) & 0x1Fu) * 255u / 31u;
                    uint32_t g = ((v >> 6)  & 0x1Fu) * 255u / 31u;
                    uint32_t b = ((v >> 1)  & 0x1Fu) * 255u / 31u;
                    uint32_t a = (v & 0x01u) ? 0xFFu : 0x00u;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x39: // Swizzled R4G4B4A4
                case 0x3E: { // Linear R4G4B4A4
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t r = ((v >> 12) & 0x0Fu) * 17u;
                    uint32_t g = ((v >> 8)  & 0x0Fu) * 17u;
                    uint32_t b = ((v >> 4)  & 0x0Fu) * 17u;
                    uint32_t a = (v         & 0x0Fu) * 17u;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                    break;
                }
                case 0x27: // Swizzled L6V5U5
                case 0x37: { // Linear L6V5U5
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    int32_t su = (static_cast<int32_t>(v) << 27) >> 27; // signed 5-bit U [-16..15]
                    int32_t sv = (static_cast<int32_t>(v) << 22) >> 27; // signed 5-bit V [-16..15]
                    int32_t ru = 128 + su * 8;
                    int32_t gv = 128 + sv * 8;
                    if (ru < 0) ru = 0; else if (ru > 255) ru = 255;
                    if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
                    uint32_t lum = ((v >> 10) & 0x3Fu) * 255u / 63u;
                    argb = 0xFF000000u | (static_cast<uint32_t>(ru) << 16) | (static_cast<uint32_t>(gv) << 8) | lum;
                    break;
                }
                case 0x28: // Swizzled V8U8 (signed du, dv)
                case 0x17: { // Linear G8B8 / V8U8
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    int8_t su = static_cast<int8_t>(v & 0xFFu);
                    int8_t sv = static_cast<int8_t>((v >> 8) & 0xFFu);
                    uint32_t r = static_cast<uint32_t>(static_cast<int32_t>(su) + 128) & 0xFFu;
                    uint32_t g = static_cast<uint32_t>(static_cast<int32_t>(sv) + 128) & 0xFFu;
                    argb = 0xFF0000FFu | (r << 16) | (g << 8);
                    break;
                }
                case 0x16: { // Linear R8B8
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t b = v & 0xFFu;
                    uint32_t r = (v >> 8) & 0xFFu;
                    argb = 0xFF000000u | (r << 16) | b;
                    break;
                }
                case 0x29: { // Swizzled G8B8
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t b = v & 0xFFu;
                    uint32_t g = (v >> 8) & 0xFFu;
                    argb = 0xFF000000u | (g << 8) | b;
                    break;
                }
                case 0x2C: // D16
                case 0x2D: // F16
                case 0x30: // LIN_D16
                case 0x31: // LIN_F16
                case 0x32: // L16
                case 0x35: { // LIN_L16
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t l = (v >> 8) & 0xFFu;
                    argb = 0xFF000000u | (l << 16) | (l << 8) | l;
                    break;
                }
                case 0x00: // Swizzled L8
                case 0x13: { // Linear L8
                    uint32_t l = sRow[x];
                    argb = 0xFF000000u | (l << 16) | (l << 8) | l;
                    break;
                }
                case 0x01: // Swizzled AL8 (R=G=B=A=L)
                case 0x1B: { // Linear AL8
                    uint32_t l = sRow[x];
                    argb = (l << 24) | (l << 16) | (l << 8) | l;
                    break;
                }
                case 0x0B: { // Swizzled P8 (8-bit paletted)
                    uint8_t idx = sRow[x];
                    if (palEntries) {
                        argb = palEntries[idx];
                    } else {
                        uint32_t l = idx;
                        argb = 0xFF000000u | (l << 16) | (l << 8) | l;
                    }
                    break;
                }
                case 0x19: // Swizzled A8
                case 0x1F: { // Linear A8
                    uint32_t a = sRow[x];
                    argb = (a << 24) | 0x00FFFFFFu;
                    break;
                }
                case 0x1A: // Swizzled A8L8
                case 0x20: { // Linear A8L8
                    uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                    uint32_t l = v & 0xFFu;
                    uint32_t a = (v >> 8) & 0xFFu;
                    argb = (a << 24) | (l << 16) | (l << 8) | l;
                    break;
                }
                default:
                    if (bpp == 4u) argb = reinterpret_cast<const uint32_t*>(sRow)[x];
                    break;
            }
            dRow[x] = argb;
        }
    }
}

// Computes the byte size of a single mip level in an Xbox swizzled/DXT container,
// matching FindSurfaceWithinTexture@PixelJar@D3D (0x006B7030..0x006B7058).
static size_t ComputeXboxMipByteSize(uint32_t logW, uint32_t logH, uint32_t logD, uint32_t xboxFmt, uint32_t bitsPerPixel) {
    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t minLog = isDxt ? 2u : 0u;
    uint32_t effW = (logW > minLog) ? logW : minLog;
    uint32_t effH = (logH > minLog) ? logH : minLog;
    return ((size_t)1u << (effW + effH + logD)) * bitsPerPixel / 8u;
}

uint32_t ComputeExactXboxTextureBytes(const XboxPixelContainer* pTex) {
    if (!pTex) return 0u;
    uint32_t w = 0u, h = 0u, levels = 1u, xboxFmt = 0u, linearPitch = 0u;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0u || h == 0u || w > 4096u || h > 4096u) return 0u;

    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
    uint32_t bitsPerPixel = isDxt ? ((xboxFmt == 0x0Cu) ? 4u : 8u) : (bpp * 8u);

    size_t totalBytes = 0u;
    if (!isSwizzled) {
        if (isDxt) {
            totalBytes = static_cast<size_t>((w + 3u) / 4u) * static_cast<size_t>((h + 3u) / 4u) *
                         ((xboxFmt == 0x0Cu) ? 8u : 16u);
        } else {
            uint32_t pitch = linearPitch ? linearPitch : (w * bpp);
            totalBytes = static_cast<size_t>(pitch) * static_cast<size_t>(h);
        }
    } else {
        bool isCubeMap  = (pTex->Format & 0x00000004u) != 0u;
        bool isVolume   = (((pTex->Format >> 4u) & 0x0Fu) == 3u);
        uint32_t depth  = isVolume ? (1u << ((pTex->Format >> 28u) & 0x0Fu)) : 1u;
        uint32_t maxDim = (w > h) ? w : h;
        if (depth > maxDim) maxDim = depth;
        uint32_t maxPossibleMips = 1u;
        while ((1u << maxPossibleMips) <= maxDim) ++maxPossibleMips;
        if (levels > maxPossibleMips) levels = maxPossibleMips;
        if (levels == 0u) levels = 1u;

        uint32_t baseLogW = (pTex->Format >> 20u) & 0x0Fu;
        uint32_t baseLogH = (pTex->Format >> 24u) & 0x0Fu;
        uint32_t baseLogD = isVolume ? ((pTex->Format >> 28u) & 0x0Fu) : 0u;

        size_t singleChainBytes = 0u;
        uint32_t logW = baseLogW, logH = baseLogH, logD = baseLogD;
        for (uint32_t m = 0u; m < levels; ++m) {
            singleChainBytes += ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
            if (logW > 0u) --logW;
            if (logH > 0u) --logH;
            if (logD > 0u) --logD;
        }
        if (isCubeMap) {
            size_t faceStride = ComputeXboxCubemapFaceStride(pTex->Format, xboxFmt, levels, bitsPerPixel);
            totalBytes = 5u * faceStride + singleChainBytes;
        } else {
            totalBytes = singleChainBytes;
        }
    }

    if (totalBytes == 0u) totalBytes = 1u;
    if (totalBytes > 0x7FFFFFFFu) totalBytes = 0x7FFFFFFFu;
    return static_cast<uint32_t>(totalBytes);
}

static void GenerateBoxMip2D(const uint32_t* src, uint32_t srcW, uint32_t srcH, uint32_t* dst, uint32_t dstW, uint32_t dstH) {
    for (uint32_t y = 0; y < dstH; ++y) {
        uint32_t sy0 = (y * 2 < srcH) ? (y * 2) : (srcH - 1);
        uint32_t sy1 = (y * 2 + 1 < srcH) ? (y * 2 + 1) : sy0;
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t sx0 = (x * 2 < srcW) ? (x * 2) : (srcW - 1);
            uint32_t sx1 = (x * 2 + 1 < srcW) ? (x * 2 + 1) : sx0;
            uint32_t p00 = src[sy0 * srcW + sx0];
            uint32_t p10 = src[sy0 * srcW + sx1];
            uint32_t p01 = src[sy1 * srcW + sx0];
            uint32_t p11 = src[sy1 * srcW + sx1];
            uint32_t b = ((p00 & 0xFFu) + (p10 & 0xFFu) + (p01 & 0xFFu) + (p11 & 0xFFu) + 2u) >> 2;
            uint32_t g = (((p00 >> 8) & 0xFFu) + ((p10 >> 8) & 0xFFu) + ((p01 >> 8) & 0xFFu) + ((p11 >> 8) & 0xFFu) + 2u) >> 2;
            uint32_t r = (((p00 >> 16) & 0xFFu) + ((p10 >> 16) & 0xFFu) + ((p01 >> 16) & 0xFFu) + ((p11 >> 16) & 0xFFu) + 2u) >> 2;
            uint32_t a = (((p00 >> 24) & 0xFFu) + ((p10 >> 24) & 0xFFu) + ((p01 >> 24) & 0xFFu) + ((p11 >> 24) & 0xFFu) + 2u) >> 2;
            dst[y * dstW + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void UploadXboxTextureToD3D11(CachedTexture& entry, const XboxPixelContainer* pTex) {
    if (!g_d3dDevice || !g_d3dCtx) return;

    constexpr uint32_t kXboxTexFormatCubemapMask   = 0x00000004u;
    constexpr uint32_t kXboxTexFormatDimShift      = 4u;
    constexpr uint32_t kXboxTexFormatDimMask       = 0x0Fu;
    constexpr uint32_t kXboxTexDimensionVolume     = 3u;
    constexpr uint32_t kXboxTexFormatDepthShift    = 28u;
    constexpr uint32_t kXboxCubemapFaceCount       = 6u;

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || !pTex->Data) return;

    bool isCubeMap = (pTex->Format & kXboxTexFormatCubemapMask) != 0u;
    bool isVolume  = (((pTex->Format >> kXboxTexFormatDimShift) & kXboxTexFormatDimMask) == kXboxTexDimensionVolume);
    uint32_t depth = isVolume ? (1u << ((pTex->Format >> kXboxTexFormatDepthShift) & 0x0Fu)) : 1u;
    if (!isSwizzled) {
        isCubeMap = false;
        isVolume  = false;
        depth     = 1u;
        levels    = 1u;
    }

    uint32_t maxDim = (w > h) ? w : h;
    if (depth > maxDim) maxDim = depth;
    uint32_t maxPossibleMips = 1u;
    while ((1u << maxPossibleMips) <= maxDim) ++maxPossibleMips;
    if (levels > maxPossibleMips) levels = maxPossibleMips;
    if (levels == 0) levels = 1u;

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(pTex->Data);
    size_t maxReadable = GetReadableBytesAt(srcData);
    if (maxReadable < 16) return;

    bool isDxt = (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F);
    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
    uint32_t bitsPerPixel = isDxt ? ((xboxFmt == 0x0C) ? 4u : 8u) : (bpp * 8u);
    size_t baseSliceBytes = isDxt
        ? ((size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) * ((xboxFmt == 0x0C) ? 8u : 16u))
        : ((size_t)(linearPitch ? linearPitch : (w * bpp)) * h);
    if (baseSliceBytes > maxReadable) return;

    struct TexUploadPerfScope {
        int64_t  t0;
        uint32_t bytes;
        ~TexUploadPerfScope() {
            g_perfAccum.texUploadTicks += (PerfQpcNow() - t0);
            ++g_perfAccum.texUploadCount;
            g_perfAccum.texUploadBytes += bytes;
        }
    } uploadPerfScope = { PerfQpcNow(), entry.exactByteSize };

    uint32_t baseLogW = (pTex->Format >> 20) & 0x0Fu;
    uint32_t baseLogH = (pTex->Format >> 24) & 0x0Fu;
    uint32_t baseLogD = isVolume ? ((pTex->Format >> 28) & 0x0Fu) : 0u;

    entry.width     = w;
    entry.height    = h;
    entry.depth     = depth;
    entry.mipLevels = levels;
    entry.isCube    = isCubeMap;
    entry.isVolume  = (isVolume && depth > 1u);

    // ------------------------------------------------------------------------
    // Case 1: True Hardware Cubemap (ID3D11Texture2D ArraySize=6, TEXTURECUBE)
    // ------------------------------------------------------------------------
    if (isCubeMap) {
        size_t faceStride = ComputeXboxCubemapFaceStride(pTex->Format, xboxFmt, levels, bitsPerPixel);
        uint32_t totalSubres = kXboxCubemapFaceCount * levels;
        std::vector<std::vector<uint32_t>> subresPixels(totalSubres);
        std::vector<D3D11_SUBRESOURCE_DATA> initData(totalSubres);

        for (uint32_t face = 0; face < kXboxCubemapFaceCount; ++face) {
            size_t faceBaseOff = (size_t)face * faceStride;
            size_t mipOff = 0;
            uint32_t logW = baseLogW, logH = baseLogH;

            for (uint32_t m = 0; m < levels; ++m) {
                uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                uint32_t mipH = (h >> m) ? (h >> m) : 1u;
                uint32_t subIdx = face * levels + m;
                subresPixels[subIdx].assign((size_t)mipW * mipH, 0xFF000000u);

                size_t mipBytes = ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel);
                if (faceBaseOff + mipOff + mipBytes <= maxReadable) {
                    DecodeSingleXboxSurfaceToBGRA8(
                        srcData + faceBaseOff + mipOff, mipW, mipH, xboxFmt,
                        isSwizzled, 0u, false, 1u, 0u, subresPixels[subIdx].data());
                } else if (m > 0) {
                    uint32_t prevW = (w >> (m - 1)) ? (w >> (m - 1)) : 1u;
                    uint32_t prevH = (h >> (m - 1)) ? (h >> (m - 1)) : 1u;
                    GenerateBoxMip2D(subresPixels[face * levels + (m - 1)].data(), prevW, prevH,
                                     subresPixels[subIdx].data(), mipW, mipH);
                }

                initData[subIdx].pSysMem = subresPixels[subIdx].data();
                initData[subIdx].SysMemPitch = mipW * 4u;
                initData[subIdx].SysMemSlicePitch = 0u;

                mipOff += mipBytes;
                if (logW > 0) --logW;
                if (logH > 0) --logH;
            }
        }

        D3D11_TEXTURE2D_DESC curCubeDesc = {};
        if (entry.tex2D) entry.tex2D->GetDesc(&curCubeDesc);
        if (!entry.tex2D || entry.tex3D ||
            (curCubeDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) == 0u ||
            curCubeDesc.MipLevels != levels ||
            entry.texWidth != w || entry.texHeight != h)
        {
            if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
            if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
            if (entry.tex2D) { ReleaseSubresourceRtvsForTex(entry.tex2D); entry.tex2D->Release(); entry.tex2D = nullptr; }
            if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
            entry.texWidth  = w;
            entry.texHeight = h;

            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = levels;
            td.ArraySize        = kXboxCubemapFaceCount;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags        = D3D11_RESOURCE_MISC_TEXTURECUBE;

            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, initData.data(), &entry.tex2D))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format                      = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvDesc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels       = levels;
                g_d3dDevice->CreateShaderResourceView(entry.tex2D, &srvDesc, &entry.srv);
            }
        } else {
            for (uint32_t face = 0; face < kXboxCubemapFaceCount; ++face) {
                for (uint32_t m = 0; m < levels; ++m) {
                    uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                    uint32_t subIdx = face * levels + m;
                    g_d3dCtx->UpdateSubresource(entry.tex2D, subIdx, nullptr, subresPixels[subIdx].data(), mipW * 4u, 0u);
                }
            }
        }
        entry.dirty = false;
        entry.gpuDirty = false;
        entry.gpuDirtySubresMask = 0ULL;
        return;
    }

    // ------------------------------------------------------------------------
    // Case 2: True Hardware 3D Volume Texture (ID3D11Texture3D, TEXTURE3D)
    // ------------------------------------------------------------------------
    if (entry.isVolume) {
        std::vector<std::vector<uint32_t>> mipPixels(levels);
        std::vector<D3D11_SUBRESOURCE_DATA> initData(levels);
        size_t mipOff = 0;
        uint32_t logW = baseLogW, logH = baseLogH, logD = baseLogD;

        for (uint32_t m = 0; m < levels; ++m) {
            uint32_t mipW = (w >> m) ? (w >> m) : 1u;
            uint32_t mipH = (h >> m) ? (h >> m) : 1u;
            uint32_t mipD = (depth >> m) ? (depth >> m) : 1u;
            mipPixels[m].assign((size_t)mipW * mipH * mipD, 0xFFFFFFFFu);

            size_t mipBytes = ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
            size_t sliceBytesMip = ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel);

            if (mipOff + mipBytes <= maxReadable) {
                const uint8_t* mipSrc = srcData + mipOff;
                for (uint32_t z = 0; z < mipD; ++z) {
                    const uint8_t* zSrc = (isDxt || !isSwizzled) ? (mipSrc + (size_t)z * sliceBytesMip) : mipSrc;
                    uint32_t* dstSlice = &mipPixels[m][(size_t)z * mipW * mipH];
                    DecodeSingleXboxSurfaceToBGRA8(
                        zSrc, mipW, mipH, xboxFmt, isSwizzled, 0u,
                        !isDxt && isSwizzled, mipD, z, dstSlice);
                }
            }

            initData[m].pSysMem          = mipPixels[m].data();
            initData[m].SysMemPitch      = mipW * 4u;
            initData[m].SysMemSlicePitch = mipW * mipH * 4u;

            mipOff += mipBytes;
            if (logW > 0) --logW;
            if (logH > 0) --logH;
            if (logD > 0) --logD;
        }

        if (!entry.tex3D || entry.tex2D || entry.texWidth != w || entry.texHeight != h) {
            if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
            if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
            if (entry.tex2D) { ReleaseSubresourceRtvsForTex(entry.tex2D); entry.tex2D->Release(); entry.tex2D = nullptr; }
            if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
            entry.texWidth  = w;
            entry.texHeight = h;

            D3D11_TEXTURE3D_DESC td3 = {};
            td3.Width     = w;
            td3.Height    = h;
            td3.Depth     = depth;
            td3.MipLevels = levels;
            td3.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
            td3.Usage     = D3D11_USAGE_DEFAULT;
            td3.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (SUCCEEDED(g_d3dDevice->CreateTexture3D(&td3, initData.data(), &entry.tex3D))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = 0;
                srvDesc.Texture3D.MipLevels       = levels;
                g_d3dDevice->CreateShaderResourceView(entry.tex3D, &srvDesc, &entry.srv);
            }
        } else {
            for (uint32_t m = 0; m < levels; ++m) {
                uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                uint32_t mipH = (h >> m) ? (h >> m) : 1u;
                g_d3dCtx->UpdateSubresource(entry.tex3D, m, nullptr, mipPixels[m].data(), mipW * 4u, mipW * mipH * 4u);
            }
        }
        entry.dirty = false;
        entry.gpuDirty = false;
        return;
    }

    // ------------------------------------------------------------------------
    // Case 3: Standard 2D Texture with Full Mip Chain (ID3D11Texture2D)
    // ------------------------------------------------------------------------
    std::vector<std::vector<uint32_t>> mipPixels(levels);
    std::vector<D3D11_SUBRESOURCE_DATA> initData(levels);
    size_t mipOff = 0;
    uint32_t logW = baseLogW, logH = baseLogH;

    for (uint32_t m = 0; m < levels; ++m) {
        uint32_t mipW = (w >> m) ? (w >> m) : 1u;
        uint32_t mipH = (h >> m) ? (h >> m) : 1u;
        mipPixels[m].assign((size_t)mipW * mipH, 0xFFFFFFFFu);

        size_t mipBytes = isSwizzled
            ? ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel)
            : baseSliceBytes;

        if (mipOff + mipBytes <= maxReadable) {
            DecodeSingleXboxSurfaceToBGRA8(
                srcData + mipOff, mipW, mipH, xboxFmt,
                isSwizzled, (m == 0) ? linearPitch : 0u, false, 1u, 0u, mipPixels[m].data());
        } else if (m > 0) {
            uint32_t prevW = (w >> (m - 1)) ? (w >> (m - 1)) : 1u;
            uint32_t prevH = (h >> (m - 1)) ? (h >> (m - 1)) : 1u;
            GenerateBoxMip2D(mipPixels[m - 1].data(), prevW, prevH, mipPixels[m].data(), mipW, mipH);
        }

        initData[m].pSysMem          = mipPixels[m].data();
        initData[m].SysMemPitch      = mipW * 4u;
        initData[m].SysMemSlicePitch = 0u;

        mipOff += mipBytes;
        if (logW > 0) --logW;
        if (logH > 0) --logH;
    }

    D3D11_TEXTURE2D_DESC cur2DDesc = {};
    if (entry.tex2D) entry.tex2D->GetDesc(&cur2DDesc);
    if (!entry.tex2D || entry.tex3D ||
        (cur2DDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0u ||
        cur2DDesc.MipLevels != levels ||
        entry.texWidth != w || entry.texHeight != h)
    {
        if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
        if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
        if (entry.tex2D) { ReleaseSubresourceRtvsForTex(entry.tex2D); entry.tex2D->Release(); entry.tex2D = nullptr; }
        if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
        entry.texWidth  = w;
        entry.texHeight = h;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = levels;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, initData.data(), &entry.tex2D))) {
            g_d3dDevice->CreateShaderResourceView(entry.tex2D, nullptr, &entry.srv);
            g_d3dDevice->CreateRenderTargetView(entry.tex2D, nullptr, &entry.rtv);
        }
    } else if (entry.tex2D) {
        for (uint32_t m = 0; m < levels; ++m) {
            uint32_t mipW = (w >> m) ? (w >> m) : 1u;
            g_d3dCtx->UpdateSubresource(entry.tex2D, m, nullptr, mipPixels[m].data(), mipW * 4u, 0u);
        }
    }
    entry.dirty = false;
    entry.gpuDirty = false;
    entry.gpuDirtySubresMask = 0ULL;
}

static OffscreenDSVEntry* GetOrCreateDepthStencilEntryWithSize(
    XboxPixelContainer* pZS, uint32_t reqW, uint32_t reqH)
{
    if (!g_d3dDevice || !pZS || !pZS->Data || static_cast<uintptr_t>(pZS->Data) < kMinValidHostAddress) {
        return nullptr;
    }

    uint32_t zsW = 0, zsH = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pZS, &zsW, &zsH, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    uint32_t w = (reqW > 0u) ? reqW : zsW;
    uint32_t h = (reqH > 0u) ? reqH : zsH;
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return nullptr;

    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        if (g_offscreenDSVs[i].dataPtr == pZS->Data &&
            g_offscreenDSVs[i].width == w &&
            g_offscreenDSVs[i].height == h)
        {
            return &g_offscreenDSVs[i];
        }
    }

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width            = w;
    dd.Height           = h;
    dd.MipLevels        = 1;
    dd.ArraySize        = 1;
    dd.Format           = DXGI_FORMAT_R24G8_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Usage            = D3D11_USAGE_DEFAULT;
    dd.BindFlags        = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    uint32_t exactZsBytes = ComputeExactXboxTextureBytes(pZS);
    if (exactZsBytes == 0u) exactZsBytes = w * h * 4u;
    OffscreenDSVEntry e = { pZS->Data, exactZsBytes, w, h, false, nullptr, nullptr, nullptr };
    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&dd, nullptr, &e.tex))) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g_d3dDevice->CreateDepthStencilView(e.tex, &dsvDesc, &e.dsv);

        D3D11_SHADER_RESOURCE_VIEW_DESC dsSrvDesc = {};
        dsSrvDesc.Format                    = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        dsSrvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        dsSrvDesc.Texture2D.MostDetailedMip = 0;
        dsSrvDesc.Texture2D.MipLevels       = 1;
        g_d3dDevice->CreateShaderResourceView(e.tex, &dsSrvDesc, &e.srv);

        if (g_d3dCtx && e.dsv) {
            g_d3dCtx->ClearDepthStencilView(e.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
    }
    NoteCachedTexAddressSpan(pZS->Data, static_cast<uintptr_t>(pZS->Data) + exactZsBytes);
    g_offscreenDSVs.push_back(e);
    return &g_offscreenDSVs.back();
}

static OffscreenDSVEntry* GetOrCreateDepthStencilEntry(XboxPixelContainer* pZS) {
    return GetOrCreateDepthStencilEntryWithSize(pZS, 0u, 0u);
}

static ID3D11RenderTargetView* GetOrCreateSubresourceRtv(
    CachedTexture& c, uint32_t faceIdx, uint32_t mipIdx, uint32_t dataPtr, uint32_t mipW, uint32_t mipH)
{
    if (!g_d3dDevice || !c.tex2D) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    c.tex2D->GetDesc(&td);
    if (mipIdx >= td.MipLevels || faceIdx >= td.ArraySize) return nullptr;

    uint32_t subIdx = D3D11CalcSubresource(mipIdx, faceIdx, td.MipLevels);
    for (size_t i = 0; i < g_subresourceRtvs.size(); ++i) {
        if (g_subresourceRtvs[i].parentTex == c.tex2D &&
            g_subresourceRtvs[i].subresourceIdx == subIdx)
        {
            g_subresourceRtvs[i].dataPtr = dataPtr;
            g_subresourceRtvs[i].width   = mipW;
            g_subresourceRtvs[i].height  = mipH;
            return g_subresourceRtvs[i].rtv;
        }
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = td.Format;
    if (td.ArraySize > 1u) {
        rtvDesc.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice        = mipIdx;
        rtvDesc.Texture2DArray.FirstArraySlice = faceIdx;
        rtvDesc.Texture2DArray.ArraySize       = 1u;
    } else {
        rtvDesc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = mipIdx;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_d3dDevice->CreateRenderTargetView(c.tex2D, &rtvDesc, &rtv)) && rtv) {
        g_subresourceRtvs.push_back({ c.tex2D, subIdx, dataPtr, mipW, mipH, rtv });
    }
    return rtv;
}

static bool FindSubresourceInCachedTexture(
    const CachedTexture& c, uint32_t surfDataPtr, uint32_t surfW, uint32_t surfH,
    uint32_t* outFaceIdx, uint32_t* outMipIdx)
{
    if (!c.tex2D || c.isVolume || surfDataPtr < c.dataPtr) return false;
    if (!c.isCube && c.mipLevels <= 1u) return false;

    uint32_t cXboxFmt = (c.format >> 8) & 0xFFu;
    bool cIsDxt = (cXboxFmt == 0x0Cu || cXboxFmt == 0x0Eu || cXboxFmt == 0x0Fu);
    if (cIsDxt || c.sizeField != 0u) return false;

    uint32_t bpp = GetXboxFormatBytesPerPixel(cXboxFmt);
    uint32_t bitsPerPixel = bpp * 8u;
    uint32_t baseLogW = (c.format >> 20) & 0x0Fu;
    uint32_t baseLogH = (c.format >> 24) & 0x0Fu;
    uint32_t numFaces = c.isCube ? 6u : 1u;
    size_t faceStride = c.isCube
        ? ComputeXboxCubemapFaceStride(c.format, cXboxFmt, c.mipLevels, bitsPerPixel)
        : 0u;

    for (uint32_t face = 0u; face < numFaces; ++face) {
        size_t faceBaseOff = static_cast<size_t>(face) * faceStride;
        size_t mipOff = 0u;
        uint32_t logW = baseLogW, logH = baseLogH;
        for (uint32_t m = 0u; m < c.mipLevels; ++m) {
            uint32_t mipW = (c.width >> m)  ? (c.width >> m)  : 1u;
            uint32_t mipH = (c.height >> m) ? (c.height >> m) : 1u;
            if (c.dataPtr + static_cast<uint32_t>(faceBaseOff + mipOff) == surfDataPtr &&
                mipW == surfW && mipH == surfH)
            {
                if (outFaceIdx) *outFaceIdx = face;
                if (outMipIdx)  *outMipIdx  = m;
                return true;
            }
            mipOff += ComputeXboxMipByteSize(logW, logH, 0u, cXboxFmt, bitsPerPixel);
            if (logW > 0u) --logW;
            if (logH > 0u) --logH;
        }
    }
    return false;
}

// True for non-owning Xbox D3DSurface subresource wrappers created by GetSurfaceLevel2 / GetCubeMapSurface2
// (D3DRTYPE_SURFACE = 0x00050000, bit 31 clear -> does not own pixel Data).
static bool IsNonOwningXboxSubSurface(const XboxPixelContainer* p) {
    if (!p) return false;
    return ((p->Common & 0x00070000u) == 0x00050000u) && (static_cast<int32_t>(p->Common) >= 0);
}

// CreateSurfaceOfTexture@D3D (0x006B44F0) allocates a 24-byte D3DSurface whose +0x14 dword points to
// the parent D3DBaseTexture (D3DRTYPE_TEXTURE = 0x00040000). Validate strictly before dereferencing +0x14.
static XboxPixelContainer* GetParentTextureOfSubSurface(const XboxPixelContainer* p) {
    if (!IsNonOwningXboxSubSurface(p)) return nullptr;
    if (GetReadableBytesAt(p) < 24u) return nullptr;
    uint32_t parentAddr = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(p) + 0x14u);
    if (parentAddr < kMinValidHostAddress || (parentAddr & 3u) != 0u ||
        parentAddr == reinterpret_cast<uintptr_t>(p))
    {
        return nullptr;
    }
    XboxPixelContainer* parent = reinterpret_cast<XboxPixelContainer*>(static_cast<uintptr_t>(parentAddr));
    if (GetReadableBytesAt(parent) < sizeof(XboxPixelContainer)) return nullptr;
    if ((parent->Common & 0x00070000u) != 0x00040000u) return nullptr;
    if (parent->Data < kMinValidHostAddress || p->Data < parent->Data) return nullptr;
    uint32_t parentBytes = ComputeExactXboxTextureBytes(parent);
    if (parentBytes == 0u) return nullptr;
    uint64_t parentEnd = static_cast<uint64_t>(parent->Data) + static_cast<uint64_t>(parentBytes);
    if (static_cast<uint64_t>(p->Data) >= parentEnd) return nullptr;
    return parent;
}

static ID3D11RenderTargetView* ResolveRenderTargetView(
    XboxPixelContainer* pTex, uint32_t* outW, uint32_t* outH,
    CachedTexture** outEntry, uint32_t* outSubresourceIdx = nullptr)
{
    if (outEntry) *outEntry = nullptr;
    if (outSubresourceIdx) *outSubresourceIdx = 0u;
    if (!g_d3dDevice || !pTex || !pTex->Data || static_cast<uintptr_t>(pTex->Data) < kMinValidHostAddress) {
        return nullptr;
    }
    if (g_texRenderThreadId == 0u) g_texRenderThreadId = GetCurrentThreadId();
    DrainPendingTextureEvictions();

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return nullptr;

    // 0. Unwrap non-owning Xbox D3DSurface (GetSurfaceLevel2 / GetCubeMapSurface2) to its parent D3DBaseTexture
    // so temporary 24-byte surface wrappers never overwrite CachedTexture::xboxTex or evict the parent on Release()!
    if (XboxPixelContainer* parentTex = GetParentTextureOfSubSurface(pTex)) {
        uint32_t pw = 0, ph = 0, plevels = 1, pxboxFmt = 0, plinearPitch = 0;
        bool pisSwizzled = false;
        DecodeXboxTextureFormat(parentTex, &pw, &ph, &plevels, &pxboxFmt, &pisSwizzled, &plinearPitch);
        if (pw > 0u && ph > 0u && pw <= 4096u && ph <= 4096u) {
            bool parentIsCube   = pisSwizzled && ((parentTex->Format & 0x00000004u) != 0u);
            uint32_t parentMips = pisSwizzled ? plevels : 1u;
            TexExactKey parentKey = { parentTex->Data, pw, ph };
            CachedTexture* pc = nullptr;
            auto pIt = g_texExactMap.find(parentKey);
            if (pIt != g_texExactMap.end() && pIt->second) {
                pc = pIt->second;
                bool shapeMismatch = (pc->isCube != parentIsCube) || (pc->mipLevels != parentMips);
                bool metaChanged   = (pc->format != parentTex->Format || pc->sizeField != parentTex->Size || pc->xboxTex != parentTex);
                if (metaChanged) {
                    RemoveTexInterval(pc);
                    pc->xboxTex       = parentTex;
                    pc->format        = parentTex->Format;
                    pc->sizeField     = parentTex->Size;
                    pc->exactByteSize = ComputeExactXboxTextureBytes(parentTex);
                    InsertTexInterval(pc);
                }
                if (shapeMismatch && !pc->isVolume && g_d3dCtx) {
                    ID3D11Texture2D* oldTex = (pc->gpuDirty && pc->tex2D) ? pc->tex2D : nullptr;
                    if (oldTex) oldTex->AddRef();
                    pc->maxReadableBytes = GetReadableBytesAt(reinterpret_cast<const void*>(parentTex->Data));
                    UploadXboxTextureToD3D11(*pc, parentTex);
                    if (oldTex && pc->tex2D) {
                        g_d3dCtx->CopySubresourceRegion(pc->tex2D, 0u, 0u, 0u, 0u, oldTex, 0u, nullptr);
                        pc->gpuDirty           = true;
                        pc->gpuDirtySubresMask = 1ULL;
                    }
                    if (oldTex) oldTex->Release();
                } else if (pc->dirty && !pc->isVolume) {
                    UploadXboxTextureToD3D11(*pc, parentTex);
                }
            } else {
                pc = new CachedTexture();
                memset(pc, 0, sizeof(*pc));
                pc->xboxTex          = parentTex;
                pc->dataPtr          = parentTex->Data;
                pc->exactByteSize    = ComputeExactXboxTextureBytes(parentTex);
                pc->format           = parentTex->Format;
                pc->sizeField        = parentTex->Size;
                pc->maxReadableBytes = GetReadableBytesAt(reinterpret_cast<const void*>(parentTex->Data));
                pc->isRenderTarget   = true;
                pc->isCube           = parentIsCube;
                pc->dirty            = false;
                pc->gpuDirty         = false;
                pc->gpuDirtySubresMask = 0ULL;
                pc->width            = pw;
                pc->height           = ph;
                pc->texWidth         = pw;
                pc->texHeight        = ph;
                pc->mipLevels        = parentMips;

                if (parentIsCube || parentMips > 1u) {
                    UploadXboxTextureToD3D11(*pc, parentTex);
                    pc->isRenderTarget = true;
                    pc->dirty          = false;
                } else {
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width            = pw;
                    td.Height           = ph;
                    td.MipLevels        = 1u;
                    td.ArraySize        = 1u;
                    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1u;
                    td.Usage            = D3D11_USAGE_DEFAULT;
                    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &pc->tex2D))) {
                        g_d3dDevice->CreateShaderResourceView(pc->tex2D, nullptr, &pc->srv);
                        g_d3dDevice->CreateRenderTargetView(pc->tex2D, nullptr, &pc->rtv);
                        float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        if (g_d3dCtx && pc->rtv) g_d3dCtx->ClearRenderTargetView(pc->rtv, zero);
                    }
                }
                g_texExactMap[parentKey] = pc;
                InsertTexInterval(pc);
            }

            if (pc && pc->tex2D) {
                pc->isRenderTarget = true;
                pc->dirty          = false;
                if (!pc->isCube && pc->mipLevels <= 1u &&
                    pTex->Data == pc->dataPtr && w == pc->width && h == pc->height)
                {
                    if (!pc->rtv) {
                        g_d3dDevice->CreateRenderTargetView(pc->tex2D, nullptr, &pc->rtv);
                    }
                    if (outW) *outW = w;
                    if (outH) *outH = h;
                    if (outEntry) *outEntry = pc;
                    if (outSubresourceIdx) *outSubresourceIdx = 0u;
                    return pc->rtv;
                }
                uint32_t faceIdx = 0u, mipIdx = 0u;
                if (FindSubresourceInCachedTexture(*pc, pTex->Data, w, h, &faceIdx, &mipIdx)) {
                    ID3D11RenderTargetView* subRtv = GetOrCreateSubresourceRtv(*pc, faceIdx, mipIdx, pTex->Data, w, h);
                    if (subRtv && faceIdx == 0u && mipIdx == 0u && !pc->rtv) {
                        pc->rtv = subRtv;
                    }
                    if (outW) *outW = w;
                    if (outH) *outH = h;
                    if (outEntry) *outEntry = pc;
                    if (outSubresourceIdx) {
                        D3D11_TEXTURE2D_DESC td = {};
                        pc->tex2D->GetDesc(&td);
                        *outSubresourceIdx = D3D11CalcSubresource(mipIdx, faceIdx, td.MipLevels);
                    }
                    return subRtv;
                }
            }
        }
    }

    // 1. O(1) Exact base container match
    TexExactKey exactKey = { pTex->Data, w, h };
    auto exactIt = g_texExactMap.find(exactKey);
    if (exactIt != g_texExactMap.end() && exactIt->second) {
        CachedTexture& c = *exactIt->second;
        bool protectParentMeta  = IsNonOwningXboxSubSurface(pTex) && (c.isCube || c.mipLevels > 1u || c.xboxTex != nullptr);
        bool fmtOrSizeChanged   = (c.format != pTex->Format || c.sizeField != pTex->Size || c.xboxTex != pTex);
        if (fmtOrSizeChanged && !protectParentMeta) {
            RemoveTexInterval(&c);
            c.xboxTex = pTex;
            c.format = pTex->Format;
            c.sizeField = pTex->Size;
            c.exactByteSize = ComputeExactXboxTextureBytes(pTex);
            InsertTexInterval(&c);
        }
        if (c.dirty && !c.isVolume) {
            UploadXboxTextureToD3D11(c, c.xboxTex ? c.xboxTex : pTex);
        }
        c.isRenderTarget = true;
        c.dirty = false;
        ID3D11RenderTargetView* resolvedRtv = c.rtv;
        if (c.tex2D && (c.isCube || c.mipLevels > 1u)) {
            resolvedRtv = GetOrCreateSubresourceRtv(c, 0u, 0u, pTex->Data, w, h);
            if (!c.rtv) c.rtv = resolvedRtv;
        } else if (c.tex2D && !c.rtv) {
            g_d3dDevice->CreateRenderTargetView(c.tex2D, nullptr, &c.rtv);
            resolvedRtv = c.rtv;
        }
        if (outW) *outW = w;
        if (outH) *outH = h;
        if (outEntry) *outEntry = &c;
        if (outSubresourceIdx) *outSubresourceIdx = 0u;
        return resolvedRtv;
    }

    // 2. O(log N) Subresource match (cubemap face or mip level > 0 inside an existing CachedTexture)
    {
        uint32_t qStart = pTex->Data;
        uint32_t qEnd   = (qStart == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (qStart + 1u);
        auto it = std::lower_bound(
            g_texIntervals.begin(), g_texIntervals.end(), qEnd,
            [](const TexIntervalNode& n, uint32_t val) { return n.start < val; });
        size_t idx = static_cast<size_t>(it - g_texIntervals.begin());
        while (idx > 0u) {
            --idx;
            const TexIntervalNode& n = g_texIntervals[idx];
            if (n.maxEndPrefix <= qStart) break;
            if (n.end > qStart && n.tex) {
                CachedTexture& c = *n.tex;
                uint32_t faceIdx = 0u, mipIdx = 0u;
                if (FindSubresourceInCachedTexture(c, pTex->Data, w, h, &faceIdx, &mipIdx)) {
                    if (c.dirty && c.xboxTex && !c.isVolume) {
                        UploadXboxTextureToD3D11(c, c.xboxTex);
                    }
                    c.isRenderTarget = true;
                    c.dirty = false;
                    ID3D11RenderTargetView* subRtv = GetOrCreateSubresourceRtv(c, faceIdx, mipIdx, pTex->Data, w, h);
                    if (outW) *outW = w;
                    if (outH) *outH = h;
                    if (outEntry) *outEntry = &c;
                    if (outSubresourceIdx && c.tex2D) {
                        D3D11_TEXTURE2D_DESC td = {};
                        c.tex2D->GetDesc(&td);
                        *outSubresourceIdx = D3D11CalcSubresource(mipIdx, faceIdx, td.MipLevels);
                    }
                    return subRtv;
                }
            }
        }
    }

    // 3. Brand-new render target container (2D or Cubemap)
    bool isCubeMap = isSwizzled && ((pTex->Format & 0x00000004u) != 0u);
    CachedTexture* added = new CachedTexture();
    memset(added, 0, sizeof(*added));
    added->xboxTex = IsNonOwningXboxSubSurface(pTex) ? nullptr : pTex;
    added->dataPtr = pTex->Data;
    added->exactByteSize = ComputeExactXboxTextureBytes(pTex);
    added->format = pTex->Format;
    added->sizeField = pTex->Size;
    added->maxReadableBytes = GetReadableBytesAt(reinterpret_cast<const void*>(pTex->Data));
    added->isRenderTarget = true;
    added->isCube = isCubeMap;
    added->dirty = false;
    added->gpuDirty = false;
    added->gpuDirtySubresMask = 0ULL;
    added->width = w;
    added->height = h;
    added->texWidth = w;
    added->texHeight = h;
    added->mipLevels = isSwizzled ? levels : 1u;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = added->mipLevels;
    td.ArraySize = isCubeMap ? 6u : 1u;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = isCubeMap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;

    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &added->tex2D))) {
        if (isCubeMap) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                      = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels       = added->mipLevels;
            g_d3dDevice->CreateShaderResourceView(added->tex2D, &srvDesc, &added->srv);
        } else {
            g_d3dDevice->CreateShaderResourceView(added->tex2D, nullptr, &added->srv);
            g_d3dDevice->CreateRenderTargetView(added->tex2D, nullptr, &added->rtv);
            float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (g_d3dCtx && added->rtv) g_d3dCtx->ClearRenderTargetView(added->rtv, zero);
        }
    }
    g_texExactMap[exactKey] = added;
    InsertTexInterval(added);
    if (isCubeMap && added->tex2D) {
        added->rtv = GetOrCreateSubresourceRtv(*added, 0u, 0u, pTex->Data, w, h);
    }
    if (outW) *outW = w;
    if (outH) *outH = h;
    if (outEntry) *outEntry = added;
    if (outSubresourceIdx) *outSubresourceIdx = 0u;
    return added->rtv;
}

static CachedTexture* GetOrCreateRenderTargetEntry(XboxPixelContainer* pTex, uint32_t* outSubIdx = nullptr) {
    CachedTexture* entry = nullptr;
    ResolveRenderTargetView(pTex, nullptr, nullptr, &entry, outSubIdx);
    return entry;
}

ID3D11ShaderResourceView* GetOrCreateD3D11SRV(
    XboxPixelContainer* pTex, float* outUScale, float* outVScale, bool* outIsCube, bool* outIsVolume, bool* outIsDepth)
{
    if (outUScale)   *outUScale   = 1.0f;
    if (outVScale)   *outVScale   = 1.0f;
    if (outIsCube)   *outIsCube   = false;
    if (outIsVolume) *outIsVolume = false;
    if (outIsDepth)  *outIsDepth  = false;
    if (!pTex || !pTex->Data || static_cast<uintptr_t>(pTex->Data) < kMinValidHostAddress) return nullptr;
    struct SrvResolvePerfScope {
        int64_t t0;
        ~SrvResolvePerfScope() {
            g_perfAccum.srvResolveTicks += (PerfQpcNow() - t0);
            ++g_perfAccum.srvResolveCalls;
        }
    } srvPerfScope = { PerfQpcNow() };
    if (g_d3dDevice && g_texRenderThreadId == 0u) g_texRenderThreadId = GetCurrentThreadId();
    DrainPendingTextureEvictions();

    // Check if pTex is the Xbox main backbuffer (bound as a source texture by bloom/blur/refraction passes)
    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
    if (pTex == mainBB0 || pTex == mainBB1 || pTex == mainBB2 || pTex == mainBB3 ||
        (mainBB0 && pTex->Data == mainBB0->Data))
    {
        if (outUScale) *outUScale = 1.0f / 640.0f;
        if (outVScale) *outVScale = 1.0f / 480.0f;
        if (g_d3dDevice && g_d3dCtx && g_backBufferTex) {
            if (!g_backBufferCopyTex) {
                D3D11_TEXTURE2D_DESC td = {};
                g_backBufferTex->GetDesc(&td);
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.CPUAccessFlags = 0;
                if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_backBufferCopyTex))) {
                    g_d3dDevice->CreateShaderResourceView(g_backBufferCopyTex, nullptr, &g_backBufferCopySRV);
                }
            }
            if (g_backBufferCopyTex && g_backBufferDirty) {
                g_d3dCtx->CopyResource(g_backBufferCopyTex, g_backBufferTex);
                ++g_diagBackBufferCopyCount;
                g_backBufferDirty = false;
            }
            return g_backBufferCopySRV;
        }
    }

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return nullptr;
    if (!isSwizzled) {
        if (outUScale) *outUScale = 1.0f / (float)w;
        if (outVScale) *outVScale = 1.0f / (float)h;
    }

    // Check if pTex aliases the Xbox main depth/stencil buffer (e.g. DOF / shadow / depth post-pass)
    XboxPixelContainer* mainZS = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A20);
    if (pTex == mainZS || (mainZS && pTex->Data == mainZS->Data)) {
        ID3D11ShaderResourceView* dsSrv = (g_curActiveDSV == g_dsv) ? nullptr : g_depthSRV;
        if (dsSrv && outIsDepth) *outIsDepth = true;
        return dsSrv;
    }

    // Check if pTex aliases an offscreen depth/stencil surface
    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        if (g_offscreenDSVs[i].dataPtr == pTex->Data &&
            g_offscreenDSVs[i].width == w &&
            g_offscreenDSVs[i].height == h)
        {
            ID3D11ShaderResourceView* dsSrv = (g_curActiveDSV == g_offscreenDSVs[i].dsv) ? nullptr : g_offscreenDSVs[i].srv;
            if (dsSrv && outIsDepth) *outIsDepth = true;
            return dsSrv;
        }
    }

    // O(1) exact hash lookup in g_texExactMap
    TexExactKey exactKey = { pTex->Data, w, h };
    auto exactIt = g_texExactMap.find(exactKey);
    if (exactIt != g_texExactMap.end() && exactIt->second) {
        CachedTexture& c = *exactIt->second;
        if (c.isRenderTarget && !c.dirty) {
            bool wantCube = isSwizzled && ((pTex->Format & 0x00000004u) != 0u);
            if (wantCube && !c.isCube && g_d3dDevice && g_d3dCtx) {
                ID3D11Texture2D* oldFace0Tex = (c.gpuDirty && c.tex2D) ? c.tex2D : nullptr;
                if (oldFace0Tex) oldFace0Tex->AddRef();
                RemoveTexInterval(&c);
                c.xboxTex          = IsNonOwningXboxSubSurface(pTex) ? c.xboxTex : pTex;
                c.format           = pTex->Format;
                c.sizeField        = pTex->Size;
                c.exactByteSize    = ComputeExactXboxTextureBytes(pTex);
                c.maxReadableBytes = GetReadableBytesAt(reinterpret_cast<const void*>(pTex->Data));
                InsertTexInterval(&c);

                UploadXboxTextureToD3D11(c, pTex);
                c.isRenderTarget = true;
                c.dirty          = false;
                if (oldFace0Tex && c.tex2D) {
                    g_d3dCtx->CopySubresourceRegion(c.tex2D, 0u, 0u, 0u, 0u, oldFace0Tex, 0u, nullptr);
                    c.gpuDirty           = true;
                    c.gpuDirtySubresMask = 1ULL;
                    c.rtv = GetOrCreateSubresourceRtv(c, 0u, 0u, pTex->Data, w, h);
                }
                if (oldFace0Tex) oldFace0Tex->Release();
            } else if (!c.xboxTex && !IsNonOwningXboxSubSurface(pTex)) {
                RemoveTexInterval(&c);
                c.xboxTex       = pTex;
                c.exactByteSize = ComputeExactXboxTextureBytes(pTex);
                InsertTexInterval(&c);
            }
            if (outIsCube)   *outIsCube   = c.isCube;
            if (outIsVolume) *outIsVolume = c.isVolume;
            bool rtvActiveOnThisTex = (g_curActiveOffscreenEntry == &c) || (c.rtv && c.rtv == g_curActiveRTV);
            if (!rtvActiveOnThisTex && g_curActiveRTV) {
                for (size_t sr = 0; sr < g_subresourceRtvs.size(); ++sr) {
                    if (g_subresourceRtvs[sr].rtv == g_curActiveRTV &&
                        g_subresourceRtvs[sr].parentTex == c.tex2D)
                    {
                        rtvActiveOnThisTex = true;
                        break;
                    }
                }
            }
            return rtvActiveOnThisTex ? nullptr : c.srv;
        }
        if (outIsCube)   *outIsCube   = c.isCube;
        if (outIsVolume) *outIsVolume = c.isVolume;
        bool fmtOrSizeChanged = (c.format != pTex->Format || c.sizeField != pTex->Size);
        if (fmtOrSizeChanged || c.dirty || c.lastCheckedFrame != g_swapFrameCount) {
            bool hdrChanged = (c.xboxTex != pTex);
            c.lastCheckedFrame = g_swapFrameCount;
            if (fmtOrSizeChanged || hdrChanged) {
                RemoveTexInterval(&c);
                c.xboxTex       = pTex;
                c.format        = pTex->Format;
                c.sizeField     = pTex->Size;
                c.exactByteSize = ComputeExactXboxTextureBytes(pTex);
                InsertTexInterval(&c);
            }
            if (fmtOrSizeChanged) {
                if (c.rtv)   { c.rtv->Release();   c.rtv   = nullptr; }
                if (c.srv)   { c.srv->Release();   c.srv   = nullptr; }
                if (c.tex2D) { ReleaseSubresourceRtvsForTex(c.tex2D); c.tex2D->Release(); c.tex2D = nullptr; }
                if (c.tex3D) { c.tex3D->Release(); c.tex3D = nullptr; }
                c.maxReadableBytes = GetReadableBytesAt(reinterpret_cast<const void*>(pTex->Data));
                c.dirty            = true;
            }
            uint64_t curHash = ComputeFastSampleHash(
                reinterpret_cast<const void*>(pTex->Data), c.exactByteSize, c.maxReadableBytes);
            if (c.dirty || c.contentHash != curHash) {
                c.contentHash = curHash;
                UploadXboxTextureToD3D11(c, pTex);
            }
        }
        if (outIsCube)   *outIsCube   = c.isCube;
        if (outIsVolume) *outIsVolume = c.isVolume;
        return c.srv;
    }

    size_t maxReadable = GetReadableBytesAt(reinterpret_cast<const void*>(pTex->Data));
    uint32_t exactBytes = ComputeExactXboxTextureBytes(pTex);
    uint64_t curHash = ComputeFastSampleHash(reinterpret_cast<const void*>(pTex->Data), exactBytes, maxReadable);

    CachedTexture* entry = new CachedTexture();
    memset(entry, 0, sizeof(*entry));
    entry->xboxTex = pTex;
    entry->dataPtr = pTex->Data;
    entry->exactByteSize = exactBytes;
    entry->format = pTex->Format;
    entry->sizeField = pTex->Size;
    entry->contentHash = curHash;
    entry->lastCheckedFrame = g_swapFrameCount;
    entry->maxReadableBytes = maxReadable;
    entry->width = w;
    entry->height = h;
    entry->dirty = true;
    UploadXboxTextureToD3D11(*entry, pTex);
    if (outIsCube)   *outIsCube   = entry->isCube;
    if (outIsVolume) *outIsVolume = entry->isVolume;
    g_texExactMap[exactKey] = entry;
    InsertTexInterval(entry);
    return entry->srv;
}

// ============================================================================
// GPU -> Xbox UMA Staging Readback & Surface Synchronization
// ============================================================================

void MarkActiveRenderTargetGpuDirty() {
    if (!g_curActiveRTV) return;
    if (g_curActiveRTV == g_rtv) {
        g_backBufferDirty    = true;
        g_backBufferCpuDirty = true;
        return;
    }

    // O(1) fast path via cached active offscreen render target pointer
    if (g_curActiveOffscreenEntry) {
        CachedTexture& c = *g_curActiveOffscreenEntry;
        c.gpuDirty       = true;
        c.isRenderTarget = true;
        c.dirty          = false;
        if (g_curActiveOffscreenSubIdx < 64u) {
            c.gpuDirtySubresMask |= (1ULL << g_curActiveOffscreenSubIdx);
        }
        return;
    }

    ID3D11Texture2D* subParentTex = nullptr;
    uint32_t         subIndex     = 0u;
    for (size_t sr = 0; sr < g_subresourceRtvs.size(); ++sr) {
        if (g_subresourceRtvs[sr].rtv == g_curActiveRTV) {
            subParentTex = g_subresourceRtvs[sr].parentTex;
            subIndex     = g_subresourceRtvs[sr].subresourceIdx;
            break;
        }
    }

    XboxPixelContainer* curRT = static_cast<XboxPixelContainer*>(g_curOffscreenRT);
    for (size_t i = 0; i < g_texIntervals.size(); ++i) {
        CachedTexture* cp = g_texIntervals[i].tex;
        if (!cp) continue;
        CachedTexture& c = *cp;
        if (c.rtv == g_curActiveRTV ||
            (subParentTex && c.tex2D == subParentTex) ||
            (curRT && c.dataPtr == curRT->Data && c.tex2D != nullptr))
        {
            uint32_t dirtySubIdx = (subParentTex && c.tex2D == subParentTex) ? subIndex : 0u;
            c.gpuDirty       = true;
            c.isRenderTarget = true;
            c.dirty          = false;
            if (dirtySubIdx < 64u) {
                c.gpuDirtySubresMask |= (1ULL << dirtySubIdx);
            }
            g_curActiveOffscreenEntry  = &c;
            g_curActiveOffscreenSubIdx = dirtySubIdx;
            break;
        }
    }
}

void MarkActiveDepthStencilGpuDirty() {
    if (!g_curActiveDSV) return;
    if (g_curActiveDSV == g_dsv) {
        g_mainDepthCpuDirty = true;
        return;
    }
    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        if (g_offscreenDSVs[i].dsv == g_curActiveDSV) {
            g_offscreenDSVs[i].gpuDirty = true;
        }
    }
}

static void EncodeBGRA8ToSingleXboxSurface(
    const uint8_t* srcBgraBytes, uint32_t srcRowPitch,
    uint32_t w, uint32_t h, uint32_t xboxFmt,
    bool isSwizzled, uint32_t linearPitch, uint8_t* dstData)
{
    if (!srcBgraBytes || !dstData || w == 0 || h == 0) return;
    if (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F) return; // DXT formats are not renderable RTs

    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
    uint32_t dstLinearPitch = isSwizzled ? (w * bpp) : (linearPitch ? linearPitch : (w * bpp));

    AlignedByteBuffer linearBuf;
    if (isSwizzled && !linearBuf.allocate((size_t)w * h * bpp)) return;
    uint8_t* linearDst = isSwizzled ? linearBuf.data() : dstData;

    size_t maxWritable = GetReadableBytesAt(dstData);
    size_t requiredBytes = isSwizzled ? ((size_t)w * h * bpp) : ((size_t)(h - 1u) * dstLinearPitch + (size_t)w * bpp);
    if (maxWritable < requiredBytes) return;

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* sRow = reinterpret_cast<const uint32_t*>(srcBgraBytes + (size_t)y * srcRowPitch);
        uint8_t* dRow = linearDst + (size_t)y * dstLinearPitch;

        if (xboxFmt == 0x06 || xboxFmt == 0x12) {
            memcpy(dRow, sRow, (size_t)w * 4u);
            continue;
        }

        for (uint32_t x = 0; x < w; ++x) {
            uint32_t argb = sRow[x];
            uint32_t a = (argb >> 24) & 0xFFu;
            uint32_t r = (argb >> 16) & 0xFFu;
            uint32_t g = (argb >> 8)  & 0xFFu;
            uint32_t b =  argb        & 0xFFu;

            switch (xboxFmt) {
                case 0x07: // Swizzled X8R8G8B8
                case 0x1E: // Linear X8R8G8B8
                    reinterpret_cast<uint32_t*>(dRow)[x] = argb | 0xFF000000u;
                    break;
                case 0x3A: // Swizzled A8B8G8R8
                case 0x3F: // Linear A8B8G8R8
                    reinterpret_cast<uint32_t*>(dRow)[x] = (a << 24) | (b << 16) | (g << 8) | r;
                    break;
                case 0x3B: // Swizzled B8G8R8A8 (0xBBGGRRAA)
                case 0x40: // Linear B8G8R8A8
                    reinterpret_cast<uint32_t*>(dRow)[x] = (b << 24) | (g << 16) | (r << 8) | a;
                    break;
                case 0x3C: // Swizzled R8G8B8A8 (0xRRGGBBAA)
                case 0x41: // Linear R8G8B8A8
                    reinterpret_cast<uint32_t*>(dRow)[x] = (r << 24) | (g << 16) | (b << 8) | a;
                    break;
                case 0x05: // Swizzled R5G6B5
                case 0x11: // Linear R5G6B5
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>(
                        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    break;
                case 0x02: // Swizzled A1R5G5B5
                case 0x10: // Linear A1R5G5B5
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>(
                        ((a >= 128u ? 1u : 0u) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                    break;
                case 0x03: // Swizzled X1R5G5B5
                case 0x1C: // Linear X1R5G5B5
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>(
                        0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                    break;
                case 0x04: // Swizzled A4R4G4B4
                case 0x1D: // Linear A4R4G4B4
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>(
                        ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                    break;
                case 0x00: // Swizzled L8
                case 0x13: // Linear L8
                    dRow[x] = static_cast<uint8_t>((r * 77u + g * 150u + b * 29u) >> 8);
                    break;
                case 0x01: // Swizzled AL8
                case 0x19: // Swizzled A8
                case 0x1B: // Linear AL8
                case 0x1F: // Linear A8
                    dRow[x] = static_cast<uint8_t>(a);
                    break;
                case 0x1A: // Swizzled A8L8
                case 0x20: { // Linear A8L8
                    uint32_t lum = (r * 77u + g * 150u + b * 29u) >> 8;
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>((a << 8) | lum);
                    break;
                }
                case 0x17: // Linear V8U8 / G8B8
                case 0x28: { // Swizzled V8U8
                    uint8_t u8 = static_cast<uint8_t>(static_cast<int32_t>(r) - 128);
                    uint8_t v8 = static_cast<uint8_t>(static_cast<int32_t>(g) - 128);
                    reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>((static_cast<uint16_t>(v8) << 8) | u8);
                    break;
                }
                default:
                    if (bpp == 4u) reinterpret_cast<uint32_t*>(dRow)[x] = argb;
                    else if (bpp == 2u) reinterpret_cast<uint16_t*>(dRow)[x] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    else dRow[x] = static_cast<uint8_t>(b);
                    break;
            }
        }
    }

    if (isSwizzled) {
        CallXGSwizzleRectSafe(linearDst, dstLinearPitch, nullptr, dstData, w, h, nullptr, bpp);
    }
}

static ID3D11Texture2D* s_readbackStagingTex = nullptr;
static uint32_t         s_readbackStagingW   = 0u;
static uint32_t         s_readbackStagingH   = 0u;

static ID3D11Texture2D* GetOrCreateReadbackStagingTex(uint32_t w, uint32_t h) {
    if (!g_d3dDevice || w == 0 || h == 0) return nullptr;
    if (s_readbackStagingTex && s_readbackStagingW == w && s_readbackStagingH == h) {
        return s_readbackStagingTex;
    }
    if (s_readbackStagingTex) {
        s_readbackStagingTex->Release();
        s_readbackStagingTex = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width              = w;
    td.Height             = h;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_STAGING;
    td.BindFlags          = 0;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &s_readbackStagingTex))) {
        s_readbackStagingW = w;
        s_readbackStagingH = h;
    } else {
        s_readbackStagingW = 0u;
        s_readbackStagingH = 0u;
    }
    return s_readbackStagingTex;
}

static ID3D11Texture2D* s_depthReadbackStagingTex = nullptr;
static uint32_t         s_depthReadbackStagingW   = 0u;
static uint32_t         s_depthReadbackStagingH   = 0u;

static ID3D11Texture2D* GetOrCreateDepthStagingTex(uint32_t w, uint32_t h) {
    if (!g_d3dDevice || w == 0 || h == 0) return nullptr;
    if (s_depthReadbackStagingTex && s_depthReadbackStagingW == w && s_depthReadbackStagingH == h) {
        return s_depthReadbackStagingTex;
    }
    if (s_depthReadbackStagingTex) {
        s_depthReadbackStagingTex->Release();
        s_depthReadbackStagingTex = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width              = w;
    td.Height             = h;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_STAGING;
    td.BindFlags          = 0;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &s_depthReadbackStagingTex))) {
        s_depthReadbackStagingW = w;
        s_depthReadbackStagingH = h;
    } else {
        s_depthReadbackStagingW = 0u;
        s_depthReadbackStagingH = 0u;
    }
    return s_depthReadbackStagingTex;
}

static void EncodeR24G8ToSingleXboxDepthSurface(
    const uint8_t* srcR24G8Bytes, uint32_t srcRowPitch,
    uint32_t w, uint32_t h, uint32_t xboxFmt,
    bool isSwizzled, uint32_t linearPitch, uint8_t* dstData)
{
    if (!srcR24G8Bytes || !dstData || w == 0 || h == 0) return;

    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
    uint32_t dstLinearPitch = isSwizzled ? (w * bpp) : (linearPitch ? linearPitch : (w * bpp));

    AlignedByteBuffer linearBuf;
    if (isSwizzled && !linearBuf.allocate((size_t)w * h * bpp)) return;
    uint8_t* linearDst = isSwizzled ? linearBuf.data() : dstData;

    size_t maxWritable = GetReadableBytesAt(dstData);
    size_t requiredBytes = isSwizzled ? ((size_t)w * h * bpp) : ((size_t)(h - 1u) * dstLinearPitch + (size_t)w * bpp);
    if (maxWritable < requiredBytes) return;

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* sRow = reinterpret_cast<const uint32_t*>(srcR24G8Bytes + (size_t)y * srcRowPitch);
        uint8_t* dRow = linearDst + (size_t)y * dstLinearPitch;
        if (bpp == 2u) {
            uint16_t* d16 = reinterpret_cast<uint16_t*>(dRow);
            for (uint32_t x = 0; x < w; ++x) {
                d16[x] = static_cast<uint16_t>((sRow[x] & 0x00FFFFFFu) >> 8);
            }
        } else {
            uint32_t* d32 = reinterpret_cast<uint32_t*>(dRow);
            for (uint32_t x = 0; x < w; ++x) {
                // DXGI R24G8_TYPELESS: [23:0]=Z24, [31:24]=S8 -> Xbox D24S8: [31:8]=Z24, [7:0]=S8
                uint32_t dxgiWord = sRow[x];
                d32[x] = ((dxgiWord & 0x00FFFFFFu) << 8) | ((dxgiWord >> 24) & 0xFFu);
            }
        }
    }

    if (isSwizzled) {
        CallXGSwizzleRectSafe(linearDst, dstLinearPitch, nullptr, dstData, w, h, nullptr, bpp);
    }
}

static void FlushRemainingGpuDirtySubresourcesToUma(CachedTexture& c, uint32_t discardSubIdx) {
    if (discardSubIdx < 64u) {
        c.gpuDirtySubresMask &= ~(1ULL << discardSubIdx);
    }
    if (!c.gpuDirty || c.gpuDirtySubresMask == 0ULL || !c.tex2D || c.isVolume || !g_d3dDevice || !g_d3dCtx) {
        c.gpuDirty = false;
        c.gpuDirtySubresMask = 0ULL;
        return;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    c.tex2D->GetDesc(&srcDesc);

    uint32_t cXboxFmt     = (c.format >> 8u) & 0xFFu;
    bool     cIsSwizzled  = (c.sizeField == 0u);
    uint32_t cLinearPitch = cIsSwizzled ? 0u : ((((c.sizeField >> 24u) & 0xFFu) + 1u) * 64u);
    uint32_t bpp          = GetXboxFormatBytesPerPixel(cXboxFmt);
    uint32_t bitsPerPixel = bpp * 8u;
    uint32_t baseLogW     = (c.format >> 20u) & 0x0Fu;
    uint32_t baseLogH     = (c.format >> 24u) & 0x0Fu;
    uint32_t numFaces     = c.isCube ? 6u : 1u;
    if (numFaces > srcDesc.ArraySize) numFaces = srcDesc.ArraySize;
    uint32_t numMips      = (c.mipLevels <= srcDesc.MipLevels) ? c.mipLevels : srcDesc.MipLevels;
    size_t   faceStride   = c.isCube
        ? ComputeXboxCubemapFaceStride(c.format, cXboxFmt, c.mipLevels, bitsPerPixel)
        : 0u;

    for (uint32_t face = 0u; face < numFaces; ++face) {
        size_t faceBaseOff = static_cast<size_t>(face) * faceStride;
        size_t mipOff      = 0u;
        uint32_t logW = baseLogW, logH = baseLogH;
        for (uint32_t m = 0u; m < numMips; ++m) {
            uint32_t mipW   = (c.width >> m)  ? (c.width >> m)  : 1u;
            uint32_t mipH   = (c.height >> m) ? (c.height >> m) : 1u;
            uint32_t subIdx = D3D11CalcSubresource(m, face, srcDesc.MipLevels);
            if (subIdx < 64u && ((c.gpuDirtySubresMask & (1ULL << subIdx)) != 0ULL)) {
                ID3D11Texture2D* staging = GetOrCreateReadbackStagingTex(mipW, mipH);
                if (staging) {
                    const int64_t tRbk0 = PerfQpcNow();
                    g_d3dCtx->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, c.tex2D, subIdx, nullptr);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(g_d3dCtx->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
                        ++g_diagSurfaceGpuReadbackCount;
                        uint8_t* dstPtr = reinterpret_cast<uint8_t*>(c.dataPtr) + faceBaseOff + mipOff;
                        EncodeBGRA8ToSingleXboxSurface(
                            static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                            mipW, mipH, cXboxFmt, cIsSwizzled,
                            (m == 0u && face == 0u) ? cLinearPitch : 0u, dstPtr);
                        g_d3dCtx->Unmap(staging, 0u);
                        c.gpuDirtySubresMask &= ~(1ULL << subIdx);
                    }
                    g_perfAccum.gpuReadbackTicks += (PerfQpcNow() - tRbk0);
                }
            }
            mipOff += cIsSwizzled
                ? ComputeXboxMipByteSize(logW, logH, 0u, cXboxFmt, bitsPerPixel)
                : (static_cast<size_t>(mipW) * static_cast<size_t>(mipH) * static_cast<size_t>(bpp));
            if (logW > 0u) --logW;
            if (logH > 0u) --logH;
        }
    }
    c.gpuDirty = false;
    c.gpuDirtySubresMask = 0ULL;
    c.contentHash = ComputeFastSampleHash(
        reinterpret_cast<const void*>(c.dataPtr), c.exactByteSize, c.maxReadableBytes);
}

static void ResolveSubresourceSpanAndEntry(
    XboxPixelContainer* pContainer, uint32_t level,
    uint32_t* outSubDataPtr, uint32_t* outSubByteLen,
    CachedTexture** outMatchedTex, uint32_t* outSubIdx)
{
    if (outSubDataPtr) *outSubDataPtr = pContainer ? pContainer->Data : 0u;
    if (outSubByteLen) *outSubByteLen = 1u;
    if (outMatchedTex) *outMatchedTex = nullptr;
    if (outSubIdx)     *outSubIdx     = 0xFFFFFFFFu;
    if (!pContainer || !pContainer->Data || static_cast<uintptr_t>(pContainer->Data) < kMinValidHostAddress) {
        return;
    }

    uint32_t w = 0u, h = 0u, levels = 1u, xboxFmt = 0u, linearPitch = 0u;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pContainer, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0u || h == 0u || w > 4096u || h > 4096u) return;

    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
    uint32_t bitsPerPixel = isDxt ? ((xboxFmt == 0x0Cu) ? 4u : 8u) : (bpp * 8u);

    uint32_t subDataPtr = pContainer->Data;
    uint32_t subByteLen = ComputeExactXboxTextureBytes(pContainer);
    if (level > 0u && isSwizzled) {
        uint32_t logW = (pContainer->Format >> 20u) & 0x0Fu;
        uint32_t logH = (pContainer->Format >> 24u) & 0x0Fu;
        bool isVol = (((pContainer->Format >> 4u) & 0x0Fu) == 3u);
        uint32_t logD = isVol ? ((pContainer->Format >> 28u) & 0x0Fu) : 0u;
        size_t mipOff = 0u;
        for (uint32_t m = 0u; m < level; ++m) {
            mipOff += ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
            if (logW > 0u) --logW;
            if (logH > 0u) --logH;
            if (logD > 0u) --logD;
        }
        size_t curMipBytes = ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
        subDataPtr = pContainer->Data + static_cast<uint32_t>(mipOff);
        subByteLen = (curMipBytes > 0u) ? static_cast<uint32_t>(curMipBytes) : 1u;
    } else if (level == 0u && isSwizzled && ((pContainer->Format & 0x00000004u) == 0u) && levels > 1u) {
        uint32_t logW = (pContainer->Format >> 20u) & 0x0Fu;
        uint32_t logH = (pContainer->Format >> 24u) & 0x0Fu;
        bool isVol = (((pContainer->Format >> 4u) & 0x0Fu) == 3u);
        uint32_t logD = isVol ? ((pContainer->Format >> 28u) & 0x0Fu) : 0u;
        size_t mip0Bytes = ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
        if (mip0Bytes > 0u) subByteLen = static_cast<uint32_t>(mip0Bytes);
    }
    if (outSubDataPtr) *outSubDataPtr = subDataPtr;
    if (outSubByteLen) *outSubByteLen = subByteLen ? subByteLen : 1u;

    if (!outMatchedTex && !outSubIdx) return;

    auto exactIt = g_texExactMap.find({ pContainer->Data, w, h });
    if (exactIt != g_texExactMap.end() && exactIt->second && exactIt->second->tex2D) {
        CachedTexture& c = *exactIt->second;
        D3D11_TEXTURE2D_DESC td = {};
        c.tex2D->GetDesc(&td);
        if (level < td.MipLevels && !c.isCube) {
            if (outMatchedTex) *outMatchedTex = &c;
            if (outSubIdx)     *outSubIdx     = D3D11CalcSubresource(level, 0u, td.MipLevels);
            return;
        } else if (level == 0u && c.isCube && IsNonOwningXboxSubSurface(pContainer)) {
            if (outMatchedTex) *outMatchedTex = &c;
            if (outSubIdx)     *outSubIdx     = D3D11CalcSubresource(0u, 0u, td.MipLevels);
            return;
        }
    }
    if (level == 0u) {
        uint32_t qStart = pContainer->Data;
        uint32_t qEnd   = (qStart == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (qStart + 1u);
        auto it = std::lower_bound(
            g_texIntervals.begin(), g_texIntervals.end(), qEnd,
            [](const TexIntervalNode& n, uint32_t val) { return n.start < val; });
        size_t idx = static_cast<size_t>(it - g_texIntervals.begin());
        while (idx > 0u) {
            --idx;
            const TexIntervalNode& n = g_texIntervals[idx];
            if (n.maxEndPrefix <= qStart) break;
            if (n.end > qStart && n.tex && n.tex->tex2D) {
                uint32_t faceIdx = 0u, mipIdx = 0u;
                if (FindSubresourceInCachedTexture(*n.tex, pContainer->Data, w, h, &faceIdx, &mipIdx)) {
                    D3D11_TEXTURE2D_DESC td = {};
                    n.tex->tex2D->GetDesc(&td);
                    if (mipIdx < td.MipLevels && faceIdx < td.ArraySize) {
                        if (outMatchedTex) *outMatchedTex = n.tex;
                        if (outSubIdx)     *outSubIdx     = D3D11CalcSubresource(mipIdx, faceIdx, td.MipLevels);
                        return;
                    }
                }
            }
        }
    }
}

void SyncSurfaceFromGpuIfNeeded(XboxPixelContainer* pSurf, uint32_t level) {
    if (!g_d3dDevice || !g_d3dCtx || !pSurf || !pSurf->Data ||
        static_cast<uintptr_t>(pSurf->Data) < kMinValidHostAddress)
    {
        return;
    }
    DrainPendingTextureEvictions();

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pSurf, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0) return;

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
    XboxPixelContainer* mainZS  = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A20);
    bool isMainBB = (pSurf == mainBB0 || pSurf == mainBB1 || pSurf == mainBB2 || pSurf == mainBB3 ||
                     (mainBB0 && pSurf->Data == mainBB0->Data) ||
                     (mainBB1 && pSurf->Data == mainBB1->Data));

    if (isMainBB) {
        if (!g_backBufferCpuDirty || !g_backBufferTex || level != 0u) return;
        ID3D11Texture2D* staging = GetOrCreateReadbackStagingTex(640u, 480u);
        if (!staging) return;
        const int64_t tRbk0 = PerfQpcNow();
        g_d3dCtx->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, g_backBufferTex, 0u, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(g_d3dCtx->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
            ++g_diagSurfaceGpuReadbackCount;
            EncodeBGRA8ToSingleXboxSurface(
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                w, h, xboxFmt, isSwizzled, linearPitch,
                reinterpret_cast<uint8_t*>(pSurf->Data));
            g_d3dCtx->Unmap(staging, 0u);
            g_backBufferCpuDirty = false;
        }
        g_perfAccum.gpuReadbackTicks += (PerfQpcNow() - tRbk0);
        return;
    }

    // Check offscreen depth-stencil surfaces first (includes dimension-matched DSVs aliasing mainZS->Data)
    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        OffscreenDSVEntry& ds = g_offscreenDSVs[i];
        if (!ds.gpuDirty || !ds.tex || level != 0u) continue;
        if (ds.dataPtr == pSurf->Data && ds.width == w && ds.height == h) {
            ID3D11Texture2D* staging = GetOrCreateDepthStagingTex(ds.width, ds.height);
            if (!staging) return;
            const int64_t tRbk0 = PerfQpcNow();
            g_d3dCtx->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, ds.tex, 0u, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(g_d3dCtx->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
                ++g_diagSurfaceGpuReadbackCount;
                EncodeR24G8ToSingleXboxDepthSurface(
                    static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                    w, h, xboxFmt, isSwizzled, linearPitch,
                    reinterpret_cast<uint8_t*>(pSurf->Data));
                g_d3dCtx->Unmap(staging, 0u);
                ds.gpuDirty = false;
            }
            g_perfAccum.gpuReadbackTicks += (PerfQpcNow() - tRbk0);
            return;
        }
    }

    // Check main depth-stencil buffer (640x480 g_depthTex)
    if (pSurf == mainZS || (mainZS && pSurf->Data == mainZS->Data)) {
        if (!g_mainDepthCpuDirty || !g_depthTex || level != 0u) return;
        ID3D11Texture2D* staging = GetOrCreateDepthStagingTex(640u, 480u);
        if (!staging) return;
        const int64_t tRbk0 = PerfQpcNow();
        g_d3dCtx->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, g_depthTex, 0u, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(g_d3dCtx->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
            ++g_diagSurfaceGpuReadbackCount;
            uint32_t copyW = (w <= 640u) ? w : 640u;
            uint32_t copyH = (h <= 480u) ? h : 480u;
            EncodeR24G8ToSingleXboxDepthSurface(
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                copyW, copyH, xboxFmt, isSwizzled, linearPitch,
                reinterpret_cast<uint8_t*>(pSurf->Data));
            g_d3dCtx->Unmap(staging, 0u);
            g_mainDepthCpuDirty = false;
        }
        g_perfAccum.gpuReadbackTicks += (PerfQpcNow() - tRbk0);
        return;
    }

    CachedTexture* matchedTex = nullptr;
    uint32_t       faceIdx    = 0u;
    uint32_t       mipIdx     = level;
    uint8_t*       dstPtr     = reinterpret_cast<uint8_t*>(pSurf->Data);
    bool           exactBase  = false;

    // 1. O(1) exact base lookup
    auto exactIt = g_texExactMap.find({ pSurf->Data, w, h });
    if (exactIt != g_texExactMap.end() && exactIt->second && exactIt->second->gpuDirty && exactIt->second->tex2D) {
        matchedTex = exactIt->second;
        exactBase  = true;
        if (level > 0u) {
            uint32_t bpp = GetXboxFormatBytesPerPixel(xboxFmt);
            uint32_t logW = (pSurf->Format >> 20) & 0x0Fu;
            uint32_t logH = (pSurf->Format >> 24) & 0x0Fu;
            size_t mipOff = 0u;
            for (uint32_t m = 0u; m < level; ++m) {
                mipOff += isSwizzled
                    ? ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bpp * 8u)
                    : ((size_t)((w >> m) ? (w >> m) : 1u) * ((h >> m) ? (h >> m) : 1u) * bpp);
                if (logW > 0u) --logW;
                if (logH > 0u) --logH;
            }
            dstPtr += mipOff;
        }
    } else if (level == 0u) {
        // 2. O(log N) subresource interval lookup
        uint32_t qStart = pSurf->Data;
        uint32_t qEnd   = (qStart == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (qStart + 1u);
        auto it = std::lower_bound(
            g_texIntervals.begin(), g_texIntervals.end(), qEnd,
            [](const TexIntervalNode& n, uint32_t val) { return n.start < val; });
        size_t idx = static_cast<size_t>(it - g_texIntervals.begin());
        while (idx > 0u) {
            --idx;
            const TexIntervalNode& n = g_texIntervals[idx];
            if (n.maxEndPrefix <= qStart) break;
            if (n.end > qStart && n.tex && n.tex->gpuDirty && n.tex->tex2D) {
                if (FindSubresourceInCachedTexture(*n.tex, pSurf->Data, w, h, &faceIdx, &mipIdx)) {
                    matchedTex = n.tex;
                    break;
                }
            }
        }
    }

    if (matchedTex) {
        CachedTexture& c = *matchedTex;
        D3D11_TEXTURE2D_DESC srcDesc = {};
        c.tex2D->GetDesc(&srcDesc);
        if (mipIdx >= srcDesc.MipLevels || faceIdx >= srcDesc.ArraySize) return;

        uint32_t subIdx = D3D11CalcSubresource(mipIdx, faceIdx, srcDesc.MipLevels);
        if (c.gpuDirtySubresMask != 0ULL && subIdx < 64u && ((c.gpuDirtySubresMask & (1ULL << subIdx)) == 0ULL)) {
            return;
        }

        uint32_t mipW = exactBase ? ((w >> level) ? (w >> level) : 1u) : w;
        uint32_t mipH = exactBase ? ((h >> level) ? (h >> level) : 1u) : h;

        ID3D11Texture2D* staging = GetOrCreateReadbackStagingTex(mipW, mipH);
        if (!staging) return;
        const int64_t tRbk0 = PerfQpcNow();
        g_d3dCtx->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, c.tex2D, subIdx, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(g_d3dCtx->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
            ++g_diagSurfaceGpuReadbackCount;
            EncodeBGRA8ToSingleXboxSurface(
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                mipW, mipH, xboxFmt, isSwizzled, (mipIdx == 0u && faceIdx == 0u) ? linearPitch : 0u, dstPtr);
            g_d3dCtx->Unmap(staging, 0u);
            if (subIdx < 64u) {
                c.gpuDirtySubresMask &= ~(1ULL << subIdx);
            }
            if (c.gpuDirtySubresMask == 0ULL) {
                c.gpuDirty = false;
            }
            c.contentHash = ComputeFastSampleHash(
                reinterpret_cast<const void*>(c.dataPtr), c.exactByteSize, c.maxReadableBytes);
        }
        g_perfAccum.gpuReadbackTicks += (PerfQpcNow() - tRbk0);
    }
}

// ============================================================================
// SetTexture, SetRenderTarget, GetRenderTarget2, LockRect, LockBox & CopyRects
// ============================================================================

static void __stdcall Bridge_D3DDevice_SetTexture(DWORD Stage, XboxPixelContainer* pTexture) {
    if (Stage < 4) {
        g_boundTextures[Stage] = pTexture;
        *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x0F88 + Stage * 4) = pTexture;
    }
}

static void ApplyRenderTargetInternal(
    XboxPixelContainer* pRenderTarget, XboxPixelContainer* pZStencil, bool resetViewport)
{
    ++g_perfAccum.rtSwitchCount;
    typedef void     (__stdcall *PFN_InternalAddRefSurface)(XboxPixelContainer*);
    typedef void     (__stdcall *PFN_InternalReleaseSurface)(XboxPixelContainer*);
    typedef uint32_t (__stdcall *PFN_GetPitch)(XboxPixelContainer*);
    typedef uint32_t (__stdcall *PFN_GetSurfaceFormat)(XboxPixelContainer*, XboxPixelContainer*);
    typedef void     (__stdcall *PFN_GetDepthBufferScale)(XboxPixelContainer*, float*);
    typedef uint32_t (__stdcall *PFN_D3DResource_AddRef)(XboxPixelContainer*);

    PFN_InternalAddRefSurface  fnAddRefSurf  = reinterpret_cast<PFN_InternalAddRefSurface>(0x006B42F0u);
    PFN_InternalReleaseSurface fnRelSurf     = reinterpret_cast<PFN_InternalReleaseSurface>(0x006B4480u);
    PFN_GetPitch               fnGetPitch    = reinterpret_cast<PFN_GetPitch>(0x006B0710u);
    PFN_GetSurfaceFormat       fnGetSurfFmt  = reinterpret_cast<PFN_GetSurfaceFormat>(0x006B6B10u);
    PFN_GetDepthBufferScale    fnGetZsScale  = reinterpret_cast<PFN_GetDepthBufferScale>(0x006B6C30u);
    (void)sizeof(PFN_D3DResource_AddRef);

    uint8_t* pDev = *reinterpret_cast<uint8_t**>(ADDR_D3D_PDEVICE);
    if (!pDev) pDev = reinterpret_cast<uint8_t*>(ADDR_D3D_DEVICE);

    // Match _D3DDevice_SetRenderTarget@8 (0x006B08D0): NULL pRenderTarget keeps current [CDevice + 0x1A04].
    if (!pRenderTarget) {
        pRenderTarget = *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A04);
    }

    // Update Xbox CDevice surface refcounts & cached fields (+0x1A04, +0x1A08, +0x1A0C, +0x948, +0x94C)
    if (pRenderTarget) {
        fnAddRefSurf(pRenderTarget);
    }
    XboxPixelContainer* oldRT = *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A04);
    if (oldRT) {
        fnRelSurf(oldRT);
    }
    *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A04) = pRenderTarget;

    // AddRef new pZStencil BEFORE releasing oldZS to avoid UAF when pZStencil == oldZS and RefCount == 1!
    XboxPixelContainer* oldZS = *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A08);
    if (pZStencil) {
        fnAddRefSurf(pZStencil);
    }
    if (oldZS) {
        fnRelSurf(oldZS);
    }
    *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A08) = pZStencil;

    if (pZStencil) {
        fnGetZsScale(pZStencil, reinterpret_cast<float*>(pDev + 0x0948));
        *reinterpret_cast<uint32_t*>(pDev + 0x094C) = fnGetPitch(pZStencil);
    }
    if (resetViewport && pRenderTarget) {
        *reinterpret_cast<uint32_t*>(pDev + 0x1A0C) = fnGetSurfFmt(pRenderTarget, pZStencil);
    }

    if (!g_d3dCtx) return;

    // Unbind PS shader resources (2D t0..t3, Cube t4..t7, Volume t8..t11, Depth t12) before switching RTV/DSV
    ID3D11ShaderResourceView* nullSrvs[13] = {};
    g_d3dCtx->PSSetShaderResources(0, 13, nullSrvs);
    InvalidateD3D11ShadowState();

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
    XboxPixelContainer* mainZS  = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A20);

    bool isNullRT = (pRenderTarget == nullptr);
    bool isMainBB = (!isNullRT) &&
                    ((pRenderTarget == mainBB0) || (pRenderTarget == mainBB1) ||
                     (pRenderTarget == mainBB2) || (pRenderTarget == mainBB3) ||
                     (mainBB0 && pRenderTarget->Data == mainBB0->Data) ||
                     (mainBB1 && pRenderTarget->Data == mainBB1->Data));

    uint32_t rtW = 640u, rtH = 480u;
    ID3D11RenderTargetView* targetRTV = nullptr;
    CachedTexture* offscreenRtEntry  = nullptr;
    uint32_t       offscreenSubIdx   = 0u;
    if (isNullRT) {
        g_curOffscreenRT = nullptr;
        targetRTV        = nullptr;
    } else if (isMainBB) {
        g_curOffscreenRT = nullptr;
        targetRTV        = g_rtv;
    } else {
        targetRTV = ResolveRenderTargetView(pRenderTarget, &rtW, &rtH, &offscreenRtEntry, &offscreenSubIdx);
        if (targetRTV) {
            g_curOffscreenRT = pRenderTarget;
        }
    }

    // Resolve caller-supplied pZStencil:
    // - NULL explicitly disables depth/stencil testing & writing!
    // - When paired with a color RTV, ensure DSV dimensions match (rtW, rtH) so D3D11 OMSetRenderTargets never rejects the pair!
    ID3D11DepthStencilView* targetDSV = nullptr;
    if (pZStencil) {
        bool isMainZS = (pZStencil == mainZS) || (mainZS && pZStencil->Data == mainZS->Data);
        if (isMainZS && (!targetRTV || (rtW == 640u && rtH == 480u))) {
            targetDSV        = g_dsv;
            g_curOffscreenZS = nullptr;
            if (!targetRTV) {
                rtW = 640u;
                rtH = 480u;
            }
        } else {
            OffscreenDSVEntry* dsEntry = targetRTV
                ? GetOrCreateDepthStencilEntryWithSize(pZStencil, rtW, rtH)
                : GetOrCreateDepthStencilEntry(pZStencil);
            targetDSV        = dsEntry ? dsEntry->dsv : nullptr;
            g_curOffscreenZS = pZStencil;
            if (!targetRTV && dsEntry) {
                rtW = dsEntry->width;
                rtH = dsEntry->height;
            }
        }
    } else {
        targetDSV        = nullptr;
        g_curOffscreenZS = nullptr;
    }

    if (targetRTV || isNullRT) {
        g_curActiveRTV             = targetRTV;
        g_curActiveDSV             = targetDSV;
        g_curActiveOffscreenEntry  = offscreenRtEntry;
        g_curActiveOffscreenSubIdx = offscreenSubIdx;
        if (targetRTV) {
            g_d3dCtx->OMSetRenderTargets(1, &g_curActiveRTV, g_curActiveDSV);
        } else {
            g_d3dCtx->OMSetRenderTargets(0, nullptr, g_curActiveDSV);
        }
        InvalidateD3D11ShadowState();
    }

    if (resetViewport) {
        XboxViewport vp = { 0u, 0u, rtW, rtH, 0.0f, 1.0f };
        Bridge_D3DDevice_SetViewport(&vp);
    }
}

void __stdcall Bridge_D3DDevice_SetRenderTarget(
    XboxPixelContainer* pRenderTarget, XboxPixelContainer* pZStencil)
{
    ApplyRenderTargetInternal(pRenderTarget, pZStencil, true);
}

void __stdcall Bridge_D3DDevice_SetRenderTargetFast(
    XboxPixelContainer* pRenderTarget, XboxPixelContainer* pZStencil, uint32_t Flags)
{
    (void)Flags;
    ApplyRenderTargetInternal(pRenderTarget, pZStencil, false);
}

static XboxPixelContainer* __stdcall Bridge_D3DDevice_GetRenderTarget2() {
    typedef uint32_t (__stdcall *PFN_D3DResource_AddRef)(XboxPixelContainer*);
    PFN_D3DResource_AddRef fnAddRef = reinterpret_cast<PFN_D3DResource_AddRef>(0x006B4330u);
    uint8_t* pDev = *reinterpret_cast<uint8_t**>(ADDR_D3D_PDEVICE);
    if (!pDev) pDev = reinterpret_cast<uint8_t*>(ADDR_D3D_DEVICE);
    XboxPixelContainer* rt = *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A04);
    if (rt) fnAddRef(rt);
    return rt;
}

static XboxPixelContainer* __stdcall Bridge_D3DDevice_GetDepthStencilSurface2() {
    typedef uint32_t (__stdcall *PFN_D3DResource_AddRef)(XboxPixelContainer*);
    PFN_D3DResource_AddRef fnAddRef = reinterpret_cast<PFN_D3DResource_AddRef>(0x006B4330u);
    uint8_t* pDev = *reinterpret_cast<uint8_t**>(ADDR_D3D_PDEVICE);
    if (!pDev) pDev = reinterpret_cast<uint8_t*>(ADDR_D3D_DEVICE);
    XboxPixelContainer* zs = *reinterpret_cast<XboxPixelContainer**>(pDev + 0x1A08);
    if (zs) fnAddRef(zs);
    return zs;
}

// Xbox XDK D3DLOCK_READONLY = 0x80; also accept Win32/test D3DLOCK_READONLY = 0x10.
static constexpr DWORD kXboxD3DLockReadOnlyMask = 0x00000080u | 0x00000010u;
static constexpr DWORD kXboxD3DLockDiscard      = 0x00002000u;

static HRESULT __stdcall Bridge_D3DTexture_LockRect(
    XboxPixelContainer* pTexture, UINT Level, XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags)
{
    if (pTexture) {
        bool isReadOnly    = (Flags & kXboxD3DLockReadOnlyMask) != 0u;
        bool isFullDiscard = (!isReadOnly) && ((Flags & kXboxD3DLockDiscard) != 0u) && (pRect == nullptr);
        if (!isFullDiscard) {
            SyncSurfaceFromGpuIfNeeded(pTexture, Level);
        }
        if (!isReadOnly) {
            uint32_t       subDataPtr = pTexture->Data;
            uint32_t       subByteLen = 1u;
            CachedTexture* discardTex = nullptr;
            uint32_t       discardSub = 0xFFFFFFFFu;
            ResolveSubresourceSpanAndEntry(
                pTexture, Level, &subDataPtr, &subByteLen,
                isFullDiscard ? &discardTex : nullptr,
                isFullDiscard ? &discardSub : nullptr);
            MarkTextureDirtyByRange(subDataPtr, subByteLen, discardTex, discardSub);
        }
        g_pfnLock2DSurface(pTexture, 0, Level, pLockedRect, pRect, Flags);
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DSurface_LockRect(
    XboxPixelContainer* pSurface, XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags)
{
    if (pSurface) {
        bool isReadOnly    = (Flags & kXboxD3DLockReadOnlyMask) != 0u;
        bool isFullDiscard = (!isReadOnly) && ((Flags & kXboxD3DLockDiscard) != 0u) && (pRect == nullptr);
        if (!isFullDiscard) {
            SyncSurfaceFromGpuIfNeeded(pSurface, 0u);
        }
        if (!isReadOnly) {
            uint32_t       subDataPtr = pSurface->Data;
            uint32_t       subByteLen = 1u;
            CachedTexture* discardTex = nullptr;
            uint32_t       discardSub = 0xFFFFFFFFu;
            ResolveSubresourceSpanAndEntry(
                pSurface, 0u, &subDataPtr, &subByteLen,
                isFullDiscard ? &discardTex : nullptr,
                isFullDiscard ? &discardSub : nullptr);
            MarkTextureDirtyByRange(subDataPtr, subByteLen, discardTex, discardSub);
        }
        g_pfnLock2DSurface(pSurface, 0, 0, pLockedRect, pRect, Flags);
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DVolumeTexture_LockBox(
    XboxPixelContainer* pVolumeTexture, UINT Level, void* pLockedVolume, const void* pBox, DWORD Flags)
{
    if (pVolumeTexture) {
        if ((Flags & kXboxD3DLockReadOnlyMask) == 0u) {
            uint32_t subDataPtr = pVolumeTexture->Data;
            uint32_t subByteLen = 1u;
            ResolveSubresourceSpanAndEntry(pVolumeTexture, Level, &subDataPtr, &subByteLen, nullptr, nullptr);
            MarkTextureDirtyByRange(subDataPtr, subByteLen);
        }
        g_pfnLock3DSurface(pVolumeTexture, Level, pLockedVolume, pBox, Flags);
    }
    return S_OK;
}

static void __stdcall Bridge_D3DDevice_CopyRects(
    XboxPixelContainer* pSourceSurface, const RECT* pSourceRectsArray,
    UINT cRects, XboxPixelContainer* pDestinationSurface, const POINT* pDestPointsArray)
{
    if (!pSourceSurface || !pDestinationSurface || !pSourceSurface->Data || !pDestinationSurface->Data) return;

    uint32_t sw = 0, sh = 0, slev = 1, sfmt = 0, spitch = 0;
    bool sswiz = false;
    DecodeXboxTextureFormat(pSourceSurface, &sw, &sh, &slev, &sfmt, &sswiz, &spitch);

    uint32_t dw = 0, dh = 0, dlev = 1, dfmt = 0, dpitch = 0;
    bool dswiz = false;
    DecodeXboxTextureFormat(pDestinationSurface, &dw, &dh, &dlev, &dfmt, &dswiz, &dpitch);

    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return;

    // Matching _D3DDevice_CopyRects@20 (0x006B0F40..0x006B1155):
    // When pSourceRectsArray == NULL or cRects == 0, default to 1 full-surface rect {0, 0, sw, sh}.
    // When pDestPointsArray == NULL, each rect's destination point defaults to {srcRect.left, srcRect.top}.
    RECT defaultRect = { 0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh) };
    const RECT* rects = (pSourceRectsArray && cRects > 0) ? pSourceRectsArray : &defaultRect;
    UINT rectCount    = (pSourceRectsArray && cRects > 0) ? cRects : 1u;

    // Check if source & destination are GPU depth-stencil surfaces or GPU color render targets
    bool srcIsDepth = IsXboxDepthStencilFormat(sfmt);
    bool dstIsDepth = IsXboxDepthStencilFormat(dfmt);

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
    XboxPixelContainer* mainZS  = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A20);

    bool gpuCopied = false;
    if (srcIsDepth && dstIsDepth && g_d3dCtx && pSourceSurface->Data != pDestinationSurface->Data) {
        ID3D11Texture2D* srcDepthTex = nullptr;
        if ((pSourceSurface == mainZS || (mainZS && pSourceSurface->Data == mainZS->Data)) && sw == 640u && sh == 480u) {
            srcDepthTex = g_depthTex;
        } else {
            for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
                if (g_offscreenDSVs[i].dataPtr == pSourceSurface->Data &&
                    g_offscreenDSVs[i].width == sw && g_offscreenDSVs[i].height == sh)
                {
                    srcDepthTex = g_offscreenDSVs[i].tex;
                    break;
                }
            }
        }
        if (srcDepthTex) {
            bool dstIsMainZS = ((pDestinationSurface == mainZS || (mainZS && pDestinationSurface->Data == mainZS->Data)) &&
                                dw == 640u && dh == 480u);
            OffscreenDSVEntry* dstDsEntry  = dstIsMainZS ? nullptr : GetOrCreateDepthStencilEntry(pDestinationSurface);
            ID3D11Texture2D*   dstDepthTex = dstIsMainZS ? g_depthTex : (dstDsEntry ? dstDsEntry->tex : nullptr);
            if (dstDepthTex) {
                bool isFullCopy = (rectCount == 1u &&
                                   rects[0].left <= 0 && rects[0].top <= 0 &&
                                   rects[0].right >= static_cast<LONG>(sw) && rects[0].bottom >= static_cast<LONG>(sh) &&
                                   (!pDestPointsArray || (pDestPointsArray[0].x == 0 && pDestPointsArray[0].y == 0)) &&
                                   sw == dw && sh == dh);
                if (isFullCopy) {
                    g_d3dCtx->CopyResource(dstDepthTex, srcDepthTex);
                } else {
                    for (UINT i = 0; i < rectCount; ++i) {
                        LONG l = rects[i].left, t = rects[i].top, r = rects[i].right, b = rects[i].bottom;
                        LONG dx = pDestPointsArray ? pDestPointsArray[i].x : l;
                        LONG dy = pDestPointsArray ? pDestPointsArray[i].y : t;
                        if (l < 0) { dx -= l; l = 0; }
                        if (t < 0) { dy -= t; t = 0; }
                        if (dx < 0) { l -= dx; dx = 0; }
                        if (dy < 0) { t -= dy; dy = 0; }
                        if (r > static_cast<LONG>(sw)) r = static_cast<LONG>(sw);
                        if (b > static_cast<LONG>(sh)) b = static_cast<LONG>(sh);
                        LONG wRect = r - l, hRect = b - t;
                        if (dx + wRect > static_cast<LONG>(dw)) wRect = static_cast<LONG>(dw) - dx;
                        if (dy + hRect > static_cast<LONG>(dh)) hRect = static_cast<LONG>(dh) - dy;
                        if (wRect <= 0 || hRect <= 0) continue;
                        D3D11_BOX srcBox = { static_cast<UINT>(l), static_cast<UINT>(t), 0u,
                                             static_cast<UINT>(l + wRect), static_cast<UINT>(t + hRect), 1u };
                        g_d3dCtx->CopySubresourceRegion(
                            dstDepthTex, 0u, static_cast<UINT>(dx), static_cast<UINT>(dy), 0u,
                            srcDepthTex, 0u, &srcBox);
                    }
                }
                if (dstIsMainZS) {
                    g_mainDepthCpuDirty = true;
                } else if (dstDsEntry) {
                    dstDsEntry->gpuDirty = true;
                }
                return;
            }
        }
    }

    ID3D11Texture2D* srcGpuTex = nullptr;
    uint32_t         srcSubIdx = 0u;
    if (!srcIsDepth && !dstIsDepth) {
        if (pSourceSurface == mainBB0 || pSourceSurface == mainBB1 ||
            pSourceSurface == mainBB2 || pSourceSurface == mainBB3 ||
            (mainBB0 && pSourceSurface->Data == mainBB0->Data) ||
            (mainBB1 && pSourceSurface->Data == mainBB1->Data))
        {
            srcGpuTex = g_backBufferTex;
            srcSubIdx = 0u;
        } else {
            CachedTexture* srcMatched = nullptr;
            uint32_t       resolvedSrcSub = 0xFFFFFFFFu;
            ResolveSubresourceSpanAndEntry(pSourceSurface, 0u, nullptr, nullptr, &srcMatched, &resolvedSrcSub);
            if (srcMatched && (srcMatched->isRenderTarget || srcMatched->tex2D) &&
                !srcMatched->dirty && resolvedSrcSub != 0xFFFFFFFFu)
            {
                srcGpuTex = srcMatched->tex2D;
                srcSubIdx = resolvedSrcSub;
            }
        }
    }

    if (srcGpuTex && g_d3dCtx && pSourceSurface->Data != pDestinationSurface->Data) {
        bool dstIsMainBB = (pDestinationSurface == mainBB0 || pDestinationSurface == mainBB1 ||
                            pDestinationSurface == mainBB2 || pDestinationSurface == mainBB3 ||
                            (mainBB0 && pDestinationSurface->Data == mainBB0->Data) ||
                            (mainBB1 && pDestinationSurface->Data == mainBB1->Data));
        ID3D11Texture2D* dstGpuTex = nullptr;
        CachedTexture*   dstEntry  = nullptr;
        uint32_t         dstSubIdx = 0u;
        if (dstIsMainBB) {
            dstGpuTex = g_backBufferTex;
            dstSubIdx = 0u;
        } else {
            dstEntry  = GetOrCreateRenderTargetEntry(pDestinationSurface, &dstSubIdx);
            dstGpuTex = dstEntry ? dstEntry->tex2D : nullptr;
        }

        if (dstGpuTex) {
            D3D11_TEXTURE2D_DESC srcTd = {}, dstTd = {};
            srcGpuTex->GetDesc(&srcTd);
            dstGpuTex->GetDesc(&dstTd);
            bool sameTexShape = (srcSubIdx == 0u && dstSubIdx == 0u &&
                                 srcTd.Width == dstTd.Width && srcTd.Height == dstTd.Height &&
                                 srcTd.MipLevels == dstTd.MipLevels && srcTd.ArraySize == dstTd.ArraySize);
            bool isFullCopy = (rectCount == 1u &&
                               rects[0].left <= 0 && rects[0].top <= 0 &&
                               rects[0].right >= static_cast<LONG>(sw) && rects[0].bottom >= static_cast<LONG>(sh) &&
                               (!pDestPointsArray || (pDestPointsArray[0].x == 0 && pDestPointsArray[0].y == 0)) &&
                               sw == dw && sh == dh);
            if (isFullCopy && sameTexShape) {
                g_d3dCtx->CopyResource(dstGpuTex, srcGpuTex);
            } else {
                for (UINT i = 0; i < rectCount; ++i) {
                    LONG l = rects[i].left;
                    LONG t = rects[i].top;
                    LONG r = rects[i].right;
                    LONG b = rects[i].bottom;
                    LONG dx = pDestPointsArray ? pDestPointsArray[i].x : l;
                    LONG dy = pDestPointsArray ? pDestPointsArray[i].y : t;

                    if (l < 0) { dx -= l; l = 0; }
                    if (t < 0) { dy -= t; t = 0; }
                    if (dx < 0) { l -= dx; dx = 0; }
                    if (dy < 0) { t -= dy; dy = 0; }
                    if (r > static_cast<LONG>(sw)) r = static_cast<LONG>(sw);
                    if (b > static_cast<LONG>(sh)) b = static_cast<LONG>(sh);

                    LONG wRect = r - l;
                    LONG hRect = b - t;
                    if (dx + wRect > static_cast<LONG>(dw)) wRect = static_cast<LONG>(dw) - dx;
                    if (dy + hRect > static_cast<LONG>(dh)) hRect = static_cast<LONG>(dh) - dy;
                    if (wRect <= 0 || hRect <= 0) continue;

                    D3D11_BOX srcBox = {};
                    srcBox.left   = static_cast<UINT>(l);
                    srcBox.top    = static_cast<UINT>(t);
                    srcBox.front  = 0u;
                    srcBox.right  = static_cast<UINT>(l + wRect);
                    srcBox.bottom = static_cast<UINT>(t + hRect);
                    srcBox.back   = 1u;
                    g_d3dCtx->CopySubresourceRegion(
                        dstGpuTex, dstSubIdx, static_cast<UINT>(dx), static_cast<UINT>(dy), 0u,
                        srcGpuTex, srcSubIdx, &srcBox);
                }
            }
            if (dstIsMainBB) {
                g_backBufferDirty    = true;
                g_backBufferCpuDirty = true;
            } else if (dstEntry) {
                dstEntry->gpuDirty       = true;
                if (dstSubIdx < 64u) {
                    dstEntry->gpuDirtySubresMask |= (1ULL << dstSubIdx);
                }
                dstEntry->isRenderTarget = true;
                dstEntry->dirty          = false;
            }
            gpuCopied = true;
        }
    }

    if (gpuCopied) {
        return;
    }

    // Ensure both source and destination UMA buffers have any pending GPU renders flushed before CPU copy
    SyncSurfaceFromGpuIfNeeded(pSourceSurface, 0u);
    bool isFullDestOverwrite = (rectCount == 1u &&
                                rects[0].left <= 0 && rects[0].top <= 0 &&
                                rects[0].right >= static_cast<LONG>(dw) && rects[0].bottom >= static_cast<LONG>(dh) &&
                                (!pDestPointsArray || (pDestPointsArray[0].x == 0 && pDestPointsArray[0].y == 0)));
    uint32_t       dstSubDataPtr = pDestinationSurface->Data;
    uint32_t       dstSubByteLen = 1u;
    CachedTexture* dstMatchedTex = nullptr;
    uint32_t       dstSubIdxCpu  = 0xFFFFFFFFu;
    ResolveSubresourceSpanAndEntry(
        pDestinationSurface, 0u, &dstSubDataPtr, &dstSubByteLen, &dstMatchedTex, &dstSubIdxCpu);
    if (!isFullDestOverwrite) {
        SyncSurfaceFromGpuIfNeeded(pDestinationSurface, 0u);
    }
    if (dstMatchedTex) {
        FlushRemainingGpuDirtySubresourcesToUma(*dstMatchedTex, isFullDestOverwrite ? dstSubIdxCpu : 0xFFFFFFFFu);
    }

    // CPU surface copy (swizzled/unswizzled/linear sub-rectangle copy)
    uint32_t bpp = GetXboxFormatBytesPerPixel(sfmt);

    uint32_t srcRowPitch = spitch ? spitch : (sw * bpp);
    uint32_t dstRowPitch = dpitch ? dpitch : (dw * bpp);
    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(pSourceSurface->Data);
    uint8_t* dstBytes       = reinterpret_cast<uint8_t*>(pDestinationSurface->Data);

    for (UINT i = 0; i < rectCount; ++i) {
        RECT r = rects[i];
        POINT pt = pDestPointsArray ? pDestPointsArray[i] : POINT{ r.left, r.top };

        if (r.left < 0) { pt.x -= r.left; r.left = 0; }
        if (r.top  < 0) { pt.y -= r.top;  r.top  = 0; }
        if (pt.x   < 0) { r.left -= pt.x; pt.x   = 0; }
        if (pt.y   < 0) { r.top  -= pt.y; pt.y   = 0; }
        if (r.right  > static_cast<LONG>(sw)) r.right  = static_cast<LONG>(sw);
        if (r.bottom > static_cast<LONG>(sh)) r.bottom = static_cast<LONG>(sh);
        if (pt.x + (r.right - r.left) > static_cast<LONG>(dw)) r.right  = r.left + (static_cast<LONG>(dw) - pt.x);
        if (pt.y + (r.bottom - r.top) > static_cast<LONG>(dh)) r.bottom = r.top  + (static_cast<LONG>(dh) - pt.y);
        if (r.right <= r.left || r.bottom <= r.top) continue;

        if (!sswiz && dswiz) {
            CallXGSwizzleRectSafe(srcBytes, srcRowPitch, &r, dstBytes, dw, dh, &pt, bpp);
        } else if (sswiz && !dswiz) {
            CallXGUnswizzleRectSafe(srcBytes, sw, sh, &r, dstBytes, dstRowPitch, &pt, bpp);
        } else if (sswiz && dswiz) {
            if (r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(sw) && r.bottom == static_cast<LONG>(sh) &&
                pt.x == 0 && pt.y == 0 && sw == dw && sh == dh && srcBytes != dstBytes)
            {
                memcpy(dstBytes, srcBytes, (size_t)sw * sh * bpp);
            } else {
                uint32_t rw = static_cast<uint32_t>(r.right - r.left);
                uint32_t rh = static_cast<uint32_t>(r.bottom - r.top);
                AlignedByteBuffer tmp((size_t)rw * rh * bpp);
                if (!tmp.data()) continue;
                POINT zeroPt = { 0, 0 };
                RECT  tmpRect = { 0, 0, static_cast<LONG>(rw), static_cast<LONG>(rh) };
                CallXGUnswizzleRectSafe(srcBytes, sw, sh, &r, tmp.data(), rw * bpp, &zeroPt, bpp);
                CallXGSwizzleRectSafe(tmp.data(), rw * bpp, &tmpRect, dstBytes, dw, dh, &pt, bpp);
            }
        } else {
            uint32_t rw = static_cast<uint32_t>(r.right - r.left);
            uint32_t rh = static_cast<uint32_t>(r.bottom - r.top);
            for (uint32_t y = 0; y < rh; ++y) {
                const uint8_t* sRow = srcBytes + (size_t)(r.top + y) * srcRowPitch + (size_t)r.left * bpp;
                uint8_t*       dRow = dstBytes + (size_t)(pt.y + y)  * dstRowPitch + (size_t)pt.x   * bpp;
                memmove(dRow, sRow, (size_t)rw * bpp);
            }
        }
    }
    MarkTextureDirtyByRange(
        dstSubDataPtr, dstSubByteLen,
        isFullDestOverwrite ? dstMatchedTex : nullptr,
        isFullDestOverwrite ? dstSubIdxCpu  : 0xFFFFFFFFu);
}

void InitXboxD3D8TextureBridges() {
    g_texRenderThreadId = GetCurrentThreadId();
    g_texExactMap.reserve(512);
    g_texByContainerMap.reserve(512);
    g_texIntervals.reserve(512);
    RegisterXboxMemoryFreeCallback(&OnXboxMemoryFreedTextureHook);

    constexpr uint32_t kAddrD3DDeviceSetRenderTarget        = 0x006B08D0u;
    constexpr uint32_t kAddrD3DDeviceSetRenderTargetFast    = 0x006B0B50u;
    constexpr uint32_t kAddrD3DDeviceGetRenderTarget2       = 0x006B1270u;
    constexpr uint32_t kAddrD3DDeviceGetDepthStencilSurf2   = 0x006B1290u;
    constexpr uint32_t kAddrD3DVolumeTextureLockBox         = 0x006B3470u;

    InstallBridge(kAddrD3DDeviceSetRenderTarget,      &Bridge_D3DDevice_SetRenderTarget);     // _D3DDevice_SetRenderTarget@8
    InstallBridge(kAddrD3DDeviceSetRenderTargetFast,  &Bridge_D3DDevice_SetRenderTargetFast); // _D3DDevice_SetRenderTargetFast@12
    InstallBridge(kAddrD3DDeviceGetRenderTarget2,     &Bridge_D3DDevice_GetRenderTarget2);    // _D3DDevice_GetRenderTarget2@0
    InstallBridge(kAddrD3DDeviceGetDepthStencilSurf2, &Bridge_D3DDevice_GetDepthStencilSurface2); // _D3DDevice_GetDepthStencilSurface2@0
    InstallBridge(0x006B1900, &Bridge_D3DDevice_SetTexture);                                  // _D3DDevice_SetTexture@8
    InstallBridge(0x006B33D0, &Bridge_D3DTexture_LockRect);                                   // _D3DTexture_LockRect@20
    InstallBridge(kAddrD3DVolumeTextureLockBox, &Bridge_D3DVolumeTexture_LockBox);            // _D3DVolumeTexture_LockBox@20
    InstallBridge(0x006B45A0, &Bridge_D3DSurface_LockRect);                                   // _D3DSurface_LockRect@16
    InstallBridge(0x006B0F40, &Bridge_D3DDevice_CopyRects);                                   // _D3DDevice_CopyRects@20
}
