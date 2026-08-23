// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Native ARM64 generators for the R5900's 128-bit MMI instructions.

static_assert(iREGCNT_XMM <= 29, "MMI scratch registers overlap the EE SIMD cache");

static __fi a64::VRegister mmiQ(int reg)
{
	return a64::QRegister(reg);
}

static __fi void mmiFinish()
{
	_clearNeededXMMregs();
}

#define REC_MMI_BINARY(NAME, OP, VIEW)                                                        \
	void rec##NAME()                                                                          \
	{                                                                                         \
		if (!_Rd_)                                                                            \
			return;                                                                           \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                                  \
		const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);  \
		armAsm->OP(mmiQ(EEREC_D).VIEW(), mmiQ(EEREC_S).VIEW(), mmiQ(EEREC_T).VIEW());         \
		mmiFinish();                                                                          \
	}

// Pack/interleave instructions place Rt in the low/even result lanes.
#define REC_MMI_BINARY_TS(NAME, OP, VIEW)                                                     \
	void rec##NAME()                                                                          \
	{                                                                                         \
		if (!_Rd_)                                                                            \
			return;                                                                           \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                                  \
		const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);  \
		armAsm->OP(mmiQ(EEREC_D).VIEW(), mmiQ(EEREC_T).VIEW(), mmiQ(EEREC_S).VIEW());         \
		mmiFinish();                                                                          \
	}

#define REC_MMI_FALLBACK(NAME, INVALIDATE) \
	REC_FUNC_DEL(NAME, INVALIDATE)

// Base MMI group

void recPLZCW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PLZCW);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READD | XMMINFO_WRITED);
	const a64::VRegister tmp = a64::q29;
	armAsm->Cls(tmp.V4S(), mmiQ(EEREC_S).V4S());
	// PLZCW only writes UL[0..1]
	armAsm->Ins(mmiQ(EEREC_D).V2D(), 0, tmp.V2D(), 0);
	mmiFinish();
}

void recPMFHL()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMFHL);
	// SLW has signed 64-bit clamp semantics
	if (_Sa_ == 0x02 || _Sa_ > 0x04)
	{
		_deleteEEreg(_Rd_, 1);
		recCall(Interp::PMFHL);
		return;
	}

	const int info = eeRecompileCodeXMM(XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITED);
	const a64::VRegister d = a64::q29;
	const a64::VRegister lo_narrow = a64::q30;
	const a64::VRegister hi_narrow = a64::q31;

	switch (_Sa_)
	{
		case 0x00: // LW: LO[0], HI[0], LO[2], HI[2]
			armAsm->Ins(d.V4S(), 0, mmiQ(EEREC_LO).V4S(), 0);
			armAsm->Ins(d.V4S(), 1, mmiQ(EEREC_HI).V4S(), 0);
			armAsm->Ins(d.V4S(), 2, mmiQ(EEREC_LO).V4S(), 2);
			armAsm->Ins(d.V4S(), 3, mmiQ(EEREC_HI).V4S(), 2);
			break;

		case 0x01: // UW: LO[1], HI[1], LO[3], HI[3]
			armAsm->Ins(d.V4S(), 0, mmiQ(EEREC_LO).V4S(), 1);
			armAsm->Ins(d.V4S(), 1, mmiQ(EEREC_HI).V4S(), 1);
			armAsm->Ins(d.V4S(), 2, mmiQ(EEREC_LO).V4S(), 3);
			armAsm->Ins(d.V4S(), 3, mmiQ(EEREC_HI).V4S(), 3);
			break;

		case 0x03: // LH: even halfwords from LO/HI
			armAsm->Ins(d.V8H(), 0, mmiQ(EEREC_LO).V8H(), 0);
			armAsm->Ins(d.V8H(), 1, mmiQ(EEREC_LO).V8H(), 2);
			armAsm->Ins(d.V8H(), 2, mmiQ(EEREC_HI).V8H(), 0);
			armAsm->Ins(d.V8H(), 3, mmiQ(EEREC_HI).V8H(), 2);
			armAsm->Ins(d.V8H(), 4, mmiQ(EEREC_LO).V8H(), 4);
			armAsm->Ins(d.V8H(), 5, mmiQ(EEREC_LO).V8H(), 6);
			armAsm->Ins(d.V8H(), 6, mmiQ(EEREC_HI).V8H(), 4);
			armAsm->Ins(d.V8H(), 7, mmiQ(EEREC_HI).V8H(), 6);
			break;

		case 0x04: // SH: signed-saturate LO/HI words to halfwords
			armAsm->Sqxtn(lo_narrow.V4H(), mmiQ(EEREC_LO).V4S());
			armAsm->Sqxtn(hi_narrow.V4H(), mmiQ(EEREC_HI).V4S());
			// Ins requires matching vector formats; Sqxtn populated lanes 0..3.
			armAsm->Ins(d.V8H(), 0, lo_narrow.V8H(), 0);
			armAsm->Ins(d.V8H(), 1, lo_narrow.V8H(), 1);
			armAsm->Ins(d.V8H(), 2, hi_narrow.V8H(), 0);
			armAsm->Ins(d.V8H(), 3, hi_narrow.V8H(), 1);
			armAsm->Ins(d.V8H(), 4, lo_narrow.V8H(), 2);
			armAsm->Ins(d.V8H(), 5, lo_narrow.V8H(), 3);
			armAsm->Ins(d.V8H(), 6, hi_narrow.V8H(), 2);
			armAsm->Ins(d.V8H(), 7, hi_narrow.V8H(), 3);
			break;
	}

	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

void recPMTHL()
{
	EE::Profiler.EmitOp(eeOpcode::PMTHL);
	if (_Sa_ != 0)
		return;

	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READLO | XMMINFO_WRITELO |
		XMMINFO_READHI | XMMINFO_WRITEHI);
	armAsm->Ins(mmiQ(EEREC_LO).V4S(), 0, mmiQ(EEREC_S).V4S(), 0);
	armAsm->Ins(mmiQ(EEREC_HI).V4S(), 0, mmiQ(EEREC_S).V4S(), 1);
	armAsm->Ins(mmiQ(EEREC_LO).V4S(), 2, mmiQ(EEREC_S).V4S(), 2);
	armAsm->Ins(mmiQ(EEREC_HI).V4S(), 2, mmiQ(EEREC_S).V4S(), 3);
	mmiFinish();
}

#define REC_MMI_IMM_SHIFT(NAME, OP, VIEW, MASK)                                       \
	void rec##NAME()                                                                  \
	{                                                                                 \
		if (!_Rd_)                                                                    \
			return;                                                                   \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                          \
		const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);          \
		const u32 shift = _Sa_ & (MASK);                                              \
		if (shift == 0)                                                               \
			armAsm->Mov(mmiQ(EEREC_D).V16B(), mmiQ(EEREC_T).V16B());                  \
		else                                                                          \
			armAsm->OP(mmiQ(EEREC_D).VIEW(), mmiQ(EEREC_T).VIEW(), shift);            \
		mmiFinish();                                                                  \
	}

REC_MMI_IMM_SHIFT(PSRLW, Ushr, V4S, 0x1f)
REC_MMI_IMM_SHIFT(PSRLH, Ushr, V8H, 0x0f)
REC_MMI_IMM_SHIFT(PSRAH, Sshr, V8H, 0x0f)
REC_MMI_IMM_SHIFT(PSRAW, Sshr, V4S, 0x1f)
REC_MMI_IMM_SHIFT(PSLLH, Shl, V8H, 0x0f)
REC_MMI_IMM_SHIFT(PSLLW, Shl, V4S, 0x1f)

// MMI0

REC_MMI_BINARY(PADDW, Add, V4S)
REC_MMI_BINARY(PADDH, Add, V8H)
REC_MMI_BINARY(PADDB, Add, V16B)
REC_MMI_BINARY(PSUBW, Sub, V4S)
REC_MMI_BINARY(PSUBH, Sub, V8H)
REC_MMI_BINARY(PSUBB, Sub, V16B)

REC_MMI_BINARY(PADDSW, Sqadd, V4S)
REC_MMI_BINARY(PADDSH, Sqadd, V8H)
REC_MMI_BINARY(PADDSB, Sqadd, V16B)
REC_MMI_BINARY(PSUBSW, Sqsub, V4S)
REC_MMI_BINARY(PSUBSH, Sqsub, V8H)
REC_MMI_BINARY(PSUBSB, Sqsub, V16B)

REC_MMI_BINARY(PMAXW, Smax, V4S)
REC_MMI_BINARY(PMAXH, Smax, V8H)
REC_MMI_BINARY(PCGTW, Cmgt, V4S)
REC_MMI_BINARY(PCGTH, Cmgt, V8H)
REC_MMI_BINARY(PCGTB, Cmgt, V16B)

REC_MMI_BINARY_TS(PEXTLW, Zip1, V4S)
REC_MMI_BINARY_TS(PPACW, Uzp1, V4S)
REC_MMI_BINARY_TS(PEXTLH, Zip1, V8H)
REC_MMI_BINARY_TS(PPACH, Uzp1, V8H)
REC_MMI_BINARY_TS(PEXTLB, Zip1, V16B)
REC_MMI_BINARY_TS(PPACB, Uzp1, V16B)

void recPEXT5()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PEXT5);
	const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister d = a64::q29;
	const a64::VRegister tmp = a64::q30;
	const a64::VRegister mask = a64::q31;

	armAsm->Shl(d.V4S(), mmiQ(EEREC_T).V4S(), 3);
	armAsm->Movi(mask.V4S(), 0xf8);
	armAsm->And(d.V16B(), d.V16B(), mask.V16B());
	armAsm->Shl(tmp.V4S(), mmiQ(EEREC_T).V4S(), 6);
	armAsm->Movi(mask.V4S(), 0xf8, a64::LSL, 8);
	armAsm->And(tmp.V16B(), tmp.V16B(), mask.V16B());
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Shl(tmp.V4S(), mmiQ(EEREC_T).V4S(), 9);
	armAsm->Movi(mask.V4S(), 0xf8, a64::LSL, 16);
	armAsm->And(tmp.V16B(), tmp.V16B(), mask.V16B());
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Shl(tmp.V4S(), mmiQ(EEREC_T).V4S(), 16);
	armAsm->Movi(mask.V4S(), 0x80, a64::LSL, 24);
	armAsm->And(tmp.V16B(), tmp.V16B(), mask.V16B());
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

void recPPAC5()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PPAC5);
	const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister d = a64::q29;
	const a64::VRegister tmp = a64::q30;
	const a64::VRegister mask = a64::q31;

	armAsm->Movi(mask.V4S(), 0xf8);
	armAsm->And(d.V16B(), mmiQ(EEREC_T).V16B(), mask.V16B());
	armAsm->Ushr(d.V4S(), d.V4S(), 3);
	armAsm->Movi(mask.V4S(), 0xf8, a64::LSL, 8);
	armAsm->And(tmp.V16B(), mmiQ(EEREC_T).V16B(), mask.V16B());
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 6);
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Movi(mask.V4S(), 0xf8, a64::LSL, 16);
	armAsm->And(tmp.V16B(), mmiQ(EEREC_T).V16B(), mask.V16B());
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 9);
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Movi(mask.V4S(), 0x80, a64::LSL, 24);
	armAsm->And(tmp.V16B(), mmiQ(EEREC_T).V16B(), mask.V16B());
	armAsm->Ushr(tmp.V4S(), tmp.V4S(), 16);
	armAsm->Orr(d.V16B(), d.V16B(), tmp.V16B());
	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

// MMI1

#define REC_MMI_UNARY(NAME, OP, VIEW)                                               \
	void rec##NAME()                                                                \
	{                                                                               \
		if (!_Rd_)                                                                  \
			return;                                                                 \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                        \
		const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);        \
		armAsm->OP(mmiQ(EEREC_D).VIEW(), mmiQ(EEREC_T).VIEW());                     \
		mmiFinish();                                                                \
	}

REC_MMI_UNARY(PABSW, Sqabs, V4S)
REC_MMI_UNARY(PABSH, Sqabs, V8H)
REC_MMI_BINARY(PMINW, Smin, V4S)
REC_MMI_BINARY(PMINH, Smin, V8H)
REC_MMI_BINARY(PCEQW, Cmeq, V4S)
REC_MMI_BINARY(PCEQH, Cmeq, V8H)
REC_MMI_BINARY(PCEQB, Cmeq, V16B)
REC_MMI_BINARY(PADDUW, Uqadd, V4S)
REC_MMI_BINARY(PADDUH, Uqadd, V8H)
REC_MMI_BINARY(PADDUB, Uqadd, V16B)
REC_MMI_BINARY(PSUBUW, Uqsub, V4S)
REC_MMI_BINARY(PSUBUH, Uqsub, V8H)
REC_MMI_BINARY(PSUBUB, Uqsub, V16B)
REC_MMI_BINARY_TS(PEXTUW, Zip2, V4S)
REC_MMI_BINARY_TS(PEXTUH, Zip2, V8H)
REC_MMI_BINARY_TS(PEXTUB, Zip2, V16B)

void recPADSBH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADSBH);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister low_sub = a64::q29;
	const a64::VRegister high_add = a64::q30;
	armAsm->Sub(low_sub.V8H(), mmiQ(EEREC_S).V8H(), mmiQ(EEREC_T).V8H());
	armAsm->Add(high_add.V8H(), mmiQ(EEREC_S).V8H(), mmiQ(EEREC_T).V8H());
	armAsm->Ins(low_sub.V2D(), 1, high_add.V2D(), 1);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), low_sub.V16B());
	mmiFinish();
}

REC_MMI_FALLBACK(QFSRV, _Rd_);

// MMI2

REC_MMI_FALLBACK(PMADDW, _Rd_);
REC_MMI_FALLBACK(PMSUBW, _Rd_);
REC_MMI_FALLBACK(PMULTW, _Rd_);
REC_MMI_FALLBACK(PDIVW, 0);
REC_MMI_FALLBACK(PMADDH, _Rd_);
REC_MMI_FALLBACK(PHMADH, _Rd_);
REC_MMI_FALLBACK(PMSUBH, _Rd_);
REC_MMI_FALLBACK(PHMSBH, _Rd_);
REC_MMI_FALLBACK(PMULTH, _Rd_);
REC_MMI_FALLBACK(PDIVBW, 0);

#define REC_MMI_VARIABLE_SHIFT(NAME, OP, NEGATE_COUNTS)                                      \
	void rec##NAME()                                                                         \
	{                                                                                        \
		if (!_Rd_)                                                                           \
			return;                                                                          \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                                 \
		const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED); \
		const a64::VRegister shifted = a64::q29;                                             \
		const a64::VRegister counts = a64::q30;                                              \
		const a64::VRegister packed = a64::q31;                                              \
		armAsm->Movi(packed.V4S(), 0x1f);                                                    \
		armAsm->And(counts.V16B(), mmiQ(EEREC_S).V16B(), packed.V16B());                     \
		if (NEGATE_COUNTS)                                                                   \
			armAsm->Neg(counts.V4S(), counts.V4S());                                         \
		armAsm->OP(shifted.V4S(), mmiQ(EEREC_T).V4S(), counts.V4S());                        \
		armAsm->Uzp1(packed.V4S(), shifted.V4S(), shifted.V4S());                            \
		armAsm->Sxtl(mmiQ(EEREC_D).V2D(), packed.V2S());                                     \
		mmiFinish();                                                                         \
	}

REC_MMI_VARIABLE_SHIFT(PSLLVW, Ushl, false)
REC_MMI_VARIABLE_SHIFT(PSRLVW, Ushl, true)

void recPMFHI()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PMFHI);
	const int info = eeRecompileCodeXMM(XMMINFO_READHI | XMMINFO_WRITED);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), mmiQ(EEREC_HI).V16B());
	mmiFinish();
}

void recPMFLO()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PMFLO);
	const int info = eeRecompileCodeXMM(XMMINFO_READLO | XMMINFO_WRITED);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), mmiQ(EEREC_LO).V16B());
	mmiFinish();
}

REC_MMI_BINARY_TS(PCPYLD, Zip1, V2D)
REC_MMI_BINARY(PAND, And, V16B)
REC_MMI_BINARY(PXOR, Eor, V16B)

void recPINTH()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PINTH);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister d = a64::q29;
	armAsm->Ins(d.V8H(), 0, mmiQ(EEREC_T).V8H(), 0);
	armAsm->Ins(d.V8H(), 1, mmiQ(EEREC_S).V8H(), 4);
	armAsm->Ins(d.V8H(), 2, mmiQ(EEREC_T).V8H(), 1);
	armAsm->Ins(d.V8H(), 3, mmiQ(EEREC_S).V8H(), 5);
	armAsm->Ins(d.V8H(), 4, mmiQ(EEREC_T).V8H(), 2);
	armAsm->Ins(d.V8H(), 5, mmiQ(EEREC_S).V8H(), 6);
	armAsm->Ins(d.V8H(), 6, mmiQ(EEREC_T).V8H(), 3);
	armAsm->Ins(d.V8H(), 7, mmiQ(EEREC_S).V8H(), 7);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

#define REC_MMI_PERMUTE_H(NAME, I0, I1, I2, I3, I4, I5, I6, I7)                     \
	void rec##NAME()                                                                \
	{                                                                               \
		if (!_Rd_)                                                                  \
			return;                                                                 \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                        \
		const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);        \
		const a64::VRegister d = a64::q29;                                          \
		armAsm->Ins(d.V8H(), 0, mmiQ(EEREC_T).V8H(), I0);                           \
		armAsm->Ins(d.V8H(), 1, mmiQ(EEREC_T).V8H(), I1);                           \
		armAsm->Ins(d.V8H(), 2, mmiQ(EEREC_T).V8H(), I2);                           \
		armAsm->Ins(d.V8H(), 3, mmiQ(EEREC_T).V8H(), I3);                           \
		armAsm->Ins(d.V8H(), 4, mmiQ(EEREC_T).V8H(), I4);                           \
		armAsm->Ins(d.V8H(), 5, mmiQ(EEREC_T).V8H(), I5);                           \
		armAsm->Ins(d.V8H(), 6, mmiQ(EEREC_T).V8H(), I6);                           \
		armAsm->Ins(d.V8H(), 7, mmiQ(EEREC_T).V8H(), I7);                           \
		armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());                                \
		mmiFinish();                                                                \
	}

#define REC_MMI_PERMUTE_W(NAME, I0, I1, I2, I3)                                     \
	void rec##NAME()                                                                \
	{                                                                               \
		if (!_Rd_)                                                                  \
			return;                                                                 \
		EE::Profiler.EmitOp(eeOpcode::NAME);                                        \
		const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);        \
		const a64::VRegister d = a64::q29;                                          \
		armAsm->Ins(d.V4S(), 0, mmiQ(EEREC_T).V4S(), I0);                           \
		armAsm->Ins(d.V4S(), 1, mmiQ(EEREC_T).V4S(), I1);                           \
		armAsm->Ins(d.V4S(), 2, mmiQ(EEREC_T).V4S(), I2);                           \
		armAsm->Ins(d.V4S(), 3, mmiQ(EEREC_T).V4S(), I3);                           \
		armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());                                \
		mmiFinish();                                                                \
	}

REC_MMI_PERMUTE_H(PEXEH, 2, 1, 0, 3, 6, 5, 4, 7)

void recPREVH()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PREVH);
	const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	armAsm->Rev64(mmiQ(EEREC_D).V8H(), mmiQ(EEREC_T).V8H());
	mmiFinish();
}

REC_MMI_PERMUTE_W(PEXEW, 2, 1, 0, 3)
REC_MMI_PERMUTE_W(PROT3W, 1, 2, 0, 3)

// MMI3

REC_MMI_FALLBACK(PMADDUW, _Rd_);
REC_MMI_FALLBACK(PMULTUW, _Rd_);
REC_MMI_FALLBACK(PDIVUW, 0);

REC_MMI_VARIABLE_SHIFT(PSRAVW, Sshl, true)

void recPMTHI()
{
	EE::Profiler.EmitOp(eeOpcode::PMTHI);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITEHI);
	armAsm->Mov(mmiQ(EEREC_HI).V16B(), mmiQ(EEREC_S).V16B());
	mmiFinish();
}

void recPMTLO()
{
	EE::Profiler.EmitOp(eeOpcode::PMTLO);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITELO);
	armAsm->Mov(mmiQ(EEREC_LO).V16B(), mmiQ(EEREC_S).V16B());
	mmiFinish();
}

void recPINTEH()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PINTEH);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister d = a64::q29;
	armAsm->Ins(d.V8H(), 0, mmiQ(EEREC_T).V8H(), 0);
	armAsm->Ins(d.V8H(), 1, mmiQ(EEREC_S).V8H(), 0);
	armAsm->Ins(d.V8H(), 2, mmiQ(EEREC_T).V8H(), 2);
	armAsm->Ins(d.V8H(), 3, mmiQ(EEREC_S).V8H(), 2);
	armAsm->Ins(d.V8H(), 4, mmiQ(EEREC_T).V8H(), 4);
	armAsm->Ins(d.V8H(), 5, mmiQ(EEREC_S).V8H(), 4);
	armAsm->Ins(d.V8H(), 6, mmiQ(EEREC_T).V8H(), 6);
	armAsm->Ins(d.V8H(), 7, mmiQ(EEREC_S).V8H(), 6);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

REC_MMI_BINARY(PCPYUD, Zip2, V2D)
REC_MMI_BINARY(POR, Orr, V16B)

void recPNOR()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PNOR);
	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	armAsm->Orr(mmiQ(EEREC_D).V16B(), mmiQ(EEREC_S).V16B(), mmiQ(EEREC_T).V16B());
	armAsm->Not(mmiQ(EEREC_D).V16B(), mmiQ(EEREC_D).V16B());
	mmiFinish();
}

void recPCPYH()
{
	if (!_Rd_)
		return;
	EE::Profiler.EmitOp(eeOpcode::PCPYH);
	const int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	const a64::VRegister low = a64::q29;
	const a64::VRegister d = a64::q30;
	armAsm->Dup(low.V8H(), mmiQ(EEREC_T).V8H(), 0);
	armAsm->Dup(d.V8H(), mmiQ(EEREC_T).V8H(), 4);
	armAsm->Ins(d.V2D(), 0, low.V2D(), 0);
	armAsm->Mov(mmiQ(EEREC_D).V16B(), d.V16B());
	mmiFinish();
}

REC_MMI_PERMUTE_H(PEXCH, 0, 2, 1, 3, 4, 6, 5, 7)
REC_MMI_PERMUTE_W(PEXCW, 0, 2, 1, 3)

#undef REC_MMI_PERMUTE_W
#undef REC_MMI_PERMUTE_H
#undef REC_MMI_VARIABLE_SHIFT
#undef REC_MMI_UNARY
#undef REC_MMI_IMM_SHIFT
#undef REC_MMI_FALLBACK
#undef REC_MMI_BINARY_TS
#undef REC_MMI_BINARY
