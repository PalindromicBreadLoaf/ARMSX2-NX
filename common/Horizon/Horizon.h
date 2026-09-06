// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#define u128 nx_u128
#define s128 nx_s128
#include <switch.h>
#undef u128
#undef s128

namespace Horizon
{
	// Lazily brings up the BSD socket
	bool EnsureNetworkInitialized();

	// Init logging
	void BreadcrumbInit(const char* report_dir);
	void Breadcrumb(const char* message);
	void BreadcrumbShutdown();
} // namespace Horizon
