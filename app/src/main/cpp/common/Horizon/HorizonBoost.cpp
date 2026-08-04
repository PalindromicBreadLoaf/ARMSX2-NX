// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/HorizonBoost.h"

#include "common/Horizon/Horizon.h"

#include <atomic>

namespace Horizon
{
	namespace
	{
		std::atomic<int> s_boost_depth{0};
	} // namespace

	CpuBoostScope::CpuBoostScope()
	{
		if (s_boost_depth.fetch_add(1, std::memory_order_acq_rel) == 0)
			appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
	}

	CpuBoostScope::~CpuBoostScope()
	{
		if (s_boost_depth.fetch_sub(1, std::memory_order_acq_rel) == 1)
			appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
	}
} // namespace Horizon
