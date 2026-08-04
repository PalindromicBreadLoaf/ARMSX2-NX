// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace Horizon
{
	/// Raises the CPU clock to boost clock speeds whenever something is being loaded.
	class CpuBoostScope
	{
	public:
		CpuBoostScope(const CpuBoostScope&) = delete;
		CpuBoostScope& operator=(const CpuBoostScope&) = delete;

#ifdef __SWITCH__
		CpuBoostScope();
		~CpuBoostScope();
#else
		CpuBoostScope() = default;
		~CpuBoostScope() = default;
#endif
	};
} // namespace Horizon
