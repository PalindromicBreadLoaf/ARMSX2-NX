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
	// Nintendo hates JIT so this is a hack but it works.
	struct HorizonCodeMapping
	{
		Jit jit;
		u8* rx; // executable view
		u8* rw; // writable alias of the same physical pages
		size_t size;
	};

	// svcMapMemory alias of a CreateSharedMemory() backing into a reserved range.
	struct HorizonRamMapping
	{
		void* src; // borrowed backing pointer
		size_t size;
	};

	std::mutex s_code_mutex;
	std::map<u8*, HorizonCodeMapping> s_code_mappings; // rx base

	std::mutex s_shm_mutex;
	std::map<void*, size_t> s_shm_handles;          // backing pointer
	std::map<void*, HorizonRamMapping> s_ram_mappings; // mapped base

	std::mutex s_area_mutex;
	std::map<u8*, VirtmemReservation*> s_area_reservations; // area base

	const HorizonCodeMapping* FindCodeMapping(const void* ptr)
	{
		if (s_code_mappings.empty())
			return nullptr;

		auto it = s_code_mappings.upper_bound(const_cast<u8*>(static_cast<const u8*>(ptr)));
		if (it == s_code_mappings.begin())
			return nullptr;

		--it;
		const HorizonCodeMapping& m = it->second;
		const u8* const p = static_cast<const u8*>(ptr);
		return (p >= m.rx && p < (m.rx + m.size)) ? &m : nullptr;
	}
} // namespace

static u32 HorizonProt(const PageProtectionMode& mode)
{
	if (mode.CanWrite())
		return Perm_Rw;
	if (mode.CanRead())
		return Perm_R;
	return Perm_None;
}

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
		static bool s_warned = false;
		if (!s_warned)
		{
			Console.Warning("HostSys::MemProtect: svcSetMemoryPermission(%p, 0x%zx) failed: 0x%08x "
							"(continuing with further failures suppressed)", baseaddr, size, static_cast<u32>(rc));
			s_warned = true;
		}
	}
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return prefix;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
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
	if (s_shm_handles.erase(ptr) > 0)
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

void* HostSys::JitGetWritablePointer(void* exec_ptr)
{
	std::lock_guard lock(s_code_mutex);
	if (const HorizonCodeMapping* m = FindCodeMapping(exec_ptr))
		return m->rw + (static_cast<u8*>(exec_ptr) - m->rx);

	// ^ Not inside a managed JIT region
	return exec_ptr;
}

void HostSys::FlushInstructionCache(void* address, u32 size)
{
	char* const rx = static_cast<char*>(address);
	char* rw = rx;
	{
		std::lock_guard lock(s_code_mutex);
		if (const HorizonCodeMapping* m = FindCodeMapping(address))
			rw = reinterpret_cast<char*>(m->rw + (reinterpret_cast<u8*>(rx) - m->rx));
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
	if (it != s_area_reservations.end())
	{
		VirtmemReservation* const rv = it->second;
		s_area_reservations.erase(it);
		lock.unlock();

		virtmemLock();
		virtmemRemoveReservation(rv);
		virtmemUnlock();
	}
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

	// On Horizon only the JIT code+data reservation is backed.
	if (!jit)
		return nullptr;

	// svcMapMemory requires the destination to live in the process' stack region.
	virtmemLock();
	void* const base = virtmemFindStack(size, 0);
	VirtmemReservation* const rv = base ? virtmemAddReservation(base, size) : nullptr;
	virtmemUnlock();

	if (!base || !rv)
	{
		Console.Error("SharedMemoryMappingArea::Create: failed to reserve 0x%zx bytes", size);
		return nullptr;
	}

	{
		std::lock_guard lock(s_area_mutex);
		s_area_reservations.emplace(static_cast<u8*>(base), rv);
	}

	return std::unique_ptr<SharedMemoryMappingArea>(
		new SharedMemoryMappingArea(static_cast<u8*>(base), size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	if (mode.CanExecute())
	{
		Jit jit;
		const Result rc = jitCreate(&jit, map_size);
		if (R_FAILED(rc))
		{
			Console.Error("SharedMemoryMappingArea::Map: jitCreate(%zu) failed: 0x%08x. This usually means the "
						  "process lacks the JIT capability. Make sure you aren't using Applet Mode.", map_size, rc);
			return nullptr;
		}

		const Result trc = jitTransitionToExecutable(&jit);
		if (R_FAILED(trc))
		{
			Console.Error("SharedMemoryMappingArea::Map: jitTransitionToExecutable() failed: 0x%08x", trc);
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
		// Anonymous, non-executable mappings are only requested by the (disabled) fastmem path.
		Console.Error("SharedMemoryMappingArea::Map: anonymous non-exec mappings are unsupported on Horizon");
		return nullptr;
	}

	u8* const src = static_cast<u8*>(file_handle) + file_offset;

	const Result rc = svcMapMemory(map_base, src, map_size);
	if (R_FAILED(rc))
	{
		Console.Error("SharedMemoryMappingArea::Map: svcMapMemory(%p, %p, 0x%zx) failed: 0x%08x", map_base, src, map_size, rc);
		return nullptr;
	}

	const u32 prot = HorizonProt(mode);
	if (prot != Perm_Rw)
		svcSetMemoryPermission(map_base, map_size, prot);

	{
		std::lock_guard lock(s_shm_mutex);
		s_ram_mappings.emplace(map_base, HorizonRamMapping{src, map_size});
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	// Executable (Jit-backed) region?
	{
		std::unique_lock clock(s_code_mutex);
		const auto cit = s_code_mappings.find(static_cast<u8*>(map_base));
		if (cit != s_code_mappings.end())
		{
			Jit jit = cit->second.jit;
			s_code_mappings.erase(cit);
			clock.unlock();
			jitClose(&jit);
			m_num_mappings--;
			return true;
		}
	}

	std::unique_lock lock(s_shm_mutex);
	const auto it = s_ram_mappings.find(map_base);
	if (it == s_ram_mappings.end())
		return false;

	void* const src = it->second.src; // owned by the shared-memory handle
	const size_t mapped_size = it->second.size;
	s_ram_mappings.erase(it);
	lock.unlock();

	svcSetMemoryPermission(map_base, mapped_size, Perm_Rw);

	virtmemLock();
	svcUnmapMemory(map_base, src, mapped_size);
	virtmemUnlock();

	m_num_mappings--;
	return true;
}

namespace PageFaultHandler
{
	static bool s_installed = false;
} // namespace PageFaultHandler

bool PageFaultHandler::Install(Error* error)
{
	// Horizon has no POSIX signal delivery for SIGSEGV
	s_installed = true;
	return true;
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return true;
}
