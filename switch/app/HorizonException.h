// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#define EXCEPTION_SLOT_COUNT 8
#define EXCEPTION_SLOT_SHIFT 10
#define EXCEPTION_SLOT_SIZE (1 << EXCEPTION_SLOT_SHIFT)
#define EXCEPTION_STACK_SHIFT 16
#define EXCEPTION_STACK_SIZE (1 << EXCEPTION_STACK_SHIFT)

#define DUMP_ERROR_DESC 0x000
#define DUMP_GPRS 0x010
#define DUMP_FP 0x0F8
#define DUMP_LR 0x100
#define DUMP_SP 0x108
#define DUMP_PC 0x110
#define DUMP_FPU 0x120
#define DUMP_PSTATE 0x320
#define DUMP_ESR 0x32C
#define DUMP_FAR 0x330

#define SLOT_ORIGINAL_X0 0x3D0
#define SLOT_ORIGINAL_PC 0x3D8
#define SLOT_DEFERRED 0x3E0
#define SLOT_RESULT 0x3E4
#define SLOT_FRAME 0x3E8
#define SLOT_INDEX 0x3F0

#define FRAME_LR 0x48
#define FRAME_SP 0x50
#define FRAME_PC 0x58
#define FRAME_PSTATE 0x60
#define FRAME_ESR 0x6C
#define FRAME_FAR 0x70
