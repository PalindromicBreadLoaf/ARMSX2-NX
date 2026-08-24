// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

/* Simple DirectMedia Layer
   Copyright (C) 1997-2015 Sam Lantinga <slouken@libsdl.org>
   This software is provided 'as-is', without any express or implied warranty. */

#include "SDL_internal.h"

#if !defined(SDL_POWER_DISABLED) && defined(SDL_POWER_SWITCH)

#include <switch.h>

bool SDL_GetPowerInfo_SWITCH(SDL_PowerState* state, int* seconds, int* percent)
{
	PsmChargerType charger;
	u32 charge;
	if (R_FAILED(psmGetChargerType(&charger)))
		return false;
	psmGetBatteryChargePercentage(&charge);
	*percent = (int)charge;
	*seconds = (int)charge * 216;
	*state = charger == PsmChargerType_Unconnected ? SDL_POWERSTATE_ON_BATTERY :
		charger == PsmChargerType_EnoughPower ? SDL_POWERSTATE_CHARGED : SDL_POWERSTATE_CHARGING;
	return true;
}

#endif
