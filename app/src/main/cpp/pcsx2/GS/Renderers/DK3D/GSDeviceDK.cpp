// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/GSDeviceDK.h"

// CreateSurface falls back to null textures so GSRendererHW always gets one.
#include "GS/Renderers/Null/GSDeviceNull.h"

#include "GS/Renderers/Common/GSVertex.h"

#include "imgui.h"

#ifdef __SWITCH__
#include "common/Console.h"
#include "common/Horizon/Horizon.h" // nwindowGetDefault()

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
	constexpr u32 CMDBUF_SIZE = 8 * 1024 * 1024;
	constexpr u32 CODE_MEMSIZE = 128 * 1024;
	constexpr u32 VERTEX_BUFFER_SIZE = 4 * 1024 * 1024;
	constexpr u32 INDEX_BUFFER_SIZE = 2 * 1024 * 1024;
	constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;

	struct ConvertVertex
	{
		float pos[4];
		float uv[2];
	};

	struct DKTfxSelector
	{
		u32 fst, tme, tfx, tcc, atst, afail, fog, aem;
		u32 aem_fmt, pal_fmt, ltf, wms, wmt, dst_fmt, fba, iip;
		u32 region_rect, adjs, adjt, tcoffsethack;
		u32 blend_a, blend_b, blend_c, blend_d, blend_mix, blend_hw, pabe, fixed_one_a;
		u32 a_masked, colclip, colclip_hw, rta_correction, dither, dither_adjust, round_inv, tex_is_fb;
		u32 channel, shuffle, shuffle_same, read16src, process_ba, process_rg, shuffle_across, write_rg;
		u32 fbmask, scanmsk, date;
		u32 depth_fmt, urban_chaos, tales;
		u32 automatic_lod, manual_lod;
	};

	constexpr DkBlendFactor kBlendFactors[16] = {
		DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_DstColor, DkBlendFactor_InvDstColor,
		DkBlendFactor_Src1Color, DkBlendFactor_InvSrc1Color, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
		DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha, DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha,
		DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor, DkBlendFactor_One, DkBlendFactor_Zero,
	};
	constexpr DkBlendOp kBlendOps[3] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub};

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
	cmdbuf_maker.userData = this;
	cmdbuf_maker.cbAddMem = &GSDeviceDK::AddCmdMemoryThunk;
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

	// Vertex / index / uniform stream buffers
	dkMemBlockMakerDefaults(&memblock_maker, m_device, VERTEX_BUFFER_SIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_vertex_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_vertex_memblock)
		return false;

	dkMemBlockMakerDefaults(&memblock_maker, m_device, INDEX_BUFFER_SIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_index_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_index_memblock)
		return false;

	dkMemBlockMakerDefaults(&memblock_maker, m_device, UNIFORM_BUFFER_SIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_uniform_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_uniform_memblock)
		return false;

	// Allow GSRendererHW to use software blending
	m_features.texture_barrier = true;

	LoadShaders();
	if (!m_convert_shaders_ok)
		Console.Warning("DK3D: No shaders available. Things will be broken.");
	else
		SetupSamplers();

	Console.WriteLn("DK3D: deko3d device up (%dx%d, %u framebuffers, convert=%d, tfx=%d).", m_present_width,
		m_present_height, NUM_FRAMEBUFFERS, m_convert_shaders_ok ? 1 : 0, m_tfx_shaders_ok ? 1 : 0);
	return true;
}

bool GSDeviceDK::SetupSamplers()
{
	// tau/tav select REPEAT vs CLAMP per axis.
	DkSamplerDescriptor descriptors[NUM_SAMPLERS];
	for (unsigned i = 0; i < NUM_SAMPLERS; i++)
	{
		const bool biln = (i & 4) != 0;
		const bool tau = (i & 2) != 0;
		const bool tav = (i & 1) != 0;

		DkSampler sampler;
		dkSamplerDefaults(&sampler);
		sampler.minFilter = biln ? DkFilter_Linear : DkFilter_Nearest;
		sampler.magFilter = biln ? DkFilter_Linear : DkFilter_Nearest;
		sampler.mipFilter = DkMipFilter_Linear;
		sampler.wrapMode[0] = tau ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		sampler.wrapMode[1] = tav ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
		dkSamplerDescriptorInitialize(&descriptors[i], &sampler);
	}

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
	dkCmdBufAddMemory(m_cmdbuf, m_cmdbuf_memblock, 0, CMDBUF_SIZE);
	m_vertex_offset = 0;
	m_index_offset = 0;
	m_uniform_offset = 0;
	m_next_image_slot = 0;

	m_frame_active = true;
}

void GSDeviceDK::AddCmdMemoryThunk(void* userData, DkCmdBuf cmdbuf, size_t minReqSize)
{
	static_cast<GSDeviceDK*>(userData)->AddCmdMemory(cmdbuf, minReqSize);
}

void GSDeviceDK::AddCmdMemory(DkCmdBuf cmdbuf, size_t minReqSize)
{
	// If a frame exceeds CMDBUF_SIZE, wrap instead of letting deko3d abort.
	Console.Warning("DK3D: command buffer ring exhausted (need %zu bytes).", minReqSize);
	dkCmdBufAddMemory(cmdbuf, m_cmdbuf_memblock, 0, CMDBUF_SIZE);
}

DkGpuAddr GSDeviceDK::StreamVertices(const void* data, u32 size)
{
	m_vertex_offset = AlignUp(m_vertex_offset, sizeof(ConvertVertex));
	if (m_vertex_offset + size > VERTEX_BUFFER_SIZE)
		m_vertex_offset = 0;
	std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(m_vertex_memblock)) + m_vertex_offset, data, size);
	const DkGpuAddr addr = dkMemBlockGetGpuAddr(m_vertex_memblock) + m_vertex_offset;
	m_vertex_offset += size;
	return addr;
}

DkGpuAddr GSDeviceDK::StreamIndices(const void* data, u32 size)
{
	m_index_offset = AlignUp(m_index_offset, sizeof(u32));
	if (m_index_offset + size > INDEX_BUFFER_SIZE)
		m_index_offset = 0;
	std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(m_index_memblock)) + m_index_offset, data, size);
	const DkGpuAddr addr = dkMemBlockGetGpuAddr(m_index_memblock) + m_index_offset;
	m_index_offset += size;
	return addr;
}

DkGpuAddr GSDeviceDK::StreamUniform(const void* data, u32 size)
{
	m_uniform_offset = AlignUp(m_uniform_offset, DK_UNIFORM_BUF_ALIGNMENT);
	if (m_uniform_offset + AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT) > UNIFORM_BUFFER_SIZE)
		m_uniform_offset = 0;
	std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(m_uniform_memblock)) + m_uniform_offset, data, size);
	const DkGpuAddr addr = dkMemBlockGetGpuAddr(m_uniform_memblock) + m_uniform_offset;
	m_uniform_offset += AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT);
	return addr;
}

u32 GSDeviceDK::PushImage(const GSTextureDK* tex)
{
	const u32 slot = m_next_image_slot;
	m_next_image_slot = (m_next_image_slot + 1) % NUM_IMAGE_DESCRIPTORS;
	const DkImageDescriptor descriptor = tex->GetDescriptor();
	dkCmdBufPushData(m_cmdbuf, m_image_descriptor_set + slot * sizeof(DkImageDescriptor), &descriptor,
		sizeof(descriptor));
	return slot;
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
	const GSVector4& dRect, const DkShader* fragment_shader, bool linear, const void* cb, u32 cb_size,
	bool depth_output, u32 color_write_mask, bool alpha_blend)
{
	if (!sTex || !m_convert_shaders_ok)
		return;

	// Depth output needs a destination depth image
	if (depth_output && (!dTex || !dTex->IsDepth()))
		return;

	BeginFrameIfNeeded();

	// Resolve pending clears and flush prior target writes before sampling.
	CommitClear(sTex);
	dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

	const bool is_present = (dTex == nullptr);
	const GSVector2i ds = is_present ? GSVector2i(m_present_width, m_present_height) : dTex->GetSize();

	DkImageView target_view;
	if (is_present)
		target_view = m_swapchain_view;
	else
		dTex->GetImageView(&target_view);
	if (depth_output)
		dkCmdBufBindRenderTargets(m_cmdbuf, nullptr, 0, &target_view);
	else
		dkCmdBufBindRenderTarget(m_cmdbuf, &target_view, nullptr);

	// Resolve pending clear before writing over the target.
	if (!is_present && dTex->GetState() == GSTexture::State::Cleared)
	{
		if (depth_output)
		{
			dkCmdBufClearDepthStencil(m_cmdbuf, true, dTex->GetClearDepth(), 0xFF, 0);
		}
		else
		{
			float cc[4];
			GSVector4::store<false>(cc, dTex->GetUNormClearColor());
			dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, cc[0], cc[1], cc[2], cc[3]);
		}
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
	dkColorWriteStateSetMask(&color_write_state, 0, color_write_mask);
	if (alpha_blend)
		dkColorStateSetBlendEnable(&color_state, 0, true);
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);
	if (alpha_blend)
	{
		DkBlendState blend_state;
		dkBlendStateDefaults(&blend_state);
		dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);
	}

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = depth_output;
	depth_state.depthWriteEnable = depth_output;
	depth_state.depthCompareOp = DkCompareOp_Always;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	const DkShader* shaders[] = {&m_convert_vsh, fragment_shader};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	const u32 image_slot = m_next_image_slot;
	m_next_image_slot = (m_next_image_slot + 1) % NUM_IMAGE_DESCRIPTORS;
	const DkImageDescriptor src_descriptor = sTex->GetDescriptor();
	dkCmdBufPushData(m_cmdbuf, m_image_descriptor_set + image_slot * sizeof(DkImageDescriptor), &src_descriptor,
		sizeof(src_descriptor));
	const DkResHandle texture_handle = dkMakeTextureHandle(image_slot, linear ? SAMPLER_LINEAR : SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);

	// Optional post-process constants.
	if (cb && cb_size)
	{
		const DkGpuAddr cb_addr = StreamUniform(cb, cb_size);
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, cb_addr, AlignUp(cb_size, DK_UNIFORM_BUF_ALIGNMENT));
	}

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
						   load_one(m_copy_fsh, "romfs:/shaders/texture_fsh.dksh") &&
						   load_one(m_convert_fsh, "romfs:/shaders/convert_fsh.dksh");

	if (m_convert_shaders_ok)
		Console.WriteLn("DK3D: convert shaders loaded.");

	m_tfx_shaders_ok = load_one(m_tfx_vsh, "romfs:/shaders/tfx_vsh.dksh") &&
					   load_one(m_tfx_fsh, "romfs:/shaders/tfx_fsh.dksh");

	if (m_tfx_shaders_ok)
		Console.WriteLn("DK3D: tfx shaders loaded.");
	else
		Console.Warning("DK3D: tfx shaders missing. Things will be broken.");

	m_postprocess_shaders_ok = load_one(m_interlace_fsh, "romfs:/shaders/interlace_fsh.dksh") &&
							   load_one(m_shadeboost_fsh, "romfs:/shaders/shadeboost_fsh.dksh") &&
							   load_one(m_fxaa_fsh, "romfs:/shaders/fxaa_fsh.dksh") &&
							   load_one(m_merge_fsh, "romfs:/shaders/merge_fsh.dksh");

	if (m_postprocess_shaders_ok)
		Console.WriteLn("DK3D: post-process shaders loaded.");
	else
		Console.Warning("DK3D: post-process shaders missing. Enhancements won't work properly.");

	m_imgui_shaders_ok = load_one(m_imgui_vsh, "romfs:/shaders/imgui_vsh.dksh") &&
						 load_one(m_imgui_fsh, "romfs:/shaders/imgui_fsh.dksh");

	if (m_imgui_shaders_ok)
		Console.WriteLn("DK3D: imgui shaders loaded.");
	else
		Console.Warning("DK3D: imgui shaders missing. No UI will be present.");

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
	if (m_index_memblock)
	{
		dkMemBlockDestroy(m_index_memblock);
		m_index_memblock = nullptr;
	}
	if (m_uniform_memblock)
	{
		dkMemBlockDestroy(m_uniform_memblock);
		m_uniform_memblock = nullptr;
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

	// Clear letterbox/unwritten areas before PresentRect draws.
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

#ifdef __SWITCH__
void GSDeviceDK::RenderImGui()
{
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (!draw_data || draw_data->CmdListsCount == 0 || !m_imgui_shaders_ok || !m_frame_active || m_present_slot < 0)
		return;

	static_assert(sizeof(ImDrawIdx) == sizeof(u16), "ImGui index buffer must be 16-bit");

	const float width = static_cast<float>(m_present_width);
	const float height = static_cast<float>(m_present_height);

	// Draw over the already-presented frame
	dkCmdBufBindRenderTarget(m_cmdbuf, &m_swapchain_view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);

	// Ignnore culling and depth
	DkRasterizerState rasterizer_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

	DkColorState color_state;
	dkColorStateDefaults(&color_state);
	dkColorStateSetBlendEnable(&color_state, 0, true);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);

	DkBlendState blend_state;
	dkBlendStateDefaults(&blend_state);
	dkBlendStateSetOps(&blend_state, DkBlendOp_Add, DkBlendOp_Add);
	dkBlendStateSetFactors(&blend_state, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
		DkBlendFactor_Zero);
	dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);

	DkColorWriteState color_write_state;
	dkColorWriteStateDefaults(&color_write_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = false;
	depth_state.depthWriteEnable = false;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	const DkShader* shaders[] = {&m_imgui_vsh, &m_imgui_fsh};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	struct ImGuiConstants
	{
		float scale[2];
		float translate[2];
	} constants;
	constants.scale[0] = 2.0f / width;
	constants.scale[1] = 2.0f / height;
	constants.translate[0] = -1.0f;
	constants.translate[1] = -1.0f;
	const DkGpuAddr cb_addr = StreamUniform(&constants, sizeof(constants));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 0, cb_addr, AlignUp(sizeof(constants), DK_UNIFORM_BUF_ALIGNMENT));

	static const DkVtxAttribState attribs[3] = {
		{0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(ImDrawVert), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 3);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);

	const GSVector4i rt_bounds = GSVector4i(0, 0, m_present_width, m_present_height);

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		const u32 vtx_size = static_cast<u32>(cmd_list->VtxBuffer.Size) * sizeof(ImDrawVert);
		const u32 idx_size = static_cast<u32>(cmd_list->IdxBuffer.Size) * sizeof(ImDrawIdx);
		const DkGpuAddr vtx_addr = StreamVertices(cmd_list->VtxBuffer.Data, vtx_size);
		const DkGpuAddr idx_addr = StreamIndices(cmd_list->IdxBuffer.Data, idx_size);
		dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vtx_addr, vtx_size);
		dkCmdBufBindIdxBuffer(m_cmdbuf, DkIdxFormat_Uint16, idx_addr);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			if (pcmd->UserCallback)
				continue;

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			const GSVector4i iclip = GSVector4i(clip).rintersect(rt_bounds);
			if (iclip.rempty())
				continue;
			const DkScissor dk_scissor = {static_cast<u32>(iclip.x), static_cast<u32>(iclip.y),
				static_cast<u32>(iclip.width()), static_cast<u32>(iclip.height())};
			dkCmdBufSetScissors(m_cmdbuf, 0, &dk_scissor, 1);

			GSTextureDK* const tex = reinterpret_cast<GSTextureDK*>(pcmd->GetTexID());
			const u32 slot = tex ? PushImage(tex) : 0;
			const DkResHandle handle = dkMakeTextureHandle(slot, SAMPLER_LINEAR);
			dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &handle, 1);

			dkCmdBufDrawIndexed(m_cmdbuf, DkPrimitive_Triangles, pcmd->ElemCount, 1, pcmd->IdxOffset, pcmd->VtxOffset,
				0);
		}
	}
}
#endif

void GSDeviceDK::EndPresent()
{
#ifdef __SWITCH__
	RenderImGui();

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
#ifdef __SWITCH__
	return GSDownloadTextureDK::Create(this, m_device, width, height, format);
#else
	return nullptr;
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::ReadbackTexture(GSTextureDK* src, const GSVector4i& rect, DkMemBlock dst_block, u32 dst_offset)
{
	if (!src || rect.rempty())
		return;

	// Preserve current frame work
	BeginFrameIfNeeded();
	CommitClear(src);

	// Flush and invalidate caches
	dkCmdBufBarrier(m_cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);

	DkImageView src_view;
	src->GetImageView(&src_view);
	const DkImageRect src_rect = {static_cast<u32>(rect.left), static_cast<u32>(rect.top), 0,
		static_cast<u32>(rect.width()), static_cast<u32>(rect.height()), 1};

	const DkCopyBuf dst = {dkMemBlockGetGpuAddr(dst_block) + dst_offset, 0, 0};
	dkCmdBufCopyImageToBuffer(m_cmdbuf, &src_view, &src_rect, &dst, 0);

	// Finish this list so EndPresent won't resubmit it
	dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueueWaitIdle(m_queue);
}
#endif

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

void GSDeviceDK::DoConvert(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear, u32 color_write_mask)
{
#ifdef __SWITCH__
	// COPY uses the plain copy shader
	if (shader == ShaderConvert::COPY)
	{
		DoStretchRectImpl(sTex, sRect, dTex, dRect, &m_copy_fsh, linear, nullptr, 0, false, color_write_mask);
		return;
	}

	struct
	{
		u32 variant;
		u32 pad[3];
	} ub = {static_cast<u32>(shader), {0, 0, 0}};
	DoStretchRectImpl(sTex, sRect, dTex, dRect, &m_convert_fsh, false, &ub, sizeof(ub), HasDepthOutput(shader),
		color_write_mask);
#endif
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{
#ifdef __SWITCH__
	DoConvert(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, shader, linear,
		ShaderConvertWriteMask(shader));
#endif
}

void GSDeviceDK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
#ifdef __SWITCH__
	const u32 mask = (red ? 1u : 0u) | (green ? 2u : 0u) | (blue ? 4u : 0u) | (alpha ? 8u : 0u);
	DoConvert(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, shader, false, mask);
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
	if (!m_tfx_shaders_ok)
		return;

	GSTextureDK* const rt = static_cast<GSTextureDK*>(config.rt);
	// Depth-only draws are not handled yet.
	// TODO: That ^
	if (!rt || rt->IsDepth() || config.nindices == 0 || !config.verts || !config.indices)
		return;

	GSTextureDK* const ds = static_cast<GSTextureDK*>(config.ds);
	GSTextureDK* const tex = static_cast<GSTextureDK*>(config.tex);
	GSTextureDK* const pal = static_cast<GSTextureDK*>(config.pal);
	const bool has_ds = (ds && ds->IsDepth());

	BeginFrameIfNeeded();

	// Resolve any pending clears on the source texture and flush
	if (tex)
		CommitClear(tex);
	dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

	DkImageView rt_view;
	DkImageView ds_view;
	rt->GetImageView(&rt_view);
	if (has_ds)
		ds->GetImageView(&ds_view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &rt_view, has_ds ? &ds_view : nullptr);

	const GSVector2i rtsize = rt->GetSize();
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(rtsize.x), static_cast<float>(rtsize.y), 0.0f, 1.0f};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);

	const GSVector4i scissor = config.scissor.rintersect(GSVector4i::loadh(rtsize));
	const DkScissor dk_scissor = {static_cast<u32>(std::max(0, scissor.x)), static_cast<u32>(std::max(0, scissor.y)),
		static_cast<u32>(std::max(0, scissor.width())), static_cast<u32>(std::max(0, scissor.height()))};
	dkCmdBufSetScissors(m_cmdbuf, 0, &dk_scissor, 1);

	// Commit pending clears on the targets before drawing over them.
	if (rt->GetState() == GSTexture::State::Cleared)
	{
		float cc[4];
		GSVector4::store<false>(cc, rt->GetUNormClearColor());
		dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, cc[0], cc[1], cc[2], cc[3]);
	}
	rt->SetState(GSTexture::State::Dirty);
	if (has_ds)
	{
		if (ds->GetState() == GSTexture::State::Cleared)
			dkCmdBufClearDepthStencil(m_cmdbuf, true, ds->GetClearDepth(), 0xFF, 0);
		ds->SetState(GSTexture::State::Dirty);
	}

	DkRasterizerState rasterizer_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

	auto bind_blend = [&](const GSHWDrawConfig::BlendState& blend) {
		DkColorState color_state;
		dkColorStateDefaults(&color_state);
		DkBlendState blend_state;
		dkBlendStateDefaults(&blend_state);
		if (blend.enable)
		{
			dkColorStateSetBlendEnable(&color_state, 0, true);
			dkBlendStateSetOps(&blend_state, kBlendOps[blend.op], DkBlendOp_Add);
			dkBlendStateSetFactors(&blend_state, kBlendFactors[blend.src_factor],
				kBlendFactors[blend.dst_factor], kBlendFactors[blend.src_factor_alpha],
				kBlendFactors[blend.dst_factor_alpha]);
			if (blend.constant_enable)
			{
				const float c = static_cast<float>(blend.constant) / 128.0f;
				dkCmdBufSetBlendConst(m_cmdbuf, c, c, c, c);
			}
		}
		dkCmdBufBindColorState(m_cmdbuf, &color_state);
		dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);
	};

	auto bind_color_mask = [&](u32 wrgba) {
		DkColorWriteState color_write_state;
		dkColorWriteStateDefaults(&color_write_state);
		dkColorWriteStateSetMask(&color_write_state, 0, wrgba);
		dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);
	};

	auto bind_depth = [&](const GSHWDrawConfig::DepthStencilSelector& depth) {
		DkDepthStencilState depth_state;
		dkDepthStencilStateDefaults(&depth_state);
		if (has_ds)
		{
			static const DkCompareOp ztst[] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Gequal,
				DkCompareOp_Greater};
			depth_state.depthTestEnable = (depth.ztst != ZTST_ALWAYS || depth.zwe);
			depth_state.depthWriteEnable = depth.zwe;
			depth_state.depthCompareOp = ztst[depth.ztst];
		}
		else
		{
			depth_state.depthTestEnable = false;
			depth_state.depthWriteEnable = false;
		}
		dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);
	};

	bind_blend(config.blend);
	bind_color_mask(config.colormask.wrgba);
	bind_depth(config.depth);

	const DkShader* shaders[] = {&m_tfx_vsh, &m_tfx_fsh};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	const u32 sampler_slot = (config.sampler.biln ? SAMPLER_LINEAR : SAMPLER_POINT) |
							 (config.sampler.tau ? 2u : 0u) | (config.sampler.tav ? 1u : 0u);
	DkResHandle handles[2];
	handles[0] = tex ? dkMakeTextureHandle(PushImage(tex), sampler_slot) : dkMakeTextureHandle(0, SAMPLER_POINT);
	handles[1] = pal ? dkMakeTextureHandle(PushImage(pal), SAMPLER_POINT) : dkMakeTextureHandle(0, SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 2, handles, 2);

	// Bind RT as RtSampler when shader-side blending/fbmask/DATE needs Cd/Ad.
	const bool needs_rt_tex = config.require_one_barrier || config.require_full_barrier;
	if (needs_rt_tex)
	{
		const DkResHandle rt_handle = dkMakeTextureHandle(PushImage(rt), SAMPLER_POINT);
		dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 4, &rt_handle, 1);
	}

	// Rebuild the selector from PSSelector so second passes can swap overrides.
	auto make_selector = [&](const GSHWDrawConfig::PSSelector& ps) {
		DKTfxSelector sel = {};
		sel.fst = ps.fst;
		// tex_is_fb samples the RT without config.tex
		// Depth sources still need ST from the VS even when config.tex is a depth copy.
		sel.tme = (tex != nullptr || ps.tex_is_fb || ps.depth_fmt != 0) ? 1u : 0u;
		sel.tfx = ps.tfx;
		sel.tcc = ps.tcc;
		sel.atst = ps.atst;
		sel.afail = ps.afail;
		sel.fog = ps.fog;
		sel.aem = ps.aem;
		sel.aem_fmt = ps.aem_fmt;
		sel.pal_fmt = ps.pal_fmt;
		sel.ltf = ps.ltf;
		sel.wms = ps.wms;
		sel.wmt = ps.wmt;
		sel.dst_fmt = ps.dst_fmt;
		sel.fba = ps.fba;
		sel.iip = ps.iip;
		sel.region_rect = ps.region_rect;
		sel.adjs = ps.adjs;
		sel.adjt = ps.adjt;
		sel.tcoffsethack = ps.tcoffsethack;
		sel.blend_a = ps.blend_a;
		sel.blend_b = ps.blend_b;
		sel.blend_c = ps.blend_c;
		sel.blend_d = ps.blend_d;
		sel.blend_mix = ps.blend_mix;
		sel.blend_hw = ps.blend_hw;
		sel.pabe = ps.pabe;
		sel.fixed_one_a = ps.fixed_one_a;
		sel.a_masked = ps.a_masked;
		sel.colclip = ps.colclip;
		sel.colclip_hw = ps.colclip_hw;
		sel.rta_correction = ps.rta_correction;
		sel.dither = ps.dither;
		sel.dither_adjust = ps.dither_adjust;
		sel.round_inv = ps.round_inv;
		sel.tex_is_fb = ps.tex_is_fb;
		sel.channel = ps.channel;
		sel.shuffle = ps.shuffle;
		sel.shuffle_same = ps.shuffle_same;
		sel.read16src = ps.real16src;
		sel.process_ba = ps.process_ba;
		sel.process_rg = ps.process_rg;
		sel.shuffle_across = ps.shuffle_across;
		sel.write_rg = ps.write_rg;
		sel.fbmask = ps.fbmask;
		sel.scanmsk = ps.scanmsk;
		sel.date = ps.date;
		sel.depth_fmt = ps.depth_fmt;
		sel.urban_chaos = ps.urban_chaos_hle;
		sel.tales = ps.tales_of_abyss_hle;
		sel.automatic_lod = ps.automatic_lod;
		sel.manual_lod = ps.manual_lod;
		return sel;
	};

	auto bind_selector = [&](const DKTfxSelector& sel) {
		const DkGpuAddr sel_addr = StreamUniform(&sel, sizeof(sel));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 0, sel_addr, AlignUp(sizeof(sel), DK_UNIFORM_BUF_ALIGNMENT));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, sel_addr, AlignUp(sizeof(sel), DK_UNIFORM_BUF_ALIGNMENT));
	};
	auto bind_ps_cb = [&]() {
		const DkGpuAddr ps_addr = StreamUniform(&config.cb_ps, sizeof(config.cb_ps));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 1, ps_addr,
			AlignUp(sizeof(config.cb_ps), DK_UNIFORM_BUF_ALIGNMENT));
	};

	const DkGpuAddr vs_addr = StreamUniform(&config.cb_vs, sizeof(config.cb_vs));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 1, vs_addr,
		AlignUp(sizeof(config.cb_vs), DK_UNIFORM_BUF_ALIGNMENT));
	bind_ps_cb();
	bind_selector(make_selector(config.ps));

	const u32 vtx_size = config.nverts * sizeof(GSVertex);
	const u32 idx_size = config.nindices * sizeof(u16);
	const DkGpuAddr vtx_addr = StreamVertices(config.verts, vtx_size);
	const DkGpuAddr idx_addr = StreamIndices(config.indices, idx_size);

	static const DkVtxAttribState attribs[7] = {
		{0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, 8, DkVtxAttribSize_4x8, DkVtxAttribType_Uint, 0},
		{0, 0, 12, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
		{0, 0, 16, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
		{0, 0, 20, DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
		{0, 0, 24, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
		{0, 0, 28, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(GSVertex), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 7);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);
	dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vtx_addr, vtx_size);
	dkCmdBufBindIdxBuffer(m_cmdbuf, DkIdxFormat_Uint16, idx_addr);

	DkPrimitive primitive = DkPrimitive_Triangles;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point: primitive = DkPrimitive_Points; break;
		case GSHWDrawConfig::Topology::Line: primitive = DkPrimitive_Lines; break;
		case GSHWDrawConfig::Topology::Triangle: primitive = DkPrimitive_Triangles; break;
	}

	SendHWDraw(config, primitive, config.require_one_barrier, config.require_full_barrier);

	// Blend multi-pass redraws with selector overrides
	if (config.blend_multi_pass.enable)
	{
		bind_blend(config.blend_multi_pass.blend);
		DKTfxSelector mp_sel = make_selector(config.ps);
		mp_sel.blend_hw = config.blend_multi_pass.blend_hw;
		mp_sel.dither = config.blend_multi_pass.dither;
		bind_selector(mp_sel);
		dkCmdBufDrawIndexed(m_cmdbuf, primitive, config.nindices, 1, 0, 0, 0);
	}

	// Alpha pass swaps PS selector, colour mask, depth, and restores original blend.
	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			bind_ps_cb();
		}
		bind_color_mask(config.alpha_second_pass.colormask.wrgba);
		bind_depth(config.alpha_second_pass.depth);
		bind_blend(config.blend);
		bind_selector(make_selector(config.alpha_second_pass.ps));
		SendHWDraw(config, primitive, config.alpha_second_pass.require_one_barrier,
			config.alpha_second_pass.require_full_barrier);
	}
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::SendHWDraw(const GSHWDrawConfig& config, DkPrimitive primitive, bool one_barrier, bool full_barrier)
{
	if (full_barrier)
	{
		// drawlist groups non-overlapping sprite runs
		if (config.drawlist)
		{
			const u32 indices_per_prim = config.indices_per_prim;
			const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
			for (u32 n = 0, p = 0; n < draw_list_size; n++)
			{
				const u32 count = static_cast<u32>((*config.drawlist)[n]) * indices_per_prim;
				dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);
				dkCmdBufDrawIndexed(m_cmdbuf, primitive, count, 1, p, 0, 0);
				p += count;
			}
			return;
		}

		// Overlapping geometry needs a barrier between primitives.
		const u32 indices_per_prim = config.indices_per_prim;
		for (u32 p = 0; p < config.nindices; p += indices_per_prim)
		{
			dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);
			dkCmdBufDrawIndexed(m_cmdbuf, primitive, indices_per_prim, 1, p, 0, 0);
		}
		return;
	}

	// A single barrier before the whole draw suffices.
	if (one_barrier)
		dkCmdBufBarrier(m_cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

	dkCmdBufDrawIndexed(m_cmdbuf, primitive, config.nindices, 1, 0, 0, 0);
}
#endif

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
	GSTextureDK* const src0 = static_cast<GSTextureDK*>(sTex[0]); // output 1 (alpha-blended foreground)
	GSTextureDK* const src1 = static_cast<GSTextureDK*>(sTex[1]); // output 2 (blend background)
	BeginFrameIfNeeded();

	// Fill the whole target with the PCRTC background colour
	DkImageView dst_view;
	dst->GetImageView(&dst_view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &dst_view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(dst->GetWidth()), static_cast<float>(dst->GetHeight()),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(dst->GetWidth()), static_cast<u32>(dst->GetHeight())};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	const GSVector4 bg_color = GSVector4::unorm8(c);
	float bg[4];
	GSVector4::store<false>(bg, bg_color);
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, bg[0], bg[1], bg[2], bg[3]);
	dst->SetState(GSTexture::State::Dirty);

	// Output 2 is the blend background
	if (src1 && PMODE.EN2 && PMODE.SLBG == 0)
		DoStretchRectImpl(src1, sRect[1], dst, dRect[1], &m_copy_fsh, linear);

	// Output 1 is alpha-blended over the background
	if (src0 && PMODE.EN1)
	{
		if (m_postprocess_shaders_ok)
		{
			struct
			{
				float BGColor[4];
				u32 mmod;
				u32 pad[3];
			} ub = {};
			GSVector4::store<false>(ub.BGColor, bg_color);
			ub.mmod = PMODE.MMOD;
			DoStretchRectImpl(src0, sRect[0], dst, dRect[0], &m_merge_fsh, linear, &ub, sizeof(ub), false, 0xf, true);
		}
		else
		{
			DoStretchRectImpl(src0, sRect[0], dst, dRect[0], &m_copy_fsh, linear);
		}
	}
#endif
}

void GSDeviceDK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	// cb0: vec4 ZrH + uint mode
	struct
	{
		float ZrH[4];
		u32 mode;
		u32 pad[3];
	} ub = {};
	GSVector4::store<false>(ub.ZrH, cb.ZrH);
	ub.mode = static_cast<u32>(shader);

	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect,
		&m_interlace_fsh, linear, &ub, sizeof(ub));
#endif
}

void GSDeviceDK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		static_cast<GSTextureDK*>(dTex), GSVector4(dTex->GetRect()), &m_fxaa_fsh, true);
#endif
}

void GSDeviceDK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	// cb0: vec4 params
	const float ub[4] = {params[0], params[1], params[2], params[3]};
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		static_cast<GSTextureDK*>(dTex), GSVector4(dTex->GetRect()), &m_shadeboost_fsh, false, ub, sizeof(ub));
#endif
}

bool GSDeviceDK::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}
