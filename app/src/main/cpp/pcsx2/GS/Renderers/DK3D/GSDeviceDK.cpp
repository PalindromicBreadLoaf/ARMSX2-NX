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

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
	constexpr u32 CMDBUF_SIZE = 256 * 1024;
	constexpr u32 CODE_MEMSIZE = 128 * 1024;
	constexpr u32 VERTEX_BUFFER_SIZE = 512 * 1024;

	struct ConvertVertex
	{
		float pos[4];
		float uv[2];
	};

	constexpr u32 AlignUp(u32 value, u32 align)
	{
		return (value + align - 1) & ~(align - 1);
	}
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

	// Descriptor ring
	const u32 descriptor_size = AlignUp(
		NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor) + NUM_SAMPLERS * sizeof(DkSamplerDescriptor),
		DK_MEMBLOCK_ALIGNMENT);
	dkMemBlockMakerDefaults(&memblock_maker, m_device, descriptor_size);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_descriptor_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_descriptor_memblock)
		return false;
	const DkGpuAddr descriptor_base = dkMemBlockGetGpuAddr(m_descriptor_memblock);
	m_image_descriptor_set = descriptor_base;
	m_sampler_descriptor_set = descriptor_base + NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor);

	// Vertex stream buffer
	dkMemBlockMakerDefaults(&memblock_maker, m_device, VERTEX_BUFFER_SIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_vertex_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_vertex_memblock)
		return false;

	LoadShaders();
	if (!m_convert_shaders_ok)
		Console.Warning("DK3D: No shaders available. Things will be broken.");
	else
		SetupSamplers();

	Console.WriteLn("DK3D: deko3d device up (%dx%d, %u framebuffers, convert=%d).", m_present_width, m_present_height,
		NUM_FRAMEBUFFERS, m_convert_shaders_ok ? 1 : 0);
	return true;
}

bool GSDeviceDK::SetupSamplers()
{
	DkSampler point;
	dkSamplerDefaults(&point);
	point.minFilter = DkFilter_Nearest;
	point.magFilter = DkFilter_Nearest;
	point.wrapMode[0] = point.wrapMode[1] = point.wrapMode[2] = DkWrapMode_ClampToEdge;

	DkSampler linear;
	dkSamplerDefaults(&linear);
	linear.minFilter = DkFilter_Linear;
	linear.magFilter = DkFilter_Linear;
	linear.wrapMode[0] = linear.wrapMode[1] = linear.wrapMode[2] = DkWrapMode_ClampToEdge;

	DkSamplerDescriptor descriptors[NUM_SAMPLERS];
	dkSamplerDescriptorInitialize(&descriptors[SAMPLER_POINT], &point);
	dkSamplerDescriptorInitialize(&descriptors[SAMPLER_LINEAR], &linear);

	// Samplers never change
	dkCmdBufClear(m_cmdbuf);
	dkCmdBufPushData(m_cmdbuf, m_sampler_descriptor_set, descriptors, sizeof(descriptors));
	dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueueWaitIdle(m_queue);
	dkCmdBufClear(m_cmdbuf);
	return true;
}

void GSDeviceDK::BeginFrameIfNeeded()
{
	if (m_frame_active)
		return;

	dkQueueWaitIdle(m_queue);
	dkCmdBufClear(m_cmdbuf);
	m_vertex_offset = 0;
	m_next_image_slot = 0;

	m_frame_active = true;
}

void GSDeviceDK::CommitClear(GSTextureDK* tex)
{
	if (!tex || tex->IsDepth() || tex->GetState() != GSTexture::State::Cleared)
		return;

	DkImageView view;
	tex->GetImageView(&view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(tex->GetWidth()), static_cast<float>(tex->GetHeight()),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(tex->GetWidth()), static_cast<u32>(tex->GetHeight())};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	float cc[4];
	GSVector4::store<false>(cc, tex->GetUNormClearColor());
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, cc[0], cc[1], cc[2], cc[3]);
	tex->SetState(GSTexture::State::Dirty);
}

void GSDeviceDK::DoStretchRectImpl(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex,
	const GSVector4& dRect, const DkShader* fragment_shader, bool linear)
{
	if (!sTex || !m_convert_shaders_ok)
		return;
	// Skip depth conversions for now rather than mis-binding a depth image
	if (sTex->IsDepth() || (dTex && dTex->IsDepth()))
		return;

	BeginFrameIfNeeded();

	// Make sure the source has real contents, then flush prior render-target writes so the sampler sees them.
	CommitClear(sTex);
	dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

	const bool is_present = (dTex == nullptr);
	const GSVector2i ds = is_present ? GSVector2i(m_present_width, m_present_height) : dTex->GetSize();

	DkImageView target_view;
	if (is_present)
		target_view = m_swapchain_view;
	else
		dTex->GetImageView(&target_view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &target_view, nullptr);

	// Commit anything pending before writing over it.
	if (!is_present && dTex->GetState() == GSTexture::State::Cleared)
	{
		float cc[4];
		GSVector4::store<false>(cc, dTex->GetUNormClearColor());
		dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, cc[0], cc[1], cc[2], cc[3]);
	}

	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(ds.x), static_cast<float>(ds.y), 0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(ds.x), static_cast<u32>(ds.y)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);

	DkRasterizerState rasterizer_state;
	DkColorState color_state;
	DkColorWriteState color_write_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	dkColorStateDefaults(&color_state);
	dkColorWriteStateDefaults(&color_write_state);
	// The default culling mode discards quads too frequently
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);
	// No depth attachment is bound; keep depth test/write off so fragments aren't discarded.
	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = false;
	depth_state.depthWriteEnable = false;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	const DkShader* shaders[] = {&m_convert_vsh, fragment_shader};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	// Grab the next descriptor ring slot for the source texture.
	const u32 image_slot = m_next_image_slot;
	m_next_image_slot = (m_next_image_slot + 1) % NUM_IMAGE_DESCRIPTORS;
	const DkImageDescriptor src_descriptor = sTex->GetDescriptor();
	dkCmdBufPushData(m_cmdbuf, m_image_descriptor_set + image_slot * sizeof(DkImageDescriptor), &src_descriptor,
		sizeof(src_descriptor));
	const DkResHandle texture_handle = dkMakeTextureHandle(image_slot, linear ? SAMPLER_LINEAR : SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);

	const float left = dRect.x * 2.0f / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2.0f / ds.y;
	const float right = dRect.z * 2.0f / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2.0f / ds.y;
	const ConvertVertex vertices[4] = {
		{{left, top, 0.5f, 1.0f}, {sRect.x, sRect.y}},
		{{right, top, 0.5f, 1.0f}, {sRect.z, sRect.y}},
		{{left, bottom, 0.5f, 1.0f}, {sRect.x, sRect.w}},
		{{right, bottom, 0.5f, 1.0f}, {sRect.z, sRect.w}},
	};

	m_vertex_offset = AlignUp(m_vertex_offset, sizeof(ConvertVertex));
	if (m_vertex_offset + sizeof(vertices) > VERTEX_BUFFER_SIZE)
		m_vertex_offset = 0;
	std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(m_vertex_memblock)) + m_vertex_offset, vertices, sizeof(vertices));
	const DkGpuAddr vertex_addr = dkMemBlockGetGpuAddr(m_vertex_memblock) + m_vertex_offset;
	m_vertex_offset += sizeof(vertices);

	static const DkVtxAttribState attribs[2] = {
		{0, 0, offsetof(ConvertVertex, pos), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ConvertVertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(ConvertVertex), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 2);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);
	dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vertex_addr, sizeof(vertices));
	dkCmdBufDraw(m_cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);

	if (!is_present)
		dTex->SetState(GSTexture::State::Dirty);
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

	m_convert_shaders_ok = load_one(m_convert_vsh, "romfs:/shaders/convert_vsh.dksh") &&
						   load_one(m_copy_fsh, "romfs:/shaders/texture_fsh.dksh");

	if (m_convert_shaders_ok)
		Console.WriteLn("DK3D: convert shaders loaded.");

	return m_convert_shaders_ok;
}

void GSDeviceDK::DestroyDeviceObjects()
{
	if (m_queue)
		dkQueueWaitIdle(m_queue);

	if (m_vertex_memblock)
	{
		dkMemBlockDestroy(m_vertex_memblock);
		m_vertex_memblock = nullptr;
	}
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
		m_convert_shaders_ok = false;
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

	// DoMerge may already have opened the frame? Otherwise we start here.
	BeginFrameIfNeeded();

	m_present_slot = dkQueueAcquireImage(m_queue, m_swapchain);
	dkImageViewDefaults(&m_swapchain_view, &m_framebuffers[m_present_slot]);

	// Clear the swapchain to black so letterbox/unwritten areas are well-defined. PresentRect draws over this.
	dkCmdBufBindRenderTarget(m_cmdbuf, &m_swapchain_view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(m_present_width), static_cast<float>(m_present_height),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

	return PresentResult::OK;
#else
	return PresentResult::FrameSkipped;
#endif
}

void GSDeviceDK::EndPresent()
{
#ifdef __SWITCH__
	if (!m_frame_active)
		return;

	if (m_present_slot >= 0)
	{
		dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
		dkQueuePresentImage(m_queue, m_swapchain, m_present_slot);
	}

	m_present_slot = -1;
	m_frame_active = false;
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
#ifdef __SWITCH__
	if (!sTex || !dTex)
		return;

	GSTextureDK* const src = static_cast<GSTextureDK*>(sTex);
	GSTextureDK* const dst = static_cast<GSTextureDK*>(dTex);

	BeginFrameIfNeeded();

	DkImageView src_view;
	DkImageView dst_view;
	src->GetImageView(&src_view);
	dst->GetImageView(&dst_view);

	const DkImageRect src_rect = {static_cast<u32>(r.left), static_cast<u32>(r.top), 0, static_cast<u32>(r.width()),
		static_cast<u32>(r.height()), 1};
	const DkImageRect dst_rect = {destX, destY, 0, static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1};
	dkCmdBufCopyImage(m_cmdbuf, &src_view, &src_rect, &dst_view, &dst_rect, 0);

	dst->SetState(GSTexture::State::Dirty);
#endif
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{
#ifdef __SWITCH__
	// Only Copy is wired up properly for now.
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, &m_copy_fsh,
		linear);
#endif
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
#ifdef __SWITCH__
	// TODO: honour the channel write mask once colour-write state is plumbed.
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, &m_copy_fsh,
		false);
#endif
}

void GSDeviceDK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, bool linear)
{
#ifdef __SWITCH__
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, &m_copy_fsh,
		linear);
#endif
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
#ifdef __SWITCH__
	GSTextureDK* const rt = static_cast<GSTextureDK*>(config.rt);
	if (!rt || rt->IsDepth())
		return;

	BeginFrameIfNeeded();

	DkImageView view;
	rt->GetImageView(&view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(rt->GetWidth()), static_cast<float>(rt->GetHeight()),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(rt->GetWidth()), static_cast<u32>(rt->GetHeight())};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
	rt->SetState(GSTexture::State::Dirty);
#endif
}

void GSDeviceDK::ClearSamplerCache()
{
}

GSTexture* GSDeviceDK::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
#ifdef __SWITCH__
	if (m_device)
	{
		std::unique_ptr<GSTextureDK> tex = GSTextureDK::Create(m_device, m_queue, type, format, width, height, levels);
		if (tex)
			return tex.release();
		Console.Error("DK3D: CreateSurface(%dx%d) failed. Falling back to null.", width, height);
	}
#endif
	return new GSTextureNull(type, width, height, levels, format);
}

void GSDeviceDK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
#ifdef __SWITCH__
	if (!dTex)
		return;

	GSTextureDK* const dst = static_cast<GSTextureDK*>(dTex);
	BeginFrameIfNeeded();

	// This is simplified and will be replaced later.
	DkImageView dst_view;
	dst->GetImageView(&dst_view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &dst_view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(dst->GetWidth()), static_cast<float>(dst->GetHeight()),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(dst->GetWidth()), static_cast<u32>(dst->GetHeight())};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	float bg[4];
	GSVector4::store<false>(bg, GSVector4::unorm8(c));
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, bg[0], bg[1], bg[2], bg[3]);
	dst->SetState(GSTexture::State::Dirty);

	if (sTex[1] && PMODE.EN2)
		DoStretchRectImpl(static_cast<GSTextureDK*>(sTex[1]), sRect[1], dst, dRect[1], &m_copy_fsh, linear);
	if (sTex[0] && PMODE.EN1)
		DoStretchRectImpl(static_cast<GSTextureDK*>(sTex[0]), sRect[0], dst, dRect[0], &m_copy_fsh, linear);
#endif
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
