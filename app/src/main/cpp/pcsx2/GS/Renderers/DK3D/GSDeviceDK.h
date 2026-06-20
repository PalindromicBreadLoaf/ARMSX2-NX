// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#ifdef __SWITCH__
#include "GS/Renderers/DK3D/GSTextureDK.h"

#include <deko3d.h>
#include <memory>
#endif

// deko3d hardware GS backend for the Nintendo Switch only.
// I really hope this gives playable performance.
class GSDeviceDK final : public GSDevice
{
public:
	GSDeviceDK();
	~GSDeviceDK() override;

	RenderAPI GetRenderAPI() const override;
	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;

	bool HasSurface() const override;
	void DestroySurface() override;
	bool UpdateWindow() override;
	void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;

	std::string GetDriverInfo() const override;
	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

#ifdef __SWITCH__
	void ReadbackTexture(GSTextureDK* src, const GSVector4i& rect, DkMemBlock dst_block, u32 dst_offset);
#endif

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY) override;
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, bool linear) override;
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
		u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
		GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
		const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void RenderHW(GSHWDrawConfig& config) override;
	void ClearSamplerCache() override;

protected:
	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
		const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
		const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;

#ifdef __SWITCH__
private:
	static constexpr unsigned NUM_FRAMEBUFFERS = 2;
	static constexpr unsigned NUM_FRAMES_IN_FLIGHT = 2;
	static constexpr unsigned NUM_IMAGE_DESCRIPTORS = 1024;
	static constexpr unsigned SAMPLER_POINT = 0;
	static constexpr unsigned SAMPLER_LINEAR = 4;
	static constexpr unsigned NUM_SAMPLERS = 8;

	bool CreateDeviceObjects();
	void DestroyDeviceObjects();
	bool LoadShaders();
	bool SetupSamplers();
	void BeginFrameIfNeeded();
	// deko3d cbAddMem hook for command streams that exceed CMDBUF_SIZE.
	static void AddCmdMemoryThunk(void* userData, DkCmdBuf cmdbuf, size_t minReqSize);
	void AddCmdMemory(DkCmdBuf cmdbuf, size_t minReqSize);
	// Flushes a pending ClearRenderTarget
	void CommitClear(GSTextureDK* tex);
	// Optional cb is fragment uniform buffer 0 for post-processing shaders.
	void DoStretchRectImpl(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex, const GSVector4& dRect,
		const DkShader* fragment_shader, bool linear, const void* cb = nullptr, u32 cb_size = 0,
		bool depth_output = false, u32 color_write_mask = 0xf, bool alpha_blend = false, bool integer_output = false);
	void DoConvert(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex, const GSVector4& dRect,
		ShaderConvert shader, bool linear, u32 color_write_mask);
	// Bump-allocate into per-frame stream buffers and return the GPU address.
	DkGpuAddr StreamVertices(const void* data, u32 size);
	DkGpuAddr StreamIndices(const void* data, u32 size);
	DkGpuAddr StreamUniform(const void* data, u32 size);
	u32 PushImage(const GSTextureDK* tex);
	// Draw tfx geometry, splitting barriers for feedback-loop reads.
	void SendHWDraw(const GSHWDrawConfig& config, DkPrimitive primitive, bool one_barrier, bool full_barrier);
	// Present ImGui over game frame
	void RenderImGui();
	// Forget the cached tfx pipeline state so the next HW draw re-binds it
	void InvalidateHWStateCache() { m_hw_invariants_bound = false; }

	DkDevice m_device = nullptr;
	DkQueue m_queue = nullptr;
	DkMemBlock m_fb_memblock = nullptr;
	DkImage m_framebuffers[NUM_FRAMEBUFFERS] = {};
	DkSwapchain m_swapchain = nullptr;

	// Buffer frame's GPU resources so the CPU can record frame N+1 while the GPU is still executing frame N.
	struct FrameContext
	{
		DkMemBlock cmdbuf_memblock = nullptr;
		DkCmdBuf cmdbuf = nullptr;
		DkMemBlock vertex_memblock = nullptr;
		DkMemBlock index_memblock = nullptr;
		DkMemBlock uniform_memblock = nullptr;
		DkFence fence{};
		bool fence_pending = false;
	};
	FrameContext m_frames[NUM_FRAMES_IN_FLIGHT] = {};
	u32 m_frame_index = 0;

	DkMemBlock m_cmdbuf_memblock = nullptr;
	DkCmdBuf m_cmdbuf = nullptr;

	// Convert/present
	DkMemBlock m_code_memblock = nullptr;
	DkShader m_convert_vsh{};
	DkShader m_copy_fsh{};
	DkShader m_convert_fsh{};
	DkShader m_convert_int_fsh{};
	bool m_convert_shaders_ok = false;

	// Hardware tfx
	DkShader m_tfx_vsh{};
	DkShader m_tfx_fsh{};
	bool m_tfx_shaders_ok = false;

	// Skip re-emitting binds that match the previous HW draw
	bool m_hw_invariants_bound = false;
	u32 m_hw_blend_key = 0;
	u32 m_hw_colormask = 0;
	u32 m_hw_depth_key = 0;

	// Post-processing fragment shaders
	DkShader m_interlace_fsh{};
	DkShader m_shadeboost_fsh{};
	DkShader m_fxaa_fsh{};
	DkShader m_merge_fsh{};
	bool m_postprocess_shaders_ok = false;

	// ImGui
	DkShader m_imgui_vsh{};
	DkShader m_imgui_fsh{};
	bool m_imgui_shaders_ok = false;

	DkMemBlock m_descriptor_memblock = nullptr;
	DkGpuAddr m_image_descriptor_set = 0;
	DkGpuAddr m_sampler_descriptor_set = 0;
	u32 m_next_image_slot = 0;

	DkMemBlock m_vertex_memblock = nullptr;
	u32 m_vertex_offset = 0;
	DkMemBlock m_index_memblock = nullptr;
	u32 m_index_offset = 0;
	DkMemBlock m_uniform_memblock = nullptr;
	u32 m_uniform_offset = 0;

	DkImageView m_swapchain_view{};
	bool m_frame_active = false;

	u32 m_framebuffer_size = 0;
	int m_present_width = 0;
	int m_present_height = 0;
	int m_present_slot = -1;
#endif
};
