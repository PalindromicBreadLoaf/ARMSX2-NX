// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef __SWITCH__

#include "common/Pcsx2Types.h"

// Statistical sampling profiler for Switch.
namespace HorizonProfiler
{
	static constexpr float DEFAULT_CAPTURE_SECONDS = 30.0f;

	bool IsRunning();

	void Start(float seconds = DEFAULT_CAPTURE_SECONDS);

	void RequestStop();

	void Shutdown();

	void PublishTextureCacheCounts(u32 sources, u32 render_targets, u32 depth_targets, u32 source_pages);
} // namespace HorizonProfiler

#endif
