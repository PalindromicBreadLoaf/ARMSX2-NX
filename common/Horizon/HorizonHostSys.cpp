// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <malloc.h>

#include "common/Horizon/Horizon.h"

namespace
{
	struct HorizonCodeMapping
	{
		Jit jit;
		u8* rx;
		u8* rw;
		size_t size;
	};

	struct HorizonRamMapping
	{
		void* src;
		size_t size;
	};

	std::mutex s_code_mutex;
	std::map<u8*, HorizonCodeMapping> s_code_mappings;

	std::mutex s_shm_mutex;
	std::map<void*, size_t> s_shm_handles;
	std::map<void*, HorizonRamMapping> s_ram_mappings;

	std::mutex s_area_mutex;
	std::map<u8*, VirtmemReservation*> s_area_reservations;

	const HorizonCodeMapping* FindCodeMapping(const void* ptr)
	{
		auto it = s_code_mappings.upper_bound(const_cast<u8*>(static_cast<const u8*>(ptr)));
		if (it == s_code_mappings.begin())
			return nullptr;

		--it;
		const HorizonCodeMapping& mapping = it->second;
		const u8* const address = static_cast<const u8*>(ptr);
		return (address >= mapping.rx && address < (mapping.rx + mapping.size)) ? &mapping : nullptr;
	}

	u32 HorizonProt(const PageProtectionMode& mode)
	{
		if (mode.CanWrite())
			return Perm_Rw;
		if (mode.CanRead())
			return Perm_R;
		return Perm_None;
	}
} // namespace

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");
	if (mode.CanExecute())
	{
		Console.Error("HostSys::MemProtect: execute permission is unsupported on Horizon");
		return;
	}

	const Result rc = svcSetMemoryPermission(baseaddr, size, HorizonProt(mode));
	if (R_FAILED(rc))
	{
		static bool warned = false;
		if (!warned)
		{
			Console.Warning("HostSys::MemProtect: svcSetMemoryPermission(%p, 0x%zx) failed: 0x%08x; further failures suppressed",
				baseaddr, size, static_cast<u32>(rc));
			warned = true;
		}
	}
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return prefix;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	(void)name;
	void* const backing = memalign(__pagesize, size);
	if (!backing)
		return nullptr;

	std::memset(backing, 0, size);
	std::lock_guard lock(s_shm_mutex);
	s_shm_handles.emplace(backing, size);
	return backing;
}

void HostSys::DestroySharedMemory(void* ptr)
{
	std::lock_guard lock(s_shm_mutex);
	if (s_shm_handles.erase(ptr) != 0)
		free(ptr);
}

size_t HostSys::GetRuntimePageSize()
{
	return 0x1000;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return 64;
}

void HostSys::FlushInstructionCache(void* address, u32 size)
{
	char* const rx = static_cast<char*>(address);
	char* rw = rx;
	{
		std::lock_guard lock(s_code_mutex);
		if (const HorizonCodeMapping* mapping = FindCodeMapping(address))
			rw = reinterpret_cast<char*>(mapping->rw + (reinterpret_cast<u8*>(rx) - mapping->rx));
	}

	__builtin___clear_cache(rw, rw + size);
	if (rw != rx)
		__builtin___clear_cache(rx, rx + size);
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");

	std::unique_lock lock(s_area_mutex);
	const auto it = s_area_reservations.find(m_base_ptr);
	if (it == s_area_reservations.end())
		return;

	VirtmemReservation* const reservation = it->second;
	s_area_reservations.erase(it);
	lock.unlock();

	virtmemLock();
	virtmemRemoveReservation(reservation);
	virtmemUnlock();
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit, uptr fixed_base_hint)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");
	(void)jit;
	(void)fixed_base_hint;

	virtmemLock();
	void* const base = virtmemFindStack(size, 0);
	VirtmemReservation* const reservation = base ? virtmemAddReservation(base, size) : nullptr;
	virtmemUnlock();
	if (!base || !reservation)
	{
		Console.Error("SharedMemoryMappingArea::Create: failed to reserve 0x%zx bytes", size);
		return nullptr;
	}

	{
		std::lock_guard lock(s_area_mutex);
		s_area_reservations.emplace(static_cast<u8*>(base), reservation);
	}
	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(base), size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));
	if (mode.CanExecute())
	{
		Jit jit;
		const Result create_result = jitCreate(&jit, map_size);
		if (R_FAILED(create_result))
		{
			Console.Error("SharedMemoryMappingArea::Map: jitCreate(%zu) failed: 0x%08x", map_size, create_result);
			return nullptr;
		}
		const Result transition_result = jitTransitionToExecutable(&jit);
		if (R_FAILED(transition_result))
		{
			Console.Error("SharedMemoryMappingArea::Map: jitTransitionToExecutable() failed: 0x%08x", transition_result);
			jitClose(&jit);
			return nullptr;
		}

		u8* const rx = static_cast<u8*>(jitGetRxAddr(&jit));
		u8* const rw = static_cast<u8*>(jitGetRwAddr(&jit));
		{
			std::lock_guard lock(s_code_mutex);
			s_code_mappings.emplace(rx, HorizonCodeMapping{jit, rx, rw, map_size});
		}
		m_num_mappings++;
		return rx;
	}

	if (!file_handle)
	{
		Console.Error("SharedMemoryMappingArea::Map: anonymous non-executable mappings are unsupported on Horizon");
		return nullptr;
	}

	u8* const source = static_cast<u8*>(file_handle) + file_offset;
	const Result map_result = svcMapMemory(map_base, source, map_size);
	if (R_FAILED(map_result))
	{
		Console.Error("SharedMemoryMappingArea::Map: svcMapMemory(%p, %p, 0x%zx) failed: 0x%08x", map_base, source, map_size, map_result);
		return nullptr;
	}
	if (HorizonProt(mode) != Perm_Rw)
		svcSetMemoryPermission(map_base, map_size, HorizonProt(mode));

	{
		std::lock_guard lock(s_shm_mutex);
		s_ram_mappings.emplace(map_base, HorizonRamMapping{source, map_size});
	}
	m_num_mappings++;
	return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	(void)map_size;
	(void)is_file;
	{
		std::unique_lock lock(s_code_mutex);
		const auto it = s_code_mappings.find(static_cast<u8*>(map_base));
		if (it != s_code_mappings.end())
		{
			Jit jit = it->second.jit;
			s_code_mappings.erase(it);
			lock.unlock();
			jitClose(&jit);
			m_num_mappings--;
			return true;
		}
	}

	std::unique_lock lock(s_shm_mutex);
	const auto it = s_ram_mappings.find(map_base);
	if (it == s_ram_mappings.end())
		return false;
	const HorizonRamMapping mapping = it->second;
	s_ram_mappings.erase(it);
	lock.unlock();

	svcSetMemoryPermission(map_base, mapping.size, Perm_Rw);
	virtmemLock();
	svcUnmapMemory(map_base, mapping.src, mapping.size);
	virtmemUnlock();
	m_num_mappings--;
	return true;
}

bool PageFaultHandler::Install(Error* error)
{
	(void)error;
	return true;
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return true;
}
