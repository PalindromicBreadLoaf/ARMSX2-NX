// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonException.h"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C"
{
	alignas(0x1000) u8 __nx_exception_stack[0x1000];
	u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

namespace
{
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

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
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
	}

	void Shutdown()
	{
		if (s_report_fd >= 0)
		{
			close(s_report_fd);
			s_report_fd = -1;
		}
	}
} // namespace HorizonException
