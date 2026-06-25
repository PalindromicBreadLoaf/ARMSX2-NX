// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/Horizon/Horizon.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

namespace
{
	// Pin threads to cores
	std::mutex s_thread_handle_map_mutex;
	std::unordered_map<void*, Handle> s_thread_handle_map;
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

static u64 get_thread_time(uptr id = 0)
{
	const pthread_t thread = id ? (pthread_t)id : pthread_self();

	clockid_t cid;
	if (pthread_getcpuclockid(thread, &cid) != 0)
		return 0;

	struct timespec ts;
	if (clock_gettime(cid, &ts) != 0)
		return 0;

	return (u64)ts.tv_sec * (u64)1e6 + (u64)ts.tv_nsec / (u64)1e3;
}

u64 Threading::GetThreadCpuTime()
{
	return get_thread_time();
}

// Per-thread CPU time
static u64 get_thread_cpu_time_us(void* native_handle)
{
	if (!native_handle)
		return 0;

	Handle handle;
	if ((void*)pthread_self() == native_handle)
	{
		handle = CUR_THREAD_HANDLE;
	}
	else
	{
		std::lock_guard<std::mutex> lock(s_thread_handle_map_mutex);
		const auto it = s_thread_handle_map.find(native_handle);
		if (it == s_thread_handle_map.end())
			return 0;
		handle = it->second;
	}

	u64 ticks = 0;
	// InfoType_ThreadTickCount is 13.0.0+
	// I don't even know if this runs sub-13.0.0, but it's a one line thing.
	if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
		R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total)))
		return 0;

	return armTicksToNs(ticks) / 1000;
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
	ThreadHandle ret;
	ret.m_native_handle = (void*)pthread_self();
	// Register the calling thread so GetCPUTime() can later resolve its handle
	{
		std::lock_guard<std::mutex> lock(s_thread_handle_map_mutex);
		s_thread_handle_map[ret.m_native_handle] = threadGetCurHandle();
	}
	return ret;
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
	return get_thread_cpu_time_us(m_native_handle);
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (!m_native_handle)
		return false;

	// Check for allowed cores (should be 3 unless under applet mode)
	u64 allowed_cores = 0;
	if (R_FAILED(svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || allowed_cores == 0)
		return false;

	// Zero mask for unpin
	const u64 mask = (processor_mask != 0) ? (processor_mask & allowed_cores) : allowed_cores;
	if (mask == 0)
		return false;

	const s32 preferred_core = __builtin_ctzll(mask);

	Handle handle;
	if ((void*)pthread_self() == m_native_handle)
	{
		handle = CUR_THREAD_HANDLE;
	}
	else
	{
		std::lock_guard<std::mutex> lock(s_thread_handle_map_mutex);
		const auto it = s_thread_handle_map.find(m_native_handle);
		if (it == s_thread_handle_map.end())
			return false;
		handle = it->second;
	}

	return R_SUCCEEDED(svcSetThreadCoreMask(handle, preferred_core, static_cast<u32>(mask)));
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
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

	void* const key = (void*)pthread_self();
	{
		std::lock_guard<std::mutex> lock(s_thread_handle_map_mutex);
		s_thread_handle_map[key] = threadGetCurHandle();
	}

	(*entry.get())();

	{
		std::lock_guard<std::mutex> lock(s_thread_handle_map_mutex);
		s_thread_handle_map.erase(key);
	}
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already started thread");

	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
	if (res != 0)
		return false;

	m_native_handle = (void*)handle;
	func_clone.release();
	return true;
}

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach((pthread_t)m_native_handle);
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	void* retval;
	const int res = pthread_join((pthread_t)m_native_handle, &retval);
	if (res != 0)
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
