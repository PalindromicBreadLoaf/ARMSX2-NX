// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNull.h"

#include <cstring>

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

GSDeviceNull::~GSDeviceNull() = default;

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

	return GSDevice::Create(vsync_mode, allow_present_throttle);
}

bool GSDeviceNull::HasSurface() const
{
	return false;
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
	// Always skip rendering
	return PresentResult::FrameSkipped;
}

void GSDeviceNull::EndPresent()
{
}

void GSDeviceNull::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNull::GetDriverInfo() const
{
	return "Null Renderer";
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
}

void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{
}

void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
}

void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, bool linear)
{
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
