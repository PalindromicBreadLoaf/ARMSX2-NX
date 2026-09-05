// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace HorizonHost
{
	void SetCPUThread();
	bool IsCPUThread();

	void RequestExit();
	bool IsExitRequested();

	void RequestVMShutdown(bool save_resume_state);
	bool TakeResumeSaveRequest();
} // namespace HorizonHost
