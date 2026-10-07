// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/Horizon.h"

#include "Horizon/HorizonProfiler.h"

#include "BuildVersion.h"
#include "Common.h"
#include "Config.h"
#include "Counters.h"
#include "Host.h"
#include "Memory.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VU.h"
#include "vtlb.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Horizon/HorizonFastmem.h"
#include "common/Horizon/HorizonProfiling.h"
#include "common/Path.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <limits>
#include <malloc.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void _start();
extern "C" char __end__[];
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace
{
	using Horizon::Profiling::JitSymbol;
	using Horizon::Profiling::JitSymbolKind;
	using Horizon::Profiling::ThreadInfo;

	constexpr u64 SAMPLE_PERIOD_NS = 2'000'000;
	constexpr u64 BUCKET_NS = 500'000'000;
	constexpr int PAUSE_RETRIES = 8;
	constexpr u64 PAUSE_RETRY_NS = 1'000;
	constexpr u64 MAX_CONSECUTIVE_FAILURES = 16;

	constexpr s32 SAMPLER_PRIORITY = 0x20;

#ifdef ARMSX2_FRAME_POINTERS
	constexpr bool FRAME_POINTERS = true;
#else
	constexpr bool FRAME_POINTERS = false;
#endif

	constexpr std::size_t MAX_THREADS = Horizon::Profiling::MAX_THREADS;
	constexpr std::size_t MAX_FRAMES = FRAME_POINTERS ? 12 : 1;
	constexpr std::size_t MAX_SAMPLES = 384 * 1024;
	constexpr std::size_t MIN_SAMPLES = 4 * 1024;

	constexpr std::size_t MAX_LISTED_STACKS = 300;
	constexpr std::size_t MAX_LISTED_BLOCKS = 40;
	constexpr std::size_t MAX_LISTED_GUEST_PCS = 60;
	constexpr std::size_t MAX_DUMPED_BLOCKS = 16;
	constexpr std::size_t MAX_BLOCK_INSTRUCTIONS = 1024;
	constexpr std::size_t MAX_LISTED_UNKNOWN = 12;
	constexpr std::size_t MAX_LOGGED_LINES = 40;

	constexpr u64 NO_TICKS = std::numeric_limits<u64>::max();

	enum class Role : u8
	{
		Other,
		EE,
		VU,
	};

	struct Region
	{
		const char* name = "";
		const char* tag = "";
		uptr start = 0;
		uptr end = 0;
	};

	struct Sample
	{
		u64 pc = 0;
		u64 lr = 0;
		std::array<u64, MAX_FRAMES> frames{};
		u32 guest_pc = 0;
		u32 aux_pc = 0;
		u8 thread = 0;
		u8 depth = 0;
		u16 svc = 0;
		bool blocked = false;
	};

	struct ThreadRecord
	{
		ThreadInfo info;
		Role role = Role::Other;
		bool gone = false;
		u64 failures = 0;
		u64 consecutive_failures = 0;
		s32 priority = -1;
		u64 affinity = 0;
		u64 first_ticks = NO_TICKS;
		u64 first_wall = 0;
		u64 last_ticks = NO_TICKS;
		u64 last_wall = 0;
	};

	struct Bucket
	{
		u64 wall_ticks = 0;
		u64 ee_cycle = 0;
		u32 vsyncs = 0;
		float fps = 0.0f;
		float gpu_usage = 0.0f;
		std::array<u64, MAX_THREADS> thread_ticks{};
	};

	struct HotBlock
	{
		JitSymbol symbol;
		u64 self = 0;
		u64 inclusive = 0;
	};

	struct Capture
	{
		std::array<Region, 8> regions{};
		std::size_t region_count = 0;

		uptr module_start = 0;
		uptr module_end = 0;
		uptr code_start = 0;
		uptr code_end = 0;

		std::array<ThreadRecord, MAX_THREADS> threads{};
		std::size_t thread_count = 0;
		u32 sampler_handle = INVALID_HANDLE;

		std::unique_ptr<Sample[]> samples;
		std::size_t sample_capacity = 0;
		std::size_t sample_count = 0;
		u64 dropped_samples = 0;
		u64 rounds = 0;

		std::vector<Bucket> buckets;

		u64 sampler_ticks = NO_TICKS;
		Result first_error = 0;
		std::string first_error_thread;
		double seconds = 0.0;
		double elapsed = 0.0;
		std::string started_at;
		std::string description;
		std::string report_path;

		std::vector<HotBlock> blocks;
		std::unordered_map<u64, u32> block_of_address;
	};

	std::atomic<bool> s_running{false};
	std::mutex s_lifecycle_mutex;
	std::thread s_sampler_thread;
	Capture s_capture;

	u64 ReadThreadTicks(Handle handle)
	{
		const u32 info = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount : InfoType_ThreadTickCountDeprecated;
		u64 ticks = 0;
		if (R_FAILED(svcGetInfo(&ticks, info, handle, TickCountInfo_Total)))
			return NO_TICKS;
		return ticks;
	}

	template <typename T>
	T LoadRelaxed(const T& value)
	{
		return __atomic_load_n(&value, __ATOMIC_RELAXED);
	}

	bool InModule(u64 address)
	{
		return address >= s_capture.module_start && address < s_capture.module_end;
	}

	bool InCodeArena(u64 address)
	{
		return address >= s_capture.code_start && address < s_capture.code_end;
	}

	const Region* FindRegion(u64 address)
	{
		for (std::size_t i = 0; i < s_capture.region_count; ++i)
		{
			const Region& region = s_capture.regions[i];
			if (address >= region.start && address < region.end)
				return &region;
		}
		return nullptr;
	}

	bool IsInSyscall(u64 pc, u16* svc)
	{
		if (pc % 4 != 0 || !InModule(pc))
			return false;
		u32 instruction;
		std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
		if ((instruction & 0xffe0001f) != 0xd4000001)
			return false;
		*svc = static_cast<u16>((instruction >> 5) & 0xffff);
		return true;
	}

	u8 WalkFrames(const ThreadRecord& thread, const ThreadContext& ctx, std::array<u64, MAX_FRAMES>& frames)
	{
		if constexpr (!FRAME_POINTERS)
			return 0;

		const uptr stack_start = thread.info.stack_start;
		const uptr stack_end = thread.info.stack_end;
		if (stack_end <= stack_start)
			return 0;

		uptr fp = ctx.fp;
		uptr previous_fp = 0;
		u8 depth = 0;
		while (depth < MAX_FRAMES)
		{
			if (fp % 16 != 0 || fp < stack_start || fp + 16 > stack_end || fp <= previous_fp)
				break;

			u64 record[2];
			std::memcpy(record, reinterpret_cast<const void*>(fp), sizeof(record));
			const u64 return_address = record[1];
			const bool module = InModule(return_address);
			if (!module && !InCodeArena(return_address))
				break;

			frames[depth++] = return_address;
			if (!module)
				break;

			previous_fp = fp;
			fp = record[0];
		}
		return depth;
	}

	void NoteFailure(const ThreadRecord& thread, Result result)
	{
		if (s_capture.first_error != 0)
			return;
		s_capture.first_error = result;
		s_capture.first_error_thread = thread.info.name.data();
	}

	void TakeSample(std::size_t index)
	{
		ThreadRecord& thread = s_capture.threads[index];
		const Handle handle = thread.info.handle;

		const Result pause_result = svcSetThreadActivity(handle, ThreadActivity_Paused);
		if (R_FAILED(pause_result))
		{
			++thread.failures;
			if (R_VALUE(pause_result) == KERNELRESULT(InvalidHandle) ||
				++thread.consecutive_failures >= MAX_CONSECUTIVE_FAILURES)
				thread.gone = true;
			else
				NoteFailure(thread, pause_result);
			return;
		}

		ThreadContext ctx{};
		Result result = 0;
		for (int attempt = 0; attempt < PAUSE_RETRIES; ++attempt)
		{
			result = svcGetThreadContext3(&ctx, handle);
			if (R_SUCCEEDED(result))
				break;
			svcSleepThread(PAUSE_RETRY_NS);
		}

		Sample* const sample =
			s_capture.sample_count < s_capture.sample_capacity ? &s_capture.samples[s_capture.sample_count] : nullptr;
		if (R_SUCCEEDED(result) && sample)
		{
			sample->pc = ctx.pc.x;
			sample->lr = ctx.lr;
			sample->thread = static_cast<u8>(index);
			sample->blocked = IsInSyscall(ctx.pc.x, &sample->svc);
			sample->depth = WalkFrames(thread, ctx, sample->frames);
			switch (thread.role)
			{
				case Role::EE:
					sample->guest_pc = LoadRelaxed(cpuRegs.pc);
					sample->aux_pc = LoadRelaxed(psxRegs.pc);
					break;
				case Role::VU:
					sample->guest_pc = LoadRelaxed(vuRegs[1].VI[REG_TPC].UL);
					break;
				default:
					break;
			}
		}

		svcSetThreadActivity(handle, ThreadActivity_Runnable);

		if (R_FAILED(result))
		{
			++thread.failures;
			++thread.consecutive_failures;
			NoteFailure(thread, result);
			return;
		}

		thread.consecutive_failures = 0;
		if (sample)
			++s_capture.sample_count;
		else
			++s_capture.dropped_samples;
	}

	Role RoleForName(std::string_view name)
	{
		if (name == "CPU Thread")
			return Role::EE;
		if (name == "MTVU")
			return Role::VU;
		return Role::Other;
	}

	void RefreshThreads()
	{
		std::array<ThreadInfo, MAX_THREADS> snapshot;
		const std::size_t count = Horizon::Profiling::SnapshotThreads(snapshot);

		for (std::size_t i = 0; i < count; ++i)
		{
			const ThreadInfo& info = snapshot[i];
			u64 thread_id;
			if (info.handle == s_capture.sampler_handle || R_FAILED(svcGetThreadId(&thread_id, info.handle)) ||
				thread_id != info.thread_id)
				continue;

			auto* const end = s_capture.threads.begin() + s_capture.thread_count;
			auto* record =
				std::find_if(s_capture.threads.begin(), end, [&](const ThreadRecord& r) { return r.info.handle == info.handle; });
			if (record != end && record->gone)
				continue;
			if (record == end)
			{
				if (s_capture.thread_count == s_capture.threads.size())
					continue;
				record = &s_capture.threads[s_capture.thread_count++];
				svcGetThreadPriority(&record->priority, info.handle);
				s32 preferred_core;
				svcGetThreadCoreMask(&preferred_core, &record->affinity, info.handle);
			}

			record->info = info;
			record->role = RoleForName(info.name.data());
		}
	}

	void RecordBucket()
	{
		if (s_capture.buckets.size() == s_capture.buckets.capacity())
			return;

		Bucket& bucket = s_capture.buckets.emplace_back();
		bucket.wall_ticks = armGetSystemTick();
		bucket.ee_cycle = LoadRelaxed(cpuRegs.cycle);
		bucket.vsyncs = LoadRelaxed(g_FrameCount);
		bucket.fps = PerformanceMetrics::GetFPS();
		bucket.gpu_usage = PerformanceMetrics::GetGPUUsage();

		for (std::size_t i = 0; i < MAX_THREADS; ++i)
		{
			bucket.thread_ticks[i] = NO_TICKS;
			if (i >= s_capture.thread_count || s_capture.threads[i].gone)
				continue;

			ThreadRecord& thread = s_capture.threads[i];
			const u64 ticks = ReadThreadTicks(thread.info.handle);
			bucket.thread_ticks[i] = ticks;
			if (ticks == NO_TICKS)
				continue;

			if (thread.first_ticks == NO_TICKS)
			{
				thread.first_ticks = ticks;
				thread.first_wall = bucket.wall_ticks;
			}
			thread.last_ticks = ticks;
			thread.last_wall = bucket.wall_ticks;
		}
	}

	std::span<const Sample> Samples()
	{
		return {s_capture.samples.get(), s_capture.sample_count};
	}

	bool KeepLinkRegister(const Sample& sample)
	{
		return !InCodeArena(sample.pc) && (InModule(sample.lr) || InCodeArena(sample.lr));
	}

	std::string SymbolName(const JitSymbol& symbol)
	{
		const bool prefixed = symbol.group && symbol.group[0] != '\0';
		std::string name;
		switch (symbol.kind)
		{
			case JitSymbolKind::Name:
				name = prefixed ? fmt::format("{}_{}", symbol.group, symbol.name) : std::string(symbol.name);
				break;
			case JitSymbolKind::PC:
				name = prefixed ? fmt::format("{}_{:08X}", symbol.group, symbol.value) :
								  fmt::format("{:08X}", symbol.value);
				break;
			case JitSymbolKind::Key:
				name = prefixed ? fmt::format("{}_{}{:016X}", symbol.group, symbol.name, symbol.value) :
								  fmt::format("{}{:016X}", symbol.name, symbol.value);
				break;
		}
		std::replace(name.begin(), name.end(), ' ', '_');
		return name;
	}

	void ResolveBlocks()
	{
		std::vector<u64> addresses;
		for (const Sample& sample : Samples())
		{
			if (InCodeArena(sample.pc))
				addresses.push_back(sample.pc);
			if (KeepLinkRegister(sample) && InCodeArena(sample.lr))
				addresses.push_back(sample.lr);
			for (u8 i = 0; i < sample.depth; ++i)
			{
				if (InCodeArena(sample.frames[i]))
					addresses.push_back(sample.frames[i]);
			}
		}
		if (addresses.empty())
			return;
		std::sort(addresses.begin(), addresses.end());
		addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

		std::vector<JitSymbol> symbols;
		Horizon::Profiling::SnapshotJitSymbols(symbols);

		std::vector<bool> claimed(addresses.size(), false);
		for (auto it = symbols.rbegin(); it != symbols.rend(); ++it)
		{
			const JitSymbol& symbol = *it;
			auto pos = std::lower_bound(addresses.begin(), addresses.end(), symbol.start);
			s64 block_index = -1;
			for (; pos != addresses.end() && *pos < symbol.start + symbol.size; ++pos)
			{
				const std::size_t index = static_cast<std::size_t>(pos - addresses.begin());
				if (claimed[index])
					continue;
				claimed[index] = true;
				if (block_index < 0)
				{
					block_index = static_cast<s64>(s_capture.blocks.size());
					s_capture.blocks.push_back({symbol});
				}
				s_capture.block_of_address[*pos] = static_cast<u32>(block_index);
			}
		}

		for (const Sample& sample : Samples())
		{
			std::set<u32> on_stack;
			const auto note = [&](u64 address, bool self) {
				const auto it = s_capture.block_of_address.find(address);
				if (it == s_capture.block_of_address.end())
					return;
				if (self)
					++s_capture.blocks[it->second].self;
				on_stack.insert(it->second);
			};
			note(sample.pc, true);
			if (KeepLinkRegister(sample))
				note(sample.lr, false);
			for (u8 i = 0; i < sample.depth; ++i)
				note(sample.frames[i], false);
			for (const u32 index : on_stack)
				++s_capture.blocks[index].inclusive;
		}
	}

	std::string_view SvcName(u16 svc)
	{
		switch (svc)
		{
			case 0x0b: return "SleepThread";
			case 0x18: return "WaitSynchronization";
			case 0x1a: return "ArbitrateLock";
			case 0x1c: return "WaitProcessWideKeyAtomic";
			case 0x21: return "SendSyncRequest";
			case 0x22: return "SendSyncRequestWithUserBuffer";
			case 0x34: return "WaitForAddress";
			case 0x43: return "ReplyAndReceive";
			default: return "";
		}
	}

	std::string Label(u64 address)
	{
		if (InCodeArena(address))
		{
			const auto it = s_capture.block_of_address.find(address);
			if (it != s_capture.block_of_address.end())
				return "j:" + SymbolName(s_capture.blocks[it->second].symbol);
			if (const Region* region = FindRegion(address))
				return fmt::format("c:{}+{:x}", region->tag, address - region->start);
			return fmt::format("c:{:x}", address - s_capture.code_start);
		}
		if (InModule(address))
			return fmt::format("m:{:x}", address - s_capture.module_start);
		return fmt::format("?:{:x}", address);
	}

	std::string_view Where(const Sample& sample)
	{
		if (sample.blocked)
			return "blocked in the kernel";
		if (const Region* region = FindRegion(sample.pc))
			return region->name;
		if (InCodeArena(sample.pc))
			return "code arena (unassigned)";
		if (InModule(sample.pc))
			return "C++ (module)";
		return "unattributed";
	}

	double TicksToSeconds(u64 ticks)
	{
		return static_cast<double>(armTicksToNs(ticks)) / 1e9;
	}

	double CpuShare(const ThreadRecord& thread)
	{
		if (thread.first_ticks == NO_TICKS || thread.last_wall <= thread.first_wall)
			return -1.0;
		return static_cast<double>(thread.last_ticks - thread.first_ticks) /
			   static_cast<double>(thread.last_wall - thread.first_wall);
	}

	void AppendSummary(std::string& out, const std::vector<std::size_t>& order, const std::vector<u64>& sample_counts,
		const std::vector<u64>& blocked_counts)
	{
		out += fmt::format("ARMSX2-NX profile started {}: {:.2f} s, {} sampling rounds (~{} ms apart), {} samples, "
						   "{} dropped (buffer holds {})\n",
			s_capture.started_at, s_capture.elapsed, s_capture.rounds, SAMPLE_PERIOD_NS / 1'000'000,
			s_capture.sample_count, s_capture.dropped_samples, s_capture.sample_capacity);
		out += fmt::format("build {} ({})\n", BuildVersion::GitRev, BuildVersion::GitHash);
		out += s_capture.description;
		out += fmt::format("module base {:#x} size {:#x}, code arena {:#x} size {:#x}\n", s_capture.module_start,
			s_capture.module_end - s_capture.module_start, s_capture.code_start,
			s_capture.code_end - s_capture.code_start);
		out += fmt::format("frame pointers: {}\n", FRAME_POINTERS ? "on" : "off (stacks are pc + link register only)");
		if (s_capture.first_error != 0)
		{
			out += fmt::format("First sampling failure was {:#x} (module {}, description {}) on thread \"{}\".\n",
				s_capture.first_error, R_MODULE(s_capture.first_error), R_DESCRIPTION(s_capture.first_error),
				s_capture.first_error_thread);
		}

		if (s_capture.buckets.size() >= 2)
		{
			const Bucket& first = s_capture.buckets.front();
			const Bucket& last = s_capture.buckets.back();
			const double wall = TicksToSeconds(last.wall_ticks - first.wall_ticks);
			const double emulated = static_cast<double>(last.ee_cycle - first.ee_cycle) / PS2CLK;
			if (wall > 0.0)
			{
				out += fmt::format("emulation speed {:.1f}% ({:.2f} s emulated), {:.2f} vsyncs/s\n",
					100.0 * emulated / wall, emulated, static_cast<u32>(last.vsyncs - first.vsyncs) / wall);
				if (s_capture.sampler_ticks != NO_TICKS)
				{
					out += fmt::format("profiler's own cost: {:.1f}% of a core\n",
						100.0 * TicksToSeconds(s_capture.sampler_ticks) / wall);
				}
			}
		}

		out += "\nThreads. cpu is the kernel's own accounting of time spent on a core, 100% being one core. running "
			   "and blocked split the samples by whether the thread was waiting in the kernel.\n";
		out += "     cpu  samples  running  blocked  prio  mask  name\n";
		for (const std::size_t i : order)
		{
			const ThreadRecord& thread = s_capture.threads[i];
			const double cpu = CpuShare(thread);
			const u64 samples = sample_counts[i];
			const double blocked = samples != 0 ? 100.0 * blocked_counts[i] / samples : 0.0;
			out += fmt::format("  {:>6}  {:7}  {:6.1f}%  {:6.1f}%  {:#4x}  {:#4x}  {}{}\n",
				cpu < 0.0 ? "n/a" : fmt::format("{:.1f}%", 100.0 * cpu), samples, samples != 0 ? 100.0 - blocked : 0.0,
				blocked, thread.priority, thread.affinity, thread.info.name.data(), thread.gone ? " (exited)" : "");
		}
	}

	void AppendTimeline(std::string& out, const std::vector<std::size_t>& order)
	{
		if (s_capture.buckets.size() < 2)
			return;

		out += fmt::format("\nTimeline, {} ms per row. speed is emulated EE time over wall time, vps is emulated "
						   "vsyncs per second, fps and gpu are the OSD's own running values, the rest are each "
						   "thread's share of a core:\n",
			BUCKET_NS / 1'000'000);
		for (std::size_t column = 0; column < order.size(); ++column)
			out += fmt::format("  T{} = {}\n", column, s_capture.threads[order[column]].info.name.data());

		out += "       t  speed    vps    fps    gpu";
		for (std::size_t column = 0; column < order.size(); ++column)
			out += fmt::format("  {:>5}", fmt::format("T{}", column));
		out += "\n";

		const Bucket& origin = s_capture.buckets.front();
		for (std::size_t b = 1; b < s_capture.buckets.size(); ++b)
		{
			const Bucket& previous = s_capture.buckets[b - 1];
			const Bucket& bucket = s_capture.buckets[b];
			const u64 wall_ticks = bucket.wall_ticks - previous.wall_ticks;
			const double wall = TicksToSeconds(wall_ticks);
			if (wall <= 0.0)
				continue;

			const double speed = static_cast<double>(bucket.ee_cycle - previous.ee_cycle) / PS2CLK / wall;
			out += fmt::format("  {:6.2f}  {:4.0f}%  {:5.1f}  {:5.1f}  {:4.0f}%",
				TicksToSeconds(bucket.wall_ticks - origin.wall_ticks), 100.0 * speed,
				static_cast<u32>(bucket.vsyncs - previous.vsyncs) / wall, bucket.fps, bucket.gpu_usage);
			for (const std::size_t i : order)
			{
				const u64 before = previous.thread_ticks[i];
				const u64 after = bucket.thread_ticks[i];
				if (before == NO_TICKS || after == NO_TICKS || after < before)
					out += "      -";
				else
					out += fmt::format("  {:4.0f}%", 100.0 * (after - before) / wall_ticks);
			}
			out += "\n";
		}
	}

	template <typename Key>
	std::vector<std::pair<Key, u64>> RankByCount(const std::map<Key, u64>& counts)
	{
		std::vector<std::pair<Key, u64>> ranked(counts.begin(), counts.end());
		std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		return ranked;
	}

	void AppendGuestPCs(std::string& out, std::string_view title, const std::map<u32, u64>& pcs, double percent)
	{
		if (pcs.empty())
			return;
		const auto ranked = RankByCount(pcs);
		out += fmt::format("{} ({} distinct, top {} shown).\n", title, ranked.size(),
			std::min(ranked.size(), MAX_LISTED_GUEST_PCS));
		for (std::size_t i = 0; i < ranked.size() && i < MAX_LISTED_GUEST_PCS; ++i)
			out += fmt::format("  {:5.2f}%  {:7}  {:08x}\n", ranked[i].second * percent, ranked[i].second, ranked[i].first);
	}

	void AppendThread(std::string& out, std::size_t index, std::size_t column)
	{
		const ThreadRecord& thread = s_capture.threads[index];

		u64 total = 0;
		std::map<std::string_view, u64> where;
		std::map<u16, u64> blocked_in;
		std::map<std::string, u64> self_pcs;
		std::map<std::string, u64> stacks;
		std::map<u32, u64> guest_pcs;
		std::map<u32, u64> aux_pcs;
		std::map<u32, std::pair<u64, u64>> blocks;

		for (const Sample& sample : Samples())
		{
			if (sample.thread != index)
				continue;
			++total;
			++where[Where(sample)];
			if (sample.blocked)
				++blocked_in[sample.svc];
			if (thread.role != Role::Other)
				++guest_pcs[sample.guest_pc];
			if (thread.role == Role::EE)
				++aux_pcs[sample.aux_pc];

			std::string label = Label(sample.pc);
			std::string key = sample.blocked ? fmt::format("svc:{:x}", sample.svc) : "run";
			key += ' ';
			key += label;
			if (!sample.blocked)
				++self_pcs[std::move(label)];
			if (KeepLinkRegister(sample))
				key += " L" + Label(sample.lr);
			for (u8 i = 0; i < sample.depth; ++i)
				key += ' ' + Label(sample.frames[i]);
			++stacks[std::move(key)];

			std::set<u32> on_stack;
			const auto note = [&](u64 address, bool self) {
				const auto it = s_capture.block_of_address.find(address);
				if (it == s_capture.block_of_address.end())
					return;
				if (self)
					++blocks[it->second].first;
				on_stack.insert(it->second);
			};
			note(sample.pc, true);
			if (KeepLinkRegister(sample))
				note(sample.lr, false);
			for (u8 i = 0; i < sample.depth; ++i)
				note(sample.frames[i], false);
			for (const u32 block : on_stack)
				++blocks[block].second;
		}

		out += fmt::format("\n== Thread T{}: {} ==\n", column, thread.info.name.data());
		if (total == 0)
		{
			out += fmt::format("No samples ({} failed).\n", thread.failures);
			return;
		}
		const double percent = 100.0 / static_cast<double>(total);

		out += fmt::format("Where it was ({} samples):\n", total);
		for (const auto& [name, count] : RankByCount(where))
			out += fmt::format("  {:5.1f}%  {:7}  {}\n", count * percent, count, name);

		if (!blocked_in.empty())
		{
			out += "Blocked in:\n";
			for (const auto& [svc, count] : blocked_in)
				out += fmt::format("  {:5.1f}%  {:7}  svc {:#04x} {}\n", count * percent, count, svc, SvcName(svc));
		}

		if (!blocks.empty())
		{
			std::vector<std::pair<u32, std::pair<u64, u64>>> ranked(blocks.begin(), blocks.end());
			std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
				return a.second.first != b.second.first ? a.second.first > b.second.first :
														  a.second.second > b.second.second;
			});
			out += fmt::format("Hot recompiled code ({} blocks, top {} shown). self is the pc inside the block.\n",
				ranked.size(), std::min(ranked.size(), MAX_LISTED_BLOCKS));
			out += "     self  inclusive  host bytes  block\n";
			for (std::size_t i = 0; i < ranked.size() && i < MAX_LISTED_BLOCKS; ++i)
			{
				const JitSymbol& symbol = s_capture.blocks[ranked[i].first].symbol;
				const Region* region = FindRegion(symbol.start);
				out += fmt::format("  {:6.2f}%    {:6.2f}%  {:10}  {} @{}+{:x}\n", ranked[i].second.first * percent,
					ranked[i].second.second * percent, symbol.size, SymbolName(symbol), region ? region->tag : "?",
					symbol.start - (region ? region->start : s_capture.code_start));
			}
		}

		const auto ranked_self = RankByCount(self_pcs);
		out += fmt::format("Self PCs while running ({} distinct). m: module offset, j: "
						   "recompiled block, c: unregistered recompiler code, ?: anything else.\n",
			ranked_self.size());
		for (const auto& [label, count] : ranked_self)
			out += fmt::format("  {:5.2f}%  {:7}  {}\n", count * percent, count, label);

		const auto ranked_stacks = RankByCount(stacks);
		out += fmt::format("Stacks ({} distinct, top {} shown). Innermost frame first. L marks the link register, "
						   "which is the caller only while the sampled function is a leaf.\n",
			ranked_stacks.size(), std::min(ranked_stacks.size(), MAX_LISTED_STACKS));
		for (std::size_t i = 0; i < ranked_stacks.size() && i < MAX_LISTED_STACKS; ++i)
			out += fmt::format("  {} {}\n", ranked_stacks[i].second, ranked_stacks[i].first);

		if (thread.role == Role::EE)
		{
			AppendGuestPCs(out, "EE PC at the last block boundary", guest_pcs, percent);
			AppendGuestPCs(out, "IOP PC at the last block boundary", aux_pcs, percent);
		}
		else if (thread.role == Role::VU)
		{
			AppendGuestPCs(out, "VU1 TPC", guest_pcs, percent);
		}
	}

	void AppendBlockDumps(std::string& out)
	{
		std::vector<const HotBlock*> ranked;
		for (const HotBlock& block : s_capture.blocks)
		{
			if (block.self != 0)
				ranked.push_back(&block);
		}
		if (ranked.empty())
			return;
		std::sort(ranked.begin(), ranked.end(), [](const HotBlock* a, const HotBlock* b) { return a->self > b->self; });
		if (ranked.size() > MAX_DUMPED_BLOCKS)
			ranked.resize(MAX_DUMPED_BLOCKS);

		std::unordered_map<u64, u64> self_by_address;
		for (const Sample& sample : Samples())
		{
			if (!sample.blocked && InCodeArena(sample.pc))
				++self_by_address[sample.pc];
		}

		out += "\nHottest recompiled blocks as AArch64 encodings, read back after the capture. "
		       "Columns are offset, self samples and encoding.\n";
		for (const HotBlock* block : ranked)
		{
			const JitSymbol& symbol = block->symbol;
			const uptr start = symbol.start & ~uptr{3};
			const uptr end = std::min<uptr>({symbol.start + symbol.size, start + MAX_BLOCK_INSTRUCTIONS * 4,
				s_capture.code_end});
			out += fmt::format("\nBlock {}, {} self samples, {} host bytes{}.\n", SymbolName(symbol), block->self,
				symbol.size, symbol.size > MAX_BLOCK_INSTRUCTIONS * 4 ? " (truncated)" : "");
			for (uptr pc = start; pc + 4 <= end; pc += 4)
			{
				u32 encoding;
				std::memcpy(&encoding, reinterpret_cast<const void*>(pc), sizeof(encoding));
				const auto count = self_by_address.find(pc);
				out += fmt::format("  +{:#06x}  {:7}  {:08x}\n", pc - start,
					count == self_by_address.end() ? 0 : count->second, encoding);
			}
		}
	}

	void AppendUnknownCode(std::string& out)
	{
		std::map<u64, u64> pages;
		for (const Sample& sample : Samples())
		{
			if (!sample.blocked && !InModule(sample.pc) && !InCodeArena(sample.pc))
				++pages[sample.pc & ~u64{0xfff}];
		}
		if (pages.empty())
			return;

		const auto ranked = RankByCount(pages);
		out += "\nUnattributed code pages:\n";
		for (std::size_t i = 0; i < ranked.size() && i < MAX_LISTED_UNKNOWN; ++i)
		{
			const auto& [page, count] = ranked[i];
			MemoryInfo info{};
			u32 page_info;
			if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, page)))
			{
				out += fmt::format("  {:7}  {:#x}  in block {:#x}+{:#x}, type {:#x}, perm {:#x}\n", count, page, info.addr,
					info.size, info.type & MemState_Type, static_cast<u32>(info.perm));
			}
			else
			{
				out += fmt::format("  {:7}  {:#x}\n", count, page);
			}
		}
	}

	std::string BuildReport()
	{
		std::vector<u64> sample_counts(s_capture.thread_count);
		std::vector<u64> blocked_counts(s_capture.thread_count);
		for (const Sample& sample : Samples())
		{
			++sample_counts[sample.thread];
			blocked_counts[sample.thread] += sample.blocked ? 1 : 0;
		}

		std::vector<std::size_t> order;
		for (std::size_t i = 0; i < s_capture.thread_count; ++i)
		{
			if (sample_counts[i] != 0 || CpuShare(s_capture.threads[i]) > 0.0)
				order.push_back(i);
		}
		std::stable_sort(order.begin(), order.end(), [](std::size_t a, std::size_t b) {
			return CpuShare(s_capture.threads[a]) > CpuShare(s_capture.threads[b]);
		});

		std::string out;
		AppendSummary(out, order, sample_counts, blocked_counts);
		AppendTimeline(out, order);
		for (std::size_t column = 0; column < order.size(); ++column)
			AppendThread(out, order[column], column);
		AppendBlockDumps(out);
		AppendUnknownCode(out);
		return out;
	}

	void WriteReport()
	{
		ResolveBlocks();
		const std::string report = BuildReport();

		std::size_t offset = 0;
		for (std::size_t line = 0; offset < report.size() && line < MAX_LOGGED_LINES; ++line)
		{
			const std::size_t newline = report.find('\n', offset);
			const std::size_t end = newline == std::string::npos ? report.size() : newline;
			Console.WriteLn("Profile: %.*s", static_cast<int>(end - offset), report.data() + offset);
			offset = end + 1;
		}

		if (FileSystem::WriteStringToFile(s_capture.report_path.c_str(), report))
		{
			Console.WriteLn("Profile: full report written to %s", s_capture.report_path.c_str());
			Host::AddIconOSDMessage("HorizonProfiler", ICON_FA_STOPWATCH,
				fmt::format("Profile written to {}", Path::GetFileName(s_capture.report_path)), Host::OSD_INFO_DURATION);
		}
		else
		{
			Console.Error("Profile: could not write %s", s_capture.report_path.c_str());
			Host::AddIconOSDMessage("HorizonProfiler", ICON_FA_TRIANGLE_EXCLAMATION,
				"Profile captured, but the report could not be written.", Host::OSD_ERROR_DURATION);
		}

		s_capture.samples.reset();
		s_capture.sample_capacity = 0;
		s_capture.blocks = {};
		s_capture.block_of_address = {};
		s_capture.buckets = {};
	}

	void SamplerThread()
	{
		s_capture.sampler_handle = threadGetCurHandle();
		if (!Horizon::PinCallingThreadToCore3())
			Horizon::ReserveCore3ForCallingThread();
		svcSetThreadPriority(CUR_THREAD_HANDLE, SAMPLER_PRIORITY);

		const u64 own_start = ReadThreadTicks(s_capture.sampler_handle);
		const u64 start = armGetSystemTick();
		const u64 bucket_ticks = armNsToTicks(BUCKET_NS);
		u64 next_bucket = start + bucket_ticks;
		u32 rng = 0x2545f491;

		RefreshThreads();
		RecordBucket();

		while (s_running.load(std::memory_order_relaxed))
		{
			const u64 now = armGetSystemTick();
			s_capture.elapsed = TicksToSeconds(now - start);
			if (s_capture.elapsed >= s_capture.seconds)
				break;

			if (now >= next_bucket)
			{
				RefreshThreads();
				RecordBucket();
				next_bucket += bucket_ticks;
			}

			++s_capture.rounds;
			for (std::size_t i = 0; i < s_capture.thread_count; ++i)
			{
				if (!s_capture.threads[i].gone)
					TakeSample(i);
			}

			if (s_capture.sample_count == 0 && s_capture.rounds >= 32)
				break;

			rng = rng * 1664525u + 1013904223u;
			svcSleepThread(SAMPLE_PERIOD_NS / 2 + rng % SAMPLE_PERIOD_NS);
		}

		RecordBucket();

		const u64 own_end = ReadThreadTicks(s_capture.sampler_handle);
		if (own_start != NO_TICKS && own_end != NO_TICKS)
			s_capture.sampler_ticks = own_end - own_start;

		WriteReport();
		s_running.store(false, std::memory_order_relaxed);
	}

	void JoinSampler()
	{
		s_running.store(false, std::memory_order_relaxed);
		if (s_sampler_thread.joinable())
			s_sampler_thread.join();
	}

	std::string OnOff(bool value)
	{
		return value ? "on" : "off";
	}

	std::string DescribeEmulation()
	{
		const std::string serial = VMManager::GetDiscSerial();
		std::string out = fmt::format("game \"{}\" serial {} crc {:08X}\n", VMManager::GetTitle(true),
			serial.empty() ? "(none)" : serial, VMManager::GetDiscCRC());

		const Pcsx2Config::GSOptions& gs = EmuConfig.GS;
		out += fmt::format("gs: renderer {}, upscale {:.2f}x, blending {}, texture preload {}, hw download {}, back "
						   "thread mode {}\n",
			Pcsx2Config::GSOptions::GetRendererName(gs.Renderer), gs.UpscaleMultiplier,
			static_cast<int>(gs.AccurateBlendingUnit), static_cast<int>(gs.TexturePreloading),
			static_cast<int>(gs.HWDownloadMode), static_cast<int>(gs.BackThreadMode));
		out += fmt::format("cpu: MTVU {}, EE cycle rate {}, EE cycle skip {}, thread pinning {}, fastmem {} (area {}, {})\n",
			OnOff(EmuConfig.Speedhacks.vuThread), EmuConfig.Speedhacks.EECycleRate, EmuConfig.Speedhacks.EECycleSkip,
			OnOff(EmuConfig.EnableThreadPinning), OnOff(EmuConfig.Cpu.Recompiler.EnableFastmem),
			vtlb_FastmemAreaUnavailable() ? "unavailable" : "mapped", HorizonFastmem::GetSupportReason());
		out += fmt::format("process core mask {:#x}\n", Horizon::GetProcessCoreMask());
		return out;
	}

	std::string ReportPath(const std::tm* time)
	{
		std::string name = "profile";
		std::string serial = VMManager::GetDiscSerial();
		std::erase_if(serial, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); });
		if (!serial.empty())
			name += '-' + serial;
		if (time)
		{
			char stamp[32];
			std::strftime(stamp, sizeof(stamp), "-%Y%m%d-%H%M%S", time);
			name += stamp;
		}
		return Path::Combine(EmuFolders::Logs, name + ".txt");
	}

	std::size_t FreeHeapBytes()
	{
		const struct mallinfo info = mallinfo();
		const std::size_t heap = static_cast<std::size_t>(fake_heap_end - fake_heap_start);
		const std::size_t used = std::min<std::size_t>(heap, info.arena);
		return heap - used + info.fordblks;
	}

	bool AllocateSamples(std::size_t wanted)
	{
		const std::size_t budget = FreeHeapBytes() / 4 / sizeof(Sample);
		for (std::size_t count = std::min({wanted, MAX_SAMPLES, budget}); count >= MIN_SAMPLES; count /= 2)
		{
			s_capture.samples.reset(new (std::nothrow) Sample[count]);
			if (s_capture.samples)
			{
				s_capture.sample_capacity = count;
				return true;
			}
		}
		return false;
	}

	void AddRegion(const char* name, const char* tag, std::size_t offset, std::size_t size)
	{
		u8* const start = SysMemory::GetCodePtr(offset);
		if (!start)
			return;
		s_capture.regions[s_capture.region_count++] = {
			name, tag, reinterpret_cast<uptr>(start), reinterpret_cast<uptr>(start) + size};
	}
} // namespace

bool HorizonProfiler::IsRunning()
{
	return s_running.load(std::memory_order_relaxed);
}

void HorizonProfiler::Start(float seconds)
{
	std::lock_guard lock(s_lifecycle_mutex);
	if (IsRunning())
		return;
	JoinSampler();

	std::array<ThreadInfo, MAX_THREADS> threads;
	const std::size_t thread_count = Horizon::Profiling::SnapshotThreads(threads);
	const bool has_cpu_thread = std::any_of(threads.begin(), threads.begin() + thread_count,
		[](const ThreadInfo& info) { return RoleForName(info.name.data()) == Role::EE; });
	if (!VMManager::HasValidVM() || !has_cpu_thread)
	{
		Host::AddIconOSDMessage("HorizonProfiler", ICON_FA_STOPWATCH, "Nothing to profile. No game is running.",
			Host::OSD_INFO_DURATION);
		return;
	}

	s_capture = Capture{};
	s_capture.seconds = seconds;
	s_capture.module_start = reinterpret_cast<uptr>(&_start);
	s_capture.module_end = reinterpret_cast<uptr>(__end__);

	using namespace HostMemoryMap;
	if (u8* const code = SysMemory::GetCodePtr(0))
	{
		s_capture.code_start = reinterpret_cast<uptr>(code);
		s_capture.code_end = s_capture.code_start + CodeSize;
	}
	AddRegion("EE recompiler", "EE", EErecOffset, EErecSize);
	AddRegion("IOP recompiler", "IOP", IOPrecOffset, IOPrecSize);
	AddRegion("VIF0 dynarec", "VIF0", VIF0recOffset, VIF0recSize);
	AddRegion("VIF1 dynarec", "VIF1", VIF1recOffset, VIF1recSize);
	AddRegion("microVU0", "VU0", mVU0recOffset, mVU0recSize);
	AddRegion("microVU1", "VU1", mVU1recOffset, mVU1recSize);
	AddRegion("VIF unpack", "VIFU", VIFUnpackRecOffset, VIFUnpackRecSize);
	AddRegion("GS software JIT", "SW", SWrecOffset, SWrecSize);

	std::time_t now = std::time(nullptr);
	std::tm local{};
	const bool have_time = localtime_r(&now, &local) != nullptr;
	char started_at[32] = "at an unknown time";
	if (have_time)
		std::strftime(started_at, sizeof(started_at), "%Y-%m-%d %H:%M:%S", &local);
	s_capture.started_at = started_at;
	s_capture.report_path = ReportPath(have_time ? &local : nullptr);
	s_capture.description = DescribeEmulation();

	const double rounds = seconds * 1e9 / SAMPLE_PERIOD_NS;
	if (!AllocateSamples(static_cast<std::size_t>(rounds * static_cast<double>(thread_count + 2))))
	{
		s_capture = Capture{};
		Host::AddIconOSDMessage("HorizonProfiler", ICON_FA_TRIANGLE_EXCLAMATION, "Not enough free memory to profile.",
			Host::OSD_ERROR_DURATION);
		return;
	}
	s_capture.buckets.reserve(static_cast<std::size_t>(seconds * 1e9 / BUCKET_NS) + 8);

	s_running.store(true, std::memory_order_relaxed);
	s_sampler_thread = std::thread(SamplerThread);

	Host::AddIconOSDMessage("HorizonProfiler", ICON_FA_STOPWATCH, fmt::format("Profiling for {:.0f} s...", seconds),
		Host::OSD_INFO_DURATION);
}

void HorizonProfiler::RequestStop()
{
	s_running.store(false, std::memory_order_relaxed);
}

void HorizonProfiler::Shutdown()
{
	std::lock_guard lock(s_lifecycle_mutex);
	JoinSampler();
}
