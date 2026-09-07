// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "Horizon.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mutex>
#include <unistd.h>

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
