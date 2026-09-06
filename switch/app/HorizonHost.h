// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace HorizonHost
{
	void SetCPUThread();
	bool IsCPUThread();

	void RequestExit();
	bool IsExitRequested();

	void RequestVMShutdown(bool save_resume_state);
	bool TakeResumeSaveRequest();

	struct SoftwareKeyboardParameters
	{
		std::string guide_text;
		std::string initial_text;
		std::string ok_text;
		std::size_t max_length = 500;
		bool password = false;
	};

	std::optional<std::string> ShowSoftwareKeyboard(const SoftwareKeyboardParameters& params);
} // namespace HorizonHost
