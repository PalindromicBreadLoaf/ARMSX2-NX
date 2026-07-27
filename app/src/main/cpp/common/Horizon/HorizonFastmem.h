// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace HorizonFastmem
{
	bool IsSupported();

	bool IsSmcProtectionActive();

	const char* GetSupportReason();

	bool HasResumableFaultHandler();

	/// Allocates size bytes and moves them into AliasCodeData.
	u8* CreateSegment(size_t size);

	/// Undoes CreateSegment. Every alias of these pages must already be unmapped.
	void DestroySegment(u8* canonical);

	bool IsManagedFault(uptr addr);
} // namespace HorizonFastmem
