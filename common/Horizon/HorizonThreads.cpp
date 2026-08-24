// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/Assertions.h"
#include "common/Threading.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "common/Horizon/Horizon.h"

namespace
{
	std::mutex s_thread_handle_map_mutex;
	std::unordered_map<void*, Handle> s_thread_handle_map;

	u64 GetThreadCPUTimeUS(void* native_handle)
	{
		if (!native_handle)
			return 0;

		Handle handle;
		if (static_cast<void*>(pthread_self()) == native_handle)
		{
			handle = CUR_THREAD_HANDLE;
		}
		else
		{
			std::lock_guard lock(s_thread_handle_map_mutex);
			const auto it = s_thread_handle_map.find(native_handle);
			if (it == s_thread_handle_map.end())
				return 0;
			handle = it->second;
		}

		u64 ticks = 0;
		if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
			R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total)))
		{
			return 0;
		}
		return armTicksToNs(ticks) / 1000;
	}
} // namespace

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

__forceinline void Threading::SpinWait()
{
	__asm__ __volatile__("isb");
}

__forceinline void Threading::EnableHiresScheduler()
{
}

__forceinline void Threading::DisableHiresScheduler()
{
}

u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

u64 Threading::GetThreadCpuTime()
{
	return GetThreadCPUTimeUS(static_cast<void*>(pthread_self()));
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle result;
	result.m_native_handle = static_cast<void*>(pthread_self());
	std::lock_guard lock(s_thread_handle_map_mutex);
	s_thread_handle_map[result.m_native_handle] = threadGetCurHandle();
	return result;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	return GetThreadCPUTimeUS(m_native_handle);
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (!m_native_handle)
		return false;

	u64 allowed_cores = 0;
	if (R_FAILED(svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || allowed_cores == 0)
		return false;
	const u64 mask = processor_mask ? (processor_mask & allowed_cores) : allowed_cores;
	if (!mask)
		return false;

	Handle handle;
	if (static_cast<void*>(pthread_self()) == m_native_handle)
	{
		handle = CUR_THREAD_HANDLE;
	}
	else
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		const auto it = s_thread_handle_map.find(m_native_handle);
		if (it == s_thread_handle_map.end())
			return false;
		handle = it->second;
	}
	return R_SUCCEEDED(svcSetThreadCoreMask(handle, __builtin_ctzll(mask), static_cast<u32>(mask)));
}

bool Threading::ThreadHandle::SetNicePriority(int nice) const
{
	(void)nice;
	return false;
}

u64 Threading::ThreadHandle::GetAffinity() const
{
	u64 allowed_cores = 0;
	return R_SUCCEEDED(svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? allowed_cores : 0;
}

int Threading::ThreadHandle::GetCurrentCpu() const
{
	return -1;
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	void* const key = static_cast<void*>(pthread_self());
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		s_thread_handle_map[key] = threadGetCurHandle();
	}
	(*entry)();
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		s_thread_handle_map.erase(key);
	}
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already started thread");
	auto entry = std::make_unique<EntryPoint>(std::move(func));
	pthread_attr_t attrs;
	pthread_attr_t* attr = nullptr;
	if (m_stack_size != 0)
	{
		pthread_attr_init(&attrs);
		pthread_attr_setstacksize(&attrs, m_stack_size);
		attr = &attrs;
	}

	pthread_t handle;
	const int result = pthread_create(&handle, attr, ThreadProc, entry.get());
	if (attr)
		pthread_attr_destroy(&attrs);
	if (result != 0)
		return false;
	m_native_handle = static_cast<void*>(handle);
	entry.release();
	return true;
}

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach(static_cast<pthread_t>(m_native_handle));
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	if (pthread_join(static_cast<pthread_t>(m_native_handle), nullptr) != 0)
		pxFailRel("pthread_join() for thread join failed");
	m_native_handle = nullptr;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	(void)name;
}
