// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/Horizon/HorizonFastmem.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <malloc.h>

#include "common/Horizon/Horizon.h"

namespace
{
	struct HorizonMapping
	{
		void* src; // heap backing pointer
		size_t size;
	};

	std::mutex s_mapping_mutex;
	std::map<void*, HorizonMapping> s_mappings;

	// Nintendo hates JIT so this is a hack but it maybe works?
	struct HorizonCodeMapping
	{
		Jit jit;
		u8* rx; // writeable page
		u8* rw; // writable alias of the same physical page
		size_t size;
	};

	std::mutex s_code_mutex;
	std::map<u8*, HorizonCodeMapping> s_code_mappings;

	std::mutex s_shm_mutex;
	struct HorizonSharedMemory
	{
		size_t size;
		bool alias_code_data;
	};
	struct HorizonSharedMapping
	{
		void* src;
		size_t size;
		bool direct;
	};
	std::map<void*, HorizonSharedMemory> s_shm_handles;
	std::map<void*, HorizonSharedMapping> s_shm_mappings;

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

void* HostSys::Mmap(void* base, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

	if (mode.IsNone())
		return nullptr;

	// Horizon cannot make anonymous pages executable
	if (mode.CanExecute())
	{
		Jit jit;
		const Result rc = jitCreate(&jit, size);
		if (R_FAILED(rc))
		{
			Console.Error("HostSys::Mmap: jitCreate(%zu) failed: 0x%08x. This usually means the "
						  "process lacks the JIT capability. Make sure you aren't using Applet Mode.", size, rc);
			return nullptr;
		}

		const Result trc = jitTransitionToExecutable(&jit);
		if (R_FAILED(trc))
		{
			Console.Error("HostSys::Mmap: jitTransitionToExecutable() failed: 0x%08x", trc);
			jitClose(&jit);
			return nullptr;
		}

		u8* const rx = static_cast<u8*>(jitGetRxAddr(&jit));
		u8* const rw = static_cast<u8*>(jitGetRwAddr(&jit));

		std::lock_guard lock(s_code_mutex);
		s_code_mappings.emplace(rx, HorizonCodeMapping{jit, rx, rw, size});
		return rx;
	}

	void* src = memalign(__pagesize, size);
	if (!src)
		return nullptr;
	std::memset(src, 0, size);

	// svcMapMemory requires the destination to live in the process' stack region
	virtmemLock();
	void* dst = base ? base : virtmemFindStack(size, 0);
	const Result rc = dst ? svcMapMemory(dst, src, size) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	virtmemUnlock();

	if (!dst || R_FAILED(rc))
	{
		Console.Error("HostSys::Mmap: svcMapMemory(%p, %p, 0x%zx) failed: 0x%08x", dst, src, size, rc);
		free(src);
		return nullptr;
	}

	const u32 prot = HorizonProt(mode);
	if (prot != Perm_Rw)
		svcSetMemoryPermission(dst, size, prot);

	std::lock_guard lock(s_mapping_mutex);
	s_mappings.emplace(dst, HorizonMapping{src, size});
	return dst;
}

void HostSys::Munmap(void* base, size_t size)
{
	if (!base)
		return;

	// Executable (Jit-backed) region?
	{
		std::unique_lock clock(s_code_mutex);
		const auto cit = s_code_mappings.find(static_cast<u8*>(base));
		if (cit != s_code_mappings.end())
		{
			Jit jit = cit->second.jit;
			s_code_mappings.erase(cit);
			clock.unlock();
			jitClose(&jit);
			return;
		}
	}

	std::unique_lock lock(s_mapping_mutex);
	const auto it = s_mappings.find(base);
	if (it == s_mappings.end())
		return;

	void* const src = it->second.src;
	const size_t mapped_size = it->second.size;
	s_mappings.erase(it);
	lock.unlock();

	svcSetMemoryPermission(base, mapped_size, Perm_Rw);

	virtmemLock();
	svcUnmapMemory(base, src, mapped_size);
	virtmemUnlock();

	free(src);
}

namespace
{
	// hbloader does not exit between NRO launches, so anything we leak is still there on the
	// next run.
	void ReleaseLeakedHorizonMappings()
	{
		{
			std::lock_guard lock(s_code_mutex);
			for (auto& mapping : s_code_mappings)
				jitClose(&mapping.second.jit);
			s_code_mappings.clear();
		}

		{
			std::lock_guard lock(s_mapping_mutex);
			for (auto& mapping : s_mappings)
			{
				svcSetMemoryPermission(mapping.first, mapping.second.size, Perm_Rw);
				virtmemLock();
				svcUnmapMemory(mapping.first, mapping.second.src, mapping.second.size);
				virtmemUnlock();
				free(mapping.second.src);
			}
			s_mappings.clear();
		}
	}

	[[gnu::constructor(101)]] void RegisterHorizonMappingRelease()
	{
		std::atexit(&ReleaseLeakedHorizonMappings);
	}
} // namespace

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

	// The error sites below are throttled to not overload the SD Card.
	if (mode.CanExecute())
	{
		// Execute permission can only be granted through the Jit aliasing path
		ERROR_LOG_THROTTLED("HostSys::MemProtect: execute permission is unsupported on Horizon");
		return;
	}

	if (HorizonFastmem::IsArenaAddress(reinterpret_cast<uptr>(baseaddr), size))
	{
		if (!HorizonFastmem::ProtectArena(baseaddr, size, mode.CanWrite()))
			ERROR_LOG_THROTTLED("HostSys::MemProtect: failed to update Horizon fastmem protection");
		return;
	}

	const bool canonical =
		HorizonFastmem::IsCanonicalAddress(reinterpret_cast<uptr>(baseaddr), size);
	if (canonical && !mode.CanWrite() &&
		!HorizonFastmem::PrepareCanonicalProtection(baseaddr, size))
	{
		ERROR_LOG_THROTTLED("HostSys::MemProtect: failed to unmap Horizon fastmem aliases");
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
	if (u8* const canonical = HorizonFastmem::CreateSegment(size))
	{
		std::lock_guard lock(s_shm_mutex);
		s_shm_handles.emplace(canonical, HorizonSharedMemory{size, true});
		Console.WriteLn(Color_StrongGreen, "Horizon SMC write protection enabled: %s",
			HorizonFastmem::GetSupportReason());
		return canonical;
	}

	static bool s_warned = false;
	if (!s_warned)
	{
		if (HorizonFastmem::IsSupported())
			Console.Warning("Horizon SMC write protection setup failed. Using manual integrity checks.");
		else
			Console.Warning("Horizon SMC write protection unavailable: %s. Using manual integrity checks.",
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

void* HostSys::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	if (mode.CanExecute())
	{
		Console.Error("HostSys::MapSharedMemory: executable shared mappings are unsupported on Horizon");
		return nullptr;
	}

	std::lock_guard lock(s_shm_mutex);
	const auto handle_it = s_shm_handles.find(handle);
	if (handle_it == s_shm_handles.end() || offset > handle_it->second.size ||
		size > handle_it->second.size - offset)
	{
		return nullptr;
	}

	u8* const src = static_cast<u8*>(handle) + offset;
	if (handle_it->second.alias_code_data)
	{
		if (baseaddr && baseaddr != src)
		{
			Console.Error("HostSys::MapSharedMemory: fixed aliases are not implemented on Horizon");
			return nullptr;
		}

		const u32 prot = HorizonProt(mode);
		if (prot != Perm_Rw)
		{
			const Result rc = svcSetMemoryPermission(src, size, prot);
			if (R_FAILED(rc))
			{
				Console.Error("HostSys::MapSharedMemory: svcSetMemoryPermission(%p, 0x%zx) failed: 0x%08x",
					src, size, static_cast<u32>(rc));
				return nullptr;
			}
		}

		s_shm_mappings.emplace(src, HorizonSharedMapping{src, size, true});
		return src;
	}

	virtmemLock();
	void* dst = baseaddr ? baseaddr : virtmemFindStack(size, 0);
	const Result rc = dst ? svcMapMemory(dst, src, size) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	virtmemUnlock();

	if (!dst || R_FAILED(rc))
	{
		Console.Error("HostSys::MapSharedMemory: svcMapMemory(%p, %p, 0x%zx) failed: 0x%08x", dst, src, size, rc);
		return nullptr;
	}

	const u32 prot = HorizonProt(mode);
	if (prot != Perm_Rw)
		svcSetMemoryPermission(dst, size, prot);

	s_shm_mappings.emplace(dst, HorizonSharedMapping{src, size, false});
	return dst;
}

void HostSys::UnmapSharedMemory(void* baseaddr, size_t size)
{
	std::unique_lock lock(s_shm_mutex);
	const auto it = s_shm_mappings.find(baseaddr);
	if (it == s_shm_mappings.end())
		return;

	void* const src = it->second.src; // borrowed. owned by the handle. do not free
	const size_t mapped_size = it->second.size;
	const bool direct = it->second.direct;
	s_shm_mappings.erase(it);
	lock.unlock();

	svcSetMemoryPermission(baseaddr, mapped_size, Perm_Rw);
	if (direct)
		return;

	virtmemLock();
	svcUnmapMemory(baseaddr, src, mapped_size);
	virtmemUnlock();
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

	if (!HorizonFastmem::DestroyArena(m_base_ptr, m_size))
		pxFailRel("Failed to release Horizon fastmem area");
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

	u8* const base = HorizonFastmem::CreateArena(size);
	if (!base)
		return nullptr;

	return std::unique_ptr<SharedMemoryMappingArea>(
		new SharedMemoryMappingArea(base, size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr &&
		static_cast<u8*>(map_base) < (m_base_ptr + m_size));

	u8* const result = HorizonFastmem::MapArena(
		file_handle, file_offset, static_cast<u8*>(map_base), map_size, mode.CanWrite());
	if (result)
		m_num_mappings++;
	return result;
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr &&
		static_cast<u8*>(map_base) < (m_base_ptr + m_size));

	if (!HorizonFastmem::UnmapArena(static_cast<u8*>(map_base), map_size))
		return false;

	m_num_mappings--;
	return true;
}

namespace PageFaultHandler
{
	static std::atomic<bool> s_installed{false};
} // namespace PageFaultHandler

bool PageFaultHandler::Install(Error* error)
{
	s_installed.store(true, std::memory_order_release);
	return true;
}

bool PageFaultHandler::IsHorizonFaultCandidate(void* fault_address)
{
	return s_installed.load(std::memory_order_acquire) &&
		HorizonFastmem::IsManagedFault(reinterpret_cast<uptr>(fault_address));
}
