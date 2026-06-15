// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Pcsx2Types.h"
#include "common/Console.h"
#include "common/HostSys.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"

#include <functional>
#include <string>

#include "common/Horizon/Horizon.h"

// Returns 0 on failure
// A lot of these are functionally stubs
u64 GetPhysicalMemory()
{
	u64 size = 0;
	svcGetInfo(&size, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	return size;
}

u64 GetAvailablePhysicalMemory()
{
	u64 total = 0;
	u64 used = 0;
	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	return (total > used) ? (total - used) : 0;
}

u64 GetTickFrequency()
{
	return armGetSystemTickFreq();
}

u64 GetCPUTicks()
{
	return armGetSystemTick();
}

std::string GetOSVersionString()
{
	return "Horizon";
}

bool Common::InhibitScreensaver(bool inhibit)
{
	return false;
}

bool Common::PlaySoundAsync(const char* path)
{
	return false;
}

void Common::SetMousePosition(int x, int y)
{
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
	return false;
}

void Common::DetachMousePositionCb()
{
}

void Threading::Sleep(int ms)
{
	svcSleepThread(static_cast<s64>(ms) * 1000000LL);
}

void Threading::SleepUntil(u64 ticks)
{
	// ticks are a part of GetCPUTicks()
	const s64 diff = static_cast<s64>(ticks - GetCPUTicks());
	if (diff <= 0)
		return;

	svcSleepThread(static_cast<s64>(armTicksToNs(static_cast<u64>(diff))));
}
