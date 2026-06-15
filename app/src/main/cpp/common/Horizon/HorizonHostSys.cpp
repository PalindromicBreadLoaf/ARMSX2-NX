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

	virtmemLock();
	void* dst = base ? base : virtmemFindAslr(size, 0);
	const Result rc = dst ? svcMapMemory(dst, src, size) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	virtmemUnlock();

	if (!dst || R_FAILED(rc))
	{
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

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

	if (mode.CanExecute())
	{
		// Execute permission can only be granted through the Jit aliasing path
		Console.Error("HostSys::MemProtect: execute permission is unsupported on Horizon");
		return;
	}

	const Result rc = svcSetMemoryPermission(baseaddr, size, HorizonProt(mode));
	if (R_FAILED(rc))
		pxFailRel("svcSetMemoryPermission() failed");
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return prefix;
}

// Fastmem doesn't really work on Horizon, so these are pretty well
// just no-ops to make the compiler happy

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	return nullptr;
}

void HostSys::DestroySharedMemory(void* ptr)
{
}

void* HostSys::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	return nullptr;
}

void HostSys::UnmapSharedMemory(void* baseaddr, size_t size)
{
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
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size)
{
	return nullptr;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	return nullptr;
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
	return false;
}

namespace PageFaultHandler
{
	static bool s_installed = false;
} // namespace PageFaultHandler

bool PageFaultHandler::Install(Error* error)
{

	s_installed = true;
	return true;
}
