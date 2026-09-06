// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Horizon crash diagnostics

namespace HorizonException
{
	void Initialize(const char* report_dir);

	void Shutdown();
} // namespace HorizonException
