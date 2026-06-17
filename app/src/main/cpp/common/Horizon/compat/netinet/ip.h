// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// ip.h shim for Horizon

#pragma once

#include <netinet/in.h>
#include <cstdint>

struct ip
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	std::uint8_t ip_v : 4;  // version
	std::uint8_t ip_hl : 4; // header length
#else
	std::uint8_t ip_hl : 4; // header length
	std::uint8_t ip_v : 4;  // version
#endif
	std::uint8_t ip_tos;   // type of service
	std::uint16_t ip_len;  // total length
	std::uint16_t ip_id;   // identification
	std::uint16_t ip_off;  // fragment offset field
	std::uint8_t ip_ttl;   // time to live
	std::uint8_t ip_p;     // protocol
	std::uint16_t ip_sum;  // checksum
	struct in_addr ip_src; // source address
	struct in_addr ip_dst; // dest address
};
