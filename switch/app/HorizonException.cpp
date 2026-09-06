// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonException.h"

#include "common/HostSys.h"
#include "common/Horizon/Horizon.h"
#include "common/Horizon/HorizonFastmem.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C"
{
	alignas(0x1000) u8 __nx_exception_stack[EXCEPTION_SLOT_COUNT][EXCEPTION_STACK_SIZE];
	u64 __nx_exception_stack_size = EXCEPTION_STACK_SIZE;
	alignas(EXCEPTION_SLOT_SIZE) u8 g_horizon_exception_slots[EXCEPTION_SLOT_COUNT][EXCEPTION_SLOT_SIZE];
	volatile u32 g_horizon_exception_slot_mask;
}

namespace
{
	struct ExceptionSlot
	{
		ThreadExceptionDump dump;
		u8 padding[SLOT_FRAME - sizeof(ThreadExceptionDump)];
		void* frame;
		u32 index;
		u8 tail[EXCEPTION_SLOT_SIZE - SLOT_INDEX - sizeof(u32)];
	};

	static_assert(sizeof(ExceptionSlot) == EXCEPTION_SLOT_SIZE);
	static_assert(offsetof(ExceptionSlot, frame) == SLOT_FRAME);
	static_assert(offsetof(ExceptionSlot, index) == SLOT_INDEX);

	static_assert(offsetof(ThreadExceptionDump, error_desc) == DUMP_ERROR_DESC);
	static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == DUMP_GPRS);
	static_assert(offsetof(ThreadExceptionDump, fp) == DUMP_FP);
	static_assert(offsetof(ThreadExceptionDump, lr) == DUMP_LR);
	static_assert(offsetof(ThreadExceptionDump, sp) == DUMP_SP);
	static_assert(offsetof(ThreadExceptionDump, pc) == DUMP_PC);
	static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == DUMP_FPU);
	static_assert(offsetof(ThreadExceptionDump, pstate) == DUMP_PSTATE);
	static_assert(offsetof(ThreadExceptionDump, esr) == DUMP_ESR);
	static_assert(offsetof(ThreadExceptionDump, far) == DUMP_FAR);

	static_assert(offsetof(ThreadExceptionFrameA64, lr) == FRAME_LR);
	static_assert(offsetof(ThreadExceptionFrameA64, sp) == FRAME_SP);
	static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == FRAME_PC);
	static_assert(offsetof(ThreadExceptionFrameA64, pstate) == FRAME_PSTATE);
	static_assert(offsetof(ThreadExceptionFrameA64, esr) == FRAME_ESR);
	static_assert(offsetof(ThreadExceptionFrameA64, far) == FRAME_FAR);

	constexpr u32 ESR_EC_DATA_ABORT_LOWER = 0x24;
	constexpr u32 ESR_EC_DATA_ABORT_SAME = 0x25;

	int s_report_fd = -1;

	const char* DescribeError(u32 error_desc)
	{
		switch (error_desc)
		{
			case ThreadExceptionDesc_InstructionAbort: return "Instruction abort";
			case ThreadExceptionDesc_MisalignedPC: return "Misaligned PC";
			case ThreadExceptionDesc_MisalignedSP: return "Misaligned SP";
			case ThreadExceptionDesc_SError: return "SError";
			case ThreadExceptionDesc_BadSVC: return "Bad SVC";
			case ThreadExceptionDesc_Trap: return "Trap";
			case ThreadExceptionDesc_Other: return "Data abort / other";
			default: return "Unknown";
		}
	}

	struct Report
	{
		char buffer[4096];
		size_t length = 0;

		void Append(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
		{
			if (length >= sizeof(buffer))
				return;

			va_list args;
			va_start(args, fmt);
			const int written = vsnprintf(buffer + length, sizeof(buffer) - length, fmt, args);
			va_end(args);

			if (written > 0)
			{
				length += static_cast<size_t>(written);
				if (length > sizeof(buffer))
					length = sizeof(buffer);
			}
		}
	};
} // namespace

bool HorizonFastmem::HasResumableFaultHandler()
{
	return true;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
	if (threadExceptionIsAArch64(ctx))
	{
		const u32 exception_class = (ctx->esr >> 26) & 0x3f;
		if (exception_class == ESR_EC_DATA_ABORT_LOWER || exception_class == ESR_EC_DATA_ABORT_SAME)
		{
			const uptr fault_address = static_cast<uptr>(ctx->far.x);

			if (HorizonFastmem::ResolveFault(fault_address))
				return;

			const bool is_write = (ctx->esr & (1u << 6)) != 0;
			if (PageFaultHandler::HandlePageFault(reinterpret_cast<void*>(ctx->pc.x),
					reinterpret_cast<void*>(fault_address), is_write) ==
				PageFaultHandler::HandlerResult::ContinueExecution)
			{
				return;
			}
		}
	}

	Report report;
	report.Append("\n===== ARMSX2-NX crash report =====\n");
	report.Append("tick=%016lx aarch64=%d\n", svcGetSystemTick(),
		threadExceptionIsAArch64(ctx) ? 1 : 0);
	report.Append("cause: %s (error_desc=0x%03x)\n", DescribeError(ctx->error_desc), ctx->error_desc);
	report.Append("pc =%016lx  lr =%016lx\n", ctx->pc.x, ctx->lr.x);
	report.Append("sp =%016lx  fp =%016lx\n", ctx->sp.x, ctx->fp.x);
	report.Append("far=%016lx  esr=%08x  pstate=%08x\n", ctx->far.x, ctx->esr, ctx->pstate);

	report.Append("handler@%016lx\n", reinterpret_cast<u64>(&__libnx_exception_handler));

	for (u32 i = 0; i < 29; i += 2)
	{
		if (i + 1 < 29)
			report.Append("x%-2u=%016lx  x%-2u=%016lx\n", i, ctx->cpu_gprs[i].x, i + 1, ctx->cpu_gprs[i + 1].x);
		else
			report.Append("x%-2u=%016lx\n", i, ctx->cpu_gprs[i].x);
	}
	report.Append("=======================================\n");

	svcOutputDebugString(report.buffer, report.length);

	if (s_report_fd >= 0)
	{
		write(s_report_fd, report.buffer, report.length);
		fsync(s_report_fd);
	}

	svcExitProcess();
}

namespace HorizonException
{
	void Initialize(const char* report_dir)
	{
		if (s_report_fd >= 0 || !report_dir)
			return;

		char path[768];
		std::snprintf(path, sizeof(path), "%s/crash.txt", report_dir);
		s_report_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);

		Horizon::BreadcrumbInit(report_dir);
		Breadcrumb("HorizonException::Initialize");
	}

	void Shutdown()
	{
		Breadcrumb("HorizonException::Shutdown");
		if (s_report_fd >= 0)
		{
			close(s_report_fd);
			s_report_fd = -1;
		}
		Horizon::BreadcrumbShutdown();
	}

	void Breadcrumb(const char* message)
	{
		Horizon::Breadcrumb(message);
	}
} // namespace HorizonException
