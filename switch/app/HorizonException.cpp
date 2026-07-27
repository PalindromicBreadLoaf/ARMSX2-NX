// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonException.h"

#include "common/HostSys.h"
#include "common/Horizon/Horizon.h"
#include "common/Horizon/HorizonFastmem.h"

#include <cstddef>
#include <cstring>

extern "C"
{
	alignas(0x1000) u8 __nx_exception_stack[EXCEPTION_SLOT_COUNT][EXCEPTION_STACK_SIZE];
	u64 __nx_exception_stack_size = EXCEPTION_STACK_SIZE;
	alignas(EXCEPTION_SLOT_SIZE) u8 g_horizon_exception_slots[EXCEPTION_SLOT_COUNT][EXCEPTION_SLOT_SIZE];
	volatile u32 g_horizon_exception_slot_mask;

	void horizon_fault_trampoline();
	void horizon_fault_resume_marker();
}

namespace
{
	struct ExceptionSlot
	{
		ThreadExceptionDump dump;
		u8 padding[SLOT_ORIGINAL_X0 - sizeof(ThreadExceptionDump)];
		u64 original_x0;
		u64 original_pc;
		u32 deferred;
		u32 result;
		void* frame;
		u32 index;
		u8 tail[EXCEPTION_SLOT_SIZE - SLOT_INDEX - sizeof(u32)];
	};

	static_assert(sizeof(ExceptionSlot) == EXCEPTION_SLOT_SIZE);
	static_assert(offsetof(ExceptionSlot, original_x0) == SLOT_ORIGINAL_X0);
	static_assert(offsetof(ExceptionSlot, original_pc) == SLOT_ORIGINAL_PC);
	static_assert(offsetof(ExceptionSlot, deferred) == SLOT_DEFERRED);
	static_assert(offsetof(ExceptionSlot, result) == SLOT_RESULT);
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

	ExceptionSlot* SlotFromDump(ThreadExceptionDump* dump)
	{
		const uptr address = reinterpret_cast<uptr>(dump);
		const uptr base = reinterpret_cast<uptr>(&g_horizon_exception_slots[0][0]);
		if (address < base || address - base >= sizeof(g_horizon_exception_slots) ||
			((address - base) & (EXCEPTION_SLOT_SIZE - 1)) != 0)
		{
			return nullptr;
		}
		return reinterpret_cast<ExceptionSlot*>(dump);
	}

	void ReleaseSlot(ExceptionSlot* slot)
	{
		if (slot && slot->index < EXCEPTION_SLOT_COUNT)
		{
			__atomic_fetch_and(&g_horizon_exception_slot_mask,
				~(1u << slot->index), __ATOMIC_RELEASE);
		}
	}

	constexpr u32 ESR_EC_DATA_ABORT_LOWER = 0x24;
	constexpr u32 ESR_EC_DATA_ABORT_SAME = 0x25;
} // namespace

bool HorizonFastmem::HasResumableFaultHandler()
{
	return true;
}

extern "C" int horizon_run_deferred_fault(ThreadExceptionDump* dump)
{
	ExceptionSlot* const slot = SlotFromDump(dump);
	if (!slot)
		return 0;

	const bool is_write = (slot->dump.esr & (1u << 6)) != 0;
	const PageFaultHandler::HandlerResult result =
		PageFaultHandler::HandlePageFault(reinterpret_cast<void*>(slot->original_pc),
			reinterpret_cast<void*>(slot->dump.far.x), is_write);

	slot->dump.cpu_gprs[0].x = slot->original_x0;
	slot->dump.pc.x = slot->original_pc;
	return result == PageFaultHandler::HandlerResult::ContinueExecution;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
	const uptr pc = static_cast<uptr>(ctx->pc.x);
	const uptr resume_marker = reinterpret_cast<uptr>(&horizon_fault_resume_marker);
	if (pc >= resume_marker && pc < resume_marker + 8)
	{
		ExceptionSlot* const original =
			SlotFromDump(reinterpret_cast<ThreadExceptionDump*>(ctx->cpu_gprs[19].x));
		if (original && original->deferred)
		{
			const bool handled = original->result != 0;
			std::memcpy(ctx, &original->dump, sizeof(*ctx));
			ReleaseSlot(original);
			if (handled)
				return;
		}
	}
	else if (threadExceptionIsAArch64(ctx))
	{
		const u32 exception_class = (ctx->esr >> 26) & 0x3f;
		if ((exception_class == ESR_EC_DATA_ABORT_LOWER || exception_class == ESR_EC_DATA_ABORT_SAME) &&
			PageFaultHandler::IsHorizonFaultCandidate(reinterpret_cast<void*>(ctx->far.x)))
		{
			ExceptionSlot* const slot = SlotFromDump(ctx);
			if (slot)
			{
				slot->original_x0 = ctx->cpu_gprs[0].x;
				slot->original_pc = ctx->pc.x;
				slot->deferred = 1;
				slot->result = 0;
				ctx->cpu_gprs[0].x = reinterpret_cast<uptr>(ctx);
				ctx->pc.x = reinterpret_cast<uptr>(&horizon_fault_trampoline);
				return;
			}
		}
	}

	static constexpr char message[] = "Unhandled Horizon exception\n";
	svcOutputDebugString(message, sizeof(message) - 1);
	svcExitProcess();
}
