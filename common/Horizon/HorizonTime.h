// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <ctime>

// Convert UTC to time_t
namespace Horizon
{
	inline std::time_t timegm_utc(const struct tm& tm)
	{
		int y = tm.tm_year + 1900;
		const unsigned m = static_cast<unsigned>(tm.tm_mon + 1);
		const unsigned d = static_cast<unsigned>(tm.tm_mday);
		y -= (m <= 2);
		const int era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		const long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
		return static_cast<std::time_t>(days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
	}
} // namespace Horizon
