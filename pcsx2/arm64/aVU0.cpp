// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0
//
// ARM64 VU0 Recompiler — Main driver.
// Adapted from the VU1 ARM64 recompiler. VU0 has 4KB micro memory,
// no XGKICK, and different VPU_STAT bits.

#include "Common.h"
#include "GS.h"
#include "Memory.h"
#include "Vif.h"
#include "VUmicro.h"
#include "VUops.h"
// .inl-side headers — pulled in here (BEFORE `using namespace vixl::aarch64;`)
// so the s8/s16 typedefs in MTVU.h → Vif_Unpack.h aren't ambiguous with the
// vixl::aarch64 namespace once it's brought into scope below.
#include "MTVU.h"
#include "VU.h"
#include "VUflags.h"
#include <cmath>
#include "arm64/AsmHelpers.h"
#include "arm64/arm64Emitter.h"
#include "arm64/aVU0.h"
#include "common/Perf.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#ifdef VU0_SHADOW_VERIFY
#include <dlfcn.h>
#include <unwind.h>
#endif

using namespace vixl::aarch64;

// ============================================================================
//  Shared file-statics used by the parent driver AND the included
//  aVU0_Upper.inl / aVU0_Lower.inl op-emitters.
//
//  VU0_BASE_REG is pinned to &VU0 throughout a compiled block (x23 in our
//  AAPCS64 callee-saved set). vfOff / viOff are byte offsets into VURegs
//  for VF[reg] / VI[reg], used by every reg-relative LDR/STR the emitters
//  build.
// ============================================================================
static const auto VU0_BASE_REG = x23;

static constexpr int64_t vfOff(u32 reg)
{
	return static_cast<int64_t>(offsetof(VURegs, VF)) + reg * static_cast<int64_t>(sizeof(VECTOR));
}

static constexpr int64_t viOff(u32 reg)
{
	return static_cast<int64_t>(offsetof(VURegs, VI)) + reg * static_cast<int64_t>(sizeof(REG_VI));
}

// Global instance
recArmVU0 CpuArmVU0;

// VU0 per-cycle interpreter entry point
extern void vu0Exec(VURegs* VU);

// Flush helpers
extern void _vuFlushAll(VURegs* VU);

// ============================================================================
//  Native NEON codegen — aligned with arm64/mac/aVU.cpp's #include layout.
//  Upper.inl emits FMAC / float-vector ops + recVU0_UpperTable. Lower.inl
//  emits integer ALU / LSU / branches / FDIV / EFU / specials + table. Both
//  consume VU0_BASE_REG / vfOff / viOff defined just above.
// ============================================================================
using VU0RecFn = void (*)();
#include "arm64/aVU0_Upper.inl"
#include "arm64/aVU0_Lower.inl"

// ============================================================================
//  Block cache
// ============================================================================

static constexpr u32 VU0_NUM_SLOTS       = VU0_PROGSIZE / 8;
static constexpr u32 VU0_MAX_BLOCK_PAIRS = 128;

static constexpr u32 POOL_SIZE = 32 * 1024;

// Block-linking patch slot. Mirrors VU1's LinkExit. Each compiled block has
// up to 2 of these (single-exit fall-through / unconditional branch, or
// 2-way conditional with split tails). `patch_site` is the address of an
// unconditional B that gets rewritten between fallthrough (unlinked) and
// successor's linkEntry (linked).
struct LinkExit
{
	u32 target_pc;
	u8* patch_site;
	u8* fallthrough;
	u8* current_target;
};
static constexpr u32 LINK_TARGET_NONE = ~0u;

struct VU0BlockEntry
{
	u8*  codeEntry;   // prologue entry — for first-call dispatch from Execute
	u8*  linkEntry;   // post-prologue entry; predecessors B here directly
	u8*  returnExit;  // post-pair-loop label where the exit-selector lives

	// Static link-exit set:
	//   0 : not linkable (ebit, JR/JALR via runtime dispatch, MFLAGSET-only).
	//   1 : single exit (B/BAL, or max-size fall-through).
	//   2 : conditional IBxx — exits[0]=NOT-TAKEN, exits[1]=TAKEN.
	u32      num_exits;
	LinkExit exits[2];

	u32  numPairs;

	// Content-keyed cache key. Snapshot of the VU0.Micro bytes this variant
	// was compiled against (numPairs * 8 bytes). findVariantVU0 picks the
	// variant whose snapshot matches the live micro at dispatch time —
	// surviving across Clear() so re-uploads of the same program get a hit
	// instead of a fresh compile. Heap-owned; freed in destroyVariantVU0.
	u8*  snapshot;

	// Set by Clear() when this variant's slot is in a cleared range. The
	// next dispatch via findVariantVU0 re-runs tryForwardLinkVU0 +
	// patchWaitingPredecessorsVU0 to re-wire incoming/outgoing edges
	// (Clear already unpatched incoming edges for correctness).
	bool needsRelink;
};

// Per-slot cap on the variant deque. Mirrors VU1's kVariantCapPerSlot. 8
// covers any sane number of distinct programs at one PC; the LRU is
// evicted via destroyVariantVU0 when the cap is hit.
static constexpr u32 kVU0VariantCapPerSlot = 8;

// Content-keyed program cache — one deque of compiled variants per slot.
// Most slots carry 0 or 1 variant in steady state; a slot grows a deque
// only when the EE uploads different bytecode programs to the same PC.
// Front of the deque is the MRU variant — findVariantVU0 bubbles hits forward.
static std::deque<VU0BlockEntry*> s_vu0_variants[VU0_NUM_SLOTS];

// Reverse index: for each VU0 slot S, the list of variants that have at
// least one exit targeting S. patchWaitingPredecessorsVU0 walks this to
// find waiters after a fresh compile; Clear() walks it to find predecessors
// that need their incoming exits unpatched.
//
// A variant is added once per UNIQUE target_pc among its exits[]. Variants
// stay in the index for their lifetime; cleaned up by destroyVariantVU0
// (per-eviction) or deleteAllVariantsVU0 (full reset).
static std::vector<VU0BlockEntry*> s_vu0_waitingForSlot[VU0_NUM_SLOTS];

static u8* s_code_base  = nullptr;
static u8* s_code_write = nullptr;
static u8* s_code_end   = nullptr;
static ArmConstantPool s_pool;

// ============================================================================
//  Branch opcode classifiers (VU instruction encoding identical to VU1).
// ============================================================================

// B / BAL — single compile-time-known target, no runtime condition.
static inline bool isUnconditionalBranchOpVU0(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x20u || top7 == 0x21u;
}

// IBEQ / IBNE / IBLTZ / IBGTZ / IBLEZ / IBGEZ — both targets known,
// runtime condition picks.
static inline bool isConditionalBranchOpVU0(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x28u || top7 == 0x29u
	    || (top7 >= 0x2Cu && top7 <= 0x2Fu);
}

// JR / JALR — runtime VI register holds target.
static inline bool isIndirectBranchOpVU0(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x24u || top7 == 0x25u;
}

// ============================================================================
//  Runtime helpers
// ============================================================================

// D/T bit firing on VU0. Fires the INTC IRQ immediately via hwIntcIrq
// rather than x86 microVU's VUFLAG_INTCINTERRUPT deferred-flag pattern
// (microVU_Compile.inl:570). Safe because hwIntcIrq only mutates state
// (INTC_STAT |= 1<<n, then cpuTestINTCInts which at most schedules an EE
// event via cpuSetNextEventDelta(4)). No preemption, no dispatch. The
// timing divergence vs x86 is sub-block (~1 pair) since ebit=1 terminates
// the block immediately after this via step 13's countdown anyway.
static void vu0CheckDTBits(u32 upper)
{
	if (upper & 0x10000000) // D flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x4)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x2;
			hwIntcIrq(INTC_VU0);
			VU0.ebit = 1;
		}
	}
	if (upper & 0x08000000) // T flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x8)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x4;
			hwIntcIrq(INTC_VU0);
			VU0.ebit = 1;
		}
	}
}

static void vu0EbitDone(VURegs* VU)
{
	VU->VIBackupCycles = 0;
	_vuFlushAll(VU);
	VU0.VI[REG_VPU_STAT].UL &= ~0x1;
	vif0Regs.stat.VEW = false;
}

static void vu0HandleDelayBranch(VURegs* VU)
{
	if (VU->takedelaybranch)
	{
		VU->branch          = 1;
		VU->branchpc        = VU->delaybranchpc;
		VU->takedelaybranch = false;
	}
}

// Predicate: does this lower instruction word decode to a NOP on VU0?
// Matches x86 microVU's isNOP=true on isVU0 for EFU ops, XGKICK, XTOP, and
// the pipeline-wait ops WAITQ/WAITP. Used to elide the per-pair VU0.code
// store and rec-table dispatch scaffolding for these ops — the rec emitters
// themselves already NOP, so the Mov+Str + table indirection was pure waste.
//
// Lower dispatch flow: lower >> 25 == 0x40 → LowerOP sub-table;
//   (lower & 0x3F) in 0x3C..0x3F → T3_xx sub-table;
//   (lower >> 6) & 0x1F → T3_xx entry.
static bool isVU0LowerNOP(u32 lower)
{
	if ((lower >> 25) != 0x40) return false;
	const u32 lower_op = lower & 0x3f;
	if (lower_op < 0x3C) return false;

	const u32 t3_sub = (lower >> 6) & 0x1f;
	switch (lower_op)
	{
		case 0x3C: // T3_00 — XTOP, XGKICK, ESADD, EATANxy, ESQRT, ESIN (all NOP on VU0)
			return (t3_sub >= 0x1A) && (t3_sub <= 0x1F);
		case 0x3D: // T3_01 — ERSADD, EATANxz, ERSQRT, EATAN (NOP). XITOP (0x1A) does real work.
			return (t3_sub >= 0x1C) && (t3_sub <= 0x1F);
		case 0x3E: // T3_10 — ELENG, ESUM, ERCPR, EEXP (all NOP on VU0)
			return (t3_sub >= 0x1C) && (t3_sub <= 0x1F);
		case 0x3F: // T3_11 — WAITQ(0x0E), ERLENG(0x1C), WAITP(0x1E). 0x1D/0x1F are Unknown (skip).
			return (t3_sub == 0x0E) || (t3_sub == 0x1C) || (t3_sub == 0x1E);
	}
	return false;
}

// Runtime state probe — dumps VU0 state + select externals so the user can
// diff "native" vs "fallback" runs at a specific pc. The JIT emits a call
// to this around the suspect pair body in both paths. Tag distinguishes
// pre/post so the diff knows which dump to compare against which.
static void vu0ProbeState(VURegs* VU, u32 pc, u32 tag)
{
	const char* label = (tag == 0) ? "PRE " : "POST";
	Console.WriteLn("VU0 PROBE [%s] pc=0x%04x  cycle=%llu  tpc=0x%08x  ebit=%u  branch=%u  branchpc=0x%04x",
		label, pc, (unsigned long long)VU->cycle,
		VU->VI[REG_TPC].UL, VU->ebit, VU->branch, VU->branchpc);
	Console.WriteLn("  ACC={%08x %08x %08x %08x}  Q=%08x  P=%08x  flags=%08x  VPU_STAT=%08x",
		VU->ACC.UL[0], VU->ACC.UL[1], VU->ACC.UL[2], VU->ACC.UL[3],
		VU->q.UL, VU->p.UL, VU->flags, VU->VI[REG_VPU_STAT].UL);
	Console.WriteLn("  macflag=%08x  statusflag=%08x  clipflag=%08x  fmacwritepos=%u  fmaccount=%u",
		VU->macflag, VU->statusflag, VU->clipflag,
		VU->fmacwritepos, VU->fmaccount);
	// Read FPCR — JIT NEON ops + interp scalar ops both honor FPCR; if it
	// drifts (FZ/RM bits) results diverge silently. Useful sanity-check.
	uint64_t fpcr = 0;
	asm volatile("mrs %0, fpcr" : "=r"(fpcr));
	Console.WriteLn("  fpcr=0x%016llx", (unsigned long long)fpcr);
}

static void vu0DecrementVIBackup(VURegs* VU, u64 cyclesBefore)
{
	if (VU->VIBackupCycles > 0)
	{
		u32 elapsed = static_cast<u32>(VU->cycle - cyclesBefore);
		if (elapsed >= VU->VIBackupCycles)
			VU->VIBackupCycles = 0;
		else
			VU->VIBackupCycles -= static_cast<u8>(elapsed);
	}
}

// ============================================================================
//  Shadow-compare debug harness (VU0_SHADOW_VERIFY).
//
//  Hooks into the native (non-fallback) per-pair path. Pre-pair: snapshots
//  VU0 to s_shadow_pre. Post-pair: saves VU0 (the JIT result) to s_shadow_post,
//  restores VU0 from s_shadow_pre, runs vu0Exec(&VU0) for the SAME pair via
//  interp, then compares interp result (VU0) against JIT result (s_shadow_post)
//  field-by-field. Logs the first divergent field per pair, then restores VU0
//  to the JIT result so the game continues with whatever JIT produced (we
//  want to find the bug, not silently fix it).
//
//  Skipped for fallback pairs because those run vu0Exec themselves and would
//  always agree with interp.
//
//  Limitations:
//    - hwIntcIrq is global state; if interp's _vu0Exec fires INTC for D/T
//      bits we'd see a duplicate IRQ. Mitigation: D/T pairs go through the
//      INTERP_VU0_DTBITS fallback in the user's harness setup, so the verify
//      hook never fires on D/T pairs.
//    - VU0.code is restored along with the rest of VU0 — no global leak.
//    - Mem / Micro pointers are shared (same backing memory); the snapshot
//      only duplicates the header struct, not the 4KB micro program.
// ============================================================================
#ifdef VU0_SHADOW_VERIFY
alignas(16) static u8 s_shadow_pre [sizeof(VURegs)];
alignas(16) static u8 s_shadow_post[sizeof(VURegs)];
// VU0.Mem (4KB data memory) is allocated separately from VURegs and only
// referenced by pointer inside the struct — so the VURegs-only snapshots
// above don't capture it. SQ/SQI/SQD/ISWR ops write here, and a buggy JIT
// codegen for any of those would silently corrupt VU0.Mem without
// tripping any VURegs comparison. Mirror snapshot/restore/compare for
// the Mem buffer so the harness covers stores too.
alignas(16) static u8 s_shadow_pre_mem [VU0_MEMSIZE];
alignas(16) static u8 s_shadow_post_mem[VU0_MEMSIZE];

// External (non-VURegs, non-Mem) state mutated by VU0 ops. Without these
// the harness goes silent on D/T-bit timing and ebit completion side
// effects that don't surface in VURegs but do affect cumulative game
// behavior:
//   - vif0Regs.stat.VEW: cleared on ebit termination (interp's
//     _vu0Exec ebit countdown OR JIT's vu0EbitDone). Both should agree
//     post-pair, but the harness has never proved it.
//   - INTC_STAT bit INTC_VU0: set by hwIntcIrq when D-bit/T-bit fires
//     under matching FBRST mask. Interp fires PRE-upper exec, JIT fires
//     POST-upper-and-lower exec via vu0CheckDTBits — same end state for
//     a single pair, but the timing affects when cpuTestINTCInts
//     schedules the EE event.
//   - cpuRegs.nextEventCycle: cpuTestINTCInts mutates this when INTC
//     pending bits AND the mask are set with EE interrupts enabled.
//     Diverges if D/T-bit firing schedules the event at a different
//     point in the pair.
//
// Snapshot full vif0Regs (small, ~64B) for safety — VEW is one bit but
// any VIF-side effect we miss would be invisible. INTC_STAT and
// nextEventCycle are scalars.
alignas(16) static u8  s_shadow_pre_vif [sizeof(vif0Regs)];
alignas(16) static u8  s_shadow_post_vif[sizeof(vif0Regs)];
static u32 s_shadow_pre_intc;
static u32 s_shadow_post_intc;
static u64 s_shadow_pre_next_event;
static u64 s_shadow_post_next_event;
// Latched on the first detected divergence. Once set, vu0_shadow_verify
// becomes a no-op so a bad 3D scene doesn't flood logcat with thousands
// of follow-on errors before the user can see the original failure. The
// halt path also sets it before aborting (belt and braces in case any
// other thread is mid-verify when we abort).
static std::atomic<bool> s_shadow_diverged{false};

// Set by the carryover-branch gate at block linkEntry when it dispatches
// a single pair to vu0Exec instead of running the JIT block normally.
// The block-shadow verify checks this and skips the interp re-run +
// comparison: the harness's "JIT and interp ran the same numPairs" math
// breaks when JIT bailed out after one pair. Cleared by snapshot at
// every block entry. Single-thread (VU0 always runs on EE thread).
static bool s_block_took_carryover = false;

// Note: D/T/M/E-bit block detection lives at compile time as a local
// `block_has_dtme` in CompileBlock (not a runtime flag), since the
// block's content is statically known when emitting. See the comment
// at the scan site for the rationale.

// Block-level shadow buffers — used by vu0_block_shadow_snapshot/verify.
// Per-pair shadow catches divergences within a single pair given identical
// inputs. If per-pair stays silent but INTERP_VU0_PAIR fixes a bug, the
// divergence must be cumulative across pairs OR in state we haven't
// snapshotted at the per-pair level. Block-level shadow detects this:
// snapshot at block entry, run the JIT block end-to-end, then re-run
// interp's `vu0Exec` loop from the same starting state for `numPairs`
// iterations (or until it terminates), compare end states.
//
// Mathematically, if every per-pair `f_jit(state) == f_interp(state)`,
// composing them gives the same end state. So block-level firing while
// per-pair stays silent means there's per-pair state we DIDN'T snapshot
// — the byte-offset of the first divergent field maps it back to the
// missing field via offsetof.
alignas(16) static u8 s_block_shadow_pre    [sizeof(VURegs)];
alignas(16) static u8 s_block_shadow_post   [sizeof(VURegs)];
alignas(16) static u8 s_block_shadow_pre_mem [VU0_MEMSIZE];
alignas(16) static u8 s_block_shadow_post_mem[VU0_MEMSIZE];
alignas(16) static u8 s_block_shadow_pre_vif [sizeof(vif0Regs)];
alignas(16) static u8 s_block_shadow_post_vif[sizeof(vif0Regs)];
static u32 s_block_shadow_pre_intc;
static u32 s_block_shadow_post_intc;
static u64 s_block_shadow_pre_next_event;
static u64 s_block_shadow_post_next_event;

static void vu0_shadow_snapshot()
{
	if (s_shadow_diverged.load(std::memory_order_relaxed))
		return;
	std::memcpy(s_shadow_pre, &VU0, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(s_shadow_pre_mem, VU0.Mem, VU0_MEMSIZE);
	std::memcpy(s_shadow_pre_vif, &vif0Regs, sizeof(vif0Regs));
	s_shadow_pre_intc       = psHu32(INTC_STAT);
	s_shadow_pre_next_event = cpuRegs.nextEventCycle;
}

// Helper for the carryover-branch gate when VU0_SHADOW_VERIFY is on: sets
// the s_block_took_carryover flag (so the block-shadow verify skips this
// run) and then dispatches to vu0Exec. Outside shadow mode the JIT calls
// vu0Exec directly.
static void vu0_block_carryover_dispatch(VURegs* VU)
{
	s_block_took_carryover = true;
	vu0Exec(VU);
}

static void vu0_shadow_dump_state(const char* label, const VURegs* state)
{
	Console.Error("--- %s ---", label);
	for (u32 v = 0; v < 32; v++)
	{
		Console.Error("  VF[%2u] = {%08x %08x %08x %08x}", v,
			state->VF[v].UL[0], state->VF[v].UL[1], state->VF[v].UL[2], state->VF[v].UL[3]);
	}
	Console.Error("  ACC    = {%08x %08x %08x %08x}",
		state->ACC.UL[0], state->ACC.UL[1], state->ACC.UL[2], state->ACC.UL[3]);
	for (u32 v = 0; v < 32; v++)
		Console.Error("  VI[%2u] = %08x", v, state->VI[v].UL);
	Console.Error("  Q=%08x  P=%08x  I=%08x  R=%08x",
		state->q.UL, state->p.UL, state->VI[REG_I].UL, state->VI[REG_R].UL);
	Console.Error("  cycle=%llu  macflag=%08x  statusflag=%08x  clipflag=%08x",
		(unsigned long long)state->cycle, state->macflag, state->statusflag, state->clipflag);
	Console.Error("  ebit=%u  branch=%u  branchpc=0x%04x  flags=%08x",
		state->ebit, state->branch, state->branchpc, state->flags);
	Console.Error("  fmacwritepos=%u  fmaccount=%u",
		state->fmacwritepos, state->fmaccount);
}

struct ShadowUnwindCtx
{
	u32 frames_seen = 0;
	static constexpr u32 kMaxFrames = 32;
};

static _Unwind_Reason_Code vu0_shadow_unwind_cb(struct _Unwind_Context* ctx, void* arg)
{
	auto* state = static_cast<ShadowUnwindCtx*>(arg);
	if (state->frames_seen >= ShadowUnwindCtx::kMaxFrames)
		return _URC_END_OF_STACK;

	const uintptr_t pc = _Unwind_GetIP(ctx);
	if (!pc)
		return _URC_END_OF_STACK;

	Dl_info info{};
	const char* sym = "?";
	uintptr_t off = 0;
	if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
	{
		sym = info.dli_sname;
		off = pc - reinterpret_cast<uintptr_t>(info.dli_saddr);
	}
	else if (info.dli_fname)
	{
		sym = info.dli_fname;
		off = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
	}
	Console.Error("  #%2u  pc=0x%016lx  %s+0x%lx",
		state->frames_seen, (unsigned long)pc, sym, (unsigned long)off);
	state->frames_seen++;
	return _URC_NO_REASON;
}

static void vu0_shadow_halt(u32 pc, const char* first_field, const char* first_detail)
{
	// Single-shot: if another invocation beats us in, just return.
	if (s_shadow_diverged.exchange(true, std::memory_order_acq_rel))
		return;

	const u32 lower = *reinterpret_cast<const u32*>(VU0.Micro + pc);
	const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);

	Console.Error("============================================================");
	Console.Error(" VU0 SHADOW DIVERGENCE  pc=0x%04x", pc);
	Console.Error("   first divergent field: %s  %s", first_field, first_detail);
	Console.Error("   pair opcodes: lower=0x%08x  upper=0x%08x", lower, upper);
	Console.Error("============================================================");

	// Three snapshots so the user can see what changed and on what input:
	//   PRE   = VU0 just before the JIT pair body ran
	//   JIT   = VU0 after the JIT pair body ran
	//   INTERP= VU0 after vu0Exec ran the same pair on the same input
	const VURegs* pre   = reinterpret_cast<const VURegs*>(s_shadow_pre);
	const VURegs* jit   = reinterpret_cast<const VURegs*>(s_shadow_post);
	vu0_shadow_dump_state("PRE-PAIR (input state)", pre);
	vu0_shadow_dump_state("JIT RESULT", jit);
	vu0_shadow_dump_state("INTERP RESULT (truth)", &VU0);

	Console.Error("--- Native C++ backtrace ---");
	ShadowUnwindCtx ctx;
	_Unwind_Backtrace(&vu0_shadow_unwind_cb, &ctx);

	Console.Error("============================================================");
	Console.Error(" Aborting — tombstone will follow");
	Console.Error("============================================================");
	// SIGABRT → debuggerd → tombstone with full unwound C++ backtrace.
	std::abort();
}

static void vu0_shadow_log(u32 pc, const char* field, const char* fmt, ...)
{
	char detail[224];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	// Halt path takes the first divergence and aborts. Anything else is
	// suppressed — see s_shadow_diverged.
	vu0_shadow_halt(pc, field, detail);
}

// Block-level halt — dumps the BLOCK shadow buffers (s_block_shadow_pre /
// s_block_shadow_post) instead of the per-pair buffers used by
// vu0_shadow_halt. This is critical for diagnosing block-level
// divergences: per-pair s_shadow_pre/post hold whatever the most recent
// native pair captured, which may not even be from THIS block.
static void vu0_block_shadow_halt(u32 startPC, u32 numPairs,
	const char* first_field, const char* first_detail)
{
	if (s_shadow_diverged.exchange(true, std::memory_order_acq_rel))
		return;

	const u32 lower = *reinterpret_cast<const u32*>(VU0.Micro + (startPC & VU0_PROGMASK));
	const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + ((startPC + 4) & VU0_PROGMASK));

	Console.Error("============================================================");
	Console.Error(" VU0 BLOCK SHADOW DIVERGENCE  startPC=0x%04x  numPairs=%u",
		startPC, numPairs);
	Console.Error("   first divergent field: %s  %s", first_field, first_detail);
	Console.Error("   block first-pair opcodes: lower=0x%08x  upper=0x%08x", lower, upper);
	Console.Error("============================================================");

	// BLOCK PRE = state captured at block linkEntry (before any pair ran).
	// BLOCK JIT = state captured at block exit, before the verify rolled
	//             VU0 back to pre and re-ran interp.
	// BLOCK INTERP = current live VU0, populated by the interp loop replay.
	const VURegs* pre  = reinterpret_cast<const VURegs*>(s_block_shadow_pre);
	const VURegs* jit  = reinterpret_cast<const VURegs*>(s_block_shadow_post);
	vu0_shadow_dump_state("BLOCK PRE  (input state at block linkEntry)", pre);
	vu0_shadow_dump_state("BLOCK JIT  (state after JIT block ran)",      jit);
	vu0_shadow_dump_state("BLOCK INTERP (state after interp loop replay)", &VU0);

	// External state — vif0Regs / INTC_STAT / nextEventCycle (block-level).
	// Console.Error doesn't take vsnprintf-style varargs cleanly; preformat.
	{
		const u32* vif_pre  = reinterpret_cast<const u32*>(s_block_shadow_pre_vif);
		const u32* vif_jit  = reinterpret_cast<const u32*>(s_block_shadow_post_vif);
		const u32* vif_int  = reinterpret_cast<const u32*>(&vif0Regs);
		Console.Error("--- BLOCK External State ---");
		Console.Error("  vif0Regs[stat]  PRE=0x%08x  JIT=0x%08x  INTERP=0x%08x",
			vif_pre[0], vif_jit[0], vif_int[0]);
		Console.Error("  INTC_STAT       PRE=0x%08x  JIT=0x%08x  INTERP=0x%08x",
			s_block_shadow_pre_intc, s_block_shadow_post_intc, psHu32(INTC_STAT));
		Console.Error("  nextEventCycle  PRE=%llu  JIT=%llu  INTERP=%llu",
			(unsigned long long)s_block_shadow_pre_next_event,
			(unsigned long long)s_block_shadow_post_next_event,
			(unsigned long long)cpuRegs.nextEventCycle);
	}

	Console.Error("--- Native C++ backtrace ---");
	ShadowUnwindCtx ctx;
	_Unwind_Backtrace(&vu0_shadow_unwind_cb, &ctx);

	Console.Error("============================================================");
	Console.Error(" Aborting — tombstone will follow");
	Console.Error("============================================================");
	std::abort();
}

static void vu0_block_shadow_log(u32 startPC, u32 numPairs,
	const char* field, const char* fmt, ...)
{
	char detail[224];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	vu0_block_shadow_halt(startPC, numPairs, field, detail);
}

static void vu0_shadow_verify(u32 pc)
{
	if (s_shadow_diverged.load(std::memory_order_relaxed))
	{
		// Already halted (or about to halt). Restore JIT result so the
		// VM doesn't drift further and skip the comparison.
		std::memcpy(&VU0, s_shadow_post, sizeof(VURegs));
		if (VU0.Mem)
			std::memcpy(VU0.Mem, s_shadow_post_mem, VU0_MEMSIZE);
		std::memcpy(&vif0Regs, s_shadow_post_vif, sizeof(vif0Regs));
		psHu32(INTC_STAT)         = s_shadow_post_intc;
		cpuRegs.nextEventCycle    = s_shadow_post_next_event;
		return;
	}

	// (per-pair verify only runs on native pairs, not the carryover-
	// gate dispatch — no s_block_took_carryover check needed here.)

	// Cycle-window gate: skip the vu0Exec re-run + memcmp when this pair
	// is outside the user's target cycle range. The JIT pair body has
	// ALREADY run by this point (we're called post-pair); the snapshot
	// at the start of the pair captured s_shadow_pre. We just don't
	// bother running the comparison interp pair, saving the dominant
	// cost of the harness in normal operation.
#if defined(VU0_SHADOW_VERIFY_FROM_CYCLE) || defined(VU0_SHADOW_VERIFY_TO_CYCLE)
	{
		const u64 cur = VU0.cycle;
#ifdef VU0_SHADOW_VERIFY_FROM_CYCLE
		if (cur < (VU0_SHADOW_VERIFY_FROM_CYCLE)) return;
#endif
#ifdef VU0_SHADOW_VERIFY_TO_CYCLE
		if (cur > (VU0_SHADOW_VERIFY_TO_CYCLE))   return;
#endif
	}
#endif
	// Snapshot post-JIT state. Capture VURegs struct, the 4KB VU0.Mem
	// buffer (mutated by SQ/SQI/SQD/ISWR), AND the external state mutated
	// by D/T-bit firing or ebit termination (vif0Regs / INTC_STAT /
	// cpuRegs.nextEventCycle). The interp re-run below would otherwise
	// overwrite all of these with its own values and the JIT-vs-interp
	// comparison would lose the JIT side.
	std::memcpy(s_shadow_post, &VU0, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(s_shadow_post_mem, VU0.Mem, VU0_MEMSIZE);
	std::memcpy(s_shadow_post_vif, &vif0Regs, sizeof(vif0Regs));
	s_shadow_post_intc       = psHu32(INTC_STAT);
	s_shadow_post_next_event = cpuRegs.nextEventCycle;

	// Patch s_shadow_pre's TPC to match the JIT's compile-time-known `pc`.
	// vu0Exec reads its pair from Micro[VI[REG_TPC]] — and at the moment
	// the snapshot was taken, TPC may be stale relative to this pair's pc.
	// Specifically: when a linked block enters via direct B (not through
	// Execute), the previous block's TPC store leaves TPC pointing at THAT
	// block's tail, not the new block's head. The JIT itself runs the
	// correct pair (compile-time-baked pc) but vu0Exec would otherwise run
	// whatever pair Micro[stale_TPC] points at — comparing two different
	// pairs and producing meaningless divergences (e.g. branch flag flips,
	// VI scratch register noise). Force TPC = pc here so vu0Exec runs the
	// SAME pair the JIT just executed.
	auto* pre_regs = reinterpret_cast<VURegs*>(s_shadow_pre);
	pre_regs->VI[REG_TPC].UL = pc;

	// Restore pre-pair state and run interp on the same pair. Mem is
	// rolled back to pre-pair contents so interp's stores land on the
	// same starting buffer the JIT saw — the post-vu0Exec contents of
	// VU0.Mem are then interp's authoritative result for the comparison.
	// External state (vif0Regs / INTC / nextEventCycle) is also rolled
	// back so D/T-bit firing and ebit termination side effects are
	// observed against a clean baseline.
	std::memcpy(&VU0, s_shadow_pre, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(VU0.Mem, s_shadow_pre_mem, VU0_MEMSIZE);
	std::memcpy(&vif0Regs, s_shadow_pre_vif, sizeof(vif0Regs));
	psHu32(INTC_STAT)         = s_shadow_pre_intc;
	cpuRegs.nextEventCycle    = s_shadow_pre_next_event;
	vu0Exec(&VU0);

	// Now: VU0 == interp result, s_shadow_post == JIT result.
	const VURegs* jit  = reinterpret_cast<const VURegs*>(s_shadow_post);
	const VURegs* iref = &VU0;
	bool diverged = false;

	auto check_u32 = [&](const char* name, u32 j, u32 i) -> bool {
		if (j != i) {
			vu0_shadow_log(pc, name, "jit=0x%08x interp=0x%08x", j, i);
			return true;
		}
		return false;
	};

	// VF[0..31] — most likely place for vertex-transform corruption to land.
	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (std::memcmp(&jit->VF[v], &iref->VF[v], sizeof(VECTOR)) != 0)
		{
			char fname[16];
			std::snprintf(fname, sizeof(fname), "VF[%u]", v);
			vu0_shadow_log(pc, fname,
				"jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
				jit->VF[v].UL[0], jit->VF[v].UL[1], jit->VF[v].UL[2], jit->VF[v].UL[3],
				iref->VF[v].UL[0], iref->VF[v].UL[1], iref->VF[v].UL[2], iref->VF[v].UL[3]);
			diverged = true;
		}
	}

	// ACC — accumulator FMAC ops write here.
	if (!diverged && std::memcmp(&jit->ACC, &iref->ACC, sizeof(VECTOR)) != 0)
	{
		vu0_shadow_log(pc, "ACC",
			"jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
			jit->ACC.UL[0], jit->ACC.UL[1], jit->ACC.UL[2], jit->ACC.UL[3],
			iref->ACC.UL[0], iref->ACC.UL[1], iref->ACC.UL[2], iref->ACC.UL[3]);
		diverged = true;
	}

	// Live working flags (drive future fmac slot snapshots).
	if (!diverged) diverged = check_u32("macflag",    jit->macflag,    iref->macflag);
	if (!diverged) diverged = check_u32("statusflag", jit->statusflag, iref->statusflag);
	if (!diverged) diverged = check_u32("clipflag",   jit->clipflag,   iref->clipflag);

	// VI registers — both integer regs and committed flag/Q/I/R/P registers.
	// VPU_STAT (VI[REG_VPU_STAT]) is FULLY EXCLUDED from per-pair compare.
	// Three reasons:
	//   1. Bits 8-15 are VU1-owned (running, D/T, XGKICK at bit 12). Under
	//      MTVU, VU1 thread mutates these asynchronously to VU0 pair
	//      execution — JIT post sees the mutation, interp single-pair
	//      replay doesn't, so spurious divergence.
	//   2. Bits 0-7 are VU0-owned but represent program-LIFECYCLE state
	//      (running, D/T fired, ebit-done). They mutate at events
	//      orthogonal to per-pair body execution (vu0ExecMicro start,
	//      vu0EbitDone in OTHER pairs of a block, EE-side vu0Finish).
	//      Bit 0 has been observed cleared in JIT post for pairs that
	//      don't touch VPU_STAT — likely an MTVU-induced cross-thread
	//      side effect, hard to reproduce in single-pair interp replay.
	//   3. Block-level shadow (`vu0_block_shadow_verify`) compares VU0-
	//      owned VPU_STAT bits over the whole block (with `0xFF` mask)
	//      and is the right scope to catch real VPU_STAT correctness
	//      bugs. Block-level skips D/T/M/E blocks where timing diverges
	//      intentionally (see CompileBlock's block_has_dtme scan).
	// Per-pair compare focuses on what pair-output it CAN verify:
	// VF / ACC / VI[0..28,30,31] / Q / P / flags / pipe slots / cycle.
	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (v == REG_VPU_STAT)
			continue;
		if (jit->VI[v].UL != iref->VI[v].UL)
		{
			char fname[16];
			std::snprintf(fname, sizeof(fname), "VI[%u]", v);
			diverged = check_u32(fname, jit->VI[v].UL, iref->VI[v].UL);
		}
	}

	// Pipeline state.
	if (!diverged && jit->cycle != iref->cycle)
	{
		vu0_shadow_log(pc, "cycle", "jit=%llu interp=%llu",
			(unsigned long long)jit->cycle, (unsigned long long)iref->cycle);
		diverged = true;
	}
	if (!diverged) diverged = check_u32("ebit",         jit->ebit,         iref->ebit);
	if (!diverged) diverged = check_u32("branch",       jit->branch,       iref->branch);
	if (!diverged) diverged = check_u32("branchpc",     jit->branchpc,     iref->branchpc);
	if (!diverged) diverged = check_u32("flags",        jit->flags,        iref->flags);

	// Delay-branch state (matters for branch-in-branch-delay-slot edge cases).
	if (!diverged) diverged = check_u32("delaybranchpc",   jit->delaybranchpc, iref->delaybranchpc);
	if (!diverged) diverged = check_u32("takedelaybranch",
		(u32)jit->takedelaybranch, (u32)iref->takedelaybranch);

	// VIBackup state (rolls back VI on branch backup-cycles unwind).
	if (!diverged) diverged = check_u32("VIBackupCycles",
		(u32)jit->VIBackupCycles, (u32)iref->VIBackupCycles);
	if (!diverged) diverged = check_u32("VIOldValue",  jit->VIOldValue,  iref->VIOldValue);
	if (!diverged) diverged = check_u32("VIRegNumber", jit->VIRegNumber, iref->VIRegNumber);

	// Pending Q/P (set by start-of-FDIV/EFU; consumed when result retires).
	if (!diverged) diverged = check_u32("pending_q", jit->pending_q, iref->pending_q);
	if (!diverged) diverged = check_u32("pending_p", jit->pending_p, iref->pending_p);

	// Q / P registers (FDIV / EFU committed results).
	if (!diverged && jit->q.UL != iref->q.UL)
		diverged = check_u32("q", jit->q.UL, iref->q.UL);
	if (!diverged && jit->p.UL != iref->p.UL)
		diverged = check_u32("p", jit->p.UL, iref->p.UL);

	// FMAC pipeline scalars.
	if (!diverged) diverged = check_u32("fmacwritepos", jit->fmacwritepos, iref->fmacwritepos);
	if (!diverged) diverged = check_u32("fmacreadpos",  jit->fmacreadpos,  iref->fmacreadpos);
	if (!diverged) diverged = check_u32("fmaccount",    jit->fmaccount,    iref->fmaccount);

	// FMAC pipeline slot contents — each queued writeback. Critical: the
	// JIT may store a slot with subtly wrong sCycle / Cycle / flagreg /
	// macflag content. Per-pair output state can match while a slot's
	// retire-time effect diverges from interp's. This is the "cumulative
	// drift" the per-pair scalar checks miss.
	//
	// Normalization before compare: when regupper/reglower is 0 the slot
	// has NO actual VF writeback for that half (VF[0] is hardwired and
	// writes to it are no-ops). The xyzwupper/xyzwlower mask in that case
	// is don't-care — the hazard checker looks at `reg == query_reg`
	// before consulting the mask. The interp's `_vuRegsMTIR` and
	// similar non-VF-writing handlers leave `VFwxyzw` uninitialized in
	// the stack-local `_VURegsNum`, so it carries 0xff from prior frames;
	// the JIT's pre-baked analyze structs are zero-init. Mask to 0 when
	// reg=0 so the compare focuses on slots with real VF writebacks.
	for (u32 i = 0; i < 4 && !diverged; i++)
	{
		fmacPipe jp = jit->fmac[i];
		fmacPipe ip = iref->fmac[i];
		if (jp.regupper == 0) jp.xyzwupper = 0;
		if (jp.reglower == 0) jp.xyzwlower = 0;
		if (ip.regupper == 0) ip.xyzwupper = 0;
		if (ip.reglower == 0) ip.xyzwlower = 0;
		if (std::memcmp(&jp, &ip, sizeof(fmacPipe)) != 0)
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "fmac[%u]", i);
			vu0_shadow_log(pc, fname,
				"jit{ru=%u rl=%u fr=%d xu=%u xl=%u sC=%llu C=%u m=%08x s=%08x c=%08x} "
				"interp{ru=%u rl=%u fr=%d xu=%u xl=%u sC=%llu C=%u m=%08x s=%08x c=%08x}",
				jp.regupper, jp.reglower, jp.flagreg, jp.xyzwupper, jp.xyzwlower,
				(unsigned long long)jp.sCycle, jp.Cycle, jp.macflag, jp.statusflag, jp.clipflag,
				ip.regupper, ip.reglower, ip.flagreg, ip.xyzwupper, ip.xyzwlower,
				(unsigned long long)ip.sCycle, ip.Cycle, ip.macflag, ip.statusflag, ip.clipflag);
			diverged = true;
		}
	}

	// IALU pipeline scalars + slot contents.
	if (!diverged) diverged = check_u32("ialuwritepos", jit->ialuwritepos, iref->ialuwritepos);
	if (!diverged) diverged = check_u32("ialureadpos",  jit->ialureadpos,  iref->ialureadpos);
	if (!diverged) diverged = check_u32("ialucount",    jit->ialucount,    iref->ialucount);
	for (u32 i = 0; i < 4 && !diverged; i++)
	{
		const ialuPipe& jp = jit->ialu[i];
		const ialuPipe& ip = iref->ialu[i];
		if (std::memcmp(&jp, &ip, sizeof(ialuPipe)) != 0)
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "ialu[%u]", i);
			vu0_shadow_log(pc, fname,
				"jit{r=%d sC=%llu C=%u} interp{r=%d sC=%llu C=%u}",
				jp.reg, (unsigned long long)jp.sCycle, jp.Cycle,
				ip.reg, (unsigned long long)ip.sCycle, ip.Cycle);
			diverged = true;
		}
	}

	// FDIV pipeline state.
	if (!diverged && std::memcmp(&jit->fdiv, &iref->fdiv, sizeof(fdivPipe)) != 0)
	{
		vu0_shadow_log(pc, "fdiv",
			"jit{en=%d reg=%08x sC=%llu C=%u sf=%08x} interp{en=%d reg=%08x sC=%llu C=%u sf=%08x}",
			jit->fdiv.enable, jit->fdiv.reg.UL,
			(unsigned long long)jit->fdiv.sCycle, jit->fdiv.Cycle, jit->fdiv.statusflag,
			iref->fdiv.enable, iref->fdiv.reg.UL,
			(unsigned long long)iref->fdiv.sCycle, iref->fdiv.Cycle, iref->fdiv.statusflag);
		diverged = true;
	}

	// EFU pipeline state.
	if (!diverged && std::memcmp(&jit->efu, &iref->efu, sizeof(efuPipe)) != 0)
	{
		vu0_shadow_log(pc, "efu",
			"jit{en=%d reg=%08x sC=%llu C=%u} interp{en=%d reg=%08x sC=%llu C=%u}",
			jit->efu.enable, jit->efu.reg.UL,
			(unsigned long long)jit->efu.sCycle, jit->efu.Cycle,
			iref->efu.enable, iref->efu.reg.UL,
			(unsigned long long)iref->efu.sCycle, iref->efu.Cycle);
		diverged = true;
	}

	// micro_macflags / micro_clipflags / micro_statusflags ring buffers
	// (used by the interpreter for end-of-program flag commit). These
	// shouldn't drift if everything else matches, but checking catches
	// subtle ring-write-position bugs.
	for (u32 i = 0; i < 4 && !diverged; i++)
	{
		if (jit->micro_macflags[i] != iref->micro_macflags[i])
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "micro_macflags[%u]", i);
			diverged = check_u32(fname, jit->micro_macflags[i], iref->micro_macflags[i]);
		}
		else if (jit->micro_clipflags[i] != iref->micro_clipflags[i])
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "micro_clipflags[%u]", i);
			diverged = check_u32(fname, jit->micro_clipflags[i], iref->micro_clipflags[i]);
		}
		else if (jit->micro_statusflags[i] != iref->micro_statusflags[i])
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "micro_statusflags[%u]", i);
			diverged = check_u32(fname, jit->micro_statusflags[i], iref->micro_statusflags[i]);
		}
	}

	// Catchall: full struct memcmp with known-don't-care fields zeroed.
	// If any state I haven't enumerated above diverges, this fires with
	// a byte offset that can be mapped back to the field via offsetof.
	// Don't-care fields:
	//   - Mem / Micro: shared backing memory pointers
	//   - code: transient current opcode word, set per-pair
	//   - start_pc: program start, constant per VU program
	//   - nextBlockCycles: JIT block-dispatcher accounting, deliberately
	//     not maintained by interp's vu0Exec
	//   - idx: VU index, constant 0/1
	//   - fmac[*].xyzwupper/xyzwlower when the corresponding reg is 0
	//     (interp _vuRegsMTIR etc. leave VFwxyzw uninitialized — see
	//     normalization above for the per-slot check; same applied here).
	if (!diverged)
	{
		alignas(16) VURegs jitNorm;
		alignas(16) VURegs irefNorm;
		std::memcpy(&jitNorm, jit, sizeof(VURegs));
		std::memcpy(&irefNorm, iref, sizeof(VURegs));
		jitNorm.Mem = nullptr;          irefNorm.Mem = nullptr;
		jitNorm.Micro = nullptr;        irefNorm.Micro = nullptr;
		jitNorm.code = 0;               irefNorm.code = 0;
		jitNorm.start_pc = 0;           irefNorm.start_pc = 0;
		jitNorm.nextBlockCycles = 0;    irefNorm.nextBlockCycles = 0;
		jitNorm.idx = 0;                irefNorm.idx = 0;
		// VPU_STAT fully excluded from per-pair compare — see the VI loop
		// comment above. Bits 8-15 are VU1-owned and async-mutated under
		// MTVU; bits 0-7 are VU0-owned but program-lifecycle state that
		// drifts orthogonally to per-pair body execution. Block-level
		// shadow is the right scope for VPU_STAT correctness checking.
		// Zero both sides' full REG_VI slot (UL + 12 bytes padding) so
		// the catchall byte memcmp ignores all of VI[REG_VPU_STAT].
		std::memset(&jitNorm.VI[REG_VPU_STAT],  0, sizeof(REG_VI));
		std::memset(&irefNorm.VI[REG_VPU_STAT], 0, sizeof(REG_VI));
		for (int k = 0; k < 4; k++)
		{
			if (jitNorm.fmac[k].regupper == 0) jitNorm.fmac[k].xyzwupper = 0;
			if (jitNorm.fmac[k].reglower == 0) jitNorm.fmac[k].xyzwlower = 0;
			if (irefNorm.fmac[k].regupper == 0) irefNorm.fmac[k].xyzwupper = 0;
			if (irefNorm.fmac[k].reglower == 0) irefNorm.fmac[k].xyzwlower = 0;
		}
		const u8* jbytes = reinterpret_cast<const u8*>(&jitNorm);
		const u8* ibytes = reinterpret_cast<const u8*>(&irefNorm);
		for (size_t off = 0; off < sizeof(VURegs); off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				vu0_shadow_log(pc, "catchall_byte",
					"first divergent byte at offset=%zu jit=%02x interp=%02x "
					"(check VURegs layout in VU.h to map offset to field)",
					off, jbytes[off], ibytes[off]);
				diverged = true;
				break;
			}
		}
	}

	// VU0.Mem (4KB data memory) — separately allocated buffer not in VURegs.
	// JIT SQ/SQI/SQD/ISWR ops store here. s_shadow_post_mem holds the JIT's
	// post-pair Mem; VU0.Mem currently holds interp's post-pair Mem (interp
	// ran above on the pre-pair Mem we restored). First divergent offset →
	// the byte address of the buggy store; the SQ/SQI/SQD/ISWR pair that
	// targeted that quad/word is the offending lower codegen.
	if (!diverged && VU0.Mem)
	{
		const u8* jbytes = s_shadow_post_mem;
		const u8* ibytes = VU0.Mem;
		for (size_t off = 0; off < VU0_MEMSIZE; off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				// 16-byte (one quad) hex context for both sides — most
				// VU0 stores are quad-sized (SQ/SQI/SQD), and aligning
				// the dump to a 16-byte boundary makes the divergent
				// quad pop out of the line.
				const size_t base = off & ~size_t(0xf);
				char jhex[40] = {0};
				char ihex[40] = {0};
				int jn = 0, in_ = 0;
				for (size_t k = 0; k < 16; k++)
				{
					jn  += std::snprintf(jhex + jn,  sizeof(jhex) - jn,  "%02x", jbytes[base + k]);
					in_ += std::snprintf(ihex + in_, sizeof(ihex) - in_, "%02x", ibytes[base + k]);
				}
				vu0_shadow_log(pc, "VU0.Mem",
					"first divergent byte offset=0x%03zx jit=%02x interp=%02x  "
					"quad@0x%03zx jit=%s interp=%s",
					off, jbytes[off], ibytes[off], base, jhex, ihex);
				diverged = true;
				break;
			}
		}
	}

	// vif0Regs comparison — VEW (and any other VIF state mutated by VU0
	// ebit completion). Most pairs will have identical vif0Regs on both
	// sides; divergences point at ebit-termination semantic gaps.
	if (!diverged && std::memcmp(s_shadow_post_vif, &vif0Regs, sizeof(vif0Regs)) != 0)
	{
		const u8* jbytes = s_shadow_post_vif;
		const u8* ibytes = reinterpret_cast<const u8*>(&vif0Regs);
		for (size_t off = 0; off < sizeof(vif0Regs); off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				vu0_shadow_log(pc, "vif0Regs",
					"first divergent byte offset=%zu jit=%02x interp=%02x  "
					"(check VIFregisters layout in Vif.h to map offset to field; "
					"VEW is in stat bitfield)",
					off, jbytes[off], ibytes[off]);
				diverged = true;
				break;
			}
		}
	}

	// INTC_STAT — D/T-bit firing sets bit INTC_VU0 via hwIntcIrq. JIT
	// fires this POST-exec (step 11b); interp fires PRE-exec. End-of-pair
	// values should match if both fire the same bits, but a divergence
	// here indicates D/T-bit handling drift between paths.
	if (!diverged && s_shadow_post_intc != psHu32(INTC_STAT))
	{
		vu0_shadow_log(pc, "INTC_STAT",
			"jit=0x%08x interp=0x%08x", s_shadow_post_intc, psHu32(INTC_STAT));
		diverged = true;
	}

	// cpuRegs.nextEventCycle — cpuTestINTCInts (called from hwIntcIrq)
	// schedules an EE event 4 cycles in the future when INTC matches.
	// Different fire timing between JIT and interp lands the event at
	// different cycle counts, which cycles back into game timing.
	if (!diverged && s_shadow_post_next_event != cpuRegs.nextEventCycle)
	{
		vu0_shadow_log(pc, "cpuRegs.nextEventCycle",
			"jit=%llu interp=%llu",
			(unsigned long long)s_shadow_post_next_event,
			(unsigned long long)cpuRegs.nextEventCycle);
		diverged = true;
	}

	// Restore JIT result so the game continues with whatever JIT produced.
	// VURegs + Mem + external (vif0Regs / INTC_STAT / nextEventCycle):
	// interp's re-run mutated all of them; without restoring the VM would
	// proceed with a partial mix of interp's external state and JIT's
	// per-VU state, an incoherent combination that can hide real bugs
	// behind harness-side noise.
	std::memcpy(&VU0, s_shadow_post, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(VU0.Mem, s_shadow_post_mem, VU0_MEMSIZE);
	std::memcpy(&vif0Regs, s_shadow_post_vif, sizeof(vif0Regs));
	psHu32(INTC_STAT)         = s_shadow_post_intc;
	cpuRegs.nextEventCycle    = s_shadow_post_next_event;
}

// Block-level snapshot — emitted at block entry (linkEntry).
static void vu0_block_shadow_snapshot()
{
	if (s_shadow_diverged.load(std::memory_order_relaxed))
		return;
	// Reset the carryover flag for this block run. If the carryover gate
	// fires below, it'll set this back to true; otherwise it stays false
	// and the block verify proceeds normally.
	s_block_took_carryover = false;
	std::memcpy(s_block_shadow_pre, &VU0, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(s_block_shadow_pre_mem, VU0.Mem, VU0_MEMSIZE);
	std::memcpy(s_block_shadow_pre_vif, &vif0Regs, sizeof(vif0Regs));
	s_block_shadow_pre_intc       = psHu32(INTC_STAT);
	s_block_shadow_pre_next_event = cpuRegs.nextEventCycle;
}

// Block-level verify — emitted at block exit (early_exit / epilogue).
// Replays the same number of pairs as the JIT block ran, mirroring the
// outer `recArmVU0::Execute` loop's termination conditions, and compares
// end-of-block states.
//
// `startPC` is the JIT's compile-time block start PC (byte units within
// the program, masked to VU0_PROGSIZE). `numPairs` is the maximum number
// of pairs the JIT block was compiled for; the actual number executed
// may be less if the block early-exited (VPU_STAT cleared, MFLAGSET).
// The interp loop here uses the same termination conditions so it
// naturally stops at the same point.
static void vu0_block_shadow_verify(u32 startPC, u32 numPairs)
{
	if (s_shadow_diverged.load(std::memory_order_relaxed))
		return;

	// Carryover-branch gate fired: JIT exited after dispatching ONE pair
	// to vu0Exec instead of running numPairs pairs. The block verify's
	// "JIT-N-pairs vs interp-N-pairs from same pre-state" comparison
	// breaks here — the interp replay loop would run more pairs than
	// the JIT did, producing a known-divergent end state that's not a
	// real bug. Skip the comparison; live VU0 already holds JIT post-
	// state (we never ran the pre-restore + interp loop here).
	if (s_block_took_carryover)
		return;

	// Cycle-window gate: same as per-pair, applied at block exit so a
	// long boot doesn't drown in interp re-runs.
#if defined(VU0_SHADOW_VERIFY_FROM_CYCLE) || defined(VU0_SHADOW_VERIFY_TO_CYCLE)
	{
		const u64 cur = VU0.cycle;
#ifdef VU0_SHADOW_VERIFY_FROM_CYCLE
		if (cur < (VU0_SHADOW_VERIFY_FROM_CYCLE)) return;
#endif
#ifdef VU0_SHADOW_VERIFY_TO_CYCLE
		if (cur > (VU0_SHADOW_VERIFY_TO_CYCLE))   return;
#endif
	}
#endif

	// Capture post-JIT block state.
	std::memcpy(s_block_shadow_post, &VU0, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(s_block_shadow_post_mem, VU0.Mem, VU0_MEMSIZE);
	std::memcpy(s_block_shadow_post_vif, &vif0Regs, sizeof(vif0Regs));
	s_block_shadow_post_intc       = psHu32(INTC_STAT);
	s_block_shadow_post_next_event = cpuRegs.nextEventCycle;

	// Restore pre-block state for interp re-run.
	std::memcpy(&VU0, s_block_shadow_pre, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(VU0.Mem, s_block_shadow_pre_mem, VU0_MEMSIZE);
	std::memcpy(&vif0Regs, s_block_shadow_pre_vif, sizeof(vif0Regs));
	psHu32(INTC_STAT)         = s_block_shadow_pre_intc;
	cpuRegs.nextEventCycle    = s_block_shadow_pre_next_event;

	// Force TPC to the JIT's compile-time block startPC. Same reasoning
	// as the per-pair verify TPC patch — pre-block snapshot's TPC may
	// reflect an earlier block's tail when reached via direct-B link.
	VU0.VI[REG_TPC].UL = startPC;

	// Replay interp pair-by-pair, mirroring the termination conditions
	// from `InterpVU0::Execute` (`VU0microInterp.cpp:256-272`):
	//   - VPU_STAT bit 0 cleared → program ended via ebit; commit
	//     pending branch then break (matches Execute's outer-loop check).
	//   - MFLAGSET → M-bit fired; break (no branch commit needed).
	//   - Otherwise run vu0Exec for one pair.
	// numPairs caps the loop in case a JIT bug produced post-state with
	// VPU_STAT still set after the block exited — without the cap, this
	// would spin indefinitely in interp's pair execution.
	for (u32 i = 0; i < numPairs; i++)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
		{
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}
		if (VU0.flags & VUFLAG_MFLAGSET)
			break;
		vu0Exec(&VU0);
	}

	// Compare interp end-state (in live globals) vs JIT end-state
	// (in s_block_shadow_post*).
	bool diverged = false;

	// VURegs catchall — find first divergent byte. Layout matches the
	// per-pair catchall, with the same don't-care field zeroing.
	{
		alignas(16) VURegs jitNorm;
		alignas(16) VURegs irefNorm;
		std::memcpy(&jitNorm,  s_block_shadow_post, sizeof(VURegs));
		std::memcpy(&irefNorm, &VU0,                sizeof(VURegs));
		jitNorm.Mem = nullptr;          irefNorm.Mem = nullptr;
		jitNorm.Micro = nullptr;        irefNorm.Micro = nullptr;
		jitNorm.code = 0;               irefNorm.code = 0;
		jitNorm.start_pc = 0;           irefNorm.start_pc = 0;
		jitNorm.nextBlockCycles = 0;    irefNorm.nextBlockCycles = 0;
		jitNorm.idx = 0;                irefNorm.idx = 0;
		// VPU_STAT: block-level still verifies VU0-owned bits (0-7) for
		// cumulative correctness (e.g., did vu0EbitDone fire correctly
		// during the block?). VU1 bits 8-15 are mutated by MTVU thread
		// asynchronously and must be masked. Padding bytes 4..15 of the
		// 16-byte REG_VI slot are also zeroed so the catchall byte memcmp
		// doesn't fire on don't-care padding.
		jitNorm.VI[REG_VPU_STAT].UL  &= 0x000000FFu;
		irefNorm.VI[REG_VPU_STAT].UL &= 0x000000FFu;
		std::memset(reinterpret_cast<u8*>(&jitNorm.VI[REG_VPU_STAT])  + 4, 0, sizeof(REG_VI) - 4);
		std::memset(reinterpret_cast<u8*>(&irefNorm.VI[REG_VPU_STAT]) + 4, 0, sizeof(REG_VI) - 4);
		for (int k = 0; k < 4; k++)
		{
			if (jitNorm.fmac[k].regupper == 0) jitNorm.fmac[k].xyzwupper = 0;
			if (jitNorm.fmac[k].reglower == 0) jitNorm.fmac[k].xyzwlower = 0;
			if (irefNorm.fmac[k].regupper == 0) irefNorm.fmac[k].xyzwupper = 0;
			if (irefNorm.fmac[k].reglower == 0) irefNorm.fmac[k].xyzwlower = 0;
		}
		const u8* jbytes = reinterpret_cast<const u8*>(&jitNorm);
		const u8* ibytes = reinterpret_cast<const u8*>(&irefNorm);
		for (size_t off = 0; off < sizeof(VURegs); off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				// Map the divergent offset back to the nearest VURegs field
				// so the user doesn't have to do offsetof arithmetic by hand.
				// Walk a hardcoded list (kept in struct-declaration order) and
				// pick the deepest field whose start <= off.
				struct FieldRange { size_t off; const char* name; };
				const FieldRange fields[] = {
					{ offsetof(VURegs, VF),               "VF[0..31]" },
					{ offsetof(VURegs, VI),               "VI[0..31]" },
					{ offsetof(VURegs, ACC),              "ACC" },
					{ offsetof(VURegs, q),                "q" },
					{ offsetof(VURegs, p),                "p" },
					{ offsetof(VURegs, idx),              "idx" },
					{ offsetof(VURegs, cycle),            "cycle" },
					{ offsetof(VURegs, flags),            "flags" },
					{ offsetof(VURegs, code),             "code" },
					{ offsetof(VURegs, start_pc),         "start_pc" },
					{ offsetof(VURegs, branch),           "branch" },
					{ offsetof(VURegs, branchpc),         "branchpc" },
					{ offsetof(VURegs, delaybranchpc),    "delaybranchpc" },
					{ offsetof(VURegs, takedelaybranch),  "takedelaybranch" },
					{ offsetof(VURegs, ebit),             "ebit" },
					{ offsetof(VURegs, pending_q),        "pending_q" },
					{ offsetof(VURegs, pending_p),        "pending_p" },
					{ offsetof(VURegs, micro_macflags),   "micro_macflags[4]" },
					{ offsetof(VURegs, micro_clipflags),  "micro_clipflags[4]" },
					{ offsetof(VURegs, micro_statusflags),"micro_statusflags[4]" },
					{ offsetof(VURegs, macflag),          "macflag" },
					{ offsetof(VURegs, statusflag),       "statusflag" },
					{ offsetof(VURegs, clipflag),         "clipflag" },
					{ offsetof(VURegs, nextBlockCycles),  "nextBlockCycles" },
					{ offsetof(VURegs, Mem),              "Mem" },
					{ offsetof(VURegs, Micro),            "Micro" },
					{ offsetof(VURegs, xgkickaddr),       "xgkickaddr" },
					{ offsetof(VURegs, xgkickdiff),       "xgkickdiff" },
					{ offsetof(VURegs, xgkicksizeremaining),"xgkicksizeremaining" },
					{ offsetof(VURegs, xgkicklastcycle),  "xgkicklastcycle" },
					{ offsetof(VURegs, xgkickcyclecount), "xgkickcyclecount" },
					{ offsetof(VURegs, xgkickenable),     "xgkickenable" },
					{ offsetof(VURegs, xgkickendpacket),  "xgkickendpacket" },
					{ offsetof(VURegs, VIBackupCycles),   "VIBackupCycles" },
					{ offsetof(VURegs, VIOldValue),       "VIOldValue" },
					{ offsetof(VURegs, VIRegNumber),      "VIRegNumber" },
					{ offsetof(VURegs, fmac),             "fmac[0..3]" },
					{ offsetof(VURegs, fmacreadpos),      "fmacreadpos" },
					{ offsetof(VURegs, fmacwritepos),     "fmacwritepos" },
					{ offsetof(VURegs, fmaccount),        "fmaccount" },
					{ offsetof(VURegs, fdiv),             "fdiv" },
					{ offsetof(VURegs, efu),              "efu" },
					{ offsetof(VURegs, ialu),             "ialu[0..3]" },
					{ offsetof(VURegs, ialureadpos),      "ialureadpos" },
					{ offsetof(VURegs, ialuwritepos),     "ialuwritepos" },
					{ offsetof(VURegs, ialucount),        "ialucount" },
				};
				const FieldRange* match = &fields[0];
				for (const FieldRange& f : fields)
				{
					if (f.off <= off) match = &f;
					else break;
				}
				const size_t in_field = off - match->off;

				// 32-byte hex window centered (best-effort) on the divergent
				// byte, aligned to 16. Most VURegs fields are 4/8/16 bytes,
				// so a 32-byte window comfortably spans the field boundary
				// and shows the pattern of agreement/disagreement.
				const size_t win_start = (off >= 16) ? ((off - 16) & ~size_t(0xf)) : 0;
				const size_t win_end   = std::min(win_start + 32, sizeof(VURegs));
				char jhex[80] = {0};
				char ihex[80] = {0};
				int jn = 0, in_ = 0;
				for (size_t k = win_start; k < win_end; k++)
				{
					const char* sep = (k == off) ? "[" : (k == off + 1) ? "]" : "";
					jn  += std::snprintf(jhex + jn,  sizeof(jhex) - jn,
						"%s%02x", sep, jbytes[k]);
					in_ += std::snprintf(ihex + in_, sizeof(ihex) - in_,
						"%s%02x", sep, ibytes[k]);
				}

				vu0_block_shadow_log(startPC, numPairs, "BLOCK_VURegs",
					"first divergent byte offset=%zu (field=%s+0x%zx) "
					"jit=%02x interp=%02x\n"
					"  context win@0x%03zx jit=%s\n"
					"  context win@0x%03zx interp=%s",
					off, match->name, in_field, jbytes[off], ibytes[off],
					win_start, jhex, win_start, ihex);
				diverged = true;
				break;
			}
		}
	}

	// VU0.Mem (4KB) — block-level: any cumulative store divergence shows up
	// here even if individual pair stores agreed (e.g., one pair wrote
	// the wrong address but later pair overwrote it with the right value
	// from a wrong-but-coincident input).
	if (!diverged && VU0.Mem
	    && std::memcmp(VU0.Mem, s_block_shadow_post_mem, VU0_MEMSIZE) != 0)
	{
		const u8* jbytes = s_block_shadow_post_mem;
		const u8* ibytes = VU0.Mem;
		for (size_t off = 0; off < VU0_MEMSIZE; off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				const size_t base = off & ~size_t(0xf);
				char jhex[40] = {0};
				char ihex[40] = {0};
				int jn = 0, in_ = 0;
				for (size_t k = 0; k < 16; k++)
				{
					jn  += std::snprintf(jhex + jn,  sizeof(jhex) - jn,  "%02x", jbytes[base + k]);
					in_ += std::snprintf(ihex + in_, sizeof(ihex) - in_, "%02x", ibytes[base + k]);
				}
				vu0_block_shadow_log(startPC, numPairs, "BLOCK_Mem",
					"block startPC=0x%04x numPairs=%u first divergent byte "
					"offset=0x%03zx jit=%02x interp=%02x  "
					"quad@0x%03zx jit=%s interp=%s",
					startPC, numPairs, off, jbytes[off], ibytes[off],
					base, jhex, ihex);
				diverged = true;
				break;
			}
		}
	}

	// External state — vif0Regs / INTC_STAT / cpuRegs.nextEventCycle.
	// Same reasoning as per-pair: a divergence here points at D/T-bit
	// timing or ebit termination semantic gaps that compound across the
	// block.
	if (!diverged
	    && std::memcmp(s_block_shadow_post_vif, &vif0Regs, sizeof(vif0Regs)) != 0)
	{
		const u8* jbytes = s_block_shadow_post_vif;
		const u8* ibytes = reinterpret_cast<const u8*>(&vif0Regs);
		for (size_t off = 0; off < sizeof(vif0Regs); off++)
		{
			if (jbytes[off] != ibytes[off])
			{
				vu0_block_shadow_log(startPC, numPairs, "BLOCK_vif0Regs",
					"block startPC=0x%04x numPairs=%u first divergent byte "
					"offset=%zu jit=%02x interp=%02x",
					startPC, numPairs, off, jbytes[off], ibytes[off]);
				diverged = true;
				break;
			}
		}
	}
	if (!diverged && s_block_shadow_post_intc != psHu32(INTC_STAT))
	{
		vu0_block_shadow_log(startPC, numPairs, "BLOCK_INTC_STAT",
			"block startPC=0x%04x numPairs=%u jit=0x%08x interp=0x%08x",
			startPC, numPairs, s_block_shadow_post_intc, psHu32(INTC_STAT));
		diverged = true;
	}
	if (!diverged && s_block_shadow_post_next_event != cpuRegs.nextEventCycle)
	{
		vu0_block_shadow_log(startPC, numPairs, "BLOCK_nextEventCycle",
			"block startPC=0x%04x numPairs=%u jit=%llu interp=%llu",
			startPC, numPairs,
			(unsigned long long)s_block_shadow_post_next_event,
			(unsigned long long)cpuRegs.nextEventCycle);
		diverged = true;
	}

	// Restore JIT post-block state so the game continues with whatever
	// the JIT block produced.
	std::memcpy(&VU0, s_block_shadow_post, sizeof(VURegs));
	if (VU0.Mem)
		std::memcpy(VU0.Mem, s_block_shadow_post_mem, VU0_MEMSIZE);
	std::memcpy(&vif0Regs, s_block_shadow_post_vif, sizeof(vif0Regs));
	psHu32(INTC_STAT)         = s_block_shadow_post_intc;
	cpuRegs.nextEventCycle    = s_block_shadow_post_next_event;
}
#endif // VU0_SHADOW_VERIFY

// ============================================================================
//  Block analysis
// ============================================================================

static bool PairHasEbit(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);
	return (upper >> 30) & 1;
}

static bool PairHasBranch(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);
	if ((upper >> 31) & 1)
		return false;
	const u32 lower = *reinterpret_cast<const u32*>(VU0.Micro + pc);
	_VURegsNum lregs{};
	// Sub-table dispatchers (e.g. LowerOP) read VU0.code internally, so the
	// global must be primed — otherwise the LowerOP index selects from stale
	// state and can hit the Unknown slot (which pxFail-aborts in debug).
	VU0.code = lower;
	VU0regs_LOWER_OPCODE[lower >> 25](&lregs);
	return lregs.pipe == VUPIPE_BRANCH;
}

// Detect pairs containing an illegal/reserved UPPER opcode so we can
// truncate the block at them. Mirrors x86 microVU's `mVUcheckBadOp`
// (microVU_Compile.inl:225) — x86 sets `mVUinfo.isBadOp` when the upper
// analyze dispatcher (`mVUopU`) lands on `mVUunknown`. We mirror that by
// inspecting the UPPER opcode bits directly:
//
//   bits[5:0] (upper & 0x3F):
//     0x00-0x2F → defined upper ops (ADD/SUB/MADD/MSUB/MAX/MINI/MUL,
//                                    broadcast variants, scalar i/q,
//                                    OPMSUB)
//     0x30-0x3B → all `mVUunknown` slots in microVU_Tables.inl:135-137
//     0x3C-0x3F → FD sub-table dispatchers (further bit-pattern check
//                                          below)
//
//   FD sub-table indices (upper >> 6) & 0x1F:
//     0x00-0x0B → all defined (12 entries: ADDA/SUBA/MADDA/MSUBA + scalar
//                              + ITOF/FTOI + ABS/CLIP/NOP per fd_type),
//                 EXCEPT FD_11[10] which is `mVUunknown` (gap before NOP
//                 at index 11)
//     0x0C-0x1F → all `mVUunknown` slots (sub-table indices 12-31)
//
// I-bit pairs (upper bit 31) are exempted: when the I-bit is set, the
// upper word IS still a real opcode — `mVUopU` runs on it. Hmm wait — in
// x86 the I-bit doesn't change opcode dispatch, only what's done with the
// LOWER word. So the upper bad-op check fires regardless of I-bit. Don't
// exempt.
//
// The 0x8000033c exclusion mirrors x86's `mVU.code != 0x8000033c` — the
// BIOS writes a reversed NOP pair where 0x8000033c lands in the upper
// slot (a lower-format MOVE encoding decoded as upper, hence "unknown"
// from the upper analyzer's POV). x86 silently ignores this pair to
// avoid log spam; we do the same.
static bool PairHasBadOp(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);

	// BIOS reversed-NOP allowlist (x86 microVU honors the same).
	if (upper == 0x8000033c)
		return false;

	const u32 maj = upper & 0x3Fu;
	if (maj < 0x30u)
		return false;       // 0x00-0x2F: defined upper ops
	if (maj < 0x3Cu)
		return true;        // 0x30-0x3B: all unknown in mVU_UPPER_OPCODE

	// 0x3C-0x3F: FD sub-tables. Index sub-table by (upper >> 6) & 0x1F.
	const u32 idx = (upper >> 6) & 0x1Fu;
	if (idx >= 12u)
		return true;        // sub-table indices 12-31 are all unknown
	if (maj == 0x3Fu && idx == 10u)
		return true;        // FD_11[10] is the only sub-table gap

	return false;
}

static u32 AnalyzeBlock(u32 startPC)
{
	u32 pairs = 0;
	u32 pc    = startPC;

	while (pairs < VU0_MAX_BLOCK_PAIRS)
	{
		const bool ebit   = PairHasEbit(pc);
		const bool branch = PairHasBranch(pc);
		const bool bad_op = PairHasBadOp(pc);

		pairs++;
		const u32 next_pc = (pc + 8) & (VU0_PROGSIZE - 1);
		pc = next_pc;

		if (ebit || branch)
		{
			pairs++;

			// E-bit-in-branch-delay-slot detection (ISA-undefined; rare).
			// x86 microVU sets `mVUregs.blockType = 1` here so the next
			// block (entered via the branch target) is forced to single-
			// pair (microVU_Compile.inl::eBitWarning + endCount=1 at
			// line 696). arm64 doesn't propagate a blockType flag — but
			// the per-pair `Tbz vpu_stat,0 → early_exit` gate emitted
			// after every non-last pair (line ~1606) achieves the same
			// runtime effect: BlockB's first pair runs, its ebit
			// countdown calls vu0EbitDone (clears VPU_STAT bit 0), and
			// the gate immediately exits the block. So at most one pair
			// of the next block ever runs before retirement is observed
			// — equivalent to blockType=1's single-pair limit.
			//
			// Log when we detect this pattern so we know if it ever
			// matters in practice. If it shows up + we see corruption,
			// we can revisit and add full blockType=1 propagation
			// (variant cache key + numPairs override on link target).
			if (branch && PairHasEbit(next_pc))
			{
				DevCon.WriteLn("VU0: E-bit in branch delay slot at pc=0x%04x → "
					"0x%04x (rare; arm64 per-pair VPU_STAT gate handles, "
					"no blockType=1 propagation needed unless this surfaces "
					"a bug)",
					(pc - 16) & (VU0_PROGSIZE - 1), next_pc);
			}

			break;
		}
		// Bad op: include the current pair (still dispatches to interp) but
		// no delay slot, and truncate the block. Matches x86 microVU's
		// mVUinfo.isEOB on bad op. Benefit is earlier block boundary so we
		// re-enter dispatch and don't keep compiling past a definitely-bad
		// opcode.
		if (bad_op)
			break;
	}

	return pairs;
}

// ============================================================================
//  Block linking — patch helpers / link-exit analysis
// ============================================================================

struct BlockLinkExits
{
	u32  num_exits;
	u32  target_pcs[2];
	bool indirect;
};

static BlockLinkExits computeBlockLinkExits(u32 startPC, u32 numPairs)
{
	BlockLinkExits out = {};
	out.target_pcs[0] = LINK_TARGET_NONE;
	out.target_pcs[1] = LINK_TARGET_NONE;

	if (numPairs == 0)
		return out;

	// Max-size block: single fall-through.
	if (numPairs >= VU0_MAX_BLOCK_PAIRS)
	{
		out.num_exits     = 1;
		out.target_pcs[0] = (startPC + numPairs * 8u) & VU0_PROGMASK;
		return out;
	}

	// Branch- or ebit-terminated. Terminator at pair numPairs-2 (pair before
	// the delay slot).
	const u32 term_pc = (startPC + (numPairs - 2u) * 8u) & VU0_PROGMASK;
	const u32 upper   = *reinterpret_cast<const u32*>(VU0.Micro + term_pc + 4);

	if ((upper >> 30) & 1)
		return out; // ebit — no successor

	if ((upper >> 31) & 1)
		return out; // I-bit upper — analyze wouldn't have terminated here on a branch

	const u32 lower = *reinterpret_cast<const u32*>(VU0.Micro + term_pc);

	const s32 imm11 = (lower & 0x400u)
		? static_cast<s32>(0xFFFFFC00u | (lower & 0x3FFu))
		: static_cast<s32>(lower & 0x3FFu);
	const u32 tpc_val    = (term_pc + 8u) & VU0_PROGMASK;
	const u32 imm_target = (tpc_val + static_cast<u32>(imm11 * 8)) & VU0_PROGMASK;
	const u32 fallthrough_pc = (startPC + numPairs * 8u) & VU0_PROGMASK;

	if (isUnconditionalBranchOpVU0(lower))
	{
		out.num_exits     = 1;
		out.target_pcs[0] = imm_target;
		return out;
	}

	if (isConditionalBranchOpVU0(lower))
	{
		out.num_exits     = 2;
		out.target_pcs[0] = fallthrough_pc;  // not taken
		out.target_pcs[1] = imm_target;      // taken
		return out;
	}

	if (isIndirectBranchOpVU0(lower))
	{
		out.indirect = true;
		return out;
	}

	return out;
}

// Patch a single LinkExit's B-imm26 to jump to `target`. No-op when the
// site already points there. Single-thread: always called on the EE
// thread (recompile / Clear / dispatch all run there for VU0).
static void patchVU0LinkSite(LinkExit& exit, u8* target)
{
	if (!exit.patch_site)
		return;
	if (exit.current_target == target)
		return;
	armEmitJmpPtr(exit.patch_site, target, true);
	exit.current_target = target;
}

static void unpatchVU0LinkSite(LinkExit& exit)
{
	if (!exit.patch_site)
		return;
	patchVU0LinkSite(exit, exit.fallthrough);
}

// Content-keyed variant lookup. Scans the slot's deque for a variant whose
// snapshot matches the live VU0.Micro bytes at `pc`; MRU-bubbles a hit to
// the front. Miss → nullptr.
//
// Fast-reject on the first 8-byte compare before the full memcmp — most
// mismatches fail there. VU0.Micro is 16-byte aligned and slot PCs are
// 8-byte aligned, so the u64 load is well-defined.
static VU0BlockEntry* findVariantVU0(u32 pc)
{
	const u32 slot = pc / 8;
	if (slot >= VU0_NUM_SLOTS)
		return nullptr;
	auto& deque = s_vu0_variants[slot];
	if (deque.empty())
		return nullptr;

	const u8* live = VU0.Micro + pc;
	const u64 live_head = *reinterpret_cast<const u64*>(live);

	for (auto it = deque.begin(); it != deque.end(); ++it)
	{
		VU0BlockEntry* blk = *it;
		const u64 snap_head = *reinterpret_cast<const u64*>(blk->snapshot);
		if (snap_head != live_head)
			continue;
		const u32 snap_bytes = blk->numPairs * 8;
		if (snap_bytes > 8
			&& std::memcmp(blk->snapshot + 8, live + 8, snap_bytes - 8) != 0)
			continue;

		if (it != deque.begin())
		{
			deque.erase(it);
			deque.push_front(blk);
		}
		return blk;
	}
	return nullptr;
}

// Add this block to the reverse index for each unique exit target. Caller
// has already pushed the entry onto its slot's deque.
static void indexVU0VariantExits(VU0BlockEntry* blk)
{
	for (u32 e = 0; e < blk->num_exits; e++)
	{
		const u32 target_pc = blk->exits[e].target_pc;
		if (target_pc == LINK_TARGET_NONE)
			continue;
		bool dup = false;
		for (u32 j = 0; j < e; j++)
		{
			if (blk->exits[j].target_pc == target_pc)
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		const u32 target_slot = target_pc / 8;
		if (target_slot < VU0_NUM_SLOTS)
			s_vu0_waitingForSlot[target_slot].push_back(blk);
	}
}

// Right after a block compiles: for each live static exit, look up the
// target slot's variant cache for a snapshot-matching entry; if found and
// it has a linkEntry, wire our exit to it.
static void tryForwardLinkVU0(VU0BlockEntry& block)
{
	for (u32 e = 0; e < block.num_exits; e++)
	{
		LinkExit& exit = block.exits[e];
		if (exit.target_pc == LINK_TARGET_NONE)
			continue;
		VU0BlockEntry* target = findVariantVU0(exit.target_pc);
		if (target && target->linkEntry)
			patchVU0LinkSite(exit, target->linkEntry);
	}
}

// Walk the reverse index for `my_slot` and patch any predecessor exit whose
// target is `my_pc` to jump directly into `my_linkEntry`. Predecessors are
// stored as VU0BlockEntry* (multiple variants can target the same slot,
// and a slot can hold multiple variants too).
static void patchWaitingPredecessorsVU0(u32 my_pc, u8* my_linkEntry)
{
	if (!my_linkEntry)
		return;
	const u32 my_slot = my_pc / 8;
	if (my_slot >= VU0_NUM_SLOTS)
		return;
	for (VU0BlockEntry* pred : s_vu0_waitingForSlot[my_slot])
	{
		for (u32 e = 0; e < pred->num_exits; e++)
		{
			LinkExit& exit = pred->exits[e];
			if (exit.patch_site != nullptr
			    && exit.target_pc == my_pc
			    && exit.current_target != my_linkEntry)
			{
				patchVU0LinkSite(exit, my_linkEntry);
			}
		}
	}
}

// Detach and free a single variant. Caller has already removed it from
// s_vu0_variants[my_slot]; this routine drops it from the reverse index
// and frees the snapshot + entry. Compiled code in the JIT buffer is left
// in place — reclaimed at the next deleteAllVariantsVU0 (buffer-full
// reset / Reset / Shutdown).
//
// Predecessors that had patches pointing at this variant's linkEntry get
// reverted to fall-through so they don't jump into code that may be
// overwritten by a future compile occupying the same buffer space.
static void destroyVariantVU0(VU0BlockEntry* blk, u32 my_slot)
{
	if (blk->linkEntry && my_slot < VU0_NUM_SLOTS)
	{
		for (VU0BlockEntry* pred : s_vu0_waitingForSlot[my_slot])
		{
			if (pred == blk)
				continue;
			for (u32 e = 0; e < pred->num_exits; e++)
			{
				LinkExit& exit = pred->exits[e];
				if (exit.current_target == blk->linkEntry)
					unpatchVU0LinkSite(exit);
			}
		}
	}

	for (u32 e = 0; e < blk->num_exits; e++)
	{
		const u32 target_pc = blk->exits[e].target_pc;
		if (target_pc == LINK_TARGET_NONE)
			continue;
		bool dup = false;
		for (u32 j = 0; j < e; j++)
		{
			if (blk->exits[j].target_pc == target_pc)
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		const u32 target_slot = target_pc / 8;
		if (target_slot >= VU0_NUM_SLOTS)
			continue;
		auto& vec = s_vu0_waitingForSlot[target_slot];
		vec.erase(std::remove(vec.begin(), vec.end(), blk), vec.end());
	}

	delete[] blk->snapshot;
	delete blk;
}

// Wipe every cached variant in every slot. Called on buffer-full reset,
// Shutdown, and Reset. The compiled code buffer itself is reclaimed by
// the caller — this only frees the per-variant heap storage.
static void deleteAllVariantsVU0()
{
	for (u32 i = 0; i < VU0_NUM_SLOTS; i++)
	{
		for (VU0BlockEntry* blk : s_vu0_variants[i])
		{
			delete[] blk->snapshot;
			delete blk;
		}
		s_vu0_variants[i].clear();
		s_vu0_waitingForSlot[i].clear();
	}
}

// JR/JALR runtime dispatcher. Given the runtime TPC, look up the target
// slot's variant cache (content-match against live micro). On hit, the
// JIT-emitted indirect tail tail-Brs to the returned linkEntry. On miss
// (no compiled variant matches live micro), fall through to flushes+Ret;
// Execute's loop will dispatch + compile, and the next visit hits.
static u8* vu0_indirect_dispatch(u32 tpc)
{
	const u32 pc = tpc & VU0_PROGMASK;
	VU0BlockEntry* blk = findVariantVU0(pc);
	return blk ? blk->linkEntry : nullptr;
}

// ============================================================================
//  Block compilation
// ============================================================================

// VU0_BASE_REG = x23; defined near the top of this file, shared with the
// included aVU0_Upper.inl / aVU0_Lower.inl emitters.

// VU0_PROFILE_OPS scaffolding (toggle in arm64/InterpFlags.h). Same shape
// as the VU1 macros — register the emit cursor span around per-pair sections.
#ifdef VU0_PROFILE_OPS
	#define VU0_PERF_BEGIN(varname) const u8* varname = armGetCurrentCodePointer()
	#define VU0_PERF_END(varname, fmt, ...) do { \
		const u8* _vu0_pe_end = armGetCurrentCodePointer(); \
		if (_vu0_pe_end > (varname)) { \
			char _vu0_pe_name[64]; \
			std::snprintf(_vu0_pe_name, sizeof(_vu0_pe_name), fmt, ##__VA_ARGS__); \
			Perf::vu0.Register((varname), \
				static_cast<size_t>(_vu0_pe_end - (varname)), _vu0_pe_name); \
		} \
	} while (0)
#else
	#define VU0_PERF_BEGIN(varname) ((void)0)
	#define VU0_PERF_END(varname, fmt, ...) ((void)0)
#endif

static u8* CompileBlock(u32 startPC, u32 numPairs, VU0BlockEntry* out_block)
{
	const size_t data_size    = numPairs * 2 * sizeof(_VURegsNum);
	const size_t code_worst   = static_cast<size_t>(numPairs) * 512 + 64;
	const size_t total_needed = data_size + code_worst;

	if (static_cast<size_t>(s_code_end - s_code_write) < total_needed)
	{
		// Wipe every cached variant. Any predecessor's patched B would now
		// point at the recycled buffer region; nuking the cache forces a
		// fresh compile (which calls patchWaitingPredecessorsVU0 to re-link).
		// The incoming out_block is fresh (caller hasn't pushed it onto a
		// deque yet), so it survives.
		deleteAllVariantsVU0();
		s_code_write = s_code_base;
		s_pool.Reset();
	}

	u8* const data_base = s_code_write;
	_VURegsNum* const uregs_data = reinterpret_cast<_VURegsNum*>(data_base);
	_VURegsNum* const lregs_data = uregs_data + numPairs;
	
	_VURegsNum* const uregs_data_w = reinterpret_cast<_VURegsNum*>(HostSys::JitGetWritablePointer(data_base));
	_VURegsNum* const lregs_data_w = uregs_data_w + numPairs;

	std::memset(uregs_data_w, 0, data_size);

	{
		u32 pc = startPC;
		for (u32 i = 0; i < numPairs; i++)
		{
			const u32 upper = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);
			const u32 lower = *reinterpret_cast<const u32*>(VU0.Micro + pc);

			VU0.code = upper;
			VU0regs_UPPER_OPCODE[upper & 0x3f](&uregs_data_w[i]);

			if (!((upper >> 31) & 1))
			{
				VU0.code = lower;
				VU0regs_LOWER_OPCODE[lower >> 25](&lregs_data_w[i]);
			}

			pc = (pc + 8) & (VU0_PROGSIZE - 1);
		}
	}

	u8* code_start = data_base + data_size;
	code_start = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(code_start) + 3) & ~3ULL);

	// Static exit set for this block — drives the exit-selector emit below.
	const BlockLinkExits link_info = computeBlockLinkExits(startPC, numPairs);

	armSetAsmPtr(code_start, static_cast<size_t>(s_code_end - code_start), &s_pool);
	u8* const entry = armStartBlock();

	armAsm->Stp(x29, x30, MemOperand(sp, -32, PreIndex));
	armAsm->Stp(x22, VU0_BASE_REG, MemOperand(sp, 16));
	armAsm->Mov(x29, sp);
	armMoveAddressToReg(VU0_BASE_REG, &VU0);

	// Block-linking entry point. Linked predecessors B here directly,
	// skipping the prologue (their callee-saves are already saved). The
	// fall-through dispatch (codeEntry → linkEntry) handles the first
	// entry from Execute. No entry-gate: the existing per-pair VPU_STAT /
	// MFLAGSET check (after each non-last pair) handles intra-block
	// termination; cycle budget is enforced by Execute's outer loop when
	// the block returns.
	out_block->linkEntry = armGetCurrentCodePointer();

	// Compile-time scan: any pair with D-bit (28), T-bit (27), M-bit (29),
	// or E-bit (30) set on the upper makes block-level shadow comparison
	// unreliable. The block verify replays interp via vu0Exec's pre-exec
	// D/T-bit semantics; the JIT runs vu0CheckDTBits POST-exec (matches
	// x86 microVU). When a D/T pair's lower modifies VI[REG_FBRST], the
	// two paths read different FBRST values and diverge on whether D/T
	// fires — propagating to ebit / VPU_STAT bit 0 / cycle / pipe state.
	// M/E-bit pairs have analogous issues per `armsx2_vu0_shadow_verify.md`.
	//
	// For these blocks, skip both snapshot AND verify emits. The block
	// runs without shadow tracing; any divergence is a known-intentional
	// timing artifact, not a real JIT bug. The user can enable the
	// corresponding INTERP_VU0_DTBITS / EBIT / MBIT flags in InterpFlags.h
	// to route those pairs through fallback for a cleaner harness pass.
	//
	// Declared at function scope (not inside the VU0_SHADOW_VERIFY ifdef)
	// so both block-shadow emit sites — pre-exit-selector and
	// early_exit-bound — can gate on it. The scan and use are both
	// trivially skipped when shadow is off (the if-blocks are empty).
	bool block_has_dtme = false;
#ifdef VU0_SHADOW_VERIFY
	{
		u32 scan_pc = startPC;
		for (u32 i = 0; i < numPairs && !block_has_dtme; i++)
		{
			const u32 scan_upper = *reinterpret_cast<const u32*>(VU0.Micro + scan_pc + 4);
			// Bits 30=E, 29=M, 28=D, 27=T.
			if (scan_upper & 0x78000000u)
				block_has_dtme = true;
			scan_pc = (scan_pc + 8) & (VU0_PROGSIZE - 1);
		}
	}

	// Block-entry snapshot for the block-level shadow harness. Captures
	// VURegs / VU0.Mem / vif0Regs / INTC_STAT / cpuRegs.nextEventCycle so
	// the post-block verify (emitted at early_exit) can compare end states
	// against an interp re-run from this same point. Fires on EVERY block
	// entry — both the codeEntry fall-through (first dispatch) and direct-
	// linked predecessor jumps land here. Skipped for D/T/M/E blocks per
	// the scan above.
	if (!block_has_dtme)
		armEmitCall(reinterpret_cast<const void*>(vu0_block_shadow_snapshot));
#endif

	const int64_t cycle_off    = (int64_t)offsetof(VURegs, cycle);
	const int64_t code_off     = (int64_t)offsetof(VURegs, code);
	const int64_t branch_off   = (int64_t)offsetof(VURegs, branch);
	const int64_t branchpc_off = (int64_t)offsetof(VURegs, branchpc);
	const int64_t ebit_off     = (int64_t)offsetof(VURegs, ebit);
	const int64_t tpc_off      = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_TPC * (int64_t)sizeof(REG_VI));
	const int64_t regi_off     = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_I   * (int64_t)sizeof(REG_VI));
	const int64_t fmacwpos_off = (int64_t)offsetof(VURegs, fmacwritepos);
	const int64_t flags_off    = (int64_t)offsetof(VURegs, flags);
	const int64_t vpu_stat_off = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_VPU_STAT * (int64_t)sizeof(REG_VI));
	const int64_t micro_off    = (int64_t)offsetof(VURegs, Micro);

	// IbitHack forces per-op immediate decode from live micro memory (mirrors
	// x86 microVU's ptr32[&curI] reads). When on, VU->code is loaded from
	// VU->Micro[pc] at runtime instead of the JIT-baked instruction word so
	// subsequent C wrappers (LQ/SQ/ILW/ISW) pick up any post-compile patches.
	// Natively-emitted IADDI/IADDIU/ISUBIU consult EmuConfig directly and emit
	// runtime-decode paths of their own.
	const bool use_ibit_hack = EmuConfig.Gamefixes.IbitHack;

	// Epilogue label — jumped to when we need early exit mid-block
	Label early_exit;

	// CARRYOVER BRANCH GATE — corrects a long-standing JIT-vs-interp
	// divergence (MGS2 physics, "bullets through walls / can't descend
	// stairs"). Diagnosis via VU0_SHADOW_VERIFY at block startPC=0x0558:
	//
	// A prior block's branch lower set VU->branch=2, VU->branchpc=<target>.
	// That block ended at the branch pair (no internal delay slot — block
	// boundary), so VU->branch decremented to 1 in step 12 but didn't
	// fire. Control transferred to THIS block (target's block) with
	// VU->branch=1 still pending.
	//
	// The JIT compiles blocks as compile-time-sequential PC walks. With
	// VU->branch=1 at entry, pair 0's step 12 fires (branch=1→0,
	// TPC=branchpc). But pair 1's step 2 OVERWRITES TPC with compile-time
	// pc+8 — and pair 1's body executes opcodes baked at THAT compile-
	// time PC, NOT at branchpc. Sequential pairs 1..N-1 run opcodes the
	// program never intended at this point in execution. INTERP, in
	// contrast, reads TPC at runtime: after pair 0 fires, interp pair 1
	// reads ptr from Micro[branchpc] and runs the right opcodes. Off-by-
	// one in instruction execution → silent state drift → MGS2 physics.
	//
	// branch=2 (rarer, mid-delay-slot carryover) decrements to 1 on this
	// block's pair 0 step 12 without firing, then this block's pair 1
	// would fire — same divergence one pair later.
	//
	// Fix: at every block linkEntry, if VU->branch != 0, hand off ONE
	// pair to vu0Exec (which reads TPC at runtime and walks correctly),
	// then exit. The outer recArmVU0::Execute loop re-dispatches at the
	// resulting TPC. branch=1 case: vu0Exec fires, branch=0, dispatch at
	// branchpc. branch=2 case: vu0Exec decrements to 1, dispatch at
	// pc+8; that block's linkEntry hits this gate again with branch=1
	// and fires on the second iteration. Mirrors what INTERP_VU0_PAIR
	// does naturally — every pair runs through vu0Exec.
	//
	// Cost in the common case (branch=0 at entry): 2 insns (Ldr+Cbz). In
	// the carryover case: BL vu0Exec + jump to early_exit. Carryover is
	// rare; the gate is a wash on the hot path and correct on the cold
	// path.
	{
		Label normal_path;
		armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, branch_off));
		armAsm->Cbz(w4, &normal_path);
		armAsm->Mov(x0, VU0_BASE_REG);
#ifdef VU0_SHADOW_VERIFY
		// Shadow build: route through the dispatch wrapper so it sets
		// s_block_took_carryover before calling vu0Exec — block verify
		// at early_exit checks the flag and skips its compare.
		armEmitCall(reinterpret_cast<const void*>(vu0_block_carryover_dispatch));
#else
		armEmitCall(reinterpret_cast<const void*>(vu0Exec));
#endif
		armAsm->B(&early_exit);
		armAsm->Bind(&normal_path);
	}

	// Tracks whether the previous pair executed a branch op — feeds the
	// "is this pair a branch delay slot?" predicate for D/T bit suppression.
	// Mirrors mVUinfo.isBdelay in x86 microVU_Compile.inl:901.
	bool prev_was_branch = false;

	// Tracks whether the previous pair had its E-bit set — feeds the
	// "branch in E-bit delay slot?" predicate for branch suppression in
	// the lower emit path. Mirrors x86 microVU_Compile.inl branchWarning
	// which sets mVUlow.isNOP when mVUup.eBit && mVUbranch.
	bool prev_was_ebit = false;

	u32 pc = startPC;
	for (u32 i = 0; i < numPairs; i++)
	{
		const u32 upper     = *reinterpret_cast<const u32*>(VU0.Micro + pc + 4);
		const u32 lower     = *reinterpret_cast<const u32*>(VU0.Micro + pc);
		const bool ibit     = (upper >> 31) & 1;
		const bool ebit_set = (upper >> 30) & 1;
		const bool dbit_set = (upper >> 28) & 1;
		const bool tbit_set = (upper >> 27) & 1;
		const _VURegsNum& uregs = uregs_data[i];
		const _VURegsNum& lregs = lregs_data[i];

		// One-shot dump for the buggy pairs surfaced by the bisect.
		// Logs upper + lower opcode words plus the analyze-pass register
		// metadata so we can decode the pair without rebuilding with
		// VU0_SHADOW_VERIFY on. Remove once the bugs are fixed.
		// 0x04D8: MGS2 symptom #1 (single pair).
		// 0x0168 + 0x0170: MGS2 symptom #2 (interacting-pair bug).
		if (pc == 0x04D8u || pc == 0x0168u || pc == 0x0170u)
		{
			static std::atomic<u32> s_dumped_mask{0};
			const u32 bit = (pc == 0x04D8u) ? 1u : (pc == 0x0168u) ? 2u : 4u;
			const u32 prev = s_dumped_mask.fetch_or(bit, std::memory_order_acq_rel);
			if (!(prev & bit))
			{
				Console.WriteLn("VU0 BISECT TARGET pc=0x%04x: lower=0x%08x upper=0x%08x "
					"ibit=%d ebit=%d dbit=%d tbit=%d",
					pc, lower, upper, (int)ibit, (int)ebit_set, (int)dbit_set, (int)tbit_set);
				Console.WriteLn("  uregs: pipe=%u VFwrite=%u VFwxyzw=%u VFread0=%u VFr0xyzw=%u "
					"VFread1=%u VFr1xyzw=%u VIwrite=0x%08x VIread=0x%08x",
					(u32)uregs.pipe, (u32)uregs.VFwrite, (u32)uregs.VFwxyzw,
					(u32)uregs.VFread0, (u32)uregs.VFr0xyzw,
					(u32)uregs.VFread1, (u32)uregs.VFr1xyzw,
					uregs.VIwrite, uregs.VIread);
				Console.WriteLn("  lregs: pipe=%u VFwrite=%u VFwxyzw=%u VFread0=%u VFr0xyzw=%u "
					"VFread1=%u VFr1xyzw=%u VIwrite=0x%08x VIread=0x%08x",
					(u32)lregs.pipe, (u32)lregs.VFwrite, (u32)lregs.VFwxyzw,
					(u32)lregs.VFread0, (u32)lregs.VFr0xyzw,
					(u32)lregs.VFread1, (u32)lregs.VFr1xyzw,
					lregs.VIwrite, lregs.VIread);
			}
		}

		// Hazard detection — match every save/restore/discard case in
		// _vu0Exec (VU0microInterp.cpp:104-158). Anything that needs the
		// "lower sees pre-upper state" or "lower discarded" semantics must
		// fall back to vu0Exec because the native machinery does neither.
		//
		//   VF: upper writes vfX, lower also writes vfX        -> discard lower
		//   VF: upper writes vfX, lower reads  vfX             -> save/restore VF
		//   CLIP: upper writes CLIP, lower writes CLIP         -> discard lower
		//   CLIP: upper writes CLIP, lower reads  CLIP         -> save/restore CLIP
		//
		// Without the discard cases, the JIT runs upper then lower
		// sequentially: when both write the same VF, lower's value wins,
		// silently clobbering the upper FMAC result. This manifests as
		// vertex/shadow corruption in titles like SA where transform
		// pipelines pair `MUL vfX, ...` (upper) with `LQI vfX, ...` (lower).
		const bool vf_hazard = !ibit && uregs.VFwrite != 0 &&
			(lregs.VFwrite == uregs.VFwrite ||
			 lregs.VFread0 == uregs.VFwrite ||
			 lregs.VFread1 == uregs.VFwrite);
		const bool vi_hazard = !ibit &&
			(uregs.VIwrite & (1u << REG_CLIP_FLAG)) &&
			((lregs.VIwrite & (1u << REG_CLIP_FLAG)) ||
			 (lregs.VIread  & (1u << REG_CLIP_FLAG)));
		const bool mbit_set    = ((upper >> 29) & 1) != 0;
		const bool fmac_pipe   = (uregs.pipe == VUPIPE_FMAC) || (lregs.pipe == VUPIPE_FMAC);
		const bool branch_pipe = !ibit && (lregs.pipe == VUPIPE_BRANCH);

		// Group bisects (see arm64Emitter.h). When the corresponding aspect macro
		// is defined, any pair where the aspect applies falls back to vu0Exec for
		// the entire pair instead of running the per-pair native machinery.
		bool fallback = false;
#ifdef INTERP_VU0_PAIR
		fallback = true;
#endif
		// Hazard fallback is always on: native VF/CLIP_FLAG save/restore (the
		// thing _vu0Exec lines 104-158 / microVU does to make lower see pre-upper
		// values) is not yet implemented in this JIT. INTERP_VU0_HAZARD is kept as
		// a documentation handle but does not toggle behavior today.
		if (vf_hazard || vi_hazard) fallback = true;
#ifdef INTERP_VU0_MBIT
		if (mbit_set) fallback = true;
#endif
#ifdef INTERP_VU0_DTBITS
		if (dbit_set || tbit_set) fallback = true;
#endif
#ifdef INTERP_VU0_EBIT
		if (ebit_set) fallback = true;
#endif
#ifdef INTERP_VU0_BRANCH
		if (branch_pipe) fallback = true;
#endif
#if defined(INTERP_VU0_PC_LOW) && defined(INTERP_VU0_PC_HIGH)
		// TPC-range bisect: pairs in [LOW, HIGH] take the fallback path
		// so the user can binary-search the buggy pair when shadow-verify
		// is silent but INTERP_VU0_PAIR fixes the bug.
		if (pc >= (INTERP_VU0_PC_LOW) && pc <= (INTERP_VU0_PC_HIGH))
			fallback = true;
#endif
#if defined(INTERP_VU0_PC_LOW2) && defined(INTERP_VU0_PC_HIGH2)
		// Second TPC range — used to bisect a separate buggy pair while
		// keeping the first range pinned (e.g. MGS2 symptom #1 pinned at
		// 0x4D8 while bisecting symptom #2 across the rest of the program).
		if (pc >= (INTERP_VU0_PC_LOW2) && pc <= (INTERP_VU0_PC_HIGH2))
			fallback = true;
#endif

		// Runtime probe around the suspect pair (pc=0x4D8, MGS2 collision).
		// User runs once with INTERP_VU0_PC range covering 0x4D8 (forces
		// fallback path → physics OK), once without (forces native →
		// physics broken). Diff the PRE+POST dumps from each run; first
		// field that differs is the JIT's hidden side-effect.
		const bool probe_this_pair = false;
		if (probe_this_pair)
		{
			armAsm->Mov(x0, VU0_BASE_REG);
			armAsm->Mov(w1, pc);
			armAsm->Mov(w2, 0u); // tag = PRE
			armEmitCall(reinterpret_cast<const void*>(vu0ProbeState));
		}

		if (fallback)
		{
			// Per-pair fallback — call vu0Exec for the whole pair. vu0Exec
			// internally bumps cycle and advances TPC by 8 from its current
			// value, so the JIT must NOT touch cycle/TPC here.
			armAsm->Mov(x0, VU0_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(vu0Exec));
#ifdef VU0_SHADOW_VERIFY
			// Log fallback pairs at JIT-emit time so we know which pairs
			// the harness DOESN'T audit (snapshot/verify only fire on the
			// native branch). If `INTERP_VU0_PAIR` fixes a bug but the
			// harness stays silent, the buggy pair is almost certainly in
			// this list (or its native neighbor's wrong output silently
			// becomes the input to one of these fallback pairs and it
			// looks correct in isolation).
			Console.WriteLn("VU0 SHADOW: FALLBACK pair at pc=0x%04x  (vf_haz=%d vi_haz=%d "
				"mbit=%d dbit=%d tbit=%d ebit=%d branch=%d ibit=%d) "
				"upper=0x%08x lower=0x%08x",
				pc, (int)vf_hazard, (int)vi_hazard, (int)mbit_set,
				(int)dbit_set, (int)tbit_set, (int)ebit_set,
				(int)branch_pipe, (int)ibit, upper, lower);
#endif
		}
		else
		{
#ifdef VU0_SHADOW_VERIFY
			// Snapshot VU0 state before the native pair runs. Post-pair the
			// vu0_shadow_verify call will restore from this, run interp on
			// the same pair, and compare results.
			armEmitCall(reinterpret_cast<const void*>(vu0_shadow_snapshot));
#endif

			// 1. VU->cycle++
			armAsm->Ldr(x4, MemOperand(VU0_BASE_REG, cycle_off));
			armAsm->Mov(x22, x4);
			armAsm->Add(x4, x4, 1);
			armAsm->Str(x4, MemOperand(VU0_BASE_REG, cycle_off));

			// 2. Advance TPC
			const u32 new_tpc = (pc + 8) & VU0_PROGMASK;
			armAsm->Mov(w4, new_tpc);
			armAsm->Str(w4, MemOperand(VU0_BASE_REG, tpc_off));

			// 3. E-bit
			if (ebit_set)
			{
				armAsm->Mov(w4, 2u);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, ebit_off));
			}

			// 3b. M-bit (early-exit signal to EE — VU0 only)
			// Mirrors _vu0Exec at VU0microInterp.cpp:40-44 and microVU at
			// microVU_Compile.inl:892. Without this, the EE never observes
			// VUFLAG_MFLAGSET and waits forever for VU0 M-bit completion.
			if (mbit_set)
			{
				armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, flags_off));
				armAsm->Orr(w4, w4, VUFLAG_MFLAGSET);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, flags_off));
			}

			// 5. Upper stalls
			VU0_PERF_BEGIN(_pp_s5);
			armAsm->Mov(x0, VU0_BASE_REG);
			armMoveAddressToReg(x1, &uregs_data[i]);
			armEmitCall(reinterpret_cast<const void*>(_vuTestUpperStalls));
			VU0_PERF_END(_pp_s5, "VU0_TestUpper_0x%04x", pc);

			// 5b. Lower stalls
			VU0_PERF_BEGIN(_pp_s5b);
			if (!ibit)
			{
				armAsm->Mov(x0, VU0_BASE_REG);
				armMoveAddressToReg(x1, &lregs_data[i]);
				armEmitCall(reinterpret_cast<const void*>(_vuTestLowerStalls));
			}
			VU0_PERF_END(_pp_s5b, "VU0_TestLower_0x%04x", pc);

			// 6. Test pipes
			VU0_PERF_BEGIN(_pp_s6);
			armAsm->Mov(x0, VU0_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(_vuTestPipes));
			VU0_PERF_END(_pp_s6, "VU0_TestPipes_0x%04x", pc);

			// 6b. VIBackupCycles
			armAsm->Mov(x0, VU0_BASE_REG);
			armAsm->Mov(x1, x22);
			armEmitCall(reinterpret_cast<const void*>(vu0DecrementVIBackup));

			// 7. Upper instruction
			if (use_ibit_hack)
			{
				// Live read from micro memory so post-compile patches are visible.
				armAsm->Ldr(x5, MemOperand(VU0_BASE_REG, micro_off));
				armAsm->Ldr(w4, MemOperand(x5, (pc + 4)));
			}
			else
			{
				armAsm->Mov(w4, upper);
			}
			armAsm->Str(w4, MemOperand(VU0_BASE_REG, code_off));
			VU0.code = upper;
			VU0_PERF_BEGIN(_pp_s7);
			recVU0_UpperTable[upper & 0x3f]();
			VU0_PERF_END(_pp_s7, "VU0_U_%02x_0x%04x", upper & 0x3f, pc);

			// 8. Lower instruction
			// NOP the lower when this pair is a branch AND the previous pair
			// set E-bit — "branch in E-bit delay slot" is ISA-undefined.
			// Matches x86 microVU_Compile.inl branchWarning which flags
			// mVUlow.isNOP when the pair is both in an E-bit delay slot and
			// contains a branch. Upper still executes; we just skip the
			// branch rec emission so VU->branch / branchpc stay untouched.
			//
			// Also elide the entire lower emit scaffold (VU0.code store +
			// rec table dispatch) when the lower is a known-NOP on VU0
			// (EFU/XGKICK/XTOP/WAITP/WAITQ) — matches x86 microVU's
			// mVUlow.isNOP pass1 optimization. Saves ~8 bytes of code per
			// NOP op plus a runtime Mov+Str pair.
			const bool suppress_branch = !ibit && branch_pipe && prev_was_ebit;
			const bool lower_is_nop    = !ibit && isVU0LowerNOP(lower);
			if (ibit)
			{
				armAsm->Mov(w4, lower);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, regi_off));
			}
			else if (!lower_is_nop)
			{
				if (use_ibit_hack)
				{
					armAsm->Ldr(x5, MemOperand(VU0_BASE_REG, micro_off));
					armAsm->Ldr(w4, MemOperand(x5, pc));
				}
				else
				{
					armAsm->Mov(w4, lower);
				}
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, code_off));
				VU0.code = lower;
				VU0_PERF_BEGIN(_pp_s8);
				if (!suppress_branch)
					recVU0_LowerTable[lower >> 25]();
				VU0_PERF_END(_pp_s8, "VU0_L_%02x_0x%04x", lower >> 25, pc);
			}

			// 9. FMAC clear
			if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
			{
				armAsm->Mov(x0, VU0_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(_vuClearFMAC));
			}

			// 10. Upper stalls add
			armAsm->Mov(x0, VU0_BASE_REG);
			armMoveAddressToReg(x1, &uregs_data[i]);
			armEmitCall(reinterpret_cast<const void*>(_vuAddUpperStalls));

			// 11. Lower stalls add
			if (!ibit)
			{
				armAsm->Mov(x0, VU0_BASE_REG);
				armMoveAddressToReg(x1, &lregs_data[i]);
				armEmitCall(reinterpret_cast<const void*>(_vuAddLowerStalls));
			}

			// 11b. D/T bits — runs AFTER the op so the pair's side effects
			// on VPU_STAT/VI mem-mapped regs happen before the VPU_STAT bit
			// is set, matching x86 microVU_Compile.inl:900-910 which calls
			// mVUDoDBit/mVUDoTBit after mVUexecuteInstruction. D/T → ebit=1
			// is picked up by step 13's ebit countdown below in the same pair.
			//
			// Suppressed when the current pair is itself a branch or is in
			// a branch delay slot — matches x86's `!mVUinfo.isBdelay && !mVUlow.branch`
			// guard (ISA undefined behavior for D/T in these contexts).
			if ((dbit_set || tbit_set) && !branch_pipe && !prev_was_branch)
			{
				armAsm->Mov(w0, upper);
				armEmitCall(reinterpret_cast<const void*>(vu0CheckDTBits));
			}

			// 12. Branch countdown
			{
				Label skip_branch;
				armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, branch_off));
				armAsm->Cbz(w4, &skip_branch);
				armAsm->Subs(w4, w4, 1);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, branch_off));
				armAsm->B(&skip_branch, ne);
				armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, branchpc_off));
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, tpc_off));
				armAsm->Mov(x0, VU0_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(vu0HandleDelayBranch));
				armAsm->Bind(&skip_branch);
			}

			// 13. Ebit countdown
			{
				Label skip_ebit;
				armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, ebit_off));
				armAsm->Cbz(w4, &skip_ebit);
				armAsm->Subs(w4, w4, 1);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, ebit_off));
				armAsm->B(&skip_ebit, ne);
				armAsm->Mov(x0, VU0_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(vu0EbitDone));
				armAsm->Bind(&skip_ebit);
			}

			// 14. FMAC write-position advance
			if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
			{
				armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, fmacwpos_off));
				armAsm->Add(w4, w4, 1);
				armAsm->And(w4, w4, 3);
				armAsm->Str(w4, MemOperand(VU0_BASE_REG, fmacwpos_off));
			}

#ifdef VU0_SHADOW_VERIFY
			// Run interp on the snapshot, compare to JIT result, log first
			// divergent field. pc baked in compile-time so the log identifies
			// the offending pair.
			armAsm->Mov(w0, pc);
			armEmitCall(reinterpret_cast<const void*>(vu0_shadow_verify));
#endif
		}

		// POST-pair runtime probe — see PRE comment above.
		if (probe_this_pair)
		{
			armAsm->Mov(x0, VU0_BASE_REG);
			armAsm->Mov(w1, pc);
			armAsm->Mov(w2, 1u); // tag = POST
			armEmitCall(reinterpret_cast<const void*>(vu0ProbeState));
		}

		pc = (pc + 8) & (VU0_PROGSIZE - 1);

		// Track branch for next pair's D/T bit suppression (step 11b).
		// Updated regardless of fallback vs native path because the delay
		// slot's D/T check must be suppressed either way.
		prev_was_branch = branch_pipe;

		// Track E-bit for next pair's branch suppression (step 8).
		// Same reasoning: delay-slot context applies regardless of path.
		prev_was_ebit = ebit_set;

		// After each pair (except last): check VPU_STAT and MFLAGSET
		if (i < numPairs - 1)
		{
			armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, vpu_stat_off));
			armAsm->Tbz(w4, 0, &early_exit);
			armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, flags_off));
			armAsm->Tbnz(w4, 1, &early_exit);
		}
	}

#ifdef VU0_SHADOW_VERIFY
	// Block-exit verify (natural-end-of-block path). Linked exits in the
	// exit selector below jump directly to the successor's linkEntry,
	// skipping the early_exit label entirely — so the verify at
	// early_exit only fires for mid-block early-exit and unpatched
	// fall-through paths. Emitting verify HERE (before the exit selector)
	// catches every linked / indirect / num_exits=0 path. Mid-block
	// early-exits skip this and rely on the early_exit-bound verify.
	//
	// Double-fire is benign: if the unpatched fall-through reaches
	// early_exit, the second verify captures the same JIT-post state we
	// just restored, runs interp on the same pre-block snapshot, and
	// produces the same comparison. Pure wasted work, no correctness
	// impact. Once link patching kicks in, only this verify fires.
	if (!block_has_dtme)
	{
		armAsm->Mov(w0, startPC);
		armAsm->Mov(w1, numPairs);
		armEmitCall(reinterpret_cast<const void*>(vu0_block_shadow_verify));
	}
#endif

	// ----------------------------------------------------------------
	// Exit selector + linkable patch slots (block linking).
	//
	// Layout when num_exits == 1 (B/BAL or max-size fall-through):
	//   returnExit:
	//     [patch_site] B flushes      (← rewritten to successor's linkEntry
	//                                    by tryForwardLink / patchWaitingPredecessors)
	//   flushes:
	//     fall through to early_exit (Ret)
	//
	// Layout when num_exits == 2 (conditional IBxx — both targets known):
	//   returnExit:
	//     Ldr w4, [VU, tpc_off]
	//     Mov w5, taken_target_pc
	//     Cmp w4, w5
	//     B.ne use_not_taken               (hardcoded — NOT a patch slot)
	//     [patch_taken]    B flushes       (exits[1])
	//   use_not_taken:
	//     [patch_not_taken] B flushes      (exits[0])
	//   flushes:
	//     fall through to early_exit (Ret)
	//
	// Layout when indirect (JR/JALR):
	//   returnExit:
	//     Ldr w0, [VU, tpc_off]
	//     Bl vu0_indirect_dispatch     (returns target linkEntry in x0, or null)
	//     Cbz x0, &flushes
	//     Br  x0                       (tail-jump into successor's linkEntry)
	//   flushes:
	//     fall through to early_exit (Ret)
	//
	// Layout when num_exits == 0 and !indirect (ebit-terminated etc.):
	//   No selector — fall directly through to early_exit.
	// ----------------------------------------------------------------
	out_block->returnExit = armGetCurrentCodePointer();
	out_block->num_exits  = link_info.num_exits;
	for (u32 e = 0; e < 2; e++)
	{
		out_block->exits[e].target_pc      = link_info.target_pcs[e];
		out_block->exits[e].patch_site     = nullptr;
		out_block->exits[e].fallthrough    = nullptr;
		out_block->exits[e].current_target = nullptr;
	}

	if (link_info.num_exits == 1)
	{
		u8* patch = armGetCurrentCodePointer();
		Label flushes;
		armAsm->B(&flushes);
		armAsm->Bind(&flushes);
		out_block->exits[0].patch_site     = patch;
		out_block->exits[0].fallthrough    = patch + 4;
		out_block->exits[0].current_target = patch + 4;
	}
	else if (link_info.num_exits == 2)
	{
		armAsm->Ldr(w4, MemOperand(VU0_BASE_REG, tpc_off));
		armAsm->Mov(w5, link_info.target_pcs[1]);
		armAsm->Cmp(w4, w5);

		Label use_not_taken_path;
		Label flushes;
		armAsm->B(&use_not_taken_path, ne);

		u8* patch_taken = armGetCurrentCodePointer();
		armAsm->B(&flushes);                 // exits[1] (taken)

		armAsm->Bind(&use_not_taken_path);
		u8* patch_not_taken = armGetCurrentCodePointer();
		armAsm->B(&flushes);                 // exits[0] (not-taken)

		armAsm->Bind(&flushes);

		out_block->exits[0].patch_site     = patch_not_taken;
		out_block->exits[0].fallthrough    = patch_not_taken + 4;
		out_block->exits[0].current_target = patch_not_taken + 4;

		out_block->exits[1].patch_site     = patch_taken;
		out_block->exits[1].fallthrough    = patch_taken + 8;
		out_block->exits[1].current_target = patch_taken + 8;
	}
	else if (link_info.indirect)
	{
		// JR/JALR: compute target at runtime, dispatch via lookup. On
		// hit, tail-Br to successor's linkEntry. On miss, fall through
		// to flushes (Execute loop will re-dispatch, compile, and the
		// next visit hits the populated cache).
		Label flushes;
		armAsm->Ldr(w0, MemOperand(VU0_BASE_REG, tpc_off));
		armEmitCall(reinterpret_cast<const void*>(vu0_indirect_dispatch));
		armAsm->Cbz(x0, &flushes);
		armAsm->Br(x0);
		armAsm->Bind(&flushes);
	}
	// num_exits == 0 + !indirect: ebit terminated — fall through to epilogue.

	// Epilogue (also used as early-exit target when MFLAGSET or VPU_STAT fires)
	armAsm->Bind(&early_exit);

#ifdef VU0_SHADOW_VERIFY
	// Block-exit verify — compares JIT post-block state vs an interp
	// re-run from the pre-block snapshot for the same number of pairs.
	// Catches cumulative drift that per-pair shadow can't see (per-pair
	// silence + same-input determinism implies block-level silence,
	// UNLESS there's per-pair state we didn't snapshot). Fires on BOTH
	// natural block exit (last pair done) AND mid-block early-exit
	// (VPU_STAT cleared / MFLAGSET fired) — interp's loop mirrors the
	// same termination conditions so it stops at the same point.
	// Skipped for D/T/M/E-bit blocks (see scan + block_has_dtme above).
	if (!block_has_dtme)
	{
		armAsm->Mov(w0, startPC);
		armAsm->Mov(w1, numPairs);
		armEmitCall(reinterpret_cast<const void*>(vu0_block_shadow_verify));
	}
#endif

	armAsm->Ldp(x22, VU0_BASE_REG, MemOperand(sp, 16));
	armAsm->Ldp(x29, x30, MemOperand(sp, 32, PostIndex));
	armAsm->Ret();

	u8* end = armEndBlock();
	s_code_write = end;

	// Register the compiled VU0 block with simpleperf/perfetto so the JIT'd
	// code shows up as `VU0_<startPC>` in profiler reports instead of
	// "unknown unknown". Cost: one map insert per block compile.
	Perf::vu0.RegisterPC(entry, static_cast<size_t>(end - entry), startPC);

	return entry;
}

// ============================================================================
//  recArmVU0
// ============================================================================

recArmVU0::recArmVU0()
{
	m_Idx = 0;
	IsInterpreter = false;
}

void recArmVU0::Reserve()
{
	u8* const buf     = SysMemory::GetVU0Rec();
	u8* const buf_end = SysMemory::GetVU0RecEnd();

	s_pool.Init(buf, POOL_SIZE);
	s_code_base  = buf + POOL_SIZE;
	s_code_write = s_code_base;
	s_code_end   = buf_end;

	deleteAllVariantsVU0();
}

void recArmVU0::Shutdown()
{
	s_pool.Destroy();
	s_code_base  = nullptr;
	s_code_write = nullptr;
	s_code_end   = nullptr;
	deleteAllVariantsVU0();
}

void recArmVU0::Reset()
{
	VU0.fmacwritepos = 0;
	VU0.fmacreadpos  = 0;
	VU0.fmaccount    = 0;
	VU0.ialuwritepos = 0;
	VU0.ialureadpos  = 0;
	VU0.ialucount    = 0;

	deleteAllVariantsVU0();
	if (s_code_base)
		s_code_write = s_code_base;
	s_pool.Reset();
}

void recArmVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void recArmVU0::Step()
{
	VU0.VI[REG_TPC].UL &= VU0_PROGMASK;
	vu0Exec(&VU0);
}

void recArmVU0::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

	VU0.VI[REG_TPC].UL <<= 3;
	VU0.flags &= ~VUFLAG_MFLAGSET;
	const u64 startcycles = VU0.cycle;

	while ((VU0.cycle - startcycles) < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
		{
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}
		if (VU0.flags & VUFLAG_MFLAGSET)
			break;

		const u32 pc   = VU0.VI[REG_TPC].UL & (VU0_PROGSIZE - 1);
		const u32 slot = pc / 8;

		// Content-keyed lookup: scan the slot's deque for a variant whose
		// snapshot matches live VU0.Micro at `pc`. A hit bubbles the variant
		// to the deque front (MRU) so subsequent dispatches find it first.
		VU0BlockEntry* blk = findVariantVU0(pc);

		if (!blk)
		{
			// Miss — compile a new variant. Allocate first so the entry is
			// stable while CompileBlock writes linkEntry/exits/etc. into it.
			const u32 numPairs = AnalyzeBlock(pc);
			blk = new VU0BlockEntry{};
			blk->numPairs = numPairs;

			// Snapshot the bytecode this variant compiles against so future
			// dispatches can content-match against it even after Clear()
			// rewrites live VU0.Micro and (eventually) the EE re-uploads
			// the same program. This is what survives the MGS2 thrash
			// pattern.
			const u32 snap_bytes = numPairs * 8;
			blk->snapshot = new u8[snap_bytes];
			std::memcpy(blk->snapshot, VU0.Micro + pc, snap_bytes);

			blk->codeEntry = CompileBlock(pc, numPairs, blk);

			// Cap the per-slot deque. Evict LRU (back) before pushing the
			// new variant; destroyVariantVU0 unpatches predecessors that
			// were linked to the evicted variant so they fall through to
			// the dispatcher (then patchWaitingPredecessorsVU0 below
			// re-links them to the new MRU variant).
			//
			// Eviction must happen BEFORE indexVU0VariantExits so the
			// destroyVariant walk of s_vu0_waitingForSlot[slot] doesn't
			// see the new variant as a predecessor candidate of itself.
			auto& deque = s_vu0_variants[slot];
			if (deque.size() >= kVU0VariantCapPerSlot)
			{
				VU0BlockEntry* victim = deque.back();
				deque.pop_back();
				destroyVariantVU0(victim, slot);
			}

			deque.push_front(blk);
			indexVU0VariantExits(blk);

			// Block-link wiring for the freshly-compiled variant.
			tryForwardLinkVU0(*blk);
			patchWaitingPredecessorsVU0(pc, blk->linkEntry);
		}
		else if (blk->needsRelink)
		{
			// Variant survived a Clear() that unpatched its incoming exit
			// edges — re-wire the graph lazily on the first dispatch
			// post-Clear so repeated hits pay this cost only once.
			tryForwardLinkVU0(*blk);
			patchWaitingPredecessorsVU0(pc, blk->linkEntry);
			blk->needsRelink = false;
		}

		using BlockFn = void (*)();
		reinterpret_cast<BlockFn>(blk->codeEntry)();
	}

	VU0.VI[REG_TPC].UL >>= 3;

	// Skip cycle scaling when either sync gamefix is active and EECycleRate is
	// positive — both VUSyncHack and FullVU0SyncHack tighten VU0 sync, and
	// scaling cycles down would defeat the sync. Matches x86 microVU which
	// treats the two flags as equivalent across microVU_{Compile,Branch,Macro}.inl.
	const bool tight_sync = EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack;
	if (EmuConfig.Speedhacks.EECycleRate != 0 && (!tight_sync || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		u64 cycle_change = VU0.cycle - startcycles;
		VU0.cycle -= cycle_change;
		switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(cycle_change)))
		{
			case -3: cycle_change = static_cast<u64>(cycle_change * 2.0f);       break;
			case -2: cycle_change = static_cast<u64>(cycle_change * 1.6666667f); break;
			case -1: cycle_change = static_cast<u64>(cycle_change * 1.3333333f); break;
			case  1: cycle_change = static_cast<u64>(cycle_change / 1.3f);       break;
			case  2: cycle_change = static_cast<u64>(cycle_change / 1.8f);       break;
			case  3: cycle_change = static_cast<u64>(cycle_change / 3.0f);       break;
			default: break;
		}
		VU0.cycle += cycle_change;
	}

	VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
}

void recArmVU0::Clear(u32 addr, u32 size)
{
	// Slot-keyed invalidation. Clear() is called on very hot paths (VIF MPG
	// uploads, VU0 DMA, SPR DMA, every CPU store to VU memory), so any
	// per-call work shows up in frame budget.
	//
	// History:
	//   - 4af448458 "fix: VU0 clear()" — full-scan span-aware. Caused
	//     graphical dropouts + lag in Twinsanity (512 iterations per call).
	//   - 8c0f6b240 "fix: regressions" — bounded span-aware (~130 iterations
	//     per call). Still cost substantial perf across many games.
	//
	// Both attempts were chasing an audit-speculative stale-block bug —
	// "blocks starting before the patched range whose span reaches into
	// it" — that was NEVER confirmed in any actual game. Real-world VU
	// memory patches are either full-program reloads (caught by slot-keyed
	// invalidation since all block start PCs land in the clear range) or
	// writes to unused data regions (no block cares). The theoretical
	// scenario requires mid-program patches to hot code, which games
	// overwhelmingly avoid.
	//
	// x86 microVU solves the cross-block case via program-level hashing
	// (mVU.prog.isSame / mVUcacheProg), a completely different architecture
	// that we don't mirror. If a real game ever pins down the stale-block
	// scenario, revisit — but don't pay the scan cost speculatively.
	const u32 first        = addr / 8;
	const u32 last         = (addr + size + 7) / 8;
	const u32 clamped_last = std::min(last, VU0_NUM_SLOTS);

	if (first >= VU0_NUM_SLOTS)
		return;

	// Block-linking invalidation: any predecessor that has a B patched to a
	// linkEntry inside the cleared range needs that exit reverted to its
	// fallthrough — otherwise the predecessor's next execution jumps into
	// code that may be overwritten by a fresh compile occupying the same
	// buffer region. Walk the reverse index for each cleared slot.
	for (u32 ts = first; ts < clamped_last; ts++)
	{
		for (VU0BlockEntry* pred : s_vu0_waitingForSlot[ts])
		{
			for (u32 e = 0; e < pred->num_exits; e++)
			{
				LinkExit& exit = pred->exits[e];
				if (!exit.patch_site || exit.target_pc == LINK_TARGET_NONE)
					continue;
				const u32 target_slot = exit.target_pc / 8;
				if (target_slot >= first && target_slot < clamped_last)
					unpatchVU0LinkSite(exit);
			}
		}
	}

	// Mark variants in the cleared range as needing relink on next dispatch.
	// We deliberately do NOT delete them: if the EE re-uploads identical
	// bytes later (the MGS2 thrash pattern), findVariantVU0 will match
	// against the preserved snapshot and reuse the compiled code without
	// re-emitting. On the first post-Clear dispatch, `needsRelink` triggers
	// tryForwardLinkVU0 + patchWaitingPredecessorsVU0 to re-wire exits.
	for (u32 i = first; i < clamped_last; i++)
	{
		for (VU0BlockEntry* blk : s_vu0_variants[i])
			blk->needsRelink = true;
	}
}
