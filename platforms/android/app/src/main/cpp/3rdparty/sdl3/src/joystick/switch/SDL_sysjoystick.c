// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_internal.h"

#if SDL_JOYSTICK_SWITCH

#include "../SDL_sysjoystick.h"
#include <switch.h>

#define SWITCH_JOYSTICK_COUNT 8
#define SWITCH_BUTTON_COUNT 16

typedef struct SWITCHJoystickState
{
	PadState pad;
	HidAnalogStickState sticks[2];
	HidVibrationDeviceHandle vibration;
	HidVibrationValue vibration_value;
} SWITCHJoystickState;

static SWITCHJoystickState state[SWITCH_JOYSTICK_COUNT];

static const HidNpadButton button_map[SWITCH_BUTTON_COUNT] = {
	HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
	HidNpadButton_StickL, HidNpadButton_StickR, HidNpadButton_L, HidNpadButton_R,
	HidNpadButton_ZL, HidNpadButton_ZR, HidNpadButton_Plus, HidNpadButton_Minus,
	HidNpadButton_Left, HidNpadButton_Up, HidNpadButton_Right, HidNpadButton_Down,
};

static SDL_JoystickID SWITCH_JoystickGetDeviceInstanceID(int device_index)
{
	return (SDL_JoystickID)(device_index + 1);
}

static bool SWITCH_JoystickInit(void)
{
	padConfigureInput(SWITCH_JOYSTICK_COUNT, HidNpadStyleSet_NpadStandard);
	hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
	for (int i = 0; i < SWITCH_JOYSTICK_COUNT; ++i)
	{
		if (i == 0)
			padInitializeDefault(&state[i].pad);
		else
			padInitialize(&state[i].pad, HidNpadIdType_No1 + i);
		padUpdate(&state[i].pad);
		hidInitializeVibrationDevices(&state[i].vibration, 1, HidNpadIdType_No1 + i,
			HidNpadStyleTag_NpadJoyDual);
		SDL_PrivateJoystickAdded(SWITCH_JoystickGetDeviceInstanceID(i));
	}
	return true;
}

static int SWITCH_JoystickGetCount(void)
{
	return SWITCH_JOYSTICK_COUNT;
}

static void SWITCH_JoystickDetect(void)
{
}

static bool SWITCH_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char* name)
{
	return false;
}

static const char* SWITCH_JoystickGetDeviceName(int device_index)
{
	return "Nintendo Switch Controller";
}

static const char* SWITCH_JoystickGetDevicePath(int device_index)
{
	return NULL;
}

static int SWITCH_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
	return -1;
}

static int SWITCH_JoystickGetDevicePlayerIndex(int device_index)
{
	return device_index;
}

static void SWITCH_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_GUID SWITCH_JoystickGetDeviceGUID(int device_index)
{
	return SDL_CreateJoystickGUIDForName(SWITCH_JoystickGetDeviceName(device_index));
}

static bool SWITCH_JoystickOpen(SDL_Joystick* joystick, int device_index)
{
	joystick->nbuttons = SWITCH_BUTTON_COUNT;
	joystick->naxes = 4;
	joystick->nhats = 0;
	joystick->instance_id = SWITCH_JoystickGetDeviceInstanceID(device_index);
	SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
	return true;
}

static bool SWITCH_JoystickRumble(SDL_Joystick* joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
	const int index = joystick->instance_id - 1;
	if (index < 0 || index >= SWITCH_JOYSTICK_COUNT)
		return SDL_SetError("Invalid Switch controller");

	state[index].vibration_value.amp_low = low_frequency_rumble ? 1.0f : 0.0f;
	state[index].vibration_value.amp_high = high_frequency_rumble ? 1.0f : 0.0f;
	state[index].vibration_value.freq_low = low_frequency_rumble ? (float)low_frequency_rumble / 204.0f : 160.0f;
	state[index].vibration_value.freq_high = high_frequency_rumble ? (float)high_frequency_rumble / 204.0f : 320.0f;
	hidSendVibrationValues(&state[index].vibration, &state[index].vibration_value, 1);
	return true;
}

static bool SWITCH_JoystickRumbleTriggers(SDL_Joystick* joystick, Uint16 left, Uint16 right)
{
	return SDL_Unsupported();
}

static bool SWITCH_JoystickSetLED(SDL_Joystick* joystick, Uint8 red, Uint8 green, Uint8 blue)
{
	return SDL_Unsupported();
}

static bool SWITCH_JoystickSendEffect(SDL_Joystick* joystick, const void* data, int size)
{
	return SDL_Unsupported();
}

static bool SWITCH_JoystickSetSensorsEnabled(SDL_Joystick* joystick, bool enabled)
{
	return SDL_Unsupported();
}

static void SWITCH_JoystickUpdate(SDL_Joystick* joystick)
{
	const int index = joystick->instance_id - 1;
	if (index < 0 || index >= SWITCH_JOYSTICK_COUNT || SDL_TextInputActive(SDL_GetKeyboardFocus()))
		return;

	PadState* pad = &state[index].pad;
	padUpdate(pad);
	if (!padIsConnected(pad))
		return;

	const Uint64 timestamp = SDL_GetTicksNS();
	for (int stick = 0; stick < 2; ++stick)
	{
		const HidAnalogStickState current = padGetStickPos(pad, stick);
		if (current.x != state[index].sticks[stick].x)
		{
			SDL_SendJoystickAxis(timestamp, joystick, stick * 2, (Sint16)current.x);
			state[index].sticks[stick].x = current.x;
		}
		if (current.y != state[index].sticks[stick].y)
		{
			SDL_SendJoystickAxis(timestamp, joystick, stick * 2 + 1, (Sint16)-current.y);
			state[index].sticks[stick].y = current.y;
		}
	}

	const u64 changed = pad->buttons_old ^ pad->buttons_cur;
	for (int button = 0; button < SWITCH_BUTTON_COUNT; ++button)
	{
		if (changed & button_map[button])
			SDL_SendJoystickButton(timestamp, joystick, button, (pad->buttons_cur & button_map[button]) != 0);
	}
}

static void SWITCH_JoystickClose(SDL_Joystick* joystick)
{
}

static void SWITCH_JoystickQuit(void)
{
	for (int i = 0; i < SWITCH_JOYSTICK_COUNT; ++i)
		SDL_PrivateJoystickRemoved(SWITCH_JoystickGetDeviceInstanceID(i));
}

static bool SWITCH_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping* out)
{
	*out = (SDL_GamepadMapping){
		.a = {.kind = EMappingKind_Button, .target = 1}, .b = {.kind = EMappingKind_Button, .target = 0},
		.x = {.kind = EMappingKind_Button, .target = 3}, .y = {.kind = EMappingKind_Button, .target = 2},
		.back = {.kind = EMappingKind_Button, .target = 11}, .start = {.kind = EMappingKind_Button, .target = 10},
		.leftstick = {.kind = EMappingKind_Button, .target = 4}, .rightstick = {.kind = EMappingKind_Button, .target = 5},
		.leftshoulder = {.kind = EMappingKind_Button, .target = 6}, .rightshoulder = {.kind = EMappingKind_Button, .target = 7},
		.dpup = {.kind = EMappingKind_Button, .target = 13}, .dpdown = {.kind = EMappingKind_Button, .target = 15},
		.dpleft = {.kind = EMappingKind_Button, .target = 12}, .dpright = {.kind = EMappingKind_Button, .target = 14},
		.leftx = {.kind = EMappingKind_Axis, .target = 0}, .lefty = {.kind = EMappingKind_Axis, .target = 1},
		.rightx = {.kind = EMappingKind_Axis, .target = 2}, .righty = {.kind = EMappingKind_Axis, .target = 3},
		.lefttrigger = {.kind = EMappingKind_Button, .target = 8}, .righttrigger = {.kind = EMappingKind_Button, .target = 9},
	};
	return true;
}

SDL_JoystickDriver SDL_SWITCH_JoystickDriver = {
	.Init = SWITCH_JoystickInit, .GetCount = SWITCH_JoystickGetCount, .Detect = SWITCH_JoystickDetect,
	.IsDevicePresent = SWITCH_JoystickIsDevicePresent, .GetDeviceName = SWITCH_JoystickGetDeviceName,
	.GetDevicePath = SWITCH_JoystickGetDevicePath, .GetDeviceSteamVirtualGamepadSlot = SWITCH_JoystickGetDeviceSteamVirtualGamepadSlot,
	.GetDevicePlayerIndex = SWITCH_JoystickGetDevicePlayerIndex, .SetDevicePlayerIndex = SWITCH_JoystickSetDevicePlayerIndex,
	.GetDeviceGUID = SWITCH_JoystickGetDeviceGUID, .GetDeviceInstanceID = SWITCH_JoystickGetDeviceInstanceID,
	.Open = SWITCH_JoystickOpen, .Rumble = SWITCH_JoystickRumble, .RumbleTriggers = SWITCH_JoystickRumbleTriggers,
	.SetLED = SWITCH_JoystickSetLED, .SendEffect = SWITCH_JoystickSendEffect, .SetSensorsEnabled = SWITCH_JoystickSetSensorsEnabled,
	.Update = SWITCH_JoystickUpdate, .Close = SWITCH_JoystickClose, .Quit = SWITCH_JoystickQuit,
	.GetGamepadMapping = SWITCH_JoystickGetGamepadMapping,
};

#endif
