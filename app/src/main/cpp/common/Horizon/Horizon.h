// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright (c): PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Rename u128/s128 types in libNX to prevent compiler conflicts

#define u128 nx_u128
#define s128 nx_s128
#include <switch.h>
#undef u128
#undef s128
