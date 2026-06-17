// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// ifaddrs shim for Horizon
#pragma once

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs
{
	struct ifaddrs* ifa_next;
	char* ifa_name;
	unsigned int ifa_flags;
	struct sockaddr* ifa_addr;
	struct sockaddr* ifa_netmask;
	union
	{
		struct sockaddr* ifu_broadaddr;
		struct sockaddr* ifu_dstaddr;
	} ifa_ifu;
	void* ifa_data;
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr ifa_ifu.ifu_dstaddr

int getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif
