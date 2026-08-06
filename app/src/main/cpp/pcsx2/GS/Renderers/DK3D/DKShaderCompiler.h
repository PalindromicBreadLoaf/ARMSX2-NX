// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

// Every selector the tfx fragment shader branches on, in the order cbSel declares them.
#define DK_TFX_SELECTOR_FIELDS(X) \
	X(fst) X(tme) X(tfx) X(tcc) X(atst) X(afail) X(fog) X(aem) \
	X(aem_fmt) X(pal_fmt) X(ltf) X(wms) X(wmt) X(dst_fmt) X(fba) X(iip) \
	X(region_rect) X(adjs) X(adjt) X(tcoffsethack) \
	X(blend_a) X(blend_b) X(blend_c) X(blend_d) X(blend_mix) X(blend_hw) X(pabe) X(fixed_one_a) \
	X(a_masked) X(colclip) X(colclip_hw) X(rta_correction) X(dither) X(dither_adjust) X(round_inv) X(tex_is_fb) \
	X(channel) X(shuffle) X(shuffle_same) X(read16src) X(process_ba) X(process_rg) X(shuffle_across) X(write_rg) \
	X(fbmask) X(scanmsk) X(date) \
	X(depth_fmt) X(urban_chaos) X(tales) \
	X(automatic_lod) X(manual_lod)

struct DKTfxSelector
{
#define DK_TFX_DECLARE_FIELD(name) u32 name;
	DK_TFX_SELECTOR_FIELDS(DK_TFX_DECLARE_FIELD)
#undef DK_TFX_DECLARE_FIELD

	bool operator==(const DKTfxSelector& rhs) const;
	bool operator!=(const DKTfxSelector& rhs) const { return !(*this == rhs); }
};

// Compiles per-draw specialisations of tfx_fsh.glsl with uam, off the GS thread, and caches
// the results on the SD card.
namespace DKShaderCompiler
{
	// Loads the tfx fragment source from romfs, opens the on-disk cache and starts the worker.
	bool Open();
	void Close();
	bool IsOpen();

	u64 HashSelector(const DKTfxSelector& sel);

	// Queue a specialisation. A blob already on the SD card is handed back through PopCompiled
	// without going near uam.
	void Request(const DKTfxSelector& sel, u64 hash);

	// Collect one finished specialisation, if any.
	bool PopCompiled(u64& hash, DKTfxSelector& sel, std::vector<u8>& blob);

	// Compiles still queued or in flight, for the OSD/stat readout.
	u32 GetPendingCount();
} // namespace DKShaderCompiler
