/*
 * Wine-NX synthetic DirectInput joystick
 *
 * Exposes Autorun's existing XInput controller as a legacy DirectInput
 * joystick. Horizon has no HID gamepad device, so Wine's normal HID-backed
 * DirectInput joystick cannot see the Switch controller.
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "dinput.h"
#include "xinput.h"

#include "dinput_private.h"
#include "device_private.h"

#include "initguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

/* Stable instance GUID for the synthetic Switch pad ("NXPAD-GA..." in bytes). */
DEFINE_GUID(nx_joystick_guid, 0x4e585041, 0x442d, 0x4741, 0x8d, 0x49, 0x52, 0x55, 0x4e, 0x50, 0x41, 0x44);

struct nx_joystick
{
    struct dinput_device base;
};

typedef DWORD (WINAPI *xinput_get_state_func)(DWORD, XINPUT_STATE *);

static HMODULE xinput_module;
static xinput_get_state_func pXInputGetState;
static BOOL xinput_tried;

/*
 * Autorun's native input layer also maps the Switch pad to keyboard/mouse.
 * It suppresses that mapping while XInput is being polled. Legacy DirectInput
 * games may create our joystick once and then poll it irregularly (or only
 * while a specific controls page is active), which lets the keyboard mapping
 * wake back up and overlap the DirectInput device.
 *
 * Keep XInput "recent" for the lifetime of a process once it has created the
 * synthetic DirectInput controller. This makes the DirectInput bridge claim
 * the pad in the same way a native XInput game does.
 */
static BOOL nx_load_xinput(void);
static INIT_ONCE nx_claim_once = INIT_ONCE_STATIC_INIT;

static DWORD WINAPI nx_claim_thread(void *arg)
{
    XINPUT_STATE state;

    for (;;)
    {
        if (pXInputGetState) pXInputGetState(0, &state);
        Sleep(250);
    }
    return 0;
}

static BOOL WINAPI nx_start_claim_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;

    if (!nx_load_xinput()) return TRUE;
    thread = CreateThread(NULL, 0, nx_claim_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    return TRUE;
}

static void nx_claim_controller(void)
{
    InitOnceExecuteOnce(&nx_claim_once, nx_start_claim_once, NULL, NULL);
}

/* NFSU2: load the Widescreen Fix without a game-local dinput8.dll.
 *
 * On Windows the release normally uses Ultimate ASI Loader as dinput8.dll.
 * Autorun already needs its own dinput8.dll for the native DirectInput bridge,
 * so replacing it would undo controller support. Load the ASI directly instead,
 * after the loader lock is out of the way.
 */
static BOOL nx_is_nfsu2(void);
static INIT_ONCE nx_nfsu2_ws_once = INIT_ONCE_STATIC_INIT;

static DWORD WINAPI nx_nfsu2_ws_thread(void *arg)
{
    WCHAR exe[MAX_PATH], path[MAX_PATH], *slash;

    if (!GetModuleFileNameW(NULL, exe, ARRAY_SIZE(exe))) return 0;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    *slash = 0;

    if (swprintf(path, ARRAY_SIZE(path), L"%s\\scripts\\NFSUnderground2.WidescreenFix.asi", exe) <= 0)
        return 0;

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
    {
        HMODULE mod = LoadLibraryW(path);
        TRACE("NFSU2 Widescreen Fix direct load: %s (%p)\n", debugstr_w(path), mod);
    }
    return 0;
}

static BOOL WINAPI nx_start_nfsu2_ws_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;

    if (!nx_is_nfsu2()) return TRUE;
    thread = CreateThread(NULL, 0, nx_nfsu2_ws_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    return TRUE;
}

static void nx_load_nfsu2_widescreen_fix(void)
{
    InitOnceExecuteOnce(&nx_nfsu2_ws_once, nx_start_nfsu2_ws_once, NULL, NULL);
}

static inline struct nx_joystick *impl_from_IDirectInputDevice8W(IDirectInputDevice8W *iface)
{
    return CONTAINING_RECORD(CONTAINING_RECORD(iface, struct dinput_device, IDirectInputDevice8W_iface),
                             struct nx_joystick, base);
}

static BOOL nx_load_xinput(void)
{
    if (xinput_tried) return !!pXInputGetState;
    xinput_tried = TRUE;

    xinput_module = LoadLibraryW(L"xinput1_3.dll");
    if (!xinput_module) xinput_module = LoadLibraryW(L"xinput1_4.dll");
    if (xinput_module)
        pXInputGetState = (xinput_get_state_func)GetProcAddress(xinput_module, "XInputGetState");

    TRACE("Autorun DirectInput bridge: XInputGetState %s\n", pXInputGetState ? "ready" : "unavailable");
    return !!pXInputGetState;
}

static BOOL nx_get_xinput_state(XINPUT_STATE *state)
{
    memset(state, 0, sizeof(*state));
    return nx_load_xinput() && pXInputGetState(0, state) == ERROR_SUCCESS;
}

/*
 * NFSU2 US 1.2 native DirectInput integration.
 *
 * XtendedInput documents these data addresses for the US 1.2 executable:
 *   GAMEFLOWMANAGER_STATUS_ADDR = 0x008654A4
 *   JOYSTICKTYPE_P1_ADDR        = 0x00864788
 *   DEVICE_COUNT_ADDR           = 0x00870764
 *
 * We do not replace the game's input scanner here. We only make the synthetic
 * DirectInput pad look like a normal PC joystick to the game's own controller
 * code and add the common "left stick also navigates menus" behaviour.
 */
static DWORD nx_pov(WORD buttons);

static BOOL nx_is_nfsu2(void)
{
    static int cached = -1;
    WCHAR path[MAX_PATH], *name;

    if (cached >= 0) return cached;
    cached = 0;
    if (!GetModuleFileNameW(NULL, path, ARRAY_SIZE(path))) return FALSE;
    name = wcsrchr(path, L'\\');
    if (!name) name = wcsrchr(path, L'/');
    name = name ? name + 1 : path;
    if (!wcsicmp(name, L"speed2.exe")) cached = 1;
    return cached;
}

static BOOL nx_game_ptr_ok(const void *ptr, SIZE_T size, BOOL write)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD protect;

    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return FALSE;
    if ((const BYTE *)ptr + size > (const BYTE *)mbi.BaseAddress + mbi.RegionSize) return FALSE;
    protect = mbi.Protect & 0xff;
    if (protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return FALSE;
    if (write && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
        protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY) return FALSE;
    return TRUE;
}

static void nx_nfsu2_mark_controller(void)
{
    volatile DWORD *device_count = (volatile DWORD *)0x00870764;
    volatile BYTE *joystick_type = (volatile BYTE *)0x00864788;

    if (!nx_is_nfsu2()) return;
    if (nx_game_ptr_ok((const void *)device_count, sizeof(*device_count), TRUE)) *device_count = 2;
    if (nx_game_ptr_ok((const void *)joystick_type, sizeof(*joystick_type), TRUE)) *joystick_type = 1;
}

/*
 * NFSU2 US 1.2 has six built-in resolution slots:
 *   widths  @ 0x00800538 = 640,800,1024,1280,1280,1600
 *   heights @ 0x00800550 = 480,600,768,960,1024,1200
 *
 * Widescreen Fix replaces the game's resolution machinery, but its injected
 * code is not safe under Wine-NX's 32-bit forwarder. For Switch, a much
 * smaller patch is enough: replace the 800x600 slot with the native handheld
 * framebuffer size 1280x720 and keep g_RacingResolution on slot 1.
 *
 * Only apply when the expected vanilla values are present, so other exe
 * revisions are left untouched.
 */
static INIT_ONCE nx_nfsu2_res_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI nx_patch_nfsu2_resolution_once(INIT_ONCE *once, void *param, void **context)
{
    volatile DWORD *widths = (volatile DWORD *)0x00800538;
    volatile DWORD *heights = (volatile DWORD *)0x00800550;
    volatile DWORD *racing_resolution = (volatile DWORD *)0x00870D1C;
    DWORD old_w = 0, old_h = 0;
    BOOL ok_w = FALSE, ok_h = FALSE;

    if (!nx_is_nfsu2()) return TRUE;
    if (!nx_game_ptr_ok((const void *)widths, 6 * sizeof(DWORD), FALSE) ||
        !nx_game_ptr_ok((const void *)heights, 6 * sizeof(DWORD), FALSE))
        return TRUE;

    if (widths[1] != 800 || heights[1] != 600)
    {
        TRACE("NFSU2 resolution table unexpected: slot1=%lu x %lu\n", widths[1], heights[1]);
        return TRUE;
    }

    if (VirtualProtect((void *)&widths[1], sizeof(DWORD), PAGE_READWRITE, &old_w))
    {
        widths[1] = 1280;
        VirtualProtect((void *)&widths[1], sizeof(DWORD), old_w, &old_w);
        ok_w = TRUE;
    }
    if (VirtualProtect((void *)&heights[1], sizeof(DWORD), PAGE_READWRITE, &old_h))
    {
        heights[1] = 720;
        VirtualProtect((void *)&heights[1], sizeof(DWORD), old_h, &old_h);
        ok_h = TRUE;
    }

    if (ok_w && ok_h)
    {
        if (nx_game_ptr_ok((const void *)racing_resolution, sizeof(*racing_resolution), TRUE))
            *racing_resolution = 1;
        TRACE("NFSU2 resolution slot 1 patched to 1280x720\n");
    }
    return TRUE;
}

static void nx_patch_nfsu2_resolution(void)
{
    InitOnceExecuteOnce(&nx_nfsu2_res_once, nx_patch_nfsu2_resolution_once, NULL, NULL);
}

static BOOL nx_nfsu2_frontend(void)
{
    volatile DWORD *status = (volatile DWORD *)0x008654A4;

    if (!nx_is_nfsu2()) return FALSE;
    if (!nx_game_ptr_ok((const void *)status, sizeof(*status), FALSE)) return FALSE;
    return *status == 3;
}

static DWORD nx_pov_from_left_stick(XINPUT_STATE *state)
{
    const SHORT threshold = 16000;
    WORD b = 0;

    if (state->Gamepad.sThumbLY > threshold) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (state->Gamepad.sThumbLY < -threshold) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (state->Gamepad.sThumbLX < -threshold) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (state->Gamepad.sThumbLX > threshold) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    return nx_pov(b);
}

static LONG nx_axis_signed(SHORT value, BOOL invert)
{
    const LONG deadzone = 4096;
    LONG v = value;

    if (invert) v = -v;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;

    if (v > -deadzone && v < deadzone) return 0;

    if (v > 0)
        v = (v - deadzone) * 32767 / (32767 - deadzone);
    else
        v = (v + deadzone) * 32768 / (32768 - deadzone);

    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return v;
}

static LONG nx_axis_to_dinput(struct nx_joystick *impl, UINT object, SHORT value, BOOL invert)
{
    struct object_properties *properties = impl->base.object_properties + object;
    LONG v = nx_axis_signed(value, invert);
    LONG min = properties->range_min;
    LONG max = properties->range_max;
    LONG center;

    /*
     * DirectInput applications are allowed to change DIPROP_RANGE. NFSU2 does
     * that. The first bridge always returned 0..65535, which becomes an
     * off-centre/extreme value when the game requests a different range.
     */
    if (min == DIPROPRANGE_NOMIN) min = 0;
    if (max == DIPROPRANGE_NOMAX) max = 65535;
    center = min + (max - min) / 2;

    if (!v) return center;
    if (v > 0) return center + MulDiv(v, max - center, 32767);
    return center + MulDiv(v, center - min, 32768);
}

static DWORD nx_pov(WORD buttons)
{
    BOOL up = !!(buttons & XINPUT_GAMEPAD_DPAD_UP);
    BOOL down = !!(buttons & XINPUT_GAMEPAD_DPAD_DOWN);
    BOOL left = !!(buttons & XINPUT_GAMEPAD_DPAD_LEFT);
    BOOL right = !!(buttons & XINPUT_GAMEPAD_DPAD_RIGHT);

    if (up && right) return 4500;
    if (right && down) return 13500;
    if (down && left) return 22500;
    if (left && up) return 31500;
    if (up) return 0;
    if (right) return 9000;
    if (down) return 18000;
    if (left) return 27000;
    return 0xffffffff;
}

static BOOL try_enum_object(struct dinput_device *impl, const DIPROPHEADER *filter, DWORD flags,
                            enum_object_callback callback, UINT index,
                            DIDEVICEOBJECTINSTANCEW *instance, void *data)
{
    if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE(instance->dwType))) return DIENUM_CONTINUE;

    switch (filter->dwHow)
    {
    case DIPH_DEVICE:
        return callback(impl, index, NULL, instance, data);
    case DIPH_BYOFFSET:
        if (filter->dwObj != instance->dwOfs) return DIENUM_CONTINUE;
        return callback(impl, index, NULL, instance, data);
    case DIPH_BYID:
        if ((filter->dwObj & 0x00ffffff) != (instance->dwType & 0x00ffffff)) return DIENUM_CONTINUE;
        return callback(impl, index, NULL, instance, data);
    }

    return DIENUM_CONTINUE;
}

static HRESULT nx_joystick_enum_objects(IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                        DWORD flags, enum_object_callback callback, void *context)
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W(iface);
    DIDEVICEOBJECTINSTANCEW instances[] =
    {
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_XAxis, DIJOFS_X,
         DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), DIDOI_ASPECTPOSITION, L"X Axis"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_YAxis, DIJOFS_Y,
         DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), DIDOI_ASPECTPOSITION, L"Y Axis"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_RxAxis, DIJOFS_RX,
         DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), DIDOI_ASPECTPOSITION, L"Right X Axis"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_RyAxis, DIJOFS_RY,
         DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), DIDOI_ASPECTPOSITION, L"Right Y Axis"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_POV, DIJOFS_POV(0),
         DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0, L"D-pad"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(0),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0), 0, L"A"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(1),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(1), 0, L"B"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(2),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(2), 0, L"X"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(3),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(3), 0, L"Y"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(4),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(4), 0, L"Left Shoulder"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(5),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(5), 0, L"Right Shoulder"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(6),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(6), 0, L"Back"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(7),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(7), 0, L"Start"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(8),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(8), 0, L"Left Stick"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(9),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(9), 0, L"Right Stick"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(10),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(10), 0, L"Left Trigger"},
        {sizeof(DIDEVICEOBJECTINSTANCEW), GUID_Button, DIJOFS_BUTTON(11),
         DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(11), 0, L"Right Trigger"},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(instances); ++i)
        if (try_enum_object(&impl->base, filter, flags, callback, i, &instances[i], context) != DIENUM_CONTINUE)
            return DIENUM_STOP;

    return DIENUM_CONTINUE;
}

static void nx_update_long(struct nx_joystick *impl, DWORD offset, UINT object, LONG value)
{
    LONG *dst = (LONG *)(impl->base.device_state + offset);

    if (*dst == value) return;
    *dst = value;
    queue_event(&impl->base.IDirectInputDevice8W_iface, object, value, GetCurrentTime(),
                impl->base.dinput->evsequence++);
}

static void nx_update_button(struct nx_joystick *impl, DWORD offset, UINT object, BOOL pressed)
{
    BYTE value = pressed ? 0x80 : 0;
    BYTE *dst = impl->base.device_state + offset;

    if (*dst == value) return;
    *dst = value;
    queue_event(&impl->base.IDirectInputDevice8W_iface, object, value, GetCurrentTime(),
                impl->base.dinput->evsequence++);
}

static HRESULT nx_joystick_poll(IDirectInputDevice8W *iface)
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W(iface);
    XINPUT_STATE state;
    WORD b;

    if (!nx_get_xinput_state(&state)) return DIERR_INPUTLOST;
    b = state.Gamepad.wButtons;

    EnterCriticalSection(&impl->base.crit);

    DWORD pov = nx_pov(b);

    nx_nfsu2_mark_controller();

    /*
     * Vanilla NFSU2 uses the POV hat for front-end navigation even though the
     * left stick X axis is used for analog steering. On a modern PC gamepad the
     * expected behaviour is that the left stick also navigates menus, so only
     * while the game is in its front-end state we mirror the left stick into
     * the POV hat. The real D-pad always wins when it is held.
     */
    if (nx_nfsu2_frontend() && pov == 0xffffffff)
        pov = nx_pov_from_left_stick(&state);

    nx_update_long(impl, DIJOFS_X, 0, nx_axis_to_dinput(impl, 0, state.Gamepad.sThumbLX, FALSE));
    nx_update_long(impl, DIJOFS_Y, 1, nx_axis_to_dinput(impl, 1, state.Gamepad.sThumbLY, TRUE));
    nx_update_long(impl, DIJOFS_RX, 2, nx_axis_to_dinput(impl, 2, state.Gamepad.sThumbRX, FALSE));
    nx_update_long(impl, DIJOFS_RY, 3, nx_axis_to_dinput(impl, 3, state.Gamepad.sThumbRY, TRUE));
    nx_update_long(impl, DIJOFS_POV(0), 4, pov);

    nx_update_button(impl, DIJOFS_BUTTON(0), 5, b & XINPUT_GAMEPAD_A);
    nx_update_button(impl, DIJOFS_BUTTON(1), 6, b & XINPUT_GAMEPAD_B);
    nx_update_button(impl, DIJOFS_BUTTON(2), 7, b & XINPUT_GAMEPAD_X);
    nx_update_button(impl, DIJOFS_BUTTON(3), 8, b & XINPUT_GAMEPAD_Y);
    nx_update_button(impl, DIJOFS_BUTTON(4), 9, b & XINPUT_GAMEPAD_LEFT_SHOULDER);
    nx_update_button(impl, DIJOFS_BUTTON(5), 10, b & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    nx_update_button(impl, DIJOFS_BUTTON(6), 11, b & XINPUT_GAMEPAD_BACK);
    nx_update_button(impl, DIJOFS_BUTTON(7), 12, b & XINPUT_GAMEPAD_START);
    nx_update_button(impl, DIJOFS_BUTTON(8), 13, b & XINPUT_GAMEPAD_LEFT_THUMB);
    nx_update_button(impl, DIJOFS_BUTTON(9), 14, b & XINPUT_GAMEPAD_RIGHT_THUMB);
    nx_update_button(impl, DIJOFS_BUTTON(10), 15, state.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    nx_update_button(impl, DIJOFS_BUTTON(11), 16, state.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD);

    if (impl->base.hEvent) SetEvent(impl->base.hEvent);
    LeaveCriticalSection(&impl->base.crit);
    return DI_OK;
}

static HRESULT nx_joystick_acquire(IDirectInputDevice8W *iface)
{
    XINPUT_STATE state;
    return nx_get_xinput_state(&state) ? DI_OK : DIERR_INPUTLOST;
}

static HRESULT nx_joystick_unacquire(IDirectInputDevice8W *iface)
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W(iface);

    memset(impl->base.device_state, 0, sizeof(impl->base.device_state));
    *(LONG *)(impl->base.device_state + DIJOFS_X) = nx_axis_to_dinput(impl, 0, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_Y) = nx_axis_to_dinput(impl, 1, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_RX) = nx_axis_to_dinput(impl, 2, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_RY) = nx_axis_to_dinput(impl, 3, 0, FALSE);
    *(DWORD *)(impl->base.device_state + DIJOFS_POV(0)) = 0xffffffff;
    return DI_OK;
}

static const struct dinput_device_vtbl nx_joystick_vtbl =
{
    NULL,
    nx_joystick_poll,
    NULL,
    nx_joystick_acquire,
    nx_joystick_unacquire,
    nx_joystick_enum_objects,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

HRESULT nx_joystick_enum_device(DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version)
{
    XINPUT_STATE state;
    DWORD size;

    if (flags & DIEDFL_FORCEFEEDBACK) return DIERR_NOTFOUND;
    if (!nx_get_xinput_state(&state)) return DIERR_DEVICENOTREG;

    size = instance->dwSize;
    memset(instance, 0, size);
    instance->dwSize = size;
    instance->guidInstance = nx_joystick_guid;
    instance->guidProduct = nx_joystick_guid;
    instance->guidFFDriver = GUID_NULL;
    if (version >= 0x0800)
        instance->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8) | DIDEVTYPE_HID;
    else
        instance->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_TRADITIONAL << 8) | DIDEVTYPE_HID;
    instance->wUsagePage = 0x01;
    instance->wUsage = 0x04; /* HID generic joystick */
    lstrcpynW(instance->tszInstanceName, L"Autorun Controller", MAX_PATH);
    lstrcpynW(instance->tszProductName, L"Autorun Xbox 360 Controller (DirectInput)", MAX_PATH);
    return DI_OK;
}

HRESULT nx_joystick_create_device(struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out)
{
    struct nx_joystick *impl;
    XINPUT_STATE state;
    HRESULT hr;
    unsigned int i;

    *out = NULL;
    if (!IsEqualGUID(guid, &nx_joystick_guid) && !IsEqualGUID(guid, &GUID_Joystick))
        return DIERR_DEVICENOTREG;
    if (!nx_get_xinput_state(&state)) return DIERR_DEVICENOTREG;
    nx_claim_controller();
    nx_nfsu2_mark_controller();
    nx_patch_nfsu2_resolution();

    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    dinput_device_init(&impl->base, &nx_joystick_vtbl, &nx_joystick_guid, dinput);
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": nx_joystick.base.crit");
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;

    nx_joystick_enum_device(0, 0, &impl->base.instance, dinput->dwVersion);
    impl->base.caps.dwDevType = impl->base.instance.dwDevType;
    impl->base.caps.dwFirmwareRevision = 100;
    impl->base.caps.dwHardwareRevision = 100;

    if (FAILED(hr = dinput_device_init_device_format(&impl->base.IDirectInputDevice8W_iface))) goto failed;

    for (i = 0; i < 4 && i < impl->base.device_format.dwNumObjs; ++i)
    {
        impl->base.object_properties[i].range_min = 0;
        impl->base.object_properties[i].range_max = 65535;
        impl->base.object_properties[i].granularity = 1;
    }

    *(LONG *)(impl->base.device_state + DIJOFS_X) = nx_axis_to_dinput(impl, 0, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_Y) = nx_axis_to_dinput(impl, 1, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_RX) = nx_axis_to_dinput(impl, 2, 0, FALSE);
    *(LONG *)(impl->base.device_state + DIJOFS_RY) = nx_axis_to_dinput(impl, 3, 0, FALSE);
    *(DWORD *)(impl->base.device_state + DIJOFS_POV(0)) = 0xffffffff;

    *out = &impl->base.IDirectInputDevice8W_iface;
    TRACE("created Autorun synthetic DirectInput controller\n");
    return DI_OK;

failed:
    IDirectInputDevice_Release(&impl->base.IDirectInputDevice8W_iface);
    return hr;
}
