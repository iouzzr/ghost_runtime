#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Vertex Declarations, Shaders, Pixel Shaders & CPU Skinning Bridges
// ============================================================================

uint32_t           g_currentVsHandle            = 0x002u;
uint32_t           g_currentPsHandle            = 0u;
int32_t            g_currentPsType              = -1; // -1 = fixed-function TSS, >=0 = programmable/combiner PS
float              g_psConstants[8][4]          = {};
float              g_psStageC0[8][4]            = {};
float              g_psStageC1[8][4]            = {};
float              g_psFcConst[2][4]            = {};
float              g_psEyeDir[4]                = { 0.0f, 0.0f, 1.0f, 0.0f };
ActiveGpuSkinSetup g_activeGpuSkin              = {};
uint64_t           g_vsConstantsDirtyGen        = 1ULL;
bool               g_boundPsCombinerDirty       = false;
uint64_t           g_diagPsPointerCacheHitCount = 0ULL;

uint64_t GetPsPointerCacheHitCount() {
    return g_diagPsPointerCacheHitCount;
}

static uint32_t g_cheopsProgramSlots[136][4] = {};

bool GetXboxVertexShaderDecl(uint32_t handle, XboxShaderDeclInfo* outDecl) {
    if (!outDecl) return false;
    if ((handle & 1u) != 0u) {
        uintptr_t ptr = static_cast<uintptr_t>(handle & ~1u);
        if (ptr >= kMinValidHostAddress) {
            const XboxVertexShaderObject* vsObj = reinterpret_cast<const XboxVertexShaderObject*>(ptr);
            *outDecl = vsObj->hostDecl;
            return true;
        }
    }
    return false;
}

void NotifyXboxPsRenderStateChanged(uint32_t rsIndex, uint32_t value) {
    if (rsIndex >= XBOX_D3DRS_PSCONSTANT0_0 && rsIndex <= XBOX_D3DRS_PSCONSTANT0_7) {
        uint32_t s = rsIndex - XBOX_D3DRS_PSCONSTANT0_0;
        UnpackD3DColor(value, &g_psStageC0[s][0], &g_psStageC0[s][1], &g_psStageC0[s][2], &g_psStageC0[s][3]);
        return;
    }
    if (rsIndex >= XBOX_D3DRS_PSCONSTANT1_0 && rsIndex <= XBOX_D3DRS_PSCONSTANT1_7) {
        uint32_t s = rsIndex - XBOX_D3DRS_PSCONSTANT1_0;
        UnpackD3DColor(value, &g_psStageC1[s][0], &g_psStageC1[s][1], &g_psStageC1[s][2], &g_psStageC1[s][3]);
        return;
    }
    if (rsIndex == XBOX_D3DRS_PSFINALCOMBINERCONSTANT0) {
        UnpackD3DColor(value, &g_psFcConst[0][0], &g_psFcConst[0][1], &g_psFcConst[0][2], &g_psFcConst[0][3]);
        return;
    }
    if (rsIndex == XBOX_D3DRS_PSFINALCOMBINERCONSTANT1) {
        UnpackD3DColor(value, &g_psFcConst[1][0], &g_psFcConst[1][1], &g_psFcConst[1][2], &g_psFcConst[1][3]);
        return;
    }
    if (g_currentPsHandle != 0u) {
        // Structural NV2A register-combiner / texture-program RenderStates: if overridden while
        // a compiled pixel shader handle is bound, bypass XboxPixelShaderObject::cachedPs and
        // resolve via structural hash from the live RenderState combiner configuration.
        if (rsIndex <= XBOX_D3DRS_PSFINALCOMBINERINPUTSEFG ||
            (rsIndex >= XBOX_D3DRS_PSALPHAOUTPUTS0 && rsIndex <= XBOX_D3DRS_PSCOMPAREMODE) ||
            (rsIndex >= XBOX_D3DRS_PSRGBOUTPUTS0 && rsIndex <= XBOX_D3DRS_PSINPUTTEXTURE) ||
            rsIndex == XBOX_D3DRS_PSTEXTUREMODES)
        {
            g_boundPsCombinerDirty = true;
        }
    }
}

void NotifyXboxTextureFactorChanged(uint32_t tfactorArgb) {
    (void)tfactorArgb;
}

// Forward declaration for CreatePixelShader pre-warming
static ID3D11PixelShader* GetOrCreateCompiledXboxPS(const XboxPixelShaderDef& def);

// Matches the 0xFC-byte Xbox D3D8 pixel shader struct created by _D3DDevice_CreatePixelShader@8 (0x006B3C60):
//   +0x00: uint32_t RefCount (= 1)
//   +0x04: uint32_t Flags    (= 1 for CreatePixelShader, 0 for SetPixelShaderProgram wrapper at CDevice+0x924)
//   +0x08: XboxPixelShaderDef* pPSDef (points to +0x0C, or external D3DPIXELSHADERDEF*)
//   +0x0C: XboxPixelShaderDef def (240 bytes)
struct XboxPixelShaderObject {
    uint32_t            refCount;
    uint32_t            flags;
    XboxPixelShaderDef* pPSDef;
    XboxPixelShaderDef  def;
    int32_t             engineType; // 0..27 if from aPixelShaders[0..27] at 0x007D1214, else -1
    ID3D11PixelShader*  cachedPs;
    ID3D11Device*       cachedPsDevice;
};

static SRWLOCK                             g_livePsObjLock = SRWLOCK_INIT;
static std::vector<XboxPixelShaderObject*> g_livePsObjects;

void EvictPixelShaderPointerByRange(uintptr_t addr, size_t byteLen) {
    if (addr == 0u || byteLen == 0u) return;
    uintptr_t endAddr = (UINTPTR_MAX - addr < byteLen) ? UINTPTR_MAX : (addr + byteLen);
    if (g_currentPsHandle >= addr && g_currentPsHandle < endAddr) {
        g_currentPsHandle      = 0u;
        g_currentPsType        = -1;
        g_boundPsCombinerDirty = false;
    }
    AcquireSRWLockExclusive(&g_livePsObjLock);
    for (size_t i = 0; i < g_livePsObjects.size();) {
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(g_livePsObjects[i]);
        if (objAddr >= addr && objAddr < endAddr) {
            g_livePsObjects[i]->cachedPs       = nullptr;
            g_livePsObjects[i]->cachedPsDevice = nullptr;
            g_livePsObjects[i] = g_livePsObjects.back();
            g_livePsObjects.pop_back();
        } else {
            ++i;
        }
    }
    ReleaseSRWLockExclusive(&g_livePsObjLock);
}

static const XboxPixelShaderDef* GetHandlePSDef(uint32_t handle, int32_t* outEngineType = nullptr) {
    if (outEngineType) *outEngineType = -1;
    if (handle < kMinValidHostAddress) return nullptr;
    if (handle == (ADDR_D3D_DEVICE + 0x924u)) {
        return *reinterpret_cast<const XboxPixelShaderDef* const*>(handle + 0x08u);
    }
    const XboxPixelShaderObject* obj = reinterpret_cast<const XboxPixelShaderObject*>(handle);
    if (outEngineType) *outEngineType = obj->engineType;
    return obj->pPSDef ? obj->pPSDef : &obj->def;
}

static float Clamp01Float(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v;
}

static uint32_t PackFloat4ToD3DColor(const float* rgba) {
    auto toByte = [](float v) -> uint32_t {
        if (v <= 0.0f) return 0u;
        if (v >= 1.0f) return 255u;
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    uint32_t r = toByte(rgba[0]);
    uint32_t g = toByte(rgba[1]);
    uint32_t b = toByte(rgba[2]);
    uint32_t a = toByte(rgba[3]);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static HRESULT __stdcall Bridge_D3DDevice_CreatePixelShader(const void* pShaderDef, DWORD* pHandle) {
    if (!pHandle) return E_INVALIDARG;
    XboxPixelShaderObject* obj = reinterpret_cast<XboxPixelShaderObject*>(
        Bridge_XMemAlloc(sizeof(XboxPixelShaderObject), 0x24800000u));
    if (!obj) return E_OUTOFMEMORY;
    memset(obj, 0, sizeof(XboxPixelShaderObject));
    obj->refCount   = 1u;
    obj->flags      = 1u;
    obj->pPSDef     = &obj->def;
    obj->engineType = -1;
    if (pShaderDef) {
        memcpy(&obj->def, pShaderDef, sizeof(XboxPixelShaderDef));
    }
    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_PIXEL_SHADERS_TABLE && hAddr < ADDR_PIXEL_SHADERS_TABLE + 28 * 8) {
        obj->engineType = static_cast<int32_t>((hAddr - ADDR_PIXEL_SHADERS_TABLE) / 8);
    }
    if (pShaderDef && g_d3dDevice) {
        obj->cachedPs       = GetOrCreateCompiledXboxPS(obj->def);
        obj->cachedPsDevice = g_d3dDevice;
    }
    AcquireSRWLockExclusive(&g_livePsObjLock);
    g_livePsObjects.push_back(obj);
    ReleaseSRWLockExclusive(&g_livePsObjLock);
    *pHandle = reinterpret_cast<DWORD>(obj);
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShader(DWORD Handle) {
    g_currentPsHandle      = Handle;
    g_boundPsCombinerDirty = false;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x784) = Handle;
    uint32_t* xboxRS = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);

    if (Handle >= kMinValidHostAddress) {
        int32_t engType = -1;
        const XboxPixelShaderDef* pDef = GetHandlePSDef(Handle, &engType);
        g_currentPsType = (engType >= 0 && engType < 28) ? engType : 28;
        if (pDef) {
            const uint32_t* dwords = reinterpret_cast<const uint32_t*>(pDef);
            // Synchronize D3DPIXELSHADERDEF into _D3D__RenderState[0..56] & PSTextureModes (136),
            // matching _D3DDevice_SetPixelShader@4 (0x006B3CF0).
            for (uint32_t i = 0; i <= 53; ++i) {
                xboxRS[i] = dwords[i];
            }
            xboxRS[XBOX_D3DRS_PSTEXTUREMODES] = dwords[54];
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x788) =
                ((pDef->PSFinalCombinerInputsABCD | pDef->PSFinalCombinerInputsEFG) != 0u) ? 1u : 0u;
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x78C) = (pDef->PSFinalCombinerConstants & 0x100u);
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x790) = dwords[54];
            xboxRS[55] = dwords[55]; // PSDotMapping
            xboxRS[56] = dwords[56]; // PSInputTexture

            // Unpack all per-stage & final-combiner embedded constants from D3DPIXELSHADERDEF
            for (int s = 0; s < 8; ++s) {
                UnpackD3DColor(dwords[10 + s], &g_psStageC0[s][0], &g_psStageC0[s][1], &g_psStageC0[s][2], &g_psStageC0[s][3]);
                UnpackD3DColor(dwords[18 + s], &g_psStageC1[s][0], &g_psStageC1[s][1], &g_psStageC1[s][2], &g_psStageC1[s][3]);
            }
            UnpackD3DColor(dwords[43], &g_psFcConst[0][0], &g_psFcConst[0][1], &g_psFcConst[0][2], &g_psFcConst[0][3]);
            UnpackD3DColor(dwords[44], &g_psFcConst[1][0], &g_psFcConst[1][1], &g_psFcConst[1][2], &g_psFcConst[1][3]);

            // Also keep g_psConstants[0..7] synchronized with embedded def defaults (matching Xbox ps.exe cN slots)
            uint32_t c0Map = dwords[57];
            uint32_t c1Map = dwords[58];
            uint32_t fcMap = dwords[59];
            for (int s = 0; s < 8; ++s) {
                uint32_t r0 = (c0Map >> (s * 4)) & 0xFu;
                if (r0 < 8u) {
                    memcpy(g_psConstants[r0], g_psStageC0[s], sizeof(float) * 4);
                }
                uint32_t r1 = (c1Map >> (s * 4)) & 0xFu;
                if (r1 < 8u) {
                    memcpy(g_psConstants[r1], g_psStageC1[s], sizeof(float) * 4);
                }
            }
            uint32_t fc0Reg = fcMap & 0xFu;
            if (fc0Reg < 8u) {
                memcpy(g_psConstants[fc0Reg], g_psFcConst[0], sizeof(float) * 4);
            }
            uint32_t fc1Reg = (fcMap >> 4) & 0xFu;
            if (fc1Reg < 8u) {
                memcpy(g_psConstants[fc1Reg], g_psFcConst[1], sizeof(float) * 4);
            }
        }
    } else {
        g_currentPsType = -1;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x788) = 0u;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x78C) = 0u;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x790) = 0u;
        xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSABCD] = 0u;
        xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSEFG]  = 0u;
        xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT]           = 0u;
        xboxRS[XBOX_D3DRS_PSDOTMAPPING]              = 0u;
        xboxRS[XBOX_D3DRS_PSINPUTTEXTURE]            = 0x00210000u;
        xboxRS[XBOX_D3DRS_PSTEXTUREMODES]            = 0u;
    }
    return S_OK;
}

// _D3DDevice_SetPixelShaderProgram@4 (0x006B3CB0):
// Takes a direct pointer to a D3DPIXELSHADERDEF (or NULL), wraps it in CDevice+0x924, and calls SetPixelShader.
static void __stdcall Bridge_D3DDevice_SetPixelShaderProgram(const XboxPixelShaderDef* pPSDef) {
    if (pPSDef) {
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x924) = 1u;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x928) = 0u;
        *reinterpret_cast<const XboxPixelShaderDef**>(ADDR_D3D_DEVICE + 0x92C) = pPSDef;
        Bridge_D3DDevice_SetPixelShader(ADDR_D3D_DEVICE + 0x924u);
    } else {
        Bridge_D3DDevice_SetPixelShader(0u);
    }
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount) {
    if (pConstantData && Register < 8 && ConstantCount > 0) {
        DWORD cnt = ConstantCount;
        if (Register + cnt > 8) cnt = 8 - Register;
        const float* srcFloats = static_cast<const float*>(pConstantData);
        for (DWORD i = 0; i < cnt; ++i) {
            DWORD r = Register + i;
            if (r == 0u) {
                // NV097_SET_EYE_DIRECTION (0x181C / CDevice+0x894) receives raw unclamped c0.xyz (0x006B4172)
                memcpy(g_psEyeDir, &srcFloats[i * 4], 16);
            }
            // NV2A combiner constant registers are unsigned [0.0, 1.0] (see maxps/minps in 0x006B3EF0)
            float clamped[4] = {
                Clamp01Float(srcFloats[i * 4 + 0]),
                Clamp01Float(srcFloats[i * 4 + 1]),
                Clamp01Float(srcFloats[i * 4 + 2]),
                Clamp01Float(srcFloats[i * 4 + 3])
            };
            memcpy(&g_psConstants[r][0], clamped, 16);
            memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0x894 + r * 16), &srcFloats[i * 4], 16);
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x8E4 + r * 4) = PackFloat4ToD3DColor(clamped);
        }

        // Synchronize mapped register combiner constant slots in _D3D__RenderState[10..25, 43..44] & g_psStageC0/C1/FcConst,
        // matching _D3DDevice_SetPixelShaderConstant@12 (0x006B3EF0).
        const XboxPixelShaderDef* pDef = GetHandlePSDef(g_currentPsHandle);
        if (pDef) {
            uint32_t* xboxRS = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);
            uint32_t c0Map = pDef->PSC0Mapping;
            uint32_t c1Map = pDef->PSC1Mapping;
            uint32_t fcMap = pDef->PSFinalCombinerConstants;
            for (DWORD r = Register; r < Register + cnt; ++r) {
                uint32_t packed = PackFloat4ToD3DColor(g_psConstants[r]);
                for (int s = 0; s < 8; ++s) {
                    if (((c0Map >> (s * 4)) & 0xFu) == r) {
                        xboxRS[10 + s] = packed;
                        memcpy(g_psStageC0[s], g_psConstants[r], 16);
                    }
                    if (((c1Map >> (s * 4)) & 0xFu) == r) {
                        xboxRS[18 + s] = packed;
                        memcpy(g_psStageC1[s], g_psConstants[r], 16);
                    }
                }
                if ((fcMap & 0xFu) == r) {
                    xboxRS[43] = packed;
                    memcpy(g_psFcConst[0], g_psConstants[r], 16);
                }
                if (((fcMap >> 4) & 0xFu) == r) {
                    xboxRS[44] = packed;
                    memcpy(g_psFcConst[1], g_psConstants[r], 16);
                }
            }
        }
    }
    return S_OK;
}

struct XboxDeclElem {
    uint8_t  stream;
    uint8_t  reg;
    uint8_t  xType;
    uint16_t offset;
};

static uint32_t GetXboxDeclTypeByteSize(uint32_t xboxType) {
    switch (xboxType) {
        case 0x12: return 4;  // FLOAT1
        case 0x22: return 8;  // FLOAT2
        case 0x32: return 12; // FLOAT3
        case 0x42: return 16; // FLOAT4
        case 0x40: return 4;  // D3DCOLOR
        case 0x41: return 4;  // UBYTE4
        case 0x25: return 4;  // SHORT2
        case 0x45: return 8;  // SHORT4
        case 0x24: return 4;  // SHORT2N
        case 0x44: return 8;  // SHORT4N
        default:   return 12;
    }
}

static DXGI_FORMAT MapXboxDeclTypeToDXGI(uint8_t xType) {
    switch (xType) {
        case 0x12: return DXGI_FORMAT_R32_FLOAT;
        case 0x22: return DXGI_FORMAT_R32G32_FLOAT;
        case 0x32: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 0x42: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 0x40: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case 0x41: return DXGI_FORMAT_R8G8B8A8_UINT;
        case 0x25: return DXGI_FORMAT_R16G16_SINT;
        case 0x45: return DXGI_FORMAT_R16G16B16A16_SINT;
        case 0x24: return DXGI_FORMAT_R16G16_SNORM;
        case 0x44: return DXGI_FORMAT_R16G16B16A16_SNORM;
        default:   return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

static void FormatXvuSwizzle(uint32_t swz, char* outBuf) {
    static const char kComp[4] = { 'x', 'y', 'z', 'w' };
    uint32_t x = (swz >> 6) & 3u;
    uint32_t y = (swz >> 4) & 3u;
    uint32_t z = (swz >> 2) & 3u;
    uint32_t w = swz & 3u;
    if (x == 0u && y == 1u && z == 2u && w == 3u) {
        outBuf[0] = '\0';
        return;
    }
    outBuf[0] = '.';
    outBuf[1] = kComp[x];
    outBuf[2] = kComp[y];
    outBuf[3] = kComp[z];
    outBuf[4] = kComp[w];
    outBuf[5] = '\0';
}

static void FormatXvuWriteMask(uint32_t mask, char* outBuf) {
    static const char kComp[4] = { 'x', 'y', 'z', 'w' };
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << (3 - i))) {
            outBuf[pos++] = kComp[i];
        }
    }
    outBuf[pos] = '\0';
}

static void FormatXvuSrcOperand(
    uint32_t src, uint32_t reg, uint32_t neg, uint32_t swz,
    uint32_t va, uint32_t cIdx, uint32_t rel, const char* constArrName,
    char* outBuf, size_t outSize)
{
    char base[64];
    if (src == 1u) {
        snprintf(base, sizeof(base), "r%u", (reg <= 12u) ? reg : 0u);
    } else if (src == 2u) {
        snprintf(base, sizeof(base), "v%u", va & 15u);
    } else if (src == 3u) {
        if (rel != 0u) {
            snprintf(base, sizeof(base), "%s[clamp(a0x + %u, 0, 191)]", constArrName, cIdx);
        } else {
            snprintf(base, sizeof(base), "%s[%u]", constArrName, (cIdx < 192u) ? cIdx : 191u);
        }
    } else {
        snprintf(base, sizeof(base), "float4(0.0f, 0.0f, 0.0f, 0.0f)");
    }

    char swzStr[8];
    FormatXvuSwizzle(swz, swzStr);
    if (neg != 0u) {
        snprintf(outBuf, outSize, "(-%s%s)", base, swzStr);
    } else {
        snprintf(outBuf, outSize, "%s%s", base, swzStr);
    }
}

// Decompiles 128-bit Xbox NV2A Cheops .xvu / XSS vertex shader microcode into D3D11 HLSL (vs_4_0)
static std::string DecompileXvuToHLSL(
    const XboxDeclElem* elems, uint32_t numElems, const DWORD* pFunction,
    const char* relPath = nullptr, uintptr_t declVa = 0)
{
    uint32_t count = (pFunction[0] >> 16) & 0xFFFFu;
    if (count > 136u) count = 136u;

    bool hasC58         = false;
    bool writesOD1      = false;
    bool writesOFog     = false;
    bool writesConstRam = false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w1 = pFunction[1 + i * 4 + 1];
        uint32_t w2 = pFunction[1 + i * 4 + 2];
        uint32_t w3 = pFunction[1 + i * 4 + 3];
        uint32_t cIdx = (w1 >> 13) & 0xFFu;
        uint32_t aSrc = (w2 >> 26) & 3u;
        uint32_t bSrc = (w2 >> 11) & 3u;
        uint32_t cSrc = (w3 >> 28) & 3u;
        uint32_t rel  = (w3 >> 1)  & 1u;
        if ((aSrc == 3u || bSrc == 3u || cSrc == 3u) && rel == 0u && (cIdx == 58u || cIdx == 59u)) {
            hasC58 = true;
        }
        uint32_t outMask  = (w3 >> 12) & 0xFu;
        uint32_t outWrite = (w3 >> 11) & 1u;
        uint32_t outReg   = (w3 >> 3)  & 0xFFu;
        if (outMask != 0u) {
            if (outWrite != 0u) {
                if (outReg == 4u) writesOD1 = true;
                if (outReg == 5u) writesOFog = true;
            } else if (outReg < 192u) {
                writesConstRam = true;
            }
        }
    }

    const char* constArrName = writesConstRam ? "cw" : "c";

    std::string s;
    s.reserve(8192);
    if (relPath && relPath[0]) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
            "// ============================================================================\n"
            "// Decompiled Xbox NV2A Cheops Vertex Shader (.xvu -> D3D11 HLSL vs_4_0)\n"
            "// Source: %s\n"
            "// Declaration VA: 0x%08X | Instructions: %u\n"
            "// ============================================================================\n\n",
            relPath, (uint32_t)declVa, count);
        s += hdr;
    }
    s += "cbuffer PerDrawCB : register(b0) {\n"
         "    float4 uWvp0; float4 uWvp1; float4 uWvp2; float4 uWvp3;\n"
         "    float4 uViewport; float4 uTFactor;\n"
         "    uint4 uStage0Ops; uint4 uStage0Alpha;\n"
         "    uint4 uStage1Ops; uint4 uStage1Alpha;\n"
         "    uint4 uStage2Ops; uint4 uStage2Alpha;\n"
         "    uint4 uStage3Ops; uint4 uStage3Alpha;\n"
         "    uint4 uFlags;\n"
         "    float4 uAlphaTest;\n"
         "    float4 uUvScale01;\n"
         "    float4 uUvScale23;\n"
         "    float4 uScreenSpaceParams;\n"
         "};\n"
         "cbuffer VSConstantsCB : register(b1) {\n"
         "    float4 c[192];\n"
         "};\n"
         "struct VSIn {\n";

    bool declaredReg[16] = {};
    char line[2048];
    for (uint32_t i = 0; i < numElems; ++i) {
        uint8_t r = elems[i].reg;
        if (r >= 16 || declaredReg[r]) continue;
        declaredReg[r] = true;
        const char* hlslType = "float3";
        switch (elems[i].xType) {
            case 0x12: hlslType = "float";  break;
            case 0x22:
            case 0x24: hlslType = "float2"; break;
            case 0x25: hlslType = "int2";   break;
            case 0x32: hlslType = "float3"; break;
            case 0x41: hlslType = "uint4";  break;
            case 0x45: hlslType = "int4";   break;
            default:   hlslType = "float4"; break;
        }
        snprintf(line, sizeof(line), "    %s raw_v%u : ATTR%u;\n", hlslType, r, r);
        s += line;
    }
    if (numElems == 0) {
        s += "    uint vid : SV_VertexID;\n";
    }
    s += "};\n"
         "struct VSOut {\n"
         "    float4 pos     : SV_POSITION;\n"
         "    float4 color   : COLOR0;\n"
         "    float4 uv0     : TEXCOORD0;\n"
         "    float4 uv1     : TEXCOORD1;\n"
         "    float4 uv2     : TEXCOORD2;\n"
         "    float4 uv3     : TEXCOORD3;\n"
         "    float2 eyeDist : TEXCOORD4;\n"
         "    float3 norm    : TEXCOORD5;\n"
         "    float3 eyeVec  : TEXCOORD6;\n"
         "};\n"
         "float nv_rcp(float x) { return (abs(x) > 1e-12f) ? clamp(1.0f / x, -1e12f, 1e12f) : 1e12f; }\n"
         "float nv_rsq(float x) { return (x > 1e-12f) ? clamp(rsqrt(x), 0.0f, 1e12f) : 0.0f; }\n"
         "float nv_rcc(float x) { float t = (abs(x) > 1e-12f) ? (1.0f / x) : 0.0f; return (t >= 0.0f) ? clamp(t, 5.42101086e-20f, 1.884467e19f) : clamp(t, -1.884467e19f, -5.42101086e-20f); }\n"
         "float4 nv_lit(float4 v) { float x = max(v.x, 0.0f); float y = (v.x > 0.0f) ? pow(max(v.y, 0.0f), clamp(v.w, -128.0f, 128.0f)) : 0.0f; return float4(1.0f, x, y, 1.0f); }\n"
         "float4 nv_expp(float x) { float fl = floor(x); float fr = x - fl; return float4(exp2(clamp(fl, -126.0f, 127.0f)), fr, exp2(clamp(x, -126.0f, 127.0f)), 1.0f); }\n"
         "float4 nv_logp(float x) {\n"
         "    float v = abs(x);\n"
         "    if (v <= 1.17549435e-38f) return float4(-128.0f, 1.0f, -128.0f, 1.0f);\n"
         "    uint bits = asuint(v);\n"
         "    int e = int((bits >> 23u) & 0xFFu) - 127;\n"
         "    float m = asfloat((bits & 0x007FFFFFu) | 0x3F800000u);\n"
         "    return float4(float(e), m, float(e) + log2(m), 1.0f);\n"
         "}\n"
         "VSOut VSMain(VSIn In) {\n";

    if (writesConstRam) {
        s += "    float4 cw[192];\n"
             "    [unroll] for (int ci = 0; ci < 192; ++ci) { cw[ci] = c[ci]; }\n";
    }

    bool unpackedReg[16] = {};
    for (uint32_t i = 0; i < numElems; ++i) {
        uint8_t r = elems[i].reg;
        if (r >= 16 || unpackedReg[r]) continue;
        unpackedReg[r] = true;
        switch (elems[i].xType) {
            case 0x12:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 0.0f, 0.0f, 1.0f);\n", r, r);
                break;
            case 0x22:
            case 0x24:
            case 0x25:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 0.0f, 1.0f);\n", r, r);
                break;
            case 0x32:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 1.0f);\n", r, r);
                break;
            default:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u);\n", r, r);
                break;
        }
        s += line;
    }
    for (uint32_t r = 0; r < 16; ++r) {
        if (!unpackedReg[r]) {
            snprintf(line, sizeof(line), "    float4 v%u = float4(0.0f, 0.0f, 0.0f, 1.0f);\n", r);
            s += line;
        }
    }

    s += "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n"
         "    float4 r12 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    int a0x = 0, next_a0x = 0;\n"
         "    float4 clipPos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    float4 oD0 = float4(1.0f, 1.0f, 1.0f, 1.0f), oD1 = 0, oFog = float4(1.0f, 0.0f, 0.0f, 0.0f), oPts = float4(1.0f, 0.0f, 0.0f, 0.0f), oB0 = 0, oB1 = 0;\n"
         "    float4 oT0 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT1 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT2 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT3 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    float4 macVal = 0, iluVal = 0;\n";

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w1 = pFunction[1 + i * 4 + 1];
        uint32_t w2 = pFunction[1 + i * 4 + 2];
        uint32_t w3 = pFunction[1 + i * 4 + 3];

        uint32_t ilu  = (w1 >> 25) & 0x7u;
        uint32_t mac  = (w1 >> 21) & 0xFu;
        uint32_t cIdx = (w1 >> 13) & 0xFFu;
        uint32_t va   = (w1 >> 9)  & 0xFu;

        uint32_t aNeg = (w1 >> 8)  & 1u;
        uint32_t aSwz =  w1        & 0xFFu;
        uint32_t aR   = (w2 >> 28) & 0xFu;
        uint32_t aSrc = (w2 >> 26) & 3u;

        uint32_t bNeg = (w2 >> 25) & 1u;
        uint32_t bSwz = (w2 >> 17) & 0xFFu;
        uint32_t bR   = (w2 >> 13) & 0xFu;
        uint32_t bSrc = (w2 >> 11) & 3u;

        uint32_t cNeg = (w2 >> 10) & 1u;
        uint32_t cSwz = (w2 >> 2)  & 0xFFu;
        uint32_t cR   = ((w2 & 3u) << 2) | ((w3 >> 30) & 3u);
        uint32_t cSrc = (w3 >> 28) & 3u;

        uint32_t macMask  = (w3 >> 24) & 0xFu;
        uint32_t macR     = (w3 >> 20) & 0xFu;
        uint32_t iluMask  = (w3 >> 16) & 0xFu;
        uint32_t outMask  = (w3 >> 12) & 0xFu;
        uint32_t outWrite = (w3 >> 11) & 1u;
        uint32_t outReg   = (w3 >> 3)  & 0xFFu;
        uint32_t outMux   = (w3 >> 2)  & 1u;
        uint32_t rel      = (w3 >> 1)  & 1u;
        uint32_t isEnd    =  w3        & 1u;

        char sA[128], sB[128], sC[128];
        FormatXvuSrcOperand(aSrc, aR, aNeg, aSwz, va, cIdx, rel, constArrName, sA, sizeof(sA));
        FormatXvuSrcOperand(bSrc, bR, bNeg, bSwz, va, cIdx, rel, constArrName, sB, sizeof(sB));
        FormatXvuSrcOperand(cSrc, cR, cNeg, cSwz, va, cIdx, rel, constArrName, sC, sizeof(sC));

        // Evaluate MAC & ILU using pre-instruction r0..r12 and pre-instruction a0x simultaneously
        if (mac == 13u) {
            snprintf(line, sizeof(line), "    next_a0x = (int)floor((%s).x);\n", sA);
            s += line;
        } else if (mac != 0u) {
            switch (mac) {
                case 1u:  snprintf(line, sizeof(line), "    macVal = %s;\n", sA); break;
                case 2u:  snprintf(line, sizeof(line), "    macVal = (%s * %s);\n", sA, sB); break;
                case 3u:  snprintf(line, sizeof(line), "    macVal = (%s + %s);\n", sA, sC); break;
                case 4u:  snprintf(line, sizeof(line), "    macVal = (%s * %s + %s);\n", sA, sB, sC); break;
                case 5u:  snprintf(line, sizeof(line), "    macVal = dot((%s).xyz, (%s).xyz).xxxx;\n", sA, sB); break;
                case 6u:  snprintf(line, sizeof(line), "    macVal = dot(float4((%s).xyz, 1.0f), %s).xxxx;\n", sA, sB); break;
                case 7u:  snprintf(line, sizeof(line), "    macVal = dot(%s, %s).xxxx;\n", sA, sB); break;
                case 8u:  snprintf(line, sizeof(line), "    macVal = float4(1.0f, (%s).y * (%s).y, (%s).z, (%s).w);\n", sA, sB, sA, sB); break;
                case 9u:  snprintf(line, sizeof(line), "    macVal = min(%s, %s);\n", sA, sB); break;
                case 10u: snprintf(line, sizeof(line), "    macVal = max(%s, %s);\n", sA, sB); break;
                case 11u: snprintf(line, sizeof(line), "    macVal = ((%s < %s) ? float4(1,1,1,1) : float4(0,0,0,0));\n", sA, sB); break;
                case 12u: snprintf(line, sizeof(line), "    macVal = ((%s >= %s) ? float4(1,1,1,1) : float4(0,0,0,0));\n", sA, sB); break;
                default:  snprintf(line, sizeof(line), "    macVal = float4(0,0,0,0);\n"); break;
            }
            s += line;
        }

        if (ilu != 0u) {
            switch (ilu) {
                case 1u: snprintf(line, sizeof(line), "    iluVal = %s;\n", sC); break;
                case 2u: snprintf(line, sizeof(line), "    iluVal = nv_rcp((%s).x).xxxx;\n", sC); break;
                case 3u: snprintf(line, sizeof(line), "    iluVal = nv_rcc((%s).x).xxxx;\n", sC); break;
                case 4u: snprintf(line, sizeof(line), "    iluVal = nv_rsq((%s).x).xxxx;\n", sC); break;
                case 5u: snprintf(line, sizeof(line), "    iluVal = nv_expp((%s).x);\n", sC); break;
                case 6u: snprintf(line, sizeof(line), "    iluVal = nv_logp((%s).x);\n", sC); break;
                case 7u: snprintf(line, sizeof(line), "    iluVal = nv_lit(%s);\n", sC); break;
                default: snprintf(line, sizeof(line), "    iluVal = float4(0,0,0,0);\n"); break;
            }
            s += line;
        }

        if (mac == 13u) {
            s += "    a0x = next_a0x;\n";
        }

        bool isViewportScaleInst = (rel == 0u && (cIdx == 58u || cIdx == 59u));
        char maskStr[8];
        if (mac != 0u && mac != 13u && macMask != 0u && macR <= 12u) {
            FormatXvuWriteMask(macMask, maskStr);
            snprintf(line, sizeof(line), "    r%u.%s = macVal.%s;\n", macR, maskStr, maskStr);
            s += line;
            if (macR == 12u && hasC58 && !isViewportScaleInst) {
                snprintf(line, sizeof(line), "    clipPos.%s = macVal.%s;\n", maskStr, maskStr);
                s += line;
            }
        }
        if (ilu != 0u && iluMask != 0u) {
            // Cheops encodes ILU temp destination in macR whenever MAC does not write an rN register (NOP or ARL)
            uint32_t iluR = ((mac == 0u || mac == 13u) && macR <= 12u) ? macR : 1u;
            FormatXvuWriteMask(iluMask, maskStr);
            snprintf(line, sizeof(line), "    r%u.%s = iluVal.%s;\n", iluR, maskStr, maskStr);
            s += line;
            if (iluR == 12u && hasC58 && !isViewportScaleInst) {
                snprintf(line, sizeof(line), "    clipPos.%s = iluVal.%s;\n", maskStr, maskStr);
                s += line;
            }
        }
        if (outMask != 0u) {
            const char* srcVal = (outMux != 0u) ? "iluVal" : "macVal";
            FormatXvuWriteMask(outMask, maskStr);
            if (outWrite != 0u) {
                const char* outName = nullptr;
                switch (outReg) {
                    case 0u:  outName = "r12";  break;
                    case 3u:  outName = "oD0";  break;
                    case 4u:  outName = "oD1";  break;
                    case 5u:  outName = "oFog"; break;
                    case 6u:  outName = "oPts"; break;
                    case 7u:  outName = "oB0";  break;
                    case 8u:  outName = "oB1";  break;
                    case 9u:  outName = "oT0";  break;
                    case 10u: outName = "oT1";  break;
                    case 11u: outName = "oT2";  break;
                    case 12u: outName = "oT3";  break;
                    default:  break;
                }
                if (outName) {
                    snprintf(line, sizeof(line), "    %s.%s = %s.%s;\n", outName, maskStr, srcVal, maskStr);
                    s += line;
                    if (outReg == 0u && hasC58 && !isViewportScaleInst) {
                        snprintf(line, sizeof(line), "    clipPos.%s = %s.%s;\n", maskStr, srcVal, maskStr);
                        s += line;
                    }
                }
            } else if (writesConstRam && outReg < 192u) {
                snprintf(line, sizeof(line), "    cw[%u].%s = %s.%s;\n", outReg, maskStr, srcVal, maskStr);
                s += line;
            }
        }
        if (isEnd != 0u) {
            break;
        }
    }

    s += "    VSOut o = (VSOut)0;\n";
    if (hasC58) {
        s += "    o.pos = clipPos;\n";
        if (writesOFog) {
            s += "    o.eyeDist = float2(oFog.x, 1.0f);\n";
        } else {
            s += "    o.eyeDist = float2(max(clipPos.w, 0.0f), 1.0f);\n";
        }
    } else {
        s += "    float vpW = max(uViewport.z, 1.0f);\n"
             "    float vpH = max(uViewport.w, 1.0f);\n"
             "    float sx = r12.x + uScreenSpaceParams.x;\n"
             "    float sy = r12.y + uScreenSpaceParams.y;\n"
             "    float ndcX = ((sx - uViewport.x) / vpW) * 2.0f - 1.0f;\n"
             "    float ndcY = 1.0f - ((sy - uViewport.y) / vpH) * 2.0f;\n"
             "    float zVal = r12.z;\n"
             "    if (zVal > 1.0f) zVal /= max(uScreenSpaceParams.z, 1.0f);\n"
             "    float zNorm = saturate(zVal);\n"
             "    float clipW = (r12.w > 1e-6f && r12.w < 1e6f) ? (1.0f / r12.w) : 1.0f;\n"
             "    o.pos = float4(ndcX * clipW, ndcY * clipW, zNorm * clipW, clipW);\n";
        if (writesOFog) {
            s += "    o.eyeDist = float2(oFog.x, r12.w);\n";
        } else {
            s += "    o.eyeDist = float2(0.0f, r12.w);\n";
        }
    }
    s += "    o.color  = saturate(oD0);\n"
         "    o.uv0    = float4(oT0.xy * uUvScale01.xy, oT0.zw);\n"
         "    o.uv1    = float4(oT1.xy * uUvScale01.zw, oT1.zw);\n"
         "    o.uv2    = float4(oT2.xy * uUvScale23.xy, oT2.zw);\n"
         "    o.uv3    = float4(oT3.xy * uUvScale23.zw, oT3.zw);\n"
         "    o.norm   = saturate(oD1.xyz);\n"
         "    o.eyeVec = float3(oT1.w, oT2.w, oT3.w);\n";

    if (writesOD1) {
        s += "    if ((uStage0Ops.x == 22u && (uStage0Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage1Ops.x == 22u && (uStage1Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage2Ops.x == 22u && (uStage2Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage3Ops.x == 22u && (uStage3Ops.z & 0x0Fu) == 0u)) {\n"
             "        o.color = float4(saturate(oD1.xyz), saturate(oD0.w));\n"
             "    }\n";
    }

    s += "    return o;\n"
         "}\n";
    return s;
}

static uint8_t GetXboxDeclTypeComponentCount(uint32_t xType) {
    switch (xType) {
        case 0x12: return 1; // FLOAT1
        case 0x22:
        case 0x24:
        case 0x25: return 2; // FLOAT2 / SHORT2 / NORMSHORT2
        case 0x32: return 3; // FLOAT3
        default:   return 4; // FLOAT4 / D3DCOLOR / UBYTE4 / SHORT4
    }
}

static void ParseXboxVertexDeclaration(
    const DWORD* pDeclaration, bool isFixedFunction,
    XboxDeclElem* outElems, uint32_t* outNumElems, XboxShaderDeclInfo* outInfo)
{
    uint32_t numElems = 0;
    if (pDeclaration) {
        uint16_t curStream = 0;
        uint16_t curOffset = 0;
        for (int i = 0; i < 512 && pDeclaration[i] != 0xFFFFFFFF; ++i) {
            DWORD tok = pDeclaration[i];
            DWORD tokType = (tok >> 29) & 0x7;
            if (tokType == 1) {
                curStream = (uint16_t)(tok & 0x0F);
                curOffset = 0;
            } else if (tokType == 4) {
                // D3DVSD_CONST(reg, count): ?ParseDeclarationConstants@D3D@@YGKPBKPCK@Z (0x006B26F0)
                uint32_t constCount = (tok >> 25) & 0x0Fu;
                uint32_t hwSlot     = tok & 0xFFu;
                for (uint32_t c = 0; c < constCount; ++c) {
                    if (i + 4 >= 512) break;
                    if (outInfo && outInfo->numDeclConsts < 32 && (hwSlot + c) < 192u) {
                        XboxDeclConstEntry& ce = outInfo->declConsts[outInfo->numDeclConsts++];
                        ce.hwSlot = static_cast<uint8_t>(hwSlot + c);
                        memcpy(ce.val, &pDeclaration[i + 1], 16);
                    }
                    i += 4;
                }
            } else if (tokType == 2) {
                if (tok & 0x10000000) {
                    DWORD skipDwords = (tok >> 16) & 0x0F;
                    curOffset += (uint16_t)(skipDwords * 4);
                } else {
                    uint32_t reg = tok & 0x1F;
                    uint32_t xType = (tok >> 16) & 0xFF;
                    uint32_t byteSize = GetXboxDeclTypeByteSize(xType);
                    uint8_t compCount = GetXboxDeclTypeComponentCount(xType);
                    if (curStream < 2 && reg < 16 && numElems < 16) {
                        if (outElems) {
                            outElems[numElems].stream = (uint8_t)curStream;
                            outElems[numElems].reg    = (uint8_t)reg;
                            outElems[numElems].xType  = (uint8_t)xType;
                            outElems[numElems].offset = curOffset;
                        }
                        numElems++;
                        if (outInfo) outInfo->usesStream[curStream] = true;
                    }
                    if (outInfo) {
                        if (isFixedFunction) {
                            // Xbox fixed-function D3DVSDE_* register mapping (see InitializeVertexShaderFromFvf at 0x006B22F0)
                            if (curStream == 0) {
                                switch (reg) {
                                    case 0:
                                        outInfo->hasPos = true;
                                        outInfo->offPos = curOffset;
                                        if (xType == 0x42) {
                                            outInfo->hasRhw = true;
                                            outInfo->offRhw = (uint16_t)(curOffset + 12);
                                        }
                                        break;
                                    case 1:
                                        if (xType == 0x41) {
                                            outInfo->hasBlendIndices = true;
                                            outInfo->offBlendIndices = curOffset;
                                        } else {
                                            outInfo->hasBlendWeight  = true;
                                            outInfo->offBlendWeight  = curOffset;
                                            outInfo->numBlendWeights = compCount;
                                        }
                                        break;
                                    case 2:  outInfo->hasNorm = true; outInfo->offNorm = curOffset; break;
                                    case 3:  outInfo->hasDiff = true; outInfo->offDiff = curOffset; break;
                                    case 4:  outInfo->hasSpec = true; outInfo->offSpec = curOffset; break;
                                    case 7:
                                        if (xType == 0x41) {
                                            outInfo->hasBlendIndices = true;
                                            outInfo->offBlendIndices = curOffset;
                                        }
                                        break;
                                    case 9:  outInfo->hasUv0  = true; outInfo->offUv0  = curOffset; outInfo->dimUv[0] = compCount; break;
                                    case 10: outInfo->hasUv1  = true; outInfo->offUv1  = curOffset; outInfo->dimUv[1] = compCount; break;
                                    case 11: outInfo->hasUv2  = true; outInfo->offUv2  = curOffset; outInfo->dimUv[2] = compCount; break;
                                    case 12: outInfo->hasUv3  = true; outInfo->offUv3  = curOffset; outInfo->dimUv[3] = compCount; break;
                                    default: break;
                                }
                            } else if (curStream == 1 && reg == 3) {
                                outInfo->hasStream1Diff = true;
                                outInfo->offStream1Diff = curOffset;
                            }
                        } else if (curStream == 0) {
                            // Ghost.exe programmable D3DVSD register convention (0x007A980C..0x007A996C)
                            switch (reg) {
                                case 0:  outInfo->hasPos  = true; outInfo->offPos  = curOffset; break;
                                case 1:  outInfo->hasNorm = true; outInfo->offNorm = curOffset; break;
                                case 2:  outInfo->hasDiff = true; outInfo->offDiff = curOffset; break;
                                case 3:  outInfo->hasUv0  = true; outInfo->offUv0  = curOffset; outInfo->dimUv[0] = compCount; break;
                                case 4:  outInfo->hasUv1  = true; outInfo->offUv1  = curOffset; outInfo->dimUv[1] = compCount; break;
                                case 5:  outInfo->hasUv2  = true; outInfo->offUv2  = curOffset; outInfo->dimUv[2] = compCount; break;
                                case 6:  outInfo->hasUv3  = true; outInfo->offUv3  = curOffset; outInfo->dimUv[3] = compCount; break;
                                case 10: outInfo->hasRhw  = true; outInfo->offRhw  = curOffset; break;
                                default: break;
                            }
                        }
                    }
                    curOffset += (uint16_t)byteSize;
                }
                if (outInfo && curStream < 2) {
                    outInfo->streamStrides[curStream] = curOffset;
                }
            }
        }
    }
    if (outNumElems) *outNumElems = numElems;
}

// ============================================================================
// First-Run HLSL Generation & D3D11 Disk Shader Cache (GhostXR_Data\shaders\)
// ============================================================================

static const char* kSharedPerDrawCBAndPSInHLSL =
    "cbuffer PerDrawCB : register(b0) {\n"
    "    float4 uWvpRow0;\n"
    "    float4 uWvpRow1;\n"
    "    float4 uWvpRow2;\n"
    "    float4 uWvpRow3;\n"
    "    float4 uViewport;\n"
    "    float4 uTFactor;\n"
    "    uint4  uStage0Ops;\n"
    "    uint4  uStage0Alpha;\n"
    "    uint4  uStage1Ops;\n"
    "    uint4  uStage1Alpha;\n"
    "    uint4  uStage2Ops;\n"
    "    uint4  uStage2Alpha;\n"
    "    uint4  uStage3Ops;\n"
    "    uint4  uStage3Alpha;\n"
    "    uint4  uFlags;\n"
    "    float4 uAlphaTest;\n"
    "    float4 uUvScale01;\n"
    "    float4 uUvScale23;\n"
    "    float4 uScreenSpaceParams;\n"
    "    float4 uBumpParams;\n"
    "    float4 uFogParams;\n"
    "    float4 uFogColor;\n"
    "    float4 uPsConst0;\n"
    "    float4 uPsConst1;\n"
    "    float4 uPsConst2;\n"
    "    float4 uPsConst3;\n"
    "    float4 uPsConst4;\n"
    "    float4 uPsConst5;\n"
    "    float4 uPsConst6;\n"
    "    float4 uPsConst7;\n"
    "    float4 uWorldRow0;\n"
    "    float4 uWorldRow1;\n"
    "    float4 uWorldRow2;\n"
    "    float4 uWorldRow3;\n"
    "    uint4  uLightCount;\n"
    "    float4 uLightPosType[8];\n"
    "    float4 uLightDirRange[8];\n"
    "    float4 uLightAttenSpot[8];\n"
    "    float4 uLightSpotParams[8];\n"
    "    float4 uLightDiffuse[8];\n"
    "    float4 uLightAmbient[8];\n"
    "    float4 uLightSpecular[8];\n"
    "    float4 uEyePosWorld;\n"
    "    float4 uMatBackDiffAmb;\n"
    "    float4 uMatBackSpec;\n"
    "    uint4  uStageColorKeyOp;\n"
    "    float4 uStageColorKeyColor[4];\n"
    "    uint4  uStageColorSign;\n"
    "    float4 uBumpLuminance;\n"
    "    float4 uScissorRects[8];\n"
    "    uint4  uSkinParams;\n"
    "    float4 uPsStageC0[8];\n"
    "    float4 uPsStageC1[8];\n"
    "    float4 uPsFcConst[2];\n"
    "    float4 uStageBumpMat[4];\n"
    "    float4 uStageBumpLum[4];\n"
    "    float4 uPsEyeDir;\n"
    "};\n\n"
    "struct PSIn {\n"
    "    float4 pos    : SV_POSITION;\n"
    "    float4 color  : COLOR0;\n"
    "    float4 uv0    : TEXCOORD0;\n"
    "    float4 uv1    : TEXCOORD1;\n"
    "    float4 uv2    : TEXCOORD2;\n"
    "    float4 uv3    : TEXCOORD3;\n"
    "    float2 eyeDist: TEXCOORD4;\n"
    "    float3 norm   : TEXCOORD5;\n"
    "    float3 eyeVec : TEXCOORD6;\n"
    "};\n\n";

static const char* kSharedPsResourcesAndHelpersHLSL =
    "Texture2D    tex0        : register(t0);\n"
    "Texture2D    tex1        : register(t1);\n"
    "Texture2D    tex2        : register(t2);\n"
    "Texture2D    tex3        : register(t3);\n"
    "TextureCube  cubeTex0    : register(t4);\n"
    "TextureCube  cubeTex1    : register(t5);\n"
    "TextureCube  cubeTex2    : register(t6);\n"
    "TextureCube  cubeTex3    : register(t7);\n"
    "Texture3D    volTex0     : register(t8);\n"
    "Texture3D    volTex1     : register(t9);\n"
    "Texture3D    volTex2     : register(t10);\n"
    "Texture3D    volTex3     : register(t11);\n"
    "Texture2D    gammaLutTex : register(t12);\n"
    "SamplerState smp0        : register(s0);\n"
    "SamplerState smp1        : register(s1);\n"
    "SamplerState smp2        : register(s2);\n"
    "SamplerState smp3        : register(s3);\n\n"
    "float4 SampleStageTexEx(\n"
    "    uint stageIdx, Texture2D tex2d, TextureCube texCube, Texture3D texVol,\n"
    "    SamplerState smp, float4 uv, bool isCube, bool isVol, float4 unboundFallback)\n"
    "{\n"
    "    if (((uFlags.y >> stageIdx) & 1u) == 0u) {\n"
    "        return unboundFallback;\n"
    "    }\n"
    "    float4 c;\n"
    "    if (isCube) {\n"
    "        c = texCube.Sample(smp, uv.xyz);\n"
    "    } else if (isVol) {\n"
    "        c = texVol.Sample(smp, uv.xyz);\n"
    "    } else {\n"
    "        c = tex2d.Sample(smp, uv.xy);\n"
    "    }\n"
    "    if (((uFlags.y >> (24u + stageIdx)) & 1u) != 0u) {\n"
    "        uint d24 = (uint)(saturate(c.r) * 16777215.0f + 0.5f);\n"
    "        c = float4(\n"
    "            (float)((d24 >> 8u)  & 0xFFu) * (1.0f / 255.0f),\n"
    "            (float)( d24         & 0xFFu) * (1.0f / 255.0f),\n"
    "            0.0f,\n"
    "            (float)((d24 >> 16u) & 0xFFu) * (1.0f / 255.0f));\n"
    "    }\n"
    "    uint ckOp = uStageColorKeyOp[stageIdx];\n"
    "    if (ckOp != 0u) {\n"
    "        float4 ckCol = uStageColorKeyColor[stageIdx];\n"
    "        bool matchRgb = all(abs(c.rgb - ckCol.rgb) < (1.5f / 255.0f));\n"
    "        if (ckOp == 1u && matchRgb) {\n"
    "            c.a = 0.0f;\n"
    "        } else if (ckOp == 2u && matchRgb && abs(c.a - ckCol.a) < (1.5f / 255.0f)) {\n"
    "            c = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "        } else if (ckOp == 3u && matchRgb) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    if (((uFlags.y >> (16u + stageIdx)) & 1u) != 0u) {\n"
    "        if (c.a < (0.5f / 255.0f)) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    uint cSign = uStageColorSign[stageIdx];\n"
    "    if (cSign != 0u) {\n"
    "        if ((cSign & 0x20u) != 0u) c.r = c.r * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x40u) != 0u) c.g = c.g * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x80u) != 0u) c.b = c.b * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x10u) != 0u) c.a = c.a * 2.0f - 1.0f;\n"
    "    }\n"
    "    return c;\n"
    "}\n\n"
    "float4 SampleStageTex(\n"
    "    uint stageIdx, Texture2D tex2d, TextureCube texCube, Texture3D texVol,\n"
    "    SamplerState smp, float4 uv, bool isCube, bool isVol)\n"
    "{\n"
    "    return SampleStageTexEx(stageIdx, tex2d, texCube, texVol, smp, uv, isCube, isVol, float4(1.0f, 1.0f, 1.0f, 1.0f));\n"
    "}\n\n"
    "float4 SampleStageTexPs(\n"
    "    uint stageIdx, Texture2D tex2d, TextureCube texCube, Texture3D texVol,\n"
    "    SamplerState smp, float4 uv, bool isCube, bool isVol)\n"
    "{\n"
    "    return SampleStageTexEx(stageIdx, tex2d, texCube, texVol, smp, uv, isCube, isVol, float4(0.0f, 0.0f, 0.0f, 0.0f));\n"
    "}\n\n"
    "float ComputeFogFactor(float d) {\n"
    "    if (uFogParams.x <= 0.5f || (uFlags.x & 1u) != 0u) return 1.0f;\n"
    "    float fogMode = uFogParams.x;\n"
    "    if (fogMode > 3.5f) {\n"
    "        return saturate(d);\n"
    "    } else if (fogMode > 2.5f) {\n"
    "        float span = max(uFogParams.z - uFogParams.y, 0.001f);\n"
    "        return saturate((uFogParams.z - d) / span);\n"
    "    } else if (fogMode < 1.5f) {\n"
    "        return saturate(exp(-d * uFogParams.w));\n"
    "    } else {\n"
    "        float df = d * uFogParams.w;\n"
    "        return saturate(exp(-(df * df)));\n"
    "    }\n"
    "}\n\n"
    "float ComputeInvProjQ(float4 uv, bool isProj) {\n"
    "    if (!isProj) return 1.0f;\n"
    "    return (abs(uv.w) > 1e-6f) ? (1.0f / uv.w) : 1.0f;\n"
    "}\n\n";

static const char* kSharedPsPrologueHLSL =
    "float4 PSMain(PSIn input) : SV_TARGET {\n"
    "    uint scissorCount = (uFlags.x >> 4u) & 0x0Fu;\n"
    "    if (scissorCount > 0u) {\n"
    "        bool insideAny = false;\n"
    "        [unroll]\n"
    "        for (uint si = 0u; si < 8u; ++si) {\n"
    "            if (si < scissorCount) {\n"
    "                float4 sr = uScissorRects[si];\n"
    "                if (input.pos.x >= sr.x && input.pos.x < sr.z &&\n"
    "                    input.pos.y >= sr.y && input.pos.y < sr.w) {\n"
    "                    insideAny = true;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        bool scissorExcl = (uFlags.x & 4u) != 0u;\n"
    "        if (scissorExcl ? insideAny : !insideAny) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n\n"
    "    float4 diffuse = input.color;\n"
    "    float4 specCol = float4(input.norm, 1.0f);\n"
    "    float4 current = diffuse;\n"
    "    float4 tempReg = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\n"
    "    uint texMask  = uFlags.y & 0x0Fu;\n"
    "    uint cubeMask = (uFlags.y >> 4u)  & 0x0Fu;\n"
    "    uint projMask = (uFlags.y >> 8u)  & 0x0Fu;\n"
    "    uint volMask  = (uFlags.y >> 12u) & 0x0Fu;\n"
    "    uint tempMask = (uFlags.y >> 20u) & 0x0Fu;\n\n"
    "    float invProjQ0 = ComputeInvProjQ(input.uv0, (projMask & 1u) != 0u);\n"
    "    float invProjQ1 = ComputeInvProjQ(input.uv1, (projMask & 2u) != 0u);\n"
    "    float invProjQ2 = ComputeInvProjQ(input.uv2, (projMask & 4u) != 0u);\n"
    "    float invProjQ3 = ComputeInvProjQ(input.uv3, (projMask & 8u) != 0u);\n\n"
    "    float4 uv0 = ((projMask & 1u) != 0u) ? float4(input.uv0.xyz * invProjQ0, input.uv0.w) : input.uv0;\n"
    "    float4 uv1 = ((projMask & 2u) != 0u) ? float4(input.uv1.xyz * invProjQ1, input.uv1.w) : input.uv1;\n"
    "    float4 uv2 = ((projMask & 4u) != 0u) ? float4(input.uv2.xyz * invProjQ2, input.uv2.w) : input.uv2;\n"
    "    float4 uv3 = ((projMask & 8u) != 0u) ? float4(input.uv3.xyz * invProjQ3, input.uv3.w) : input.uv3;\n\n"
    "    float4 t0 = ((texMask & 1u) != 0u) ? SampleStageTex(0u, tex0, cubeTex0, volTex0, smp0, uv0, (cubeMask & 1u) != 0u, (volMask & 1u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
    "    float4 t1 = ((texMask & 2u) != 0u) ? SampleStageTex(1u, tex1, cubeTex1, volTex1, smp1, uv1, (cubeMask & 2u) != 0u, (volMask & 2u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n";

static const char* kSharedPsAlphaTestAndGammaHLSL =
    "    if (uFlags.z != 0u) {\n"
    "        uint fn = (uint)uAlphaTest.x;\n"
    "        float refVal = uAlphaTest.y;\n"
    "        bool passAlpha = true;\n"
    "        if (fn == 1u)      passAlpha = false;\n"
    "        else if (fn == 2u) passAlpha = (current.a <  refVal - 0.001f);\n"
    "        else if (fn == 3u) passAlpha = (abs(current.a - refVal) < 0.004f);\n"
    "        else if (fn == 4u) passAlpha = (current.a <= refVal + 0.001f);\n"
    "        else if (fn == 5u) passAlpha = (current.a >  refVal + 0.001f);\n"
    "        else if (fn == 6u) passAlpha = (abs(current.a - refVal) >= 0.004f);\n"
    "        else if (fn == 7u) passAlpha = (current.a >= refVal - 0.001f);\n"
    "        if (!passAlpha) discard;\n"
    "    }\n";

static const char* kSharedPsFogHLSL =
    "    if (uFogParams.x > 0.5f && (uFlags.x & 1u) == 0u) {\n"
    "        float fogAmt = 1.0f - fogFactor;\n"
    "        if (uFogColor.w > 0.5f) {\n"
    "            current.rgb *= fogFactor;\n"
    "        } else {\n"
    "            current.rgb = lerp(current.rgb, uFogColor.rgb, fogAmt);\n"
    "        }\n"
    "    }\n";

static const char* kSharedPsEpilogueHLSL =
    "    return current;\n"
    "}\n";

static std::string BuildFixedVsHLSL() {
    std::string s;
    s.reserve(10240);
    s += "// ============================================================================\n"
         "// StarCraft: Ghost (" GHOST_STR(GHOST_PROJECT_NAME) ") — Fixed-Function + GPU Skinning Vertex Shader (vs_4_0)\n"
         "// ============================================================================\n\n";
    s += kSharedPerDrawCBAndPSInHLSL;
    s += "cbuffer BonePaletteCB : register(b2) {\n"
         "    float4 uBoneRows[768];\n"
         "};\n\n"
         "struct VSIn {\n"
         "    float4 pos   : POSITION;\n"
         "    float3 norm  : NORMAL;\n"
         "    float4 color : COLOR0;\n"
         "    float2 uv0   : TEXCOORD0;\n"
         "    float2 uv1   : TEXCOORD1;\n"
         "    float2 uv2   : TEXCOORD2;\n"
         "    float2 uv3   : TEXCOORD3;\n"
         "    float4 uvw_w : TEXCOORD4;\n"
         "    float4 spec  : COLOR1;\n"
         "    float4 bw    : BLENDWEIGHT0;\n"
         "    uint4  bi    : BLENDINDICES0;\n"
         "    float4 uvw_q : TEXCOORD5;\n"
         "};\n\n"
         "PSIn VSMain(VSIn input) {\n"
         "    PSIn o;\n"
         "    bool isRhw = (uFlags.x & 1u) != 0u;\n"
         "    float3 localPos  = input.pos.xyz;\n"
         "    float3 localNorm = input.norm;\n"
         "    if (uSkinParams.x > 0u && !isRhw) {\n"
         "        float3 sp = float3(0.0f, 0.0f, 0.0f);\n"
         "        float3 sn = float3(0.0f, 0.0f, 0.0f);\n"
         "        float  weights[4] = { input.bw.x, input.bw.y, input.bw.z, input.bw.w };\n"
         "        uint   indices[4] = { input.bi.x, input.bi.y, input.bi.z, input.bi.w };\n"
         "        uint   wCount     = min(uSkinParams.x, 4u);\n"
         "        [unroll]\n"
         "        for (uint wi = 0u; wi < 4u; ++wi) {\n"
         "            if (wi < wCount) {\n"
         "                float  w     = weights[wi];\n"
         "                uint   rBase = min(indices[wi], 255u) * 3u;\n"
         "                float4 r0    = uBoneRows[rBase + 0u];\n"
         "                float4 r1    = uBoneRows[rBase + 1u];\n"
         "                float4 r2    = uBoneRows[rBase + 2u];\n"
         "                sp += w * float3(dot(float4(localPos, 1.0f), r0),\n"
         "                                 dot(float4(localPos, 1.0f), r1),\n"
         "                                 dot(float4(localPos, 1.0f), r2));\n"
         "                sn += w * float3(dot(localNorm, r0.xyz),\n"
         "                                 dot(localNorm, r1.xyz),\n"
         "                                 dot(localNorm, r2.xyz));\n"
         "            }\n"
         "        }\n"
         "        localPos  = sp;\n"
         "        localNorm = sn;\n"
         "    }\n"
         "    if (isRhw) {\n"
         "        float vpW = max(uViewport.z, 1.0f);\n"
         "        float vpH = max(uViewport.w, 1.0f);\n"
         "        float sx = input.pos.x + uScreenSpaceParams.x;\n"
         "        float sy = input.pos.y + uScreenSpaceParams.y;\n"
         "        float ndcX = ((sx - uViewport.x) / vpW) * 2.0f - 1.0f;\n"
         "        float ndcY = 1.0f - ((sy - uViewport.y) / vpH) * 2.0f;\n"
         "        float zVal = input.pos.z;\n"
         "        if (zVal > 1.0f) zVal /= max(uScreenSpaceParams.z, 1.0f);\n"
         "        float zNorm = saturate(zVal);\n"
         "        float rhw = input.pos.w;\n"
         "        float clipW = (rhw > 1e-6f && rhw < 1e6f) ? (1.0f / rhw) : 1.0f;\n"
         "        o.pos = float4(ndcX * clipW, ndcY * clipW, zNorm * clipW, clipW);\n"
         "        o.eyeDist = float2(0.0f, rhw);\n"
         "    } else {\n"
         "        float4 clipPos = localPos.x * uWvpRow0 +\n"
         "                         localPos.y * uWvpRow1 +\n"
         "                         localPos.z * uWvpRow2 +\n"
         "                         uWvpRow3;\n"
         "        o.pos = clipPos;\n"
         "        o.eyeDist = float2(max(clipPos.w, 0.0f), input.pos.w);\n"
         "    }\n"
         "    if (uLightCount.y != 0u && !isRhw) {\n"
         "        float3 wPos = localPos.x * uWorldRow0.xyz +\n"
         "                      localPos.y * uWorldRow1.xyz +\n"
         "                      localPos.z * uWorldRow2.xyz +\n"
         "                      uWorldRow3.xyz;\n"
         "        float3 wNorm = localNorm.x * uWorldRow0.xyz +\n"
         "                       localNorm.y * uWorldRow1.xyz +\n"
         "                       localNorm.z * uWorldRow2.xyz;\n"
         "        float nLen = length(wNorm);\n"
         "        if (nLen > 0.0001f) wNorm /= nLen;\n"
         "        bool specEnable  = (uLightCount.z & 1u) != 0u;\n"
         "        bool localViewer = (uLightCount.z & 2u) != 0u;\n"
         "        float specPower  = max(uEyePosWorld.w, 0.001f);\n"
         "        float3 V = localViewer ? normalize(uEyePosWorld.xyz - wPos) : normalize(uEyePosWorld.xyz);\n"
         "        float3 totRgb  = input.color.rgb;\n"
         "        float3 totSpec = input.spec.rgb;\n"
         "        uint numLights = min(uLightCount.x, 8u);\n"
         "        for (uint l = 0u; l < numLights; ++l) {\n"
         "            float4 posType = uLightPosType[l];\n"
         "            float lType = posType.w;\n"
         "            float3 L = float3(0.0f, 0.0f, 0.0f);\n"
         "            float atten = 1.0f;\n"
         "            if (lType > 2.5f) {\n"
         "                L = posType.xyz;\n"
         "            } else {\n"
         "                float3 toLight = posType.xyz - wPos;\n"
         "                float dist = length(toLight);\n"
         "                float range = uLightDirRange[l].w;\n"
         "                if (range > 0.0f && dist > range) continue;\n"
         "                if (dist > 0.0001f) L = toLight / dist;\n"
         "                float4 attSpot = uLightAttenSpot[l];\n"
         "                float denom = attSpot.x + attSpot.y * dist + attSpot.z * dist * dist;\n"
         "                if (denom > 0.0001f) atten = 1.0f / denom;\n"
         "                if (lType > 1.5f) {\n"
         "                    float cosAlpha = -dot(L, uLightDirRange[l].xyz);\n"
         "                    float cosHalfPhi = attSpot.w;\n"
         "                    if (cosAlpha <= cosHalfPhi) continue;\n"
         "                    float cosHalfTheta = uLightSpotParams[l].x;\n"
         "                    if (cosAlpha < cosHalfTheta && (cosHalfTheta - cosHalfPhi) > 0.0001f) {\n"
         "                        atten *= pow(max((cosAlpha - cosHalfPhi) / (cosHalfTheta - cosHalfPhi), 0.0f), uLightSpotParams[l].y);\n"
         "                    }\n"
         "                }\n"
         "            }\n"
         "            float ndotl = max(dot(wNorm, L), 0.0f);\n"
         "            totRgb += atten * (uLightAmbient[l].rgb + ndotl * uLightDiffuse[l].rgb);\n"
         "            if (specEnable && ndotl > 0.0f) {\n"
         "                float3 H = normalize(L + V);\n"
         "                float ndoth = max(dot(wNorm, H), 0.0f);\n"
         "                totSpec += atten * pow(ndoth, specPower) * uLightSpecular[l].rgb;\n"
         "            }\n"
         "        }\n"
         "        o.color = float4(saturate(totRgb), input.color.a);\n"
         "        o.norm  = specEnable ? saturate(totSpec) : ((dot(abs(input.spec.rgb), float3(1.0f, 1.0f, 1.0f)) > 0.0f) ? input.spec.rgb : localNorm);\n"
         "    } else {\n"
         "        o.color = input.color;\n"
         "        o.norm  = (dot(abs(input.spec.rgb), float3(1.0f, 1.0f, 1.0f)) > 0.0f) ? input.spec.rgb : localNorm;\n"
         "    }\n"
         "    float4 qv = float4(\n"
         "        (abs(input.uvw_q.x) > 1e-6f) ? input.uvw_q.x : 1.0f,\n"
         "        (abs(input.uvw_q.y) > 1e-6f) ? input.uvw_q.y : 1.0f,\n"
         "        (abs(input.uvw_q.z) > 1e-6f) ? input.uvw_q.z : 1.0f,\n"
         "        (abs(input.uvw_q.w) > 1e-6f) ? input.uvw_q.w : 1.0f);\n"
         "    o.uv0    = float4(input.uv0 * uUvScale01.xy, input.uvw_w.x, qv.x);\n"
         "    o.uv1    = float4(input.uv1 * uUvScale01.zw, input.uvw_w.y, qv.y);\n"
         "    o.uv2    = float4(input.uv2 * uUvScale23.xy, input.uvw_w.z, qv.z);\n"
         "    o.uv3    = float4(input.uv3 * uUvScale23.zw, input.uvw_w.w, qv.w);\n"
         "    o.eyeVec = float3(0.0f, 0.0f, 0.0f);\n"
         "    return o;\n"
         "}\n";
    return s;
}

static std::string BuildFixedPsHLSL() {
    std::string s;
    s.reserve(12288);
    s += "// ============================================================================\n"
         "// StarCraft: Ghost (" GHOST_STR(GHOST_PROJECT_NAME) ") — Fixed-Function Pixel Shader (ps_4_0)\n"
         "// ============================================================================\n\n";
    s += kSharedPerDrawCBAndPSInHLSL;
    s += kSharedPsResourcesAndHelpersHLSL;
    s += "float4 EvalArg(uint arg, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 spec, float4 tempReg) {\n"
         "    uint src = arg & 0x0Fu;\n"
         "    float4 v = current;\n"
         "    if (src == 0u)      v = diffuse;\n"
         "    else if (src == 1u) v = current;\n"
         "    else if (src == 2u) v = tex;\n"
         "    else if (src == 3u) v = tfactor;\n"
         "    else if (src == 4u) v = spec;\n"
         "    else if (src == 5u) v = tempReg;\n"
         "    if ((arg & 0x10u) != 0u) v = 1.0f - v;\n"
         "    if ((arg & 0x20u) != 0u) v = v.aaaa;\n"
         "    return v;\n"
         "}\n\n"
         "float3 EvalColorOp(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {\n"
         "    if (op == 2u)  return a1.rgb;\n"
         "    if (op == 3u)  return a2.rgb;\n"
         "    if (op == 4u)  return a1.rgb * a2.rgb;\n"
         "    if (op == 5u)  return saturate(a1.rgb * a2.rgb * 2.0f);\n"
         "    if (op == 6u)  return saturate(a1.rgb * a2.rgb * 4.0f);\n"
         "    if (op == 7u)  return saturate(a1.rgb + a2.rgb);\n"
         "    if (op == 8u)  return saturate(a1.rgb + a2.rgb - 0.5f);\n"
         "    if (op == 9u)  return saturate((a1.rgb + a2.rgb - 0.5f) * 2.0f);\n"
         "    if (op == 10u) return saturate(a1.rgb - a2.rgb);\n"
         "    if (op == 11u) return saturate(a1.rgb + a2.rgb - a1.rgb * a2.rgb);\n"
         "    if (op == 12u) return lerp(a2.rgb, a1.rgb, diffuse.a);\n"
         "    if (op == 13u) return lerp(a2.rgb, a1.rgb, current.a);\n"
         "    if (op == 14u) return lerp(a2.rgb, a1.rgb, tex.a);\n"
         "    if (op == 15u) return lerp(a2.rgb, a1.rgb, tfactor.a);\n"
         "    if (op == 16u) return saturate(a1.rgb + a2.rgb * (1.0f - tex.a));\n"
         "    if (op == 17u) return saturate(a1.rgb + a2.rgb * (1.0f - current.a));\n"
         "    if (op == 18u) return saturate(a1.rgb + a1.a * a2.rgb);\n"
         "    if (op == 19u) return saturate(a1.rgb * a2.rgb + a1.a);\n"
         "    if (op == 20u) return saturate(a1.rgb + (1.0f - a1.a) * a2.rgb);\n"
         "    if (op == 21u) return saturate((1.0f - a1.rgb) * a2.rgb + a1.a);\n"
         "    if (op == 22u) return saturate(dot((a1.rgb - 0.5f) * 2.0f, (a2.rgb - 0.5f) * 2.0f)).xxx;\n"
         "    if (op == 23u) return saturate(a0.rgb + a1.rgb * a2.rgb);\n"
         "    if (op == 24u) return lerp(a2.rgb, a1.rgb, a0.rgb);\n"
         "    if (op == 25u || op == 26u) return current.rgb;\n"
         "    return a1.rgb * a2.rgb;\n"
         "}\n\n"
         "float EvalAlphaOp(uint op, float a1, float a2, float a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {\n"
         "    if (op == 2u)  return a1;\n"
         "    if (op == 3u)  return a2;\n"
         "    if (op == 4u)  return a1 * a2;\n"
         "    if (op == 5u)  return saturate(a1 * a2 * 2.0f);\n"
         "    if (op == 6u)  return saturate(a1 * a2 * 4.0f);\n"
         "    if (op == 7u)  return saturate(a1 + a2);\n"
         "    if (op == 8u)  return saturate(a1 + a2 - 0.5f);\n"
         "    if (op == 9u)  return saturate((a1 + a2 - 0.5f) * 2.0f);\n"
         "    if (op == 10u) return saturate(a1 - a2);\n"
         "    if (op == 11u) return saturate(a1 + a2 - a1 * a2);\n"
         "    if (op == 12u) return lerp(a2, a1, diffuse.a);\n"
         "    if (op == 13u) return lerp(a2, a1, current.a);\n"
         "    if (op == 14u) return lerp(a2, a1, tex.a);\n"
         "    if (op == 15u) return lerp(a2, a1, tfactor.a);\n"
         "    if (op == 16u) return saturate(a1 + a2 * (1.0f - tex.a));\n"
         "    if (op == 17u) return saturate(a1 + a2 * (1.0f - current.a));\n"
         "    if (op == 23u) return saturate(a0 + a1 * a2);\n"
         "    if (op == 24u) return lerp(a2, a1, a0);\n"
         "    return a1 * a2;\n"
         "}\n\n"
         "bool OpUsesMissingTex(uint4 ops, bool hasStageTex) {\n"
         "    if (hasStageTex) return false;\n"
         "    uint op = ops.x;\n"
         "    if (op <= 1u || op == 25u || op == 26u) return false;\n"
         "    if (op == 14u || op == 16u) return true;\n"
         "    if (op != 3u && (ops.y & 0x0Fu) == 2u) return true;\n"
         "    if (op != 2u && op != 17u && (ops.z & 0x0Fu) == 2u) return true;\n"
         "    if ((op == 23u || op == 24u) && (ops.w & 0x0Fu) == 2u) return true;\n"
         "    return false;\n"
         "}\n\n"
         "void EvalStage(\n"
         "    uint4 colorOps, uint4 alphaOps, float4 diffuse, inout float4 current, inout float4 tempReg,\n"
         "    float4 tex, float4 tfactor, float4 spec, bool writeToTemp, bool hasStageTex, bool isStage0)\n"
         "{\n"
         "    bool missingColorTex = OpUsesMissingTex(colorOps, hasStageTex);\n"
         "    float3 outRgb;\n"
         "    if (missingColorTex) {\n"
         "        outRgb = isStage0 ? diffuse.rgb : current.rgb;\n"
         "    } else {\n"
         "        float4 c1 = EvalArg(colorOps.y, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        float4 c2 = EvalArg(colorOps.z, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        float4 c0 = EvalArg(colorOps.w, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        outRgb = EvalColorOp(colorOps.x, c1, c2, c0, diffuse, current, tex, tfactor);\n"
         "    }\n"
         "    float outA = (colorOps.x == 22u && !missingColorTex)\n"
         "               ? outRgb.x\n"
         "               : (writeToTemp ? tempReg.a : current.a);\n"
         "    if (alphaOps.x > 1u && colorOps.x != 22u) {\n"
         "        if (OpUsesMissingTex(alphaOps, hasStageTex)) {\n"
         "            outA = isStage0 ? diffuse.a : current.a;\n"
         "        } else {\n"
         "            float4 a1 = EvalArg(alphaOps.y, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            float4 a2 = EvalArg(alphaOps.z, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            float4 a0 = EvalArg(alphaOps.w, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            outA = EvalAlphaOp(alphaOps.x, a1.a, a2.a, a0.a, diffuse, current, tex, tfactor);\n"
         "        }\n"
         "    }\n"
         "    if (writeToTemp) {\n"
         "        tempReg = float4(outRgb, outA);\n"
         "    } else {\n"
         "        current = float4(outRgb, outA);\n"
         "    }\n"
         "}\n\n";
    s += kSharedPsPrologueHLSL;
    s += "    if (uStage0Ops.x == 25u || uStage0Ops.x == 26u) {\n"
         "        float2 d = ((uStageColorSign[0] & 0x60u) != 0u) ? t0.rg : (t0.rg * 2.0f - 1.0f);\n"
         "        float4 bm = any(uStageBumpMat[0] != 0.0f) ? uStageBumpMat[0] : uStageBumpMat[1];\n"
         "        float2 bl = any(uStageBumpLum[0].xy != 0.0f) ? uStageBumpLum[0].xy : uStageBumpLum[1].xy;\n"
         "        uv1.xy += float2(d.x * bm.x + d.y * bm.z, d.x * bm.y + d.y * bm.w);\n"
         "        if ((texMask & 2u) != 0u) {\n"
         "            t1 = SampleStageTex(1u, tex1, cubeTex1, volTex1, smp1, uv1, (cubeMask & 2u) != 0u, (volMask & 2u) != 0u);\n"
         "        }\n"
         "        if (uStage0Ops.x == 26u && (bl.x != 0.0f || bl.y != 0.0f)) {\n"
         "            t1.rgb *= saturate(t0.b * bl.x + bl.y);\n"
         "        }\n"
         "    }\n"
         "    if (uStage1Ops.x == 25u || uStage1Ops.x == 26u) {\n"
         "        float2 d = ((uStageColorSign[1] & 0x60u) != 0u) ? t1.rg : (t1.rg * 2.0f - 1.0f);\n"
         "        float4 bm = any(uStageBumpMat[1] != 0.0f) ? uStageBumpMat[1] : uStageBumpMat[2];\n"
         "        uv2.xy += float2(d.x * bm.x + d.y * bm.z, d.x * bm.y + d.y * bm.w);\n"
         "    }\n"
         "    float4 t2 = ((texMask & 4u) != 0u) ? SampleStageTex(2u, tex2, cubeTex2, volTex2, smp2, uv2, (cubeMask & 4u) != 0u, (volMask & 4u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    if (uStage1Ops.x == 26u) {\n"
         "        float2 bl = any(uStageBumpLum[1].xy != 0.0f) ? uStageBumpLum[1].xy : uStageBumpLum[2].xy;\n"
         "        if (bl.x != 0.0f || bl.y != 0.0f) {\n"
         "            t2.rgb *= saturate(t1.b * bl.x + bl.y);\n"
         "        }\n"
         "    }\n"
         "    if (uStage2Ops.x == 25u || uStage2Ops.x == 26u) {\n"
         "        float2 d = ((uStageColorSign[2] & 0x60u) != 0u) ? t2.rg : (t2.rg * 2.0f - 1.0f);\n"
         "        float4 bm = any(uStageBumpMat[2] != 0.0f) ? uStageBumpMat[2] : uStageBumpMat[3];\n"
         "        uv3.xy += float2(d.x * bm.x + d.y * bm.z, d.x * bm.y + d.y * bm.w);\n"
         "    }\n"
         "    float4 t3 = ((texMask & 8u) != 0u) ? SampleStageTex(3u, tex3, cubeTex3, volTex3, smp3, uv3, (cubeMask & 8u) != 0u, (volMask & 8u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    if (uStage2Ops.x == 26u) {\n"
         "        float2 bl = any(uStageBumpLum[2].xy != 0.0f) ? uStageBumpLum[2].xy : uStageBumpLum[3].xy;\n"
         "        if (bl.x != 0.0f || bl.y != 0.0f) {\n"
         "            t3.rgb *= saturate(t2.b * bl.x + bl.y);\n"
         "        }\n"
         "    }\n"
         "    float fogFactor = ComputeFogFactor(input.eyeDist.x);\n\n"
         "    if (uStage0Ops.x > 1u) {\n"
         "        EvalStage(uStage0Ops, uStage0Alpha, diffuse, current, tempReg, t0, uTFactor, specCol, (tempMask & 1u) != 0u, (texMask & 1u) != 0u, true);\n"
         "        if (uStage1Ops.x > 1u) {\n"
         "            EvalStage(uStage1Ops, uStage1Alpha, diffuse, current, tempReg, t1, uTFactor, specCol, (tempMask & 2u) != 0u, (texMask & 2u) != 0u, false);\n"
         "            if (uStage2Ops.x > 1u) {\n"
         "                EvalStage(uStage2Ops, uStage2Alpha, diffuse, current, tempReg, t2, uTFactor, specCol, (tempMask & 4u) != 0u, (texMask & 4u) != 0u, false);\n"
         "                if (uStage3Ops.x > 1u) {\n"
         "                    EvalStage(uStage3Ops, uStage3Alpha, diffuse, current, tempReg, t3, uTFactor, specCol, (tempMask & 8u) != 0u, (texMask & 8u) != 0u, false);\n"
         "                }\n"
         "            }\n"
         "        }\n"
         "    }\n"
         "    if ((uFlags.x & 8u) != 0u || (uLightCount.z & 1u) != 0u) {\n"
         "        current.rgb = saturate(current.rgb + specCol.rgb);\n"
         "    }\n\n";
    s += kSharedPsAlphaTestAndGammaHLSL;
    s += kSharedPsFogHLSL;
    s += kSharedPsEpilogueHLSL;
    return s;
}

// ============================================================================
// Universal Xbox NV2A Pixel Shader (D3DPIXELSHADERDEF / Inline Combiners -> HLSL ps_4_0)
// ============================================================================

static uint64_t Fnv1aMixU32(uint64_t h, uint32_t v) {
    h ^= static_cast<uint64_t>(v);
    h *= 1099511628211ULL;
    return h;
}

static uint64_t ComputeXboxPsStructuralHash(const XboxPixelShaderDef& def) {
    uint64_t h = 14695981039346656037ULL;
    h = Fnv1aMixU32(h, 0x4E563241u); // "NV2A"
    h = Fnv1aMixU32(h, 0x50533034u); // "PS04"

    uint32_t cc = def.PSCombinerCount & 0x0001110Fu;
    uint32_t numStages = cc & 0xFu;
    if (numStages > 8u) numStages = 8u;

    h = Fnv1aMixU32(h, cc);
    h = Fnv1aMixU32(h, def.PSTextureModes & 0x000FFFFFu);
    h = Fnv1aMixU32(h, def.PSDotMapping   & 0x00000777u);
    h = Fnv1aMixU32(h, def.PSInputTexture & 0x00FFF000u);
    h = Fnv1aMixU32(h, def.PSCompareMode  & 0x0000FFFFu);
    h = Fnv1aMixU32(h, def.PSFinalCombinerInputsABCD);
    h = Fnv1aMixU32(h, def.PSFinalCombinerInputsEFG);

    for (uint32_t st = 0; st < numStages; ++st) {
        h = Fnv1aMixU32(h, def.PSRGBInputs[st]);
        h = Fnv1aMixU32(h, def.PSAlphaInputs[st]);
        h = Fnv1aMixU32(h, def.PSRGBOutputs[st]);
        h = Fnv1aMixU32(h, def.PSAlphaOutputs[st]);
    }
    return h;
}

static const char* GetGcRegNameInStage(uint32_t reg) {
    switch (reg & 0xFu) {
        case 0x1u: return "c0";
        case 0x2u: return "c1";
        case 0x3u: return "fogReg";
        case 0x4u: return "v0_in";
        case 0x5u: return "v1_in";
        case 0x8u: return "t0_in";
        case 0x9u: return "t1_in";
        case 0xAu: return "t2_in";
        case 0xBu: return "t3_in";
        case 0xCu: return "r0_in";
        case 0xDu: return "r1_in";
        default:   return "float4(0.0f, 0.0f, 0.0f, 0.0f)";
    }
}

static const char* GetGcDstRegName(uint32_t reg) {
    switch (reg & 0xFu) {
        case 0x4u: return "diffuse";
        case 0x5u: return "specCol";
        case 0x8u: return "t0";
        case 0x9u: return "t1";
        case 0xAu: return "t2";
        case 0xBu: return "t3";
        case 0xCu: return "r0";
        case 0xDu: return "r1";
        default:   return nullptr;
    }
}

static void FormatGcRgbInputExpr(uint32_t inByte, char* outBuf, size_t outSize) {
    uint32_t reg  = inByte & 0xFu;
    uint32_t chan = (inByte >> 4) & 1u;
    uint32_t map  = (inByte >> 5) & 7u;
    const char* regStr  = GetGcRegNameInStage(reg);
    const char* swzStr  = chan ? ".aaa" : ".rgb";
    if (reg == 0u) {
        snprintf(outBuf, outSize, "MapGcInput3(float3(0.0f, 0.0f, 0.0f), %uu)", map);
    } else {
        snprintf(outBuf, outSize, "MapGcInput3(%s%s, %uu)", regStr, swzStr, map);
    }
}

static void FormatGcAlphaInputExpr(uint32_t inByte, char* outBuf, size_t outSize) {
    uint32_t reg  = inByte & 0xFu;
    uint32_t chan = (inByte >> 4) & 1u;
    uint32_t map  = (inByte >> 5) & 7u;
    const char* regStr  = GetGcRegNameInStage(reg);
    const char* swzStr  = chan ? ".a" : ".b";
    if (reg == 0u) {
        snprintf(outBuf, outSize, "MapGcInput1(0.0f, %uu)", map);
    } else {
        snprintf(outBuf, outSize, "MapGcInput1(%s%s, %uu)", regStr, swzStr, map);
    }
}

static void FormatFcRgbInputExpr(uint32_t inByte, bool clampSum, char* outBuf, size_t outSize) {
    uint32_t reg  = inByte & 0xFu;
    uint32_t chan = (inByte >> 4) & 1u;
    uint32_t map  = (inByte >> 5) & 7u;

    char rawExpr[128];
    switch (reg) {
        case 0x0u: snprintf(rawExpr, sizeof(rawExpr), "float3(0.0f, 0.0f, 0.0f)"); break;
        case 0x1u: snprintf(rawExpr, sizeof(rawExpr), "uPsFcConst[0].%s", chan ? "aaa" : "rgb"); break;
        case 0x2u: snprintf(rawExpr, sizeof(rawExpr), "uPsFcConst[1].%s", chan ? "aaa" : "rgb"); break;
        case 0x3u: snprintf(rawExpr, sizeof(rawExpr), "fogReg.%s",        chan ? "aaa" : "rgb"); break;
        case 0x4u: snprintf(rawExpr, sizeof(rawExpr), "diffuse.%s",       chan ? "aaa" : "rgb"); break;
        case 0x5u: snprintf(rawExpr, sizeof(rawExpr), "specCol.%s",       chan ? "aaa" : "rgb"); break;
        case 0x8u: snprintf(rawExpr, sizeof(rawExpr), "t0.%s",            chan ? "aaa" : "rgb"); break;
        case 0x9u: snprintf(rawExpr, sizeof(rawExpr), "t1.%s",            chan ? "aaa" : "rgb"); break;
        case 0xAu: snprintf(rawExpr, sizeof(rawExpr), "t2.%s",            chan ? "aaa" : "rgb"); break;
        case 0xBu: snprintf(rawExpr, sizeof(rawExpr), "t3.%s",            chan ? "aaa" : "rgb"); break;
        case 0xCu: snprintf(rawExpr, sizeof(rawExpr), "r0.%s",            chan ? "aaa" : "rgb"); break;
        case 0xDu: snprintf(rawExpr, sizeof(rawExpr), "r1.%s",            chan ? "aaa" : "rgb"); break;
        case 0xEu: snprintf(rawExpr, sizeof(rawExpr), "%s",               chan ? "float3(0.0f, 0.0f, 0.0f)" : "v1r0Sum"); break;
        case 0xFu: snprintf(rawExpr, sizeof(rawExpr), "%s",               chan ? "float3(0.0f, 0.0f, 0.0f)" : "efProd"); break;
        default:   snprintf(rawExpr, sizeof(rawExpr), "float3(0.0f, 0.0f, 0.0f)"); break;
    }

    if ((map & 1u) != 0u) {
        snprintf(outBuf, outSize, "(1.0f - saturate(%s))", rawExpr);
    } else if (reg == 0xEu && !clampSum && !chan) {
        snprintf(outBuf, outSize, "max(%s, 0.0f)", rawExpr);
    } else {
        snprintf(outBuf, outSize, "saturate(%s)", rawExpr);
    }
}

static void FormatFcAlphaInputExpr(uint32_t inByte, char* outBuf, size_t outSize) {
    uint32_t reg  = inByte & 0xFu;
    uint32_t chan = (inByte >> 4) & 1u;
    uint32_t map  = (inByte >> 5) & 7u;

    char rawExpr[128];
    switch (reg) {
        case 0x0u: snprintf(rawExpr, sizeof(rawExpr), "0.0f"); break;
        case 0x1u: snprintf(rawExpr, sizeof(rawExpr), "uPsFcConst[0].%s", chan ? "a" : "b"); break;
        case 0x2u: snprintf(rawExpr, sizeof(rawExpr), "uPsFcConst[1].%s", chan ? "a" : "b"); break;
        case 0x3u: snprintf(rawExpr, sizeof(rawExpr), "fogReg.%s",        chan ? "a" : "b"); break;
        case 0x4u: snprintf(rawExpr, sizeof(rawExpr), "diffuse.%s",       chan ? "a" : "b"); break;
        case 0x5u: snprintf(rawExpr, sizeof(rawExpr), "specCol.%s",       chan ? "a" : "b"); break;
        case 0x8u: snprintf(rawExpr, sizeof(rawExpr), "t0.%s",            chan ? "a" : "b"); break;
        case 0x9u: snprintf(rawExpr, sizeof(rawExpr), "t1.%s",            chan ? "a" : "b"); break;
        case 0xAu: snprintf(rawExpr, sizeof(rawExpr), "t2.%s",            chan ? "a" : "b"); break;
        case 0xBu: snprintf(rawExpr, sizeof(rawExpr), "t3.%s",            chan ? "a" : "b"); break;
        case 0xCu: snprintf(rawExpr, sizeof(rawExpr), "r0.%s",            chan ? "a" : "b"); break;
        case 0xDu: snprintf(rawExpr, sizeof(rawExpr), "r1.%s",            chan ? "a" : "b"); break;
        case 0xEu: snprintf(rawExpr, sizeof(rawExpr), "%s",               chan ? "0.0f" : "v1r0Sum.b"); break;
        case 0xFu: snprintf(rawExpr, sizeof(rawExpr), "%s",               chan ? "0.0f" : "efProd.b"); break;
        default:   snprintf(rawExpr, sizeof(rawExpr), "0.0f"); break;
    }

    if ((map & 1u) != 0u) {
        snprintf(outBuf, outSize, "(1.0f - saturate(%s))", rawExpr);
    } else {
        snprintf(outBuf, outSize, "saturate(%s)", rawExpr);
    }
}

static const char* GetGcScaleSuffix(uint32_t scaleCode) {
    switch (scaleCode & 3u) {
        case 1u: return " * 2.0f";
        case 2u: return " * 4.0f";
        case 3u: return " * 0.5f";
        default: return "";
    }
}

static std::string CompileXboxPixelShaderDefToHLSL(const XboxPixelShaderDef& def, const char* debugLabel = nullptr) {
    uint32_t numStages = def.PSCombinerCount & 0xFu;
    if (numStages > 8u) numStages = 8u;

    uint32_t tm[4] = {
        (def.PSTextureModes >>  0) & 0x1Fu,
        (def.PSTextureModes >>  5) & 0x1Fu,
        (def.PSTextureModes >> 10) & 0x1Fu,
        (def.PSTextureModes >> 15) & 0x1Fu
    };
    bool writesCustomDepth = (tm[0] == 0x0Au || tm[1] == 0x0Au || tm[2] == 0x0Au || tm[3] == 0x0Au);

    std::string s;
    s.reserve(12288);
    char line[2048];
    snprintf(line, sizeof(line),
        "// ============================================================================\n"
        "// Compiled Xbox NV2A Pixel Shader (D3DPIXELSHADERDEF -> D3D11 HLSL ps_4_0)\n"
        "// Source: %s\n"
        "// PSCombinerCount: 0x%08X (%u stages) | PSTextureModes: 0x%08X\n"
        "// PSDotMapping: 0x%08X | PSInputTexture: 0x%08X | PSCompareMode: 0x%08X\n"
        "// FinalCombiner: ABCD=0x%08X EFG=0x%08X\n"
        "// ============================================================================\n\n",
        (debugLabel && debugLabel[0]) ? debugLabel : "Dynamic NV2A Combiner Program",
        def.PSCombinerCount, numStages, def.PSTextureModes,
        def.PSDotMapping, def.PSInputTexture, def.PSCompareMode,
        def.PSFinalCombinerInputsABCD, def.PSFinalCombinerInputsEFG);
    s += line;

    s += kSharedPerDrawCBAndPSInHLSL;
    s += kSharedPsResourcesAndHelpersHLSL;

    s += "float3 MapPsDotInput(float4 src, uint dotMap) {\n"
         "    if (dotMap == 0u || dotMap == 4u) return src.rgb;\n"
         "    if (dotMap == 1u || dotMap == 3u) return clamp((src.rgb - (128.0f / 255.0f)) * (255.0f / 127.0f), -1.0f, 1.0f);\n"
         "    if (dotMap == 2u) return clamp(src.rgb * 2.0f - 1.0f, -1.0f, 1.0f);\n"
         "    float2 hxy = src.rg * 2.0f - 1.0f;\n"
         "    return float3(hxy, sqrt(max(1.0f - dot(hxy, hxy), 0.0f)));\n"
         "}\n\n"
         "float3 MapGcInput3(float3 x, uint mapMode) {\n"
         "    if (mapMode == 0u) return saturate(x);\n"
         "    if (mapMode == 1u) return 1.0f - saturate(x);\n"
         "    if (mapMode == 2u) return 2.0f * saturate(x) - 1.0f;\n"
         "    if (mapMode == 3u) return 1.0f - 2.0f * saturate(x);\n"
         "    if (mapMode == 4u) return saturate(x) - 0.5f;\n"
         "    if (mapMode == 5u) return 0.5f - saturate(x);\n"
         "    if (mapMode == 6u) return clamp(x, -1.0f, 1.0f);\n"
         "    return -clamp(x, -1.0f, 1.0f);\n"
         "}\n\n"
         "float MapGcInput1(float x, uint mapMode) {\n"
         "    if (mapMode == 0u) return saturate(x);\n"
         "    if (mapMode == 1u) return 1.0f - saturate(x);\n"
         "    if (mapMode == 2u) return 2.0f * saturate(x) - 1.0f;\n"
         "    if (mapMode == 3u) return 1.0f - 2.0f * saturate(x);\n"
         "    if (mapMode == 4u) return saturate(x) - 0.5f;\n"
         "    if (mapMode == 5u) return 0.5f - saturate(x);\n"
         "    if (mapMode == 6u) return clamp(x, -1.0f, 1.0f);\n"
         "    return -clamp(x, -1.0f, 1.0f);\n"
         "}\n\n";

    if (writesCustomDepth) {
        s += "struct PSOut {\n"
             "    float4 color : SV_TARGET;\n"
             "    float  depth : SV_Depth;\n"
             "};\n\n"
             "PSOut PSMain(PSIn input) {\n"
             "    float customDepth = saturate(input.pos.z);\n";
    } else {
        s += "float4 PSMain(PSIn input) : SV_TARGET {\n";
    }

    s += "    uint scissorCount = (uFlags.x >> 4u) & 0x0Fu;\n"
         "    if (scissorCount > 0u) {\n"
         "        bool insideAny = false;\n"
         "        [unroll]\n"
         "        for (uint si = 0u; si < 8u; ++si) {\n"
         "            if (si < scissorCount) {\n"
         "                float4 sr = uScissorRects[si];\n"
         "                if (input.pos.x >= sr.x && input.pos.x < sr.z &&\n"
         "                    input.pos.y >= sr.y && input.pos.y < sr.w) {\n"
         "                    insideAny = true;\n"
         "                }\n"
         "            }\n"
         "        }\n"
         "        bool scissorExcl = (uFlags.x & 4u) != 0u;\n"
         "        if (scissorExcl ? insideAny : !insideAny) {\n"
         "            discard;\n"
         "        }\n"
         "    }\n\n"
         "    float4 diffuse = input.color;\n"
         "    float4 specCol = float4(input.norm, 1.0f);\n"
         "    float4 current = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\n"
         "    uint cubeMask = (uFlags.y >> 4u)  & 0x0Fu;\n"
         "    uint projMask = (uFlags.y >> 8u)  & 0x0Fu;\n"
         "    uint volMask  = (uFlags.y >> 12u) & 0x0Fu;\n\n"
         "    float invProjQ0 = ComputeInvProjQ(input.uv0, (projMask & 1u) != 0u);\n"
         "    float invProjQ1 = ComputeInvProjQ(input.uv1, (projMask & 2u) != 0u);\n"
         "    float invProjQ2 = ComputeInvProjQ(input.uv2, (projMask & 4u) != 0u);\n"
         "    float invProjQ3 = ComputeInvProjQ(input.uv3, (projMask & 8u) != 0u);\n\n";
    for (uint32_t st = 0; st < 4u; ++st) {
        uint32_t mask = 1u << st;
        if (tm[st] == 0x02u) {
            snprintf(line, sizeof(line),
                "    float4 uv%u = ((projMask & %uu) != 0u) ? float4(input.uv%u.xyz * invProjQ%u, input.uv%u.w) : input.uv%u;\n",
                st, mask, st, st, st, st);
        } else {
            snprintf(line, sizeof(line),
                "    float4 uv%u = ((projMask & %uu) != 0u) ? float4(input.uv%u.xy * invProjQ%u, input.uv%u.zw) : input.uv%u;\n",
                st, mask, st, st, st, st);
        }
        s += line;
    }
    s += "\n"
         "    float4 rawUv0 = float4((abs(uUvScale01.x) > 1e-6f) ? (input.uv0.x / uUvScale01.x) : input.uv0.x,\n"
         "                           (abs(uUvScale01.y) > 1e-6f) ? (input.uv0.y / uUvScale01.y) : input.uv0.y,\n"
         "                           input.uv0.zw);\n"
         "    float4 rawUv1 = float4((abs(uUvScale01.z) > 1e-6f) ? (input.uv1.x / uUvScale01.z) : input.uv1.x,\n"
         "                           (abs(uUvScale01.w) > 1e-6f) ? (input.uv1.y / uUvScale01.w) : input.uv1.y,\n"
         "                           input.uv1.zw);\n"
         "    float4 rawUv2 = float4((abs(uUvScale23.x) > 1e-6f) ? (input.uv2.x / uUvScale23.x) : input.uv2.x,\n"
         "                           (abs(uUvScale23.y) > 1e-6f) ? (input.uv2.y / uUvScale23.y) : input.uv2.y,\n"
         "                           input.uv2.zw);\n"
         "    float4 rawUv3 = float4((abs(uUvScale23.z) > 1e-6f) ? (input.uv3.x / uUvScale23.z) : input.uv3.x,\n"
         "                           (abs(uUvScale23.w) > 1e-6f) ? (input.uv3.y / uUvScale23.w) : input.uv3.y,\n"
         "                           input.uv3.zw);\n\n"
         "    float4 t0 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float4 t1 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float4 t2 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float4 t3 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float dotRes0 = 0.0f, dotRes1 = 0.0f, dotRes2 = 0.0f, dotRes3 = 0.0f;\n\n";

    static const char* const kUvScaleExpr[4] = {
        "uUvScale01.xy", "uUvScale01.zw", "uUvScale23.xy", "uUvScale23.zw"
    };

    // --- 4-Stage NV2A Texture Shader ---
    for (uint32_t st = 0; st < 4u; ++st) {
        uint32_t mode = tm[st];
        uint32_t mask = 1u << st;
        uint32_t inS  = 0u;
        if (st >= 1u) {
            // PS_INPUTTEXTURE(s1, s2, s3) = (s1 << 12) | (s2 << 16) | (s3 << 20) (verified at 0x006B3D5F = 0x00210000)
            inS = (def.PSInputTexture >> (12u + (st - 1u) * 4u)) & 0x3u;
            if (inS >= st) inS = st - 1u;
        }
        uint32_t dotMap = (st >= 1u) ? ((def.PSDotMapping >> ((st - 1u) * 4u)) & 0x7u) : 0u;

        switch (mode) {
            case 0x00u: // PS_TEXTUREMODES_NONE
                break;

            case 0x01u: // PS_TEXTUREMODES_PROJECT2D
            case 0x02u: // PS_TEXTUREMODES_PROJECT3D
            case 0x08u: // PS_TEXTUREMODES_BRDF
                snprintf(line, sizeof(line),
                    "    t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, uv%u, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n",
                    st, st, st, st, st, st, st, mask, mask);
                s += line;
                break;

            case 0x03u: // PS_TEXTUREMODES_CUBEMAP
                snprintf(line, sizeof(line),
                    "    t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, rawUv%u, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n",
                    st, st, st, st, st, st, st, mask, mask);
                s += line;
                break;

            case 0x04u: // PS_TEXTUREMODES_PASSTHRU
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float4 ptc = saturate(rawUv%u);\n"
                    "        uint cs = uStageColorSign[%uu];\n"
                    "        if ((cs & 0x20u) != 0u) ptc.r = ptc.r * 2.0f - 1.0f;\n"
                    "        if ((cs & 0x40u) != 0u) ptc.g = ptc.g * 2.0f - 1.0f;\n"
                    "        if ((cs & 0x80u) != 0u) ptc.b = ptc.b * 2.0f - 1.0f;\n"
                    "        if ((cs & 0x10u) != 0u) ptc.a = ptc.a * 2.0f - 1.0f;\n"
                    "        t%u = ptc;\n"
                    "    }\n",
                    st, st, st);
                s += line;
                break;

            case 0x05u: { // PS_TEXTUREMODES_CLIPPLANE
                uint32_t cmpNib = (def.PSCompareMode >> (st * 4u)) & 0xFu;
                static const char kCompChars[4] = { 'x', 'y', 'z', 'w' };
                for (uint32_t c = 0; c < 4u; ++c) {
                    bool cullGe = ((cmpNib >> c) & 1u) != 0u;
                    snprintf(line, sizeof(line),
                        "    if (rawUv%u.%c %s 0.0f) discard;\n",
                        st, kCompChars[c], cullGe ? ">=" : "<");
                    s += line;
                }
                break;
            }

            case 0x06u: // PS_TEXTUREMODES_BUMPENVMAP
            case 0x07u: // PS_TEXTUREMODES_BUMPENVMAP_LUM
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float2 d = t%u.rg;\n"
                    "        float4 bm = any(uStageBumpMat[%uu] != 0.0f) ? uStageBumpMat[%uu] : uStageBumpMat[%uu];\n"
                    "        float4 bUv = uv%u;\n"
                    "        bUv.xy += float2(d.x * bm.x + d.y * bm.z, d.x * bm.y + d.y * bm.w) * %s;\n"
                    "        t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, bUv, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n",
                    inS, st, st, inS, st, kUvScaleExpr[st],
                    st, st, st, st, st, st, mask, mask);
                s += line;
                if (mode == 0x07u) {
                    snprintf(line, sizeof(line),
                        "        float2 bl = any(uStageBumpLum[%uu].xy != 0.0f) ? uStageBumpLum[%uu].xy : uStageBumpLum[%uu].xy;\n"
                        "        t%u.rgb *= saturate(t%u.b * bl.x + bl.y);\n",
                        st, st, inS, st, inS);
                    s += line;
                }
                s += "    }\n";
                break;

            case 0x09u: // PS_TEXTUREMODES_DOT_ST
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        float2 stUv = float2(%s, dotRes%u) * %s;\n"
                    "        t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, float4(stUv, 0.0f, 1.0f), false, false);\n"
                    "    }\n",
                    inS, dotMap, st, st,
                    (st >= 1u) ? ((st == 1u) ? "dotRes0" : ((st == 2u) ? "dotRes1" : "dotRes2")) : "0.0f",
                    st, kUvScaleExpr[st],
                    st, st, st, st, st, st);
                s += line;
                break;

            case 0x0Au: // PS_TEXTUREMODES_DOT_ZW
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        float prevD = %s;\n"
                    "        customDepth = (abs(dotRes%u) > 1e-6f) ? saturate(prevD / dotRes%u) : saturate(input.pos.z);\n"
                    "    }\n",
                    inS, dotMap, st, st,
                    (st >= 1u) ? ((st == 1u) ? "dotRes0" : ((st == 2u) ? "dotRes1" : "dotRes2")) : "0.0f",
                    st, st);
                s += line;
                break;

            case 0x0Bu: { // PS_TEXTUREMODES_DOT_RFLCT_DIFF
                uint32_t inS3 = (def.PSInputTexture >> 20u) & 0x3u;
                if (inS3 >= 3u) inS3 = 2u;
                uint32_t dotMap3 = (def.PSDotMapping >> 8u) & 0x7u;
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        float3 dm3 = MapPsDotInput(t%u, %uu);\n"
                    "        float3 N = float3(%s, dotRes%u, dot(rawUv3.xyz, dm3));\n"
                    "        t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, float4(N, 1.0f), (cubeMask & %uu) != 0u, false);\n"
                    "    }\n",
                    inS, dotMap, st, st, inS3, dotMap3,
                    (st >= 1u) ? ((st == 1u) ? "dotRes0" : ((st == 2u) ? "dotRes1" : "dotRes2")) : "0.0f",
                    st, st, st, st, st, st, st, mask);
                s += line;
                break;
            }

            case 0x0Cu: // PS_TEXTUREMODES_DOT_RFLCT_SPEC
            case 0x12u: // PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        float3 N = float3(%s, %s, dotRes%u);\n"
                    "        float3 E = %s;\n"
                    "        float3 Nn = (dot(N, N) > 1e-12f) ? normalize(N) : float3(0.0f, 0.0f, 1.0f);\n"
                    "        float3 En = (dot(E, E) > 1e-12f) ? normalize(E) : float3(0.0f, 0.0f, 1.0f);\n"
                    "        float3 R  = reflect(-En, Nn);\n"
                    "        bool isC  = (cubeMask & %uu) != 0u;\n"
                    "        float4 rUv = isC ? float4(R, 1.0f) : float4(R.x * 0.5f + 0.5f, 0.5f - R.y * 0.5f, 0.0f, 1.0f);\n"
                    "        t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, rUv, isC, false);\n"
                    "    }\n",
                    inS, dotMap, st, st,
                    (st >= 2u) ? ((st == 2u) ? "dotRes0" : "dotRes1") : "0.0f",
                    (st >= 1u) ? ((st == 1u) ? "dotRes0" : ((st == 2u) ? "dotRes1" : "dotRes2")) : "0.0f",
                    st,
                    (mode == 0x12u) ? "uPsEyeDir.xyz" : "input.eyeVec",
                    mask, st, st, st, st, st, st);
                s += line;
                break;

            case 0x0Du: { // PS_TEXTUREMODES_DOT_STR_3D
            case 0x0Eu:   // PS_TEXTUREMODES_DOT_STR_CUBE
                char volExpr[48] = {};
                if (mode == 0x0Du) {
                    snprintf(volExpr, sizeof(volExpr), "((volMask & %uu) != 0u)", mask);
                } else {
                    snprintf(volExpr, sizeof(volExpr), "false");
                }
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        float3 str = float3(%s, %s, dotRes%u);\n"
                    "        t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, float4(str, 1.0f), (cubeMask & %uu) != 0u, %s);\n"
                    "    }\n",
                    inS, dotMap, st, st,
                    (st >= 2u) ? ((st == 2u) ? "dotRes0" : "dotRes1") : "0.0f",
                    (st >= 1u) ? ((st == 1u) ? "dotRes0" : ((st == 2u) ? "dotRes1" : "dotRes2")) : "0.0f",
                    st, st, st, st, st, st, st, mask, volExpr);
                s += line;
                break;
            }

            case 0x0Fu: // PS_TEXTUREMODES_DEPEND_AR
                snprintf(line, sizeof(line),
                    "    t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, float4(t%u.a, t%u.r, 0.0f, 1.0f), false, false);\n",
                    st, st, st, st, st, st, inS, inS);
                s += line;
                break;

            case 0x10u: // PS_TEXTUREMODES_DEPEND_GB
                snprintf(line, sizeof(line),
                    "    t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, float4(t%u.g, t%u.b, 0.0f, 1.0f), false, false);\n",
                    st, st, st, st, st, st, inS, inS);
                s += line;
                break;

            case 0x11u: // PS_TEXTUREMODES_DOTPRODUCT
                snprintf(line, sizeof(line),
                    "    {\n"
                    "        float3 dm = MapPsDotInput(t%u, %uu);\n"
                    "        dotRes%u = dot(rawUv%u.xyz, dm);\n"
                    "        t%u = dotRes%u.xxxx;\n"
                    "    }\n",
                    inS, dotMap, st, st, st, st);
                s += line;
                break;

            default:
                snprintf(line, sizeof(line),
                    "    t%u = SampleStageTexPs(%uu, tex%u, cubeTex%u, volTex%u, smp%u, uv%u, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n",
                    st, st, st, st, st, st, st, mask, mask);
                s += line;
                break;
        }
    }

    // --- NV2A General Register Combiner Stages (0 .. numStages - 1) ---
    snprintf(line, sizeof(line),
        "\n    float fogFactor = ComputeFogFactor(input.eyeDist.x);\n"
        "    float4 fogReg = float4(uFogColor.rgb, fogFactor);\n"
        "    float4 r0 = float4(0.0f, 0.0f, 0.0f, %s);\n"
        "    float4 r1 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\n",
        (tm[0] != 0u) ? "t0.a" : "0.0f");
    s += line;

    bool muxMsb   = (def.PSCombinerCount & 0x00100u) != 0u;
    bool uniqueC0 = (def.PSCombinerCount & 0x01000u) != 0u;
    bool uniqueC1 = (def.PSCombinerCount & 0x10000u) != 0u;

    for (uint32_t st = 0; st < numStages; ++st) {
        uint32_t rgbIn    = def.PSRGBInputs[st];
        uint32_t alphaIn  = def.PSAlphaInputs[st];
        uint32_t rgbOut   = def.PSRGBOutputs[st];
        uint32_t alphaOut = def.PSAlphaOutputs[st];

        char rA[128], rB[128], rC[128], rD[128];
        FormatGcRgbInputExpr((rgbIn >> 24) & 0xFFu, rA, sizeof(rA));
        FormatGcRgbInputExpr((rgbIn >> 16) & 0xFFu, rB, sizeof(rB));
        FormatGcRgbInputExpr((rgbIn >>  8) & 0xFFu, rC, sizeof(rC));
        FormatGcRgbInputExpr( rgbIn        & 0xFFu, rD, sizeof(rD));

        char aA[128], aB[128], aC[128], aD[128];
        FormatGcAlphaInputExpr((alphaIn >> 24) & 0xFFu, aA, sizeof(aA));
        FormatGcAlphaInputExpr((alphaIn >> 16) & 0xFFu, aB, sizeof(aB));
        FormatGcAlphaInputExpr((alphaIn >>  8) & 0xFFu, aC, sizeof(aC));
        FormatGcAlphaInputExpr( alphaIn        & 0xFFu, aD, sizeof(aD));

        snprintf(line, sizeof(line),
            "    // General Combiner Stage %u\n"
            "    {\n"
            "        float4 c0 = uPsStageC0[%uu];\n"
            "        float4 c1 = uPsStageC1[%uu];\n"
            "        float4 v0_in = diffuse, v1_in = specCol;\n"
            "        float4 t0_in = t0, t1_in = t1, t2_in = t2, t3_in = t3;\n"
            "        float4 r0_in = r0, r1_in = r1;\n"
            "        bool muxCond = %s;\n",
            st,
            uniqueC0 ? st : 0u,
            uniqueC1 ? st : 0u,
            muxMsb ? "(r0_in.a >= 0.5f)" : "((((uint)(saturate(r0_in.a) * 255.0f + 0.5f)) & 1u) != 0u)");
        s += line;

        uint32_t rgbCdDst   =  rgbOut        & 0xFu;
        uint32_t rgbAbDst   = (rgbOut >>  4) & 0xFu;
        uint32_t rgbSumDst  = (rgbOut >>  8) & 0xFu;
        bool     rgbCdDot   = ((rgbOut >> 12) & 1u) != 0u;
        bool     rgbAbDot   = ((rgbOut >> 13) & 1u) != 0u;
        bool     rgbMux     = ((rgbOut >> 14) & 1u) != 0u;
        bool     rgbBias    = ((rgbOut >> 15) & 1u) != 0u;
        uint32_t rgbScale   = (rgbOut >> 16) & 3u;
        bool     cdBlueToA  = ((rgbOut >> 18) & 1u) != 0u;
        bool     abBlueToA  = ((rgbOut >> 19) & 1u) != 0u;
        const char* rgbScS  = GetGcScaleSuffix(rgbScale);
        const char* rgbBiS  = rgbBias ? " - 0.5f" : "";

        if (rgbAbDst != 0u || rgbCdDst != 0u || rgbSumDst != 0u) {
            snprintf(line, sizeof(line),
                "        float3 rgbA = %s;\n"
                "        float3 rgbB = %s;\n"
                "        float3 rgbC = %s;\n"
                "        float3 rgbD = %s;\n"
                "        float3 abRaw = %s;\n"
                "        float3 cdRaw = %s;\n"
                "        float3 sumRaw = %s;\n",
                rA, rB, rC, rD,
                rgbAbDot ? "dot(rgbA, rgbB).xxx" : "(rgbA * rgbB)",
                rgbCdDot ? "dot(rgbC, rgbD).xxx" : "(rgbC * rgbD)",
                rgbMux   ? "(muxCond ? cdRaw : abRaw)" : "(abRaw + cdRaw)");
            s += line;

            const char* abDstName  = GetGcDstRegName(rgbAbDst);
            const char* cdDstName  = GetGcDstRegName(rgbCdDst);
            const char* sumDstName = GetGcDstRegName(rgbSumDst);
            if (abDstName) {
                snprintf(line, sizeof(line), "        float3 abVal = clamp((abRaw%s)%s, -1.0f, 1.0f);\n        %s.rgb = abVal;\n",
                         rgbBiS, rgbScS, abDstName);
                s += line;
                if (abBlueToA) {
                    snprintf(line, sizeof(line), "        %s.a = abVal.b;\n", abDstName);
                    s += line;
                }
            }
            if (cdDstName) {
                snprintf(line, sizeof(line), "        float3 cdVal = clamp((cdRaw%s)%s, -1.0f, 1.0f);\n        %s.rgb = cdVal;\n",
                         rgbBiS, rgbScS, cdDstName);
                s += line;
                if (cdBlueToA) {
                    snprintf(line, sizeof(line), "        %s.a = cdVal.b;\n", cdDstName);
                    s += line;
                }
            }
            if (sumDstName) {
                snprintf(line, sizeof(line), "        %s.rgb = clamp((sumRaw%s)%s, -1.0f, 1.0f);\n",
                         sumDstName, rgbBiS, rgbScS);
                s += line;
            }
        }

        uint32_t aCdDst  =  alphaOut        & 0xFu;
        uint32_t aAbDst  = (alphaOut >>  4) & 0xFu;
        uint32_t aSumDst = (alphaOut >>  8) & 0xFu;
        bool     aMux    = ((alphaOut >> 14) & 1u) != 0u;
        bool     aBias   = ((alphaOut >> 15) & 1u) != 0u;
        uint32_t aScale  = (alphaOut >> 16) & 3u;
        const char* aScS = GetGcScaleSuffix(aScale);
        const char* aBiS = aBias ? " - 0.5f" : "";

        if (aAbDst != 0u || aCdDst != 0u || aSumDst != 0u) {
            snprintf(line, sizeof(line),
                "        float aA = %s;\n"
                "        float aB = %s;\n"
                "        float aC = %s;\n"
                "        float aD = %s;\n"
                "        float abRawA = aA * aB;\n"
                "        float cdRawA = aC * aD;\n"
                "        float sumRawA = %s;\n",
                aA, aB, aC, aD,
                aMux ? "(muxCond ? cdRawA : abRawA)" : "(abRawA + cdRawA)");
            s += line;

            const char* abDstName  = GetGcDstRegName(aAbDst);
            const char* cdDstName  = GetGcDstRegName(aCdDst);
            const char* sumDstName = GetGcDstRegName(aSumDst);
            if (abDstName) {
                snprintf(line, sizeof(line), "        %s.a = clamp((abRawA%s)%s, -1.0f, 1.0f);\n",
                         abDstName, aBiS, aScS);
                s += line;
            }
            if (cdDstName) {
                snprintf(line, sizeof(line), "        %s.a = clamp((cdRawA%s)%s, -1.0f, 1.0f);\n",
                         cdDstName, aBiS, aScS);
                s += line;
            }
            if (sumDstName) {
                snprintf(line, sizeof(line), "        %s.a = clamp((sumRawA%s)%s, -1.0f, 1.0f);\n",
                         sumDstName, aBiS, aScS);
                s += line;
            }
        }

        s += "    }\n\n";
    }

    // --- NV2A Final Combiner ---
    bool hasExplicitFC = ((def.PSFinalCombinerInputsABCD | def.PSFinalCombinerInputsEFG) != 0u);
    if (hasExplicitFC) {
        uint32_t abcd = def.PSFinalCombinerInputsABCD;
        uint32_t efg  = def.PSFinalCombinerInputsEFG;
        bool compV1   = (efg & 0x20u) != 0u;
        bool compR0   = (efg & 0x40u) != 0u;
        bool clampSum = (efg & 0x80u) != 0u;

        char sE[128], sF[128], sA[128], sB[128], sC[128], sD[128], sG[128];
        FormatFcRgbInputExpr((efg  >> 24) & 0xFFu, clampSum, sE, sizeof(sE));
        FormatFcRgbInputExpr((efg  >> 16) & 0xFFu, clampSum, sF, sizeof(sF));
        FormatFcRgbInputExpr((abcd >> 24) & 0xFFu, clampSum, sA, sizeof(sA));
        FormatFcRgbInputExpr((abcd >> 16) & 0xFFu, clampSum, sB, sizeof(sB));
        FormatFcRgbInputExpr((abcd >>  8) & 0xFFu, clampSum, sC, sizeof(sC));
        FormatFcRgbInputExpr( abcd        & 0xFFu, clampSum, sD, sizeof(sD));
        FormatFcAlphaInputExpr((efg >> 8) & 0xFFu, sG, sizeof(sG));

        snprintf(line, sizeof(line),
            "    // Explicit NV2A Final Combiner\n"
            "    {\n"
            "        float3 v1Val = %s;\n"
            "        float3 r0Val = %s;\n"
            "        float3 v1r0Sum = %s;\n"
            "        float3 fcE = %s;\n"
            "        float3 fcF = %s;\n"
            "        float3 efProd = fcE * fcF;\n"
            "        float3 fcA = %s;\n"
            "        float3 fcB = %s;\n"
            "        float3 fcC = %s;\n"
            "        float3 fcD = %s;\n"
            "        float  fcG = %s;\n"
            "        current = saturate(float4(fcA * fcB + (1.0f - fcA) * fcC + fcD, fcG));\n"
            "    }\n\n",
            compV1   ? "(1.0f - saturate(specCol.rgb))" : "saturate(specCol.rgb)",
            compR0   ? "(1.0f - saturate(r0.rgb))"      : "saturate(r0.rgb)",
            clampSum ? "saturate(v1Val + r0Val)"        : "(v1Val + r0Val)",
            sE, sF, sA, sB, sC, sD, sG);
        s += line;
        s += kSharedPsAlphaTestAndGammaHLSL;
    } else {
        s += "    current = saturate(r0);\n"
             "    if ((uFlags.x & 8u) != 0u) {\n"
             "        current.rgb = saturate(current.rgb + saturate(specCol.rgb));\n"
             "    }\n\n";
        s += kSharedPsAlphaTestAndGammaHLSL;
        s += kSharedPsFogHLSL;
    }

    if (writesCustomDepth) {
        s += "    PSOut psOut;\n"
             "    psOut.color = current;\n"
             "    psOut.depth = customDepth;\n"
             "    return psOut;\n"
             "}\n";
    } else {
        s += kSharedPsEpilogueHLSL;
    }
    return s;
}

// ============================================================================
// Shader Disk & In-Memory Hash Caches (PS & Cheops VS)
// ============================================================================

static void GetShaderCacheFilePaths(
    const char* shaderBaseName, char* outHlslPath, size_t hlslMax, char* outCsoPath, size_t csoMax)
{
    if (outHlslPath && hlslMax > 0) {
        snprintf(outHlslPath, hlslMax, "%s\\shaders\\%s.hlsl", g_appRootDir, shaderBaseName);
    }
    if (outCsoPath && csoMax > 0) {
        snprintf(outCsoPath, csoMax, "%s\\shaders\\%s.cso", g_appRootDir, shaderBaseName);
    }
}

static bool CompileHlslAndPersistToDisk(
    const char* shaderBaseName, const std::string& hlslSource, bool isPixel,
    void** outData, size_t* outSize)
{
    if (outData) *outData = nullptr;
    if (outSize) *outSize = 0;

    if (g_appRootDir[0] != '\0') {
        char shadersDir[kMaxHostPath] = {};
        snprintf(shadersDir, sizeof(shadersDir), "%s\\shaders", g_appRootDir);
        EnsureDirExists(shadersDir);

        char hlslPath[kMaxHostPath] = {};
        GetShaderCacheFilePaths(shaderBaseName, hlslPath, sizeof(hlslPath), nullptr, 0);
        FILE* fHlsl = OpenFileUtf8(hlslPath, L"wb");
        if (fHlsl) {
            fwrite(hlslSource.data(), 1, hlslSource.size(), fHlsl);
            fclose(fHlsl);
        }
    }

    ID3DBlob* blob    = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(
        hlslSource.data(), hlslSource.size(), shaderBaseName,
        nullptr, nullptr,
        isPixel ? "PSMain" : "VSMain",
        isPixel ? "ps_4_0" : "vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errBlob);
    if (errBlob) {
        errBlob->Release();
    }
    if (FAILED(hr) || !blob) {
        return false;
    }

    size_t sz = blob->GetBufferSize();
    if (g_appRootDir[0] != '\0') {
        char csoPath[kMaxHostPath] = {};
        GetShaderCacheFilePaths(shaderBaseName, nullptr, 0, csoPath, sizeof(csoPath));
        FILE* fCso = OpenFileUtf8(csoPath, L"wb");
        if (fCso) {
            fwrite(blob->GetBufferPointer(), 1, sz, fCso);
            fclose(fCso);
        }
    }

    if (outData && outSize) {
        void* buf = malloc(sz);
        if (buf) {
            memcpy(buf, blob->GetBufferPointer(), sz);
            *outData = buf;
            *outSize = sz;
        } else {
            blob->Release();
            return false;
        }
    }
    blob->Release();
    return true;
}

bool LoadShaderCsoFromDisk(const char* shaderBaseName, void** outData, size_t* outSize) {
    if (!shaderBaseName || !outData || !outSize) return false;
    *outData = nullptr;
    *outSize = 0;

    if (g_appRootDir[0] != '\0') {
        char csoPath[kMaxHostPath] = {};
        GetShaderCacheFilePaths(shaderBaseName, nullptr, 0, csoPath, sizeof(csoPath));
        FILE* f = OpenFileUtf8(csoPath, L"rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                void* buf = malloc(static_cast<size_t>(sz));
                if (buf && fread(buf, 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz)) {
                    fclose(f);
                    *outData = buf;
                    *outSize = static_cast<size_t>(sz);
                    return true;
                }
                if (buf) free(buf);
            }
            fclose(f);
        }
    }

    // On-demand fallback compilation for core fixed-function shaders if disk cache was cleared
    if (strcmp(shaderBaseName, "fixed_ps") == 0) {
        return CompileHlslAndPersistToDisk("fixed_ps", BuildFixedPsHLSL(), true, outData, outSize);
    }
    if (strcmp(shaderBaseName, "fixed_vs") == 0) {
        return CompileHlslAndPersistToDisk("fixed_vs", BuildFixedVsHLSL(), false, outData, outSize);
    }
    return false;
}

static SRWLOCK                                         g_psCompileCacheLock   = SRWLOCK_INIT;
static std::unordered_map<uint64_t, ID3D11PixelShader*> g_compiledPsCache;
static ID3D11Device*                                   g_compiledPsCacheDevice = nullptr;

static ID3D11PixelShader* GetOrCreateCompiledXboxPS(const XboxPixelShaderDef& def) {
    if (!g_d3dDevice) return nullptr;
    uint64_t hash = ComputeXboxPsStructuralHash(def);

    AcquireSRWLockShared(&g_psCompileCacheLock);
    if (g_compiledPsCacheDevice == g_d3dDevice) {
        auto it = g_compiledPsCache.find(hash);
        if (it != g_compiledPsCache.end()) {
            ID3D11PixelShader* found = it->second;
            ReleaseSRWLockShared(&g_psCompileCacheLock);
            return found;
        }
    }
    ReleaseSRWLockShared(&g_psCompileCacheLock);

    char baseName[64] = {};
    snprintf(baseName, sizeof(baseName), "ps_%016llx", static_cast<unsigned long long>(hash));

    void*  csoData = nullptr;
    size_t csoSize = 0;
    if (!LoadShaderCsoFromDisk(baseName, &csoData, &csoSize)) {
        std::string hlsl = CompileXboxPixelShaderDefToHLSL(def, baseName);
        CompileHlslAndPersistToDisk(baseName, hlsl, true, &csoData, &csoSize);
    }

    ID3D11PixelShader* createdPs = nullptr;
    if (csoData && csoSize > 0) {
        g_d3dDevice->CreatePixelShader(csoData, csoSize, nullptr, &createdPs);
        free(csoData);
    }

    AcquireSRWLockExclusive(&g_psCompileCacheLock);
    if (g_compiledPsCacheDevice != g_d3dDevice) {
        for (auto& kv : g_compiledPsCache) {
            if (kv.second) kv.second->Release();
        }
        g_compiledPsCache.clear();
        g_compiledPsCacheDevice = g_d3dDevice;
    }
    auto it = g_compiledPsCache.find(hash);
    if (it != g_compiledPsCache.end()) {
        ID3D11PixelShader* existing = it->second;
        ReleaseSRWLockExclusive(&g_psCompileCacheLock);
        if (createdPs) createdPs->Release();
        return existing;
    }
    g_compiledPsCache.emplace(hash, createdPs);
    ReleaseSRWLockExclusive(&g_psCompileCacheLock);
    return createdPs;
}

ID3D11PixelShader* GetActiveProgramPixelShader(bool* outIsProgramPs) {
    if (g_currentPsHandle != 0u) {
        constexpr uint32_t kInlineProgramWrapperHandle = ADDR_D3D_DEVICE + 0x924u;
        // Fast O(1) pointer cache path for heap XboxPixelShaderObject handles created via
        // CreatePixelShader, excluding SetPixelShaderProgram's 12-byte wrapper at CDevice+0x924
        // and falling back to structural hash lookup if combiner RenderStates were overridden.
        if (g_currentPsHandle >= kMinValidHostAddress &&
            g_currentPsHandle != kInlineProgramWrapperHandle &&
            !g_boundPsCombinerDirty)
        {
            XboxPixelShaderObject* obj = reinterpret_cast<XboxPixelShaderObject*>(g_currentPsHandle);
            if (obj->pPSDef == &obj->def) {
                if (obj->cachedPs != nullptr && obj->cachedPsDevice == g_d3dDevice) {
                    ++g_diagPsPointerCacheHitCount;
                    if (outIsProgramPs) *outIsProgramPs = true;
                    return obj->cachedPs;
                }
                ID3D11PixelShader* compiled = GetOrCreateCompiledXboxPS(obj->def);
                if (compiled) {
                    obj->cachedPs       = compiled;
                    obj->cachedPsDevice = g_d3dDevice;
                }
                if (outIsProgramPs) *outIsProgramPs = (compiled != nullptr);
                return compiled;
            }
        }

        const XboxPixelShaderDef* pDef = GetHandlePSDef(g_currentPsHandle);
        if (pDef) {
            if (g_boundPsCombinerDirty) {
                const uint32_t* xboxRS = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
                XboxPixelShaderDef overriddenDef = *pDef;
                uint32_t* dstDwords = reinterpret_cast<uint32_t*>(&overriddenDef);
                for (uint32_t i = 0; i <= 53u; ++i) {
                    dstDwords[i] = xboxRS[i];
                }
                overriddenDef.PSTextureModes = xboxRS[XBOX_D3DRS_PSTEXTUREMODES];
                overriddenDef.PSDotMapping   = xboxRS[XBOX_D3DRS_PSDOTMAPPING];
                overriddenDef.PSInputTexture = xboxRS[XBOX_D3DRS_PSINPUTTEXTURE];
                ID3D11PixelShader* compiled = GetOrCreateCompiledXboxPS(overriddenDef);
                if (outIsProgramPs) *outIsProgramPs = (compiled != nullptr);
                return compiled;
            }
            ID3D11PixelShader* compiled = GetOrCreateCompiledXboxPS(*pDef);
            if (outIsProgramPs) *outIsProgramPs = (compiled != nullptr);
            return compiled;
        }
    } else {
        const uint32_t* xboxRS = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
        if ((xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] & 0xFu) > 0u) {
            XboxPixelShaderDef inlineDef = {};
            uint32_t* dstDwords = reinterpret_cast<uint32_t*>(&inlineDef);
            for (uint32_t i = 0; i <= 53u; ++i) {
                dstDwords[i] = xboxRS[i];
            }
            inlineDef.PSTextureModes           = xboxRS[XBOX_D3DRS_PSTEXTUREMODES];
            inlineDef.PSDotMapping             = xboxRS[XBOX_D3DRS_PSDOTMAPPING];
            inlineDef.PSInputTexture           = xboxRS[XBOX_D3DRS_PSINPUTTEXTURE];
            inlineDef.PSC0Mapping              = 0xFFFFFFFFu;
            inlineDef.PSC1Mapping              = 0xFFFFFFFFu;
            inlineDef.PSFinalCombinerConstants = 0x000001FFu;
            ID3D11PixelShader* compiled = GetOrCreateCompiledXboxPS(inlineDef);
            if (outIsProgramPs) *outIsProgramPs = (compiled != nullptr);
            return compiled;
        }
    }
    if (outIsProgramPs) *outIsProgramPs = false;
    return nullptr;
}

static uint64_t ComputeXboxVsHash(const XboxDeclElem* elems, uint32_t numElems, const DWORD* pFunction) {
    uint64_t h = 14695981039346656037ULL;
    h = Fnv1aMixU32(h, 0x4348454Fu); // "CHEO"
    h = Fnv1aMixU32(h, 0x50533034u); // "PS04"

    h = Fnv1aMixU32(h, numElems);
    bool seenReg[16] = {};
    for (uint32_t i = 0; i < numElems; ++i) {
        uint8_t r = elems[i].reg;
        if (r >= 16u || seenReg[r]) continue;
        seenReg[r] = true;
        h = Fnv1aMixU32(h, (static_cast<uint32_t>(r) << 8) | static_cast<uint32_t>(elems[i].xType));
    }

    uint32_t instCount = (pFunction[0] >> 16) & 0xFFFFu;
    if (instCount > 136u) instCount = 136u;
    h = Fnv1aMixU32(h, (instCount << 16) | (pFunction[0] & 0xFFFFu));
    for (uint32_t i = 0; i < instCount; ++i) {
        uint32_t w0 = pFunction[1 + i * 4 + 0];
        uint32_t w1 = pFunction[1 + i * 4 + 1];
        uint32_t w2 = pFunction[1 + i * 4 + 2];
        uint32_t w3 = pFunction[1 + i * 4 + 3];
        h = Fnv1aMixU32(h, w0);
        h = Fnv1aMixU32(h, w1);
        h = Fnv1aMixU32(h, w2);
        h = Fnv1aMixU32(h, w3);
        if ((w3 & 1u) != 0u) break;
    }
    return h;
}

struct CachedCompiledVsEntry {
    uint64_t             hash;
    ID3D11VertexShader*  vs;
    std::vector<uint8_t> bytecode;
};

struct CachedCompiledIlEntry {
    uint64_t           ilHash;
    ID3D11InputLayout* il;
};

static SRWLOCK                            g_vsCompileCacheLock = SRWLOCK_INIT;
static std::vector<CachedCompiledVsEntry> g_compiledVsCache;
static std::vector<CachedCompiledIlEntry> g_compiledIlCache;

static bool GetOrCreateCompiledXboxVS(
    const XboxDeclElem* elems, uint32_t numElems, const DWORD* pFunction,
    const char* debugLabel, uintptr_t declVa,
    ID3D11VertexShader** outVS, ID3D11InputLayout** outIL)
{
    if (outVS) *outVS = nullptr;
    if (outIL) *outIL = nullptr;
    if (!g_d3dDevice || !pFunction || (pFunction[0] & 0xFFFFu) != 0x2078u) return false;

    uint64_t hash = ComputeXboxVsHash(elems, numElems, pFunction);

    ID3D11VertexShader*  gpuVS = nullptr;
    std::vector<uint8_t> csoBytes;

    AcquireSRWLockShared(&g_vsCompileCacheLock);
    for (size_t i = 0; i < g_compiledVsCache.size(); ++i) {
        if (g_compiledVsCache[i].hash == hash) {
            gpuVS    = g_compiledVsCache[i].vs;
            csoBytes = g_compiledVsCache[i].bytecode;
            break;
        }
    }
    ReleaseSRWLockShared(&g_vsCompileCacheLock);

    if (!gpuVS || csoBytes.empty()) {
        char baseName[64] = {};
        snprintf(baseName, sizeof(baseName), "vs_%016llx", static_cast<unsigned long long>(hash));

        void*  csoData = nullptr;
        size_t csoSize = 0;
        if (!LoadShaderCsoFromDisk(baseName, &csoData, &csoSize)) {
            std::string hlsl = DecompileXvuToHLSL(
                elems, numElems, pFunction,
                (debugLabel && debugLabel[0]) ? debugLabel : baseName, declVa);
            CompileHlslAndPersistToDisk(baseName, hlsl, false, &csoData, &csoSize);
        }

        if (csoData && csoSize > 0) {
            ID3D11VertexShader* createdVs = nullptr;
            g_d3dDevice->CreateVertexShader(csoData, csoSize, nullptr, &createdVs);
            csoBytes.assign(static_cast<const uint8_t*>(csoData), static_cast<const uint8_t*>(csoData) + csoSize);
            free(csoData);

            AcquireSRWLockExclusive(&g_vsCompileCacheLock);
            bool existed = false;
            for (size_t i = 0; i < g_compiledVsCache.size(); ++i) {
                if (g_compiledVsCache[i].hash == hash) {
                    gpuVS    = g_compiledVsCache[i].vs;
                    csoBytes = g_compiledVsCache[i].bytecode;
                    existed  = true;
                    break;
                }
            }
            if (existed) {
                if (createdVs) createdVs->Release();
            } else if (createdVs) {
                gpuVS = createdVs;
                g_compiledVsCache.push_back({ hash, createdVs, csoBytes });
            }
            ReleaseSRWLockExclusive(&g_vsCompileCacheLock);
        }
    }

    if (!gpuVS || csoBytes.empty()) {
        return false;
    }

    ID3D11InputLayout* gpuIL = nullptr;
    if (numElems > 0u) {
        D3D11_INPUT_ELEMENT_DESC ied[16] = {};
        uint32_t iedCount = 0;
        bool seenReg[16] = {};
        uint64_t ilHash = Fnv1aMixU32(hash, 0x494C3031u); // "IL01"
        for (uint32_t i = 0; i < numElems; ++i) {
            uint8_t r = elems[i].reg;
            if (r >= 16u || seenReg[r]) continue;
            seenReg[r] = true;
            ied[iedCount].SemanticName         = "ATTR";
            ied[iedCount].SemanticIndex        = r;
            ied[iedCount].Format               = MapXboxDeclTypeToDXGI(elems[i].xType);
            ied[iedCount].InputSlot            = elems[i].stream;
            ied[iedCount].AlignedByteOffset    = elems[i].offset;
            ied[iedCount].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
            ied[iedCount].InstanceDataStepRate = 0;
            ilHash = Fnv1aMixU32(ilHash, (static_cast<uint32_t>(r) << 24) |
                                         (static_cast<uint32_t>(elems[i].xType) << 16) |
                                         (static_cast<uint32_t>(elems[i].stream) << 8));
            ilHash = Fnv1aMixU32(ilHash, elems[i].offset);
            iedCount++;
        }
        if (iedCount > 0u) {
            AcquireSRWLockShared(&g_vsCompileCacheLock);
            for (size_t i = 0; i < g_compiledIlCache.size(); ++i) {
                if (g_compiledIlCache[i].ilHash == ilHash) {
                    gpuIL = g_compiledIlCache[i].il;
                    break;
                }
            }
            ReleaseSRWLockShared(&g_vsCompileCacheLock);

            if (!gpuIL) {
                ID3D11InputLayout* createdIl = nullptr;
                g_d3dDevice->CreateInputLayout(ied, iedCount, csoBytes.data(), csoBytes.size(), &createdIl);
                if (!createdIl) return false;

                AcquireSRWLockExclusive(&g_vsCompileCacheLock);
                bool ilExisted = false;
                for (size_t i = 0; i < g_compiledIlCache.size(); ++i) {
                    if (g_compiledIlCache[i].ilHash == ilHash) {
                        gpuIL     = g_compiledIlCache[i].il;
                        ilExisted = true;
                        break;
                    }
                }
                if (ilExisted) {
                    createdIl->Release();
                } else {
                    gpuIL = createdIl;
                    g_compiledIlCache.push_back({ ilHash, createdIl });
                }
                ReleaseSRWLockExclusive(&g_vsCompileCacheLock);
            }
        }
    }

    if (outVS) *outVS = gpuVS;
    if (outIL) *outIL = gpuIL;
    return true;
}

// ============================================================================
// First-Run Batch Shader Precompilation (GhostXR_Data\shaders\)
// ============================================================================

struct ShaderCompileJob {
    std::string baseName;
    std::string hlslSource;
    bool        isPixel;
};

struct ShaderCompileBatchCtx {
    const std::vector<ShaderCompileJob>* jobs;
    volatile LONG                        nextIdx;
    volatile LONG                        doneCount;
    volatile LONG                        failCount;
    volatile LONG                        currentJobIdx;
    volatile LONG                        abortFlag;
};

static DWORD WINAPI ShaderCompileWorkerProc(LPVOID param) {
    ShaderCompileBatchCtx* ctx = reinterpret_cast<ShaderCompileBatchCtx*>(param);
    const size_t total = ctx->jobs->size();
    for (;;) {
        if (ctx->abortFlag != 0) break;
        LONG idx = InterlockedIncrement(&ctx->nextIdx) - 1;
        if (idx < 0 || static_cast<size_t>(idx) >= total) break;

        InterlockedExchange(&ctx->currentJobIdx, idx);
        const ShaderCompileJob& job = (*ctx->jobs)[static_cast<size_t>(idx)];
        if (!CompileHlslAndPersistToDisk(job.baseName.c_str(), job.hlslSource, job.isPixel, nullptr, nullptr)) {
            InterlockedIncrement(&ctx->failCount);
        }
        InterlockedIncrement(&ctx->doneCount);
    }
    return 0;
}

void ClearShaderCacheDirectory() {
    char shadersDir[kMaxHostPath] = {};
    snprintf(shadersDir, sizeof(shadersDir), "%s\\shaders", g_appRootDir);
    EnsureDirExists(shadersDir);

    wchar_t shadersDirW[kMaxHostPath] = {};
    if (!Utf8ToWide(shadersDir, shadersDirW, kMaxHostPath) || !shadersDirW[0]) return;
    wchar_t searchPatternW[kMaxHostPath] = {};
    swprintf(searchPatternW, kMaxHostPath, L"%ls\\*", shadersDirW);

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(searchPatternW, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            wchar_t filePathW[kMaxHostPath] = {};
            swprintf(filePathW, kMaxHostPath, L"%ls\\%ls", shadersDirW, fd.cFileName);
            DeleteFileW(filePathW);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static constexpr const char* kShaderCacheVersionTagFile = "shader_cache_v3.tag";

bool IsShaderCacheReadyOnDisk() {
    char shadersDir[kMaxHostPath] = {};
    snprintf(shadersDir, sizeof(shadersDir), "%s\\shaders", g_appRootDir);
    EnsureDirExists(shadersDir);

    char fixedVsCso[kMaxHostPath] = {};
    char fixedPsCso[kMaxHostPath] = {};
    char tagPath[kMaxHostPath]    = {};
    GetShaderCacheFilePaths("fixed_vs", nullptr, 0, fixedVsCso, sizeof(fixedVsCso));
    GetShaderCacheFilePaths("fixed_ps", nullptr, 0, fixedPsCso, sizeof(fixedPsCso));
    snprintf(tagPath, sizeof(tagPath), "%s\\shaders\\%s", g_appRootDir, kShaderCacheVersionTagFile);

    return IsShaderCacheCompiledInIni() &&
           GetFileAttributesUtf8(fixedVsCso) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesUtf8(fixedPsCso) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesUtf8(tagPath)    != INVALID_FILE_ATTRIBUTES;
}

bool CompileAllGameShadersToCache(PFN_ShaderCompileProgressCallback progressCb, void* userData) {
    ClearShaderCacheDirectory();

    std::vector<ShaderCompileJob> jobs;
    jobs.reserve(340);

    std::vector<uint64_t> seenPsHashes;
    std::vector<uint64_t> seenVsHashes;
    seenPsHashes.reserve(64);
    seenVsHashes.reserve(320);

    auto addPsJob = [&](const XboxPixelShaderDef& def, const char* label) {
        uint64_t h = ComputeXboxPsStructuralHash(def);
        for (size_t i = 0; i < seenPsHashes.size(); ++i) {
            if (seenPsHashes[i] == h) return;
        }
        seenPsHashes.push_back(h);
        char baseName[64] = {};
        snprintf(baseName, sizeof(baseName), "ps_%016llx", static_cast<unsigned long long>(h));
        jobs.push_back({ baseName, CompileXboxPixelShaderDefToHLSL(def, label), true });
    };

    // 1. Fixed-function Pixel & Vertex Shaders (place fixed_ps first so Thread #0 starts it immediately)
    jobs.push_back({ "fixed_ps", BuildFixedPsHLSL(), true });
    jobs.push_back({ "fixed_vs", BuildFixedVsHLSL(), false });

    // 2. All .xpu Pixel Shaders from aPixelShaders[0..27] (0x007D1210) in VertexShaders/xbox
    for (uint32_t pt = 0; pt < 28u; ++pt) {
        const char* relPath = *reinterpret_cast<const char* const*>(0x007D1210u + pt * 8u);
        if (!relPath || reinterpret_cast<uintptr_t>(relPath) < kMinValidXboxAddress) continue;

        char fullPath[kMaxHostPath] = {};
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", g_gameRootDir, relPath);
        FILE* f = OpenFileUtf8(fullPath, L"rb");
        if (!f) continue;

        uint32_t magic = 0;
        XboxPixelShaderDef def = {};
        bool ok = (fread(&magic, 1, 4, f) == 4 && magic == 0x30425350u &&
                   fread(&def, 1, sizeof(XboxPixelShaderDef), f) == sizeof(XboxPixelShaderDef));
        fclose(f);
        if (!ok) continue;

        addPsJob(def, relPath);
    }

    // 2b. Embedded YUV Pixel Shaders in Ghost.exe .data (dwM4PSyuvLowPixelShader & dwM4PSyuvHighPixelShader)
    static constexpr uint32_t kEmbeddedPsVas[2] = { 0x007D1508u, 0x007D15F8u };
    static const char* const  kEmbeddedPsLabels[2] = {
        "dwM4PSyuvLowPixelShader (0x007D1508)",
        "dwM4PSyuvHighPixelShader (0x007D15F8)"
    };
    for (int e = 0; e < 2; ++e) {
        const XboxPixelShaderDef* pEmb = reinterpret_cast<const XboxPixelShaderDef*>(kEmbeddedPsVas[e]);
        if ((pEmb->PSCombinerCount & 0xFu) > 0u || (pEmb->PSTextureModes & 0xFFFFFu) != 0u) {
            addPsJob(*pEmb, kEmbeddedPsLabels[e]);
        }
    }

    // 3. All .xvu Vertex Shaders from aVertexShaders[0..65] (0x007D0BE0) in VertexShaders/xbox
    static const char* const kXvuSuffixes[5] = { ".xvu", "0.xvu", "1.xvu", "2.xvu", "3.xvu" };

    for (uint32_t vsType = 0; vsType < 66u; ++vsType) {
        uint32_t entryVa = ADDR_VERTEX_SHADERS_TABLE + vsType * 24u;
        const char* prefixRel = *reinterpret_cast<const char* const*>(entryVa + 0u);
        const DWORD* pDecl    = *reinterpret_cast<const DWORD* const*>(entryVa + 4u);
        if (!prefixRel || reinterpret_cast<uintptr_t>(prefixRel) < kMinValidXboxAddress ||
            !pDecl     || reinterpret_cast<uintptr_t>(pDecl)     < kMinValidXboxAddress)
        {
            continue;
        }

        XboxDeclElem elems[16] = {};
        uint32_t numElems = 0;
        ParseXboxVertexDeclaration(pDecl, false, elems, &numElems, nullptr);

        for (int sfxIdx = 0; sfxIdx < 5; ++sfxIdx) {
            char relFile[kMaxHostPath]  = {};
            char fullFile[kMaxHostPath] = {};
            snprintf(relFile, sizeof(relFile), "%s%s", prefixRel, kXvuSuffixes[sfxIdx]);
            snprintf(fullFile, sizeof(fullFile), "%s\\%s", g_gameRootDir, relFile);

            FILE* f = OpenFileUtf8(fullFile, L"rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz < 4) {
                fclose(f);
                continue;
            }
            std::vector<uint8_t> xvuBytes(static_cast<size_t>(sz));
            if (fread(xvuBytes.data(), 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
                fclose(f);
                continue;
            }
            fclose(f);

            const DWORD* pFunction = reinterpret_cast<const DWORD*>(xvuBytes.data());
            if ((pFunction[0] & 0xFFFFu) != 0x2078u) continue;

            uint64_t vsHash = ComputeXboxVsHash(elems, numElems, pFunction);
            bool duplicate = false;
            for (size_t k = 0; k < seenVsHashes.size(); ++k) {
                if (seenVsHashes[k] == vsHash) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            seenVsHashes.push_back(vsHash);

            char vsBaseName[64] = {};
            snprintf(vsBaseName, sizeof(vsBaseName), "vs_%016llx", static_cast<unsigned long long>(vsHash));
            jobs.push_back({
                vsBaseName,
                DecompileXvuToHLSL(elems, numElems, pFunction, relFile, reinterpret_cast<uintptr_t>(pDecl)),
                false
            });
        }
    }

    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    uint32_t numThreads = sysInfo.dwNumberOfProcessors;
    if (numThreads < 1u) numThreads = 1u;
    if (numThreads > 3u) numThreads = 3u;

    ShaderCompileBatchCtx batchCtx = { &jobs, 0, 0, 0, 0, 0 };
    HANDLE threads[3] = {};
    for (uint32_t t = 0; t < numThreads; ++t) {
        threads[t] = CreateThread(nullptr, 0, ShaderCompileWorkerProc, &batchCtx, 0, nullptr);
    }

    const uint32_t totalJobs = static_cast<uint32_t>(jobs.size());
    bool cancelled = false;
    while (WaitForMultipleObjects(numThreads, threads, TRUE, 30) == WAIT_TIMEOUT) {
        if (progressCb) {
            LONG done   = batchCtx.doneCount;
            LONG curIdx = batchCtx.currentJobIdx;
            if (done < 0) done = 0;
            if (static_cast<uint32_t>(done) > totalJobs) done = static_cast<LONG>(totalJobs);
            const char* curName = (curIdx >= 0 && static_cast<uint32_t>(curIdx) < totalJobs)
                ? jobs[static_cast<size_t>(curIdx)].baseName.c_str()
                : "fixed_ps";
            if (!progressCb(static_cast<uint32_t>(done), totalJobs, curName, userData)) {
                cancelled = true;
                InterlockedExchange(&batchCtx.abortFlag, 1);
            }
        }
    }

    for (uint32_t t = 0; t < numThreads; ++t) {
        if (threads[t]) CloseHandle(threads[t]);
    }

    if (cancelled) {
        return false;
    }
    if (progressCb && totalJobs > 0) {
        progressCb(totalJobs, totalJobs, jobs.back().baseName.c_str(), userData);
    }
    if (batchCtx.failCount == 0) {
        char tagPath[kMaxHostPath] = {};
        snprintf(tagPath, sizeof(tagPath), "%s\\shaders\\%s", g_appRootDir, kShaderCacheVersionTagFile);
        FILE* fTag = OpenFileUtf8(tagPath, L"wb");
        if (fTag) {
            static const char kTagContent[] = "NV2APS04_CHEOPS04\n";
            fwrite(kTagContent, 1, sizeof(kTagContent) - 1, fTag);
            fclose(fTag);
        }
        SetShaderCacheCompiledInIni(true);
    }
    return true;
}

// ============================================================================
// Dynamic Cheops Vertex Shader Objects, Microcode Slot Array & Bridges
// ============================================================================

struct VsDeclElemRecord {
    const XboxVertexShaderObject* vsObj;
    uint32_t                      numElems;
    XboxDeclElem                  elems[16];
};

static SRWLOCK                        g_vsObjDeclLock = SRWLOCK_INIT;
static std::vector<VsDeclElemRecord>  g_vsObjDeclRecords;
alignas(16) static XboxVertexShaderObject g_selectedCheopsVsObj = {};

static void SaveVsObjDeclElems(const XboxVertexShaderObject* vsObj, const XboxDeclElem* elems, uint32_t numElems) {
    AcquireSRWLockExclusive(&g_vsObjDeclLock);
    for (size_t i = 0; i < g_vsObjDeclRecords.size(); ++i) {
        if (g_vsObjDeclRecords[i].vsObj == vsObj) {
            g_vsObjDeclRecords[i].numElems = numElems;
            memcpy(g_vsObjDeclRecords[i].elems, elems, sizeof(XboxDeclElem) * 16u);
            ReleaseSRWLockExclusive(&g_vsObjDeclLock);
            return;
        }
    }
    VsDeclElemRecord rec = {};
    rec.vsObj    = vsObj;
    rec.numElems = numElems;
    if (elems && numElems > 0u) {
        memcpy(rec.elems, elems, sizeof(XboxDeclElem) * ((numElems <= 16u) ? numElems : 16u));
    }
    g_vsObjDeclRecords.push_back(rec);
    ReleaseSRWLockExclusive(&g_vsObjDeclLock);
}

static bool LookupVsObjDeclElems(const XboxVertexShaderObject* vsObj, XboxDeclElem* outElems, uint32_t* outNumElems) {
    AcquireSRWLockShared(&g_vsObjDeclLock);
    for (size_t i = 0; i < g_vsObjDeclRecords.size(); ++i) {
        if (g_vsObjDeclRecords[i].vsObj == vsObj) {
            *outNumElems = g_vsObjDeclRecords[i].numElems;
            memcpy(outElems, g_vsObjDeclRecords[i].elems, sizeof(XboxDeclElem) * 16u);
            ReleaseSRWLockShared(&g_vsObjDeclLock);
            return true;
        }
    }
    ReleaseSRWLockShared(&g_vsObjDeclLock);
    return false;
}

static bool InspectCheopsMicrocodeWritesOFog(const DWORD* pFunction) {
    if (!pFunction || (pFunction[0] & 0xFFFFu) != 0x2078u) return false;
    uint32_t instCount = (pFunction[0] >> 16) & 0xFFFFu;
    if (instCount > 136u) instCount = 136u;
    for (uint32_t i = 0; i < instCount; ++i) {
        uint32_t w3       = pFunction[1 + i * 4 + 3];
        uint32_t outMask  = (w3 >> 12) & 0xFu;
        uint32_t outWrite = (w3 >> 11) & 1u;
        uint32_t outReg   = (w3 >> 3)  & 0xFFu;
        if (outWrite != 0u && outMask != 0u && outReg == 5u) {
            return true;
        }
        if ((w3 & 1u) != 0u) break;
    }
    return false;
}

static HRESULT __stdcall Bridge_D3DDevice_CreateVertexShader(
    const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
    if (!pHandle) return E_INVALIDARG;

    XboxVertexShaderObject* vsObj = reinterpret_cast<XboxVertexShaderObject*>(
        _aligned_malloc(sizeof(XboxVertexShaderObject), 16));
    if (!vsObj) return E_OUTOFMEMORY;
    memset(vsObj, 0, sizeof(XboxVertexShaderObject));

    XboxShaderDeclInfo& info = vsObj->hostDecl;
    info.vsType = -1;
    const char* debugLabel = nullptr;

    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_VERTEX_SHADERS_TABLE && hAddr < ADDR_VERTEX_SHADERS_TABLE + 66u * 24u) {
        uint32_t offset = static_cast<uint32_t>(hAddr - ADDR_VERTEX_SHADERS_TABLE);
        info.vsType = static_cast<int32_t>(offset / 24u);
        const char* prefixRel = *reinterpret_cast<const char* const*>(
            ADDR_VERTEX_SHADERS_TABLE + static_cast<uint32_t>(info.vsType) * 24u);
        if (prefixRel && reinterpret_cast<uintptr_t>(prefixRel) >= kMinValidXboxAddress) {
            debugLabel = prefixRel;
        }
    }

    bool hasProgram = (pFunction != nullptr && (pFunction[0] & 0xFFFFu) == 0x2078u);
    XboxDeclElem elems[16] = {};
    uint32_t numElems = 0;
    ParseXboxVertexDeclaration(pDeclaration, !hasProgram, elems, &numElems, &info);
    SaveVsObjDeclElems(vsObj, elems, numElems);

    vsObj->RefCount       = 1u;
    vsObj->Flags          = hasProgram ? ((Usage != 0u) ? 0x03u : 0x01u) : (info.hasRhw ? 0x10u : 0x00u);
    vsObj->Dimensionality = 0x1111u;
    vsObj->ConstantsCount = info.numDeclConsts;
    vsObj->StreamCount    = (info.usesStream[1] ? 2u : (info.usesStream[0] ? 1u : 0u));

    if (hasProgram) {
        uint32_t instCount = (pFunction[0] >> 16) & 0xFFFFu;
        if (instCount > 136u) instCount = 136u;
        vsObj->ProgramSize = instCount;
        memcpy(vsObj->ProgramAndConstants, &pFunction[1], static_cast<size_t>(instCount) * 16u);
        info.writesOFog = InspectCheopsMicrocodeWritesOFog(pFunction);
        if (info.writesOFog) {
            vsObj->Flags |= 0x20u;
        }
        vsObj->programHash = ComputeXboxVsHash(elems, numElems, pFunction);

        if (g_d3dDevice) {
            info.isCompiledXvu = GetOrCreateCompiledXboxVS(
                elems, numElems, pFunction, debugLabel,
                reinterpret_cast<uintptr_t>(pDeclaration),
                &info.gpuVS, &info.gpuInputLayout);
        }
    }

    *pHandle = static_cast<DWORD>(reinterpret_cast<uintptr_t>(vsObj) | 1u);
    return S_OK;
}

static void ApplyVsConstantsDwords(int32_t xboxReg, const void* pConstantData, uint32_t dwordCount) {
    if (!pConstantData || dwordCount == 0) return;
    if (xboxReg < 0) {
        xboxReg += 96; // Support unbiased Xbox VS constant slots (-96..95 -> 0..191)
    }
    if (xboxReg >= 0 && xboxReg < 192) {
        uint32_t maxDwords  = static_cast<uint32_t>(192 - xboxReg) * 4u;
        uint32_t copyDwords = (dwordCount > maxDwords) ? maxDwords : dwordCount;
        memcpy(reinterpret_cast<uint8_t*>(ADDR_D3D_VS_CONSTANTS) + static_cast<size_t>(xboxReg) * 16u,
               pConstantData, static_cast<size_t>(copyDwords) * 4u);
        ++g_vsConstantsDirtyGen;
    }
}

static void ApplyVsConstants(int32_t xboxReg, const void* pConstantData, uint32_t ConstantCount) {
    ApplyVsConstantsDwords(xboxReg, pConstantData, ConstantCount * 4u);
}

void LoadCheopsProgramSlots(uint32_t startSlot, const uint32_t* dwords, uint32_t slotCount) {
    if (!dwords || slotCount == 0u || startSlot >= 136u) return;
    uint32_t maxSlots = 136u - startSlot;
    if (slotCount > maxSlots) slotCount = maxSlots;
    memcpy(g_cheopsProgramSlots[startSlot], dwords, static_cast<size_t>(slotCount) * 16u);
}

void __stdcall Bridge_D3DDevice_SetVertexShader(uint32_t Handle) {
    g_activeGpuSkin.active = false;
    g_currentVsHandle = Handle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x798) = Handle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x79C) = 0;

    if ((Handle & 1u) == 0u) {
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = 0x006C2F20u;
        typedef void (__stdcall *PFN_InitVsFromFvf)(void* pVs, DWORD fvf);
        PFN_InitVsFromFvf fnInitFvf = reinterpret_cast<PFN_InitVsFromFvf>(0x006B22F0);
        fnInitFvf(reinterpret_cast<void*>(0x006C2F20), Handle);
    } else {
        uintptr_t ptr = static_cast<uintptr_t>(Handle & ~1u);
        if (ptr >= kMinValidHostAddress) {
            XboxVertexShaderObject* vsObj = reinterpret_cast<XboxVertexShaderObject*>(ptr);
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = static_cast<uint32_t>(ptr);
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x79C) = (vsObj->Flags & 0x10u) ? 1u : 0u;
            if (vsObj->ProgramSize > 0u) {
                LoadCheopsProgramSlots(0u, vsObj->ProgramAndConstants, vsObj->ProgramSize);
            }
            if ((vsObj->Flags & 1u) != 0u && !vsObj->hostDecl.isCompiledXvu && vsObj->ProgramSize > 0u && g_d3dDevice) {
                XboxDeclElem elems[16] = {};
                uint32_t numElems = 0u;
                LookupVsObjDeclElems(vsObj, elems, &numElems);
                DWORD synthFunc[1 + 136 * 4] = {};
                synthFunc[0] = (vsObj->ProgramSize << 16) | 0x2078u;
                memcpy(&synthFunc[1], vsObj->ProgramAndConstants, static_cast<size_t>(vsObj->ProgramSize) * 16u);
                vsObj->hostDecl.isCompiledXvu = GetOrCreateCompiledXboxVS(
                    elems, numElems, synthFunc, "DeferredCreateVS", 0,
                    &vsObj->hostDecl.gpuVS, &vsObj->hostDecl.gpuInputLayout);
            }
            for (uint8_t c = 0; c < vsObj->hostDecl.numDeclConsts; ++c) {
                ApplyVsConstants(static_cast<int32_t>(vsObj->hostDecl.declConsts[c].hwSlot),
                                 vsObj->hostDecl.declConsts[c].val, 1u);
            }
        } else {
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = 0x006C2F20u;
        }
    }
}

// _D3DDevice_LoadVertexShader@8 (0x006B2BF0)
static void __stdcall Bridge_D3DDevice_LoadVertexShader(DWORD Handle, DWORD Address) {
    if ((Handle & 1u) == 0u || Address >= 136u) return;
    uintptr_t ptr = static_cast<uintptr_t>(Handle & ~1u);
    if (ptr < kMinValidHostAddress) return;
    const XboxVertexShaderObject* vsObj = reinterpret_cast<const XboxVertexShaderObject*>(ptr);
    if (vsObj->ProgramSize > 0u) {
        LoadCheopsProgramSlots(Address, vsObj->ProgramAndConstants, vsObj->ProgramSize);
    }
}

// _D3DDevice_SelectVertexShader@8 (0x006B2C50)
static void __stdcall Bridge_D3DDevice_SelectVertexShader(DWORD Handle, DWORD Address) {
    g_activeGpuSkin.active = false;
    if (Address >= 136u) return;

    const XboxVertexShaderObject* declSrcObj = nullptr;
    if ((Handle & 1u) != 0u && static_cast<uintptr_t>(Handle & ~1u) >= kMinValidHostAddress) {
        declSrcObj = reinterpret_cast<const XboxVertexShaderObject*>(static_cast<uintptr_t>(Handle & ~1u));
    } else if ((g_currentVsHandle & 1u) != 0u && static_cast<uintptr_t>(g_currentVsHandle & ~1u) >= kMinValidHostAddress) {
        declSrcObj = reinterpret_cast<const XboxVertexShaderObject*>(static_cast<uintptr_t>(g_currentVsHandle & ~1u));
    }

    XboxDeclElem elems[16] = {};
    uint32_t numElems = 0u;
    if (declSrcObj) {
        LookupVsObjDeclElems(declSrcObj, elems, &numElems);
        g_selectedCheopsVsObj = *declSrcObj;
    } else {
        memset(&g_selectedCheopsVsObj, 0, sizeof(g_selectedCheopsVsObj));
        g_selectedCheopsVsObj.RefCount       = 1u;
        g_selectedCheopsVsObj.Dimensionality = 0x1111u;
        g_selectedCheopsVsObj.hostDecl.vsType = -1;
    }

    DWORD synthFunc[1 + 136 * 4] = {};
    uint32_t instCount = 0u;
    for (uint32_t slot = Address; slot < 136u; ++slot) {
        memcpy(&synthFunc[1 + instCount * 4u], g_cheopsProgramSlots[slot], 16u);
        uint32_t w3 = g_cheopsProgramSlots[slot][3];
        instCount++;
        if ((w3 & 1u) != 0u) break;
    }
    if (instCount == 0u) return;

    synthFunc[0] = (instCount << 16) | 0x2078u;
    g_selectedCheopsVsObj.Flags       = 0x01u;
    g_selectedCheopsVsObj.ProgramSize = instCount;
    memcpy(g_selectedCheopsVsObj.ProgramAndConstants, &synthFunc[1], static_cast<size_t>(instCount) * 16u);
    g_selectedCheopsVsObj.hostDecl.writesOFog = InspectCheopsMicrocodeWritesOFog(synthFunc);
    if (g_selectedCheopsVsObj.hostDecl.writesOFog) {
        g_selectedCheopsVsObj.Flags |= 0x20u;
    }

    g_selectedCheopsVsObj.hostDecl.gpuVS          = nullptr;
    g_selectedCheopsVsObj.hostDecl.gpuInputLayout = nullptr;
    g_selectedCheopsVsObj.hostDecl.isCompiledXvu  = GetOrCreateCompiledXboxVS(
        elems, numElems, synthFunc, "CheopsSlotProgram", 0,
        &g_selectedCheopsVsObj.hostDecl.gpuVS, &g_selectedCheopsVsObj.hostDecl.gpuInputLayout);
    SaveVsObjDeclElems(&g_selectedCheopsVsObj, elems, numElems);

    if (Handle != 0u && declSrcObj) {
        for (uint8_t c = 0; c < declSrcObj->hostDecl.numDeclConsts; ++c) {
            ApplyVsConstants(static_cast<int32_t>(declSrcObj->hostDecl.declConsts[c].hwSlot),
                             declSrcObj->hostDecl.declConsts[c].val, 1u);
        }
    }

    uint32_t selHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_selectedCheopsVsObj) | 1u);
    g_currentVsHandle = selHandle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_selectedCheopsVsObj));
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x798) = (Handle != 0u) ? Handle : selHandle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x79C) = 0u;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DIRTYFLAGS) |= 0x70u;
}

// _D3DDevice_SetShaderConstantMode@4 (0x006B2CF0)
static void __stdcall Bridge_D3DDevice_SetShaderConstantMode(DWORD Mode) {
    uint32_t* pDevFlags = reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x08);
    if ((Mode & 0x10u) != 0u) {
        *pDevFlags |= 0x200u;
    } else {
        *pDevFlags &= ~0x200u;
    }
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1928) = (Mode & ~0x10u);
}

// ?RestoreVertexShaders@D3D@@YGXPBKK@Z (0x006B2E10 -> 0x006B24E0):
// Size is DWORD count (4 DWORDs per Cheops instruction slot; e.g. 544 DWORDs = 136 slots).
static void __stdcall Bridge_D3DDevice_RestoreVertexShaders(const DWORD* pBuffer, DWORD Size) {
    if (!pBuffer || Size == 0u) return;
    uint32_t slots = Size / 4u;
    if (slots == 0u) return;
    LoadCheopsProgramSlots(0u, reinterpret_cast<const uint32_t*>(pBuffer), slots);
}

static void __fastcall Bridge_D3DDevice_SetVertexShaderConstant1(int32_t reg, const void* pData) {
    ApplyVsConstants(reg, pData, 1);
}

static void __fastcall Bridge_D3DDevice_SetVertexShaderConstant4(int32_t reg, const void* pData) {
    ApplyVsConstants(reg, pData, 4);
}

// @D3DDevice_SetVertexShaderConstantNotInlineFast@12 (0x006B2A40): __fastcall (ecx=hwSlot, edx=pData, [esp+4]=dwordCount)
static void __fastcall Bridge_D3DDevice_SetVertexShaderConstantNotInlineFast(int32_t reg, const void* pData, uint32_t dwordCount) {
    ApplyVsConstantsDwords(reg, pData, dwordCount);
}

// @D3DDevice_SetVertexShaderConstantNotInline@12 (0x006B2B20): __fastcall (ecx=hwSlot, edx=pData, [esp+4]=dwordCount)
static void __fastcall Bridge_D3DDevice_SetVertexShaderConstantNotInline(int32_t reg, const void* pData, uint32_t dwordCount) {
    ApplyVsConstantsDwords(reg, pData, dwordCount);
}

// ============================================================================
// Morph-Target + GPU Bone Palette Skinning (cMeshGroup::RenderSetupBlendWeightedSkin)
// ============================================================================

const uint32_t* g_curPrelitColors        = nullptr;
uint32_t        g_curPrelitBaseVertex    = 0;
bool            g_curPrelitUseCustomBase = false;

static void SetupPrelitStreamForMesh(const uint8_t* pMeshGroup, const uint8_t* pModelInst) {
    g_curPrelitColors        = nullptr;
    g_curPrelitBaseVertex    = 0;
    g_curPrelitUseCustomBase = false;
    if (pMeshGroup && pModelInst && (pModelInst[0x14] & 1) != 0) {
        uint8_t lodIdx = pMeshGroup[0x50];
        if (lodIdx < 3) {
            XboxVertexBuffer* prelitVB = *reinterpret_cast<XboxVertexBuffer* const*>(pModelInst + 0xB0 + lodIdx * 4);
            if (prelitVB && prelitVB->Data) {
                g_curPrelitColors        = reinterpret_cast<const uint32_t*>(prelitVB->Data);
                g_curPrelitBaseVertex    = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
                g_curPrelitUseCustomBase = true;
            }
        }
    }
}

// Replacement for cMeshGroup::RenderSetupBlendWeightedSkin (0x00607390):
// Evaluates facial morph-target deltas on CPU into the dynamic VB while offloading 3-weight
// bone matrix skinning (position + normal) to BonePaletteCB : register(b2) in fixed_vs!
static uint32_t __fastcall Bridge_MeshGroup_RenderSetupBlendWeightedSkin(
    uint8_t* pMeshGroup, void* edxUnused, uint8_t* pModelInst, uint32_t lod)
{
    (void)edxUnused;
    g_activeGpuSkin.active = false;
    if (!pMeshGroup) return 0;

    uint32_t numVerts = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x14 + lod * 16);
    void* pDynVB = *reinterpret_cast<void**>(ADDR_DYN_VB_PTR);
    if (!pDynVB) return 0;

    if (!pModelInst || !*reinterpret_cast<void* const*>(pModelInst + 0xAC)) {
        return 0;
    }
    typedef const uint8_t* (__thiscall *PFN_GetBlendState)(void* pBlendCtrl, uint32_t blendGroupIdx);
    PFN_GetBlendState fnGetBlendState = reinterpret_cast<PFN_GetBlendState>(0x005E2CF0);
    const uint8_t* pBlendState = fnGetBlendState(*reinterpret_cast<void* const*>(pModelInst + 0xAC), pMeshGroup[0x53]);
    if (!pBlendState) {
        return 0;
    }

    // sBlendState layout (0x006073FA..0x0060754E): +0x00 = uint8_t numTargets (max 3),
    // +0x01..+0x03 = uint8_t targetIdx[3], +0x04..+0x0F = float targetWeights[3].
    uint32_t numTargets = pBlendState[0];
    if (numTargets > 3u) numTargets = 3u;
    const float* targetWeights = reinterpret_cast<const float*>(pBlendState + 4);
    const float* targetPtrs[3] = {};
    uint32_t lod0Verts = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x14);
    const uint8_t* morphBase = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x4C);
    if (!morphBase) {
        numTargets = 0u;
    }
    bool hasNonZeroWeight = false;
    for (uint32_t t = 0; t < numTargets; ++t) {
        targetPtrs[t] = reinterpret_cast<const float*>(morphBase + static_cast<uint32_t>(pBlendState[1 + t]) * lod0Verts * 24u);
        if (targetWeights[t] != 0.0f) {
            hasNonZeroWeight = true;
        }
    }
    if (!hasNonZeroWeight) {
        numTargets = 0u;
    }

    uint8_t lodIdx = pMeshGroup[0x50];
    const uint8_t* pModel = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x04);
    const uint8_t* pLod = pModel + static_cast<uint32_t>(lodIdx) * 24u;
    uint8_t vertType = pLod[0x24];
    uint32_t srcStride = *reinterpret_cast<const uint8_t*>(0x007AB8DC + vertType);
    uint32_t meshBaseVert = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
    const uint8_t* lodVertBase = *reinterpret_cast<const uint8_t* const*>(pLod + 0x18);
    const uint8_t* srcMeshStart = lodVertBase ? (lodVertBase + meshBaseVert * srcStride) : nullptr;
    const float* boneMats = *reinterpret_cast<const float* const*>(ADDR_CUR_BONE_MATS_PTR);

    uint32_t maxBoneIdx   = 0u;
    uint32_t outBaseVert  = 0u;
    uint32_t activeStride = 0x20u;
    XboxVertexBuffer* boundVbObj = nullptr;
    uintptr_t boundVbDataPtr = 0u;

    if (numTargets == 0u && srcMeshStart && srcStride >= 0x30u) {
        // [P1-3] Zero-copy fast path when no facial morph targets are active:
        // bind the static mesh vertex stream in-place without locking or copying into cDynamicVB.
        const uint8_t* srcPtr = srcMeshStart;
        for (uint32_t i = 0; i < numVerts; ++i) {
            uint32_t b0 = srcPtr[0x2C];
            uint32_t b1 = srcPtr[0x2D];
            uint32_t b2 = srcPtr[0x2E];
            if (b0 > maxBoneIdx) maxBoneIdx = b0;
            if (b1 > maxBoneIdx) maxBoneIdx = b1;
            if (b2 > maxBoneIdx) maxBoneIdx = b2;
            srcPtr += srcStride;
        }
        static XboxVertexBuffer s_zeroCopyMorphVB = { 0x01000001u, 0u, 0u };
        s_zeroCopyMorphVB.Common = 0x01000001u;
        s_zeroCopyMorphVB.Data   = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(srcMeshStart));
        s_zeroCopyMorphVB.Lock   = 0u;
        boundVbObj     = &s_zeroCopyMorphVB;
        boundVbDataPtr = reinterpret_cast<uintptr_t>(srcMeshStart);
        activeStride   = srcStride;
        outBaseVert    = 0u;
    } else {
        typedef uint8_t* (__thiscall *PFN_DynVB_Lock)(void* thisPtr, uint32_t count, uint32_t* outBaseVert);
        typedef void     (__thiscall *PFN_DynVB_Unlock)(void* thisPtr);
        PFN_DynVB_Lock   fnLock   = reinterpret_cast<PFN_DynVB_Lock>(0x005FBFA0);
        PFN_DynVB_Unlock fnUnlock = reinterpret_cast<PFN_DynVB_Unlock>(0x005FC010);

        uint8_t* dstBase = fnLock(pDynVB, numVerts, &outBaseVert);
        if (dstBase && srcMeshStart && numVerts > 0u) {
            const uint8_t* srcPtr = srcMeshStart;
            uint8_t* dstPtr = dstBase;
            for (uint32_t i = 0; i < numVerts; ++i) {
                const float* sf = reinterpret_cast<const float*>(srcPtr);
                float px = sf[0], py = sf[1], pz = sf[2];
                float nx = sf[3], ny = sf[4], nz = sf[5];
                float u  = sf[6], v  = sf[7];

                for (uint32_t t = 0; t < numTargets; ++t) {
                    const float* d = targetPtrs[t] + i * 6u;
                    float tw = targetWeights[t];
                    px += d[0] * tw;
                    py += d[1] * tw;
                    pz += d[2] * tw;
                }

                uint32_t b0 = srcPtr[0x2C];
                uint32_t b1 = srcPtr[0x2D];
                uint32_t b2 = srcPtr[0x2E];
                if (b0 > maxBoneIdx) maxBoneIdx = b0;
                if (b1 > maxBoneIdx) maxBoneIdx = b1;
                if (b2 > maxBoneIdx) maxBoneIdx = b2;

                float* df = reinterpret_cast<float*>(dstPtr);
                df[0] = px; df[1] = py; df[2] = pz;
                df[3] = nx; df[4] = ny; df[5] = nz;
                df[6] = u;  df[7] = v;

                srcPtr += srcStride;
                dstPtr += 0x20u;
            }
        }
        fnUnlock(pDynVB);

        boundVbObj     = *reinterpret_cast<XboxVertexBuffer**>(reinterpret_cast<uint8_t*>(pDynVB) + 0x10);
        boundVbDataPtr = (boundVbObj && boundVbObj->Data) ? static_cast<uintptr_t>(boundVbObj->Data) : 0u;
        activeStride   = 0x20u;
    }

    if (*reinterpret_cast<uint8_t*>(0x008AA454) == 0) {
        Bridge_D3DDevice_SetTransform(6, reinterpret_cast<const Mat44*>(0x007D0790));
        *reinterpret_cast<uint8_t*>(0x008AA454) = 1;
    }
    *reinterpret_cast<uint32_t*>(0x008AA468) = 0x112u;
    Bridge_D3DDevice_SetVertexShader(0x112u);

    *reinterpret_cast<XboxVertexBuffer**>(0x008AA480) = boundVbObj;
    Bridge_D3DDevice_SetStreamSource(0, boundVbObj, activeStride);

    XboxVertexBuffer* idxBuf = *reinterpret_cast<XboxVertexBuffer* const*>(pModel + 0x7C);
    Bridge_D3DDevice_SetIndices(idxBuf, outBaseVert);

    if (boundVbDataPtr != 0u && srcMeshStart && boneMats && numVerts > 0u) {
        uint32_t boneCount = (maxBoneIdx < 256u) ? (maxBoneIdx + 1u) : 256u;
        g_activeGpuSkin.dynVbDataPtr = boundVbDataPtr;
        g_activeGpuSkin.boundStride  = activeStride;
        g_activeGpuSkin.outBaseVert  = outBaseVert;
        g_activeGpuSkin.numVerts     = numVerts;
        g_activeGpuSkin.srcMeshVerts = srcMeshStart;
        g_activeGpuSkin.srcStride    = srcStride;
        g_activeGpuSkin.numBones     = boneCount;
        for (uint32_t b = 0; b < boneCount; ++b) {
            const float* m = boneMats + b * 16u;
            g_activeGpuSkin.boneRows[b * 3u + 0u][0] = m[0];
            g_activeGpuSkin.boneRows[b * 3u + 0u][1] = m[4];
            g_activeGpuSkin.boneRows[b * 3u + 0u][2] = m[8];
            g_activeGpuSkin.boneRows[b * 3u + 0u][3] = m[12];
            g_activeGpuSkin.boneRows[b * 3u + 1u][0] = m[1];
            g_activeGpuSkin.boneRows[b * 3u + 1u][1] = m[5];
            g_activeGpuSkin.boneRows[b * 3u + 1u][2] = m[9];
            g_activeGpuSkin.boneRows[b * 3u + 1u][3] = m[13];
            g_activeGpuSkin.boneRows[b * 3u + 2u][0] = m[2];
            g_activeGpuSkin.boneRows[b * 3u + 2u][1] = m[6];
            g_activeGpuSkin.boneRows[b * 3u + 2u][2] = m[10];
            g_activeGpuSkin.boneRows[b * 3u + 2u][3] = m[14];
        }
        g_activeGpuSkin.active = true;
    }

    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return 0;
}

static void OnXboxMemoryFreedPsHook(const void* ptr, size_t byteLen) {
    EvictPixelShaderPointerByRange(reinterpret_cast<uintptr_t>(ptr), byteLen);
}

void InitXboxD3D8ShaderBridges() {
    RegisterXboxMemoryFreeCallback(&OnXboxMemoryFreedPsHook);

    // 1. Bridge Cheops Vertex Shaders, Microcode Slot Load/Select/Restore & Constants
    InstallBridge(0x006B27F0, &Bridge_D3DDevice_CreateVertexShader);                   // _D3DDevice_CreateVertexShader@16
    InstallBridge(0x006B2BF0, &Bridge_D3DDevice_LoadVertexShader);                     // _D3DDevice_LoadVertexShader@8
    InstallBridge(0x006B2C50, &Bridge_D3DDevice_SelectVertexShader);                   // _D3DDevice_SelectVertexShader@8
    InstallBridge(0x006B2CF0, &Bridge_D3DDevice_SetShaderConstantMode);                // _D3DDevice_SetShaderConstantMode@4
    InstallBridge(0x006B2E10, &Bridge_D3DDevice_RestoreVertexShaders);                 // ?RestoreVertexShaders@D3D@@YGXPBKK@Z
    InstallBridge(0x006B2F40, &Bridge_D3DDevice_SetVertexShader);                      // _D3DDevice_SetVertexShader@4
    InstallBridge(0x006B2930, &Bridge_D3DDevice_SetVertexShaderConstant1);             // @D3DDevice_SetVertexShaderConstant1@8
    InstallBridge(0x006B2990, &Bridge_D3DDevice_SetVertexShaderConstant4);             // @D3DDevice_SetVertexShaderConstant4@8
    InstallBridge(0x006B2A40, &Bridge_D3DDevice_SetVertexShaderConstantNotInlineFast); // @D3DDevice_SetVertexShaderConstantNotInlineFast@12
    InstallBridge(0x006B2B20, &Bridge_D3DDevice_SetVertexShaderConstantNotInline);     // @D3DDevice_SetVertexShaderConstantNotInline@12

    // 2. Bridge NV2A Pixel Shaders & Constants
    InstallBridge(0x006B3C60, &Bridge_D3DDevice_CreatePixelShader);                    // _D3DDevice_CreatePixelShader@8
    InstallBridge(0x006B3CB0, &Bridge_D3DDevice_SetPixelShaderProgram);                // _D3DDevice_SetPixelShaderProgram@4
    InstallBridge(0x006B3CF0, &Bridge_D3DDevice_SetPixelShader);                       // _D3DDevice_SetPixelShader@4
    InstallBridge(0x006B3EF0, &Bridge_D3DDevice_SetPixelShaderConstant);               // _D3DDevice_SetPixelShaderConstant@12

    // 3. Bridge Facial Morph-Target + GPU Bone Palette Skinning (0x00607390).
    // All standard weighted-skin meshes & all 25 cShader::VertexSetup* functions run compiled Cheops .xvu on GPU.
    InstallBridge(0x00607390, &Bridge_MeshGroup_RenderSetupBlendWeightedSkin);         // cMeshGroup::RenderSetupBlendWeightedSkin
}
