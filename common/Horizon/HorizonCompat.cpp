// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/HTTPDownloader.h"

#include <cerrno>
#include <cstddef>
#include <malloc.h>
#include <memory>
#include <string>

extern "C" int posix_memalign(void** memptr, size_t alignment, size_t size)
{
	void* const p = memalign(alignment, size);
	if (!p)
		return ENOMEM;
	*memptr = p;
	return 0;
}

extern "C" int lockf(int fd, int cmd, off_t len)
{
	(void)fd;
	(void)cmd;
	(void)len;
	return 0;
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	(void)user_agent;
	return nullptr;
}
