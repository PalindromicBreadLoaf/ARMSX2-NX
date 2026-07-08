// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// newlib shims required by NXVK.

#include <sys/types.h>
#include <cstddef>
#include <regex.h>
#include <unistd.h>

#include <switch.h>

extern "C" {

// CSRNG-backed getrandom for Rust.
ssize_t getrandom(void* buf, size_t buflen, unsigned int flags)
{
	(void)flags;
	randomGet(buf, buflen);
	return static_cast<ssize_t>(buflen);
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

// Minimal sysconf for NVK memory/page queries.
long sysconf(int name)
{
	switch (name)
	{
		case _SC_PAGESIZE: return 4096;
		case _SC_PHYS_PAGES: return (3ll * 1024 * 1024 * 1024) / 4096;
		case _SC_NPROCESSORS_CONF:
		case _SC_NPROCESSORS_ONLN: return 4;
		default: return -1;
	}
}

int regcomp(regex_t* preg, const char* regex, int cflags)
{
	(void)regex;
	(void)cflags;
	if (preg)
		preg->re_nsub = 0;
	return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
	(void)preg;
	(void)string;
	(void)nmatch;
	(void)pmatch;
	(void)eflags;
	return REG_NOMATCH;
}

void regfree(regex_t* preg) { (void)preg; }

} // extern "C"
