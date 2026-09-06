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
#include "common/Horizon/HorizonFastmem.h"

namespace
{
	constexpr size_t FASTMEM_AREA_SIZE = 0x100000000ULL;

	enum class AreaBackend
	{
		LibnxSvcMapMemory, // svcMapMemory into the stack-alias region (fallback / jit reservation)
		AliasEager,        // eager svcMapProcessMemory alias of an AliasCodeData segment
		FastmemArena,
	};

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

	struct HorizonSharedMemory
	{
		size_t size;
		bool alias_code_data;
	};

	struct HorizonAreaState
	{
		AreaBackend backend;
		VirtmemReservation* reservation;
	};

	std::mutex s_code_mutex;
	std::map<u8*, HorizonCodeMapping> s_code_mappings;

	std::mutex s_shm_mutex;
	std::map<void*, HorizonSharedMemory> s_shm_handles;
	std::map<void*, HorizonRamMapping> s_ram_mappings;   // LibnxSvcMapMemory mappings
	std::map<void*, HorizonRamMapping> s_alias_mappings; // AliasEager mappings

	std::mutex s_area_mutex;
	std::map<u8*, HorizonAreaState> s_areas;

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

	AreaBackend GetAreaBackend(u8* base)
	{
		std::lock_guard lock(s_area_mutex);
		const auto it = s_areas.find(base);
		return (it != s_areas.end()) ? it->second.backend : AreaBackend::LibnxSvcMapMemory;
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

	if (HorizonFastmem::IsArenaAddress(reinterpret_cast<uptr>(baseaddr), size))
	{
		if (!HorizonFastmem::ProtectArena(baseaddr, size, mode.CanWrite()))
		{
			static bool s_warned = false;
			if (!s_warned)
			{
				Console.Warning("HostSys::MemProtect: failed to update Horizon fastmem protection "
								"(further failures suppressed)");
				s_warned = true;
			}
		}
		return;
	}

	const bool canonical =
		HorizonFastmem::IsCanonicalAddress(reinterpret_cast<uptr>(baseaddr), size);
	if (canonical && !mode.CanWrite() && !HorizonFastmem::PrepareCanonicalProtection(baseaddr, size))
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			Console.Warning("HostSys::MemProtect: failed to unmap Horizon fastmem aliases "
							"(further failures suppressed)");
			s_warned = true;
		}
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
	else if (canonical && mode.CanWrite())
	{
		HorizonFastmem::RestoreCanonicalProtection(baseaddr, size);
	}
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return prefix;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	(void)name;

	if (u8* const canonical = HorizonFastmem::CreateSegment(size))
	{
		std::lock_guard lock(s_shm_mutex);
		s_shm_handles.emplace(canonical, HorizonSharedMemory{size, true});
		return canonical;
	}

	static bool s_warned = false;
	if (!s_warned)
	{
		if (HorizonFastmem::IsSupported())
			Console.Warning("Horizon fastmem segment allocation failed.");
		else
			Console.Warning("Horizon fastmem unavailable: %s. Using plain shared memory.",
				HorizonFastmem::GetSupportReason());
		s_warned = true;
	}

	void* const backing = memalign(__pagesize, size);
	if (!backing)
		return nullptr;

	std::memset(backing, 0, size);
	std::lock_guard lock(s_shm_mutex);
	s_shm_handles.emplace(backing, HorizonSharedMemory{size, false});
	return backing;
}

void HostSys::DestroySharedMemory(void* ptr)
{
	std::unique_lock lock(s_shm_mutex);
	const auto it = s_shm_handles.find(ptr);
	if (it == s_shm_handles.end())
		return;

	const bool alias_code_data = it->second.alias_code_data;
	s_shm_handles.erase(it);
	lock.unlock();

	if (alias_code_data)
		HorizonFastmem::DestroySegment(static_cast<u8*>(ptr));
	else
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

	HorizonAreaState state{AreaBackend::LibnxSvcMapMemory, nullptr};
	bool found = false;
	{
		std::lock_guard lock(s_area_mutex);
		const auto it = s_areas.find(m_base_ptr);
		if (it != s_areas.end())
		{
			state = it->second;
			s_areas.erase(it);
			found = true;
		}
	}
	if (!found)
		return;

	if (state.backend == AreaBackend::FastmemArena)
	{
		if (!HorizonFastmem::DestroyArena(m_base_ptr, m_size))
			pxFailRel("Failed to release Horizon fastmem area");
		return;
	}

	virtmemLock();
	virtmemRemoveReservation(state.reservation);
	virtmemUnlock();
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit, uptr fixed_base_hint)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");
	(void)fixed_base_hint;

	if (!jit && HorizonFastmem::IsSupported())
	{
		if (size == FASTMEM_AREA_SIZE)
		{
			if (u8* const base = HorizonFastmem::CreateArena(size))
			{
				std::lock_guard lock(s_area_mutex);
				s_areas.emplace(base, HorizonAreaState{AreaBackend::FastmemArena, nullptr});
				return std::unique_ptr<SharedMemoryMappingArea>(
					new SharedMemoryMappingArea(base, size, size / __pagesize));
			}
		}
		else
		{
			virtmemLock();
			void* const base = virtmemFindAslr(size, 0);
			VirtmemReservation* const reservation =
				base ? virtmemAddReservation(base, size) : nullptr;
			virtmemUnlock();
			if (base && reservation)
			{
				std::lock_guard lock(s_area_mutex);
				s_areas.emplace(static_cast<u8*>(base), HorizonAreaState{AreaBackend::AliasEager, reservation});
				return std::unique_ptr<SharedMemoryMappingArea>(
					new SharedMemoryMappingArea(static_cast<u8*>(base), size, size / __pagesize));
			}
		}
	}

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
		s_areas.emplace(static_cast<u8*>(base), HorizonAreaState{AreaBackend::LibnxSvcMapMemory, reservation});
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

	switch (GetAreaBackend(m_base_ptr))
	{
		case AreaBackend::FastmemArena:
		{
			u8* const result = HorizonFastmem::MapArena(
				file_handle, file_offset, static_cast<u8*>(map_base), map_size, mode.CanWrite());
			if (result)
				m_num_mappings++;
			return result;
		}

		case AreaBackend::AliasEager:
		{
			if (!file_handle)
			{
				Console.Error("SharedMemoryMappingArea::Map: anonymous mappings are unsupported on the Horizon alias path");
				return nullptr;
			}

			u8* const source = static_cast<u8*>(file_handle) + file_offset;
			const Result map_result = svcMapProcessMemory(map_base, envGetOwnProcessHandle(),
				reinterpret_cast<u64>(source), map_size);
			if (R_FAILED(map_result))
			{
				Console.Error("SharedMemoryMappingArea::Map: svcMapProcessMemory(%p, %p, 0x%zx) failed: 0x%08x",
					map_base, source, map_size, map_result);
				return nullptr;
			}
			if (HorizonProt(mode) != Perm_Rw)
				svcSetMemoryPermission(map_base, map_size, HorizonProt(mode));

			{
				std::lock_guard lock(s_shm_mutex);
				s_alias_mappings.emplace(map_base, HorizonRamMapping{source, map_size});
			}
			m_num_mappings++;
			return static_cast<u8*>(map_base);
		}

		case AreaBackend::LibnxSvcMapMemory:
		default:
		{
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
	}
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	(void)is_file;

	// Executable (Jit-backed) region?
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

	switch (GetAreaBackend(m_base_ptr))
	{
		case AreaBackend::FastmemArena:
		{
			if (!HorizonFastmem::UnmapArena(static_cast<u8*>(map_base), map_size))
				return false;
			m_num_mappings--;
			return true;
		}

		case AreaBackend::AliasEager:
		{
			std::unique_lock lock(s_shm_mutex);
			const auto it = s_alias_mappings.find(map_base);
			if (it == s_alias_mappings.end())
				return false;
			const HorizonRamMapping mapping = it->second;
			s_alias_mappings.erase(it);
			lock.unlock();

			svcSetMemoryPermission(map_base, mapping.size, Perm_Rw);
			virtmemLock();
			svcUnmapProcessMemory(map_base, envGetOwnProcessHandle(),
				reinterpret_cast<u64>(mapping.src), mapping.size);
			virtmemUnlock();
			m_num_mappings--;
			return true;
		}

		case AreaBackend::LibnxSvcMapMemory:
		default:
		{
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
	}
}

bool PageFaultHandler::Install(Error* error)
{
	(void)error;
	return HorizonFastmem::HasResumableFaultHandler();
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return HorizonFastmem::HasResumableFaultHandler();
}
