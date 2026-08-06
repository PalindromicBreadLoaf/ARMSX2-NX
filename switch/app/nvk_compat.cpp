// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// newlib shims required by NXVK.

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <cstddef>
#include <cstring>
#include <regex.h>
#include <signal.h>
#include <sys/stat.h>
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

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset)
{
	(void)how;
	(void)set;
	if (oldset)
		std::memset(oldset, 0, sizeof(*oldset));
	return 0;
}

// Mesa disables its disk cache when these unavailable POSIX calls fail.
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result)
{
	(void)uid;
	(void)pwd;
	(void)buf;
	(void)buflen;
	if (result)
		*result = nullptr;
	return ENOENT;
}

int dirfd(DIR* dirp)
{
	(void)dirp;
	errno = ENOTSUP;
	return -1;
}

int fstatat(int fd, const char* path, struct stat* buf, int flag)
{
	(void)fd;
	(void)path;
	(void)buf;
	(void)flag;
	errno = ENOSYS;
	return -1;
}

int flock(int fd, int operation)
{
	(void)fd;
	(void)operation;
	errno = ENOSYS;
	return -1;
}

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
