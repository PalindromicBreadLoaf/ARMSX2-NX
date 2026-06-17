// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/GSDeviceDK.h"

// CreateSurface still hands back the CPU-backed null texture
// This keeps GSRendererHW from dereferencing nothing.
#include "GS/Renderers/Null/GSDeviceNull.h"

#ifdef __SWITCH__
#include "common/Console.h"
#include "common/Horizon/Horizon.h" // nwindowGetDefault()

namespace
{
	constexpr u32 CMDBUF_SIZE = 64 * 1024;

	// Cornflower blue because it's funny.
	constexpr float CLEAR_R = 0.125f;
	constexpr float CLEAR_G = 0.294f;
	constexpr float CLEAR_B = 0.478f;
} // namespace
#endif

GSDeviceDK::GSDeviceDK() = default;

GSDeviceDK::~GSDeviceDK()
{
#ifdef __SWITCH__
	DestroyDeviceObjects();
#endif
}

RenderAPI GSDeviceDK::GetRenderAPI() const
{
	return RenderAPI::DK3D;
}

bool GSDeviceDK::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	m_name = "deko3d";
	m_max_texture_size = 16384;

	m_window_info.surface_width = 1280;
	m_window_info.surface_height = 720;
	m_window_info.surface_scale = 1.0f;
	m_window_info.surface_refresh_rate = 60.0f;

#ifdef __SWITCH__
	m_present_width = static_cast<int>(m_window_info.surface_width);
	m_present_height = static_cast<int>(m_window_info.surface_height);
	if (!CreateDeviceObjects())
	{
		Console.Error("DK3D: failed to create deko3d device objects.");
		DestroyDeviceObjects();
		return false;
	}
#endif

	return GSDevice::Create(vsync_mode, allow_present_throttle);
}

#ifdef __SWITCH__
bool GSDeviceDK::CreateDeviceObjects()
{
	DkDeviceMaker device_maker;
	dkDeviceMakerDefaults(&device_maker);
	m_device = dkDeviceCreate(&device_maker);
	if (!m_device)
		return false;

	DkImageLayoutMaker layout_maker;
	dkImageLayoutMakerDefaults(&layout_maker, m_device);
	layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
	layout_maker.format = DkImageFormat_RGBA8_Unorm;
	layout_maker.dimensions[0] = m_present_width;
	layout_maker.dimensions[1] = m_present_height;

	DkImageLayout framebuffer_layout;
	dkImageLayoutInitialize(&framebuffer_layout, &layout_maker);

	u32 fb_size = dkImageLayoutGetSize(&framebuffer_layout);
	const u32 fb_align = dkImageLayoutGetAlignment(&framebuffer_layout);
	fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);
	m_framebuffer_size = fb_size;

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, NUM_FRAMEBUFFERS * fb_size);
	memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	m_fb_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_fb_memblock)
		return false;

	// Initialise the framebuffer images and build a swapchain from them.
	const DkImage* swapchain_images[NUM_FRAMEBUFFERS];
	for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++)
	{
		dkImageInitialize(&m_framebuffers[i], &framebuffer_layout, m_fb_memblock, i * fb_size);
		swapchain_images[i] = &m_framebuffers[i];
	}

	DkSwapchainMaker swapchain_maker;
	dkSwapchainMakerDefaults(&swapchain_maker, m_device, nwindowGetDefault(), swapchain_images, NUM_FRAMEBUFFERS);
	m_swapchain = dkSwapchainCreate(&swapchain_maker);
	if (!m_swapchain)
		return false;

	// Command buffer scratch + the command buffer itself.
	dkMemBlockMakerDefaults(&memblock_maker, m_device, CMDBUF_SIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_cmdbuf_memblock)
		return false;

	DkCmdBufMaker cmdbuf_maker;
	dkCmdBufMakerDefaults(&cmdbuf_maker, m_device);
	m_cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
	if (!m_cmdbuf)
		return false;
	dkCmdBufAddMemory(m_cmdbuf, m_cmdbuf_memblock, 0, CMDBUF_SIZE);

	// Graphics queue we submit to.
	DkQueueMaker queue_maker;
	dkQueueMakerDefaults(&queue_maker, m_device);
	queue_maker.flags = DkQueueFlags_Graphics;
	m_queue = dkQueueCreate(&queue_maker);
	if (!m_queue)
		return false;

	Console.WriteLn("DK3D: deko3d device up (%dx%d, %u framebuffers).", m_present_width, m_present_height,
		NUM_FRAMEBUFFERS);
	return true;
}

void GSDeviceDK::DestroyDeviceObjects()
{
	if (m_queue)
	{
		dkQueueWaitIdle(m_queue);
		dkQueueDestroy(m_queue);
		m_queue = nullptr;
	}
	if (m_cmdbuf)
	{
		dkCmdBufDestroy(m_cmdbuf);
		m_cmdbuf = nullptr;
	}
	if (m_cmdbuf_memblock)
	{
		dkMemBlockDestroy(m_cmdbuf_memblock);
		m_cmdbuf_memblock = nullptr;
	}
	if (m_swapchain)
	{
		dkSwapchainDestroy(m_swapchain);
		m_swapchain = nullptr;
	}
	if (m_fb_memblock)
	{
		dkMemBlockDestroy(m_fb_memblock);
		m_fb_memblock = nullptr;
	}
	if (m_device)
	{
		dkDeviceDestroy(m_device);
		m_device = nullptr;
	}
}
#endif

bool GSDeviceDK::HasSurface() const
{
#ifdef __SWITCH__
	return m_swapchain != nullptr;
#else
	return false;
#endif
}

void GSDeviceDK::DestroySurface()
{
}

bool GSDeviceDK::UpdateWindow()
{
	return true;
}

void GSDeviceDK::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
}

bool GSDeviceDK::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceDK::BeginPresent(bool frame_skip)
{
#ifdef __SWITCH__
	if (frame_skip || !m_swapchain)
		return PresentResult::FrameSkipped;

	// This will be replaced later with something better, it just needs to work
	// right now.
	dkQueueWaitIdle(m_queue);
	dkCmdBufClear(m_cmdbuf);

	m_present_slot = dkQueueAcquireImage(m_queue, m_swapchain);

	DkImageView color_view;
	dkImageViewDefaults(&color_view, &m_framebuffers[m_present_slot]);
	dkCmdBufBindRenderTarget(m_cmdbuf, &color_view, nullptr);

	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(m_present_width),
		static_cast<float>(m_present_height), 0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, CLEAR_R, CLEAR_G, CLEAR_B, 1.0f);

	return PresentResult::OK;
#else
	return PresentResult::FrameSkipped;
#endif
}

void GSDeviceDK::EndPresent()
{
#ifdef __SWITCH__
	if (m_present_slot < 0)
		return;

	const DkCmdList commands = dkCmdBufFinishList(m_cmdbuf);
	dkQueueSubmitCommands(m_queue, commands);
	dkQueuePresentImage(m_queue, m_swapchain, m_present_slot);
	m_present_slot = -1;
#endif
}

void GSDeviceDK::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceDK::GetDriverInfo() const
{
	return "deko3d";
}

bool GSDeviceDK::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceDK::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

void GSDeviceDK::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceDK::PopDebugGroup()
{
}

void GSDeviceDK::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

std::unique_ptr<GSDownloadTexture> GSDeviceDK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return nullptr;
}

void GSDeviceDK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
}

void GSDeviceDK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, bool linear)
{
}

void GSDeviceDK::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
}

void GSDeviceDK::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
	GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceDK::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceDK::RenderHW(GSHWDrawConfig& config)
{
}

void GSDeviceDK::ClearSamplerCache()
{
}

GSTexture* GSDeviceDK::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNull(type, width, height, levels, format);
}

void GSDeviceDK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
}

void GSDeviceDK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
}

void GSDeviceDK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceDK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceDK::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}
