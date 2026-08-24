// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

/* Simple DirectMedia Layer
   Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>
   This software is provided 'as-is', without any express or implied warranty. */

#include "SDL_internal.h"

#ifdef SDL_FILESYSTEM_SWITCH

#include <limits.h>
#include <unistd.h>

char* SDL_SYS_GetBasePath(void)
{
	return SDL_strdup("romfs:/");
}

char* SDL_SYS_GetPrefPath(const char* org, const char* app)
{
	char path[PATH_MAX];
	char* result;
	if (!getcwd(path, sizeof(path)))
		return NULL;
	if (SDL_asprintf(&result, "%s/", path) < 0)
		return NULL;
	return result;
}

char* SDL_SYS_GetUserFolder(SDL_Folder folder)
{
	SDL_Unsupported();
	return NULL;
}

#endif
