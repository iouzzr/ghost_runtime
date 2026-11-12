#include "ghost_runtime.h"

// ============================================================================
// Xbox Input Structures (Exact 22-byte XINPUT_STATE layout)
// ============================================================================

#pragma pack(push, 1)
struct XBOX_XINPUT_GAMEPAD {
    uint16_t wButtons;          // +0x00
    uint8_t  bAnalogButtons[8]; // +0x02 (0=A, 1=B, 2=X, 3=Y, 4=Black, 5=White, 6=LTrig, 7=RTrig)
    int16_t  sThumbLX;          // +0x0A
    int16_t  sThumbLY;          // +0x0C
    int16_t  sThumbRX;          // +0x0E
    int16_t  sThumbRY;          // +0x10
};

struct XBOX_XINPUT_STATE {
    uint32_t            dwPacketNumber; // +0x00
    XBOX_XINPUT_GAMEPAD Gamepad;        // +0x04 (18 bytes)
};

struct XBOX_XINPUT_KEYSTROKE {
    uint8_t VirtualKey; // +0x00
    char    Ascii;      // +0x01
    uint8_t Flags;      // +0x02 (0x01=CTRL, 0x02=SHIFT, 0x04=ALT, 0x40=KEYUP, 0x80=REPEAT)
};
static_assert(sizeof(XBOX_XINPUT_KEYSTROKE) == 3, "XBOX_XINPUT_KEYSTROKE must be 3 bytes");
#pragma pack(pop)

// Xbox wButtons bitmasks (identical to Windows XInput for bits 0..7)
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

// Windows XInput dynamic loader
#pragma pack(push, 1)
struct WIN_XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct WIN_XINPUT_STATE {
    DWORD              dwPacketNumber;
    WIN_XINPUT_GAMEPAD Gamepad;
};
struct WIN_XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};
#pragma pack(pop)

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, WIN_XINPUT_STATE* pState);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD dwUserIndex, WIN_XINPUT_VIBRATION* pVibration);
static PFN_XInputGetState g_pfnXInputGetState = nullptr;
static PFN_XInputSetState g_pfnXInputSetState = nullptr;

static uint32_t             g_packetCounter            = 1;
static volatile uint32_t    g_connectedPadMask         = 0x00000001u; // Port 0 always available for KBM + Pad 0
static uint32_t             g_reportedPadMask          = 0x00000000u;
static bool                 g_physPadPresent[4]        = { false, false, false, false };
static DWORD                g_lastPhysProbeTick        = 0;
static volatile LONG        g_deviceChangePending      = 0;
static uint32_t             g_emptySlotRescanBudget    = 0;
static WIN_XINPUT_VIBRATION g_lastRequestedVib[4]      = {};
static bool                 g_vibrationMutedForFocus   = false;

void NotifyHostDeviceChange() {
    InterlockedExchange(&g_deviceChangePending, 1);
}

// Debug Keyboard Queue
static XBOX_XINPUT_KEYSTROKE g_keyQueue[64];
static volatile int g_keyHead = 0;
static volatile int g_keyTail = 0;

void StopAllControllerVibration() {
    if (!g_pfnXInputSetState) return;
    WIN_XINPUT_VIBRATION zeroVib = { 0, 0 };
    for (DWORD i = 0; i < 4; ++i) {
        g_lastRequestedVib[i] = zeroVib;
        if (g_physPadPresent[i] || i == 0) {
            g_pfnXInputSetState(i, &zeroVib);
        }
    }
}

void PushDebugKeystroke(uint16_t vk, wchar_t ch, bool keyUp) {
    int next = (g_keyHead + 1) & 63;
    if (next == g_keyTail) return;
    if (vk == VK_SHIFT)   vk = VK_LSHIFT;
    if (vk == VK_CONTROL) vk = VK_LCONTROL;
    if (vk == VK_MENU)    vk = VK_LMENU;

    uint8_t flags = keyUp ? 0x40u : 0x00u; // 0x40 = XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) flags |= 0x01u;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) flags |= 0x02u;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) flags |= 0x04u;

    g_keyQueue[g_keyHead].VirtualKey = static_cast<uint8_t>(vk & 0xFFu);
    g_keyQueue[g_keyHead].Ascii      = (ch >= 1 && ch <= 127) ? static_cast<char>(ch) : 0;
    g_keyQueue[g_keyHead].Flags      = flags;
    g_keyHead = next;
}

static void RefreshPhysicalControllers(bool forceProbeAll) {
    if (!g_pfnXInputGetState) {
        g_connectedPadMask = 0x00000001u;
        return;
    }
    bool devChanged = (InterlockedExchange(&g_deviceChangePending, 0) != 0);
    if (devChanged) {
        // Probe immediately on WM_DEVICECHANGE plus 2 follow-up 500ms ticks for wireless adapter enumeration
        g_emptySlotRescanBudget = 3u;
    }
    DWORD now = QueryXboxTickCountMs();
    if (!forceProbeAll && !devChanged && (now - g_lastPhysProbeTick) < 500u) {
        return;
    }

    bool probeEmptySlots = forceProbeAll || (g_emptySlotRescanBudget > 0u);
    bool anyConnected = (g_physPadPresent[0] || g_physPadPresent[1] || g_physPadPresent[2] || g_physPadPresent[3]);
    if (!probeEmptySlots && !anyConnected) {
        g_lastPhysProbeTick = now;
        return;
    }
    g_lastPhysProbeTick = now;
    if (!forceProbeAll && g_emptySlotRescanBudget > 0u) {
        --g_emptySlotRescanBudget;
    }

    uint32_t mask = 0x00000001u; // Port 0 always connected (Keyboard/Mouse + optional physical Pad 0)
    for (DWORD i = 0; i < 4; ++i) {
        if (!probeEmptySlots && !g_physPadPresent[i]) {
            continue;
        }
        WIN_XINPUT_STATE xst;
        bool ok = (g_pfnXInputGetState(i, &xst) == ERROR_SUCCESS);
        g_physPadPresent[i] = ok;
        if (ok) {
            mask |= (1u << i);
        } else {
            g_lastRequestedVib[i] = { 0, 0 };
        }
    }
    g_connectedPadMask = mask;
}

void UpdateXboxInputTick() {
    g_packetCounter++;
    RefreshPhysicalControllers(false);

    bool focused = (g_hWnd != nullptr && GetForegroundWindow() == g_hWnd);
    if (!focused && !g_vibrationMutedForFocus) {
        g_vibrationMutedForFocus = true;
        if (g_pfnXInputSetState) {
            WIN_XINPUT_VIBRATION zeroVib = { 0, 0 };
            for (DWORD i = 0; i < 4; ++i) {
                if (g_physPadPresent[i]) {
                    g_pfnXInputSetState(i, &zeroVib);
                }
            }
        }
    } else if (focused && g_vibrationMutedForFocus) {
        g_vibrationMutedForFocus = false;
        if (g_pfnXInputSetState) {
            for (DWORD i = 0; i < 4; ++i) {
                if (g_physPadPresent[i]) {
                    g_pfnXInputSetState(i, &g_lastRequestedVib[i]);
                }
            }
        }
    }
}

static void __stdcall Bridge_XInitDevices(DWORD dwPreallocTypeCount, void* PreallocTypes) {
    (void)dwPreallocTypeCount; (void)PreallocTypes;
    g_reportedPadMask = 0u;
    RefreshPhysicalControllers(true);
    *reinterpret_cast<uint32_t*>(0x00747C38) = 1; // _XPP_XInitDevicesHasBeenCalled
}

static DWORD __stdcall Bridge_XGetDevices(void* pDeviceType) {
    uintptr_t devType = reinterpret_cast<uintptr_t>(pDeviceType);
    // _XDEVICE_TYPE_GAMEPAD_TABLE (0x00747B8C)
    if (devType == 0x00747B8C) {
        RefreshPhysicalControllers(false);
        uint32_t cur = g_connectedPadMask;
        g_reportedPadMask = cur;
        return cur;
    }
    return 0;
}

static BOOL __stdcall Bridge_XGetDeviceChanges(void* pDeviceType, DWORD* pdwInsertions, DWORD* pdwRemovals) {
    if (pdwInsertions) *pdwInsertions = 0;
    if (pdwRemovals)   *pdwRemovals   = 0;

    uintptr_t devType = reinterpret_cast<uintptr_t>(pDeviceType);
    if (devType == 0x00747B8C) {
        RefreshPhysicalControllers(false);
        uint32_t cur = g_connectedPadMask;
        uint32_t ins = cur & ~g_reportedPadMask;
        uint32_t rem = ~cur & g_reportedPadMask;
        g_reportedPadMask = cur;
        if (pdwInsertions) *pdwInsertions = ins;
        if (pdwRemovals)   *pdwRemovals   = rem;
        return (ins | rem) != 0 ? TRUE : FALSE;
    }
    return FALSE;
}

static HANDLE __stdcall Bridge_XInputOpen(
    void* pDeviceType, DWORD dwPort, DWORD dwSlot, void* pPollingParameters)
{
    (void)pDeviceType; (void)dwSlot; (void)pPollingParameters;
    if (dwPort < 4 && (g_connectedPadMask & (1u << dwPort)) != 0) {
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1001u + dwPort));
    }
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return nullptr;
}

static void __stdcall Bridge_XInputClose(HANDLE hDevice) {
    uintptr_t h = reinterpret_cast<uintptr_t>(hDevice);
    if (h >= 0x1001u && h <= 0x1004u) {
        DWORD port = static_cast<DWORD>(h - 0x1001u);
        g_lastRequestedVib[port] = { 0, 0 };
        if (g_pfnXInputSetState && g_physPadPresent[port]) {
            WIN_XINPUT_VIBRATION zeroVib = { 0, 0 };
            g_pfnXInputSetState(port, &zeroVib);
        }
    }
}

GhostKeyBindings g_keyBindings = {
    'C',        // vkCrouch      (L-Trigger)
    VK_CONTROL, // vkFire        (R-Trigger)
    'W',        // vkMoveUp      (Left Stick Up)
    'S',        // vkMoveDown    (Left Stick Down)
    'A',        // vkMoveLeft    (Left Stick Left)
    'D',        // vkMoveRight   (Left Stick Right)
    VK_UP,      // vkSight       (D-Pad Up)
    VK_RETURN,  // vkMenuStart   (Start)
    VK_TAB,     // vkMenuBack    (Back)
    VK_LEFT,    // vkCloak       (D-Pad Left)
    VK_DOWN,    // vkGunLight    (D-Pad Down: Gun Light / Psi Charge)
    VK_RIGHT,   // vkDpadRight   (D-Pad Right: --)
    'I',        // vkAimUp       (Right Stick Up)
    'K',        // vkAimDown     (Right Stick Down)
    'J',        // vkAimLeft     (Right Stick Left)
    'L',        // vkAimRight    (Right Stick Right)
    'Z',        // vkAimClick    (Right Stick Click: Sniper)
    'R',        // vkMelee       (X: Melee Attack)
    'F',        // vkWeaponMode  (Y: Weapon Mode)
    'E',        // vkAction      (B: Action)
    VK_SPACE,   // vkJump        (A: Jump)
    'Q',        // vkNextFirearm (Black: Next Firearm)
    'G',        // vkNextGrenade (White: Next Grenade)
};

void ResetKeyBindingsToDefault(GhostKeyBindings* kb) {
    if (!kb) return;
    kb->vkCrouch      = 'C';
    kb->vkFire        = VK_CONTROL;
    kb->vkMoveUp      = 'W';
    kb->vkMoveDown    = 'S';
    kb->vkMoveLeft    = 'A';
    kb->vkMoveRight   = 'D';
    kb->vkSight       = VK_UP;
    kb->vkMenuStart   = VK_RETURN;
    kb->vkMenuBack    = VK_TAB;
    kb->vkCloak       = VK_LEFT;
    kb->vkGunLight    = VK_DOWN;
    kb->vkDpadRight   = VK_RIGHT;
    kb->vkAimUp       = 'I';
    kb->vkAimDown     = 'K';
    kb->vkAimLeft     = 'J';
    kb->vkAimRight    = 'L';
    kb->vkAimClick    = 'Z';
    kb->vkMelee       = 'R';
    kb->vkWeaponMode  = 'F';
    kb->vkAction      = 'E';
    kb->vkJump        = VK_SPACE;
    kb->vkNextFirearm = 'Q';
    kb->vkNextGrenade = 'G';
}

static bool IsRawVkDown(int vk) {
    if (vk <= 0 || GetForegroundWindow() != g_hWnd) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool IsExactVkBoundInConfig(int vk) {
    if (vk <= 0) return false;
    const int* slots = reinterpret_cast<const int*>(&g_keyBindings);
    for (size_t i = 0; i < kNumKeyBindSlots; ++i) {
        if (slots[i] == vk) return true;
    }
    return false;
}

static bool IsModifierPairDown(int genVk, int leftVk, int rightVk) {
    bool lClaimed = IsExactVkBoundInConfig(leftVk);
    bool rClaimed = IsExactVkBoundInConfig(rightVk);
    bool lDown    = IsRawVkDown(leftVk);
    bool rDown    = IsRawVkDown(rightVk);
    if (lDown && !lClaimed) return true;
    if (rDown && !rClaimed) return true;
    if (!lDown && !rDown && (!lClaimed || !rClaimed) && IsRawVkDown(genVk)) return true;
    return false;
}

// Evaluates any bound virtual-key slot while respecting left/right modifier splits:
// if a slot uses generic VK_SHIFT / VK_CONTROL / VK_MENU (e.g. default Fire=VK_CONTROL)
// and the user binds a side-specific key (e.g. VK_RCONTROL or VK_LSHIFT) in Keyboard Config...,
// pressing that side-specific key triggers ONLY its assigned action without also firing the generic slot.
static bool IsBoundVkDown(int vk) {
    if (vk <= 0) return false;
    switch (vk) {
        case VK_SHIFT:   return IsModifierPairDown(VK_SHIFT,   VK_LSHIFT,   VK_RSHIFT);
        case VK_CONTROL: return IsModifierPairDown(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
        case VK_MENU:    return IsModifierPairDown(VK_MENU,    VK_LMENU,    VK_RMENU);
        default:         return IsRawVkDown(vk);
    }
}

static DWORD __stdcall Bridge_XInputGetState(HANDLE hDevice, XBOX_XINPUT_STATE* pState) {
    if (!pState) return ERROR_INVALID_PARAMETER;
    uintptr_t h = reinterpret_cast<uintptr_t>(hDevice);
    if (h < 0x1001u || h > 0x1004u) return ERROR_INVALID_HANDLE;
    DWORD port = static_cast<DWORD>(h - 0x1001u);
    if ((g_connectedPadMask & (1u << port)) == 0) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pState, 0, sizeof(*pState));
    pState->dwPacketNumber = g_packetCounter;

    // 1. Poll physical XInput controller if present on this port
    if (g_pfnXInputGetState && g_physPadPresent[port]) {
        WIN_XINPUT_STATE xst;
        if (g_pfnXInputGetState(port, &xst) == ERROR_SUCCESS) {
            if (GetForegroundWindow() == g_hWnd) {
                pState->Gamepad.wButtons = xst.Gamepad.wButtons & 0x00FF;
                if (xst.Gamepad.wButtons & 0x1000) pState->Gamepad.bAnalogButtons[0] = 255; // A
                if (xst.Gamepad.wButtons & 0x2000) pState->Gamepad.bAnalogButtons[1] = 255; // B
                if (xst.Gamepad.wButtons & 0x4000) pState->Gamepad.bAnalogButtons[2] = 255; // X
                if (xst.Gamepad.wButtons & 0x8000) pState->Gamepad.bAnalogButtons[3] = 255; // Y
                if (xst.Gamepad.wButtons & 0x0200) pState->Gamepad.bAnalogButtons[4] = 255; // RB -> Black
                if (xst.Gamepad.wButtons & 0x0100) pState->Gamepad.bAnalogButtons[5] = 255; // LB -> White
                pState->Gamepad.bAnalogButtons[6] = xst.Gamepad.bLeftTrigger;
                pState->Gamepad.bAnalogButtons[7] = xst.Gamepad.bRightTrigger;
                pState->Gamepad.sThumbLX = xst.Gamepad.sThumbLX;
                pState->Gamepad.sThumbLY = xst.Gamepad.sThumbLY;
                pState->Gamepad.sThumbRX = xst.Gamepad.sThumbRX;
                pState->Gamepad.sThumbRY = xst.Gamepad.sThumbRY;
            }
        } else {
            g_physPadPresent[port] = false;
            if (port != 0) {
                g_connectedPadMask &= ~(1u << port);
                return ERROR_DEVICE_NOT_CONNECTED;
            }
        }
    }

    // 2. Overlay user-configured Keyboard & Mouse controls on Port 0 when window is focused
    if (port == 0 && GetForegroundWindow() == g_hWnd) {
        // D-Pad: Sight (Up), Gun Light / Psi Charge (Down), Cloak (Left), -- (Right)
        if (IsBoundVkDown(g_keyBindings.vkSight))     pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
        if (IsBoundVkDown(g_keyBindings.vkGunLight))  pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        if (IsBoundVkDown(g_keyBindings.vkCloak))     pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
        if (IsBoundVkDown(g_keyBindings.vkDpadRight)) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;

        // Menu (Start & Back; Esc falls back to Back only when Esc is not explicitly bound to another slot)
        if (IsBoundVkDown(g_keyBindings.vkMenuStart)) pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
        if (IsBoundVkDown(g_keyBindings.vkMenuBack) ||
            (!IsExactVkBoundInConfig(VK_ESCAPE) && IsRawVkDown(VK_ESCAPE))) {
            pState->Gamepad.wButtons |= XBOX_GAMEPAD_BACK;
        }

        // Right Stick Click: Sniper (click)
        if (IsBoundVkDown(g_keyBindings.vkAimClick))  pState->Gamepad.wButtons |= XBOX_GAMEPAD_RIGHT_THUMB;

        // Analog buttons (0=A Jump, 1=B Action, 2=X Melee, 3=Y Weapon Mode, 4=Black Next Firearm, 5=White Next Grenade, 6=LTrig Crouch, 7=RTrig Fire)
        if (IsBoundVkDown(g_keyBindings.vkJump))        pState->Gamepad.bAnalogButtons[0] = 255;
        if (IsBoundVkDown(g_keyBindings.vkAction))      pState->Gamepad.bAnalogButtons[1] = 255;
        if (IsBoundVkDown(g_keyBindings.vkMelee))       pState->Gamepad.bAnalogButtons[2] = 255;
        if (IsBoundVkDown(g_keyBindings.vkWeaponMode))  pState->Gamepad.bAnalogButtons[3] = 255;
        if (IsBoundVkDown(g_keyBindings.vkNextFirearm)) pState->Gamepad.bAnalogButtons[4] = 255;
        if (IsBoundVkDown(g_keyBindings.vkNextGrenade)) pState->Gamepad.bAnalogButtons[5] = 255;
        if (IsBoundVkDown(g_keyBindings.vkCrouch))      pState->Gamepad.bAnalogButtons[6] = 255;
        if (IsBoundVkDown(g_keyBindings.vkFire))        pState->Gamepad.bAnalogButtons[7] = 255;

        // Left Stick: Move
        // Matched to Ghost.exe XReadGamepad (0x005A8100: per-axis 7700 deadzone, /25067 scale)
        // and cControl::GetPlayerRelativeJoystick (0x00459A60: unit-circle len clamp):
        //   cardinal 32767 -> norm 1.0000 -> len 1.0000 -> 4.00 m/s
        //   diagonal 25426 -> norm 0.7071 -> len 1.0001 -> clamped 1.0000 -> 4.00 m/s
        bool moveL = IsBoundVkDown(g_keyBindings.vkMoveLeft);
        bool moveR = IsBoundVkDown(g_keyBindings.vkMoveRight);
        bool moveD = IsBoundVkDown(g_keyBindings.vkMoveDown);
        bool moveU = IsBoundVkDown(g_keyBindings.vkMoveUp);
        if (moveL || moveR || moveD || moveU) {
            int moveX = (moveR ? 1 : 0) - (moveL ? 1 : 0);
            int moveY = (moveU ? 1 : 0) - (moveD ? 1 : 0);
            int amp   = 0;
            if (moveX != 0 || moveY != 0) {
                bool diag = (moveX != 0 && moveY != 0);
                amp       = diag ? 25426 : 32767;
            }
            pState->Gamepad.sThumbLX = static_cast<int16_t>(moveX * amp);
            pState->Gamepad.sThumbLY = static_cast<int16_t>(moveY * amp);
        }

        // Right Stick: Aim / Camera
        // Ghost.exe UpdateCameraThirdPerson (0x00456B90) and UpdateCameraSniper (0x0046ED40)
        // evaluate cControl::GetCameraYaw (0x00453430) and GetCameraPitch (0x004536A0) on X and Y
        // independently with quadratic curve ((|a| - 0.1) / 0.9)^2 (and GetCameraAxes uses
        // mathVector_ProjectCube, not radial clamping). Keep full per-axis 28000 deflection on
        // diagonals so pressing J+K together never reduces yaw or pitch turn speed.
        bool aimL = IsBoundVkDown(g_keyBindings.vkAimLeft);
        bool aimR = IsBoundVkDown(g_keyBindings.vkAimRight);
        bool aimD = IsBoundVkDown(g_keyBindings.vkAimDown);
        bool aimU = IsBoundVkDown(g_keyBindings.vkAimUp);
        if (aimL || aimR || aimD || aimU) {
            int aimX = (aimR ? 1 : 0) - (aimL ? 1 : 0);
            int aimY = (aimU ? 1 : 0) - (aimD ? 1 : 0);
            pState->Gamepad.sThumbRX = static_cast<int16_t>(aimX * 28000);
            pState->Gamepad.sThumbRY = static_cast<int16_t>(aimY * 28000);
        }
    }

    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XInputSetState(HANDLE hDevice, void* pFeedback) {
    if (!pFeedback) return ERROR_INVALID_PARAMETER;
    uintptr_t h = reinterpret_cast<uintptr_t>(hDevice);
    if (h < 0x1001u || h > 0x1004u) return ERROR_INVALID_HANDLE;
    DWORD port = static_cast<DWORD>(h - 0x1001u);
    if ((g_connectedPadMask & (1u << port)) == 0) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    uint8_t* fbBytes = reinterpret_cast<uint8_t*>(pFeedback);
    // Verified Ghost.exe (0x005A8910..0x005A8A4D & _XInputSetState@8 at 0x00749154) XINPUT_FEEDBACK layout:
    // +0x00: DWORD  dwStatus
    // +0x04: HANDLE hEvent
    // +0x08..+0x41: XINPUT_FEEDBACK_HEADERInternal[58]
    // +0x42: WORD   wLeftMotorSpeed
    // +0x44: WORD   wRightMotorSpeed
    WIN_XINPUT_VIBRATION vib = {};
    vib.wLeftMotorSpeed  = *reinterpret_cast<const uint16_t*>(fbBytes + 0x42);
    vib.wRightMotorSpeed = *reinterpret_cast<const uint16_t*>(fbBytes + 0x44);
    g_lastRequestedVib[port] = vib;

    if (g_pfnXInputSetState && g_physPadPresent[port]) {
        WIN_XINPUT_VIBRATION outVib = (GetForegroundWindow() == g_hWnd) ? vib : WIN_XINPUT_VIBRATION{ 0, 0 };
        g_pfnXInputSetState(port, &outVib);
    }

    // Complete feedback header status immediately so next frame's poll (0x005A8918) accepts updates,
    // while returning ERROR_IO_PENDING (997 / 0x3E5) as required by caller at 0x005A898F so its
    // rumble duration countdown ([esi+0x50]) is preserved rather than cleared.
    *reinterpret_cast<uint32_t*>(fbBytes + 0x00) = ERROR_SUCCESS;
    HANDLE hEvent = *reinterpret_cast<HANDLE*>(fbBytes + 0x04);
    if (hEvent) {
        SetEvent(hEvent);
    }
    return ERROR_IO_PENDING;
}

static DWORD __stdcall Bridge_XInputDebugInitKeyboardQueue(void* pParameters) {
    (void)pParameters;
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XInputDebugGetKeystroke(XBOX_XINPUT_KEYSTROKE* pKeystroke) {
    if (!pKeystroke) return ERROR_INVALID_PARAMETER;
    if (g_keyTail == g_keyHead) {
        return ERROR_EMPTY;
    }
    *pKeystroke = g_keyQueue[g_keyTail];
    g_keyTail = (g_keyTail + 1) & 63;
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XMountMUA(DWORD dwPort, DWORD dwSlot, char* pchDrive) {
    (void)dwPort; (void)dwSlot;
    if (pchDrive) *pchDrive = '\0';
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD __stdcall Bridge_XUnmountMU(DWORD dwPort, DWORD dwSlot) {
    (void)dwPort; (void)dwSlot;
    return ERROR_SUCCESS;
}

void InitXboxInput() {
    HMODULE hXInput = LoadLibraryA("xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput1_3.dll");
    if (hXInput) {
        g_pfnXInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hXInput, "XInputGetState"));
        g_pfnXInputSetState = reinterpret_cast<PFN_XInputSetState>(GetProcAddress(hXInput, "XInputSetState"));
    }
    RefreshPhysicalControllers(true);

    // Bridge XInput & XPP functions
    InstallBridge(0x007491A9, &Bridge_XInitDevices);                 // _XInitDevices@8
    InstallBridge(0x007491AE, &Bridge_XGetDevices);                  // _XGetDevices@4
    InstallBridge(0x007491D0, &Bridge_XGetDeviceChanges);            // _XGetDeviceChanges@12
    InstallBridge(0x0074907F, &Bridge_XInputOpen);                   // _XInputOpen@16
    InstallBridge(0x007490D5, &Bridge_XInputClose);                  // _XInputClose@4
    InstallBridge(0x007490E1, &Bridge_XInputGetState);               // _XInputGetState@8
    InstallBridge(0x00749154, &Bridge_XInputSetState);               // _XInputSetState@8
    InstallBridge(0x0074923D, &Bridge_XMountMUA);                    // _XMountMUA@12
    InstallBridge(0x0074936F, &Bridge_XUnmountMU);                   // _XUnmountMU@8
    InstallBridge(0x0074FB64, &Bridge_XInputDebugInitKeyboardQueue); // _XInputDebugInitKeyboardQueue@4
    InstallBridge(0x0074F8A3, &Bridge_XInputDebugGetKeystroke);      // _XInputDebugGetKeystroke@4

    // Skip the 1.5-second Memory Unit startup enumeration wait in cXBoxMemoryUnits::Startup (0x005AC2F0)
    PatchRet(0x005AC2F0, 0, 1, true);
}
