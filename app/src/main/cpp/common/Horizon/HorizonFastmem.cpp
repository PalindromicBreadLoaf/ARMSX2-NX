// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/HorizonFastmem.h"

#include "common/Console.h"
#include "common/Horizon/Horizon.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <malloc.h>

namespace HorizonFastmem
{
namespace
{
	constexpr size_t PAGE_SIZE = 0x1000;

	struct Segment
	{
		void* backing;
		VirtmemReservation* reservation;
		size_t size;
	};

	struct Support
	{
		bool supported = false;
		std::array<char, 160> reason{};
	};

	std::mutex s_segment_mutex;
	std::map<u8*, Segment> s_segments;
	std::atomic<uptr> s_managed_begin{0};
	std::atomic<uptr> s_managed_end{0};

	void SetReason(Support& support, const char* stage, Result result)
	{
		std::snprintf(support.reason.data(), support.reason.size(), "%s failed (0x%08x)",
			stage, static_cast<unsigned>(result));
	}

	Support DetectSupport()
	{
		Support support;

		static constexpr struct
		{
			unsigned number;
			const char* name;
		} REQUIRED_SYSCALLS[] = {
			{0x02, "svcSetMemoryPermission"},
			{0x73, "svcSetProcessMemoryPermission"},
			{0x74, "svcMapProcessMemory"},
			{0x75, "svcUnmapProcessMemory"},
			{0x77, "svcMapProcessCodeMemory"},
			{0x78, "svcUnmapProcessCodeMemory"},
		};

		for (const auto& syscall : REQUIRED_SYSCALLS)
		{
			if (!envIsSyscallHinted(syscall.number))
			{
				std::snprintf(support.reason.data(), support.reason.size(),
					"%s is not available; use title override instead of applet mode", syscall.name);
				return support;
			}
		}

		const Handle self = envGetOwnProcessHandle();
		if (self == INVALID_HANDLE)
		{
			std::snprintf(support.reason.data(), support.reason.size(), "the process has no handle to itself");
			return support;
		}
		if (!HasResumableFaultHandler())
		{
			std::snprintf(support.reason.data(), support.reason.size(), "the resumable exception entry is not linked");
			return support;
		}

		void* const backing = memalign(PAGE_SIZE, PAGE_SIZE);
		if (!backing)
		{
			std::snprintf(support.reason.data(), support.reason.size(), "probe allocation failed");
			return support;
		}
		std::memset(backing, 0, PAGE_SIZE);

		void* canonical = nullptr;
		void* alias = nullptr;
		VirtmemReservation* canonical_reservation = nullptr;
		VirtmemReservation* alias_reservation = nullptr;
		bool canonical_mapped = false;
		bool alias_mapped = false;

		virtmemLock();
		canonical = virtmemFindCodeMemory(PAGE_SIZE, PAGE_SIZE);
		canonical_reservation = canonical ? virtmemAddReservation(canonical, PAGE_SIZE) : nullptr;
		alias = virtmemFindAslr(PAGE_SIZE, 0);
		alias_reservation = alias ? virtmemAddReservation(alias, PAGE_SIZE) : nullptr;
		virtmemUnlock();

		if (!canonical_reservation || !alias_reservation)
		{
			std::snprintf(support.reason.data(), support.reason.size(), "probe address reservation failed");
			goto cleanup;
		}

		{
			Result result = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
				reinterpret_cast<u64>(backing), PAGE_SIZE);
			if (R_FAILED(result))
			{
				SetReason(support, "svcMapProcessCodeMemory", result);
				goto cleanup;
			}
			canonical_mapped = true;

			result = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(canonical), PAGE_SIZE, Perm_Rw);
			if (R_FAILED(result))
			{
				SetReason(support, "svcSetProcessMemoryPermission(RW)", result);
				goto cleanup;
			}

			result = svcMapProcessMemory(alias, self, reinterpret_cast<u64>(canonical), PAGE_SIZE);
			if (R_FAILED(result))
			{
				SetReason(support, "svcMapProcessMemory", result);
				goto cleanup;
			}
			alias_mapped = true;

			result = svcUnmapProcessMemory(alias, self, reinterpret_cast<u64>(canonical), PAGE_SIZE);
			if (R_FAILED(result))
			{
				SetReason(support, "svcUnmapProcessMemory", result);
				goto cleanup;
			}
			alias_mapped = false;

			result = svcSetMemoryPermission(canonical, PAGE_SIZE, Perm_R);
			if (R_FAILED(result))
			{
				SetReason(support, "svcSetMemoryPermission(R)", result);
				goto cleanup;
			}

			result = svcSetMemoryPermission(canonical, PAGE_SIZE, Perm_Rw);
			if (R_FAILED(result))
			{
				SetReason(support, "svcSetMemoryPermission(RW)", result);
				goto cleanup;
			}
		}

		support.supported = true;
		std::snprintf(support.reason.data(), support.reason.size(), "AliasCodeData SMC probe passed");

	cleanup:
		if (alias_mapped)
		{
			const Result result =
				svcUnmapProcessMemory(alias, self, reinterpret_cast<u64>(canonical), PAGE_SIZE);
			if (R_FAILED(result))
			{
				SetReason(support, "probe alias cleanup", result);
				return support;
			}
		}
		if (canonical_mapped)
		{
			const Result result = svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
				reinterpret_cast<u64>(backing), PAGE_SIZE);
			if (R_FAILED(result))
			{
				SetReason(support, "probe code cleanup", result);
				return support;
			}
		}

		virtmemLock();
		if (alias_reservation)
			virtmemRemoveReservation(alias_reservation);
		if (canonical_reservation)
			virtmemRemoveReservation(canonical_reservation);
		virtmemUnlock();
		free(backing);
		return support;
	}

	const Support& GetSupport()
	{
		static const Support support = DetectSupport();
		return support;
	}
} // namespace

bool IsSupported()
{
	return GetSupport().supported;
}

bool IsSmcProtectionActive()
{
	return s_managed_begin.load(std::memory_order_acquire) != 0;
}

const char* GetSupportReason()
{
	return GetSupport().reason.data();
}

u8* CreateSegment(size_t size)
{
	if (!IsSupported() || size == 0 || (size & (PAGE_SIZE - 1)) != 0)
		return nullptr;

	void* const backing = memalign(PAGE_SIZE, size);
	if (!backing)
		return nullptr;
	std::memset(backing, 0, size);

	virtmemLock();
	void* const canonical = virtmemFindCodeMemory(size, PAGE_SIZE);
	VirtmemReservation* const reservation =
		canonical ? virtmemAddReservation(canonical, size) : nullptr;
	virtmemUnlock();
	if (!reservation)
	{
		free(backing);
		return nullptr;
	}

	const Handle self = envGetOwnProcessHandle();
	Result result = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
		reinterpret_cast<u64>(backing), size);
	if (R_FAILED(result))
	{
		virtmemLock();
		virtmemRemoveReservation(reservation);
		virtmemUnlock();
		free(backing);
		return nullptr;
	}

	result = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(canonical), size, Perm_Rw);
	if (R_FAILED(result))
	{
		const Result unmap_result = svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
			reinterpret_cast<u64>(backing), size);
		if (R_SUCCEEDED(unmap_result))
		{
			virtmemLock();
			virtmemRemoveReservation(reservation);
			virtmemUnlock();
			free(backing);
		}
		return nullptr;
	}

	{
		std::lock_guard lock(s_segment_mutex);
		s_segments.emplace(static_cast<u8*>(canonical), Segment{backing, reservation, size});
	}
	s_managed_end.store(reinterpret_cast<uptr>(canonical) + size, std::memory_order_release);
	s_managed_begin.store(reinterpret_cast<uptr>(canonical), std::memory_order_release);
	return static_cast<u8*>(canonical);
}

void DestroySegment(u8* canonical)
{
	std::unique_lock lock(s_segment_mutex);
	const auto it = s_segments.find(canonical);
	if (it == s_segments.end())
		return;

	const Segment segment = it->second;
	s_managed_begin.store(0, std::memory_order_release);
	s_managed_end.store(0, std::memory_order_release);

	const Result result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
		reinterpret_cast<u64>(canonical), reinterpret_cast<u64>(segment.backing), segment.size);
	if (R_FAILED(result))
	{
		Console.Error("svcUnmapProcessCodeMemory(%p, 0x%zx) failed: 0x%08x",
			canonical, segment.size, static_cast<unsigned>(result));
		s_segments.erase(it);
		return;
	}

	s_segments.erase(it);
	lock.unlock();
	virtmemLock();
	virtmemRemoveReservation(segment.reservation);
	virtmemUnlock();
	free(segment.backing);
}

bool IsManagedFault(uptr addr)
{
	const uptr begin = s_managed_begin.load(std::memory_order_acquire);
	const uptr end = s_managed_end.load(std::memory_order_acquire);
	return begin != 0 && addr >= begin && addr < end;
}
} // namespace HorizonFastmem
