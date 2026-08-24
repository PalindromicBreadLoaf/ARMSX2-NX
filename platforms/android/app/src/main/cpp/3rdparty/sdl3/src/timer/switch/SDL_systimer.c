// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

/* Simple DirectMedia Layer
   Copyright (C) 1997-2015 Sam Lantinga <slouken@libsdl.org>
   This software is provided 'as-is', without any express or implied warranty. */

#include "SDL_internal.h"

#ifdef SDL_TIMER_SWITCH

#include <switch.h>

Uint64 SDL_GetPerformanceCounter(void)
{
	return armGetSystemTick();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	return armGetSystemTickFreq();
}

void SDL_SYS_DelayNS(Uint64 ns)
{
	svcSleepThread(ns);
}

#endif
