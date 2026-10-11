// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "Horizon.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mutex>
#include <unistd.h>

extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace Horizon
{
	namespace
	{
		bool s_network_available = false;
		int s_boot_fd = -1;
		char s_report_dir[768] = {};
	} // namespace

	bool EnsureNetworkInitialized()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			if (R_FAILED(socketInitializeDefault()))
				return;

			s_network_available = true;

			sslInitialize(4);
		});

		return s_network_available;
	}

	void BreadcrumbInit(const char* report_dir)
	{
		if (s_boot_fd >= 0 || !report_dir)
			return;

		std::snprintf(s_report_dir, sizeof(s_report_dir), "%s", report_dir);

		char path[768];
		std::snprintf(path, sizeof(path), "%s/boot.txt", report_dir);
		s_boot_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	}

	void Breadcrumb(const char* message)
	{
		if (s_boot_fd < 0 || !message)
			return;

		char line[512];
		const int len = std::snprintf(line, sizeof(line), "[%016lx] %s\n",
			svcGetSystemTick(), message);
		if (len > 0)
		{
			const size_t count = static_cast<size_t>(len < static_cast<int>(sizeof(line)) ? len : sizeof(line));
			write(s_boot_fd, line, count);
			fsync(s_boot_fd);
		}
	}

	void BreadcrumbShutdown()
	{
		if (s_boot_fd >= 0)
		{
			close(s_boot_fd);
			s_boot_fd = -1;
		}
	}

	void ExitTrace(const char* message)
	{
		if (!message || s_report_dir[0] == '\0')
			return;

		char path[768];
		std::snprintf(path, sizeof(path), "%s/exit.txt", s_report_dir);
		const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
		if (fd < 0)
			return;

		char line[512];
		const int len = std::snprintf(line, sizeof(line), "[%016lx] %s\n",
			svcGetSystemTick(), message);
		if (len > 0)
		{
			const size_t count = static_cast<size_t>(len < static_cast<int>(sizeof(line)) ? len : sizeof(line));
			write(fd, line, count);
			fsync(fd);
		}
		close(fd);
	}

	void AuditHeapToExitTrace(const char* when)
	{
		const u64 begin = reinterpret_cast<u64>(fake_heap_start);
		const u64 end = reinterpret_cast<u64>(fake_heap_end);
		char line[256];
		if (begin >= end)
		{
			std::snprintf(line, sizeof(line), "heap audit (%s): no heap bounds", when);
			ExitTrace(line);
			return;
		}

		constexpr u32 MAX_LISTED = 24;
		u32 listed = 0;
		u64 bad_bytes = 0;
		u64 addr = begin;
		while (addr < end)
		{
			MemoryInfo info{};
			u32 page_info;
			if (R_FAILED(svcQueryMemory(&info, &page_info, addr)) || info.size == 0)
				break;

			const u64 range_begin = std::max<u64>(info.addr, begin);
			const u64 range_end = std::min<u64>(info.addr + info.size, end);
			const bool plain = (info.type & MemState_Type) == MemType_Heap && info.perm == Perm_Rw && info.attr == 0;
			if (!plain && range_end > range_begin)
			{
				bad_bytes += range_end - range_begin;
				if (listed++ < MAX_LISTED)
				{
					std::snprintf(line, sizeof(line),
						"heap audit (%s): %#lx-%#lx (+%#lx into heap) type %#x perm %#x attr %#x", when,
						range_begin, range_end, range_begin - begin, info.type & MemState_Type, info.perm, info.attr);
					ExitTrace(line);
				}
			}
			addr = info.addr + info.size;
		}

		std::snprintf(line, sizeof(line), "heap audit (%s): heap %#lx-%#lx, %u odd ranges, %#lx bytes", when, begin, end,
			listed, bad_bytes);
		ExitTrace(line);
	}
} // namespace Horizon

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
