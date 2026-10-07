// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// Bookkeeping the Switch sampling profiler needs.
namespace Horizon::Profiling
{
	static constexpr std::size_t MAX_THREADS = 48;
	static constexpr std::size_t MAX_THREAD_NAME = 32;

	struct ThreadInfo
	{
		u32 handle = 0;
		u64 thread_id = 0;
		uptr stack_start = 0;
		uptr stack_end = 0;
		std::array<char, MAX_THREAD_NAME> name{};
	};

	void RegisterCurrentThread(const char* name);

	std::size_t SnapshotThreads(std::span<ThreadInfo> out);

	enum class JitSymbolKind : u8
	{
		Name,
		PC,
		Key,
	};

	struct JitSymbol
	{
		uptr start = 0;
		const char* group = nullptr;
		const char* name = nullptr;
		u64 value = 0;
		u32 size = 0;
		u32 sequence = 0;
		JitSymbolKind kind = JitSymbolKind::Name;
	};

	void AddJitSymbol(const void* start, std::size_t size, const char* group, JitSymbolKind kind, const char* name,
		u64 value);

	void SnapshotJitSymbols(std::vector<JitSymbol>& out);
} // namespace Horizon::Profiling
