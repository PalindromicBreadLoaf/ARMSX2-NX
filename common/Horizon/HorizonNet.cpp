// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include <cerrno>
#include <ifaddrs.h>

extern "C" int getifaddrs(struct ifaddrs** ifap)
{
	if (ifap)
		*ifap = nullptr;
	errno = ENOSYS;
	return -1;
}

extern "C" void freeifaddrs(struct ifaddrs* ifa)
{
	(void)ifa;
}
