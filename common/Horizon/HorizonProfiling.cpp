// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/Horizon.h"
#include "common/Horizon/HorizonProfiling.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>

namespace
{
	std::mutex s_thread_mutex;
	std::array<Horizon::Profiling::ThreadInfo, Horizon::Profiling::MAX_THREADS> s_threads;
	std::size_t s_thread_count = 0;

	constexpr std::size_t JIT_SYMBOL_CAPACITY = 64 * 1024;

	std::mutex s_jit_mutex;
	std::unique_ptr<Horizon::Profiling::JitSymbol[]> s_jit_symbols;
	bool s_jit_alloc_failed = false;
	u32 s_jit_sequence = 0;
	std::set<std::string, std::less<>> s_jit_names;

	bool IsAlive(Handle handle)
	{
		u64 id;
		return R_SUCCEEDED(svcGetThreadId(&id, handle));
	}

	const char* Intern(const char* name)
	{
		if (!name)
			return "";
		const auto it = s_jit_names.find(std::string_view(name));
		if (it != s_jit_names.end())
			return it->c_str();
		return s_jit_names.emplace(name).first->c_str();
	}
} // namespace

void Horizon::Profiling::RegisterCurrentThread(const char* name)
{
	const Handle handle = threadGetCurHandle();
	u64 thread_id = 0;
	svcGetThreadId(&thread_id, handle);

	MemoryInfo stack{};
	u32 page_info;
	const u8 local = 0;
	if (R_FAILED(svcQueryMemory(&stack, &page_info, reinterpret_cast<uptr>(&local))))
		stack = {};

	std::lock_guard lock(s_thread_mutex);

	auto* const end = s_threads.begin() + s_thread_count;
	auto* slot = std::find_if(s_threads.begin(), end, [&](const ThreadInfo& info) { return info.handle == handle; });
	if (slot == end)
	{
		if (s_thread_count < s_threads.size())
		{
			++s_thread_count;
		}
		else
		{
			slot = std::find_if(s_threads.begin(), end, [](const ThreadInfo& info) { return !IsAlive(info.handle); });
			if (slot == end)
				return;
		}
	}

	slot->handle = handle;
	slot->thread_id = thread_id;
	slot->stack_start = stack.addr;
	slot->stack_end = stack.addr + stack.size;
	slot->name.fill('\0');
	std::strncpy(slot->name.data(), name ? name : "", slot->name.size() - 1);
}

std::size_t Horizon::Profiling::SnapshotThreads(std::span<ThreadInfo> out)
{
	std::lock_guard lock(s_thread_mutex);
	const std::size_t count = std::min(out.size(), s_thread_count);
	std::copy_n(s_threads.begin(), count, out.begin());
	return count;
}

void Horizon::Profiling::AddJitSymbol(const void* start, std::size_t size, const char* group, JitSymbolKind kind,
	const char* name, u64 value)
{
	if (!start || size == 0)
		return;

	std::lock_guard lock(s_jit_mutex);
	if (!s_jit_symbols)
	{
		if (s_jit_alloc_failed)
			return;
		s_jit_symbols.reset(new (std::nothrow) JitSymbol[JIT_SYMBOL_CAPACITY]);
		if (!s_jit_symbols)
		{
			s_jit_alloc_failed = true;
			return;
		}
	}

	JitSymbol& symbol = s_jit_symbols[s_jit_sequence % JIT_SYMBOL_CAPACITY];
	symbol.start = reinterpret_cast<uptr>(start);
	symbol.size = static_cast<u32>(std::min<std::size_t>(size, UINT32_MAX));
	symbol.group = group ? group : "";
	symbol.kind = kind;
	symbol.name = kind == JitSymbolKind::PC ? "" : Intern(name);
	symbol.value = value;
	symbol.sequence = s_jit_sequence++;
}

void Horizon::Profiling::SnapshotJitSymbols(std::vector<JitSymbol>& out)
{
	std::lock_guard lock(s_jit_mutex);
	out.clear();
	if (!s_jit_symbols)
		return;

	const u32 count = static_cast<u32>(std::min<std::size_t>(s_jit_sequence, JIT_SYMBOL_CAPACITY));
	out.reserve(count);
	for (u32 i = s_jit_sequence - count; i != s_jit_sequence; ++i)
		out.push_back(s_jit_symbols[i % JIT_SYMBOL_CAPACITY]);
}
