// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright (c): PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
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
		Console.Error("HostSys::Mmap: executable mappings are unsupported on Horizon");
		return nullptr;
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

void HostSys::FlushInstructionCache(void* address, u32 size)
{
	char* const start = static_cast<char*>(address);
	__builtin___clear_cache(start, start + size);
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
