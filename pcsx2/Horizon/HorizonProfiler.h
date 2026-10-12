// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef __SWITCH__

#include "common/Pcsx2Types.h"

#include <atomic>

// Statistical sampling profiler for Switch.
namespace HorizonProfiler
{
	static constexpr float DEFAULT_CAPTURE_SECONDS = 30.0f;

	bool IsRunning();

	void Start(float seconds = DEFAULT_CAPTURE_SECONDS);

	void RequestStop();

	void Shutdown();

	void PublishTextureCacheCounts(u32 sources, u32 render_targets, u32 depth_targets, u32 source_pages);

	inline std::atomic<u64> s_gs_draws{0};

	inline void CountGSDraw()
	{
		s_gs_draws.store(s_gs_draws.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}
} // namespace HorizonProfiler

#endif
