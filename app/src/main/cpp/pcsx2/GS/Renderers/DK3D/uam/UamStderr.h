// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// Force-included into every uam translation unit so mesa's diagnostics land in a per-compile
// memstream instead of the emulog.

#pragma once

#include <stdio.h>

static inline FILE* UamGetSystemStderr(void)
{
	return stderr;
}

#ifdef stderr
#undef stderr
#endif

#ifdef __cplusplus
extern "C" {
#endif

FILE* UamGetStderr(void);

#ifdef __cplusplus
}
#endif

#define stderr UamGetStderr()
