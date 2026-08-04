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

	u8* CreateArena(size_t size);
	bool DestroyArena(u8* base, size_t size);

	u8* MapArena(void* handle, size_t file_offset, u8* map_base, size_t map_size, bool writable);
	bool UnmapArena(u8* map_base, size_t map_size);

	bool IsCanonicalAddress(uptr addr, size_t size);
	bool IsArenaAddress(uptr addr, size_t size);
	bool PrepareCanonicalProtection(void* addr, size_t size);
	void RestoreCanonicalProtection(void* addr, size_t size);
	bool ProtectArena(void* addr, size_t size, bool writable);

	bool ResolveFault(uptr addr);
	bool IsManagedFault(uptr addr);
} // namespace HorizonFastmem
