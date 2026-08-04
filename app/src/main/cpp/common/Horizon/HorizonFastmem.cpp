// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/HorizonFastmem.h"

#include "common/Console.h"
#include "common/Horizon/Horizon.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>

#include <malloc.h>

namespace HorizonFastmem
{
namespace
{
	constexpr size_t PAGE_SIZE = 0x1000;
	constexpr size_t ARENA_SIZE = 0x100000000ULL;
	constexpr size_t ARENA_PAGE_COUNT = ARENA_SIZE / PAGE_SIZE;
	constexpr unsigned LIVE_PAGE_LIMIT = 16384;
	constexpr size_t FAULT_BATCH_PAGES = 16;

	constexpr u32 ENTRY_EMPTY = std::numeric_limits<u32>::max();
	constexpr u32 ENTRY_PROTECTED = 0x80000000u;
	constexpr u32 ENTRY_LAZY = 0x40000000u;
	constexpr u32 ENTRY_REMAP = 0x20000000u;
	constexpr u32 ENTRY_PAGE_MASK = 0x000fffffu;
	constexpr u32 REVERSE_END = std::numeric_limits<u32>::max();

	struct Segment
	{
		void* backing;
		VirtmemReservation* reservation;
		size_t size;
		u32* reverse_heads = nullptr;
	};

	struct Arena
	{
		u8* base = nullptr;
		VirtmemReservation* reservation = nullptr;
		u8* segment_base = nullptr;
		u32* pages = nullptr;
		u32* reverse_next = nullptr;
		size_t size = 0;
		unsigned live_pages = 0;
	};

	struct Support
	{
		bool supported = false;
		std::array<char, 160> reason{};
	};

	std::mutex s_segment_mutex;
	std::map<u8*, Segment> s_segments;
	Arena s_arena;
	std::atomic<uptr> s_managed_begin{0};
	std::atomic<uptr> s_managed_end{0};
	std::atomic<uptr> s_arena_base{0};
	std::atomic<u32*> s_arena_pages{nullptr};

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

	bool ContainsRange(uptr base, size_t total_size, uptr addr, size_t size)
	{
		return base != 0 && size <= total_size && addr >= base && addr - base <= total_size - size;
	}

	u32 LoadEntry(size_t page)
	{
		return __atomic_load_n(&s_arena.pages[page], __ATOMIC_ACQUIRE);
	}

	void StoreEntry(size_t page, u32 entry)
	{
		__atomic_store_n(&s_arena.pages[page], entry, __ATOMIC_RELEASE);
	}

	std::map<u8*, Segment>::iterator FindSegmentContainingLocked(uptr addr, size_t size)
	{
		auto it = s_segments.upper_bound(reinterpret_cast<u8*>(addr));
		if (it == s_segments.begin())
			return s_segments.end();

		--it;
		const uptr base = reinterpret_cast<uptr>(it->first);
		return ContainsRange(base, it->second.size, addr, size) ? it : s_segments.end();
	}

	Segment* GetArenaSegmentLocked()
	{
		const auto it = s_segments.find(s_arena.segment_base);
		return it != s_segments.end() ? &it->second : nullptr;
	}

	bool EntrySourcePageLocked(u32 entry, Segment** segment, size_t* source_page)
	{
		if (entry == ENTRY_EMPTY)
			return false;

		Segment* const current = GetArenaSegmentLocked();
		const size_t page = entry & ENTRY_PAGE_MASK;
		if (!current || page >= current->size / PAGE_SIZE)
			return false;

		*segment = current;
		*source_page = page;
		return true;
	}

	bool ReverseInsertLocked(size_t arena_page, u32 entry)
	{
		if (!s_arena.reverse_next || arena_page >= ARENA_PAGE_COUNT ||
			s_arena.reverse_next[arena_page] != REVERSE_END)
		{
			return false;
		}

		Segment* segment;
		size_t source_page;
		if (!EntrySourcePageLocked(entry, &segment, &source_page) || !segment->reverse_heads)
			return false;

		s_arena.reverse_next[arena_page] = segment->reverse_heads[source_page];
		segment->reverse_heads[source_page] = static_cast<u32>(arena_page);
		return true;
	}

	bool ReverseRemoveLocked(size_t arena_page, u32 entry)
	{
		if (!s_arena.reverse_next || arena_page >= ARENA_PAGE_COUNT)
			return false;

		Segment* segment;
		size_t source_page;
		if (!EntrySourcePageLocked(entry, &segment, &source_page) || !segment->reverse_heads)
			return false;

		u32* link = &segment->reverse_heads[source_page];
		size_t guard = 0;
		while (*link != REVERSE_END)
		{
			const u32 current = *link;
			if (current >= ARENA_PAGE_COUNT || guard++ >= ARENA_PAGE_COUNT)
				return false;
			if (current == arena_page)
			{
				*link = s_arena.reverse_next[current];
				s_arena.reverse_next[current] = REVERSE_END;
				return true;
			}
			link = &s_arena.reverse_next[current];
		}
		return false;
	}

	bool UnmapRangeLocked(size_t first, size_t count)
	{
		const Handle self = envGetOwnProcessHandle();
		for (size_t i = 0; i < count;)
		{
			const u32 entry = LoadEntry(first + i);
			if (entry == ENTRY_EMPTY)
			{
				i++;
				continue;
			}

			if (entry & ENTRY_LAZY)
			{
				ReverseRemoveLocked(first + i, entry);
				StoreEntry(first + i, ENTRY_EMPTY);
				i++;
				continue;
			}

			Segment* segment;
			size_t source_page;
			if (!EntrySourcePageLocked(entry, &segment, &source_page))
				return false;

			size_t run = 1;
			while (i + run < count)
			{
				const u32 next = LoadEntry(first + i + run);
				Segment* next_segment;
				size_t next_source_page;
				if (next == ENTRY_EMPTY || (next & ENTRY_LAZY) ||
					!EntrySourcePageLocked(next, &next_segment, &next_source_page) ||
					next_segment != segment || next_source_page != source_page + run)
				{
					break;
				}
				run++;
			}

			const uptr destination =
				reinterpret_cast<uptr>(s_arena.base) + (first + i) * PAGE_SIZE;
			const uptr source =
				reinterpret_cast<uptr>(s_arena.segment_base) + source_page * PAGE_SIZE;
			const Result result = svcUnmapProcessMemory(reinterpret_cast<void*>(destination),
				self, source, run * PAGE_SIZE);
			if (R_FAILED(result))
			{
				// Deliberately leave the shadow entries intact to avoid potential corruption.
				ERROR_LOG_THROTTLED("HorizonFastmem: svcUnmapProcessMemory({:#x}, {:#x}, {:#x}) "
									"failed: {:#010x}. Keeping shadow entry for retry",
					destination, source, run * PAGE_SIZE, static_cast<unsigned>(result));
				return false;
			}

			s_arena.live_pages =
				(s_arena.live_pages >= run) ? s_arena.live_pages - static_cast<unsigned>(run) : 0;
			for (size_t page = 0; page < run; page++)
			{
				const u32 old = LoadEntry(first + i + page);
				ReverseRemoveLocked(first + i + page, old);
				StoreEntry(first + i + page, ENTRY_EMPTY);
			}
			i += run;
		}
		return true;
	}

	bool MapRunLocked(size_t first, size_t count, size_t source_page)
	{
		if (count == 0 || count > LIVE_PAGE_LIMIT - s_arena.live_pages)
			return false;

		const uptr destination = reinterpret_cast<uptr>(s_arena.base) + first * PAGE_SIZE;
		const uptr source =
			reinterpret_cast<uptr>(s_arena.segment_base) + source_page * PAGE_SIZE;
		const Result result = svcMapProcessMemory(reinterpret_cast<void*>(destination),
			envGetOwnProcessHandle(), source, count * PAGE_SIZE);
		if (R_FAILED(result))
			return false;

		s_arena.live_pages += static_cast<unsigned>(count);
		for (size_t i = 0; i < count; i++)
		{
			StoreEntry(first + i,
				LoadEntry(first + i) & ~(ENTRY_LAZY | ENTRY_PROTECTED | ENTRY_REMAP));
		}
		return true;
	}

	bool SetAliasProtectionLocked(size_t arena_page, bool writable)
	{
		u32 entry = LoadEntry(arena_page);
		if (entry == ENTRY_EMPTY)
			return true;

		Segment* segment;
		size_t source_page;
		if (!EntrySourcePageLocked(entry, &segment, &source_page))
			return false;

		if (writable)
		{
			if (!(entry & ENTRY_PROTECTED))
				return true;

			const bool remap = (entry & ENTRY_REMAP) != 0;
			entry = (entry | ENTRY_LAZY) & ~(ENTRY_PROTECTED | ENTRY_REMAP);
			StoreEntry(arena_page, entry);
			if (remap && s_arena.live_pages < LIVE_PAGE_LIMIT)
				MapRunLocked(arena_page, 1, source_page);
			return true;
		}

		if (entry & ENTRY_PROTECTED)
			return true;
		if (entry & ENTRY_LAZY)
		{
			StoreEntry(arena_page, entry | ENTRY_PROTECTED);
			return true;
		}

		const uptr destination = reinterpret_cast<uptr>(s_arena.base) + arena_page * PAGE_SIZE;
		const uptr source =
			reinterpret_cast<uptr>(s_arena.segment_base) + source_page * PAGE_SIZE;
		const Result result = svcUnmapProcessMemory(reinterpret_cast<void*>(destination),
			envGetOwnProcessHandle(), source, PAGE_SIZE);
		if (R_FAILED(result))
		{
			// Same rule as UnmapRangeLocked: the shadow entry stays, so the alias is never
			// forgotten while the kernel still holds it.
			ERROR_LOG_THROTTLED("HorizonFastmem: write-protect unmap of arena page {:#x} "
								"failed: {:#010x}; keeping shadow entry for retry",
				destination, static_cast<unsigned>(result));
			return false;
		}

		if (s_arena.live_pages)
			s_arena.live_pages--;
		StoreEntry(arena_page, entry | ENTRY_LAZY | ENTRY_PROTECTED | ENTRY_REMAP);
		return true;
	}

	bool SetCanonicalAliasesLocked(uptr addr, size_t size, bool writable)
	{
		const auto segment_it = FindSegmentContainingLocked(addr, size);
		if (segment_it == s_segments.end() || !s_arena.pages || !segment_it->second.reverse_heads)
			return segment_it != s_segments.end();

		const size_t first_source_page =
			(addr - reinterpret_cast<uptr>(segment_it->first)) / PAGE_SIZE;
		const size_t source_page_count = size / PAGE_SIZE;
		for (size_t page = 0; page < source_page_count; page++)
		{
			u32 alias = segment_it->second.reverse_heads[first_source_page + page];
			size_t guard = 0;
			while (alias != REVERSE_END)
			{
				if (alias >= ARENA_PAGE_COUNT || guard++ >= ARENA_PAGE_COUNT)
					return false;

				const u32 next = s_arena.reverse_next[alias];
				Segment* mapped_segment;
				size_t mapped_source_page;
				const u32 entry = LoadEntry(alias);
				if (!EntrySourcePageLocked(entry, &mapped_segment, &mapped_source_page) ||
					mapped_segment != &segment_it->second ||
					mapped_source_page != first_source_page + page ||
					!SetAliasProtectionLocked(alias, writable))
				{
					return false;
				}
				alias = next;
			}
		}
		return true;
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
		s_segments.emplace(static_cast<u8*>(canonical), Segment{backing, reservation, size, nullptr});
	}
	s_managed_end.store(reinterpret_cast<uptr>(canonical) + size, std::memory_order_release);
	s_managed_begin.store(reinterpret_cast<uptr>(canonical), std::memory_order_release);
	return static_cast<u8*>(canonical);
}

u8* CreateArena(size_t size)
{
	if (!IsSupported() || size != ARENA_SIZE)
		return nullptr;

	std::lock_guard lock(s_segment_mutex);
	if (s_arena.base || s_segments.size() != 1)
		return nullptr;

	auto segment_it = s_segments.begin();
	Segment& segment = segment_it->second;
	const size_t segment_page_count = segment.size / PAGE_SIZE;
	if (segment_page_count == 0 || segment_page_count > ENTRY_PAGE_MASK + 1)
		return nullptr;

	u32* const pages = new (std::nothrow) u32[ARENA_PAGE_COUNT];
	u32* const reverse_next = new (std::nothrow) u32[ARENA_PAGE_COUNT];
	u32* const reverse_heads = new (std::nothrow) u32[segment_page_count];
	if (!pages || !reverse_next || !reverse_heads)
	{
		delete[] pages;
		delete[] reverse_next;
		delete[] reverse_heads;
		return nullptr;
	}
	std::fill_n(pages, ARENA_PAGE_COUNT, ENTRY_EMPTY);
	std::fill_n(reverse_next, ARENA_PAGE_COUNT, REVERSE_END);
	std::fill_n(reverse_heads, segment_page_count, REVERSE_END);

	virtmemLock();
	void* const base = virtmemFindAslr(size, 0);
	VirtmemReservation* const reservation = base ? virtmemAddReservation(base, size) : nullptr;
	virtmemUnlock();
	if (!reservation)
	{
		delete[] pages;
		delete[] reverse_next;
		delete[] reverse_heads;
		return nullptr;
	}

	segment.reverse_heads = reverse_heads;
	s_arena = Arena{
		static_cast<u8*>(base), reservation, segment_it->first, pages, reverse_next, size, 0};
	s_arena_pages.store(pages, std::memory_order_release);
	s_arena_base.store(reinterpret_cast<uptr>(base), std::memory_order_release);
	return static_cast<u8*>(base);
}

bool DestroyArena(u8* base, size_t size)
{
	std::unique_lock lock(s_segment_mutex);
	if (base != s_arena.base || size != s_arena.size)
		return false;
	if (!UnmapRangeLocked(0, s_arena.size / PAGE_SIZE))
		return false;

	Segment* const segment = GetArenaSegmentLocked();
	u32* const reverse_heads = segment ? segment->reverse_heads : nullptr;
	const size_t segment_page_count = segment ? segment->size / PAGE_SIZE : 0;
	if (segment)
		segment->reverse_heads = nullptr;

	VirtmemReservation* const reservation = s_arena.reservation;
	u32* const pages = s_arena.pages;
	u32* const reverse_next = s_arena.reverse_next;
	s_arena_pages.store(nullptr, std::memory_order_release);
	s_arena_base.store(0, std::memory_order_release);
	s_arena = {};
	lock.unlock();

	if (reverse_heads)
		std::fill_n(reverse_heads, segment_page_count, REVERSE_END);
	virtmemLock();
	virtmemRemoveReservation(reservation);
	virtmemUnlock();
	delete[] pages;
	delete[] reverse_next;
	delete[] reverse_heads;
	return true;
}

u8* MapArena(void* handle, size_t file_offset, u8* map_base, size_t map_size, bool writable)
{
	if (!map_base || map_size == 0 || (file_offset & (PAGE_SIZE - 1)) != 0 ||
		(reinterpret_cast<uptr>(map_base) & (PAGE_SIZE - 1)) != 0 ||
		(map_size & (PAGE_SIZE - 1)) != 0)
	{
		return nullptr;
	}

	std::lock_guard lock(s_segment_mutex);
	const auto segment_it = s_segments.find(static_cast<u8*>(handle));
	if (!s_arena.pages || segment_it == s_segments.end() ||
		segment_it->first != s_arena.segment_base ||
		file_offset > segment_it->second.size ||
		map_size > segment_it->second.size - file_offset ||
		!ContainsRange(reinterpret_cast<uptr>(s_arena.base), s_arena.size,
			reinterpret_cast<uptr>(map_base), map_size))
	{
		return nullptr;
	}

	const size_t first =
		(reinterpret_cast<uptr>(map_base) - reinterpret_cast<uptr>(s_arena.base)) / PAGE_SIZE;
	const size_t count = map_size / PAGE_SIZE;
	if (!UnmapRangeLocked(first, count))
		return nullptr;

	const size_t first_source_page = file_offset / PAGE_SIZE;
	const u32 flags = ENTRY_LAZY | (writable ? 0 : ENTRY_PROTECTED);
	for (size_t i = 0; i < count; i++)
	{
		const u32 entry = static_cast<u32>(first_source_page + i) | flags;
		StoreEntry(first + i, entry);
		if (!ReverseInsertLocked(first + i, entry))
		{
			StoreEntry(first + i, ENTRY_EMPTY);
			for (size_t previous = 0; previous < i; previous++)
			{
				const u32 old = LoadEntry(first + previous);
				ReverseRemoveLocked(first + previous, old);
				StoreEntry(first + previous, ENTRY_EMPTY);
			}
			return nullptr;
		}
	}
	return map_base;
}

bool UnmapArena(u8* map_base, size_t map_size)
{
	if (!map_base || map_size == 0 ||
		(reinterpret_cast<uptr>(map_base) & (PAGE_SIZE - 1)) != 0 ||
		(map_size & (PAGE_SIZE - 1)) != 0)
	{
		return false;
	}

	std::lock_guard lock(s_segment_mutex);
	if (!s_arena.pages ||
		!ContainsRange(reinterpret_cast<uptr>(s_arena.base), s_arena.size,
			reinterpret_cast<uptr>(map_base), map_size))
	{
		return false;
	}

	const size_t first =
		(reinterpret_cast<uptr>(map_base) - reinterpret_cast<uptr>(s_arena.base)) / PAGE_SIZE;
	return UnmapRangeLocked(first, map_size / PAGE_SIZE);
}

bool IsCanonicalAddress(uptr addr, size_t size)
{
	const uptr begin = s_managed_begin.load(std::memory_order_acquire);
	const uptr end = s_managed_end.load(std::memory_order_acquire);
	return begin != 0 && end >= begin && ContainsRange(begin, end - begin, addr, size);
}

bool IsArenaAddress(uptr addr, size_t size)
{
	return ContainsRange(s_arena_base.load(std::memory_order_acquire), ARENA_SIZE, addr, size);
}

bool PrepareCanonicalProtection(void* addr, size_t size)
{
	if ((reinterpret_cast<uptr>(addr) & (PAGE_SIZE - 1)) != 0 ||
		(size & (PAGE_SIZE - 1)) != 0)
	{
		return false;
	}

	std::lock_guard lock(s_segment_mutex);
	return SetCanonicalAliasesLocked(reinterpret_cast<uptr>(addr), size, false);
}

void RestoreCanonicalProtection(void* addr, size_t size)
{
	if ((reinterpret_cast<uptr>(addr) & (PAGE_SIZE - 1)) != 0 ||
		(size & (PAGE_SIZE - 1)) != 0)
	{
		return;
	}

	std::lock_guard lock(s_segment_mutex);
	SetCanonicalAliasesLocked(reinterpret_cast<uptr>(addr), size, true);
}

bool ProtectArena(void* addr, size_t size, bool writable)
{
	if ((reinterpret_cast<uptr>(addr) & (PAGE_SIZE - 1)) != 0 ||
		(size & (PAGE_SIZE - 1)) != 0)
	{
		return false;
	}

	std::lock_guard lock(s_segment_mutex);
	if (!s_arena.pages ||
		!ContainsRange(reinterpret_cast<uptr>(s_arena.base), s_arena.size,
			reinterpret_cast<uptr>(addr), size))
	{
		return false;
	}

	const size_t first =
		(reinterpret_cast<uptr>(addr) - reinterpret_cast<uptr>(s_arena.base)) / PAGE_SIZE;
	const size_t count = size / PAGE_SIZE;
	for (size_t i = 0; i < count; i++)
	{
		if (!SetAliasProtectionLocked(first + i, writable))
			return false;
	}
	return true;
}

bool ResolveFault(uptr addr)
{
	u32* const published_pages = s_arena_pages.load(std::memory_order_acquire);
	const uptr published_base = s_arena_base.load(std::memory_order_acquire);
	if (!published_pages || !ContainsRange(published_base, ARENA_SIZE, addr, 1))
		return false;

	const size_t fault_page = (addr - published_base) / PAGE_SIZE;
	const u32 candidate = __atomic_load_n(&published_pages[fault_page], __ATOMIC_ACQUIRE);
	if (candidate == ENTRY_EMPTY || !(candidate & ENTRY_LAZY) ||
		(candidate & ENTRY_PROTECTED))
	{
		return false;
	}

	std::lock_guard lock(s_segment_mutex);
	if (!s_arena.pages || published_pages != s_arena.pages ||
		!ContainsRange(reinterpret_cast<uptr>(s_arena.base), s_arena.size, addr, 1))
	{
		return false;
	}

	const size_t first =
		(addr - reinterpret_cast<uptr>(s_arena.base)) / PAGE_SIZE;
	const u32 entry = LoadEntry(first);
	Segment* segment;
	size_t source_page;
	if (entry == ENTRY_EMPTY || !(entry & ENTRY_LAZY) || (entry & ENTRY_PROTECTED) ||
		!EntrySourcePageLocked(entry, &segment, &source_page) ||
		s_arena.live_pages >= LIVE_PAGE_LIMIT)
	{
		return false;
	}

	const size_t available = LIVE_PAGE_LIMIT - s_arena.live_pages;
	size_t count = 1;
	while (count < FAULT_BATCH_PAGES && count < available &&
		first + count < s_arena.size / PAGE_SIZE)
	{
		const u32 next = LoadEntry(first + count);
		Segment* next_segment;
		size_t next_source_page;
		if (next == ENTRY_EMPTY || !(next & ENTRY_LAZY) || (next & ENTRY_PROTECTED) ||
			!EntrySourcePageLocked(next, &next_segment, &next_source_page) ||
			next_segment != segment || next_source_page != source_page + count)
		{
			break;
		}
		count++;
	}

	if (MapRunLocked(first, count, source_page))
		return true;
	return count > 1 && MapRunLocked(first, 1, source_page);
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
	delete[] segment.reverse_heads;
}

bool IsManagedFault(uptr addr)
{
	return IsCanonicalAddress(addr, 1) || IsArenaAddress(addr, 1);
}
} // namespace HorizonFastmem
