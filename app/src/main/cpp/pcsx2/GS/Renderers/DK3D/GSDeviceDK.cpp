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

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	constexpr u32 CMDBUF_SIZE = 64 * 1024;
	constexpr u32 CODE_MEMSIZE = 128 * 1024;

	// Test texture pattern dimensions
	constexpr int TEST_TEX_SIZE = 256;

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

	// Test image and sampler
	dkMemBlockMakerDefaults(&memblock_maker, m_device, DK_MEMBLOCK_ALIGNMENT);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_descriptor_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_descriptor_memblock)
		return false;
	const DkGpuAddr descriptor_base = dkMemBlockGetGpuAddr(m_descriptor_memblock);
	m_image_descriptor_set = descriptor_base;
	m_sampler_descriptor_set = descriptor_base + NUM_IMAGE_DESCRIPTORS * DK_IMAGE_DESCRIPTOR_ALIGNMENT;

	LoadShaders();
	if (!m_have_test_triangle && !m_present_shaders_ok)
		Console.Warning("DK3D: no shaders available. Things will be broken.");

	m_have_test_texture = m_present_shaders_ok && SetupTestTexture();

	Console.WriteLn("DK3D: deko3d device up (%dx%d, %u framebuffers, triangle=%d, textured=%d).", m_present_width,
		m_present_height, NUM_FRAMEBUFFERS, m_have_test_triangle ? 1 : 0, m_have_test_texture ? 1 : 0);
	return true;
}

bool GSDeviceDK::SetupTestTexture()
{
	m_test_texture = GSTextureDK::Create(m_device, m_queue, GSTexture::Type::Texture, GSTexture::Format::Color,
		TEST_TEX_SIZE, TEST_TEX_SIZE, 1);
	if (!m_test_texture)
	{
		Console.Error("DK3D: failed to create test texture.");
		return false;
	}

	// Checkerboard with a fancy gradient. Please hold your oohs and aahs.
	std::vector<u32> pixels(static_cast<size_t>(TEST_TEX_SIZE) * TEST_TEX_SIZE);
	for (int y = 0; y < TEST_TEX_SIZE; ++y)
	{
		for (int x = 0; x < TEST_TEX_SIZE; ++x)
		{
			const bool checker = (((x >> 5) ^ (y >> 5)) & 1) != 0;
			const u32 r = static_cast<u32>(x);
			const u32 g = static_cast<u32>(y);
			const u32 b = checker ? 0xFFu : 0x40u;
			pixels[static_cast<size_t>(y) * TEST_TEX_SIZE + x] = r | (g << 8) | (b << 16) | (0xFFu << 24);
		}
	}

	if (!m_test_texture->Update(GSVector4i(0, 0, TEST_TEX_SIZE, TEST_TEX_SIZE), pixels.data(), TEST_TEX_SIZE * 4, 0))
	{
		Console.Error("DK3D: failed to upload the test texture.");
		m_test_texture.reset();
		return false;
	}

	m_offscreen_rt = GSTextureDK::Create(m_device, m_queue, GSTexture::Type::RenderTarget, GSTexture::Format::Color,
		TEST_TEX_SIZE, TEST_TEX_SIZE, 1);
	if (!m_offscreen_rt)
	{
		Console.Error("DK3D: failed to create the offscreen render target.");
		m_test_texture.reset();
		return false;
	}

	DkSampler sampler;
	dkSamplerDefaults(&sampler);
	sampler.minFilter = DkFilter_Linear;
	sampler.magFilter = DkFilter_Linear;
	sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
	sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
	sampler.wrapMode[2] = DkWrapMode_ClampToEdge;

	DkSamplerDescriptor sampler_descriptor;
	dkSamplerDescriptorInitialize(&sampler_descriptor, &sampler);

	const DkImageDescriptor image_descriptors[NUM_IMAGE_DESCRIPTORS] = {
		m_test_texture->GetDescriptor(), m_offscreen_rt->GetDescriptor()};
	dkCmdBufClear(m_cmdbuf);
	dkCmdBufPushData(m_cmdbuf, m_image_descriptor_set, image_descriptors, sizeof(image_descriptors));
	dkCmdBufPushData(m_cmdbuf, m_sampler_descriptor_set, &sampler_descriptor, sizeof(sampler_descriptor));
	dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueueWaitIdle(m_queue);
	dkCmdBufClear(m_cmdbuf);
	return true;
}

void GSDeviceDK::DrawTexturedQuad(const DkImageView* target, int width, int height, u32 image_id)
{
	dkCmdBufBindRenderTarget(m_cmdbuf, target, nullptr);

	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(width), static_cast<u32>(height)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);

	DkRasterizerState rasterizer_state;
	DkColorState color_state;
	DkColorWriteState color_write_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	dkColorStateDefaults(&color_state);
	dkColorWriteStateDefaults(&color_write_state);
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, 1);

	const DkShader* shaders[] = {&m_present_vsh, &m_present_fsh};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	const DkResHandle texture_handle = dkMakeTextureHandle(image_id, 0);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);
	dkCmdBufDraw(m_cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
}

bool GSDeviceDK::LoadShaders()
{
	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, CODE_MEMSIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
	m_code_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_code_memblock)
		return false;

	u32 code_offset = 0;
	const auto load_one = [&](DkShader& shader, const char* path) -> bool {
		std::FILE* f = std::fopen(path, "rb");
		if (!f)
		{
			Console.Error("DK3D: could not open shader %s", path);
			return false;
		}
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::rewind(f);
		if (size <= 0)
		{
			std::fclose(f);
			return false;
		}

		const u32 offset = code_offset;
		code_offset += (static_cast<u32>(size) + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
		if (code_offset > CODE_MEMSIZE)
		{
			std::fclose(f);
			Console.Error("DK3D: shader code memblock too small for %s", path);
			return false;
		}

		void* dst = static_cast<u8*>(dkMemBlockGetCpuAddr(m_code_memblock)) + offset;
		const size_t read = std::fread(dst, 1, static_cast<size_t>(size), f);
		std::fclose(f);
		if (read != static_cast<size_t>(size))
			return false;

		DkShaderMaker shader_maker;
		dkShaderMakerDefaults(&shader_maker, m_code_memblock, offset);
		dkShaderInitialize(&shader, &shader_maker);
		return true;
	};

	m_have_test_triangle = load_one(m_vertex_shader, "romfs:/shaders/triangle_vsh.dksh") &&
						   load_one(m_fragment_shader, "romfs:/shaders/color_fsh.dksh");

	m_present_shaders_ok = load_one(m_present_vsh, "romfs:/shaders/fullscreen_vsh.dksh") &&
						   load_one(m_present_fsh, "romfs:/shaders/texture_fsh.dksh");

	if (m_have_test_triangle)
		Console.WriteLn("DK3D: test-triangle shaders loaded.");
	if (m_present_shaders_ok)
		Console.WriteLn("DK3D: textured-present shaders loaded.");

	return m_have_test_triangle || m_present_shaders_ok;
}

void GSDeviceDK::DestroyDeviceObjects()
{
	if (m_queue)
		dkQueueWaitIdle(m_queue);

	// Destroy the textures before the device goes away.
	m_test_texture.reset();
	m_offscreen_rt.reset();
	m_have_test_texture = false;

	if (m_descriptor_memblock)
	{
		dkMemBlockDestroy(m_descriptor_memblock);
		m_descriptor_memblock = nullptr;
	}
	if (m_queue)
	{
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
	if (m_code_memblock)
	{
		dkMemBlockDestroy(m_code_memblock);
		m_code_memblock = nullptr;
		m_have_test_triangle = false;
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

	// This is all testing and will be thrown away once the renderer is more up-to-speed.
	dkQueueWaitIdle(m_queue);
	dkCmdBufClear(m_cmdbuf);

	m_present_slot = dkQueueAcquireImage(m_queue, m_swapchain);

	DkImageView swapchain_view;
	dkImageViewDefaults(&swapchain_view, &m_framebuffers[m_present_slot]);

	if (m_have_test_texture)
	{
		DkImageView rt_view;
		m_offscreen_rt->GetImageView(&rt_view);
		DrawTexturedQuad(&rt_view, m_offscreen_rt->GetWidth(), m_offscreen_rt->GetHeight(), 0);

		dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

		DrawTexturedQuad(&swapchain_view, m_present_width, m_present_height, 1);
	}
	else
	{
		dkCmdBufBindRenderTarget(m_cmdbuf, &swapchain_view, nullptr);
		const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(m_present_width),
			static_cast<float>(m_present_height), 0.0f, 1.0f};
		const DkScissor scissor = {0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height)};
		dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
		dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
		dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, CLEAR_R, CLEAR_G, CLEAR_B, 1.0f);

		if (m_have_test_triangle)
		{
			DkRasterizerState rasterizer_state;
			DkColorState color_state;
			DkColorWriteState color_write_state;
			dkRasterizerStateDefaults(&rasterizer_state);
			dkColorStateDefaults(&color_state);
			dkColorWriteStateDefaults(&color_write_state);
			dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);
			dkCmdBufBindColorState(m_cmdbuf, &color_state);
			dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

			const DkShader* shaders[] = {&m_vertex_shader, &m_fragment_shader};
			dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
			dkCmdBufDraw(m_cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
		}
	}

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
