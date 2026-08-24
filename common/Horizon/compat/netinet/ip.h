// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <netinet/in.h>

struct ip
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	std::uint8_t ip_v : 4;
	std::uint8_t ip_hl : 4;
#else
	std::uint8_t ip_hl : 4;
	std::uint8_t ip_v : 4;
#endif
	std::uint8_t ip_tos;
	std::uint16_t ip_len;
	std::uint16_t ip_id;
	std::uint16_t ip_off;
	std::uint8_t ip_ttl;
	std::uint8_t ip_p;
	std::uint16_t ip_sum;
	struct in_addr ip_src;
	struct in_addr ip_dst;
};
