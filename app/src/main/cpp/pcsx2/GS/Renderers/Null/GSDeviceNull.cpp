// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNull.h"

#include <algorithm>
#include <cstring>

#ifdef __SWITCH__
#include "common/Console.h"
#include "common/Horizon/Horizon.h"

namespace
{
	// Nearest-neighbour RGBA8 blit from a source rect to a destination rect, clipped to
	// the destination surface.
	// This is the entire renderer
	void BlitRGBA8(const u8* src, int src_pitch, int src_w, int src_h, const GSVector4i& srect,
		u8* dst, int dst_pitch, int dst_w, int dst_h, const GSVector4i& drect)
	{
		const int dw = drect.z - drect.x;
		const int dh = drect.w - drect.y;
		const int sw = srect.z - srect.x;
		const int sh = srect.w - srect.y;
		if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
			return;

		const int dx0 = std::max(0, drect.x);
		const int dy0 = std::max(0, drect.y);
		const int dx1 = std::min(dst_w, drect.z);
		const int dy1 = std::min(dst_h, drect.w);

		for (int y = dy0; y < dy1; ++y)
		{
			int sy = srect.y + ((y - drect.y) * sh) / dh;
			sy = std::clamp(sy, 0, src_h - 1);
			const u32* srow = reinterpret_cast<const u32*>(src + static_cast<size_t>(sy) * src_pitch);
			u32* drow = reinterpret_cast<u32*>(dst + static_cast<size_t>(y) * dst_pitch);
			for (int x = dx0; x < dx1; ++x)
			{
				int sx = srect.x + ((x - drect.x) * sw) / dw;
				sx = std::clamp(sx, 0, src_w - 1);
				drow[x] = srow[sx];
			}
		}
	}
} // namespace
#endif

GSTextureNull::GSTextureNull(Type type, int width, int height, int levels, Format format)
{
	m_type = type;
	m_format = format;
	m_size.x = std::max(1, width);
	m_size.y = std::max(1, height);
	m_mipmap_levels = std::max(1, levels);
	m_buffer_pitch = m_size.x * ((format == Format::UNorm8) ? 1 : 4);
}

GSTextureNull::~GSTextureNull() = default;

void* GSTextureNull::GetNativeHandle() const
{
	// Non-null so callers that store it as a texture id treat it as valid
	return const_cast<GSTextureNull*>(this);
}

bool GSTextureNull::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
#ifdef __SWITCH__
	// The SW renderer fills its display texture through here
	if (!m_buffer)
		m_buffer = std::make_unique<u8[]>(static_cast<size_t>(m_buffer_pitch) * static_cast<size_t>(m_size.y));

	const int bpp = (m_format == Format::UNorm8) ? 1 : 4;
	const int rows = r.w - r.y;
	const int row_bytes = (r.z - r.x) * bpp;
	const u8* src = static_cast<const u8*>(data);
	u8* dst = m_buffer.get() + static_cast<size_t>(r.y) * m_buffer_pitch + static_cast<size_t>(r.x) * bpp;
	for (int y = 0; y < rows; ++y)
		std::memcpy(dst + static_cast<size_t>(y) * m_buffer_pitch, src + static_cast<size_t>(y) * pitch, row_bytes);
#endif
	return true;
}

bool GSTextureNull::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (!m_buffer)
		m_buffer = std::make_unique<u8[]>(static_cast<size_t>(m_buffer_pitch) * static_cast<size_t>(m_size.y));

	const int x = r ? r->x : 0;
	const int y = r ? r->y : 0;
	m.pitch = m_buffer_pitch;
	m.bits = m_buffer.get() + static_cast<size_t>(y) * m_buffer_pitch + static_cast<size_t>(x) *
		((m_format == Format::UNorm8) ? 1 : 4);
	return true;
}

void GSTextureNull::Unmap()
{
}

void GSTextureNull::GenerateMipmap()
{
}

#ifdef PCSX2_DEVBUILD
void GSTextureNull::SetDebugName(std::string_view name)
{
}
#endif

GSDeviceNull::GSDeviceNull() = default;

GSDeviceNull::~GSDeviceNull()
{
#ifdef __SWITCH__
	if (m_framebuffer)
	{
		framebufferClose(m_framebuffer);
		delete m_framebuffer;
		m_framebuffer = nullptr;
	}
#endif
}

RenderAPI GSDeviceNull::GetRenderAPI() const
{
	return RenderAPI::None;
}

bool GSDeviceNull::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	m_name = "Null";
	m_max_texture_size = 16384;

	// It gets mad if something isn't set. Give it sane defaults please.
	m_window_info.surface_width = 1280;
	m_window_info.surface_height = 720;
	m_window_info.surface_scale = 1.0f;
	m_window_info.surface_refresh_rate = 60.0f;

#ifdef __SWITCH__
	m_present_width = static_cast<int>(m_window_info.surface_width);
	m_present_height = static_cast<int>(m_window_info.surface_height);
	m_nwindow = nwindowGetDefault();
	if (m_nwindow)
	{
		m_framebuffer = new Framebuffer;
		const Result rc = framebufferCreate(m_framebuffer, m_nwindow, m_present_width, m_present_height,
			PIXEL_FORMAT_RGBA_8888, 2);
		if (R_SUCCEEDED(rc))
		{
			framebufferMakeLinear(m_framebuffer);
		}
		else
		{
			Console.Error("framebufferCreate failed: 0x%08X; falling back to headless.", rc);
			delete m_framebuffer;
			m_framebuffer = nullptr;
		}
	}
#endif

	return GSDevice::Create(vsync_mode, allow_present_throttle);
}

bool GSDeviceNull::HasSurface() const
{
#ifdef __SWITCH__
	return m_framebuffer != nullptr;
#else
	return false;
#endif
}

void GSDeviceNull::DestroySurface()
{
}

bool GSDeviceNull::UpdateWindow()
{
	return true;
}

void GSDeviceNull::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
}

bool GSDeviceNull::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceNull::BeginPresent(bool frame_skip)
{
#ifdef __SWITCH__
	if (frame_skip || !m_framebuffer)
		return PresentResult::FrameSkipped;

	u32 stride = 0;
	m_present_bits = static_cast<u8*>(framebufferBegin(m_framebuffer, &stride));
	m_present_stride = stride;

	// Clear to black
	for (int y = 0; y < m_present_height; ++y)
		std::memset(m_present_bits + static_cast<size_t>(y) * stride, 0, static_cast<size_t>(m_present_width) * 4);

	return PresentResult::OK;
#else
	return PresentResult::FrameSkipped;
#endif
}

void GSDeviceNull::EndPresent()
{
#ifdef __SWITCH__
	if (m_present_bits)
	{
		framebufferEnd(m_framebuffer);
		m_present_bits = nullptr;
	}
#endif
}

void GSDeviceNull::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNull::GetDriverInfo() const
{
#ifdef __SWITCH__
	return "Software renderer";
#else
	return "Null Renderer";
#endif
}

bool GSDeviceNull::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceNull::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

void GSDeviceNull::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceNull::PopDebugGroup()
{
}

void GSDeviceNull::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return nullptr;
}

void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
#ifdef __SWITCH__
	GSTexture::GSMap sm, dm;
	if (!sTex || !dTex || !sTex->Map(sm) || !dTex->Map(dm))
		return;

	const int w = r.z - r.x;
	const int h = r.w - r.y;
	for (int y = 0; y < h; ++y)
	{
		const u8* s = sm.bits + static_cast<size_t>(r.y + y) * sm.pitch + static_cast<size_t>(r.x) * 4;
		u8* d = dm.bits + static_cast<size_t>(destY + y) * dm.pitch + static_cast<size_t>(destX) * 4;
		std::memcpy(d, s, static_cast<size_t>(w) * 4);
	}
#endif
}

void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{
#ifdef __SWITCH__
	GSTexture::GSMap sm, dm;
	if (!sTex || !dTex || !sTex->Map(sm) || !dTex->Map(dm))
		return;

	const int sw = sTex->GetWidth();
	const int sh = sTex->GetHeight();
	const GSVector4i sr(static_cast<int>(sRect.x * sw), static_cast<int>(sRect.y * sh),
		static_cast<int>(sRect.z * sw), static_cast<int>(sRect.w * sh));
	const GSVector4i dr(static_cast<int>(dRect.x), static_cast<int>(dRect.y),
		static_cast<int>(dRect.z), static_cast<int>(dRect.w));
	BlitRGBA8(sm.bits, sm.pitch, sw, sh, sr, dm.bits, dm.pitch, dTex->GetWidth(), dTex->GetHeight(), dr);
#endif
}

void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
}

void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, bool linear)
{
#ifdef __SWITCH__
	// dTex != null means present to a texture
	if (dTex)
	{
		StretchRect(sTex, sRect, dTex, dRect, ShaderConvert::COPY, linear);
		return;
	}

	GSTexture::GSMap sm;
	if (!m_present_bits || !sTex || !sTex->Map(sm))
		return;

	const int sw = sTex->GetWidth();
	const int sh = sTex->GetHeight();
	const GSVector4i sr(static_cast<int>(sRect.x * sw), static_cast<int>(sRect.y * sh),
		static_cast<int>(sRect.z * sw), static_cast<int>(sRect.w * sh));
	const GSVector4i dr(static_cast<int>(dRect.x), static_cast<int>(dRect.y),
		static_cast<int>(dRect.z), static_cast<int>(dRect.w));
	BlitRGBA8(sm.bits, sm.pitch, sw, sh, sr, m_present_bits, static_cast<int>(m_present_stride),
		m_present_width, m_present_height, dr);
#endif
}

void GSDeviceNull::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
}

void GSDeviceNull::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
	GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceNull::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceNull::RenderHW(GSHWDrawConfig& config)
{
}

void GSDeviceNull::ClearSamplerCache()
{
}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNull(type, width, height, levels, format);
}

void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
#ifdef __SWITCH__
	GSTexture::GSMap dm;
	if (!dTex || !dTex->Map(dm))
		return;

	const int dw = dTex->GetWidth();
	const int dh = dTex->GetHeight();

	for (int y = 0; y < dh; ++y)
	{
		u32* row = reinterpret_cast<u32*>(dm.bits + static_cast<size_t>(y) * dm.pitch);
		std::fill(row, row + dw, c);
	}

	// Composite circuit 1 then circuit 0 on top
	for (int i = 1; i >= 0; --i)
	{
		if (!sTex[i])
			continue;

		GSTexture::GSMap sm;
		if (!sTex[i]->Map(sm))
			continue;

		const int sw = sTex[i]->GetWidth();
		const int sh = sTex[i]->GetHeight();
		const GSVector4i sr(static_cast<int>(sRect[i].x * sw), static_cast<int>(sRect[i].y * sh),
			static_cast<int>(sRect[i].z * sw), static_cast<int>(sRect[i].w * sh));
		const GSVector4i dr(static_cast<int>(dRect[i].x), static_cast<int>(dRect[i].y),
			static_cast<int>(dRect[i].z), static_cast<int>(dRect[i].w));
		BlitRGBA8(sm.bits, sm.pitch, sw, sh, sr, dm.bits, dm.pitch, dw, dh, dr);
	}
#endif
}

void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
}

void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceNull::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}
