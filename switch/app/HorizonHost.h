// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Helpers shared between the Switch run loop and the host
namespace HorizonHost
{
	// Record the calling thread as the emulator CPU/main thread.
	void SetCPUThread();

	// True when called on the thread previously passed to SetCPUThread().
	bool IsCPUThread();

	// Set by Host::RequestExitApplication / Host::RequestExitBigPicture and the quit combo
	void RequestExit();
	bool IsExitRequested();
} // namespace HorizonHost
